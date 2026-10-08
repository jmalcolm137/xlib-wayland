/* session.c — the X11 session protocol, relayed across shim processes.
 *
 * X sessions use two ClientMessages to a top-level window: WM_SAVE_YOURSELF
 * (save state, then republish WM_COMMAND) and WM_DELETE_WINDOW (close).  A
 * client opts in by listing the atom in WM_PROTOCOLS.  The sender is normally
 * the session manager or window manager -- one process, while every X client
 * here is its own X server, so it cannot reach them directly.
 *
 * When XLIB_WAYLAND_SESSION is set, each process listens on a Unix socket under
 * XDG_RUNTIME_DIR and a session manager relays the request by connecting and
 * writing "save" or "close"; the process then delivers the ClientMessage to its
 * own opted-in top-level windows.  (XSMP/ICE is a separate protocol and is not
 * libX11's concern.)
 */
#define _GNU_SOURCE
#include "internal.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

static bool session_enabled(Display *d)
{
    (void)d;
    const char *env = getenv("XLIB_WAYLAND_SESSION");
    return env && *env && env[0] != '0' && env[0] != 'n' && env[0] != 'N';
}

void mw_session_init(Display *d)
{
    XDisplayImpl *dp = MWD(d);
    dp->session_fd = -1;
    dp->session_path[0] = 0;
    if (!session_enabled(d)) return;

    const char *rt = getenv("XDG_RUNTIME_DIR");
    if (!rt || !*rt) rt = "/tmp";
    char dir[160];
    snprintf(dir, sizeof dir, "%s/xlib-wayland", rt);
    mkdir(dir, 0700);
    snprintf(dir, sizeof dir, "%s/xlib-wayland/session", rt);
    mkdir(dir, 0700);
    snprintf(dp->session_path, sizeof dp->session_path, "%s/%ld", dir,
             (long)getpid());
    unlink(dp->session_path);

    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (fd < 0) return;
    struct sockaddr_un sa;
    memset(&sa, 0, sizeof sa);
    sa.sun_family = AF_UNIX;
    snprintf(sa.sun_path, sizeof sa.sun_path, "%s", dp->session_path);
    if (bind(fd, (struct sockaddr *)&sa, sizeof sa) != 0 ||
        listen(fd, 8) != 0) {
        close(fd);
        unlink(dp->session_path);
        dp->session_path[0] = 0;
        return;
    }
    dp->session_fd = fd;
}

void mw_session_fini(Display *d)
{
    XDisplayImpl *dp = MWD(d);
    if (dp->session_fd >= 0) {
        close(dp->session_fd);
        dp->session_fd = -1;
    }
    if (dp->session_path[0]) {
        unlink(dp->session_path);
        dp->session_path[0] = 0;
    }
}

int mw_session_poll_fd(Display *d)
{
    return MWD(d)->session_fd;
}

/* Deliver `msg` (a WM_PROTOCOLS atom) to every mapped toplevel that listed it
 * in WM_PROTOCOLS. */
static void session_deliver(Display *d, Atom msg)
{
    Atom wmproto = mw_intern_atom(d, "WM_PROTOCOLS", False);
    MwWindow *root = mw_window(d, MWSCR(d)->root);
    if (!root) return;
    for (MwWindow *w = root->children; w; w = w->next_sib) {
        if (w->input_only || !w->mapped) continue;

        Atom type; int fmt; unsigned long n = 0, ba = 0;
        unsigned char *data = NULL;
        int ok = XGetWindowProperty(d, w->id, wmproto, 0, 32, False, XA_ATOM,
                                    &type, &fmt, &n, &ba, &data);
        if (ok != Success) continue;
        int has = 0;
        if (data) {
            for (unsigned long i = 0; i < n; i++) {
                long v = 0;
                memcpy(&v, data + i * sizeof(long), sizeof(long));
                if ((Atom)v == msg) { has = 1; break; }
            }
            XFree(data);
        }
        if (!has) continue;

        XClientMessageEvent ev;
        memset(&ev, 0, sizeof ev);
        ev.type = ClientMessage;
        ev.display = d;
        ev.window = w->id;
        ev.message_type = wmproto;
        ev.format = 32;
        ev.data.l[0] = (long)msg;
        ev.data.l[1] = (long)mw_now();
        mw_put_event(d, (XEvent *)&ev);
        if (getenv("MW_TRACE"))
            fprintf(stderr, "MW: session %s -> win 0x%lx\n",
                    msg == XInternAtom(d, "WM_DELETE_WINDOW", False)
                        ? "WM_DELETE_WINDOW" : "WM_SAVE_YOURSELF",
                    (unsigned long)w->id);
    }
}

void mw_session_handle_ready(Display *d)
{
    XDisplayImpl *dp = MWD(d);
    if (dp->session_fd < 0) return;
    int fd = accept4(dp->session_fd, NULL, NULL, SOCK_CLOEXEC | SOCK_NONBLOCK);
    if (fd < 0) return;
    char buf[64];
    ssize_t n = read(fd, buf, sizeof buf - 1);
    close(fd);
    if (n <= 0) return;
    buf[n] = 0;
    if (strncmp(buf, "save", 4) == 0)
        session_deliver(d, mw_intern_atom(d, "WM_SAVE_YOURSELF", False));
    else if (strncmp(buf, "close", 5) == 0)
        session_deliver(d, mw_intern_atom(d, "WM_DELETE_WINDOW", False));
}
