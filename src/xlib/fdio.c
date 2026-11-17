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
    sigset_t pending;
    sigpending(&pending);
    bool was_pending = sigismember(&pending, SIGPIPE);
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

    if (epipe && !was_pending) {
        /* Swallow the SIGPIPE this write raised (and only that one). */
        struct timespec zero = { 0, 0 };
        while (sigtimedwait(&pipe_set, NULL, &zero) < 0 && errno == EINTR) {}
    }
    pthread_sigmask(SIG_SETMASK, &old, NULL);
    if (fl >= 0 && !(fl & O_NONBLOCK)) fcntl(fd, F_SETFL, fl);
    return rc;
}
