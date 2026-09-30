
#include "core/WiiUMenuApp.hpp"
#include "core/DebugLog.hpp"
#include "core/Config.hpp"
#include "core/NsService.hpp"
#include "tutorial/TutorialActivity.hpp"
#include "services/NtpClient.hpp"
#include <nxui/Application.hpp>
#include <fmt/format.h>
#ifdef SWITCHU_MENU
#include <nxui/core/GpuDevice.hpp>
#include <nxui/core/Renderer.hpp>
#include <switchu/smi_protocol.hpp>
#include <switchu/file_log.hpp>
#endif
#include <switch.h>
#include <SDL2/SDL.h>
#include <SDL2/SDL_ttf.h>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#ifdef SWITCHU_MENU
#include <cstring>
#endif
#include <memory>

namespace {
constexpr size_t kMenuAppletHeapSize = 224u * 1024u * 1024u;
}

extern "C" {
#ifdef SWITCHU_HOMEBREW
    u32 __nx_applet_type = AppletType_Application;

    size_t __nx_heap_size = 0xD000000;
#else
    u32 __nx_applet_type = AppletType_LibraryApplet;
    u32 __nx_fs_num_sessions = 1;

    // Ignorado enquanto __libnx_initheap existir logo abaixo; fica como o
    // ultimo degrau da escada de recuo, que e o valor que sempre funcionou.
    size_t __nx_heap_size = kMenuAppletHeapSize;

    TimeServiceType __nx_time_service_type = TimeServiceType_Menu;

    void __libnx_init_time(void);

    void __nx_win_init(void);
    void __nx_win_exit(void);
#endif
}

#ifndef SWITCHU_HOMEBREW
// Quanto do que o console concede o menu de fato pede.
//
// Eram 224 MB fixos, e tudo sai dali: memoria de imagem, framebuffers, fontes,
// icones, espeak, o heap do C++. O kernel concede 458 MB a este processo -- o
// [mem] do arranque mede isso -- entao havia mais de 200 MB nunca pedidos, e o
// muro de ~108 MB de imagem que derrubou o menu era este teto, nao o hardware.
//
// Em degraus, e nao um valor unico, porque o menu e um applet: a memoria vem de
// um bolso compartilhado, e ele sobe com um jogo suspenso na memoria. Pedir
// demais numa hora ruim faz svcSetHeapSize falhar, e uma falha aqui e o menu
// nao abrir. Descendo degrau a degrau, o pior caso e exatamente o que se tinha
// antes.
namespace {
// Degraus acima de 352 acrescentados depois de medir no console: o processo
// relata 457,9 MB no total e o teto anterior da escada, 352, era concedido na
// primeira tentativa -- ou seja, ela parava de pedir antes de o sistema recusar.
// Nao ha custo em tentar: svcSetHeapSize devolve erro e o degrau seguinte entra.
// Quanto o sistema concede de fato aparece no log como "heap concedido".
constexpr size_t kHeapLadder[] = {
    416u * 1024u * 1024u,
    400u * 1024u * 1024u,
    384u * 1024u * 1024u,
    368u * 1024u * 1024u,
    352u * 1024u * 1024u,
    320u * 1024u * 1024u,
    288u * 1024u * 1024u,
    256u * 1024u * 1024u,
    kMenuAppletHeapSize,
};

}  // namespace

// Preenchido no arranque, lido depois: nao da para registrar daqui, porque isto
// roda antes de existir heap, log ou cartao montado.
extern "C" size_t g_switchuHeapSize = 0;

extern "C" void __libnx_initheap(void) {
    extern char* fake_heap_start;
    extern char* fake_heap_end;

    void*  addr = nullptr;
    size_t size = 0;
    for (size_t want : kHeapLadder) {
        if (R_SUCCEEDED(svcSetHeapSize(&addr, want))) {
            size = want;
            break;
        }
    }

    g_switchuHeapSize = size;
    fake_heap_start = static_cast<char*>(addr);
    fake_heap_end   = static_cast<char*>(addr) + size;
}
#endif

#ifdef SWITCHU_HOMEBREW
extern "C" void userAppInit(void) {
    timeInitialize();
    setInitialize();
    setsysInitialize();
    accountInitialize(AccountServiceType_Application);
    psmInitialize();
    lblInitialize();
    romfsInit();
}

extern "C" void userAppExit(void) {
    lblExit();
    psmExit();
    accountExit();
    setsysExit();
    setExit();
    timeExit();
    romfsExit();
}
#else
extern "C" void __appInit(void) {
    Result rc;

    svcOutputDebugString("[SwitchU-menu] __appInit start", 30);

    rc = smInitialize();
    if (R_FAILED(rc)) diagAbortWithResult(MAKERESULT(Module_Libnx, 500));

    rc = fsInitialize();
    if (R_FAILED(rc)) diagAbortWithResult(MAKERESULT(Module_Libnx, 501));

    rc = appletInitialize();
    if (R_FAILED(rc)) {
        svcOutputDebugString("[SwitchU-menu] appletInitialize FAIL", 37);
        diagAbortWithResult(MAKERESULT(Module_Libnx, 502));
    }
    svcOutputDebugString("[SwitchU-menu] appletInitialize OK", 34);

    rc = hidInitialize();
    if (R_FAILED(rc)) svcOutputDebugString("[SwitchU-menu] hidInitialize FAIL", 33);

    timeInitialize();
    __libnx_init_time();
    setsysInitialize();
    setInitialize();
    lblInitialize();
    splInitialize();
    accountInitialize(AccountServiceType_System);

    {
        SetSysFirmwareVersion fw = {};
        if (R_SUCCEEDED(setsysGetFirmwareVersion(&fw)))
            hosversionSet(MAKEHOSVERSION(fw.major, fw.minor, fw.micro) | BIT(31));
    }

    rc = fsdevMountSdmc();
    if (R_FAILED(rc)) {
        svcOutputDebugString("[SwitchU-menu] fsdevMountSdmc FAIL, retry", 42);
        svcSleepThread(100'000'000ULL);
        rc = fsdevMountSdmc();
    }

    // Appended to across menu restarts, not rotated on each one: the menu
    // starts again every time a game is closed, and rotating there threw away
    // the morning to keep the afternoon. The explicit "rotate logs" action
    // still rotates, which is what it is for.
    switchu::FileLog::open("menu", switchu::FileLog::MENU_ROTATE_BYTES);
    DebugLog::openFileLog();
    DebugLog::log("[menu] __appInit complete (sd mount: 0x%X)", rc);

    __nx_win_init();
    DebugLog::log("[menu] __nx_win_init done");

    svcOutputDebugString("[SwitchU-menu] __appInit done", 29);
}

extern "C" void __appExit(void) {
    switchu::menu::shutdownNsService();
    DebugLog::closeFileLog();
    switchu::FileLog::close();

    __nx_win_exit();

    accountExit();
    splExit();
    lblExit();

    hidExit();
    appletExit();

    setExit();
    setsysExit();
    timeExit();

    fsdevUnmountAll();
    fsExit();
    smExit();
}

static switchu::smi::MenuStartMode readStartMode() {
    LibAppletArgs args{};
    AppletStorage stor{};
    if (R_SUCCEEDED(appletPopInData(&stor))) {
        s64 sz = 0;
        appletStorageGetSize(&stor, &sz);
        if (sz >= (s64)sizeof(LibAppletArgs)) {
            appletStorageRead(&stor, 0, &args, sizeof(args));
        }
        appletStorageClose(&stor);
    }
    return static_cast<switchu::smi::MenuStartMode>(args.LaVersion);
}

static switchu::smi::SystemStatus readSystemStatus() {
    switchu::smi::SystemStatus status{};
    AppletStorage stor{};
    if (R_SUCCEEDED(appletPopInData(&stor))) {
        s64 sz = 0;
        appletStorageGetSize(&stor, &sz);
        if (sz >= (s64)sizeof(switchu::smi::SystemStatus)) {
            appletStorageRead(&stor, 0, &status, sizeof(status));
        }
        appletStorageClose(&stor);
    }
    return status;
}
#endif

int main(int argc, char* argv[]) {
    (void)argc; (void)argv;

#ifdef SWITCHU_MENU
    const uint64_t menuMainTick = armGetSystemTick();
    const uint32_t menuMainCore = svcGetCurrentProcessorNumber();
#endif

    std::srand(static_cast<unsigned>(std::time(nullptr)));

#ifdef SWITCHU_HOMEBREW
    DebugLog::openFileLog();
    DebugLog::log("[hb] main() entry");
    DebugLog::log("[main] applet config...");
#else
    DebugLog::log("[menu] main() entry");

    auto startMode = readStartMode();
    auto sysStatus = readSystemStatus();

    DebugLog::log("[menu] start mode=%d  suspended=0x%016lX running=%d",
                  (int)startMode, sysStatus.suspended_app_id, sysStatus.app_running);

    {
        std::string sdPath = fmt::format("{}/shaders/", SD_ASSETS);
        nxui::Renderer::setShaderBasePath(sdPath);
        DebugLog::log("[menu] shader path: %s", sdPath.c_str());
    }
#endif

    DebugLog::log("[main] SDL_Init");
    if (SDL_Init(SDL_INIT_AUDIO) < 0)
        DebugLog::log("[main] SDL_Init FAILED: %s", SDL_GetError());

    DebugLog::log("[main] TTF_Init");
    if (TTF_Init() < 0)
        DebugLog::log("[main] TTF_Init FAILED: %s", TTF_GetError());

    DebugLog::log("[main] creating app...");
    {
        nxui::Application app;
#ifdef SWITCHU_HOMEBREW
        AppConfig startupConfig;
        startupConfig.load();
        auto makeMenuActivity = [startupConfig](bool fromTutorial = false) -> std::unique_ptr<nxui::Activity> {
            auto activity = std::make_unique<WiiUMenuApp>();
            if (!fromTutorial)
                activity->setStartupConfig(startupConfig);
            activity->setTutorialStartupFade(fromTutorial);
            return activity;
        };
        if (startupConfig.tutorialCompleted)
            app.setActivity(makeMenuActivity(false));
        else
            app.setActivity(std::make_unique<TutorialActivity>(makeMenuActivity));
        DebugLog::log("[hb] app.initialize...");
        app.setLogSink([](const char* msg) { DebugLog::log("%s", msg); });
        nxui::GpuDevice::setDebugSink([](const char* msg) {
            DebugLog::log("%s", msg);
            switchu::FileLog::flush();
        });
        if (app.initialize()) {
            DebugLog::log("[hb] app.run...");
            app.run();
        } else {
            DebugLog::log("[hb] app.initialize FAILED");
        }
        DebugLog::log("[hb] app.shutdown...");
        app.shutdown();
#else
        AppConfig startupConfig;
        startupConfig.load();
        auto makeMenuActivity = [sysStatus, startupConfig, menuMainTick, menuMainCore](bool fromTutorial = false) -> std::unique_ptr<nxui::Activity> {
            auto activity = std::make_unique<WiiUMenuApp>();
            if (!fromTutorial)
                activity->setStartupConfig(startupConfig);
            activity->setStartupStatus(sysStatus);
            activity->setMenuMainTrace(menuMainTick, menuMainCore);
            activity->setTutorialStartupFade(fromTutorial);
            return activity;
        };
        if (startupConfig.tutorialCompleted)
            app.setActivity(makeMenuActivity(false));
        else
            app.setActivity(std::make_unique<TutorialActivity>(makeMenuActivity));
        DebugLog::log("[menu] app.initialize...");
        {
            u64 total = 0;
            u64 used = 0;
            const Result totalRc = svcGetInfo(
                &total, InfoType_TotalMemorySize, CUR_PROCESS_HANDLE, 0);
            const Result usedRc = svcGetInfo(
                &used, InfoType_UsedMemorySize, CUR_PROCESS_HANDLE, 0);
            DebugLog::log(
                "[mem-startup] heap_mib=%.1f process_mib=%.1f used_mib=%.1f "
                "free_mib=%.1f total_rc=0x%X used_rc=0x%X",
                g_switchuHeapSize / 1048576.0, total / 1048576.0,
                used / 1048576.0,
                total >= used ? (total - used) / 1048576.0 : 0.0,
                totalRc, usedRc);
        }
        app.setLogSink([](const char* msg) { DebugLog::log("%s", msg); });
        nxui::GpuDevice::setDebugSink([](const char* msg) {
            DebugLog::log("%s", msg);
            switchu::FileLog::flush();
        });
        if (app.initialize()) {
            DebugLog::log("[menu] app.run...");
            app.run();
        } else {
            DebugLog::log("[menu] app.initialize FAILED");
        }
        DebugLog::log("[menu] app.shutdown...");
        app.shutdown();
        switchu::services::NtpClient::cleanup();
#endif
    }

    TTF_Quit();
    SDL_Quit();
#ifdef SWITCHU_HOMEBREW
    DebugLog::log("[hb] exit");
    DebugLog::closeFileLog();
#endif
#ifdef SWITCHU_MENU
    DebugLog::log("[menu] exit");
#endif
    return 0;
}
