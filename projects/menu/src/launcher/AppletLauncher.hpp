#pragma once
#include <functional>
#include <atomic>
#include <switch.h>
#include <switchu/smi_protocol.hpp>

class AppletLauncher {
public:
    struct Callbacks {
        std::function<void()>     playSfxModalHide;
        std::function<void()>     requestExit;
        // Blocks until this process has no disk write outstanding. Power
        // requests call it before handing off, so the reboot cannot catch one
        // of our writes half-done.
        std::function<void()>     quiesceWriters;
    };

    void init(Callbacks cbs);

    void quiesceForPower(const char* what);

    void launchAlbum();
    void launchMiiEditor();
    void launchControllerPairing();
    void launchControllerRemapping();
    void launchNetConnect();
    void launchUserPage(AccountUid uid);
    void launchUserCreator();
    void enterSleep();
    void shutdown();
    void reboot();
    // Requests a reboot after the caller has written the staged removal marker.
    // The daemon alone changes the live qlaunch override at the following boot.
    Result requestSelfUninstall();
    // Leaves the daemon's safe mode. The daemon reboots into a normal start.
    Result leaveSafeMode();
    // Pede ao daemon que releia os titulos instalados, jogando fora nomes e
    // icones em cache. Sem daemon nao ha catalogo para reler, e a versao
    // homebrew simplesmente nao faz nada.
    Result refreshCatalog();
    // Closes both logs and starts fresh ones, so the finished files can be
    // copied off the card while the console is running.
    Result rotateLogs();
    // Forgets every cached name and icon. About a second per installed title,
    // so it is offered separately from the ordinary catalogue refresh.
    Result rebuildControlCache();

    Result prepareApplication(uint64_t titleId, AccountUid uid,
                              switchu::smi::LaunchTransitionTrace& trace);
    void launchApplication(uint64_t titleId, AccountUid uid,
                           switchu::smi::LaunchTransitionTrace trace);
#ifdef SWITCHU_PREFLIGHT_EDGE_TEST
    void launchApplicationFailureDiagnostic(
        uint64_t titleId, AccountUid uid,
        switchu::smi::LaunchTransitionTrace trace);
#endif
    void resumeApplication(switchu::smi::LaunchTransitionTrace trace);
#ifdef SWITCHU_RESUME_FAILURE_TEST
    void resumeApplicationFailureDiagnostic(
        switchu::smi::LaunchTransitionTrace trace);
#endif
    void terminateApplication();
#ifdef SWITCHU_TERMINATION_QUEUE_TEST
    void terminateApplicationDuplicate();
    void terminateApplicationHold();
    void terminateApplicationForce();
#endif

    void checkRunningApplication();

    bool     isAppRunning()  const;
    bool     appHasForeground() const;
    bool     isAppSuspended(uint64_t titleId) const;
    uint64_t suspendedTitleId() const;

    void setAppRunning(bool v);
    void setAppHasForeground(bool v);
    void setSuspendedTitleId(uint64_t v);

#ifdef SWITCHU_MENU
    void setStartupStatus(uint64_t suspendedTitleId, bool appRunning);
#endif

private:
#ifdef SWITCHU_MENU
    std::atomic<bool>     m_appRunning{false};
    std::atomic<bool>     m_appHasForeground{false};
    std::atomic<uint64_t> m_suspendedTitleId{0};
#endif

    Callbacks m_cb;
};
