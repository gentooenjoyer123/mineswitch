#include "install.h"
#include <errno.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <switch.h>
#include <sys/iosupport.h>
#include <sys/socket.h>
#include <unistd.h>

/* This installer has no JVM. Restore stdio and close networking only after
 * the sole installer worker has exited, then use the normal loader return. */
static Thread installer_thread;
static atomic_bool finished;
static bool installed;
static Mutex log_lock;
static const devoptab_t *original_stdout, *original_stderr;
static devoptab_t output_tab, error_tab;
static int log_socket = -1;
static unsigned char queue[16384];
static size_t queue_head, queue_used, dropped;

static ssize_t log_write(struct _reent *r, void *fd, const char *data, size_t size) {
    mutexLock(&log_lock);
    if (original_stdout && original_stdout->write_r)
        original_stdout->write_r(r, fd, data, size);
    if (log_socket >= 0) {
        size_t count = size < sizeof(queue) - queue_used ? size : sizeof(queue) - queue_used;
        size_t tail = (queue_head + queue_used) % sizeof(queue);
        size_t first = count < sizeof(queue) - tail ? count : sizeof(queue) - tail;
        memcpy(queue + tail, data, first);
        memcpy(queue, data + first, count - first);
        queue_used += count;
        dropped += size - count;
    }
    mutexUnlock(&log_lock);
    return (ssize_t)size;
}

static void log_init(int fd) {
    log_socket = fd;
    original_stdout = devoptab_list[STD_OUT];
    original_stderr = devoptab_list[STD_ERR];
    output_tab = *original_stdout;
    error_tab = *original_stderr;
    output_tab.name = "package-out";
    error_tab.name = "package-err";
    output_tab.write_r = error_tab.write_r = log_write;
    devoptab_list[STD_OUT] = &output_tab;
    devoptab_list[STD_ERR] = &error_tab;
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);
}

static void log_pump(bool render) {
    if (!mutexTryLock(&log_lock)) return;
    if (log_socket >= 0 && queue_used) {
        size_t count = queue_used < sizeof(queue) - queue_head ? queue_used : sizeof(queue) - queue_head;
        ssize_t sent = send(log_socket, queue + queue_head, count, MSG_DONTWAIT);
        if (sent > 0) {
            queue_head = (queue_head + (size_t)sent) % sizeof(queue);
            queue_used -= (size_t)sent;
        } else if (sent < 0 && errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK) {
            close(log_socket);
            log_socket = -1;
        }
    }
    if (render) consoleUpdate(NULL);
    mutexUnlock(&log_lock);
}

static void log_shutdown(void) {
    if (dropped) printf("PACKAGE_LOG_DROPPED bytes=%zu\n", dropped);
    uint64_t started = armGetSystemTick();
    while (log_socket >= 0 && queue_used &&
           armTicksToNs(armGetSystemTick() - started) < UINT64_C(3000000000)) {
        log_pump(false);
        svcSleepThread(16000000);
    }
    fflush(stdout);
    fflush(stderr);
    devoptab_list[STD_OUT] = original_stdout;
    devoptab_list[STD_ERR] = original_stderr;
    if (log_socket >= 0) close(log_socket);
    log_socket = -1;
}

static void installer(void *unused) {
    (void)unused;
    installed = launcher_install_runtime();
    printf("MINESWITCH_PACKAGE_RESULT success=%d target=/switch/mineswitch/MineSwitch.nro\n", installed);
    atomic_store_explicit(&finished, true, memory_order_release);
}

int main(void) {
    bool mounted = false, exit_requested = false, app_running = true;
    consoleInit(NULL);
    Result sockets = socketInitializeDefault();
    int network = R_SUCCEEDED(sockets) ? nxlinkConnectToHost(false, false) : -1;
    log_init(network);
    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    PadState pad;
    padInitializeDefault(&pad);
    printf("MINESWITCH_PACKAGE_BEGIN applet=%d payload_files=1\n", (int)appletGetAppletType());
    printf("Installing MineSwitch. Existing Minecraft data are reused.\n");
    printf("Destination: /switch/mineswitch/MineSwitch.nro\n");
    Result result = romfsInit();
    if (R_FAILED(result)) {
        printf("PACKAGE_ROMFS_FAILED result=0x%08x\n", result);
        goto finish;
    }
    mounted = true;
    result = threadCreate(&installer_thread, installer, NULL, NULL, 256 * 1024, 0x3b, -2);
    if (R_FAILED(result)) {
        printf("PACKAGE_THREAD_CREATE_FAILED result=0x%08x\n", result);
        goto finish;
    }
    result = threadStart(&installer_thread);
    if (R_FAILED(result)) {
        Result closed = threadClose(&installer_thread);
        printf("PACKAGE_THREAD_START_FAILED result=0x%08x close=0x%08x\n", result, closed);
        if (R_FAILED(closed)) { log_shutdown(); svcExitProcess(); }
        goto finish;
    }
    uint64_t started = armGetSystemTick(), last = started;
    while (!atomic_load_explicit(&finished, memory_order_acquire)) {
        if (app_running) app_running = appletMainLoop();
        if (!app_running) exit_requested = true;
        padUpdate(&pad);
        if (padGetButtonsDown(&pad) & HidNpadButton_Plus) exit_requested = true;
        log_pump(app_running);
        svcSleepThread(16000000);
        uint64_t now = armGetSystemTick();
        if (armTicksToNs(now - last) >= UINT64_C(5000000000)) {
            printf("PACKAGE_HEARTBEAT elapsed_s=%llu\n",
                   (unsigned long long)(armTicksToNs(now - started) / UINT64_C(1000000000)));
            last = now;
        }
    }
    result = threadWaitForExit(&installer_thread);
    if (R_SUCCEEDED(result)) result = threadClose(&installer_thread);
    printf("PACKAGE_WORKER_JOIN result=0x%08x\n", result);
    if (R_FAILED(result)) {
        /* Never unload executable code while a worker's exit is unconfirmed. */
        printf("MINESWITCH_PACKAGE_END success=0 exit=process reason=worker_cleanup\n");
        log_shutdown();
        svcExitProcess();
    }
finish:
    printf(installed ? "Installed. Open MineSwitch in Sphaira.\n" : "Installation failed. See the log before retrying.\n");
    printf("MINESWITCH_PACKAGE_END success=%d exit=loader\n", installed);
    uint64_t shown = armGetSystemTick();
    while (!exit_requested && app_running &&
           armTicksToNs(armGetSystemTick() - shown) < UINT64_C(3000000000)) {
        app_running = appletMainLoop();
        padUpdate(&pad);
        if (padGetButtonsDown(&pad) & HidNpadButton_Plus) exit_requested = true;
        log_pump(app_running);
        svcSleepThread(16000000);
    }
    log_shutdown();
    if (R_SUCCEEDED(sockets)) socketExit();
    if (mounted) romfsExit();
    consoleExit(NULL);
    return installed ? 0 : 1;
}
