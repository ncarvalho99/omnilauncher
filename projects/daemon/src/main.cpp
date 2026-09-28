
#include <switch.h>
#include <switch/applets/friends_la.h>
#include <cstdlib>
#include <switchu/smi_protocol.hpp>
#include <switchu/smi_helpers.hpp>
#include <switchu/control_cache.hpp>
#include <switchu/ns_ext.hpp>
#include <switchu/file_log.hpp>
#include <switchu/sd_commit.hpp>
#include "app_manager.hpp"
#include "ecs.hpp"
#include "update_apply.hpp"
#include "self_uninstall.hpp"
#include "menu_launcher.hpp"
#include "mem_probe.hpp"
#include "library_applet_runner.hpp"
#include "system_action_queue.hpp"
#include <ctime>
#include <cstdio>
#include <cstring>
#include <atomic>
#include <mutex>
#include <vector>
#include <string>
#include <utility>
#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <system_error>

using namespace switchu;

static bool g_timeReady = false;
static bool g_setsysReady = false;
static bool g_setReady = false;
static bool g_nsReady = false;
static bool g_ldrShellReady = false;
static bool g_accountReady = false;
static bool g_nssuReady = false;
static bool g_avmReady = false;
static bool g_psmReady = false;
static bool g_lblReady = false;
static bool g_hidReady = false;

// libnx reads the clock exactly once, in __libnx_init_time(), and derives every
// later timestamp from the tick counter since then. The default __appInit calls
// it; this daemon has its own and never did, so the sample stayed at zero and
// every line it ever logged was dated 1970 plus the console's uptime -- which
// also went into the names of its archived logs. Not declared in any libnx
// header, so it is declared here.
extern "C" void __libnx_init_time(void);

#ifndef SWITCHU_VERSION
#define SWITCHU_VERSION "unknown"
#endif

extern "C" {
    u32 __nx_applet_type = AppletType_SystemApplet;
    u32 __nx_fs_num_sessions = 3;

    // time:s, the clock a system process is meant to use. Left at the default,
    // timeInitialize() opens time:u -- the application one -- and every clock
    // read from this process failed, so gettimeofday fell back to the tick
    // counter: the whole daemon log, and the names of its archived copies,
    // carried 1970 dates for as long as the console stayed up. The menu has
    // always set this (to time:a) and has always had real timestamps.
    TimeServiceType __nx_time_service_type = TimeServiceType_System;

    size_t __nx_heap_size = 0x800000;
}

// The controller applet is handed the npad configuration of whoever launches
// it, and this daemon never published one: it asked hid for a style set it had
// never set. qlaunch publishes the standard set and the sideways hold type, so
// that a lone Joy-Con can be registered horizontally and used as a player of
// its own; this does the same.
static void setupNpad() {
    static const HidNpadIdType npadIds[] = {
        HidNpadIdType_No1, HidNpadIdType_No2, HidNpadIdType_No3,
        HidNpadIdType_No4, HidNpadIdType_No5, HidNpadIdType_No6,
        HidNpadIdType_No7, HidNpadIdType_No8, HidNpadIdType_Handheld,
    };
    hidInitializeNpad();
    hidSetSupportedNpadIdType(npadIds, sizeof(npadIds) / sizeof(npadIds[0]));
    hidSetSupportedNpadStyleSet(HidNpadStyleSet_NpadStandard | HidNpadStyleTag_NpadSystemExt);
    hidSetNpadJoyHoldType(HidNpadJoyHoldType_Horizontal);
}

extern "C" void __appInit(void) {
    Result rc;

    svcOutputDebugString("[SwitchU-daemon] __appInit start", 34);
    daemon::initializeExternalContentAllocator();

    rc = smInitialize();
    if (R_FAILED(rc)) {
        svcOutputDebugString("[SwitchU-daemon] smInitialize FAIL", 35);
        diagAbortWithResult(MAKERESULT(Module_Libnx, 500));
    }

    rc = fsInitialize();
    if (R_FAILED(rc)) {
        svcOutputDebugString("[SwitchU-daemon] fsInitialize FAIL", 35);
        diagAbortWithResult(MAKERESULT(Module_Libnx, 501));
    }

    rc = appletInitialize();
    if (R_FAILED(rc)) {
        svcOutputDebugString("[SwitchU-daemon] appletInitialize FAIL", 39);
        diagAbortWithResult(MAKERESULT(Module_Libnx, 502));
    }
    svcOutputDebugString("[SwitchU-daemon] appletInitialize OK", 37);

    rc = timeInitialize();
    g_timeReady = R_SUCCEEDED(rc);
    if (R_FAILED(rc))
        svcOutputDebugString("[SwitchU-daemon] timeInitialize FAIL", 37);
    if (g_timeReady)
        __libnx_init_time();

    rc = setsysInitialize();
    g_setsysReady = R_SUCCEEDED(rc);
    if (R_FAILED(rc))
        svcOutputDebugString("[SwitchU-daemon] setsysInitialize FAIL", 39);

    rc = setInitialize();
    g_setReady = R_SUCCEEDED(rc);
    if (R_FAILED(rc))
        svcOutputDebugString("[SwitchU-daemon] setInitialize FAIL", 36);

    if (g_setsysReady) {
        SetSysFirmwareVersion fw = {};
        if (R_SUCCEEDED(setsysGetFirmwareVersion(&fw)))
            hosversionSet(MAKEHOSVERSION(fw.major, fw.minor, fw.micro) | BIT(31));
    }

    rc = nsInitialize();
    if (R_FAILED(rc)) {
        svcOutputDebugString("[SwitchU-daemon] nsInitialize FAIL", 35);
        diagAbortWithResult(rc);
    }
    g_nsReady = true;

    rc = ldrShellInitialize();
    if (R_FAILED(rc)) {
        svcOutputDebugString("[SwitchU-daemon] ldrShellInitialize FAIL", 41);
        diagAbortWithResult(rc);
    }
    g_ldrShellReady = true;

    rc = accountInitialize(AccountServiceType_System);
    if (R_FAILED(rc)) {
        svcOutputDebugString("[SwitchU-daemon] accountInitialize FAIL", 40);
        diagAbortWithResult(rc);
    }
    g_accountReady = true;

    rc = nssuInitialize();
    g_nssuReady = R_SUCCEEDED(rc);
    if (R_FAILED(rc))
        svcOutputDebugString("[SwitchU-daemon] nssuInitialize FAIL", 37);

    rc = avmInitialize();
    g_avmReady = R_SUCCEEDED(rc);
    if (R_FAILED(rc))
        svcOutputDebugString("[SwitchU-daemon] avmInitialize FAIL", 36);

    rc = psmInitialize();
    g_psmReady = R_SUCCEEDED(rc);
    if (R_FAILED(rc))
        svcOutputDebugString("[SwitchU-daemon] psmInitialize FAIL", 36);

    rc = lblInitialize();
    g_lblReady = R_SUCCEEDED(rc);
    if (R_FAILED(rc))
        svcOutputDebugString("[SwitchU-daemon] lblInitialize FAIL", 36);

    rc = hidInitialize();
    g_hidReady = R_SUCCEEDED(rc);
    if (R_FAILED(rc))
        svcOutputDebugString("[SwitchU-daemon] hidInitialize FAIL", 36);
    else
        setupNpad();

    rc = fsdevMountSdmc();
    if (R_FAILED(rc)) {
        svcOutputDebugString("[SwitchU-daemon] fsdevMountSdmc FAIL", 37);
        svcSleepThread(100'000'000ULL);
        rc = fsdevMountSdmc();
    }

    switchu::FileLog::open("daemon");
    // Which build this is, in the log of the process itself. The menu prints
    // its own version on the About tab, and for a long time that was the only
    // version anybody could read -- while an update left these two out of step
    // for a whole session, the log gave no way to notice.
    switchu::FileLog::log("[daemon] version %s", SWITCHU_VERSION);
    switchu::FileLog::log("[daemon] __appInit complete (sd mount: 0x%X)", rc);
    switchu::FileLog::log("[daemon] services time=%d setsys=%d set=%d ns=%d ldr=%d account=%d nssu=%d avm=%d psm=%d lbl=%d hid=%d",
                          g_timeReady ? 1 : 0,
                          g_setsysReady ? 1 : 0,
                          g_setReady ? 1 : 0,
                          g_nsReady ? 1 : 0,
                          g_ldrShellReady ? 1 : 0,
                          g_accountReady ? 1 : 0,
                          g_nssuReady ? 1 : 0,
                          g_avmReady ? 1 : 0,
                          g_psmReady ? 1 : 0,
                          g_lblReady ? 1 : 0,
                          g_hidReady ? 1 : 0);

    svcOutputDebugString("[SwitchU-daemon] __appInit done", 31);
}

extern "C" void __appExit(void) {
    switchu::FileLog::log("[daemon] __appExit");
    switchu::FileLog::close();

    if (g_hidReady) hidExit();
    if (g_lblReady) lblExit();
    if (g_psmReady) psmExit();
    if (g_avmReady) avmExit();
    if (g_nssuReady) nssuExit();
    if (g_accountReady) accountExit();
    if (g_ldrShellReady) ldrShellExit();
    if (g_nsReady) nsExit();
    if (g_setReady) setExit();
    if (g_setsysReady) setsysExit();
    if (g_timeReady) timeExit();

    appletExit();
    fsdevUnmountAll();
    fsExit();
    smExit();
}

static std::atomic<bool> g_running{true};
static std::atomic<bool> g_powerSequenceStarted{false};
static UEvent g_mainWakeEvent{};
static UEvent g_controlCacheWakeEvent{};
static Event g_generalChannelEvent{};
static bool g_generalChannelEventReady = false;
static std::atomic<bool> g_eventRefreshPending{false};
static std::atomic<bool> g_eventGcMountFailure{false};
static std::atomic<bool> g_batteryRefreshPending{true};
static std::atomic<Result> g_eventGcMountRc{0};
static bool g_initialEventSkipped = false;
static int  g_eventPollCountdown  = 0;
static int  g_eventPollsRemaining = 0;
// mainLoop sleeps 10 ms. Querying application views opens enough NS state to
// make the menu visibly stutter, so this must never become a 200 ms loop after
// a game exits or crashes.
constexpr int kViewPollIntervalTicks = 200; // 2 seconds
constexpr int kViewPollAttempts      = 6;   // observe settling without a storm
static int  g_menuRelaunchCooldown = 0;
static int  g_menuFastExitCount = 0;
static s32      g_lastRecordCount = 0;
static uint64_t g_lastRecordTids[1024] = {};
static uint32_t g_lastViewFlags[1024]  = {};

static constexpr s32 kMaxTrackedApplicationRecords = 1024;
static constexpr s32 kApplicationRecordChunkCount = 30;

struct DaemonAppCatalogEntry {
    uint64_t titleId = 0;
    uint32_t viewFlags = 0;
    bool startupUserKnown = false;
    uint8_t startupUserAccount = 1;
    uint8_t startupUserAccountOption = 0;
    std::string name;
};

static std::vector<DaemonAppCatalogEntry> g_appCatalog;
static std::atomic<bool> g_appCatalogRefreshPending{false};
// The catalogue changed while the menu was not on screen, so nobody could be
// told. Installing a game from a homebrew is exactly that: the installer is
// in front, the notification was raised and dropped, and the shortcut only
// appeared once something restarted the menu -- reported as "the first two
// games never showed up, then the third made all three appear at once".
static std::atomic<bool> g_catalogChangedWhileAway{false};
// The first rebuild of a boot only records what is installed; it has nothing to
// compare against. Invalidating cached names and icons starts from the second.
static bool g_catalogBaselineTaken = false;
static int g_appCatalogRefreshDelay = 0;
static constexpr const char* kAppCatalogPath = smi::kAppCatalogPath;
static constexpr const char* kAppCatalogTmpPath = smi::kAppCatalogTmpPath;
static std::mutex g_controlCacheQueueMutex;
static std::vector<uint64_t> g_controlCacheQueue;
static std::atomic<bool> g_controlCacheRefreshPending{false};
static std::atomic<int> g_controlCacheRefreshDelay{0};

enum class ActionType : uint32_t {
    LaunchApplication,
    ResumeApplication,
    OpenAlbum,
    OpenMiiEditor,
    OpenControllers,
    OpenControllerRemapping,
    OpenNetConnect,
    OpenUserPage,
    OpenUserCreator,
};

struct Action {
    ActionType type;
    uint64_t title_id = 0;
    AccountUid uid = {};
    smi::LaunchTransitionTrace transition{};
    uint64_t commandReceivedTick = 0;
#ifdef SWITCHU_PREFLIGHT_EDGE_TEST
    bool injectLaunchFailure = false;
#endif
#ifdef SWITCHU_RESUME_FAILURE_TEST
    bool injectResumeForegroundFailure = false;
#endif
};

static std::vector<Action> g_actionQueue;
static smi::MenuClosingArgs g_lastMenuClosingTrace{};
static uint64_t g_lastMenuClosingReceiveTick = 0;
static bool g_foregroundAppletActive = false;
static bool g_pendingForegroundAppletHome = false;
static uint8_t g_lastBatteryPercent = 0xFF;
static PsmChargerType g_lastChargerType = (PsmChargerType)0xFF;
static int g_batteryPollCountdown = 0;

static uint64_t tickDeltaUs(uint64_t start, uint64_t end) {
    if (start == 0 || end == 0 || end < start)
        return 0;
    return armTicksToNs(end - start) / 1'000ULL;
}

static bool shouldDeferViewPolling() {
    return daemon::app::isRunning() &&
           daemon::app::hasForeground() &&
           !daemon::menu_la::isActive() &&
           !g_foregroundAppletActive;
}

static bool listApplicationRecords(std::vector<switchu::ns::ExtApplicationRecord>& records,
                                   const char* tag) {
    records.clear();

    switchu::ns::ExtApplicationRecord chunk[kApplicationRecordChunkCount] = {};
    s32 offset = 0;
    while (offset < kMaxTrackedApplicationRecords) {
        s32 readCount = 0;
        Result rc = nsListApplicationRecord(
            reinterpret_cast<NsApplicationRecord*>(chunk),
            kApplicationRecordChunkCount,
            offset,
            &readCount);
        if (R_FAILED(rc)) {
            switchu::FileLog::log("[%s] nsListApplicationRecord FAIL: 0x%X offset=%d",
                                  tag, rc, offset);
            records.clear();
            return false;
        }
        if (readCount <= 0)
            break;

        const s32 remaining = kMaxTrackedApplicationRecords - offset;
        const s32 appendCount = readCount > remaining ? remaining : readCount;
        records.insert(records.end(), chunk, chunk + appendCount);
        offset += readCount;

        if (readCount < kApplicationRecordChunkCount)
            break;
    }

    std::sort(records.begin(), records.end(),
              [](const switchu::ns::ExtApplicationRecord& a,
                 const switchu::ns::ExtApplicationRecord& b) {
                  return a.id < b.id;
              });
    return true;
}

static bool queryApplicationViews(const std::vector<switchu::ns::ExtApplicationRecord>& records,
                                  std::vector<switchu::ns::ExtApplicationView>& views,
                                  const char* tag) {
    views.clear();
    if (records.empty())
        return true;

    std::vector<uint64_t> tids(records.size());
    for (size_t i = 0; i < records.size(); ++i)
        tids[i] = records[i].id;

    views.resize(records.size());
    Result rc = switchu::ns::queryApplicationViews(
        tids.data(),
        static_cast<int>(tids.size()),
        views.data());
    if (R_FAILED(rc)) {
        switchu::FileLog::log("[%s] queryApplicationViews FAIL: 0x%X", tag, rc);
        std::fill(views.begin(), views.end(), switchu::ns::ExtApplicationView{});
        return false;
    }

    return true;
}

static void enqueueControlCacheTitles(const std::vector<uint64_t>& titleIds) {
    std::lock_guard<std::mutex> lock(g_controlCacheQueueMutex);
    for (uint64_t titleId : titleIds) {
        if (titleId == 0)
            continue;
        if (std::find(g_controlCacheQueue.begin(), g_controlCacheQueue.end(), titleId) ==
            g_controlCacheQueue.end()) {
            g_controlCacheQueue.push_back(titleId);
        }
    }
}

static std::size_t controlCacheQueueSize() {
    std::lock_guard<std::mutex> lock(g_controlCacheQueueMutex);
    return g_controlCacheQueue.size();
}

static bool popControlCacheTitle(uint64_t& outTitleId) {
    std::lock_guard<std::mutex> lock(g_controlCacheQueueMutex);
    if (g_controlCacheQueue.empty())
        return false;

    outTitleId = g_controlCacheQueue.front();
    g_controlCacheQueue.erase(g_controlCacheQueue.begin());
    return true;
}

static bool writeAppCatalogFile() {
    std::error_code fsEc;
    std::filesystem::create_directory("sdmc:/config", fsEc);
    fsEc.clear();
    std::filesystem::create_directory("sdmc:/config/SwitchU", fsEc);

    std::ofstream f(kAppCatalogTmpPath, std::ios::binary | std::ios::trunc);
    if (!f.is_open()) {
        switchu::FileLog::log("[catalog] fopen tmp FAIL");
        return false;
    }

    uint32_t count = static_cast<uint32_t>(g_appCatalog.size());
    bool ok = static_cast<bool>(f.write(reinterpret_cast<const char*>(&count), sizeof(count)));
    for (const auto& ent : g_appCatalog) {
        if (!ok) break;

        smi::AppEntryHeader eh{};
        eh.title_id = ent.titleId;
        eh.name_len = static_cast<uint32_t>(ent.name.size());
        eh.icon_data_len = 0;
        eh.view_flags = ent.viewFlags;
        eh.startup_user_account = ent.startupUserAccount;
        eh.startup_user_account_option = ent.startupUserAccountOption;
        eh.startup_user_known = ent.startupUserKnown ? 1 : 0;

        ok = static_cast<bool>(f.write(reinterpret_cast<const char*>(&eh), sizeof(eh)));
        if (ok && eh.name_len > 0)
            ok = static_cast<bool>(f.write(ent.name.data(), static_cast<std::streamsize>(eh.name_len)));
    }

    f.close();
    ok = ok && static_cast<bool>(f);
    if (!ok) {
        fsEc.clear();
        std::filesystem::remove(kAppCatalogTmpPath, fsEc);
        switchu::FileLog::log("[catalog] write FAIL");
        return false;
    }

    // Back to the author's swap: one remove, one rename. The .bak parking
    // dance I added tripled the directory operations per rebuild, and the
    // author's build — which does not corrupt — never did any of it. The
    // window it was closing (a reader finding no catalog mid-swap) costs a
    // retry in getAppList; corrupting the card costs a reflash.
    fsEc.clear();
    std::filesystem::remove(kAppCatalogPath, fsEc);
    fsEc.clear();
    std::filesystem::rename(kAppCatalogTmpPath, kAppCatalogPath, fsEc);
    if (fsEc) {
        fsEc.clear();
        std::filesystem::remove(kAppCatalogTmpPath, fsEc);
        switchu::FileLog::log("[catalog] rename FAIL");
        return false;
    }

    return true;
}

static bool rebuildAppCatalog(const char* reason, bool* outChanged = nullptr) {
    std::vector<switchu::ns::ExtApplicationRecord> records;
    if (!listApplicationRecords(records, "catalog"))
        return false;

    std::vector<switchu::ns::ExtApplicationView> views;
    queryApplicationViews(records, views, "catalog");

    const s32 count = static_cast<s32>(records.size());

    // A title that just arrived or just left loses whatever was cached for it.
    //
    // The cache is keyed by title id and never expired, which is fine until an
    // id comes back meaning something else. A forwarder shortcut deleted and
    // recreated for a different app keeps its id, hasMeta() then says the work
    // is done, and the shortcut wears the old app's name and icon. Reported
    // exactly that way.
    //
    // Only from the second rebuild of a boot: the first one has no previous
    // list to compare against, and treating every title as new there would
    // re-download every icon on every boot.
    if (g_catalogBaselineTaken) {
        auto wasPresent = [&](uint64_t tid) {
            for (s32 i = 0; i < g_lastRecordCount && i < kMaxTrackedApplicationRecords; ++i)
                if (g_lastRecordTids[i] == tid) return true;
            return false;
        };
        auto isPresent = [&](uint64_t tid) {
            for (s32 i = 0; i < count; ++i)
                if (records[i].id == tid) return true;
            return false;
        };
        for (s32 i = 0; i < count; ++i) {
            const uint64_t tid = records[i].id;
            if (tid != 0 && !wasPresent(tid)) {
                switchu::control_cache::forget(tid);
                switchu::FileLog::log("[control-cache] forgot 0x%016lX (new record)", tid);
            }
        }
        for (s32 i = 0; i < g_lastRecordCount && i < kMaxTrackedApplicationRecords; ++i) {
            const uint64_t tid = g_lastRecordTids[i];
            if (tid != 0 && !isPresent(tid)) {
                switchu::control_cache::forget(tid);
                switchu::FileLog::log("[control-cache] forgot 0x%016lX (record gone)", tid);
            }
        }
    }
    g_catalogBaselineTaken = true;
    g_appCatalog.clear();
    g_appCatalog.reserve(count);

    // Resolve display name and startup-user policy here, on the daemon, so the
    // menu can build its grid straight from applist.bin. Otherwise every menu
    // cold start reopens one .meta file per installed title.
    std::vector<uint64_t> missingMeta;
    for (s32 i = 0; i < count; ++i) {
        const uint64_t tid = records[i].id;
        DaemonAppCatalogEntry ent;
        ent.titleId = tid;
        ent.viewFlags = views[i].flags;
        char fallbackName[17] = {};
        std::snprintf(fallbackName, sizeof(fallbackName), "%016lX",
                      static_cast<unsigned long>(tid));
        ent.name = fallbackName;

        switchu::control_cache::Meta meta{};
        if (tid != 0 && switchu::control_cache::readMeta(tid, meta)) {
            if (meta.name[0] != '\0')
                ent.name = meta.name;
            ent.startupUserKnown = true;
            ent.startupUserAccount = meta.startup_user_account;
            ent.startupUserAccountOption = meta.startup_user_account_option;
        } else if (tid != 0) {
            const char* known = switchu::control_cache::getKnownTitleName(tid);
            if (known)
                ent.name = known;
            missingMeta.push_back(tid);
        }

        g_appCatalog.push_back(std::move(ent));
    }
    enqueueControlCacheTitles(missingMeta);

    const s32 prevCount = g_lastRecordCount;
    bool changed = prevCount != count;
    if (!changed) {
        for (s32 i = 0; i < count; ++i) {
            if (g_lastRecordTids[i] != records[i].id) {
                changed = true;
                break;
            }
        }
    }
    g_lastRecordCount = count;
    for (s32 i = 0; i < count; ++i) {
        g_lastRecordTids[i] = records[i].id;
        g_lastViewFlags[i] = views[i].flags;
    }
    for (s32 i = count; i < prevCount && i < kMaxTrackedApplicationRecords; ++i) {
        g_lastRecordTids[i] = 0;
        g_lastViewFlags[i] = 0;
    }

    const bool ok = writeAppCatalogFile();
    switchu::FileLog::log("[catalog] rebuilt %d/%d apps reason=%s write=%d",
                          (int)g_appCatalog.size(), (int)count, reason, ok ? 1 : 0);
    g_appCatalog.clear();
    g_appCatalog.shrink_to_fit();
    if (outChanged)
        *outChanged = changed;
    return ok;
}

static void cancelViewPolling(const char* reason) {
    const bool hadPendingEvent = g_eventRefreshPending.exchange(false);
    if (hadPendingEvent || g_eventPollsRemaining > 0) {
        switchu::FileLog::log("[views] cancelling background poll (%s) pending=%d remaining=%d",
                              reason,
                              hadPendingEvent ? 1 : 0,
                              g_eventPollsRemaining);
    }
    g_eventPollCountdown = 0;
    g_eventPollsRemaining = 0;
}

static smi::SystemStatus buildSystemStatus(
    smi::MenuTransitionReason reason = smi::MenuTransitionReason::Unknown,
    uint64_t originTick = 0) {
    smi::SystemStatus st{};
    st.suspended_app_id = daemon::app::suspendedTitleId();
    st.app_running = daemon::app::isRunning();
    st.transition_origin_tick = originTick;
    st.transition_reason = reason;
    return st;
}

struct ScopedService {
    Service value{};

    ScopedService() = default;
    ScopedService(const ScopedService&) = delete;
    ScopedService& operator=(const ScopedService&) = delete;

    ~ScopedService() {
        serviceClose(&value);
    }
};

static Result openTimeAdminService(ScopedService& out) {
    const Result rc = smGetService(&out.value, "time:a");
    switchu::FileLog::log(
        "[settings-time] daemon open time:a rc=0x%X", rc);
    return rc;
}

static Result setLiveAutomaticCorrection(bool enabled) {
    ScopedService admin;
    Result rc = openTimeAdminService(admin);
    if (R_SUCCEEDED(rc)) {
        const u8 flag = enabled ? 1 : 0;
        rc = serviceDispatchIn(&admin.value, 101, flag);
    }

    switchu::FileLog::log(
        "[settings-time] daemon live automaticCorrection enabled=%d rc=0x%X",
        enabled ? 1 : 0, rc);
    return rc;
}

static Result setInternetTimeSync(bool enabled) {
    const Result setsysRc =
        setsysSetUserSystemClockAutomaticCorrectionEnabled(enabled);
    const Result liveRc = R_SUCCEEDED(setsysRc)
        ? setLiveAutomaticCorrection(enabled)
        : setsysRc;

    bool confirmed = !enabled;
    const Result confirmRc =
        setsysIsUserSystemClockAutomaticCorrectionEnabled(&confirmed);
    Result result = 0;
    if (R_FAILED(setsysRc)) {
        result = setsysRc;
    } else if (R_FAILED(liveRc)) {
        result = liveRc;
    } else if (R_FAILED(confirmRc)) {
        result = confirmRc;
    } else if (confirmed != enabled) {
        result = MAKERESULT(Module_Libnx, 908);
    }

    switchu::FileLog::log(
        "[settings-time] daemon internetTimeSync enabled=%d setsys=0x%X live=0x%X confirm=0x%X state=%d result=0x%X",
        enabled ? 1 : 0, setsysRc, liveRc, confirmRc,
        confirmed ? 1 : 0, result);
    return result;
}

static Result setClockByService(const char* srvName, u32 getClockCmd, u64 timestamp) {
    ScopedService srv;
    Result rc = smGetService(&srv.value, srvName);
    if (R_FAILED(rc)) {
        switchu::FileLog::log("[settings-time] smGetService(%s) failed: 0x%X", srvName, rc);
        return rc;
    }
    ScopedService clock;
    rc = serviceDispatch(&srv.value, getClockCmd,
        .out_num_objects = 1,
        .out_objects = &clock.value,
    );
    if (R_FAILED(rc)) {
        switchu::FileLog::log("[settings-time] %s cmd %u failed: 0x%X", srvName, getClockCmd, rc);
        return rc;
    }
    rc = serviceDispatchIn(&clock.value, 1, timestamp);
    switchu::FileLog::log("[settings-time] %s cmd %u SetCurrentTime(%llu) rc=0x%X",
        srvName, getClockCmd, (unsigned long long)timestamp, rc);
    return rc;
}

static Result applySystemTime(u64 targetTimestamp, bool isInternetSync) {
    // 1. Set NetworkSystemClock using time:s cmd 1 (GetStandardNetworkSystemClock)
    const Result netRc = setClockByService("time:s", 1, targetTimestamp);

    // 2. Set LocalSystemClock using time:a cmd 4 (GetStandardLocalSystemClock)
    const Result localRc = setClockByService("time:a", 4, targetTimestamp);

    // 3. Set UserSystemClock using time:a cmd 0 (GetStandardUserSystemClock)
    const Result userRc = setClockByService("time:a", 0, targetTimestamp);

    // 4. Update persistent automatic correction setting in setsys and live in time:a
    const Result setsysRc = setsysSetUserSystemClockAutomaticCorrectionEnabled(isInternetSync);
    const Result liveRc = setLiveAutomaticCorrection(isInternetSync);

    u64 actual = 0;
    Result readRc = timeGetCurrentTime(TimeType_UserSystemClock, &actual);

    switchu::FileLog::log(
        "[settings-time] applySystemTime posix=%llu internet=%d net=0x%X local=0x%X user=0x%X setsys=0x%X live=0x%X actual=%llu readRc=0x%X",
        (unsigned long long)targetTimestamp, isInternetSync ? 1 : 0,
        netRc, localRc, userRc, setsysRc, liveRc,
        (unsigned long long)actual, readRc);

    if (R_FAILED(localRc) && R_FAILED(userRc) && R_FAILED(netRc)) {
        return localRc != 0 ? localRc : (userRc != 0 ? userRc : netRc);
    }
    // The menu now reports this result to the player, so success has to mean
    // the clock the home screen shows reads the new time, not merely that one
    // of the three clocks accepted it. A couple of minutes of slack covers the
    // time spent getting here.
    constexpr u64 kClockSlackSeconds = 120;
    if (R_SUCCEEDED(readRc)) {
        const u64 drift = actual > targetTimestamp ? actual - targetTimestamp
                                                   : targetTimestamp - actual;
        if (drift > kClockSlackSeconds)
            return MAKERESULT(Module_Libnx, 909);
    }
    return 0;
}

static Result setManualDateTime(const smi::ManualDateTimeArgs& args) {
    TimeCalendarTime calendar{};
    calendar.year = static_cast<u16>(args.year);
    calendar.month = static_cast<u8>(args.month);
    calendar.day = static_cast<u8>(args.day);
    calendar.hour = static_cast<u8>(args.hour);
    calendar.minute = static_cast<u8>(args.minute);
    calendar.second = 0;

    u64 timestamps[2]{};
    s32 timestampCount = 0;
    Result rc = timeToPosixTimeWithMyRule(&calendar, timestamps, 2, &timestampCount);
    switchu::FileLog::log(
        "[settings-time] daemon convert %04u-%02u-%02u %02u:%02u rc=0x%X count=%d posix=%llu",
        args.year, args.month, args.day, args.hour, args.minute,
        rc, timestampCount,
        (unsigned long long)(timestampCount > 0 ? timestamps[0] : 0));
    if (R_SUCCEEDED(rc) && timestampCount <= 0)
        rc = MAKERESULT(Module_Libnx, 902);
    if (R_FAILED(rc))
        return rc;

    return applySystemTime(timestamps[0], /*isInternetSync=*/false);
}

static void pushNotification(smi::MenuMessage msg,
                             uint64_t app_id = 0,
                             uint32_t payload = 0) {
    if (!daemon::menu_la::isActive()) return;
    smi::DaemonNotification notif{};
    notif.magic   = smi::kNotifyMagic;
    notif.msg     = msg;
    notif.app_id  = app_id;
    notif.payload = payload;
    AppletStorage st;
    if (R_SUCCEEDED(appletCreateStorage(&st, sizeof(notif)))) {
        Result rc = appletStorageWrite(&st, 0, &notif, sizeof(notif));
        if (R_SUCCEEDED(rc)) {
            switchu::FileLog::log("[notify] push msg=%u", (unsigned)msg);
            daemon::menu_la::pushStorage(&st);
        } else {
            appletStorageClose(&st);
            switchu::FileLog::log("[notify] write FAIL: 0x%X msg=%u", rc, (unsigned)msg);
        }
    } else {
        switchu::FileLog::log("[notify] push FAIL (alloc) msg=%u", (unsigned)msg);
    }
}

static bool queryBatteryStatus(uint8_t& percent, PsmChargerType& chargerType) {
    if (!g_psmReady)
        return false;

    u32 charge = 100;
    Result chargeRc = psmGetBatteryChargePercentage(&charge);
    if (R_FAILED(chargeRc)) {
        switchu::FileLog::log("[battery] psmGetBatteryChargePercentage FAIL: 0x%X", chargeRc);
        return false;
    }

    PsmChargerType ct = PsmChargerType_Unconnected;
    Result chargerRc = psmGetChargerType(&ct);
    if (R_FAILED(chargerRc)) {
        switchu::FileLog::log("[battery] psmGetChargerType FAIL: 0x%X", chargerRc);
        ct = PsmChargerType_Unconnected;
    }

    if (charge > 100)
        charge = 100;
    percent = static_cast<uint8_t>(charge);
    chargerType = ct;
    return true;
}

static void pushBatteryStatusNotification(bool force) {
    if (!daemon::menu_la::isActive())
        return;

    uint8_t percent = 0;
    PsmChargerType chargerType = PsmChargerType_Unconnected;
    if (!queryBatteryStatus(percent, chargerType))
        return;

    if (!force && percent == g_lastBatteryPercent && chargerType == g_lastChargerType)
        return;

    g_lastBatteryPercent = percent;
    g_lastChargerType = chargerType;

    const uint32_t payload = smi::makeBatteryPayload(percent, static_cast<uint32_t>(chargerType));
    switchu::FileLog::log("[battery] notify percent=%u charger=%u",
                          (unsigned)percent,
                          (unsigned)chargerType);
    pushNotification(smi::MenuMessage::BatteryStatusChanged, 0, payload);
}

static void logHomeState(const char* source, const char* stage) {
    switchu::FileLog::log("[%s] HOME %s: appRunning=%d appFg=%d suspended=0x%016lX menuHolder=%d menuActive=%d fgApplet=%d",
                          source, stage,
                          daemon::app::isRunning() ? 1 : 0,
                          daemon::app::hasForeground() ? 1 : 0,
                          daemon::app::suspendedTitleId(),
                          daemon::menu_la::hasHolder() ? 1 : 0,
                          daemon::menu_la::isActive() ? 1 : 0,
                          g_foregroundAppletActive ? 1 : 0);
}

static bool takeForegroundFromRunningApp(const char* source) {
    if (!daemon::app::isRunning() || !daemon::app::hasForeground())
        return true;

    Result unlockRc = appletUnlockForeground();
    switchu::FileLog::log("[%s] UnlockForeground rc=0x%X", source, unlockRc);
    Result fgRc = appletRequestToGetForeground();
    switchu::FileLog::log("[%s] RequestToGetForeground rc=0x%X", source, fgRc);
    if (R_FAILED(fgRc))
        return false;

    daemon::app::onHomeSuspend();
    return true;
}

// Set once a power sequence has been handed to the system. The daemon keeps
// running until it is killed, so without this the main loop carries on writing
// — flushIfStale alone puts the log on the card every couple of seconds — while
// the console is shutting down underneath it.
static void stopControlCacheWorker();

// Reboot and shutdown must enter Horizon's orderly power sequence first.
// This daemon runs as the qlaunch replacement (AppletType_SystemApplet), so
// appletStart{Reboot,Shutdown}Sequence is the same path stock qlaunch uses: it
// coordinates service teardown and lets Atmosphere's bpc MITM choose the
// configured reboot target (payload, RCM, standard reboot, or PMIC on Mariko).
//
// spsmShutdown remains a fallback only. Calling it first bypasses the qlaunch
// sequence; on this console that consistently powers down into RCM before
// atmosphere/reboot_payload.bin is chainloaded, leaving an entirely black
// screen until a payload is injected externally.
static Result requestPowerStateChange(const char* source, bool reboot) {
    switchu::FileLog::log("[power] request source=%s action=%s",
                          source, reboot ? "reboot" : "shutdown");
    switchu::FileLog::flush();

    Result appletRc = reboot ? appletStartRebootSequence()
                             : appletStartShutdownSequence();
    if (R_SUCCEEDED(appletRc)) {
        switchu::FileLog::log("[power] applet sequence accepted action=%s",
                              reboot ? "reboot" : "shutdown");
        switchu::FileLog::flush();
        return appletRc;
    }

    Result spsmRc = spsmInitialize();
    if (R_SUCCEEDED(spsmRc)) {
        spsmRc = spsmShutdown(reboot);
        spsmExit();
    }
    switchu::FileLog::log(
        "[power] applet sequence failed rc=0x%X; spsm fallback rc=0x%X",
        appletRc, spsmRc);
    switchu::FileLog::flush();
    return spsmRc;
}

// Sleep is not a power-down and must not use the shutdown teardown below.
// The process keeps running, the filesystem stays mounted, and the daemon has
// to be alive on the other side to handle the wake. Routing sleep through
// startPowerSequence set g_powerSequenceStarted, which parks the main loop for
// good: the console woke to a daemon that had stopped reading applet messages
// and menu commands, so the wake notification never reached the menu and every
// power action in the menu did nothing from then on.
static void startSleepSequence(const char* source) {
    uint8_t batteryPercent = 0xFF;
    PsmChargerType chargerType = (PsmChargerType)0xFF;
    const bool batteryValid = queryBatteryStatus(batteryPercent, chargerType);
    switchu::FileLog::log(
        "[power] sleep requested source=%s battery_valid=%d percent=%u charger=%u "
        "appRunning=%d appFg=%d menuActive=%d",
        source, batteryValid ? 1 : 0, (unsigned)batteryPercent,
        (unsigned)chargerType, daemon::app::isRunning() ? 1 : 0,
        daemon::app::hasForeground() ? 1 : 0,
        daemon::menu_la::isActive() ? 1 : 0);
    if (daemon::menu_la::isActive())
        pushNotification(smi::MenuMessage::SleepSequence);

    // Preserve the final reason and state even if sleep ends in a forced power
    // loss. fflush must precede the device commit or the newest log data remains
    // only in the process buffer.
    switchu::FileLog::flush();
    switchu::commitSdCard("sleep");
    appletStartSleepSequence(true);
}

static void startPowerSequence(const char* source, smi::SystemMessage action) {
    if (action == smi::SystemMessage::EnterSleep) {
        // Defensive: no caller should reach the teardown with a sleep.
        startSleepSequence(source);
        return;
    }
    cancelViewPolling(source);
    takeForegroundFromRunningApp(source);
    g_powerSequenceStarted.store(true);

    // The corruption is intermittent — roughly one reboot in three or four —
    // which rules out anything deterministic and points at a race: the reboot
    // catching a write in flight. Every "this is fixed" in this investigation,
    // including ones confirmed over several reboots, was within the odds of
    // simply not losing that race.
    //
    // The control cache worker is a background thread that writes a .meta and
    // a .jpg per title, on its own schedule, with nothing coordinating it with
    // shutdown. Stop it and join before handing power off, so no write can be
    // half-finished when the console goes down. The menu quiesces its own
    // writer before it sends the request.
    //
    stopControlCacheWorker();

    // And then commit, which this deliberately did not do before.
    //
    // The reasoning against it was that an interrupted commit leaves worse FAT
    // state than none. That is true of a commit racing the power cut -- but
    // this one runs before spsm is asked for anything, synchronously, with the
    // cache worker already stopped and joined. Nothing can interrupt it,
    // because power has not been requested yet.
    //
    // Not committing is not the safe option: it is the option that guarantees
    // the metadata stays dirty. The menu commits its own writes before sending
    // the request, and that was not enough -- the daemon holds a separate fsdev
    // handle, and everything it wrote (its log, the control cache, the app
    // lists) is still outstanding at this point. A reboot from the menu came
    // back to hekate unable to find nyx with "card committed for power action"
    // sitting in the menu log, because the process that committed was not the
    // process with the dirty writes.
    switchu::FileLog::log("[power] sequence started source=%s action=%d", source, static_cast<int>(action));
    switchu::FileLog::flush();
    if (!switchu::commitSdCard("power sequence")) {
        switchu::FileLog::log("[power] SD commit failed; power action canceled");
        switchu::FileLog::flush();
        g_powerSequenceStarted.store(false);
        return;
    }

    Result rc = MAKERESULT(Module_Libnx, LibnxError_BadInput);
    switch (action) {
        case smi::SystemMessage::Shutdown:
            rc = requestPowerStateChange(source, false);
            break;
        case smi::SystemMessage::Reboot:
            rc = requestPowerStateChange(source, true);
            break;
        default:
            break;
    }

    if (R_FAILED(rc)) {
        switchu::FileLog::log("[power] action failed rc=0x%X; resuming daemon", rc);
        switchu::FileLog::flush();
        g_powerSequenceStarted.store(false);
        return;
    }

    // The accepted sequence completes asynchronously. Never return to normal
    // daemon work or relaunch the menu while Horizon is tearing the system down.
    while (true)
        svcSleepThread(100'000'000ULL);
}

static void openMenuFromHome(const char* source) {
    const uint64_t homeTick = armGetSystemTick();
    logHomeState(source, "request");
    cancelViewPolling("home");

    if (daemon::app::isRunning() && daemon::app::hasForeground()) {
        if (!takeForegroundFromRunningApp(source)) {
            switchu::FileLog::log("[%s] HOME aborted: foreground request failed", source);
            return;
        }
        const auto status = buildSystemStatus(smi::MenuTransitionReason::HomeRequest,
                                              homeTick);
        switchu::FileLog::log("[%s] HOME launching MainMenu status.running=%d suspended=0x%016lX",
                              source, status.app_running ? 1 : 0, status.suspended_app_id);
        Result menuRc = daemon::menu_la::launch(smi::MenuStartMode::MainMenu, status);
        switchu::FileLog::log("[%s] HOME MainMenu launch rc=0x%X", source, menuRc);
        if (R_SUCCEEDED(menuRc))
            g_appCatalogRefreshDelay = 200;
        logHomeState(source, "after");
        return;
    }

    if (daemon::menu_la::isActive()) {
        switchu::FileLog::log("[%s] HOME forwarding HomeRequest to active menu", source);
        pushNotification(smi::MenuMessage::HomeRequest);
    } else if (g_foregroundAppletActive) {
        switchu::FileLog::log("[%s] HOME requested while foreground applet active", source);
        g_pendingForegroundAppletHome = true;
    } else {
        switchu::FileLog::log("[%s] HOME no app/menu active; launching MainMenu", source);
        Result menuRc = daemon::menu_la::launch(
            smi::MenuStartMode::MainMenu,
            buildSystemStatus(smi::MenuTransitionReason::HomeRequest, homeTick));
        switchu::FileLog::log("[%s] HOME MainMenu launch rc=0x%X", source, menuRc);
        if (R_SUCCEEDED(menuRc))
            g_appCatalogRefreshDelay = 80;
    }
}

static bool sendViewFlagsUpdates() {
    std::vector<switchu::ns::ExtApplicationRecord> records;
    if (!listApplicationRecords(records, "views"))
        return false;

    std::vector<switchu::ns::ExtApplicationView> views;
    queryApplicationViews(records, views, "views");

    const s32 count = static_cast<s32>(records.size());

    if (count != g_lastRecordCount) {
        const s32 prevCount = g_lastRecordCount;
        switchu::FileLog::log("[views] title count changed %d -> %d, full reload needed",
                              g_lastRecordCount, count);

        for (s32 i = 0; i < count; ++i) {
            g_lastRecordTids[i] = records[i].id;
            g_lastViewFlags[i]  = views[i].flags;
        }
        for (s32 i = count; i < prevCount && i < kMaxTrackedApplicationRecords; ++i) {
            g_lastRecordTids[i] = 0;
            g_lastViewFlags[i]  = 0;
        }
        g_lastRecordCount = count;

        return true;
    }

    int pushed = 0;
    for (s32 i = 0; i < count; ++i) {
        uint32_t newFlags = views[i].flags;
        uint32_t oldFlags = 0;
        for (s32 j = 0; j < g_lastRecordCount; ++j) {
            if (g_lastRecordTids[j] == records[i].id) {
                oldFlags = g_lastViewFlags[j];
                break;
            }
        }
        if (newFlags != oldFlags) {
            pushNotification(smi::MenuMessage::AppViewFlagsUpdate,
                             records[i].id, newFlags);
            ++pushed;
        }
        g_lastRecordTids[i] = records[i].id;
        g_lastViewFlags[i]  = newFlags;
    }
    g_lastRecordCount = count;

    switchu::FileLog::log("[views] checked %d titles, pushed %d flag updates",
                          count, pushed);
    return false;
}

static void handleGeneralChannel() {
    AppletStorage st;
    if (R_FAILED(appletPopFromGeneralChannel(&st))) return;

    struct SamsHeader {
        u32 magic;
        u32 version;
        u32 msg;
        u32 reserved;
    } hdr = {};

    s64 sz = 0;
    appletStorageGetSize(&st, &sz);
    if (sz > 0)
        appletStorageRead(&st, 0, &hdr, (size_t)sz < sizeof(hdr) ? (size_t)sz : sizeof(hdr));
    appletStorageClose(&st);

    if (hdr.magic != 0x534D4153) return;

    switchu::FileLog::log("[sams] msg=%u", hdr.msg);
    switch (hdr.msg) {
        case 2:
        switchu::FileLog::log("[sams] -> Home");
        openMenuFromHome("sams");
        break;
        case 3:
        switchu::FileLog::log("[sams] -> Sleep");
        startSleepSequence("sams-sleep");
        break;
        case 5:
        switchu::FileLog::log("[sams] -> Shutdown");
        startPowerSequence("sams-shutdown", smi::SystemMessage::Shutdown);
        break;
        case 6:
        switchu::FileLog::log("[sams] -> Reboot");
        startPowerSequence("sams-reboot", smi::SystemMessage::Reboot);
        break;
    }
}

static void handleAppletMessages() {
    u32 msg = 0;
    Result rc = appletGetMessage(&msg);
    if (R_FAILED(rc))
        return;

    switchu::FileLog::log("[ae] msg=%u", msg);
    switch (msg) {
        case 1:
        switchu::FileLog::log("[ae] -> ChangeIntoForeground");
        break;

        case 2:
        // AppletMessage_ChangeIntoBackground: another foreground participant is
        // taking over. The menu no longer stays alive in a hidden suspended
        // state, so there is no extra holder to clean up here.
        switchu::FileLog::log("[ae] -> ChangeIntoBackground");
        break;

        case 20:
        openMenuFromHome("ae");
        break;

        case 30:
        case 31: {
            // OperationModeChanged / PerformanceModeChanged — dock or undock.
            // Nothing acted on these before, and an undock while the menu was
            // up has been observed to wedge the whole console. The framebuffer
            // is a fixed 1280x720 in both modes so there is no swapchain to
            // rebuild here; this records the mode and confirms whether the
            // daemon loop is still alive on the other side of the transition.
            const u8 opMode   = appletGetOperationMode();
            const u32 perfMode = appletGetPerformanceMode();
            switchu::FileLog::log("[ae] -> %s mode: operation=%u performance=%u menuActive=%d appRunning=%d",
                                  msg == 30 ? "OperationModeChanged" : "PerformanceModeChanged",
                                  (unsigned)opMode, (unsigned)perfMode,
                                  daemon::menu_la::isActive() ? 1 : 0,
                                  daemon::app::isRunning() ? 1 : 0);
            // Flush immediately: if the console wedges right after this, the
            // buffered tail would never reach the SD card.
            switchu::FileLog::flush();
            break;
        }

        case 22:
        case 27:
        case 28:
        case 29:
        case 32: {
            const char* source =
                msg == 22 ? "ae-power-button-short" :
                msg == 27 ? "ae-high-temperature" :
                msg == 28 ? "ae-low-battery" :
                msg == 29 ? "ae-auto-power-down" :
                            "ae-cec-standby";
            switchu::FileLog::log("[ae] -> Sleep source=%s msg=%u", source, msg);
            startSleepSequence(source);
            break;
        }

        case 26:
        switchu::FileLog::log("[ae] -> Wakeup");
        g_batteryRefreshPending.store(true);
        if (daemon::app::isRunning() && !daemon::menu_la::isActive()) {
            Result rc = daemon::app::resume();
            if (R_FAILED(rc)) {
                switchu::FileLog::log("[ae] wake resume FAIL: 0x%X", rc);
                appletRequestToGetForeground();
            }
        } else {
            appletRequestToGetForeground();
        }
        if (daemon::menu_la::isActive()) {
            pushNotification(smi::MenuMessage::WakeUp);
        } else if (!daemon::app::isRunning()) {
            daemon::menu_la::launch(
                smi::MenuStartMode::MainMenu,
                buildSystemStatus(smi::MenuTransitionReason::WakeRecovery,
                                  armGetSystemTick()));
        }
        break;
    }
}

static void pumpForegroundAppletMessages() {
    handleGeneralChannel();
    handleAppletMessages();
}

static bool consumeForegroundAppletHomeRequest() {
    if (!g_pendingForegroundAppletHome)
        return false;
    g_pendingForegroundAppletHome = false;
    return true;
}

static Result launchLibraryApplet(AppletId id, const char* name,
                                  const void* inData = nullptr, size_t inDataSize = 0,
                                  u32 libAppletVersion = 0) {
    switchu::FileLog::log("[applet] launching %s id=0x%X version=0x%X in=%zu",
                          name, (u32)id, libAppletVersion, inDataSize);
    Result fgRc = appletRequestToGetForeground();
    switchu::FileLog::log("[applet] %s RequestToGetForeground rc=0x%X", name, fgRc);

    g_foregroundAppletActive = true;
    g_pendingForegroundAppletHome = false;
    daemon::LibraryAppletInput input{inData, inDataSize};
    const daemon::LibraryAppletRequest request{
        .id = id,
        .name = name,
        .version = libAppletVersion,
        .pushCommonArgs = libAppletVersion != 0,
        .playStartupSound = true,
        .inputs = inData && inDataSize ? &input : nullptr,
        .inputCount = inData && inDataSize ? 1U : 0U,
    };
    const Result rc = daemon::runLibraryApplet(
        request, pumpForegroundAppletMessages,
        consumeForegroundAppletHomeRequest);
    g_foregroundAppletActive = false;
    return rc;
}

static u32 controllerAppletVersion() {
    if (hosversionAtLeast(11, 0, 0)) return 0x8;
    if (hosversionAtLeast(8, 0, 0)) return 0x7;
    if (hosversionAtLeast(6, 0, 0)) return 0x5;
    if (hosversionAtLeast(3, 0, 0)) return 0x4;
    return 0x3;
}

static Result setupControllerPrivateArg(HidLaControllerSupportArgPrivate& privateArg,
                                        HidLaControllerSupportMode mode,
                                        size_t publicArgSize,
                                        bool homeMenuStyle) {
    privateArg.private_size = sizeof(privateArg);
    privateArg.arg_size = publicArgSize;
    privateArg.flag0 = homeMenuStyle ? 1 : 0;
    privateArg.flag1 = 1;
    privateArg.mode = mode;
    if (hosversionAtLeast(3, 0, 0)) {
        // Both of these are answers to what this process itself configured, so
        // if setupNpad() was called they reflect that; if hid is unavailable or
        // neither has been set yet, fall back to the standard home menu set.
        u32 style = HidNpadStyleSet_NpadStandard | HidNpadStyleTag_NpadSystemExt;
        hidGetSupportedNpadStyleSet(&style);
        privateArg.npad_style_set = style;

        HidNpadJoyHoldType holdType = HidNpadJoyHoldType_Horizontal;
        hidGetNpadJoyHoldType(&holdType);
        privateArg.npad_joy_hold_type = holdType;
        return 0;
    } else {
        privateArg.npad_style_set = 0;
        privateArg.npad_joy_hold_type = HidNpadJoyHoldType_Horizontal;
    }
    return 0;
}

static Result runControllerApplet(const char* name,
                                  HidLaControllerSupportArgPrivate& privateArg,
                                  const void* publicArg,
                                  size_t publicArgSize) {
    HidLaControllerSupportResultInfoInternal output{};
    const daemon::LibraryAppletInput inputs[] = {
        {&privateArg, sizeof(privateArg)},
        {publicArg, publicArgSize},
    };
    daemon::LibraryAppletRequest request{
        .id = AppletId_LibraryAppletController,
        .name = name,
        .version = controllerAppletVersion(),
        .playStartupSound = true,
        .inputs = inputs,
        .inputCount = 2,
        .output = &output,
        .outputSize = sizeof(output),
    };

    appletRequestToGetForeground();
    g_foregroundAppletActive = true;
    g_pendingForegroundAppletHome = false;
    Result rc = daemon::runLibraryApplet(
        request, pumpForegroundAppletMessages,
        consumeForegroundAppletHomeRequest);
    g_foregroundAppletActive = false;
    switchu::FileLog::log(
        "[applet] %s output res=0x%X players=%d selected=%u runner=0x%X",
        name, output.res, output.info.player_count, output.info.selected_id, rc);
    if (R_SUCCEEDED(rc) && output.res == 1) {
        switchu::FileLog::log("[applet] %s completed outcome=cancelled", name);
    } else if (R_SUCCEEDED(rc) && output.res != 0) {
         rc = MAKERESULT(Module_Libnx, LibnxError_LibAppletBadExit);
    }
    return rc;
}

static Result launchControllerPairing() {
    switchu::FileLog::log("[applet] launching Controller pairing");
    HidLaControllerSupportArg arg;
    hidLaCreateControllerSupportArg(&arg);
    arg.hdr.player_count_max = 8;
    arg.hdr.enable_single_mode = false;
    // Change Grip/Order exists to redo the player order, so it has to start
    // from nothing: taking the current connections over leaves player 1 pinned
    // to whichever controller opened the applet, and it can never be replaced.
    arg.hdr.enable_take_over_connection = 0;

    HidLaControllerSupportArgV3 legacyArg{};
    const void* publicArg = &arg;
    size_t publicArgSize = sizeof(arg);
    if (hosversionBefore(8, 0, 0)) {
        legacyArg.hdr = arg.hdr;
        std::memcpy(legacyArg.identification_color, arg.identification_color,
                    sizeof(legacyArg.identification_color));
        legacyArg.enable_explain_text = arg.enable_explain_text;
        std::memcpy(legacyArg.explain_text, arg.explain_text,
                    sizeof(legacyArg.explain_text));
        legacyArg.hdr.player_count_min = std::min<s8>(legacyArg.hdr.player_count_min, 4);
        legacyArg.hdr.player_count_max = std::min<s8>(legacyArg.hdr.player_count_max, 4);
        publicArg = &legacyArg;
        publicArgSize = sizeof(legacyArg);
    }

    HidLaControllerSupportArgPrivate privateArg{};
    Result rc = setupControllerPrivateArg(
        privateArg, HidLaControllerSupportMode_ShowControllerSupport,
        publicArgSize, true);
    if (R_SUCCEEDED(rc))
        rc = runControllerApplet("Controllers", privateArg, publicArg, publicArgSize);
    if (R_FAILED(rc))
        switchu::FileLog::log("[applet] Controller FAIL: 0x%X", rc);
    else
        switchu::FileLog::log("[applet] Controller pairing done");
    return rc;
}

static Result launchControllerRemapping() {
    if (hosversionBefore(11, 0, 0))
        return MAKERESULT(Module_Libnx, LibnxError_IncompatSysVer);
    switchu::FileLog::log("[applet] launching controller remapping");
    HidLaControllerKeyRemappingArg arg{};
    hidLaCreateControllerKeyRemappingArg(&arg);
    const Result rc = hidLaShowControllerKeyRemappingForSystem(
        &arg, HidLaControllerSupportCaller_System);
    switchu::FileLog::log("[applet] controller remapping rc=0x%X", rc);
    return rc;
}

static void logMenuReadyTrace(const smi::MenuReadyArgs& ready, uint64_t receiveTick) {
    const auto& launch = daemon::menu_la::launchTrace();
    daemon::menu_la::markMenuReady(receiveTick);
    switchu::FileLog::log(
        "[trace-return-ready] reason=%u origin_to_holder_us=%llu prepare_us=%llu registration_us=%llu create_us=%llu holder_start_us=%llu holder_to_main_us=%llu ready_ipc_us=%llu origin_to_ready_us=%llu cores=%u/%u catalog=%u",
        static_cast<unsigned>(launch.reason),
        (unsigned long long)tickDeltaUs(launch.originTick, launch.holderStartEndTick),
        (unsigned long long)tickDeltaUs(launch.prepareStartTick, launch.holderStartCallTick),
        (unsigned long long)tickDeltaUs(launch.registrationStartTick, launch.registrationEndTick),
        (unsigned long long)tickDeltaUs(launch.createStartTick, launch.createEndTick),
        (unsigned long long)tickDeltaUs(launch.holderStartCallTick, launch.holderStartEndTick),
        (unsigned long long)tickDeltaUs(launch.holderStartEndTick, ready.menu_main_tick),
        (unsigned long long)tickDeltaUs(ready.command_send_tick, receiveTick),
        (unsigned long long)tickDeltaUs(launch.originTick, receiveTick),
        ready.menu_main_core,
        ready.activity_core,
        ready.catalog_count);
    switchu::FileLog::log(
        "[trace-return-init] reason=%u main_to_init_us=%llu gpu_us=%llu renderer_us=%llu blank_us=%llu oncreate_us=%llu",
        static_cast<unsigned>(launch.reason),
        (unsigned long long)tickDeltaUs(ready.menu_main_tick, ready.initialize_start_tick),
        (unsigned long long)tickDeltaUs(ready.initialize_start_tick, ready.gpu_ready_tick),
        (unsigned long long)tickDeltaUs(ready.gpu_ready_tick, ready.renderer_ready_tick),
        (unsigned long long)tickDeltaUs(ready.renderer_ready_tick, ready.blank_frame_tick),
        (unsigned long long)tickDeltaUs(ready.activity_create_start_tick, ready.activity_create_end_tick));
}

static void logMenuFirstFrameTrace(const smi::MenuFirstFrameArgs& first,
                                   uint64_t receiveTick) {
    const auto& launch = daemon::menu_la::launchTrace();
    switchu::FileLog::log(
        "[trace-return-frame] reason=%u origin_to_frame_us=%llu ready_to_frame_us=%llu input_to_frame_us=%llu frame_ipc_us=%llu image_kib=%llu core=%u catalog=%u",
        static_cast<unsigned>(launch.reason),
        (unsigned long long)tickDeltaUs(launch.originTick, first.first_frame_tick),
        (unsigned long long)tickDeltaUs(launch.menuReadyReceiveTick, first.first_frame_tick),
        (unsigned long long)tickDeltaUs(first.first_input_tick, first.first_frame_tick),
        (unsigned long long)tickDeltaUs(first.first_frame_tick, receiveTick),
        (unsigned long long)(first.image_memory_bytes / 1024ULL),
        first.core,
        first.catalog_count);
}

static void logMenuExitTrace(uint64_t holderFinishedTick) {
    const auto& closing = g_lastMenuClosingTrace;
    switchu::FileLog::log(
        "[trace-menu-exit] shutdown_to_drain_us=%llu gpu_drain_us=%llu destroy_to_cancel_us=%llu cancel_to_workers_us=%llu workers_to_state_us=%llu state_to_http_us=%llu http_to_bt_us=%llu bt_to_closecmd_us=%llu closecmd_ipc_us=%llu closecmd_to_holder_us=%llu core=%u",
        (unsigned long long)tickDeltaUs(closing.shutdown_start_tick, closing.gpu_drain_end_tick),
        (unsigned long long)tickDeltaUs(closing.gpu_drain_start_tick, closing.gpu_drain_end_tick),
        (unsigned long long)tickDeltaUs(closing.on_destroy_start_tick, closing.http_cancel_done_tick),
        (unsigned long long)tickDeltaUs(closing.http_cancel_done_tick, closing.worker_drain_done_tick),
        (unsigned long long)tickDeltaUs(closing.worker_drain_done_tick, closing.state_persist_done_tick),
        (unsigned long long)tickDeltaUs(closing.state_persist_done_tick, closing.http_shutdown_done_tick),
        (unsigned long long)tickDeltaUs(closing.http_shutdown_done_tick, closing.bluetooth_done_tick),
        (unsigned long long)tickDeltaUs(closing.bluetooth_done_tick, closing.command_send_tick),
        (unsigned long long)tickDeltaUs(closing.command_send_tick, g_lastMenuClosingReceiveTick),
        (unsigned long long)tickDeltaUs(closing.command_send_tick, holderFinishedTick),
        closing.core);
}

static void logApplicationLaunchTrace(const Action& action,
                                      const daemon::app::LaunchTiming& timing,
                                      uint64_t holderFinishedTick) {
    const auto& trace = action.transition;
    const auto& preflight = timing.preflight;
    switchu::FileLog::log(
        "[trace-preflight] title=0x%016lX activation_to_send_us=%llu ipc_us=%llu work_us=%llu touch_us=%llu saves_us=%llu lead_us=%llu tail_us=%llu end_to_action_us=%llu attempted=%d complete=%d cache_hit=%d reject=%u core=%u",
        action.title_id,
        (unsigned long long)tickDeltaUs(trace.activation_tick, trace.preflight_send_tick),
        (unsigned long long)tickDeltaUs(preflight.requestSendTick, preflight.commandReceiveTick),
        (unsigned long long)tickDeltaUs(preflight.workStartTick, preflight.workEndTick),
        (unsigned long long)tickDeltaUs(preflight.touchStartTick, preflight.touchEndTick),
        (unsigned long long)tickDeltaUs(preflight.saveStartTick, preflight.saveEndTick),
        (unsigned long long)tickDeltaUs(preflight.workEndTick, trace.command_send_tick),
        (unsigned long long)tickDeltaUs(trace.command_send_tick, preflight.workEndTick),
        (unsigned long long)tickDeltaUs(preflight.workEndTick, timing.actionStartTick),
        preflight.attempted ? 1 : 0,
        preflight.complete ? 1 : 0,
        preflight.cacheHit ? 1 : 0,
        static_cast<unsigned>(preflight.rejectReason),
        preflight.core);
    switchu::FileLog::log(
        "[trace-launch] title=0x%016lX activation_to_user_us=%llu user_to_animation_us=%llu recency_total_us=%llu animation_to_commit_us=%llu activation_to_command_us=%llu command_ipc_us=%llu command_to_holder_us=%llu holder_to_action_us=%llu command_to_foreground_us=%llu activation_to_foreground_us=%llu core=%u",
        action.title_id,
        (unsigned long long)tickDeltaUs(trace.activation_tick, trace.user_selected_tick),
        (unsigned long long)tickDeltaUs(trace.user_selected_tick, trace.animation_complete_tick),
        (unsigned long long)tickDeltaUs(trace.recency_submit_tick, trace.recency_commit_complete_tick),
        (unsigned long long)tickDeltaUs(trace.animation_complete_tick, trace.recency_commit_complete_tick),
        (unsigned long long)tickDeltaUs(trace.activation_tick, trace.command_send_tick),
        (unsigned long long)tickDeltaUs(trace.command_send_tick, action.commandReceivedTick),
        (unsigned long long)tickDeltaUs(trace.command_send_tick, holderFinishedTick),
        (unsigned long long)tickDeltaUs(holderFinishedTick, timing.actionStartTick),
        (unsigned long long)tickDeltaUs(trace.command_send_tick, timing.foregroundEndTick),
        (unsigned long long)tickDeltaUs(trace.activation_tick, timing.foregroundEndTick),
        timing.core);
    switchu::FileLog::log(
        "[trace-launch-phases] title=0x%016lX previous_close_us=%llu touch_us=%llu saves_us=%llu create_us=%llu launch_parameter_us=%llu start_us=%llu foreground_us=%llu",
        action.title_id,
        (unsigned long long)tickDeltaUs(timing.previousExitRequestTick, timing.previousJoinedTick),
        (unsigned long long)tickDeltaUs(timing.touchStartTick, timing.touchEndTick),
        (unsigned long long)tickDeltaUs(timing.saveStartTick, timing.saveEndTick),
        (unsigned long long)tickDeltaUs(timing.createStartTick, timing.createEndTick),
        (unsigned long long)tickDeltaUs(timing.createEndTick, timing.startStartTick),
        (unsigned long long)tickDeltaUs(timing.startStartTick, timing.startEndTick),
        (unsigned long long)tickDeltaUs(timing.foregroundStartTick, timing.foregroundEndTick));
    switchu::FileLog::log(
        "[trace-save] title=0x%016lX account_us=%llu device_us=%llu temporary_us=%llu cache_us=%llu bcat_us=%llu total_us=%llu",
        action.title_id,
        (unsigned long long)armTicksToNs(timing.accountSaveTicks) / 1'000ULL,
        (unsigned long long)armTicksToNs(timing.deviceSaveTicks) / 1'000ULL,
        (unsigned long long)armTicksToNs(timing.temporarySaveTicks) / 1'000ULL,
        (unsigned long long)armTicksToNs(timing.cacheSaveTicks) / 1'000ULL,
        (unsigned long long)armTicksToNs(timing.bcatSaveTicks) / 1'000ULL,
        (unsigned long long)tickDeltaUs(timing.saveStartTick, timing.saveEndTick));
    switchu::FileLog::log(
        "[trace-save-state] title=0x%016lX account=%s account_open_rc=0x%X account_create_rc=0x%X device=%s device_open_rc=0x%X device_create_rc=0x%X temporary=%s temporary_open_rc=0x%X temporary_create_rc=0x%X cache=%s cache_open_rc=0x%X cache_create_rc=0x%X bcat=%s bcat_open_rc=0x%X bcat_create_rc=0x%X",
        action.title_id,
        daemon::app::saveEnsureOutcomeName(timing.accountSaveEnsure.outcome),
        timing.accountSaveEnsure.openRc, timing.accountSaveEnsure.createRc,
        daemon::app::saveEnsureOutcomeName(timing.deviceSaveEnsure.outcome),
        timing.deviceSaveEnsure.openRc, timing.deviceSaveEnsure.createRc,
        daemon::app::saveEnsureOutcomeName(timing.temporarySaveEnsure.outcome),
        timing.temporarySaveEnsure.openRc, timing.temporarySaveEnsure.createRc,
        daemon::app::saveEnsureOutcomeName(timing.cacheSaveEnsure.outcome),
        timing.cacheSaveEnsure.openRc, timing.cacheSaveEnsure.createRc,
        daemon::app::saveEnsureOutcomeName(timing.bcatSaveEnsure.outcome),
        timing.bcatSaveEnsure.openRc, timing.bcatSaveEnsure.createRc);
    logMenuExitTrace(holderFinishedTick);
}

static void logApplicationResumeTrace(const Action& action,
                                      const daemon::app::ResumeTiming& timing,
                                      uint64_t holderFinishedTick) {
    const auto& trace = action.transition;
    switchu::FileLog::log(
        "[trace-resume] title=0x%016lX activation_to_animation_us=%llu activation_to_command_us=%llu command_ipc_us=%llu command_to_holder_us=%llu holder_to_action_us=%llu unlock_us=%llu unlock_rc=0x%X foreground_us=%llu activation_to_foreground_us=%llu core=%u",
        action.title_id,
        (unsigned long long)tickDeltaUs(trace.activation_tick, trace.animation_complete_tick),
        (unsigned long long)tickDeltaUs(trace.activation_tick, trace.command_send_tick),
        (unsigned long long)tickDeltaUs(trace.command_send_tick, action.commandReceivedTick),
        (unsigned long long)tickDeltaUs(trace.command_send_tick, holderFinishedTick),
        (unsigned long long)tickDeltaUs(holderFinishedTick, timing.actionStartTick),
        (unsigned long long)tickDeltaUs(timing.unlockStartTick, timing.unlockEndTick),
        timing.unlockResult,
        (unsigned long long)tickDeltaUs(timing.foregroundStartTick, timing.foregroundEndTick),
        (unsigned long long)tickDeltaUs(trace.activation_tick, timing.foregroundEndTick),
        timing.core);
    logMenuExitTrace(holderFinishedTick);
}

static void handleMenuCommand() {
    if (!daemon::menu_la::isActive()) return;

    AppletStorage st;
    if (R_FAILED(daemon::menu_la::popStorage(&st))) return;

    smi::StorageReader reader(st);
    if (!reader.valid()) return;

    auto msg = reader.systemMessage();
    const uint64_t commandReceiveTick = armGetSystemTick();
    switchu::FileLog::log("[smi] command=%u", (u32)msg);

    switch (msg) {
    case smi::SystemMessage::PrepareApplication: {
        const auto args = reader.pop<smi::PrepareAppArgs>();
        AccountUid uid{};
        std::memcpy(&uid, args.user_uid, sizeof(uid));
        switchu::daemon::mem::snapshot("prepare-app");
        daemon::app::prepare(args.title_id, uid, args.request_send_tick,
                             commandReceiveTick);
        break;
    }

    case smi::SystemMessage::LaunchApplication:
#ifdef SWITCHU_PREFLIGHT_EDGE_TEST
    case smi::SystemMessage::DiagnosticLaunchFailure:
#endif
    {
        auto args = reader.pop<smi::LaunchAppArgs>();
        Action action{};
        action.type = ActionType::LaunchApplication;
        action.title_id = args.title_id;
        std::memcpy(&action.uid, args.user_uid, sizeof(action.uid));
        action.transition = args.trace;
        action.commandReceivedTick = commandReceiveTick;
#ifdef SWITCHU_PREFLIGHT_EDGE_TEST
        action.injectLaunchFailure =
            msg == smi::SystemMessage::DiagnosticLaunchFailure;
#endif
        g_lastMenuClosingTrace = {};
        g_lastMenuClosingReceiveTick = 0;
        g_actionQueue.push_back(action);
#ifdef SWITCHU_PREFLIGHT_EDGE_TEST
        if (action.injectLaunchFailure) {
            switchu::FileLog::log(
                "[diagnostic-preflight-edge] queued synthetic create failure title=0x%016lX (actions=%zu)",
                args.title_id, g_actionQueue.size());
            break;
        }
#endif
        switchu::FileLog::log("[smi] queued launch 0x%016lX (actions=%zu)", args.title_id, g_actionQueue.size());
        break;
    }

    case smi::SystemMessage::ResumeApplication:
#ifdef SWITCHU_RESUME_FAILURE_TEST
    case smi::SystemMessage::DiagnosticResumeFailure:
#endif
    {
        auto args = reader.pop<smi::ResumeAppArgs>();
        Action action{};
        action.type = ActionType::ResumeApplication;
        action.title_id = daemon::app::suspendedTitleId();
        action.transition = args.trace;
        action.commandReceivedTick = commandReceiveTick;
#ifdef SWITCHU_RESUME_FAILURE_TEST
        action.injectResumeForegroundFailure =
            msg == smi::SystemMessage::DiagnosticResumeFailure;
#endif
        g_lastMenuClosingTrace = {};
        g_lastMenuClosingReceiveTick = 0;
        g_actionQueue.push_back(action);
#ifdef SWITCHU_RESUME_FAILURE_TEST
        if (action.injectResumeForegroundFailure) {
            switchu::FileLog::log(
                "[diagnostic-resume] queued synthetic foreground failure title=0x%016lX (actions=%zu)",
                action.title_id, g_actionQueue.size());
            break;
        }
#endif
        switchu::FileLog::log("[smi] queued resume (actions=%zu)", g_actionQueue.size());
        break;
    }

    case smi::SystemMessage::TerminateApplication: {
        const bool hadRunningApplication = daemon::app::isRunning();
        const Result result = daemon::app::beginTerminate();
        if (R_FAILED(result)) {
            switchu::FileLog::log("[smi] begin terminate FAIL: 0x%X", result);
        } else if (!hadRunningApplication) {
            pushNotification(smi::MenuMessage::ApplicationExited);
        }
        break;
    }
#ifdef SWITCHU_TERMINATION_QUEUE_TEST
    case smi::SystemMessage::DiagnosticTerminateHold: {
        switchu::FileLog::log("[diagnostic] command terminate sleep-wake");
        const Result result = daemon::app::beginTerminate(
            daemon::app::TerminateDiagnosticMode::HoldForSleepWake);
        if (R_FAILED(result))
            switchu::FileLog::log("[diagnostic] begin sleep-wake terminate FAIL: 0x%X", result);
        break;
    }

    case smi::SystemMessage::DiagnosticTerminateForce: {
        switchu::FileLog::log("[diagnostic] command terminate force-at-deadline");
        const Result result = daemon::app::beginTerminate(
            daemon::app::TerminateDiagnosticMode::ForceAtDeadline);
        if (R_FAILED(result))
            switchu::FileLog::log("[diagnostic] begin forced terminate FAIL: 0x%X", result);
        break;
    }
#endif

    case smi::SystemMessage::LaunchAlbum:
        {
            Action action{};
            action.type = ActionType::OpenAlbum;
            g_actionQueue.push_back(action);
        }
        switchu::FileLog::log("[smi] queued album launch (actions=%zu)", g_actionQueue.size());
        break;

    case smi::SystemMessage::LaunchMiiEditor:
        {
            Action action{};
            action.type = ActionType::OpenMiiEditor;
            g_actionQueue.push_back(action);
        }
        switchu::FileLog::log("[smi] queued Mii Editor launch (actions=%zu)", g_actionQueue.size());
        break;

    case smi::SystemMessage::LaunchUserCreator:
        {
            Action action{};
            action.type = ActionType::OpenUserCreator;
            g_actionQueue.push_back(action);
        }
        switchu::FileLog::log("[smi] queued user creator launch (actions=%zu)", g_actionQueue.size());
        break;

    case smi::SystemMessage::RotateLogs:
        // open() closes the current file, renames it to daemon-<timestamp>.log
        // and starts a fresh one. The renamed copy is held by nobody, so it can
        // be read over MTP with the console still running.
        switchu::FileLog::log("[smi] rotating log on request");
        switchu::FileLog::flush();
        switchu::FileLog::open("daemon");
        fsdevCommitDevice("sdmc");
        switchu::FileLog::log("[smi] log rotated");
        break;

    case smi::SystemMessage::LaunchNetConnect:
        {
            Action action{};
            action.type = ActionType::OpenNetConnect;
            g_actionQueue.push_back(action);
        }
        switchu::FileLog::log("[smi] queued NetConnect launch (actions=%zu)", g_actionQueue.size());
        break;

    case smi::SystemMessage::LaunchUserPage: {
        auto args = reader.pop<smi::UserArgs>();
        Action action{};
        action.type = ActionType::OpenUserPage;
        std::memcpy(&action.uid, args.user_uid, sizeof(action.uid));
        g_actionQueue.push_back(action);
        switchu::FileLog::log("[smi] queued User Page launch (actions=%zu)", g_actionQueue.size());
        break;
    }

    case smi::SystemMessage::LaunchControllers:
        {
            Action action{};
            action.type = ActionType::OpenControllers;
            g_actionQueue.push_back(action);
        }
        switchu::FileLog::log("[smi] queued Controller launch (actions=%zu)", g_actionQueue.size());
        break;

    case smi::SystemMessage::LaunchControllerRemapping:
        {
            Action action{};
            action.type = ActionType::OpenControllerRemapping;
            g_actionQueue.push_back(action);
        }
        switchu::FileLog::log("[smi] queued controller remapping (actions=%zu)",
                              g_actionQueue.size());
        break;

    case smi::SystemMessage::EnterSleep:
        startSleepSequence("smi-sleep");
        break;

    case smi::SystemMessage::Shutdown:
        startPowerSequence("smi-shutdown", smi::SystemMessage::Shutdown);
        break;

    case smi::SystemMessage::Reboot:
        startPowerSequence("smi-reboot", smi::SystemMessage::Reboot);
        break;

    case smi::SystemMessage::RequestSelfUninstall:
        if (reader.remaining() != 0) {
            switchu::FileLog::log("[uninstall] rejected request with unexpected payload=%zu",
                                  reader.remaining());
            break;
        }
        // The menu wrote and committed the marker before sending this command.
        // Never alter the override while this qlaunch replacement is executing;
        // reboot so the boot-time apply path can do it before a menu is opened.
        switchu::FileLog::log("[uninstall] staged request accepted; rebooting");
        startPowerSequence("smi-self-uninstall", smi::SystemMessage::Reboot);
        break;

    case smi::SystemMessage::RequestForeground:
        appletRequestToGetForeground();
        break;

    case smi::SystemMessage::SetManualDateTime: {
        const auto args = reader.pop<smi::ManualDateTimeArgs>();
        const Result rc = setManualDateTime(args);
        switchu::FileLog::log("[settings-time] manual date/time rc=0x%X", rc);
        pushNotification(smi::MenuMessage::TimeSettingApplied,
                         static_cast<uint64_t>(smi::TimeSettingKind::ManualDateTime),
                         static_cast<uint32_t>(rc));
        break;
    }

    case smi::SystemMessage::SetInternetTimeSync: {
        const auto args = reader.pop<smi::InternetTimeSyncArgs>();
        const Result rc = setInternetTimeSync(args.enabled != 0);
        switchu::FileLog::log(
            "[settings-time] Internet synchronization enabled=%d rc=0x%X",
            args.enabled ? 1 : 0, rc);
        pushNotification(smi::MenuMessage::TimeSettingApplied,
                         static_cast<uint64_t>(smi::TimeSettingKind::InternetSync),
                         static_cast<uint32_t>(rc));
        break;
    }

    case smi::SystemMessage::SetPosixTime: {
        const auto args = reader.pop<smi::SetPosixTimeArgs>();
        const Result rc = applySystemTime(args.timestamp, args.is_internet_sync != 0);
        switchu::FileLog::log(
            "[settings-time] SetPosixTime posix=%llu internet=%d rc=0x%X",
            (unsigned long long)args.timestamp, args.is_internet_sync ? 1 : 0, rc);
        pushNotification(smi::MenuMessage::TimeSettingApplied,
                         static_cast<uint64_t>(smi::TimeSettingKind::NetworkTime),
                         static_cast<uint32_t>(rc));
        break;
    }

    case smi::SystemMessage::GetAppList: {
        break;
    }

    case smi::SystemMessage::RefreshCatalog: {
        // Only what the catalogue is missing. Reading control data costs about
        // 0.9 s per title -- measured over 547 reads on a 117-title console,
        // median 899 ms -- so clearing the cache first made this take about six
        // minutes, during which the grid sits on loading spinners. Picking up a
        // newly installed title, which is what this is normally used for, needs
        // none of that work. RebuildControlCache below is the one that forgets.
        switchu::FileLog::log("[control-cache] refresh on request (%d titles tracked)",
                              (int)g_lastRecordCount);
        bool catalogChanged = false;
        rebuildAppCatalog("refresh-request", &catalogChanged);
        if (daemon::menu_la::isActive())
            pushNotification(smi::MenuMessage::AppRecordsChanged);
        break;
    }

    case smi::SystemMessage::RebuildControlCache: {
        // The cache is wrong rather than incomplete: a shortcut that reused an
        // id, or a title whose name could not be read when it was first seen.
        // The player is the one who can see that, so they ask for it.
        for (s32 i = 0; i < g_lastRecordCount && i < kMaxTrackedApplicationRecords; ++i) {
            if (g_lastRecordTids[i] != 0)
                switchu::control_cache::forget(g_lastRecordTids[i]);
        }
        switchu::FileLog::log("[control-cache] cleared on request (%d titles)",
                              (int)g_lastRecordCount);
        bool rebuiltChanged = false;
        rebuildAppCatalog("rebuild-request", &rebuiltChanged);
        if (daemon::menu_la::isActive())
            pushNotification(smi::MenuMessage::AppRecordsChanged);
        break;
    }

    case smi::SystemMessage::GetSystemStatus: {
        auto status = buildSystemStatus();
        smi::StorageWriter writer((Result)0);
        writer.push(status);
        AppletStorage respSt;
        Result respRc = writer.createStorage(respSt);
        if (R_SUCCEEDED(respRc))
            daemon::menu_la::pushStorage(&respSt);
        else
            switchu::FileLog::log("[smi] GetSystemStatus response create FAIL: 0x%X", respRc);
        return;
    }

    case smi::SystemMessage::MenuReady: {
        const auto args = reader.pop<smi::MenuReadyArgs>();
        switchu::FileLog::log("[smi] menu ready");
        logMenuReadyTrace(args, commandReceiveTick);
        if (g_menuFastExitCount != 0 || g_menuRelaunchCooldown != 0) {
            switchu::FileLog::log(
                "[main] healthy menu reset fast-exit guard count=%d cooldown=%d",
                g_menuFastExitCount, g_menuRelaunchCooldown);
        }
        // A menu that completed initialization is not part of a startup crash
        // loop. Normal title handoffs must not accumulate forever and delay a
        // later launch-failure recovery by the five-second crash-loop guard.
        g_menuFastExitCount = 0;
        g_menuRelaunchCooldown = 0;
        g_batteryRefreshPending.store(true);
        break;
    }

    case smi::SystemMessage::MenuClosing:
        g_lastMenuClosingTrace = reader.pop<smi::MenuClosingArgs>();
        g_lastMenuClosingReceiveTick = commandReceiveTick;
        switchu::FileLog::log("[smi] menu closing");
        break;

    case smi::SystemMessage::MenuFirstFrame: {
        const auto args = reader.pop<smi::MenuFirstFrameArgs>();
        logMenuFirstFrameTrace(args, commandReceiveTick);
        break;
    }

    }

    // Commands are one-way. Their sender observes only whether its outgoing
    // storage was accepted; launch completion and state changes arrive through
    // MenuMessage notifications. Do not allocate and enqueue an unread reply.
}

static void recoverMenuAfterApplicationHandoffFailure(
    const char* operation, uint64_t titleId, Result failureRc) {
    const uint64_t recoveryTick = armGetSystemTick();
    switchu::FileLog::log(
        "[recovery] %s failed title=0x%016lX rc=0x%X; relaunching menu",
        operation, titleId, failureRc);
    const Result menuRc = daemon::menu_la::launch(
        smi::MenuStartMode::MainMenu,
        buildSystemStatus(smi::MenuTransitionReason::LaunchFailure,
                          recoveryTick));
    switchu::FileLog::log(
        "[recovery] %s menu relaunch rc=0x%X running=%d suspended=0x%016lX",
        operation, menuRc,
        daemon::app::isRunning() ? 1 : 0,
        daemon::app::suspendedTitleId());
}

static bool handleAction(Action& action) {
    if (daemon::menu_la::hasHolder() || g_foregroundAppletActive ||
        daemon::app::isTerminating())
        return false;

    switchu::FileLog::log("[action] handling type=%u", (u32)action.type);
    switch (action.type) {
        case ActionType::LaunchApplication: {
            const uint64_t holderFinishedTick = daemon::menu_la::lastFinishedTick();
            daemon::app::LaunchTiming timing{};
            Result rc = daemon::app::launch(action.title_id, action.uid, &timing
#ifdef SWITCHU_PREFLIGHT_EDGE_TEST
                                            , action.injectLaunchFailure
#endif
            );
            logApplicationLaunchTrace(action, timing, holderFinishedTick);
            if (R_FAILED(rc)) {
                switchu::FileLog::log("[action] launch 0x%016lX FAIL: 0x%X", action.title_id, rc);
                recoverMenuAfterApplicationHandoffFailure("launch", action.title_id, rc);
            }
            return true;
        }

        case ActionType::ResumeApplication: {
            const uint64_t holderFinishedTick = daemon::menu_la::lastFinishedTick();
            daemon::app::ResumeTiming timing{};
            Result rc = daemon::app::resume(&timing
#ifdef SWITCHU_RESUME_FAILURE_TEST
                                            , action.injectResumeForegroundFailure
#endif
            );
            logApplicationResumeTrace(action, timing, holderFinishedTick);
            if (R_FAILED(rc)) {
                switchu::FileLog::log("[action] resume FAIL: 0x%X", rc);
                recoverMenuAfterApplicationHandoffFailure(
                    "resume", action.title_id, rc);
            }
            return true;
        }

        case ActionType::OpenAlbum: {
            const u8 albumArg = AlbumLaArg_ShowAllAlbumFilesForHomeMenu;
            Result rc = launchLibraryApplet(AppletId_LibraryAppletPhotoViewer,
                                            "Album",
                                            &albumArg,
                                            sizeof(albumArg),
                                            0x10000);
            if (R_FAILED(rc))
                switchu::FileLog::log("[action] album FAIL: 0x%X", rc);
            switchu::FileLog::log("[action] relaunching menu after album");
            daemon::menu_la::launch(
                smi::MenuStartMode::MainMenu,
                buildSystemStatus(smi::MenuTransitionReason::LibraryAppletReturn,
                                  armGetSystemTick()));
            return true;
        }

        case ActionType::OpenMiiEditor: {
            const auto miiVer = hosversionAtLeast(10, 2, 0) ? 0x4 : 0x3;
            const MiiLaAppletInput in = {
                .version = miiVer,
                .mode = MiiLaAppletMode_ShowMiiEdit,
                .special_key_code = MiiSpecialKeyCode_Normal,
            };
            Result rc = launchLibraryApplet(AppletId_LibraryAppletMiiEdit,
                                            "MiiEditor", &in, sizeof(in));
            if (R_FAILED(rc))
                switchu::FileLog::log("[action] Mii Editor FAIL: 0x%X", rc);
            switchu::FileLog::log("[action] relaunching menu after Mii Editor");
            daemon::menu_la::launch(
                smi::MenuStartMode::MainMenu,
                buildSystemStatus(smi::MenuTransitionReason::LibraryAppletReturn,
                                  armGetSystemTick()));
            return true;
        }

        case ActionType::OpenControllers: {
            Result rc = launchControllerPairing();
            if (R_FAILED(rc))
                switchu::FileLog::log("[action] Controllers FAIL: 0x%X", rc);
            switchu::FileLog::log("[action] relaunching menu after Controllers");
            daemon::menu_la::launch(
                smi::MenuStartMode::MainMenu,
                buildSystemStatus(smi::MenuTransitionReason::LibraryAppletReturn,
                                  armGetSystemTick()));
            return true;
        }

        case ActionType::OpenControllerRemapping: {
            const Result rc = launchControllerRemapping();
            if (R_FAILED(rc))
                switchu::FileLog::log(
                    "[action] controller remapping FAIL: 0x%X", rc);
            daemon::menu_la::launch(
                smi::MenuStartMode::MainMenu,
                buildSystemStatus(smi::MenuTransitionReason::LibraryAppletReturn,
                                  armGetSystemTick()));
            return true;
        }

        case ActionType::OpenNetConnect: {
            const u32 netType = 1;
            Result rc = launchLibraryApplet(AppletId_LibraryAppletNetConnect,
                                            "NetConnect", &netType,
                                            sizeof(netType), 1);
            if (R_FAILED(rc))
                switchu::FileLog::log("[action] NetConnect FAIL: 0x%X", rc);
            switchu::FileLog::log("[action] relaunching menu after NetConnect");
            daemon::menu_la::launch(
                smi::MenuStartMode::MainMenu,
                buildSystemStatus(smi::MenuTransitionReason::LibraryAppletReturn,
                                  armGetSystemTick()));
            return true;
        }

        case ActionType::OpenUserCreator: {
            // libnx wraps the whole libapplet handshake for this one, so there
            // is no input struct to build as there is for the Mii editor.
            Result rc = pselShowUserCreator();
            if (R_FAILED(rc))
                switchu::FileLog::log("[action] user creator FAIL: 0x%X", rc);
            switchu::FileLog::log("[action] relaunching menu after user creator");
            daemon::menu_la::launch(
                smi::MenuStartMode::MainMenu,
                buildSystemStatus(smi::MenuTransitionReason::LibraryAppletReturn,
                                  armGetSystemTick()));
            return true;
        }

        case ActionType::OpenUserPage: {
            Result rc = friendsLaShowMyProfileForHomeMenu(action.uid);
            if (R_FAILED(rc))
                switchu::FileLog::log("[action] User Page FAIL: 0x%X", rc);
            switchu::FileLog::log("[action] relaunching menu after User Page");
            daemon::menu_la::launch(
                smi::MenuStartMode::MainMenu,
                buildSystemStatus(smi::MenuTransitionReason::LibraryAppletReturn,
                                  armGetSystemTick()));
            return true;
        }
    }

    return false;
}

static bool consumeOneAction() {
    if (g_actionQueue.empty())
        return false;

    for (size_t i = 0; i < g_actionQueue.size(); ++i) {
        if (handleAction(g_actionQueue[i])) {
            g_actionQueue.erase(g_actionQueue.begin() + i);
            return true;
        }
    }

    return false;
}

static bool mainLoopNeedsFastTick() {
    if (g_powerSequenceStarted.load())
        return false;
    return g_eventPollsRemaining > 0
        || g_appCatalogRefreshPending.load()
        || g_controlCacheRefreshPending.load()
        || g_menuRelaunchCooldown > 0
        || !g_actionQueue.empty();
}

static void waitForMainWork() {
    Waiter waiters[3]{};
    s32 waiterCount = 0;

    if (Event* messageEvent = appletGetMessageEvent())
        waiters[waiterCount++] = waiterForEvent(messageEvent);
    if (g_generalChannelEventReady)
        waiters[waiterCount++] = waiterForEvent(&g_generalChannelEvent);

    waiters[waiterCount++] = waiterForUEvent(&g_mainWakeEvent);

    if (waiterCount == 0) {
        svcSleepThread(1'000'000'000ULL);
        return;
    }

    s32 signalledIndex = -1;
    const u64 timeout = mainLoopNeedsFastTick()
        ? 10'000'000ULL
        : 1'000'000'000ULL;
    static Result s_lastWaitFailure = 0;
    static uint32_t s_waitFailureRepeatCount = 0;
    const Result rc = waitObjects(&signalledIndex, waiters, waiterCount, timeout);
    if (R_FAILED(rc) && rc != KERNELRESULT(TimedOut)) {
        if (rc == s_lastWaitFailure) {
            ++s_waitFailureRepeatCount;
        } else {
            s_lastWaitFailure = rc;
            s_waitFailureRepeatCount = 1;
        }
        if (s_waitFailureRepeatCount <= 3 || (s_waitFailureRepeatCount % 120) == 0) {
            switchu::FileLog::log("[main] waitObjects FAIL: 0x%X count=%u waiters=%d",
                                  rc,
                                  (unsigned)s_waitFailureRepeatCount,
                                  (int)waiterCount);
        }
        // Applet holder or applet-manager events can be rejected by waitObjects
        // while foreground ownership is changing. The next main-loop tick polls
        // all holders and channels again, so keep a conservative backoff instead
        // of turning an invalid waiter into a hot loop.
        svcSleepThread(100'000'000ULL);
    } else {
        s_lastWaitFailure = 0;
        s_waitFailureRepeatCount = 0;
    }
    return;
}

static void routeFinishedApplication(const char* source) {
    switchu::FileLog::log("[main] app exited source=%s", source);
    if (daemon::menu_la::isActive()) {
        pushNotification(smi::MenuMessage::ApplicationExited);
    } else if (!g_actionQueue.empty()) {
        // A close-then-launch handoff already has an authoritative destination.
        // Starting a cold menu here would put a new holder in front of that
        // action and waste an entire construction/destruction cycle.
        switchu::FileLog::log("[main] menu relaunch skipped: queued actions=%zu",
                              g_actionQueue.size());
    } else {
        daemon::menu_la::launch(
            smi::MenuStartMode::MainMenu,
            buildSystemStatus(smi::MenuTransitionReason::ApplicationFinished,
                              daemon::app::lastFinishedTick()));
    }
}

static void mainLoop() {
    handleGeneralChannel();
    handleAppletMessages();
    handleMenuCommand();

    bool didWork = false;

    if (g_eventRefreshPending.load() && shouldDeferViewPolling()) {
        g_eventPollCountdown = kViewPollIntervalTicks;
        g_eventPollsRemaining = 1;
    } else if (g_eventRefreshPending.exchange(false)) {
        if (!g_initialEventSkipped) {
            g_initialEventSkipped = true;
            g_appCatalogRefreshPending.store(false);
            switchu::FileLog::log("[views] skipping initial catch-up event");
        } else {
            switchu::FileLog::log("[views] app record event — starting poll");
            g_eventPollCountdown  = kViewPollIntervalTicks;
            g_eventPollsRemaining = kViewPollAttempts;
        }
    }

    if (g_appCatalogRefreshPending.load() && !shouldDeferViewPolling() &&
        g_appCatalogRefreshDelay > 0) {
        --g_appCatalogRefreshDelay;
    } else if (g_appCatalogRefreshPending.load() && !shouldDeferViewPolling()) {
        g_appCatalogRefreshPending.store(false);
        bool catalogChanged = false;
        if (rebuildAppCatalog("record-event", &catalogChanged)) {
            if (catalogChanged) {
                if (daemon::menu_la::isActive())
                    pushNotification(smi::MenuMessage::AppRecordsChanged);
                else
                    g_catalogChangedWhileAway.store(true);
            }
            didWork = true;
        }
    }
    if (g_controlCacheRefreshPending.load() && g_controlCacheRefreshDelay.load() > 0) {
        --g_controlCacheRefreshDelay;
    } else if (g_controlCacheRefreshPending.exchange(false)) {
        // No rebuild here. Rewriting the catalog once per cached title turned
        // one write at boot into dozens, each with its own directory churn.
        // The menu falls back to reading the .meta directly for any title the
        // catalog still names in hex, so this costs a file open per unnamed
        // title on the next load and nothing after the catalog is rebuilt for
        // another reason.
        if (daemon::menu_la::isActive())
            pushNotification(smi::MenuMessage::AppRecordsChanged);
        else
            g_catalogChangedWhileAway.store(true);
        didWork = true;
    }

    // Deliver what was missed. The menu cannot ask for this itself: from its
    // side a stale grid and a current one look identical, which is why it sat
    // there showing games that had been deleted.
    if (g_catalogChangedWhileAway.load() && daemon::menu_la::isActive()) {
        g_catalogChangedWhileAway.store(false);
        switchu::FileLog::log("[event] catalog changed while the menu was away, telling it now");
        pushNotification(smi::MenuMessage::AppRecordsChanged);
        didWork = true;
    }
    if (g_eventPollsRemaining > 0 && shouldDeferViewPolling()) {
        g_eventPollCountdown = kViewPollIntervalTicks;
    } else if (g_eventPollsRemaining > 0 && --g_eventPollCountdown == 0) {
        bool needFullReload = sendViewFlagsUpdates();
        if (needFullReload) {
            if (daemon::menu_la::isActive())
                pushNotification(smi::MenuMessage::AppRecordsChanged);
            g_eventPollsRemaining = 0;
        } else {
            --g_eventPollsRemaining;
            if (g_eventPollsRemaining > 0)
                g_eventPollCountdown = kViewPollIntervalTicks;
        }
    }
    if (g_eventGcMountFailure.exchange(false)) {
        if (daemon::menu_la::isActive()) {
            pushNotification(smi::MenuMessage::GameCardMountFailure, 0,
                             (uint32_t)g_eventGcMountRc.load());
        }
    }
    if (g_batteryRefreshPending.exchange(false)) {
        pushBatteryStatusNotification(true);
        g_batteryPollCountdown = 50;
    } else if (daemon::menu_la::isActive()) {
        if (g_batteryPollCountdown > 0)
            --g_batteryPollCountdown;
        if (g_batteryPollCountdown <= 0) {
            pushBatteryStatusNotification(false);
            g_batteryPollCountdown = 50;
        }
    }

    if (g_menuRelaunchCooldown > 0)
        --g_menuRelaunchCooldown;

    if (daemon::menu_la::checkFinished()) {
        switchu::FileLog::log("[main] menu exited (reason=%d)",
            (int)daemon::menu_la::exitReason());
        ++g_menuFastExitCount;
        if (g_menuFastExitCount >= 3) {
            g_menuRelaunchCooldown = 500;
            switchu::FileLog::log("[main] menu fast-exit guard active count=%d cooldown=%d",
                                  g_menuFastExitCount, g_menuRelaunchCooldown);
        }
        didWork = true;
    }

    if (daemon::app::pollTerminate()) {
        routeFinishedApplication("terminate");
        didWork = true;
    }

    didWork |= consumeOneAction();

    if (daemon::app::checkFinished()) {
        routeFinishedApplication("natural");
        didWork = true;
    }

    if (!didWork && g_menuRelaunchCooldown <= 0 && g_actionQueue.empty() &&
        !daemon::app::isRunning() && !daemon::menu_la::hasHolder() &&
        !g_foregroundAppletActive) {
        switchu::FileLog::log("[main] no app/menu active; relaunching menu");
        daemon::menu_la::launch(
            smi::MenuStartMode::MainMenu,
            buildSystemStatus(smi::MenuTransitionReason::IdleRecovery,
                              armGetSystemTick()));
    }
}


static Thread g_eventThread = {};
static std::atomic<bool> g_eventRunning{false};

static void eventManagerThreadFunc(void* arg) {
    (void)arg;
    switchu::FileLog::log("[event] thread alive");

    Event recordEvent = {};
    Result rc = nsGetApplicationRecordUpdateSystemEvent(&recordEvent);
    if (R_FAILED(rc)) {
        switchu::FileLog::log("[event] nsGetApplicationRecordUpdateSystemEvent FAIL: 0x%X", rc);
        return;
    }
    switchu::FileLog::log("[event] registered ApplicationRecordUpdateSystemEvent");

    Event gcMountFailEvent = {};
    bool hasGcEvent = false;
    if (hosversionAtLeast(3, 0, 0)) {
        rc = nsGetGameCardMountFailureEvent(&gcMountFailEvent);
        if (R_SUCCEEDED(rc)) {
            hasGcEvent = true;
            switchu::FileLog::log("[event] registered GameCardMountFailureEvent");
        } else {
            switchu::FileLog::log("[event] nsGetGameCardMountFailureEvent FAIL: 0x%X", rc);
        }
    } else {
        switchu::FileLog::log("[event] GameCardMountFailureEvent not supported on this firmware");
    }

    PsmSession psmSession{};
    bool hasPsmEvent = false;
    if (g_psmReady) {
        rc = psmBindStateChangeEvent(&psmSession, true, true, true);
        if (R_SUCCEEDED(rc)) {
            hasPsmEvent = true;
            switchu::FileLog::log("[event] registered PSM state change event");
        } else {
            switchu::FileLog::log("[event] psmBindStateChangeEvent FAIL: 0x%X", rc);
        }
    } else {
        switchu::FileLog::log("[event] PSM unavailable; battery events disabled");
    }

    while (g_eventRunning.load()) {
        s32 evIdx = -1;
        Result waitRc;
        if (hasGcEvent && hasPsmEvent) {
            waitRc = waitMulti(&evIdx, 1'000'000'000ULL,
                waiterForEvent(&recordEvent),
                waiterForEvent(&gcMountFailEvent),
                waiterForEvent(&psmSession.StateChangeEvent));
        } else if (hasGcEvent) {
            waitRc = waitMulti(&evIdx, 1'000'000'000ULL,
                waiterForEvent(&recordEvent),
                waiterForEvent(&gcMountFailEvent));
        } else if (hasPsmEvent) {
            waitRc = waitMulti(&evIdx, 1'000'000'000ULL,
                waiterForEvent(&recordEvent),
                waiterForEvent(&psmSession.StateChangeEvent));
        } else {
            waitRc = waitMulti(&evIdx, 1'000'000'000ULL,
                waiterForEvent(&recordEvent));
        }

        if (waitRc == KERNELRESULT(TimedOut)) continue;
        if (R_FAILED(waitRc)) continue;

        if (evIdx == 0) {
            eventClear(&recordEvent);
            switchu::FileLog::log("[event] ApplicationRecordUpdateSystemEvent fired");

            g_appCatalogRefreshPending.store(true);
            g_eventRefreshPending.store(true);
            ueventSignal(&g_mainWakeEvent);
        } else if (evIdx == 1 && hasGcEvent) {
            eventClear(&gcMountFailEvent);

            Result failRc = switchu::ns::getLastGameCardMountFailure();
            switchu::FileLog::log("[event] GameCardMountFailure rc=0x%X", failRc);

            g_eventGcMountRc.store(failRc);
            g_eventGcMountFailure.store(true);
        } else if ((hasGcEvent && hasPsmEvent && evIdx == 2) ||
                   (!hasGcEvent && hasPsmEvent && evIdx == 1)) {
            eventClear(&psmSession.StateChangeEvent);
            switchu::FileLog::log("[event] PSM state change fired");
            g_batteryRefreshPending.store(true);
        }

        svcSleepThread(100'000ULL);
    }

    eventClose(&recordEvent);
    if (hasGcEvent) eventClose(&gcMountFailEvent);
    if (hasPsmEvent) psmUnbindStateChangeEvent(&psmSession);
    switchu::FileLog::log("[event] thread exiting");
}

static Result startEventManager() {
    g_eventRunning.store(true);
    Result rc = threadCreate(&g_eventThread, eventManagerThreadFunc, nullptr,
                             nullptr, 0x4000, 0x2C, 3);
    if (R_FAILED(rc)) {
        switchu::FileLog::log("[event] threadCreate FAIL: 0x%X", rc);
        return rc;
    }
    rc = threadStart(&g_eventThread);
    if (R_FAILED(rc)) {
        switchu::FileLog::log("[event] threadStart FAIL: 0x%X", rc);
        threadClose(&g_eventThread);
        return rc;
    }
    switchu::FileLog::log("[event] thread started");
    return 0;
}

static void stopEventManager() {
    g_eventRunning.store(false);
    threadWaitForExit(&g_eventThread);
    threadClose(&g_eventThread);
    switchu::FileLog::log("[event] thread stopped");
}

static Thread g_controlCacheThread = {};
static std::atomic<bool> g_controlCacheRunning{false};
static bool g_controlCacheStarted = false;

static void controlCacheThreadFunc(void* arg) {
    (void)arg;
    switchu::FileLog::log("[control-cache] thread alive");
    switchu::control_cache::ensureDirectory();

    while (g_controlCacheRunning.load()) {
        // Nothing while a game holds the foreground.
        //
        // The catalogue rebuild already stands aside for this, through
        // shouldDeferViewPolling, and this worker did not -- an asymmetry, not
        // a decision. It keeps chewing whatever was queued before the game
        // started, and each title costs an nsGetApplicationControlData, which
        // reads the icon and NACP from storage, plus a .meta and a .jpg written
        // to the card. That is ns IPC and SD writes underneath a running game,
        // while ams_mitm is serving that game's LayeredFS from the same card.
        //
        // Whether it is what produced the aborts and the kernel panic is not
        // established. Doing this work behind a game is worth stopping either
        // way: none of it is needed until the player is back in the menu.
        if (shouldDeferViewPolling()) {
            svcSleepThread(500'000'000ULL);
            continue;
        }

        uint64_t titleId = 0;
        if (!popControlCacheTitle(titleId)) {
            svcSleepThread(100'000'000ULL);
            continue;
        }

        if (titleId == 0 || switchu::control_cache::hasMeta(titleId))
            continue;

        auto* controlData = new NsApplicationControlData();
        if (!controlData) {
            switchu::FileLog::log("[control-cache] alloc FAIL title=0x%016lX", titleId);
            svcSleepThread(250'000'000ULL);
            continue;
        }

        // Storage is what official software asks for, and it answers from the
        // system's own control cache when that has an entry. A title downgraded
        // to an older build was reported stuck with no name at all, which is
        // what a stale or empty entry there looks like -- so the other two
        // sources are tried before giving up on the name. StorageOnly ignores
        // that cache and reads the installed content; CacheOnly is the last
        // resort, and can still hold the name the title had before.
        static constexpr NsApplicationControlSource kSources[] = {
            NsApplicationControlSource_Storage,
            NsApplicationControlSource_StorageOnly,
            NsApplicationControlSource_CacheOnly,
        };

        bool named = false;
        bool cached = false;
        for (const NsApplicationControlSource source : kSources) {
            size_t controlSize = 0;
            const uint64_t startTick = armGetSystemTick();
            const Result rc = nsGetApplicationControlData(source,
                                                          titleId,
                                                          controlData,
                                                          sizeof(*controlData),
                                                          &controlSize);
            const uint64_t elapsedMs =
                armTicksToNs(armGetSystemTick() - startTick) / 1'000'000ULL;
            if (R_FAILED(rc) || controlSize < sizeof(NacpStruct)) {
                switchu::FileLog::log(
                    "[control-cache] GetControlData FAIL title=0x%016lX source=%d rc=0x%X size=%zu elapsed=%lums",
                    titleId,
                    static_cast<int>(source),
                    rc,
                    controlSize,
                    static_cast<unsigned long>(elapsedMs));
                continue;
            }

            const auto outcome = switchu::control_cache::writeFromControlData(
                titleId,
                *controlData,
                controlSize);
            named = outcome == switchu::control_cache::CacheOutcome::Named;
            cached = cached || named ||
                     outcome == switchu::control_cache::CacheOutcome::Unnamed;
            switchu::FileLog::log(
                "[control-cache] cached 0x%016lX source=%d size=%zu elapsed=%lums named=%d written=%d",
                titleId,
                static_cast<int>(source),
                controlSize,
                static_cast<unsigned long>(elapsedMs),
                named ? 1 : 0,
                cached ? 1 : 0);
            if (named)
                break;
        }

        if (cached) {
            // An unnamed entry is kept as it is: the grid falls back to the id
            // for the label, and the title is not asked for again every time the
            // catalogue is rebuilt. Reload games and shortcuts forgets it and
            // starts this over, which is the way back once the content that
            // carries the name is installed again.
            //
            // Each notification makes the menu rebuild its entire grid, so while
            // there is still a queue the next one is held back: a full rebuild
            // was telling the menu about once a second for minutes, and the grid
            // spent that time being torn down and built again.
            g_controlCacheRefreshPending.store(true);
            g_controlCacheRefreshDelay.store(controlCacheQueueSize() > 0 ? 800 : 60);
        }
        if (!named) {
            switchu::FileLog::log("[control-cache] no name for 0x%016lX from any source",
                                  titleId);
        }
        switchu::FileLog::flush();

        delete controlData;
        svcSleepThread(10'000'000ULL);
    }

    switchu::FileLog::log("[control-cache] thread exiting");
}

static Result startControlCacheWorker() {
    g_controlCacheRunning.store(true);
    Result rc = threadCreate(&g_controlCacheThread, controlCacheThreadFunc, nullptr,
                             nullptr, 0x10000, 0x2D, 3);
    if (R_FAILED(rc)) {
        switchu::FileLog::log("[control-cache] threadCreate FAIL: 0x%X", rc);
        return rc;
    }

    rc = threadStart(&g_controlCacheThread);
    if (R_FAILED(rc)) {
        switchu::FileLog::log("[control-cache] threadStart FAIL: 0x%X", rc);
        threadClose(&g_controlCacheThread);
        return rc;
    }

    switchu::FileLog::log("[control-cache] thread started");
    g_controlCacheStarted = true;
    return 0;
}

static void stopControlCacheWorker() {
    if (!g_controlCacheStarted)
        return;
    g_controlCacheRunning.store(false);
    threadWaitForExit(&g_controlCacheThread);
    threadClose(&g_controlCacheThread);
    g_controlCacheStarted = false;
}

// The system clock is not necessarily set when a sysmodule starts at boot, and
// libnx only samples it once. One failed sample would date the whole session
// from 1970, so the sample is retried from the main loop until it looks like a
// real date -- after which this costs one comparison per iteration.
static bool g_wallClockReady = false;
static void ensureWallClock() {
    if (g_wallClockReady || !g_timeReady)
        return;
    __libnx_init_time();
    const std::time_t now = std::time(nullptr);
    std::tm calendar{};
    if (::localtime_r(&now, &calendar) && calendar.tm_year + 1900 >= 2020) {
        g_wallClockReady = true;
        switchu::FileLog::log("[daemon] wall clock available");
    }
}

int main(int argc, char* argv[]) {
    const uint64_t daemonMainTick = armGetSystemTick();
    switchu::FileLog::log("[daemon] main() entry");

    ueventCreate(&g_mainWakeEvent, true);
    ueventCreate(&g_controlCacheWakeEvent, true);
    Result generalEventRc = appletGetPopFromGeneralChannelEvent(&g_generalChannelEvent);
    g_generalChannelEventReady = R_SUCCEEDED(generalEventRc);
    if (R_FAILED(generalEventRc))
        switchu::FileLog::log("[daemon] general channel event unavailable: 0x%X", generalEventRc);

    appletLoadAndApplyIdlePolicySettings();

    // Apply removal before the catalogue workers or an external menu start. A
    // request that cannot be applied blocks staged updates so they cannot
    // restore its qlaunch override behind the player's back.
    const auto uninstallResult = switchu::daemon::self_uninstall::applyStagedRequest();
    if (uninstallResult == switchu::daemon::self_uninstall::StagedRequestResult::Applied) {
        switchu::FileLog::flush();
        if (R_SUCCEEDED(requestPowerStateChange("self-uninstall applied", true))) {
            while (true)
                svcSleepThread(100'000'000ULL);
        }
        return 0;
    }
    const bool uninstallPending =
        uninstallResult == switchu::daemon::self_uninstall::StagedRequestResult::Pending;
    if (uninstallPending)
        switchu::FileLog::log("[uninstall] pending request blocks update apply");

    // Before the menu exists, so nothing it would replace is open. Never apply
    // an archive while an uninstall marker remains unresolved.
    //
    // And then reboot, rather than carry on into the menu. The payload holds
    // both halves of the launcher, and only one of them can be picked up here:
    // the menu is launched from the card after this runs, so it is the new one,
    // while this daemon was loaded by Atmosphère before any of it happened and
    // stays the old one until the console comes up again. That ran a menu
    // against a daemon one release behind for a whole session -- every
    // daemon-side fix in an update looked as though it had not shipped, and a
    // command the two disagreed about had nothing to catch it. The staging is
    // already cleared and committed by now, so this boots straight through.
    if (!uninstallPending && switchu::daemon::update::applyStagedUpdate()) {
        switchu::daemon::mem::snapshot("update");
        switchu::FileLog::log("[update] applied; rebooting so the new daemon runs");
        switchu::FileLog::flush();
        if (R_SUCCEEDED(requestPowerStateChange("update applied", true))) {
            while (true)
                svcSleepThread(100'000'000ULL);
        }
        return 0;
    }

    rebuildAppCatalog("boot");

    Result rc = startControlCacheWorker();
    if (R_FAILED(rc))
        switchu::FileLog::log("[daemon] control cache worker failed: 0x%X (non-fatal)", rc);

    rc = startEventManager();
    if (R_FAILED(rc))
        switchu::FileLog::log("[daemon] event manager failed: 0x%X (non-fatal)", rc);

    switchu::FileLog::log("[daemon] launching menu...");
    rc = daemon::menu_la::launch(
        smi::MenuStartMode::StartupBoot,
        buildSystemStatus(smi::MenuTransitionReason::StartupBoot, daemonMainTick));
    if (R_FAILED(rc))
        switchu::FileLog::log("[daemon] menu launch failed: 0x%X", rc);

    while (g_running.load()) {
        if (g_powerSequenceStarted.load()) {
            // Idle until the system kills us. No filesystem access from here
            // on: the card is the thing being corrupted, and every write
            // issued during a shutdown is a chance to be interrupted midway.
            svcSleepThread(50'000'000ULL);
            continue;
        }
        ensureWallClock();
        mainLoop();
        // The daemon never closes its log, so drain the write buffer on a timer.
        switchu::FileLog::flushIfStale();
        svcSleepThread(10'000'000ULL);
    }

    stopEventManager();
    stopControlCacheWorker();
    daemon::menu_la::terminate();
    daemon::app::cleanup();
    switchu::FileLog::log("[daemon] shutdown complete");
    return 0;
}
