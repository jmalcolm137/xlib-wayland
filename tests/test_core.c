/* test_core.c — unit tests for the parts that don't need a compositor. */
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/Xatom.h>
#include <X11/Xresource.h>
#include <X11/keysym.h>
#include <X11/extensions/shape.h>
#include <wchar.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>

static int failures;

#define CHECK(cond) do { if (!(cond)) { \
    fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

/* ------------------------------------------------------ Xrm and keysyms */

static void write_file(const char *path, const char *text)
{
    FILE *f = fopen(path, "w");
    if (f) { fputs(text, f); fclose(f); }
}

static void check_xrm(void)
{
    /* XrmQGetSearchList asks for a bigger array when its own is too small
     * (Xt grows it and retries), and the list then answers lookups. */
    XrmDatabase db = XrmGetStringDatabase("*Form.background: blue\n"
                                          "app.form.foreground: green\n");
    XrmQuark names[3] = { XrmStringToQuark("app"), XrmStringToQuark("form"), NULLQUARK };
    XrmQuark classes[3] = { XrmStringToQuark("App"), XrmStringToQuark("Form"), NULLQUARK };
    XrmHashTable small[2], big[16];
    CHECK(!XrmQGetSearchList(db, names, classes, small, 2));
    CHECK(XrmQGetSearchList(db, names, classes, big, 16));
    XrmRepresentation type;
    XrmValue v;
    CHECK(XrmQGetSearchResource(big, XrmStringToQuark("background"),
                                XrmStringToQuark("Background"), &type, &v) &&
          v.addr && !strcmp((char *)v.addr, "blue"));
    CHECK(XrmQGetSearchResource(big, XrmStringToQuark("foreground"),
                                XrmStringToQuark("Foreground"), &type, &v) &&
          v.addr && !strcmp((char *)v.addr, "green"));
    CHECK(!XrmQGetSearchResource(big, XrmStringToQuark("font"),
                                 XrmStringToQuark("Font"), &type, &v));
    /* Many queries must not accumulate anything (LeakSanitizer checks). */
    for (int i = 0; i < 1000; i++)
        XrmQGetSearchList(db, names, classes, big, 16);
    XrmDestroyDatabase(db);

    /* #include "file", relative to the including file's directory. */
    char dir[] = "/tmp/mw-xrm-XXXXXX";
    if (mkdtemp(dir)) {
        char a[64], b[64];
        snprintf(a, sizeof a, "%s/main", dir);
        snprintf(b, sizeof b, "%s/base", dir);
        write_file(b, "XTerm*scrollBar: true\n");
        write_file(a, "#include \"base\"\nXTerm*saveLines: 500\n");
        db = XrmGetFileDatabase(a);
        char *t = NULL;
        CHECK(db && XrmGetResource(db, "xterm.vt100.scrollBar",
                                   "XTerm.VT100.ScrollBar", &t, &v) &&
              !strcmp((char *)v.addr, "true"));
        CHECK(db && XrmGetResource(db, "xterm.saveLines", "XTerm.SaveLines",
                                   &t, &v) && !strcmp((char *)v.addr, "500"));
        if (db) XrmDestroyDatabase(db);
        unlink(a); unlink(b); rmdir(dir);
    }

    /* Unicode and hex keysym names. */
    CHECK(XStringToKeysym("U20AC") == 0x10020ac);
    CHECK(XStringToKeysym("U00E9") == XK_eacute);
    CHECK(XStringToKeysym("0x1008ff13") == 0x1008ff13);
    CHECK(XStringToKeysym("Uxyz") == NoSymbol);
    const char *ks = XKeysymToString(0x10020ac);
    CHECK(ks && !strcmp(ks, "U20AC"));
    CHECK(XKeysymToString(0x10020ac) == ks);         /* the same, stable */
}

/* ------------------------------------------------ display-dependent checks */

static Bool any_event(Display *d, XEvent *e, XPointer a)
{ (void)d; (void)e; (void)a; return True; }

static void drain(Display *d)
{
    XEvent ev;
    XSync(d, False);
    while (XCheckIfEvent(d, &ev, any_event, NULL)) {}
}

static unsigned long pix(XImage *im, int x, int y)
{ return XGetPixel(im, x, y) & 0xffffff; }

static void check_properties(Display *d, Window win)
{
    Atom at; int af; unsigned long ni, ba; unsigned char *data = NULL;
    Atom p = XInternAtom(d, "MW_TEST_PROP", False);

    /* WM_COMMAND holds the strings, not the argv pointer array. */
    char *argv[] = { "prog", "-x", "a b" };
    XSetCommand(d, win, argv, 3);
    XGetWindowProperty(d, win, XA_WM_COMMAND, 0, 100, False, XA_STRING,
                       &at, &af, &ni, &ba, &data);
    CHECK(ni == 12 && data && memcmp(data, "prog\0-x\0a b\0", 12) == 0);
    XFree(data); data = NULL;
    char **rargv = NULL; int rargc = 0;
    CHECK(XGetCommand(d, win, &rargv, &rargc) && rargc == 3);
    if (rargc == 3)
        CHECK(!strcmp(rargv[0], "prog") && !strcmp(rargv[2], "a b"));
    if (rargv) XFreeStringList(rargv);

    /* A long WM_CLASS name used to overflow a 1 KiB stack buffer. */
    char longname[3000];
    memset(longname, 'n', sizeof longname - 1);
    longname[sizeof longname - 1] = 0;
    XClassHint ch = { longname, "Cls" }, rch = { NULL, NULL };
    XSetClassHint(d, win, &ch);
    CHECK(XGetClassHint(d, win, &rch));
    CHECK(rch.res_name && strlen(rch.res_name) == sizeof longname - 1);
    CHECK(rch.res_class && !strcmp(rch.res_class, "Cls"));
    XFree(rch.res_name); XFree(rch.res_class);
    /* Only one string, without its NUL: the class is empty. */
    XChangeProperty(d, win, XA_WM_CLASS, XA_STRING, 8, PropModeReplace,
                    (const unsigned char *)"only", 4);
    CHECK(XGetClassHint(d, win, &rch));
    CHECK(rch.res_name && !strcmp(rch.res_name, "only"));
    CHECK(rch.res_class && !strcmp(rch.res_class, ""));
    XFree(rch.res_name); XFree(rch.res_class);

    /* long_length 0 probes the size: no data, the size in bytes_after. */
    XChangeProperty(d, win, p, XA_STRING, 8, PropModeReplace,
                    (const unsigned char *)"hello", 5);
    XGetWindowProperty(d, win, p, 0, 0, False, AnyPropertyType,
                       &at, &af, &ni, &ba, &data);
    CHECK(ni == 0 && ba == 5 && af == 8 && at == XA_STRING);
    XFree(data); data = NULL;

    /* Appending another format is BadMatch: the property is unchanged. */
    long l = 7;
    XChangeProperty(d, win, p, XA_STRING, 32, PropModeAppend,
                    (const unsigned char *)&l, 1);
    XGetWindowProperty(d, win, p, 0, 100, False, AnyPropertyType,
                       &at, &af, &ni, &ba, &data);
    CHECK(af == 8 && ni == 5 && data && !memcmp(data, "hello", 5));
    XFree(data); data = NULL;

    /* delete only takes effect once everything has been read, and then
     * reports PropertyDelete. */
    XSelectInput(d, win, PropertyChangeMask);
    XChangeProperty(d, win, p, XA_STRING, 8, PropModeReplace,
                    (const unsigned char *)"abcdefgh", 8);
    drain(d);
    XGetWindowProperty(d, win, p, 0, 1, True, AnyPropertyType,
                       &at, &af, &ni, &ba, &data);
    CHECK(ni == 4 && ba == 4);
    XFree(data); data = NULL;
    XEvent ev;
    CHECK(!XCheckTypedWindowEvent(d, win, PropertyNotify, &ev));
    XGetWindowProperty(d, win, p, 1, 1, True, AnyPropertyType,
                       &at, &af, &ni, &ba, &data);
    CHECK(ni == 4 && ba == 0 && data && !memcmp(data, "efgh", 4));
    XFree(data); data = NULL;
    CHECK(XCheckTypedWindowEvent(d, win, PropertyNotify, &ev) &&
          ev.xproperty.state == PropertyDelete);
    XGetWindowProperty(d, win, p, 0, 100, False, AnyPropertyType,
                       &at, &af, &ni, &ba, &data);
    CHECK(at == None && ni == 0);
    XFree(data);
    XSelectInput(d, win, 0);
}

static void check_text_apis(Display *d, Window win)
{
    /* XListFonts' result is NULL-terminated for XFreeFontNames. */
    int n = 0;
    char **names = XListFonts(d, "*", 100, &n);
    CHECK(names && n > 0 && names[n] == NULL);
    XFreeFontNames(names);

    /* The overall ink/logical returns are single rectangles; a too-small
     * per-character buffer fails with 0 (writing them per character used to
     * overrun the caller's struct). */
    XRectangle ink[2], logical[2], all_ink, all_logical;
    int num = 0;
    int ok = XmbTextPerCharExtents(NULL, "abcd", 4, ink, logical, 2, &num,
                                   &all_ink, &all_logical);
    CHECK(ok == 0 && num == 4);
    CHECK(all_logical.width == all_ink.width && all_ink.width > 0);
    ok = XmbTextPerCharExtents(NULL, "ab", 2, NULL, logical, 2, &num,
                               &all_ink, &all_logical);
    CHECK(ok == 1 && num == 2 && logical[1].x == (short)logical[0].width);

    /* XShapeGetRectangles hands back a copy; it used to free the window's
     * own shape, so the second call (and the destroy) touched freed memory. */
    XRectangle r = { 0, 0, 20, 10 };
    XShapeCombineRectangles(d, win, ShapeBounding, 0, 0, &r, 1, ShapeSet,
                            Unsorted);
    int cnt = 0, ord = 0;
    for (int i = 0; i < 2; i++) {
        XRectangle *rs = XShapeGetRectangles(d, win, ShapeBounding, &cnt, &ord);
        CHECK(cnt == 1 && rs && rs[0].width == 20 && rs[0].height == 10);
        XFree(rs);
    }

    /* XwcLookupString reports a short buffer instead of truncating. */
    KeyCode kc = XKeysymToKeycode(d, XK_a);
    if (kc) {
        XKeyEvent ke = { 0 };
        ke.type = KeyPress; ke.display = d; ke.window = win; ke.keycode = kc;
        wchar_t wbuf[4];
        KeySym ks; Status st;
        int r1 = XwcLookupString(NULL, &ke, wbuf, 0, &ks, &st);
        CHECK(r1 == 1 && st == XBufferOverflow);
        r1 = XwcLookupString(NULL, &ke, wbuf, 4, &ks, &st);
        CHECK(r1 == 1 && wbuf[0] == L'a' && st == XLookupBoth && ks == XK_a);
        char mb[8];
        r1 = Xutf8LookupString(NULL, &ke, mb, 0, &ks, &st);
        CHECK(r1 == 1 && st == XBufferOverflow);
    }
}

static void check_drawing(Display *d, Window root)
{
    Pixmap pm = XCreatePixmap(d, root, 40, 40, 24);
    GC gc = XCreateGC(d, pm, 0, NULL);
    XSetForeground(d, gc, 0xffffff);
    XFillRectangle(d, pm, gc, 0, 0, 40, 40);
    XSetForeground(d, gc, 0x000000);

    /* An outline arc is just the arc: the GC's default arc_mode (PieSlice)
     * only applies to fills, so no radius line through the centre. */
    XDrawArc(d, pm, gc, 5, 5, 30, 30, 0, 360 * 64);
    /* XDrawArcs: each arc its own sub-path, no line joining them. */
    XArc arcs[2] = { { 2, 2, 8, 8, 0, 180 * 64 }, { 30, 30, 8, 8, 0, 180 * 64 } };
    XDrawArcs(d, pm, gc, arcs, 2);
    XImage *im = XGetImage(d, pm, 0, 0, 40, 40, AllPlanes, ZPixmap);
    CHECK(im != NULL);
    if (im) {
        CHECK(pix(im, 20, 20) == 0xffffff);      /* centre: no radius */
        CHECK(pix(im, 28, 20) == 0xffffff);      /* along the radius */
        CHECK(pix(im, 16, 24) == 0xffffff);      /* between the two arcs */
        int dark = 0;
        for (int x = 3; x <= 7; x++) dark += pix(im, x, 20) < 0x808080;
        CHECK(dark > 0);                          /* the circle itself */
        XDestroyImage(im);
    }

    /* X angles run counter-clockwise from 3 o'clock: a 0..90 degree pie is
     * the top-right quadrant (the arc used to come out as its complement). */
    XSetForeground(d, gc, 0xffffff);
    XFillRectangle(d, pm, gc, 0, 0, 40, 40);
    XSetForeground(d, gc, 0x000000);
    XFillArc(d, pm, gc, 0, 0, 40, 40, 0, 90 * 64);
    im = XGetImage(d, pm, 0, 0, 40, 40, AllPlanes, ZPixmap);
    if (im) {
        CHECK(pix(im, 28, 12) == 0x000000);      /* top right: inside */
        CHECK(pix(im, 12, 28) == 0xffffff);      /* bottom left: outside */
        CHECK(pix(im, 12, 12) == 0xffffff);      /* top left: outside */
        XDestroyImage(im);
    }
    XFreeGC(d, gc);
    XFreePixmap(d, pm);
}

static void check_fonts(Display *d, Window root)
{
    Pixmap pm = XCreatePixmap(d, root, 60, 20, 24);

    /* GCs created without a font share one default font. */
    GC g1 = XCreateGC(d, pm, 0, NULL), g2 = XCreateGC(d, pm, 0, NULL);
    XGCValues v1, v2;
    XGetGCValues(d, g1, GCFont, &v1);
    XGetGCValues(d, g2, GCFont, &v2);
    CHECK(v1.font != None && v1.font == v2.font);

    /* Freeing a font a GC still uses is legal X: the GC keeps drawing with
     * it (this used to draw through freed memory). */
    XFontStruct *fs = XLoadQueryFont(d, "fixed");
    CHECK(fs != NULL);
    if (fs) {
        Font fid = fs->fid;
        XSetFont(d, g1, fid);
        XCopyGC(d, g1, GCFont, g2);          /* GCFont is copied too */
        XGetGCValues(d, g2, GCFont, &v2);
        CHECK(v2.font == fid);
        XFreeFont(d, fs);
        CHECK(XQueryFont(d, fid) == NULL);   /* the ID is gone ... */
        XSetForeground(d, g1, 0xffffff);
        XFillRectangle(d, pm, g1, 0, 0, 60, 20);
        XSetForeground(d, g1, 0);
        XDrawString(d, pm, g1, 2, 14, "Hi", 2);      /* ... the font is not */
        XDrawImageString(d, pm, g2, 20, 14, "Hi", 2);
        XImage *im = XGetImage(d, pm, 0, 0, 60, 20, AllPlanes, ZPixmap);
        int dark = 0;
        for (int y = 0; im && y < 20; y++)
            for (int x = 0; x < 18; x++) dark += pix(im, x, y) < 0x808080;
        CHECK(dark > 0);
        if (im) XDestroyImage(im);
    }
    XFreeGC(d, g1);
    XFreeGC(d, g2);
    XFreePixmap(d, pm);
}

/* ---------------------------------------------- core drawing semantics */

static Pixmap filled_pixmap(Display *d, Window root, int w, int h,
                            unsigned long pixel)
{
    Pixmap pm = XCreatePixmap(d, root, w, h, 24);
    GC gc = XCreateGC(d, pm, 0, NULL);
    XSetForeground(d, gc, pixel);
    XFillRectangle(d, pm, gc, 0, 0, w, h);
    XFreeGC(d, gc);
    return pm;
}

static unsigned long pixel_at(Display *d, Drawable dr, int x, int y)
{
    XImage *im = XGetImage(d, dr, x, y, 1, 1, AllPlanes, ZPixmap);
    if (!im) return 0xdeadbeef;
    unsigned long v = pix(im, 0, 0);
    XDestroyImage(im);
    return v;
}

static void check_raster_ops(Display *d, Window root)
{
    Pixmap pm = filled_pixmap(d, root, 20, 20, 0x123456);
    GC gc = XCreateGC(d, pm, 0, NULL);

    /* GXxor is bitwise: applying it twice restores the pixels (cairo's XOR
     * operator is Porter-Duff and made opaque pixels transparent). */
    XSetFunction(d, gc, GXxor);
    XSetForeground(d, gc, 0xffffff);
    XFillRectangle(d, pm, gc, 0, 0, 10, 10);
    CHECK(pixel_at(d, pm, 5, 5) == 0xedcba9);
    CHECK(pixel_at(d, pm, 15, 15) == 0x123456);
    XFillRectangle(d, pm, gc, 0, 0, 10, 10);
    CHECK(pixel_at(d, pm, 5, 5) == 0x123456);

    /* A XOR line drawn twice leaves nothing behind (rubber banding). */
    XDrawLine(d, pm, gc, 0, 15, 19, 15);
    CHECK(pixel_at(d, pm, 10, 15) == 0xedcba9);
    XDrawLine(d, pm, gc, 0, 15, 19, 15);
    CHECK(pixel_at(d, pm, 10, 15) == 0x123456);

    /* GXand, GXor, GXnoop, GXequiv. */
    XSetFunction(d, gc, GXand);
    XSetForeground(d, gc, 0x0ff00f);
    XFillRectangle(d, pm, gc, 0, 0, 2, 2);
    CHECK(pixel_at(d, pm, 0, 0) == (0x123456 & 0x0ff00f));
    XSetFunction(d, gc, GXor);
    XSetForeground(d, gc, 0x800000);
    XFillRectangle(d, pm, gc, 2, 0, 2, 2);
    CHECK(pixel_at(d, pm, 2, 0) == (0x123456 | 0x800000));
    XSetFunction(d, gc, GXnoop);
    XFillRectangle(d, pm, gc, 4, 0, 2, 2);
    CHECK(pixel_at(d, pm, 4, 0) == 0x123456);
    XSetFunction(d, gc, GXequiv);
    XSetForeground(d, gc, 0x00ff00);
    XFillRectangle(d, pm, gc, 6, 0, 2, 2);
    CHECK(pixel_at(d, pm, 6, 0) == ((~(0x123456 ^ 0x00ff00)) & 0xffffff));

    /* GXinvert inverts the path, not its bounding box. */
    XSetFunction(d, gc, GXinvert);
    XPoint tri[3] = { { 10, 10 }, { 19, 10 }, { 10, 19 } };
    XFillPolygon(d, pm, gc, tri, 3, Convex, CoordModeOrigin);
    CHECK(pixel_at(d, pm, 11, 11) == (~0x123456 & 0xffffff));
    CHECK(pixel_at(d, pm, 18, 18) == 0x123456);

    /* The plane mask limits which bits change. */
    XSetFunction(d, gc, GXcopy);
    XSetPlaneMask(d, gc, 0x0000ff);
    XSetForeground(d, gc, 0xffffff);
    XFillRectangle(d, pm, gc, 0, 19, 1, 1);
    CHECK(pixel_at(d, pm, 0, 19) == 0x1234ff);
    XSetPlaneMask(d, gc, AllPlanes);

    /* XCopyArea with GXxor combines source and destination. */
    Pixmap src = filled_pixmap(d, root, 4, 4, 0x00ff00);
    XSetFunction(d, gc, GXxor);
    XCopyArea(d, src, pm, gc, 0, 0, 4, 4, 16, 0);
    CHECK(pixel_at(d, pm, 17, 1) == (0x123456 ^ 0x00ff00));
    XFreePixmap(d, src);

    XFreeGC(d, gc);
    XFreePixmap(d, pm);
}

static void check_copies(Display *d, Window root)
{
    GC gc = XCreateGC(d, root, 0, NULL);

    /* Source pixels outside the source drawable leave the destination
     * alone (they used to come out transparent black). */
    Pixmap src = filled_pixmap(d, root, 10, 10, 0xff0000);
    Pixmap dst = filled_pixmap(d, root, 30, 30, 0xffffff);
    XCopyArea(d, src, dst, gc, 0, 0, 20, 20, 5, 5);
    CHECK(pixel_at(d, dst, 6, 6) == 0xff0000);
    CHECK(pixel_at(d, dst, 20, 20) == 0xffffff);
    XCopyArea(d, src, dst, gc, -5, -5, 10, 10, 0, 0);   /* negative source */
    CHECK(pixel_at(d, dst, 1, 1) == 0xffffff);
    CHECK(pixel_at(d, dst, 6, 6) == 0xff0000);

    /* XCopyPlane: set bits in the foreground, clear bits in the background,
     * from the plane that was asked for. */
    Pixmap bm = XCreatePixmap(d, root, 8, 2, 1);
    GC bgc = XCreateGC(d, bm, 0, NULL);
    XSetForeground(d, bgc, 0);
    XFillRectangle(d, bm, bgc, 0, 0, 8, 2);
    XSetForeground(d, bgc, 1);
    XFillRectangle(d, bm, bgc, 0, 0, 4, 2);
    XSetForeground(d, gc, 0xff0000);
    XSetBackground(d, gc, 0x0000ff);
    XCopyPlane(d, bm, dst, gc, 0, 0, 8, 2, 0, 28, 1);
    CHECK(pixel_at(d, dst, 1, 28) == 0xff0000);
    CHECK(pixel_at(d, dst, 6, 28) == 0x0000ff);
    Pixmap p24 = filled_pixmap(d, root, 2, 1, 0x000004);
    XCopyPlane(d, p24, dst, gc, 0, 0, 2, 1, 10, 28, 4);
    CHECK(pixel_at(d, dst, 10, 28) == 0xff0000);
    XCopyPlane(d, p24, dst, gc, 0, 0, 2, 1, 12, 28, 2);
    CHECK(pixel_at(d, dst, 12, 28) == 0x0000ff);

    /* XPutImage clips the source rectangle to the image. */
    XImage *im = XGetImage(d, src, 0, 0, 4, 4, AllPlanes, ZPixmap);
    if (im) {
        XPutImage(d, dst, gc, im, 2, 0, 20, 0, 4, 4);
        CHECK(pixel_at(d, dst, 21, 1) == 0xff0000);
        CHECK(pixel_at(d, dst, 23, 1) == 0xffffff);   /* past the image */
        XPutImage(d, dst, gc, im, -2, 0, 24, 5, 4, 1);
        CHECK(pixel_at(d, dst, 24, 5) == 0xffffff);   /* before the image */
        CHECK(pixel_at(d, dst, 26, 5) == 0xff0000);
        XDestroyImage(im);
    }

    XFreeGC(d, bgc);
    XFreePixmap(d, bm);
    XFreePixmap(d, p24);
    XFreePixmap(d, src);
    XFreePixmap(d, dst);
    XFreeGC(d, gc);
}

static void check_clipping(Display *d, Window root)
{
    Pixmap pm = filled_pixmap(d, root, 20, 20, 0xffffff);
    GC gc = XCreateGC(d, pm, 0, NULL);
    XSetForeground(d, gc, 0);

    /* An empty rectangle list disables output, not clipping. */
    XSetClipRectangles(d, gc, 0, 0, NULL, 0, Unsorted);
    XFillRectangle(d, pm, gc, 0, 0, 20, 20);
    CHECK(pixel_at(d, pm, 10, 10) == 0xffffff);

    /* XSetRegion clips to the region, not its bounding box. */
    Region r = XCreateRegion();
    XRectangle a = { 0, 0, 10, 5 }, b = { 0, 0, 5, 10 };
    XUnionRectWithRegion(&a, r, r);
    XUnionRectWithRegion(&b, r, r);
    XSetClipOrigin(d, gc, 3, 3);                 /* XSetRegion resets it */
    XSetRegion(d, gc, r);
    XFillRectangle(d, pm, gc, 0, 0, 20, 20);
    CHECK(pixel_at(d, pm, 2, 2) == 0);
    CHECK(pixel_at(d, pm, 8, 2) == 0);
    CHECK(pixel_at(d, pm, 8, 8) == 0xffffff);    /* inside the box, not the L */
    XDestroyRegion(r);

    /* Rectangles replace a clip mask. */
    Pixmap mask = XCreatePixmap(d, root, 20, 20, 1);
    GC mgc = XCreateGC(d, mask, 0, NULL);
    XSetForeground(d, mgc, 0);
    XFillRectangle(d, mask, mgc, 0, 0, 20, 20);   /* clips everything */
    XSetClipMask(d, gc, mask);
    XRectangle all = { 0, 0, 20, 20 };
    XSetClipRectangles(d, gc, 0, 0, &all, 1, Unsorted);
    XFillRectangle(d, pm, gc, 15, 15, 2, 2);
    CHECK(pixel_at(d, pm, 15, 15) == 0);
    XFreeGC(d, mgc);
    XFreePixmap(d, mask);

    XFreeGC(d, gc);
    XFreePixmap(d, pm);
}

static void check_stipples(Display *d, Window root)
{
    /* A 2x1 stipple: bit set at x=0, clear at x=1. */
    Pixmap st = XCreatePixmap(d, root, 2, 1, 1);
    GC sgc = XCreateGC(d, st, 0, NULL);
    XSetForeground(d, sgc, 0);
    XFillRectangle(d, st, sgc, 0, 0, 2, 1);
    XSetForeground(d, sgc, 1);
    XDrawPoint(d, st, sgc, 0, 0);

    Pixmap pm = filled_pixmap(d, root, 8, 4, 0xffffff);
    GC gc = XCreateGC(d, pm, 0, NULL);
    XSetForeground(d, gc, 0x000000);
    XSetBackground(d, gc, 0xff0000);
    XSetStipple(d, gc, st);
    XSetFillStyle(d, gc, FillStippled);
    XFillRectangle(d, pm, gc, 0, 0, 8, 1);
    CHECK(pixel_at(d, pm, 0, 0) == 0x000000);
    CHECK(pixel_at(d, pm, 1, 0) == 0xffffff);    /* clear bit: untouched */
    CHECK(pixel_at(d, pm, 2, 0) == 0x000000);

    XSetFillStyle(d, gc, FillOpaqueStippled);
    XFillRectangle(d, pm, gc, 0, 1, 8, 1);
    CHECK(pixel_at(d, pm, 0, 1) == 0x000000);
    CHECK(pixel_at(d, pm, 1, 1) == 0xff0000);    /* clear bit: background */

    /* The tile/stipple origin: (1,0) shifts the pattern one pixel right. */
    XSetTSOrigin(d, gc, 1, 0);
    XFillRectangle(d, pm, gc, 0, 2, 8, 1);
    CHECK(pixel_at(d, pm, 1, 2) == 0x000000);
    CHECK(pixel_at(d, pm, 0, 2) == 0xff0000);
    XSetTSOrigin(d, gc, 0, 0);

    /* A tile paints its own pixels. */
    Pixmap tile = filled_pixmap(d, root, 2, 1, 0x00ff00);
    GC tgc = XCreateGC(d, tile, 0, NULL);
    XSetForeground(d, tgc, 0x0000ff);
    XDrawPoint(d, tile, tgc, 1, 0);
    XSetTile(d, gc, tile);
    XSetFillStyle(d, gc, FillTiled);
    XFillRectangle(d, pm, gc, 0, 3, 8, 1);
    CHECK(pixel_at(d, pm, 0, 3) == 0x00ff00);
    CHECK(pixel_at(d, pm, 1, 3) == 0x0000ff);

    XFreeGC(d, tgc);
    XFreePixmap(d, tile);
    XFreeGC(d, gc);
    XFreeGC(d, sgc);
    XFreePixmap(d, st);
    XFreePixmap(d, pm);
}

/* XDrawString's bytes are Latin-1 (core fonts are ISO 8859-1): "\xe9" is é
 * and must draw what the UTF-8 entry point draws for "é". */
static void check_latin1_text(Display *d, Window root)
{
    Pixmap a = filled_pixmap(d, root, 40, 20, 0xffffff);
    Pixmap b = filled_pixmap(d, root, 40, 20, 0xffffff);
    GC gc = XCreateGC(d, a, 0, NULL);
    XSetForeground(d, gc, 0);
    XDrawString(d, a, gc, 2, 15, "\xe9" "A", 2);
    Xutf8DrawString(d, b, NULL, gc, 2, 15, "\xc3\xa9" "A", 3);
    XImage *ia = XGetImage(d, a, 0, 0, 40, 20, AllPlanes, ZPixmap);
    XImage *ib = XGetImage(d, b, 0, 0, 40, 20, AllPlanes, ZPixmap);
    int diff = 0, ink = 0;
    for (int y = 0; ia && ib && y < 20; y++)
        for (int x = 0; x < 40; x++) {
            diff += pix(ia, x, y) != pix(ib, x, y);
            ink += pix(ia, x, y) != 0xffffff;
        }
    CHECK(ink > 0 && diff == 0);
    if (ia) XDestroyImage(ia);
    if (ib) XDestroyImage(ib);
    XFreeGC(d, gc);
    XFreePixmap(d, a);
    XFreePixmap(d, b);
}

static void run_display_checks(Display *d)
{
    Window root = DefaultRootWindow(d);
    Window win = XCreateSimpleWindow(d, root, 0, 0, 50, 50, 0, 0, 0);
    check_properties(d, win);
    check_text_apis(d, win);
    check_drawing(d, root);
    check_fonts(d, root);
    check_raster_ops(d, root);
    check_copies(d, root);
    check_clipping(d, root);
    check_stipples(d, root);
    check_latin1_text(d, root);
    XDestroyWindow(d, win);
}

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
    /* libX11: a negative offset is returned signed, with YValue as well as
     * YNegative (clients test YValue, then measure from the far edge). */
    CHECK((m & YValue) && (m & YNegative) && y == -20);
    m = XParseGeometry("-0-0", &x, &y, &w, &h);
    CHECK(m == (XValue | YValue | XNegative | YNegative) && x == 0 && y == 0);
    w = h = 7;
    m = XParseGeometry("x200", &x, &y, &w, &h);
    CHECK(m == HeightValue && h == 200 && w == 7);
    CHECK(XParseGeometry("100x", &x, &y, &w, &h) == 0);     /* no height digits */
    CHECK(XParseGeometry("80x24junk", &x, &y, &w, &h) == 0); /* trailing garbage */
    CHECK(XParseGeometry("", &x, &y, &w, &h) == NoValue);

    /* XWMGeometry: sizes are in resize increments over the base size (Xt's
     * Shell builds def_geom in character cells -- "xterm -geometry 80x24"),
     * and a negative position selects the matching gravity. */
    {
        XSizeHints hints = { 0 };
        hints.flags = PResizeInc | PBaseSize;
        hints.width_inc = 6; hints.height_inc = 13;
        hints.base_width = 4; hints.base_height = 2;
        int gx, gy, gw, gh, grav;
        int gm = XWMGeometry(NULL, 0, "80x24", "10x10", 1, &hints,
                             &gx, &gy, &gw, &gh, &grav);
        CHECK(gw == 4 + 80 * 6 && gh == 2 + 24 * 13);
        CHECK(!(gm & XValue) && gx == 0 && gy == 0 && grav == NorthWestGravity);
        gm = XWMGeometry(NULL, 0, "-5+7", "10x10", 1, &hints,
                         &gx, &gy, &gw, &gh, &grav);
        CHECK(gw == 4 + 10 * 6 && (gm & XNegative) && (gm & XValue));
        CHECK(grav == NorthEastGravity && gy == 7);
        CHECK(gx == 1280 - 5 - gw - 2);           /* default screen width */
        /* A default position alone is used but not reported as the user's. */
        gm = XWMGeometry(NULL, 0, NULL, "10x10-0-0", 0, &hints,
                         &gx, &gy, &gw, &gh, &grav);
        CHECK(!(gm & XValue) && (gm & XNegative) && grav == SouthEastGravity);
    }

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
    /* XSubImage returns an independent copy the caller destroys; it used to
     * alias the parent's pixels, so XDestroyImage freed a pointer into the
     * middle of the parent's buffer. */
    XImage *sub = XSubImage(img, 1, 1, 4, 2);
    CHECK(sub != NULL);
    if (sub) {
        CHECK(sub->width == 4 && sub->height == 2);
        CHECK(XGetPixel(sub, 2, 1) == 0x123456);
        XPutPixel(sub, 2, 1, 0xabcdef);
        CHECK(XGetPixel(img, 3, 2) == 0x123456);
        XDestroyImage(sub);
    }
    /* Clipped at the source edge: the part outside is zero. */
    sub = XSubImage(img, 6, 2, 4, 4);
    CHECK(sub && XGetPixel(sub, 0, 0) == XGetPixel(img, 6, 2));
    CHECK(sub && XGetPixel(sub, 3, 3) == 0);
    if (sub) XDestroyImage(sub);
    XDestroyImage(img);

    /* 1-bit bitmaps have a sub-image too (it used to return NULL). */
    XImage *bm = XCreateImage(NULL, NULL, 1, XYBitmap, 0, NULL, 16, 4, 8, 0);
    CHECK(bm != NULL);
    XPutPixel(bm, 9, 3, 1);
    sub = XSubImage(bm, 8, 2, 8, 2);
    CHECK(sub && XGetPixel(sub, 1, 1) == 1 && XGetPixel(sub, 0, 0) == 0);
    if (sub) XDestroyImage(sub);
    XDestroyImage(bm);

    check_xrm();

    /* XGetDefault reads the display's resources (RESOURCE_MANAGER, built
     * from $XENVIRONMENT among others). */
    char envfile[] = "/tmp/mw-xenv-XXXXXX";
    int efd = mkstemp(envfile);
    if (efd >= 0) {
        const char *res = "*background: red\nxterm.foreground: green\n"
                          "Program.font: fixed\n";
        if (write(efd, res, strlen(res)) < 0) {}
        close(efd);
        setenv("XENVIRONMENT", envfile, 1);
    }

    /* Atoms/properties need a display; skip if none. */
    Display *d = XOpenDisplay(NULL);
    if (d && efd >= 0) {
        /* Loose bindings and class names match; results stay distinct and
         * valid (they used to share one static buffer). */
        char *bg = XGetDefault(d, "/usr/bin/xterm", "background");
        char *fg = XGetDefault(d, "xterm", "foreground");
        char *fn = XGetDefault(d, "anything", "font");
        CHECK(bg && !strcmp(bg, "red"));
        CHECK(fg && !strcmp(fg, "green"));
        CHECK(bg && fg && bg != fg && !strcmp(bg, "red"));
        CHECK(fn && !strcmp(fn, "fixed"));
        CHECK(XGetDefault(d, "xterm", "nosuch") == NULL);
    }
    if (efd >= 0) unlink(envfile);
    if (d) {
        Atom a = XInternAtom(d, "TEST_ATOM", False);
        CHECK(a != None);
        char *an = XGetAtomName(d, a);
        CHECK(an && strcmp(an, "TEST_ATOM") == 0);
        XFree(an);
        CHECK(XInternAtom(d, "TEST_ATOM", True) == a);
        CHECK(XInternAtom(d, "NO_SUCH_ATOM_XYZ", True) == None);
        CHECK(XInternAtom(d, "PRIMARY", True) == XA_PRIMARY);

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

        run_display_checks(d);
        XCloseDisplay(d);
    } else {
        printf("note: no display; skipping display-dependent checks\n");
    }

    if (failures == 0) { printf("test_core: all checks passed\n"); return 0; }
    fprintf(stderr, "test_core: %d failure(s)\n", failures);
    return 1;
}
