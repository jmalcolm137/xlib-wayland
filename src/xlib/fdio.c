/* fdio.c — writing a whole buffer to a peer's descriptor (a Wayland
 * clipboard pipe, a broker socket) without the peer being able to kill or
 * hang the X client. */
#define _GNU_SOURCE
#include "internal.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <time.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static uint64_t mono_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

/* Write all `len` bytes to `fd`.
 *  - SIGPIPE is blocked for the write, and one it raised is consumed, so a
 *    peer that has closed its end (it gave up, or a clipboard manager closed
 *    early) yields an error here instead of terminating the application;
 *  - the fd is made non-blocking and writability is awaited with poll(), so a
 *    peer that stops reading costs at most `timeout_ms`, not a hung client;
 *  - EINTR and short writes are retried.
 * Returns 0 when everything was written, -1 otherwise. */
int mw_write_all(int fd, const void *buf, size_t len, int timeout_ms)
{
    if (fd < 0) return -1;
    const char *p = buf;
    int fl = fcntl(fd, F_GETFL);
    if (fl >= 0 && !(fl & O_NONBLOCK)) fcntl(fd, F_SETFL, fl | O_NONBLOCK);

    sigset_t pipe_set, old;
    sigemptyset(&pipe_set);
    sigaddset(&pipe_set, SIGPIPE);
    pthread_sigmask(SIG_BLOCK, &pipe_set, &old);

    uint64_t deadline = mono_ms() + (uint64_t)(timeout_ms > 0 ? timeout_ms : 0);
    int rc = 0;
    bool epipe = false;
    while (len > 0) {
        ssize_t n = write(fd, p, len);
        if (n > 0) { p += n; len -= (size_t)n; continue; }
        if (n < 0 && errno == EINTR) continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            uint64_t now = mono_ms();
            if (now >= deadline) { rc = -1; break; }
            struct pollfd pfd = { .fd = fd, .events = POLLOUT };
            int pr = poll(&pfd, 1, (int)(deadline - now));
            if (pr < 0 && errno == EINTR) continue;
            if (pr <= 0 || (pfd.revents & (POLLERR | POLLNVAL))) { rc = -1; break; }
            continue;
        }
        if (n < 0 && errno == EPIPE) epipe = true;
        rc = -1;
        break;
    }

    /* Swallow the SIGPIPE this write raised -- unless the caller's mask
     * already blocked SIGPIPE, in which case a plain write would have left it
     * pending as well, and the application expects to find it there. */
    if (epipe && !sigismember(&old, SIGPIPE)) {
        struct timespec zero = { 0, 0 };
        while (sigtimedwait(&pipe_set, NULL, &zero) < 0 && errno == EINTR) {}
    }
    pthread_sigmask(SIG_SETMASK, &old, NULL);
    if (fl >= 0 && !(fl & O_NONBLOCK)) fcntl(fd, F_SETFL, fl);
    return rc;
}

/* One non-blocking write attempt, with the same SIGPIPE handling: the number
 * of bytes written (0 when the peer is not ready), or -1 on an error such as
 * the peer having gone away. */
ssize_t mw_write_some(int fd, const void *buf, size_t len)
{
    if (fd < 0) return -1;
    if (len == 0) return 0;
    int fl = fcntl(fd, F_GETFL);
    if (fl >= 0 && !(fl & O_NONBLOCK)) fcntl(fd, F_SETFL, fl | O_NONBLOCK);
    sigset_t pipe_set, old;
    sigemptyset(&pipe_set);
    sigaddset(&pipe_set, SIGPIPE);
    pthread_sigmask(SIG_BLOCK, &pipe_set, &old);
    ssize_t n;
    do n = write(fd, buf, len); while (n < 0 && errno == EINTR);
    int err = errno;
    if (n < 0 && err == EPIPE && !sigismember(&old, SIGPIPE)) {
        struct timespec zero = { 0, 0 };
        while (sigtimedwait(&pipe_set, NULL, &zero) < 0 && errno == EINTR) {}
    }
    pthread_sigmask(SIG_SETMASK, &old, NULL);
    if (fl >= 0 && !(fl & O_NONBLOCK)) fcntl(fd, F_SETFL, fl);
    if (n < 0) return (err == EAGAIN || err == EWOULDBLOCK) ? 0 : -1;
    return n;
}

/* The directory for the files and sockets shim processes share:
 * $XDG_RUNTIME_DIR/xlib-wayland, or /tmp/xlib-wayland-<uid> without one.
 * It is created mode 0700 and only used if it is a real directory owned by
 * this user and closed to others -- a shared /tmp/xlib-wayland could be
 * created first by another user, who would then own (or symlink) the files
 * in it.  False when there is no safe directory. */
bool mw_runtime_dir(char *buf, size_t n)
{
    const char *rt = getenv("XDG_RUNTIME_DIR");
    int w;
    if (rt && *rt) w = snprintf(buf, n, "%s/xlib-wayland", rt);
    else           w = snprintf(buf, n, "/tmp/xlib-wayland-%u", (unsigned)getuid());
    if (w < 0 || (size_t)w >= n) return false;
    mkdir(buf, 0700);
    struct stat st;
    if (lstat(buf, &st) != 0 || !S_ISDIR(st.st_mode) ||
        st.st_uid != getuid() || (st.st_mode & 077) != 0)
        return false;
    return true;
}
