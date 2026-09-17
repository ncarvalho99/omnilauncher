#include "WiiUMenuApp.hpp"
#include "services/NtpClient.hpp"
#include <cctype>
#include <switchu/sd_commit.hpp>
#include <switchu/control_cache.hpp>
#include "core/PlayTime.hpp"
#include "widgets/GlossyIcon.hpp"
#include "widgets/FolderPalette.hpp"
#include "themeshop/ThemeHttp.hpp"
#include <nxui/core/Animation.hpp>
#include <nxui/core/I18n.hpp>
#include "DebugLog.hpp"

// Definido em main.cpp: quanto heap a escada conseguiu no arranque.
#ifndef SWITCHU_HOMEBREW
extern "C" size_t g_switchuHeapSize;
#endif
#include "bluetooth/BluetoothManager.hpp"
#include <switch.h>
#ifdef SWITCHU_MENU
#include "smi_commands.hpp"
#endif
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <string>
#include <chrono>
#include <vector>
#include <algorithm>
#include <unordered_map>
#include <limits>
#include <unordered_set>
#include <fstream>
#include <filesystem>
#include <nxui/third_party/stb/stb_image.h>
#include <nlohmann/json.hpp>
#include <system_error>

namespace {

static constexpr const char* kLayoutPath = "sdmc:/config/SwitchU/layout.json";

// A controller that cannot press A is worth being able to tell apart after the
// fact: a lone Joy-Con registered on its own reports style JoyLeft or JoyRight
// and, with the sideways hold type, has its four direction buttons standing in
// for A/B/X/Y, while half of a pair that is still registered as a pair reports
// JoyDual with one side missing and has no action buttons at all. Logged only
// when it changes.
void logControllerState(float dt) {
    static float timer = 0.f;
    static std::string previous;
    timer += dt;
    if (timer < 2.f)
        return;
    timer = 0.f;

    static const struct { HidNpadIdType id; const char* name; } kPads[] = {
        {HidNpadIdType_Handheld, "handheld"}, {HidNpadIdType_No1, "p1"},
        {HidNpadIdType_No2, "p2"}, {HidNpadIdType_No3, "p3"},
        {HidNpadIdType_No4, "p4"}, {HidNpadIdType_No5, "p5"},
        {HidNpadIdType_No6, "p6"}, {HidNpadIdType_No7, "p7"},
        {HidNpadIdType_No8, "p8"},
    };

    std::string line;
    char entry[96];
    for (const auto& pad : kPads) {
        const u32 style = hidGetNpadStyleSet(pad.id);
        if (style == 0)
            continue;
        u32 attributes = 0;
        if (style & HidNpadStyleTag_NpadJoyDual) {
            HidNpadJoyDualState state{};
            if (hidGetNpadStatesJoyDual(pad.id, &state, 1) > 0)
                attributes = state.attributes;
        }
        std::snprintf(entry, sizeof(entry), " %s=0x%X/0x%X",
                      pad.name, style, attributes);
        line += entry;
    }
    if (line.empty())
        line = " none";
    if (line == previous)
        return;
    previous = line;

    HidNpadJoyHoldType holdType = HidNpadJoyHoldType_Vertical;
    hidGetNpadJoyHoldType(&holdType);
    DebugLog::log("[controllers] hold=%s style/attr:%s",
                  holdType == HidNpadJoyHoldType_Horizontal ? "sideways" : "upright",
                  line.c_str());
}

static constexpr int kMinHomePages = 8;
static constexpr const char* kBuiltInSoundPreset = "wiiu";
// How often the console's own sleep plan is re-read. It changes only when
// somebody edits it in Settings, so once every few seconds is plenty and keeps
// a system-settings IPC call out of the frame loop.
static constexpr float kSleepPlanPollSeconds = 3.f;

// The two ladders Horizon stores for automatic sleep, in seconds, in the order
// the Sleep tab shows them. The sixth position is Never in both.
static float sleepPlanSeconds(bool docked, std::uint8_t plan) {
    static const float kHandheld[] = { 60.f, 180.f, 300.f, 600.f, 1800.f };
    static const float kDocked[]   = { 3600.f, 7200.f, 10800.f, 21600.f, 43200.f };
    const int index = static_cast<int>(plan);
    if (index < 0 || index > 4)
        return -1.f;
    return docked ? kDocked[index] : kHandheld[index];
}

static constexpr float kGridRectX = 0.f;
static constexpr float kGridRectY = 90.f;
static constexpr float kGridRectW = 1280.f;
static constexpr float kGridRectH = 540.f;

static constexpr float kGridBaseCellW = 150.f;
static constexpr float kGridBaseCellH = 150.f;
static constexpr float kGridBasePadX  = 20.f;
static constexpr float kGridBasePadY  = 16.f;

// Filters the canonical model down to what a single-row view can show, keeping
// every surviving entry's stable identity untouched. `dropWidgets` additionally
// removes widget entries: Flow draws physical cases, and there is no meaningful
// case for a clock or a battery readout, so those are hidden by this explicit
// view-capability rule rather than by being drawn wrongly. Folders survive in
// both views and still open into Grid.
GridModel compactDynamicLineEntries(const GridModel& source, bool dropWidgets = false) {
    GridModel compacted;
    for (const auto& entry : source.entries()) {
        if (entry.kind == GridEntryKind::Empty ||
            entry.kind == GridEntryKind::WidgetContinuation)
            continue;
        if (dropWidgets && entry.kind == GridEntryKind::Widget)
            continue;
        compacted.addEntry(entry);
    }
    // No padding back out to the source slot count. The home layout reserves a
    // hundred and twenty slots, so padding left the line with about ninety empty
    // entries after the last real one. They cost nothing to draw once hidden,
    // but they sit in the carousel's index space: wrapping from the last title
    // to the first would have had to travel across all of them, or take the
    // shorter way round and rewind the whole line. The line keeps only what it
    // actually shows, so its index space is the ring.
    return compacted;
}

bool isPackageSoundPreset(const std::string& preset) {
    return preset.rfind("package:", 0) == 0;
}

bool pathExists(const std::string& path) {
    std::error_code ec;
    return std::filesystem::exists(path, ec);
}

bool directoryExists(const std::string& path) {
    std::error_code ec;
    return std::filesystem::is_directory(path, ec);
}

std::string resolveAudioOverridePath(const std::string& preferredBase,
                                    const std::string& fallbackBase,
                                    const char* relativePath) {
    if (!preferredBase.empty()) {
        const std::string preferredPath = preferredBase + "/" + relativePath;
        if (pathExists(preferredPath))
            return preferredPath;
    }
    return fallbackBase + "/" + relativePath;
}

std::string installedThemePathFromPackagePreset(const std::string& preset) {
    if (!isPackageSoundPreset(preset))
        return {};

    const std::string slug = preset.substr(std::strlen("package:"));
    if (slug.empty())
        return {};

    const std::string installPath = std::string("sdmc:/config/SwitchU/themes/") + slug;
    return directoryExists(installPath) ? installPath : std::string();
}

std::string resolveThemeSoundBase(const std::string& installPath) {
    if (installPath.empty())
        return {};

    const std::string directSfx = installPath + "/sfx";
    const std::string directMusic = installPath + "/music";
    const std::string soundsRoot = installPath + "/sounds";

    const bool hasDirect = directoryExists(directSfx) || directoryExists(directMusic);
    if (hasDirect)
        return installPath;
    if (directoryExists(soundsRoot))
        return soundsRoot;
    return {};
}

// Keep enough side/top clearance so large grids do not overlap HUD/side buttons.
static constexpr float kGridSafeSideMargin = 220.f;

// Contextual action capsules, bottom-right.
static constexpr float kHintIconScale = 0.72f;
static constexpr float kHintTextScale = 0.62f;
static constexpr float kHintCapH      = 28.f;
static constexpr float kHintCapPadX   = 12.f;
static constexpr float kHintIconGap   = 6.f;
static constexpr float kHintCapGap    = 8.f;
static constexpr float kHintRowGap    = 7.f;
static constexpr float kHintEdgeX     = 18.f;
static constexpr float kHintEdgeY     = 16.f;
static constexpr float kHintRowMaxW   = 522.f;
// Clearance kept between the title pill and the first capsule, and the width
// below which the bar stops shrinking and starts stacking instead.
static constexpr float kHintPillGap   = 18.f;
static constexpr float kHintRowMinW   = 168.f;
static constexpr int   kHintMaxItems  = 8;
static constexpr float kHintWidthDur  = 0.22f;
static constexpr float kHintSwapDur   = 0.18f;

// Page arrows flanking the grid.
static constexpr float kPageArrowInset = 152.f;
// The dynamic line fills the whole width, so the paged inset put both arrows on
// top of the outermost games. They sit near the screen edge there instead.
static constexpr float kLineArrowInset = 64.f;
// Long enough that a deliberate single press stays a single step, then quick
// enough that crossing the line is a scroll rather than a queue of taps.
static constexpr float kLineRepeatDelay = 0.40f;
static constexpr float kLineRepeatInterval = 0.11f;
static constexpr float kPageArrowW     = 54.f;
static constexpr float kPageArrowH     = 72.f;
static constexpr float kPageArrowFade  = 0.20f;
static constexpr float kPageArrowKick  = 0.32f;
static constexpr float kAddPageHoldDur = 1.0f;

static constexpr float kGridSafeTopBottomMargin = 20.f;

std::string titleIdToHex(uint64_t v) {
    char buf[17] = {};
    std::snprintf(buf, sizeof(buf), "%016llX", (unsigned long long)v);
    return std::string(buf);
}

bool hexToTitleId(const std::string& s, uint64_t& out) {
    if (s.empty()) {
        out = 0;
        return false;
    }
    char* end = nullptr;
    unsigned long long v = std::strtoull(s.c_str(), &end, 16);
    if (end == s.c_str() || *end != '\0') {
        out = 0;
        return false;
    }
    out = (uint64_t)v;
    return true;
}

int hexNibble(char ch) {
    if (ch >= '0' && ch <= '9') return ch - '0';
    if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
    if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
    return -1;
}

bool hexToAccountUid(const std::string& s, AccountUid& out) {
    if (s.size() != 32)
        return false;

    AccountUid uid{};
    for (int part = 0; part < 2; ++part) {
        uint64_t value = 0;
        for (int i = 0; i < 16; ++i) {
            int nibble = hexNibble(s[(size_t)(part * 16 + i)]);
            if (nibble < 0)
                return false;
            value = (value << 4) | (uint64_t)nibble;
        }
        uid.uid[part] = value;
    }

    if (!accountUidIsValid(&uid))
        return false;
    out = uid;
    return true;
}

const char* safeTag(const nxui::Widget* widget) {
    if (!widget || widget->tag().empty())
        return "<none>";
    return widget->tag().c_str();
}

std::string utf8Codepoint(uint32_t cp) {
    std::string out;
    if (cp <= 0x7F) {
        out.push_back((char)cp);
    } else if (cp <= 0x7FF) {
        out.push_back((char)(0xC0 | (cp >> 6)));
        out.push_back((char)(0x80 | (cp & 0x3F)));
    } else if (cp <= 0xFFFF) {
        out.push_back((char)(0xE0 | (cp >> 12)));
        out.push_back((char)(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back((char)(0x80 | (cp & 0x3F)));
    } else {
        out.push_back((char)(0xF0 | (cp >> 18)));
        out.push_back((char)(0x80 | ((cp >> 12) & 0x3F)));
        out.push_back((char)(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back((char)(0x80 | (cp & 0x3F)));
    }
    return out;
}

std::string buttonGlyph(nxui::Button button) {
    switch (button) {
        case nxui::Button::A: return utf8Codepoint(0xE0E0);
        case nxui::Button::B: return utf8Codepoint(0xE0E1);
        case nxui::Button::X: return utf8Codepoint(0xE0E2);
        case nxui::Button::Y: return utf8Codepoint(0xE0E3);
        case nxui::Button::L: return utf8Codepoint(0xE0E4);
        case nxui::Button::R: return utf8Codepoint(0xE0E5);
        case nxui::Button::ZL: return utf8Codepoint(0xE0E6);
        case nxui::Button::ZR: return utf8Codepoint(0xE0E7);
        case nxui::Button::Plus: return utf8Codepoint(0xE0F1);
        case nxui::Button::Minus: return utf8Codepoint(0xE0F2);
        case nxui::Button::LStick: return utf8Codepoint(0xE0C4);
        case nxui::Button::RStick: return utf8Codepoint(0xE0C5);
        default: return {};
    }
}

std::string dpadGlyph() {
    return utf8Codepoint(0xE0EA);
}

bool appEntriesRefreshEquivalent(const AppEntry& a, const AppEntry& b) {
    return a.id == b.id &&
           a.title == b.title &&
           a.titleId == b.titleId &&
           a.viewFlags == b.viewFlags &&
           a.userRequired == b.userRequired &&
           a.startupUserKnown == b.startupUserKnown &&
           a.startupUserAccount == b.startupUserAccount &&
           a.startupUserAccountOption == b.startupUserAccountOption;
}

bool gridModelsRefreshEquivalent(const GridModel& a, const GridModel& b) {
    if (a.count() != b.count())
        return false;
    for (int i = 0; i < a.count(); ++i) {
        if (!appEntriesRefreshEquivalent(a.at(i), b.at(i)))
            return false;
    }
    return true;
}

}


WiiUMenuApp::WiiUMenuApp() {}
WiiUMenuApp::~WiiUMenuApp() {
    m_animalesePlayer.clear();
    rootBox().clearChildren();
}

void WiiUMenuApp::setTutorialStartupFade(bool enabled) {
    m_tutorialStartupFade = enabled;
}

void WiiUMenuApp::setStartupConfig(const AppConfig& config) {
    m_config = config;
    m_startupConfigProvided = true;
}

#ifdef SWITCHU_MENU
void WiiUMenuApp::setStartupStatus(const switchu::smi::SystemStatus& status) {
    m_launcher.setStartupStatus(status.suspended_app_id, status.app_running);
    m_transitionOriginTick = status.transition_origin_tick;
}

void WiiUMenuApp::setMenuMainTrace(uint64_t tick, uint32_t core) {
    m_menuMainTick = tick;
    m_menuMainCore = core;
}
#endif

bool WiiUMenuApp::onCreate() {
    const uint64_t activityCreateStartTick = armGetSystemTick();
    auto initElapsedMs = [activityCreateStartTick]() -> unsigned long {
        return static_cast<unsigned long>(
            armTicksToNs(armGetSystemTick() - activityCreateStartTick) / 1000000ULL);
    };
    DebugLog::log("[init] onCreate enter");
    m_iconStreamer.setThreadPool(&m_threadPool);
    if (!m_startupConfigProvided)
        m_config.load();
    const bool fastReturn = m_launcher.suspendedTitleId() != 0;
    m_fastReturnRequested = fastReturn;
    m_fastReturnStartupTick = fastReturn ? activityCreateStartTick : 0;
    m_appLayoutMode = m_config.appLayoutMode;
    applyGlassSharpness(m_config.glassSharpness);
    loadMenuLayout();
    // The menu is recreated on every return from a game, so this is also the
    // moment the session just played has to reach the grid. The first frame
    // sorts from the cache saved at launch; pdm is asked once the grid is up.
    m_playtimeRefreshQueued = m_config.sortMode == 4;
    if (!m_folderStore.load())
        DebugLog::log("[folders] store unavailable; continuing with an empty folder list");
    if (!m_widgetStore.load())
        DebugLog::log("[widgets] store unavailable; continuing with an empty widget list");
    normalizeWidgetPlacements();
    if (m_widgetStore.recentActivity().titleId != 0) {
        refreshRecentActivityDuration();
        if (!m_widgetStore.save())
            DebugLog::log("[widgets] recent activity duration could not be saved");
    }
    m_appLoader.setPendingTransform([this](std::vector<PendingApp>& apps) {
        applyMenuLayoutToPending(apps);
    });
    m_appLoader.setPrefetchIcons(!fastReturn);
    // Catalog I/O is independent of font/GPU setup. Start it now so SD reads
    // and metadata parsing overlap with i18n, audio, and resource creation.
    m_appLoader.startAsync(m_threadPool);

    nxui::I18n::instance().initialize(std::string(SD_ASSETS) + "/i18n", "en-US");
    applyUiLanguage();
    m_accessibility.initialize(m_config.accessibilityEnabled,
                               nxui::I18n::instance().activeLanguageTag(),
                               SD_ASSETS);
    m_accessibility.setSpeakHints(m_config.accessibilitySpeakHints);
    m_accessibility.setSpeakContextEveryFocus(m_config.accessibilitySpeakContextEveryFocus);
    m_accessibility.setSpeechRate(m_config.accessibilitySpeechRate);

    m_audioFuture = m_threadPool.submit([this]() {
        m_audio.initialize();
        m_availablePresets = scanAvailablePresets();
        if (!isPackageSoundPreset(m_config.soundPreset) && m_config.soundPreset != kBuiltInSoundPreset) {
            DebugLog::log("[audio] preset '%s' is no longer shipped, falling back to '%s'",
                          m_config.soundPreset.c_str(),
                          kBuiltInSoundPreset);
            m_config.soundPreset = kBuiltInSoundPreset;
        }
        loadSoundPreset(resolveSoundPresetId(m_config.soundPreset));
    });
    DebugLog::log("[init] Audio loading started on background thread");

    if (m_launcher.suspendedTitleId() != 0) {
        m_deferredBluetoothInitFrames = 120;
        DebugLog::log("[init] Bluetooth manager initialization deferred for fast return");
    } else {
        bluetooth::Initialize();
        DebugLog::log("[init] Bluetooth manager %s",
                      bluetooth::IsAvailable() ? "initialized" : "unavailable");
    }
    DebugLog::log("[init] Theme Shop HTTP runtime deferred until first request");

    DebugLog::log("[init] Config loaded (theme=%s, musicVol=%.2f, sfxVol=%.2f)",
                  m_config.themePreset.c_str(), m_config.musicVolume, m_config.sfxVolume);

    m_launcher.init({
        .playSfxModalHide = [this]() { m_audio.playSfx(Sfx::ModalHide); },
        .requestExit = [this]() { app().requestExit(); },
        .quiesceWriters = [this]() { quiesceWritersForPowerAction(); },
    });



    // How much room this process actually has, asked of the kernel rather than
    // assumed. The 32 MB image budget is a number chosen in nxui, not a limit
    // anything reported: if the process holds far more than it spends, theme
    // animations can be sharper, and if it does not, that settles it.
    {
        u64 total = 0, used = 0;
        const Result rt = svcGetInfo(&total, InfoType_TotalMemorySize, CUR_PROCESS_HANDLE, 0);
        const Result ru = svcGetInfo(&used,  InfoType_UsedMemorySize,  CUR_PROCESS_HANDLE, 0);
        if (R_SUCCEEDED(rt) && R_SUCCEEDED(ru)) {
            // O heap e o teto de verdade: toda imagem sai dele, junto com
            // framebuffers, fontes, icones e o heap do C++. Medido no console,
            // com 224 MB de heap o processo morria por volta de 108 MB de
            // imagem, ou seja o resto ocupava cerca de 115 MB.
            //
            // Entao o orcamento e o que sobra do heap concedido depois de
            // reservar essa parte, e nao um numero escolhido a mao: com 224 MB
            // reproduz os 112 de antes, com 352 MB sobe para 232.
#ifndef SWITCHU_HOMEBREW
            {
                constexpr uint64_t kNonImageReserve = 120ull * 1024ull * 1024ull;
                const uint64_t heap = g_switchuHeapSize;
                if (heap > kNonImageReserve + nxui::GpuDevice::kDefaultImageBudget)
                    nxui::GpuDevice::setImageBudget(heap - kNonImageReserve);
                DebugLog::log("[mem] heap concedido %.1f MB", heap / 1048576.0);
            }
#endif
            DebugLog::log("[mem] process has %.1f MB, using %.1f MB, %.1f MB free "
                          "-- image budget is %.1f MB of that",
                          total / 1048576.0, used / 1048576.0,
                          (double)(total - used) / 1048576.0,
                          nxui::GpuDevice::imageBudget() / 1048576.0);
        } else {
            DebugLog::log("[mem] svcGetInfo failed (total=0x%x used=0x%x)", rt, ru);
        }
    }

    DebugLog::log("[init] loadResources...");
    loadResources();
    DebugLog::log("[init] loadResources done at %lums", initElapsedMs());
    DebugLog::log("[init] buildGrid...");
    buildGrid();
    DebugLog::log("[init] buildGrid done at %lums", initElapsedMs());

    setupLockScreen();

#ifdef SWITCHU_DEBUG_UI
    m_debugOverlay = std::make_unique<DebugImGuiOverlay>();
    if (!m_debugOverlay->initialize(app().gpu(), app().renderer())) {
        DebugLog::log("[debug] ImGui overlay init failed");
        m_debugOverlay.reset();
    } else {
        DebugLog::log("[debug] ImGui overlay ready");
    }
#endif

#ifdef SWITCHU_MENU
    m_sysMsg.setCallback([this](SysAction a) { handleSystemAction(a); });
    DebugLog::log("[init] async notifications via AppletStorage only");

    const auto& initTrace = app().initializeTrace();
    switchu::smi::MenuReadyArgs ready{};
    ready.transition_origin_tick = m_transitionOriginTick;
    ready.menu_main_tick = m_menuMainTick;
    ready.initialize_start_tick = initTrace.initializeStartTick;
    ready.gpu_ready_tick = initTrace.gpuReadyTick;
    ready.renderer_ready_tick = initTrace.rendererReadyTick;
    ready.blank_frame_tick = initTrace.blankFrameTick;
    ready.activity_create_start_tick = activityCreateStartTick;
    ready.activity_create_end_tick = armGetSystemTick();
    ready.menu_main_core = m_menuMainCore;
    ready.activity_core = svcGetCurrentProcessorNumber();
    ready.catalog_count = static_cast<uint32_t>(m_model.count());
    switchu::menu::smi_cmd::menuReady(ready);

    app().setFirstFrameCallback([this](uint64_t firstInputTick, uint64_t firstFrameTick) {
        switchu::smi::MenuFirstFrameArgs first{};
        first.transition_origin_tick = m_transitionOriginTick;
        first.first_input_tick = firstInputTick;
        first.first_frame_tick = firstFrameTick;
        first.image_memory_bytes = app().gpu().imageMemoryUsed();
        first.core = svcGetCurrentProcessorNumber();
        first.catalog_count = static_cast<uint32_t>(m_model.count());
        switchu::menu::smi_cmd::menuFirstFrame(first);
    });
#endif

    DebugLog::log("[init] DONE total=%lums fastReturn=%d",
                  initElapsedMs(), fastReturn ? 1 : 0);
    return true;
}

// Blocks until this process has no disk write outstanding. Called before a
// power request is handed to the daemon, so nothing of ours is mid-write when
// the console goes down. Deliberately does not ask the applet to exit: doing
// that made the daemon see no menu running and relaunch it into the middle of
// the reboot, leaving the console black and needing RCM.
void WiiUMenuApp::quiesceWritersForPowerAction() {
    if (m_configSaveFuture.valid())
        m_configSaveFuture.get();
    if (m_themePackageTransferFuture.valid())
        m_themePackageTransferFuture.wait();
    if (m_gameArtworkSaveFuture.valid())
        m_gameArtworkSaveFuture.wait();
    if (m_layoutDirty)
        saveMenuLayout();

    // Waiting for the writes is not the same as the card having them. fsdev
    // returns before the FAT is updated, so a reboot here leaves the card
    // inconsistent -- hekate then comes up unable to find nyx, and the CFW
    // files have to be restored by hand. Reported after rebooting from the
    // power menu, more than once.
    //
    // This function exists to be the last thing that runs before power is cut,
    // which makes it the one place the commit cannot be skipped.
    switchu::commitSdCard("power action");

    DebugLog::log("[menu] writers quiesced and card committed for power action");
}


void WiiUMenuApp::onDestroy() {
#ifdef SWITCHU_MENU
    switchu::smi::MenuClosingArgs closingTrace{};
    const auto& shutdownTrace = app().shutdownTrace();
    closingTrace.shutdown_start_tick = shutdownTrace.shutdownStartTick;
    closingTrace.gpu_drain_start_tick = shutdownTrace.gpuDrainStartTick;
    closingTrace.gpu_drain_end_tick = shutdownTrace.gpuDrainEndTick;
    closingTrace.on_destroy_start_tick = shutdownTrace.activityDestroyStartTick;
#endif
    // Wake any libcurl transfer before shutdown tries to take its mutex or the
    // worker pool joins. Without this, a stalled package transfer can hold the
    // outgoing menu process for its 30-second low-speed window.
    themeshop::http::cancelPendingRequests();
#ifdef SWITCHU_MENU
    closingTrace.http_cancel_done_tick = armGetSystemTick();
#endif

    if (m_audioFuture.valid()) m_audioFuture.get();
#ifdef SWITCHU_MENU
    if (m_playtimeRefresh)
        m_playtimeRefresh->cancelled.store(true);
#endif

    // Theme Shop, Gallery, metadata, icon, and artwork work all shares this
    // pool.  The old order stopped libcurl/Bluetooth first and relied on the
    // ThreadPool destructor much later to join workers.  A launch immediately
    // after Theme Shop/Gallery activity could therefore tear down a runtime
    // while one of those workers was still using it.  Drain while every owner
    // is still alive; normal launch animation time already absorbs this wait.
    const uint64_t workerDrainStartTick = armGetSystemTick();
    m_threadPool.waitForIdle();
    const uint64_t workerDrainUs =
        (armGetSystemTick() - workerDrainStartTick) * 1'000'000ULL /
        armGetSystemTickFreq();
    DebugLog::log("[menu] handoff workers quiesced in %llu us",
                  (unsigned long long)workerDrainUs);
#ifdef SWITCHU_MENU
    closingTrace.worker_drain_done_tick = armGetSystemTick();
#endif

#ifdef SWITCHU_MENU
    // Remember where we were. Returning from a suspended game already jumps to
    // its page, but closing a game outright or opening an applet suspends
    // nothing, and the menu came back on page one every time. Recorded as a
    // title rather than a page number: the grid can be rebuilt at a different
    // width between leaving and coming back, and the page number would then
    // point somewhere else.
    if (m_grid && m_grid->iconsPerPage() > 0) {
        const int first = m_grid->currentPage() * m_grid->iconsPerPage();
        if (first >= 0 && first < m_model.count()) {
            const std::uint64_t anchor = m_model.at(first).titleId;
            if (anchor != m_config.lastPageTitleId) {
                m_config.lastPageTitleId = anchor;
                m_config.save();
                switchu::commitSdCard("last page");
            }
        }
    }
#endif

#ifdef SWITCHU_MENU
    closingTrace.state_persist_done_tick = armGetSystemTick();
#endif

#ifdef SWITCHU_DEBUG_UI
    if (m_debugOverlay) {
        m_debugOverlay->shutdown(app().gpu());
        m_debugOverlay.reset();
    }
#endif

    stopEditGhost();

    themeshop::http::shutdown();
#ifdef SWITCHU_MENU
    closingTrace.http_shutdown_done_tick = armGetSystemTick();
#endif

    bluetooth::Finalize();

#ifdef SWITCHU_MENU
    closingTrace.bluetooth_done_tick = armGetSystemTick();
    closingTrace.core = svcGetCurrentProcessorNumber();
    switchu::menu::smi_cmd::menuClosing(closingTrace);
#endif
    if (m_layoutDirty)
        saveMenuLayout();
    m_animalesePlayer.clear();
    m_accessibility.shutdown();
    m_audio.shutdown();
}

void WiiUMenuApp::loadResources() {
    DebugLog::log("[init] loadResources: regular font");
    std::string fontPath = std::string(SD_ASSETS) + "/fonts/DejaVuSans.ttf";
    if (m_fontNormal.load(app().gpu(), app().renderer(), fontPath, 24))
        m_loadedRegularFontPath = fontPath;
    if (m_fontSmall.load(app().gpu(), app().renderer(), fontPath, 18))
        m_loadedSmallFontPath = fontPath;
    m_fontIcons.load(app().gpu(), app().renderer(), std::string(SD_ASSETS) + "/fonts/switch_icons.ttf", 24);
    m_fontClock.load(app().gpu(), app().renderer(), fontPath, 72);

    // loadStaticTextures() also owns the gamecard badge. The 1.2 merge added it
    // with the page-arrow and battery-widget textures inside, but never called
    // it from the retained fork loadResources(), so those textures stayed
    // invalid: renderPageArrows() bails on !texture.valid(), which is why the
    // ZL/ZR page arrows never appeared on hardware.
    loadStaticTextures();

    m_appLoader.finalize(m_model, m_iconStreamer);
    // AppListLoader owns the initial native-icon setup.  Install the optional
    // Gallery lookup immediately afterwards so custom covers work after a
    // cold start too, not only after a sort or refresh rebuild.
    m_iconStreamer.setArtworkDataLoader(gallery::GameArtworkStore::loadCover);
}

#if 0 // Replaced by the 1.2.0 folder/widget-aware implementation below.
void WiiUMenuApp::buildUserAvatarBar() {
    m_userAvatarButtons.clear();

    m_userAvatarBar = std::make_shared<nxui::Box>(nxui::Axis::ROW);
    m_userAvatarBar->setMarginTop(17.f);
    m_userAvatarBar->setGap(10.f);
    m_userAvatarBar->setShrink(0.f);
    m_userAvatarBar->setSize(0.f, 56.f);
    m_userAvatarBar->setTag("userAvatarBar");
    m_userAvatarBar->setWireframeEnabled(false);

    AccountUid uids[8] = {};
    s32 count = 0;
    Result rc = accountListAllUsers(uids, 8, &count);
    DebugLog::log("[profiles] accountListAllUsers rc=0x%X count=%d", rc, count);
    if (R_FAILED(rc) || count <= 0)
        return;

    for (int i = 0; i < count; ++i) {
        AccountProfile profile{};
        rc = accountGetProfile(&profile, uids[i]);
        if (R_FAILED(rc))
            continue;

        auto avatar = std::make_shared<UserAvatarButton>();
        avatar->setSize(56.f, 56.f);
        avatar->setMinWidth(56.f);
        avatar->setMinHeight(56.f);
        avatar->setShrink(0.f);
        avatar->setCornerRadius(28.f);
        avatar->setUid(uids[i]);
        avatar->setFocusable(true);

        AccountProfileBase base{};
        AccountUserData userData{};
        if (R_SUCCEEDED(accountProfileGet(&profile, &userData, &base)))
            avatar->setNickname(base.nickname);

        u32 imgSize = 0;
        if (R_SUCCEEDED(accountProfileGetImageSize(&profile, &imgSize)) && imgSize > 0) {
            std::vector<uint8_t> imgBuf(imgSize);
            u32 realSize = 0;
            if (R_SUCCEEDED(accountProfileLoadImage(&profile, imgBuf.data(), imgSize, &realSize))
                    && realSize > 0) {
                avatar->loadAvatar(app().gpu(), app().renderer(), imgBuf.data(), realSize);
            }
        }

        avatar->setOnActivate([this]() {
            m_audio.playSfx(Sfx::Activate);
#ifdef SWITCHU_MENU
            // The accounts open in the same styled dialog the launch flow uses,
            // with a create tile after them. Picking an account still opens its
            // page, which is what tapping the avatar used to do directly.
            if (!m_userSelect)
                return;
            auto& i18n = nxui::I18n::instance();
            m_userSelect->loadUsers(app().gpu(), app().renderer());
            m_userSelect->showUserSwitcher(
                [this](AccountUid picked) { m_launcher.launchUserPage(picked); },
                [this]() { m_launcher.launchUserCreator(); },
                i18n.tr("userselect.add_user", "Add user"));
            // Without this the dialog draws but no button reaches it, which is
            // how it came up touch-only.
            raiseOverlay(m_userSelect);
            focusManager().setFocus(m_userSelect.get());
#endif
        });

        accountProfileClose(&profile);
        m_userAvatarButtons.push_back(avatar);
        m_userAvatarBar->addChild(avatar);
    }

    if (!m_userAvatarButtons.empty()) {
        const float countF = static_cast<float>(m_userAvatarButtons.size());
        m_userAvatarBar->setSize(countF * 56.f + (countF - 1.f) * 10.f, 56.f);
        m_userAvatarButtons.front()->setCustomNavigation(nxui::FocusDirection::LEFT,
                                                         m_userAvatarButtons.front().get());
        m_userAvatarButtons.back()->setCustomNavigation(nxui::FocusDirection::RIGHT,
                                                        m_userAvatarButtons.back().get());
    }
}

WiiUMenuApp::GridLayoutMetrics WiiUMenuApp::computeGridLayoutMetrics() const {
    const int cols = std::clamp(m_config.gridColumns, 3, 8);
    const int rows = std::clamp(m_config.gridRows, 2, 5);

    const float baseGridW = cols * kGridBaseCellW + (cols - 1) * kGridBasePadX;
    const float baseGridH = rows * kGridBaseCellH + (rows - 1) * kGridBasePadY;

    const float safeW = std::max(1.f, kGridRectW - (kGridSafeSideMargin * 2.f));
    const float safeH = std::max(1.f, kGridRectH - (kGridSafeTopBottomMargin * 2.f));

    const float scaleW = safeW / baseGridW;
    const float scaleH = safeH / baseGridH;
    const float scale = std::min(1.f, std::min(scaleW, scaleH));

    GridLayoutMetrics m;
    m.cellW = std::max(88.f, kGridBaseCellW * scale);
    m.cellH = std::max(88.f, kGridBaseCellH * scale);
    m.padX = std::max(8.f, kGridBasePadX * scale);
    m.padY = std::max(8.f, kGridBasePadY * scale);
    return m;
}



void WiiUMenuApp::reflowHomeGrid() {
    if (!m_grid)
        return;

    const int oldFocusedIndex = m_grid->focusedGlobalIndex();
    const int oldPage = m_grid->currentPage();
    uint64_t focusedTitleId = 0;
    if (oldFocusedIndex >= 0 && oldFocusedIndex < m_model.count())
        focusedTitleId = m_model.at(oldFocusedIndex).titleId;

    std::unordered_map<uint64_t, AppEntry> byId;
    std::vector<uint64_t> appOrder;
    byId.reserve((size_t)std::max(0, m_model.count()));
    appOrder.reserve((size_t)std::max(0, m_model.count()));
    for (const auto& entry : m_model.entries()) {
        if (entry.titleId == 0 || byId.count(entry.titleId))
            continue;
        appOrder.push_back(entry.titleId);
        byId.emplace(entry.titleId, entry);
    }

    // Recent, Favorites, and Most played start from the personal arrangement.
    // When the metric ties, falling back to A-Z makes the view visually
    // indistinguishable from another mode and makes R appear broken.
    if ((m_config.sortMode == 2 || m_config.sortMode == 3 || m_config.sortMode == 4) &&
        !m_layoutSlots.empty()) {
        std::vector<uint64_t> customOrder;
        std::unordered_set<uint64_t> seen;
        customOrder.reserve(appOrder.size());
        seen.reserve(appOrder.size());
        for (uint64_t tid : m_layoutSlots) {
            if (tid != 0 && byId.count(tid) && seen.insert(tid).second)
                customOrder.push_back(tid);
        }
        for (uint64_t tid : appOrder) {
            if (seen.insert(tid).second)
                customOrder.push_back(tid);
        }
        appOrder = std::move(customOrder);
    }

    // Sorting replaces the saved arrangement rather than editing it: the
    // hand-made layout is kept on disk untouched, so switching back to it
    // restores exactly what the owner built instead of an approximation.
    if (m_config.sortMode != 0) {
        std::stable_sort(appOrder.begin(), appOrder.end(),
                  [&](uint64_t a, uint64_t b) {
            if (m_config.sortMode == 4) {
                const auto playedA = m_config.playtimeOf(a);
                const auto playedB = m_config.playtimeOf(b);
                // Never-played titles sort last rather than first, and equal
                // durations retain the owner's personal arrangement.
                if (playedA != playedB) return playedA > playedB;
                return false;
            }
            if (m_config.sortMode == 3) {
                const bool favA = m_config.isFavorite(a);
                const bool favB = m_config.isFavorite(b);
                if (favA != favB) return favA > favB;
                return false;
            }
            if (m_config.sortMode == 2) {
                const auto ta = m_config.lastOpenedAt(a);
                const auto tb = m_config.lastOpenedAt(b);
                // Never-opened titles sort last rather than first, which is
                // what a zero timestamp would otherwise do.
                if (ta != tb) return ta > tb;
                // Keep the personal order for equal timestamps. In particular
                // it makes the first use of Recent useful before any title has
                // been opened by this version of SwitchU.
                return false;
            }
            const auto& ea = byId.at(a);
            const auto& eb = byId.at(b);
            // Compared the way a person reads them, so "apple" and "Apple"
            // land together instead of in two separate blocks of the
            // alphabet. Inline because a file-scope helper has to be defined
            // above this and kept there.
            const std::string& la = ea.title;
            const std::string& lb = eb.title;
            const size_t n = std::min(la.size(), lb.size());
            for (size_t i = 0; i < n; ++i) {
                const unsigned char ca = (unsigned char)std::tolower((unsigned char)la[i]);
                const unsigned char cb = (unsigned char)std::tolower((unsigned char)lb[i]);
                if (ca != cb) return ca < cb;
            }
            return la.size() < lb.size();
        });
    }

    std::vector<uint64_t> slots = m_config.sortMode == 0 ? m_layoutSlots : appOrder;
    if (slots.empty())
        slots = appOrder;

    std::unordered_set<uint64_t> placed;
    placed.reserve(byId.size());
    for (auto& slotTid : slots) {
        if (slotTid == 0)
            continue;
        if (!byId.count(slotTid) || placed.count(slotTid)) {
            slotTid = 0;
            continue;
        }
        placed.insert(slotTid);
    }

    for (uint64_t tid : appOrder) {
        if (placed.count(tid))
            continue;

        auto emptyIt = std::find(slots.begin(), slots.end(), 0);
        if (emptyIt != slots.end())
            *emptyIt = tid;
        else
            slots.push_back(tid);
        placed.insert(tid);
    }

    const int cols = std::clamp(m_config.gridColumns, 3, 8);
    const int rows = std::clamp(m_config.gridRows, 2, 5);
    const int perPage = std::max(1, cols * rows);
    const int minimum = perPage * (m_config.dynamicPages ? 1 : kMinHomePages);
    int minSlots = std::max(minimum, (int)slots.size());
    if (m_config.dynamicPages) {
        int lastOccupied = -1;
        for (int i = static_cast<int>(slots.size()) - 1; i >= 0; --i) {
            if (slots[static_cast<size_t>(i)] != 0) {
                lastOccupied = i;
                break;
            }
        }
        const int occupiedPages = lastOccupied >= 0 ? (lastOccupied / perPage + 1) : 1;
        minSlots = std::max(perPage, occupiedPages * perPage);
        if ((int)slots.size() > minSlots)
            slots.resize(minSlots);
    }
    int roundedSlots = ((minSlots + perPage - 1) / perPage) * perPage;
    if ((int)slots.size() < roundedSlots)
        slots.resize(roundedSlots, 0);

    std::string orderSample;
    int sampleCount = 0;
    for (uint64_t tid : slots) {
        if (tid == 0)
            continue;
        const auto it = byId.find(tid);
        if (it == byId.end())
            continue;
        if (!orderSample.empty())
            orderSample += " | ";
        orderSample += it->second.title;
        if (++sampleCount >= 4)
            break;
    }
    int recentCount = 0;
    for (uint64_t tid : appOrder)
        recentCount += m_config.lastOpenedAt(tid) != 0 ? 1 : 0;
    DebugLog::log("[grid] sort=%d recent=%d first=%s",
                  m_config.sortMode, recentCount, orderSample.c_str());

    // Automatic views are temporary projections of the saved layout. Writing
    // them back here replaced the user's custom order every time R was used.
    if (m_config.sortMode == 0 && slots != m_layoutSlots) {
        m_layoutSlots = slots;
        m_layoutDirty = true;
    }

    GridModel rebuiltModel;
    for (uint64_t tid : slots) {
        if (tid == 0) {
            rebuiltModel.addEntry(AppEntry{});
            continue;
        }

        auto it = byId.find(tid);
        if (it != byId.end())
            rebuiltModel.addEntry(it->second);
        else
            rebuiltModel.addEntry(AppEntry{});
    }

    std::vector<std::shared_ptr<GlossyIcon>> icons;
    icons.reserve((size_t)std::max(0, rebuiltModel.count()));
    const auto& oldIcons = m_grid->allIcons();
    for (int i = 0; i < rebuiltModel.count(); ++i) {
        if (i < (int)oldIcons.size() && oldIcons[i] &&
            i < m_model.count() &&
            m_model.at(i).titleId == rebuiltModel.at(i).titleId) {
            icons.push_back(oldIcons[i]);
        } else {
            auto icon = makeIcon(rebuiltModel.at(i));
            icon->setBaseColor(m_theme.iconDefault);
            icons.push_back(std::move(icon));
        }
    }

    // setup() replaces m_allIcons.  Both focus managers keep raw Widget*
    // entries, so an icon whose title moved would otherwise be destroyed
    // while one of those entries still pointed to it.  The next R can then
    // make FocusManager call onFocusLost() through a freed vtable.  This was
    // the Data Abort at FocusManager::changeFocusTo after several sorts.
    // Invalidate before setup() so it never observes an old icon as current.
    for (const auto& icon : oldIcons) {
        if (!icon)
            continue;
        focusManager().invalidateWidget(icon.get());
        m_grid->focusManager().invalidateWidget(icon.get());
    }
    m_editBoundIcon = nullptr;
    m_editSourceIcon = nullptr;

    m_model = std::move(rebuiltModel);

    // IconStreamer associates its GPU slots with grid indices. Reusing that
    // mapping after a sort kept the old artwork at each physical index while
    // the title/focus model had already moved, so the cursor moved but the
    // visible icons appeared not to. Sorting is infrequent; rebuild this small
    // page cache from title IDs so the artwork follows its title exactly.
    for (const auto& icon : oldIcons) {
        if (icon)
            icon->setTexture(nullptr);
    }

    // A Texture owns both a deko3d descriptor and the GPU memory behind it.
    // The preceding frame may still be sampling an icon when R is handled, so
    // clearing the streamer immediately can free that memory while the queue
    // is using it.  That presents as a frame of corrupted tiles followed by a
    // User Break and an automatic menu relaunch.  Sorting is deliberately an
    // infrequent operation, making one queue drain here the safe trade-off.
    app().gpu().waitIdle();
    m_iconStreamer.clear();
    m_iconStreamer.init(m_model.count());
    m_iconStreamer.setIconDataLoader(AppListLoader::loadIconData);
    m_iconStreamer.setArtworkDataLoader(gallery::GameArtworkStore::loadCover);
    for (int i = 0; i < m_model.count(); ++i)
        m_iconStreamer.setTitleId(i, m_model.at(i).titleId);

    GridLayoutMetrics gridMetrics = computeGridLayoutMetrics();
    m_grid->setup(std::move(icons), cols, rows,
                  gridMetrics.cellW, gridMetrics.cellH,
                  gridMetrics.padX, gridMetrics.padY);

    int targetIndex = -1;
    if (focusedTitleId != 0)
        targetIndex = findTitleIndex(focusedTitleId);
    if (targetIndex < 0 && oldFocusedIndex >= 0 && m_model.count() > 0)
        targetIndex = std::clamp(oldFocusedIndex, 0, m_model.count() - 1);

    if (targetIndex >= 0)
        m_grid->focusGlobalIndex(targetIndex);
    else
        m_grid->setPage(oldPage);

    for (auto* icon : m_grid->pageIcons()) {
        if (icon)
            icon->forceVisible();
    }

    m_iconStreamer.onPageChanged(m_grid->currentPage(), m_grid->iconsPerPage(),
                                 app().gpu(), app().renderer(),
                                 m_grid->allIcons());
    DebugLog::log("[grid] icon cache rebuilt for sort=%d", m_config.sortMode);

    const bool overlayActive =
        (m_dialog && m_dialog->isActive()) ||
        (m_themeShop && m_themeShop->isActive()) ||
        (m_gameGallery && m_gameGallery->isActive()) ||
        (m_gameMods && m_gameMods->isActive()) ||
        (m_gameCheats && m_gameCheats->isActive()) ||
        (m_gameDetails && m_gameDetails->isActive()) ||
        (m_settings && m_settings->isActive()) ||
        (m_userSelect && m_userSelect->isActive());
    if (!overlayActive) {
        if (auto* cur = m_grid->focusManager().current())
            focusManager().setFocus(cur);
        updateCursor();
    }

    DebugLog::log("[grid] reflowed layout cols=%d rows=%d apps=%d page=%d",
                  cols, rows, m_model.count(), m_grid->currentPage());
}

void WiiUMenuApp::loadMenuLayout() {
    m_layoutSlots.clear();

    std::ifstream f(kLayoutPath);
    if (!f.is_open())
        return;

    nlohmann::json j;
    try {
        f >> j;
    } catch (...) {
        return;
    }

    auto it = j.find("slots");
    if (it == j.end() || !it->is_array())
        return;

    for (const auto& v : *it) {
        uint64_t tid = 0;
        if (v.is_string()) {
            std::string s = v.get<std::string>();
            if (!hexToTitleId(s, tid))
                tid = 0;
        } else if (v.is_number_unsigned()) {
            tid = v.get<uint64_t>();
        } else if (v.is_number_integer()) {
            auto raw = v.get<int64_t>();
            tid = raw > 0 ? (uint64_t)raw : 0;
        }
        m_layoutSlots.push_back(tid);
    }
}

#endif

std::string WiiUMenuApp::sortModeLabel() const {
    auto& i18n = nxui::I18n::instance();
    switch (m_config.sortMode) {
        case 1:  return i18n.tr("hint.sort_alpha", "A-Z");
        case 2:  return i18n.tr("hint.sort_recent", "Recent");
        case 3:  return i18n.tr("hint.sort_favorites", "Favorites");
        case 4:  return i18n.tr("hint.sort_playtime", "Most played");
        default: return i18n.tr("hint.sort_custom", "My order");
    }
}

void WiiUMenuApp::cycleSortMode() {
#ifdef SWITCHU_MENU
    if (m_editMode) return;
    m_config.sortMode = (m_config.sortMode + 1) % AppConfig::kSortModeCount;
    m_config.save();
    switchu::commitSdCard("sort mode");
    DebugLog::log("[menu] sort mode -> %d", m_config.sortMode);

    // Rebuilt in place so the change is visible at once. The hand-made layout
    // on disk is never rewritten by this, so coming back to it returns what
    // the owner actually arranged.
    m_audio.playSfx(Sfx::Navigate);
    reflowHomeGrid();
    // Most played sorts from the cache straight away, so R answers at once,
    // and asks pdm again behind it. The cache is only as fresh as the last
    // time this mode was in use; pollPlaytimeRefresh() re-sorts if pdm
    // disagrees with it.
    if (m_config.sortMode == 4)
        requestPlaytimeRefresh("sort mode");
#endif
}

void WiiUMenuApp::requestPlaytimeRefresh(const char* reason) {
#ifdef SWITCHU_MENU
    // Started by the next pollPlaytimeRefresh(), from onUpdate, rather than
    // here: callers are in the middle of a rebuild or a notification pass.
    DebugLog::log("[playtime] refresh requested (%s)", reason);
    m_playtimeRefreshQueued = true;
#else
    (void)reason;
#endif
}

void WiiUMenuApp::pollPlaytimeRefresh() {
#ifdef SWITCHU_MENU
    if (m_playtimeFuture.valid()) {
        if (m_playtimeFuture.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
            return;
        auto state = std::move(m_playtimeRefresh);
        bool failed = false;
        try {
            m_playtimeFuture.get();
        } catch (...) {
            // The pool refusing work during shutdown; the cache stays as it was.
            failed = true;
        }
        int changed = 0;
        if (!failed && state) {
            for (const auto& [titleId, nanoseconds] : state->playtime)
                changed += m_config.setPlaytime(titleId, nanoseconds) ? 1 : 0;
        }
        DebugLog::log("[playtime] refresh done titles=%zu changed=%d",
                      state ? state->playtime.size() : static_cast<std::size_t>(0), changed);
        // Nothing is saved here. Every launch writes the config before the
        // handoff, and that write carries this cache with it; a write of its
        // own would be one more SD commit for a copy pdm can always rebuild.
        if (changed > 0 && m_config.sortMode == 4)
            m_playtimeResortPending = true;
    }

    if (m_playtimeResortPending && m_grid) {
        if (m_config.sortMode != 4) {
            m_playtimeResortPending = false;
        } else if (m_openFolderId != 0) {
            // A folder keeps its own order. closeFolder() recomposes the root,
            // and that reads the cache just updated.
            m_playtimeResortPending = false;
            applyPlaytimeBadges();
        } else if (focusRoot() == &rootBox() && !m_editMode &&
                   !(m_launchAnim && m_launchAnim->isPlaying())) {
            // Held back while anything else owns input: a rebuild moves focus
            // to the grid and frees every icon, and edit mode and the launch
            // animation both hold on to one.
            m_playtimeResortPending = false;
            GridModel model = buildRootFolderModel();
            bool sameOrder = model.count() == m_model.count();
            for (int i = 0; sameOrder && i < model.count(); ++i)
                sameOrder = model.at(i).titleId == m_model.at(i).titleId;
            if (sameOrder) {
                applyPlaytimeBadges();
            } else {
                std::uint64_t focused = 0;
                if (auto* current = m_grid->focusManager().current();
                    current && current->tag() == "glossy_icon")
                    focused = static_cast<GlossyIcon*>(current)->titleId();
                applyDisplayModel(std::move(model), focused, false);
                if (m_layoutDirty) saveMenuLayout();
            }
            DebugLog::log("[playtime] grid %s", sameOrder ? "badges updated" : "re-sorted");
        }
    }

    if (!m_playtimeRefreshQueued)
        return;
    // Only once the catalogue is in and the first page has had its uploads:
    // the batch holds one of the pool's two workers for a round trip per title,
    // and the icons on screen come first.
    if (!m_grid || m_allApps.empty() || m_asyncRefreshPending ||
        m_deferredInitialAssetFrames > 0)
        return;
    m_playtimeRefreshQueued = false;

    // Only what can have changed. A title's play time moves when it is played,
    // and the menu is recreated after every session, so the title just played
    // plus anything with no figure yet is the whole set -- usually one query
    // instead of a hundred and seventeen. Asking for all of them cost 29
    // seconds on a console with that many, measured in its own log, and the
    // handoff to a game waits for this pool: launching or resuming during those
    // seconds left the launch animation frozen on screen for as long as the
    // batch had left to run.
    std::vector<std::uint64_t> titleIds;
    titleIds.reserve(m_allApps.size());
    const std::uint64_t justPlayed = m_launcher.suspendedTitleId() != 0
        ? m_launcher.suspendedTitleId() : m_config.lastPageTitleId;
    for (const auto& app : m_allApps) {
        if (!app.isApplication() || app.titleId == 0)
            continue;
        if (app.titleId == justPlayed || !m_config.hasPlaytime(app.titleId))
            titleIds.push_back(app.titleId);
    }
    if (titleIds.empty())
        return;

    auto state = std::make_shared<PlaytimeRefreshState>();
    m_playtimeRefresh = state;
    m_playtimeFuture = m_threadPool.submit(
        [state, titleIds = std::move(titleIds)]() {
            state->playtime = switchu::menu::playtime::queryAll(titleIds, &state->cancelled);
        });
#endif
}

std::string WiiUMenuApp::playtimeBadgeFor(const AppEntry& entry) const {
    // Only in the view the number orders. Everywhere else it would be clutter
    // on every icon, answering a question nobody asked of that view.
    if (m_config.sortMode != 4 || !entry.isApplication())
        return {};
    return switchu::menu::playtime::formatCompact(m_config.playtimeOf(entry.titleId));
}

void WiiUMenuApp::applyPlaytimeBadges() {
    if (!m_grid)
        return;
    const auto& icons = m_grid->allIcons();
    for (int i = 0; i < m_model.count() && i < static_cast<int>(icons.size()); ++i) {
        if (icons[static_cast<std::size_t>(i)])
            icons[static_cast<std::size_t>(i)]->setPlaytimeBadge(
                playtimeBadgeFor(m_model.at(i)));
    }
}

#if 0 // Replaced by the 1.2.0 folder/widget-aware implementation below.
void WiiUMenuApp::saveMenuLayout() {
    std::error_code ec;
    std::filesystem::create_directory("sdmc:/config", ec);
    ec.clear();
    std::filesystem::create_directory("sdmc:/config/SwitchU", ec);

    nlohmann::json j;
    j["version"] = 1;
    j["slots"] = nlohmann::json::array();
    for (uint64_t tid : m_layoutSlots) {
        if (tid == 0)
            j["slots"].push_back("0");
        else
            j["slots"].push_back(titleIdToHex(tid));
    }

    std::ofstream f(kLayoutPath, std::ios::trunc);
    if (!f.is_open())
        return;
    f << j.dump(2);
    f.close();
    switchu::commitSdCard("layout");
    m_layoutDirty = false;
}

void WiiUMenuApp::applyMenuLayoutToPending(std::vector<PendingApp>& apps) {
    // Only the custom view consumes the saved slot arrangement. This lets a
    // refresh keep the complete list available to the A-Z and recent views.
    if (m_config.sortMode != 0)
        return;

    const int cols = std::clamp(m_config.gridColumns, 3, 8);
    const int rows = std::clamp(m_config.gridRows, 2, 5);
    const int perPage = std::max(1, cols * rows);

    std::unordered_map<uint64_t, PendingApp> byId;
    byId.reserve(apps.size());
    for (auto& app : apps) {
        if (app.titleId != 0)
            byId.emplace(app.titleId, std::move(app));
    }

    std::vector<uint64_t> slots = m_layoutSlots;
    if (slots.empty()) {
        slots.reserve(apps.size());
        for (const auto& app : apps)
            if (app.titleId != 0)
                slots.push_back(app.titleId);
    }

    std::unordered_set<uint64_t> placed;
    placed.reserve(byId.size());
    for (auto& slotTid : slots) {
        if (slotTid == 0)
            continue;
        auto it = byId.find(slotTid);
        if (it == byId.end() || placed.count(slotTid)) {
            slotTid = 0;
            continue;
        }
        placed.insert(slotTid);
    }

    for (const auto& app : apps) {
        if (app.titleId == 0 || placed.count(app.titleId))
            continue;

        auto emptyIt = std::find(slots.begin(), slots.end(), 0);
        if (emptyIt != slots.end())
            *emptyIt = app.titleId;
        else
            slots.push_back(app.titleId);

        placed.insert(app.titleId);
    }

    const int minimum = perPage * (m_config.dynamicPages ? 1 : kMinHomePages);
    int minSlots = std::max(minimum, (int)slots.size());
    if (m_config.dynamicPages) {
        int lastOccupied = -1;
        for (int i = static_cast<int>(slots.size()) - 1; i >= 0; --i) {
            if (slots[static_cast<size_t>(i)] != 0) {
                lastOccupied = i;
                break;
            }
        }
        const int occupiedPages = lastOccupied >= 0 ? (lastOccupied / perPage + 1) : 1;
        minSlots = std::max(perPage, occupiedPages * perPage);
        if ((int)slots.size() > minSlots)
            slots.resize(minSlots);
    }
    int roundedSlots = ((minSlots + perPage - 1) / perPage) * perPage;
    if ((int)slots.size() < roundedSlots)
        slots.resize(roundedSlots, 0);

    std::vector<PendingApp> ordered;
    ordered.reserve(slots.size());
    for (uint64_t tid : slots) {
        if (tid == 0) {
            ordered.emplace_back();
            continue;
        }
        auto it = byId.find(tid);
        if (it != byId.end()) {
            ordered.push_back(std::move(it->second));
        } else {
            ordered.emplace_back();
        }
    }

    if (slots != m_layoutSlots) {
        m_layoutSlots = std::move(slots);
        m_layoutDirty = true;
    }

    apps = std::move(ordered);
}

#endif

void WiiUMenuApp::loadStaticTextures() {
    DebugLog::log("[init] loadResources: static textures");
    std::string gameCardPath = std::string(SD_ASSETS) + "/icons/gamecard.png";
    if (m_gameCardTex.loadFromFile(app().gpu(), app().renderer(), gameCardPath))
        m_loadedGameCardPath = gameCardPath;

    m_arrowTexLeft.loadFromFile(app().gpu(), app().renderer(),
                                std::string(SD_ASSETS) + "/icons/page_arrow_left.png");
    m_arrowTexRight.loadFromFile(app().gpu(), app().renderer(),
                                 std::string(SD_ASSETS) + "/icons/page_arrow_right.png");
    m_batteryConsoleTex.loadFromFile(app().gpu(), app().renderer(),
                                     std::string(SD_ASSETS) + "/icons/widget_battery_switch.png");
    m_batteryJoyconLeftTex.loadFromFile(app().gpu(), app().renderer(),
                                        std::string(SD_ASSETS) + "/icons/widget_battery_joycon_left.png");
    m_batteryJoyconRightTex.loadFromFile(app().gpu(), app().renderer(),
                                          std::string(SD_ASSETS) + "/icons/widget_battery_joycon_right.png");
    m_batteryControllerTex.loadFromFile(app().gpu(), app().renderer(),
                                         std::string(SD_ASSETS) + "/icons/widget_battery_controller.png");

    m_miiAvatarManager.initialize(app().gpu(), app().renderer(), SD_ASSETS);
    m_plazaDialogueEngine.initialize(SD_ASSETS);
    m_animalesePlayer.initialize(std::string(SD_ASSETS) + "/sounds/animalese");
}

void WiiUMenuApp::buildUserAvatarBar(bool loadImmediately) {
    m_userAvatarButtons.clear();

    if (!m_userAvatarBar) {
        m_userAvatarBar = std::make_shared<nxui::Box>(nxui::Axis::ROW);
        m_userAvatarBar->setMarginTop(17.f);
        m_userAvatarBar->setGap(10.f);
        m_userAvatarBar->setShrink(0.f);
        m_userAvatarBar->setSize(0.f, 56.f);
        m_userAvatarBar->setTag("userAvatarBar");
        m_userAvatarBar->setWireframeEnabled(false);
    } else {
        m_userAvatarBar->clearChildren();
        m_userAvatarBar->setSize(0.f, 56.f);
    }

    m_pendingProfileUids.clear();
    m_pendingProfileIndex = 0;
    if (!loadImmediately && m_fastReturnRequested) {
        auto state = std::make_shared<DeferredProfileList>();
        m_deferredProfileList = state;
        m_deferredProfileListFuture = m_threadPool.submit([state]() {
            AccountUid uids[8] = {};
            s32 count = 0;
            state->result = accountListAllUsers(uids, 8, &count);
            if (R_SUCCEEDED(state->result) && count > 0)
                state->uids.assign(uids, uids + count);
        });
        DebugLog::log("[profiles] account enumeration deferred off fast-return path");
        return;
    }

    AccountUid uids[8] = {};
    s32 count = 0;
    Result rc = accountListAllUsers(uids, 8, &count);
    DebugLog::log("[profiles] accountListAllUsers rc=0x%X count=%d", rc, count);
    if (R_SUCCEEDED(rc) && count > 0)
        m_pendingProfileUids.assign(uids, uids + count);

    if (loadImmediately) {
        while (m_pendingProfileIndex < m_pendingProfileUids.size())
            loadNextUserAvatar();
        appendAddUserButton();
    } else if (!m_pendingProfileUids.empty()) {
        m_deferredProfileFrames = 1;
        DebugLog::log("[profiles] %d avatars deferred until after first frame", count);
    } else {
        appendAddUserButton();
    }
}

void WiiUMenuApp::appendAddUserButton() {
    if (!m_userAvatarBar || m_pendingProfileUids.size() >= 8)
        return;
    if (!m_userAvatarButtons.empty() && m_userAvatarButtons.back()->addUserMode())
        return;

    auto add = std::make_shared<UserAvatarButton>();
    add->setSize(56.f, 56.f);
    add->setMinWidth(56.f);
    add->setMinHeight(56.f);
    add->setShrink(0.f);
    add->setCornerRadius(28.f);
    add->setChromeEnabled(true);
    add->setTheme(&m_theme);
    add->setAddUserMode(true);
    add->setFocusable(true);
    add->setOnActivate([this]() {
        m_audio.playSfx(Sfx::Activate);
#ifdef SWITCHU_MENU
        m_launcher.launchUserCreator();
#endif
    });
    m_userAvatarButtons.push_back(add);
    m_userAvatarBar->addChild(add);

    const float countF = static_cast<float>(m_userAvatarButtons.size());
    m_userAvatarBar->setSize(countF * 56.f + (countF - 1.f) * 10.f, 56.f);
    wireUserAvatarNavigation();
    if (m_topHud)
        m_topHud->layout();
    DebugLog::log("[profiles] add-user tile appended count=%d",
                  static_cast<int>(m_pendingProfileUids.size()));
}

void WiiUMenuApp::wireUserAvatarNavigation() {
    const bool dynamicLine = isCarouselLayout();
    auto returnToGrid = [this]() {
        if (!m_grid || m_navigator.route() != switchu::navigation::Route::Home)
            return;
        if (auto* target = m_grid->focusManager().current())
            focusManager().setFocus(target);
    };
    for (std::size_t i = 0; i < m_userAvatarButtons.size(); ++i) {
        auto* current = m_userAvatarButtons[i].get();
        nxui::Widget* left = current;
        if (i > 0)
            left = m_userAvatarButtons[i - 1].get();
        else if (dynamicLine && !m_sidebar.leftButtons().empty())
            left = m_sidebar.leftButtons().back().get();
        nxui::Widget* right = current;
        if (i + 1 < m_userAvatarButtons.size())
            right = m_userAvatarButtons[i + 1].get();
        else if (m_screenSwapButton)
            right = m_screenSwapButton.get();
        else if (dynamicLine && !m_sidebar.rightButtons().empty())
            right = m_sidebar.rightButtons().front().get();
        current->setCustomNavigation(nxui::FocusDirection::LEFT, left);
        current->setCustomNavigation(nxui::FocusDirection::RIGHT, right);
        current->setCustomNavigation(nxui::FocusDirection::DOWN, nullptr);
        current->removeAction(static_cast<uint64_t>(nxui::Button::DDown));
        current->removeAction(static_cast<uint64_t>(nxui::Button::LStickD));
        current->removeAction(static_cast<uint64_t>(nxui::Button::RStickD));
        current->addDirectionAction(nxui::FocusDirection::DOWN, returnToGrid);
    }
    if (m_screenSwapButton) {
        nxui::Widget* left = m_userAvatarButtons.empty() ? nullptr : m_userAvatarButtons.back().get();
        nxui::Widget* right = nullptr;
        if (m_mediaCenterButton)
            right = m_mediaCenterButton.get();
        else if (dynamicLine && !m_sidebar.rightButtons().empty())
            right = m_sidebar.rightButtons().front().get();

        m_screenSwapButton->setCustomNavigation(nxui::FocusDirection::LEFT, left);
        m_screenSwapButton->setCustomNavigation(nxui::FocusDirection::RIGHT, right);
        m_screenSwapButton->setCustomNavigation(nxui::FocusDirection::DOWN, nullptr);
        m_screenSwapButton->removeAction(static_cast<uint64_t>(nxui::Button::DDown));
        m_screenSwapButton->removeAction(static_cast<uint64_t>(nxui::Button::LStickD));
        m_screenSwapButton->removeAction(static_cast<uint64_t>(nxui::Button::RStickD));
        m_screenSwapButton->addDirectionAction(nxui::FocusDirection::DOWN, returnToGrid);
    }
    if (m_mediaCenterButton) {
        nxui::Widget* left = nullptr;
        if (m_screenSwapButton)
            left = m_screenSwapButton.get();
        else if (!m_userAvatarButtons.empty())
            left = m_userAvatarButtons.back().get();

        nxui::Widget* right = (dynamicLine && !m_sidebar.rightButtons().empty())
            ? m_sidebar.rightButtons().front().get()
            : nullptr;
        m_mediaCenterButton->setCustomNavigation(nxui::FocusDirection::LEFT, left);
        m_mediaCenterButton->setCustomNavigation(nxui::FocusDirection::RIGHT, right);
        m_mediaCenterButton->setCustomNavigation(nxui::FocusDirection::DOWN, nullptr);
        m_mediaCenterButton->removeAction(static_cast<uint64_t>(nxui::Button::DDown));
        m_mediaCenterButton->removeAction(static_cast<uint64_t>(nxui::Button::LStickD));
        m_mediaCenterButton->removeAction(static_cast<uint64_t>(nxui::Button::RStickD));
        m_mediaCenterButton->addDirectionAction(nxui::FocusDirection::DOWN, returnToGrid);
    }
    if (dynamicLine && !m_userAvatarButtons.empty()) {
        if (m_grid)
            m_grid->setDynamicLineUpTarget(
                m_userAvatarButtons[m_userAvatarButtons.size() / 2].get());
        m_sidebar.setDynamicLineUpTarget(
            m_userAvatarButtons[m_userAvatarButtons.size() / 2].get());
        m_sidebar.setDynamicLineProfileTargets(m_userAvatarButtons.front().get(),
                                               m_userAvatarButtons.back().get());
    } else {
        m_sidebar.setDynamicLineProfileTargets(nullptr, nullptr);
    }
}

void WiiUMenuApp::loadNextUserAvatar() {
    if (!m_userAvatarBar || m_pendingProfileIndex >= m_pendingProfileUids.size())
        return;

    const AccountUid uid = m_pendingProfileUids[m_pendingProfileIndex++];
    AccountProfile profile{};
    Result rc = accountGetProfile(&profile, uid);
    if (R_FAILED(rc))
        return;

    auto avatar = std::make_shared<UserAvatarButton>();
    avatar->setSize(56.f, 56.f);
    avatar->setMinWidth(56.f);
    avatar->setMinHeight(56.f);
    avatar->setShrink(0.f);
    avatar->setCornerRadius(28.f);
    avatar->setChromeEnabled(true);
    avatar->setTheme(&m_theme);
    avatar->setUid(uid);
    avatar->setFocusable(true);

    AccountProfileBase base{};
    AccountUserData userData{};
    if (R_SUCCEEDED(accountProfileGet(&profile, &userData, &base)))
        avatar->setNickname(base.nickname);

    u32 imgSize = 0;
    if (R_SUCCEEDED(accountProfileGetImageSize(&profile, &imgSize)) && imgSize > 0) {
        std::vector<uint8_t> imgBuf(imgSize);
        u32 realSize = 0;
        if (R_SUCCEEDED(accountProfileLoadImage(&profile, imgBuf.data(), imgSize, &realSize))
                && realSize > 0) {
            avatar->loadAvatar(app().gpu(), app().renderer(), imgBuf.data(), realSize);
        }
    }

    avatar->setOnActivate([this, uid]() {
        m_audio.playSfx(Sfx::Activate);
#ifdef SWITCHU_MENU
        m_launcher.launchUserPage(uid);
#endif
    });

    accountProfileClose(&profile);
    m_userAvatarButtons.push_back(avatar);
    m_userAvatarBar->addChild(avatar);

    if (!m_userAvatarButtons.empty()) {
        const float countF = static_cast<float>(m_userAvatarButtons.size());
        m_userAvatarBar->setSize(countF * 56.f + (countF - 1.f) * 10.f, 56.f);
        wireUserAvatarNavigation();
    }

    if (m_topHud)
        m_topHud->layout();

    if (m_pendingProfileIndex >= m_pendingProfileUids.size())
        appendAddUserButton();
}

WiiUMenuApp::GridLayoutMetrics WiiUMenuApp::computeGridLayoutMetrics() const {
    const int cols = std::clamp(m_config.gridColumns, 3, 8);
    const int rows = std::clamp(m_config.gridRows, 2, 5);
    return computeGridLayoutMetrics(cols, rows);
}

WiiUMenuApp::GridLayoutMetrics WiiUMenuApp::computeGridLayoutMetrics(int cols,
                                                                      int rows) const {
    cols = std::clamp(cols, 3, 8);
    rows = std::clamp(rows, 2, 5);

    const float baseGridW = cols * kGridBaseCellW + (cols - 1) * kGridBasePadX;
    const float baseGridH = rows * kGridBaseCellH + (rows - 1) * kGridBasePadY;

    const float safeW = std::max(1.f, kGridRectW - (kGridSafeSideMargin * 2.f));
    const float safeH = std::max(1.f, kGridRectH - (kGridSafeTopBottomMargin * 2.f));

    const float scaleW = safeW / baseGridW;
    const float scaleH = safeH / baseGridH;
    const float scale = std::min(1.f, std::min(scaleW, scaleH));

    GridLayoutMetrics m;
    m.cellW = std::max(88.f, kGridBaseCellW * scale);
    m.cellH = std::max(88.f, kGridBaseCellH * scale);
    m.padX = std::max(8.f, kGridBasePadX * scale);
    m.padY = std::max(8.f, kGridBasePadY * scale);
    return m;
}

std::pair<int, int> WiiUMenuApp::folderGridDimensions(std::uint32_t folderId) const {
    const auto* folder = m_folderStore.find(folderId);
    const int size = folder ? std::clamp(folder->sizeIndex, 0, 2) : 1;
    switch (size) {
        case 0: return {4, 2};
        case 2: return {6, 4};
        default: return {5, 3};
    }
}

void WiiUMenuApp::reflowHomeGrid() {
    if (!m_grid)
        return;

    // Folder-aware models include synthetic entries which must never be sent
    // to the icon loader as application title IDs. Rebuild them through the
    // same composition path used at startup when grid dimensions change.
    if (!m_allApps.empty() && m_openFolderId == 0) {
        std::uint64_t focused = 0;
        if (auto* current = m_grid->focusManager().current();
            current && current->tag() == "glossy_icon")
            focused = static_cast<GlossyIcon*>(current)->titleId();
        applyDisplayModel(buildRootFolderModel(), focused, false);
        if (m_layoutDirty) saveMenuLayout();
        return;
    }

    const int oldFocusedIndex = m_grid->focusedGlobalIndex();
    const int oldPage = m_grid->currentPage();
    uint64_t focusedTitleId = 0;
    if (oldFocusedIndex >= 0 && oldFocusedIndex < m_model.count())
        focusedTitleId = m_model.at(oldFocusedIndex).titleId;

    std::unordered_map<uint64_t, AppEntry> byId;
    std::vector<uint64_t> appOrder;
    byId.reserve((size_t)std::max(0, m_model.count()));
    appOrder.reserve((size_t)std::max(0, m_model.count()));
    for (const auto& entry : m_model.entries()) {
        if (entry.titleId == 0 || byId.count(entry.titleId))
            continue;
        appOrder.push_back(entry.titleId);
        byId.emplace(entry.titleId, entry);
    }

    std::vector<uint64_t> slots = m_layoutSlots;
    if (slots.empty())
        slots = appOrder;

    std::unordered_set<uint64_t> placed;
    placed.reserve(byId.size());
    for (auto& slotTid : slots) {
        if (slotTid == 0)
            continue;
        if (!byId.count(slotTid) || placed.count(slotTid)) {
            slotTid = 0;
            continue;
        }
        placed.insert(slotTid);
    }

    std::vector<bool> covered = layoutSpanCoverage(slots);
    for (uint64_t tid : appOrder) {
        if (placed.count(tid))
            continue;
        claimFreeLayoutSlot(slots, covered, tid);
        placed.insert(tid);
    }

    const int cols = std::clamp(m_config.gridColumns, 3, 8);
    const int rows = std::clamp(m_config.gridRows, 2, 5);
    const int perPage = std::max(1, cols * rows);
    const int minimum = perPage * (m_config.dynamicPages ? 1 : kMinHomePages);
    int minSlots = std::max(minimum, (int)slots.size());
    if (m_config.dynamicPages) {
        int lastOccupied = -1;
        for (int i = static_cast<int>(slots.size()) - 1; i >= 0; --i) {
            if (slots[static_cast<size_t>(i)] != 0) {
                lastOccupied = i;
                break;
            }
        }
        const int occupiedPages = lastOccupied >= 0 ? (lastOccupied / perPage + 1) : 1;
        minSlots = std::max(perPage, occupiedPages * perPage);
        if ((int)slots.size() > minSlots)
            slots.resize(minSlots);
    }
    int roundedSlots = ((minSlots + perPage - 1) / perPage) * perPage;
    if ((int)slots.size() < roundedSlots)
        slots.resize(roundedSlots, 0);

    // Never from inside a folder. This path rebuilds the saved arrangement out
    // of m_model, which holds the folder's contents while one is open, so it
    // would drop every home entry that is not in the folder -- the second half
    // of the same defect the sort shortcut guard above prevents. Belt and
    // braces: the shortcut is the only caller that could arrive here that way,
    // and the saved layout is not something to lose to a third route.
    if (m_openFolderId == 0 && slots != m_layoutSlots) {
        m_layoutSlots = slots;
        m_layoutDirty = true;
    }

    GridModel rebuiltModel;
    for (uint64_t tid : slots) {
        if (tid == 0) {
            rebuiltModel.addEntry(AppEntry{});
            continue;
        }

        auto it = byId.find(tid);
        if (it != byId.end())
            rebuiltModel.addEntry(it->second);
        else
            rebuiltModel.addEntry(AppEntry{});
    }

    std::vector<std::shared_ptr<GlossyIcon>> icons;
    icons.reserve((size_t)std::max(0, rebuiltModel.count()));
    const auto& oldIcons = m_grid->allIcons();
    for (int i = 0; i < rebuiltModel.count(); ++i) {
        if (i < (int)oldIcons.size() && oldIcons[i] &&
            i < m_model.count() &&
            m_model.at(i).titleId == rebuiltModel.at(i).titleId) {
            icons.push_back(oldIcons[i]);
        } else {
            auto icon = makeIcon(rebuiltModel.at(i));
            icon->setBaseColor(m_theme.iconDefault);
            icons.push_back(std::move(icon));
        }
    }

    m_model = std::move(rebuiltModel);
    m_iconStreamer.setIconDataLoader(AppListLoader::loadIconData);
    std::vector<uint64_t> reflowedTitleIds;
    reflowedTitleIds.reserve((size_t)std::max(0, m_model.count()));
    for (int i = 0; i < m_model.count(); ++i)
        reflowedTitleIds.push_back(m_model.at(i).titleId);
    m_iconStreamer.reconcileTitleIds(reflowedTitleIds);

    GridLayoutMetrics gridMetrics = computeGridLayoutMetrics();
    m_grid->setup(std::move(icons), cols, rows,
                  gridMetrics.cellW, gridMetrics.cellH,
                  gridMetrics.padX, gridMetrics.padY);

    int targetIndex = -1;
    if (focusedTitleId != 0)
        targetIndex = findTitleIndex(focusedTitleId);
    if (targetIndex < 0 && oldFocusedIndex >= 0 && m_model.count() > 0)
        targetIndex = std::clamp(oldFocusedIndex, 0, m_model.count() - 1);

    if (targetIndex >= 0)
        m_grid->focusGlobalIndex(targetIndex);
    else
        m_grid->setPage(oldPage);

    for (auto* icon : m_grid->pageIcons()) {
        if (icon)
            icon->forceVisible();
    }

    m_iconStreamer.onPageChanged(m_grid->currentPage(), m_grid->iconsPerPage(),
                                 app().gpu(), app().renderer(),
                                 m_grid->allIcons());

    const bool overlayActive =
        (m_dialog && m_dialog->isActive()) ||
        (m_themeShop && m_themeShop->isActive()) ||
        (m_settings && m_settings->isActive()) ||
        (m_userSelect && m_userSelect->isActive());
    if (!overlayActive) {
        if (auto* cur = m_grid->focusManager().current())
            focusManager().setFocus(cur);
        updateCursor();
    }

    DebugLog::log("[grid] reflowed layout cols=%d rows=%d apps=%d page=%d",
                  cols, rows, m_model.count(), m_grid->currentPage());
}

void WiiUMenuApp::loadMenuLayout() {
    m_layoutSlots.clear();
    m_gameSizes.clear();

    std::ifstream f(kLayoutPath);
    if (!f.is_open())
        return;

    nlohmann::json j;
    try {
        f >> j;
    } catch (...) {
        return;
    }

    if (const auto sizes = j.find("gameSizes");
        sizes != j.end() && sizes->is_array()) {
        for (const auto& item : *sizes) {
            if (!item.is_object()) continue;
            std::uint64_t titleId = 0;
            const std::string encoded = item.value("titleId", std::string());
            if (!hexToTitleId(encoded, titleId) || titleId == 0) continue;
            const int columns = item.value("columns", 1);
            const int rows = item.value("rows", 1);
            if ((columns == 1 && rows == 1) ||
                (columns == 2 && (rows == 1 || rows == 2)))
                m_gameSizes[titleId] = {columns, rows};
        }
    }

    auto it = j.find("slots");
    if (it == j.end() || !it->is_array())
        return;

    for (const auto& v : *it) {
        uint64_t tid = 0;
        if (v.is_string()) {
            std::string s = v.get<std::string>();
            if (!hexToTitleId(s, tid))
                tid = 0;
        } else if (v.is_number_unsigned()) {
            tid = v.get<uint64_t>();
        } else if (v.is_number_integer()) {
            auto raw = v.get<int64_t>();
            tid = raw > 0 ? (uint64_t)raw : 0;
        }
        m_layoutSlots.push_back(tid);
    }
}

void WiiUMenuApp::saveMenuLayout() {
    std::error_code ec;
    std::filesystem::create_directory("sdmc:/config", ec);
    ec.clear();
    std::filesystem::create_directory("sdmc:/config/SwitchU", ec);

    nlohmann::json j;
    j["version"] = 1;
    j["slots"] = nlohmann::json::array();
    for (uint64_t tid : m_layoutSlots) {
        if (tid == 0)
            j["slots"].push_back("0");
        else
            j["slots"].push_back(titleIdToHex(tid));
    }
    j["gameSizes"] = nlohmann::json::array();
    for (const auto& [titleId, size] : m_gameSizes) {
        if (size == switchu::widgets::WidgetSize{1, 1}) continue;
        j["gameSizes"].push_back({
            {"titleId", titleIdToHex(titleId)},
            {"columns", size.columns},
            {"rows", size.rows},
        });
    }

    std::ofstream f(kLayoutPath, std::ios::trunc);
    if (!f.is_open())
        return;
    f << j.dump(2);
    m_layoutDirty = false;
}

switchu::widgets::WidgetSize WiiUMenuApp::gameGridSize(
    std::uint64_t titleId, AppLayoutMode mode) const {
    // Neither carousel view honours multi-cell sizes: both are one row of
    // equal items.
    if (mode == AppLayoutMode::DynamicLine || mode == AppLayoutMode::Flow)
        return {1, 1};
    const auto found = m_gameSizes.find(titleId);
    if (found == m_gameSizes.end()) return {1, 1};
    const auto size = found->second;
    if ((size.columns == 2 && (size.rows == 1 || size.rows == 2)) ||
        (size.columns == 1 && size.rows == 1))
        return size;
    return {1, 1};
}

void WiiUMenuApp::applyMenuLayoutToPending(std::vector<PendingApp>& apps) {
    composeRootPending(apps);
}

void WiiUMenuApp::composeRootPending(std::vector<PendingApp>& apps) {
    // The one place a chosen name is applied. Everything downstream -- the grid
    // label, the title pill, the A-Z order, folders, the dossier, the widgets --
    // reads these entries, so applying it once here keeps the name from
    // disagreeing with itself in one of them.
    for (auto& pending : apps) {
        if (pending.titleId == 0 || !m_config.hasCustomTitle(pending.titleId))
            continue;
        const std::string chosen = m_config.customTitle(pending.titleId, pending.title);
        // The artwork lookup searches englishTitle, and a title with no usable
        // name of its own carries the hex id there too -- which never matches
        // anything. A name the owner typed is a better search term than that.
        if (pending.englishTitle.empty() || pending.englishTitle == pending.title)
            pending.englishTitle = chosen;
        pending.title = chosen;
    }

    const int cols = std::clamp(m_config.gridColumns, 3, 8);
    const int rows = std::clamp(m_config.gridRows, 2, 5);
    const int perPage = std::max(1, cols * rows);

    m_allApps.clear();
    m_allApps.reserve(apps.size());
    for (const auto& pending : apps) {
        if (pending.titleId == 0)
            continue;
        AppEntry entry;
        entry.id = pending.id;
        entry.title = pending.title;
        entry.englishTitle = pending.englishTitle;
        entry.titleId = pending.titleId;
        entry.viewFlags = pending.viewFlags;
        entry.userRequired = pending.userRequired;
        entry.startupUserKnown = pending.startupUserKnown;
        entry.startupUserAccount = pending.startupUserAccount;
        entry.startupUserAccountOption = pending.startupUserAccountOption;
        entry.kind = GridEntryKind::Application;
        const auto gameSize = gameGridSize(entry.titleId, m_appLayoutMode);
        entry.widgetColumns = gameSize.columns;
        entry.widgetRows = gameSize.rows;
        m_allApps.push_back(std::move(entry));
    }
    normalizeWidgetPlacements();

    std::unordered_map<uint64_t, PendingApp> byId;
    byId.reserve(apps.size() + m_folderStore.all().size() + m_widgetStore.all().size());
    std::vector<uint64_t> itemOrder;
    itemOrder.reserve(apps.size() + m_folderStore.all().size() + m_widgetStore.all().size());
    for (auto& app : apps) {
        if (app.titleId != 0 && m_folderStore.folderForTitle(app.titleId) == 0) {
            const auto gameSize = gameGridSize(app.titleId, m_appLayoutMode);
            app.widgetColumns = gameSize.columns;
            app.widgetRows = gameSize.rows;
            itemOrder.push_back(app.titleId);
            byId.emplace(app.titleId, std::move(app));
        }
    }
    for (const auto& folder : m_folderStore.all()) {
        PendingApp item;
        item.id = "folder:" + std::to_string(folder.id);
        item.title = folder.name;
        item.titleId = folderTitleId(folder.id);
        item.kind = GridEntryKind::Folder;
        item.folderId = folder.id;
        item.folderPreviewCount = static_cast<int>(folder.titleCount());
        item.folderColorIndex = folder.colorIndex;
        itemOrder.push_back(item.titleId);
        byId.emplace(item.titleId, std::move(item));
    }
    for (const auto& widget : m_widgetStore.all()) {
        const auto supported = switchu::widgets::supportedSizes(
            widget.type, m_appLayoutMode);
        if (supported.empty())
            continue;
        PendingApp item;
        item.id = "widget:" + std::to_string(widget.id);
        item.title = widgetTypeLabel(widget.type);
        item.titleId = switchu::widgets::widgetTitleId(widget.id);
        item.kind = GridEntryKind::Widget;
        item.widgetId = widget.id;
        item.widgetType = widget.type;
        const auto effectiveSize = switchu::widgets::validatedSize(
            widget.type, widget.size, m_appLayoutMode);
        item.widgetColumns = effectiveSize.columns;
        item.widgetRows = effectiveSize.rows;
        item.widgetAssetRef = widget.assetRef;
        itemOrder.push_back(item.titleId);
        byId.emplace(item.titleId, std::move(item));
    }

    std::vector<uint64_t> slots = m_layoutSlots;
    if (slots.empty()) {
        slots = itemOrder;
    }

    std::unordered_set<uint64_t> placed;
    placed.reserve(byId.size());
    std::unordered_set<std::uint64_t> hiddenWidgetIds;
    for (const auto& widget : m_widgetStore.all()) {
        if (switchu::widgets::supportedSizes(widget.type, m_appLayoutMode).empty())
            hiddenWidgetIds.insert(switchu::widgets::widgetTitleId(widget.id));
    }
    for (auto& slotTid : slots) {
        if (slotTid == 0)
            continue;
        if (hiddenWidgetIds.count(slotTid)) {
            placed.insert(slotTid);
            continue;
        }
        auto it = byId.find(slotTid);
        if (it == byId.end() || placed.count(slotTid)) {
            slotTid = 0;
            continue;
        }
        placed.insert(slotTid);
    }

    std::vector<bool> covered = layoutSpanCoverage(slots);
    for (uint64_t itemId : itemOrder) {
        if (itemId == 0 || placed.count(itemId))
            continue;
        claimFreeLayoutSlot(slots, covered, itemId);
        placed.insert(itemId);
    }

    const int minimum = perPage * (m_config.dynamicPages ? 1 : kMinHomePages);
    int minSlots = std::max(minimum, (int)slots.size());
    if (m_config.dynamicPages) {
        int lastOccupied = -1;
        for (int i = static_cast<int>(slots.size()) - 1; i >= 0; --i) {
            if (slots[static_cast<size_t>(i)] != 0) {
                lastOccupied = i;
                break;
            }
        }
        const int occupiedPages = lastOccupied >= 0 ? (lastOccupied / perPage + 1) : 1;
        minSlots = std::max(perPage, occupiedPages * perPage);
        if ((int)slots.size() > minSlots)
            slots.resize(minSlots);
    }
    int roundedSlots = ((minSlots + perPage - 1) / perPage) * perPage;
    if ((int)slots.size() < roundedSlots)
        slots.resize(roundedSlots, 0);

    std::vector<int> coveredBy(slots.size(), -1);
    if (m_appLayoutMode == AppLayoutMode::Grid) {
        for (int index = 0; index < static_cast<int>(slots.size()); ++index) {
            auto found = byId.find(slots[static_cast<std::size_t>(index)]);
            if (found == byId.end() ||
                (found->second.kind != GridEntryKind::Widget &&
                 found->second.kind != GridEntryKind::Application))
                continue;
            auto& item = found->second;
            int spanColumns = std::max(1, item.widgetColumns);
            int spanRows = std::max(1, item.widgetRows);
            if (spanColumns == 1 && spanRows == 1)
                continue;
            const int pageOffset = index % perPage;
            const int column = pageOffset % cols;
            const int row = pageOffset / cols;
            bool fits = column + spanColumns <= cols && row + spanRows <= rows;
            for (int dy = 0; fits && dy < spanRows; ++dy) {
                for (int dx = 0; dx < spanColumns; ++dx) {
                    const int cell = index + dy * cols + dx;
                    if (cell >= static_cast<int>(slots.size()) ||
                        (cell != index && slots[static_cast<std::size_t>(cell)] != 0) ||
                        coveredBy[static_cast<std::size_t>(cell)] >= 0) {
                        fits = false;
                        break;
                    }
                }
            }
            if (!fits) {
                // Placement normalization normally relocates a large item.
                // Keep a safe 1x1 fallback for malformed legacy layouts.
                item.widgetColumns = 1;
                item.widgetRows = 1;
                coveredBy[static_cast<std::size_t>(index)] = index;
                continue;
            }
            for (int dy = 0; dy < spanRows; ++dy)
                for (int dx = 0; dx < spanColumns; ++dx)
                    coveredBy[static_cast<std::size_t>(index + dy * cols + dx)] = index;
        }
    }

    std::vector<PendingApp> ordered;
    ordered.reserve(slots.size());
    for (int index = 0; index < static_cast<int>(slots.size()); ++index) {
        if (coveredBy[static_cast<std::size_t>(index)] >= 0 &&
            coveredBy[static_cast<std::size_t>(index)] != index) {
            PendingApp continuation;
            continuation.kind = GridEntryKind::WidgetContinuation;
            ordered.push_back(std::move(continuation));
            continue;
        }
        const std::uint64_t tid = slots[static_cast<std::size_t>(index)];
        if (hiddenWidgetIds.count(tid))
            continue;
        if (tid == 0) {
            PendingApp empty;
            empty.kind = GridEntryKind::Empty;
            ordered.push_back(std::move(empty));
            continue;
        }
        auto it = byId.find(tid);
        if (it != byId.end()) {
            ordered.push_back(std::move(it->second));
        } else {
            PendingApp empty;
            empty.kind = GridEntryKind::Empty;
            ordered.push_back(std::move(empty));
        }
    }

    if (slots != m_layoutSlots) {
        m_layoutSlots = std::move(slots);
        m_layoutDirty = true;
    }

    apps = std::move(ordered);
}

GridModel WiiUMenuApp::buildRootFolderModel() {
    normalizeWidgetPlacements();
    // Folders store title ids, not entries, so a member that has gone -- deleted
    // here, or installed on another card -- stays a member forever. The tile
    // then draws a preview cell with no icon behind it, which is the flat
    // coloured square. Pruning against the live catalogue heals the folders
    // already carrying one, which removing it at delete time cannot do.
    {
        bool prunedAny = false;
        for (const auto& folder : m_folderStore.all()) {
            std::vector<std::uint64_t> gone;
            for (std::uint64_t titleId : folder.titleIds) {
                const bool known = std::any_of(
                    m_allApps.begin(), m_allApps.end(),
                    [titleId](const AppEntry& app) { return app.titleId == titleId; });
                if (!known) gone.push_back(titleId);
            }
            for (std::uint64_t titleId : gone) {
                DebugLog::log("[folders] pruning missing title %016llX from folder %u",
                              static_cast<unsigned long long>(titleId), folder.id);
                m_folderStore.removeTitle(folder.id, titleId);
                prunedAny = true;
            }
        }
        if (prunedAny)
            m_folderStore.save();
    }
    GridModel model;
    std::unordered_map<std::uint64_t, AppEntry> entries;
    for (const auto& app : m_allApps) {
        if (m_folderStore.folderForTitle(app.titleId) == 0) {
            AppEntry effective = app;
            effective.isFavorite = m_config.isFavorite(app.titleId);
            const auto size = gameGridSize(app.titleId, m_appLayoutMode);
            effective.widgetColumns = size.columns;
            effective.widgetRows = size.rows;
            entries.emplace(effective.titleId, std::move(effective));
        }
    }
    for (const auto& folder : m_folderStore.all()) {
        AppEntry entry;
        entry.id = "folder:" + std::to_string(folder.id);
        entry.title = folder.name;
        entry.titleId = folderTitleId(folder.id);
        entry.kind = GridEntryKind::Folder;
        entry.folderId = folder.id;
        entry.folderPreviewCount = static_cast<int>(folder.titleCount());
        entry.folderColorIndex = folder.colorIndex;
        entries.emplace(entry.titleId, std::move(entry));
    }
    for (const auto& widget : m_widgetStore.all()) {
        if (switchu::widgets::supportedSizes(widget.type, m_appLayoutMode).empty())
            continue;
        AppEntry entry;
        entry.id = "widget:" + std::to_string(widget.id);
        entry.title = widgetTypeLabel(widget.type);
        entry.titleId = switchu::widgets::widgetTitleId(widget.id);
        entry.kind = GridEntryKind::Widget;
        entry.widgetId = widget.id;
        entry.widgetType = widget.type;
        const auto size = switchu::widgets::validatedSize(
            widget.type, widget.size, m_appLayoutMode);
        entry.widgetColumns = size.columns;
        entry.widgetRows = size.rows;
        entry.widgetAssetRef = widget.assetRef;
        entries.emplace(entry.titleId, std::move(entry));
    }

    const int perPage = std::max(1, std::clamp(m_config.gridColumns, 3, 8) *
                                     std::clamp(m_config.gridRows, 2, 5));
    if (m_layoutSlots.empty()) {
        for (const auto& app : m_allApps)
            if (entries.count(app.titleId)) m_layoutSlots.push_back(app.titleId);
        for (const auto& folder : m_folderStore.all())
            m_layoutSlots.push_back(folderTitleId(folder.id));
        for (const auto& widget : m_widgetStore.all())
            m_layoutSlots.push_back(switchu::widgets::widgetTitleId(widget.id));
    }
    std::vector<bool> covered = layoutSpanCoverage(m_layoutSlots);
    for (const auto& pair : entries) {
        if (std::find(m_layoutSlots.begin(), m_layoutSlots.end(), pair.first) ==
            m_layoutSlots.end()) {
            claimFreeLayoutSlot(m_layoutSlots, covered, pair.first);
            m_layoutDirty = true;
        }
    }
    const int minimum = perPage * (m_config.dynamicPages ? 1 : kMinHomePages);
    const int rounded = ((std::max(minimum, static_cast<int>(m_layoutSlots.size())) + perPage - 1) / perPage) * perPage;
    if (static_cast<int>(m_layoutSlots.size()) < rounded) {
        m_layoutSlots.resize(rounded, 0);
        m_layoutDirty = true;
    }

    // R cycles the home view. The 1.2 builder read m_layoutSlots directly, so
    // the hint label changed to A-Z or Recent while every icon stayed where it
    // was. The automatic views are temporary projections: m_layoutSlots keeps
    // the hand-made arrangement on disk untouched, so returning to My order
    // restores exactly what the owner built instead of an approximation.
    //
    // Folders and widgets keep their slots. Sorting them alongside applications
    // moved a 2x1 tile to a position whose second cell was already taken, and
    // the fit check below then degraded it to 1x1: the home grid came back from
    // an A-Z or Recent switch with every wide widget shrunk and rearranged.
    std::vector<std::uint64_t> projected;
    if (m_config.sortMode != 0) {
        const int columns = std::clamp(m_config.gridColumns, 3, 8);
        projected.assign(m_layoutSlots.size(), 0);
        std::vector<bool> reserved(m_layoutSlots.size(), false);

        for (std::size_t index = 0; index < m_layoutSlots.size(); ++index) {
            const std::uint64_t stored = m_layoutSlots[index];
            if (stored == 0)
                continue;
            const auto found = entries.find(stored);
            if (found == entries.end() || found->second.isApplication())
                continue;
            projected[index] = stored;
            // Reserve every cell the tile spans, so a sorted application cannot
            // be dropped into the second half of a 2x1 folder or widget.
            const int spanColumns = std::max(1, found->second.widgetColumns);
            const int spanRows = std::max(1, found->second.widgetRows);
            for (int dy = 0; dy < spanRows; ++dy) {
                for (int dx = 0; dx < spanColumns; ++dx) {
                    const std::size_t cell =
                        index + static_cast<std::size_t>(dy * columns + dx);
                    if (cell < reserved.size())
                        reserved[cell] = true;
                }
            }
        }

        // Rank by the personal arrangement first so the sort below, being
        // stable, keeps it wherever the sort key ties.
        std::unordered_map<std::uint64_t, int> personalRank;
        personalRank.reserve(entries.size());
        int rank = 0;
        for (std::uint64_t stored : m_layoutSlots)
            if (stored != 0 && personalRank.find(stored) == personalRank.end())
                personalRank.emplace(stored, rank++);

        std::vector<std::uint64_t> apps;
        apps.reserve(entries.size());
        for (const auto& pair : entries)
            if (pair.second.isApplication())
                apps.push_back(pair.first);
        std::sort(apps.begin(), apps.end(),
                  [&](std::uint64_t a, std::uint64_t b) {
            const auto rankA = personalRank.find(a);
            const auto rankB = personalRank.find(b);
            const int valueA = rankA == personalRank.end()
                ? std::numeric_limits<int>::max() : rankA->second;
            const int valueB = rankB == personalRank.end()
                ? std::numeric_limits<int>::max() : rankB->second;
            if (valueA != valueB) return valueA < valueB;
            return a < b;
        });

        const int mode = m_config.sortMode;
        std::stable_sort(apps.begin(), apps.end(),
                         [&](std::uint64_t a, std::uint64_t b) {
            if (mode == 4) {
                const auto playedA = m_config.playtimeOf(a);
                const auto playedB = m_config.playtimeOf(b);
                // Never-played entries sort last rather than first as a zero
                // duration would; ties retain the owner's personal order.
                if (playedA != playedB) return playedA > playedB;
                return false;
            }
            if (mode == 3) {
                const bool favA = m_config.isFavorite(a);
                const bool favB = m_config.isFavorite(b);
                if (favA != favB) return favA > favB;
                return false;
            }
            if (mode == 2) {
                const auto recentA = m_config.lastOpenedAt(a);
                const auto recentB = m_config.lastOpenedAt(b);
                // Never-opened entries sort last rather than first as a zero
                // timestamp would.
                if (recentA != recentB) return recentA > recentB;
                return false;
            }
            // Compared the way a person reads them, so "apple" and "Apple" land
            // together instead of in two separate blocks of the alphabet.
            const std::string& labelA = entries.at(a).title;
            const std::string& labelB = entries.at(b).title;
            const std::size_t shared = std::min(labelA.size(), labelB.size());
            for (std::size_t i = 0; i < shared; ++i) {
                const unsigned char charA = static_cast<unsigned char>(
                    std::tolower(static_cast<unsigned char>(labelA[i])));
                const unsigned char charB = static_cast<unsigned char>(
                    std::tolower(static_cast<unsigned char>(labelB[i])));
                if (charA != charB) return charA < charB;
            }
            return labelA.size() < labelB.size();
        });

        std::size_t next = 0;
        for (std::size_t index = 0; index < projected.size() && next < apps.size();
             ++index) {
            if (reserved[index])
                continue;
            projected[index] = apps[next++];
        }
        // More applications than free cells only happens if the saved layout is
        // shorter than the catalogue; they go on the end rather than vanish.
        for (; next < apps.size(); ++next)
            projected.push_back(apps[next]);
        DebugLog::log("[grid] sort=%d projected=%zu apps=%zu", mode,
                      projected.size(), apps.size());
    }
    const std::vector<std::uint64_t>& slots =
        m_config.sortMode == 0 ? m_layoutSlots : projected;

    const int columns = std::clamp(m_config.gridColumns, 3, 8);
    const int rows = std::clamp(m_config.gridRows, 2, 5);
    std::vector<int> coveredBy(slots.size(), -1);
    if (m_appLayoutMode == AppLayoutMode::Grid) {
        for (int index = 0; index < static_cast<int>(slots.size()); ++index) {
            auto found = entries.find(slots[static_cast<std::size_t>(index)]);
            if (found == entries.end() ||
                (!found->second.isWidget() && !found->second.isApplication()))
                continue;
            auto& entry = found->second;
            const int spanColumns = std::max(1, entry.widgetColumns);
            const int spanRows = std::max(1, entry.widgetRows);
            if (spanColumns == 1 && spanRows == 1) continue;
            const int local = index % perPage;
            const int column = local % columns;
            const int row = local / columns;
            bool fits = column + spanColumns <= columns && row + spanRows <= rows;
            for (int dy = 0; fits && dy < spanRows; ++dy) {
                for (int dx = 0; dx < spanColumns; ++dx) {
                    const int cell = index + dy * columns + dx;
                    if (cell >= static_cast<int>(slots.size()) ||
                        (cell != index && slots[static_cast<std::size_t>(cell)] != 0) ||
                        coveredBy[static_cast<std::size_t>(cell)] >= 0) {
                        fits = false;
                        break;
                    }
                }
            }
            if (!fits) {
                entry.widgetColumns = 1;
                entry.widgetRows = 1;
                coveredBy[static_cast<std::size_t>(index)] = index;
                continue;
            }
            for (int dy = 0; dy < spanRows; ++dy)
                for (int dx = 0; dx < spanColumns; ++dx)
                    coveredBy[static_cast<std::size_t>(index + dy * columns + dx)] = index;
        }
    }

    for (int index = 0; index < static_cast<int>(slots.size()); ++index) {
        const auto storedTitleId = slots[static_cast<std::size_t>(index)];
        if (switchu::widgets::isWidgetTitleId(storedTitleId)) {
            const auto* widget = m_widgetStore.find(
                switchu::widgets::widgetIdFromTitleId(storedTitleId));
            if (widget && switchu::widgets::supportedSizes(
                    widget->type, m_appLayoutMode).empty())
                continue;
        }
        if (coveredBy[static_cast<std::size_t>(index)] >= 0 &&
            coveredBy[static_cast<std::size_t>(index)] != index) {
            AppEntry continuation;
            continuation.kind = GridEntryKind::WidgetContinuation;
            model.addEntry(std::move(continuation));
            continue;
        }
        const auto titleId = slots[static_cast<std::size_t>(index)];
        auto found = entries.find(titleId);
        if (found != entries.end()) {
            model.addEntry(found->second);
        } else {
            model.addEntry({});
        }
    }
    if (isCarouselLayout())
        return compactDynamicLineEntries(model, m_appLayoutMode == AppLayoutMode::Flow);
    return model;
}

GridModel WiiUMenuApp::buildOpenFolderModel(std::uint32_t folderId) const {
    GridModel model;
    const auto* folder = m_folderStore.find(folderId);
    if (!folder)
        return model;
    const auto [folderCols, folderRows] = folderGridDimensions(folderId);
    const int perPage = std::max(1, folderCols * folderRows);
    const int occupied = (static_cast<int>(folder->titleIds.size()) + perPage - 1) /
                         perPage;
    const int pages = std::clamp(std::max(folder->pageCount, occupied),
                                 1, switchu::folders::kMaxFolderPages);
    std::vector<std::uint64_t> slots = folder->titleIds;
    slots.resize(static_cast<std::size_t>(pages * perPage), 0);
    std::vector<int> coveredBy(slots.size(), -1);
    std::unordered_map<int, AppEntry> anchors;

    for (int index = 0; index < static_cast<int>(slots.size()); ++index) {
        const std::uint64_t titleId = slots[static_cast<std::size_t>(index)];
        if (titleId == 0) continue;
        auto found = std::find_if(m_allApps.begin(), m_allApps.end(),
            [titleId](const AppEntry& app) { return app.titleId == titleId; });
        if (found == m_allApps.end()) {
            DebugLog::log("[folders] missing title ignored folder=%u tid=%016lX",
                          folderId, static_cast<unsigned long>(titleId));
            continue;
        }

        AppEntry entry = *found;
        entry.isFavorite = m_config.isFavorite(titleId);
        const auto size = gameGridSize(titleId, m_appLayoutMode);
        entry.widgetColumns = std::max(1, size.columns);
        entry.widgetRows = std::max(1, size.rows);
        const int local = index % perPage;
        const int column = local % folderCols;
        const int row = local / folderCols;
        bool fits = column + entry.widgetColumns <= folderCols &&
                    row + entry.widgetRows <= folderRows;
        for (int dy = 0; fits && dy < entry.widgetRows; ++dy) {
            for (int dx = 0; dx < entry.widgetColumns; ++dx) {
                const int cell = index + dy * folderCols + dx;
                if (cell >= static_cast<int>(slots.size()) ||
                    (cell != index && slots[static_cast<std::size_t>(cell)] != 0) ||
                    coveredBy[static_cast<std::size_t>(cell)] >= 0) {
                    fits = false;
                    break;
                }
            }
        }
        if (!fits) {
            entry.widgetColumns = 1;
            entry.widgetRows = 1;
        }
        anchors.emplace(index, entry);
        for (int dy = 0; dy < entry.widgetRows; ++dy)
            for (int dx = 0; dx < entry.widgetColumns; ++dx)
                coveredBy[static_cast<std::size_t>(index + dy * folderCols + dx)] = index;
    }

    for (int index = 0; index < static_cast<int>(slots.size()); ++index) {
        if (coveredBy[static_cast<std::size_t>(index)] >= 0 &&
            coveredBy[static_cast<std::size_t>(index)] != index) {
            AppEntry continuation;
            continuation.kind = GridEntryKind::WidgetContinuation;
            model.addEntry(std::move(continuation));
            continue;
        }
        const auto anchor = anchors.find(index);
        if (anchor != anchors.end())
            model.addEntry(anchor->second);
        else
            model.addEntry({});
    }
    if (isCarouselLayout())
        return compactDynamicLineEntries(model, m_appLayoutMode == AppLayoutMode::Flow);
    return model;
}

void WiiUMenuApp::applyDisplayModel(GridModel model, std::uint64_t focusId, bool animate) {
    if (!m_grid)
        return;
    const auto isImageAssetWidget = [](switchu::widgets::WidgetType type) {
        return type == switchu::widgets::WidgetType::ImagePin ||
               type == switchu::widgets::WidgetType::RandomScreenshot;
    };
    // Image widgets own GPU textures, sometimes several animation frames.
    // Preserve their icon object across a reflow/move: destroying it directly
    // after the previous frame was submitted can release image memory still in
    // use by Deko3D and crash the menu.
    const auto previousIcons = m_grid->allIcons();
    for (int i = 0; i < m_model.count() && i < static_cast<int>(previousIcons.size()); ++i) {
        const auto& entry = m_model.at(i);
        if (entry.isWidget() && isImageAssetWidget(entry.widgetType) &&
            entry.titleId != 0 && previousIcons[static_cast<std::size_t>(i)])
            m_retainedImagePins[entry.titleId] = {
                entry.widgetAssetRef,
                previousIcons[static_cast<std::size_t>(i)]->widgetImageAssetPath(),
                previousIcons[static_cast<std::size_t>(i)]};
    }
    m_model = std::move(model);
    // Retention only bridges a reflow/move of the current model. Keeping pins
    // from a closed folder or another page tree would keep every GIF frame
    // alive indefinitely.
    for (auto it = m_retainedImagePins.begin(); it != m_retainedImagePins.end();) {
        bool stillPresent = false;
        for (int i = 0; i < m_model.count(); ++i) {
            const auto& entry = m_model.at(i);
            if (entry.titleId == it->first && entry.isWidget() &&
                isImageAssetWidget(entry.widgetType)) {
                stillPresent = true;
                break;
            }
        }
        if (!stillPresent)
            it = m_retainedImagePins.erase(it);
        else
            ++it;
    }
    std::vector<std::shared_ptr<GlossyIcon>> icons;
    std::vector<std::uint64_t> titleIds;
    icons.reserve(m_model.count());
    titleIds.reserve(m_model.count());
    for (int i = 0; i < m_model.count(); ++i) {
        const auto& entry = m_model.at(i);
        std::shared_ptr<GlossyIcon> icon;
        if (entry.isWidget() && isImageAssetWidget(entry.widgetType)) {
            const auto found = m_retainedImagePins.find(entry.titleId);
            const std::string resolvedPath =
                entry.widgetType == switchu::widgets::WidgetType::RandomScreenshot
                    ? randomScreenshotPath(entry.widgetId)
                    : resolveWidgetAssetRef(entry.widgetAssetRef);
            if (found != m_retainedImagePins.end() &&
                found->second.assetRef == entry.widgetAssetRef &&
                found->second.assetPath == resolvedPath) {
                icon = found->second.icon;
                if (icon)
                    icon->setGridSpan(entry.widgetColumns, entry.widgetRows);
            }
        }
        if (!icon)
            icon = makeIcon(entry);
        icon->setBaseColor(m_theme.iconDefault);
        icon->setBorderColor(m_theme.panelBorder);
        icon->setHighlightColor(m_theme.panelHighlight);
        icon->setCornerRadius(m_theme.iconCornerRadius);
        icon->setLoadingColor(m_theme.cursorNormal);
        icons.push_back(std::move(icon));
        titleIds.push_back(entry.isApplication() ? entry.titleId : 0);
    }
    m_iconStreamer.reconcileTitleIds(titleIds);
    int columns = std::clamp(m_config.gridColumns, 3, 8);
    int rows = std::clamp(m_config.gridRows, 2, 5);
    if (m_openFolderId != 0)
        std::tie(columns, rows) = folderGridDimensions(m_openFolderId);
    auto metrics = computeGridLayoutMetrics(columns, rows);
    if (m_openFolderId != 0) {
        // Reserve a real title band above and a title-pill band below. Scaling
        // the cells, rather than merely shrinking the grid rect, prevents the
        // centered first/last rows from escaping those bands.
        metrics.cellW *= 0.92f;
        metrics.cellH *= 0.92f;
        metrics.padX *= 0.92f;
        metrics.padY *= 0.92f;
    }
    // Inside a folder there is no sidebar to the left or right of the grid, so
    // the edge columns are free to flip the page instead.
    const bool inFolder = (m_openFolderId != 0);
    m_grid->setEdgePaging(inFolder);
    m_grid->setSlideTransition(inFolder);
    m_grid->setLayoutMode(m_appLayoutMode);
    // The line is a ring, so the streamer's window has to wrap with it.
    m_iconStreamer.setRingMode(isCarouselLayout());
    m_grid->setup(std::move(icons), columns, rows, metrics.cellW, metrics.cellH,
                  metrics.padX, metrics.padY);
    applyPlaytimeBadges();
    wireFocusCallback();
    m_grid->onEdgePage([this](int dir) { flipPageFromEdge(dir); });
    m_grid->onEdgePageHold([this](int dir) -> bool {
        if (app().navHoldFrames() < 12)
            return false;
        flipPageFromEdge(dir);
        return true;
    });
    m_grid->onPageSwitched([this]() {
        if (m_editMode && m_editTargetIndex >= 0) {
            const int perPage = std::max(1, m_grid->iconsPerPage());
            const int local = m_editTargetIndex % perPage;
            m_editTargetIndex = m_grid->currentPage() * perPage + local;
            if (m_editTargetIndex >= m_model.count())
                m_editTargetIndex = std::max(0, m_model.count() - 1);
            if (m_editGhostIcon)
                m_editGhostTargetRect = m_grid->gridSpanRect(
                    m_editTargetIndex,
                    m_editGhostIcon->gridSpanColumns(),
                    m_editGhostIcon->gridSpanRows());
        }
        m_iconStreamer.onPageChanged(m_grid->currentPage(), m_grid->iconsPerPage(),
                                     app().gpu(), app().renderer(), m_grid->allIcons());
        if (auto* target = m_grid->focusManager().current())
            focusManager().setFocus(target);
        updateCursor();
    });
    if (!focusTitle(focusId)) {
        if (auto* first = m_grid->focusManager().current())
            focusManager().setFocus(first);
    }
    m_iconStreamer.onPageChanged(m_grid->currentPage(), m_grid->iconsPerPage(),
                                 app().gpu(), app().renderer(), m_grid->allIcons());
    m_widgetAssetPage = -1;
    if (animate) m_grid->startAppearAnimation();
    else for (auto& icon : m_grid->allIcons()) icon->forceVisible();
    updateCursor();
}

// Text entry is drawn by the menu itself. TextEntryScreen carries why the
// system keyboard cannot be used from a library applet.
void WiiUMenuApp::createTextEntry() {
    if (m_textEntry) return;
    m_textEntry = std::make_shared<TextEntryScreen>();
    if (m_overlayLayer) m_overlayLayer->addChild(m_textEntry);
    m_textEntry->setFont(&m_fontNormal);
    m_textEntry->setSmallFont(&m_fontSmall);
    m_textEntry->setTheme(&m_theme);
    m_textEntry->onKeySfx([this]() { m_audio.playSfx(Sfx::Activate); });
    m_textEntry->onNavigateSfx([this]() { m_audio.playSfx(Sfx::Navigate); });
    m_textEntry->onCloseSfx([this]() { m_audio.playSfx(Sfx::ModalHide); });
    m_textEntry->onAccessibilityAnnouncement([this](const std::string& text) {
        m_accessibility.announce(text);
    });
}

void WiiUMenuApp::requestTextEntry(const std::string& title, const std::string& guide,
                                   const std::string& initial, int maxLength,
                                   bool password,
                                   std::function<void(const std::string&)> onAccept,
                                   std::function<void()> onCancel) {
    if (m_contextMenu && m_contextMenu->isActive())
        m_contextMenu->hide(false);
    createTextEntry();
    if (!m_textEntry)
        return;
    // Created in onCreate, before the overlays that can sit above it, so it is
    // moved to the end of the tree before being shown. Same ordering fix the
    // dialogs and the controller test carry.
    raiseOverlay(m_textEntry);
    nxui::Widget* returnFocus = focusManager().current();
    DebugLog::log("[textentry] requestTextEntry title=%s returnFocus=%p", title.c_str(), (void*)returnFocus);
    auto restoreFocus = [this, returnFocus]() {
        nxui::Widget* target = isCurrentFocusableWidget(returnFocus)
            ? returnFocus : nullptr;
        // Whatever had focus may be gone by now, and leaving focus on the closed
        // keyboard is what dragged the selection ring along with it.
        if (!target && m_grid)
            target = m_grid->focusManager().current();
        if (target) {
            m_suppressNextNavigateSfx = true;
            focusManager().setFocus(target);
        }
        // The ring was hidden while the keyboard was up and its target was never
        // moved, so showing it again animated it down from the panel's own rect
        // ÃƒÂ¢Ã¢â€šÂ¬Ã¢â‚¬Â the keyboard-sized selection left behind on close. Snap it instead.
        if (m_cursor && target)
            m_cursor->moveTo(target->focusRect().expanded(4.f), 0.f);
        DebugLog::log("[textentry] restoreFocus target=%p returnFocus=%p m_dialog=%p fellBackToGrid=%d",
                      (void*)target, (void*)returnFocus, (void*)m_dialog.get(),
                      (target && target != returnFocus) ? 1 : 0);
    };
    m_textEntry->onAccept([this, onAccept, restoreFocus](const std::string& value) {
        DebugLog::log("[textentry] onAccept fired value=%s", value.c_str());
        restoreFocus();
        if (onAccept) onAccept(value);
        DebugLog::log("[textentry] onAccept callback returned");
    });

    m_textEntry->onCancel([restoreFocus, onCancel]() {
        restoreFocus();
        if (onCancel) onCancel();
    });

    TextEntryScreen::Request request;
    request.title = title;
    request.guide = guide;
    request.initial = initial;
    request.maxLength = maxLength;
    request.password = password;
    m_audio.playSfx(Sfx::ModalShow);
    m_textEntry->show(request);
    focusManager().setFocus(m_textEntry.get());
    DebugLog::log("[textentry] show() called, focus set to m_textEntry=%p isActive=%d",
                  (void*)m_textEntry.get(), m_textEntry->isActive());
}
// A folder is still worth having when the keyboard cannot be reached, so one is
// created under the first free default name instead of the action doing nothing.
std::string WiiUMenuApp::defaultFolderName() const {
    const std::string base = nxui::I18n::instance().tr("folder.default_name", "Folder");
    auto taken = [this](const std::string& candidate) {
        for (const auto& folder : m_folderStore.all())
            if (folder.name == candidate)
                return true;
        return false;
    };
    if (!taken(base))
        return base;
    for (int suffix = 2; suffix < 1000; ++suffix) {
        const std::string candidate = base + " " + std::to_string(suffix);
        if (!taken(candidate))
            return candidate;
    }
    return base;
}

bool WiiUMenuApp::saveFoldersOrReport(const char* operation) {
    if (m_folderStore.save())
        return true;
    DebugLog::log("[folders] operation failed op=%s", operation ? operation : "unknown");
    m_folderStore.load();
    auto& i18n = nxui::I18n::instance();
    m_dialog->show(i18n.tr("folder.error_title", "Folder error"),
                   i18n.tr("folder.save_error", "The folder change could not be saved."),
                   {{i18n.tr("button.ok", "OK"), {}, true}});
    focusManager().setFocus(m_dialog.get());
    return false;
}

// FolderStore::addTitle() and removeTitle() existed from the 1.2 merge with no
// caller and no way in: a folder could be created but nothing could be put in
// it. Y only ever swapped two icons, because moving is a placement, not a
// membership change. This is the membership change, offered from the dossier.
void WiiUMenuApp::showFolderAssignment(std::uint64_t titleId, const std::string& title) {
#ifdef SWITCHU_MENU
    if (!m_dialog || titleId == 0)
        return;
    auto& i18n = nxui::I18n::instance();
    raiseOverlay(m_dialog);
    // Only return to the dossier if that is where this came from; homebrew
    // reaches here from a plain dialog with no dossier behind it.
    m_dialogReturnFocus = (m_gameDetails && m_gameDetails->isActive())
        ? m_gameDetails.get() : m_dialogReturnFocus;
    m_audio.playSfx(Sfx::ModalShow);

    const std::uint32_t current = m_folderStore.folderForTitle(titleId);
    if (current != 0) {
        const auto* folder = m_folderStore.find(current);
        const std::string name = folder ? folder->name : std::string();
        m_dialog->show(i18n.tr("folder.remove_game", "Remove from folder"),
                       i18n.tr("folder.remove_game_desc",
                               "Move this software back to the HOME menu.") + "\n" + name,
                       {{i18n.tr("button.cancel", "Cancel"), [] {}, true},
                        {i18n.tr("folder.remove_game", "Remove from folder"),
                         [this, titleId]() { removeTitleFromFolder(titleId); }, true}},
                       0, {});
        focusManager().setFocus(m_dialog.get());
        return;
    }

    if (m_folderStore.all().empty()) {
        m_dialog->show(i18n.tr("folder.no_folder_title", "No folder"),
                       i18n.tr("folder.no_folder", "Create a folder first."),
                       {{i18n.tr("button.ok", "OK"), {}, true}}, 0, {});
        focusManager().setFocus(m_dialog.get());
        return;
    }

    // One button per folder. The dialog scrolls, so a long list stays reachable.
    std::vector<OverlayDialog::ButtonDef> buttons;
    buttons.push_back({i18n.tr("button.cancel", "Cancel"), [] {}, true});
    for (const auto& folder : m_folderStore.all()) {
        const std::uint32_t folderId = folder.id;
        buttons.push_back({folder.name,
                           [this, folderId, titleId]() {
                               assignTitleToFolder(folderId, titleId);
                           }, true});
    }
    m_dialog->show(i18n.tr("folder.choose", "Choose a folder"),
                   i18n.tr("folder.add_game_desc", "Choose a folder for this software.")
                       + "\n" + title,
                   std::move(buttons), 0, {});
    focusManager().setFocus(m_dialog.get());
#else
    (void)titleId;
    (void)title;
#endif
}

void WiiUMenuApp::assignTitleToFolder(std::uint32_t folderId, std::uint64_t titleId) {
    if (!m_folderStore.addTitle(folderId, titleId))
        return;
    if (!saveFoldersOrReport("add to folder"))
        return;
    // The tile has to decode again with the new member list.
    m_folderPreviews.clear();
    if (m_folderPreviewDecode) m_folderPreviewDecode->cancelled.store(true);
    m_folderPreviewDecode.reset();
    m_folderPreviewUploadStage = 0;
    m_folderPreviewDecoded = false;
    // The title leaves the home arrangement: its slot frees up and the folder
    // tile is what represents it from now on.
    for (auto& slot : m_layoutSlots)
        if (slot == titleId)
            slot = 0;
    m_layoutDirty = true;
    saveMenuLayout();
    m_audio.playSfx(Sfx::ConfirmPositive);
    applyDisplayModel(buildRootFolderModel(), folderTitleId(folderId), false);
}

void WiiUMenuApp::removeTitleFromFolder(std::uint64_t titleId) {
    const std::uint32_t folderId = m_folderStore.folderForTitle(titleId);
    if (folderId == 0 || !m_folderStore.removeTitle(folderId, titleId))
        return;
    if (!saveFoldersOrReport("remove from folder"))
        return;
    // The tile has to decode again with the new member list.
    m_folderPreviews.clear();
    if (m_folderPreviewDecode) m_folderPreviewDecode->cancelled.store(true);
    m_folderPreviewDecode.reset();
    m_folderPreviewUploadStage = 0;
    m_folderPreviewDecoded = false;
    m_layoutDirty = true;
    m_audio.playSfx(Sfx::ConfirmPositive);
    if (m_openFolderId == folderId)
        applyDisplayModel(buildOpenFolderModel(m_openFolderId), 0, false);
    else
        applyDisplayModel(buildRootFolderModel(), titleId, false);
}

void WiiUMenuApp::createFolder(int targetSlot) {
    auto& i18n = nxui::I18n::instance();
    requestTextEntry(i18n.tr("folder.create", "Create folder"),
                     i18n.tr("folder.name_guide", "Enter a folder name"),
                     defaultFolderName(), 48, false,
                     [this, targetSlot](const std::string& typed) {
                         finishCreateFolder(targetSlot, typed);
                     });
}

void WiiUMenuApp::finishCreateFolder(int targetSlot, const std::string& typed) {
    auto& i18n = nxui::I18n::instance();
    (void)i18n;
    // An empty field still produces a folder rather than silently doing nothing.
    const std::string name = typed.empty() ? defaultFolderName() : typed;
    DebugLog::log("[folders] create requested slot=%d name=%s", targetSlot, name.c_str());
    const std::uint32_t id = m_folderStore.create(name);
    if (id == 0 || !saveFoldersOrReport("create")) return;
    if (targetSlot >= 0 && targetSlot < static_cast<int>(m_layoutSlots.size()) &&
        m_layoutSlots[static_cast<std::size_t>(targetSlot)] == 0) {
        m_layoutSlots[static_cast<std::size_t>(targetSlot)] = folderTitleId(id);
        m_layoutDirty = true;
        saveMenuLayout();
    }
    m_audio.playSfx(Sfx::ConfirmPositive);
    applyDisplayModel(buildRootFolderModel(), folderTitleId(id), true);
}

std::string WiiUMenuApp::widgetTypeLabel(switchu::widgets::WidgetType type) const {
    auto& i18n = nxui::I18n::instance();
    switch (type) {
        case switchu::widgets::WidgetType::Clock:
            return i18n.tr("widget.clock", "Clock");
        case switchu::widgets::WidgetType::RecentlyPlayed:
            return i18n.tr("widget.recently_played", "Recently played");
        case switchu::widgets::WidgetType::RecentPlaytime:
            return i18n.tr("widget.recent_playtime", "Recent playtime");
        case switchu::widgets::WidgetType::RandomScreenshot:
            return i18n.tr("widget.random_screenshot", "Random screenshot");
        case switchu::widgets::WidgetType::ImagePin:
            return i18n.tr("widget.image_pin", "Image pin");
        case switchu::widgets::WidgetType::Batteries:
            return i18n.tr("widget.batteries", "Batteries");
    }
    return i18n.tr("widget.title", "Widget");
}

std::string WiiUMenuApp::widgetDurationLabel(std::uint64_t seconds) const {
    auto& i18n = nxui::I18n::instance();
    if (seconds == 0)
        return i18n.tr("widget.no_playtime", "No recent playtime");
    const std::uint64_t hours = seconds / 3600;
    const std::uint64_t minutes = (seconds % 3600) / 60;
    if (hours > 0)
        return std::to_string(hours) + " h " + std::to_string(minutes) + " min";
    return std::to_string(std::max<std::uint64_t>(1, minutes)) + " min";
}

void WiiUMenuApp::refreshRecentActivityDuration() {
    const std::uint64_t titleId = m_widgetStore.recentActivity().titleId;
    if (titleId != 0 && !isNativeApplicationId(titleId) && !m_config.isGamePort(titleId)) {
        m_widgetStore.clearRecentActivity();
        m_widgetStore.save();
        return;
    }
    m_widgetStore.updateRecentDuration(
        static_cast<std::int64_t>(std::time(nullptr)));
#ifdef SWITCHU_MENU
    if (const auto total = switchu::menu::playtime::query(
            m_widgetStore.recentActivity().titleId))
        m_widgetStore.setTotalSeconds(*total / 1000000000ULL);
#endif
}

void WiiUMenuApp::ensureRecentWidgetAssets(std::uint64_t titleId) {
    if (titleId == 0 || m_recentWidgetAssetTitleId == titleId) return;
    if (m_recentWidgetAssetDecode)
        m_recentWidgetAssetDecode->cancelled.store(true);
    m_recentWidgetAssetTitleId = titleId;
    const std::string heroPath = SteamGridDbManager::heroPath(titleId);
    const std::string logoPath = SteamGridDbManager::logoPath(titleId);
    m_recentWidgetAssetReady.reset();
    m_recentWidgetAssetUploadStage = 0;

    auto state = std::make_shared<RecentWidgetAssetDecodeState>();
    state->titleId = titleId;
    m_recentWidgetAssetDecode = state;
    m_recentWidgetAssetFuture = m_threadPool.submit(
        [state, heroPath, logoPath, titleId]() {
            const auto started = std::chrono::steady_clock::now();
            std::error_code error;
            if (std::filesystem::is_regular_file(heroPath, error) &&
                !state->cancelled.load()) {
                state->hero = steamgriddb::artwork::decode(heroPath, 640, 360, true);
            }
            error.clear();
            if (std::filesystem::is_regular_file(logoPath, error) &&
                !state->cancelled.load()) {
                state->logo = steamgriddb::artwork::decode(logoPath, 384, 192, false);
            }
            if (!state->cancelled.load()) {
                const auto iconData = AppListLoader::loadIconData(titleId);
                if (!state->cancelled.load())
                    state->icon = IconStreamer::decodeIconData(iconData);
            }
            state->elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - started).count();
        });
    DebugLog::log("[widget-recent-assets] queued title=0x%016lX",
                  static_cast<unsigned long>(titleId));
}

void WiiUMenuApp::syncRecentWidgetTextures() {
    if (!m_grid) return;
    const bool matches = m_recentWidgetLoadedTitleId != 0 &&
                         m_recentWidgetLoadedTitleId == m_recentWidgetAssetTitleId;
    for (const auto& icon : m_grid->allIcons()) {
        if (!icon || icon->entryKind() != GridEntryKind::Widget)
            continue;
        if (icon->widgetType() != switchu::widgets::WidgetType::RecentlyPlayed &&
            icon->widgetType() != switchu::widgets::WidgetType::RecentPlaytime)
            continue;
        if (icon->widgetGameTitleId() != m_recentWidgetAssetTitleId)
            continue;
        icon->setWidgetGameTextures(
            m_recentWidgetAssetTitleId,
            matches ? m_recentWidgetHero.get() : nullptr,
            matches ? m_recentWidgetLogo.get() : nullptr,
            matches ? m_recentWidgetIcon.get() : nullptr);
    }
}

void WiiUMenuApp::pollRecentWidgetAssets() {
    if (!m_recentWidgetAssetReady && m_recentWidgetAssetFuture.valid() &&
        m_recentWidgetAssetFuture.wait_for(std::chrono::seconds(0)) ==
            std::future_status::ready) {
        try {
            m_recentWidgetAssetFuture.get();
            auto decoded = std::move(m_recentWidgetAssetDecode);
            if (decoded && !decoded->cancelled.load() &&
                decoded->titleId == m_recentWidgetAssetTitleId) {
                DebugLog::log(
                    "[widget-recent-assets] decoded title=0x%016lX hero=%zu logo=%zu icon=%zu in %ldms",
                    static_cast<unsigned long>(decoded->titleId),
                    decoded->hero.rgba.size(), decoded->logo.rgba.size(),
                    decoded->icon.rgba.size(), static_cast<long>(decoded->elapsedMs));
                m_recentWidgetAssetReady = std::move(decoded);
                m_recentWidgetAssetUploadStage = 0;
            }
        } catch (const std::exception& ex) {
            DebugLog::log("[widget-recent-assets] decode failed: %s", ex.what());
            m_recentWidgetAssetDecode.reset();
        } catch (...) {
            DebugLog::log("[widget-recent-assets] decode failed: unknown exception");
            m_recentWidgetAssetDecode.reset();
        }
    }

    if (!m_recentWidgetAssetReady)
        return;

    auto& decoded = *m_recentWidgetAssetReady;
    if (m_recentWidgetAssetUploadStage == 0) {
        auto texture = std::make_unique<nxui::Texture>();
        if (!decoded.hero.rgba.empty() && texture->loadFromPixels(
                app().gpu(), app().renderer(), decoded.hero.rgba.data(),
                decoded.hero.width, decoded.hero.height))
            m_recentWidgetHero = std::move(texture);
        else
            m_recentWidgetHero.reset();
        decoded.hero.rgba.clear();
    } else if (m_recentWidgetAssetUploadStage == 1) {
        auto texture = std::make_unique<nxui::Texture>();
        if (!decoded.logo.rgba.empty() && texture->loadFromPixels(
                app().gpu(), app().renderer(), decoded.logo.rgba.data(),
                decoded.logo.width, decoded.logo.height))
            m_recentWidgetLogo = std::move(texture);
        else
            m_recentWidgetLogo.reset();
        decoded.logo.rgba.clear();
    } else if (m_recentWidgetAssetUploadStage == 2) {
        auto texture = std::make_unique<nxui::Texture>();
        if (!decoded.icon.rgba.empty() && texture->loadFromPixels(
                app().gpu(), app().renderer(), decoded.icon.rgba.data(),
                decoded.icon.w, decoded.icon.h))
            m_recentWidgetIcon = std::move(texture);
        else
            m_recentWidgetIcon.reset();
        decoded.icon.rgba.clear();
    }

    m_recentWidgetLoadedTitleId = decoded.titleId;
    ++m_recentWidgetAssetUploadStage;
    syncRecentWidgetTextures();
    if (m_recentWidgetAssetUploadStage >= 3) {
        DebugLog::log("[widget-recent-assets] upload complete title=0x%016lX",
                      static_cast<unsigned long>(decoded.titleId));
        m_recentWidgetAssetReady.reset();
        m_recentWidgetAssetUploadStage = 0;
    }
}

void WiiUMenuApp::ensureGameArtwork(std::uint64_t titleId) {
    if (titleId == 0 || m_gameArtwork.count(titleId)) return;
    if ((m_gameArtworkDecode && m_gameArtworkDecode->titleId == titleId) ||
        (m_gameArtworkReady && m_gameArtworkReady->titleId == titleId) ||
        std::find(m_gameArtworkDecodeQueue.begin(), m_gameArtworkDecodeQueue.end(),
                  titleId) != m_gameArtworkDecodeQueue.end())
        return;
    m_gameArtworkDecodeQueue.push_back(titleId);
    startNextGameArtworkDecode();
}

void WiiUMenuApp::startNextGameArtworkDecode() {
    if (m_gameArtworkFuture.valid() || m_gameArtworkDecode || m_gameArtworkReady ||
        m_gameArtworkDecodeQueue.empty())
        return;
    const std::uint64_t titleId = m_gameArtworkDecodeQueue.front();
    m_gameArtworkDecodeQueue.erase(m_gameArtworkDecodeQueue.begin());
    auto state = std::make_shared<GameArtworkDecodeState>();
    state->titleId = titleId;
    m_gameArtworkDecode = state;
    const std::string heroPath = SteamGridDbManager::heroPath(titleId);
    const std::string logoPath = SteamGridDbManager::logoPath(titleId);
    m_gameArtworkFuture = m_threadPool.submit([state, heroPath, logoPath]() {
        const auto started = std::chrono::steady_clock::now();
        std::error_code error;
        if (std::filesystem::is_regular_file(heroPath, error) &&
            !state->cancelled.load()) {
            state->hero = steamgriddb::artwork::decode(heroPath, 640, 360, true);
        }
        error.clear();
        if (std::filesystem::is_regular_file(logoPath, error) &&
            !state->cancelled.load()) {
            state->logo = steamgriddb::artwork::decode(logoPath, 384, 192, false);
        }
        state->elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - started).count();
    });
    DebugLog::log("[game-artwork] queued title=0x%016lX remaining=%zu",
                  static_cast<unsigned long>(titleId),
                  m_gameArtworkDecodeQueue.size());
}

void WiiUMenuApp::syncGameArtworkTextures(std::uint64_t titleId) {
    if (!m_grid) return;
    const auto artwork = m_gameArtwork.find(titleId);
    if (artwork == m_gameArtwork.end()) return;
    for (const auto& icon : m_grid->allIcons()) {
        if (icon && icon->entryKind() == GridEntryKind::Application &&
            icon->titleId() == titleId && icon->gridSpanColumns() > 1 &&
            icon->gridSpanRows() == 1) {
            icon->setWideGameTextures(artwork->second.hero.get(),
                                      artwork->second.logo.get());
        }
    }
}

void WiiUMenuApp::pollGameArtworkAssets() {
    if (!m_gameArtworkReady && m_gameArtworkFuture.valid() &&
        m_gameArtworkFuture.wait_for(std::chrono::seconds(0)) ==
            std::future_status::ready) {
        try {
            m_gameArtworkFuture.get();
            auto decoded = std::move(m_gameArtworkDecode);
            if (decoded && !decoded->cancelled.load()) {
                DebugLog::log(
                    "[game-artwork] decoded title=0x%016lX hero=%zu logo=%zu in %ldms",
                    static_cast<unsigned long>(decoded->titleId),
                    decoded->hero.rgba.size(), decoded->logo.rgba.size(),
                    static_cast<long>(decoded->elapsedMs));
                m_gameArtworkReady = std::move(decoded);
                m_gameArtworkUploadTextures = {};
                m_gameArtworkUploadStage = 0;
            }
        } catch (const std::exception& ex) {
            DebugLog::log("[game-artwork] decode failed: %s", ex.what());
            m_gameArtworkDecode.reset();
        } catch (...) {
            DebugLog::log("[game-artwork] decode failed: unknown exception");
            m_gameArtworkDecode.reset();
        }
    }

    // Let recent-widget artwork finish first. This keeps the combined handoff
    // capped to one large texture upload on these frames.
    if (!m_gameArtworkReady) {
        startNextGameArtworkDecode();
        return;
    }
    if (m_recentWidgetAssetReady)
        return;

    auto& decoded = *m_gameArtworkReady;
    if (m_gameArtworkUploadStage == 0) {
        auto texture = std::make_unique<nxui::Texture>();
        if (!decoded.hero.rgba.empty() && texture->loadFromPixels(
                app().gpu(), app().renderer(), decoded.hero.rgba.data(),
                decoded.hero.width, decoded.hero.height))
            m_gameArtworkUploadTextures.hero = std::move(texture);
        decoded.hero.rgba.clear();
    } else {
        auto texture = std::make_unique<nxui::Texture>();
        if (!decoded.logo.rgba.empty() && texture->loadFromPixels(
                app().gpu(), app().renderer(), decoded.logo.rgba.data(),
                decoded.logo.width, decoded.logo.height))
            m_gameArtworkUploadTextures.logo = std::move(texture);
        decoded.logo.rgba.clear();
    }
    ++m_gameArtworkUploadStage;
    if (m_gameArtworkUploadStage >= 2) {
        const std::uint64_t titleId = decoded.titleId;
        m_gameArtwork[titleId] = std::move(m_gameArtworkUploadTextures);
        m_gameArtworkReady.reset();
        m_gameArtworkUploadStage = 0;
        syncGameArtworkTextures(titleId);
        DebugLog::log("[game-artwork] upload complete title=0x%016lX",
                      static_cast<unsigned long>(titleId));
        startNextGameArtworkDecode();
    }
}

bool WiiUMenuApp::saveWidgetsOrReport(const char* operation) {
    if (m_widgetStore.save()) return true;
    DebugLog::log("[widgets] operation failed op=%s",
                  operation ? operation : "unknown");
    m_widgetStore.load();
    auto& i18n = nxui::I18n::instance();
    m_dialogReturnFocus = m_contextMenuReturnFocus;
    m_dialog->show(i18n.tr("widget.error_title", "Widget error"),
                   i18n.tr("widget.save_error", "The widget change could not be saved."),
                   {{i18n.tr("button.ok", "OK"), {}, true}});
    focusManager().setFocus(m_dialog.get());
    return false;
}

bool WiiUMenuApp::canPlaceWidget(int targetSlot,
                                 switchu::widgets::WidgetSize size,
                                 std::uint32_t ignoringWidgetId) const {
    return canPlaceGridItem(targetSlot, size,
        ignoringWidgetId == 0 ? 0 :
            switchu::widgets::widgetTitleId(ignoringWidgetId));
}

bool WiiUMenuApp::canPlaceGridItem(int targetSlot,
                                   switchu::widgets::WidgetSize size,
                                   std::uint64_t ignoringTitleId,
                                   std::uint64_t alsoIgnoringTitleId) const {
    if (targetSlot < 0)
        return false;
    if (targetSlot >= static_cast<int>(m_layoutSlots.size())) {
        const_cast<WiiUMenuApp*>(this)->m_layoutSlots.resize(
            static_cast<size_t>(targetSlot + 1), 0);
        const_cast<WiiUMenuApp*>(this)->m_layoutDirty = true;
    }
    size = isCarouselLayout() ? switchu::widgets::WidgetSize{1, 1} : size;
    const int columns = std::clamp(m_config.gridColumns, 3, 8);
    const int rows = std::clamp(m_config.gridRows, 2, 5);
    const int perPage = columns * rows;
    const int local = targetSlot % perPage;
    const int targetColumn = local % columns;
    const int targetRow = local / columns;
    if (targetColumn + size.columns > columns || targetRow + size.rows > rows)
        return false;

    std::vector<bool> occupied(m_layoutSlots.size(), false);
    for (int index = 0; index < static_cast<int>(m_layoutSlots.size()); ++index) {
        const std::uint64_t value = m_layoutSlots[static_cast<std::size_t>(index)];
        if (value == 0) continue;
        if (value == ignoringTitleId || value == alsoIgnoringTitleId) continue;
        const std::uint32_t widgetId = switchu::widgets::widgetIdFromTitleId(value);
        const auto* widget = widgetId != 0 ? m_widgetStore.find(widgetId) : nullptr;
        if (isCarouselLayout()) {
            occupied[static_cast<std::size_t>(index)] = true;
            continue;
        }
        const auto widgetSize = widget
            ? switchu::widgets::validatedSize(
                widget->type, widget->size, AppLayoutMode::Grid)
            : gameGridSize(value, AppLayoutMode::Grid);
        const int widgetLocal = index % perPage;
        const int widgetColumn = widgetLocal % columns;
        const int widgetRow = widgetLocal / columns;
        if (widgetColumn + widgetSize.columns > columns ||
            widgetRow + widgetSize.rows > rows) {
            occupied[static_cast<std::size_t>(index)] = true;
            continue;
        }
        for (int dy = 0; dy < widgetSize.rows; ++dy) {
            for (int dx = 0; dx < widgetSize.columns; ++dx) {
                const int cell = index + dy * columns + dx;
                if (cell < static_cast<int>(occupied.size()))
                    occupied[static_cast<std::size_t>(cell)] = true;
            }
        }
    }

    for (int dy = 0; dy < size.rows; ++dy) {
        for (int dx = 0; dx < size.columns; ++dx) {
            const int cell = targetSlot + dy * columns + dx;
            if (cell >= static_cast<int>(occupied.size()) ||
                occupied[static_cast<std::size_t>(cell)])
                return false;
        }
    }
    return true;
}

// A 2x1 or larger tile stores its id in its anchor cell only; every other cell
// it covers stays 0, which is also what an empty cell holds. Placing a new
// entry by looking for the first 0 therefore dropped it inside a widget's own
// footprint. normalizeWidgetPlacements() then found that widget no longer fit
// where it was, moved it to the first free cell anywhere in the layout, and set
// m_layoutDirty -- so the arrangement was rewritten to disk. Creating a folder
// or filing a game into one is enough to start it, and each rebuild evicted
// another tile: a home screen with four 2x1 widgets across the top two rows
// came back with two of them on page three.
std::vector<bool> WiiUMenuApp::layoutSpanCoverage(
    const std::vector<std::uint64_t>& slots) const {
    const int columns = std::clamp(m_config.gridColumns, 3, 8);
    std::vector<bool> covered(slots.size(), false);
    for (std::size_t index = 0; index < slots.size(); ++index) {
        const std::uint64_t titleId = slots[index];
        if (titleId == 0)
            continue;
        switchu::widgets::WidgetSize size{1, 1};
        if (const auto* widget = m_widgetStore.find(
                switchu::widgets::widgetIdFromTitleId(titleId))) {
            if (switchu::widgets::supportedSizes(widget->type,
                                                 AppLayoutMode::Grid).empty())
                continue;
            size = switchu::widgets::validatedSize(widget->type, widget->size,
                                                   AppLayoutMode::Grid);
        } else {
            const bool isGame = std::any_of(
                m_allApps.begin(), m_allApps.end(),
                [titleId](const AppEntry& app) { return app.titleId == titleId; });
            if (!isGame)
                continue;
            size = gameGridSize(titleId, AppLayoutMode::Grid);
        }
        if (size.columns <= 1 && size.rows <= 1)
            continue;
        // The saved layout is always the grid arrangement, so spans are
        // measured against AppLayoutMode::Grid even while the dynamic line
        // view, where everything is 1x1, is the one on screen.
        for (int dy = 0; dy < size.rows; ++dy) {
            for (int dx = 0; dx < size.columns; ++dx) {
                if (dy == 0 && dx == 0)
                    continue;
                const std::size_t cell =
                    index + static_cast<std::size_t>(dy * columns + dx);
                if (cell < covered.size())
                    covered[cell] = true;
            }
        }
    }
    return covered;
}

void WiiUMenuApp::claimFreeLayoutSlot(std::vector<std::uint64_t>& slots,
                                      std::vector<bool>& covered,
                                      std::uint64_t titleId) const {
    for (std::size_t index = 0; index < slots.size(); ++index) {
        if (slots[index] != 0 || covered[index])
            continue;
        slots[index] = titleId;
        return;
    }
    slots.push_back(titleId);
    covered.push_back(false);
}

void WiiUMenuApp::normalizeWidgetPlacements() {
    if (m_layoutSlots.empty()) return;
    const int columns = std::clamp(m_config.gridColumns, 3, 8);
    const int rows = std::clamp(m_config.gridRows, 2, 5);
    const int perPage = std::max(1, columns * rows);

    struct Placement {
        std::uint64_t titleId = 0;
        int anchor = -1;
        switchu::widgets::WidgetSize size;
    };
    std::vector<Placement> placements;
    for (int index = 0; index < static_cast<int>(m_layoutSlots.size()); ++index) {
        const std::uint64_t titleId = m_layoutSlots[static_cast<std::size_t>(index)];
        const auto* widget = m_widgetStore.find(
            switchu::widgets::widgetIdFromTitleId(titleId));
        switchu::widgets::WidgetSize size{1, 1};
        if (widget) {
            const auto sizes = switchu::widgets::supportedSizes(
                widget->type, AppLayoutMode::Grid);
            if (sizes.empty()) continue;
            size = switchu::widgets::validatedSize(
                widget->type, widget->size, AppLayoutMode::Grid);
        } else {
            const bool isGame = std::any_of(m_allApps.begin(), m_allApps.end(),
                [titleId](const AppEntry& app) { return app.titleId == titleId; });
            if (!isGame) continue;
            size = gameGridSize(titleId, AppLayoutMode::Grid);
        }
        if (size.columns > 1 || size.rows > 1)
            placements.push_back({titleId, index, size});
    }
    if (placements.empty()) return;

    std::vector<bool> reserved(m_layoutSlots.size(), false);
    for (int index = 0; index < static_cast<int>(m_layoutSlots.size()); ++index) {
        const std::uint64_t titleId = m_layoutSlots[static_cast<std::size_t>(index)];
        reserved[static_cast<std::size_t>(index)] = titleId != 0;
        if (const auto* widget = m_widgetStore.find(
                switchu::widgets::widgetIdFromTitleId(titleId));
            widget && switchu::widgets::supportedSizes(
                widget->type, AppLayoutMode::Grid).empty())
            reserved[static_cast<std::size_t>(index)] = false;
    }
    std::vector<bool> occupied(m_layoutSlots.size(), false);

    auto fits = [&](int anchor, switchu::widgets::WidgetSize size) {
        if (anchor < 0 || anchor >= static_cast<int>(m_layoutSlots.size()))
            return false;
        const int local = anchor % perPage;
        const int column = local % columns;
        const int row = local / columns;
        if (column + size.columns > columns || row + size.rows > rows)
            return false;
        for (int dy = 0; dy < size.rows; ++dy) {
            for (int dx = 0; dx < size.columns; ++dx) {
                const int cell = anchor + dy * columns + dx;
                if (cell >= static_cast<int>(reserved.size()) ||
                    reserved[static_cast<std::size_t>(cell)] ||
                    occupied[static_cast<std::size_t>(cell)])
                    return false;
            }
        }
        return true;
    };
    auto occupy = [&](int anchor, switchu::widgets::WidgetSize size) {
        for (int dy = 0; dy < size.rows; ++dy)
            for (int dx = 0; dx < size.columns; ++dx)
                occupied[static_cast<std::size_t>(anchor + dy * columns + dx)] = true;
    };

    for (const auto& placement : placements) {
        reserved[static_cast<std::size_t>(placement.anchor)] = false;
        int target = fits(placement.anchor, placement.size) ? placement.anchor : -1;
        if (target < 0) {
            for (int candidate = 0;
                 candidate < static_cast<int>(m_layoutSlots.size()); ++candidate) {
                if (fits(candidate, placement.size)) {
                    target = candidate;
                    break;
                }
            }
        }
        if (target < 0) {
            const std::size_t oldSize = m_layoutSlots.size();
            m_layoutSlots.resize(oldSize + static_cast<std::size_t>(perPage), 0);
            reserved.resize(m_layoutSlots.size(), false);
            occupied.resize(m_layoutSlots.size(), false);
            for (int candidate = static_cast<int>(oldSize);
                 candidate < static_cast<int>(m_layoutSlots.size()); ++candidate) {
                if (fits(candidate, placement.size)) {
                    target = candidate;
                    break;
                }
            }
        }
        if (target < 0) {
            reserved[static_cast<std::size_t>(placement.anchor)] = true;
            continue;
        }
        if (target != placement.anchor) {
            m_layoutSlots[static_cast<std::size_t>(placement.anchor)] = 0;
            m_layoutSlots[static_cast<std::size_t>(target)] = placement.titleId;
            m_layoutDirty = true;
        }
        reserved[static_cast<std::size_t>(target)] = true;
        occupy(target, placement.size);
    }
}

std::string WiiUMenuApp::resolveWidgetAssetRef(const std::string& assetRef) const {
    if (assetRef.empty() || assetRef.find("..") != std::string::npos ||
        assetRef.find('\\') != std::string::npos)
        return {};
    if (assetRef.rfind("widget:", 0) == 0) {
        const std::string relative = assetRef.substr(7);
        return relative.empty() ? std::string()
            : std::string(switchu::widgets::WidgetStore::kAssetRoot) + "/" + relative;
    }
    if (assetRef.rfind("theme:", 0) == 0) {
        const std::string relative = assetRef.substr(6);
        if (relative.empty() || m_effectivePreset.installPath.empty()) return {};
        return m_effectivePreset.installPath + "/" + relative;
    }
    return {};
}

std::string WiiUMenuApp::folderPreviewSignature(
    const switchu::folders::Folder& folder) const {
    std::string signature;
    signature.reserve(folder.titleIds.size() * 17);
    std::size_t count = 0;
    for (std::uint64_t titleId : folder.titleIds) {
        if (titleId == 0)
            continue;
        char buffer[18]{};
        std::snprintf(buffer, sizeof(buffer), "%016llX:",
                      static_cast<unsigned long long>(titleId));
        signature += buffer;
        if (++count >= 9)
            break;
    }
    return signature;
}

// One folder is decoded at a time and its icons are uploaded one per frame, for
// the same reason the icon streamer does: a burst of uploads inside one frame is
// what the GPU budget here cannot absorb.
void WiiUMenuApp::syncFolderPreviews() {
#ifdef SWITCHU_MENU
    if (!m_grid)
        return;

    // Finished decode: take the pixels and upload them a texture at a time.
    // get() invalidates the future, so readiness is remembered in a flag; testing
    // valid() again on the next frame was false and the upload stopped dead after
    // the first icon, which is why a folder of three games drew one large one.
    if (m_folderPreviewDecode && !m_folderPreviewDecoded &&
        m_folderPreviewFuture.valid() &&
        m_folderPreviewFuture.wait_for(std::chrono::seconds(0)) ==
            std::future_status::ready) {
        try {
            m_folderPreviewFuture.get();
            m_folderPreviewDecoded = true;
        } catch (...) {
            m_folderPreviewDecode.reset();
        }
    }
    if (m_folderPreviewDecoded) {
        if (m_folderPreviewDecode && !m_folderPreviewDecode->cancelled.load()) {
            auto& decode = *m_folderPreviewDecode;
            auto& assets = m_folderPreviews[decode.folderId];
            if (m_folderPreviewUploadStage == 0) {
                assets.signature = decode.signature;
                assets.textures.clear();
                // Every icon drops its borrowed pointers before the old set is
                // released, so nothing can render a freed texture.
                for (const auto& icon : m_grid->allIcons())
                    if (icon && icon->entryKind() == GridEntryKind::Folder)
                        icon->setFolderPreview({});
            }
            if (m_folderPreviewUploadStage < decode.icons.size()) {
                auto& source = decode.icons[m_folderPreviewUploadStage];
                auto texture = std::make_unique<nxui::Texture>();
                if (!source.rgba.empty() &&
                    texture->loadFromPixels(app().gpu(), app().renderer(),
                                            source.rgba.data(), source.w, source.h))
                    assets.textures.push_back(std::move(texture));
                else
                    assets.textures.push_back(nullptr);
                source.rgba.clear();
                ++m_folderPreviewUploadStage;
            } else {
                DebugLog::log("[folder-preview] folder=%u icons=%zu",
                              decode.folderId, assets.textures.size());
                m_folderPreviewDecode.reset();
                m_folderPreviewUploadStage = 0;
                m_folderPreviewDecoded = false;
            }
        } else {
            m_folderPreviewDecode.reset();
            m_folderPreviewUploadStage = 0;
            m_folderPreviewDecoded = false;
        }
    }

    // Hand the current textures to the tiles, and find one that still needs a
    // decode. Only folders on screen are worth the work.
    std::uint32_t wanted = 0;
    std::string wantedSignature;
    for (const auto& icon : m_grid->allIcons()) {
        if (!icon || icon->entryKind() != GridEntryKind::Folder || !icon->isVisible())
            continue;
        const std::uint32_t folderId =
            static_cast<std::uint32_t>(icon->titleId() - kFolderTitleIdPrefix);
        const auto* folder = m_folderStore.find(folderId);
        if (!folder)
            continue;
        const std::string signature = folderPreviewSignature(*folder);
        const auto found = m_folderPreviews.find(folderId);
        if (found != m_folderPreviews.end() && found->second.signature == signature) {
            std::vector<nxui::Texture*> textures;
            textures.reserve(found->second.textures.size());
            for (const auto& texture : found->second.textures)
                textures.push_back(texture.get());
            icon->setFolderPreview(std::move(textures));
            continue;
        }
        icon->setFolderPreview({});
        if (wanted == 0 && !signature.empty()) {
            wanted = folderId;
            wantedSignature = signature;
        }
    }

    if (wanted == 0 || m_folderPreviewDecode)
        return;

    const auto* folder = m_folderStore.find(wanted);
    if (!folder)
        return;
    std::vector<std::uint64_t> members;
    for (std::uint64_t titleId : folder->titleIds) {
        if (titleId == 0)
            continue;
        members.push_back(titleId);
        if (members.size() >= 9)
            break;
    }
    if (members.empty())
        return;

    auto decode = std::make_shared<FolderPreviewDecode>();
    decode->folderId = wanted;
    decode->signature = wantedSignature;
    m_folderPreviewDecode = decode;
    m_folderPreviewUploadStage = 0;
    m_folderPreviewDecoded = false;
    m_folderPreviewFuture = m_threadPool.submit([decode, members]() {
        decode->icons.reserve(members.size());
        for (std::uint64_t titleId : members) {
            if (decode->cancelled.load())
                return;
            decode->icons.push_back(
                IconStreamer::decodeIconData(AppListLoader::loadIconData(titleId)));
        }
    });
#endif
}

// Widget tiles are built once per model rebuild, so the values that move while
// the menu is open have to be pushed in every frame.
void WiiUMenuApp::syncWidgetIconContent() {
    if (!m_grid)
        return;
    const auto& recent = m_widgetStore.recentActivity();
    if (recent.titleId != 0)
        ensureRecentWidgetAssets(recent.titleId);
    for (const auto& icon : m_grid->allIcons()) {
        if (!icon || icon->entryKind() != GridEntryKind::Widget)
            continue;
        icon->setConsoleBattery(m_consoleBatteryPercent, m_consoleBatteryCharging);
        // The playtime tile draws the game icon in the same way the
        // recently-played one does, and it was never fed, so its icon slot stayed
        // an empty rounded rectangle.
        const auto kind = icon->widgetType();
        if (kind == switchu::widgets::WidgetType::RecentlyPlayed ||
            kind == switchu::widgets::WidgetType::RecentPlaytime) {
            icon->setWidgetGameTextures(m_recentWidgetLoadedTitleId,
                                        m_recentWidgetHero.get(),
                                        m_recentWidgetLogo.get(),
                                        m_recentWidgetIcon.get());
        }
    }
}

void WiiUMenuApp::syncWidgetPageAssets() {
    if (!m_grid || m_grid->allIcons().empty()) return;

    const auto& icons = m_grid->allIcons();
    m_widgetAssetCurrentScratch.assign(icons.size(), 0);
    m_widgetAssetKeepScratch.assign(icons.size(), 0);
    auto& current = m_widgetAssetCurrentScratch;
    auto& keep = m_widgetAssetKeepScratch;
    const bool dynamicLine = isCarouselLayout();
    const int page = dynamicLine
        ? std::max(0, m_grid->focusedGlobalIndex())
        : m_grid->currentPage();
    const bool pageChanged = page != m_widgetAssetPage;
    const bool sliding = m_grid->isTransitioning();
    const bool transitionEnded = m_widgetAssetsWereSliding && !sliding;
    m_widgetAssetPage = page;
    m_widgetAssetsWereSliding = sliding;

    if (dynamicLine) {
        // The carousel renderer keeps roughly four neighbours on each side in
        // view. One extra item avoids a decode exactly as it enters the clip.
        const int begin = std::max(0, page - 5);
        const int end = std::min(static_cast<int>(icons.size()), page + 6);
        for (int i = begin; i < end; ++i) {
            keep[static_cast<std::size_t>(i)] = true;
            current[static_cast<std::size_t>(i)] = true;
        }
    } else {
        const int perPage = std::max(1, m_grid->iconsPerPage());
        const int totalPages = std::max(1, m_grid->totalPages());
        const auto markPage = [&](int wantedPage, bool visibleNow) {
            if (wantedPage < 0 || wantedPage >= totalPages) return;
            const int begin = wantedPage * perPage;
            const int end = std::min(begin + perPage,
                                     static_cast<int>(icons.size()));
            for (int i = begin; i < end; ++i) {
                keep[static_cast<std::size_t>(i)] = true;
                if (visibleNow)
                    current[static_cast<std::size_t>(i)] = true;
            }
        };
        markPage(page, true);
        // Adjacent pages are warmed progressively while idle. Normal page
        // changes therefore never decode a GIF in the middle of the slide.
        markPage(page - 1, false);
        markPage(page + 1, false);
    }

    if (pageChanged || transitionEnded) {
        for (std::size_t i = 0; i < icons.size(); ++i) {
            if (current[i] && icons[i])
                icons[i]->allowWidgetImageAssetRetry();
        }
    }

    // Decoding is performed by the worker pool. Keep the Deko side deliberately
    // small: at most two finished frames are uploaded per UI frame.
    int animationUploads = 0;
    for (const auto& icon : icons) {
        if (!icon || !icon->isWidgetImageAssetLoading()) continue;
        if (animationUploads >= 2) break;
        if (icon->pollWidgetImageAssetLoad(app().gpu(), app().renderer()))
            ++animationUploads;
    }

    bool currentNeedsMemory = false;
    for (std::size_t i = 0; i < icons.size(); ++i) {
        const auto* icon = icons[i].get();
        if (current[i] && icon && icon->hasWidgetImageAsset() &&
            !icon->isWidgetImageAssetLoaded() &&
            !icon->widgetImageAssetLoadAttempted()) {
            currentNeedsMemory = true;
            break;
        }
    }

    // Do not release the outgoing page until its last transition frame has
    // completed. Destruction of Deko image memory also requires the queue to
    // be idle because the preceding frame may still reference it.
    std::vector<GlossyIcon*> release;
    constexpr std::uint64_t kCurrentPageReserve = 12u * 1024u * 1024u;
    const bool reclaimPrefetchForCurrent = !sliding && currentNeedsMemory &&
        app().gpu().imageMemoryAvailable() < kCurrentPageReserve;
    if (!sliding) {
        for (std::size_t i = 0; i < icons.size(); ++i) {
            auto* icon = icons[i].get();
            const bool retainedForSmoothPaging = keep[i] &&
                !(reclaimPrefetchForCurrent && !current[i]);
            if (!icon || retainedForSmoothPaging || icon == m_editSourceIcon ||
                !icon->hasWidgetImageAsset() ||
                (!icon->isWidgetImageAssetLoaded() &&
                 !icon->isWidgetImageAssetLoading()))
                continue;
            release.push_back(icon);
        }
    }
    if (!release.empty()) {
        const bool releasesGpuMemory = std::any_of(
            release.begin(), release.end(),
            [](const GlossyIcon* icon) {
                return icon && icon->isWidgetImageAssetLoaded();
            });
        if (releasesGpuMemory)
            app().gpu().waitIdle();
        bool released = false;
        for (auto* icon : release)
            released = icon->unloadWidgetImageAsset() || released;
#ifdef NXUI_BACKEND_DEKO3D
        if (released)
            app().renderer().reclaimReleasedTextureSlotsAfterIdle();
#endif
        DebugLog::log("[widget-assets] released=%zu page=%d gpu=%llu/%llu",
                      release.size(), page,
                      static_cast<unsigned long long>(app().gpu().imageMemoryUsed()),
                      static_cast<unsigned long long>(app().gpu().imageMemoryBudget()));
    }

    // Visible assets have priority and are all attempted before rendering.
    // A failure is remembered until the page changes, preventing an expensive
    // GIF decode loop when the fixed GPU budget is genuinely exhausted.
    for (std::size_t i = 0; i < icons.size(); ++i) {
        auto* icon = icons[i].get();
        if (!current[i] || !icon || !icon->hasWidgetImageAsset() ||
            icon->isWidgetImageAssetLoaded() ||
            icon->widgetImageAssetLoadAttempted())
            continue;
        icon->startWidgetImageAssetLoad(
            m_threadPool, app().gpu(), app().renderer());
    }

    if (sliding) return;

    // Decode at most one off-screen asset per frame. Keep enough space for
    // text and for a reasonably-sized current-page animation; prefetching is
    // opportunistic and must never starve the UI itself.
    constexpr std::uint64_t kPrefetchReserve = 8u * 1024u * 1024u;
    if (app().gpu().imageMemoryAvailable() <= kPrefetchReserve) return;
    for (std::size_t i = 0; i < icons.size(); ++i) {
        auto* icon = icons[i].get();
        if (!keep[i] || current[i] || !icon ||
            !icon->hasWidgetImageAsset() || icon->isWidgetImageAssetLoaded() ||
            icon->widgetImageAssetLoadAttempted())
            continue;
        icon->startWidgetImageAssetLoad(
            m_threadPool, app().gpu(), app().renderer());
        break;
    }
}

std::vector<std::pair<std::string, std::string>>
WiiUMenuApp::listWidgetAssets(bool screenshotsOnly) const {
    std::vector<std::pair<std::string, std::string>> result;
    std::unordered_set<std::string> seen;
    auto supported = [](std::string extension) {
        std::transform(extension.begin(), extension.end(), extension.begin(),
            [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
        return extension == ".png" || extension == ".jpg" || extension == ".jpeg"
            || extension == ".webp" || extension == ".gif";
    };
    auto scan = [&](const std::string& root, const std::string& referencePrefix,
                    const std::string& relativeBase) {
        std::error_code ec;
        if (root.empty() || !std::filesystem::is_directory(root, ec)) return;
        std::filesystem::recursive_directory_iterator iterator(
            root, std::filesystem::directory_options::skip_permission_denied, ec);
        const std::filesystem::recursive_directory_iterator end;
        for (; !ec && iterator != end && result.size() < 64; iterator.increment(ec)) {
            if (!iterator->is_regular_file(ec) || !supported(iterator->path().extension().string()))
                continue;
            std::string relative = iterator->path().string().substr(root.size());
            while (!relative.empty() && relative.front() == '/') relative.erase(relative.begin());
            if (relative.empty()) continue;
            std::string stored = referencePrefix + relativeBase + relative;
            if (!seen.insert(stored).second) continue;
            result.emplace_back(iterator->path().filename().string(), std::move(stored));
        }
    };

    const std::string widgetRoot = switchu::widgets::WidgetStore::kAssetRoot;
    if (screenshotsOnly) {
        scan(widgetRoot + "/screenshots", "widget:", "screenshots/");
    } else {
        scan(widgetRoot, "widget:", "");
    }
    if (!m_effectivePreset.installPath.empty()) {
        const std::string& themeRoot = m_effectivePreset.installPath;
        if (screenshotsOnly) {
            scan(themeRoot + "/widgets/screenshots", "theme:", "widgets/screenshots/");
            scan(themeRoot + "/screenshots", "theme:", "screenshots/");
        } else {
            scan(themeRoot + "/widgets", "theme:", "widgets/");
            scan(themeRoot + "/assets/widgets", "theme:", "assets/widgets/");
        }
    }
    std::sort(result.begin(), result.end(), [](const auto& left, const auto& right) {
        return left.first < right.first;
    });
    return result;
}

std::string WiiUMenuApp::randomScreenshotPath(std::uint32_t widgetId) const {
    const auto assets = listWidgetAssets(true);
    if (assets.empty()) return {};
    const std::uint64_t bucket = static_cast<std::uint64_t>(std::time(nullptr)) / 60u;
    const std::size_t index = static_cast<std::size_t>(
        (bucket * 11400714819323198485ull + widgetId) % assets.size());
    return resolveWidgetAssetRef(assets[index].second);
}

void WiiUMenuApp::showAddContextMenu(int targetSlot, const nxui::Rect& anchor) {
    if (!m_contextMenu || m_openFolderId != 0) return;
    auto& i18n = nxui::I18n::instance();
    if (!m_contextMenu->isActive())
        m_contextMenuReturnFocus = focusManager().current();
    m_contextMenu->show(anchor, i18n.tr("add.title", "Add"), {
        {i18n.tr("folder.create", "Create new folder"),
         [this, targetSlot]() {
             m_contextMenu->hide();
             createFolder(targetSlot);
         }},
        {i18n.tr("widget.create", "Create new widget"),
         [this, targetSlot, anchor]() { showWidgetTypeMenu(targetSlot, anchor); }},
    });
    m_audio.playSfx(Sfx::ModalShow);
    focusManager().setFocus(m_contextMenu.get());
}

void WiiUMenuApp::showWidgetTypeMenu(int targetSlot, const nxui::Rect& anchor) {
    auto& i18n = nxui::I18n::instance();
    std::vector<ContextMenu::Item> items;
    for (auto type : {switchu::widgets::WidgetType::Clock,
                      switchu::widgets::WidgetType::RecentlyPlayed,
                      switchu::widgets::WidgetType::RecentPlaytime,
                      switchu::widgets::WidgetType::ImagePin,
                      switchu::widgets::WidgetType::Batteries}) {
        if (switchu::widgets::supportedSizes(type, m_appLayoutMode).empty())
            continue;
        items.push_back({widgetTypeLabel(type),
            [this, targetSlot, anchor, type]() {
                showWidgetSizeMenu(targetSlot, anchor, type);
            }});
    }
    m_contextMenu->show(anchor, i18n.tr("widget.choose_type", "Choose widget"),
                        std::move(items), 0,
                        [this, targetSlot, anchor]() {
                            showAddContextMenu(targetSlot, anchor);
                        });
    focusManager().setFocus(m_contextMenu.get());
}

void WiiUMenuApp::showWidgetSizeMenu(int targetSlot, const nxui::Rect& anchor,
                                     switchu::widgets::WidgetType type) {
    auto& i18n = nxui::I18n::instance();
    std::vector<ContextMenu::Item> items;
    for (const auto size : switchu::widgets::supportedSizes(type, m_appLayoutMode)) {
        const bool available = canPlaceWidget(targetSlot, size);
        const std::string label = std::to_string(size.columns) + "ÃƒÆ’Ã¢â‚¬â€"
            + std::to_string(size.rows)
            + (available ? std::string() : " ÃƒÂ¢Ã¢â€šÂ¬Ã¢â‚¬Â " + i18n.tr("widget.no_space", "No space"));
        items.push_back({label, [this, targetSlot, anchor, type, size]() {
            if (type == switchu::widgets::WidgetType::ImagePin)
                showWidgetAssetMenu(targetSlot, anchor, type, size);
            else
                createWidget(targetSlot, type, size);
        }, available});
    }
    m_contextMenu->show(anchor, i18n.tr("widget.choose_size", "Choose size"),
                        std::move(items), 0,
                        [this, targetSlot, anchor]() {
                            showWidgetTypeMenu(targetSlot, anchor);
                        });
    focusManager().setFocus(m_contextMenu.get());
}

void WiiUMenuApp::showWidgetAssetMenu(int targetSlot, const nxui::Rect& anchor,
                                      switchu::widgets::WidgetType type,
                                      switchu::widgets::WidgetSize size) {
    auto assets = listWidgetAssets(false);
    auto& i18n = nxui::I18n::instance();
    std::vector<ContextMenu::Item> items;
    items.reserve(assets.size() + 1);
    items.push_back({"Search SteamGridDB...", [this, targetSlot, anchor, size]() {
        if (m_contextMenu) m_contextMenu->hide(false);
        requestTextEntry("Search SteamGridDB", "Image Pin", "", 128, false,
                         [this, targetSlot, anchor, size](const std::string& query) {
            if (!query.empty())
                openImagePinSteamGridDbPicker(targetSlot, anchor, size, query);
            else
                showWidgetAssetMenu(targetSlot, anchor,
                                    switchu::widgets::WidgetType::ImagePin, size);
        },
        [this, targetSlot, anchor, size]() {
            showWidgetAssetMenu(targetSlot, anchor,
                                switchu::widgets::WidgetType::ImagePin, size);
        });
    }});
    for (auto& [label, reference] : assets) {
        items.push_back({label, [this, targetSlot, type, size, reference]() {
            createWidget(targetSlot, type, size, reference);
        }});
    }
    m_contextMenu->show(anchor, i18n.tr("widget.choose_image", "Choose image"),
                        std::move(items), 0,
                        [this, targetSlot, anchor, type]() {
                            showWidgetSizeMenu(targetSlot, anchor, type);
                        });
    focusManager().setFocus(m_contextMenu.get());
}

void WiiUMenuApp::createWidget(int targetSlot, switchu::widgets::WidgetType type,
                               switchu::widgets::WidgetSize size,
                               const std::string& assetRef) {
    size = switchu::widgets::validatedSize(type, size, m_appLayoutMode);
    if (size.columns <= 0 || size.rows <= 0) return;
    if (!canPlaceWidget(targetSlot, size)) return;
    const std::uint32_t id = m_widgetStore.create(type, size, assetRef);
    if (id == 0 || !saveWidgetsOrReport("create")) return;
    m_layoutSlots[static_cast<std::size_t>(targetSlot)] =
        switchu::widgets::widgetTitleId(id);
    m_layoutDirty = true;
    saveMenuLayout();
    if (m_contextMenu) m_contextMenu->hide();
    m_audio.playSfx(Sfx::ConfirmPositive);
    applyDisplayModel(buildRootFolderModel(), switchu::widgets::widgetTitleId(id), true);
}

void WiiUMenuApp::showWidgetOptionsMenu(std::uint32_t widgetId, int slot,
                                        const nxui::Rect& anchor) {
    const auto* widget = m_widgetStore.find(widgetId);
    if (!widget || !m_contextMenu) return;
    m_contextMenuReturnFocus = focusManager().current();
    auto& i18n = nxui::I18n::instance();
    std::vector<ContextMenu::Item> items;
    for (const auto size : switchu::widgets::supportedSizes(widget->type, m_appLayoutMode)) {
        const bool available = canPlaceWidget(slot, size, widgetId);
        const std::string label = i18n.tr("widget.resize", "Resize") + " "
            + std::to_string(size.columns) + "ÃƒÆ’Ã¢â‚¬â€" + std::to_string(size.rows);
        items.push_back({label, [this, widgetId, size]() {
            if (!m_widgetStore.setSize(widgetId, size)) {
                m_contextMenu->hide();
                return;
            }
            if (!saveWidgetsOrReport("resize")) return;
            m_contextMenu->hide();
            applyDisplayModel(buildRootFolderModel(),
                              switchu::widgets::widgetTitleId(widgetId), false);
        }, available});
    }
    items.push_back({i18n.tr("widget.delete", "Delete widget"),
        [this, widgetId]() {
            if (!m_widgetStore.remove(widgetId) || !saveWidgetsOrReport("delete")) return;
            const auto pseudoId = switchu::widgets::widgetTitleId(widgetId);
            m_retainedImagePins.erase(pseudoId);
            std::replace(m_layoutSlots.begin(), m_layoutSlots.end(), pseudoId,
                         std::uint64_t{0});
            m_layoutDirty = true;
            saveMenuLayout();
            m_contextMenu->hide();
            applyDisplayModel(buildRootFolderModel(), 0, false);
            m_audio.playSfx(Sfx::ConfirmPositive);
        }});
    m_contextMenu->show(anchor, widgetTypeLabel(widget->type), std::move(items));
    m_audio.playSfx(Sfx::ModalShow);
    focusManager().setFocus(m_contextMenu.get());
}

void WiiUMenuApp::renameFolder(std::uint32_t folderId) {
    const auto* folder = m_folderStore.find(folderId);
    if (!folder) return;
    const std::string oldName = folder->name;
    auto& i18n = nxui::I18n::instance();
    requestTextEntry(i18n.tr("folder.rename", "Rename"),
                     i18n.tr("folder.rename_guide", "Rename folder"),
                     oldName, 48, false,
                     [this, folderId, oldName](const std::string& name) {
                         if (name.empty() || name == oldName) return;
                         m_folderStore.rename(folderId, name);
                         if (!saveFoldersOrReport("rename")) return;
                         if (m_openFolderId == folderId && m_folderHeaderLabel)
                             m_folderHeaderLabel->setText(name);
                         else
                             applyDisplayModel(buildRootFolderModel(),
                                               folderTitleId(folderId), false);
                     });
}

void WiiUMenuApp::requestOpenFolder(std::uint32_t folderId, std::uint64_t focusTitleId) {
    if (!m_folderStore.find(folderId) || m_folderCaptureRequested) return;
    m_requestedFolderId = folderId;
    m_folderOpenFocusTitleId = focusTitleId;
    m_folderCaptureRequested = true;
    m_folderCaptureReady = false;
    if (m_cursor) m_cursor->setVisible(false);
}

void WiiUMenuApp::flipPageFromEdge(int dir) {
    if (!m_grid || m_grid->isTransitioning())
        return;
    const int total = m_grid->totalPages();
    if (total <= 1)
        return;

    int target = (m_grid->currentPage() + dir + total) % total;

    const int cols = std::max(1, m_grid->columns());
    const int perPage = std::max(1, m_grid->iconsPerPage());
    const int global = m_grid->focusedGlobalIndex();
    const int row = global >= 0 ? (global % perPage) / cols : 0;

    m_grid->startPageTransition(target);
    kickPageArrow(dir);

    // Carry on along the same row, entering from the opposite edge.
    const int col = (dir > 0) ? 0 : cols - 1;
    if (m_grid->focusGlobalIndex(target * perPage + row * cols + col)) {
        if (auto* focused = m_grid->focusManager().current())
            focusManager().setFocus(focused);
    }
    m_audio.playSfx(Sfx::PageChange);
}

void WiiUMenuApp::syncPageIndicator() {
    if (!m_pageIndicator || !m_grid)
        return;
    // Neither carousel view pages, so the page pill has nothing to report.
    if (isCarouselLayout()) {
        m_pageIndicator->setVisible(false);
        return;
    }
    const int total = m_grid->totalPages();
    m_pageIndicator->setVisible(total > 1); // a lone page would draw an empty pill
    m_pageIndicator->setPageCount(total);
    m_pageIndicator->setCurrentPage(m_grid->currentPage());
}

// Minus cycles the enabled views on the unobstructed Home route:
// Grid -> DynamicLine -> Flow -> Grid. Grid stays reachable from every view, so
// it remains the escape hatch if a newer view misbehaves.
void WiiUMenuApp::toggleAppLayoutMode() {
    switch (m_appLayoutMode) {
        case AppLayoutMode::Grid:        setAppLayoutMode(AppLayoutMode::DynamicLine); break;
        case AppLayoutMode::DynamicLine: setAppLayoutMode(AppLayoutMode::Flow);        break;
        case AppLayoutMode::Flow:        setAppLayoutMode(AppLayoutMode::Grid);        break;
    }
}

void WiiUMenuApp::configureDynamicLineNavigation() {
    // Both carousel views use the single-row sidebar arrangement and the same
    // UP/DOWN escape targets.
    const bool dynamicLine = isCarouselLayout();
    m_sidebar.setDynamicLineLayout(dynamicLine);

    if (m_grid) {
        std::vector<nxui::Widget*> leftTargets;
        std::vector<nxui::Widget*> rightTargets;
        leftTargets.reserve(m_sidebar.leftButtons().size());
        rightTargets.reserve(m_sidebar.rightButtons().size());
        for (const auto& button : m_sidebar.leftButtons())
            leftTargets.push_back(button.get());
        for (const auto& button : m_sidebar.rightButtons())
            rightTargets.push_back(button.get());
        m_grid->setGridSideTargets(std::move(leftTargets), std::move(rightTargets));
        // UP leaves the row for the profile strip on the home line, but an open
        // folder puts its name up there instead and that name is the rename
        // control. Sending UP past it to the avatars behind the folder is what
        // made the name unreachable in this view.
        nxui::Widget* upTarget = nullptr;
        if (dynamicLine) {
            if (m_openFolderId != 0 && m_folderHeader && m_folderHeader->isFocusable())
                upTarget = m_folderHeader.get();
            else if (!m_userAvatarButtons.empty())
                upTarget = m_userAvatarButtons[m_userAvatarButtons.size() / 2].get();
        }
        m_grid->setDynamicLineUpTarget(upTarget);
        m_grid->setDynamicLineDownTarget(nullptr);
    }

    if (!dynamicLine) {
        m_sidebar.setDynamicLineDownAction({});
        wireUserAvatarNavigation();
        return;
    }

    wireUserAvatarNavigation();

    // Resolve the app when DOWN is pressed. A persistent raw pointer here can
    // outlive icons rebuilt by a move or catalogue refresh.
    m_sidebar.setDynamicLineDownAction([this]() {
        if (!m_grid || !isCarouselLayout() ||
            m_navigator.route() != switchu::navigation::Route::Home)
            return;
        auto* target = m_grid->focusManager().current();
        if (target && isCurrentFocusableWidget(target))
            focusManager().setFocus(target);
    });
}

void WiiUMenuApp::setAppLayoutMode(AppLayoutMode mode) {
    if (m_appLayoutMode == mode && m_grid && m_grid->layoutMode() == mode)
        return;
    m_appLayoutMode = mode;
    m_config.appLayoutMode = mode;
    if (m_configSaveFuture.valid())
        m_configSaveFuture.wait();
    m_configSaveFuture = m_threadPool.submit([config = m_config]() {
        config.save();
    });

    const bool rebuildRoot = m_grid && m_openFolderId == 0;
    if (m_grid && !rebuildRoot) {
        m_grid->setLayoutMode(m_appLayoutMode);
        // Both carousel views are one wrapping row, so the icon streamer holds
        // a window around the cursor instead of a page.
        m_iconStreamer.setRingMode(isCarouselLayout());
    }
    if (m_steamGridDbBackdrop)
        m_steamGridDbBackdrop->setLayoutMode(m_appLayoutMode);
    if (m_background)
        m_background->setAmbientMiisEnabled(m_appLayoutMode != AppLayoutMode::Flow);
    if (m_themeShop)
        m_themeShop->setLayoutModeState(m_appLayoutMode);

    if (rebuildRoot) {
        std::uint64_t focused = 0;
        if (auto* current = m_grid->focusManager().current();
            current && current->tag() == "glossy_icon")
            focused = static_cast<GlossyIcon*>(current)->titleId();
        applyDisplayModel(buildRootFolderModel(), focused, false);
    }
    configureDynamicLineNavigation();

    m_audio.playSfx(Sfx::ThemeToggle);

    auto& i18n = nxui::I18n::instance();
    const std::string announcement =
        (m_appLayoutMode == AppLayoutMode::DynamicLine)
            ? i18n.tr("accessibility.layout.dynamic_line", "Dynamic line mode")
        : (m_appLayoutMode == AppLayoutMode::Flow)
            ? i18n.tr("accessibility.layout.flow", "Flow mode")
            : i18n.tr("accessibility.layout.grid", "Grid mode");
    m_accessibility.announce(announcement, true, true);

    syncPageIndicator();
    updateCursor();
}

void WiiUMenuApp::openCapturedFolder() {
    const auto* folder = m_folderStore.find(m_requestedFolderId);
    if (!folder) return;
    m_openFolderId = m_requestedFolderId;
    m_requestedFolderId = 0;
    m_folderCaptureReady = false;
    const bool refocus = (m_folderOpenFocusTitleId != 0);
    if (m_folderBackdrop) m_folderBackdrop->show(refocus);
    if (m_folderHeader) {
        m_folderHeader->setVisible(true);
        // The name is the obvious thing to press to rename it, so it is a
        // control while the folder is open: reachable with UP from the top row,
        // and tappable.
        m_folderHeader->setFocusable(true);
        m_folderHeader->clearActions();
        m_folderHeader->addAction(static_cast<std::uint64_t>(nxui::Button::A),
                                  [this]() { renameFolder(m_openFolderId); });
        if (m_grid) m_grid->setGridUpTarget(m_folderHeader.get());
        // setGridUpTarget only covers the paged grid. The line keeps its own
        // UP target, and it has to be pointed at the name too.
        configureDynamicLineNavigation();
        // The profile strip is hidden behind the folder but its buttons stayed
        // focusable, and UP out of the row landed on an avatar instead of the
        // folder name. A control nobody can see is not a place focus may go.
        for (const auto& avatar : m_userAvatarButtons)
            if (avatar) avatar->setFocusable(false);
    }
    if (m_topHud) m_topHud->setVisible(false);
    if (m_leftSidebar) m_leftSidebar->setVisible(false);
    if (m_rightSidebar) m_rightSidebar->setVisible(false);
    if (m_pageIndicator)
        m_pageIndicator->setActiveColor(switchu::folders::colorForIndex(folder->colorIndex));
    if (m_folderHeaderLabel) {
        m_folderHeaderLabel->setText(folder->name);
        m_folderHeaderLabel->setTextColor(m_theme.textPrimary);
    }
    m_grid->setRect({kGridRectX, 148.f, kGridRectW, 470.f});
    applyDisplayModel(buildOpenFolderModel(m_openFolderId), m_folderOpenFocusTitleId, false);
    m_folderOpenFocusTitleId = 0;
    syncPageIndicator();
    if (m_editMode) {
        reattachEditSourceIcon();
        if (m_editSourceIndex < 0) {
            int targetSlot = 0;
            for (int i = 0; i < m_model.count(); ++i) {
                if (m_model.at(i).kind == GridEntryKind::Empty) {
                    targetSlot = i;
                    break;
                }
            }
            m_editTargetIndex = targetSlot;
            if (m_grid && targetSlot < static_cast<int>(m_grid->allIcons().size())) {
                auto* targetIcon = m_grid->allIcons()[static_cast<std::size_t>(targetSlot)].get();
                if (targetIcon) {
                    focusManager().setFocus(targetIcon);
                    bindEditActions(targetIcon);
                }
            }
            if (m_grid && m_editGhostIcon) {
                m_editGhostTargetRect = m_grid->gridSpanRect(
                    targetSlot, m_editGhostIcon->gridSpanColumns(), m_editGhostIcon->gridSpanRows());
                m_editGhostIcon->setRect(m_editGhostTargetRect);
                m_editGhostIcon->forceVisible();
            }
        }
    }
    if (!refocus)
        m_audio.playSfx(Sfx::ModalShow);
}

void WiiUMenuApp::closeFolder(bool preserveEditMode) {
    if (m_openFolderId == 0) return;
    const std::uint32_t oldId = m_openFolderId;
    if (preserveEditMode) {
        detachEditSourceIcon();
        unbindEditActions();
    }
    m_openFolderId = 0;
    if (m_folderBackdrop) m_folderBackdrop->hide();
    if (m_folderHeader) {
        m_folderHeader->setVisible(false);
        m_folderHeader->setFocusable(false);
        m_folderHeader->clearActions();
    }
    if (m_grid) m_grid->setGridUpTarget(nullptr);
    for (const auto& avatar : m_userAvatarButtons)
        if (avatar) avatar->setFocusable(true);
    // The line's UP target differs inside a folder, so it has to be recomputed
    // on the way out as well as on the way in.
    configureDynamicLineNavigation();
    if (m_topHud) m_topHud->setVisible(true);
    if (m_leftSidebar) m_leftSidebar->setVisible(true);
    if (m_rightSidebar) m_rightSidebar->setVisible(true);
    if (m_pageIndicator)
        m_pageIndicator->clearActiveColor();
    m_grid->setRect({kGridRectX, kGridRectY, kGridRectW, kGridRectH});
    applyDisplayModel(buildRootFolderModel(), folderTitleId(oldId), false);
    syncPageIndicator();
    if (preserveEditMode) {
        reattachEditSourceIcon();
        if (m_editSourceIndex >= 0 && m_grid) {
            m_editTargetIndex = m_editSourceIndex;
            if (m_editSourceIndex < static_cast<int>(m_grid->allIcons().size())) {
                auto* targetIcon = m_grid->allIcons()[static_cast<std::size_t>(m_editSourceIndex)].get();
                if (targetIcon) {
                    focusManager().setFocus(targetIcon);
                    bindEditActions(targetIcon);
                }
            }
            if (m_editGhostIcon) {
                m_editGhostTargetRect = m_grid->gridSpanRect(
                    m_editSourceIndex, m_editGhostIcon->gridSpanColumns(), m_editGhostIcon->gridSpanRows());
                m_editGhostIcon->setRect(m_editGhostTargetRect);
                m_editGhostIcon->forceVisible();
            }
        }
        m_titlePill->setText(nxui::I18n::instance().tr("game.move_prefix", "Move: ") + m_editHeldTitle);
        m_titlePill->setVisible(true);
    }
    m_audio.playSfx(Sfx::ModalHide);
}

#ifdef SWITCHU_MENU
// The recently-played and playtime tiles read m_widgetStore, and only
// activateApplication() ever wrote it. The handler installed by makeIcon() is
// the path A on a game icon actually takes, and it wrote m_config alone, so the
// store stayed empty however many games were played: after a launch logged at
// [2026-08-31 16:52:45.4] the card still held recentActivity.titleId = 0 with no
// "[widgets] saved" line behind it. Every launch path goes through here now.
void WiiUMenuApp::commitLaunchRecency(std::uint64_t titleId, const std::string& title) {
    if (titleId == 0)
        return;
    if (!isNativeApplicationId(titleId) && !m_config.isGamePort(titleId)) {
        if (m_widgetStore.recentActivity().titleId == titleId) {
            m_widgetStore.clearRecentActivity();
            m_widgetStore.save();
        }
        return;
    }
    m_widgetStore.recordLaunch(titleId, title,
                               static_cast<std::int64_t>(std::time(nullptr)));
    // WidgetStore::save() commits the SD card itself, which matters because the
    // menu process is destroyed moments later for the title handoff.
    if (!m_widgetStore.save())
        DebugLog::log("[widgets] recent activity could not be saved tid=%016lX",
                      static_cast<unsigned long>(titleId));
}

void WiiUMenuApp::activateApplication(GlossyIcon* source, AppEntry* entry,
                                      std::uint64_t titleId,
                                      const std::string& launchTitle) {
    if (titleId == 0) return;
    if (!source && m_grid) {
        for (auto& icon : m_grid->allIcons()) {
            if (icon && icon->titleId() == titleId) {
                source = icon.get();
                break;
            }
        }
    }
    if (m_launcher.isAppSuspended(titleId)) {
        switchu::smi::LaunchTransitionTrace transitionTrace{};
        transitionTrace.activation_tick = armGetSystemTick();
        transitionTrace.user_selected_tick = transitionTrace.activation_tick;
        m_audio.playSfx(Sfx::LaunchGame);
        commitLaunchRecency(titleId, launchTitle);
        const nxui::Rect focusR = source ? source->focusRect() : nxui::Rect{640.0f - 64.0f, 360.0f - 64.0f, 128.0f, 128.0f};
        const nxui::Texture* tex = source ? source->texture() : nullptr;
        const float r = source ? source->cornerRadius() : 18.0f;
        m_launchAnim->start(focusR, tex,
            r, m_theme.panelBase, m_theme.panelBorder,
            0, {}, nullptr,
            [this, transitionTrace]() mutable {
                transitionTrace.animation_complete_tick = armGetSystemTick();
                transitionTrace.recency_commit_complete_tick =
                    transitionTrace.animation_complete_tick;
                m_launcher.resumeApplication(transitionTrace);
            });
        return;
    }

    if (entry && !entry->isLaunchable()) {
        m_audio.playSfx(Sfx::ModalShow);
        m_dialogReturnFocus = source;
        std::string reason;
        auto& i18n = nxui::I18n::instance();
        if (entry->isGameCardNotInserted())
            reason = i18n.tr("error.gamecard_not_inserted", "Game card is not inserted.");
        else if (entry->needsVerify())
            reason = i18n.tr("error.needs_verify", "Game data needs verification.");
        else if (entry->needsUpdate())
            reason = i18n.tr("error.needs_update", "A required update is available.");
        else if (!entry->hasContents())
            reason = i18n.tr("error.no_contents", "Game data is missing.");
        else
            reason = i18n.tr("error.cannot_launch", "This game cannot be launched.");
        m_dialog->show(i18n.tr("error.title", "Cannot Launch"), reason,
                       {{i18n.tr("button.ok", "OK"), [this]() {}, true}}, 0, {});
        focusManager().setFocus(m_dialog.get());
        return;
    }

    const nxui::Rect frame = source ? source->focusRect() : nxui::Rect{640.0f - 64.0f, 360.0f - 64.0f, 128.0f, 128.0f};
    const nxui::Texture* texture = source ? source->texture() : nullptr;
    const float radius = source ? source->cornerRadius() : 18.0f;
    const nxui::Color base = m_theme.panelBase;
    const nxui::Color border = m_theme.panelBorder;
    auto startLaunch = [this, frame, texture, radius, base, border,
                        titleId, launchTitle](AccountUid uid) {
        switchu::smi::LaunchTransitionTrace transitionTrace{};
        transitionTrace.activation_tick = armGetSystemTick();
        transitionTrace.user_selected_tick = transitionTrace.activation_tick;
        m_launcher.prepareApplication(titleId, uid, transitionTrace);
        themeshop::http::cancelPendingRequests();
        if (m_configSaveFuture.valid())
            m_configSaveFuture.get();
        const std::uint64_t openedAt = m_config.nextLastOpenedAt();
        m_config.noteOpened(titleId, openedAt);
        m_configSaveFuture = m_threadPool.submit(
            [cfg = m_config, titleId, openedAt]() {
                if (!cfg.save())
                    DebugLog::log("[menu] could not save last-opened title=%016lX", titleId);
                switchu::commitSdCard("last opened");
            });
        transitionTrace.recency_submit_tick = armGetSystemTick();
        m_audio.playSfx(Sfx::LaunchGame);
        // Same reason as the resume path above: commit the recency before the
        // animation rather than from its completion callback.
        commitLaunchRecency(titleId, launchTitle);
        m_launchAnim->start(frame, texture, radius, base, border, titleId, uid,
            [this, transitionTrace](std::uint64_t id,
                                    AccountUid selectedUid) mutable {
                transitionTrace.animation_complete_tick = armGetSystemTick();
                if (m_configSaveFuture.valid())
                    m_configSaveFuture.get();
                transitionTrace.recency_commit_complete_tick = armGetSystemTick();
                m_launcher.launchApplication(id, selectedUid, transitionTrace);
            });
    };

    if (entry) {
        if (!entry->startupUserKnown) {
            entry->startupUserAccount = 1;
            entry->startupUserAccountOption = 0;
            entry->userRequired = true;
        }
        DebugLog::log("[launcher] user decision tid=%016lX startup_user=%u option=%u interactive_user=%d",
                      titleId, (unsigned)entry->startupUserAccount,
                      (unsigned)entry->startupUserAccountOption,
                      entry->userRequired ? 1 : 0);

        if (entry->startupUserAccount == 0) {
            AccountUid emptyUid{};
            startLaunch(emptyUid);
            return;
        }
        if (m_config.defaultProfileEnabled) {
            AccountUid defaultUid{};
            if (hexToAccountUid(m_config.defaultProfileUid, defaultUid)) {
                startLaunch(defaultUid);
                return;
            }
        }

        AccountUid silentUid{};
        const bool networkRequired = entry->startupUserAccount == 2;
        const Result silentResult = accountTrySelectUserWithoutInteraction(
            &silentUid, networkRequired);
        if (R_SUCCEEDED(silentResult) && accountUidIsValid(&silentUid)) {
            startLaunch(silentUid);
            return;
        }
        if (!entry->userRequired) {
            AccountUid emptyUid{};
            startLaunch(emptyUid);
            return;
        }
    }

    if (m_userSelect) {
        const bool usersLoaded = m_userSelect->loadUsers(app().gpu(), app().renderer());
        if (usersLoaded) m_audio.playSfx(Sfx::ModalShow);
        m_userSelect->showUserSelect(
            [startLaunch](AccountUid uid) { startLaunch(uid); });
        focusManager().setFocus(m_userSelect.get());
    }
}
#endif

std::shared_ptr<GlossyIcon> WiiUMenuApp::makeIcon(const AppEntry& entry) {
    auto icon = std::make_shared<GlossyIcon>();

    // The second cell of a 2x1 tile. It carries no title id, so it used to fall
    // into the empty-slot branch below, which leaves m_entryKind at its default
    // and keeps the icon focusable and visible. GlossyIcon::onRender draws the
    // drop shadow and the glass panel before it looks at the kind, so that cell
    // painted a one-cell panel over the right half of every 2x1 widget: the
    // "1x1 icon cutting it in half". The move ghost renders through a different
    // path, which is why the tile looked correct only while being moved. The
    // instrumentation showed the span was never lost ÃƒÂ¢Ã¢â€šÂ¬Ã¢â‚¬Â the model reported
    // span=2x1 and layoutPage assigned rect 320x150 against a 150x150 cell.
    // bindGridNavigation already skips icons that are not focusable or not
    // visible, and it is span-aware, so hiding this one keeps navigation intact.
    if (entry.kind == GridEntryKind::WidgetContinuation) {
        icon->setTag("glossy_icon");
        icon->setEntryKind(GridEntryKind::WidgetContinuation);
        icon->setTitleId(0);
        icon->setFocusable(false);
        icon->setVisible(false);
        return icon;
    }

    if (entry.titleId == 0) {
        icon->setTag("glossy_icon");
        icon->setEntryKind(GridEntryKind::Empty);
        icon->setTitle("");
        icon->setTitleId(0);
        // The dynamic line compacts real entries to the front and pads the rest
        // with empty slots. Leaving those focusable gave the carousel a run of
        // blank cells past the last game, which is the "last icon" the cycle kept
        // stopping on. They are not offered in this mode.
        const bool lineMode = isCarouselLayout();
        icon->setFocusable(!lineMode);
        icon->setVisible(!lineMode);
        auto& i18n = nxui::I18n::instance();
        icon->setAccessibilityLabel(i18n.tr("accessibility.grid.empty_slot", "Empty slot"));
        icon->setAccessibilityRole(i18n.tr("accessibility.roles.slot", "slot"));
        icon->setAccessibilityHint(i18n.tr("accessibility.hints.grid_empty", "Use the directional pad to move to another slot."));
        icon->setNotLaunchable(false);
        icon->setCornerRadius(m_theme.iconCornerRadius);
        return icon;
    }

    // Folders and widgets are not applications, and nothing in the tree called
    // setEntryKind, setWidgetData, setFolderPreviewCount or
    // setBatteryIconTextures: the 1.2 presentation layer was merged in but this
    // factory was left as the fork wrote it. Every folder and widget therefore
    // took the application path below, which gave them no texture ÃƒÂ¢Ã¢â€šÂ¬Ã¢â‚¬Â so the
    // loading spinner span forever ÃƒÂ¢Ã¢â€šÂ¬Ã¢â‚¬Â and an activation that asked the launcher
    // to start a title id that is not a title. That is the user picker followed
    // by the whole menu being recreated on page one.
    if (entry.isFolder()) {
        auto& folderI18n = nxui::I18n::instance();
        icon->setTag("glossy_icon");
        icon->setEntryKind(GridEntryKind::Folder);
        icon->setFont(&m_fontNormal);
        icon->setTitle(entry.title);
        icon->setTitleId(entry.titleId);
        icon->setFolderPreviewCount(entry.folderPreviewCount);
        icon->setFolderColorIndex(entry.folderColorIndex);
        icon->setFolderVisualSeed(entry.folderId);
        icon->setGridSpan(entry.widgetColumns, entry.widgetRows);
        icon->setCornerRadius(m_theme.iconCornerRadius);
        icon->setLoadingColor(m_theme.cursorNormal);
        icon->setNotLaunchable(false);
        icon->setFocusable(true);
        icon->setAccessibilityLabel(entry.title);
        icon->setAccessibilityRole(folderI18n.tr("folder.default_name", "Folder"));
        icon->setAccessibilityHint(folderI18n.tr(
            "folder.open_hint", "A to open. Plus for folder options. Y to move."));
        const std::uint32_t folderId = entry.folderId;
        icon->setOnActivate([this, folderId]() {
            m_audio.playSfx(Sfx::Activate);
            requestOpenFolder(folderId);
        });
        return icon;
    }

    if (entry.isWidget()) {
        auto& widgetI18n = nxui::I18n::instance();
        icon->setTag("glossy_icon");
        icon->setEntryKind(GridEntryKind::Widget);
        icon->setFont(&m_fontNormal);
        icon->setTitle(entry.title);
        icon->setTitleId(entry.titleId);
        icon->setGridSpan(entry.widgetColumns, entry.widgetRows);
        icon->setCornerRadius(m_theme.iconCornerRadius);
        icon->setLoadingColor(m_theme.cursorNormal);
        icon->setNotLaunchable(true);
        icon->setFocusable(true);
        icon->setBatteryIconTextures(&m_batteryConsoleTex,
                                      &m_batteryJoyconLeftTex,
                                      &m_batteryJoyconRightTex,
                                      &m_batteryControllerTex);
        icon->setConsoleBattery(m_consoleBatteryPercent, m_consoleBatteryCharging);
        icon->setWidgetHeader(widgetTypeLabel(entry.widgetType));

        std::string primary;
        std::string secondary;
        if (entry.widgetType == switchu::widgets::WidgetType::RecentlyPlayed) {
            const auto& recent = m_widgetStore.recentActivity();
            primary = recent.titleId != 0
                ? recent.title
                : widgetI18n.tr("widget.no_recent_game", "No recent game");
            secondary = recent.titleId != 0
                ? widgetI18n.tr("widget.last_played", "Last played") : std::string();
        } else if (entry.widgetType == switchu::widgets::WidgetType::RecentPlaytime) {
            primary = widgetDurationLabel(m_widgetStore.recentActivity().recentSeconds);
            secondary = widgetI18n.tr("widget.played_recently", "Played recently");
        }

        const std::string assetPath =
            entry.widgetType == switchu::widgets::WidgetType::RandomScreenshot
                ? randomScreenshotPath(entry.widgetId)
                : resolveWidgetAssetRef(entry.widgetAssetRef);
        icon->setWidgetData(entry.widgetType, entry.widgetColumns, entry.widgetRows,
                            std::move(primary), std::move(secondary), assetPath,
                            &app().gpu(), &app().renderer(), true);
        icon->setAccessibilityLabel(widgetTypeLabel(entry.widgetType));
        icon->setAccessibilityRole(widgetI18n.tr("widget.title", "Widget"));
        icon->setAccessibilityHint(widgetI18n.tr(
            "accessibility.hints.widget",
            "Plus for widget options. Y to move. Minus to change view."));
#ifdef SWITCHU_MENU
        // A on the recently-played tile starts that game, which is what the tile
        // is for. It resolves the real application first: the launcher must never
        // be handed the synthetic widget id, which is what recreated the menu.
        // Every other widget stays inert; their actions live on Plus.
        if (entry.widgetType == switchu::widgets::WidgetType::RecentlyPlayed) {
            GlossyIcon* rawWidget = icon.get();
            icon->setOnActivate([this, rawWidget]() {
                const auto& activity = m_widgetStore.recentActivity();
                if (activity.titleId == 0)
                    return;
                const auto found = std::find_if(
                    m_allApps.begin(), m_allApps.end(),
                    [&activity](const AppEntry& app) {
                        return app.titleId == activity.titleId;
                    });
                if (found == m_allApps.end())
                    return;
                activateApplication(rawWidget, &(*found), found->titleId,
                                    found->title);
            });
        }
#endif
        DebugLog::log("[widget-tile] id=%u type=%d span=%dx%d",
                      entry.widgetId, static_cast<int>(entry.widgetType),
                      entry.widgetColumns, entry.widgetRows);
        return icon;
    }

    icon->setTag("glossy_icon");
    icon->setFont(&m_fontNormal);
    icon->setTitle(entry.title);
    icon->setTitleId(entry.titleId);
    icon->setAccessibilityLabel(entry.title);
    auto& i18n = nxui::I18n::instance();
    icon->setAccessibilityRole(entry.isGameCard()
        ? i18n.tr("accessibility.roles.game_card", "game card")
        : i18n.tr("accessibility.roles.game", "game"));
    icon->setAccessibilityHint(entry.isLaunchable()
        ? i18n.tr("accessibility.hints.game_launchable", "A to launch. X for options. Y to move. ZL or ZR to change page.")
        : i18n.tr("accessibility.hints.game_blocked", "A to show why this item is blocked."));
    // Texture is set by IconStreamer::onPageChanged() ÃƒÂ¢Ã¢â€šÂ¬Ã¢â‚¬Â not here.
    icon->setCornerRadius(m_theme.iconCornerRadius);
    icon->setLoadingColor(m_theme.cursorNormal);
    icon->setIsGameCard(entry.isGameCard());
    icon->setGameCardTexture(&m_gameCardTex);
    icon->setNotLaunchable(!entry.isLaunchable());
    icon->setFavorite(entry.isFavorite);
    icon->setPlaytimeBadge(playtimeBadgeFor(entry));
    icon->setGridSpan(entry.widgetColumns, entry.widgetRows);
    if (entry.widgetColumns > 1 && entry.widgetRows == 1 &&
        m_appLayoutMode == AppLayoutMode::Grid) {
        ensureGameArtwork(entry.titleId);
        const auto artwork = m_gameArtwork.find(entry.titleId);
        if (artwork != m_gameArtwork.end())
            icon->setWideGameTextures(artwork->second.hero.get(),
                                      artwork->second.logo.get());
    }

#ifdef SWITCHU_MENU
    if (m_launcher.suspendedTitleId() != 0 &&
        entry.titleId == m_launcher.suspendedTitleId())
        icon->setSuspended(true);

    GlossyIcon* raw = icon.get();
    icon->setOnActivate([this, raw]() {
        switchu::smi::LaunchTransitionTrace transitionTrace{};
        transitionTrace.activation_tick = armGetSystemTick();
        uint64_t tid = raw->titleId();
        if (m_launcher.isAppSuspended(tid)) {
            nxui::Rect   fr   = raw->focusRect();
            const nxui::Texture* tex = raw->texture();
            float  cr   = raw->cornerRadius();
            nxui::Color  base = m_theme.panelBase;
            nxui::Color  bord = m_theme.panelBorder;
            transitionTrace.user_selected_tick = transitionTrace.activation_tick;
            const std::string resumeTitle = raw->title();
            auto continueResume = [this, fr, tex, cr, base, bord, tid,
                                   resumeTitle, transitionTrace]() mutable {
                commitLaunchRecency(tid, resumeTitle);
                m_audio.playSfx(Sfx::LaunchGame);
                m_launchAnim->start(fr, tex, cr, base, bord, 0, {},
                    nullptr,
                    [this, transitionTrace]() mutable {
                        transitionTrace.animation_complete_tick = armGetSystemTick();
                        transitionTrace.recency_commit_complete_tick =
                            transitionTrace.animation_complete_tick;
                        m_launcher.resumeApplication(transitionTrace);
                    });
            };
#ifdef SWITCHU_RESUME_FAILURE_TEST
            m_audio.playSfx(Sfx::ModalShow);
            m_dialogReturnFocus = raw;
            DebugLog::log("[diagnostic-resume] dialog armed tid=%016lX", tid);
            m_dialog->show(
                "Resume recovery test",
                "DIAGNOSTIC BUILD. Failure recovery keeps the suspended title "
                "alive. After SwitchU returns, select it again and choose Resume selected.",
                {
                    {"Resume selected", [continueResume]() mutable {
                        continueResume();
                    }, true},
                    {"Failure recovery", [this, tid, transitionTrace]() mutable {
                        auto failureTrace = transitionTrace;
                        const uint64_t now = armGetSystemTick();
                        failureTrace.animation_complete_tick = now;
                        failureTrace.recency_commit_complete_tick = now;
                        DebugLog::log(
                            "[diagnostic-resume] requesting synthetic foreground failure tid=%016lX",
                            tid);
                        m_launcher.resumeApplicationFailureDiagnostic(failureTrace);
                    }, true},
                    {"Cancel", []() {}, true},
                },
                0,
                {});
            focusManager().setFocus(m_dialog.get());
#else
            continueResume();
#endif
        } else {
            AppEntry* entry = nullptr;
            int entryIndex = findTitleIndex(tid);
            if (entryIndex >= 0)
                entry = &m_model.at(entryIndex);
            if (entry && !entry->isLaunchable()) {
                m_audio.playSfx(Sfx::ModalShow);
                m_dialogReturnFocus = raw;
                std::string reason;
                auto& i18n = nxui::I18n::instance();
                if (entry->isGameCardNotInserted())
                    reason = i18n.tr("error.gamecard_not_inserted", "Game card is not inserted.");
                else if (entry->needsVerify())
                    reason = i18n.tr("error.needs_verify", "Game data needs verification.");
                else if (entry->needsUpdate())
                    reason = i18n.tr("error.needs_update", "A required update is available.");
                else if (!entry->hasContents())
                    reason = i18n.tr("error.no_contents", "Game data is missing.");
                else
                    reason = i18n.tr("error.cannot_launch", "This game cannot be launched.");
                m_dialog->show(
                    i18n.tr("error.title", "Cannot Launch"),
                    reason,
                    {{i18n.tr("button.ok", "OK"), [this]() {}, true}},
                    0, {}
                );
                focusManager().setFocus(m_dialog.get());
                return;
            }

            nxui::Rect   fr   = raw->focusRect();
            const nxui::Texture* tex = raw->texture();
            float  cr   = raw->cornerRadius();
            nxui::Color  base = m_theme.panelBase;
            nxui::Color  bord = m_theme.panelBorder;
            const std::string launchTitle = raw->title();
            auto startLaunch = [this, raw, fr, tex, cr, base, bord, tid,
                                 launchTitle, transitionTrace](AccountUid uid) mutable {
                transitionTrace.user_selected_tick = armGetSystemTick();
                // Ask the persistent daemon to touch NS and ensure title save
                // data while this process renders the acknowledgement. The
                // final launch command remains authoritative and repeats this
                // work if the bounded preflight cannot be reused safely.
                m_launcher.prepareApplication(tid, uid, transitionTrace);
                auto continueLaunch = [this, fr, tex, cr, base, bord, tid, uid,
                                       launchTitle, transitionTrace]() mutable {
                    themeshop::http::cancelPendingRequests();
                    // Persist recency while the visual acknowledgement runs.
                    // The handoff still waits for this write and SD commit,
                    // preserving the existing durability guarantee without
                    // putting all of their latency after the animation.
                    if (m_configSaveFuture.valid())
                        m_configSaveFuture.get();
                    const std::uint64_t openedAt = m_config.nextLastOpenedAt();
                    m_config.noteOpened(tid, openedAt);
                    m_configSaveFuture = m_threadPool.submit(
                        [cfg = m_config, tid, openedAt]() {
                            if (!cfg.save())
                                DebugLog::log("[menu] could not save last-opened title=%016lX", tid);
                            switchu::commitSdCard("last opened");
                            DebugLog::log("[menu] last-opened title=%016lX at=%llu", tid,
                                         (unsigned long long)openedAt);
                        });
                    commitLaunchRecency(tid, launchTitle);
                    transitionTrace.recency_submit_tick = armGetSystemTick();

                    m_audio.playSfx(Sfx::LaunchGame);
                    m_launchAnim->start(fr, tex, cr, base, bord, tid, uid,
                        [this, transitionTrace](uint64_t id, AccountUid u) mutable {
                            transitionTrace.animation_complete_tick = armGetSystemTick();
                            if (m_configSaveFuture.valid())
                                m_configSaveFuture.get();
                            transitionTrace.recency_commit_complete_tick = armGetSystemTick();
                            m_launcher.launchApplication(id, u, transitionTrace);
                        });
                };

#ifdef SWITCHU_PREFLIGHT_EDGE_TEST
                m_audio.playSfx(Sfx::ModalShow);
                m_dialogReturnFocus = raw;
                m_preflightEdgePending = true;
                m_preflightEdgeLockObserved = false;
                DebugLog::log(
                    "[diagnostic-preflight-edge] dialog armed tid=%016lX",
                    tid);
                m_dialog->show(
                    "Preflight edge test",
                    "DIAGNOSTIC BUILD. Sleep test: leave this open, press POWER, "
                    "wake and unlock, then choose Launch selected. Failure test: "
                    "choose Failure recovery and wait for the menu to return.",
                    {
                        {"Launch selected", [this, continueLaunch]() mutable {
                            DebugLog::log(
                                "[diagnostic-preflight-edge] launch after wake observed=%d",
                                m_preflightEdgeLockObserved ? 1 : 0);
                            m_preflightEdgePending = false;
                            m_preflightEdgeLockObserved = false;
                            continueLaunch();
                        }, true},
                        {"Failure recovery", [this, tid, uid, transitionTrace]() mutable {
                            auto failureTrace = transitionTrace;
                            const uint64_t now = armGetSystemTick();
                            failureTrace.animation_complete_tick = now;
                            failureTrace.recency_commit_complete_tick = now;
                            m_preflightEdgePending = false;
                            m_preflightEdgeLockObserved = false;
                            DebugLog::log(
                                "[diagnostic-preflight-edge] requesting synthetic failure tid=%016lX",
                                tid);
                            m_launcher.launchApplicationFailureDiagnostic(
                                tid, uid, failureTrace);
                        }, true},
                        {"Cancel", [this]() {
                            m_preflightEdgePending = false;
                            m_preflightEdgeLockObserved = false;
                        }, true},
                    },
                    0,
                    [this]() {
                        m_preflightEdgePending = false;
                        m_preflightEdgeLockObserved = false;
                    });
                focusManager().setFocus(m_dialog.get());
#else
                continueLaunch();
#endif
            };
            if (entry) {
                if (!entry->startupUserKnown) {
                    entry->startupUserAccount = 1;
                    entry->startupUserAccountOption = 0;
                    entry->userRequired = true;
                }
                DebugLog::log("[launcher] user decision tid=%016lX startup_user=%u option=%u interactive_user=%d",
                              tid,
                              (unsigned)entry->startupUserAccount,
                              (unsigned)entry->startupUserAccountOption,
                              entry->userRequired ? 1 : 0);

                if (entry->startupUserAccount == 0) {
                    AccountUid emptyUid = {};
                    DebugLog::log("[launcher] skipping user select: NACP StartupUserAccount=None");
                    startLaunch(emptyUid);
                    return;
                }

                if (m_config.defaultProfileEnabled) {
                    AccountUid defaultUid = {};
                    if (hexToAccountUid(m_config.defaultProfileUid, defaultUid)) {
                        DebugLog::log("[launcher] skipping user select: default profile configured uid[0]=0x%016lX uid[1]=0x%016lX",
                                      defaultUid.uid[0], defaultUid.uid[1]);
                        startLaunch(defaultUid);
                        return;
                    }
                    DebugLog::log("[launcher] default profile enabled but uid is invalid");
                }

                AccountUid silentUid = {};
                const bool networkRequired = entry->startupUserAccount == 2;
                Result silentRc = accountTrySelectUserWithoutInteraction(&silentUid, networkRequired);
                DebugLog::log("[launcher] TrySelectUserWithoutInteraction network_required=%d rc=0x%X uid_valid=%d uid[0]=0x%016lX uid[1]=0x%016lX",
                              networkRequired ? 1 : 0,
                              silentRc,
                              accountUidIsValid(&silentUid) ? 1 : 0,
                              silentUid.uid[0],
                              silentUid.uid[1]);
                if (R_SUCCEEDED(silentRc) && accountUidIsValid(&silentUid)) {
                    DebugLog::log("[launcher] skipping user select: silent account selection succeeded");
                    startLaunch(silentUid);
                    return;
                }

                if (!entry->userRequired) {
                    AccountUid emptyUid = {};
                    DebugLog::log("[launcher] skipping user select fallback: startup_user=%u option=%u did not require interactive picker",
                                  (unsigned)entry->startupUserAccount,
                                  (unsigned)entry->startupUserAccountOption);
                    startLaunch(emptyUid);
                    return;
                }
            }
            if (m_userSelect) {
                bool usersLoaded = m_userSelect->loadUsers(app().gpu(), app().renderer());
                DebugLog::log("[UserSelect] lazy load result=%d", usersLoaded ? 1 : 0);
                if (usersLoaded)
                    m_audio.playSfx(Sfx::ModalShow);
            }
            // The picker joins the overlay tree at startup, while the dossier,
            // gallery and mods screens are created on demand and land after it.
            // Once any of those exists it covers the picker, which then takes
            // focus without ever being drawn: the grid appears frozen under a
            // menu nobody can see. Move it back to the end before showing.
            raiseOverlay(m_userSelect);
            m_userSelect->showUserSelect([startLaunch](AccountUid uid) mutable { startLaunch(uid); });
            focusManager().setFocus(m_userSelect.get());
        }
    });
#else
    icon->setOnActivate([this]() {
        m_audio.playSfx(Sfx::Activate);
    });
#endif
    return icon;
}

void WiiUMenuApp::buildGrid() {
    reloadThemePresets();

    m_activePresetName = m_config.themePreset;
    ThemePreset* preset = findPresetPtr(m_activePresetName);
    if (!preset) {
        m_activePresetName = "builtin:Default Dark";
        preset = findPresetPtr(m_activePresetName);
    }
    if (!preset) {
        m_activePresetName = "Default Dark";
        preset = findPresetPtr(m_activePresetName);
    }

    if (preset)
        m_activePresetName = preset->id.empty() ? preset->name : preset->id;

    m_activeColors = preset->colors;
    m_activeMode = preset->mode;

    m_effectivePreset = buildEffectiveThemePreset();
    m_theme = m_effectivePreset.toTheme();

    m_background = std::make_shared<WaraWaraBackground>();
    m_background->setRect({0, 0, 1280, 720});
    m_gameArtworkBackdrop = std::make_shared<GameArtworkBackdrop>();
    m_gameArtworkBackdrop->setRect({0, 0, 1280, 720});
    applyThemeResources(m_effectivePreset);

    // Compose the model the same way every later rebuild does. buildGrid() used
    // m_model straight from the loader, which is the raw slot arrangement: real
    // entries wherever the saved layout puts them and empty slots in between.
    // The grid view hides that, but the dynamic line renders slot order, so a
    // restart into the line put the focused title next to empty slots and drew
    // nothing on either side of it, with no folders or widgets. buildRootFolderModel()
    // merges folders and widgets and compacts the result for the line.
    m_model = buildRootFolderModel();
    // AppListLoader::finalize() seeded the streamer from the loader's model, so
    // replacing that model above leaves the streamer's title table indexed
    // against the old arrangement. It then looked up the wrong entry for every
    // icon: the log showed only native titles 0..6 ever scheduled and no
    // homebrew at all, because the compacted line puts them at indices the old
    // table holds zeros for. applyDisplayModel() reconciles for exactly this
    // reason on every later rebuild; boot has to do it too.
    {
        std::vector<std::uint64_t> titleIds;
        titleIds.reserve(static_cast<std::size_t>(std::max(0, m_model.count())));
        for (int i = 0; i < m_model.count(); ++i) {
            const auto& entry = m_model.at(i);
            titleIds.push_back(entry.isApplication() ? entry.titleId : 0);
        }
        m_iconStreamer.reconcileTitleIds(titleIds);
    }

    std::vector<std::shared_ptr<GlossyIcon>> icons;
    for (int i = 0; i < m_model.count(); ++i)
        icons.push_back(makeIcon(m_model.at(i)));

    GridLayoutMetrics gridMetrics = computeGridLayoutMetrics();

    m_grid = std::make_shared<IconGrid>();
    m_grid->setRect({kGridRectX, kGridRectY, kGridRectW, kGridRectH});
    m_grid->setThreadPool(&m_threadPool);
    if (m_background)
        m_background->setAmbientMiisEnabled(m_appLayoutMode != AppLayoutMode::Flow);
    // buildGrid() creates the grid and calls setup() directly, without going
    // through applyDisplayModel(), which is the only other place that sets this.
    // The grid therefore stayed in its default page mode after a restart while
    // m_appLayoutMode said dynamic line, and setup() laid the icons out as pages
    // and bound page navigation for a view the rest of the menu treated as a
    // line. That is the restart with no working selection: the two disagreed
    // from the first frame, which is exactly the mismatch setAppLayoutMode()
    // guards against when it compares m_grid->layoutMode() with its argument.
    m_grid->setLayoutMode(m_appLayoutMode);
    // The line is a ring, so the streamer's window has to wrap with it.
    m_iconStreamer.setRingMode(isCarouselLayout());
    m_grid->setup(std::move(icons),
                  std::clamp(m_config.gridColumns, 3, 8),
                  std::clamp(m_config.gridRows, 2, 5),
                  gridMetrics.cellW, gridMetrics.cellH,
                  gridMetrics.padX, gridMetrics.padY);

    m_cursor = std::make_shared<SelectionCursor>();
    m_pointerCursor = std::make_shared<SelectionCursor>();
    m_pointerCursor->setVisible(false);

    m_clock = std::make_shared<DateTimeWidget>();
    m_clock->setSize(150, 62);
    m_clock->setMarginTop(14.f);
    m_clock->setMarginLeft(24.f);
    m_clock->setFont(&m_fontNormal);
    m_clock->setSmallFont(&m_fontSmall);
    m_clock->setClockService(&m_clockService);
    m_clock->setUse12HourClock(m_config.clockUse12Hour);
    m_clock->setCornerRadius(m_theme.cellCornerRadius);
    m_clock->setForceLiquidGlass(true);
    m_clock->setBlurEnabled(false);

    m_battery = std::make_shared<BatteryWidget>();
    m_battery->setMarginTop(14.f);
    m_battery->setMarginRight(24.f);
    m_battery->setSize(150, 62);
    m_battery->setFont(&m_fontSmall);
    m_battery->setCornerRadius(m_theme.cellCornerRadius);
    m_battery->setForceLiquidGlass(true);
    m_battery->setBlurEnabled(false);

    buildUserAvatarBar();

    m_titlePill = std::make_shared<TitlePillWidget>();
    m_titlePill->setPosition(0, 630.f);
    m_titlePill->setFont(&m_fontNormal);
    m_titlePill->setPadding(9.f, 22.f, 9.f, 22.f);
    m_titlePill->setForceLiquidGlass(true);
    m_titlePill->setBlurEnabled(false);

    m_pageIndicator = std::make_shared<PageIndicator>();
    m_pageIndicator->setRect({0, 685.f, 1280.f, 28.f});
    m_pageIndicator->setTheme(&m_theme);
    m_pageIndicator->setForceLiquidGlass(true);
    m_pageIndicator->setBlurEnabled(false);

    m_launchAnim = std::make_shared<LaunchAnimation>();

    m_userSelect = std::make_shared<OverlayDialog>();
    m_userSelect->setFont(&m_fontNormal);
    m_userSelect->setSmallFont(&m_fontSmall);
    m_userSelect->setTheme(&m_theme);
    m_userSelect->setAccessibilitySpeechPreferences(m_config.accessibilitySpeakHints,
                                                    m_config.accessibilitySpeakPosition);
    m_userSelect->onNavigateSfx([this]() { m_audio.playSfx(Sfx::Navigate); });
    m_userSelect->onActivateSfx([this]() { m_audio.playSfx(Sfx::Activate); });
    m_userSelect->onCloseSfx([this]() { m_audio.playSfx(Sfx::ModalHide); });
    m_userSelect->onAccessibilityAnnouncement([this](const std::string& text) {
        m_accessibility.announce(text);
    });
    m_userSelect->onAccessibilityStructuredAnnouncement([this](const std::string& context,
                                                               const std::string& position,
                                                               const std::string& summary,
                                                               bool forceRepeat,
                                                               bool forceContext) {
        m_accessibility.announceStructuredFocus(context, position, summary, forceRepeat, forceContext);
    });

    m_dialog = std::make_shared<OverlayDialog>();
    m_dialog->setFont(&m_fontNormal);
    m_dialog->setSmallFont(&m_fontSmall);
    m_dialog->setTheme(&m_theme);
    m_dialog->setAccessibilitySpeechPreferences(m_config.accessibilitySpeakHints,
                                                m_config.accessibilitySpeakPosition);
    m_dialog->onNavigateSfx([this]() { m_audio.playSfx(Sfx::Navigate); });
    m_dialog->onActivateSfx([this]() { m_audio.playSfx(Sfx::Activate); });
    m_dialog->onCloseSfx([this]() { m_audio.playSfx(Sfx::ModalHide); });
    m_dialog->onAccessibilityAnnouncement([this](const std::string& text) {
        m_accessibility.announce(text);
    });
    m_dialog->onAccessibilityStructuredAnnouncement([this](const std::string& context,
                                                           const std::string& position,
                                                           const std::string& summary,
                                                           bool forceRepeat,
                                                           bool forceContext) {
        m_accessibility.announceStructuredFocus(context, position, summary, forceRepeat, forceContext);
    });

    m_progressDialog = std::make_shared<ProgressDialog>();
    m_progressDialog->setFont(&m_fontNormal);
    m_progressDialog->setSmallFont(&m_fontSmall);
    m_progressDialog->setTheme(&m_theme);

    app().renderer().setBoxWireframeEnabled(m_showWireframe);

    wireFocusCallback();
    m_grid->onPageSwitched([this]() {
        if (m_editMode && m_editTargetIndex >= 0) {
            const int perPage = std::max(1, m_grid->iconsPerPage());
            const int local = m_editTargetIndex % perPage;
            m_editTargetIndex = m_grid->currentPage() * perPage + local;
            if (m_editTargetIndex >= m_model.count())
                m_editTargetIndex = std::max(0, m_model.count() - 1);
            if (m_editGhostIcon)
                m_editGhostTargetRect = m_grid->gridSpanRect(
                    m_editTargetIndex,
                    m_editGhostIcon->gridSpanColumns(),
                    m_editGhostIcon->gridSpanRows());
        }
        // Stream icon textures for the new page.
        m_iconStreamer.onPageChanged(m_grid->currentPage(), m_grid->iconsPerPage(),
                                     app().gpu(), app().renderer(),
                                     m_grid->allIcons());
        auto* target = m_grid->focusManager().current();
        if (target)
            focusManager().setFocus(target);
        updateCursor();
    });

    int initialPage = 0;
#ifdef SWITCHU_MENU
    // A suspended game is the better anchor when there is one: it is where the
    // person actually was. The remembered page covers everything else -- a game
    // closed rather than suspended, an applet, the menu restarting itself.
    std::uint64_t pageAnchor = m_launcher.suspendedTitleId();
    if (pageAnchor == 0)
        pageAnchor = m_config.lastPageTitleId;

    if (pageAnchor != 0) {
        int anchorIndex = findTitleIndex(pageAnchor);
        if (anchorIndex >= 0 && m_grid->iconsPerPage() > 0)
            initialPage = anchorIndex / m_grid->iconsPerPage();
        if (initialPage > 0)
            m_grid->setPage(initialPage);
    }
#endif

    const bool returningFromSuspendedApp = m_launcher.suspendedTitleId() != 0;
    if (returningFromSuspendedApp) {
        m_deferredInitialAssetFrames = 1;
        DebugLog::log("[init] return path: deferring initial icon/sidebar uploads");
    } else {
        // Load textures for the initial visible page.
        m_iconStreamer.onPageChanged(m_grid->currentPage(), m_grid->iconsPerPage(),
                                     app().gpu(), app().renderer(),
                                     m_grid->allIcons());
    }

    if (returningFromSuspendedApp) {
        for (auto& icon : m_grid->allIcons())
            icon->forceVisible();
        m_returnFadeTimer = kReturnFadeInDur;
    } else {
        m_grid->startAppearAnimation();
    }
    if (m_tutorialStartupFade) {
        m_tutorialStartupFadeTimer = kTutorialStartupFadeDur;
        const std::uint64_t frequency = armGetSystemTickFreq();
        m_tutorialStartupFadeDeadlineTick = armGetSystemTick() +
            static_cast<std::uint64_t>(
                kTutorialStartupFadeDur * static_cast<float>(frequency));
    } else {
        m_tutorialStartupFadeDeadlineTick = 0;
    }

    SidebarManager::Actions sidebarActions;
#ifdef SWITCHU_MENU
    sidebarActions.onAlbum       = [this]() { m_launcher.launchAlbum(); };
    sidebarActions.onMiiEditor   = [this]() { m_launcher.launchMiiEditor(); };
    sidebarActions.onControllers = [this]() { m_launcher.launchControllerPairing(); };
#else
    sidebarActions.onAlbum       = [this]() { m_audio.playSfx(Sfx::Activate); };
    sidebarActions.onMiiEditor   = [this]() { m_audio.playSfx(Sfx::Activate); };
    sidebarActions.onControllers = [this]() { m_audio.playSfx(Sfx::Activate); };
#endif
    sidebarActions.onSettings = [this]() {
        m_audio.playSfx(Sfx::ModalShow);
        createSettings();
        if (m_settings) {
            m_navigator.navigate(switchu::navigation::Route::Settings);
            if (m_themeShop && m_themeShop->isActive())
                m_themeShop->hide();
            m_settings->show();
            focusManager().setFocus(m_settings.get());
        }
    };
    sidebarActions.onSleep = [this]() {
        if (!m_dialog) return;
        auto& i18n = nxui::I18n::instance();
        m_audio.playSfx(Sfx::ModalShow);
        m_dialogReturnFocus = focusManager().current();
        m_dialog->show(
            i18n.tr("power.title", "Power"),
            i18n.tr("power.choose_action", "Choose a power action."),
            {
                {i18n.tr("button.cancel", "Cancel"), [this]() {  }, true},
                {i18n.tr("power.sleep", "Sleep"), [this]() {
#ifdef SWITCHU_MENU
                    m_audio.playSfx(Sfx::ConfirmPositive);
                    m_launcher.enterSleep();
#else
                    m_audio.playSfx(Sfx::ConfirmPositive);
                    app().requestExit();
#endif
                }, true},
                {i18n.tr("power.shutdown", "Shutdown"), [this]() {
#ifdef SWITCHU_MENU
                    m_audio.playSfx(Sfx::ConfirmPositive);
                    m_launcher.shutdown();
#else
                    m_audio.playSfx(Sfx::ConfirmPositive);
                    app().requestExit();
#endif
                }, true},
                {i18n.tr("power.reboot", "Reboot"), [this]() {
#ifdef SWITCHU_MENU
                    m_audio.playSfx(Sfx::ConfirmPositive);
                    m_launcher.reboot();
#else
                    m_audio.playSfx(Sfx::ConfirmPositive);
                    app().requestExit();
#endif
                }, true}
            },
            0,
            {}
        );
        focusManager().setFocus(m_dialog.get());
    };
    sidebarActions.onMiiverse = [this]() {
        m_audio.playSfx(Sfx::ModalShow);
        createThemeShop();
        if (!m_themeShop) return;
        m_navigator.navigate(switchu::navigation::Route::ThemeShop);
        if (m_settings && m_settings->isActive())
            m_settings->hide();
        m_themeShop->beginStateRefreshBatch();
        publishUpdateState();
        refreshThemeShopState();
        m_themeShop->endStateRefreshBatch();
        // Capture only the first visible frames. The base overlay records the
        // backdrop, glass, content, and cursor command-build costs so the
        // remaining Theme Shop opening spike can be measured safely.
        m_themeShop->requestRenderDiagnostics(8);
        m_themeShop->show();
        focusManager().setFocus(m_themeShop.get());
    };

    m_sidebar.build(app().gpu(), app().renderer(), SD_ASSETS, sidebarActions);
    if (!returningFromSuspendedApp) {
        m_sidebar.reloadAssets(app().gpu(), app().renderer(), SD_ASSETS,
                               resolveThemeAssetPath(m_effectivePreset, m_effectivePreset.icons.basePath));
    }

    // Prefetch activity log data in background so opening it is instantaneous
    m_threadPool.submit([this]() {
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
    });

    wireGlobalActions();
    applyTheme();

    auto& root = rootBox();
    root.clearChildren();

    m_bgLayer = std::make_shared<nxui::Box>();
    m_bgLayer->setRect({0, 0, 1280, 720});
    m_bgLayer->setTag("bgLayer");
    m_bgLayer->setWireframeEnabled(false);
    m_bgLayer->addChild(m_background);
    m_bgLayer->addChild(m_gameArtworkBackdrop);

    m_contentLayer = std::make_shared<nxui::Box>();
    m_contentLayer->setRect({0, 0, 1280, 720});
    m_contentLayer->setTag("contentLayer");
    m_contentLayer->setWireframeEnabled(false);

    m_folderBackdrop = std::make_shared<FolderBackdrop>();
    m_folderBackdrop->setRect({0, 0, 1280, 720});
    m_folderBackdrop->setVisible(false);

    // Never constructed until now. The pointer was declared, added to the
    // content layer, and driven every frame by showFocusedSteamGridDbArtwork(),
    // which returns at its first line on a null pointer -- so showTitle() has
    // never run once and this widget has never drawn anything. It owns the
    // SteamGridDB hero, its gradient, and the logo above the row in the dynamic
    // line view, which is the logo reported as missing after the artwork was
    // confirmed downloaded. The wallpaper behind the grid comes from
    // GameArtworkBackdrop instead, which is why nothing looked obviously absent.
    m_steamGridDbBackdrop = std::make_shared<SteamGridDbBackdrop>(
        app().gpu(), app().renderer(), &m_threadPool);
    m_steamGridDbBackdrop->setRect({0, 0, 1280, 720});
    m_steamGridDbBackdrop->setLayoutMode(m_appLayoutMode);
    m_steamGridDbBackdrop->setEnabled(m_config.steamGridDbEnabled);

    m_folderHeader = std::make_shared<nxui::GlassPanel>();
    m_folderHeader->setRect({410.f, 78.f, 460.f, 58.f});
    m_folderHeader->setCornerRadius(22.f);
    m_folderHeader->setLiquidGlassEnabled(true);
    m_folderHeader->setForceLiquidGlass(true);
    m_folderHeader->setBlurEnabled(false);
    m_folderHeader->setVisible(false);
    m_folderHeaderLabel = std::make_shared<nxui::Label>("");
    // nxui child rectangles are screen-space; using {18, 6} placed this
    // label outside its bubble in the upper-left corner.
    m_folderHeaderLabel->setRect({428.f, 84.f, 424.f, 46.f});
    m_folderHeaderLabel->setFont(&m_fontNormal);
    m_folderHeaderLabel->setScale(0.66f);
    m_folderHeaderLabel->setMultiline(true);
    m_folderHeaderLabel->setLineSpacing(1.0f);
    m_folderHeaderLabel->setHAlign(nxui::Label::HAlign::Center);
    m_folderHeaderLabel->setVAlign(nxui::Label::VAlign::Center);
    m_folderHeader->addChild(m_folderHeaderLabel);

    m_topHud = std::make_shared<nxui::Box>(nxui::Axis::ROW);
    m_topHud->setRect({0, 0, 1280, 90});
    m_topHud->setTag("topHud");
    m_topHud->setWireframeEnabled(false);
    m_topHud->setJustifyContent(nxui::JustifyContent::SPACE_BETWEEN);
    m_topHud->setAlignItems(nxui::AlignItems::FLEX_START);
    m_topHud->addChild(m_clock);
    if (m_userAvatarBar)
        m_topHud->addChild(m_userAvatarBar);

    m_topCenterCluster = std::make_shared<nxui::Box>(nxui::Axis::ROW);
    m_topCenterCluster->setTag("topCenterCluster");
    m_topCenterCluster->setWireframeEnabled(false);
    m_topCenterCluster->setAlignItems(nxui::AlignItems::CENTER);
    m_topCenterCluster->setMarginTop(18.f);
    m_topCenterCluster->setShrink(0.f);
    m_topCenterCluster->setGap(12.f);
    m_topCenterCluster->setSize(120.f, 48.f);

    m_screenSwapButton = std::make_shared<warawara::PlazaScreenSwapButton>();
    m_screenSwapButton->setSize(54.f, 48.f);
    m_screenSwapButton->setMarginTop(0.f);
    m_screenSwapButton->setShrink(0.f);
    m_screenSwapButton->onActivate([this]() {
        m_audio.playSfx(Sfx::Activate);
        toggleWaraWaraPlaza();
    });
    m_topCenterCluster->addChild(m_screenSwapButton);

    m_mediaCenterButton = std::make_shared<widgets::MediaCenterButton>();
    m_mediaCenterButton->setSize(54.f, 48.f);
    m_mediaCenterButton->setMarginTop(0.f);
    m_mediaCenterButton->setShrink(0.f);
    m_mediaCenterButton->onActivate([this]() {
        m_audio.playSfx(Sfx::Activate);
        toggleMediaCenter();
    });
    m_topCenterCluster->addChild(m_mediaCenterButton);

    m_topCenterCluster->layout();
    m_topHud->addChild(m_topCenterCluster);

    m_topHud->addChild(m_battery);
    m_topHud->layout();

    m_leftSidebar = std::make_shared<nxui::Box>(nxui::Axis::COLUMN);
    m_leftSidebar->setTag("leftSidebar");
    m_leftSidebar->setWireframeEnabled(false);
    for (auto& btn : m_sidebar.leftButtons())
        m_leftSidebar->addChild(btn);

    m_rightSidebar = std::make_shared<nxui::Box>(nxui::Axis::COLUMN);
    m_rightSidebar->setTag("rightSidebar");
    m_rightSidebar->setWireframeEnabled(false);
    for (auto& btn : m_sidebar.rightButtons())
        m_rightSidebar->addChild(btn);

    m_contentLayer->addChild(m_folderBackdrop);
    // Keep live SteamGridDB artwork above the folder's frozen transition
    // snapshot, while still placing it behind every interactive HOME widget.
    m_contentLayer->addChild(m_steamGridDbBackdrop);
    m_contentLayer->addChild(m_grid);
    m_contentLayer->addChild(m_folderHeader);
    m_contentLayer->addChild(m_leftSidebar);
    m_contentLayer->addChild(m_rightSidebar);
    m_contentLayer->addChild(m_topHud);
    m_contentLayer->addChild(m_titlePill);
    m_contentLayer->addChild(m_pageIndicator);

    m_overlayLayer = std::make_shared<nxui::Box>();
    m_overlayLayer->setRect({0, 0, 1280, 720});
    m_overlayLayer->setTag("overlayLayer");
    m_overlayLayer->setWireframeEnabled(false);
    m_overlayLayer->addChild(m_cursor);
    m_overlayLayer->addChild(m_userSelect);

    createGameOptions();
    m_steamGridDbPicker = std::make_shared<SteamGridDbPickerScreen>(
        app().gpu(), app().renderer(), m_threadPool);
    m_steamGridDbPicker->setFont(&m_fontNormal);
    m_steamGridDbPicker->setSmallFont(&m_fontSmall);
    m_steamGridDbPicker->setTheme(&m_theme);
    m_steamGridDbPicker->onClosed([this]() {
        if (m_imagePinTargetSlot >= 0 && !m_imagePinApplyPending) {
            const int targetSlot = m_imagePinTargetSlot;
            const auto anchor = m_imagePinAnchor;
            const auto size = m_imagePinSize;
            m_imagePinTargetSlot = -1;
            showWidgetAssetMenu(targetSlot, anchor,
                                switchu::widgets::WidgetType::ImagePin, size);
            return;
        }
        if (m_gameOptions && m_gameOptions->isActive())
            focusManager().setFocus(m_gameOptions.get());
    });
    m_steamGridDbPicker->onSearch([this]() { editSteamGridDbPickerQuery(); });
    m_steamGridDbPicker->onApply(
        [this](const SteamGridDbManager::BrowseResult& browse,
               const SteamGridDbManager::Candidate& candidate) {
            if (m_imagePinTargetSlot >= 0)
                applyImagePinSteamGridDbCandidate(browse, candidate);
            else
                applySteamGridDbCandidate(browse, candidate);
        });
    m_overlayLayer->addChild(m_steamGridDbPicker);

    m_mediaCenterScreen = std::make_shared<media::MediaCenterScreen>();
    m_mediaCenterScreen->setFont(&m_fontNormal);
    m_mediaCenterScreen->setSmallFont(&m_fontSmall);
    m_mediaCenterScreen->setTheme(&m_theme);
    m_mediaCenterScreen->onPlayPause([this]() {
        m_audio.togglePlayPause();
        syncMediaCenterState();
    });
    m_mediaCenterScreen->onNextTrack([this]() {
        m_audio.nextTrack();
        syncMediaCenterState();
    });
    m_mediaCenterScreen->onPrevTrack([this]() {
        m_audio.previousTrack();
        syncMediaCenterState();
    });
    m_mediaCenterScreen->onShuffleToggle([this]() {
        m_config.customBgmShuffle = !m_audio.isShuffle();
        m_config.save();
        m_audio.setShuffle(m_config.customBgmShuffle);
        syncMediaCenterState();
    });
    m_mediaCenterScreen->onPlayTrack([this](int trackIdx) {
        m_audio.playTrack(trackIdx);
        syncMediaCenterState();
    });
    m_mediaCenterScreen->onAudioModeChange([this](const std::string& mode) {
        m_config.audioSourcePreference = mode;
        m_config.save();
        reloadMusicTracks();
        syncMediaCenterState();
    });
    m_mediaCenterScreen->onVolumeChange([this](float vol) {
        m_config.musicVolume = vol;
        m_config.save();
        m_audio.setVolume(vol);
        syncMediaCenterState();
    });
    m_mediaCenterScreen->onRescan([this]() {
        m_customBgmTracks = scanCustomBgmTracks();
        reloadMusicTracks();
        syncMediaCenterState();
    });
    m_mediaCenterScreen->onDeleteTrack([this](int trackIdx) {
        if (trackIdx < 0 || trackIdx >= static_cast<int>(m_customBgmTracks.size()))
            return;
        std::string pathToDelete = m_customBgmTracks[trackIdx].path;
        std::string titleToDelete = m_customBgmTracks[trackIdx].title;
        std::error_code ec;
        std::filesystem::remove(pathToDelete, ec);
        std::string thumbPath = pathToDelete.substr(0, pathToDelete.find_last_of('.')) + ".jpg";
        std::filesystem::remove(thumbPath, ec);
        DebugLog::log("[media] Deleted track: %s (%s)", titleToDelete.c_str(), pathToDelete.c_str());

        reloadMusicTracks();
        syncMediaCenterState();
    });
    m_mediaCenterScreen->onClose([this]() {
        if (m_mediaCenterReturnFocus) {
            focusManager().setFocus(m_mediaCenterReturnFocus);
            m_mediaCenterReturnFocus = nullptr;
        } else if (m_mediaCenterButton) {
            focusManager().setFocus(m_mediaCenterButton.get());
        }
    });
    m_overlayLayer->addChild(m_mediaCenterScreen);
    m_platformPicker = std::make_shared<PlatformPickerScreen>(
        app().gpu(), app().renderer(), m_threadPool);
    m_platformPicker->setFont(&m_fontNormal);
    m_platformPicker->setSmallFont(&m_fontSmall);
    m_platformPicker->setAssetBase(SD_ASSETS);
    m_platformPicker->setTheme(&m_theme);
    m_platformPicker->onClosed([this]() {
        DebugLog::log("[platformpicker] onClosed fired m_platformPickerTitleId=%016llX dialogActive=%d gameDetailsActive=%d",
                      (unsigned long long)m_platformPickerTitleId,
                      (m_dialog && m_dialog->isActive()) ? 1 : 0,
                      (m_gameDetails && m_gameDetails->isActive()) ? 1 : 0);
        // onSelected clears m_platformPickerTitleId before hide(), so a nonzero
        // id identifies only the cancel path. Restore the top active parent,
        // never rootBox: whole-screen focus leaves the d-pad with no actionable
        // selection until a touch event happens to repair it.
        if (m_platformPickerTitleId != 0) {
            nxui::Widget* target = nullptr;
            if (m_dialog && m_dialog->isActive())
                target = m_dialog.get();
            else if (m_gameDetails && m_gameDetails->isActive())
                target = m_gameDetails.get();
            if (target) {
                m_suppressNextNavigateSfx = true;
                focusManager().setFocus(target);
            }
            m_platformPickerTitleId = 0;
            m_platformPickerTitle.clear();
        }
    });
    m_platformPicker->onRejected([this]() {
        m_audio.playSfx(Sfx::ToggleOff);
    });
    m_platformPicker->onSelected([this](const std::string& slug) {
        const std::uint64_t titleId = m_platformPickerTitleId;
        const std::string title = m_platformPickerTitle;
        m_platformPickerTitleId = 0;
        m_platformPickerTitle.clear();
        if (m_dialog && m_dialog->isActive()) m_dialog->hide();
        m_config.gamePortPlatforms.erase(
            std::remove_if(m_config.gamePortPlatforms.begin(), m_config.gamePortPlatforms.end(),
                           [titleId](const auto& entry) { return entry.first == titleId; }),
            m_config.gamePortPlatforms.end());
        m_config.gamePortPlatforms.emplace_back(titleId, slug);
        m_config.save();
        m_platformPicker->hide();
        promptSteamGridDbForGame(titleId, title);
    });
    m_overlayLayer->addChild(m_platformPicker);
    createFolderOptions();
    createControllerTest();
    createTextEntry();

    // ContextMenu was declared and added to the overlay layer by the 1.2 merge
    // but never constructed, so every "if (!m_contextMenu) return;" guard bailed
    // and Plus on an empty slot did nothing at all: folder and widget creation
    // were both unreachable because this pointer was null.
    m_contextMenu = std::make_shared<ContextMenu>();
    m_contextMenu->setFont(&m_fontNormal);
    m_contextMenu->setSmallFont(&m_fontSmall);
    m_contextMenu->setTheme(&m_theme);
    m_contextMenu->onNavigate([this]() { m_audio.playSfx(Sfx::Navigate); });
    m_contextMenu->onActivate([this]() { m_audio.playSfx(Sfx::Activate); });
    m_contextMenu->onClose([this]() {
        m_audio.playSfx(Sfx::ModalHide);
        if (isCurrentFocusableWidget(m_contextMenuReturnFocus)) {
            m_suppressNextNavigateSfx = true;
            focusManager().setFocus(m_contextMenuReturnFocus);
        }
        m_contextMenuReturnFocus = nullptr;
    });
    m_overlayLayer->addChild(m_contextMenu);
    createWaraWaraPlaza();
    // Miis are exclusively rendered in WaraWara Plaza, not on the home screen background.
    if (m_background) {
        m_background->setAmbientMiisEnabled(false);
    }
    m_overlayLayer->addChild(m_dialog);
    m_overlayLayer->addChild(m_progressDialog);
    m_overlayLayer->addChild(m_launchAnim);
    m_overlayLayer->addChild(m_pointerCursor);

    root.addChild(m_bgLayer);
    root.addChild(m_contentLayer);
    root.addChild(m_overlayLayer);

    // setAppLayoutMode() does this whenever Minus switches the view, and the boot
    // path did not: it only assigned m_appLayoutMode from the config and let
    // applyDisplayModel() set the grid's mode. Restarting while the dynamic line
    // was the saved view therefore came back with the sidebar still laid out for
    // the grid and none of the line's navigation targets bound, which read as a
    // menu with no working selection that only showed the title pill. Pressing
    // Minus twice fixed it because that path does run this.
    if (m_steamGridDbBackdrop)
        m_steamGridDbBackdrop->setLayoutMode(m_appLayoutMode);
    configureDynamicLineNavigation();

    if (!focusTitle(m_launcher.suspendedTitleId())) {
        if (auto* firstIcon = m_grid->focusManager().current())
            focusManager().setFocus(firstIcon);
    }

    if (m_layoutDirty)
        saveMenuLayout();
}

std::string WiiUMenuApp::resolveSoundPresetId(const std::string& preset) const {
    std::string effectivePreset = preset;
    if (!isPackageSoundPreset(effectivePreset) && effectivePreset != kBuiltInSoundPreset) {
        DebugLog::log("[audio] preset '%s' blocked, using '%s' instead",
                      effectivePreset.c_str(),
                      kBuiltInSoundPreset);
        return kBuiltInSoundPreset;
    }

    if (!isPackageSoundPreset(effectivePreset))
        return effectivePreset;

    if (!resolveThemeSoundBase(installedThemePathFromPackagePreset(effectivePreset)).empty()) {
        DebugLog::log("[audio] package preset '%s' resolved from install directory", effectivePreset.c_str());
        return effectivePreset;
    }

    for (const auto& themePreset : m_allPresets) {
        if (themePreset.source != ThemePresetSource::InstalledPackage || themePreset.installPath.empty())
            continue;
        if (themePreset.id != effectivePreset && themePreset.soundPreset != effectivePreset)
            continue;
        return effectivePreset;
    }

    DebugLog::log("[audio] package preset '%s' unavailable, using '%s' instead",
                  effectivePreset.c_str(),
                  kBuiltInSoundPreset);
    return kBuiltInSoundPreset;
}

void WiiUMenuApp::loadSoundPreset(const std::string& preset) {
    std::string effectivePreset = preset;
    const bool useBuiltInBase = (effectivePreset == kBuiltInSoundPreset);
    const std::string builtInBase = std::string(SD_ASSETS) + "/sounds/" + kBuiltInSoundPreset;

    std::string base;
    if (!useBuiltInBase) {
        base = resolveThemeSoundBase(installedThemePathFromPackagePreset(effectivePreset));

        for (const auto& themePreset : m_allPresets) {
            if (!base.empty())
                break;
            if (themePreset.source != ThemePresetSource::InstalledPackage || themePreset.installPath.empty())
                continue;
            if (themePreset.id != effectivePreset && themePreset.soundPreset != effectivePreset)
                continue;

            base = resolveThemeSoundBase(themePreset.installPath);
            break;
        }
    }

    if (base.empty()) {
        if (isPackageSoundPreset(effectivePreset)) {
            base = installedThemePathFromPackagePreset(effectivePreset);
        } else {
            base = std::string(SD_ASSETS) + "/sounds/" + effectivePreset;
        }
    }
    DebugLog::log("[audio] Loading preset '%s' from %s", effectivePreset.c_str(), base.c_str());

    const bool hasCustomSfx = directoryExists(base + "/sfx");
    const bool hasCustomMusic = directoryExists(base + "/music");
    const std::string musicBase = hasCustomMusic ? base : builtInBase;
    const std::string preferredSfxBase = (!useBuiltInBase && hasCustomSfx) ? base : std::string();
    auto sfxPath = [&](const char* relativePath) {
        return resolveAudioOverridePath(preferredSfxBase, builtInBase, relativePath);
    };

    if (!useBuiltInBase && !hasCustomSfx) {
        DebugLog::log("[audio] preset '%s' has no custom SFX directory, using '%s' SFX fallback",
                      effectivePreset.c_str(),
                      kBuiltInSoundPreset);
    }
    if (!useBuiltInBase && !hasCustomMusic) {
        DebugLog::log("[audio] preset '%s' has no custom music, using '%s' music fallback",
                      effectivePreset.c_str(),
                      kBuiltInSoundPreset);
    }

    m_audio.loadSfx(Sfx::Navigate,        sfxPath("sfx/navigation.wav"));
    m_audio.loadSfx(Sfx::Activate,        sfxPath("sfx/activation.wav"));
    m_audio.loadSfx(Sfx::PageChange,      sfxPath("sfx/tab_transition.wav"));
    m_audio.loadSfx(Sfx::ModalShow,       sfxPath("sfx/show_modal.wav"));
    m_audio.loadSfx(Sfx::ModalHide,       sfxPath("sfx/hide_modal.wav"));
    m_audio.loadSfx(Sfx::LaunchGame,      sfxPath("sfx/launch_game.wav"));
    m_audio.loadSfx(Sfx::ThemeToggle,     sfxPath("sfx/toggle_on.wav"));
    m_audio.loadSfx(Sfx::ToggleOff,       sfxPath("sfx/toggle_off.wav"));
    m_audio.loadSfx(Sfx::SliderUp,        sfxPath("sfx/slider_up.wav"));
    m_audio.loadSfx(Sfx::SliderDown,      sfxPath("sfx/slider_down.wav"));
    m_audio.loadSfx(Sfx::ConfirmPositive, sfxPath("sfx/confirm.wav"));
    m_audio.loadSfx(Sfx::Volume,          sfxPath("sfx/volume.wav"));

    m_loadedSoundPreset = effectivePreset;
    reloadMusicTracks();
}

constexpr const char* kCustomMusicDir = "sdmc:/config/SwitchU/music";

std::vector<TrackInfo> WiiUMenuApp::scanCustomBgmTracks() {
    std::vector<TrackInfo> tracks;
    std::error_code ec;
    std::filesystem::create_directories(kCustomMusicDir, ec);
    ec.clear();

    if (!std::filesystem::is_directory(kCustomMusicDir, ec)) {
        return tracks;
    }

    for (const auto& entry : std::filesystem::directory_iterator(kCustomMusicDir, ec)) {
        if (ec) {
            ec.clear();
            continue;
        }
        if (!entry.is_regular_file(ec))
            continue;

        std::error_code szEc;
        auto sz = entry.file_size(szEc);
        if (!szEc && sz < 1024)
            continue;

        std::string ext = entry.path().extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

        if (ext == ".mp3" || ext == ".ogg" || ext == ".wav") {
            tracks.push_back({entry.path().string(), entry.path().stem().string()});
        }
    }

    std::sort(tracks.begin(), tracks.end(), [](const TrackInfo& a, const TrackInfo& b) {
        return a.title < b.title;
    });

    return tracks;
}

void WiiUMenuApp::reloadMusicTracks() {
    const bool wasPlaying = m_audio.isPlaying();
    m_audio.stop();
    m_audio.clearTracks();
    m_audio.setShuffle(m_config.customBgmShuffle);

    const std::string& pref = m_config.audioSourcePreference;
    m_customBgmTracks = scanCustomBgmTracks();
    bool loaded = false;

    if (pref == "custom_only") {
        if (!m_customBgmTracks.empty()) {
            for (const auto& track : m_customBgmTracks)
                m_audio.loadTrack(track.path, track.title);
            loaded = true;
            DebugLog::log("[audio] Custom Only: Loaded %zu custom track(s)", m_customBgmTracks.size());
        }
    } else if (pref == "theme_first") {
        if (!m_themeMusicTracks.empty()) {
            for (const auto& track : m_themeMusicTracks)
                m_audio.loadTrack(track);
            loaded = true;
            DebugLog::log("[audio] Theme First: Loaded %zu theme track(s)", m_themeMusicTracks.size());
        } else if (!m_customBgmTracks.empty()) {
            for (const auto& track : m_customBgmTracks)
                m_audio.loadTrack(track.path, track.title);
            loaded = true;
            DebugLog::log("[audio] Theme First (fallback): Loaded %zu custom track(s)", m_customBgmTracks.size());
        }
    } else if (pref == "theme_only") {
        if (!m_themeMusicTracks.empty()) {
            for (const auto& track : m_themeMusicTracks)
                m_audio.loadTrack(track);
            loaded = true;
            DebugLog::log("[audio] Theme Only: Loaded %zu theme track(s)", m_themeMusicTracks.size());
        }
    } else { // "custom_first" (default)
        if (!m_customBgmTracks.empty()) {
            for (const auto& track : m_customBgmTracks)
                m_audio.loadTrack(track.path, track.title);
            loaded = true;
            DebugLog::log("[audio] Custom First: Loaded %zu custom track(s)", m_customBgmTracks.size());
        } else if (!m_themeMusicTracks.empty()) {
            for (const auto& track : m_themeMusicTracks)
                m_audio.loadTrack(track);
            loaded = true;
            DebugLog::log("[audio] Custom First (fallback): Loaded %zu theme track(s)", m_themeMusicTracks.size());
        }
    }

    // Fallback to Preset Music if nothing loaded and mode allows it
    if (!loaded && pref != "custom_only") {
        const std::string effectivePreset = resolveSoundPresetId(m_loadedSoundPreset.empty() ? m_config.soundPreset : m_loadedSoundPreset);
        std::string musicBase = std::string(SD_ASSETS) + "/sounds/" + effectivePreset;
        std::string musicDir = musicBase + "/music";
        std::error_code ec;
        if (std::filesystem::is_directory(musicDir, ec)) {
            std::vector<std::string> tracks;
            ec.clear();
            for (const auto& entry : std::filesystem::directory_iterator(musicDir, ec)) {
                if (ec)
                    break;

                std::string name = entry.path().filename().string();
                if (name.size() > 4 && name.substr(name.size() - 4) == ".mp3")
                    tracks.push_back(name);
            }
            std::sort(tracks.begin(), tracks.end(), [](const std::string& left, const std::string& right) {
                const bool leftIsHome = (left == "home.mp3");
                const bool rightIsHome = (right == "home.mp3");
                if (leftIsHome != rightIsHome)
                    return leftIsHome;
                return left < right;
            });
            for (const auto& t : tracks)
                m_audio.loadTrack(musicDir + "/" + t);
            DebugLog::log("[audio] Loaded %zu preset music tracks", tracks.size());
        } else {
            DebugLog::log("[audio] No music directory for preset '%s'", effectivePreset.c_str());
        }
    }

    if (m_config.musicEnabled && !m_lockScreen.isLocked() && m_audioStarted) {
        m_audio.play();
    }
    syncMediaCenterState();
}

void WiiUMenuApp::toggleMediaCenter() {
    if (!m_mediaCenterScreen) return;
    if (m_mediaCenterScreen->isActive()) {
        m_mediaCenterScreen->hide();
    } else {
        m_mediaCenterReturnFocus = focusManager().current();
        m_customBgmTracks = scanCustomBgmTracks();
        syncMediaCenterState();
        m_mediaCenterScreen->show();
        focusManager().setFocus(m_mediaCenterScreen.get());
    }
}

void WiiUMenuApp::syncMediaCenterState() {
    if (!m_mediaCenterScreen) return;
    const bool isPl = m_audio.isPlaying();
    const bool isPa = m_audio.isPaused();
    m_mediaCenterScreen->setPlaybackState(isPl && !isPa, isPa, m_audio.isShuffle(),
                                         m_audio.currentTrackIndex(),
                                         m_audio.currentTrackTitle(),
                                         m_customBgmTracks,
                                         m_config.musicVolume,
                                         m_config.audioSourcePreference);
    if (m_mediaCenterButton) {
        m_mediaCenterButton->setPlaying(isPl && !isPa);
    }
}

void WiiUMenuApp::changeSoundPreset(const std::string& preset) {
    const std::string effectivePreset = resolveSoundPresetId(preset);
    if (m_presetChangePending && effectivePreset == m_pendingSoundPreset) {
        DebugLog::log("[audio] Preset change skipped; '%s' is already pending",
                      effectivePreset.c_str());
        return;
    }

    if (m_audioStarted && effectivePreset == m_loadedSoundPreset) {
        DebugLog::log("[audio] Preset change skipped; '%s' is already active",
                      effectivePreset.c_str());
        return;
    }

    m_audio.stop();
    m_audio.clearTracks();
    m_audio.clearSfx();

    m_presetChangePending = true;
    m_pendingSoundPreset = effectivePreset;
    m_audioFuture = m_threadPool.submit([this, effectivePreset]() {
        loadSoundPreset(effectivePreset);
    });
}

std::vector<std::string> WiiUMenuApp::scanAvailablePresets() {
    std::vector<std::string> presets;
    std::string soundsDir = std::string(SD_ASSETS) + "/sounds";
    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator(soundsDir, ec)) {
        if (ec)
            break;

        std::string name = entry.path().filename().string();
        if (name != kBuiltInSoundPreset) continue;

        std::string sub = entry.path().string();
        if (!entry.is_directory(ec)) {
            ec.clear();
            continue;
        }

        std::string sfxSub = sub + "/sfx";
        std::string musicSub = sub + "/music";
        bool hasSfx = directoryExists(sfxSub);
        bool hasMusic = directoryExists(musicSub);
        if (hasSfx || hasMusic)
            presets.push_back(name);
    }
    std::sort(presets.begin(), presets.end());
    return presets;
}

#ifdef SWITCHU_MENU
void WiiUMenuApp::refreshAppList() {
    DebugLog::log("[refresh] starting async app list fetch");

    if (m_editMode)
        exitEditMode();

    if (m_asyncRefreshPending) {
        DebugLog::log("[refresh] already in progress, queueing another pass");
        m_refreshQueued = true;
        return;
    }

    if (m_launchAnim && m_launchAnim->isPlaying()) m_launchAnim->stop();
    if (m_userSelect && m_userSelect->isActive()) m_userSelect->hide();

    m_refreshPrevPage = m_grid ? m_grid->currentPage() : 0;
    m_asyncRefreshPending = true;
    m_refreshQueued = false;

    m_appLoader.startAsync(m_threadPool);
}

void WiiUMenuApp::finalizeRefresh() {
    DebugLog::log("[refresh] finalizing (GPU upload)");
    m_asyncRefreshPending = false;

    GridModel refreshedModel;
    IconStreamer refreshedStreamer;
    // This streamer is move-assigned over m_iconStreamer below, so it needs
    // the pool too or the assignment would drop it.
    refreshedStreamer.setThreadPool(&m_threadPool);
    m_appLoader.finalize(refreshedModel, refreshedStreamer);
    refreshedStreamer.setArtworkDataLoader(gallery::GameArtworkStore::loadCover);
    DebugLog::log("[refresh] found %d apps", refreshedModel.count());

    if (gridModelsRefreshEquivalent(m_model, refreshedModel)) {
        DebugLog::log("[refresh] unchanged, keeping existing grid");
        m_refreshCooldownFrames = 20;
        if (m_layoutDirty)
            saveMenuLayout();
        return;
    }

    app().gpu().waitIdle();

    // The icons about to be replaced are held as raw pointers by both focus
    // managers, and FocusManager::changeFocusTo calls onFocusLost() ÃƒÂ¢Ã¢â€šÂ¬Ã¢â‚¬Â a virtual
    // ÃƒÂ¢Ã¢â€šÂ¬Ã¢â‚¬Â on whatever it thinks is focused. Destroying them without saying so
    // leaves that call reading a freed vtable, which is what two crash reports
    // from a clean install show: a garbage pointer in x1, then
    // ldr x1,[x1,#40]; blr x1 inside changeFocusTo.
    //
    // invalidateWidget was written for this and had no callers.
    for (const auto& icon : m_grid->allIcons()) {
        focusManager().invalidateWidget(icon.get());
        m_grid->focusManager().invalidateWidget(icon.get());
    }

    // The focus managers were not the only ones holding these. Edit mode keeps
    // two raw icon pointers and dereferences both without checking -- cancelEdit
    // calls setOpacity on one and clearActions on the other -- so a refresh
    // arriving mid-drag leaves those calls reading a freed vtable, the same way
    // changeFocusTo did. m_dialogReturnFocus needs nothing: it is checked
    // against the live grid by isCurrentFocusableWidget before it is used.
    m_editBoundIcon = nullptr;
    m_editSourceIcon = nullptr;

    m_grid->clearChildren();
    m_model = std::move(refreshedModel);
    m_iconStreamer = std::move(refreshedStreamer);
    // The loader hands back the raw slot arrangement: real entries wherever the
    // saved layout puts them, empty slots in between, and no folders or widgets.
    // The grid view hides that, but the line renders slot order, so a refresh --
    // which every delete queues -- dropped the line back to a lone icon with
    // gaps on both sides. buildGrid() composes through here for the same reason.
    m_model = buildRootFolderModel();

    std::vector<std::shared_ptr<GlossyIcon>> icons;
    std::vector<std::uint64_t> titleIds;
    icons.reserve(m_model.count());
    titleIds.reserve(m_model.count());
    for (int i = 0; i < m_model.count(); ++i) {
        auto icon = makeIcon(m_model.at(i));
        icon->setBaseColor(m_theme.iconDefault);
        icons.push_back(std::move(icon));
        titleIds.push_back(m_model.at(i).isApplication() ? m_model.at(i).titleId : 0);
    }
    // Reconcile the streamer's internal mappings with the newly composed and
    // sorted display model. Without this, the streamer remains indexed by the
    // raw layout from the loader while the grid icons are arranged in
    // projected order (such as Most played), mapping textures to the wrong
    // cells, leaving titles spinning, and desyncing the selection pill.
    m_iconStreamer.reconcileTitleIds(titleIds);

    GridLayoutMetrics gridMetrics = computeGridLayoutMetrics();

    // setup() rebuilds the grid in its default layout. Without these the line
    // came back as a grid that still drew one row: nothing moved left or right,
    // and leaving the view with Minus and returning was the only way out.
    m_grid->setLayoutMode(m_appLayoutMode);
    m_iconStreamer.setRingMode(isCarouselLayout());
    m_grid->setup(std::move(icons),
                  std::clamp(m_config.gridColumns, 3, 8),
                  std::clamp(m_config.gridRows, 2, 5),
                  gridMetrics.cellW, gridMetrics.cellH,
                  gridMetrics.padX, gridMetrics.padY);
    if (m_refreshPrevPage > 0) m_grid->setPage(m_refreshPrevPage);
    wireFocusCallback();
    m_grid->onPageSwitched([this]() {
        if (m_editMode && m_editTargetIndex >= 0) {
            const int perPage = std::max(1, m_grid->iconsPerPage());
            const int local = m_editTargetIndex % perPage;
            m_editTargetIndex = m_grid->currentPage() * perPage + local;
            if (m_editTargetIndex >= m_model.count())
                m_editTargetIndex = std::max(0, m_model.count() - 1);
            if (m_editGhostIcon)
                m_editGhostTargetRect = m_grid->gridSpanRect(
                    m_editTargetIndex,
                    m_editGhostIcon->gridSpanColumns(),
                    m_editGhostIcon->gridSpanRows());
        }
        m_iconStreamer.onPageChanged(m_grid->currentPage(), m_grid->iconsPerPage(),
                                     app().gpu(), app().renderer(),
                                     m_grid->allIcons());
        auto* target = m_grid->focusManager().current();
        if (target) focusManager().setFocus(target);
        updateCursor();
    });

    // Load textures for the restored page.
    int page = m_refreshPrevPage > 0 ? m_refreshPrevPage : 0;
    m_iconStreamer.onPageChanged(page, m_grid->iconsPerPage(),
                                 app().gpu(), app().renderer(),
                                 m_grid->allIcons());

    // The ring's wrap-around navigation is wired per grid build, so it has to be
    // re-applied to the one just built.
    configureDynamicLineNavigation();

    m_grid->startAppearAnimation();
    // If the rebuilt grid has nothing focusable, the app focus manager must be
    // left holding nothing rather than whatever it held before.
    if (auto* currentIcon = m_grid->focusManager().current()) {
        focusManager().setFocus(currentIcon);
        if (currentIcon->tag() == "glossy_icon") {
            auto* icon = static_cast<GlossyIcon*>(currentIcon);
            if (icon->titleId() != 0) {
                m_titlePill->setText(icon->title());
                m_titlePill->setVisible(true);
            } else {
                m_titlePill->hideAnimated();
            }
            refreshGameArtworkBackdrop(icon->titleId());
        }
    } else {
        focusManager().setFocus(nullptr);
        m_titlePill->hideAnimated();
    }
    updateCursor();

    // Keep a short cooldown to coalesce duplicate app-record notifications.
    m_refreshCooldownFrames = 20;
    applyTheme();
    if (m_layoutDirty)
        saveMenuLayout();
    DebugLog::log("[refresh] done, %d icons on page %d", m_model.count(), m_grid->currentPage());
    checkNewGameSteamGridDbPrompt();
}

void WiiUMenuApp::checkNewGameSteamGridDbPrompt() {
    if (m_allApps.empty())
        return;

    std::vector<AppEntry> newGames;
    std::unordered_set<uint64_t> known(
        m_config.steamGridDbKnownTitles.begin(),
        m_config.steamGridDbKnownTitles.end());

    if (known.empty()) {
        for (const auto& app : m_allApps) {
            if (app.titleId != 0 && (isNativeApplicationId(app.titleId) || m_config.isGamePort(app.titleId))) {
                m_config.steamGridDbKnownTitles.push_back(app.titleId);
            }
        }
        m_config.save();
        return;
    }

    for (const auto& app : m_allApps) {
        if (app.titleId == 0)
            continue;
        const bool isGameOrPort = isNativeApplicationId(app.titleId) || m_config.isGamePort(app.titleId);
        if (!isGameOrPort)
            continue;
        if (known.find(app.titleId) == known.end()) {
            newGames.push_back(app);
        }
    }

    if (newGames.empty())
        return;

    for (const auto& app : newGames) {
        m_config.steamGridDbKnownTitles.push_back(app.titleId);
    }
    m_config.save();

    if (!m_dialog)
        return;

    auto& i18n = nxui::I18n::instance();
    std::string title = i18n.tr("steamgriddb.new_games_title", "SteamGridDB");
    std::string message;
    if (newGames.size() == 1) {
        std::string displayName = resolveAppTitle(newGames[0].titleId, newGames[0].title);
        newGames[0].title = displayName;
        for (auto& a : m_allApps) {
            if (a.titleId == newGames[0].titleId) {
                a.title = displayName;
                break;
            }
        }
        message = i18n.tr("steamgriddb.new_game_detected", "New game detected: ") +
                  displayName + "\n\n" +
                  i18n.tr("steamgriddb.new_game_ask", "Would you like to check and download covers and artwork from SteamGridDB?");
    } else {
        message = std::to_string(newGames.size()) + " " +
                  i18n.tr("steamgriddb.new_games_detected", "new games or shortcuts detected.\n\nWould you like to check and download covers and artwork from SteamGridDB?");
    }

    raiseOverlay(m_dialog);
    m_audio.playSfx(Sfx::ModalShow);
    m_dialogReturnFocus = focusManager().current();
    m_dialog->show(title, message, {
        {i18n.tr("button.download", "Download"), [this]() {
            if (m_dialog) m_dialog->hide();
            startSteamGridDbScrape();
        }, true},
        {i18n.tr("button.cancel", "Cancel"), [this]() {
            if (m_dialog) m_dialog->hide();
            if (m_dialogReturnFocus) focusManager().setFocus(m_dialogReturnFocus);
        }}
    });
    focusManager().setFocus(m_dialog.get());
    if (m_cursor) m_cursor->setVisible(false);
}

void WiiUMenuApp::promptSteamGridDbForGame(std::uint64_t titleId, const std::string& title) {
    if (titleId == 0 || !m_dialog) return;

    if (std::find(m_config.steamGridDbKnownTitles.begin(),
                  m_config.steamGridDbKnownTitles.end(),
                  titleId) == m_config.steamGridDbKnownTitles.end()) {
        m_config.steamGridDbKnownTitles.push_back(titleId);
        m_config.save();
    }

    auto& i18n = nxui::I18n::instance();
    std::string resolvedTitle = resolveAppTitle(titleId, title);
    std::string dlgTitle = i18n.tr("steamgriddb.new_games_title", "SteamGridDB");
    std::string message = i18n.tr("steamgriddb.new_game_detected", "New game detected: ") +
                          resolvedTitle + "\n\n" +
                          i18n.tr("steamgriddb.new_game_ask",
                                  "Would you like to check and download covers and artwork from SteamGridDB?");

    raiseOverlay(m_dialog);
    m_audio.playSfx(Sfx::ModalShow);
    m_dialogReturnFocus = focusManager().current();
    m_dialog->show(dlgTitle, message, {
        {i18n.tr("button.download", "Download"), [this]() {
            if (m_dialog) m_dialog->hide();
            startSteamGridDbScrape();
        }, true},
        {i18n.tr("button.cancel", "Cancel"), [this, titleId, resolvedTitle]() {
            if (m_dialog) m_dialog->hide();
            showGameDetails(titleId, resolvedTitle);
        }}
    });
    focusManager().setFocus(m_dialog.get());
    if (m_cursor) m_cursor->setVisible(false);
}

std::string WiiUMenuApp::resolveAppTitle(std::uint64_t titleId, const std::string& currentTitle) {
    if (titleId != 0 && m_config.hasCustomTitle(titleId)) {
        return m_config.customTitle(titleId, currentTitle);
    }
    if (titleId != 0 && m_config.isGamePort(titleId)) {
        const std::string searchTitle = m_config.gamePortSearchTitle(titleId, "");
        if (!searchTitle.empty())
            return searchTitle;
    }
    std::string title = currentTitle;
    char tidBuf[17]{};
    std::snprintf(tidBuf, sizeof(tidBuf), "%016llX", (unsigned long long)titleId);
    const bool isFallback = title.empty() || title == tidBuf;

    if (isFallback && titleId != 0) {
        const char* known = switchu::control_cache::getKnownTitleName(titleId);
        if (known) {
            return std::string(known);
        }
        // 1. Try reading metadata from control cache
        switchu::control_cache::Meta meta{};
        if (switchu::control_cache::readMeta(titleId, meta) && meta.name[0] != '\0') {
            title = meta.name;
        }
#ifdef __SWITCH__
        // 2. Try live query from Horizon OS
        if (title.empty() || title == tidBuf) {
            auto* controlData = new (std::nothrow) NsApplicationControlData();
            if (controlData) {
                size_t controlSize = 0;
                Result rc = nsGetApplicationControlData(NsApplicationControlSource_Storage,
                                                        titleId,
                                                        controlData,
                                                        sizeof(NsApplicationControlData),
                                                        &controlSize);
                if (R_SUCCEEDED(rc)) {
                    switchu::control_cache::Meta liveMeta{};
                    if (switchu::control_cache::fillMetaFromControlData(titleId, *controlData, liveMeta)) {
                        if (liveMeta.name[0] != '\0') {
                            title = liveMeta.name;
                        }
                    }
                }
                delete controlData;
            }
        }
#endif
    }
    return title.empty() ? std::string(tidBuf) : title;
}

void WiiUMenuApp::pollAutoNtpSync(float dt) {
#ifdef SWITCHU_MENU
    if (m_autoNtpSynced)
        return;

    m_ntpCheckTimer += dt;
    if (m_ntpCheckTimer < 5.0f)
        return;
    m_ntpCheckTimer = 0.f;

    NifmInternetConnectionStatus nifmStatus = NifmInternetConnectionStatus_ConnectingUnknown1;
    if (R_SUCCEEDED(nifmGetInternetConnectionStatus(nullptr, nullptr, &nifmStatus)) &&
        nifmStatus == NifmInternetConnectionStatus_Connected) {
        switchu::services::NtpClient::syncAsync([this](bool ok, uint64_t timestamp) {
            if (ok) {
                m_autoNtpSynced = true;
                DebugLog::log("[ntp] auto internet clock sync successful: timestamp=%llu",
                              static_cast<unsigned long long>(timestamp));
            }
        });
    }
#endif
}

#endif

bool WiiUMenuApp::inThemeShopForMemorySampling() const {
    return m_themeShop && m_themeShop->isActive();
}

void WiiUMenuApp::setupLockScreen() {
    auto& i18n = nxui::I18n::instance();

    m_lockScreen.setFonts(&m_fontClock, &m_fontSmall);
    m_lockScreen.setTheme(&m_theme);
    m_lockScreen.setUse12HourClock(m_config.clockUse12Hour);
    m_lockScreen.onSleepRequested([this]() {
        // The daemon owns the real sleep sequence. Asking for it here is what
        // makes the panel actually go dark: stopping our own presentation only
        // ends GPU work, and Horizon keeps the backlight on regardless.
#ifdef SWITCHU_PREFLIGHT_EDGE_TEST
        if (m_preflightEdgePending)
            DebugLog::log("[diagnostic-preflight-edge] preserving dialog for requested sleep");
        closeActiveOverlays(m_preflightEdgePending);
#else
        closeActiveOverlays();
#endif
        m_launcher.enterSleep();
    });

    m_lockScreen.onLocked([this, &i18n]() {
        // Whatever was open goes with it. Coming back to a half-open menu the
        // owner cannot remember opening is worse than coming back to the grid.
#ifdef SWITCHU_PREFLIGHT_EDGE_TEST
        if (m_preflightEdgePending) {
            m_preflightEdgeLockObserved = true;
            DebugLog::log("[diagnostic-preflight-edge] preserving dialog across lock");
        }
        closeActiveOverlays(m_preflightEdgePending);
#else
        closeActiveOverlays();
#endif
        m_audio.stopAll();
        m_accessibility.announce(i18n.tr("lock_screen.locked",
                                         "Screen locked. Press the same button three times to unlock."),
                                 true, true);
    });
    m_lockScreen.onProgress([]() {});
    m_lockScreen.onUnlocked([this, &i18n]() {
        if (m_config.musicEnabled && m_audioStarted)
            m_audio.play();
        m_audio.playSfx(Sfx::ModalHide);
        m_accessibility.announce(i18n.tr("lock_screen.unlocked", "Unlocked."), true, true);
    });
}

void WiiUMenuApp::onUpdate(float dt) {
    logControllerState(dt);
    m_animalesePlayer.update(dt);
    m_audio.update();

    const bool isAudioPlaying = m_audio.isPlaying() && !m_audio.isPaused();
    if (m_mediaCenterButton && m_mediaCenterButton->isPlaying() != isAudioPlaying) {
        m_mediaCenterButton->setPlaying(isAudioPlaying);
    }
    if (m_mediaCenterScreen && m_mediaCenterScreen->isActive()) {
        syncMediaCenterState();
    }

    // Retire and upload widget-owned textures before the next frame begins.
    syncWidgetPageAssets();
    pollRecentWidgetAssets();
    syncWidgetIconContent();
    syncFolderPreviews();
    pollGameArtworkAssets();

#ifdef NXUI_BACKEND_DEKO3D
    // Keep a reserve for labels that only appear after opening an overlay.
    // Texture retirement is safe here, before the next command buffer records.
    {
        auto& gpu = app().gpu();
        constexpr std::uint64_t kTextImageReserve = 4u * 1024u * 1024u;
        constexpr std::size_t kPressureEntriesPerFont = 32;
        constexpr std::size_t kPressureBytesPerFont = 1u * 1024u * 1024u;
        const std::size_t textBytesBefore =
            m_fontNormal.cacheBytes() + m_fontSmall.cacheBytes();
        const bool requested = m_fontNormal.maintenanceRequested() ||
                               m_fontSmall.maintenanceRequested();
        const bool memoryPressure = gpu.imageMemoryAvailable() < kTextImageReserve;
        const bool usefulPressureTrim = memoryPressure &&
            (m_fontNormal.cacheEntryCount() > kPressureEntriesPerFont ||
             m_fontSmall.cacheEntryCount() > kPressureEntriesPerFont ||
             m_fontNormal.cacheBytes() > kPressureBytesPerFont ||
             m_fontSmall.cacheBytes() > kPressureBytesPerFont);
        if (requested || usefulPressureTrim) {
            DebugLog::log(
                "[text-cache] maintenance requested=%d pressure=%d gpu=%llu/%llu text=%zu entries=%zu",
                requested ? 1 : 0, memoryPressure ? 1 : 0,
                static_cast<unsigned long long>(gpu.imageMemoryUsed()),
                static_cast<unsigned long long>(gpu.imageMemoryBudget()),
                textBytesBefore,
                m_fontNormal.cacheEntryCount() + m_fontSmall.cacheEntryCount());
            if (textBytesBefore > 0)
                gpu.waitIdle();
            if (memoryPressure) {
                const std::size_t normalEntries = requested
                    ? m_fontNormal.cacheEntryCount() / 2 : kPressureEntriesPerFont;
                const std::size_t smallEntries = requested
                    ? m_fontSmall.cacheEntryCount() / 2 : kPressureEntriesPerFont;
                const std::size_t normalBytes = requested
                    ? m_fontNormal.cacheBytes() / 2 : kPressureBytesPerFont;
                const std::size_t smallBytes = requested
                    ? m_fontSmall.cacheBytes() / 2 : kPressureBytesPerFont;
                m_fontNormal.trimCache(std::min(normalEntries, kPressureEntriesPerFont),
                                       std::min(normalBytes, kPressureBytesPerFont));
                m_fontSmall.trimCache(std::min(smallEntries, kPressureEntriesPerFont),
                                      std::min(smallBytes, kPressureBytesPerFont));
            } else {
                m_fontNormal.trimCache();
                m_fontSmall.trimCache();
            }
            if (textBytesBefore > 0)
                app().renderer().reclaimReleasedTextureSlotsAfterIdle();
        }
    }
#endif

    // The 1.2 streamer decodes off the UI thread and uploads at most one
    // texture per onPageChanged() call, so the single call made while the grid
    // is built leaves every icon after the first four blank. Nothing else calls
    // it until an unrelated event happens to, which on hardware showed as icons
    // that never appeared until the settings overlay was opened and closed.
    // needsVisibleLoads() was added for exactly this pump and had no caller.
    //
    // Held off during the deferred first-asset frame so the intentional startup
    // deferral below still owns the first upload, and during the launch
    // animation so no new decode or GPU upload is started while the menu is
    // already draining its asynchronous work for title handoff.
    //
    // The window has to be asked for in the terms of the view on screen. The
    // dynamic line has no pages: wireFocusCallback() loads around the focused
    // icon with a page size of one, and this pump kept asking for page 0 of a
    // full grid page. It therefore only ever scheduled the first fifteen icons,
    // and everything past them ÃƒÂ¢Ã¢â€šÂ¬Ã¢â‚¬Â the homebrew at the end of the line ÃƒÂ¢Ã¢â€šÂ¬Ã¢â‚¬Â stayed on
    // its loading spinner until the focus callback happened to reach it.
    if (m_plazaScreen && m_plazaScreen->isActive()) {
        // Plaza communities are populated before startup icon decodes finish.
        // Refreshing here pumps the one-title streamer windows and publishes
        // each completed GPU texture through setData() without rebuilding the
        // pedestals or Miis. The call is deliberately limited to the active
        // Plaza and ten entries; after all are loaded it is pointer-only work.
        refreshPlazaCommunities();
    }

    if (m_grid && m_deferredInitialAssetFrames == 0
        && !(m_launchAnim && m_launchAnim->isPlaying())) {
        const bool line = isCarouselLayout();
        const int pumpPage = line ? std::max(0, m_grid->focusedGlobalIndex())
                                  : m_grid->currentPage();
        const int pumpPerPage = line ? 1 : m_grid->iconsPerPage();
        if (m_iconStreamer.needsVisibleLoads(pumpPage, pumpPerPage)) {
            m_iconStreamer.onPageChanged(pumpPage, pumpPerPage,
                                         app().gpu(), app().renderer(),
                                         m_grid->allIcons());
        }
    }

    // updateCursor() returns early whenever the route is not Home, and returns
    // without moving or hiding the ring -- so a route left behind by an overlay
    // freezes the selection outline wherever it last was while focus carries on
    // moving underneath it. That is the ring stuck on the SwitchU icon after
    // opening the theme shop, and why a suspend and resume cleared it: the menu
    // is rebuilt from scratch. focusRoot() is the authority on what owns input,
    // and when it says the root box does, Home is what the route is.
    if (focusRoot() == &rootBox()
        && m_navigator.route() != switchu::navigation::Route::Home) {
        DebugLog::log("[nav] route left at %d with nothing open; resetting to Home",
                      static_cast<int>(m_navigator.route()));
        m_navigator.resetToHome();
    }

    syncSteamGridDb();
    if (m_navigator.route() == switchu::navigation::Route::Home
        && !(m_dialog && m_dialog->isActive())
        && !(m_settings && m_settings->isActive())
        && !(m_themeShop && m_themeShop->isActive())
        && !(m_gameOptions && m_gameOptions->isActive())
        && !(m_folderOptions && m_folderOptions->isActive())
        && !(m_userSelect && m_userSelect->isActive())) {
        showFocusedSteamGridDbArtwork();
    }

    if (m_folderCaptureReady)
        openCapturedFolder();

    if (m_config.actionHintStyle != "panel")
        syncHintCapsules(dt);

    if (m_grid) {
        const nxui::Rect gr = m_grid->rect();
        const float target = gr.y + gr.height * 0.5f;
        if (!m_arrowCenterInit) {
            m_arrowCenterInit = true;
            m_arrowCenterY.setImmediate(target);
        } else if (std::abs(m_arrowCenterY.target() - target) > 0.5f) {
            m_arrowCenterY.set(target, 0.28f, nxui::Easing::outCubic);
        }
    }

    {
        const bool paging = pagingAvailable();
        const int page = m_grid ? m_grid->currentPage() : 0;
        const int total = m_grid ? m_grid->totalPages() : 1;
        auto step = [dt](PageArrowAnim& a, bool visible) {
            const float d = dt / kPageArrowFade;
            a.show = std::clamp(a.show + (visible ? d : -d), 0.f, 1.f);
            a.press = std::max(0.f, a.press - dt / kPageArrowKick);
        };
        m_addPageMode = addPageAvailable();
        m_deletePageMode = deletePageAvailable();
        const bool line = isCarouselLayout();
        const bool hasLeft = line ? dynamicLineNeighbour(-1) >= 0 : page > 0;
        const bool hasRight = line ? dynamicLineNeighbour(+1) >= 0 : page < total - 1;
        step(m_arrowAnimLeft, (paging && hasLeft) || m_deletePageMode);
        step(m_arrowAnimRight, (paging && hasRight) || m_addPageMode);

        if (m_deletePageMode) {
            const bool holding = m_deletePageTouchHold ||
                                 app().input().isHeld(nxui::Button::ZL);
            if (holding) {
                if (!m_deletePageTriggered) {
                    m_deletePageHold = std::min(1.f, m_deletePageHold + dt / kAddPageHoldDur);
                    if (m_deletePageHold >= 1.f) {
                        m_deletePageHold = 0.f;
                        m_deletePageTouchHold = false;
                        m_deletePageTriggered = true;
                        m_zlQuickFlipArmed = false;
                        m_zlQuickFlipHeld = 0.f;
                        if (m_openFolderId != 0)
                            deleteFolderPage();
                        else
                            deleteHomePage();
                    }
                }
            } else {
                m_deletePageTriggered = false;
                m_deletePageHold = std::max(0.f,
                    m_deletePageHold - dt / (kAddPageHoldDur * 0.4f));
            }
        } else {
            m_deletePageHold = 0.f;
            m_deletePageTouchHold = false;
            m_deletePageTriggered = false;
        }

        if (m_addPageMode) {
            const bool holding = m_addPageTouchHold ||
                                 app().input().isHeld(nxui::Button::ZR);
            if (holding) {
                if (!m_addPageTriggered) {
                    m_addPageHold = std::min(1.f, m_addPageHold + dt / kAddPageHoldDur);
                    if (m_addPageHold >= 1.f) {
                        m_addPageHold = 0.f;
                        m_addPageTouchHold = false;
                        m_addPageTriggered = true;
                        if (m_openFolderId != 0)
                            createFolderPage();
                        else
                            createHomePage();
                    }
                }
            } else {
                m_addPageTriggered = false;
                m_addPageHold = std::max(0.f,
                    m_addPageHold - dt / (kAddPageHoldDur * 0.4f));
            }
        } else {
            m_addPageHold = 0.f;
            m_addPageTouchHold = false;
            m_addPageTriggered = false;
        }
    }

    const bool sliding = m_grid && m_grid->isTransitioning();
    if (sliding != m_gridSliding) {
        m_gridSliding = sliding;
        if (sliding) {
            if (m_cursor) m_cursor->setVisible(false);
        } else {
            if (m_cursor && focusManager().current())
                m_cursor->moveTo(focusManager().current()->focusRect().expanded(4.f), 0.01f);
        }
    }

    // The selection ring used to be placed only from onFocusChanged, so any
    // focus change that landed while the grid was mid-transition ÃƒÂ¢Ã¢â€šÂ¬Ã¢â‚¬Â which is
    // what moving quickly between the top row and the grid produces ÃƒÂ¢Ã¢â€šÂ¬Ã¢â‚¬Â left the
    // ring hidden or parked on the previous widget while the hint bar already
    // described the new one. SelectionCursor::moveTo ignores a target it is
    // already animating towards, so repairing it every frame costs nothing and
    // the carousel needs the per-frame placement anyway.
    updateCursor();

    syncSoftwareDeletion();

    // O restante da sequencia de fundo entra por aqui, alguns quadros por vez.
    // Ler os 71 MB onde o tema e aplicado custava 3.4 dos 4.1 segundos de
    // retorno de um jogo; agora o menu abre no primeiro quadro e o resto chega
    // enquanto ele ja esta na mao de quem usa.
    if (m_background)
        m_background->pumpImageSequence(app().gpu(), app().renderer());

    // A capa personalizada do jogo em foco chega por aqui. O trabalho caro --
    // ler do cartao e decodificar -- ja aconteceu numa thread de trabalho;
    // sobra a subida para a GPU, que e barata o bastante para o quadro.
    if (m_gameArtworkBackdrop) {
        if (m_gameArtworkBackdrop->pollPendingArtwork(app().gpu(), app().renderer()))
            DebugLog::log("[gallery] background applied: %s",
                          m_gameArtworkBackdrop->artworkPath().c_str());
    }

    // Amostrado enquanto a loja de temas esta aberta, que e onde o menu morreu
    // duas vezes sem deixar rastro. O daemon registrou "menu exited (reason=0)"
    // -- saida limpa, nao queda -- e uma saida limpa pedida pelo sistema tem
    // cara de pressao de memoria. Sem medir durante, isso fica em suposicao.
    if (inThemeShopForMemorySampling()) {
        m_memSampleTimer += dt;
        if (m_memSampleTimer >= 2.f) {
            m_memSampleTimer = 0.f;
            u64 total = 0, used = 0;
            if (R_SUCCEEDED(svcGetInfo(&total, InfoType_TotalMemorySize, CUR_PROCESS_HANDLE, 0))
             && R_SUCCEEDED(svcGetInfo(&used,  InfoType_UsedMemorySize,  CUR_PROCESS_HANDLE, 0))) {
                DebugLog::log("[mem] processo %.1f de %.1f MB  imagem %.1f de %.1f MB",
                              used / 1048576.0, total / 1048576.0,
                              app().gpu().imageMemoryUsed() / 1048576.0,
                              nxui::GpuDevice::imageBudget() / 1048576.0);
            }
        }
    } else {
        m_memSampleTimer = 0.f;
    }

#ifdef SWITCHU_MENU
    // Stay unbuffered for the first few seconds of the run loop, not just up to
    // the first frame. The deferred icon and sidebar uploads run on frame one,
    // and a menu that hangs there would otherwise leave no trace: the previous
    // attempt switched to buffered before writing its own marker, so the marker
    // went into the buffer and died with the process, proving nothing.
    if (m_logImmediateFrames > 0) {
        // Numbering the first frames pins down whether the loop ran at all and
        // how far it got, which "app.run..." as a last line could not say.
        if (m_logImmediateFrames > 296)
            DebugLog::log("[menu] frame %d", 301 - m_logImmediateFrames);
        if (--m_logImmediateFrames == 0) {
            DebugLog::log("[menu] run loop healthy, log buffering enabled");
            switchu::FileLog::setImmediate(false);
        }
    }
    // A hard power-off still loses whatever came after the last flush, so
    // drain on a timer as the daemon does.
    //
    // One second while the theme screen is open, two otherwise. Every crash
    // reported so far happened in there -- applying a theme, or seconds after a
    // catalogue loaded -- and every one of them arrived with the last lines
    // still in RAM. Three investigations ran on logs that stopped before the
    // interesting part; the difference in cost is one flush a second.
    const bool inThemeScreen = m_themeShop && m_themeShop->isActive();
    switchu::FileLog::flushIfStale(inThemeScreen ? 1 : 2);


    // Skip rendering the home scene while the settings overlay is settled.
    // The A/B probe measured it: with the occluded scene rendered the frame
    // costs ~31ms of GPU; with it hidden, ~14-15ms ÃƒÂ¢Ã¢â€šÂ¬Ã¢â‚¬Â inside the 16.7ms
    // budget. The scene under the panel was ~16ms a frame spent on pixels
    // the panel covers. The glass widgets sample the held offscreen capture,
    // not the live framebuffer, so their appearance does not change, and the
    // overlay draws the frozen blurred backdrop as its base so the margin
    // around the panel still shows the scene.
    //
    // Gated on the renderer actually holding the capture, which the overlay
    // only sets while open, not animating, not scrolling and with no
    // dropdown up; any interaction releases it and the scene renders again
    // the next frame.
    //
    // The theme screen covers the scene exactly as completely and was left out
    // of this, so it kept paying the ~16ms the settings overlay stopped paying:
    // 31.7ms of GPU against 8.5ms on the home grid, locked to 30fps with vsync
    // never waited on. Reported as the theme options lagging while settings did
    // not, which is precisely the shape of one screen having this and the other
    // not. It holds the capture already -- that lives in the shared base class
    // -- so only the question asked here had to widen.
    {
        const bool settingsUp = m_settings && m_settings->isFullyVisible();
        const bool themeShopUp = m_themeShop && m_themeShop->isFullyVisible();
        const bool galleryUp = m_gameGallery && m_gameGallery->isFullyVisible();
        const bool modsUp = m_gameMods && m_gameMods->isFullyVisible();
        const bool cheatsUp = m_gameCheats && m_gameCheats->isFullyVisible();
        const bool detailsUp = m_gameDetails && m_gameDetails->isFullyVisible();
        const bool plazaUp = m_plazaScreen && m_plazaScreen->isFullyVisible();
        const bool hideScene = ((settingsUp || themeShopUp || galleryUp || modsUp || cheatsUp || detailsUp) &&
                                app().renderer().holdOffscreenCapture()) || plazaUp;
        m_probeSceneHidden = hideScene;
        // Each overlay is told only about itself: the one that is not up must
        // not start drawing the frozen backdrop as its base.
        if (m_settings)
            m_settings->setSceneHidden(hideScene && settingsUp);
        if (m_themeShop)
            m_themeShop->setSceneHidden(hideScene && themeShopUp);
        if (m_gameGallery)
            m_gameGallery->setSceneHidden(hideScene && galleryUp);
        if (m_gameMods)
            m_gameMods->setSceneHidden(hideScene && modsUp);
        if (m_gameCheats)
            m_gameCheats->setSceneHidden(hideScene && cheatsUp);
        if (m_gameDetails)
            m_gameDetails->setSceneHidden(hideScene && detailsUp);
        if (m_bgLayer) m_bgLayer->setVisible(!hideScene);
        if (m_contentLayer) m_contentLayer->setVisible(!hideScene);
    }

    // Frame cost sampled once a second, tagged with whether an overlay is up.
    // Comparing the home grid against the settings overlay says whether its
    // 10-15 fps comes from submission count or from fragment shading, which
    // reading the render path did not settle.
    //
    // Sampled from the real frame time, not from dt. The activity delta is
    // clamped to 0.1 s before it gets here, so every frame slower than that was
    // counted as 16 ms: the Theme Shop opening frame measured 287.790 ms by its
    // own trace and was reported on this line as worst=84.8ms in the same
    // second. Animation still uses dt; only the measurement changed.
    const float frameSeconds = app().lastFrameSeconds();
    m_perfAccumDt += frameSeconds;
    ++m_perfFrames;
    m_perfWorstDt = std::max(m_perfWorstDt, frameSeconds);
    if (m_perfAccumDt >= 1.0f) {
        const auto& ren = app().renderer();
        // fps alone cannot show the margin: vsync pins anything between 16.7ms
        // and 33ms to exactly 30fps, which is what the settings overlay
        // measured. Milliseconds say how far over budget a frame actually is,
        // and therefore how much has to be saved to get back to 60.
        const float avgMs = (m_perfAccumDt / m_perfFrames) * 1000.f;
        const auto& gpu = app().gpu();
        // Operation and performance mode. After exiting DBI the menu renders
        // an unchanged workload ÃƒÂ¢Ã¢â€šÂ¬Ã¢â‚¬Â same 90 draws, same 17448 vertices ÃƒÂ¢Ã¢â€šÂ¬Ã¢â‚¬Â at
        // gpu=65ms instead of the usual 0-15ms, which is the GPU running at a
        // fraction of its clock. Nothing in this project manages the
        // performance configuration; the real qlaunch does, and this daemon
        // stands in for it. Log the modes so a repro says which state the
        // console was left in rather than leaving it to inference.
        const unsigned opMode = (unsigned)appletGetOperationMode();
        const unsigned perfMode = (unsigned)appletGetPerformanceMode();
        DebugLog::log("[perf] %.1f fps  avg=%.1fms worst=%.1fms  vsync=%.1fms gpu=%.1fms  opmode=%u perf=%u  draws=%u binds=%u verts=%u blur=%u caps=%u uploads=%u batches=%u uploadwait=%.3fms  settings=%d(1=fullScene,2=sceneHidden) themeshop=%d",
                      m_perfFrames / m_perfAccumDt,
                      avgMs,
                      m_perfWorstDt * 1000.f,
                      gpu.lastAcquireNs() / 1000000.0,
                      gpu.lastFenceWaitNs() / 1000000.0,
                      opMode,
                      perfMode,
                      ren.lastFrameDrawCalls(),
                      ren.lastFramePipelineBinds(),
                      ren.lastFrameVertices(),
                      ren.lastFrameBlurPasses(),
                      ren.lastFrameCaptures(),
                      gpu.lastFrameUploads(),
                      gpu.lastFrameUploadBatches(),
                      gpu.lastFrameUploadWaitNs() / 1000000.0,
                      (m_settings && m_settings->isActive())
                          ? (m_probeSceneHidden ? 2 : 1) : 0,
                      (m_themeShop && m_themeShop->isActive()) ? 1 : 0);
        m_perfAccumDt = 0.f;
        m_perfFrames = 0;
        m_perfWorstDt = 0.f;
    }
#endif

#ifdef SWITCHU_DEBUG_UI
    if (m_debugOverlay) {
        m_debugOverlay->setDeltaTime(dt);
    }
#endif

    if (m_returnFadeTimer > 0.f)
        m_returnFadeTimer = std::max(0.f, m_returnFadeTimer - dt);
    if (m_tutorialStartupFadeTimer > 0.f)
        m_tutorialStartupFadeTimer = std::max(0.f, m_tutorialStartupFadeTimer - dt);
    if (m_tutorialStartupFadeDeadlineTick != 0 &&
        armGetSystemTick() >= m_tutorialStartupFadeDeadlineTick) {
        m_tutorialStartupFadeTimer = 0.f;
        m_tutorialStartupFadeDeadlineTick = 0;
    }

    if (m_launchAnim && m_launchAnim->isPlaying()) {
        const float progress = m_launchAnim->musicFadeProgress();
        m_audio.setMusicFade(1.f - nxui::Easing::outQuad(progress));
    } else {
        m_audio.setMusicFade(1.f);
    }

    syncThemePackageTransfer();
    syncGameArtworkSave();

    if (m_deferredInitialAssetFrames > 0) {
        --m_deferredInitialAssetFrames;
        if (m_deferredInitialAssetFrames == 0) {
            DebugLog::log("[init] deferred initial icon/sidebar uploads start");
            if (m_grid) {
                m_iconStreamer.onPageChanged(m_grid->currentPage(), m_grid->iconsPerPage(),
                                             app().gpu(), app().renderer(),
                                             m_grid->allIcons());
            }
            m_sidebar.reloadAssets(app().gpu(), app().renderer(), SD_ASSETS,
                                   resolveThemeAssetPath(m_effectivePreset,
                                                         m_effectivePreset.icons.basePath));
            DebugLog::log("[init] deferred initial icon/sidebar uploads done");
        }
    }

    if (m_deferredBluetoothInitFrames > 0) {
        --m_deferredBluetoothInitFrames;
        if (m_deferredBluetoothInitFrames == 0) {
            bluetooth::Initialize();
            DebugLog::log("[init] Bluetooth manager %s (deferred)",
                          bluetooth::IsAvailable() ? "initialized" : "unavailable");
            // Deferred with the rest of the network-dependent startup so the
            // grid is already on screen before anything reaches for the wire.
            publishUpdateState();   // bundled notes, before any network call
            startUpdateCheck(false);
        }
    }

    if (!m_audioStarted && m_audioFuture.valid() &&
        m_audioFuture.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        m_audioFuture.get();
        m_audio.setVolume(m_config.musicVolume);
        m_audio.setSfxVolume(m_config.sfxVolume);
        if (m_config.musicEnabled && !m_lockScreen.isLocked()) m_audio.play();
        m_loadedSoundPreset = resolveSoundPresetId(m_config.soundPreset);
        m_audioStarted = true;
        DebugLog::log("[init] Audio ready (deferred)");
    }

    if (m_presetChangePending && m_audioFuture.valid() &&
        m_audioFuture.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        m_audioFuture.get();
        m_audio.setVolume(m_config.musicVolume);
        m_audio.setSfxVolume(m_config.sfxVolume);
        if (m_config.musicEnabled && !m_lockScreen.isLocked())
            m_audio.play();
        m_loadedSoundPreset = m_pendingSoundPreset.empty() ? resolveSoundPresetId(m_config.soundPreset)
                                                           : m_pendingSoundPreset;
        m_pendingSoundPreset.clear();
        m_presetChangePending = false;
        DebugLog::log("[audio] Preset change complete: %s", m_config.soundPreset.c_str());
    }

    if (m_settingsNeedRefresh && m_settings) {
        m_settingsNeedRefresh = false;
        m_settings->refreshTranslations();
    }

    if (m_pendingNetConnect) {
        m_pendingNetConnect = false;
        // Metadata and gallery calls run on the worker pool. Do not make the
        // system Network settings wait for one of their HTTP timeouts while
        // this applet is closing.
        DebugLog::log("[launcher] cancelling HTTP for NetConnect handoff");
        themeshop::http::cancelPendingRequests();
        m_launcher.launchNetConnect();
        return;
    }

#ifdef SWITCHU_MENU
    {
        AppletStorage notifySt;
        while (R_SUCCEEDED(appletPopInteractiveInData(&notifySt))) {
            switchu::smi::DaemonNotification notif{};
            s64 sz = 0;
            appletStorageGetSize(&notifySt, &sz);
            if (sz >= (s64)sizeof(notif))
                appletStorageRead(&notifySt, 0, &notif, sizeof(notif));
            appletStorageClose(&notifySt);

            if (notif.magic != switchu::smi::kNotifyMagic) continue;
            DebugLog::log("[notify] msg=%u", (unsigned)notif.msg);

            switch (notif.msg) {
            case switchu::smi::MenuMessage::HomeRequest:
                m_sysMsg.pushAction(SysAction::HomeButton);
                break;
            case switchu::smi::MenuMessage::ApplicationExited:
                m_launcher.setAppRunning(false);
                m_launcher.setAppHasForeground(false);
                m_launcher.setSuspendedTitleId(0);
                m_sysMsg.pushAction(SysAction::HomeButton);
                break;
            case switchu::smi::MenuMessage::ApplicationSuspended:
                m_launcher.setAppRunning(true);
                m_launcher.setAppHasForeground(false);
                m_launcher.setSuspendedTitleId(notif.app_id);
                m_sysMsg.pushAction(SysAction::HomeButton);
                break;
            case switchu::smi::MenuMessage::AppRecordsChanged:
            case switchu::smi::MenuMessage::GameCardMountFailure:
                m_refreshQueued = true;
                m_deferredRefreshFrames = std::max(m_deferredRefreshFrames, 3);
                break;
            case switchu::smi::MenuMessage::SleepSequence:
                // Lock on the way under, so the console is already in the state
                // the owner will come back to. Best effort: Horizon may suspend
                // the applet before this runs, which is why WakeUp locks too.
                m_lockScreen.lockNow();
                break;
            case switchu::smi::MenuMessage::WakeUp:
                // The lock screen exists for this moment. Waking is the one
                // thing that raises it, and it is not optional, the same way
                // the stock home menu does not offer to skip it.
                m_lockScreen.lockNow();
                m_lockScreen.showAfterSystemWake();
                // Rendering is restored either way: the black frame belongs to
                // the lock screen, but a stopped presentation loop would strand
                // any menu, locked or not.
                // A console that just slept on its own is not still idle.
                m_lockScreen.resetSleepCountdown();
                m_lockScreenLowPowerPrimed = false;
                app().setRenderEnabled(true);
                break;
            case switchu::smi::MenuMessage::AppViewFlagsUpdate: {
                uint64_t tid = notif.app_id;
                uint32_t flags = notif.payload;
                m_model.updateViewFlags(tid, flags);
                for (auto& icon : m_grid->allIcons()) {
                    if (icon->titleId() == tid) {
                        bool launchable = (flags == 0) ||
                            (flags & switchu::ns::AppViewFlag_CanLaunch);
                        icon->setNotLaunchable(!launchable);
                        icon->setIsGameCard(
                            flags & switchu::ns::AppViewFlag_IsGameCard);
                        break;
                    }
                }
                break;
            }
            case switchu::smi::MenuMessage::BatteryStatusChanged: {
                const uint32_t percent = switchu::smi::batteryPayloadPercentage(notif.payload);
                const bool charging = switchu::smi::batteryPayloadCharging(notif.payload);
                // The battery widget reads these two members and nothing ever
                // wrote them, so its console ring sat at 0% while the HUD above
                // it showed the real charge from the same notification.
                m_consoleBatteryPercent = static_cast<int>(percent);
                m_consoleBatteryCharging = charging;
                if (m_battery) {
                    m_battery->setBatteryStatus(percent, charging);
                    DebugLog::log("[battery] daemon status percent=%u charging=%d",
                                  (unsigned)percent,
                                  charging ? 1 : 0);
                }
                if (m_quickSettings) {
                    m_quickSettings->setBatteryStatus(static_cast<int>(percent), charging);
                }
                break;
            }
            default:
                break;
            }
        }
    }
    m_sysMsg.pump();
    if (m_refreshCooldownFrames > 0)
        --m_refreshCooldownFrames;
    if (m_deferredRefreshFrames > 0)
        --m_deferredRefreshFrames;
    if (m_refreshQueued && m_deferredRefreshFrames == 0 &&
        !m_asyncRefreshPending && m_refreshCooldownFrames == 0) {
        DebugLog::log("[update] deferred refresh triggered, starting refreshAppList");
        refreshAppList();
    }
    if (m_asyncRefreshPending && m_appLoader.isReady()) {
        finalizeRefresh();
    }
    pollPlaytimeRefresh();
#endif

    bool debugTouchBlocked = false;
#ifdef SWITCHU_DEBUG_UI
    debugTouchBlocked = m_showDebugOverlay;
#endif

    // Shortcuts that read the pad straight rather than going through focus
    // dispatch, so blocking focusRoot() is not enough to stop them: the lock
    // screen has to be asked about here too.
    const bool lockScreenUp = m_lockScreen.isLocked();

    if (!lockScreenUp && handleAccessibilityToggleCombo()) {
        m_plusExitPending = false;
        m_plusExitPendingTimer = 0.f;
    }

    if (!lockScreenUp)
        handleFrameDumpShortcut();
    syncFrameDumpCapture();

    if (!lockScreenUp) {
        handleSortShortcutRelease(dt);
        handleZlShortcutRelease(dt);
    }
    syncUpdateCheck();
    syncUpdateDownload();
    pollAutoNtpSync(dt);

    if (!m_newGamePromptChecked && !lockScreenUp && !m_fastReturnRequested && !m_allApps.empty()) {
        m_newGamePromptChecked = true;
        checkNewGameSteamGridDbPrompt();
    }

    if (!app().input().isDown(nxui::Button::Plus) || !app().input().isDown(nxui::Button::Minus))
        m_accessibilityToggleComboHeld = false;

#ifdef SWITCHU_MENU
    // Route Plus from frame input so transient grid rebuilds cannot lose the
    // focused icon's action binding.
    if (!lockScreenUp &&
        app().input().isDown(nxui::Button::Plus) &&
        !app().input().isDown(nxui::Button::Minus) &&
        m_navigator.route() == switchu::navigation::Route::Home &&
        !m_editMode &&
        !(m_contextMenu && m_contextMenu->isActive()) &&
        !(m_dialog && m_dialog->isActive()) &&
        !(m_settings && m_settings->isActive()) &&
        !(m_themeShop && m_themeShop->isActive()) &&
        !(m_gameGallery && m_gameGallery->isActive()) &&
        !(m_gameMods && m_gameMods->isActive()) &&
        !(m_gameCheats && m_gameCheats->isActive()) &&
        !(m_gameDetails && m_gameDetails->isActive()) &&
        !(m_gameOptions && m_gameOptions->isActive()) &&
        !(m_folderOptions && m_folderOptions->isActive()) &&
        !(m_controllerTest && m_controllerTest->isActive()) &&
        !(m_textEntry && m_textEntry->isActive()) &&
        !(m_userSelect && m_userSelect->isActive())) {
        auto* current = focusManager().current();
        if (current && current->tag() == "glossy_icon" && m_grid) {
            auto* icon = static_cast<GlossyIcon*>(current);
            const auto& icons = m_grid->allIcons();
            const auto found = std::find_if(
                icons.begin(), icons.end(),
                [icon](const auto& candidate) { return candidate.get() == icon; });
            const int index = found == icons.end()
                ? -1 : static_cast<int>(std::distance(icons.begin(), found));
            if (index >= 0 && index < m_model.count()) {
                const AppEntry& entry = m_model.at(index);
                if (entry.titleId == 0 || entry.kind == GridEntryKind::Empty) {
                    if (m_openFolderId == 0)
                        showAddContextMenu(index, icon->focusRect());
                } else if (entry.isFolder()) {
                    showFolderContextMenu(entry.folderId);
                } else if (entry.isWidget()) {
                    showWidgetOptionsMenu(entry.widgetId, index, icon->focusRect());
                } else if (entry.isApplication()) {
                    // Applications keep this fork's dossier. The 1.2 tabbed
                    // game screen replaced it during the merge and lost the
                    // cover, installed version, mod summary and play time the
                    // dossier shows. Folders, widgets and empty slots keep
                    // their 1.2 menus, which have no fork equivalent.
                    showIconOptions();
                }
            }
        }
    }
#endif

    if (!lockScreenUp &&
        app().input().isDown(nxui::Button::Minus) &&
        !app().input().isDown(nxui::Button::Plus) &&
        m_navigator.route() == switchu::navigation::Route::Home &&
        !m_editMode &&
        !(m_quickSettings && m_quickSettings->isActive()) &&
        !(m_contextMenu && m_contextMenu->isActive()) &&
        !(m_dialog && m_dialog->isActive()) &&
        !(m_settings && m_settings->isActive()) &&
        !(m_themeShop && m_themeShop->isActive()) &&
        !(m_gameGallery && m_gameGallery->isActive()) &&
        !(m_gameMods && m_gameMods->isActive()) &&
        !(m_gameCheats && m_gameCheats->isActive()) &&
        !(m_gameDetails && m_gameDetails->isActive()) &&
        !(m_gameOptions && m_gameOptions->isActive()) &&
        !(m_folderOptions && m_folderOptions->isActive()) &&
        !(m_controllerTest && m_controllerTest->isActive()) &&
        !(m_textEntry && m_textEntry->isActive()) &&
        !(m_mediaCenterScreen && m_mediaCenterScreen->isActive()) &&
        !(m_userSelect && m_userSelect->isActive())) {
        toggleAppLayoutMode();
    }

    if (!lockScreenUp && m_plazaScreen && m_plazaScreen->isActive()) {
        m_plazaScreen->handleInput(app().input(), dt);
    }

    if (!lockScreenUp && m_mediaCenterScreen && m_mediaCenterScreen->isActive()) {
        m_mediaCenterScreen->handleInput(app().input(), dt);
    }

#ifdef SWITCHU_MENU
    // ZL and ZR are plain actions, so they fired once per press while the d-pad
    // already repeated through Application's navigation hold. Holding either one
    // now keeps the carousel moving, after a pause long enough that a deliberate
    // single press is still a single step, and quickening once it is clearly
    // being held. The d-pad is deliberately left to the engine's own repeat so a
    // held direction cannot be stepped twice.
    if (isCarouselLayout() && !lockScreenUp && !m_editMode
        && m_navigator.route() == switchu::navigation::Route::Home
        && focusRoot() == &rootBox()) {
        const bool holdLeft = app().input().isHeld(nxui::Button::ZL);
        const bool holdRight = app().input().isHeld(nxui::Button::ZR);
        const int holdDir = (holdRight && !holdLeft) ? 1 : ((holdLeft && !holdRight) ? -1 : 0);
        if (holdDir == 0 || holdDir != m_lineRepeatDir) {
            m_lineRepeatDir = holdDir;
            m_lineRepeatTimer = kLineRepeatDelay;
        } else {
            m_lineRepeatTimer -= dt;
            if (m_lineRepeatTimer <= 0.f) {
                m_lineRepeatTimer = kLineRepeatInterval;
                stepDynamicLine(holdDir);
            }
        }
    } else {
        m_lineRepeatDir = 0;
        m_lineRepeatTimer = kLineRepeatDelay;
    }
#endif

    if (m_plusExitPending) {
        m_plusExitPendingTimer -= dt;
        if (m_plusExitPendingTimer <= 0.f) {
            m_plusExitPending = false;
            m_plusExitPendingTimer = 0.f;
#ifdef SWITCHU_HOMEBREW
            m_audio.playSfx(Sfx::ModalHide);
            app().requestExit();
#endif
        }
    }

    if (!debugTouchBlocked
        && !lockScreenUp
        && !m_launchAnim->isPlaying()
        && !(m_dialog && m_dialog->isActive())
        && !(m_themeShop && m_themeShop->isActive())
        && !(m_gameGallery && m_gameGallery->isActive())
        && !(m_gameMods && m_gameMods->isActive())
        && !(m_gameCheats && m_gameCheats->isActive())
        && !(m_gameDetails && m_gameDetails->isActive())
        && !(m_settings && m_settings->isActive())
        && !(m_gameOptions && m_gameOptions->isActive())
        && !(m_folderOptions && m_folderOptions->isActive())
        && !(m_controllerTest && m_controllerTest->isActive())
        && !(m_textEntry && m_textEntry->isActive())
        && !(m_userSelect && m_userSelect->isActive())
        && !(m_quickSettings && m_quickSettings->isActive()))
    {
        handleTouch();
    }

    bool dialogActiveNow = (m_dialog && m_dialog->isActive());
    const bool platformPickerActive = m_platformPicker && m_platformPicker->isActive();
    const bool textEntryActive = (m_textEntry && m_textEntry->isActive());
    const bool progressDialogActive = (m_progressDialog && m_progressDialog->isActive());

    if (!debugTouchBlocked && textEntryActive) {
        m_textEntry->handleTouch(app().input());
    } else if (!debugTouchBlocked && !progressDialogActive) {
        if (!lockScreenUp && dialogActiveNow && !platformPickerActive)
            m_dialog->handleTouch(app().input());

        if (!lockScreenUp && !dialogActiveNow && m_themeShop && m_themeShop->isActive())
            m_themeShop->handleTouch(app().input());

        // L e R viram a página do catálogo de temas. Roteado daqui porque é onde a
        // entrada está: a loja é um widget e não alcança o Input por conta própria.
        if (!lockScreenUp && m_themeShop && m_themeShop->isActive()
            && !dialogActiveNow) {
            const int delta = app().input().isDown(nxui::Button::L) ? -1
                            : app().input().isDown(nxui::Button::R) ? 1 : 0;
            if (delta != 0 && m_themeShop->stepCataloguePage(delta))
                m_audio.playSfx(Sfx::Navigate);
        }

        if (!lockScreenUp && !dialogActiveNow && m_gameGallery && m_gameGallery->isActive())
            m_gameGallery->handleTouch(app().input());

        if (!lockScreenUp && !dialogActiveNow && m_gameMods && m_gameMods->isActive())
            m_gameMods->handleTouch(app().input());

        if (!lockScreenUp && !dialogActiveNow && m_gameCheats && m_gameCheats->isActive())
            m_gameCheats->handleTouch(app().input());

        if (!lockScreenUp && !dialogActiveNow && !(m_gameMods && m_gameMods->isActive())
            && !(m_gameCheats && m_gameCheats->isActive())
            && m_gameDetails && m_gameDetails->isActive())
            m_gameDetails->handleTouch(app().input());

        // The settings overlay stays active behind the controller test so it can be
        // returned to, but it must not keep taking touches while the test owns the
        // screen: the test routes raw touch input and the panel underneath is not
        // reachable.
        if (!lockScreenUp && !dialogActiveNow && m_settings && m_settings->isActive()
            && !(m_controllerTest && m_controllerTest->isActive()))
            m_settings->handleTouch(app().input());

        if (m_gameOptions && m_gameOptions->isActive()
            && !(m_steamGridDbPicker && m_steamGridDbPicker->isActive())
            && !(m_platformPicker && m_platformPicker->isActive()))
            m_gameOptions->handleTouch(app().input());

        if (m_steamGridDbPicker && m_steamGridDbPicker->isActive())
            m_steamGridDbPicker->handleTouch(app().input());

        if (!lockScreenUp && m_platformPicker && m_platformPicker->isActive())
            m_platformPicker->handleTouch(app().input());

        if (m_folderOptions && m_folderOptions->isActive())
            m_folderOptions->handleTouch(app().input());

        if (m_controllerTest && m_controllerTest->isActive())
            m_controllerTest->handleTouch(app().input());

        // The context menu owns folder and widget creation and is the topmost thing
        // on screen while it is up, so it takes touch before anything under it.
        if (!lockScreenUp && m_contextMenu && m_contextMenu->isActive())
            m_contextMenu->handleTouch(app().input());

        if (!lockScreenUp && m_openFolderId != 0 && m_folderHeader
            && m_folderHeader->isVisible() && app().input().touchDown()
            && !dialogActiveNow
            && !(m_contextMenu && m_contextMenu->isActive())
            && !(m_folderOptions && m_folderOptions->isActive())
            && m_folderHeader->rect().contains(app().input().touchX(),
                                               app().input().touchY())) {
            renameFolder(m_openFolderId);
        }

        if (!lockScreenUp && m_userSelect && m_userSelect->isActive())
            m_userSelect->handleTouch(app().input());

        if (!lockScreenUp && m_quickSettings && m_quickSettings->isActive())
            m_quickSettings->handleTouch(app().input());

        if (!lockScreenUp && m_screenSwapButton)
            m_screenSwapButton->handleTouch(app().input());

        if (!lockScreenUp && m_mediaCenterButton)
            m_mediaCenterButton->handleTouch(app().input());

        if (!lockScreenUp && m_mediaCenterScreen && m_mediaCenterScreen->isActive())
            m_mediaCenterScreen->handleTouch(app().input());

        if (!lockScreenUp && m_plazaScreen && m_plazaScreen->isActive())
            m_plazaScreen->handleTouch(app().input());
    }

    if (m_dialogWasActive && !dialogActiveNow) {
        if (isCurrentFocusableWidget(m_dialogReturnFocus)) {
            m_suppressNextNavigateSfx = true;
            focusManager().setFocus(m_dialogReturnFocus);
        }
        m_dialogReturnFocus = nullptr;
    }
    m_dialogWasActive = dialogActiveNow;

    if (!debugTouchBlocked && !lockScreenUp && m_userSelect && m_userSelect->isActive())
        m_userSelect->handleTouch(app().input());

    if (!debugTouchBlocked && !lockScreenUp && m_quickSettings && m_quickSettings->isActive())
        m_quickSettings->handleTouch(app().input());

    if (!debugTouchBlocked && !lockScreenUp && m_screenSwapButton)
        m_screenSwapButton->handleTouch(app().input());

    if (!debugTouchBlocked && !lockScreenUp && m_mediaCenterButton)
        m_mediaCenterButton->handleTouch(app().input());

    if (!debugTouchBlocked && !lockScreenUp && m_mediaCenterScreen && m_mediaCenterScreen->isActive())
        m_mediaCenterScreen->handleTouch(app().input());

    if (!debugTouchBlocked && !lockScreenUp && m_plazaScreen && m_plazaScreen->isActive())
        m_plazaScreen->handleTouch(app().input());

    if (!(m_userSelect && m_userSelect->isActive())
        && !(m_dialog && m_dialog->isActive())
        && !(m_quickSettings && m_quickSettings->isActive())
        && !m_launchAnim->isPlaying())
    {
        auto* cur = focusManager().current();
        if (!cur || !cur->isFocusable() || cur == m_quickSettings.get()) {
            if (m_themeShop && m_themeShop->isActive()) {
                focusManager().setFocus(m_themeShop.get());
            } else if (m_gameGallery && m_gameGallery->isActive()) {
                focusManager().setFocus(m_gameGallery.get());
            } else if (m_gameMods && m_gameMods->isActive()) {
                focusManager().setFocus(m_gameMods.get());
            } else if (m_gameCheats && m_gameCheats->isActive()) {
                focusManager().setFocus(m_gameCheats.get());
            } else if (m_gameDetails && m_gameDetails->isActive()) {
                focusManager().setFocus(m_gameDetails.get());
            } else if (m_settings && m_settings->isActive()) {
                focusManager().setFocus(m_settings.get());
            } else {
                auto* target = m_grid->focusManager().current();
                if (target)
                    focusManager().setFocus(target);
            }
        }
    }

    m_sidebar.update(dt, focusManager().current());

    if (m_pointerCursor) {
        bool showPointer = app().input().virtualPointerEnabled();
        m_pointerCursor->setVisible(showPointer);
        if (showPointer) {
            constexpr float kPointerSize = 30.f;
            float half = kPointerSize * 0.5f;
            nxui::Rect pointerRect {
                app().input().virtualPointerX() - half,
                app().input().virtualPointerY() - half,
                kPointerSize,
                kPointerSize,
            };
            m_pointerCursor->setOpacity(app().input().isTouching() ? 1.f : 0.92f);
            m_pointerCursor->moveTo(pointerRect, half, 0.06f);
        }
    }

    nxui::AnimationManager::instance().update(dt);

    // Sample the cursor after animation update to avoid one-frame lag.
    updateEditGhost(dt);

    if (m_editMode && m_editGhostIcon)
        m_editGhostIcon->update(dt);

    // Last, with this frame's state settled. The suspend countdown is held off
    // while a game is starting, while a title is running, and while a progress
    // dialog is up: none of those are somebody walking away. It deliberately
    // ignores the menu's own black low-power frame, which also turns rendering
    // off, because that state is the console sitting idle, which is precisely
    // when it should be heading for sleep.
    const bool sleepSuppressed =
        (m_launchAnim && m_launchAnim->isPlaying()) ||
        (m_progressDialog && m_progressDialog->isActive()) ||
        m_launcher.isAppRunning() ||
        (!app().renderEnabled() && !m_lockScreenLowPowerPrimed);

    m_sleepPlanTimer += dt;
    if (m_sleepPlanTimer >= kSleepPlanPollSeconds) {
        m_sleepPlanTimer = 0.f;
        SetSysSleepSettings plan{};
        if (R_SUCCEEDED(setsysGetSleepSettings(&plan))) {
            const bool docked = appletGetOperationMode() != AppletOperationMode_Handheld;
            m_lockScreen.setSleepDelaySeconds(
                sleepPlanSeconds(docked, docked ? plan.console_sleep_plan
                                                : plan.handheld_sleep_plan));
        }
    }

    m_lockScreen.update(dt, app().input(), sleepSuppressed);

    if (!m_lockScreen.hidesScene()) {
        // Any unlock starts rendering on this same loop iteration, so the
        // black low-power frame fades back into the normal menu immediately.
        m_lockScreenLowPowerPrimed = false;
        if (!app().renderEnabled()) {
            DebugLog::log("[lock] low power: presentation resumed");
            app().setRenderEnabled(true);
        }
    } else if (!m_lockScreenLowPowerPrimed) {
        // Leave rendering on for one frame: LockScreen::render() replaces the
        // menu with black, after which the static framebuffer can be kept.
        m_lockScreenLowPowerPrimed = true;
    } else {
        if (app().renderEnabled())
            DebugLog::log("[lock] low power: presentation stopped");
        app().setRenderEnabled(false);
        // Application::run() yields for 100 ms while presentation is disabled.
        // Do not add a second sleep here: the old 250 ms wait compounded with
        // that yield, reduced locked input polling to about 3 Hz, and dropped
        // short unlock presses after wake or undock.
    }
}

std::vector<WiiUMenuApp::ActionHint> WiiUMenuApp::buildActionHints() {
    std::vector<ActionHint> hints;
    auto& i18n = nxui::I18n::instance();
    auto add = [&](const std::string& icon, const std::string& label) {
        if (!icon.empty() && !label.empty())
            hints.push_back({icon, label});
    };
    auto addVoiceControls = [&]() {
        if (!m_config.accessibilityEnabled)
            return;
        add(buttonGlyph(nxui::Button::L), i18n.tr("hint.repeat", "Repeat"));
        add(buttonGlyph(nxui::Button::Plus) + buttonGlyph(nxui::Button::Minus),
            i18n.tr("hint.voice", "Voice"));
    };

    if (m_launchAnim && m_launchAnim->isPlaying())
        return hints;

    if (m_plazaScreen && m_plazaScreen->isActive()) {
        add(dpadGlyph(), i18n.tr("hint.navigate", "Select"));
        add(buttonGlyph(nxui::Button::L) + buttonGlyph(nxui::Button::R), "Rotate");
        add(buttonGlyph(nxui::Button::ZL) + buttonGlyph(nxui::Button::ZR), "Zoom");
        add(buttonGlyph(nxui::Button::A), i18n.tr("hint.select", "Open Game"));
        add(buttonGlyph(nxui::Button::B), i18n.tr("hint.back", "Return"));
        return hints;
    }

    if (m_quickSettings && m_quickSettings->isActive()) {
        add(dpadGlyph(), i18n.tr("hint.navigate", "Navigate"));
        add(buttonGlyph(nxui::Button::A), i18n.tr("hint.select", "Select"));
        add(buttonGlyph(nxui::Button::B), i18n.tr("hint.close", "Close"));
        return hints;
    }

    if (m_contextMenu && m_contextMenu->isActive()) {
        add(dpadGlyph(), i18n.tr("hint.navigate", "Navigate"));
        add(buttonGlyph(nxui::Button::A), i18n.tr("hint.select", "Select"));
        add(buttonGlyph(nxui::Button::B), i18n.tr("hint.back", "Back"));
        return hints;
    }

    if (m_dialog && m_dialog->isActive()) {
        add(buttonGlyph(nxui::Button::A), i18n.tr("hint.confirm", "Confirm"));
        add(buttonGlyph(nxui::Button::B), i18n.tr("hint.back", "Back"));
        addVoiceControls();
        return hints;
    }

    if (m_userSelect && m_userSelect->isActive()) {
        add(dpadGlyph(), i18n.tr("hint.navigate", "Navigate"));
        add(buttonGlyph(nxui::Button::A), i18n.tr("hint.select", "Select"));
        add(buttonGlyph(nxui::Button::B), i18n.tr("hint.back", "Back"));
        addVoiceControls();
        return hints;
    }

    if (m_mediaCenterScreen && m_mediaCenterScreen->isActive()) {
        add(dpadGlyph(), i18n.tr("hint.navigate", "Navigate"));
        add(buttonGlyph(nxui::Button::A), i18n.tr("hint.select", "Select"));
        if (m_mediaCenterScreen->isFocusedOnPlaylist()) {
            add(buttonGlyph(nxui::Button::X), i18n.tr("media.hint_delete", "Delete Track"));
        } else {
            add(buttonGlyph(nxui::Button::X), i18n.tr("media.hint_play_pause", "Play/Pause"));
        }
        add(buttonGlyph(nxui::Button::Y), i18n.tr("media.hint_rescan", "Rescan"));
        add(buttonGlyph(nxui::Button::B), i18n.tr("hint.back", "Back"));
        addVoiceControls();
        return hints;
    }

    if (m_steamGridDbPicker && m_steamGridDbPicker->isActive()) {
        add(dpadGlyph(), i18n.tr("hint.navigate", "Navigate"));
        add(buttonGlyph(nxui::Button::A), i18n.tr("hint.select", "Select"));
        add(buttonGlyph(nxui::Button::X), i18n.tr("hint.search", "Search"));
        add(buttonGlyph(nxui::Button::B), i18n.tr("hint.back", "Back"));
        addVoiceControls();
        return hints;
    }

    if (m_platformPicker && m_platformPicker->isActive()) {
        add(dpadGlyph(), i18n.tr("hint.navigate", "Navigate"));
        add(buttonGlyph(nxui::Button::A), i18n.tr("hint.select", "Select"));
        add(buttonGlyph(nxui::Button::B), i18n.tr("hint.back", "Back"));
        addVoiceControls();
        return hints;
    }

    if (m_themeShop && m_themeShop->isActive()) {
        add(dpadGlyph(), i18n.tr("hint.navigate", "Navigate"));
        add(buttonGlyph(nxui::Button::A), i18n.tr("hint.select", "Select"));
        add(buttonGlyph(nxui::Button::B), i18n.tr("hint.back", "Back"));
        add(buttonGlyph(nxui::Button::X), i18n.tr("hint.search", "Search"));
        addVoiceControls();
        return hints;
    }

    if (m_gameGallery && m_gameGallery->isActive()) {
        add(dpadGlyph(), i18n.tr("hint.navigate", "Navigate"));
        add(buttonGlyph(nxui::Button::A), i18n.tr("hint.select", "Select"));
        if (!m_gameGallery->isFullscreen())
            add(buttonGlyph(nxui::Button::X), i18n.tr("dialog.gallery_dimension_filter", "Change dimensions"));
        if (m_gameGallery->isFullscreen())
            add(buttonGlyph(nxui::Button::Plus), i18n.tr("dialog.gallery_apply", "Use image"));
        add(buttonGlyph(nxui::Button::Minus), i18n.tr("dialog.gallery_restore", "Restore default"));
        add(buttonGlyph(nxui::Button::B), i18n.tr("hint.back", "Back"));
        addVoiceControls();
        return hints;
    }

    if (m_gameMods && m_gameMods->isActive()) {
        add(dpadGlyph(), i18n.tr("hint.navigate", "Navigate"));
        add(buttonGlyph(nxui::Button::A), i18n.tr("hint.select", "Select"));
        add(buttonGlyph(nxui::Button::X), i18n.tr("dialog.mods_remove", "Remove"));
        add(buttonGlyph(nxui::Button::B), i18n.tr("hint.back", "Back"));
        addVoiceControls();
        return hints;
    }

    if (m_gameCheats && m_gameCheats->isActive()) {
        add(dpadGlyph(), i18n.tr("hint.navigate", "Navigate"));
        if (m_gameCheats->buildCount() > 1) {
            add(buttonGlyph(nxui::Button::L) + buttonGlyph(nxui::Button::R),
                i18n.tr("dialog.cheats_build_id", "Build"));
        }
        add(buttonGlyph(nxui::Button::A), i18n.tr("hint.select", "Select"));
        add(buttonGlyph(nxui::Button::X), i18n.tr("dialog.cheats_toggle_all", "Toggle all"));
        add(buttonGlyph(nxui::Button::B), i18n.tr("hint.back", "Back"));
        addVoiceControls();
        return hints;
    }

    if (m_gameDetails && m_gameDetails->isActive()) {
        add(dpadGlyph(), i18n.tr("hint.navigate", "Navigate"));
        add(buttonGlyph(nxui::Button::A), i18n.tr("dialog.details_expand", "Expand image"));
        add(buttonGlyph(nxui::Button::B), i18n.tr("hint.back", "Back"));
        addVoiceControls();
        return hints;
    }

    if ((m_controllerTest && m_controllerTest->isActive()) ||
        (m_textEntry && m_textEntry->isActive()) ||
        (m_gameOptions && m_gameOptions->isActive()) ||
        (m_folderOptions && m_folderOptions->isActive()))
        return hints;

    if (m_activityLog && m_activityLog->isActive()) {
        add(dpadGlyph(), i18n.tr("hint.navigate", "Navigate"));
        if (m_activityLog->activeTabIndex() == 2 || m_activityLog->focusArea() == TabbedOverlayScreen::FocusArea::Content) {
            add(buttonGlyph(nxui::Button::A), i18n.tr("hint.select", "Select"));
        }
        if (m_activityLog->activeTabIndex() != 2) {
            add(buttonGlyph(nxui::Button::ZL) + buttonGlyph(nxui::Button::ZR),
                i18n.tr("activity_log.hint_step", "Change Date"));
            add(buttonGlyph(nxui::Button::Y), i18n.tr("activity_log.hint_today", "Today"));
        }
        add(buttonGlyph(nxui::Button::B), i18n.tr("hint.back", "Back"));
        addVoiceControls();
        return hints;
    }

    if (m_settings && m_settings->isActive()) {
        add(dpadGlyph(), i18n.tr("hint.navigate", "Navigate"));
        add(buttonGlyph(nxui::Button::A), i18n.tr("hint.select", "Select"));
        add(buttonGlyph(nxui::Button::B), i18n.tr("hint.back", "Back"));
        addVoiceControls();
        return hints;
    }

    if (m_editMode) {
        add(dpadGlyph(), i18n.tr("hint.move", "Move"));
        add(buttonGlyph(nxui::Button::A), i18n.tr("hint.place", "Place"));
        add(buttonGlyph(nxui::Button::B), m_openFolderId != 0
            ? i18n.tr("folder.leave_while_moving", "Leave folder")
            : i18n.tr("hint.cancel", "Cancel"));
        addVoiceControls();
        return hints;
    }

    if (m_openFolderId != 0)
        add(buttonGlyph(nxui::Button::B), i18n.tr("hint.back", "Back"));

    nxui::Widget* cur = focusManager().current();
    if (cur && cur->tag() == "glossy_icon") {
        auto* icon = static_cast<GlossyIcon*>(cur);
        if (icon->titleId() != 0) {
            const int index = findTitleIndex(icon->titleId());
            const AppEntry* entry = index >= 0 ? &m_model.at(index) : nullptr;
            if (entry && entry->isFolder()) {
                add(buttonGlyph(nxui::Button::A), i18n.tr("folder.open", "Open"));
                add(buttonGlyph(nxui::Button::Plus), i18n.tr("hint.options", "Options"));
                if (m_openFolderId == 0)
                    add(buttonGlyph(nxui::Button::Y), i18n.tr("hint.move", "Move"));
            } else if (entry && entry->isWidget()) {
                add(buttonGlyph(nxui::Button::Plus), i18n.tr("hint.options", "Options"));
                if (m_openFolderId == 0)
                    add(buttonGlyph(nxui::Button::Y), i18n.tr("hint.move", "Move"));
            } else {
#ifdef SWITCHU_MENU
            add(buttonGlyph(nxui::Button::A),
                m_launcher.isAppSuspended(icon->titleId())
                    ? i18n.tr("hint.resume", "Resume")
                    : i18n.tr("hint.launch", "Launch"));
            if (m_launcher.isAppSuspended(icon->titleId()))
                add(buttonGlyph(nxui::Button::X), i18n.tr("hint.close", "Close"));
            else if (m_openFolderId != 0)
                add(buttonGlyph(nxui::Button::X), i18n.tr("folder.remove_game", "Remove from folder"));
#else
            add(buttonGlyph(nxui::Button::A), i18n.tr("hint.open", "Open"));
#endif
            // The hint carries the current mode, not the word "Sort": a hint
            // that only names the button leaves someone pressing it to find
            // out what it does and what it just did.
            // Not in the dynamic line. Sorting projects a different order onto
            // the same slots, and the line renders slot order as a ring, so the
            // row rearranged itself around the cursor for no gain. Advertising
            // a button that now does nothing there would be worse than not
            // having it.
            if (m_openFolderId == 0 && !isCarouselLayout())
                add(buttonGlyph(nxui::Button::R), sortModeLabel());
#ifdef SWITCHU_MENU
            if (entry && entry->isApplication())
                add(buttonGlyph(nxui::Button::RStick), i18n.tr("hint.favorite", "Favorite"));
            add(buttonGlyph(nxui::Button::LStick), i18n.tr("quicksettings.hint_shortcut", "Quick Settings"));
            // The options menu was reachable and unannounced: every other
            // button on this icon is listed here, so somebody who never pressed
            // + had no way to learn that software information and delete exist.
            add(buttonGlyph(nxui::Button::Plus), i18n.tr("hint.options", "Options"));
#endif
            if (m_openFolderId == 0)
                add(buttonGlyph(nxui::Button::Y), i18n.tr("hint.move", "Move"));
            else
                add(buttonGlyph(nxui::Button::Y), i18n.tr("folder.move", "Move"));
            }
        } else if (m_openFolderId == 0) {
#ifdef SWITCHU_MENU
            add(buttonGlyph(nxui::Button::Plus), i18n.tr("add.title", "Add"));
#endif
            if (deletePageAvailable()) {
                add(buttonGlyph(nxui::Button::ZL), i18n.tr("folder.delete_page", "Delete page"));
            }
        } else if (m_openFolderId != 0 && deletePageAvailable()) {
            add(buttonGlyph(nxui::Button::ZL), i18n.tr("folder.delete_page", "Delete page"));
        }
    } else if (cur) {
        for (const auto& btn : m_sidebar.leftButtons()) {
            if (btn.get() == cur) {
                add(buttonGlyph(nxui::Button::A), btn->label());
                break;
            }
        }
        for (const auto& btn : m_sidebar.rightButtons()) {
            if (btn.get() == cur) {
                add(buttonGlyph(nxui::Button::A), btn->label());
                break;
            }
        }
        for (const auto& avatar : m_userAvatarButtons) {
            if (avatar.get() == cur) {
                add(buttonGlyph(nxui::Button::A), i18n.tr("hint.profile", "Profile"));
                break;
            }
        }
    }

    if (m_navigator.route() == switchu::navigation::Route::Home && !m_editMode) {
        add(buttonGlyph(nxui::Button::Minus),
            i18n.tr("hint.switch_layout", "Switch view"));
        add(buttonGlyph(nxui::Button::LStick),
            i18n.tr("quicksettings.hint_shortcut", "Quick Settings"));
    }
    addVoiceControls();

    return hints;
}

float WiiUMenuApp::hintCapsuleWidth(const std::string& icon, const std::string& label) {
    return kHintCapPadX * 2.f
         + m_fontIcons.measure(icon).x * kHintIconScale
         + kHintIconGap
         + m_fontSmall.measure(label).x * kHintTextScale;
}

void WiiUMenuApp::syncHintCapsules(float dt) {
    std::vector<ActionHint> hints = buildActionHints();
    if ((int)hints.size() > kHintMaxItems)
        hints.resize((size_t)kHintMaxItems);

    bool sameBindings = hints.size() == m_hintCapsules.size();
    for (size_t i = 0; sameBindings && i < hints.size(); ++i)
        sameBindings = hints[i].icon == m_hintCapsules[i].icon;

    if (!sameBindings) {
        m_hintCapsules.clear();
        m_hintCapsules.reserve(hints.size());
        for (const auto& h : hints) {
            HintCapsule capsule;
            capsule.icon = h.icon;
            capsule.label = h.label;
            capsule.width = capsule.widthFrom = capsule.widthTo =
                hintCapsuleWidth(h.icon, h.label);
            m_hintCapsules.push_back(std::move(capsule));
        }
        if (!hints.empty()) {
            if (!m_hintCapsulesInitialized) {
                m_hintCapsulesInitialized = true;
                m_hintContentReveal.setImmediate(1.f);
            } else {
                m_hintContentReveal.setImmediate(0.45f);
                m_hintContentReveal.set(1.f, 0.18f, nxui::Easing::outCubic);
            }
        }
    } else {
        for (size_t i = 0; i < hints.size(); ++i) {
            HintCapsule& capsule = m_hintCapsules[i];
            if (capsule.label == hints[i].label)
                continue;
            capsule.outgoing = capsule.label;
            capsule.label = hints[i].label;
            capsule.swapT = 0.f;
            capsule.widthFrom = capsule.width;
            capsule.widthTo = hintCapsuleWidth(capsule.icon, capsule.label);
            capsule.widthT = 0.f;
        }
    }

    for (HintCapsule& capsule : m_hintCapsules) {
        if (capsule.widthT < 1.f) {
            capsule.widthT = std::min(1.f, capsule.widthT + dt / kHintWidthDur);
            capsule.width = capsule.widthFrom +
                (capsule.widthTo - capsule.widthFrom) *
                nxui::Easing::outCubic(capsule.widthT);
        }
        if (capsule.swapT < 1.f) {
            capsule.swapT = std::min(1.f, capsule.swapT + dt / kHintSwapDur);
            if (capsule.swapT >= 1.f)
                capsule.outgoing.clear();
        }
    }
}

void WiiUMenuApp::renderActionHintBar(nxui::Renderer& ren) {
    const int count = (int)m_hintCapsules.size();
    if (count <= 0)
        return;

    // The row is right-aligned and the title pill is centred, so a long title
    // ran straight into the first capsule. Wrapping is already how this bar
    // handles not fitting; it just never knew what else was on the line. Giving
    // it the space the pill leaves free turns the overlap into a second row,
    // which grows upward and away from the pill. Overlays do not show the pill,
    // so they keep the full width available to stay on a single row.
    float rowMaxW = (focusRoot() != &rootBox()) ? 1200.f : kHintRowMaxW;
    if (focusRoot() == &rootBox() && m_titlePill && m_titlePill->isVisible()) {
        const nxui::Rect pill = m_titlePill->rect();
        if (pill.width > 1.f) {
            const float free = 1280.f - kHintEdgeX - pill.right() - kHintPillGap;
            rowMaxW = std::clamp(free, kHintRowMinW, kHintRowMaxW);
        }
    }

    std::vector<std::pair<int, int>> rows;
    for (int i = 0; i < count;) {
        float width = 0.f;
        int end = i;
        while (end < count) {
            const float add = m_hintCapsules[(size_t)end].width +
                              (end > i ? kHintCapGap : 0.f);
            if (end > i && width + add > rowMaxW)
                break;
            width += add;
            ++end;
        }
        rows.emplace_back(i, end);
        i = end;
    }

    const float reveal = std::clamp(m_hintContentReveal.value(), 0.f, 1.f);
    const float blockH = rows.size() * kHintCapH +
                         (rows.size() - 1) * kHintRowGap;
    float y = 720.f - kHintEdgeY - blockH + (1.f - reveal) * 4.f;
    const nxui::Color tint = m_theme.panelBase.withAlpha(
        m_theme.mode == nxui::ThemeMode::Dark ? 0.30f : 0.24f);

    for (const auto& row : rows) {
        const int first = row.first;
        const int last = row.second;
        float rowW = 0.f;
        for (int i = first; i < last; ++i)
            rowW += m_hintCapsules[(size_t)i].width +
                    (i > first ? kHintCapGap : 0.f);

        float x = 1280.f - kHintEdgeX - rowW;
        for (int i = first; i < last; ++i) {
            const HintCapsule& capsule = m_hintCapsules[(size_t)i];
            const nxui::Rect cap = {x, y, capsule.width, kHintCapH};
            const float radius = kHintCapH * 0.5f;
            ren.drawRoundedRect({cap.x, cap.y + 3.f, cap.width, cap.height},
                                nxui::Color(0.f, 0.f, 0.f, 0.14f * reveal), radius);
            ren.drawFrostedInset(cap, tint.withAlpha(tint.a * reveal),
                                 m_theme.panelBorder.withAlpha(0.24f * reveal),
                                 m_theme.panelHighlight.withAlpha(0.08f * reveal),
                                 radius, 0.86f);

            const nxui::Vec2 iconSize = m_fontIcons.measure(capsule.icon);
            const float iconW = iconSize.x * kHintIconScale;
            ren.drawText(capsule.icon,
                         {cap.x + kHintCapPadX,
                          cap.y + (kHintCapH - iconSize.y * kHintIconScale) * 0.5f},
                         &m_fontIcons,
                         m_theme.textPrimary.withAlpha(0.94f * reveal),
                         kHintIconScale);

            const float textX = cap.x + kHintCapPadX + iconW + kHintIconGap;
            const float swap = nxui::Easing::outCubic(
                std::clamp(capsule.swapT, 0.f, 1.f));
            ren.pushClipRect(cap);
            if (!capsule.outgoing.empty()) {
                const nxui::Vec2 outgoingSize = m_fontSmall.measure(capsule.outgoing);
                ren.drawText(capsule.outgoing,
                             {textX - 7.f * swap,
                              cap.y + (kHintCapH - outgoingSize.y * kHintTextScale) * 0.5f},
                             &m_fontSmall,
                             m_theme.textSecondary.withAlpha(
                                 0.90f * reveal * (1.f - swap)),
                             kHintTextScale);
            }
            const nxui::Vec2 labelSize = m_fontSmall.measure(capsule.label);
            ren.drawText(capsule.label,
                         {textX + 7.f * (1.f - swap),
                          cap.y + (kHintCapH - labelSize.y * kHintTextScale) * 0.5f},
                         &m_fontSmall,
                         m_theme.textSecondary.withAlpha(0.90f * reveal * swap),
                         kHintTextScale);
            ren.popClipRect();
            x += capsule.width + kHintCapGap;
        }
        y += kHintCapH + kHintRowGap;
    }
}

void WiiUMenuApp::renderActionHintPanel(nxui::Renderer& ren) {
    std::vector<ActionHint> hints = buildActionHints();
    if (hints.empty())
        return;

    constexpr float kIconScale = 0.66f;
    constexpr float kTextScale = 0.54f;
    constexpr float kRowH = 22.f;
    constexpr float kRowGap = 3.f;
    constexpr float kPadX = 10.f;
    constexpr float kPadY = 8.f;
    constexpr float kIconTextGap = 6.f;
    constexpr float kScreenMargin = 18.f;
    int count = std::min((int)hints.size(), kHintMaxItems);
    if (count <= 0)
        return;

    float contentW = 0.f;
    for (int i = 0; i < count; ++i) {
        nxui::Vec2 iconSize = m_fontIcons.measure(hints[(size_t)i].icon);
        nxui::Vec2 labelSize = m_fontSmall.measure(hints[(size_t)i].label);
        contentW = std::max(contentW,
                            iconSize.x * kIconScale + kIconTextGap + labelSize.x * kTextScale);
    }

    float panelW = std::clamp(contentW + kPadX * 2.f, 104.f, 210.f);
    float panelH = kPadY * 2.f + count * kRowH + (count - 1) * kRowGap;
    std::string signature;
    for (int i = 0; i < count; ++i) {
        signature += hints[(size_t)i].icon;
        signature += '\n';
        signature += hints[(size_t)i].label;
        signature += '\n';
    }

    if (!m_hintPanelInitialized) {
        m_hintPanelInitialized = true;
        m_hintPanelW.setImmediate(panelW);
        m_hintPanelH.setImmediate(panelH);
        m_hintContentReveal.setImmediate(1.f);
        m_hintSignature = signature;
    } else {
        if (std::abs(m_hintPanelW.target() - panelW) > 0.5f)
            m_hintPanelW.set(panelW, 0.20f, nxui::Easing::outCubic);
        if (std::abs(m_hintPanelH.target() - panelH) > 0.5f)
            m_hintPanelH.set(panelH, 0.20f, nxui::Easing::outCubic);
        if (m_hintSignature != signature) {
            m_hintSignature = signature;
            m_hintContentReveal.setImmediate(0.45f);
            m_hintContentReveal.set(1.f, 0.18f, nxui::Easing::outCubic);
        }
    }

    panelW = std::max(1.f, m_hintPanelW.value());
    panelH = std::max(1.f, m_hintPanelH.value());

    nxui::Rect panel = {
        1280.f - kScreenMargin - panelW,
        720.f - kScreenMargin - panelH,
        panelW,
        panelH
    };
    float radius = 16.f;

    ren.drawRoundedRect({panel.x + 0.f, panel.y + 4.f, panel.width, panel.height},
                        nxui::Color(0.f, 0.f, 0.f, 0.12f),
                        radius);

    nxui::LiquidGlassSettings savedGlass = ren.liquidGlassSettings();
    auto& glass = ren.liquidGlassSettings();
    glass.refractionIntensity = 0.018f;
    glass.blurIntensity = 0.10f;
    glass.noiseIntensity = 0.0f;
    glass.glowIntensity = 0.035f;
    glass.saturation = 0.96f;
    glass.opacityMultiplier = 1.0f;
    glass.roughness = 0.004f;
    glass.powerFactor = 18.0f;

    nxui::Color tint = m_theme.panelBase.withAlpha(m_theme.mode == nxui::ThemeMode::Dark ? 0.22f : 0.18f);
    ren.drawLiquidGlass(0, panel, radius, tint, 0.86f, m_theme.mode == nxui::ThemeMode::Dark ? 0.08f : 0.04f);
    ren.liquidGlassSettings() = savedGlass;

    ren.drawRoundedRect(panel, m_theme.panelBase.withAlpha(m_theme.mode == nxui::ThemeMode::Dark ? 0.10f : 0.08f), radius);

    ren.pushClipRect(panel.shrunk(3.f));

    float reveal = std::clamp(m_hintContentReveal.value(), 0.f, 1.f);
    float y = panel.y + kPadY + (1.f - reveal) * 4.f;
    for (int i = 0; i < count; ++i) {
        const auto& hint = hints[(size_t)i];
        nxui::Vec2 iconSize = m_fontIcons.measure(hint.icon);
        nxui::Vec2 labelSize = m_fontSmall.measure(hint.label);
        float iconX = panel.x + kPadX;
        float iconY = y + (kRowH - iconSize.y * kIconScale) * 0.5f;
        float labelX = iconX + iconSize.x * kIconScale + 7.f;
        float labelY = y + (kRowH - labelSize.y * kTextScale) * 0.5f;

        ren.drawText(hint.icon, {iconX, iconY}, &m_fontIcons,
                     m_theme.textPrimary.withAlpha(0.88f * reveal), kIconScale);
        ren.drawText(hint.label, {labelX, labelY}, &m_fontSmall,
                     m_theme.textSecondary.withAlpha(0.82f * reveal), kTextScale);
        y += kRowH + kRowGap;
    }

    ren.popClipRect();
}

bool WiiUMenuApp::pagingAvailable() {
    if (m_navigator.route() != switchu::navigation::Route::Home
        || focusRoot() != &rootBox() || !m_grid)
        return false;
    // Neither carousel view has pages; ZL/ZR step them one icon at a time, so
    // the arrows are offered whenever there is a neighbour to step to.
    if (isCarouselLayout())
        return dynamicLineNeighbour(-1) >= 0 || dynamicLineNeighbour(+1) >= 0;
    return m_grid->totalPages() > 1;
}

// Next focusable icon on one side of the focused one, wrapping around the end
// of the line, or -1 when there is no other one to move to. ZL/ZR and the d-pad
// share this, so both cycle the carousel the same way.
int WiiUMenuApp::dynamicLineNeighbour(int dir) const {
    if (!m_grid || dir == 0)
        return -1;
    const auto& icons = m_grid->allIcons();
    const int count = static_cast<int>(icons.size());
    const int focused = m_grid->focusedGlobalIndex();
    if (focused < 0 || count <= 0)
        return -1;
    const int step = dir > 0 ? 1 : -1;
    for (int offset = 1; offset <= count; ++offset) {
        const int candidate = ((focused + step * offset) % count + count) % count;
        if (candidate == focused)
            break;
        const auto& icon = icons[static_cast<std::size_t>(candidate)];
        if (icon && icon->isFocusable())
            return candidate;
    }
    return -1;
}

bool WiiUMenuApp::stepDynamicLine(int dir) {
    const int target = dynamicLineNeighbour(dir);
    if (target < 0 || !m_grid->focusGlobalIndex(target))
        return false;
    if (auto* focused = m_grid->focusManager().current())
        focusManager().setFocus(focused);
    m_audio.playSfx(Sfx::PageChange);
    kickPageArrow(dir);
    return true;
}

nxui::Rect WiiUMenuApp::pageArrowRect(bool left) {
    const float inset = isCarouselLayout() ? kLineArrowInset : kPageArrowInset;
    const float cx = left ? inset : 1280.f - inset;
    const float cy = m_arrowCenterY.value();
    return {cx - kPageArrowW * 0.5f, cy - kPageArrowH * 0.5f,
            kPageArrowW, kPageArrowH};
}

void WiiUMenuApp::kickPageArrow(int dir) {
    (dir < 0 ? m_arrowAnimLeft : m_arrowAnimRight).press = 1.f;
}

bool WiiUMenuApp::addPageAvailable() {
    if (isCarouselLayout())
        return false;
    if (!m_grid || m_editMode)
        return false;
    if (m_navigator.route() != switchu::navigation::Route::Home ||
        focusRoot() != &rootBox())
        return false;

    if (m_openFolderId != 0) {
        const auto* folder = m_folderStore.find(m_openFolderId);
        if (!folder || folder->pageCount >= switchu::folders::kMaxFolderPages)
            return false;
        return m_grid->currentPage() >= m_grid->totalPages() - 1;
    }

    if (m_config.dynamicPages) {
        if (m_grid->totalPages() >= switchu::folders::kMaxFolderPages)
            return false;
        return m_grid->currentPage() >= m_grid->totalPages() - 1;
    }

    return false;
}

void WiiUMenuApp::createFolderPage() {
    if (m_openFolderId == 0 || !m_grid)
        return;
    const auto* folder = m_folderStore.find(m_openFolderId);
    if (!folder)
        return;

    const auto dimensions = folderGridDimensions(m_openFolderId);
    const int perPage = std::max(1, dimensions.first * dimensions.second);
    const int pages = std::max(folder->pageCount, m_grid->totalPages());
    if (pages >= switchu::folders::kMaxFolderPages)
        return;
    if (!m_folderStore.setPageCount(m_openFolderId, pages + 1))
        return;
    if (!saveFoldersOrReport("add_folder_page"))
        return;

    applyDisplayModel(buildOpenFolderModel(m_openFolderId), 0, false);
    syncPageIndicator();
    const int target = pages;
    m_grid->setPage(target - 1);
    m_grid->startPageTransition(target);
    if (m_grid->focusGlobalIndex(target * perPage)) {
        if (auto* current = m_grid->focusManager().current())
            focusManager().setFocus(current);
    }
    m_addPageHold = 0.f;
    m_addPageTouchHold = false;
    kickPageArrow(+1);
    m_audio.playSfx(Sfx::ConfirmPositive);
    m_accessibility.announce(nxui::I18n::instance().tr(
        "folder.page_added", "Page added"), true, true);
    updateCursor();
}

void WiiUMenuApp::createHomePage() {
    if (m_openFolderId != 0 || !m_grid)
        return;
    const int cols = std::clamp(m_config.gridColumns, 3, 8);
    const int rows = std::clamp(m_config.gridRows, 2, 5);
    const int perPage = std::max(1, cols * rows);
    const int pages = m_grid->totalPages();
    if (pages >= switchu::folders::kMaxFolderPages)
        return;

    m_layoutSlots.resize(static_cast<size_t>((pages + 1) * perPage), 0);
    m_layoutDirty = true;
    saveMenuLayout();

    applyDisplayModel(buildRootFolderModel(), 0, false);
    syncPageIndicator();
    const int target = pages;
    m_grid->setPage(target - 1);
    m_grid->startPageTransition(target);
    if (m_grid->focusGlobalIndex(target * perPage)) {
        if (auto* current = m_grid->focusManager().current())
            focusManager().setFocus(current);
    }
    m_addPageHold = 0.f;
    m_addPageTouchHold = false;
    kickPageArrow(+1);
    m_audio.playSfx(Sfx::ConfirmPositive);
    m_accessibility.announce(nxui::I18n::instance().tr(
        "home.page_added", "Page added"), true, true);
    updateCursor();
}

bool WiiUMenuApp::deletePageAvailable() {
    if (m_appLayoutMode == AppLayoutMode::DynamicLine)
        return false;
    if (!m_grid || m_editMode)
        return false;
    if (m_navigator.route() != switchu::navigation::Route::Home ||
        focusRoot() != &rootBox())
        return false;

    const int page = m_grid->currentPage();
    const int perPage = std::max(1, m_grid->iconsPerPage());
    const int start = page * perPage;
    const int end = std::min(m_model.count(), start + perPage);

    // Check if the current page has any occupied items
    for (int i = start; i < end; ++i) {
        const auto& entry = m_model.at(i);
        if (entry.titleId != 0 && entry.kind != GridEntryKind::Empty &&
            entry.kind != GridEntryKind::WidgetContinuation)
            return false;
    }

    if (m_openFolderId != 0) {
        const auto* folder = m_folderStore.find(m_openFolderId);
        if (!folder || folder->pageCount <= 1)
            return false;
        return true;
    }

    if (m_config.dynamicPages) {
        if (m_grid->totalPages() <= 1)
            return false;
        return true;
    }

    return false;
}

void WiiUMenuApp::deleteFolderPage() {
    if (m_openFolderId == 0 || !m_grid)
        return;
    auto* folder = m_folderStore.find(m_openFolderId);
    if (!folder || folder->pageCount <= 1)
        return;

    const int currentPage = m_grid->currentPage();
    const auto dimensions = folderGridDimensions(m_openFolderId);
    const int perPage = std::max(1, dimensions.first * dimensions.second);
    const int start = currentPage * perPage;
    if (start < static_cast<int>(folder->titleIds.size())) {
        const int count = std::min(perPage, static_cast<int>(folder->titleIds.size()) - start);
        folder->titleIds.erase(folder->titleIds.begin() + start, folder->titleIds.begin() + start + count);
    }
    const int newPages = std::max(1, folder->pageCount - 1);
    if (!m_folderStore.setPageCount(m_openFolderId, newPages))
        return;
    if (!saveFoldersOrReport("delete_folder_page"))
        return;

    applyDisplayModel(buildOpenFolderModel(m_openFolderId), 0, false);
    syncPageIndicator();

    const int target = std::min(currentPage, newPages - 1);
    m_grid->setPage(target);
    if (m_grid->focusGlobalIndex(target * perPage)) {
        if (auto* current = m_grid->focusManager().current())
            focusManager().setFocus(current);
    }
    m_audio.playSfx(Sfx::ToggleOff);
    m_accessibility.announce(nxui::I18n::instance().tr(
        "folder.page_deleted", "Page deleted"), true, true);
    updateCursor();
}

void WiiUMenuApp::deleteHomePage() {
    if (m_openFolderId != 0 || !m_grid)
        return;
    const int totalPages = m_grid->totalPages();
    if (totalPages <= 1)
        return;

    const int cols = std::clamp(m_config.gridColumns, 3, 8);
    const int rows = std::clamp(m_config.gridRows, 2, 5);
    const int perPage = std::max(1, cols * rows);

    const int currentPage = m_grid->currentPage();
    const int start = currentPage * perPage;
    if (start < static_cast<int>(m_layoutSlots.size())) {
        const int count = std::min(perPage, static_cast<int>(m_layoutSlots.size()) - start);
        m_layoutSlots.erase(m_layoutSlots.begin() + start, m_layoutSlots.begin() + start + count);
        m_layoutDirty = true;
        saveMenuLayout();
    }

    applyDisplayModel(buildRootFolderModel(), 0, false);
    syncPageIndicator();

    const int target = std::min(currentPage, totalPages - 2);
    m_grid->setPage(target);
    if (m_grid->focusGlobalIndex(target * perPage)) {
        if (auto* current = m_grid->focusManager().current())
            focusManager().setFocus(current);
    }
    m_audio.playSfx(Sfx::ToggleOff);
    m_accessibility.announce(nxui::I18n::instance().tr(
        "home.page_deleted", "Page deleted"), true, true);
    updateCursor();
}

bool WiiUMenuApp::flipPage(int dir) {
    if (!m_grid || m_grid->isTransitioning())
        return false;
    if (isCarouselLayout())
        return stepDynamicLine(dir);
    const int page = m_grid->currentPage() + dir;
    if (page < 0 || page >= m_grid->totalPages())
        return false;
    if (m_editMode && m_editTargetIndex >= 0) {
        const int perPage = std::max(1, m_grid->iconsPerPage());
        m_editTargetIndex = page * perPage + (m_editTargetIndex % perPage);
        if (m_editTargetIndex >= m_model.count())
            m_editTargetIndex = std::max(0, m_model.count() - 1);
    }
    m_grid->startPageTransition(page);
    m_audio.playSfx(Sfx::PageChange);
    kickPageArrow(dir);
    return true;
}

void WiiUMenuApp::renderPageArrows(nxui::Renderer& ren) {
    if (m_plazaScreen && m_plazaScreen->isActive()) return;
    if (m_navigator.route() != switchu::navigation::Route::Home) return;
    if (focusRoot() != &rootBox()) return;

    constexpr float kGlyphScale = 0.70f;
    auto drawArrow = [&](bool left, const nxui::Texture& texture,
                         const PageArrowAnim& animation,
                         const std::string& glyph, bool plus, bool minus) {
        if (animation.show <= 0.002f || (!plus && !minus && !texture.valid()))
            return;

        const float eased = animation.show * animation.show *
                            (3.f - 2.f * animation.show);
        const float bump = animation.press * animation.press;
        const nxui::Rect base = pageArrowRect(left);
        const float outward = (left ? -1.f : 1.f) *
                              ((1.f - eased) * 16.f + bump * 9.f);
        const float holdVal = plus ? m_addPageHold : (minus ? m_deletePageHold : 0.f);
        const float grow = (plus || minus) ? 0.10f * holdVal : 0.f;
        const float scale = (0.86f + 0.14f * eased) *
                            (1.f + 0.18f * bump + grow);
        const float cx = base.x + base.width * 0.5f + outward;
        const float cy = base.y + base.height * 0.5f;
        const float width = base.width * scale;
        const float height = base.height * scale;

        if (plus || minus) {
            const float ring = std::min(width, height) * 0.40f;
            ren.drawCircle({cx, cy + 2.f}, ring,
                           nxui::Color(0.02f, 0.04f, 0.06f, 0.32f * eased), 28);
            ren.drawCircle({cx, cy}, ring,
                           m_theme.panelBase.withAlpha(0.88f * eased), 28);
            ren.drawCircle({cx, cy}, ring - 1.6f,
                           m_theme.panelHighlight.withAlpha(0.10f * eased), 28);
            const float bar = ring * 0.92f;
            const float thick = std::max(2.f, ring * 0.17f);
            const nxui::Color ink = m_theme.textPrimary.withAlpha(0.92f * eased);
            // Horizontal bar for both plus and minus
            ren.drawRoundedRect({cx - bar * 0.5f, cy - thick * 0.5f, bar, thick},
                                ink, thick * 0.5f);
            // Vertical bar only for plus
            if (plus) {
                ren.drawRoundedRect({cx - thick * 0.5f, cy - bar * 0.5f, thick, bar},
                                    ink, thick * 0.5f);
            }
            if (holdVal > 0.002f) {
                constexpr int kSegments = 44;
                const float radius = ring + 3.5f;
                const int lit = std::max(1,
                    (int)std::ceil(kSegments * holdVal));
                const nxui::Color arc = minus
                    ? nxui::Color(1.0f, 0.35f, 0.35f, 0.95f * eased)
                    : m_theme.cursorNormal.withAlpha(0.95f * eased);
                for (int i = 0; i < lit; ++i) {
                    const float a0 = -1.5707963f +
                        6.2831853f * (float)i / kSegments;
                    const float a1 = -1.5707963f +
                        6.2831853f * (float)(i + 1) / kSegments;
                    ren.drawLine({cx + std::cos(a0) * radius,
                                  cy + std::sin(a0) * radius},
                                 {cx + std::cos(a1) * radius,
                                  cy + std::sin(a1) * radius}, arc, 3.f);
                }
            }
        } else {
            ren.drawTexture(&texture,
                            {cx - width * 0.5f, cy - height * 0.5f,
                             width, height},
                            nxui::Color(1.f, 1.f, 1.f, eased));
        }

        const nxui::Vec2 glyphSize = m_fontIcons.measure(glyph);
        ren.drawText(glyph,
                     {cx - glyphSize.x * kGlyphScale * 0.5f,
                      cy + height * 0.5f + 6.f},
                     &m_fontIcons,
                     m_theme.textPrimary.withAlpha(0.9f * eased),
                     kGlyphScale);
    };

    drawArrow(true, m_arrowTexLeft, m_arrowAnimLeft,
              buttonGlyph(nxui::Button::ZL), false, m_deletePageMode);
    drawArrow(false, m_arrowTexRight, m_arrowAnimRight,
              buttonGlyph(nxui::Button::ZR), m_addPageMode, false);
}

void WiiUMenuApp::onRender(nxui::Renderer& ren) {
    if (m_folderCaptureRequested) {
        if (ren.gpu().offscreenReady()) {
            // Capture one complete HOME frame for the folder transition. Keep
            // the fork's compact blur kernel to avoid wide-tap lattice noise.
            // captureToOffscreen() writes OFF_SCENE, which is half resolution,
            // while applyBlur() works on the full-resolution OFF_SHARP_A/B pair
            // and leaves its result in OFF_SHARP_A. Copying target 0 into 2
            // therefore published the half-res scene capture, never the blurred
            // frame, and FolderBackdrop drew that quarter-sized image stretched
            // across the full 1280x720 ÃƒÂ¢Ã¢â€šÂ¬Ã¢â‚¬Â the doubled, smeared frame seen when a
            // folder opened. Capture sharp, blur it, publish the blurred one.
            ren.captureToOffscreenSharp();
            ren.applyBlur(4.f, 2);
            ren.copyOffscreen(nxui::GpuDevice::OFF_SHARP_A,
                              nxui::GpuDevice::OFF_SETTINGS);
        }
        m_folderCaptureRequested = false;
        m_folderCaptureReady = true;
    }
    if (m_fastReturnStartupTick != 0) {
        const auto elapsedMs = static_cast<unsigned long>(
            armTicksToNs(armGetSystemTick() - m_fastReturnStartupTick) /
            1'000'000ULL);
        DebugLog::log("[first-frame] fast HOME return rendered in %lums", elapsedMs);
        m_fastReturnStartupTick = 0;
    }
    if (m_returnFadeTimer > 0.f) {
        float alpha = m_returnFadeTimer / kReturnFadeInDur;
        ren.drawRect({0, 0, 1280, 720}, nxui::Color(0, 0, 0, alpha));
    }
    if (m_tutorialStartupFadeTimer > 0.f &&
        (m_tutorialStartupFadeDeadlineTick == 0 ||
         armGetSystemTick() < m_tutorialStartupFadeDeadlineTick)) {
        float t = std::clamp(m_tutorialStartupFadeTimer / kTutorialStartupFadeDur, 0.f, 1.f);
        float alpha = nxui::Easing::outCubic(t);
        ren.drawRect({0, 0, 1280, 720}, nxui::Color(1.f, 1.f, 1.f, alpha));
    } else if (m_tutorialStartupFadeTimer > 0.f) {
        m_tutorialStartupFadeTimer = 0.f;
        m_tutorialStartupFadeDeadlineTick = 0;
    }

    if (m_touchHitIndex >= 0 && !m_touchOnFocused && app().input().isTouching()) {
        auto icons = m_grid->pageIcons();
        if (m_touchHitIndex < (int)icons.size()) {
            nxui::Rect r = icons[m_touchHitIndex]->focusRect();
            float cr = icons[m_touchHitIndex]->cornerRadius();
            ren.drawRoundedRect(r, nxui::Color(1.f, 1.f, 1.f, 0.18f), cr);
        }
    }

    syncPageIndicator();

    if (m_themeRenderDebugFrames > 0) {
        nxui::Widget* focus = focusManager().current();
        nxui::Widget* focusParent = focus ? focus->parent() : nullptr;
        std::vector<GlossyIcon*> pageIcons = m_grid ? m_grid->pageIcons() : std::vector<GlossyIcon*>();
        GlossyIcon* firstPageIcon = pageIcons.empty() ? nullptr : pageIcons.front();
        const nxui::Texture* firstTexture = firstPageIcon ? firstPageIcon->texture() : nullptr;

        DebugLog::log("[theme-render] preset=%s focus=%s parent=%s rootChildren=%zu contentChildren=%zu overlayChildren=%zu grid(all=%zu page=%zu vis=%d op=%.2f firstTex=%d firstIconVis=%d firstIconOp=%.2f) settings(active=%d vis=%d op=%.2f) themeshop(active=%d vis=%d op=%.2f)",
                      m_activePresetName.c_str(),
                      safeTag(focus),
                      safeTag(focusParent),
                      rootBox().children().size(),
                      m_contentLayer ? m_contentLayer->children().size() : 0,
                      m_overlayLayer ? m_overlayLayer->children().size() : 0,
                      m_grid ? m_grid->allIcons().size() : 0,
                      pageIcons.size(),
                      m_grid && m_grid->isVisible() ? 1 : 0,
                      m_grid ? m_grid->opacity() : 0.f,
                      (firstTexture && firstTexture->valid()) ? 1 : 0,
                      (firstPageIcon && firstPageIcon->isVisible()) ? 1 : 0,
                      firstPageIcon ? firstPageIcon->opacity() : 0.f,
                      (m_settings && m_settings->isActive()) ? 1 : 0,
                      (m_settings && m_settings->isVisible()) ? 1 : 0,
                      m_settings ? m_settings->opacity() : 0.f,
                      (m_themeShop && m_themeShop->isActive()) ? 1 : 0,
                      (m_themeShop && m_themeShop->isVisible()) ? 1 : 0,
                      m_themeShop ? m_themeShop->opacity() : 0.f);
        --m_themeRenderDebugFrames;
    }

    // Final topmost pass for move-mode ghost.
    if (m_editMode && m_editGhostIcon)
        m_editGhostIcon->render(ren);

    renderPageArrows(ren);
    if (m_config.actionHintStyle == "panel")
        renderActionHintPanel(ren);
    else
        renderActionHintBar(ren);

    // Over the hint bar as well: while the lock screen is up none of those
    // buttons do anything, so showing them would be a lie.
    m_lockScreen.render(ren);

#ifdef SWITCHU_DEBUG_UI
    if (m_debugOverlay) {
        m_debugOverlay->render(ren, app().input(), m_showDebugOverlay);
    }
#endif
}
