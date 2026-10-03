#pragma once
#include <switch.h>
#include <zlib.h>
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <vector>

namespace switchu::control_cache {

inline constexpr const char* kCacheDir = "sdmc:/config/OmniLaunch/control_cache";
inline constexpr uint32_t kMetaMagic = 0x53554343;
inline constexpr uint32_t kMetaVersion = 5;

struct Meta {
    uint32_t magic = kMetaMagic;
    uint32_t version = kMetaVersion;
    uint64_t title_id = 0;
    uint8_t startup_user_account = 1;
    uint8_t startup_user_account_option = 0;
    uint8_t reserved[6] = {};
    uint64_t save_data_owner_id = 0;
    uint64_t user_account_save_data_size = 0;
    uint64_t user_account_save_data_journal_size = 0;
    uint64_t device_save_data_size = 0;
    uint64_t device_save_data_journal_size = 0;
    uint64_t temporary_storage_size = 0;
    uint64_t cache_storage_size = 0;
    uint64_t cache_storage_journal_size = 0;
    uint64_t bcat_delivery_cache_storage_size = 0;
    char display_version[0x10] = {};
    char name[0x201] = {};
    char english_name[0x201] = {};
    char publisher[0x101] = {};
};

inline std::string formatTitleId(uint64_t titleId) {
    char buf[17] = {};
    std::snprintf(buf, sizeof(buf), "%016lX", static_cast<unsigned long>(titleId));
    return std::string(buf);
}

inline void copyString(char* dst, size_t dstSize, const char* src, size_t srcSize) {
    if (!dst || dstSize == 0)
        return;

    dst[0] = '\0';
    if (!src || srcSize == 0)
        return;

    size_t len = 0;
    while (len < srcSize && src[len] != '\0')
        ++len;
    len = std::min(dstSize - 1, len);
    std::memcpy(dst, src, len);
    dst[len] = '\0';
}

inline bool isValidUtf8(const char* value, size_t capacity) {
    if (!value || capacity == 0)
        return false;

    const auto* bytes = reinterpret_cast<const unsigned char*>(value);
    size_t i = 0;
    bool terminated = false;
    while (i < capacity) {
        const unsigned char lead = bytes[i];
        if (lead == 0) {
            terminated = true;
            break;
        }
        if (lead < 0x80) {
            // Reject control bytes other than the whitespace used by titles.
            if (lead < 0x20 && lead != '\t' && lead != '\n' && lead != '\r')
                return false;
            ++i;
            continue;
        }

        size_t continuationCount = 0;
        uint32_t codepoint = 0;
        if (lead >= 0xC2 && lead <= 0xDF) {
            continuationCount = 1;
            codepoint = lead & 0x1F;
        } else if (lead >= 0xE0 && lead <= 0xEF) {
            continuationCount = 2;
            codepoint = lead & 0x0F;
        } else if (lead >= 0xF0 && lead <= 0xF4) {
            continuationCount = 3;
            codepoint = lead & 0x07;
        } else {
            return false;
        }

        if (i + continuationCount >= capacity)
            return false;
        for (size_t n = 1; n <= continuationCount; ++n) {
            const unsigned char next = bytes[i + n];
            if ((next & 0xC0) != 0x80)
                return false;
            codepoint = (codepoint << 6) | (next & 0x3F);
        }

        const uint32_t minimum = continuationCount == 1 ? 0x80
                               : continuationCount == 2 ? 0x800 : 0x10000;
        if (codepoint < minimum || codepoint > 0x10FFFF
            || (codepoint >= 0xD800 && codepoint <= 0xDFFF)) {
            return false;
        }
        i += continuationCount + 1;
    }
    return terminated;
}

// The hex title id was once written into the name field when a NACP carried no
// usable one, which made a placeholder indistinguishable from a real name: the
// file then satisfied every "is it cached?" test and the title kept its id as
// its name forever, in the grid and in the artwork lookup alike. Nothing writes
// it any more, and readMeta() treats one that is already on a card as absent so
// the name is asked for again.
inline bool nameIsTitleIdPlaceholder(const char* name, uint64_t titleId);

inline const char* getKnownTitleName(uint64_t titleId) {
    switch (titleId) {
        case 0x010015100B514000ULL: return "Super Mario Bros. Wonder";
        case 0x0100000000010000ULL: return "Super Mario Odyssey";
        case 0x0100152000022000ULL: return "Mario Kart 8 Deluxe";
        case 0x01006A800016E000ULL: return "Super Smash Bros. Ultimate";
        case 0x01007EF00011E000ULL: return "The Legend of Zelda: Breath of the Wild";
        case 0x0100F2C0115B6000ULL: return "The Legend of Zelda: Tears of the Kingdom";
        case 0x01008CF01BAAC000ULL: return "The Legend of Zelda: Echoes of Wisdom";
        case 0x01006BB00C6F0000ULL: return "The Legend of Zelda: Link's Awakening";
        case 0x01002DA013484000ULL: return "The Legend of Zelda: Skyward Sword HD";
        case 0x010099C022B96000ULL: return "Super Mario Galaxy";
        case 0x0100EA80032EA000ULL: return "New Super Mario Bros. U Deluxe";
        default: return nullptr;
    }
}

inline std::string metaPath(uint64_t titleId) {
    return std::string(kCacheDir) + "/" + formatTitleId(titleId) + ".meta";
}

inline std::string iconPath(uint64_t titleId) {
    return std::string(kCacheDir) + "/" + formatTitleId(titleId) + ".jpg";
}

inline void ensureDirectory() {
    std::error_code ec;
    std::filesystem::create_directory("sdmc:/config", ec);
    ec.clear();
    std::filesystem::create_directory("sdmc:/config/OmniLaunch", ec);
    ec.clear();
    std::filesystem::create_directory(kCacheDir, ec);
}

inline bool nameIsTitleIdPlaceholder(const char* name, uint64_t titleId) {
    if (!name || name[0] == '\0')
        return false;
    return formatTitleId(titleId) == name;
}

inline bool readMeta(uint64_t titleId, Meta& out) {
    std::ifstream file(metaPath(titleId), std::ios::binary);
    if (!file.is_open())
        return false;

    Meta meta{};
    if (!file.read(reinterpret_cast<char*>(&meta), sizeof(meta)))
        return false;

    if (meta.magic != kMetaMagic || meta.version != kMetaVersion || meta.title_id != titleId)
        return false;

    meta.display_version[sizeof(meta.display_version) - 1] = '\0';
    meta.name[sizeof(meta.name) - 1] = '\0';
    meta.english_name[sizeof(meta.english_name) - 1] = '\0';
    meta.publisher[sizeof(meta.publisher) - 1] = '\0';
    if (!isValidUtf8(meta.name, sizeof(meta.name)))
        return false;
    if (meta.name[0] == '\0' || nameIsTitleIdPlaceholder(meta.name, titleId)) {
        const char* known = getKnownTitleName(titleId);
        if (known) {
            copyString(meta.name, sizeof(meta.name), known, std::strlen(known));
            if (meta.english_name[0] == '\0') {
                copyString(meta.english_name, sizeof(meta.english_name), known, std::strlen(known));
            }
        } else {
            return false;
        }
    }
    if (meta.english_name[0] != '\0'
        && !isValidUtf8(meta.english_name, sizeof(meta.english_name)))
        meta.english_name[0] = '\0';
    if (meta.publisher[0] != '\0' && !isValidUtf8(meta.publisher, sizeof(meta.publisher)))
        meta.publisher[0] = '\0';
    if (meta.display_version[0] != '\0'
        && !isValidUtf8(meta.display_version, sizeof(meta.display_version))) {
        meta.display_version[0] = '\0';
    }

    out = meta;
    return true;
}

inline bool hasMeta(uint64_t titleId) {
    Meta meta{};
    return readMeta(titleId, meta);
}

// Remove cached metadata and art when a title id is reused by a different
// forwarder. The next catalogue pass will repopulate both files.
inline void forget(uint64_t titleId) {
    std::remove(metaPath(titleId).c_str());
    std::remove(iconPath(titleId).c_str());
}

inline std::vector<uint8_t> readIcon(uint64_t titleId) {
    std::vector<uint8_t> data;
    std::ifstream file(iconPath(titleId), std::ios::binary | std::ios::ate);
    if (!file.is_open())
        return data;

    const std::streamoff size = file.tellg();
    if (size <= 0 || size > 0x100000)
        return data;

    file.seekg(0, std::ios::beg);
    data.resize(static_cast<size_t>(size));
    if (!file.read(reinterpret_cast<char*>(data.data()), size))
        data.clear();
    return data;
}

inline bool writeIcon(uint64_t titleId, const uint8_t* data, size_t size) {
    if (!data || size == 0)
        return false;

    ensureDirectory();
    std::ofstream file(iconPath(titleId), std::ios::binary | std::ios::trunc);
    if (!file.is_open())
        return false;

    file.write(reinterpret_cast<const char*>(data), static_cast<std::streamsize>(size));
    return static_cast<bool>(file);
}

inline bool decompressNacpTitles(const NacpStruct& nacp, std::vector<uint8_t>& out) {
    const auto* nacpBytes = reinterpret_cast<const uint8_t*>(&nacp);
    const bool flagFormat1 = (nacpBytes[0x3215] == 0x01);
    const uint16_t bufferSize = *reinterpret_cast<const uint16_t*>(nacpBytes);

    // If flag is not set, only attempt decompression if the first language entry name
    // does not appear to be printable ASCII (i.e. binary payload) and bufferSize is valid.
    if (!flagFormat1) {
        if (nacpBytes[0] >= 0x20 && nacpBytes[0] < 0x7F && nacpBytes[1] >= 0x20 && nacpBytes[1] < 0x7F) {
            return false;
        }
        if (bufferSize == 0 || bufferSize > 0x2FFE) {
            return false;
        }
    }

    out.assign(0x6000, 0);

    const int wbitsList[] = {-15, 15, 47};
    const size_t inputSizes[] = {
        (bufferSize > 0 && bufferSize <= 0x2FFE) ? static_cast<size_t>(bufferSize) : 0x2FFE,
        0x2FFE
    };

    for (int wbits : wbitsList) {
        for (size_t inSize : inputSizes) {
            z_stream strm{};
            strm.next_in = const_cast<Bytef*>(nacpBytes + 2);
            strm.avail_in = static_cast<uInt>(inSize);
            strm.next_out = reinterpret_cast<Bytef*>(out.data());
            strm.avail_out = static_cast<uInt>(out.size());

            if (inflateInit2(&strm, wbits) != Z_OK)
                continue;

            const int ret = inflate(&strm, Z_FINISH);
            const size_t decompressedBytes = strm.total_out;
            inflateEnd(&strm);

            if ((ret == Z_STREAM_END || ret == Z_OK || ret == Z_BUF_ERROR) &&
                decompressedBytes >= sizeof(NacpLanguageEntry)) {
                const auto* testEntries = reinterpret_cast<const NacpLanguageEntry*>(out.data());
                const int numTest = static_cast<int>(decompressedBytes / sizeof(NacpLanguageEntry));
                for (int t = 0; t < numTest; ++t) {
                    if (testEntries[t].name[0] != '\0' && isValidUtf8(testEntries[t].name, sizeof(testEntries[t].name))) {
                        return true;
                    }
                }
            }
        }
    }

    out.clear();
    return false;
}

inline bool fillMetaFromControlData(uint64_t titleId, const NsApplicationControlData& controlData,
                                    Meta& out) {
    Meta meta{};
    meta.title_id = titleId;
    meta.startup_user_account = controlData.nacp.startup_user_account;
    meta.startup_user_account_option = controlData.nacp.startup_user_account_option;
    meta.save_data_owner_id = controlData.nacp.save_data_owner_id;
    meta.user_account_save_data_size = controlData.nacp.user_account_save_data_size;
    meta.user_account_save_data_journal_size = controlData.nacp.user_account_save_data_journal_size;
    meta.device_save_data_size = controlData.nacp.device_save_data_size;
    meta.device_save_data_journal_size = controlData.nacp.device_save_data_journal_size;
    meta.temporary_storage_size = controlData.nacp.temporary_storage_size;
    meta.cache_storage_size = controlData.nacp.cache_storage_size;
    meta.cache_storage_journal_size = controlData.nacp.cache_storage_journal_size;
    meta.bcat_delivery_cache_storage_size = controlData.nacp.bcat_delivery_cache_storage_size;
    copyString(meta.display_version, sizeof(meta.display_version),
               controlData.nacp.display_version,
               sizeof(controlData.nacp.display_version));

    std::vector<uint8_t> decompressed;
    const bool isCompressed = decompressNacpTitles(controlData.nacp, decompressed);
    const auto* langEntries = isCompressed
        ? reinterpret_cast<const NacpLanguageEntry*>(decompressed.data())
        : controlData.nacp.lang;
    const int maxEntries = isCompressed
        ? static_cast<int>(decompressed.size() / sizeof(NacpLanguageEntry))
        : 16;

    const NacpLanguageEntry* langEntry = nullptr;

    // 1. Try preferred system language
    SetLanguage sysLang = SetLanguage_ENUS;
    u64 sysLangCode = 0;
    if (R_SUCCEEDED(setGetSystemLanguage(&sysLangCode)) &&
        R_SUCCEEDED(setMakeLanguage(sysLangCode, &sysLang))) {
        static constexpr uint8_t s_langTable[] = {
            2,  // SetLanguage_JA = 0
            0,  // SetLanguage_ENUS = 1
            3,  // SetLanguage_FR = 2
            4,  // SetLanguage_DE = 3
            7,  // SetLanguage_IT = 4
            6,  // SetLanguage_ES = 5
            14, // SetLanguage_ZHCN = 6
            12, // SetLanguage_KO = 7
            8,  // SetLanguage_NL = 8
            10, // SetLanguage_PT = 9
            11, // SetLanguage_RU = 10
            13, // SetLanguage_ZHTW = 11
            1,  // SetLanguage_ENGB = 12
            9,  // SetLanguage_FRCA = 13
            5,  // SetLanguage_ES419 = 14
            13, // SetLanguage_ZHHANT = 15
            14, // SetLanguage_ZHHANS = 16
            15  // SetLanguage_PTBR = 17
        };
        const int langIdx = static_cast<int>(sysLang);
        if (langIdx >= 0 && langIdx < static_cast<int>(sizeof(s_langTable))) {
            const int entryIdx = s_langTable[langIdx];
            if (entryIdx < maxEntries) {
                const auto* candidate = &langEntries[entryIdx];
                if (candidate->name[0] != '\0' && isValidUtf8(candidate->name, sizeof(candidate->name))) {
                    langEntry = candidate;
                }
            }
        }
    }

    if (!langEntry && !isCompressed) {
        NacpLanguageEntry* preferred = nullptr;
        if (R_SUCCEEDED(nacpGetLanguageEntry(
                const_cast<NacpStruct*>(&controlData.nacp), &preferred))
            && preferred && preferred->name[0] != '\0'
            && isValidUtf8(preferred->name, sizeof(preferred->name))) {
            langEntry = preferred;
        }
    }

    if (!langEntry) {
        for (int i = 0; i < maxEntries; ++i) {
            const auto* candidate = &langEntries[i];
            if (candidate->name[0] != '\0'
                && isValidUtf8(candidate->name, sizeof(candidate->name))) {
                langEntry = candidate;
                break;
            }
        }
    }

    if (langEntry) {
        copyString(meta.name, sizeof(meta.name), langEntry->name, sizeof(langEntry->name));
        if (isValidUtf8(langEntry->author, sizeof(langEntry->author))) {
            copyString(meta.publisher, sizeof(meta.publisher),
                       langEntry->author, sizeof(langEntry->author));
        }
    }

    // NACP slots 0 and 1 are American and British English. Keep this stable
    // search title independent from the console's display language.
    const NacpLanguageEntry* englishEntry = nullptr;
    for (int languageIndex : {0, 1}) {
        if (languageIndex < maxEntries) {
            const auto* candidate = &langEntries[languageIndex];
            if (candidate->name[0] != '\0'
                && isValidUtf8(candidate->name, sizeof(candidate->name))) {
                englishEntry = candidate;
                break;
            }
        }
    }
    if (englishEntry) {
        copyString(meta.english_name, sizeof(meta.english_name),
                   englishEntry->name, sizeof(englishEntry->name));
    }

    // A name that could not be read stays empty, unless it matches a known title ID.
    if (meta.name[0] == '\0') {
        const char* known = getKnownTitleName(titleId);
        if (known) {
            copyString(meta.name, sizeof(meta.name), known, std::strlen(known));
            if (meta.english_name[0] == '\0') {
                copyString(meta.english_name, sizeof(meta.english_name), known, std::strlen(known));
            }
        }
    }

    if (meta.english_name[0] == '\0' && meta.name[0] != '\0')
        copyString(meta.english_name, sizeof(meta.english_name),
                   meta.name, sizeof(meta.name));

    out = meta;
    return true;
}

inline bool writeMeta(const Meta& meta) {
    ensureDirectory();
    std::ofstream file(metaPath(meta.title_id), std::ios::binary | std::ios::trunc);
    if (!file.is_open())
        return false;

    file.write(reinterpret_cast<const char*>(&meta), sizeof(meta));
    return static_cast<bool>(file);
}

enum class CacheOutcome {
    Named,        // a real name was read; everything is cached
    Unnamed,      // the control data carried no usable name
    WriteFailed,  // the card refused one of the two files
};

// Caching an unnamed title is still worth doing: the startup-user policy and
// the save-data sizes come from the same NACP, and the icon usually decodes
// even when no language entry has a name. The caller decides whether to look
// for a name somewhere else before settling for this.
inline CacheOutcome writeFromControlData(uint64_t titleId,
                                         const NsApplicationControlData& controlData,
                                         size_t controlSize) {
    Meta meta{};
    if (!fillMetaFromControlData(titleId, controlData, meta))
        return CacheOutcome::WriteFailed;

    const bool metaOk = writeMeta(meta);
    bool iconOk = true;
    if (controlSize > sizeof(NacpStruct)) {
        const size_t iconSize = controlSize - sizeof(NacpStruct);
        iconOk = writeIcon(titleId, controlData.icon, iconSize);
    }
    if (!metaOk || !iconOk)
        return CacheOutcome::WriteFailed;
    return meta.name[0] != '\0' ? CacheOutcome::Named : CacheOutcome::Unnamed;
}

}
