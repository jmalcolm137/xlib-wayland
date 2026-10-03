/* test_draw.c — exercise the Motif/Wayland Xlib shim directly.
 *
 * Not an Xt/Motif program: this drives the Xlib API the same way a toolkit
 * would, so it validates display setup, window tree, mapping, GCs, drawing,
 * XImage, fonts and events.  Draws for a fixed time then exits 0.
 */
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/Xatom.h>
#include <X11/keysym.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/time.h>

static double now_ms(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return tv.tv_sec * 1000.0 + tv.tv_usec / 1000.0;
}

static void draw(Display *d, Window w, GC gc, int W, int H)
{
    XSetForeground(d, gc, WhitePixel(d, 0));
    XFillRectangle(d, w, gc, 0, 0, W, H);

    XSetForeground(d, gc, BlackPixel(d, 0));
    XDrawRectangle(d, w, gc, 10, 10, W - 20, H - 20);

    XSetForeground(d, gc, 0xcc0000);
    XFillRectangle(d, w, gc, 30, 30, 120, 60);

    XSetForeground(d, gc, 0x0000cc);
    XFillArc(d, w, gc, 200, 30, 130, 90, 0, 360 * 64);
    XDrawArc(d, w, gc, 200, 30, 130, 90, 45 * 64, 270 * 64);

    XSetForeground(d, gc, 0x008800);
    XPoint poly[3] = { { 350, 120 }, { 460, 120 }, { 405, 40 } };
    XFillPolygon(d, w, gc, poly, 3, Convex, CoordModeOrigin);

    XSetForeground(d, gc, 0x000000);
    const char *msg = "Motif on Wayland — Xlib shim";
    XDrawString(d, w, gc, 30, 150, msg, (int)strlen(msg));

    /* An XImage blit (XV's main pixel path). */
    int iw = 200, ih = 60;
    XImage *img = XCreateImage(d, DefaultVisual(d, 0), 24, ZPixmap, 0,
                               NULL, iw, ih, 32, 0);
    for (int y = 0; y < ih; y++)
        for (int x = 0; x < iw; x++) {
            unsigned long r = (x * 255 / iw), g = (y * 255 / ih), b = 128;
            XPutPixel(img, x, y, (r << 16) | (g << 8) | b);
        }
    XPutImage(d, w, gc, img, 0, 0, 30, 180, iw, ih);
    XDestroyImage(img);

    XSetForeground(d, gc, 0x444444);
    XDrawLine(d, w, gc, 30, 260, W - 30, 260);
    XFlush(d);
}

int main(int argc, char **argv)
{
    double seconds = argc > 1 ? atof(argv[1]) : 2.0;

    Display *d = XOpenDisplay(NULL);
    if (!d) { fprintf(stderr, "cannot open display\n"); return 1; }

    int scr = DefaultScreen(d);
    printf("vendor       : %s\n", ServerVendor(d));
    printf("protocol     : %d.%d\n", ProtocolVersion(d), ProtocolRevision(d));
    printf("screens      : %d\n", ScreenCount(d));
    printf("screen size  : %dx%d depth %d\n", DisplayWidth(d, scr),
           DisplayHeight(d, scr), DefaultDepth(d, scr));
    Visual *v = DefaultVisual(d, scr);
    printf("visual       : id=0x%lx class=%d masks %lx/%lx/%lx\n",
           XVisualIDFromVisual(v), v->class, v->red_mask, v->green_mask,
           v->blue_mask);

    Atom a = XInternAtom(d, "MOTIF_WAYLAND_TEST", False);
    char *an = XGetAtomName(d, a);
    printf("atom roundtrip: %lu -> %s\n", a, an);
    XFree(an);

    int W = 500, H = 300;
    Window w = XCreateSimpleWindow(d, RootWindow(d, scr), 0, 0, W, H, 1,
                                   BlackPixel(d, scr), WhitePixel(d, scr));
    XStoreName(d, w, "Motif/Wayland test");
    XSelectInput(d, w, ExposureMask | ButtonPressMask | KeyPressMask |
                         StructureNotifyMask | PointerMotionMask);
    XMapWindow(d, w);

    GC gc = XCreateGC(d, w, 0, NULL);
    XSetLineAttributes(d, gc, 1, LineSolid, CapButt, JoinMiter);
    XSetFont(d, gc, XLoadFont(d, "fixed"));

    double start = now_ms();
    int exposes = 0, others = 0;
    while ((now_ms() - start) < seconds * 1000.0) {
        while (XPending(d)) {
            XEvent e;
            XNextEvent(d, &e);
            switch (e.type) {
            case Expose:
                exposes++;
                draw(d, w, gc, W, H);
                break;
            case ConfigureNotify:
                printf("ConfigureNotify %dx%d\n", e.xconfigure.width, e.xconfigure.height);
                break;
            case ButtonPress:
                printf("ButtonPress button=%u at %d,%d\n", e.xbutton.button,
                       e.xbutton.x, e.xbutton.y);
                break;
            case KeyPress: {
                char buf[32];
                KeySym ks;
                int n = XLookupString(&e.xkey, buf, sizeof buf, &ks, NULL);
                printf("KeyPress keysym=0x%lx str=%.*s\n", ks, n, buf);
                break;
            }
            case MotionNotify:
                others++;
                break;
            default:
                others++;
                break;
            }
        }
        usleep(5000);
    }

    printf("events: %d exposes, %d others\n", exposes, others);
    XFreeGC(d, gc);
    XDestroyWindow(d, w);
    XCloseDisplay(d);
    printf("OK\n");
    return 0;
}
