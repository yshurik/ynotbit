/*
 * Notbit - A Bitmessage client
 *
 * Windows support for the notbit engine (ynotbit). The engine is written
 * against POSIX sockets, pipes and files; these helpers give it the same
 * contract on Winsock: -1 on failure with errno set to the POSIX value, so
 * the callers' EINTR / EAGAIN / EINPROGRESS logic works unchanged.
 */

#ifndef NTB_WIN32_H
#define NTB_WIN32_H

#ifdef _WIN32

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <io.h>
#include <malloc.h> /* alloca */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <pthread.h>

/* Winsock setup; call once before any socket is made. */
bool
ntb_win32_init(void);

/* Maps a Winsock error code to the closest POSIX errno value. */
int
ntb_win32_errno_from_wsa(int wsa_error);

int ntb_win32_socket(int domain, int type, int protocol);
int ntb_win32_connect(int sock, const struct sockaddr *address, int length);
int ntb_win32_accept(int sock, struct sockaddr *address, int *length);
int ntb_win32_bind(int sock, const struct sockaddr *address, int length);
int ntb_win32_listen(int sock, int backlog);
int ntb_win32_getsockopt(int sock, int level, int name, void *value,
                         int *length);
int ntb_win32_setsockopt(int sock, int level, int name, const void *value,
                         int length);
int ntb_win32_recv(int sock, void *buf, size_t length);
int ntb_win32_send(int sock, const void *buf, size_t length);
int ntb_win32_poll(struct pollfd *fds, unsigned long n_fds, int timeout);
bool ntb_win32_set_nonblock(int sock);

/* A connected pair of loopback sockets, standing in for pipe(): Winsock can
 * only poll sockets. */
int ntb_win32_pipe(int fds[2]);

/* Closes a socket (every "fd" the engine closes this way is one). */
int ntb_close(int fd);

/* rename() that replaces an existing target, as POSIX rename does. */
int ntb_win32_rename(const char *from, const char *to);

int ntb_win32_mkdir(const char *path);

/* Microseconds from a monotonic clock. */
uint64_t ntb_win32_monotonic_us(void);

int ntb_win32_cpu_count(void);

/* The C runtime's *_s variants, with the POSIX *_r argument order. */
#define gmtime_r(timep, result) (gmtime_s((result), (timep)) ? NULL : (result))
#define localtime_r(timep, result) \
        (localtime_s((result), (timep)) ? NULL : (result))

pthread_t
ntb_create_thread(void *(* thread_func)(void *),
                  void *user_data);

#endif /* _WIN32 */

#endif /* NTB_WIN32_H */
