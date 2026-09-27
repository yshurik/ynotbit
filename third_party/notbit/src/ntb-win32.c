/*
 * Notbit - A Bitmessage client
 *
 * Windows support for the notbit engine (ynotbit). See ntb-win32.h.
 */

#include "config.h"

#ifdef _WIN32

#include "ntb-win32.h"

#include <direct.h>
#include <errno.h>
#include <string.h>

#include "ntb-util.h"

bool
ntb_win32_init(void)
{
        WSADATA data;

        return WSAStartup(MAKEWORD(2, 2), &data) == 0;
}

int
ntb_win32_errno_from_wsa(int wsa_error)
{
        switch (wsa_error) {
        case WSAEINTR: return EINTR;
        case WSAEWOULDBLOCK: return EWOULDBLOCK;
        case WSAEINPROGRESS: return EINPROGRESS;
        case WSAEALREADY: return EALREADY;
        case WSAENOTSOCK: return ENOTSOCK;
        case WSAEADDRINUSE: return EADDRINUSE;
        case WSAEADDRNOTAVAIL: return EADDRNOTAVAIL;
        case WSAENETDOWN: return ENETDOWN;
        case WSAENETUNREACH: return ENETUNREACH;
        case WSAECONNABORTED: return ECONNABORTED;
        case WSAECONNRESET: return ECONNRESET;
        case WSAENOBUFS: return ENOBUFS;
        case WSAEISCONN: return EISCONN;
        case WSAENOTCONN: return ENOTCONN;
        case WSAETIMEDOUT: return ETIMEDOUT;
        case WSAECONNREFUSED: return ECONNREFUSED;
        case WSAEHOSTUNREACH: return EHOSTUNREACH;
        case WSAEAFNOSUPPORT: return EAFNOSUPPORT;
        case WSAEINVAL: return EINVAL;
        case WSAEACCES: return EACCES;
        case WSAEMFILE: return EMFILE;
        case 0: return 0;
        default: return EIO;
        }
}

/* Winsock reports failure as SOCKET_ERROR / INVALID_SOCKET and keeps the
 * reason in WSAGetLastError(); translate both into the POSIX convention. */
static int
fail(void)
{
        errno = ntb_win32_errno_from_wsa(WSAGetLastError());
        return -1;
}

int
ntb_win32_socket(int domain, int type, int protocol)
{
        SOCKET sock = socket(domain, type, protocol);

        if (sock == INVALID_SOCKET)
                return fail();

        /* Winsock handles are small kernel handle values, so they fit in the
         * int the engine keeps sockets in. */
        return (int) sock;
}

int
ntb_win32_connect(int sock, const struct sockaddr *address, int length)
{
        if (connect((SOCKET) sock, address, length) == SOCKET_ERROR) {
                fail();
                /* A non-blocking connect that is under way reports
                 * WSAEWOULDBLOCK where POSIX says EINPROGRESS. */
                if (errno == EWOULDBLOCK)
                        errno = EINPROGRESS;
                return -1;
        }

        return 0;
}

int
ntb_win32_accept(int sock, struct sockaddr *address, int *length)
{
        SOCKET accepted = accept((SOCKET) sock, address, length);

        if (accepted == INVALID_SOCKET)
                return fail();

        return (int) accepted;
}

int
ntb_win32_bind(int sock, const struct sockaddr *address, int length)
{
        return bind((SOCKET) sock, address, length) == SOCKET_ERROR ?
                fail() : 0;
}

int
ntb_win32_listen(int sock, int backlog)
{
        return listen((SOCKET) sock, backlog) == SOCKET_ERROR ? fail() : 0;
}

int
ntb_win32_getsockopt(int sock, int level, int name, void *value, int *length)
{
        if (getsockopt((SOCKET) sock, level, name,
                       (char *) value, length) == SOCKET_ERROR)
                return fail();

        /* SO_ERROR hands back a Winsock code; callers strerror() it. */
        if (level == SOL_SOCKET && name == SO_ERROR &&
            *length == (int) sizeof(int))
                *(int *) value = ntb_win32_errno_from_wsa(*(int *) value);

        return 0;
}

int
ntb_win32_setsockopt(int sock, int level, int name,
                     const void *value, int length)
{
        /* SO_REUSEADDR on Windows lets another process steal a bound port,
         * which is not what POSIX code means by it; Windows already allows
         * an immediate rebind after close, so skip it. */
        if (level == SOL_SOCKET && name == SO_REUSEADDR)
                return 0;

        return setsockopt((SOCKET) sock, level, name,
                          (const char *) value, length) == SOCKET_ERROR ?
                fail() : 0;
}

int
ntb_win32_recv(int sock, void *buf, size_t length)
{
        int got = recv((SOCKET) sock, buf,
                       length > INT32_MAX ? INT32_MAX : (int) length, 0);

        return got == SOCKET_ERROR ? fail() : got;
}

int
ntb_win32_send(int sock, const void *buf, size_t length)
{
        int wrote = send((SOCKET) sock, buf,
                         length > INT32_MAX ? INT32_MAX : (int) length, 0);

        return wrote == SOCKET_ERROR ? fail() : wrote;
}

int
ntb_win32_poll(struct pollfd *fds, unsigned long n_fds, int timeout)
{
        int n;

        /* WSAPoll rejects an empty set; POSIX poll just sleeps. */
        if (n_fds == 0) {
                Sleep(timeout < 0 ? INFINITE : (DWORD) timeout);
                return 0;
        }

        n = WSAPoll(fds, n_fds, timeout);

        return n == SOCKET_ERROR ? fail() : n;
}

bool
ntb_win32_set_nonblock(int sock)
{
        u_long on = 1;

        if (ioctlsocket((SOCKET) sock, FIONBIO, &on) == SOCKET_ERROR) {
                fail();
                return false;
        }

        return true;
}

int
ntb_win32_pipe(int fds[2])
{
        struct sockaddr_in address;
        int length = sizeof address;
        SOCKET listener, writer = INVALID_SOCKET, reader = INVALID_SOCKET;

        listener = socket(AF_INET, SOCK_STREAM, 0);
        if (listener == INVALID_SOCKET)
                return fail();

        memset(&address, 0, sizeof address);
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address.sin_port = 0;

        if (bind(listener, (struct sockaddr *) &address, sizeof address) ||
            getsockname(listener, (struct sockaddr *) &address, &length) ||
            listen(listener, 1))
                goto error;

        writer = socket(AF_INET, SOCK_STREAM, 0);
        if (writer == INVALID_SOCKET ||
            connect(writer, (struct sockaddr *) &address, sizeof address))
                goto error;

        reader = accept(listener, NULL, NULL);
        if (reader == INVALID_SOCKET)
                goto error;

        closesocket(listener);

        fds[0] = (int) reader;
        fds[1] = (int) writer;

        return 0;

error:
        fail();
        closesocket(listener);
        if (writer != INVALID_SOCKET)
                closesocket(writer);
        return -1;
}

int
ntb_close(int fd)
{
        return closesocket((SOCKET) fd) == SOCKET_ERROR ? fail() : 0;
}

int
ntb_win32_rename(const char *from, const char *to)
{
        if (MoveFileExA(from, to, MOVEFILE_REPLACE_EXISTING))
                return 0;

        errno = GetLastError() == ERROR_FILE_NOT_FOUND ||
                GetLastError() == ERROR_PATH_NOT_FOUND ? ENOENT : EACCES;
        return -1;
}

int
ntb_win32_mkdir(const char *path)
{
        return _mkdir(path);
}

uint64_t
ntb_win32_monotonic_us(void)
{
        static LARGE_INTEGER frequency;
        LARGE_INTEGER now;

        if (frequency.QuadPart == 0)
                QueryPerformanceFrequency(&frequency);

        QueryPerformanceCounter(&now);

        return (uint64_t) (now.QuadPart / frequency.QuadPart) * 1000000 +
                (uint64_t) (now.QuadPart % frequency.QuadPart) * 1000000 /
                frequency.QuadPart;
}

int
ntb_win32_cpu_count(void)
{
        SYSTEM_INFO info;

        GetSystemInfo(&info);

        return info.dwNumberOfProcessors > 0 ?
                (int) info.dwNumberOfProcessors : 1;
}

pthread_t
ntb_create_thread(void *(* thread_func)(void *),
                  void *user_data)
{
        pthread_t thread;
        int result;

        result = pthread_create(&thread,
                                NULL, /* attr */
                                thread_func,
                                user_data);

        if (result)
                ntb_fatal("Error creating thread: %s",
                          strerror(result));

        return thread;
}

#endif /* _WIN32 */
