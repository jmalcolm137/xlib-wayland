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
    uint64_t signalled = 0;

    for (;;) {
        uint64_t dl     = __atomic_load_n(&dp->wake_deadline, __ATOMIC_SEQ_CST);
        int      active = __atomic_load_n(&dp->wake_active, __ATOMIC_SEQ_CST);
        int      timeout = -1;

        if (active && dl != signalled) {
            uint64_t now = mw_now();
            timeout = dl > now ? (int)(dl - now) : 0;
            if (timeout > 1000) timeout = 1000;
        }

        struct pollfd pfd[3] = {
            { dp->wl_fd,     POLLIN, 0 },
            { dp->wake_stop, POLLIN, 0 },
            { dp->wake_cmd,  POLLIN, 0 },
        };
        int r = poll(pfd, 3, timeout);
        if (r < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (pfd[1].revents & POLLIN)        /* asked to stop */
            break;
        if (pfd[2].revents & POLLIN) {      /* the main thread moved the deadline */
            uint64_t v;
            ssize_t n = read(dp->wake_cmd, &v, sizeof v);
            (void)n;
            continue;
        }
        if (pfd[0].revents & POLLIN) {      /* Wayland has data to read */
            wake_signal(dp);
            continue;
        }
        /* Timed out: a repeat is due.  Signal once per deadline -- signalling
         * every loop would spin until the pump advances the deadline. */
        if (active && dl != signalled && mw_now() >= dl) {
            wake_signal(dp);
            signalled = dl;
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
}
