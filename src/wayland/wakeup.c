/* wakeup.c — the fd that XConnectionNumber hands out.
 *
 * libXt (and any client that waits on the display itself) blocks in select() on
 * ConnectionNumber(dpy).  If that is the Wayland socket, the client only wakes
 * when the compositor sends something on the connection, and while a key is
 * held that traffic is unrelated to the repeat deadline: repeats arrived in
 * bursts whenever some other event happened to arrive.  The pump could catch up
 * on the average rate, but the output still felt uneven.
 *
 * A small helper thread polls the Wayland fd *and* the repeat deadline and
 * forwards readiness to a pipe, which is what XConnectionNumber returns.  The
 * thread must not touch Wayland or Xlib state -- it only makes the pipe
 * readable -- so the single-threaded protocol handling is undisturbed.  The
 * deadline is shared through the two __atomic fields on the display; the main
 * thread updates them from the repeat pump.
 *
 * MW_NO_WAKEUP=1 disables the thread and falls back to returning the Wayland
 * fd, which is useful when debugging the event loop itself.
 */
#define _GNU_SOURCE
#include "internal.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/eventfd.h>

/* Wake the client's select: a readable pipe, or POLLERR once the display is
 * gone.  A full pipe already means "wake up", so a failed write is harmless. */
static void wake_signal(XDisplayImpl *dp)
{
    ssize_t n = write(dp->wake_pipe[1], "w", 1);
    (void)n;
}

static void *wake_thread(void *arg)
{
    XDisplayImpl *dp = arg;
    uint64_t signalled = 0, timer_signalled = 0;

    for (;;) {
        uint64_t dl     = __atomic_load_n(&dp->wake_deadline, __ATOMIC_SEQ_CST);
        int      active = __atomic_load_n(&dp->wake_active, __ATOMIC_SEQ_CST);
        uint64_t timer  = __atomic_load_n(&dp->wake_timer, __ATOMIC_SEQ_CST);
        int      armed  = __atomic_load_n(&dp->wake_armed, __ATOMIC_SEQ_CST);
        int      extra  = __atomic_load_n(&dp->wake_extra_fd, __ATOMIC_SEQ_CST);
        int      timeout = -1;
        uint64_t now = mw_now();

        if (active && dl != signalled) {
            int t = dl > now ? (int)(dl - now) : 0;
            timeout = t;
        }
        if (timer && timer != timer_signalled) {
            int t = timer > now ? (int)(timer - now) : 0;
            if (timeout < 0 || t < timeout) timeout = t;
        }
        if (timeout > 1000) timeout = 1000;

        /* The source fds are level-triggered: once one has been signalled, it
         * stays readable until the main thread reads it, so it is not watched
         * again until the main thread has drained the wake pipe (re-arming
         * it).  Watching it regardless made this thread spin whenever the
         * client was busy. */
        struct pollfd pfd[5] = {
            { armed ? dp->wl_fd : -1,      POLLIN, 0 },
            { dp->wake_stop,               POLLIN, 0 },
            { dp->wake_cmd,                POLLIN, 0 },
            { armed ? dp->session_fd : -1, POLLIN, 0 },
            { armed ? extra : -1,          POLLIN, 0 },
        };
        int r = poll(pfd, 5, timeout);
        if (r < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (pfd[1].revents & POLLIN)        /* asked to stop */
            break;
        if (pfd[2].revents & POLLIN) {      /* re-armed, or a deadline/fd moved */
            uint64_t v;
            ssize_t n = read(dp->wake_cmd, &v, sizeof v);
            (void)n;
            continue;
        }
        if ((pfd[0].revents | pfd[3].revents | pfd[4].revents) &
            (POLLIN | POLLHUP | POLLERR)) {
            /* Wayland data, a session command, or clipboard data (or the
             * peer's EOF) for a paste in progress. */
            __atomic_store_n(&dp->wake_armed, 0, __ATOMIC_SEQ_CST);
            wake_signal(dp);
            continue;
        }
        /* Timed out: a repeat, or a clipboard timeout, is due.  Signal once
         * per deadline -- signalling every loop would spin until the main
         * thread moves it. */
        now = mw_now();
        if (active && dl != signalled && now >= dl) {
            wake_signal(dp);
            signalled = dl;
        }
        if (timer && timer != timer_signalled && now >= timer) {
            wake_signal(dp);
            timer_signalled = timer;
        }
    }
    return NULL;
}

int mw_wakeup_start(XDisplayImpl *dp)
{
    dp->wake_pipe[0] = dp->wake_pipe[1] = -1;
    dp->wake_stop = -1;
    dp->wake_cmd = -1;

    if (getenv("MW_NO_WAKEUP"))
        return -1;

    if (pipe2(dp->wake_pipe, O_NONBLOCK | O_CLOEXEC) < 0)
        return -1;
    dp->wake_stop = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    dp->wake_cmd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (dp->wake_stop < 0 || dp->wake_cmd < 0)
        goto fail;

    dp->wake_active = 0;
    dp->wake_deadline = 0;
    dp->wake_armed = 1;
    dp->wake_extra_fd = -1;
    dp->wake_timer = 0;
    if (pthread_create(&dp->wake_thread, NULL, wake_thread, dp) != 0)
        goto fail;

    /* From here on the public fd is the pipe: libXt (and anything else using
     * the ConnectionNumber() macro) then waits on the fd the helper signals. */
    dp->fd = dp->wake_pipe[0];
    dp->wake_running = true;
    return 0;

fail:
    if (dp->wake_stop >= 0) { close(dp->wake_stop); dp->wake_stop = -1; }
    if (dp->wake_cmd >= 0) { close(dp->wake_cmd); dp->wake_cmd = -1; }
    if (dp->wake_pipe[0] >= 0) { close(dp->wake_pipe[0]); dp->wake_pipe[0] = -1; }
    if (dp->wake_pipe[1] >= 0) { close(dp->wake_pipe[1]); dp->wake_pipe[1] = -1; }
    return -1;
}

void mw_wakeup_stop(XDisplayImpl *dp)
{
    if (!dp->wake_running)
        return;
    uint64_t one = 1;
    ssize_t n = write(dp->wake_stop, &one, sizeof one);
    (void)n;
    pthread_join(dp->wake_thread, NULL);
    dp->wake_running = false;
    dp->fd = dp->wl_fd;

    if (dp->wake_stop >= 0) { close(dp->wake_stop); dp->wake_stop = -1; }
    if (dp->wake_cmd >= 0) { close(dp->wake_cmd); dp->wake_cmd = -1; }
    if (dp->wake_pipe[0] >= 0) { close(dp->wake_pipe[0]); dp->wake_pipe[0] = -1; }
    if (dp->wake_pipe[1] >= 0) { close(dp->wake_pipe[1]); dp->wake_pipe[1] = -1; }
}

/* Tell the helper when the next repeat is due.  The deadline is stored before
 * the flag so the thread never sees "active" with a stale deadline, and the
 * command eventfd wakes it out of poll() so a changed deadline is picked up at
 * once rather than after the previous one expired. */
void mw_wakeup_set(XDisplayImpl *dp, int active, uint64_t deadline_ms)
{
    if (!dp->wake_running)
        return;
    __atomic_store_n(&dp->wake_deadline, deadline_ms, __ATOMIC_SEQ_CST);
    __atomic_store_n(&dp->wake_active, active, __ATOMIC_SEQ_CST);
    uint64_t one = 1;
    ssize_t n = write(dp->wake_cmd, &one, sizeof one);
    (void)n;
}

/* Clear the wake pipe.  Called on every event pump so a stale byte cannot make
 * the client's select spin. */
void mw_wakeup_drain(XDisplayImpl *dp)
{
    if (dp->wake_pipe[0] < 0)
        return;
    char buf[64];
    while (read(dp->wake_pipe[0], buf, sizeof buf) > 0)
        ;
    /* The main thread is about to read whatever was ready: watch the source
     * fds again. */
    if (dp->wake_running && !__atomic_load_n(&dp->wake_armed, __ATOMIC_SEQ_CST)) {
        __atomic_store_n(&dp->wake_armed, 1, __ATOMIC_SEQ_CST);
        uint64_t one = 1;
        ssize_t n = write(dp->wake_cmd, &one, sizeof one);
        (void)n;
    }
}

/* Also wake the client for `fd` (an in-flight clipboard fetch, -1 for none)
 * and at `timer_ms` (0 for none): a paste's data, or its timeout, must reach
 * an Xt client that is blocked in select() on ConnectionNumber. */
void mw_wakeup_watch(XDisplayImpl *dp, int fd, uint64_t timer_ms)
{
    if (!dp->wake_running)
        return;
    int ofd = __atomic_exchange_n(&dp->wake_extra_fd, fd, __ATOMIC_SEQ_CST);
    uint64_t ot = __atomic_exchange_n(&dp->wake_timer, timer_ms, __ATOMIC_SEQ_CST);
    if (ofd != fd || ot != timer_ms) {
        uint64_t one = 1;
        ssize_t n = write(dp->wake_cmd, &one, sizeof one);
        (void)n;
    }
}
