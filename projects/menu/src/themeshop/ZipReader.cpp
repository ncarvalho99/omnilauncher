#include "ZipReader.hpp"

#include "core/DebugLog.hpp"

#include <zlib.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <unordered_set>
#include <vector>

namespace {

constexpr std::uint32_t kEndOfCentralDirectory = 0x06054b50;
constexpr std::uint32_t kCentralFileHeader     = 0x02014b50;
constexpr std::uint32_t kLocalFileHeader       = 0x04034b50;

constexpr std::uint16_t kMethodStored  = 0;
constexpr std::uint16_t kMethodDeflate = 8;

// Ceilings on what a theme may be. A package needing more than this is not a
// theme somebody made; it is a mistake or an attempt to fill the card.
constexpr std::uint64_t kMaxEntryBytes = 96ull * 1024ull * 1024ull;
constexpr std::uint64_t kMaxTotalBytes = 512ull * 1024ull * 1024ull;
constexpr int           kMaxEntries    = 4000;
constexpr std::size_t   kCopyChunk     = 256 * 1024;

std::uint16_t read16(const std::uint8_t* p) {
    return (std::uint16_t)(p[0] | (p[1] << 8));
}

std::uint32_t read32(const std::uint8_t* p) {
    return (std::uint32_t)p[0] | ((std::uint32_t)p[1] << 8)
         | ((std::uint32_t)p[2] << 16) | ((std::uint32_t)p[3] << 24);
}

// The same allowlist the catalogue server enforces on submission. Repeated here
// on purpose: the console cannot assume the archive came through that server.
bool allowedExtension(const std::string& name) {
    static const char* kAllowed[] = {
        ".json", ".jpg", ".jpeg", ".png", ".dds", ".webp",
        ".txt", ".md", ".ttf", ".otf",
        ".mp3", ".ogg", ".wav", ".flac",
        ".mp4", ".mkv", ".mov", ".webm",
    };
    const std::size_t dot = name.find_last_of('.');
    if (dot == std::string::npos)
        return false;
    std::string ext = name.substr(dot);
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) {
        return (char)std::tolower(c);
    });
    for (const char* allowed : kAllowed) {
        if (ext == allowed)
            return true;
    }
    return false;
}

bool safeRelativePath(const std::string& raw, std::string& out) {
    if (raw.empty() || raw.size() > 240)
        return false;
    // A drive letter or a leading slash leaves the destination entirely.
    if (raw.front() == '/' || raw.front() == '\\' || raw.find(':') != std::string::npos)
        return false;

    std::string normalized;
    normalized.reserve(raw.size());
    for (char ch : raw)
        normalized.push_back(ch == '\\' ? '/' : ch);

    // ".." anywhere walks out of the folder, and extraction would follow it.
    // Rejected rather than stripped: a path that needs cleaning is not a path
    // this was asked to read.
    std::size_t start = 0;
    while (start <= normalized.size()) {
        const std::size_t slash = normalized.find('/', start);
        const std::string part = normalized.substr(
            start, slash == std::string::npos ? std::string::npos : slash - start);
        if (part == ".." || part == ".")
            return false;
        if (slash == std::string::npos)
            break;
        start = slash + 1;
    }

    out = std::move(normalized);
    return true;
}

bool readAt(std::FILE* f, std::uint64_t offset, void* dst, std::size_t bytes) {
    if (std::fseek(f, (long)offset, SEEK_SET) != 0)
        return false;
    return std::fread(dst, 1, bytes, f) == bytes;
}

// Copies an entry out, decompressing as it goes. Never holds more than a chunk
// of input and a chunk of output, whatever the entry's size.
bool copyEntry(std::FILE* src, std::uint64_t offset, std::uint64_t compressed,
               std::uint64_t plain, bool deflated, const std::string& target,
               std::uint64_t& written) {
    const std::string staging = target + ".part";
    std::FILE* out = std::fopen(staging.c_str(), "wb");
    if (!out)
        return false;

    std::vector<std::uint8_t> inBuf(kCopyChunk);
    std::vector<std::uint8_t> outBuf(deflated ? kCopyChunk : 0);
    bool ok = true;
    written = 0;

    z_stream zs{};
    // Negative window bits: the payload is a bare deflate stream with no zlib
    // header, which is what zip stores.
    if (deflated && inflateInit2(&zs, -MAX_WBITS) != Z_OK) {
        std::fclose(out);
        return false;
    }

    std::uint64_t remaining = compressed;
    if (std::fseek(src, (long)offset, SEEK_SET) != 0)
        ok = false;

    while (ok && remaining > 0) {
        const std::size_t want = (std::size_t)std::min<std::uint64_t>(remaining, inBuf.size());
        if (std::fread(inBuf.data(), 1, want, src) != want) {
            ok = false;
            break;
        }
        remaining -= want;

        if (!deflated) {
            if (std::fwrite(inBuf.data(), 1, want, out) != want) {
                ok = false;
                break;
            }
            written += want;
            continue;
        }

        zs.next_in = inBuf.data();
        zs.avail_in = (uInt)want;
        while (zs.avail_in > 0) {
            zs.next_out = outBuf.data();
            zs.avail_out = (uInt)outBuf.size();
            const int rc = inflate(&zs, Z_NO_FLUSH);
            if (rc != Z_OK && rc != Z_STREAM_END && rc != Z_BUF_ERROR) {
                ok = false;
                break;
            }
            const std::size_t produced = outBuf.size() - zs.avail_out;
            if (produced > 0) {
                if (std::fwrite(outBuf.data(), 1, produced, out) != produced) {
                    ok = false;
                    break;
                }
                written += produced;
            }
            if (rc == Z_STREAM_END)
                break;
            if (produced == 0 && rc == Z_BUF_ERROR)
                break;
        }
    }

    if (deflated)
        inflateEnd(&zs);
    if (std::fclose(out) != 0)
        ok = false;
    if (ok && written != plain)
        ok = false;                      // truncado ou corrompido
    if (ok) {
        // rename() replaces the destination in one step, so a reader never sees
        // a half-written file. If it fails -- the file is open elsewhere, say --
        // the original is still there, untouched.
        std::remove(target.c_str());
        if (std::rename(staging.c_str(), target.c_str()) != 0)
            ok = false;
    }
    if (!ok)
        std::remove(staging.c_str());
    return ok;
}

} // namespace

namespace themeshop {

static ZipExtractResult walkArchive(const std::string& archivePath,
                                    const std::string& destinationDir,
                                    const std::function<void(int, int)>& onProgress,
                                    const ZipExtractPolicy& policy,
                                    bool inspectOnly);

ZipExtractResult inspectZipFile(const std::string& archivePath,
                                const ZipExtractPolicy& policy) {
    return walkArchive(archivePath, std::string(), {}, policy, true);
}

ZipExtractResult extractZipFile(const std::string& archivePath,
                                const std::string& destinationDir,
                                const std::function<void(int, int)>& onProgress,
                                const ZipExtractPolicy& policy) {
    return walkArchive(archivePath, destinationDir, onProgress, policy, false);
}

static ZipExtractResult walkArchive(const std::string& archivePath,
                                    const std::string& destinationDir,
                                    const std::function<void(int, int)>& onProgress,
                                    const ZipExtractPolicy& policy,
                                    bool inspectOnly) {
    ZipExtractResult result;

    std::FILE* f = std::fopen(archivePath.c_str(), "rb");
    if (!f) {
        result.error = "could not open the downloaded package";
        return result;
    }
    std::fseek(f, 0, SEEK_END);
    const std::uint64_t size = (std::uint64_t)std::ftell(f);
    if (size < 22) {
        std::fclose(f);
        result.error = "package is too small to be an archive";
        return result;
    }

    // The end record sits at the tail, after a comment of unknown length, so it
    // is found by scanning backwards over the last stretch of the file.
    const std::uint64_t tailSize = std::min<std::uint64_t>(size, 66000);
    std::vector<std::uint8_t> tail((std::size_t)tailSize);
    if (!readAt(f, size - tailSize, tail.data(), tail.size())) {
        std::fclose(f);
        result.error = "could not read the end of the package";
        return result;
    }

    std::int64_t eocdInTail = -1;
    for (std::int64_t i = (std::int64_t)tail.size() - 22; i >= 0; --i) {
        if (read32(tail.data() + i) == kEndOfCentralDirectory) {
            eocdInTail = i;
            break;
        }
    }
    if (eocdInTail < 0) {
        std::fclose(f);
        result.error = "no end-of-central-directory record";
        return result;
    }

    const std::uint8_t* eocd = tail.data() + eocdInTail;
    const int entryCount = (int)read16(eocd + 10);
    const std::uint32_t dirSize = read32(eocd + 12);
    const std::uint32_t dirOffset = read32(eocd + 16);

    if (entryCount <= 0 || entryCount > kMaxEntries) {
        std::fclose(f);
        result.error = "archive declares an implausible number of files";
        return result;
    }
    if ((std::uint64_t)dirOffset + dirSize > size || dirSize == 0) {
        std::fclose(f);
        result.error = "central directory points outside the archive";
        return result;
    }

    std::vector<std::uint8_t> dir(dirSize);
    if (!readAt(f, dirOffset, dir.data(), dir.size())) {
        std::fclose(f);
        result.error = "could not read the central directory";
        return result;
    }

    std::error_code ec;
    if (!inspectOnly)
        std::filesystem::create_directories(destinationDir, ec);
    if (ec) {
        std::fclose(f);
        result.error = "could not create extraction directory";
        return result;
    }
    std::uint64_t walker = 0;
    std::uint8_t localHeader[30];
    std::unordered_set<std::string> seenPaths;

    for (int i = 0; i < entryCount; ++i) {
        if (walker + 46 > dir.size() || read32(dir.data() + walker) != kCentralFileHeader) {
            result.error = "central directory entry is malformed";
            break;
        }

        const std::uint16_t method     = read16(dir.data() + walker + 10);
        const std::uint32_t compressed = read32(dir.data() + walker + 20);
        const std::uint32_t plain      = read32(dir.data() + walker + 24);
        const std::uint16_t nameLen    = read16(dir.data() + walker + 28);
        const std::uint16_t extraLen   = read16(dir.data() + walker + 30);
        const std::uint16_t commentLen = read16(dir.data() + walker + 32);
        const std::uint32_t localOff   = read32(dir.data() + walker + 42);

        if (walker + 46 + nameLen > dir.size()) {
            result.error = "entry name runs past the central directory";
            break;
        }
        std::string rawName((const char*)(dir.data() + walker + 46), nameLen);
        walker += 46u + nameLen + extraLen + commentLen;

        if (!rawName.empty() && (rawName.back() == '/' || rawName.back() == '\\'))
            continue;                                   // directory record

        std::string relative;
        if (!safeRelativePath(rawName, relative)) {
            result.error = "refusing entry that escapes the theme folder: " + rawName;
            break;
        }

        if (!relative.empty() && (relative.back() == '/' || relative.back() == '\\'))
            continue;                                   // directory record
        if (!policy.allowExecutablePayload && !allowedExtension(relative)) {
            result.error = "refusing file type: " + relative;
            break;
        }
        if (!policy.requiredRoots.empty()) {
            bool inside = false;
            for (const auto& root : policy.requiredRoots) {
                if (relative.rfind(root, 0) == 0) { inside = true; break; }
            }
            if (!inside) {
                result.error = "entry outside the allowed folders: " + relative;
                break;
            }
        }
        if (!policy.onlyRoots.empty()) {
            bool wanted = false;
            for (const auto& root : policy.onlyRoots) {
                if (relative.rfind(root, 0) == 0) { wanted = true; break; }
            }
            if (!wanted)
                continue;   // somebody else's half of this archive
        }
        if (!seenPaths.insert(relative).second) {
            result.error = "archive has duplicate file: " + relative;
            break;
        }
        if (plain > kMaxEntryBytes || result.bytesWritten + plain > kMaxTotalBytes) {
            result.error = "package is larger than a theme is allowed to be";
            break;
        }
        if (method != kMethodStored && method != kMethodDeflate) {
            result.error = "unsupported compression in " + relative;
            break;
        }

        if (!readAt(f, localOff, localHeader, sizeof(localHeader))
            || read32(localHeader) != kLocalFileHeader) {
            result.error = "local header is malformed for " + relative;
            break;
        }
        // The local header repeats the name and extra lengths, and they may
        // differ from the central directory's -- the data starts after the
        // local ones, so those are the values that matter here.
        const std::uint64_t dataOffset =
            (std::uint64_t)localOff + 30 + read16(localHeader + 26) + read16(localHeader + 28);
        if (dataOffset + compressed > size) {
            result.error = "file data runs past the end of the archive: " + relative;
            break;
        }

        if (inspectOnly) {
            // Every check above has run; writing is the only thing skipped.
            ++result.filesWritten;
            result.bytesWritten += plain;
            continue;
        }

        const std::filesystem::path target = std::filesystem::path(destinationDir) / relative;
        std::filesystem::create_directories(target.parent_path(), ec);
        if (ec) {
            result.error = "could not create parent directory for " + relative;
            break;
        }

        // safeRelativePath() rejects absolute paths, drive prefixes, and dot
        // segments before this point. Do not use weakly_canonical here: the
        // libnx filesystem does not reliably resolve sdmc: paths.

        std::uint64_t written = 0;
        if (!copyEntry(f, dataOffset, compressed, plain,
                       method == kMethodDeflate, target.string(), written)) {
            result.error = "could not unpack " + relative;
            break;
        }

        result.filesWritten += 1;
        result.bytesWritten += written;
        if (onProgress)
            onProgress(i + 1, entryCount);
    }

    std::fclose(f);

    if (!result.error.empty())
        return result;
    if (result.filesWritten == 0) {
        result.error = "archive contained no usable files";
        return result;
    }

    DebugLog::log("[themeshop] package extracted: %d files, %.1f MB",
                  result.filesWritten, result.bytesWritten / 1048576.0);
    result.success = true;
    return result;
}

} // namespace themeshop
