/* test_core.c — unit tests for the parts that don't need a compositor. */
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/Xatom.h>
#include <X11/Xresource.h>
#include <X11/keysym.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static int failures;

#define CHECK(cond) do { if (!(cond)) { \
    fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

int main(void)
{
    /* Regions (pure, no server). */
    Region r1 = XCreateRegion();
    Region r2 = XCreateRegion();
    Region ri = XCreateRegion();
    XRectangle rect = { 10, 10, 100, 100 };
    XUnionRectWithRegion(&rect, r1, r1);
    rect = (XRectangle){ 50, 50, 100, 100 };
    XUnionRectWithRegion(&rect, r2, r2);
    XIntersectRegion(r1, r2, ri);
    CHECK(XPointInRegion(ri, 60, 60));
    CHECK(!XPointInRegion(ri, 20, 20));
    CHECK(XRectInRegion(ri, 60, 60, 10, 10) == RectangleIn);
    XRectangle box;
    XClipBox(ri, &box);
    CHECK(box.x == 50 && box.y == 50 && box.width == 60 && box.height == 60);
    CHECK(XEqualRegion(r1, r1));
    XDestroyRegion(r1); XDestroyRegion(r2); XDestroyRegion(ri);

    /* Geometry. */
    int x, y; unsigned int w, h;
    int m = XParseGeometry("=800x600+10-20", &x, &y, &w, &h);
    CHECK((m & WidthValue) && w == 800 && h == 600);
    CHECK((m & XValue) && x == 10);
    CHECK((m & YNegative) && y == 20);

    /* Keysyms. */
    CHECK(XStringToKeysym("Return") == XK_Return);
    CHECK(XStringToKeysym("a") == XK_a);
    CHECK(strcmp(XKeysymToString(XK_Escape), "Escape") == 0);
    KeySym lo, up;
    XConvertCase(XK_A, &lo, &up);
    CHECK(lo == XK_a && up == XK_A);

    /* Colour parsing. */
    XColor c;
    XrmInitialize();
    CHECK(XParseColor(NULL, 0, "#ff0000", &c) && c.red == 0xffff);
    CHECK(XParseColor(NULL, 0, "black", &c) && c.red == 0 && c.green == 0);
    CHECK(XParseColor(NULL, 0, "white", &c) && c.blue == 0xffff);
    XColor screen;
    XAllocColor(NULL, 0, &c);
    CHECK(XAllocNamedColor(NULL, 0, "red", &screen, &c));

    /* Xrm database. */
    XrmDatabase db = XrmGetStringDatabase(
        "App*background: #ffffff\nApp.window.width: 640\n"
        "App.window.title: Hello\n");
    CHECK(db != NULL);
    char *type = NULL;
    XrmValue value;
    if (XrmGetResource(db, "app.window.width", "App.Window.Width",
                       &type, &value)) {
        CHECK(value.addr != NULL);
        CHECK(strcmp((char *)value.addr, "640") == 0);
    } else {
        CHECK(0 && "XrmGetResource should find app.window.width");
    }
    XrmDestroyDatabase(db);

    /* XImage pixel round-trip. */
    XImage *img = XCreateImage(NULL, NULL, 24, ZPixmap, 0, NULL, 8, 4, 32, 0);
    CHECK(img != NULL);
    CHECK(img->width == 8 && img->height == 4);
    XPutPixel(img, 3, 2, 0x123456);
    CHECK(XGetPixel(img, 3, 2) == 0x123456);
    CHECK(XSubImage(img, 1, 1, 4, 2) != NULL);
    XDestroyImage(img);

    /* Atoms/properties need a display; skip if none. */
    Display *d = XOpenDisplay(NULL);
    if (d) {
        Atom a = XInternAtom(d, "TEST_ATOM", False);
        CHECK(a != None);
        CHECK(strcmp(XGetAtomName(d, a), "TEST_ATOM") == 0);
        CHECK(XInternAtom(d, "TEST_ATOM", True) == a);
        CHECK(XInternAtom(d, "NO_SUCH_ATOM_XYZ", True) == None);
        CHECK(XInternAtom(d, "PRIMARY", True) == XA_PRIMARY);
        XFree(XGetAtomName(d, a));

        Window root = DefaultRootWindow(d);
        const char *val = "hello";
        XChangeProperty(d, root, a, XA_STRING, 8, PropModeReplace,
                        (const unsigned char *)val, 5);
        Atom at; int af; unsigned long ni, ba; unsigned char *data = NULL;
        XGetWindowProperty(d, root, a, 0, 100, False, XA_STRING, &at, &af,
                           &ni, &ba, &data);
        CHECK(ni == 5 && data && memcmp(data, "hello", 5) == 0);
        XFree(data);

        /* Visual/screen sanity. */
        CHECK(DefaultDepth(d, 0) == 24);
        CHECK(DefaultVisual(d, 0)->class == TrueColor);

        /* Stacking changes.  XRaiseWindow used to read win->parent after
         * unlink_child() had already cleared it, so every call dereferenced
         * NULL -- and NEdit raises a document window the moment a file has
         * been loaded, which killed the application. */
        {
            Window p = XCreateSimpleWindow(d, root, 0, 0, 100, 100, 0, 0, 0);
            Window c1 = XCreateSimpleWindow(d, p, 0, 0, 10, 10, 0, 0, 0);
            Window c2 = XCreateSimpleWindow(d, p, 0, 0, 10, 10, 0, 0, 0);
            Window rr; int rx, ry; unsigned int rw, rh, rb, rd;
            XMapWindow(d, p);
            XMapWindow(d, c1);
            XMapWindow(d, c2);
            CHECK(XRaiseWindow(d, c1) == 1);
            CHECK(XLowerWindow(d, c1) == 1);
            CHECK(XRaiseWindow(d, c2) == 1);
            CHECK(XMoveResizeWindow(d, c2, 5, 6, 30, 40) == 1);
            CHECK(XGetGeometry(d, c2, &rr, &rx, &ry, &rw, &rh, &rb, &rd));
            CHECK(rx == 5 && ry == 6 && rw == 30 && rh == 40);
            XDestroyWindow(d, p);
        }

        XCloseDisplay(d);
    } else {
        printf("note: no display; skipping display-dependent checks\n");
    }

    if (failures == 0) { printf("test_core: all checks passed\n"); return 0; }
    fprintf(stderr, "test_core: %d failure(s)\n", failures);
    return 1;
}
