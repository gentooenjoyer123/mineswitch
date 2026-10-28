#include "horizon_pipe.h"
#include "horizon_socket_io.h"
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <poll.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
#include <switch.h>

static FILE *network_log;
static unsigned passed, failed;
static Thread worker_thread;
static struct { int fd; bool receive; ssize_t result; int error; atomic_bool entered; } worker_state;
static Thread poll_threads[4];
static struct { int source, sink, result; short events; atomic_bool entered; } poll_states[4];
static void report(const char *format, ...) {
    va_list args;
    va_start(args, format); vprintf(format, args); va_end(args); fflush(stdout);
    if (network_log) {
        va_start(args, format); vfprintf(network_log, format, args); va_end(args); fflush(network_log);
    }
}
static bool check(const char *name, bool success) {
    if (success) ++passed; else ++failed;
    report("%s: %s errno=%d\n", name, success ? "PASS" : "FAIL", errno);
    return success;
}
static void worker(void *unused) {
    (void)unused;
    char value = 'W';
    atomic_store(&worker_state.entered, true);
    if (worker_state.receive) worker_state.result = recv(worker_state.fd, &value, 1, 0);
    else { svcSleepThread(INT64_C(100000000)); worker_state.result = send(worker_state.fd, &value, 1, 0); }
    worker_state.error = errno;
}
static bool start_worker(int fd, bool receiving) {
    worker_state.fd = fd;
    worker_state.receive = receiving;
    worker_state.result = -99;
    worker_state.error = 0;
    atomic_store(&worker_state.entered, false);
    Result result = threadCreate(&worker_thread, worker, NULL, NULL, 65536, 0x2c, -2);
    if (!check("worker.create", R_SUCCEEDED(result))) return false;
    result = threadStart(&worker_thread);
    if (!check("worker.start", R_SUCCEEDED(result))) { threadClose(&worker_thread); return false; }
    return true;
}
static bool finish_worker(void) {
    Result result = svcWaitSynchronizationSingle(worker_thread.handle, INT64_C(3000000000));
    if (!check("worker.finish_within_3s", R_SUCCEEDED(result))) return false;
    check("worker.close", R_SUCCEEDED(threadClose(&worker_thread)));
    return true;
}
static void poll_worker(void *argument) {
    size_t index = (size_t)argument;
    struct pollfd watch = {.fd=poll_states[index].source, .events=POLLIN, .revents=0};
    atomic_store(&poll_states[index].entered, true);
    poll_states[index].result = nxio_socket_poll(&watch, 1, 5000);
    poll_states[index].events = watch.revents;
}
static bool concurrent_poll_probe(void) {
    unsigned pairs = 0, created = 0, started = 0;
    for (unsigned i = 0; i < 4; ++i) {
        int fds[2];
        if (!check("sessions.pipe_create", nxio_make_pipe(fds, false) == 0)) goto finish;
        poll_states[i].source = fds[0]; poll_states[i].sink = fds[1];
        atomic_store(&poll_states[i].entered, false);
        ++pairs;
    }
    for (unsigned i = 0; i < 4; ++i) {
        Result rc = threadCreate(&poll_threads[i], poll_worker, (void*)(size_t)i, NULL, 65536, 0x2c, -2);
        if (!check("sessions.thread_create", R_SUCCEEDED(rc))) break;
        ++created;
        rc = threadStart(&poll_threads[i]);
        if (!check("sessions.thread_start", R_SUCCEEDED(rc))) break;
        ++started;
    }
    uint64_t begin = armGetSystemTick();
    bool entered;
    do {
        entered = true;
        for (unsigned i = 0; i < started; ++i) entered &= atomic_load(&poll_states[i].entered);
        if (!entered) svcSleepThread(1000000);
    } while (!entered && armTicksToNs(armGetSystemTick()-begin) < UINT64_C(1000000000));
    check("sessions.polls_entered", entered && started == 4);
    svcSleepThread(150000000);
    // More polling threads than libnx's three default BSD IPC sessions.
    // Each poll must release its session often enough to permit these sends.
    for (unsigned i = 0; i < started; ++i) {
        char wake = (char)('0' + i);
        check("sessions.wakeup_send", send(poll_states[i].sink, &wake, 1, 0) == 1);
    }
    for (unsigned i = 0; i < started; ++i) {
        Result rc = svcWaitSynchronizationSingle(poll_threads[i].handle, INT64_C(6000000000));
        if (!check("sessions.poll_thread_finished", R_SUCCEEDED(rc))) return false;
        check("sessions.actual_readiness", poll_states[i].result == 1 && (poll_states[i].events & POLLIN));
    }
    for (unsigned i = 0; i < created; ++i) check("sessions.thread_close", R_SUCCEEDED(threadClose(&poll_threads[i])));
finish:
    for (unsigned i = 0; i < pairs; ++i) {
        check("sessions.source_close", close(poll_states[i].source) == 0);
        check("sessions.sink_close", close(poll_states[i].sink) == 0);
    }
    return true;
}
static bool probe(void) {
    int descriptors[2];
    if (!check("pipe.nonblocking.create", nxio_make_pipe(descriptors, false) == 0)) return true;
    check("pipe.distinct_descriptors", descriptors[0] >= 0 && descriptors[1] >= 0 && descriptors[0] != descriptors[1]);
    int source_flags = fcntl(descriptors[0], F_GETFL), sink_flags = fcntl(descriptors[1], F_GETFL);
    check("pipe.nonblocking.flags", source_flags >= 0 && sink_flags >= 0 &&
          (source_flags & O_NONBLOCK) && (sink_flags & O_NONBLOCK));
    struct pollfd watch = {.fd=descriptors[0], .events=POLLIN, .revents=0};
    uint64_t timeout_start = armGetSystemTick();
    check("poll.empty_timeout", nxio_socket_poll(&watch, 1, 120) == 0);
    uint64_t timeout_elapsed = armTicksToNs(armGetSystemTick() - timeout_start);
    check("poll.no_early_slice_timeout", timeout_elapsed >= UINT64_C(90000000));
    char byte = 'A';
    check("pipe.write_byte", send(descriptors[1], &byte, 1, 0) == 1);
    watch.revents = 0;
    if (check("poll.real_readiness", nxio_socket_poll(&watch, 1, 1000) == 1 && (watch.revents & POLLIN))) {
        byte = 0;
        check("pipe.read_byte", recv(descriptors[0], &byte, 1, 0) == 1 && byte == 'A');
    }
    check("pipe.drained_would_block", recv(descriptors[0], &byte, 1, 0) == -1 &&
          (errno == EAGAIN || errno == EWOULDBLOCK));
    if (start_worker(descriptors[1], false)) {
        watch.revents = 0;
        uint64_t start = armGetSystemTick();
        int ready = nxio_socket_poll(&watch, 1, 2000);
        uint64_t elapsed = armTicksToNs(armGetSystemTick() - start);
        check("poll.worker_wakeup", ready == 1 && (watch.revents & POLLIN));
        report("poll wakeup elapsed=%.2f ms\n", elapsed / 1000000.0);
        if (!finish_worker()) return false;
        check("worker.write_result", worker_state.result == 1);
        if (ready == 1) check("poll.wakeup_payload", recv(descriptors[0], &byte, 1, 0) == 1 && byte == 'W');
    }
    char first[] = "vector-one", second[] = "VECTOR-TWO";
    struct iovec outgoing[2] = {{first, sizeof(first)-1}, {second, sizeof(second)-1}};
    check("vectors.send", horizon_socket_vector(descriptors[1], outgoing, 2, 1, 0) == 20);
    watch.revents = 0;
    if (check("vectors.ready", nxio_socket_poll(&watch, 1, 1000) == 1 && (watch.revents & POLLIN))) {
        char gathered[20] = {0};
        struct iovec incoming[2] = {{gathered, 7}, {gathered+7, 13}};
        ssize_t n = horizon_socket_vector(descriptors[0], incoming, 2, 0, 0);
        check("vectors.receive", n > 0 && n <= 20);
        if (n > 0) {
            check("vectors.prefix", memcmp(gathered, "vector-oneVECTOR-TWO", n) == 0);
            while (n < 20) {
                watch.revents = 0;
                if (nxio_socket_poll(&watch, 1, 1000) <= 0) break;
                ssize_t more = recv(descriptors[0], gathered+n, 20-n, 0);
                if (more <= 0) break;
                n += more;
            }
            check("vectors.complete_content", n == 20 && memcmp(gathered, "vector-oneVECTOR-TWO", 20) == 0);
        }
    }
    check("pipe.sink_close", close(descriptors[1]) == 0);
    watch.revents = 0;
    if (check("pipe.eof_ready", nxio_socket_poll(&watch, 1, 1000) == 1))
        check("pipe.eof_read", recv(descriptors[0], &byte, 1, 0) == 0);
    check("pipe.source_close", close(descriptors[0]) == 0);

    if (!check("pipe.blocking.create", nxio_make_pipe(descriptors, true) == 0)) return true;
    source_flags = fcntl(descriptors[0], F_GETFL); sink_flags = fcntl(descriptors[1], F_GETFL);
    check("pipe.blocking.flags", source_flags >= 0 && sink_flags >= 0 &&
          !(source_flags & O_NONBLOCK) && !(sink_flags & O_NONBLOCK));
    if (start_worker(descriptors[0], true)) {
        uint64_t start = armGetSystemTick();
        while (!atomic_load(&worker_state.entered) &&
               armTicksToNs(armGetSystemTick()-start) < UINT64_C(1000000000)) svcSleepThread(1000000);
        check("pipe.blocking_receiver_entered", atomic_load(&worker_state.entered));
        svcSleepThread(50000000);
        check("pipe.shutdown_to_cancel", shutdown(descriptors[0], SHUT_RDWR) == 0);
        if (!finish_worker()) return false;
        report("blocked receive after shutdown: result=%zd errno=%d\n", worker_state.result, worker_state.error);
        check("pipe.blocked_receive_released", worker_state.result == 0 || worker_state.result == -1);
    }
    check("pipe.blocking.source_close", close(descriptors[0]) == 0);
    check("pipe.blocking.sink_close", close(descriptors[1]) == 0);
    return concurrent_poll_probe();
}
int main(void) {
    consoleInit(NULL);
    Result sockets = socketInitializeDefault();
    if (R_SUCCEEDED(sockets)) {
        int fd = nxlinkConnectToHost(false, false);
        if (fd >= 0) { network_log = fdopen(fd, "w"); if (!network_log) close(fd); }
    }
    report("MINESWITCH_PIPE_BEGIN version=0.1.0 applet=%d\n", (int)appletGetAppletType());
    report("Real IPv4 loopback pipe, poll and shutdown checks; Java is not executed.\n");
    bool safe = true;
    if (check("sockets.init", R_SUCCEEDED(sockets))) safe = probe();
    report("MINESWITCH_PIPE_COMPLETE passed=%u failed=%u\n", passed, failed);
    if (!safe) {
        report("MINESWITCH_PIPE_END passed=%u failed=%u exit=process\n", passed, failed);
        svcExitProcess();
    }
    uint64_t start = armGetSystemTick();
    while (appletMainLoop() && armTicksToNs(armGetSystemTick()-start) < UINT64_C(2000000000)) consoleUpdate(NULL);
    report("MINESWITCH_PIPE_END passed=%u failed=%u exit=loader\n", passed, failed);
    if (network_log) fclose(network_log);
    if (R_SUCCEEDED(sockets)) socketExit();
    consoleExit(NULL);
    return 0;
}
