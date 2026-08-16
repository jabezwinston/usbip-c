/**
 * Copyright (C) 2026 Jabez Winston
 *
 * SPDX-License-Identifier: MIT
 *
 * Date : 14-June-2026
 *
 * os_compat.h - a thin POSIX-sockets vs. Winsock shim so the USB/IP transport
 * (usbip.c) and the Bluetooth H4-over-TCP server (tcp_server.c) build on both
 * Linux, macOS and Windows (mingw). On Linux it expands to exactly the set of
 * network headers those files already pulled in, in the same order, so the
 * generated code is unchanged. On Windows it maps the BSD socket API onto
 * Winsock2 (closesocket, SHUT_RDWR) and exposes a one-time WSAStartup. The
 * non-socket portability shims live in usbip_device_internal.h instead, so no
 * file picks up the Winsock headers just to read a clock.
 */
#ifndef OS_COMPAT_H
#define OS_COMPAT_H

#ifdef _WIN32
#include <winsock2.h>           /* must precede windows.h / ws2tcpip.h */
#include <ws2tcpip.h>
#include <pthread.h>            /* winpthreads */

#ifndef SHUT_RDWR
#define SHUT_RDWR SD_BOTH
#endif
#define sock_close closesocket
/* Winsock setsockopt()/getsockopt() take const char*; POSIX takes const void*.
 * The cast is a no-op on the wire, so the Linux object code is unchanged. */
#define SOCKOPT_VAL(p) ((const char *)(p))

typedef unsigned long in_addr_t;   /* POSIX type Winsock lacks; matches s_addr */

void usbip_net_startup(void);   /* idempotent WSAStartup (pthread_once) */

#else  /* POSIX - same headers, same order usbip.c/tcp_server.c used before */
#include <unistd.h>
#include <pthread.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <netdb.h>

#define sock_close close
#define SOCKOPT_VAL(p) (p)
static inline void usbip_net_startup(void) {}
#endif

/* ---- SIGPIPE ------------------------------------------------------------
 * A peer that vanishes mid-transfer has to surface as EPIPE from send(), never
 * as a signal that kills the whole process. The two spellings do not overlap
 * cleanly: MSG_NOSIGNAL is per-call and always there on Linux, but only reached
 * macOS in 14 and does not exist on Windows (which has no SIGPIPE to raise);
 * SO_NOSIGPIPE is per-socket and BSD/macOS-only, and accept() does NOT hand it
 * down - so set it on every socket we send on, not just the listener. Belt and
 * braces: whichever the host has, it is in force. */
#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

static inline void usbip_sock_nosigpipe(int fd)
{
#ifdef SO_NOSIGPIPE
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, SOCKOPT_VAL(&one), sizeof(one));
#else
    (void)fd;
#endif
}

#endif /* OS_COMPAT_H */
