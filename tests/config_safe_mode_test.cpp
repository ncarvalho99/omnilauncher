#include "core/Config.hpp"
#include <cassert>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <nlohmann/json.hpp>

static nlohmann::json readCfg(const char* path) {
    std::ifstream f(path);
    nlohmann::json j;
    f >> j;
    return j;
}

int main() {
    // The real paths are sdmc:/...; the test runs with a relative "sdmc:" dir.
    std::system("mkdir -p 'sdmc:/config/OmniLaunch'");
    const char* kPath = AppConfig::kConfigPath;

    AppConfig player;
    player.themePreset = "package:some-video-theme";
    player.soundPreset = "custom-pack";
    player.musicEnabled = true;
    player.customBgmEnabled = true;
    assert(player.save());

    // Safe mode run: load, override, save (as the menu does many times).
    AppConfig run;
    assert(run.load());
    run.enterSafeMode();
    assert(run.themePreset == "Default Dark" && !run.musicEnabled && run.soundPreset == "wiiu");
    run.gridColumns = 6;                 // an unrelated change the player makes
    assert(run.save());
    {
        auto j = readCfg(kPath);
        assert(j["themePreset"] == "package:some-video-theme");   // untouched
        assert(j["soundPreset"] == "custom-pack");
        assert(j["musicEnabled"] == true && j["customBgmEnabled"] == true);
        assert(j["gridColumns"] == 6);                             // real change kept
    }
    // The player picks a theme and turns music on while in safe mode: honoured.
    run.themePreset = "package:other";
    run.musicEnabled = true;
    assert(run.save());
    {
        auto j = readCfg(kPath);
        assert(j["themePreset"] == "package:other" && j["musicEnabled"] == true);
    }
    // Choosing the safe value on purpose after changing away from it stays chosen.
    run.musicEnabled = false;
    run.themePreset = "Default Dark";
    assert(run.save());
    {
        auto j = readCfg(kPath);
        assert(j["musicEnabled"] == false && j["themePreset"] == "Default Dark");
    }
    // A normal (non-safe) config is unaffected by the logic.
    AppConfig normal;
    normal.themePreset = "Default Dark";
    normal.musicEnabled = false;
    assert(normal.save());
    {
        auto j = readCfg(kPath);
        assert(j["themePreset"] == "Default Dark" && j["musicEnabled"] == false);
    }
    std::puts("CONFIG_SAFE_MODE_TEST_PASS");
    return 0;
}
