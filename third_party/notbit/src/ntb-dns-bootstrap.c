/*
 * Notbit - A Bitmessage client
 * Copyright (C) 2014  Neil Roberts
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
#include <netdb.h>
#endif
#include <string.h>

#include "ntb-dns-bootstrap.h"
#include "ntb-buffer.h"
#include "ntb-netaddress.h"
#include "ntb-main-context.h"
#include "ntb-log.h"

static void
lookup_address(const char *node,
               int port,
               ntb_dns_bootstrap_func callback,
               void *user_data)
{
        struct ntb_netaddress_native native_address;
        struct ntb_netaddress address;
        struct addrinfo *addrinfo, *a;
        int ret;

        ret = getaddrinfo(node,
                          NULL, /* service */
                          NULL, /* hints */
                          &addrinfo);

        if (ret) {
                ntb_log("Resolving %s failed: %s",
                        node,
                        gai_strerror(ret));
                return;
        }

        for (a = addrinfo; a; a = a->ai_next) {
                switch (a->ai_family) {
                case AF_INET:
                        if (a->ai_addrlen != sizeof (struct sockaddr_in))
                                continue;
                        break;
                case AF_INET6:
                        if (a->ai_addrlen != sizeof (struct sockaddr_in6))
                                continue;
                        break;
                default:
                        continue;
                }

                memcpy(&native_address.sockaddr, a->ai_addr, a->ai_addrlen);
                native_address.length = a->ai_addrlen;

                ntb_netaddress_from_native(&address, &native_address);
                address.port = port;

                callback(&address, user_data);
        }

        freeaddrinfo(addrinfo);
}

void
ntb_dns_bootstrap(ntb_dns_bootstrap_func callback,
                  void *user_data)
{
        ntb_log("Doing DNS bootstrap");

        lookup_address("bootstrap8080.bitmessage.org",
                       8080,
                       callback,
                       user_data);
        lookup_address("bootstrap8444.bitmessage.org",
                       8444,
                       callback,
                       user_data);
}

#include <pthread.h>
#include <stdlib.h>

struct ntb_dns_bootstrap_job {
        pthread_t thread;
        pthread_mutex_t mutex;
        bool done;
        unsigned count;
        struct ntb_netaddress addresses[128];
};

static void
collect_address(const struct ntb_netaddress *address, void *data)
{
        struct ntb_dns_bootstrap_job *job = data;
        if (job->count < 128)
                job->addresses[job->count++] = *address;
}

static void *
resolve_bootstrap(void *data)
{
        struct ntb_dns_bootstrap_job *job = data;
        ntb_dns_bootstrap(collect_address, job);
        pthread_mutex_lock(&job->mutex);
        job->done = true;
        pthread_mutex_unlock(&job->mutex);
        return NULL;
}

struct ntb_dns_bootstrap_job *
ntb_dns_bootstrap_start(void)
{
        struct ntb_dns_bootstrap_job *job = calloc(1, sizeof *job);
        if (!job)
                return NULL;
        if (pthread_mutex_init(&job->mutex, NULL)) {
                free(job);
                return NULL;
        }
        if (pthread_create(&job->thread, NULL, resolve_bootstrap, job)) {
                pthread_mutex_destroy(&job->mutex);
                free(job);
                return NULL;
        }
        return job;
}

bool
ntb_dns_bootstrap_poll(struct ntb_dns_bootstrap_job *job,
                       ntb_dns_bootstrap_func callback, void *data)
{
        pthread_mutex_lock(&job->mutex);
        bool done = job->done;
        pthread_mutex_unlock(&job->mutex);
        if (!done)
                return false;
        for (unsigned i = 0; i < job->count; i++)
                callback(&job->addresses[i], data);
        return true;
}

void
ntb_dns_bootstrap_free(struct ntb_dns_bootstrap_job *job)
{
        if (!job)
                return;
        /* getaddrinfo owns its resources until it returns; never detach a worker
         * that might still log through a destroyed logger or access this job. */
        pthread_join(job->thread, NULL);
        pthread_mutex_destroy(&job->mutex);
        free(job);
}
