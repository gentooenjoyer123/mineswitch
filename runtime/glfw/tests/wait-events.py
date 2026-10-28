#!/usr/bin/env python3
"""Test the real Switch GLFW wait functions with a pthread-backed UEvent.

Host behavior evidence only; SDK compilation and device CPU/input checks remain
separate. The previous poll-only implementation fails the blocking assertions.
"""
import os
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[3]
if os.environ.get("MINESWITCH_BUILD_LIMITED") != "1":
    os.execv(sys.executable, [sys.executable, str(ROOT / "runtime/limit-build.py"),
                             "--", sys.executable, __file__])
source = (ROOT / ".upstream/glfw-switch/src/switch_window.c").read_text()
start = source.index("void _glfwPlatformWaitEvents(void)")
end = source.index("void _glfwPlatformGetCursorPos", start)
functions = source[start:end]
build = ROOT / "runtime/glfw/build/wait-events"
build.mkdir(parents=True, exist_ok=True)
header = r'''
#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <errno.h>
#include <math.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <time.h>
typedef uint64_t u64;
typedef struct { pthread_mutex_t mutex; pthread_cond_t condition; bool signal; } UEvent;
typedef UEvent* Waiter;
static struct { struct { UEvent wake_event; } nx; } _glfw;
static unsigned waits, polls, wakeups;
static u64 last_timeout;
static u64 now(clockid_t clock) {
    struct timespec t; assert(clock_gettime(clock, &t) == 0);
    return (u64)t.tv_sec * 1000000000ULL + (u64)t.tv_nsec;
}
static void ueventCreate(UEvent* event, bool clear) {
    assert(clear); assert(pthread_mutex_init(&event->mutex, NULL) == 0);
    pthread_condattr_t attributes;
    assert(pthread_condattr_init(&attributes) == 0);
    assert(pthread_condattr_setclock(&attributes, CLOCK_MONOTONIC) == 0);
    assert(pthread_cond_init(&event->condition, &attributes) == 0);
    assert(pthread_condattr_destroy(&attributes) == 0);
    event->signal = false;
}
static Waiter waiterForUEvent(UEvent* event) { return event; }
static void ueventSignal(UEvent* event) {
    assert(pthread_mutex_lock(&event->mutex) == 0);
    event->signal = true;
    assert(pthread_cond_signal(&event->condition) == 0);
    assert(pthread_mutex_unlock(&event->mutex) == 0);
}
static int waitSingle(Waiter event, u64 timeout) {
    ++waits; last_timeout = timeout;
    u64 end = now(CLOCK_MONOTONIC) + timeout;
    struct timespec deadline = {(time_t)(end / 1000000000ULL), (long)(end % 1000000000ULL)};
    assert(pthread_mutex_lock(&event->mutex) == 0);
    int result = 0;
    while (!event->signal && result == 0)
        result = pthread_cond_timedwait(&event->condition, &event->mutex, &deadline);
    assert(result == 0 || result == ETIMEDOUT);
    if (event->signal) { ++wakeups; event->signal = false; }
    assert(pthread_mutex_unlock(&event->mutex) == 0);
    return result;
}
static void _glfwPlatformPollEvents(void) { ++polls; }
void _glfwPlatformWaitEventsTimeout(double timeout);
'''
tests = r'''
static void* post(void* unused) {
    (void)unused;
    const struct timespec delay = {0, 2000000};
    nanosleep(&delay, NULL);
    _glfwPlatformPostEmptyEvent();
    return NULL;
}
int main(void) {
    ueventCreate(&_glfw.nx.wake_event, true);
    u64 begun = now(CLOCK_MONOTONIC);
    _glfwPlatformWaitEventsTimeout(0.008);
    assert(waits == 1 && polls == 1 && last_timeout == 8000000);
    assert(now(CLOCK_MONOTONIC) - begun >= 5000000);

    _glfwPlatformPostEmptyEvent();
    _glfwPlatformWaitEventsTimeout(1000.0);
    assert(last_timeout == 16000000 && wakeups == 1);
    assert(!_glfw.nx.wake_event.signal);
    _glfwPlatformWaitEventsTimeout(0.0);
    _glfwPlatformWaitEventsTimeout(-1.0);
    _glfwPlatformWaitEventsTimeout(NAN);
    assert(waits == 2 && polls == 5);

    _glfwPlatformPostEmptyEvent();
    _glfwPlatformWaitEventsTimeout(0.000000000001);
    assert(last_timeout == 1 && wakeups == 2);
    _glfwPlatformPostEmptyEvent();
    _glfwPlatformWaitEvents();
    assert(last_timeout == 16000000 && wakeups == 3);

    pthread_t producer;
    assert(pthread_create(&producer, NULL, post, NULL) == 0);
    _glfwPlatformWaitEventsTimeout(1.0);
    assert(pthread_join(producer, NULL) == 0);
    assert(wakeups == 4 && !_glfw.nx.wake_event.signal);

    // The real Minecraft limiter retries until its deadline. It must wait
    // instead of repeatedly polling for most of a core's available time.
    unsigned before_waits = waits;
    begun = now(CLOCK_MONOTONIC);
    u64 cpu = now(CLOCK_THREAD_CPUTIME_ID), deadline = begun + 60000000;
    for (;;) {
        u64 current = now(CLOCK_MONOTONIC);
        if (current >= deadline) break;
        double remaining = (deadline - current) / 1000000000.0;
        _glfwPlatformWaitEventsTimeout(remaining);
    }
    u64 used = now(CLOCK_THREAD_CPUTIME_ID) - cpu;
    assert(waits - before_waits >= 3 && waits - before_waits <= 8);
    assert(used < 15000000);
    printf("GLFW_WAIT_PASS timeout, queued wake, concurrent wake, bounded HID polling, frame limiter cpu_ns=%llu\n", (unsigned long long)used);
    assert(pthread_cond_destroy(&_glfw.nx.wake_event.condition) == 0);
    assert(pthread_mutex_destroy(&_glfw.nx.wake_event.mutex) == 0);
    return 0;
}
'''
unit = build / "wait-events.c"
unit.write_text(header + functions + tests)
binary = build / "wait-events"
subprocess.run(["cc", "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror", "-pthread",
                str(unit), "-o", str(binary)], check=True)
subprocess.run([str(binary)], check=True, timeout=10)
