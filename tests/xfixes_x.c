/* xfixes_x.c — exercise the libXfixes and libXcursor facades.
 *
 * Registers an XFixes selection-input interest and checks a
 * XFixesSelectionNotify arrives when the owner changes (what GDK uses to keep
 * the paste UI in sync), then loads a themed cursor by shape and an image
 * cursor through libXcursor.
 */
#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <X11/cursorfont.h>
#include <X11/extensions/Xfixes.h>
#include <X11/Xcursor/Xcursor.h>

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

int main(void)
{
    setbuf(stdout, NULL);
    Display *d = XOpenDisplay(NULL);
    if (!d) { fprintf(stderr, "xfixes_x: no display\n"); return 2; }

    int base = 0, eb = 0;
    Bool ok = XFixesQueryExtension(d, &base, &eb);
    printf("XFIXES:QUERY ok=%d base=%d\n", ok, base);

    int scr = DefaultScreen(d);
    Window w = XCreateSimpleWindow(d, RootWindow(d, scr), 0, 0, 100, 100, 0, 0, 0);
    XSelectInput(d, w, StructureNotifyMask);
    XMapWindow(d, w);
    XFlush(d);

    /* A private selection, so this does not disturb the clipboard bridge. */
    Atom sel = XInternAtom(d, "_MW_XFIXES_TEST", False);
    XFixesSelectSelectionInput(d, w, sel,
                               XFixesSetSelectionOwnerNotifyMask |
                               XFixesSelectionWindowDestroyNotifyMask |
                               XFixesSelectionClientCloseNotifyMask);
    XFlush(d);

    XSetSelectionOwner(d, sel, w, CurrentTime);
    XFlush(d);

    int got = 0;
    for (int i = 0; i < 200 && !got; i++) {
        while (XPending(d)) {
            XEvent e;
            XNextEvent(d, &e);
            if (e.type == base) {
                XFixesSelectionNotifyEvent *sn = (XFixesSelectionNotifyEvent *)&e;
                printf("XFIXES:GOT sel=%lu owner=0x%lx subtype=%d\n",
                       (unsigned long)sn->selection, (unsigned long)sn->owner,
                       sn->subtype);
                got = 1;
            }
        }
        if (!got) usleep(10000);
    }
    if (!got) printf("XFIXES:NONE\n");

    /* Xcursor: settings, a themed cursor by shape, and an image cursor. */
    int size = XcursorGetDefaultSize(d);
    char *theme = XcursorGetTheme(d);
    XcursorSetDefaultSize(d, 32);
    XcursorSetTheme(d, "Adwaita");
    Cursor shape = XcursorShapeLoadCursor(d, XC_left_ptr);

    XcursorImage *img = XcursorImageCreate(4, 4);
    Cursor image = None;
    if (img) {
        for (int i = 0; i < 16; i++) img->pixels[i] = 0xffff3000u;
        img->xhot = 1; img->yhot = 1;
        image = XcursorImageLoadCursor(d, img);
        XcursorImageDestroy(img);
    }
    printf("XCURSOR:size=%d theme=%s shape=%ld image=%ld argb=%d\n",
           size, theme ? theme : "(null)", (long)shape, (long)image,
           XcursorSupportsARGB(d));
    free(theme);

    XCloseDisplay(d);
    return (ok && got && shape != None && image != None) ? 0 : 1;
}
