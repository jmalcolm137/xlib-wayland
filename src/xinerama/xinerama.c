/* xinerama.c — a libXinerama facade over the Wayland Xlib shim.
 *
 * Installed as libXinerama.so.1.  Xinerama is the pre-RandR multi-monitor
 * extension: clients enumerate monitors with XineramaQueryScreens().  The shim
 * derives the monitor list from the Wayland output(s) (src/xlib/xinerama.c),
 * so this just wraps it in the XineramaScreenInfo shape.
 */
#include <X11/Xlib.h>
#include <X11/extensions/Xinerama.h>

#include <stdlib.h>

extern int  mw_xinerama_count(Display *);
extern void mw_xinerama_screen(Display *, int, int *, int *, int *, int *);

Bool XineramaQueryExtension(Display *dpy, int *event_base, int *error_base)
{
    (void)dpy;
    if (event_base) *event_base = 0;
    if (error_base) *error_base = 0;
    return True;
}

Status XineramaQueryVersion(Display *dpy, int *major, int *minor)
{
    (void)dpy;
    if (major) *major = 1;
    if (minor) *minor = 1;
    return 1;
}

Bool XineramaIsActive(Display *dpy)
{
    return mw_xinerama_count(dpy) > 0;
}

XineramaScreenInfo *XineramaQueryScreens(Display *dpy, int *number)
{
    int n = mw_xinerama_count(dpy);
    if (number) *number = 0;
    if (n <= 0) return NULL;
    XineramaScreenInfo *s = calloc((size_t)n, sizeof *s);
    if (!s) return NULL;
    for (int i = 0; i < n; i++) {
        int x = 0, y = 0, w = 0, h = 0;
        mw_xinerama_screen(dpy, i, &x, &y, &w, &h);
        s[i].screen_number = i;
        s[i].x_org  = (short)x;
        s[i].y_org  = (short)y;
        s[i].width  = (short)w;
        s[i].height = (short)h;
    }
    if (number) *number = n;
    return s;
}
