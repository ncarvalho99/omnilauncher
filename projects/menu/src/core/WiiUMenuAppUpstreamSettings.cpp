#include "WiiUMenuApp.hpp"

#include <switchu/title_footprint.hpp>
#include "themeshop/ThemePackageInstaller.hpp"
#include "themeshop/ThemeHttp.hpp"
#include "widgets/GlossyIcon.hpp"
#include "DebugLog.hpp"
#include "NsService.hpp"

#include <nxui/core/I18n.hpp>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <filesystem>
#include <fmt/format.h>
#include <fstream>
#include <nlohmann/json.hpp>
#include <system_error>
#include <utility>

namespace {

bool removeDirectoryRecursive(const std::string& path) {
    std::error_code ec;
    std::filesystem::remove_all(path, ec);
    return !ec;
}
bool pathExists(const std::string& path) {
    if (path.empty())
        return false;

    std::error_code ec;
    return std::filesystem::exists(path, ec);
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
    return "builtin:Default Light";
}

} // namespace

#if 0 // Local fork implementation retained.
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
#ifdef NXUI_BACKEND_DEKO3D
        app().renderer().reclaimReleasedTextureSlotsAfterIdle();
#endif
        m_settingsNeedRefresh = true;
        // Dead copy: WiiUMenuAppSettings.cpp holds the live handler.
        // Switching the catalogue only changes what tr() returns next time it is
        // asked. Anything that asked once and kept the answer stays in the old
        // language until it is built again, which is why a restart was needed:
        //  - A widget tile's label is widgetTypeLabel() captured into the model
        //    entry at build time, so the home model is composed again here.
        //  - The SwitchU screen, the update tab and its changelog among it,
        //    builds its strings when it is constructed. It is created on demand
        //    and cached, so dropping it means the next open builds it in the new
        //    language. Only when it is not on screen: the language lives in
        //    Settings, so it never is, but freeing a live overlay would take its
        //    textures with it.
        // The action hints and the title pill are rebuilt every frame already
        // and were never part of this.
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
    m_settings->onClockUse12HourChange([this](bool enabled) {
        if (m_config.clockUse12Hour == enabled)
            return;
        m_config.clockUse12Hour = enabled;
        if (m_clock)
            m_clock->setUse12HourClock(enabled);
    });
    m_settings->onAccessibilityEnabledChange([this](bool enabled) {
        if (m_config.accessibilityEnabled == enabled)
            return;
        m_config.accessibilityEnabled = enabled;
        if (m_settings)
            m_settings->setAccessibilityVoiceEnabled(enabled);
        if (m_themeShop)
            m_themeShop->setAccessibilityVoiceEnabled(enabled);
        if (m_gameOptions)
            m_gameOptions->setAccessibilityVoiceEnabled(enabled);
        if (m_folderOptions)
            m_folderOptions->setAccessibilityVoiceEnabled(enabled);
        if (enabled) {
            m_accessibility.setEnabled(true);
            // Startup skips espeak init when voice guidance is off, so bring it
            // up on first enable.
            if (!m_accessibilityReady)
                m_accessibilityReady = m_accessibility.initializeConfigured(
                    SD_ASSETS, nxui::I18n::instance().activeLanguageTag(),
                    m_config.accessibilitySpeechRate);
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
        if (m_gameOptions)
            m_gameOptions->setAccessibilitySpeechPreferences(m_config.accessibilitySpeakHints,
                                                              m_config.accessibilitySpeakPosition);
        if (m_folderOptions)
            m_folderOptions->setAccessibilitySpeechPreferences(m_config.accessibilitySpeakHints,
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
        if (m_gameOptions)
            m_gameOptions->setAccessibilitySpeechPreferences(m_config.accessibilitySpeakHints,
                                                              m_config.accessibilitySpeakPosition);
        if (m_folderOptions)
            m_folderOptions->setAccessibilitySpeechPreferences(m_config.accessibilitySpeakHints,
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
            OverlayDialog::DateTimeValue dialogValue;
            dialogValue.year = initial.year;
            dialogValue.month = initial.month;
            dialogValue.day = initial.day;
            dialogValue.hour = initial.hour;
            dialogValue.minute = initial.minute;
            m_dialog->showDateTimeEditor(
                dialogValue,
                [this, onCommit = std::move(onCommit)](
                    const OverlayDialog::DateTimeValue& value) mutable {
                    TabbedOverlayScreen::DateTimeEditorValue committed;
                    committed.year = value.year;
                    committed.month = value.month;
                    committed.day = value.day;
                    committed.hour = value.hour;
                    committed.minute = value.minute;
                    const bool saved = !onCommit || onCommit(committed);
                    if (saved)
                        m_clockService.invalidate();
                    return saved;
                });
            focusManager().setFocus(m_dialog.get());
        });
    m_settings->onClosed([this]() {
        m_navigator.routeDidClose(switchu::navigation::Route::Settings);
        if (m_configSaveFuture.valid())
            m_configSaveFuture.wait();
        m_configSaveFuture = m_threadPool.submit([cfg = m_config]() {
            cfg.save();
        });
        DebugLog::log("[config] save queued");
        if (isCurrentFocusableWidget(m_sidebar.settingsButton())) {
            m_suppressNextNavigateSfx = true;
            focusManager().setFocus(m_sidebar.settingsButton());
        }
    });

}
#endif

void WiiUMenuApp::editSteamGridDbApiKey() {
    auto& i18n = nxui::I18n::instance();
    requestTextEntry(i18n.tr("settings.steamgriddb.api_key", "API key"),
                     // Not masked. The key is stored in plain text in
                     // config.json on the card, so hiding it while typing
                     // protects nothing and left no way to check a mistyped
                     // key against a failed download.
                     "SteamGridDB", m_config.steamGridDbApiKey, 192, false,
                     [this](const std::string& typed) {
        m_config.steamGridDbApiKey = typed;
        m_config.save();
        if (m_settings) {
            m_settings->setSteamGridDbState(m_config.steamGridDbEnabled,
                                            !m_config.steamGridDbApiKey.empty());
            m_settings->refreshCurrentTabWidgets();
            m_settings->requestToast(m_config.steamGridDbApiKey.empty()
                ? nxui::I18n::instance().tr("settings.steamgriddb.key_cleared", "API key cleared.")
                : nxui::I18n::instance().tr("settings.steamgriddb.key_saved", "API key saved."));
            focusManager().setFocus(m_settings.get());
        }
    });
}

void WiiUMenuApp::startSteamGridDbScrape() {
    // No key is required: an empty key routes the manager through this fork's
    // gallery service. A key only adds the logo source the proxy does not have.
    if (m_steamGridDbBrowseFuture.valid() || m_steamGridDbApplyFuture.valid()) {
        if (m_settings)
            m_settings->requestToast(nxui::I18n::instance().tr(
                "settings.steamgriddb.already_running", "A SteamGridDB operation is already running."));
        return;
    }
    if (!m_steamGridDb.start(m_config.steamGridDbApiKey, m_allApps)) {
        if (m_settings)
            m_settings->requestToast(nxui::I18n::instance().tr(
                "settings.steamgriddb.start_failed", "The artwork scan could not be started."));
        return;
    }
    m_steamGridDbWasRunning = true;
    if (m_progressDialog) {
        m_progressDialog->setTheme(&m_theme);
        m_progressDialog->show(nxui::I18n::instance().tr(
            "settings.steamgriddb.download_title", "Downloading artwork"),
            nxui::I18n::instance().tr(
                "settings.steamgriddb.download_start", "Searching SteamGridDB..."), 0.f);
        focusManager().setFocus(m_progressDialog.get());
    }
    if (m_settings)
        m_settings->requestToast(nxui::I18n::instance().tr(
            "settings.steamgriddb.started", "SteamGridDB scan started."));
}

void WiiUMenuApp::openSteamGridDbPicker(GameOptionsScreen::ArtworkKind kind,
                                        const std::string& requestedQuery) {
    if (!m_gameOptions || m_gameOptionsTitleId == 0) return;
    if (kind == GameOptionsScreen::ArtworkKind::Logo
        && m_config.steamGridDbApiKey.empty()) {
        // Heroes and icons come from this fork's gallery service without a key;
        // logos have no source there and still need the player's own key.
        m_gameOptions->requestToast(nxui::I18n::instance().tr(
            "settings.steamgriddb.need_key_logo",
            "Logos need a SteamGridDB API key. Configure one in Settings."));
        return;
    }
    SteamGridDbManager::ArtworkKind managerKind = SteamGridDbManager::ArtworkKind::Hero;
    if (kind == GameOptionsScreen::ArtworkKind::Logo)
        managerKind = SteamGridDbManager::ArtworkKind::Logo;
    else if (kind == GameOptionsScreen::ArtworkKind::Icon)
        managerKind = SteamGridDbManager::ArtworkKind::Icon;

    std::string title;
    std::string defaultQuery;
    for (const auto& appEntry : m_allApps) {
        if (appEntry.titleId == m_gameOptionsTitleId) {
            title = appEntry.title;
            defaultQuery = appEntry.steamGridDbTitle();
            break;
        }
    }
    if (title.empty() || m_steamGridDbBrowseFuture.valid()
        || m_steamGridDbApplyFuture.valid() || m_steamGridDb.running()) {
        m_gameOptions->requestToast(nxui::I18n::instance().tr(
            "settings.steamgriddb.already_running", "A SteamGridDB operation is already running."));
        return;
    }
    const std::string query = requestedQuery.empty() ? defaultQuery : requestedQuery;
    m_steamGridDbPicker->showLoading(m_gameOptionsTitleId, title, query, managerKind);
    focusManager().setFocus(m_steamGridDbPicker.get());
    m_steamGridDbBrowseFuture = std::async(std::launch::async,
        [apiKey = m_config.steamGridDbApiKey, titleId = m_gameOptionsTitleId,
         title, query, managerKind]() {
            return SteamGridDbManager::browse(apiKey, titleId, title, query, managerKind);
        });
}

void WiiUMenuApp::openImagePinSteamGridDbPicker(int targetSlot, const nxui::Rect& anchor,
                                                switchu::widgets::WidgetSize size,
                                                const std::string& requestedQuery) {
    if (m_steamGridDbBrowseFuture.valid() || m_steamGridDbApplyFuture.valid() || m_steamGridDb.running())
        return;
    if (requestedQuery.empty()) return;
    m_imagePinTargetSlot = targetSlot;
    m_imagePinSize = size;
    m_imagePinAnchor = anchor;
    const std::string query = requestedQuery;
    const std::uint64_t pickerId = switchu::widgets::widgetTitleId(
        static_cast<std::uint32_t>(targetSlot + 1));
    m_steamGridDbPicker->showLoading(pickerId, query, query,
                                     SteamGridDbManager::ArtworkKind::Hero);
    focusManager().setFocus(m_steamGridDbPicker.get());
    m_steamGridDbBrowseFuture = std::async(std::launch::async,
        [apiKey = m_config.steamGridDbApiKey, pickerId, query]() {
            return SteamGridDbManager::browse(apiKey, pickerId, query, query,
                                              SteamGridDbManager::ArtworkKind::Hero);
        });
}

void WiiUMenuApp::editSteamGridDbPickerQuery() {
    if (!m_steamGridDbPicker || !m_steamGridDbPicker->isActive()) return;
    auto& i18n = nxui::I18n::instance();
    const auto kind = m_steamGridDbPicker->artworkKind();
    requestTextEntry(i18n.tr("settings.steamgriddb.scan", "Search artwork"),
                     "SteamGridDB", m_steamGridDbPicker->query(), 128, false,
                     [this, kind](const std::string& typed) {
        if (typed.empty()) return;
        if (m_imagePinTargetSlot >= 0) {
            openImagePinSteamGridDbPicker(m_imagePinTargetSlot, m_imagePinAnchor,
                                          m_imagePinSize, typed);
            return;
        }
        const auto mappedKind = kind == SteamGridDbManager::ArtworkKind::Logo
            ? GameOptionsScreen::ArtworkKind::Logo
            : kind == SteamGridDbManager::ArtworkKind::Icon
                ? GameOptionsScreen::ArtworkKind::Icon : GameOptionsScreen::ArtworkKind::Hero;
        openSteamGridDbPicker(mappedKind, typed);
    });
}

void WiiUMenuApp::applyImagePinSteamGridDbCandidate(
    const SteamGridDbManager::BrowseResult&,
    const SteamGridDbManager::Candidate& candidate) {
    if (m_steamGridDbApplyFuture.valid() || candidate.url.empty()) return;
    auto progress = std::make_shared<SteamGridDbApplyProgressShared>();
    progress->message = "Downloading pin image...";
    m_steamGridDbApplyProgress = progress;
    m_steamGridDbApplyProgressUiRevision = 0;
    if (m_progressDialog) {
        m_progressDialog->setTheme(&m_theme);
        m_progressDialog->show("Downloading image pin", progress->message, 0.f);
        focusManager().setFocus(m_progressDialog.get());
    }
    m_imagePinApplyPending = true;
    const int targetSlot = m_imagePinTargetSlot;
    const auto size = m_imagePinSize;
    m_steamGridDbApplyFuture = std::async(std::launch::async, [candidate, progress, targetSlot, size]() {
        SteamGridDbManager::ApplyResult result;
        result.titleId = static_cast<std::uint64_t>(targetSlot);
        try {
            std::error_code ec;
            std::filesystem::create_directories(switchu::widgets::WidgetStore::kAssetRoot, ec);
            const std::string filename = "steamgriddb_pin_" + std::to_string(candidate.id) + ".png";
            const std::string fullPath = std::string(switchu::widgets::WidgetStore::kAssetRoot) + "/" + filename;
            auto bytes = themeshop::http::getBytes(candidate.url);
            if (bytes.empty()) throw std::runtime_error("Empty image data");
            std::ofstream out(fullPath, std::ios::binary | std::ios::trunc);
            out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
            if (!out.good()) throw std::runtime_error("Could not save image file");
            result.success = true;
            result.message = filename;
        } catch (const std::exception& ex) {
            result.message = ex.what();
        }
        return result;
    });
}

void WiiUMenuApp::applySteamGridDbCandidate(
    const SteamGridDbManager::BrowseResult& browse,
    const SteamGridDbManager::Candidate& candidate) {
    if (m_steamGridDbApplyFuture.valid() || m_steamGridDb.running()) return;
    auto progress = std::make_shared<SteamGridDbApplyProgressShared>();
    progress->message = nxui::I18n::instance().tr(
        "settings.steamgriddb.download_start", "Starting download...");
    m_steamGridDbApplyProgress = progress;
    m_steamGridDbApplyProgressUiRevision = 0;
    if (m_progressDialog) {
        m_progressDialog->setTheme(&m_theme);
        m_progressDialog->show(nxui::I18n::instance().tr(
            "settings.steamgriddb.download_title", "Downloading artwork"),
            progress->message, 0.f);
        focusManager().setFocus(m_progressDialog.get());
    }
    m_steamGridDbApplyFuture = std::async(std::launch::async, [browse, candidate, progress]() {
        return SteamGridDbManager::applyCandidate(browse, candidate,
            [progress](const std::string& message, float value) {
                std::lock_guard<std::mutex> lock(progress->mutex);
                progress->message = message;
                progress->progress01 = value;
                ++progress->revision;
            });
    });
}

void WiiUMenuApp::syncSteamGridDb() {
    if (m_steamGridDbBrowseFuture.valid()
        && m_steamGridDbBrowseFuture.wait_for(std::chrono::seconds(0))
            == std::future_status::ready) {
        auto result = m_steamGridDbBrowseFuture.get();
        if (m_steamGridDbPicker && m_steamGridDbPicker->isActive()
            && m_steamGridDbPicker->titleId() == result.titleId) {
            m_steamGridDbPicker->setResult(std::move(result));
            focusManager().setFocus(m_steamGridDbPicker.get());
        }
    }
    if (m_steamGridDbApplyProgress) {
        std::string message;
        float progress01 = 0.f;
        std::uint64_t revision = 0;
        {
            std::lock_guard<std::mutex> lock(m_steamGridDbApplyProgress->mutex);
            message = m_steamGridDbApplyProgress->message;
            progress01 = m_steamGridDbApplyProgress->progress01;
            revision = m_steamGridDbApplyProgress->revision;
        }
        if (revision != m_steamGridDbApplyProgressUiRevision && m_progressDialog) {
            m_steamGridDbApplyProgressUiRevision = revision;
            m_progressDialog->updateState(message, progress01);
        }
        if (m_progressDialog && m_progressDialog->isActive()
            && focusManager().current() != m_progressDialog.get())
            focusManager().setFocus(m_progressDialog.get());
    }
    if (m_steamGridDbApplyFuture.valid()
        && m_steamGridDbApplyFuture.wait_for(std::chrono::seconds(0))
            == std::future_status::ready) {
        const auto result = m_steamGridDbApplyFuture.get();
        if (m_imagePinTargetSlot >= 0) {
            const int targetSlot = m_imagePinTargetSlot;
            const auto size = m_imagePinSize;
            m_imagePinTargetSlot = -1;
            m_imagePinApplyPending = false;
            if (m_progressDialog) m_progressDialog->hide();
            if (m_steamGridDbPicker && m_steamGridDbPicker->isActive())
                m_steamGridDbPicker->hide();
            m_steamGridDbApplyProgress.reset();
            m_steamGridDbApplyProgressUiRevision = 0;
            if (result.success) {
                createWidget(targetSlot, switchu::widgets::WidgetType::ImagePin,
                             size, "widget:" + result.message);
            }
            return;
        }
        if (result.success) {
            // Wide tiles and recent widgets can still be referenced by frames
            // already submitted to deko3d. Drain them before destroying their
            // old textures after a manual artwork replacement.
            DebugLog::log("[steamgriddb-ui] draining GPU before replacing artwork title=%016lX",
                          static_cast<unsigned long>(result.titleId));
            app().gpu().waitIdle();
#ifdef NXUI_BACKEND_DEKO3D
            app().renderer().reclaimReleasedTextureSlotsAfterIdle();
#endif
            m_gameArtwork.erase(result.titleId);
            if (m_recentWidgetAssetTitleId == result.titleId) {
                m_recentWidgetAssetTitleId = 0;
                m_recentWidgetHero.reset();
                m_recentWidgetLogo.reset();
                m_recentWidgetIcon.reset();
            }
            if (result.kind == SteamGridDbManager::ArtworkKind::Icon && m_grid) {
                m_iconStreamer.reloadTitle(result.titleId, m_grid->currentPage(),
                                           m_grid->iconsPerPage(), app().gpu(),
                                           app().renderer(), m_grid->allIcons());
            } else {
                showFocusedSteamGridDbArtwork(true);
            }
            if (m_grid && m_openFolderId == 0 &&
                gameGridSize(result.titleId, AppLayoutMode::Grid) !=
                    switchu::widgets::WidgetSize{1, 1})
                applyDisplayModel(buildRootFolderModel(), result.titleId, false);
        }
        if (m_steamGridDbPicker && m_steamGridDbPicker->isActive())
            m_steamGridDbPicker->setMessage(result.message, false);
        if (m_progressDialog) m_progressDialog->hide();
        m_steamGridDbApplyProgress.reset();
        m_steamGridDbApplyProgressUiRevision = 0;
        if (m_steamGridDbPicker && m_steamGridDbPicker->isActive())
            focusManager().setFocus(m_steamGridDbPicker.get());
    }

    const auto state = m_steamGridDb.status();
    if (state.revision != m_steamGridDbUiRevision) {
        m_steamGridDbUiRevision = state.revision;
        if (m_settings) {
            m_settings->setSteamGridDbProgress(
                state.running, state.finished, state.completed, state.total,
                state.matched, state.failed, state.currentTitle, state.message);
        }
        if (state.running && m_steamGridDbWasRunning && m_progressDialog) {
            m_progressDialog->updateState(
                state.message.empty() ? state.currentTitle : state.message,
                state.progress01);
            if (focusManager().current() != m_progressDialog.get())
                focusManager().setFocus(m_progressDialog.get());
        }
        if (state.lastCompletedTitleId != 0
            && state.lastCompletedTitleId != m_steamGridDbLastCompletedTitleId) {
            m_steamGridDbLastCompletedTitleId = state.lastCompletedTitleId;
            if (m_grid && m_grid->focusManager().current()) {
                auto* focused = m_grid->focusManager().current();
                if (focused->tag() == "glossy_icon"
                    && static_cast<GlossyIcon*>(focused)->titleId() == state.lastCompletedTitleId) {
                    showFocusedSteamGridDbArtwork(true);
                }
            }
        }
    }

    if (m_steamGridDbWasRunning && !state.running && state.finished) {
        m_steamGridDbWasRunning = false;
        if (m_progressDialog) m_progressDialog->hide();
        if (m_steamGridDbBackdrop)
            m_steamGridDbBackdrop->invalidateArtworkCaches();
        showFocusedSteamGridDbArtwork(true);
        // Any completed scan writes new files under steamgriddb/, and nothing in
        // the running menu re-reads them: the streamer keeps the textures it has
        // already uploaded, and ensureRecentWidgetAssets() refuses to look again
        // at a title it has tried once. This was gated on the per-title icon
        // picker, so the bulk scan from Settings finishes with selectedKind None
        // and left every tile showing what it had before. Both caches are
        // invalidated here so the artwork appears without restarting the menu.
        if (state.matched > 0) {
            m_recentWidgetAssetTitleId = 0;
            if (m_grid) {
                // forceReload releases the pool texture used by the panel header;
                // detach it first so the overlay never renders a stale pointer.
                if (m_gameOptions) m_gameOptions->setGameIcon(nullptr);
                const bool line = isCarouselLayout();
                const int page = line ? std::max(0, m_grid->focusedGlobalIndex())
                                      : m_grid->currentPage();
                const int perPage = line ? 1 : m_grid->iconsPerPage();
                m_iconStreamer.forceReload(page, perPage,
                                           app().gpu(), app().renderer(),
                                           m_grid->allIcons());
            }
        }
        if (state.selectedKind != SteamGridDbManager::ArtworkKind::None
            && m_gameOptions && m_gameOptions->isActive()) {
            m_gameOptions->requestToast(state.message, 3.2f);
        }
        if (m_settings && m_settings->isActive()) {
            // A reason, when there is one, beats a count: "9 missing" was all
            // the run said when the console was offline.
            m_settings->requestToast(!state.message.empty()
                ? state.message
                : std::to_string(state.matched) + " artwork sets found, "
                  + std::to_string(state.failed) + " missing.", 3.2f);
        }
    }
}

void WiiUMenuApp::showFocusedSteamGridDbArtwork(bool forceReload) {
    if (!m_steamGridDbBackdrop || !m_config.steamGridDbEnabled) return;
    std::uint64_t titleId = 0;
    std::vector<std::uint64_t> nearbyTitleIds;
    if (m_grid) {
        auto* current = m_grid->focusManager().current();
        if (current && current->tag() == "glossy_icon") {
            const int focused = m_grid->focusedGlobalIndex();
            if (focused >= 0 && focused < m_model.count()
                && m_model.at(focused).isApplication())
                titleId = static_cast<GlossyIcon*>(current)->titleId();
        }
        const int focused = m_grid->focusedGlobalIndex();
        for (int distance = 1; focused >= 0 && nearbyTitleIds.size() < 8
                               && distance < m_model.count(); ++distance) {
            for (int direction : {1, -1}) {
                const int index = focused + direction * distance;
                if (index < 0 || index >= m_model.count()) continue;
                const auto& entry = m_model.at(index);
                if (!entry.isApplication() || entry.titleId == 0) continue;
                nearbyTitleIds.push_back(entry.titleId);
                if (nearbyTitleIds.size() >= 8) break;
            }
        }
    }
    // Zero is meaningful: it clears artwork when focus moves to a folder,
    // an empty slot or any non-application widget.
    m_steamGridDbBackdrop->showTitle(titleId, forceReload);
    m_steamGridDbBackdrop->setPreloadTitles(std::move(nearbyTitleIds));
}

void WiiUMenuApp::createGameOptions() {
    if (m_gameOptions) return;
    m_gameOptions = std::make_shared<GameOptionsScreen>();
    if (m_overlayLayer) m_overlayLayer->addChild(m_gameOptions);
    m_gameOptions->setFont(&m_fontNormal);
    m_gameOptions->setSmallFont(&m_fontSmall);
    m_gameOptions->setTheme(&m_theme);
    m_gameOptions->setAccessibilityVoiceEnabled(m_config.accessibilityEnabled);
    m_gameOptions->setAccessibilitySpeechPreferences(m_config.accessibilitySpeakHints,
                                                     m_config.accessibilitySpeakPosition);
    m_gameOptions->onNavigateSfx([this]() { m_audio.playSfx(Sfx::Navigate); });
    m_gameOptions->onActivateSfx([this]() { m_audio.playSfx(Sfx::Activate); });
    m_gameOptions->onCloseSfx([this]() { m_audio.playSfx(Sfx::ModalHide); });
    m_gameOptions->onAccessibilityAnnouncement([this](const std::string& text) {
        m_accessibility.announce(text);
    });
    m_gameOptions->onAccessibilityStructuredAnnouncement(
        [this](const std::string& context, const std::string& position,
               const std::string& summary, bool forceRepeat, bool forceContext) {
            m_accessibility.announceStructuredFocus(context, position, summary, forceRepeat, forceContext);
        });
    m_gameOptions->onClosed([this]() {
        m_navigator.routeDidClose(switchu::navigation::Route::GameOptions);
        if (focusTitle(m_gameOptionsTitleId)) {
            if (auto* focused = m_grid->focusManager().current())
                focusManager().setFocus(focused);
        }
    });
}

void WiiUMenuApp::createFolderOptions() {
    if (m_folderOptions) return;
    m_folderOptions = std::make_shared<FolderOptionsScreen>();
    if (m_overlayLayer) m_overlayLayer->addChild(m_folderOptions);
    m_folderOptions->setFont(&m_fontNormal);
    m_folderOptions->setSmallFont(&m_fontSmall);
    m_folderOptions->setTheme(&m_theme);
    m_folderOptions->setAccessibilityVoiceEnabled(m_config.accessibilityEnabled);
    m_folderOptions->setAccessibilitySpeechPreferences(m_config.accessibilitySpeakHints,
                                                       m_config.accessibilitySpeakPosition);
    m_folderOptions->onNavigateSfx([this]() { m_audio.playSfx(Sfx::Navigate); });
    m_folderOptions->onActivateSfx([this]() { m_audio.playSfx(Sfx::Activate); });
    m_folderOptions->onCloseSfx([this]() { m_audio.playSfx(Sfx::ModalHide); });
    m_folderOptions->onAccessibilityAnnouncement([this](const std::string& text) {
        m_accessibility.announce(text);
    });
    m_folderOptions->onAccessibilityStructuredAnnouncement(
        [this](const std::string& context, const std::string& position,
               const std::string& summary, bool forceRepeat, bool forceContext) {
            m_accessibility.announceStructuredFocus(context, position, summary,
                                                    forceRepeat, forceContext);
        });
    m_folderOptions->onClosed([this]() {
        m_navigator.routeDidClose(switchu::navigation::Route::FolderOptions);
        if (focusTitle(folderTitleId(m_folderOptionsId))) {
            if (auto* focused = m_grid->focusManager().current())
                focusManager().setFocus(focused);
        }
    });
}

void WiiUMenuApp::createControllerTest() {
    if (m_controllerTest) return;
    m_controllerTest = std::make_shared<ControllerTestScreen>();
    if (m_overlayLayer) m_overlayLayer->addChild(m_controllerTest);
    m_controllerTest->setFont(&m_fontNormal);
    m_controllerTest->setSmallFont(&m_fontSmall);
    m_controllerTest->setTheme(&m_theme);
    m_controllerTest->setInput(&app().input());
    m_controllerTest->onCloseSfx([this]() { m_audio.playSfx(Sfx::ModalHide); });
    m_controllerTest->onAccessibilityAnnouncement([this](const std::string& text) {
        m_accessibility.announce(text);
    });
    m_controllerTest->onClosed([this]() {
        if (m_navigator.route() == switchu::navigation::Route::ControllerTest) {
            if (m_settings && m_settings->isActive())
                m_navigator.navigate(switchu::navigation::Route::Settings);
            else
                m_navigator.resetToHome();
        }
        if (m_settings && m_settings->isActive())
            focusManager().setFocus(m_settings.get());
    });
}

#if 0 // Local fork implementation retained.
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
            showThemeShopInfo(i18n.tr("sidebar.theme_shop", "Theme Shop"),
                              i18n.tr("themeshop.community.selected_missing",
                                      "The selected community theme is no longer available in the catalog."));
            return;
        }

        ThemeCatalogClient::Entry entryCopy = *entry;
        ThemePackageInstaller::Mode mode = applyAfterInstall
            ? ThemePackageInstaller::Mode::InstallAndApply
            : ThemePackageInstaller::Mode::InstallOnly;
        std::string destination = ThemePackageInstaller::destinationRootFor(entryCopy.id, mode);

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
    m_themeShop->setMusicState(m_audio.isPlaying(), m_audio.volume(), m_audio.sfxVolume());
    m_themeShop->setGridLayoutState(m_config.gridColumns, m_config.gridRows);
    m_themeShop->setDynamicPagesState(m_config.dynamicPages);
    m_themeShop->setActionHintStyleState(m_config.actionHintStyle);
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
    m_themeShop->onActionHintStyleChange([this](int style) {
        m_config.actionHintStyle = style == 0 ? "panel" : "capsules";
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
    m_themeShop->onNetConnectRequest([this]() {
        m_pendingNetConnect = true;
    });
    m_themeShop->onThemeShopApply([this](const std::string& presetId) {
        DebugLog::log("[theme-apply] request from Theme Shop: preset=%s", presetId.c_str());
        ThemePreset* preset = findPresetPtr(presetId);
        if (!preset) {
            DebugLog::log("[theme-apply] preset not found: %s", presetId.c_str());
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
    m_themeShop->onClosed([this, clearCompletedThemeTransferState]() {
        m_navigator.routeDidClose(switchu::navigation::Route::ThemeShop);
        if (m_configSaveFuture.valid())
            m_configSaveFuture.wait();
        m_configSaveFuture = m_threadPool.submit([cfg = m_config]() {
            cfg.save();
        });
        DebugLog::log("[config] save queued");
        clearCompletedThemeTransferState();
        if (isCurrentFocusableWidget(m_sidebar.themeShopButton())) {
            m_suppressNextNavigateSfx = true;
            focusManager().setFocus(m_sidebar.themeShopButton());
        }
    });

}
#endif

#if 0 // Local fork implementation retained.
void WiiUMenuApp::reloadThemePresets() {
    m_allPresets = ThemePreset::builtInPresets();
    auto userPresets = ThemePreset::loadUserPresets();
    auto installedPackages = ThemePreset::loadInstalledPackages();
    m_allPresets.insert(m_allPresets.end(), userPresets.begin(), userPresets.end());
    m_allPresets.insert(m_allPresets.end(), installedPackages.begin(), installedPackages.end());
}
#endif

#if 0 // Local fork implementation retained.
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
        m_progressDialog->show(i18n.tr("themeshop.transfer.dialog_title", "Downloading Theme"),
                               preparingLabel,
                               -1.f);
        focusManager().setFocus(m_progressDialog.get());
    }

    const std::string catalogUrl = m_themeShop
        ? m_themeShop->communityCatalogUrl()
        : ThemeCatalogClient::kDefaultCatalogUrl;
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
#endif

#if 0 // Local fork implementation retained.
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
            if (preset) {
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
#endif

#if 0 // Local fork implementation retained.
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
        entry.active = (entry.id == activeId);
        entry.removable = (preset.source != ThemePresetSource::BuiltIn);
        entries.push_back(std::move(entry));
    }

    return entries;
}
#endif

#if 0 // Local fork implementation retained.
void WiiUMenuApp::refreshThemeShopState() {
    if (!m_themeShop) return;

    ThemePreset* activePreset = findPresetPtr(m_activePresetName);
    std::string activeId = activePreset ? (activePreset->id.empty() ? activePreset->name : activePreset->id)
                                        : m_activePresetName;

    m_themeShop->setTheme(&m_theme);
    m_themeShop->setMusicState(m_audio.isPlaying(), m_audio.volume(), m_audio.sfxVolume());
    m_themeShop->setThemeShopState(buildThemeShopEntries(), activeId);
    m_themeShop->rebuildCurrentTab();
}
#endif

#if 0 // Local fork implementation retained.
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
#endif

#if 0 // Local fork implementation retained.
void WiiUMenuApp::applyUiLanguage() {
    auto& i18n = nxui::I18n::instance();
    if (m_config.uiLanguageOverride == "auto" || m_config.uiLanguageOverride.empty())
        i18n.setLanguageAuto();
    else
        i18n.setLanguage(m_config.uiLanguageOverride);
    m_accessibility.setVoiceForLanguageTag(i18n.activeLanguageTag());
}
#endif

#if 0 // Local fork implementation retained.
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
#endif

#if 0 // Local fork implementation retained.
ThemePreset WiiUMenuApp::buildEffectiveThemePreset() {
    ThemePreset effective;
    if (ThemePreset* preset = findPresetPtr(m_activePresetName))
        effective = *preset;

    effective.mode = m_activeMode;
    effective.colors = m_activeColors;
    return effective;
}
#endif

#if 0 // Local fork implementation retained.
void WiiUMenuApp::applyThemeResources(const ThemePreset& preset) {
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

    // This application supersedes any retry left by the previous theme.
    m_pendingBackgroundImagePath.clear();
    m_backgroundImageRetryFrames = 0;
    m_backgroundImageRetryAttempts = 0;

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

    const bool imageExists = !backgroundImagePath.empty() && pathExists(backgroundImagePath);
    const bool wantsBackgroundImage = imageExists;
    const bool backgroundImageNeedsReload = forceResourceReload
        || m_loadedBackgroundImagePath != backgroundImagePath
        || m_backgroundImageLoaded != wantsBackgroundImage;

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

        if (backgroundImageNeedsReload) {
            const bool imageLoaded = imageExists && m_background->loadImage(gpu, ren, backgroundImagePath);
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
                if (!backgroundImagePath.empty()) {
                    m_pendingBackgroundImagePath = backgroundImagePath;
                    m_backgroundImageRetryFrames = 2;
                    DebugLog::log("[theme-apply] background image retry scheduled: path=%s",
                                  safeLogPath(backgroundImagePath));
                }
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

    if (needsGpuResourceReload)
        ren.reclaimReleasedTextureSlotsAfterIdle();

    DebugLog::log("[theme-apply] resources done: preset=%s", presetRef.c_str());
}
#endif

void WiiUMenuApp::retryPendingBackgroundImage() {
    if (m_pendingBackgroundImagePath.empty() || !m_background)
        return;
    if (m_backgroundImageRetryFrames > 0) {
        --m_backgroundImageRetryFrames;
        return;
    }

    const std::string expectedPath = resolveThemeAssetPath(
        m_effectivePreset, m_effectivePreset.background.imagePath);
    if (expectedPath != m_pendingBackgroundImagePath) {
        DebugLog::log("[theme-apply] background retry cancelled after theme changed");
        m_pendingBackgroundImagePath.clear();
        return;
    }

    ++m_backgroundImageRetryAttempts;
    const bool exists = pathExists(m_pendingBackgroundImagePath);
    bool loaded = false;
    if (exists) {
        app().gpu().waitIdle();
        loaded = m_background->loadImage(app().gpu(), app().renderer(),
                                         m_pendingBackgroundImagePath);
    }
    DebugLog::log("[theme-apply] background retry %d/3: path=%s exists=%d loaded=%d",
                  m_backgroundImageRetryAttempts,
                  safeLogPath(m_pendingBackgroundImagePath),
                  exists ? 1 : 0, loaded ? 1 : 0);

    if (loaded) {
        m_backgroundImageLoaded = true;
        m_loadedBackgroundImagePath = m_pendingBackgroundImagePath;
        m_pendingBackgroundImagePath.clear();
        m_backgroundImageRetryAttempts = 0;
        return;
    }

    if (m_backgroundImageRetryAttempts >= 3) {
        DebugLog::log("[theme-apply] background retry exhausted: path=%s",
                      safeLogPath(m_pendingBackgroundImagePath));
        m_pendingBackgroundImagePath.clear();
        return;
    }
    m_backgroundImageRetryFrames = 2 << (m_backgroundImageRetryAttempts - 1);
}

#if 0 // Local fork implementation retained.
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
        icon->setLoadingColor(m_theme.cursorNormal);
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
    for (auto& avatar : m_userAvatarButtons) {
        if (avatar)
            avatar->setTheme(&m_theme);
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

    m_titlePill->setBaseColor(m_theme.panelBase.withAlpha(
        m_theme.mode == nxui::ThemeMode::Dark ? 0.90f : 0.86f));
    m_titlePill->setBorderColor(m_theme.panelBorder.withAlpha(0.84f));
    m_titlePill->setHighlightColor(m_theme.panelHighlight.withAlpha(0.42f));
    m_titlePill->setTextColor(m_theme.textPrimary);

    m_pageIndicator->setBaseColor(m_theme.panelBase);
    m_pageIndicator->setBorderColor(m_theme.panelBorder);
    m_pageIndicator->setHighlightColor(m_theme.panelHighlight);
    m_pageIndicator->setTheme(&m_theme);
    if (m_folderHeader) {
        m_folderHeader->setBaseColor(m_theme.panelBase);
        m_folderHeader->setBorderColor(m_theme.panelBorder);
        m_folderHeader->setHighlightColor(m_theme.panelHighlight);
    }
    if (m_folderHeaderLabel)
        m_folderHeaderLabel->setTextColor(m_theme.textPrimary);
    for (auto& avatar : m_userAvatarButtons) {
        avatar->setBaseColor(m_theme.iconDefault.withAlpha(
            m_theme.mode == nxui::ThemeMode::Dark ? 0.92f : 0.94f));
        avatar->setBorderColor(m_theme.panelBorder);
        avatar->setHighlightColor(m_theme.panelHighlight);
        avatar->setCornerRadius(28.f);
        avatar->setChromeEnabled(true);
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
    if (m_contextMenu)
        m_contextMenu->setTheme(&m_theme);
    DebugLog::log("[theme-apply] widget recolor overlays done");

    if (m_settings)
        m_settings->setTheme(&m_theme);
    if (m_themeShop)
        m_themeShop->setTheme(&m_theme);
    if (m_gameOptions)
        m_gameOptions->setTheme(&m_theme);
    if (m_steamGridDbPicker)
        m_steamGridDbPicker->setTheme(&m_theme);
    if (m_folderOptions)
        m_folderOptions->setTheme(&m_theme);
    if (m_controllerTest)
        m_controllerTest->setTheme(&m_theme);

    m_sidebar.applyTheme(m_theme);
    DebugLog::log("[theme-apply] widget recolor complete");
}
#endif

#if 0 // Local fork implementation retained.
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
#endif

#if 0 // Local fork implementation retained.
ThemePreset* WiiUMenuApp::findPresetPtr(const std::string& name) {
    for (auto& p : m_allPresets)
        if (p.id == name || p.name == name) return &p;
    return nullptr;
}
#endif

#if 0 // Local fork implementation retained.
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

    if (source == ThemePresetSource::InstalledPackage && !installPath.empty()) {
        if (m_themeDeleteFuture.valid())
            m_themeDeleteFuture.wait();
        m_themeDeleteFuture = m_threadPool.submit([installPath]() {
            const bool removed = removeDirectoryRecursive(installPath);
            DebugLog::log("[theme-delete] async removal %s: %s",
                          removed ? "complete" : "failed", safeLogPath(installPath));
        });
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
#endif

void WiiUMenuApp::startSoftwareDeletion(uint64_t titleId, const std::string& title,
                                        bool closeGameOptionsOnSuccess) {
    if (m_softwareDeleteFuture.valid()
        && m_softwareDeleteFuture.wait_for(std::chrono::seconds(0)) != std::future_status::ready) {
        return;
    }

    auto& i18n = nxui::I18n::instance();
    m_softwareDeleteTitle = title;
    m_softwareDeleteTitleId = titleId;
    m_softwareDeleteClosesGameOptions = closeGameOptionsOnSuccess;
    m_dialogReturnFocus = closeGameOptionsOnSuccess
        ? static_cast<nxui::Widget*>(m_gameOptions.get())
        : static_cast<nxui::Widget*>(m_settings.get());
    // The confirmation is done with. It was left on screen and, because
    // showing it raised it to the end of the overlay tree, it was drawn over
    // the progress dialog while input went to the progress dialog instead:
    // the menu looked frozen on the confirmation for the whole delete, and
    // came back to that same dialog afterwards with nothing said.
    if (m_dialog && m_dialog->isActive())
        m_dialog->hide();
    if (m_progressDialog) {
        raiseOverlay(m_progressDialog);
        m_progressDialog->show(
            i18n.tr("game.uninstall_progress_title", "Deleting software"),
            i18n.tr("game.uninstall_progress", "Please wait. Do not turn off the console.")
                + std::string("\n") + title,
            -1.f);
        focusManager().setFocus(m_progressDialog.get());
    }

    DebugLog::log("[software-delete] queued title=%016llX",
                  static_cast<unsigned long long>(titleId));
    m_softwareDeleteResult = MAKERESULT(Module_Libnx, LibnxError_BadInput);
    m_softwareDeleteSdRemoved = 0;
    m_softwareDeleteSdFailure.clear();
    m_softwareDeleteDone.store(0);
    m_softwareDeleteTotal.store(0);
    m_softwareDeleteFuture = m_threadPool.submit([this, titleId]() {
        // Counted before anything is removed so the bar has a denominator. The
        // walk only reads directory entries, and it is what turns a long silent
        // wait into a bar that moves.
        m_softwareDeleteTotal.store(switchu::titles::countSdFootprint(titleId));
        const Result nsRc = switchu::menu::ensureNsService("software-delete");
        if (R_SUCCEEDED(nsRc)) {
            m_softwareDeleteResult = nsDeleteApplicationCompletely(titleId);
        } else {
            m_softwareDeleteResult = nsRc;
        }
        // ns owns only what it installed. Anything copied onto the card by hand
        // has to be removed here or it stays, which is how a deleted port kept
        // its whole romfs.
        const auto report =
            switchu::titles::removeSdFootprint(titleId, &m_softwareDeleteDone);
        m_softwareDeleteSdRemoved = report.removed;
        m_softwareDeleteSdFailure = report.firstFailure;
    });
}

void WiiUMenuApp::syncSoftwareDeletion() {
    if (!m_softwareDeleteFuture.valid())
        return;
    if (m_softwareDeleteFuture.wait_for(std::chrono::seconds(0)) != std::future_status::ready) {
        // Still running. Report how far the sweep has got; before the count
        // finishes there is no denominator, so the dialog keeps its spinner.
        if (m_progressDialog && m_progressDialog->isActive()) {
            auto& progressI18n = nxui::I18n::instance();
            const std::uint64_t total = m_softwareDeleteTotal.load();
            const std::uint64_t done = m_softwareDeleteDone.load();
            std::string message =
                progressI18n.tr("game.uninstall_progress",
                                "Please wait. Do not turn off the console.")
                + std::string("\n") + m_softwareDeleteTitle;
            if (total > 0) {
                message += fmt::format("\n{}/{}", done, total);
                m_progressDialog->updateState(
                    message,
                    std::min(1.f, static_cast<float>(done)
                                      / static_cast<float>(total)));
            } else {
                m_progressDialog->updateState(message, -1.f);
            }
        }
        return;
    }

    Result rc = MAKERESULT(Module_Libnx, LibnxError_BadInput);
    try {
        m_softwareDeleteFuture.get();
        rc = m_softwareDeleteResult;
    } catch (const std::exception& ex) {
        DebugLog::log("[software-delete] worker exception: %s", ex.what());
    } catch (...) {
        DebugLog::log("[software-delete] worker exception: unknown");
    }

    if (m_progressDialog)
        m_progressDialog->hide();

    auto& i18n = nxui::I18n::instance();
    // A port that was only ever copied to the card has nothing registered with
    // ns, so the ns call fails while the removal succeeds. Treating the ns
    // result as the whole answer is what let "deleted" be reported for a title
    // whose files were all still there.
    const bool removedFromCard = m_softwareDeleteSdRemoved > 0;
    const bool sweepFailed = !m_softwareDeleteSdFailure.empty();
    DebugLog::log("[software-delete] ns rc=0x%08X sd-removed=%d sd-failure=%s",
                  rc, m_softwareDeleteSdRemoved,
                  m_softwareDeleteSdFailure.empty()
                      ? "none" : m_softwareDeleteSdFailure.c_str());
    if (!sweepFailed && (R_SUCCEEDED(rc) || removedFromCard)) {
        DebugLog::log("[software-delete] complete: %s", m_softwareDeleteTitle.c_str());
        // A folder keeps title ids, not entries, so a deleted title stayed a
        // member of whatever folder it was filed in. The folder tile then drew a
        // preview cell for a title with no icon behind it, which is the flat
        // coloured square left behind after deleting a game from inside a folder.
        if (const std::uint32_t folderId =
                m_folderStore.folderForTitle(m_softwareDeleteTitleId);
            folderId != 0) {
            m_folderStore.removeTitle(folderId, m_softwareDeleteTitleId);
            m_folderStore.save();
        }
        std::replace(m_layoutSlots.begin(), m_layoutSlots.end(),
                     m_softwareDeleteTitleId, std::uint64_t{0});
        m_layoutDirty = true;
        if (m_config.hasCustomTitle(m_softwareDeleteTitleId)) {
            m_config.setCustomTitle(m_softwareDeleteTitleId, "");
            m_config.save();
        }
        if (m_gameDetails && m_gameDetails->isActive() &&
            m_gameDetails->titleId() == m_softwareDeleteTitleId) {
            m_gameDetails->hide();
        }
        m_audio.playSfx(Sfx::ConfirmPositive);
        if (m_softwareDeleteClosesGameOptions && m_gameOptions) {
            m_gameOptions->hide();
        } else if (m_settings && m_settings->isActive()) {
            m_settings->requestToast(
                i18n.tr("settings.storage.uninstall_success", "Uninstalled successfully."), 2.8f);
            m_settings->rebuildCurrentTab();
            focusManager().setFocus(m_settings.get());
        } else if (m_dialog) {
            // Deleting from the grid or the dossier leaves no overlay to return
            // to, and saying nothing is what made a finished delete look like a
            // hang. The queued refresh below takes the tile away.
            raiseOverlay(m_dialog);
            m_dialog->show(
                i18n.tr("dialog.delete_software", "Delete software?"),
                i18n.tr("dialog.delete_software_done", "The software was deleted."),
                {{i18n.tr("button.ok", "OK"), [this]() {}, true}},
                0, {});
            focusManager().setFocus(m_dialog.get());
        }
        m_refreshQueued = true;
        m_deferredRefreshFrames = std::max(m_deferredRefreshFrames, 3);
    } else {
        DebugLog::log("[software-delete] failed rc=0x%08X module=%u description=%u",
                      rc, R_MODULE(rc), R_DESCRIPTION(rc));
        const std::string details = sweepFailed
            ? i18n.tr("settings.storage.uninstall_failed_files",
                      "Some files could not be removed from the SD card.")
                  + std::string("\n") + m_softwareDeleteSdFailure
            : fmt::format(
                  "{}\nResult: 0x{:08X} (module {}, description {})",
                  i18n.tr("settings.storage.uninstall_failed",
                          "Failed to uninstall the selected title."),
                  rc, R_MODULE(rc), R_DESCRIPTION(rc));
        if (m_dialog) {
            m_dialog->show(
                i18n.tr("settings.storage.uninstall_failed_title", "Uninstall Failed"),
                details,
                {{i18n.tr("button.ok", "OK"), []() {}, true}});
            focusManager().setFocus(m_dialog.get());
        }
    }

    m_softwareDeleteTitle.clear();
    m_softwareDeleteClosesGameOptions = false;
}
