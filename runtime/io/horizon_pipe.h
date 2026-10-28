/* Copyright (c) 2026 MineSwitch contributors. SPDX-License-Identifier: GPL-2.0-only */
#ifndef MINESWITCH_HORIZON_PIPE_H
#define MINESWITCH_HORIZON_PIPE_H
#include <stdbool.h>
#include <poll.h>
#ifdef __cplusplus
extern "C" {
#endif
/* A real connected IPv4 loopback transport for Java Pipe/selector wakeups.
 * fds[0] is the Java source, fds[1] the Java sink. On failure no descriptors
 * remain open. Connection establishment has a 5 second deadline. */
int nxio_make_pipe(int fds[2], bool blocking);
/* Preserves poll semantics while yielding scarce BSD IPC sessions between
 * bounded waits. An empty slice is never reported as a spurious timeout. */
int nxio_socket_poll(struct pollfd *fds, nfds_t count, int timeout_ms);
#ifdef __cplusplus
}
#endif
#endif
