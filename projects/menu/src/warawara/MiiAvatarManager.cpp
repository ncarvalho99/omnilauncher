#include "MiiAvatarManager.hpp"
#include "core/DebugLog.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <random>

namespace warawara {

namespace {

static std::string utf16ToUtf8(const uint16_t* u16, size_t maxLen) {
    std::string out;
    for (size_t i = 0; i < maxLen && u16[i] != 0; ++i) {
        uint16_t c = u16[i];
        if (c < 0x80) {
            out.push_back(static_cast<char>(c));
        } else if (c < 0x800) {
            out.push_back(static_cast<char>(0xC0 | ((c >> 6) & 0x1F)));
            out.push_back(static_cast<char>(0x80 | (c & 0x3F)));
        } else {
            out.push_back(static_cast<char>(0xE0 | ((c >> 12) & 0x0F)));
            out.push_back(static_cast<char>(0x80 | ((c >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (c & 0x3F)));
        }
    }
    return out;
}

} // namespace

nxui::Color getFavoriteColorRgb(MiiFavoriteColor color) {
    switch (color) {
        case MiiFavoriteColor::Red:    return {0.847f, 0.157f, 0.000f, 1.0f}; // #D82800
        case MiiFavoriteColor::Orange: return {0.973f, 0.408f, 0.000f, 1.0f}; // #F86800
        case MiiFavoriteColor::Yellow: return {0.973f, 0.816f, 0.000f, 1.0f}; // #F8D000
        case MiiFavoriteColor::Lime:   return {0.502f, 0.816f, 0.063f, 1.0f}; // #80D010
        case MiiFavoriteColor::Green:  return {0.000f, 0.533f, 0.125f, 1.0f}; // #008820
        case MiiFavoriteColor::Blue:   return {0.000f, 0.408f, 0.784f, 1.0f}; // #0068C8
        case MiiFavoriteColor::Cyan:   return {0.188f, 0.690f, 0.910f, 1.0f}; // #30B0E8
        case MiiFavoriteColor::Pink:   return {0.973f, 0.471f, 0.627f, 1.0f}; // #F878A0
        case MiiFavoriteColor::Purple: return {0.439f, 0.157f, 0.659f, 1.0f}; // #7028A8
        case MiiFavoriteColor::Brown:  return {0.345f, 0.188f, 0.094f, 1.0f}; // #583018
        case MiiFavoriteColor::White:  return {0.910f, 0.910f, 0.910f, 1.0f}; // #E8E8E8
        case MiiFavoriteColor::Black:  return {0.157f, 0.157f, 0.157f, 1.0f}; // #282828
        default:                       return {0.847f, 0.157f, 0.000f, 1.0f};
    }
}

void MiiAvatarManager::clear() {
    m_avatars.clear();
    m_guestTextures.clear();
    m_fallbackTexture.reset();
    m_initialized = false;
    m_randomCursor = 0;
}

void MiiAvatarManager::initialize(nxui::GpuDevice& gpu, nxui::Renderer& ren, const std::string& assetBase) {
    if (m_initialized) return;

    DebugLog::log("[warawara] Initializing MiiAvatarManager (assetBase: %s)...", assetBase.c_str());

    // 1. Create guaranteed procedural fallback texture first
    ensureFallback(gpu, ren);

    // 2. Load bundled authentic guest Mii face sprites
    loadBundledGuests(gpu, ren, assetBase);

    // 3. Extract local console user account profile avatars (VIP Miis)
    loadUserAccounts(gpu, ren);

    // 4. Query system Mii database (mii:u / mii:e) if available on Horizon
    loadSystemMiiDatabase(gpu, ren, assetBase);

    // 5. If for any reason no avatars are present, add fallback guest
    if (m_avatars.empty()) {
        MiiAvatarData fb;
        fb.nickname = "Player";
        fb.shirtColor = MiiFavoriteColor::Blue;
        fb.headTexture = m_fallbackTexture;
        fb.source = "fallback";
        m_avatars.push_back(std::move(fb));
    }

    m_initialized = true;
    DebugLog::log("[warawara] MiiAvatarManager initialized: %zu avatars ready.", m_avatars.size());
}

void MiiAvatarManager::loadUserAccounts(nxui::GpuDevice& gpu, nxui::Renderer& ren) {
#ifdef __SWITCH__
    AccountUid uids[8] = {};
    s32 count = 0;
    Result rc = accountListAllUsers(uids, 8, &count);
    if (R_FAILED(rc) || count <= 0) {
        DebugLog::log("[warawara] accountListAllUsers rc=0x%X (count=%d)", rc, count);
        return;
    }

    DebugLog::log("[warawara] Extracting %d user profile accounts...", count);
    for (int i = 0; i < count; ++i) {
        AccountProfile profile{};
        rc = accountGetProfile(&profile, uids[i]);
        if (R_FAILED(rc)) continue;

        AccountProfileBase base{};
        AccountUserData userData{};
        std::string nickname = "Player";
        if (R_SUCCEEDED(accountProfileGet(&profile, &userData, &base))) {
            if (base.nickname[0] != '\0') {
                nickname = base.nickname;
            }
        }

        std::shared_ptr<nxui::Texture> avatarTex;
        u32 imgSize = 0;
        if (R_SUCCEEDED(accountProfileGetImageSize(&profile, &imgSize)) && imgSize > 0) {
            std::vector<uint8_t> imgBuf(imgSize);
            u32 realSize = 0;
            if (R_SUCCEEDED(accountProfileLoadImage(&profile, imgBuf.data(), imgSize, &realSize)) && realSize > 0) {
                avatarTex = std::make_shared<nxui::Texture>();
                if (!avatarTex->loadFromMemory(gpu, ren, imgBuf.data(), realSize, 96)) {
                    avatarTex.reset();
                }
            }
        }
        accountProfileClose(&profile);

        if (!avatarTex) {
            // Assign fallback or first guest texture
            avatarTex = (!m_guestTextures.empty()) ? m_guestTextures[i % m_guestTextures.size()] : m_fallbackTexture;
        }

        MiiAvatarData userMii;
        userMii.nickname = nickname;
        userMii.isUserAccount = true;
        userMii.accountUid = uids[i];
        userMii.headTexture = avatarTex;
        // Assign distinct vibrant favorite shirt color for each account profile
        userMii.shirtColor = static_cast<MiiFavoriteColor>((i * 5 + 5) % 12);
        userMii.gender = (i % 2);
        userMii.height = 64;
        userMii.build = 64;
        userMii.source = "account";

        m_avatars.insert(m_avatars.begin(), std::move(userMii)); // Place VIP user accounts at the front
    }
#endif
}

void MiiAvatarManager::loadBundledGuests(nxui::GpuDevice& gpu, nxui::Renderer& ren, const std::string& assetBase) {
    struct GuestDef {
        const char* filename;
        const char* name;
        MiiFavoriteColor color;
        uint8_t gender;
    };

    static const GuestDef kGuests[] = {
        {"guest_01.png", "Alex",     MiiFavoriteColor::Red,    0},
        {"guest_02.png", "Sam",      MiiFavoriteColor::Blue,   0},
        {"guest_03.png", "Charlie",  MiiFavoriteColor::Pink,   1},
        {"guest_04.png", "Jordan",   MiiFavoriteColor::Green,  0},
        {"guest_05.png", "Taylor",   MiiFavoriteColor::Cyan,   1},
        {"guest_06.png", "Morgan",   MiiFavoriteColor::Yellow, 0},
        {"guest_07.png", "Casey",    MiiFavoriteColor::Orange, 0},
        {"guest_08.png", "Riley",    MiiFavoriteColor::Purple, 1},
        {"guest_09.png", "MarioFan", MiiFavoriteColor::Red,    0},
        {"guest_10.png", "Avery",    MiiFavoriteColor::Brown,  0},
        {"guest_11.png", "Sensei",   MiiFavoriteColor::White,  0},
        {"guest_12.png", "Quinn",    MiiFavoriteColor::Lime,   1},
        {"guest_13.png", "Kai",      MiiFavoriteColor::Black,  0},
        {"guest_14.png", "Skyler",   MiiFavoriteColor::Pink,   1},
        {"guest_15.png", "Dakota",   MiiFavoriteColor::Blue,   0},
        {"guest_16.png", "Rowan",    MiiFavoriteColor::Orange, 0},
    };

    m_guestTextures.reserve(16);

    for (const auto& g : kGuests) {
        std::string fullPath = assetBase + "/avatars/" + g.filename;
        auto tex = std::make_shared<nxui::Texture>();
        bool loaded = tex->loadFromFile(gpu, ren, fullPath, 96);
        if (!loaded) {
            // Fallback path check
            std::string fallbackPath = std::string("romfs:/avatars/") + g.filename;
            loaded = tex->loadFromFile(gpu, ren, fallbackPath, 96);
        }
        if (!loaded) {
            std::string sdPath = std::string("sdmc:/switch/OmniLaunch/avatars/") + g.filename;
            loaded = tex->loadFromFile(gpu, ren, sdPath, 96);
        }

        if (loaded) {
            m_guestTextures.push_back(tex);

            MiiAvatarData guest;
            guest.nickname = g.name;
            guest.shirtColor = g.color;
            guest.isUserAccount = false;
            guest.headTexture = tex;
            guest.gender = g.gender;
            guest.height = 64;
            guest.build = 64;
            guest.source = "guest";
            m_avatars.push_back(std::move(guest));
        }
    }

    DebugLog::log("[warawara] Loaded %zu bundled guest avatar textures.", m_guestTextures.size());
}

void MiiAvatarManager::loadSystemMiiDatabase(nxui::GpuDevice& /*gpu*/, nxui::Renderer& /*ren*/, const std::string& /*assetBase*/) {
#ifdef __SWITCH__
    Result rc = miiInitialize(MiiServiceType_System);
    if (R_FAILED(rc)) {
        rc = miiInitialize(MiiServiceType_User);
    }
    if (R_FAILED(rc)) {
        DebugLog::log("[warawara] miiInitialize failed: 0x%X", rc);
        return;
    }

    MiiDatabase db{};
    rc = miiOpenDatabase(&db, MiiSpecialKeyCode_Normal);
    if (R_FAILED(rc)) {
        DebugLog::log("[warawara] miiOpenDatabase failed: 0x%X", rc);
        miiExit();
        return;
    }

    s32 totalCount = 0;
    rc = miiDatabaseGetCount(&db, &totalCount, MiiSourceFlag_All);
    if (R_SUCCEEDED(rc) && totalCount > 0) {
        s32 fetchCount = std::min(totalCount, 32);
        std::vector<MiiCharInfo> charInfos(fetchCount);
        s32 outCount = 0;
        rc = miiDatabaseGet1(&db, MiiSourceFlag_All, charInfos.data(), fetchCount, &outCount);
        if (R_SUCCEEDED(rc)) {
            DebugLog::log("[warawara] Extracted %d system Mii records from database.", outCount);
            for (s32 i = 0; i < outCount; ++i) {
                const auto& ci = charInfos[i];
                std::string nick = utf16ToUtf8(ci.mii_name, 11);
                static const char* kFriendlyNames[] = {
                    "Alex", "Sam", "Charlie", "Jordan", "Taylor", "Morgan",
                    "Casey", "Riley", "MarioFan", "Avery", "Sensei", "Quinn",
                    "Kai", "Skyler", "Dakota", "Rowan", "Nico", "Robin"
                };
                if (nick.empty() || nick == "no name" || nick == "Mii" || nick == "???") {
                    nick = kFriendlyNames[(static_cast<size_t>(ci.mii_color) + static_cast<size_t>(i)) %
                                          (sizeof(kFriendlyNames) / sizeof(kFriendlyNames[0]))];
                }

                // Check if an avatar with this nickname is already loaded
                bool duplicate = false;
                for (const auto& a : m_avatars) {
                    if (a.nickname == nick) {
                        duplicate = true;
                        break;
                    }
                }
                if (duplicate) {
                    // Try another friendly name to prevent dropping valid database Miis
                    for (const char* candidateName : kFriendlyNames) {
                        bool nameTaken = false;
                        for (const auto& a : m_avatars) {
                            if (a.nickname == candidateName) {
                                nameTaken = true;
                                break;
                            }
                        }
                        if (!nameTaken) {
                            nick = candidateName;
                            duplicate = false;
                            break;
                        }
                    }
                }
                if (duplicate) continue;

                MiiAvatarData dbMii;
                dbMii.nickname = nick;
                dbMii.shirtColor = static_cast<MiiFavoriteColor>(ci.mii_color % 12);
                dbMii.gender = ci.mii_sex;
                dbMii.height = ci.mii_height;
                dbMii.build = ci.mii_width;
                dbMii.isUserAccount = false;
                dbMii.source = "database";

                // Assign matching guest head texture based on color/index
                if (!m_guestTextures.empty()) {
                    size_t texIdx = (static_cast<size_t>(ci.mii_color) + static_cast<size_t>(i)) % m_guestTextures.size();
                    dbMii.headTexture = m_guestTextures[texIdx];
                } else {
                    dbMii.headTexture = m_fallbackTexture;
                }

                m_avatars.push_back(std::move(dbMii));
            }
        }
    }

    miiDatabaseClose(&db);
    miiExit();
#endif
}

void MiiAvatarManager::ensureFallback(nxui::GpuDevice& gpu, nxui::Renderer& ren) {
    if (m_fallbackTexture) return;

    // Generate a clean 64x64 smiling face texture in RGBA memory
    constexpr int kSide = 64;
    std::vector<uint8_t> pixels(kSide * kSide * 4, 0);

    const float cx = kSide * 0.5f;
    const float cy = kSide * 0.5f;
    const float rHead = kSide * 0.44f;

    for (int y = 0; y < kSide; ++y) {
        for (int x = 0; x < kSide; ++x) {
            float dx = x - cx;
            float dy = y - cy;
            float dist = std::sqrt(dx * dx + dy * dy);
            int idx = (y * kSide + x) * 4;

            if (dist <= rHead) {
                // Peach skin tone #FFE0BD
                pixels[idx + 0] = 255;
                pixels[idx + 1] = 224;
                pixels[idx + 2] = 189;
                pixels[idx + 3] = 255;

                // Eyes: (cx - 10, cy - 4) and (cx + 10, cy - 4)
                float dEyeL = std::sqrt((dx + 10.f) * (dx + 10.f) + (dy + 4.f) * (dy + 4.f));
                float dEyeR = std::sqrt((dx - 10.f) * (dx - 10.f) + (dy + 4.f) * (dy + 4.f));
                if (dEyeL <= 3.5f || dEyeR <= 3.5f) {
                    pixels[idx + 0] = 30;
                    pixels[idx + 1] = 30;
                    pixels[idx + 2] = 30;
                }

                // Smile arc at cy + 8
                if (dy >= 6.f && dy <= 11.f && std::abs(dx) <= 12.f) {
                    float arcY = 6.f + (dx * dx) / 28.f;
                    if (std::abs(dy - arcY) <= 1.5f) {
                        pixels[idx + 0] = 40;
                        pixels[idx + 1] = 25;
                        pixels[idx + 2] = 25;
                    }
                }
            } else if (dist <= rHead + 1.2f) {
                // Smooth edge antialiasing
                float alpha = (rHead + 1.2f - dist) / 1.2f;
                pixels[idx + 0] = 255;
                pixels[idx + 1] = 224;
                pixels[idx + 2] = 189;
                pixels[idx + 3] = static_cast<uint8_t>(255.f * alpha);
            }
        }
    }

    m_fallbackTexture = std::make_shared<nxui::Texture>();
    m_fallbackTexture->loadFromPixels(gpu, ren, pixels.data(), kSide, kSide);
}

const MiiAvatarData& MiiAvatarManager::getRandomAvatar() {
    if (m_avatars.empty()) {
        static const MiiAvatarData s_dummy;
        return s_dummy;
    }
    // Rotate cursor through pool with a prime step
    m_randomCursor = (m_randomCursor + 7) % m_avatars.size();
    return m_avatars[m_randomCursor];
}

const MiiAvatarData* MiiAvatarManager::getUserAvatar(const AccountUid& uid) const {
    for (const auto& a : m_avatars) {
        if (a.isUserAccount) {
#ifdef __SWITCH__
            if (accountUidIsValid(&uid) && accountUidIsValid(&a.accountUid)) {
                if (std::memcmp(&uid, &a.accountUid, sizeof(AccountUid)) == 0) {
                    return &a;
                }
            }
#else
            if (a.accountUid == uid) return &a;
#endif
        }
    }
    return nullptr;
}

} // namespace warawara
