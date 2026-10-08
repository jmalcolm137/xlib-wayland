/* xinerama.c — the monitor list behind the libXinerama facade.
 *
 * Xinerama exposes each monitor's geometry in root-window coordinates.  The
 * shim derives its screen from the Wayland wl_output, and tracks one output, so
 * this reports a single screen covering the root.  (The libXrandr facade
 * reports the same output; RandR is the modern channel and GDK prefers it, but
 * old clients call XineramaQueryScreens directly.)
 */
#include "internal.h"

int mw_xinerama_count(Display *d)
{
    (void)d;
    return 1;
}

void mw_xinerama_screen(Display *d, int i, int *x, int *y, int *w, int *h)
{
    if (i != 0) return;
    Screen *scr = MWSCR(d);
    if (x) *x = 0;
    if (y) *y = 0;
    if (w) *w = scr->width  > 0 ? scr->width  : MW_DEFAULT_W;
    if (h) *h = scr->height > 0 ? scr->height : MW_DEFAULT_H;
}
