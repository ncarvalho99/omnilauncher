#include "AppletLauncher.hpp"
#include "core/DebugLog.hpp"
#include "usb/Mtp.hpp"
#include <switchu/file_log.hpp>
#ifdef SWITCHU_MENU
#include "smi_commands.hpp"
#include <switchu/smi_protocol.hpp>
#endif
#include <switch.h>

void AppletLauncher::init(Callbacks cbs) {
    m_cb = std::move(cbs);
}

#ifdef SWITCHU_MENU
bool AppletLauncher::isAppRunning() const  { return m_appRunning; }
bool AppletLauncher::isAppSuspended(uint64_t titleId) const {
    return m_suspendedTitleId != 0 && m_suspendedTitleId == titleId;
}
uint64_t AppletLauncher::suspendedTitleId() const { return m_suspendedTitleId; }

void AppletLauncher::setAppRunning(bool v)          { m_appRunning = v; }
void AppletLauncher::setAppHasForeground(bool v)    { m_appHasForeground = v; }
void AppletLauncher::setSuspendedTitleId(uint64_t v){ m_suspendedTitleId = v; }

void AppletLauncher::setStartupStatus(uint64_t suspendedTitleId, bool appRunning) {
    m_suspendedTitleId = suspendedTitleId;
    m_appRunning       = appRunning;
    m_appHasForeground = false;
    DebugLog::log("[launcher] startup status: suspended=0x%016lX running=%d",
                  suspendedTitleId, appRunning);
}

void AppletLauncher::launchAlbum() {
    DebugLog::log("[launcher] requesting Album launch via daemon");
    Result rc = switchu::menu::smi_cmd::sendSimple(switchu::smi::SystemMessage::LaunchAlbum);
    DebugLog::log("[launcher] Album rc=0x%X", rc);
    if (R_SUCCEEDED(rc)) {
        if (m_cb.playSfxModalHide) m_cb.playSfxModalHide();
        if (m_cb.requestExit)      m_cb.requestExit();
    }
}

void AppletLauncher::launchMiiEditor() {
    DebugLog::log("[launcher] requesting Mii Editor launch via daemon");
    Result rc = switchu::menu::smi_cmd::sendSimple(switchu::smi::SystemMessage::LaunchMiiEditor);
    DebugLog::log("[launcher] Mii Editor rc=0x%X", rc);
    if (R_SUCCEEDED(rc)) {
        if (m_cb.playSfxModalHide) m_cb.playSfxModalHide();
        if (m_cb.requestExit)      m_cb.requestExit();
    }
}

void AppletLauncher::launchControllerPairing() {
    DebugLog::log("[launcher] requesting Controller pairing via daemon");
    Result rc = switchu::menu::smi_cmd::sendSimple(switchu::smi::SystemMessage::LaunchControllers);
    DebugLog::log("[launcher] Controller pairing rc=0x%X", rc);
    if (R_SUCCEEDED(rc)) {
        if (m_cb.playSfxModalHide) m_cb.playSfxModalHide();
        if (m_cb.requestExit)      m_cb.requestExit();
    }
}

void AppletLauncher::launchControllerRemapping() {
    DebugLog::log("[launcher] requesting Controller remapping via daemon");
    Result rc = switchu::menu::smi_cmd::sendSimple(
        switchu::smi::SystemMessage::LaunchControllerRemapping);
    DebugLog::log("[launcher] controller remapping rc=0x%X", rc);
    if (R_SUCCEEDED(rc)) {
        if (m_cb.playSfxModalHide) m_cb.playSfxModalHide();
        if (m_cb.requestExit)      m_cb.requestExit();
    }
}

void AppletLauncher::launchNetConnect() {
    DebugLog::log("[launcher] requesting NetConnect launch via daemon");
    Result rc = switchu::menu::smi_cmd::sendSimple(switchu::smi::SystemMessage::LaunchNetConnect);
    DebugLog::log("[launcher] NetConnect rc=0x%X", rc);
    if (R_SUCCEEDED(rc)) {
        if (m_cb.playSfxModalHide) m_cb.playSfxModalHide();
        if (m_cb.requestExit)      m_cb.requestExit();
    }
}

void AppletLauncher::launchUserCreator() {
    DebugLog::log("[launcher] requesting user creator via daemon");
    Result rc = switchu::menu::smi_cmd::sendSimple(switchu::smi::SystemMessage::LaunchUserCreator);
    DebugLog::log("[launcher] user creator rc=0x%X", rc);
    if (R_SUCCEEDED(rc)) {
        if (m_cb.playSfxModalHide) m_cb.playSfxModalHide();
        if (m_cb.requestExit)      m_cb.requestExit();
    }
}

void AppletLauncher::launchUserPage(AccountUid uid) {
    DebugLog::log("[launcher] requesting User Page launch via daemon");
    Result rc = switchu::menu::smi_cmd::launchUserPage(uid);
    DebugLog::log("[launcher] User Page rc=0x%X", rc);
    if (R_SUCCEEDED(rc)) {
        if (m_cb.playSfxModalHide) m_cb.playSfxModalHide();
        if (m_cb.requestExit)      m_cb.requestExit();
    }
}

// Wait for our own writes before the daemon is told to cut power. The menu
// does not exit here: asking it to made the daemon see no menu running and
// relaunch it into the middle of the reboot, leaving the console black.
void AppletLauncher::quiesceForPower(const char* what) {
    if (m_cb.quiesceWriters)
        m_cb.quiesceWriters();
    DebugLog::log("[launcher] requesting %s, writers quiesced", what);
}

void AppletLauncher::enterSleep() {
    quiesceForPower("sleep");
    switchu::menu::smi_cmd::enterSleep();
}

void AppletLauncher::shutdown() {
    quiesceForPower("shutdown");
    switchu::menu::smi_cmd::shutdown();
}

void AppletLauncher::reboot() {
    quiesceForPower("reboot");
    switchu::menu::smi_cmd::reboot();
}

Result AppletLauncher::leaveSafeMode() {
    quiesceForPower("leave-safe-mode");
    DebugLog::log("[launcher] asking the daemon to leave safe mode");
    return switchu::menu::smi_cmd::leaveSafeMode();
}

Result AppletLauncher::requestSelfUninstall() {
    quiesceForPower("self-uninstall");
    DebugLog::log("[launcher] requesting staged SwitchU removal");
    return switchu::menu::smi_cmd::requestSelfUninstall();
}

Result AppletLauncher::refreshCatalog() {
    DebugLog::log("[launcher] requesting catalog refresh");
    return switchu::menu::smi_cmd::sendSimple(switchu::smi::SystemMessage::RefreshCatalog);
}

Result AppletLauncher::rotateLogs() {
    DebugLog::log("[launcher] rotating logs on request");
    // The menu holds menu.log open for as long as it runs, and the daemon does
    // the same with daemon.log, so neither can be copied over MTP while the
    // console is up. Both are rotated: this process closes and reopens its own,
    // and the daemon is asked to do the same with its.
    switchu::FileLog::flush();
    switchu::FileLog::open("menu");
    const Result rc = switchu::menu::smi_cmd::sendSimple(switchu::smi::SystemMessage::RotateLogs);
    DebugLog::log("[launcher] log rotation rc=0x%X", rc);
    return rc;
}

Result AppletLauncher::rebuildControlCache() {
    DebugLog::log("[launcher] requesting control cache rebuild");
    return switchu::menu::smi_cmd::sendSimple(switchu::smi::SystemMessage::RebuildControlCache);
}

Result AppletLauncher::prepareApplication(uint64_t titleId, AccountUid uid,
                                          switchu::smi::LaunchTransitionTrace& trace) {
    const Result rc = switchu::menu::smi_cmd::prepareApplication(titleId, uid, trace);
    if (R_FAILED(rc))
        DebugLog::log("[launcher] preflight enqueue failed tid=%016lX rc=0x%X", titleId, rc);
    return rc;
}

void AppletLauncher::launchApplication(uint64_t titleId, AccountUid uid,
                                       switchu::smi::LaunchTransitionTrace trace) {
    DebugLog::log("[launcher] tid=%016lX", titleId);
    switchu::usb::mtpStop();
    Result rc = switchu::menu::smi_cmd::launchApplication(titleId, uid, trace);
    if (R_FAILED(rc)) {
        DebugLog::log("[launcher] FAIL: 0x%X", rc);
        return;
    }
    DebugLog::log("[launcher] command sent, closing menu");
    if (m_cb.requestExit) m_cb.requestExit();
}

#ifdef SWITCHU_PREFLIGHT_EDGE_TEST
void AppletLauncher::launchApplicationFailureDiagnostic(
    uint64_t titleId, AccountUid uid,
    switchu::smi::LaunchTransitionTrace trace) {
    DebugLog::log("[diagnostic-preflight-edge] sending synthetic create failure tid=%016lX",
                  titleId);
    const Result rc = switchu::menu::smi_cmd::launchApplicationFailureDiagnostic(
        titleId, uid, trace);
    if (R_FAILED(rc)) {
        DebugLog::log("[diagnostic-preflight-edge] command enqueue FAIL: 0x%X", rc);
        return;
    }
    DebugLog::log("[diagnostic-preflight-edge] command sent, closing menu");
    if (m_cb.requestExit) m_cb.requestExit();
}
#endif

void AppletLauncher::resumeApplication(switchu::smi::LaunchTransitionTrace trace) {
    if (m_suspendedTitleId == 0) {
        DebugLog::log("[launcher] no app suspended!");
        return;
    }
    switchu::usb::mtpStop();
    const Result rc = switchu::menu::smi_cmd::resumeApplication(trace);
    if (R_FAILED(rc)) {
        DebugLog::log("[launcher] resume enqueue FAIL: 0x%X", rc);
        return;
    }
    DebugLog::log("[launcher] resume command sent, closing menu");
    if (m_cb.requestExit) m_cb.requestExit();
}

#ifdef SWITCHU_RESUME_FAILURE_TEST
void AppletLauncher::resumeApplicationFailureDiagnostic(
    switchu::smi::LaunchTransitionTrace trace) {
    if (m_suspendedTitleId == 0) {
        DebugLog::log("[diagnostic-resume] ignored: no suspended application");
        return;
    }
    DebugLog::log("[diagnostic-resume] sending synthetic foreground failure tid=%016lX",
                  (uint64_t)m_suspendedTitleId);
    const Result rc = switchu::menu::smi_cmd::resumeApplicationFailureDiagnostic(trace);
    if (R_FAILED(rc)) {
        DebugLog::log("[diagnostic-resume] command enqueue FAIL: 0x%X", rc);
        return;
    }
    DebugLog::log("[diagnostic-resume] command sent, closing menu");
    if (m_cb.requestExit) m_cb.requestExit();
}
#endif

void AppletLauncher::terminateApplication() {
    if (m_suspendedTitleId == 0) {
        DebugLog::log("[launcher] no app suspended, nothing to terminate");
        return;
    }
    DebugLog::log("[launcher] requesting terminate 0x%016lX", (uint64_t)m_suspendedTitleId);
    const Result rc = switchu::menu::smi_cmd::terminateApplication();
    if (R_FAILED(rc))
        DebugLog::log("[launcher] terminate command FAIL: 0x%X", rc);
}

#ifdef SWITCHU_TERMINATION_QUEUE_TEST
void AppletLauncher::terminateApplicationDuplicate() {
    if (m_suspendedTitleId == 0) {
        DebugLog::log("[diagnostic] duplicate terminate ignored: no suspended app");
        return;
    }
    const Result first = switchu::menu::smi_cmd::terminateApplication();
    const Result second = switchu::menu::smi_cmd::terminateApplication();
    DebugLog::log("[diagnostic] duplicate terminate sent tid=%016lX rc=0x%X/0x%X",
                  (uint64_t)m_suspendedTitleId, first, second);
}

void AppletLauncher::terminateApplicationHold() {
    if (m_suspendedTitleId == 0) {
        DebugLog::log("[diagnostic] sleep/wake terminate ignored: no suspended app");
        return;
    }
    const Result rc = switchu::menu::smi_cmd::terminateApplicationHold();
    DebugLog::log("[diagnostic] sleep/wake terminate sent tid=%016lX rc=0x%X",
                  (uint64_t)m_suspendedTitleId, rc);
}

void AppletLauncher::terminateApplicationForce() {
    if (m_suspendedTitleId == 0) {
        DebugLog::log("[diagnostic] forced terminate ignored: no suspended app");
        return;
    }
    const Result rc = switchu::menu::smi_cmd::terminateApplicationForce();
    DebugLog::log("[diagnostic] forced terminate sent tid=%016lX rc=0x%X",
                  (uint64_t)m_suspendedTitleId, rc);
}
#endif

void AppletLauncher::checkRunningApplication() {
}

#else

bool AppletLauncher::isAppRunning() const            { return false; }
bool AppletLauncher::isAppSuspended(uint64_t) const  { return false; }
uint64_t AppletLauncher::suspendedTitleId() const    { return 0; }
void AppletLauncher::setAppRunning(bool)             {}
void AppletLauncher::setAppHasForeground(bool)       {}
void AppletLauncher::setSuspendedTitleId(uint64_t)   {}

void AppletLauncher::launchAlbum()             {}
void AppletLauncher::launchMiiEditor()         {}
void AppletLauncher::launchUserCreator()       {}
void AppletLauncher::launchControllerPairing() {}
void AppletLauncher::launchControllerRemapping() {}
void AppletLauncher::launchNetConnect()        {}
void AppletLauncher::launchUserPage(AccountUid) {}
void AppletLauncher::enterSleep()              {}
void AppletLauncher::shutdown()                {}
void AppletLauncher::reboot()                  {}
// No daemon to hand the removal to. A failure, not a pretend success: the
// caller then reports that nothing was changed instead of waiting for a
// restart that will never come.
Result AppletLauncher::requestSelfUninstall()  {
    return MAKERESULT(Module_Libnx, LibnxError_NotInitialized);
}
Result AppletLauncher::leaveSafeMode()         {
    return MAKERESULT(Module_Libnx, LibnxError_NotInitialized);
}
Result AppletLauncher::prepareApplication(uint64_t, AccountUid,
                                          switchu::smi::LaunchTransitionTrace&) { return 0; }
void AppletLauncher::launchApplication(uint64_t, AccountUid,
                                       switchu::smi::LaunchTransitionTrace) {}
Result AppletLauncher::refreshCatalog()        { return 0; }
Result AppletLauncher::rotateLogs()            { return 0; }
Result AppletLauncher::rebuildControlCache()   { return 0; }
void AppletLauncher::resumeApplication(switchu::smi::LaunchTransitionTrace) {}
void AppletLauncher::terminateApplication()    {}
void AppletLauncher::checkRunningApplication() {}

#endif
