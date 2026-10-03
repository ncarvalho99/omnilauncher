#pragma once
#include <cstdint>
#include <cstring>

namespace switchu::smi {

static constexpr uint32_t kCommandMagic      = 0x53575543;
static constexpr uint32_t kStorageSize       = 0x8000;
static constexpr uint32_t kMaxRetries        = 5000;
static constexpr uint64_t kRetrySleepNs      = 10'000'000;

enum class MenuMessage : uint32_t {
    Invalid               =  0,
    HomeRequest           =  1,
    ApplicationExited     =  2,
    ApplicationSuspended  =  3,
    SdCardEjected         =  4,
    AppRecordsChanged     =  5,
    SleepSequence         =  6,
    WakeUp                =  7,
    GameCardMountFailure  =  8,
    AppViewFlagsUpdate    =  9,
    BatteryStatusChanged  = 10,
    // The answer to a clock command. Those commands go out as storage with no
    // reply, so the menu used to say "Date and time updated." as soon as the
    // command was queued, before the daemon had run it or knew whether it had
    // worked. app_id carries a TimeSettingKind, payload the Result.
    TimeSettingApplied    = 11,
};

enum class TimeSettingKind : uint64_t {
    ManualDateTime = 1,
    InternetSync   = 2,
    NetworkTime    = 3,
};

enum class SystemMessage : uint32_t {
    Invalid               =  0,

    LaunchApplication     =  1,
    ResumeApplication     =  2,
    TerminateApplication  =  3,
    PrepareApplication    =  4,

    LaunchAlbum           = 10,
    LaunchMiiEditor       = 11,
    LaunchControllers     = 12,
    LaunchNetConnect      = 13,
    LaunchUserPage        = 14,
    // The system's own account creation applet. Creating a user was the one
    // account operation the menu had no route to at all.
    LaunchUserCreator     = 15,
    LaunchControllerRemapping = 16,

    EnterSleep            = 20,
    Shutdown              = 21,
    Reboot                = 22,
    RequestForeground     = 23,
    // The menu has durably staged a request to disable its qlaunch override.
    // The daemon reboots now and applies that request before launching a menu.
    RequestSelfUninstall  = 24,

    GetAppList            = 30,
    GetSystemStatus       = 31,
    IsApplicationValid    = 32,
    SetManualDateTime     = 33,
    SetInternetTimeSync   = 34,
    SetPosixTime          = 35,

    MenuReady             = 40,
    MenuClosing           = 41,
    // Rebuild the catalogue now, forgetting every cached name and icon first.
    //
    // The daemon already refreshes when the record list changes, but a shortcut
    // whose id was reused looks unchanged to that check, and a player who has
    // just made one has no way to say "look again" short of rebooting. Asked
    // for after exactly that.
    RefreshCatalog        = 42,
    // Sent after the first real menu frame has been submitted. MenuReady
    // measures construction; this measures the first usable visual frame.
    MenuFirstFrame        = 43,
    // Closes the daemon's log and starts a new one, so the finished file can be
    // copied off the card. Nothing can read it while the daemon holds it open:
    // copying it over MTP with the console running fails with 2002-0007,
    // "resource already in use", which is how diagnostics kept getting stuck.
    RotateLogs            = 44,
    // Forgets every cached name and icon and reads them from the titles again.
    // Expensive on purpose -- about a second per installed title -- so it is a
    // separate, explicit action from RefreshCatalog, which only picks up what
    // the catalogue is missing.
    RebuildControlCache   = 45,
#ifdef SWITCHU_TERMINATION_QUEUE_TEST
    // Diagnostic-only commands. Production builds neither expose nor handle
    // these IDs, so lifecycle fault injection cannot alter normal timing.
    DiagnosticTerminateHold  = 44,
    DiagnosticTerminateForce = 45,
#endif
#ifdef SWITCHU_PREFLIGHT_EDGE_TEST
    // Carries a real selected title through the ordinary launch queue, then
    // injects a deterministic failure immediately before application creation.
    // No guessed or uninstalled title ID ever reaches Horizon.
    DiagnosticLaunchFailure  = 46,
#endif
#ifdef SWITCHU_RESUME_FAILURE_TEST
    // Preserves the real suspended application but substitutes a deterministic
    // result at the foreground-request boundary.
    DiagnosticResumeFailure  = 47,
#endif
};

enum class MenuStartMode : uint32_t {
    MainMenu       = 0,
    StartupBoot    = 1,
};

struct CommandHeader {
    uint32_t magic;
    uint32_t message;
};
static_assert(sizeof(CommandHeader) == 8);

// Raw ARM system-counter ticks are shared across the daemon and menu during a
// boot. Carrying them through the existing command storage gives millisecond
// transition measurements without synchronous SD logging on the hot path.
struct LaunchTransitionTrace {
    uint64_t activation_tick;
    uint64_t user_selected_tick;
    uint64_t preflight_send_tick;
    uint64_t recency_submit_tick;
    uint64_t animation_complete_tick;
    uint64_t recency_commit_complete_tick;
    uint64_t command_send_tick;
};
static_assert(sizeof(LaunchTransitionTrace) == 56);

struct PrepareAppArgs {
    uint64_t title_id;
    uint8_t  user_uid[16];
    uint64_t request_send_tick;
};
static_assert(sizeof(PrepareAppArgs) == 32);

struct LaunchAppArgs {
    uint64_t title_id;
    uint8_t  user_uid[16];
    LaunchTransitionTrace trace;
};
static_assert(sizeof(LaunchAppArgs) == 80);

struct ResumeAppArgs {
    LaunchTransitionTrace trace;
};
static_assert(sizeof(ResumeAppArgs) == 56);

struct UserArgs {
    uint8_t user_uid[16];
};
static_assert(sizeof(UserArgs) == 16);

struct ManualDateTimeArgs {
    uint32_t year;
    uint32_t month;
    uint32_t day;
    uint32_t hour;
    uint32_t minute;
};
static_assert(sizeof(ManualDateTimeArgs) == 20);

struct InternetTimeSyncArgs {
    uint8_t enabled;
    uint8_t _pad[7];
};
static_assert(sizeof(InternetTimeSyncArgs) == 8);

struct SetPosixTimeArgs {
    uint64_t timestamp;
    uint8_t  is_internet_sync;
    uint8_t  _pad[7];
};
static_assert(sizeof(SetPosixTimeArgs) == 16);

enum class MenuTransitionReason : uint32_t {
    Unknown             = 0,
    StartupBoot         = 1,
    HomeRequest         = 2,
    ApplicationFinished = 3,
    LibraryAppletReturn = 4,
    WakeRecovery        = 5,
    IdleRecovery        = 6,
    LaunchFailure       = 7,
};

struct SystemStatus {
    uint64_t  suspended_app_id;
    uint8_t   selected_user[16];
    bool      app_running;
    uint8_t   _pad[7];
    uint64_t  transition_origin_tick;
    MenuTransitionReason transition_reason;
    uint32_t  _trace_pad;
};
static_assert(sizeof(SystemStatus) == 48);

struct MenuReadyArgs {
    uint64_t transition_origin_tick;
    uint64_t menu_main_tick;
    uint64_t initialize_start_tick;
    uint64_t gpu_ready_tick;
    uint64_t renderer_ready_tick;
    uint64_t blank_frame_tick;
    uint64_t activity_create_start_tick;
    uint64_t activity_create_end_tick;
    uint64_t command_send_tick;
    uint32_t menu_main_core;
    uint32_t activity_core;
    uint32_t catalog_count;
    uint32_t _pad;
};
static_assert(sizeof(MenuReadyArgs) == 88);

struct MenuFirstFrameArgs {
    uint64_t transition_origin_tick;
    uint64_t first_input_tick;
    uint64_t first_frame_tick;
    uint64_t image_memory_bytes;
    uint32_t core;
    uint32_t catalog_count;
};
static_assert(sizeof(MenuFirstFrameArgs) == 40);

struct MenuClosingArgs {
    uint64_t shutdown_start_tick;
    uint64_t gpu_drain_start_tick;
    uint64_t gpu_drain_end_tick;
    uint64_t on_destroy_start_tick;
    uint64_t http_cancel_done_tick;
    uint64_t worker_drain_done_tick;
    uint64_t state_persist_done_tick;
    uint64_t http_shutdown_done_tick;
    uint64_t bluetooth_done_tick;
    uint64_t command_send_tick;
    uint32_t core;
    uint32_t _pad;
};
static_assert(sizeof(MenuClosingArgs) == 88);

struct AppEntryHeader {
    uint64_t  title_id;
    uint32_t  name_len;
    uint32_t  icon_data_len;
    uint32_t  view_flags;
    uint8_t   startup_user_account;
    uint8_t   startup_user_account_option;
    uint8_t   startup_user_known;
    uint8_t   _pad;
};
static_assert(sizeof(AppEntryHeader) == 24);

static constexpr uint32_t kNotifyMagic = 0x53574E54;

struct DaemonNotification {
    uint32_t magic;
    MenuMessage msg;
    uint64_t  app_id;
    uint32_t  payload;
    uint32_t  _pad;
};

static constexpr uint32_t kBatteryPercentMask  = 0xFF;
static constexpr uint32_t kBatteryChargerShift = 8;
static constexpr uint32_t kBatteryChargerMask  = 0xFF << kBatteryChargerShift;

inline uint32_t makeBatteryPayload(uint32_t percentage, uint32_t chargerType) {
    if (percentage > 100)
        percentage = 100;
    return (percentage & kBatteryPercentMask)
        | ((chargerType & 0xFF) << kBatteryChargerShift);
}

inline uint32_t batteryPayloadPercentage(uint32_t payload) {
    return payload & kBatteryPercentMask;
}

inline uint32_t batteryPayloadChargerType(uint32_t payload) {
    return (payload & kBatteryChargerMask) >> kBatteryChargerShift;
}

inline bool batteryPayloadCharging(uint32_t payload) {
    return batteryPayloadChargerType(payload) != 0;
}

// App catalog file the daemon publishes and the menu reads at startup.
// A fresh catalog is staged at kAppCatalogTmpPath and swapped in by rename;
// the previous copy is kept at kAppCatalogBakPath for the duration of the
// swap so a reader landing in that window still finds a valid catalog.
static constexpr const char* kAppCatalogPath    = "sdmc:/config/OmniLaunch/applist.bin";
static constexpr const char* kAppCatalogTmpPath = "sdmc:/config/OmniLaunch/applist.tmp";
static constexpr const char* kAppCatalogBakPath = "sdmc:/config/OmniLaunch/applist.bak";

static constexpr uint64_t kMenuTakeoverProgramId = 0x010000000000100DULL;
static constexpr uint64_t kMenuProcessProgramId  = 0x010000000000FFFFULL;
static constexpr uint32_t kLdrAtmosRegisterExternalCode   = 65000;
static constexpr uint32_t kLdrAtmosUnregisterExternalCode = 65001;

}
