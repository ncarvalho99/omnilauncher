#include "WiiUMenuApp.hpp"
#include "video/VideoPlayer.hpp"
#include "themeshop/ThemePackageInstaller.hpp"
#include "settings/SettingsGlassTuning.hpp"
#include "widgets/GlossyIcon.hpp"
#include "DebugLog.hpp"

#include <switchu/fs_remove.hpp>
#include <switchu/sd_commit.hpp>
#include <switchu/self_uninstall.hpp>

#include <nxui/core/I18n.hpp>

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cctype>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <cstdio>
#include <nlohmann/json.hpp>
#include <system_error>
#include <utility>

namespace {

// Deleting a theme goes through the POSIX walk in switchu/fs_remove.hpp.
//
// This used std::filesystem::remove_all and discarded the error, which on the
// console meant the folder simply stayed. Removing a live wallpaper freed the
// space in the list and nothing on the card.
bool removeDirectoryRecursive(const std::string& path, std::string* failedPath = nullptr) {
    return switchu::removeRecursive(path, failedPath);
}

bool pathExists(const std::string& path) {
    if (path.empty())
        return false;

    std::error_code ec;
    return std::filesystem::exists(path, ec);
}

bool stageSelfUninstallRequest() {
    namespace uninstall = switchu::self_uninstall;

    std::error_code ec;
    std::filesystem::create_directories(uninstall::kDirectory, ec);
    if (ec)
        return false;

    // A complete temporary marker is renamed into place only after it has been
    // flushed. The daemon ignores a missing request, so a power loss cannot
    // turn a partial write into an uninstall request. Commit it before rename:
    // FAT has no atomic rename durability guarantee across sudden power loss.
    std::remove(uninstall::kRequestTemporary);
    std::FILE* request = std::fopen(uninstall::kRequestTemporary, "wb");
    if (!request)
        return false;
    const bool wrote = std::fwrite(uninstall::kRequestContents, 1,
                                   sizeof(uninstall::kRequestContents) - 1, request)
                       == sizeof(uninstall::kRequestContents) - 1;
    const bool closed = std::fclose(request) == 0;
    if (!wrote || !closed) {
        std::remove(uninstall::kRequestTemporary);
        return false;
    }
    if (!switchu::commitSdCard("write self-uninstall marker"))
        return false;

    // A prior failed attempt leaves a valid request marker behind. Replacing it
    // is intentional: this confirmation stages a fresh, fully committed request.
    if (std::remove(uninstall::kRequest) != 0 && errno != ENOENT)
        return false;
    if (std::rename(uninstall::kRequestTemporary, uninstall::kRequest) != 0)
        return false;
    if (switchu::commitSdCard("stage self-uninstall"))
        return true;

    // The marker might be durable even though the commit call reported an
    // error; retain it rather than falsely claim cancellation and re-enable a
    // removal the player already confirmed. The daemon validates it on boot.
    return false;
}

std::string joinPath(const std::string& base, const std::string& name) {
    if (base.empty())
        return name;
    if (name.empty())
        return base;
    if (base.back() == '/')
        return base + name;
    return base + "/" + name;
}

bool isAbsoluteThemePath(const std::string& path) {
    return (!path.empty() && path.front() == '/') || (path.find(":/") != std::string::npos);
}

std::string trimWhitespace(const std::string& value) {
    const auto begin = value.find_first_not_of(" \t\r\n");
    if (begin == std::string::npos) return {};
    const auto end = value.find_last_not_of(" \t\r\n");
    return value.substr(begin, end - begin + 1);
}

std::string trimSlashes(std::string path) {
    while (!path.empty() && path.front() == '/')
        path.erase(path.begin());
    while (!path.empty() && path.back() == '/')
        path.pop_back();
    return path;
}

bool readJsonString(const nlohmann::json& obj, const char* key, std::string& out) {
    auto it = obj.find(key);
    if (it == obj.end() || !it->is_string())
        return false;
    out = it->get<std::string>();
    return !out.empty();
}

std::string installedThemePreviewPath(const ThemePreset& preset) {
    if (preset.installPath.empty())
        return {};

    auto resolveLocal = [&](const std::string& raw) -> std::string {
        if (raw.empty() || raw.rfind("http://", 0) == 0 || raw.rfind("https://", 0) == 0)
            return {};
        std::string path = isAbsoluteThemePath(raw) ? raw : joinPath(preset.installPath, trimSlashes(raw));
        return pathExists(path) ? path : std::string();
    };

    nlohmann::json manifest;
    std::ifstream input(joinPath(preset.installPath, "theme.json"));
    if (input) {
        try {
            input >> manifest;
        } catch (...) {
            manifest = nlohmann::json{};
        }
    }

    if (manifest.is_object()) {
        std::string raw;
        if ((readJsonString(manifest, "cover", raw)
             || readJsonString(manifest, "screenshot", raw)
             || readJsonString(manifest, "thumbnail", raw))) {
            std::string path = resolveLocal(raw);
            if (!path.empty())
                return path;
        }

        auto previewIt = manifest.find("preview");
        if (previewIt != manifest.end() && previewIt->is_object()) {
            raw.clear();
            if ((readJsonString(*previewIt, "cover", raw)
                 || readJsonString(*previewIt, "screenshot", raw)
                 || readJsonString(*previewIt, "thumbnail", raw))) {
                std::string path = resolveLocal(raw);
                if (!path.empty())
                    return path;
            }

            auto screenshotsIt = previewIt->find("screenshots");
            if (screenshotsIt != previewIt->end() && screenshotsIt->is_array()) {
                for (const auto& value : *screenshotsIt) {
                    if (!value.is_string())
                        continue;
                    std::string path = resolveLocal(value.get<std::string>());
                    if (!path.empty())
                        return path;
                }
            }
        }

        auto screenshotsIt = manifest.find("screenshots");
        if (screenshotsIt != manifest.end() && screenshotsIt->is_array()) {
            for (const auto& value : *screenshotsIt) {
                if (!value.is_string())
                    continue;
                std::string path = resolveLocal(value.get<std::string>());
                if (!path.empty())
                    return path;
            }
        }
    }

    static constexpr const char* kFallbacks[] = {
        "preview/cover.png",
        "preview/screenshot.png",
        "preview/cover.jpg",
        "preview/screenshot.jpg",
        "screenshots/cover.png",
        "screenshots/screenshot.png",
        "cover.png",
        "screenshot.png",
    };
    for (const char* fallback : kFallbacks) {
        std::string path = joinPath(preset.installPath, fallback);
        if (pathExists(path))
            return path;
    }

    return {};
}

const char* safeLogPath(const std::string& path) {
    return path.empty() ? "<empty>" : path.c_str();
}

const char* themeSourceName(ThemePresetSource source) {
    switch (source) {
        case ThemePresetSource::BuiltIn: return "builtin";
        case ThemePresetSource::UserPreset: return "user";
        case ThemePresetSource::InstalledPackage: return "package";
        default: return "unknown";
    }
}

WaraWaraBackground::Layout toBackgroundLayout(ThemeBackgroundLayout layout) {
    switch (layout) {
        case ThemeBackgroundLayout::Grid:
            return WaraWaraBackground::Layout::Grid;
        case ThemeBackgroundLayout::Floating:
        default:
            return WaraWaraBackground::Layout::Floating;
    }
}

WaraWaraBackground::ShapeSet toBackgroundShapeSet(ThemeBackgroundShapeSet shapeSet) {
    switch (shapeSet) {
        case ThemeBackgroundShapeSet::Circle:
            return WaraWaraBackground::ShapeSet::Circle;
        case ThemeBackgroundShapeSet::Triangle:
            return WaraWaraBackground::ShapeSet::Triangle;
        case ThemeBackgroundShapeSet::Square:
            return WaraWaraBackground::ShapeSet::Square;
        case ThemeBackgroundShapeSet::Diamond:
            return WaraWaraBackground::ShapeSet::Diamond;
        case ThemeBackgroundShapeSet::Hexagon:
            return WaraWaraBackground::ShapeSet::Hexagon;
        case ThemeBackgroundShapeSet::Mixed:
        default:
            return WaraWaraBackground::ShapeSet::Mixed;
    }
}

WaraWaraBackground::Symmetry toBackgroundSymmetry(ThemeBackgroundSymmetry symmetry) {
    switch (symmetry) {
        case ThemeBackgroundSymmetry::MirrorX:
            return WaraWaraBackground::Symmetry::MirrorHorizontal;
        case ThemeBackgroundSymmetry::MirrorY:
            return WaraWaraBackground::Symmetry::MirrorVertical;
        case ThemeBackgroundSymmetry::Quad:
            return WaraWaraBackground::Symmetry::Quad;
        case ThemeBackgroundSymmetry::None:
        default:
            return WaraWaraBackground::Symmetry::None;
    }
}

std::string sourceLabel(ThemePresetSource source) {
    auto& i18n = nxui::I18n::instance();
    switch (source) {
        case ThemePresetSource::BuiltIn:
            return i18n.tr("themeshop.source.builtin", "Built-in");
        case ThemePresetSource::UserPreset:
            return i18n.tr("themeshop.source.custom", "Custom");
        case ThemePresetSource::InstalledPackage:
            return i18n.tr("themeshop.source.community", "Community");
    }

    return i18n.tr("themeshop.source.unknown", "Unknown");
}

std::string defaultThemeRef() {
    // Keep first-run behavior aligned with the author's original static theme.
    // Animated wallpapers remain available only through separately installed
    // themes, rather than inflating every SwitchU installation.
    return "builtin:Default Dark";
}

} // namespace

// Radius 3 at half resolution reached six screen pixels; at full resolution it
// reaches three, which is the sharpening that was reported as too strong on a
// dark theme. 0.4 maps back to a reach of six, so the default looks like what
// it replaced and the slider goes either way from there.
void WiiUMenuApp::applyGlassSharpness(float sharpness) {
    const float s = std::clamp(sharpness, 0.f, 1.f);
    settings::debug::settingsGlassTuning().preBlurRadius = 9.f - s * 7.f;
}

void WiiUMenuApp::createSettings() {
    if (m_settings) return;

    m_settings = std::make_shared<SettingsScreen>();
    if (m_overlayLayer) {
        m_overlayLayer->addChild(m_settings);
    }
    m_settings->setFont(&m_fontNormal);
    m_settings->setSmallFont(&m_fontSmall);
    m_settings->setTheme(&m_theme);
    m_settings->setWireframeState(m_showWireframe);
    m_settings->setGridLayoutState(m_config.gridColumns, m_config.gridRows);
    m_settings->setUiLanguageOverride(m_config.uiLanguageOverride);
    m_settings->setDefaultProfileState(m_config.defaultProfileEnabled,
                                       m_config.defaultProfileUid);
    m_settings->setClockUse12HourState(m_config.clockUse12Hour);
    m_settings->setAccessibilityEnabledState(m_config.accessibilityEnabled);
    m_settings->setAccessibilitySpeechState(m_config.accessibilitySpeakHints,
                                            m_config.accessibilitySpeakContextEveryFocus,
                                            m_config.accessibilitySpeakPosition,
                                            m_config.accessibilitySpeechRate);
    m_settings->setAccessibilitySpeechPreferences(m_config.accessibilitySpeakHints,
                                                  m_config.accessibilitySpeakPosition);
    m_settings->setSteamGridDbState(m_config.steamGridDbEnabled,
                                    !m_config.steamGridDbApiKey.empty());

    m_settings->onNavigateSfx([this]() { m_audio.playSfx(Sfx::Navigate); });
    m_settings->onActivateSfx([this]() { m_audio.playSfx(Sfx::Activate); });
    m_settings->onCloseSfx([this]() { m_audio.playSfx(Sfx::ModalHide); });
    m_settings->onAccessibilityAnnouncement([this](const std::string& text) {
        m_accessibility.announce(text);
    });
    m_settings->onAccessibilityStructuredAnnouncement([this](const std::string& context,
                                                             const std::string& position,
                                                             const std::string& summary,
                                                             bool forceRepeat,
                                                             bool forceContext) {
        m_accessibility.announceStructuredFocus(context, position, summary, forceRepeat, forceContext);
    });
    m_settings->onToggleSfx([this](bool on) {
        m_audio.playSfx(on ? Sfx::ThemeToggle : Sfx::ToggleOff);
    });
    m_settings->onSliderSfx([this](bool up) {
        m_audio.playSfx(up ? Sfx::SliderUp : Sfx::SliderDown);
    });
    m_settings->onWireframeChange([this](bool enabled) {
        m_showWireframe = enabled;
        app().renderer().setBoxWireframeEnabled(enabled);
    });
    m_settings->onGridColumnsChange([this](int cols) {
        cols = std::clamp(cols, 3, 8);
        if (m_config.gridColumns == cols)
            return;
        m_config.gridColumns = cols;
        reflowHomeGrid();
    });
    m_settings->onGridRowsChange([this](int rows) {
        rows = std::clamp(rows, 2, 5);
        if (m_config.gridRows == rows)
            return;
        m_config.gridRows = rows;
        reflowHomeGrid();
    });
    m_settings->onUiLanguageChange([this](const std::string& tag) {
        m_config.uiLanguageOverride = tag;
        if (m_settings) m_settings->setUiLanguageOverride(tag);
        applyUiLanguage();
        app().gpu().waitIdle();
        m_fontNormal.clearCache();
        m_fontSmall.clearCache();
        m_fontClock.clearCache();
        m_settingsNeedRefresh = true;
        // Switching the catalogue only changes what tr() returns the next time
        // it is asked. Whatever asked once and kept the answer stays in the old
        // language until it is built again, which is why a restart was needed:
        //  - A widget tile's header and its two lines are read from tr() in
        //    makeIcon(), so the home model is composed again here.
        //  - The SwitchU screen, the update tab and its changelog among it,
        //    builds its strings when it is constructed and is then cached, so
        //    dropping it means the next open builds it in the new language.
        //    Only while it is off screen: the language lives in Settings, so it
        //    always is, but freeing a live overlay would take its textures too.
        // The action hints and the title pill are rebuilt every frame and were
        // never part of this.
        //
        // The same fix went into WiiUMenuAppUpstreamSettings.cpp first, which is
        // the copy of this handler inside an #if 0 block. It compiled, shipped
        // in 2.3.0 and did nothing.
        reflowHomeGrid();
        if (m_themeShop && !m_themeShop->isActive())
            m_themeShop.reset();
    });
    m_settings->onDefaultProfileChange([this](const std::string& uidHex) {
        m_config.defaultProfileEnabled = !uidHex.empty();
        m_config.defaultProfileUid = uidHex;
        if (m_settings)
            m_settings->setDefaultProfileState(m_config.defaultProfileEnabled,
                                               m_config.defaultProfileUid);
    });
    m_settings->onAddUser([this]() {
        m_launcher.launchUserCreator();
    });
    m_settings->onClockUse12HourChange([this](bool enabled) {
        if (m_config.clockUse12Hour == enabled)
            return;
        m_config.clockUse12Hour = enabled;
        if (m_clock)
            m_clock->setUse12HourClock(enabled);
        m_lockScreen.setUse12HourClock(enabled);
    });
    m_settings->onAccessibilityEnabledChange([this](bool enabled) {
        if (m_config.accessibilityEnabled == enabled)
            return;
        m_config.accessibilityEnabled = enabled;
        if (m_settings)
            m_settings->setAccessibilityVoiceEnabled(enabled);
        if (m_themeShop)
            m_themeShop->setAccessibilityVoiceEnabled(enabled);
        if (m_gameGallery)
            m_gameGallery->setAccessibilityVoiceEnabled(enabled);
        if (m_gameMods)
            m_gameMods->setAccessibilityVoiceEnabled(enabled);
        if (m_gameCheats)
            m_gameCheats->setAccessibilityVoiceEnabled(enabled);
        if (m_gameDetails)
            m_gameDetails->setAccessibilityVoiceEnabled(enabled);
        if (enabled) {
            m_accessibility.setEnabled(true);
            m_accessibility.announce(nxui::I18n::instance().tr(
                "accessibility.speech.enabled",
                "Voice guidance enabled."));
        } else {
            m_accessibility.announceAndDisable(nxui::I18n::instance().tr(
                "accessibility.speech.disabled",
                "Voice guidance disabled."));
        }
    });
    m_settings->onAccessibilitySpeakHintsChange([this](bool enabled) {
        m_config.accessibilitySpeakHints = enabled;
        m_accessibility.setSpeakHints(enabled);
        if (m_settings)
            m_settings->setAccessibilitySpeechPreferences(m_config.accessibilitySpeakHints,
                                                          m_config.accessibilitySpeakPosition);
        if (m_themeShop)
            m_themeShop->setAccessibilitySpeechPreferences(m_config.accessibilitySpeakHints,
                                                           m_config.accessibilitySpeakPosition);
        if (m_gameGallery)
            m_gameGallery->setAccessibilitySpeechPreferences(m_config.accessibilitySpeakHints,
                                                             m_config.accessibilitySpeakPosition);
        if (m_gameMods)
            m_gameMods->setAccessibilitySpeechPreferences(m_config.accessibilitySpeakHints,
                                                          m_config.accessibilitySpeakPosition);
        if (m_gameCheats)
            m_gameCheats->setAccessibilitySpeechPreferences(m_config.accessibilitySpeakHints,
                                                            m_config.accessibilitySpeakPosition);
        if (m_gameDetails)
            m_gameDetails->setAccessibilitySpeechPreferences(m_config.accessibilitySpeakHints,
                                                              m_config.accessibilitySpeakPosition);
        if (m_dialog)
            m_dialog->setAccessibilitySpeechPreferences(m_config.accessibilitySpeakHints,
                                                        m_config.accessibilitySpeakPosition);
        if (m_userSelect)
            m_userSelect->setAccessibilitySpeechPreferences(m_config.accessibilitySpeakHints,
                                                            m_config.accessibilitySpeakPosition);
    });
    m_settings->onAccessibilitySpeakContextEveryFocusChange([this](bool enabled) {
        m_config.accessibilitySpeakContextEveryFocus = enabled;
        m_accessibility.setSpeakContextEveryFocus(enabled);
    });
    m_settings->onAccessibilitySpeakPositionChange([this](bool enabled) {
        m_config.accessibilitySpeakPosition = enabled;
        if (m_settings)
            m_settings->setAccessibilitySpeechPreferences(m_config.accessibilitySpeakHints,
                                                          m_config.accessibilitySpeakPosition);
        if (m_themeShop)
            m_themeShop->setAccessibilitySpeechPreferences(m_config.accessibilitySpeakHints,
                                                           m_config.accessibilitySpeakPosition);
        if (m_gameGallery)
            m_gameGallery->setAccessibilitySpeechPreferences(m_config.accessibilitySpeakHints,
                                                             m_config.accessibilitySpeakPosition);
        if (m_gameMods)
            m_gameMods->setAccessibilitySpeechPreferences(m_config.accessibilitySpeakHints,
                                                          m_config.accessibilitySpeakPosition);
        if (m_gameCheats)
            m_gameCheats->setAccessibilitySpeechPreferences(m_config.accessibilitySpeakHints,
                                                            m_config.accessibilitySpeakPosition);
        if (m_gameDetails)
            m_gameDetails->setAccessibilitySpeechPreferences(m_config.accessibilitySpeakHints,
                                                              m_config.accessibilitySpeakPosition);
        if (m_dialog)
            m_dialog->setAccessibilitySpeechPreferences(m_config.accessibilitySpeakHints,
                                                        m_config.accessibilitySpeakPosition);
        if (m_userSelect)
            m_userSelect->setAccessibilitySpeechPreferences(m_config.accessibilitySpeakHints,
                                                            m_config.accessibilitySpeakPosition);
    });
    m_settings->onAccessibilitySpeechRateChange([this](int rate) {
        m_config.accessibilitySpeechRate = std::clamp(rate, 120, 320);
        m_accessibility.setSpeechRate(m_config.accessibilitySpeechRate);
    });
    m_settings->onNetConnect([this]() {
        m_pendingNetConnect = true;
        m_settings->hide();
    });
    m_settings->onSteamGridDbEnabledChange([this](bool enabled) {
        m_config.steamGridDbEnabled = enabled;
        if (m_steamGridDbBackdrop) {
            m_steamGridDbBackdrop->setEnabled(enabled);
            if (enabled) showFocusedSteamGridDbArtwork(true);
        }
    });
    m_settings->onSteamGridDbApiKeyRequest([this]() {
        editSteamGridDbApiKey();
    });
    m_settings->onRotateLogsRequest([this]() {
        auto& i18n = nxui::I18n::instance();
        const Result rc = m_launcher.rotateLogs();
        switchu::commitSdCard("log rotation");
        if (m_settings) {
            m_settings->requestToast(
                R_SUCCEEDED(rc)
                    ? i18n.tr("settings.system.save_logs_done",
                              "Logs saved. Copy menu-*.log and daemon-*.log from config/OmniLaunch.")
                    : i18n.tr("settings.system.save_logs_failed",
                              "The menu log was saved; the daemon did not answer."),
                4.5f);
        }
    });
    m_settings->onConsoleNicknameRequest([this]() {
        auto& i18n = nxui::I18n::instance();
        SetSysDeviceNickName current{};
        setsysGetDeviceNickname(&current);
        requestTextEntry(
            i18n.tr("settings.system.console_nickname", "Console Nickname"),
            i18n.tr("settings.system.console_nickname_guide", "Enter a name for this console"),
            current.nickname, 32, false,
            [this](const std::string& value) {
                const std::string trimmed = trimWhitespace(value);
                if (trimmed.empty())
                    return;
                SetSysDeviceNickName nickname{};
                std::snprintf(nickname.nickname, sizeof(nickname.nickname), "%s",
                              trimmed.c_str());
                const Result rc = setsysSetDeviceNickname(&nickname);
                DebugLog::log("[settings] console nickname rc=0x%X", rc);
                if (R_SUCCEEDED(rc) && m_settings)
                    m_settings->rebuildCurrentTab();
            });
    });
    m_settings->onSteamGridDbScrapeRequest([this]() {
        startSteamGridDbScrape();
    });
    m_settings->onControllerPairing([this]() {
        if (m_settings) m_settings->hide();
        m_launcher.launchControllerPairing();
    });
    m_settings->onControllerRemapping([this]() {
        if (m_settings) m_settings->hide();
        m_launcher.launchControllerRemapping();
    });
    m_settings->onControllerTest([this]() {
        if (!m_controllerTest) return;
        // The controller test is created in onCreate; the settings overlay is
        // created lazily the first time it is opened, so it is appended to the
        // overlay layer after it and painted over it. Focus moved to a screen
        // the player could not see: the settings panel stayed on screen and
        // stopped responding, which on hardware read as a frozen console.
        raiseOverlay(m_controllerTest);
        m_navigator.navigate(switchu::navigation::Route::ControllerTest);
        m_controllerTest->show();
        focusManager().setFocus(m_controllerTest.get());
        m_audio.playSfx(Sfx::ModalShow);
    });
    m_settings->onSoftwareDelete([this](uint64_t titleId, const std::string& title) {
        startSoftwareDeletion(titleId, title, false);
    });
    m_settings->onSleepRequest([this]() {
        if (m_settings) m_settings->hide();
        m_launcher.enterSleep();
    });
    m_settings->onShutdownRequest([this]() {
        if (m_settings) m_settings->hide();
        m_launcher.shutdown();
    });
    m_settings->onRebootRequest([this]() {
        if (m_settings) m_settings->hide();
        m_launcher.reboot();
    });
    m_settings->onDialogRequest([this](const std::string& title,
                                       const std::string& msg,
                                       std::vector<SettingsScreen::DialogButtonDef> buttons) {
        if (!m_dialog) return;
        std::vector<OverlayDialog::ButtonDef> dlgButtons;
        bool preserveReturnFocus = (m_dialog && m_dialog->isActive() && focusManager().current() == m_dialog.get() && m_dialogReturnFocus != nullptr);
        for (size_t i = 0; i < buttons.size(); ++i) {
            auto cb = buttons[i].onPress;
            bool isLast = (i == buttons.size() - 1);
            if (buttons.size() == 1) {
                dlgButtons.push_back({buttons[i].label, [this, cb]() {
                    m_audio.playSfx(Sfx::ConfirmPositive);
                    m_dialog->hide();
                    if (cb) cb();
                }, true});
            } else if (isLast) {
                dlgButtons.push_back({buttons[i].label, [cb]() { if (cb) cb(); }, true});
            } else {
                dlgButtons.push_back({buttons[i].label, [this, cb]() {
                    m_audio.playSfx(Sfx::ConfirmPositive);
                    m_dialog->hide();
                    if (cb) cb();
                }, true});
            }
        }
        if (!preserveReturnFocus)
            m_dialogReturnFocus = focusManager().current();
        m_dialog->show(title, msg, std::move(dlgButtons));
        focusManager().setFocus(m_dialog.get());
    });
    m_settings->onDateTimeEditorRequest(
        [this](const TabbedOverlayScreen::DateTimeEditorValue& initial,
               TabbedOverlayScreen::DateTimeCommitCb onCommit) {
            if (!m_dialog) return;
            m_dialogReturnFocus = m_settings.get();
            OverlayDialog::DateTimeValue value;
            value.year = initial.year;
            value.month = initial.month;
            value.day = initial.day;
            value.hour = initial.hour;
            value.minute = initial.minute;
            m_dialog->showDateTimeEditor(
                value,
                [this, onCommit = std::move(onCommit)](
                    const OverlayDialog::DateTimeValue& edited) mutable {
                    TabbedOverlayScreen::DateTimeEditorValue committed;
                    committed.year = edited.year;
                    committed.month = edited.month;
                    committed.day = edited.day;
                    committed.hour = edited.hour;
                    committed.minute = edited.minute;
                    const bool saved = !onCommit || onCommit(committed);
                    if (saved)
                        m_clockService.invalidate();
                    return saved;
                });
            focusManager().setFocus(m_dialog.get());
        });
    m_settings->onClosed([this]() {
        m_navigator.routeDidClose(switchu::navigation::Route::Settings);
        m_configSaveFuture = m_threadPool.submit([cfg = m_config]() {
            cfg.save();
        });
        DebugLog::log("[config] save queued");
        if (isCurrentFocusableWidget(m_settingsReturnFocus)) {
            m_suppressNextNavigateSfx = true;
            focusManager().setFocus(m_settingsReturnFocus);
        } else if (m_grid && m_grid->focusManager().current()) {
            m_suppressNextNavigateSfx = true;
            focusManager().setFocus(m_grid->focusManager().current());
        } else if (isCurrentFocusableWidget(m_sidebar.settingsButton())) {
            m_suppressNextNavigateSfx = true;
            focusManager().setFocus(m_sidebar.settingsButton());
        }
        m_settingsReturnFocus = nullptr;
    });

    // This screen is created on first use, after the shared overlays already
    // exist. Restore their original z-order so settings-owned dialogs, launch
    // blackouts, and the pointer remain above it.
    raiseOverlay(m_dialog);
    raiseOverlay(m_progressDialog);
    raiseOverlay(m_launchAnim);
    raiseOverlay(m_pointerCursor);

}

void WiiUMenuApp::createQuickSettings() {
    if (m_quickSettings) return;

    m_quickSettings = std::make_shared<QuickSettingsOverlay>();
    if (m_overlayLayer) {
        m_overlayLayer->addChild(m_quickSettings);
    }
    m_quickSettings->setFont(&m_fontNormal);
    m_quickSettings->setSmallFont(&m_fontSmall);
    m_quickSettings->setIconFont(&m_fontIcons);
    m_quickSettings->setTheme(&m_theme);
    m_quickSettings->setInput(&app().input());

    QuickSettingsOverlay::Callbacks cbs;
    cbs.onBrightnessChanged = [](float /*val*/) {
    };
    cbs.onBgmVolumeChanged = [this](float val) {
        m_audio.setVolume(val);
        m_config.musicVolume = val;
    };
    cbs.onSfxVolumeChanged = [this](float val) {
        m_audio.setSfxVolume(val);
        m_config.sfxVolume = val;
    };
    cbs.onAirplaneModeToggled = [](bool /*enabled*/) {
    };
    cbs.onWifiToggled = [](bool /*enabled*/) {
    };
    cbs.onActivityLogRequested = [this]() {
        closeQuickSettings();
        openActivityLog();
    };
    cbs.onSleepRequested = [this]() {
        if (!m_dialog) return;
        auto& i18n = nxui::I18n::instance();
        m_audio.playSfx(Sfx::ModalShow);
        m_dialogReturnFocus = focusManager().current();
        raiseOverlay(m_dialog);
        m_dialog->show(
            i18n.tr("power.title", "Power"),
            i18n.tr("settings.sleep.sleep_confirm", "Put the console into sleep mode?"),
            {
                {i18n.tr("button.cancel", "Cancel"), []() {}, true},
                {i18n.tr("power.sleep", "Sleep"), [this]() {
#ifdef SWITCHU_MENU
                    m_audio.playSfx(Sfx::ConfirmPositive);
                    m_launcher.enterSleep();
#else
                    m_audio.playSfx(Sfx::ConfirmPositive);
                    app().requestExit();
#endif
                }, true}
            });
    };
    cbs.onRebootRequested = [this]() {
        if (!m_dialog) return;
        auto& i18n = nxui::I18n::instance();
        m_audio.playSfx(Sfx::ModalShow);
        m_dialogReturnFocus = focusManager().current();
        raiseOverlay(m_dialog);
        m_dialog->show(
            i18n.tr("power.title", "Power"),
            i18n.tr("settings.sleep.reboot_confirm", "Restart the console?"),
            {
                {i18n.tr("button.cancel", "Cancel"), []() {}, true},
                {i18n.tr("power.reboot", "Reboot"), [this]() {
#ifdef SWITCHU_MENU
                    m_audio.playSfx(Sfx::ConfirmPositive);
                    m_launcher.reboot();
#else
                    m_audio.playSfx(Sfx::ConfirmPositive);
                    app().requestExit();
#endif
                }, true}
            });
    };
    cbs.onShutdownRequested = [this]() {
        if (!m_dialog) return;
        auto& i18n = nxui::I18n::instance();
        m_audio.playSfx(Sfx::ModalShow);
        m_dialogReturnFocus = focusManager().current();
        raiseOverlay(m_dialog);
        m_dialog->show(
            i18n.tr("power.title", "Power"),
            i18n.tr("settings.sleep.shutdown_confirm", "Power off the console?"),
            {
                {i18n.tr("button.cancel", "Cancel"), []() {}, true},
                {i18n.tr("power.shutdown", "Shutdown"), [this]() {
#ifdef SWITCHU_MENU
                    m_audio.playSfx(Sfx::ConfirmPositive);
                    m_launcher.shutdown();
#else
                    m_audio.playSfx(Sfx::ConfirmPositive);
                    app().requestExit();
#endif
                }, true}
            });
    };
    cbs.onClose = [this]() {
        closeQuickSettings();
    };
    cbs.onNavigateSfx = [this]() { m_audio.playSfx(Sfx::Navigate); };
    cbs.onActivateSfx = [this]() { m_audio.playSfx(Sfx::Activate); };
    cbs.onToggleOffSfx = [this]() { m_audio.playSfx(Sfx::ToggleOff); };

    m_quickSettings->setCallbacks(cbs);
}

void WiiUMenuApp::openQuickSettings() {
    createQuickSettings();
    if (!m_quickSettings || m_quickSettings->isActive()) return;

    if (m_editMode) return;
    if (m_dialog && m_dialog->isActive()) return;
    if (m_userSelect && m_userSelect->isActive()) return;

    m_dialogReturnFocus = focusManager().current();

    m_quickSettings->setInitialValues(0.5f, m_audio.volume(), m_audio.sfxVolume(), false, true);
    m_quickSettings->setBatteryStatus(m_consoleBatteryPercent, m_consoleBatteryCharging);

    raiseOverlay(m_quickSettings);
    m_quickSettings->show();
    focusManager().setFocus(m_quickSettings.get());
    m_audio.playSfx(Sfx::ModalShow);
}

void WiiUMenuApp::closeQuickSettings() {
    if (!m_quickSettings || !m_quickSettings->isActive()) return;
    m_quickSettings->hide();
    m_config.save();
    if (isCurrentFocusableWidget(m_dialogReturnFocus)) {
        m_suppressNextNavigateSfx = true;
        focusManager().setFocus(m_dialogReturnFocus);
    } else if (m_grid) {
        auto* target = m_grid->focusManager().current();
        if (target && isCurrentFocusableWidget(target)) {
            m_suppressNextNavigateSfx = true;
            focusManager().setFocus(target);
        } else {
            auto icons = m_grid->allIcons();
            if (!icons.empty()) {
                m_suppressNextNavigateSfx = true;
                focusManager().setFocus(icons[0].get());
            }
        }
    }
    m_dialogReturnFocus = nullptr;
    m_audio.playSfx(Sfx::ModalHide);
}

void WiiUMenuApp::createActivityLog() {
    if (m_activityLog) return;

    m_activityLog = std::make_shared<ActivityLogScreen>();
    m_activityLog->setRect({32.f, 20.f, 1216.f, 642.f});
    if (m_overlayLayer) {
        m_overlayLayer->addChild(m_activityLog);
    }
    m_activityLog->setFont(&m_fontNormal);
    m_activityLog->setSmallFont(&m_fontSmall);
    m_activityLog->setTheme(&m_theme);
    m_activityLog->setThreadPool(&m_threadPool);
    m_activityLog->setRenderContext(&app().gpu(), &app().renderer());
    m_activityLog->setManager(&m_activityLogManager);
    m_activityLog->setInput(&app().input());
    m_activityLog->setIconProvider([this](std::uint64_t titleId) -> nxui::Texture* {
        if (m_grid) {
            for (const auto& icon : m_grid->allIcons()) {
                if (icon && icon->titleId() == titleId && icon->texture()) {
                    return icon->texture();
                }
            }
        }
        return nullptr;
    });

    m_activityLog->onNavigateSfx([this]() { m_audio.playSfx(Sfx::Navigate); });
    m_activityLog->onActivateSfx([this]() { m_audio.playSfx(Sfx::Activate); });
    m_activityLog->onCloseSfx([this]() { m_audio.playSfx(Sfx::ModalHide); });
    m_activityLog->onTabChangeSfx([this]() { m_audio.playSfx(Sfx::ThemeToggle); });
    m_activityLog->onDateChangeSfx([this](bool forward) {
        m_audio.playSfx(forward ? Sfx::SliderUp : Sfx::SliderDown);
    });
    m_activityLog->onClose([this]() {
        closeActivityLog();
    });
    m_activityLog->onTitleSelected([this](std::uint64_t titleId) {
        if (titleId != 0) {
            m_gameDetailsReturnFocus = m_activityLog.get();
            showGameDetails(titleId, "");
        }
    });
}

void WiiUMenuApp::openActivityLog(std::uint64_t initialTitleId) {
    if (m_editMode) return;
    if (m_dialog && m_dialog->isActive()) return;
    if (m_userSelect && m_userSelect->isActive()) return;

    createActivityLog();
    if (!m_activityLog || m_activityLog->isActive()) return;

    m_activityLogReturnFocus = focusManager().current();

    if (m_activityLogManager.allTimeRankings().empty()) {
        // Fallback: populate if prefetch hasn't finished yet
        std::vector<std::pair<std::uint64_t, std::string>> installed;
        if (!m_allApps.empty()) {
            for (const auto& app : m_allApps) {
                if (app.titleId != 0 && (app.isApplication() || m_config.isGamePort(app.titleId))) {
                    installed.emplace_back(app.titleId, app.title);
                }
            }
        } else {
            for (int i = 0; i < m_model.count(); ++i) {
                const auto& entry = m_model.at(i);
                if (entry.titleId != 0 && (entry.isApplication() || m_config.isGamePort(entry.titleId))) {
                    installed.emplace_back(entry.titleId, entry.title);
                }
            }
        }
        m_activityLogManager.refresh(installed);
    }

    raiseOverlay(m_activityLog);
    m_activityLog->open(initialTitleId);
    m_navigator.navigate(switchu::navigation::Route::ActivityLog);
    focusManager().setFocus(m_activityLog.get());
    m_audio.playSfx(Sfx::ModalShow);
}

void WiiUMenuApp::closeActivityLog() {
    if (!m_activityLog || !m_activityLog->isActive()) return;
    m_activityLog->hide();
    m_navigator.routeDidClose(switchu::navigation::Route::ActivityLog);
    m_audio.playSfx(Sfx::ModalHide);

    if (isCurrentFocusableWidget(m_activityLogReturnFocus)) {
        m_suppressNextNavigateSfx = true;
        focusManager().setFocus(m_activityLogReturnFocus);
    } else if (m_grid) {
        auto* target = m_grid->focusManager().current();
        if (target && isCurrentFocusableWidget(target)) {
            m_suppressNextNavigateSfx = true;
            focusManager().setFocus(target);
        } else {
            auto icons = m_grid->allIcons();
            if (!icons.empty()) {
                m_suppressNextNavigateSfx = true;
                focusManager().setFocus(icons[0].get());
            }
        }
    }
    m_activityLogReturnFocus = nullptr;
}

void WiiUMenuApp::createWaraWaraPlaza() {
    if (m_plazaScreen) return;

    m_plazaScreen = std::make_shared<warawara::WaraWaraPlazaScreen>();
    m_plazaScreen->setFonts(&m_fontNormal, &m_fontSmall);
    m_plazaScreen->initGpuAssets(app().gpu(), app().renderer());
    m_plazaScreen->setServices(&m_miiAvatarManager,
                               &m_plazaDialogueEngine,
                               &m_animalesePlayer,
                               &m_activityLogManager);
    refreshPlazaCommunities();

    m_plazaScreen->onClose([this]() {
        closeWaraWaraPlaza();
    });

    m_plazaScreen->onNavigateSfx([this]() {
        m_audio.playSfx(Sfx::Navigate);
    });
    m_plazaScreen->onActivateSfx([this]() {
        m_audio.playSfx(Sfx::Activate);
    });

    m_plazaScreen->onLaunchGame([this](std::uint64_t titleId) {
        if (titleId == 0) return;
        for (auto& app : m_allApps) {
            if (app.titleId == titleId) {
                GlossyIcon* iconSource = nullptr;
                if (m_grid) {
                    for (auto& icon : m_grid->allIcons()) {
                        if (icon && icon->titleId() == titleId) {
                            iconSource = icon.get();
                            break;
                        }
                    }
                }
                closeWaraWaraPlaza();
                if (iconSource) {
                    m_grid->focusManager().setFocus(iconSource);
                    focusManager().setFocus(iconSource);
                }
#ifdef SWITCHU_MENU
                activateApplication(iconSource, &app, app.titleId, app.title);
#endif
                break;
            }
        }
    });

    if (m_overlayLayer) {
        m_overlayLayer->addChild(m_plazaScreen);
    }
}

void WiiUMenuApp::refreshPlazaCommunities() {
    if (!m_plazaScreen) return;

    std::vector<warawara::WaraWaraPlazaScreen::GameCommunityEntry> entries;
    entries.reserve(10);

    // Plaza holds up to 10 communities. To prevent page-eviction thrashing on
    // the home grid and endless flickering between images and placeholder tiles,
    // Plaza loads and retains its own textures directly via AppListLoader::loadIconData,
    // pumping at most one decode/upload per frame without touching IconStreamer.
    const auto* gridIcons = m_grid ? &m_grid->allIcons() : nullptr;
    std::unordered_map<std::uint64_t, int> displayIndexByTitle;
    displayIndexByTitle.reserve(static_cast<std::size_t>(std::max(0, m_model.count())));
    for (int i = 0; i < m_model.count(); ++i) {
        const auto& displayEntry = m_model.at(i);
        if (displayEntry.isApplication() && displayEntry.titleId != 0)
            displayIndexByTitle.emplace(displayEntry.titleId, i);
    }

    const int communityCount = std::min(10, static_cast<int>(m_allApps.size()));
    for (int offset = 0; offset < communityCount; ++offset) {
        const int appPos = (m_plazaIconPumpIndex + offset) % communityCount;
        const auto& candidate = m_allApps[static_cast<std::size_t>(appPos)];
        if (candidate.titleId == 0 || candidate.isWidget())
            continue;

        auto& tex = m_plazaCommunityTextures[candidate.titleId];
        if (!tex.valid()) {
            std::vector<uint8_t> raw = AppListLoader::loadIconData(candidate.titleId);
            if (!raw.empty()) {
                tex.loadFromMemory(this->app().gpu(), this->app().renderer(),
                                   raw.data(), raw.size(), 256);
            }
            m_plazaIconPumpIndex = (appPos + 1) % communityCount;
            break;
        }
    }

    for (const auto& app : m_allApps) {
        if (entries.size() >= 10) break;
        if (app.titleId == 0 || app.isWidget()) continue;

        const auto displayFound = displayIndexByTitle.find(app.titleId);
        const int displayIndex = displayFound == displayIndexByTitle.end()
            ? -1 : displayFound->second;

        warawara::WaraWaraPlazaScreen::GameCommunityEntry entry;
        entry.titleId = app.titleId;
        entry.title = app.title;

        const auto* stats = m_activityLogManager.findTitleStats(app.titleId);
        std::uint64_t playtimeSec = stats ? stats->totalPlaytimeSeconds : 0;
        if (playtimeSec > 0) {
            std::uint32_t hours = static_cast<std::uint32_t>(playtimeSec / 3600);
            std::uint32_t minutes = static_cast<std::uint32_t>((playtimeSec % 3600) / 60);
            if (hours > 0) {
                entry.subtitle = std::to_string(hours) + "h " + std::to_string(minutes) + "m played";
            } else {
                entry.subtitle = std::to_string(minutes) + "m played";
            }
        } else {
            entry.subtitle = "Installed Game";
        }

        auto texIt = m_plazaCommunityTextures.find(app.titleId);
        if (texIt != m_plazaCommunityTextures.end() && texIt->second.valid()) {
            entry.iconTexture = &texIt->second;
        } else if (gridIcons && displayIndex >= 0 &&
                   displayIndex < static_cast<int>(gridIcons->size())) {
            const auto& icon = (*gridIcons)[static_cast<std::size_t>(displayIndex)];
            if (icon && icon->titleId() == app.titleId && icon->texture() && icon->texture()->valid())
                entry.iconTexture = icon->texture();
        }

        entries.push_back(std::move(entry));
    }

    m_plazaScreen->setupCommunities(entries);
}

void WiiUMenuApp::openWaraWaraPlaza() {
    if (m_editMode) return;
    if ((m_dialog && m_dialog->isActive()) ||
        (m_userSelect && m_userSelect->isActive()) ||
        (m_settings && m_settings->isActive()) ||
        (m_themeShop && m_themeShop->isActive()) ||
        (m_activityLog && m_activityLog->isActive())) {
        return;
    }

    if (!m_plazaScreen) {
        createWaraWaraPlaza();
    } else {
        refreshPlazaCommunities();
    }
    m_plazaReturnFocus = focusManager().current();
    if (m_cursor) {
        m_cursor->setVisible(false);
    }
    m_navigator.navigate(switchu::navigation::Route::WaraWaraPlaza);
    m_plazaIconPumpIndex = 0;
    m_plazaScreen->open();
    // focusRoot() also returns the Plaza while active, but dispatchInput() runs
    // before onUpdate() and must not spend a frame repairing stale HOME focus.
    // Bind the owner explicitly now so its registered Button A action receives
    // the very first controller press reliably.
    focusManager().setFocus(m_plazaScreen.get());
    if (m_screenSwapButton) {
        m_screenSwapButton->setPlazaActive(true);
    }
    m_audio.playSfx(Sfx::ThemeToggle);
}

void WiiUMenuApp::closeWaraWaraPlaza() {
    if (!m_plazaScreen) return;
    if (!m_plazaScreen->isActive() && m_navigator.route() != switchu::navigation::Route::WaraWaraPlaza) {
        return;
    }

    m_plazaScreen->close();
    m_plazaScreen->setVisible(false);
    m_plazaCommunityTextures.clear();
    m_navigator.routeDidClose(switchu::navigation::Route::WaraWaraPlaza);
    if (m_screenSwapButton) {
        m_screenSwapButton->setPlazaActive(false);
    }
    m_audio.playSfx(Sfx::ModalHide);

    nxui::Widget* target = nullptr;
    if (isCurrentFocusableWidget(m_plazaReturnFocus)) {
        target = m_plazaReturnFocus;
    } else if (m_grid) {
        target = m_grid->focusManager().current();
        if (!target || !isCurrentFocusableWidget(target)) {
            auto icons = m_grid->allIcons();
            if (!icons.empty() && icons[0]) {
                target = icons[0].get();
            }
        }
    }
    m_plazaReturnFocus = nullptr;

    if (target) {
        m_suppressNextNavigateSfx = true;
        if (m_grid) {
            m_grid->focusManager().setFocus(target);
        }
        focusManager().setFocus(target);
        if (m_cursor) {
            m_cursor->moveTo(target->focusRect().expanded(4.f), 0.f);
            m_cursor->setVisible(true);
        }
    }
}

void WiiUMenuApp::toggleWaraWaraPlaza() {
    if (m_plazaScreen && m_plazaScreen->isActive()) {
        closeWaraWaraPlaza();
    } else {
        openWaraWaraPlaza();
    }
}

void WiiUMenuApp::createThemeShop() {
    if (m_themeShop) return;

    auto showThemeShopInfo = [this](const std::string& title, const std::string& message) {
        if (!m_dialog) return;
        m_dialogReturnFocus = focusManager().current();
        m_dialog->show(title, message, {{"OK", {}, true}});
        focusManager().setFocus(m_dialog.get());
    };
    auto queueThemeTransfer = [this, showThemeShopInfo](const std::string& themeId, bool applyAfterInstall) {
        if (!m_themeShop)
            return;

        const auto* entry = m_themeShop->findCommunityThemeEntry(themeId);
        if (!entry) {
            auto& i18n = nxui::I18n::instance();
            showThemeShopInfo(i18n.tr("sidebar.theme_shop", "OmniLaunch"),
                              i18n.tr("themeshop.community.selected_missing",
                                      "The selected community theme is no longer available in the catalog."));
            return;
        }

        ThemeCatalogClient::Entry entryCopy = *entry;
        ThemePackageInstaller::Mode mode = applyAfterInstall
            ? ThemePackageInstaller::Mode::InstallAndApply
            : ThemePackageInstaller::Mode::InstallOnly;
        std::string destination = ThemePackageInstaller::destinationRootFor(entryCopy.id, mode);

        // The catalog now publishes one definitive package: BC7 at 912x512.
        // It fixes BC1 artifacts in dark scenes while retaining the complete
        // animation at 30 fps inside the measured image-memory budget.
        auto startTransfer = [this, entryCopy, applyAfterInstall]() {
            startThemePackageTransfer(entryCopy, applyAfterInstall);
        };

        if (!pathExists(destination)) {
            startTransfer();
            return;
        }

        auto& i18n = nxui::I18n::instance();
        if (!m_dialog) {
            startTransfer();
            return;
        }

        m_dialogReturnFocus = focusManager().current();
        m_dialog->show(
            applyAfterInstall
                ? i18n.tr("themeshop.community.overwrite_apply_title", "Replace And Apply Theme")
                : i18n.tr("themeshop.community.overwrite_install_title", "Replace Installed Theme"),
            applyAfterInstall
                ? i18n.tr("themeshop.community.overwrite_apply_message",
                          "A package with this theme ID is already installed. Replace it with the GitHub version, then apply it?")
                : i18n.tr("themeshop.community.overwrite_install_message",
                          "A package with this theme ID is already installed. Replace it with the version from GitHub?"),
            {
                {i18n.tr("button.cancel", "Cancel"), {}, true},
                {i18n.tr("button.replace", "Replace"), [startTransfer]() { startTransfer(); }, true}
            },
            1,
            {});
        focusManager().setFocus(m_dialog.get());
    };
    auto clearCompletedThemeTransferState = [this]() {
        if (!m_themePackageTransfer)
            return;

        bool isRunning = false;
        {
            std::lock_guard<std::mutex> lk(m_themePackageTransfer->mutex);
            isRunning = m_themePackageTransfer->state.isRunning();
        }

        if (isRunning)
            return;

        m_themePackageTransfer.reset();
        m_themePackageTransferUiRevision = 0;
        m_themePackageTransferHandledRevision = 0;
        if (m_themeShop) {
            ThemeTransferState cleared;
            m_themeShop->setPackageTransferState(cleared, {}, false);
        }
    };

    m_themeShop = std::make_shared<ThemeShopScreen>();
    if (m_overlayLayer) {
        m_overlayLayer->addChild(m_themeShop);
    }
    m_themeShop->setFont(&m_fontNormal);
    m_themeShop->setSmallFont(&m_fontSmall);
    m_themeShop->setTheme(&m_theme);
    m_themeShop->setThreadPool(&m_threadPool);
    m_themeShop->setRenderContext(&app().gpu(), &app().renderer());
    m_themeShop->youTubeClient().setBackendUrl(m_config.ytdlBackendUrl);
    m_themeShop->setMusicState(m_audio.isPlaying(), m_audio.volume(), m_audio.sfxVolume());
    m_themeShop->setGridLayoutState(m_config.gridColumns, m_config.gridRows);
    m_themeShop->setDynamicPagesState(m_config.dynamicPages);
    m_themeShop->setAppearanceState(m_config.glassSharpness,
                                    m_config.backgroundSpeed,
                                    m_config.backgroundBlur);
    m_themeShop->setSteamGridDbOpacity(m_config.steamGridDbOpacity);
    m_themeShop->setSteamGridDbState(m_config.steamGridDbEnabled,
                                    !m_config.steamGridDbApiKey.empty());
    m_themeShop->setAccessibilityVoiceEnabled(m_config.accessibilityEnabled);
    m_themeShop->setAccessibilitySpeechPreferences(m_config.accessibilitySpeakHints,
                                                   m_config.accessibilitySpeakPosition);

    m_themeShop->onMusicEnabledChange([this](bool enabled) {
        if (enabled) m_audio.play(); else m_audio.stop();
        m_config.musicEnabled = enabled;
    });
    m_themeShop->onMusicVolumeChange([this](float v) {
        m_audio.setVolume(v);
        m_config.musicVolume = v;
    });
    m_themeShop->onSfxVolumeChange([this](float v) {
        m_audio.setSfxVolume(v);
        m_config.sfxVolume = v;
    });
    // Sole home for these three now. They used to be mirrored into Settings >
    // Display so the two screens agreed; with the Display copies gone there is
    // nothing left to keep in step.
    m_themeShop->onGlassSharpnessChange([this](float v) {
        m_config.glassSharpness = v;
        applyGlassSharpness(v);
    });
    m_themeShop->onBackgroundSpeedChange([this](float v) {
        m_config.backgroundSpeed = v;
        m_config.backgroundSpeedChosen = true;
        if (m_background) m_background->setSpeedScale(v * 2.f);
        if (m_background) m_background->setWallpaperSpeedScale(v);
    });
    m_themeShop->onBackgroundBlurChange([this](float v) {
        m_config.backgroundBlur = v;
        if (m_background) m_background->setBlurStrength(v);
    });
    m_themeShop->onSteamGridDbOpacityChange([this](float v) {
        m_config.steamGridDbOpacity = v;
        if (m_steamGridDbBackdrop) m_steamGridDbBackdrop->setArtworkOpacityScale(v);
    });
    m_themeShop->onSteamGridDbEnabledChange([this](bool enabled) {
        m_config.steamGridDbEnabled = enabled;
        if (m_steamGridDbBackdrop) {
            m_steamGridDbBackdrop->setEnabled(enabled);
            if (enabled) showFocusedSteamGridDbArtwork(true);
        }
    });
    m_themeShop->onSteamGridDbApiKeyRequest([this]() {
        editSteamGridDbApiKey();
    });
    m_themeShop->onSteamGridDbScrapeRequest([this]() {
        startSteamGridDbScrape();
    });
    m_themeShop->onGridColumnsChange([this](int cols) {
        cols = std::clamp(cols, 3, 8);
        if (m_config.gridColumns == cols)
            return;
        m_config.gridColumns = cols;
        reflowHomeGrid();
    });
    m_themeShop->onGridRowsChange([this](int rows) {
        rows = std::clamp(rows, 2, 5);
        if (m_config.gridRows == rows)
            return;
        m_config.gridRows = rows;
        reflowHomeGrid();
    });
    m_themeShop->onDynamicPagesChange([this](bool enabled) {
        if (m_config.dynamicPages == enabled)
            return;
        m_config.dynamicPages = enabled;
        m_config.save();
        reflowHomeGrid();
    });
    m_themeShop->setLayoutModeState(m_appLayoutMode);
    m_themeShop->onLayoutModeChange([this](AppLayoutMode mode) {
        setAppLayoutMode(mode);
    });
    m_themeShop->onNextTrack([this]() {
        m_audio.nextTrack();
        m_audio.playSfx(Sfx::ConfirmPositive);
    });
    m_themeShop->onNavigateSfx([this]() { m_audio.playSfx(Sfx::Navigate); });
    m_themeShop->onActivateSfx([this]() { m_audio.playSfx(Sfx::Activate); });
    m_themeShop->onCloseSfx([this]() { m_audio.playSfx(Sfx::ModalHide); });
    m_themeShop->onAccessibilityAnnouncement([this](const std::string& text) {
        m_accessibility.announce(text);
    });
    m_themeShop->onAccessibilityStructuredAnnouncement([this](const std::string& context,
                                                              const std::string& position,
                                                              const std::string& summary,
                                                              bool forceRepeat,
                                                              bool forceContext) {
        m_accessibility.announceStructuredFocus(context, position, summary, forceRepeat, forceContext);
    });
    m_themeShop->onToggleSfx([this](bool on) {
        m_audio.playSfx(on ? Sfx::ThemeToggle : Sfx::ToggleOff);
    });
    m_themeShop->onSliderSfx([this](bool up) {
        m_audio.playSfx(up ? Sfx::SliderUp : Sfx::SliderDown);
    });
    m_themeShop->onUpdateCheck([this]() { startUpdateCheck(true); });
    m_themeShop->onSelfUninstall([this]() {
        auto& i18n = nxui::I18n::instance();
        m_audio.playSfx(Sfx::ModalShow);
        raiseOverlay(m_dialog);
        m_dialog->show(
            i18n.tr("themeshop.uninstall", "Uninstall OmniLaunch"),
            i18n.tr("themeshop.uninstall_information",
                    "This permanently removes OmniLaunch from your console and restarts into the stock Nintendo HOME Menu.\n\n"
                    "All OmniLaunch data on your SD card will be completely deleted, including:\n"
                    "• All settings, configurations, and logs\n"
                    "• All downloaded static and animated themes\n"
                    "• All cached game artwork and icons\n"
                    "• OmniLaunch menu binaries and OmniLaunch Manager\n\n"
                    "To use OmniLaunch again in the future, a fresh manual installation will be required."),
            {
                {i18n.tr("button.cancel", "Cancel"), [this]() {}, true},
                {i18n.tr("themeshop.uninstall_continue", "Continue"), [this]() {
                    auto& i18n = nxui::I18n::instance();
                    m_audio.playSfx(Sfx::ModalShow);
                    raiseOverlay(m_dialog);
                    m_dialog->show(
                        i18n.tr("themeshop.uninstall_confirm_title", "Permanently Delete OmniLaunch?"),
                        i18n.tr("themeshop.uninstall_confirm",
                                "This action cannot be undone. "
                                "All OmniLaunch files, themes, artwork, and settings will be permanently erased from your SD card."),
                        {
                            {i18n.tr("button.cancel", "Cancel"), [this]() {}, true},
                            {i18n.tr("themeshop.uninstall_confirm_action", "Uninstall and delete everything"),
                             [this]() {
                                auto& i18n = nxui::I18n::instance();
                                if (m_progressDialog) {
                                    m_progressDialog->setTheme(&m_theme);
                                    raiseOverlay(m_progressDialog);
                                    m_progressDialog->show(
                                        i18n.tr("themeshop.uninstall_progress_title", "Removing OmniLaunch"),
                                        i18n.tr("themeshop.uninstall_preparing", "Preparing removal..."), 0.25f);
                                    focusManager().setFocus(m_progressDialog.get());
                                }

                                if (!stageSelfUninstallRequest()) {
                                    DebugLog::log("[uninstall] failed to stage request");
                                    if (m_progressDialog)
                                        m_progressDialog->hide();
                                    raiseOverlay(m_dialog);
                                    m_dialog->show(
                                        i18n.tr("themeshop.uninstall_failed_title", "Removal could not be prepared"),
                                        i18n.tr("themeshop.uninstall_failed",
                                                "OmniLaunch was not changed. Check that the SD card is writable and try again."),
                                        {{i18n.tr("button.ok", "OK"), [this]() {}, true}}, 0, {});
                                    focusManager().setFocus(m_dialog.get());
                                    return;
                                }

                                if (m_progressDialog)
                                    m_progressDialog->updateState(
                                        i18n.tr("themeshop.uninstall_restarting", "Restarting..."), 1.f);
                                const Result rc = m_launcher.requestSelfUninstall();
                                DebugLog::log("[uninstall] daemon request rc=0x%X", rc);
                                if (R_FAILED(rc)) {
                                    if (m_progressDialog)
                                        m_progressDialog->hide();
                                    raiseOverlay(m_dialog);
                                    m_dialog->show(
                                        i18n.tr("themeshop.uninstall_failed_title", "Removal could not be prepared"),
                                        i18n.tr("themeshop.uninstall_daemon_failed",
                                                "OmniLaunch was not changed because the restart request failed."),
                                        {{i18n.tr("button.ok", "OK"), [this]() {}, true}}, 0, {});
                                    focusManager().setFocus(m_dialog.get());
                                }
                             }, false},
                        },
                        0, {});
                    focusManager().setFocus(m_dialog.get());
                }, false},
            },
            0, {});
        focusManager().setFocus(m_dialog.get());
    });
    m_themeShop->onReleaseNotes([this]() { showReleaseNotes(); });
    m_themeShop->onUpdateInstall([this]() {
        // The notes dialog is the confirmation step: nothing downloads until a
        // player has seen what changed and chosen to go ahead.
        if (!m_pendingUpdate.downloadUrl.empty())
            offerUpdate(m_pendingUpdate, false);
    });
    // Offered only while a downloaded update waits for the reboot that applies
    // it. Asked for so the player does not have to leave the tab that told them
    // a restart was needed in order to perform it.
    m_themeShop->onUpdateRestart([this]() {
        auto& i18n = nxui::I18n::instance();
        m_audio.playSfx(Sfx::ModalShow);
        raiseOverlay(m_dialog);
        m_dialog->show(
            i18n.tr("themeshop.update_restart", "Restart now"),
            i18n.tr("themeshop.update_restart_confirm",
                    "The console restarts and applies the update while it starts. "
                    "This first boot takes about half a minute longer than usual."),
            {
                {i18n.tr("button.cancel", "Cancel"), [this]() {}, true},
                {i18n.tr("themeshop.update_restart", "Restart now"), [this]() {
                     DebugLog::log("[update] restart requested from the Update tab");
                     m_launcher.reboot();
                 }, false},
            },
            0, {});
        focusManager().setFocus(m_dialog.get());
    });

    m_themeShop->onNetConnectRequest([this]() {
        m_pendingNetConnect = true;
    });
    // O daemon relê os títulos e joga fora os nomes e ícones em cache. A grade
    // se reconstrói quando ele avisa que o catálogo mudou, então aqui não há o
    // que esperar.
    m_themeShop->onReloadCatalog([this]() {
        const Result rc = m_launcher.refreshCatalog();
        DebugLog::log("[catalog] reload requested rc=0x%X", rc);
        auto& i18n = nxui::I18n::instance();
        if (m_settings) {
            m_settings->requestToast(R_SUCCEEDED(rc)
                ? i18n.tr("settings.display.reload_grid_done", "Reading the installed titles again...")
                : i18n.tr("settings.display.reload_grid_failed", "Could not ask for a reload."), 2.6f);
        }
        m_refreshQueued = true;
        m_deferredRefreshFrames = std::max(m_deferredRefreshFrames, 3);
    });
    // The slow half, now that the ordinary reload no longer pays for it: every
    // name and icon is read from the titles again, about a second each.
    m_themeShop->onRebuildControlCache([this]() {
        const Result rc = m_launcher.rebuildControlCache();
        DebugLog::log("[catalog] control cache rebuild requested rc=0x%X", rc);
        auto& i18n = nxui::I18n::instance();
        if (m_settings) {
            m_settings->requestToast(R_SUCCEEDED(rc)
                ? i18n.tr("settings.display.rebuild_names_done",
                          "Reading names and icons again. This takes a few minutes.")
                : i18n.tr("settings.display.reload_grid_failed", "Could not ask for a reload."),
                4.0f);
        }
        m_refreshQueued = true;
        m_deferredRefreshFrames = std::max(m_deferredRefreshFrames, 3);
    });
    m_themeShop->onThemeShopApply([this](const std::string& presetId) {
        DebugLog::log("[theme-apply] request from Theme Shop: preset=%s", presetId.c_str());
        ThemePreset* preset = findPresetPtr(presetId);
        if (!preset) {
            DebugLog::log("[theme-apply] preset not found: %s", presetId.c_str());
            return;
        }

        // Refuse before touching any state: applying first and failing to load
        // the video left the purple fallback up while the UI said "applied".
        std::string rejectReason;
        if (!themeWallpaperPlayable(*preset, rejectReason)) {
            DebugLog::log("[theme-apply] refused preset=%s: %s", presetId.c_str(), rejectReason.c_str());
            showThemeRejectedDialog(*preset, rejectReason);
            return;
        }

        activateThemePreset(preset, true);
    });
    m_themeShop->onThemeShopDelete([this](const std::string& presetId) {
        auto& i18n = nxui::I18n::instance();
        ThemePreset* preset = findPresetPtr(presetId);
        if (!preset || preset->source == ThemePresetSource::BuiltIn) {
            if (!m_dialog) return;
            m_dialogReturnFocus = focusManager().current();
            m_dialog->show(
                i18n.tr("themeshop.installed.remove", "Remove Theme"),
                i18n.tr("settings.theme.builtin_readonly", "Built-in presets cannot be modified."),
                {{ i18n.tr("button.ok", "OK"), {}, true }});
            focusManager().setFocus(m_dialog.get());
            return;
        }

        deletePreset(presetId);
    });
    m_themeShop->onThemeShopDownload([queueThemeTransfer](const std::string& themeId) {
        queueThemeTransfer(themeId, false);
    });
    m_themeShop->onThemeShopDownloadInstall([queueThemeTransfer](const std::string& themeId) {
        queueThemeTransfer(themeId, true);
    });
    m_themeShop->onDialogRequest([this](const std::string& title,
                                        const std::string& msg,
                                        std::vector<ThemeShopScreen::DialogButtonDef> buttons) {
        if (!m_dialog) return;
        std::vector<OverlayDialog::ButtonDef> dlgButtons;
        bool preserveReturnFocus = (m_dialog && m_dialog->isActive() && focusManager().current() == m_dialog.get() && m_dialogReturnFocus != nullptr);
        for (size_t i = 0; i < buttons.size(); ++i) {
            auto cb = buttons[i].onPress;
            bool isLast = (i == buttons.size() - 1);
            if (buttons.size() == 1) {
                dlgButtons.push_back({buttons[i].label, [this, cb]() {
                    m_audio.playSfx(Sfx::ConfirmPositive);
                    m_dialog->hide();
                    if (cb) cb();
                }, true});
            } else if (isLast) {
                dlgButtons.push_back({buttons[i].label, [cb]() { if (cb) cb(); }, true});
            } else {
                dlgButtons.push_back({buttons[i].label, [this, cb]() {
                    m_audio.playSfx(Sfx::ConfirmPositive);
                    m_dialog->hide();
                    if (cb) cb();
                }, true});
            }
        }
        if (!preserveReturnFocus)
            m_dialogReturnFocus = focusManager().current();
        m_dialog->show(title, msg, std::move(dlgButtons));
        focusManager().setFocus(m_dialog.get());
    });
    m_themeShop->onRequestTextEntry([this](const std::string& title,
                                           const std::string& guide,
                                           const std::string& initial,
                                           std::function<void(std::string)> onAccept) {
        requestTextEntry(title, guide, initial, 64, false, std::move(onAccept), nullptr);
    });
    m_themeShop->onProgressShow([this](const std::string& title, const std::string& message, float p) {
        if (!m_progressDialog) return;
        raiseOverlay(m_progressDialog);
        m_progressDialog->setTheme(&m_theme);
        m_progressDialog->show(title, message, p);
        focusManager().setFocus(m_progressDialog.get());
    });
    m_themeShop->onProgressUpdate([this](const std::string& message, float p) {
        if (m_progressDialog && m_progressDialog->isActive()) {
            m_progressDialog->updateState(message, p);
        }
    });
    m_themeShop->onProgressHide([this]() {
        if (m_progressDialog && m_progressDialog->isActive()) {
            m_progressDialog->hide();
        }
    });
    m_themeShop->onMusicDownloaded([this]() {
        DebugLog::log("[themeshop] Music downloaded, reloading BGM tracks...");
        reloadMusicTracks();
    });
    m_themeShop->onPlayMusic([this](const std::string& path, const std::string& title) {
        DebugLog::log("[themeshop] Playing track: %s (%s)", title.c_str(), path.c_str());
        m_audio.stop();
        m_audio.clearTracks();
        m_audio.loadTrack(path, title);
        m_audio.play();
        syncMediaCenterState();
    });
    m_themeShop->onClosed([this, clearCompletedThemeTransferState]() {
        m_themeShop->resetMusicTabState();
        m_configSaveFuture = m_threadPool.submit([cfg = m_config]() {
            cfg.save();
        });
        DebugLog::log("[config] save queued");
        clearCompletedThemeTransferState();
        if (isCurrentFocusableWidget(m_themeShopReturnFocus)) {
            m_suppressNextNavigateSfx = true;
            focusManager().setFocus(m_themeShopReturnFocus);
        } else if (m_grid && m_grid->focusManager().current()) {
            m_suppressNextNavigateSfx = true;
            focusManager().setFocus(m_grid->focusManager().current());
        } else if (isCurrentFocusableWidget(m_sidebar.themeShopButton())) {
            m_suppressNextNavigateSfx = true;
            focusManager().setFocus(m_sidebar.themeShopButton());
        }
        m_themeShopReturnFocus = nullptr;
    });

    raiseOverlay(m_dialog);
    raiseOverlay(m_progressDialog);
    raiseOverlay(m_launchAnim);
    raiseOverlay(m_pointerCursor);

}

void WiiUMenuApp::createGameGallery() {
    if (m_gameGallery)
        return;

    m_gameGallery = std::make_shared<GameGalleryScreen>();
    if (m_overlayLayer)
        m_overlayLayer->addChild(m_gameGallery);
    m_gameGallery->setFont(&m_fontNormal);
    m_gameGallery->setSmallFont(&m_fontSmall);
    m_gameGallery->setTheme(&m_theme);
    m_gameGallery->setThreadPool(&m_threadPool);
    m_gameGallery->setRenderContext(&app().gpu(), &app().renderer());
    m_gameGallery->setAccessibilityVoiceEnabled(m_config.accessibilityEnabled);
    m_gameGallery->setAccessibilitySpeechPreferences(m_config.accessibilitySpeakHints,
                                                     m_config.accessibilitySpeakPosition);
    m_gameGallery->onNavigateSfx([this]() { m_audio.playSfx(Sfx::Navigate); });
    m_gameGallery->onActivateSfx([this]() { m_audio.playSfx(Sfx::Activate); });
    m_gameGallery->onCloseSfx([this]() { m_audio.playSfx(Sfx::ModalHide); });
    m_gameGallery->onAccessibilityAnnouncement([this](const std::string& text) {
        m_accessibility.announce(text);
    });
    m_gameGallery->onApplyArtwork([this](std::uint64_t titleId, const std::string& title,
                                         GameGalleryClient::Category category,
                                         GameGalleryClient::Asset asset,
                                         std::shared_ptr<const std::vector<std::uint8_t>> bytes) {
        confirmGameArtwork(titleId, title, category, std::move(asset), std::move(bytes));
    });
    m_gameGallery->onResetArtwork([this](std::uint64_t titleId, const std::string& title,
                                         GameGalleryClient::Category category) {
        confirmRestoreGameArtwork(titleId, title, category);
    });
    m_gameGallery->onClosed([this]() {
        if (m_gameDetails && m_gameGalleryReturnFocus == m_gameDetails.get()) {
            m_gameDetails->resumeFromChild();
            m_suppressNextNavigateSfx = true;
            focusManager().setFocus(m_gameDetails.get());
        } else if (isCurrentFocusableWidget(m_gameGalleryReturnFocus)) {
            m_suppressNextNavigateSfx = true;
            focusManager().setFocus(m_gameGalleryReturnFocus);
        }
        m_gameGalleryReturnFocus = nullptr;
    });
}

void WiiUMenuApp::createGameDetails() {
    if (m_gameDetails)
        return;

    m_gameDetails = std::make_shared<GameDetailsScreen>();
    if (m_overlayLayer)
        m_overlayLayer->addChild(m_gameDetails);
    m_gameDetails->setFont(&m_fontNormal);
    m_gameDetails->setSmallFont(&m_fontSmall);
    m_gameDetails->setTheme(&m_theme);
    m_gameDetails->setThreadPool(&m_threadPool);
    m_gameDetails->setRenderContext(&app().gpu(), &app().renderer());
    m_gameDetails->setAccessibilityVoiceEnabled(m_config.accessibilityEnabled);
    m_gameDetails->setAccessibilitySpeechPreferences(m_config.accessibilitySpeakHints,
                                                     m_config.accessibilitySpeakPosition);
    m_gameDetails->onNavigateSfx([this]() { m_audio.playSfx(Sfx::Navigate); });
    m_gameDetails->onActivateSfx([this]() { m_audio.playSfx(Sfx::Activate); });
    m_gameDetails->onCloseSfx([this]() { m_audio.playSfx(Sfx::ModalHide); });
    m_gameDetails->onAccessibilityAnnouncement([this](const std::string& text) {
        m_accessibility.announce(text);
    });
    m_gameDetails->onOpenGallery([this]() {
        if (!m_gameDetails) return;
        const std::uint64_t titleId = m_gameDetails->titleId();
        const std::string title = m_gameDetails->title();
        // Gallery takes over immediately. Its Back action must resume this
        // dossier, not skip the whole parent flow and land on the home icon.
        m_gameDetailsReturnFocus = nullptr;
        m_gameDetails->hide();
        showGameGallery(titleId, title);
    });
    m_gameDetails->onShowArtwork([this]() {
        if (!m_gameDetails) return;
        m_dialogReturnFocus = m_gameDetails.get();
        showGameArtworkStatus(m_gameDetails->titleId(), m_gameDetails->title());
    });
    m_gameDetails->onRestoreArtwork([this]() {
        if (!m_gameDetails) return;
        m_dialogReturnFocus = m_gameDetails.get();
        showGameArtworkRestoreMenu(m_gameDetails->titleId(), m_gameDetails->title());
    });
    m_gameDetails->onManageMods([this]() {
        if (!m_gameDetails) return;
        showGameMods(m_gameDetails->titleId(), m_gameDetails->title());
    });
    m_gameDetails->onCheats([this]() {
        if (!m_gameDetails) return;
        showGameCheats(m_gameDetails->titleId(), m_gameDetails->title());
    });
    m_gameDetails->onFolderAction([this]() {
        if (!m_gameDetails) return;
        showFolderAssignment(m_gameDetails->titleId(), m_gameDetails->title());
    });
    m_gameDetails->onRemoveGamePort([this]() {
        if (!m_gameDetails) return;
        removeGamePort(m_gameDetails->titleId());
    });
    m_gameDetails->onMarkAsGamePort([this]() {
        if (!m_gameDetails) return;
        m_dialogReturnFocus = m_gameDetails.get();
        showGamePortPlatformMenu(m_gameDetails->titleId(), m_gameDetails->title());
    });
    m_gameDetails->onEditSearchTitle([this]() {
        if (!m_gameDetails) return;
        const std::uint64_t titleId = m_gameDetails->titleId();
        auto& i18n = nxui::I18n::instance();
        requestTextEntry(
            i18n.tr("dialog.edit_search_title", "Edit search title"),
            i18n.tr("dialog.edit_search_title_guide", "Search title"),
            m_gameDetails->searchTitle(), 128, false,
            [this, titleId](const std::string& value) {
                const std::string trimmed = trimWhitespace(value);
                if (trimmed.empty()) return;
                m_config.setGamePortSearchTitle(titleId, trimmed);
                m_config.save();
                if (m_gameDetails && m_gameDetails->titleId() == titleId)
                    m_gameDetails->updateSearchTitle(trimmed);
            });
    });
    // Only reachable from the dossier once a title is already marked as a
    // port -- "Mark as game port" is offered instead in the rail when it is
    // not. Folds edit-search-title and unmark behind one entry rather than
    // two permanent rail rows, matching the same "roll related settings into
    // one submenu" shrink applied to Active artwork.
    m_gameDetails->onPortOptions([this]() {
        if (!m_gameDetails || !m_dialog) return;
        raiseOverlay(m_dialog);
        const std::uint64_t titleId = m_gameDetails->titleId();
        const std::string title = m_gameDetails->title();
        auto& i18n = nxui::I18n::instance();
        auto returnToDetails = [this]() {
            if (m_gameDetails && m_gameDetails->isActive())
                focusManager().setFocus(m_gameDetails.get());
        };
        m_dialog->show(
            i18n.tr("dialog.port_options", "Port options"), title,
            {
                {i18n.tr("dialog.details_rename", "Rename"),
                 [this, titleId]() {
                     auto& editI18n = nxui::I18n::instance();
                     requestTextEntry(
                         editI18n.tr("dialog.details_rename", "Rename"),
                         editI18n.tr("dialog.details_rename_guide",
                                     "Enter a name. Leave it empty to use the original."),
                         m_gameDetails ? m_gameDetails->title() : std::string{}, 128, false,
                         [this, titleId](const std::string& value) {
                             const std::string original = [&]() {
                                 for (const auto& app : m_allApps) {
                                     if (app.titleId != titleId) continue;
                                     return app.title;
                                 }
                                 return std::string{};
                             }();
                             const std::string custom = trimWhitespace(value);
                             m_config.setCustomTitle(titleId, custom);
                             m_config.save();
                             switchu::commitSdCard("custom title");
                             for (auto& app : m_allApps) {
                                 if (app.titleId != titleId) continue;
                                 app.title = m_config.customTitle(titleId, original);
                                 break;
                             }
                             if (m_gameDetails && m_gameDetails->titleId() == titleId)
                                 m_gameDetails->updateTitle(m_config.customTitle(titleId, original));
                             if (m_grid && m_openFolderId == 0)
                                 applyDisplayModel(buildRootFolderModel(), titleId, false);
                         });
                 }, false},
                {i18n.tr("dialog.edit_search_title", "Edit search title"),
                 [this, titleId]() {
                     auto& editI18n = nxui::I18n::instance();
                     requestTextEntry(
                         editI18n.tr("dialog.edit_search_title", "Edit search title"),
                         editI18n.tr("dialog.edit_search_title_guide", "Search title"),
                         m_gameDetails ? m_gameDetails->searchTitle() : std::string{}, 128, false,
                         [this, titleId](const std::string& value) {
                             const std::string trimmed = trimWhitespace(value);
                             if (trimmed.empty()) return;
                             m_config.setGamePortSearchTitle(titleId, trimmed);
                             m_config.save();
                             if (m_gameDetails && m_gameDetails->titleId() == titleId)
                                 m_gameDetails->updateSearchTitle(trimmed);
                         });
                 }, false},
                {i18n.tr("dialog.unmark_port", "Unmark port"),
                 [this, titleId]() { removeGamePort(titleId); }, false},
                {i18n.tr("button.cancel", "Cancel"), returnToDetails, true},
            },
            0, returnToDetails);
        focusManager().setFocus(m_dialog.get());
    });
    m_gameDetails->onRename([this]() {
        if (!m_gameDetails) return;
        const std::uint64_t titleId = m_gameDetails->titleId();
        auto& i18n = nxui::I18n::instance();
        requestTextEntry(
            i18n.tr("dialog.details_rename", "Rename"),
            i18n.tr("dialog.details_rename_guide",
                    "Enter a name. Leave it empty to use the original."),
            m_gameDetails->title(), 128, false,
            [this, titleId](const std::string& value) {
                // An empty field restores the original title reported by the
                // catalogue. Preserve it before changing the visible entry so
                // the restoration does not simply reapply the custom name.
                const std::string original = [&]() {
                    for (const auto& app : m_allApps) {
                        if (app.titleId != titleId) continue;
                        return app.title;
                    }
                    return std::string{};
                }();
                const std::string custom = trimWhitespace(value);
                m_config.setCustomTitle(titleId, custom);
                m_config.save();
                switchu::commitSdCard("custom title");
                for (auto& app : m_allApps) {
                    if (app.titleId != titleId) continue;
                    app.title = m_config.customTitle(titleId, original);
                    break;
                }
                if (m_gameDetails && m_gameDetails->titleId() == titleId)
                    m_gameDetails->updateTitle(m_config.customTitle(titleId, original));
                // The entry above is what the grid, the sort and the folders
                // are built from, so recomposing is enough -- and instant.
                // Re-reading the catalogue would cost a full control-data pass
                // over every installed title for one renamed game.
                if (m_grid && m_openFolderId == 0)
                    applyDisplayModel(buildRootFolderModel(), titleId, false);
            });
    });
    m_gameDetails->onToggleFavorite([this]() {
        if (!m_gameDetails) return;
        const std::uint64_t titleId = m_gameDetails->titleId();
        const bool current = m_config.isFavorite(titleId);
        m_config.setFavorite(titleId, !current);
        m_config.save();
        switchu::commitSdCard("toggle favorite");
        m_gameDetails->setFavorite(!current);
        if (!current) {
            m_audio.playSfx(Sfx::Activate);
        } else {
            m_audio.playSfx(Sfx::ToggleOff);
        }
        if (m_openFolderId != 0) {
            applyDisplayModel(buildOpenFolderModel(m_openFolderId), titleId, false);
        } else {
            applyDisplayModel(buildRootFolderModel(), titleId, false);
        }
    });
    m_gameDetails->onDeleteSoftware([this]() {
        if (!m_gameDetails) return;
        m_dialogReturnFocus = m_gameDetails.get();
        confirmDeleteSoftware(m_gameDetails->titleId(), m_gameDetails->title());
    });
    m_gameDetails->onClosed([this]() {
        if (isCurrentFocusableWidget(m_gameDetailsReturnFocus)) {
            m_suppressNextNavigateSfx = true;
            focusManager().setFocus(m_gameDetailsReturnFocus);
        }
        m_gameDetailsReturnFocus = nullptr;
    });
}

void WiiUMenuApp::reloadThemePresets() {
    m_allPresets = ThemePreset::builtInPresets();
    auto userPresets = ThemePreset::loadUserPresets();
    auto installedPackages = ThemePreset::loadInstalledPackages();
    m_allPresets.insert(m_allPresets.end(), userPresets.begin(), userPresets.end());
    m_allPresets.insert(m_allPresets.end(), installedPackages.begin(), installedPackages.end());
    DebugLog::log("[theme-apply] reloadThemePresets: builtIn=%zu user=%zu installed=%zu total=%zu",
                  ThemePreset::builtInPresets().size(), userPresets.size(), installedPackages.size(), m_allPresets.size());
}

void WiiUMenuApp::startThemePackageTransfer(const ThemeCatalogClient::Entry& entry, bool installMode) {
    syncThemePackageTransfer();

    if (m_themePackageTransferFuture.valid()
        && m_themePackageTransferFuture.wait_for(std::chrono::seconds(0)) != std::future_status::ready) {
        if (m_themeShop) {
            auto& i18n = nxui::I18n::instance();
            m_themeShop->requestToast(i18n.tr("themeshop.community.transfer_busy",
                                              "Another theme transfer is already running."),
                                      2.5f);
        }
        return;
    }

    auto shared = std::make_shared<ThemePackageTransferShared>();
    shared->themeId = entry.id;
    shared->installMode = installMode;
    shared->destinationPath = ThemePackageInstaller::destinationRootFor(
        entry.id,
        installMode ? ThemePackageInstaller::Mode::InstallAndApply
                    : ThemePackageInstaller::Mode::InstallOnly);
    auto& i18n = nxui::I18n::instance();
    const std::string preparingLabel = installMode
        ? i18n.tr("themeshop.transfer.prepare_apply", "Preparing download + apply...")
        : i18n.tr("themeshop.transfer.prepare_install", "Preparing download + install...");
    const std::string unknownTransferError = i18n.tr("themeshop.transfer.unknown_error", "Unknown transfer error");
    shared->state.begin(preparingLabel);
    shared->revision = 1;

    m_themePackageTransfer = shared;
    m_themePackageTransferUiRevision = 0;
    m_themePackageTransferHandledRevision = 0;

    if (m_themeShop)
        m_themeShop->setPackageTransferState(shared->state, shared->themeId, shared->installMode);
    if (m_progressDialog) {
        m_progressDialog->setTheme(&m_theme);
        raiseOverlay(m_progressDialog);
        m_progressDialog->show(i18n.tr("themeshop.transfer.dialog_title", "Downloading Theme"),
                               preparingLabel,
                               -1.f);
        focusManager().setFocus(m_progressDialog.get());
    }

    // The entry's own index, not the shop's first one: with two catalogues
    // merged, half the entries resolve against the other.
    const std::string catalogUrl = !entry.catalogUrl.empty()
        ? entry.catalogUrl
        : (m_themeShop ? m_themeShop->communityCatalogUrl()
                       : std::string(ThemeCatalogClient::kDefaultCatalogUrl));
    ThemePackageInstaller::Mode mode = installMode
        ? ThemePackageInstaller::Mode::InstallAndApply
        : ThemePackageInstaller::Mode::InstallOnly;

    DebugLog::log("[themeshop] package transfer start: id=%s apply=%d",
                  entry.id.c_str(),
                  installMode ? 1 : 0);

    m_themePackageTransferFuture = m_threadPool.submit([shared, catalogUrl, entry, mode, unknownTransferError]() {
        try {
            auto result = ThemePackageInstaller::run(
                catalogUrl,
                entry,
                mode,
                [shared](std::string label, float progress) {
                    std::lock_guard<std::mutex> lk(shared->mutex);
                    shared->state.update(std::move(label), progress);
                    ++shared->revision;
                });

            std::lock_guard<std::mutex> lk(shared->mutex);
            shared->destinationPath = result.destinationPath;
            shared->state.succeed(result.message);
            ++shared->revision;
            DebugLog::log("[themeshop] package transfer success: %s", result.themeId.c_str());
        } catch (const std::exception& ex) {
            std::lock_guard<std::mutex> lk(shared->mutex);
            shared->state.fail(ex.what());
            ++shared->revision;
            DebugLog::log("[themeshop] package transfer failed: %s", ex.what());
        } catch (...) {
            std::lock_guard<std::mutex> lk(shared->mutex);
            shared->state.fail(unknownTransferError);
            ++shared->revision;
            DebugLog::log("[themeshop] package transfer failed: unknown error");
        }
    });
}

void WiiUMenuApp::syncThemePackageTransfer() {
    auto shared = m_themePackageTransfer;
    if (!shared)
        return;

    bool futureReady = false;
    if (m_themePackageTransferFuture.valid()
        && m_themePackageTransferFuture.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        m_themePackageTransferFuture.get();
        futureReady = true;
    }

    ThemeTransferState state;
    std::string themeId;
    bool installMode = false;
    std::uint64_t revision = 0;
    std::string destinationPath;
    {
        std::lock_guard<std::mutex> lk(shared->mutex);
        state = shared->state;
        themeId = shared->themeId;
        installMode = shared->installMode;
        revision = shared->revision;
        destinationPath = shared->destinationPath;
    }

    if (revision != m_themePackageTransferUiRevision) {
        if (m_themeShop)
            m_themeShop->setPackageTransferState(state, themeId, installMode);
        if (m_progressDialog && state.isRunning()) {
            auto& i18n = nxui::I18n::instance();
            m_progressDialog->updateState(
                state.label().empty()
                    ? i18n.tr("themeshop.transfer.downloading", "Downloading theme...")
                    : state.label(),
                state.progress01());
            if (focusManager().current() != m_progressDialog.get())
                focusManager().setFocus(m_progressDialog.get());
        }
        m_themePackageTransferUiRevision = revision;
    }

    if (!futureReady || revision == m_themePackageTransferHandledRevision)
        return;

    if (state.isReady()) {
        DebugLog::log("[theme-apply] transfer ready: id=%s installMode=%d destination=%s",
                      themeId.c_str(),
                      installMode ? 1 : 0,
                      safeLogPath(destinationPath));

        reloadThemePresets();
        DebugLog::log("[theme-apply] presets reloaded after transfer: count=%zu", m_allPresets.size());

        auto& i18n = nxui::I18n::instance();
        std::string successMessage;
        if (installMode) {
            ThemePreset* preset = findPresetPtr("package:" + themeId);
            std::string rejectReason;
            if (preset && !themeWallpaperPlayable(*preset, rejectReason)) {
                DebugLog::log("[theme-apply] auto-apply refused id=%s: %s", themeId.c_str(), rejectReason.c_str());
                refreshThemeShopState();
                showThemeRejectedDialog(*preset, rejectReason);
                successMessage = i18n.tr("themeshop.transfer.installed", "Theme installed.");
            } else if (preset) {
                m_forceThemeResourceReload = !preset->fonts.regularPath.empty()
                    || !preset->fonts.smallPath.empty()
                    || !preset->background.imagePath.empty();
                if (!preset->icons.basePath.empty())
                    m_sidebar.invalidateAssetsCache();
                DebugLog::log("[theme-apply] auto-applying installed package: id=%s", themeId.c_str());
                activateThemePreset(preset, true);
                successMessage = i18n.tr("themeshop.transfer.installed_applied", "Theme installed and applied.");
            } else {
                DebugLog::log("[theme-apply] auto-apply failed, preset missing after install: id=%s", themeId.c_str());
                refreshThemeShopState();
                successMessage = i18n.tr("themeshop.transfer.installed_apply_failed",
                                         "Theme installed, but it could not be applied automatically.");
            }
        } else {
            DebugLog::log("[theme-apply] install-only transfer complete: id=%s", themeId.c_str());
            refreshThemeShopState();
            successMessage = i18n.tr("themeshop.transfer.installed", "Theme installed.");
        }

        state.succeed(successMessage);
        if (m_themeShop)
            m_themeShop->setPackageTransferState(state, themeId, installMode);
    }

    if (m_themeShop && !state.label().empty())
        m_themeShop->requestToast(state.label(), state.hasFailed() ? 3.5f : 2.8f);
    if (m_progressDialog && m_progressDialog->isActive()) {
        m_progressDialog->updateState(state.label(), state.progress01());
        m_progressDialog->hide();
        if (m_themeShop && m_themeShop->isActive())
            focusManager().setFocus(m_themeShop.get());
    }

    m_themePackageTransferHandledRevision = revision;
}

std::vector<ThemeShopScreen::ThemeShopEntry> WiiUMenuApp::buildThemeShopEntries() {
    std::vector<ThemeShopScreen::ThemeShopEntry> entries;
    entries.reserve(m_allPresets.size());

    ThemePreset* activePreset = findPresetPtr(m_activePresetName);
    std::string activeId = activePreset ? (activePreset->id.empty() ? activePreset->name : activePreset->id)
                                        : m_activePresetName;

    for (const auto& preset : m_allPresets) {
        ThemeShopScreen::ThemeShopEntry entry;
        entry.id = preset.id.empty() ? preset.name : preset.id;
        entry.name = preset.name;
        entry.author = preset.author;
        entry.version = preset.version;
        entry.source = sourceLabel(preset.source);
        if (preset.soundPreset.rfind("package:", 0) == 0)
            entry.soundPreset = "Bundled";
        else if (preset.soundPreset == "wiiu")
            entry.soundPreset = "Wii U";
        else
            entry.soundPreset = preset.soundPreset;
        entry.coverPath = installedThemePreviewPath(preset);
        // Só o caminho. Medir o que ele ocupa é percorrer centenas de quadros no
        // cartão, e isso a loja faz numa thread de trabalho, não aqui.
        entry.installPath = preset.installPath;
        entry.active = (entry.id == activeId);
        entry.removable = (preset.source != ThemePresetSource::BuiltIn);
        entries.push_back(std::move(entry));
    }

    return entries;
}

void WiiUMenuApp::refreshThemeShopState() {
    if (!m_themeShop) return;

    reloadThemePresets();

    ThemePreset* activePreset = findPresetPtr(m_activePresetName);
    std::string activeId = activePreset ? (activePreset->id.empty() ? activePreset->name : activePreset->id)
                                        : m_activePresetName;

    m_themeShop->setTheme(&m_theme);
    m_themeShop->setMusicState(m_audio.isPlaying(), m_audio.volume(), m_audio.sfxVolume());
    m_themeShop->setThemeShopState(buildThemeShopEntries(), activeId);
    m_themeShop->refreshState();
}

void WiiUMenuApp::activateThemePreset(ThemePreset* preset, bool applyBundledSound) {
    if (!preset) return;

    const std::string presetRef = preset->id.empty() ? preset->name : preset->id;
    DebugLog::log("[theme-apply] begin preset=%s name=%s source=%s installPath=%s applyBundledSound=%d soundPreset=%s",
                  presetRef.c_str(),
                  preset->name.c_str(),
                  themeSourceName(preset->source),
                  safeLogPath(preset->installPath),
                  applyBundledSound ? 1 : 0,
                  safeLogPath(preset->soundPreset));

    m_activePresetName = preset->id.empty() ? preset->name : preset->id;
    m_activeColors = preset->colors;
    m_activeMode = preset->mode;
    m_config.themePreset = m_activePresetName;

    DebugLog::log("[theme-apply] rebuildThemeFromColors start: preset=%s", presetRef.c_str());
    rebuildThemeFromColors();
    DebugLog::log("[theme-apply] rebuildThemeFromColors done: preset=%s", presetRef.c_str());


    if (applyBundledSound && !preset->soundPreset.empty()) {
        DebugLog::log("[theme-apply] changeSoundPreset queued: preset=%s sound=%s",
                      presetRef.c_str(),
                      preset->soundPreset.c_str());
        changeSoundPreset(preset->soundPreset);
        m_config.soundPreset = preset->soundPreset;
    } else {
        DebugLog::log("[theme-apply] bundled sound skipped: preset=%s apply=%d soundPreset=%s",
                      presetRef.c_str(),
                      applyBundledSound ? 1 : 0,
                      safeLogPath(preset->soundPreset));
    }

    DebugLog::log("[theme-apply] refreshThemeShopState start: preset=%s", presetRef.c_str());
    refreshThemeShopState();
    DebugLog::log("[theme-apply] refreshThemeShopState done: preset=%s", presetRef.c_str());
    m_themeRenderDebugFrames = 6;
    if (m_themeShop)
        m_themeShop->requestRenderDiagnostics(6);
    m_audio.playSfx(Sfx::ThemeToggle);
    DebugLog::log("[theme-apply] complete preset=%s", presetRef.c_str());
}

bool WiiUMenuApp::themeWallpaperPlayable(const ThemePreset& preset, std::string& reason) const {
    reason.clear();
    // Same path resolution as applyThemeResources().
    const std::string imagePath = resolveThemeAssetPath(preset, preset.background.imagePath);
    const std::string videoPath = !preset.background.videoPath.empty()
        ? resolveThemeAssetPath(preset, preset.background.videoPath)
        : (switchu::video::isVideoPath(imagePath) ? imagePath : std::string());
    if (videoPath.empty() || !pathExists(videoPath))
        return true;   // no video wallpaper: nothing to reject
    return switchu::video::probeWallpaper(videoPath, reason);
}

void WiiUMenuApp::showThemeRejectedDialog(const ThemePreset& preset, const std::string& reason) {
    if (!m_dialog) return;
    auto& i18n = nxui::I18n::instance();
    const std::string name = preset.name.empty() ? preset.id : preset.name;
    m_dialogReturnFocus = focusManager().current();
    m_dialog->show(
        i18n.tr("themeshop.unsupported.title", "Theme not supported"),
        name + "\n" +
        i18n.tr("themeshop.unsupported.body",
                "This theme's video wallpaper cannot be played on this console, so it was not applied. "
                "Supported: H.264, up to 1920x1080 at 60 fps.") +
        "\n(" + reason + ")",
        {{ i18n.tr("button.ok", "OK"), {}, true }});
    focusManager().setFocus(m_dialog.get());
}

// Troca a trilha pela do tema. Fica separado de changeSoundPreset porque as
// duas coisas nao andam juntas: o preset governa os efeitos e continua valendo,
// enquanto as faixas passam a ser propriedade do tema.
void WiiUMenuApp::applyThemeMusic(const std::vector<std::string>& tracks) {
    if (m_audioFuture.valid())
        m_audioFuture.get();   // o audio carrega em outra linha; sem isto as
                               // faixas do preset chegariam depois destas
    if (tracks.empty()) {
        DebugLog::log("[audio] theme declared music but none of it exists");
        return;
    }

    m_themeMusicTracks = tracks;
    reloadMusicTracks();
}

void WiiUMenuApp::applyUiLanguage() {
    auto& i18n = nxui::I18n::instance();
    if (m_config.uiLanguageOverride == "auto" || m_config.uiLanguageOverride.empty())
        i18n.setLanguageAuto();
    else
        i18n.setLanguage(m_config.uiLanguageOverride);
    m_accessibility.setVoiceForLanguageTag(i18n.activeLanguageTag());
}

std::string WiiUMenuApp::resolveThemeAssetPath(const ThemePreset& preset, const std::string& rawPath) const {
    if (rawPath.empty())
        return {};
    if (isAbsoluteThemePath(rawPath))
        return rawPath;
    if ((preset.source == ThemePresetSource::InstalledPackage || preset.source == ThemePresetSource::BuiltIn)
        && !preset.installPath.empty())
        return joinPath(preset.installPath, rawPath);
    return joinPath(SD_ASSETS, rawPath);
}

ThemePreset WiiUMenuApp::buildEffectiveThemePreset() {
    ThemePreset effective;
    if (ThemePreset* preset = findPresetPtr(m_activePresetName))
        effective = *preset;

    effective.mode = m_activeMode;
    effective.colors = m_activeColors;
    return effective;
}

void WiiUMenuApp::applyThemeResources(const ThemePreset& preset) {
    // A trilha do tema decidida aqui, e nao no caminho de aplicar manualmente.
    // Este e o unico ponto por onde os dois passam: no boot o tema e carregado
    // direto por aqui, entao a musica so trocava quando alguem aplicava a mao,
    // e reiniciar o console devolvia a trilha do preset.
    //
    // Definida sempre, inclusive vazia: um tema sem trilha precisa devolver a
    // musica ao preset, e isso tem de estar resolvido antes de changeSoundPreset
    // ser disparado, porque ele carrega em outra thread e le este valor.
    m_themeMusicTracks.clear();
    if (!preset.music.empty()) {
        for (const auto& track : preset.music) {
            std::string path = resolveThemeAssetPath(preset, track);
            std::error_code ec;
            if (!path.empty() && std::filesystem::exists(path, ec))
                m_themeMusicTracks.push_back(std::move(path));
            else
                DebugLog::log("[theme-apply] theme track missing: %s", safeLogPath(path));
        }
        applyThemeMusic(m_themeMusicTracks);
    }

    auto& gpu = app().gpu();
    auto& ren = app().renderer();
    const bool forceResourceReload = m_forceThemeResourceReload;

    const std::string fallbackFontPath = std::string(SD_ASSETS) + "/fonts/DejaVuSans.ttf";
    const std::string regularFontPath = resolveThemeAssetPath(preset, preset.fonts.regularPath);
    const std::string smallFontPath = resolveThemeAssetPath(
        preset,
        !preset.fonts.smallPath.empty() ? preset.fonts.smallPath : preset.fonts.regularPath);
    const std::string themeIconsBase = resolveThemeAssetPath(preset, preset.icons.basePath);
    const std::string backgroundImagePath = resolveThemeAssetPath(preset, preset.background.imagePath);

    const std::string presetRef = preset.id.empty() ? preset.name : preset.id;
    DebugLog::log("[theme-apply] resources start: preset=%s regularFont=%s smallFont=%s iconsBase=%s bgImage=%s",
                  presetRef.c_str(),
                  safeLogPath(regularFontPath),
                  safeLogPath(smallFontPath),
                  safeLogPath(themeIconsBase),
                  safeLogPath(backgroundImagePath));

    bool settingsLayoutNeedsRebuild = false;

    const bool regularFontExists = !regularFontPath.empty() && pathExists(regularFontPath);
    const std::string desiredRegularFontPath = regularFontExists ? regularFontPath : fallbackFontPath;
    const bool regularFontNeedsReload = forceResourceReload || m_loadedRegularFontPath != desiredRegularFontPath;

    const bool smallFontExists = !smallFontPath.empty() && pathExists(smallFontPath);
    const std::string desiredSmallFontPath = smallFontExists ? smallFontPath : fallbackFontPath;
    const bool smallFontNeedsReload = forceResourceReload || m_loadedSmallFontPath != desiredSmallFontPath;

    const std::string defaultGameCardPath = std::string(SD_ASSETS) + "/icons/gamecard.png";
    const std::string gameCardPath = !themeIconsBase.empty() ? joinPath(themeIconsBase, "gamecard.png") : std::string();
    const bool gameCardExists = !gameCardPath.empty() && pathExists(gameCardPath);
    const std::string desiredGameCardPath = gameCardExists ? gameCardPath : defaultGameCardPath;
    const bool gameCardNeedsReload = forceResourceReload || m_loadedGameCardPath != desiredGameCardPath;

    const std::string backgroundVideoPath = !preset.background.videoPath.empty()
        ? resolveThemeAssetPath(preset, preset.background.videoPath)
        : (switchu::video::isVideoPath(backgroundImagePath) ? backgroundImagePath : std::string());
    const bool videoExists = !backgroundVideoPath.empty() && pathExists(backgroundVideoPath);
    const bool wantsVideo = videoExists;
    const bool isVideoLoaded = m_background && m_background->isVideoActive();

    const bool imageExists = !backgroundImagePath.empty() && pathExists(backgroundImagePath);
    const bool wantsBackgroundImage = imageExists || wantsVideo;
    // A frames-only theme has no imagePath, so both sides of the path check are
    // empty and this decided nothing needed loading -- the frames never loaded
    // and the wallpaper came up blank. The gate has to know about the sequence
    // as well as the single image.
    const bool wantsFrames = !preset.background.imageFrames.empty();
    const bool hasFramesLoaded = m_background && m_background->hasAnimatedBackground();
    const bool backgroundImageNeedsReload = forceResourceReload
        || m_loadedBackgroundImagePath != backgroundImagePath
        || m_backgroundImageLoaded != wantsBackgroundImage
        || wantsFrames != hasFramesLoaded
        || wantsVideo != isVideoLoaded;

    const bool needsGpuResourceReload = regularFontNeedsReload
        || smallFontNeedsReload
        || gameCardNeedsReload
        || backgroundImageNeedsReload;
    if (needsGpuResourceReload) {
        DebugLog::log("[theme-apply] waiting for GPU idle before reloading theme resources");
        gpu.waitIdle();
    }

    if (regularFontNeedsReload) {
        bool regularFontLoaded = regularFontExists && m_fontNormal.load(gpu, ren, regularFontPath, 24);
        DebugLog::log("[theme-apply] regular font: path=%s exists=%d reloaded=%d loaded=%d",
                      safeLogPath(regularFontPath),
                      regularFontExists ? 1 : 0,
                      1,
                      regularFontLoaded ? 1 : 0);
        if (!regularFontLoaded) {
            regularFontLoaded = m_fontNormal.load(gpu, ren, fallbackFontPath, 24);
            DebugLog::log("[theme-apply] regular font fallback: path=%s loaded=%d",
                          fallbackFontPath.c_str(),
                          regularFontLoaded ? 1 : 0);
        }
        if (regularFontLoaded) {
            m_loadedRegularFontPath = desiredRegularFontPath;
            settingsLayoutNeedsRebuild = true;
        }
    } else {
        DebugLog::log("[theme-apply] regular font: path=%s exists=%d reloaded=%d loaded=%d",
                      safeLogPath(regularFontPath),
                      regularFontExists ? 1 : 0,
                      0,
                      1);
    }

    if (smallFontNeedsReload) {
        bool smallFontLoaded = smallFontExists && m_fontSmall.load(gpu, ren, smallFontPath, 18);
        DebugLog::log("[theme-apply] small font: path=%s exists=%d reloaded=%d loaded=%d",
                      safeLogPath(smallFontPath),
                      smallFontExists ? 1 : 0,
                      1,
                      smallFontLoaded ? 1 : 0);
        if (!smallFontLoaded) {
            smallFontLoaded = m_fontSmall.load(gpu, ren, fallbackFontPath, 18);
            DebugLog::log("[theme-apply] small font fallback: path=%s loaded=%d",
                          fallbackFontPath.c_str(),
                          smallFontLoaded ? 1 : 0);
        }
        if (smallFontLoaded) {
            m_loadedSmallFontPath = desiredSmallFontPath;
            settingsLayoutNeedsRebuild = true;
        }
    } else {
        DebugLog::log("[theme-apply] small font: path=%s exists=%d reloaded=%d loaded=%d",
                      safeLogPath(smallFontPath),
                      smallFontExists ? 1 : 0,
                      0,
                      1);
    }

    if (gameCardNeedsReload) {
        bool gameCardLoaded = gameCardExists && m_gameCardTex.loadFromFile(gpu, ren, gameCardPath);
        DebugLog::log("[theme-apply] gamecard icon: path=%s exists=%d reloaded=%d loaded=%d",
                      safeLogPath(gameCardPath),
                      gameCardExists ? 1 : 0,
                      1,
                      gameCardLoaded ? 1 : 0);
        if (!gameCardLoaded) {
            gameCardLoaded = m_gameCardTex.loadFromFile(gpu, ren, defaultGameCardPath);
            DebugLog::log("[theme-apply] gamecard fallback: path=%s loaded=%d",
                          defaultGameCardPath.c_str(),
                          gameCardLoaded ? 1 : 0);
        }
        if (gameCardLoaded)
            m_loadedGameCardPath = desiredGameCardPath;
    } else {
        DebugLog::log("[theme-apply] gamecard icon: path=%s exists=%d reloaded=%d loaded=%d",
                      safeLogPath(gameCardPath),
                      gameCardExists ? 1 : 0,
                      0,
                      1);
    }

    if (m_background) {
        DebugLog::log("[theme-apply] background config start: preset=%s", presetRef.c_str());
        WaraWaraBackground::Config backgroundConfig;
        backgroundConfig.layout = toBackgroundLayout(preset.background.layout);
        backgroundConfig.shapeSet = toBackgroundShapeSet(preset.background.shapeSet);
        backgroundConfig.symmetry = toBackgroundSymmetry(preset.background.symmetry);
        backgroundConfig.shapeCount = preset.background.shapeCount;
        backgroundConfig.gridColumns = preset.background.gridColumns;
        backgroundConfig.gridRows = preset.background.gridRows;
        backgroundConfig.spacingX = preset.background.spacingX;
        backgroundConfig.spacingY = preset.background.spacingY;
        backgroundConfig.sizeMin = preset.background.sizeMin;
        backgroundConfig.sizeMax = preset.background.sizeMax;
        backgroundConfig.speedMin = preset.background.speedMin;
        backgroundConfig.speedMax = preset.background.speedMax;
        backgroundConfig.wobble = preset.background.wobble;
        backgroundConfig.opacity = preset.background.opacity;
        backgroundConfig.rotationSpeed = preset.background.rotationSpeed;
        backgroundConfig.fixedOrientation = preset.background.fixedOrientation;
        backgroundConfig.orientationDegrees = preset.background.orientationDegrees;
        backgroundConfig.cornerRoundness = preset.background.cornerRoundness;
        backgroundConfig.imageOpacity = preset.background.imageOpacity;
        backgroundConfig.imageCover = preset.background.imageCover;
        m_background->setConfig(backgroundConfig);
        m_background->setBlurStrength(m_config.backgroundBlur);
        m_background->setSpeedScale(m_config.backgroundSpeed * 2.f);
        // 0.35 was picked for drifting shapes, which have no speed of their own
        // to be wrong about. A frame sequence does: it was filmed at a rate, and
        // 0.35 plays a ten second loop over twenty-nine. So an animated theme
        // starts at the speed of its clip, until somebody moves the slider --
        // after which their choice is theirs, and applies to any theme.
        m_background->setWallpaperSpeedScale(
            (wantsFrames && !m_config.backgroundSpeedChosen) ? 1.f
                                                            : m_config.backgroundSpeed);

        if (backgroundImageNeedsReload) {
            // A theme that lists frames gets the moving wallpaper; one that
            // names a single image keeps the still, and pays exactly what it
            // paid before. Frames are resolved against the theme's own folder,
            // the same way its single image is.
            bool imageLoaded = false;
            if (wantsVideo) {
                imageLoaded = m_background->loadVideo(backgroundVideoPath, true);
                DebugLog::log("[theme-apply] background hardware video wallpaper: path=%s loaded=%d",
                              safeLogPath(backgroundVideoPath), imageLoaded ? 1 : 0);
            }
            if (!imageLoaded && !preset.background.imageFrames.empty()) {
                std::vector<std::string> framePaths;
                framePaths.reserve(preset.background.imageFrames.size());
                for (const auto& rel : preset.background.imageFrames) {
                    std::string abs = resolveThemeAssetPath(preset, rel);
                    if (!abs.empty() && pathExists(abs))
                        framePaths.push_back(std::move(abs));
                }
                imageLoaded = !framePaths.empty() &&
                              m_background->loadImageSequence(gpu, ren, framePaths,
                                                              preset.background.imageFps);
                DebugLog::log("[theme-apply] background frames: listed=%zu found=%zu loaded=%d",
                              preset.background.imageFrames.size(),
                              framePaths.size(), imageLoaded ? 1 : 0);
            }
            if (!imageLoaded)
                imageLoaded = imageExists && m_background->loadImage(gpu, ren, backgroundImagePath);
            DebugLog::log("[theme-apply] background image: path=%s exists=%d reloaded=%d loaded=%d",
                          safeLogPath(backgroundImagePath),
                          imageExists ? 1 : 0,
                          1,
                          imageLoaded ? 1 : 0);
            if (!imageLoaded) {
                if (m_backgroundImageLoaded)
                    m_background->clearImage();
                m_backgroundImageLoaded = false;
                m_loadedBackgroundImagePath.clear();
                DebugLog::log("[theme-apply] background image cleared");
            } else {
                m_backgroundImageLoaded = true;
                m_loadedBackgroundImagePath = backgroundImagePath;
            }
        } else {
            DebugLog::log("[theme-apply] background image: path=%s exists=%d reloaded=%d loaded=%d",
                          safeLogPath(backgroundImagePath),
                          imageExists ? 1 : 0,
                          0,
                          m_backgroundImageLoaded ? 1 : 0);
        }
    } else {
        DebugLog::log("[theme-apply] background widget missing");
    }

    if (settingsLayoutNeedsRebuild && m_settings && m_settings->isActive()) {
        m_settings->rebuildCurrentTab();
        DebugLog::log("[theme-apply] settings current tab rebuilt for font change");
    }

    DebugLog::log("[theme-apply] resources done: preset=%s", presetRef.c_str());
}

void WiiUMenuApp::applyTheme() {
    DebugLog::log("[theme-apply] widget recolor start");
    m_background->setAccentColor(m_theme.backgroundAccent);
    m_background->setSecondaryColor(m_theme.background);
    m_background->setShapeColor(m_theme.shapeColor);
    DebugLog::log("[theme-apply] widget recolor background done");

    for (auto& icon : m_grid->allIcons()) {
        icon->setBaseColor(m_theme.iconDefault);
        icon->setBorderColor(m_theme.panelBorder);
        icon->setHighlightColor(m_theme.panelHighlight);
        icon->setCornerRadius(m_theme.iconCornerRadius);
    }
    DebugLog::log("[theme-apply] widget recolor grid icons done");

    m_cursor->setColor(m_theme.cursorNormal);
    m_cursor->setCornerRadius(m_theme.cursorCornerRadius);
    m_cursor->setBorderWidth(m_theme.cursorBorderWidth);
    if (m_pointerCursor) {
        m_pointerCursor->setColor(m_theme.cursorNormal);
        m_pointerCursor->setCornerRadius(15.f);
        m_pointerCursor->setBorderWidth(2.5f);
    }
    DebugLog::log("[theme-apply] widget recolor cursors done");

    m_clock->setBaseColor(m_theme.panelBase);
    m_clock->setBorderColor(m_theme.panelBorder);
    m_clock->setHighlightColor(m_theme.panelHighlight);
    m_clock->setTextColor(m_theme.textPrimary);
    m_clock->setSecondaryTextColor(m_theme.textSecondary);

    m_battery->setBaseColor(m_theme.panelBase);
    m_battery->setBorderColor(m_theme.panelBorder);
    m_battery->setHighlightColor(m_theme.panelHighlight);
    m_battery->setTextColor(m_theme.textPrimary);

    m_titlePill->setBaseColor(m_theme.panelBase);
    m_titlePill->setBorderColor(m_theme.panelBorder);
    m_titlePill->setHighlightColor(m_theme.panelHighlight);
    m_titlePill->setTextColor(m_theme.textPrimary);

    m_pageIndicator->setBaseColor(m_theme.panelBase);
    m_pageIndicator->setBorderColor(m_theme.panelBorder);
    m_pageIndicator->setHighlightColor(m_theme.panelHighlight);
    m_pageIndicator->setTheme(&m_theme);
    for (auto& avatar : m_userAvatarButtons) {
        avatar->setBaseColor(m_theme.panelBase);
        avatar->setBorderColor(m_theme.panelBorder);
        avatar->setHighlightColor(m_theme.panelHighlight);
        avatar->setCornerRadius(28.f);
        avatar->setChromeEnabled(false);
    }
    DebugLog::log("[theme-apply] widget recolor HUD done");

    m_userSelect->setTheme(&m_theme);
    m_userSelect->cursor().setColor(m_theme.cursorNormal);

    if (m_dialog) {
        m_dialog->setTheme(&m_theme);
        m_dialog->setBaseColor(m_theme.panelBase);
        m_dialog->setBorderColor(m_theme.panelBorder);
        m_dialog->setHighlightColor(m_theme.panelHighlight);
        m_dialog->cursor().setColor(m_theme.cursorNormal);
    }
    DebugLog::log("[theme-apply] widget recolor overlays done");

    if (m_settings)
        m_settings->setTheme(&m_theme);
    if (m_themeShop)
        m_themeShop->setTheme(&m_theme);
    if (m_gameGallery)
        m_gameGallery->setTheme(&m_theme);
    if (m_gameMods)
        m_gameMods->setTheme(&m_theme);
    if (m_gameCheats)
        m_gameCheats->setTheme(&m_theme);
    if (m_gameDetails)
        m_gameDetails->setTheme(&m_theme);
    // The 1.2 overlays were left out of the recolor pass and kept the previous
    // theme's colours until the menu process was recreated.
    if (m_contextMenu)
        m_contextMenu->setTheme(&m_theme);
    if (m_gameOptions)
        m_gameOptions->setTheme(&m_theme);
    if (m_folderOptions)
        m_folderOptions->setTheme(&m_theme);
    if (m_controllerTest)
        m_controllerTest->setTheme(&m_theme);
    if (m_steamGridDbPicker)
        m_steamGridDbPicker->setTheme(&m_theme);
    if (m_platformPicker)
        m_platformPicker->setTheme(&m_theme);

    m_sidebar.applyTheme(m_theme);
    DebugLog::log("[theme-apply] widget recolor complete");
}

void WiiUMenuApp::rebuildThemeFromColors() {
    DebugLog::log("[theme-apply] rebuild start: activePreset=%s", m_activePresetName.c_str());
    m_effectivePreset = buildEffectiveThemePreset();
    DebugLog::log("[theme-apply] effective preset resolved: id=%s name=%s source=%s installPath=%s",
                  safeLogPath(m_effectivePreset.id),
                  m_effectivePreset.name.c_str(),
                  themeSourceName(m_effectivePreset.source),
                  safeLogPath(m_effectivePreset.installPath));
    m_theme = m_effectivePreset.toTheme();
    DebugLog::log("[theme-apply] theme object rebuilt");

    DebugLog::log("[theme-apply] applyThemeResources start: preset=%s",
                  safeLogPath(m_effectivePreset.id.empty() ? m_effectivePreset.name : m_effectivePreset.id));
    applyThemeResources(m_effectivePreset);
    DebugLog::log("[theme-apply] applyThemeResources done: preset=%s",
                  safeLogPath(m_effectivePreset.id.empty() ? m_effectivePreset.name : m_effectivePreset.id));

    const std::string customIconsBase = resolveThemeAssetPath(m_effectivePreset, m_effectivePreset.icons.basePath);
    DebugLog::log("[theme-apply] sidebar.reloadAssets start: customIcons=%s",
                  safeLogPath(customIconsBase));
    m_sidebar.reloadAssets(app().gpu(), app().renderer(), SD_ASSETS, customIconsBase);
    DebugLog::log("[theme-apply] sidebar.reloadAssets done");

    DebugLog::log("[theme-apply] applyTheme start");
    applyTheme();
    m_forceThemeResourceReload = false;
    DebugLog::log("[theme-apply] rebuild complete: activePreset=%s", m_activePresetName.c_str());
}

ThemePreset* WiiUMenuApp::findPresetPtr(const std::string& name) {
    for (auto& p : m_allPresets)
        if (p.id == name || p.name == name) return &p;
    return nullptr;
}

void WiiUMenuApp::deletePreset(const std::string& presetId) {
    ThemePreset* preset = findPresetPtr(presetId);
    if (!preset) return;

    ThemePreset* activePreset = findPresetPtr(m_activePresetName);
    std::string activeId = activePreset ? (activePreset->id.empty() ? activePreset->name : activePreset->id)
                                        : m_activePresetName;
    std::string idToDelete = preset->id.empty() ? preset->name : preset->id;
    std::string nameToDelete = preset->name;
    std::string installPath = preset->installPath;
    std::string soundPreset = preset->soundPreset;
    ThemePresetSource source = preset->source;
    // A loose video file sitting directly in themes/ is registered with the
    // themes directory itself as installPath. Removing that recursively would
    // wipe every other installed theme, so such a preset owns only its file.
    // Decided by discovery, never by the id: a manifest can name its own id.
    const bool looseVideo = preset->looseVideo;
    const std::string looseVideoFile = looseVideo ? preset->background.videoPath : std::string();
    if (looseVideo) installPath.clear();
    bool deletingActive = (activeId == idToDelete);

    m_allPresets.erase(
        std::remove_if(m_allPresets.begin(), m_allPresets.end(),
            [&](const ThemePreset& p) { return p.id == idToDelete || p.name == nameToDelete; }),
        m_allPresets.end());

    auto userPresets = ThemePreset::loadUserPresets();
    userPresets.erase(
        std::remove_if(userPresets.begin(), userPresets.end(),
            [&](const ThemePreset& p) { return p.id == idToDelete || p.name == nameToDelete; }),
        userPresets.end());
    ThemePreset::saveUserPresets(userPresets);

    if (source == ThemePresetSource::InstalledPackage && !looseVideoFile.empty()) {
        if (std::remove(looseVideoFile.c_str()) == 0) {
            DebugLog::log("[theme] removed loose video %s", looseVideoFile.c_str());
            switchu::commitSdCard("theme removed");
        } else {
            DebugLog::log("[theme] could not remove loose video %s", looseVideoFile.c_str());
        }
    } else if (source == ThemePresetSource::InstalledPackage && !installPath.empty()) {
        // The result is checked now. It was discarded before, which is why a
        // failure to delete looked exactly like a success from the player's
        // side: the theme left the list either way.
        std::string failedPath;
        if (removeDirectoryRecursive(installPath, &failedPath)) {
            DebugLog::log("[theme] removed %s", installPath.c_str());
            // The deletion has to reach the card, not just the cache: a reboot
            // before that and the folder is back with the theme gone from the
            // list, which is the same symptom by another route.
            switchu::commitSdCard("theme removed");
        } else {
            DebugLog::log("[theme] could not remove %s (stopped at %s)",
                          installPath.c_str(), failedPath.c_str());
        }
    }

    if (!soundPreset.empty() && m_config.soundPreset == soundPreset) {
        m_config.soundPreset = "wiiu";
        changeSoundPreset(m_config.soundPreset);
    }

    if (deletingActive) {
        m_activePresetName = defaultThemeRef();
        m_config.themePreset = defaultThemeRef();

        ThemePreset* fallback = findPresetPtr(defaultThemeRef());
        if (!fallback)
            fallback = findPresetPtr("Default Light");
        if (fallback) {
            m_activePresetName = fallback->id.empty() ? fallback->name : fallback->id;
            m_config.themePreset = m_activePresetName;
            m_activeColors = fallback->colors;
            m_activeMode = fallback->mode;
        }
        rebuildThemeFromColors();
    }

    refreshThemeShopState();
    m_audio.playSfx(Sfx::ConfirmPositive);
}
