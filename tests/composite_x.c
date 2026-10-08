/* composite_x.c — the compositing/transparency path:
 * a depth-32 ARGB visual, the _NET_WM_CM_Sn claim GDK uses to decide the screen
 * is composited, alpha-preserving drawing, and the XComposite/XDamage facades.
 */
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/extensions/Xcomposite.h>
#include <X11/extensions/Xdamage.h>

#include <stdio.h>
#include <string.h>
#include <unistd.h>

int main(void)
{
    setbuf(stdout, NULL);
    Display *d = XOpenDisplay(NULL);
    if (!d) { fprintf(stderr, "composite_x: no display\n"); return 2; }

    /* The ARGB visual GTK needs (depth 32, RGB masks 0xff0000/0xff00/0xff). */
    XVisualInfo tmpl;
    memset(&tmpl, 0, sizeof tmpl);
    tmpl.depth = 32;
    int n = 0;
    XVisualInfo *list = XGetVisualInfo(d, VisualDepthMask, &tmpl, &n);
    int rgba_ok = list && n >= 1 &&
                  list[0].red_mask == 0xff0000 &&
                  list[0].green_mask == 0x00ff00 &&
                  list[0].blue_mask == 0x0000ff;

    /* The shim owns _NET_WM_CM_S0, so gdk_screen_is_composited() is true. */
    Atom cm = XInternAtom(d, "_NET_WM_CM_S0", False);
    int comp_ok = XGetSelectionOwner(d, cm) != None;

    int eb = 0, erb = 0;
    Bool composite = XCompositeQueryExtension(d, &eb, &erb);
    int cmaj = 0, cmin = 0;
    if (composite) XCompositeQueryVersion(d, &cmaj, &cmin);
    int deb = 0, derb = 0;
    Bool damage = XDamageQueryExtension(d, &deb, &derb);

    Window root = DefaultRootWindow(d);
    Colormap cmap = XCreateColormap(d, root, list[0].visual, AllocNone);
    XSetWindowAttributes a;
    memset(&a, 0, sizeof a);
    a.colormap = cmap;
    a.background_pixel = 0;
    a.event_mask = ExposureMask;
    Window w = XCreateWindow(d, root, 0, 0, 32, 32, 0, 32, InputOutput,
                             list[0].visual, CWColormap | CWBackPixel | CWEventMask,
                             &a);
    XMapWindow(d, w);
    for (;;) {
        XEvent e;
        XNextEvent(d, &e);
        if (e.type == Expose) break;
    }

    Damage dm = damage ? XDamageCreate(d, w, XDamageReportBoundingBox) : None;

    XImage *img = XCreateImage(d, list[0].visual, 32, ZPixmap, 0, NULL, 32, 32, 32, 0);
    for (int i = 0; i < 32 * 32; i++)
        ((unsigned int *)img->data)[i] = 0x80ff0000u;   /* half-alpha red */
    GC gc = XCreateGC(d, w, 0, NULL);
    XPutImage(d, w, gc, img, 0, 0, 0, 0, 32, 32);
    XFlush(d);

    XImage *back = XGetImage(d, w, 0, 0, 32, 32, AllPlanes, ZPixmap);
    int alpha = back ? (int)(((unsigned int *)back->data)[0] >> 24) : 0xff;

    int got = 0;
    for (int i = 0; i < 100 && !got; i++) {
        while (XPending(d)) {
            XEvent e;
            XNextEvent(d, &e);
            if (e.type == deb) got = 1;
        }
        if (!got) usleep(10000);
    }

    printf("COMPOSITE:rgba=%d cm=%d composite=%d xcomp=%d.%d damage=%d alpha=%02x notify=%d\n",
           rgba_ok, comp_ok, composite, cmaj, cmin, damage, alpha, got);

    return (rgba_ok && comp_ok && composite && damage && alpha < 0xff && got) ? 0 : 1;
}
