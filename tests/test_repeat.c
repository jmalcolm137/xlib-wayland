/* test_repeat.c — key repeat is paced by the fd handed to the client.
 *
 * libXt does not call XNextEvent() and let Xlib block; it waits in select() on
 * ConnectionNumber(dpy) -- a *macro* that reads dpy->fd -- and only calls into
 * Xlib once that fd is ready.  So key repeat can only be regular if dpy->fd
 * becomes readable at each repeat deadline.  The shim points dpy->fd at a pipe
 * its helper thread signals; before that, a held key repeated only when
 * unrelated Wayland traffic arrived and a 1.5s hold produced a handful of
 * repeats instead of the ~33 a 25/s rate implies.
 *
 * Run under the headless compositor with tests/repeat.input, which holds 'a'
 * for 1.5s.
 */
#include <X11/Xlib.h>
#include <stdio.h>
#include <sys/select.h>
#include <sys/time.h>
#include <unistd.h>

static long now_ms(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return tv.tv_sec * 1000L + tv.tv_usec / 1000;
}

int main(void)
{
    Display *d = XOpenDisplay(NULL);
    if (!d) { printf("test_repeat: no display\n"); return 1; }
    int screen = DefaultScreen(d);
    Window w = XCreateSimpleWindow(d, RootWindow(d, screen), 0, 0, 360, 260, 0,
                                   BlackPixel(d, screen), WhitePixel(d, screen));
    XSelectInput(d, w, KeyPressMask | KeyReleaseMask);
    XMapWindow(d, w);
    XFlush(d);

    /* As libXt does: wait on the connection fd itself, then drain Xlib. */
    int fd = ConnectionNumber(d);
    int presses = 0, releases = 0;
    long last = 0, max_gap = 0;
    long start = now_ms();
    while (now_ms() - start < 3200) {
        fd_set r;
        FD_ZERO(&r);
        FD_SET(fd, &r);
        struct timeval tv = { 0, 200000 };
        if (select(fd + 1, &r, NULL, NULL, &tv) < 0)
            break;
        while (XPending(d)) {
            XEvent e;
            XNextEvent(d, &e);
            if (e.type != KeyPress && e.type != KeyRelease)
                continue;
            if (e.xkey.keycode != 38)   /* evdev 30 + 8 */
                continue;
            if (e.type == KeyRelease) { releases++; continue; }
            long t = now_ms();
            /* Skip the first press->repeat gap: that is the initial delay,
             * not the repeat interval. */
            if (presses >= 2 && t - last > max_gap)
                max_gap = t - last;
            last = t;
            presses++;
        }
    }

    if (presses >= 20 && max_gap <= 120) {
        printf("all checks passed (presses=%d releases=%d max_gap=%ldms)\n",
               presses, releases, max_gap);
        return 0;
    }
    printf("FAIL: presses=%d releases=%d max_gap=%ldms\n",
           presses, releases, max_gap);
    return 1;
}
