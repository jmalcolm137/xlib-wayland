/* x11xcb.c — the libX11-xcb ABI on the shim.  There is no XCB transport, but
 * toolkits reach for XGetXCBConnection to drive MIT-SHM through XCB (Firefox's
 * nsShmImage does).  Hand back a connection in libxcb's error state: XCB
 * capability probes then report the extension as absent and the caller falls
 * back to the core XPutImage path.  XCB requests must not be issued against it.
 * The connection is made on an invalid fd, so no display name is parsed and no
 * socket (local or TCP) is ever opened. */
#include <X11/Xlib-xcb.h>

#include <pthread.h>
#include <xcb/xcb.h>

static xcb_connection_t *conn;
static pthread_once_t conn_once = PTHREAD_ONCE_INIT;

static void conn_init(void)
{
    conn = xcb_connect_to_fd(-1, NULL);
}

xcb_connection_t *XGetXCBConnection(Display *dpy)
{
    (void)dpy;
    pthread_once(&conn_once, conn_init);
    return conn;
}
