#include "install.h"
#include "log_tee.h"
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <switch.h>

static Thread installer_thread;
static atomic_bool finished;
static bool installed;

static void installer(void *unused) {
    (void)unused;
    installed = launcher_install_runtime();
    printf("MINESWITCH_GAME_DATA_RESULT success=%d\n", installed);
    atomic_store_explicit(&finished, true, memory_order_release);
}

int main(void) {
    consoleInit(NULL);
    Result sockets = socketInitializeDefault();
    int network = R_SUCCEEDED(sockets) ? nxlinkConnectToHost(false, false) : -1;
    launcher_log_init(network);
    printf("MINESWITCH_GAME_DATA_BEGIN version=1.21.1 applet=%d\n", (int)appletGetAppletType());
    printf("Installing verified Minecraft client, libraries and assets.\n");
    printf("Destination: /switch/mineswitch/minecraft\n");
    Result mounted = romfsInit();
    if (R_FAILED(mounted)) {
        printf("GAME_DATA_ROMFS_FAILED result=0x%08x\n", mounted);
        launcher_log_drain();
        svcExitProcess();
    }
    Result started = threadCreate(&installer_thread, installer, NULL, NULL, 256 * 1024, 0x3b, -2);
    if (R_SUCCEEDED(started)) started = threadStart(&installer_thread);
    if (R_FAILED(started)) {
        printf("GAME_DATA_THREAD_FAILED result=0x%08x\n", started);
        launcher_log_drain();
        svcExitProcess();
    }
    uint64_t begin = armGetSystemTick(), last = begin;
    while (appletMainLoop() && !atomic_load_explicit(&finished, memory_order_acquire)) {
        launcher_log_pump();
        launcher_console_update();
        uint64_t now = armGetSystemTick();
        if (armTicksToNs(now - last) >= UINT64_C(5000000000)) {
            printf("GAME_DATA_HEARTBEAT elapsed_s=%llu\n",
                   (unsigned long long)(armTicksToNs(now - begin) / UINT64_C(1000000000)));
            last = now;
        }
    }
    if (atomic_load_explicit(&finished, memory_order_acquire)) {
        Result joined = threadWaitForExit(&installer_thread);
        if (R_SUCCEEDED(joined)) joined = threadClose(&installer_thread);
        printf("MINESWITCH_GAME_DATA_END success=%d thread_result=0x%08x exit=process\n", installed, joined);
    } else {
        printf("MINESWITCH_GAME_DATA_END interrupted=1 exit=process\n");
    }
    launcher_log_drain();
    svcExitProcess();
}
