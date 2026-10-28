/* Copyright (c) 2026 MineSwitch contributors. SPDX-License-Identifier: GPL-2.0-only */
#include "horizon_pipe.h"
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <stdint.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>
#include <switch/kernel/svc.h>

static int64_t monotonic_millis(void) {
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) return -1;
    return (int64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000;
}
static int blocking_mode(int fd, bool blocking) {
    int flags = fcntl(fd, F_GETFL);
    if (flags < 0) return -1;
    return fcntl(fd, F_SETFL, blocking ? flags & ~O_NONBLOCK : flags | O_NONBLOCK);
}

int nxio_socket_poll(struct pollfd *fds, nfds_t count, int timeout_ms) {
    if (timeout_ms == 0) return poll(fds, count, 0);
    int64_t start = monotonic_millis();
    if (start < 0) return -1;
    int64_t deadline = timeout_ms > 0 ? start + timeout_ms : 0;
    for (;;) {
        int slice = 50;
        if (timeout_ms > 0) {
            int64_t now = monotonic_millis();
            if (now < 0) return -1;
            if (now >= deadline) return poll(fds, count, 0);
            if (deadline - now < slice) slice = (int)(deadline - now);
        }
        int result = poll(fds, count, slice);
        if (result != 0) return result;
        // libnx has three BSD IPC sessions by default. Long waits must release
        // their session so another Java thread can send the wakeup byte.
        svcSleepThread(0);
    }
}
static int await_socket(int fd, short events, int64_t deadline) {
    for (;;) {
        int64_t now = monotonic_millis();
        if (now < 0) return -1;
        if (now >= deadline) { errno = ETIMEDOUT; return -1; }
        struct pollfd watch = {.fd=fd, .events=events, .revents=0};
        int ready = nxio_socket_poll(&watch, 1, (int)(deadline - now));
        if (ready < 0 && errno == EINTR) continue;
        if (ready < 0) return -1;
        if (!ready) { errno = ETIMEDOUT; return -1; }
        if (watch.revents & POLLNVAL) { errno = EBADF; return -1; }
        if (watch.revents & (events | POLLERR | POLLHUP)) return 0;
    }
}

int nxio_make_pipe(int fds[2], bool blocking) {
    if (!fds) { errno = EINVAL; return -1; }
    fds[0] = fds[1] = -1;
    int listener = -1, source = -1, sink = -1;
    int64_t start = monotonic_millis();
    if (start < 0) return -1;
    int64_t deadline = start + 5000;
    struct sockaddr_in address;
    memset(&address, 0, sizeof(address));
    address.sin_len = sizeof(address);
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listener < 0 || blocking_mode(listener, false) != 0 ||
        bind(listener, (struct sockaddr*)&address, sizeof(address)) != 0 ||
        listen(listener, 1) != 0) goto error;
    socklen_t length = sizeof(address);
    if (getsockname(listener, (struct sockaddr*)&address, &length) != 0) goto error;
    sink = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sink < 0 || blocking_mode(sink, false) != 0) goto error;
    if (connect(sink, (struct sockaddr*)&address, sizeof(address)) != 0 &&
        errno != EINPROGRESS && errno != EWOULDBLOCK) goto error;
    if (await_socket(sink, POLLOUT, deadline) != 0) goto error;
    int connect_error = 0;
    length = sizeof(connect_error);
    if (getsockopt(sink, SOL_SOCKET, SO_ERROR, &connect_error, &length) != 0) goto error;
    if (connect_error != 0) { errno = connect_error; goto error; }
    struct sockaddr_in client;
    length = sizeof(client);
    if (getsockname(sink, (struct sockaddr*)&client, &length) != 0) goto error;
    for (;;) {
        if (await_socket(listener, POLLIN, deadline) != 0) goto error;
        struct sockaddr_in peer;
        length = sizeof(peer);
        source = accept(listener, (struct sockaddr*)&peer, &length);
        if (source < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)) continue;
        if (source < 0) goto error;
        // Return only the connection created above, even with concurrent users
        // of loopback sockets. The listener is bound exclusively to loopback.
        if (peer.sin_family == AF_INET && peer.sin_port == client.sin_port &&
            peer.sin_addr.s_addr == client.sin_addr.s_addr) break;
        close(source);
        source = -1;
    }
    close(listener);
    listener = -1;
    int enabled = 1;
    if (setsockopt(source, IPPROTO_TCP, TCP_NODELAY, &enabled, sizeof(enabled)) != 0 ||
        setsockopt(sink, IPPROTO_TCP, TCP_NODELAY, &enabled, sizeof(enabled)) != 0 ||
        blocking_mode(source, blocking) != 0 || blocking_mode(sink, blocking) != 0) goto error;
    fds[0] = source;
    fds[1] = sink;
    return 0;
error: {
    int error = errno;
    if (source >= 0) close(source);
    if (sink >= 0) close(sink);
    if (listener >= 0) close(listener);
    errno = error;
    return -1;
    }
}
