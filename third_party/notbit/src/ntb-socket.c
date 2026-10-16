/*
 * Notbit - A Bitmessage client
 * Copyright (C) 2013  Neil Roberts
 *
 * Permission to use, copy, modify, distribute, and sell this software and its
 * documentation for any purpose is hereby granted without fee, provided that
 * the above copyright notice appear in all copies and that both that copyright
 * notice and this permission notice appear in supporting documentation, and
 * that the name of the copyright holders not be used in advertising or
 * publicity pertaining to distribution of the software without specific,
 * written prior permission.  The copyright holders make no representations
 * about the suitability of this software for any purpose.  It is provided "as
 * is" without express or implied warranty.
 *
 * THE COPYRIGHT HOLDERS DISCLAIM ALL WARRANTIES WITH REGARD TO THIS SOFTWARE,
 * INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS, IN NO
 * EVENT SHALL THE COPYRIGHT HOLDERS BE LIABLE FOR ANY SPECIAL, INDIRECT OR
 * CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM LOSS OF USE,
 * DATA OR PROFITS, WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER
 * TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR PERFORMANCE
 * OF THIS SOFTWARE.
 */

#include "config.h"

#ifdef _WIN32
#include "ntb-win32.h"
#else
#include <fcntl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#endif
#include <errno.h>
#include <string.h>

#include "ntb-socket.h"
#include "ntb-file-error.h"

/* The system asks a peer that has been silent this long, again every
 * INTERVAL seconds, and gives up after COUNT asks without an answer:
 * 60 + 4 * 15 = two minutes to find a peer that vanished (a laptop that
 * slept, a router that forgot the flow). The node's own timeout is ten
 * minutes of silence, and a busy link is never silent. */
#define NTB_SOCKET_KEEPALIVE_IDLE 60
#define NTB_SOCKET_KEEPALIVE_INTERVAL 15
#define NTB_SOCKET_KEEPALIVE_COUNT 4

static void
set_int_option(int sock, int level, int name, int value)
{
        /* The cast is for Windows' setsockopt. Best effort: an option a
         * system lacks leaves its default. */
        (void) setsockopt(sock, level, name, (const char *) &value, sizeof value);
}

void
ntb_socket_set_keepalive(int sock)
{
        set_int_option(sock, SOL_SOCKET, SO_KEEPALIVE, 1);
#if defined(TCP_KEEPIDLE)
        set_int_option(sock, IPPROTO_TCP, TCP_KEEPIDLE, NTB_SOCKET_KEEPALIVE_IDLE);
#elif defined(TCP_KEEPALIVE)
        /* macOS: the idle time in seconds */
        set_int_option(sock, IPPROTO_TCP, TCP_KEEPALIVE, NTB_SOCKET_KEEPALIVE_IDLE);
#endif
#if defined(TCP_KEEPINTVL)
        set_int_option(sock, IPPROTO_TCP, TCP_KEEPINTVL, NTB_SOCKET_KEEPALIVE_INTERVAL);
#endif
#if defined(TCP_KEEPCNT)
        set_int_option(sock, IPPROTO_TCP, TCP_KEEPCNT, NTB_SOCKET_KEEPALIVE_COUNT);
#endif
}

bool
ntb_socket_set_nonblock(int sock,
                        struct ntb_error **error)
{
#ifdef _WIN32
        if (!ntb_win32_set_nonblock(sock)) {
#else
        int flags;

        flags = fcntl(sock, F_GETFL, 0);

        if (flags == -1 || fcntl(sock, F_SETFL, flags | O_NONBLOCK) == -1) {
#endif
                ntb_file_error_set(error,
                                   errno,
                                   "Error setting non-blocking mode: %s",
                                   strerror(errno));
                return false;
        }

        return true;
}
