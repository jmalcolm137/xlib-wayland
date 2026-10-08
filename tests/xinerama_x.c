/* xinerama_x.c — the libXinerama facade reports the Wayland output.
 *
 * Xinerama is the pre-RandR multi-monitor extension: clients enumerate monitors
 * with XineramaQueryScreens().  This checks the extension is present and that
 * the single screen matches the root window.
 */
#include <X11/Xlib.h>
#include <X11/extensions/Xinerama.h>

#include <stdio.h>
#include <unistd.h>

int main(void)
{
    setbuf(stdout, NULL);
    Display *d = XOpenDisplay(NULL);
    if (!d) { fprintf(stderr, "xinerama_x: no display\n"); return 2; }

    int eb = 0, er = 0;
    Bool ext = XineramaQueryExtension(d, &eb, &er);
    int maj = 0, min = 0;
    Status ver = XineramaQueryVersion(d, &maj, &min);
    Bool active = XineramaIsActive(d);

    int n = 0;
    XineramaScreenInfo *s = XineramaQueryScreens(d, &n);
    int rw = DisplayWidth(d, DefaultScreen(d));
    int rh = DisplayHeight(d, DefaultScreen(d));

    int ok = ext && ver && active && n >= 1 && s &&
             s[0].width == rw && s[0].height == rh &&
             s[0].x_org == 0 && s[0].y_org == 0;

    printf("XINERAMA:ext=%d ver=%d.%d active=%d n=%d screen0=%dx%d+%d+%d root=%dx%d\n",
           ext, maj, min, active, n,
           s ? s[0].width : 0, s ? s[0].height : 0,
           s ? s[0].x_org : 0, s ? s[0].y_org : 0, rw, rh);
    if (s) XFree(s);
    return ok ? 0 : 1;
}
