#pragma once
#include <switchu/smi_protocol.hpp>
#include <switchu/smi_helpers.hpp>
#include <switch.h>

#include <cstring>
#include <fstream>
#include <string>
#include <vector>

namespace switchu::menu::smi_cmd {


static Result pushOutStorage(const void* data, size_t size) {
    AppletStorage stor{};
    Result rc = appletCreateStorage(&stor, static_cast<s64>(size));
    if (R_FAILED(rc)) return rc;

    rc = appletStorageWrite(&stor, 0, data, size);
    if (R_FAILED(rc)) { appletStorageClose(&stor); return rc; }

    rc = appletPushInteractiveOutData(&stor);
    appletStorageClose(&stor);
    return rc;
}

static Result popInStorage(void* out, size_t maxSize, size_t* outActual) {
    AppletStorage stor{};
    Result rc = appletPopInteractiveInData(&stor);
    if (R_FAILED(rc)) return rc;

    s64 sz = 0;
    appletStorageGetSize(&stor, &sz);
    size_t toRead = (size_t)sz < maxSize ? (size_t)sz : maxSize;
    rc = appletStorageRead(&stor, 0, out, toRead);
    appletStorageClose(&stor);
    if (outActual) *outActual = toRead;
    return rc;
}

inline Result sendSimple(smi::SystemMessage msg) {
    smi::CommandHeader hdr{smi::kCommandMagic, static_cast<uint32_t>(msg)};
    return pushOutStorage(&hdr, sizeof(hdr));
}

inline Result requestSelfUninstall() {
    return sendSimple(smi::SystemMessage::RequestSelfUninstall);
}

// The player chose to leave safe mode: the daemon clears its boot guard and
// reboots into a normal start.
inline Result leaveSafeMode() {
    return sendSimple(smi::SystemMessage::LeaveSafeMode);
}

inline Result setManualDateTime(const smi::ManualDateTimeArgs& value) {
    uint8_t buf[sizeof(smi::CommandHeader) + sizeof(value)]{};
    auto* hdr = reinterpret_cast<smi::CommandHeader*>(buf);
    hdr->magic = smi::kCommandMagic;
    hdr->message = static_cast<uint32_t>(smi::SystemMessage::SetManualDateTime);
    std::memcpy(buf + sizeof(*hdr), &value, sizeof(value));
    return pushOutStorage(buf, sizeof(buf));
}

inline Result setInternetTimeSync(bool enabled) {
    smi::InternetTimeSyncArgs value{};
    value.enabled = enabled ? 1 : 0;
    uint8_t buf[sizeof(smi::CommandHeader) + sizeof(value)]{};
    auto* hdr = reinterpret_cast<smi::CommandHeader*>(buf);
    hdr->magic = smi::kCommandMagic;
    hdr->message = static_cast<uint32_t>(smi::SystemMessage::SetInternetTimeSync);
    std::memcpy(buf + sizeof(*hdr), &value, sizeof(value));
    return pushOutStorage(buf, sizeof(buf));
}

inline Result setPosixTime(uint64_t timestamp, bool isInternetSync) {
    smi::SetPosixTimeArgs value{};
    value.timestamp = timestamp;
    value.is_internet_sync = isInternetSync ? 1 : 0;
    uint8_t buf[sizeof(smi::CommandHeader) + sizeof(value)]{};
    auto* hdr = reinterpret_cast<smi::CommandHeader*>(buf);
    hdr->magic = smi::kCommandMagic;
    hdr->message = static_cast<uint32_t>(smi::SystemMessage::SetPosixTime);
    std::memcpy(buf + sizeof(*hdr), &value, sizeof(value));
    return pushOutStorage(buf, sizeof(buf));
}

inline Result prepareApplication(uint64_t titleId, AccountUid uid,
                                 smi::LaunchTransitionTrace& trace) {
    uint8_t buf[sizeof(smi::CommandHeader) + sizeof(smi::PrepareAppArgs)]{};
    auto* hdr = reinterpret_cast<smi::CommandHeader*>(buf);
    auto* args = reinterpret_cast<smi::PrepareAppArgs*>(buf + sizeof(smi::CommandHeader));

    hdr->magic = smi::kCommandMagic;
    hdr->message = static_cast<uint32_t>(smi::SystemMessage::PrepareApplication);
    args->title_id = titleId;
    std::memcpy(args->user_uid, &uid, sizeof(uid));
    trace.preflight_send_tick = armGetSystemTick();
    args->request_send_tick = trace.preflight_send_tick;

    return pushOutStorage(buf, sizeof(buf));
}

inline Result sendLaunchApplicationCommand(smi::SystemMessage message,
                                           uint64_t titleId, AccountUid uid,
                                           smi::LaunchTransitionTrace trace) {
    uint8_t buf[sizeof(smi::CommandHeader) + sizeof(smi::LaunchAppArgs)]{};
    auto* hdr = reinterpret_cast<smi::CommandHeader*>(buf);
    auto* args = reinterpret_cast<smi::LaunchAppArgs*>(buf + sizeof(smi::CommandHeader));

    hdr->magic    = smi::kCommandMagic;
    hdr->message  = static_cast<uint32_t>(message);
    args->title_id = titleId;
    std::memcpy(args->user_uid, &uid, sizeof(uid));
    args->trace = trace;
    args->trace.command_send_tick = armGetSystemTick();

    return pushOutStorage(buf, sizeof(buf));
}

inline Result launchApplication(uint64_t titleId, AccountUid uid,
                                smi::LaunchTransitionTrace trace) {
    return sendLaunchApplicationCommand(smi::SystemMessage::LaunchApplication,
                                        titleId, uid, trace);
}

#ifdef SWITCHU_PREFLIGHT_EDGE_TEST
inline Result launchApplicationFailureDiagnostic(
    uint64_t titleId, AccountUid uid, smi::LaunchTransitionTrace trace) {
    return sendLaunchApplicationCommand(smi::SystemMessage::DiagnosticLaunchFailure,
                                        titleId, uid, trace);
}
#endif

inline Result launchUserPage(AccountUid uid) {
    uint8_t buf[sizeof(smi::CommandHeader) + sizeof(smi::UserArgs)]{};
    auto* hdr = reinterpret_cast<smi::CommandHeader*>(buf);
    auto* args = reinterpret_cast<smi::UserArgs*>(buf + sizeof(smi::CommandHeader));

    hdr->magic    = smi::kCommandMagic;
    hdr->message  = static_cast<uint32_t>(smi::SystemMessage::LaunchUserPage);
    std::memcpy(args->user_uid, &uid, sizeof(uid));

    return pushOutStorage(buf, sizeof(buf));
}

inline Result sendResumeApplicationCommand(smi::SystemMessage message,
                                           smi::LaunchTransitionTrace trace) {
    uint8_t buf[sizeof(smi::CommandHeader) + sizeof(smi::ResumeAppArgs)]{};
    auto* hdr = reinterpret_cast<smi::CommandHeader*>(buf);
    auto* args = reinterpret_cast<smi::ResumeAppArgs*>(buf + sizeof(smi::CommandHeader));

    hdr->magic   = smi::kCommandMagic;
    hdr->message = static_cast<uint32_t>(message);
    args->trace = trace;
    args->trace.command_send_tick = armGetSystemTick();

    return pushOutStorage(buf, sizeof(buf));
}

inline Result resumeApplication(smi::LaunchTransitionTrace trace) {
    return sendResumeApplicationCommand(smi::SystemMessage::ResumeApplication,
                                        trace);
}

#ifdef SWITCHU_RESUME_FAILURE_TEST
inline Result resumeApplicationFailureDiagnostic(
    smi::LaunchTransitionTrace trace) {
    return sendResumeApplicationCommand(smi::SystemMessage::DiagnosticResumeFailure,
                                        trace);
}
#endif

inline Result terminateApplication() {
    return sendSimple(smi::SystemMessage::TerminateApplication);
}
#ifdef SWITCHU_TERMINATION_QUEUE_TEST
inline Result terminateApplicationHold() {
    return sendSimple(smi::SystemMessage::DiagnosticTerminateHold);
}
inline Result terminateApplicationForce() {
    return sendSimple(smi::SystemMessage::DiagnosticTerminateForce);
}
#endif

inline Result enterSleep()  { return sendSimple(smi::SystemMessage::EnterSleep); }
inline Result shutdown()    { return sendSimple(smi::SystemMessage::Shutdown); }
inline Result reboot()      { return sendSimple(smi::SystemMessage::Reboot); }
inline Result menuReady(smi::MenuReadyArgs args) {
    args.command_send_tick = armGetSystemTick();
    uint8_t buf[sizeof(smi::CommandHeader) + sizeof(args)]{};
    auto* hdr = reinterpret_cast<smi::CommandHeader*>(buf);
    hdr->magic = smi::kCommandMagic;
    hdr->message = static_cast<uint32_t>(smi::SystemMessage::MenuReady);
    std::memcpy(buf + sizeof(*hdr), &args, sizeof(args));
    return pushOutStorage(buf, sizeof(buf));
}

inline Result menuFirstFrame(const smi::MenuFirstFrameArgs& args) {
    uint8_t buf[sizeof(smi::CommandHeader) + sizeof(args)]{};
    auto* hdr = reinterpret_cast<smi::CommandHeader*>(buf);
    hdr->magic = smi::kCommandMagic;
    hdr->message = static_cast<uint32_t>(smi::SystemMessage::MenuFirstFrame);
    std::memcpy(buf + sizeof(*hdr), &args, sizeof(args));
    return pushOutStorage(buf, sizeof(buf));
}

inline Result menuClosing(smi::MenuClosingArgs args) {
    args.command_send_tick = armGetSystemTick();
    uint8_t buf[sizeof(smi::CommandHeader) + sizeof(args)]{};
    auto* hdr = reinterpret_cast<smi::CommandHeader*>(buf);
    hdr->magic = smi::kCommandMagic;
    hdr->message = static_cast<uint32_t>(smi::SystemMessage::MenuClosing);
    std::memcpy(buf + sizeof(*hdr), &args, sizeof(args));
    return pushOutStorage(buf, sizeof(buf));
}

struct AppEntry {
    uint64_t titleId;
    uint32_t nameLen;
    uint32_t iconLen;
    uint32_t viewFlags = 0;
    bool startupUserKnown = false;
    uint8_t startupUserAccount = 1;
    uint8_t startupUserAccountOption = 0;
    std::string name;
    std::vector<uint8_t> icon;
};

inline Result getAppList(std::vector<AppEntry>& outList, bool waitForDaemon = true) {
    outList.clear();
    std::ifstream file;
    // The daemon swaps the catalog in by rename. If we land inside that window
    // the previous copy is still on disk under .bak, so check it before
    // sleeping — sleeping is only for the case where the daemon hasn't
    // published a catalog yet at all.
    const int retries = waitForDaemon ? 10 : 1;
    for (int retry = 0; retry < retries && !file.is_open(); ++retry) {
        file.open(smi::kAppCatalogPath, std::ios::binary);
        if (!file.is_open())
            file.open(smi::kAppCatalogBakPath, std::ios::binary);
        if (!file.is_open() && waitForDaemon) svcSleepThread(20'000'000ULL);
    }
    if (!file.is_open()) return MAKERESULT(Module_Libnx, 0xFE);

    uint32_t count = 0;
    if (!file.read(reinterpret_cast<char*>(&count), sizeof(count)))
        return MAKERESULT(Module_Libnx, 0xFD);

    for (uint32_t i = 0; i < count; ++i) {
        smi::AppEntryHeader eh{};
        if (!file.read(reinterpret_cast<char*>(&eh), sizeof(eh))) break;

        AppEntry ent{};
        ent.titleId = eh.title_id;
        ent.nameLen = eh.name_len;
        ent.iconLen = eh.icon_data_len;
        ent.viewFlags = eh.view_flags;
        ent.startupUserKnown = eh.startup_user_known != 0;
        ent.startupUserAccount = eh.startup_user_account;
        ent.startupUserAccountOption = eh.startup_user_account_option;

        if (eh.name_len > 0) {
            ent.name.resize(eh.name_len);
            if (!file.read(ent.name.data(), static_cast<std::streamsize>(eh.name_len))) break;
        }

        if (eh.icon_data_len > 0) {
            ent.icon.resize(eh.icon_data_len);
            if (!file.read(reinterpret_cast<char*>(ent.icon.data()), static_cast<std::streamsize>(eh.icon_data_len))) break;
        }

        outList.push_back(std::move(ent));
    }

    return 0;
}


inline Result getSystemStatus(smi::SystemStatus& out) {
    Result rc = sendSimple(smi::SystemMessage::GetSystemStatus);
    if (R_FAILED(rc)) return rc;

    uint8_t buf[sizeof(smi::CommandHeader) + sizeof(smi::SystemStatus)]{};
    size_t actual = 0;
    for (int retry = 0; retry < 200; retry++) {
        rc = popInStorage(buf, sizeof(buf), &actual);
        if (R_SUCCEEDED(rc)) break;
        svcSleepThread(10'000'000ULL);
    }
    if (R_FAILED(rc)) return rc;

    if (actual >= sizeof(smi::CommandHeader) + sizeof(smi::SystemStatus)) {
        std::memcpy(&out, buf + sizeof(smi::CommandHeader), sizeof(smi::SystemStatus));
    }
    return 0;
}

}
