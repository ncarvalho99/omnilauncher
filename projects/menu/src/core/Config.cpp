#include "Config.hpp"
#include <cstdlib>
#include <cstdio>
#include <fstream>
#include <algorithm>
#include <filesystem>
#include <nlohmann/json.hpp>
#include <system_error>

namespace {

bool isGamePortPlatform(const std::string& platform) {
    static constexpr const char* kPlatforms[] = {
        "nintendo-switch", "pc", "playstation-5", "playstation-4", "xbox-series-x",
        "xbox-one", "xbox-360", "playstation-3", "playstation-2", "playstation",
        "gamecube", "wii", "wii-u", "nintendo-64", "super-nintendo", "nes",
        "game-boy-advance", "nintendo-ds", "nintendo-3ds", "psp", "playstation-vita",
        "dreamcast", "sega-genesis",
    };
    for (const char* candidate : kPlatforms)
        if (platform == candidate) return true;
    return false;
}

template <typename T>
void readJsonOpt(const nlohmann::json& j, const char* key, T& out) {
    auto it = j.find(key);
    if (it != j.end() && !it->is_null()) {
        try {
            out = it->get<T>();
        } catch (...) {
        }
    }
}

} // namespace

bool AppConfig::load() {
    // The previous copy is tried when the current one is missing or unreadable.
    // A crash used to cost the player every setting they had ever chosen --
    // including tutorialCompleted, which is why the tutorial greeted them again
    // after the theme crash. Losing the last change is acceptable; losing the
    // whole file is not.
    nlohmann::json j;
    bool parsed = false;
    for (const char* path : {kConfigPath, kBackupPath}) {
        std::ifstream f(path);
        if (!f.is_open()) continue;
        try {
            f >> j;
            parsed = true;
            break;
        } catch (...) {
            // Truncated or half-written: fall through to the backup.
        }
    }
    if (!parsed) return false;

    readJsonOpt(j, "musicEnabled", musicEnabled);
    readJsonOpt(j, "customBgmEnabled", customBgmEnabled);
    readJsonOpt(j, "customBgmShuffle", customBgmShuffle);
    readJsonOpt(j, "audioSourcePreference", audioSourcePreference);
    readJsonOpt(j, "musicVolume", musicVolume);
    readJsonOpt(j, "sfxVolume", sfxVolume);
    readJsonOpt(j, "gridColumns", gridColumns);
    readJsonOpt(j, "gridRows", gridRows);
    readJsonOpt(j, "dynamicPages", dynamicPages);
    {
        std::string mode;
        readJsonOpt(j, "appLayoutMode", mode);
        if (mode == "dynamic_line" || mode == "line")
            appLayoutMode = AppLayoutMode::DynamicLine;
        else if (mode == "flow")
            appLayoutMode = AppLayoutMode::Flow;
        else if (mode == "shelf")
            appLayoutMode = AppLayoutMode::Shelf;
        else if (mode == "deck")
            appLayoutMode = AppLayoutMode::Deck;
        else if (mode == "cover")
            appLayoutMode = AppLayoutMode::Cover;
        else if (mode == "xmb")
            appLayoutMode = AppLayoutMode::Xmb;
        else if (mode == "grid")
            appLayoutMode = AppLayoutMode::Grid;
    }
    readJsonOpt(j, "actionHintStyle", actionHintStyle);
    readJsonOpt(j, "uiLanguageOverride", uiLanguageOverride);
    readJsonOpt(j, "soundPreset", soundPreset);
    readJsonOpt(j, "defaultProfileEnabled", defaultProfileEnabled);
    readJsonOpt(j, "defaultProfileUid", defaultProfileUid);
    readJsonOpt(j, "tutorialCompleted", tutorialCompleted);
    readJsonOpt(j, "lastUpdateCheckDay", lastUpdateCheckDay);
    readJsonOpt(j, "clockUse12Hour", clockUse12Hour);
    readJsonOpt(j, "backgroundBlur", backgroundBlur);
    readJsonOpt(j, "glassSharpness", glassSharpness);
    readJsonOpt(j, "backgroundSpeed", backgroundSpeed);
    readJsonOpt(j, "backgroundSpeedChosen", backgroundSpeedChosen);
    readJsonOpt(j, "accessibilityEnabled", accessibilityEnabled);
    readJsonOpt(j, "accessibilitySpeakHints", accessibilitySpeakHints);
    readJsonOpt(j, "accessibilitySpeakContextEveryFocus", accessibilitySpeakContextEveryFocus);
    readJsonOpt(j, "accessibilitySpeakPosition", accessibilitySpeakPosition);
    readJsonOpt(j, "accessibilitySpeechRate", accessibilitySpeechRate);
    readJsonOpt(j, "steamGridDbEnabled", steamGridDbEnabled);
    readJsonOpt(j, "steamGridDbApiKey", steamGridDbApiKey);
    readJsonOpt(j, "ytdlBackendUrl", ytdlBackendUrl);
    readJsonOpt(j, "themePreset", themePreset);
    readJsonOpt(j, "lastPageTitleId", lastPageTitleId);
    readJsonOpt(j, "sortMode", sortMode);
    readJsonOpt(j, "lastOpenedSequence", lastOpenedSequence);
    lastOpened.clear();
    if (auto it = j.find("lastOpened"); it != j.end() && it->is_object()) {
        for (auto& [k, v] : it->items()) {
            if (!v.is_number_unsigned()) continue;
            lastOpened.emplace_back(std::strtoull(k.c_str(), nullptr, 16),
                                    v.get<std::uint64_t>());
        }
    }
    playtime.clear();
    if (auto it = j.find("playtime"); it != j.end() && it->is_object()) {
        for (auto& [k, v] : it->items()) {
            if (!v.is_number_unsigned()) continue;
            // Zero is kept: it means "asked, never played", which is what stops
            // the title being asked about again at every boot.
            const std::uint64_t ns = v.get<std::uint64_t>();
            playtime.emplace_back(std::strtoull(k.c_str(), nullptr, 16), ns);
        }
    }
    gamePortPlatforms.clear();
    if (auto it = j.find("gamePorts"); it != j.end() && it->is_object()) {
        for (auto& [k, v] : it->items()) {
            if (!v.is_string()) continue;
            const std::string platform = v.get<std::string>();
            if (!isGamePortPlatform(platform)) continue;
            gamePortPlatforms.emplace_back(std::strtoull(k.c_str(), nullptr, 16), platform);
        }
    }
    gamePortSearchTitles.clear();
    if (auto it = j.find("gamePortSearchTitles"); it != j.end() && it->is_object()) {
        for (auto& [k, v] : it->items()) {
            if (!v.is_string()) continue;
            const std::string title = v.get<std::string>();
            if (title.empty()) continue;
            gamePortSearchTitles.emplace_back(std::strtoull(k.c_str(), nullptr, 16), title);
        }
    }
    customTitles.clear();
    if (auto it = j.find("customTitles"); it != j.end() && it->is_object()) {
        for (auto& [k, v] : it->items()) {
            if (!v.is_string()) continue;
            const std::string title = v.get<std::string>();
            if (title.empty()) continue;
            customTitles.emplace_back(std::strtoull(k.c_str(), nullptr, 16), title);
        }
    }
    favoriteTitleIds.clear();
    if (auto it = j.find("favorites"); it != j.end() && it->is_array()) {
        for (auto& v : *it) {
            if (v.is_string()) {
                const std::string s = v.get<std::string>();
                if (!s.empty()) {
                    favoriteTitleIds.push_back(std::strtoull(s.c_str(), nullptr, 16));
                }
            } else if (v.is_number_unsigned()) {
                favoriteTitleIds.push_back(v.get<std::uint64_t>());
            }
        }
    }
    steamGridDbKnownTitles.clear();
    if (auto it = j.find("steamGridDbKnownTitles"); it != j.end() && it->is_array()) {
        for (auto& v : *it) {
            if (v.is_string()) {
                const std::string s = v.get<std::string>();
                if (!s.empty()) {
                    steamGridDbKnownTitles.push_back(std::strtoull(s.c_str(), nullptr, 16));
                }
            } else if (v.is_number_unsigned()) {
                steamGridDbKnownTitles.push_back(v.get<std::uint64_t>());
            }
        }
    }
    if (musicVolume < 0.f) musicVolume = 0.f;
    if (musicVolume > 1.f) musicVolume = 1.f;
    if (sfxVolume   < 0.f) sfxVolume   = 0.f;
    if (sfxVolume   > 1.f) sfxVolume   = 1.f;
    gridColumns = std::clamp(gridColumns, 3, 8);
    gridRows = std::clamp(gridRows, 2, 5);
    if (sortMode < 0 || sortMode >= kSortModeCount) sortMode = 0;
    if (actionHintStyle != "panel" && actionHintStyle != "capsules")
        actionHintStyle = "capsules";
    if (uiLanguageOverride.empty()) uiLanguageOverride = "auto";
    if (soundPreset.empty()) soundPreset = "wiiu";
    if (!defaultProfileEnabled) defaultProfileUid.clear();
    accessibilitySpeechRate = std::clamp(accessibilitySpeechRate, 120, 320);
    if (themePreset.empty()) themePreset = "Default Dark";

    return true;
}

bool AppConfig::save() const {
    std::error_code ec;
    std::filesystem::create_directory("sdmc:/config", ec);
    ec.clear();
    std::filesystem::create_directory(kConfigDir, ec);

    nlohmann::json j;
    j["musicEnabled"] = musicEnabled;
    j["customBgmEnabled"] = customBgmEnabled;
    j["customBgmShuffle"] = customBgmShuffle;
    j["audioSourcePreference"] = audioSourcePreference;
    j["musicVolume"] = musicVolume;
    j["sfxVolume"] = sfxVolume;
    j["gridColumns"] = std::clamp(gridColumns, 3, 8);
    j["gridRows"] = std::clamp(gridRows, 2, 5);
    j["dynamicPages"] = dynamicPages;
    // Stable strings, never enum ordinals: an installation written by a build
    // that knows Flow/Shelf/Deck must still read correctly on one that does not.
    j["appLayoutMode"] = appLayoutMode == AppLayoutMode::DynamicLine ? "dynamic_line"
                       : appLayoutMode == AppLayoutMode::Flow        ? "flow"
                       : appLayoutMode == AppLayoutMode::Shelf       ? "shelf"
                       : appLayoutMode == AppLayoutMode::Deck        ? "deck"
                       : appLayoutMode == AppLayoutMode::Cover       ? "cover"
                       : appLayoutMode == AppLayoutMode::Xmb         ? "xmb"
                                                                     : "grid";
    j["actionHintStyle"] = actionHintStyle == "panel" ? "panel" : "capsules";
    j["uiLanguageOverride"] = uiLanguageOverride;
    j["soundPreset"] = soundPreset;
    j["defaultProfileEnabled"] = defaultProfileEnabled;
    j["defaultProfileUid"] = defaultProfileEnabled ? defaultProfileUid : std::string();
    j["tutorialCompleted"] = tutorialCompleted;
    j["lastUpdateCheckDay"] = lastUpdateCheckDay;
    j["clockUse12Hour"] = clockUse12Hour;
    j["backgroundBlur"] = backgroundBlur;
    j["glassSharpness"] = glassSharpness;
    j["backgroundSpeed"] = backgroundSpeed;
    j["backgroundSpeedChosen"] = backgroundSpeedChosen;
    j["accessibilityEnabled"] = accessibilityEnabled;
    j["accessibilitySpeakHints"] = accessibilitySpeakHints;
    j["accessibilitySpeakContextEveryFocus"] = accessibilitySpeakContextEveryFocus;
    j["accessibilitySpeakPosition"] = accessibilitySpeakPosition;
    j["accessibilitySpeechRate"] = std::clamp(accessibilitySpeechRate, 120, 320);
    j["steamGridDbEnabled"] = steamGridDbEnabled;
    j["steamGridDbApiKey"] = steamGridDbApiKey;
    j["ytdlBackendUrl"] = ytdlBackendUrl;
    j["themePreset"] = themePreset;
    j["lastPageTitleId"] = lastPageTitleId;
    j["sortMode"] = sortMode;
    j["lastOpenedSequence"] = lastOpenedSequence;
    {
        nlohmann::json opened = nlohmann::json::object();
        char key[17];
        for (const auto& e : lastOpened) {
            std::snprintf(key, sizeof(key), "%016llX", (unsigned long long)e.first);
            opened[key] = e.second;
        }
        j["lastOpened"] = std::move(opened);
    }
    {
        nlohmann::json played = nlohmann::json::object();
        char key[17];
        for (const auto& e : playtime) {
            std::snprintf(key, sizeof(key), "%016llX", (unsigned long long)e.first);
            played[key] = e.second;
        }
        j["playtime"] = std::move(played);
    }
    {
        nlohmann::json ports = nlohmann::json::object();
        char key[17];
        for (const auto& port : gamePortPlatforms) {
            std::snprintf(key, sizeof(key), "%016llX", (unsigned long long)port.first);
            ports[key] = port.second;
        }
        j["gamePorts"] = std::move(ports);
    }
    {
        nlohmann::json searchTitles = nlohmann::json::object();
        char key[17];
        for (const auto& entry : gamePortSearchTitles) {
            std::snprintf(key, sizeof(key), "%016llX", (unsigned long long)entry.first);
            searchTitles[key] = entry.second;
        }
        j["gamePortSearchTitles"] = std::move(searchTitles);
    }
    {
        nlohmann::json titles = nlohmann::json::object();
        char key[17];
        for (const auto& entry : customTitles) {
            std::snprintf(key, sizeof(key), "%016llX", (unsigned long long)entry.first);
            titles[key] = entry.second;
        }
        j["customTitles"] = std::move(titles);
    }
    {
        nlohmann::json favorites = nlohmann::json::array();
        char key[17];
        for (std::uint64_t tid : favoriteTitleIds) {
            std::snprintf(key, sizeof(key), "%016llX", (unsigned long long)tid);
            favorites.push_back(std::string(key));
        }
        j["favorites"] = std::move(favorites);
    }
    {
        nlohmann::json known = nlohmann::json::array();
        char key[17];
        for (std::uint64_t tid : steamGridDbKnownTitles) {
            std::snprintf(key, sizeof(key), "%016llX", (unsigned long long)tid);
            known.push_back(std::string(key));
        }
        j["steamGridDbKnownTitles"] = std::move(known);
    }

    // Written beside the real file and swapped in, never over it. Truncating
    // the live config and then dying mid-write is how a crash used to reset
    // every setting to its default: load() found a half-written file, failed to
    // parse it, and started from scratch. The staging file absorbs that risk.
    const std::string staging = std::string(kConfigPath) + ".part";

    {
        std::ofstream f(staging, std::ios::trunc);
        if (!f.is_open()) return false;
        f << j.dump(2);
        f.flush();
        if (!f.good()) {
            f.close();
            std::remove(staging.c_str());
            return false;
        }
    }

    // The outgoing copy becomes the backup rather than being deleted, so the
    // window in which neither file is complete costs nothing.
    std::remove(kBackupPath);
    std::rename(kConfigPath, kBackupPath);   // absent on the very first save
    if (std::rename(staging.c_str(), kConfigPath) != 0) {
        std::remove(staging.c_str());
        return false;
    }
    // No commit here: save() is submitted to the thread pool, so this ran on a
    // worker while the main thread was also writing. Committing an fs session
    // from two threads at once is its own hazard, and the author's build —
    // which does not corrupt — commits nowhere.
    return true;
}
