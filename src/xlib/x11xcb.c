/* x11xcb.c — the libX11-xcb ABI on the shim.  There is no XCB transport, but
 * toolkits reach for XGetXCBConnection to drive MIT-SHM through XCB (Firefox's
 * nsShmImage does).  Hand back a connection in libxcb's error state: XCB
 * capability probes then report the extension as absent and the caller falls
 * back to the core XPutImage path.  XCB requests must not be issued against it.
 * The connect targets a locally absent display, so no X server is consulted. */
#include <X11/Xlib-xcb.h>

#include <xcb/xcb.h>

xcb_connection_t *XGetXCBConnection(Display *dpy)
{
    (void)dpy;
    static xcb_connection_t *conn;
    static int resolved;
    if (!resolved) {
        resolved = 1;
        conn = xcb_connect(NULL, NULL);
    }
    return conn;
}
