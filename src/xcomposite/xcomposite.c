/* xcomposite.c — a libXcomposite facade over the Wayland Xlib shim.
 *
 * Installed as libXcomposite.so.1.  Under Wayland there is no separate X
 * compositor to redirect windows into; the Wayland compositor blends our
 * surfaces.  The shim owns _NET_WM_CM_S0 so GDK takes its composited path for
 * RGBA windows, and the redirect/unredirect calls are therefore no-ops.
 */
#include <X11/Xlib.h>
#include <X11/extensions/Xcomposite.h>
#include <X11/extensions/Xfixes.h>

Bool XCompositeQueryExtension(Display *dpy, int *event_base_return,
                              int *error_base_return)
{
    (void)dpy;
    if (event_base_return) *event_base_return = 0;
    if (error_base_return) *error_base_return = 0;
    return True;
}

Status XCompositeQueryVersion(Display *dpy, int *major_version_return,
                              int *minor_version_return)
{
    (void)dpy;
    /* 0.4 so GDK's have_xcomposite test (major > 0 || minor >= 4) passes. */
    if (major_version_return) *major_version_return = 0;
    if (minor_version_return) *minor_version_return = 4;
    return 1;
}

int XCompositeVersion(void) { return 0x00040000; }

void XCompositeRedirectWindow(Display *dpy, Window window, int update)
{ (void)dpy; (void)window; (void)update; }

void XCompositeRedirectSubwindows(Display *dpy, Window window, int update)
{ (void)dpy; (void)window; (void)update; }

void XCompositeUnredirectWindow(Display *dpy, Window window, int update)
{ (void)dpy; (void)window; (void)update; }

void XCompositeUnredirectSubwindows(Display *dpy, Window window, int update)
{ (void)dpy; (void)window; (void)update; }

XserverRegion XCompositeCreateRegionFromBorderClip(Display *dpy, Window window)
{ (void)dpy; (void)window; return None; }

Pixmap XCompositeNameWindowPixmap(Display *dpy, Window window)
{ (void)dpy; (void)window; return None; }

/* The overlay window is where a compositor draws; GDK uses it for the XDND
 * drag window when composited.  The root window stands in for it. */
Window XCompositeGetOverlayWindow(Display *dpy, Window window)
{ (void)dpy; return window; }

void XCompositeReleaseOverlayWindow(Display *dpy, Window window)
{ (void)dpy; (void)window; }
