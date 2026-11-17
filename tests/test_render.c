/* test_render.c — focused Render tests, through the real libXrender request
 * encoders: glyph uploads (batched AddGlyphs, A1/ARGB32 strides, referenced
 * glyph sets), solid and gradient masks, CPClipMask given as a pixmap, and
 * the trapezoid source origin. */
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/extensions/Xrender.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;

#define CHECK(cond) do { if (!(cond)) { \
    fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

static Display *d;
static Window root;

static const XRenderColor white = { 0xffff, 0xffff, 0xffff, 0xffff };
static const XRenderColor red   = { 0xffff, 0, 0, 0xffff };

typedef struct { Pixmap pm; Picture pic; } Target;

static Target target(int w, int h)
{
    Target t;
    t.pm = XCreatePixmap(d, root, w, h, 24);
    t.pic = XRenderCreatePicture(d, t.pm,
                                 XRenderFindStandardFormat(d, PictStandardRGB24),
                                 0, NULL);
    XRenderFillRectangle(d, PictOpSrc, t.pic, &white, 0, 0, w, h);
    return t;
}

static void target_free(Target t)
{
    XRenderFreePicture(d, t.pic);
    XFreePixmap(d, t.pm);
}

static unsigned long px(Target t, int x, int y)
{
    XImage *im = XGetImage(d, t.pm, x, y, 1, 1, AllPlanes, ZPixmap);
    if (!im) return 0xdeadbeef;
    unsigned long v = XGetPixel(im, 0, 0) & 0xffffff;
    XDestroyImage(im);
    return v;
}

static int near(unsigned long a, unsigned long b, int tol)
{
    for (int s = 0; s < 24; s += 8) {
        int ca = (int)((a >> s) & 0xff), cb = (int)((b >> s) & 0xff);
        if (ca - cb > tol || cb - ca > tol) return 0;
    }
    return 1;
}

/* Two glyphs in one AddGlyphs request.  libXrender sends all ids, then all
 * glyph infos, then all images -- they used to be parsed as interleaved
 * records, so only a single-glyph upload worked. */
static void batched_glyphs(void)
{
    Target t = target(40, 10);
    GlyphSet gs = XRenderCreateGlyphSet(d,
                      XRenderFindStandardFormat(d, PictStandardA8));
    Glyph ids[2] = { 'a', 'b' };
    XGlyphInfo info[2] = {
        { 2, 4, 0, 0, 10, 0 },   /* w=2 h=4, advance 10 */
        { 3, 4, 0, 0, 10, 0 },   /* w=3 h=4 */
    };
    /* A8 rows are padded to 4 bytes. */
    char img[2 * 16];
    memset(img, 0xff, sizeof img);
    XRenderAddGlyphs(d, gs, ids, info, 2, img, sizeof img);
    Picture src = XRenderCreateSolidFill(d, &red);
    XRenderCompositeString8(d, PictOpOver, src, t.pic, NULL, gs, 0, 0,
                            5, 2, "ab", 2);
    CHECK(px(t, 5, 3) == 0xff0000);      /* 'a' */
    CHECK(px(t, 6, 3) == 0xff0000);
    CHECK(px(t, 7, 3) == 0xffffff);      /* 'a' is two wide */
    CHECK(px(t, 15, 3) == 0xff0000);     /* 'b' at the advance */
    CHECK(px(t, 17, 3) == 0xff0000);
    CHECK(px(t, 18, 3) == 0xffffff);

    /* A referenced glyph set shares the glyphs. */
    GlyphSet ref = XRenderReferenceGlyphSet(d, gs);
    XRenderFreeGlyphSet(d, gs);
    Target t2 = target(40, 10);
    XRenderCompositeString8(d, PictOpOver, src, t2.pic, NULL, ref, 0, 0,
                            5, 2, "b", 1);
    CHECK(px(t2, 5, 3) == 0xff0000);
    XRenderFreeGlyphSet(d, ref);
    XRenderFreePicture(d, src);
    target_free(t2);
    target_free(t);
}

/* A1 glyph rows are padded to 32 bits (the bitmap scanline pad), not to a
 * byte; ARGB32 glyphs are four bytes per pixel. */
static void glyph_formats(void)
{
    Picture src = XRenderCreateSolidFill(d, &red);

    Target t = target(40, 10);
    GlyphSet a1 = XRenderCreateGlyphSet(d,
                      XRenderFindStandardFormat(d, PictStandardA1));
    Glyph id = 1;
    XGlyphInfo info = { 10, 2, 0, 0, 12, 0 };
    /* Row 0: only pixel 0 set; row 1: only pixel 9 set (LSB first). */
    unsigned char img[8] = { 0x01, 0, 0, 0,  0x00, 0x02, 0, 0 };
    XRenderAddGlyphs(d, a1, &id, &info, 1, (const char *)img, sizeof img);
    char s = 1;
    XRenderCompositeString8(d, PictOpOver, src, t.pic, NULL, a1, 0, 0,
                            0, 0, &s, 1);
    CHECK(px(t, 0, 0) == 0xff0000);
    CHECK(px(t, 1, 0) == 0xffffff);
    CHECK(px(t, 9, 1) == 0xff0000);
    CHECK(px(t, 0, 1) == 0xffffff);
    XRenderFreeGlyphSet(d, a1);
    target_free(t);

    t = target(40, 10);
    GlyphSet argb = XRenderCreateGlyphSet(d,
                        XRenderFindStandardFormat(d, PictStandardARGB32));
    XGlyphInfo ai = { 3, 1, 0, 0, 4, 0 };
    unsigned int apx[3] = { 0xff000000u, 0x00000000u, 0xff000000u };
    XRenderAddGlyphs(d, argb, &id, &ai, 1, (const char *)apx, sizeof apx);
    XRenderCompositeString8(d, PictOpOver, src, t.pic, NULL, argb, 0, 0,
                            0, 0, &s, 1);
    CHECK(px(t, 0, 0) == 0xff0000);
    CHECK(px(t, 1, 0) == 0xffffff);
    CHECK(px(t, 2, 0) == 0xff0000);
    XRenderFreeGlyphSet(d, argb);
    target_free(t);

    XRenderFreePicture(d, src);
}

/* Solid and gradient pictures as masks (they used to be ignored, so the
 * composite came out unmasked). */
static void picture_masks(void)
{
    Picture src = XRenderCreateSolidFill(d, &red);

    Target t = target(20, 4);
    XRenderColor half = { 0, 0, 0, 0x8000 };
    Picture m = XRenderCreateSolidFill(d, &half);
    XRenderComposite(d, PictOpOver, src, m, t.pic, 0, 0, 0, 0, 0, 0, 20, 4);
    CHECK(near(px(t, 5, 1), 0xff7f7f, 3));
    XRenderFreePicture(d, m);
    target_free(t);

    t = target(100, 4);
    XLinearGradient lg = { { 0, 0 }, { XDoubleToFixed(100), 0 } };
    XFixed stops[2] = { 0, XDoubleToFixed(1) };
    XRenderColor cols[2] = { { 0, 0, 0, 0 }, { 0, 0, 0, 0xffff } };
    m = XRenderCreateLinearGradient(d, &lg, stops, cols, 2);
    XRenderComposite(d, PictOpOver, src, m, t.pic, 0, 0, 0, 0, 0, 0, 100, 4);
    unsigned long left = px(t, 1, 1), right = px(t, 98, 1);
    CHECK(near(left, 0xffffff, 8));
    CHECK(near(right, 0xff0000, 8));
    XRenderFreePicture(d, m);
    target_free(t);

    XRenderFreePicture(d, src);
}

/* CPClipMask is a Pixmap (a depth-1 bitmap) whose origin is the clip
 * origin; SetPictureClipRectangles replaces it. */
static void clip_mask_pixmap(void)
{
    Target t = target(40, 10);
    Pixmap bm = XCreatePixmap(d, root, 40, 10, 1);
    GC gc = XCreateGC(d, bm, 0, NULL);
    XSetForeground(d, gc, 0);
    XFillRectangle(d, bm, gc, 0, 0, 40, 10);
    XSetForeground(d, gc, 1);
    XFillRectangle(d, bm, gc, 0, 0, 10, 10);       /* bits set for x < 10 */
    XFreeGC(d, gc);

    XRenderPictureAttributes pa;
    pa.clip_mask = bm;
    pa.clip_x_origin = 5;                          /* mask covers x 5..14 */
    pa.clip_y_origin = 0;
    XRenderChangePicture(d, t.pic, CPClipMask | CPClipXOrigin | CPClipYOrigin, &pa);
    Picture src = XRenderCreateSolidFill(d, &red);
    XRenderComposite(d, PictOpOver, src, None, t.pic, 0, 0, 0, 0, 0, 0, 40, 10);
    CHECK(px(t, 2, 2) == 0xffffff);
    CHECK(px(t, 6, 2) == 0xff0000);
    CHECK(px(t, 13, 2) == 0xff0000);
    CHECK(px(t, 20, 2) == 0xffffff);

    /* Clip rectangles replace the mask. */
    XRectangle r = { 30, 0, 5, 10 };
    XRenderSetPictureClipRectangles(d, t.pic, 0, 0, &r, 1);
    XRenderComposite(d, PictOpOver, src, None, t.pic, 0, 0, 0, 0, 0, 0, 40, 10);
    CHECK(px(t, 32, 2) == 0xff0000);

    XRenderFreePicture(d, src);
    XFreePixmap(d, bm);
    target_free(t);
}

/* Trapezoids: a source pixel is taken at src + (dst - first vertex), so a
 * trapezoid at x=50 with xSrc=0 starts at source x=0. */
static void trapezoid_source_origin(void)
{
    Target t = target(80, 10);
    Target srcp = target(20, 10);
    XRenderColor blue = { 0, 0, 0xffff, 0xffff }, green = { 0, 0xffff, 0, 0xffff };
    XRenderFillRectangle(d, PictOpSrc, srcp.pic, &blue, 0, 0, 10, 10);
    XRenderFillRectangle(d, PictOpSrc, srcp.pic, &green, 10, 0, 10, 10);

    XTrapezoid tr;
    tr.top = XDoubleToFixed(0);
    tr.bottom = XDoubleToFixed(10);
    tr.left.p1.x = XDoubleToFixed(50);  tr.left.p1.y = XDoubleToFixed(0);
    tr.left.p2.x = XDoubleToFixed(50);  tr.left.p2.y = XDoubleToFixed(10);
    tr.right.p1.x = XDoubleToFixed(70); tr.right.p1.y = XDoubleToFixed(0);
    tr.right.p2.x = XDoubleToFixed(70); tr.right.p2.y = XDoubleToFixed(10);
    XRenderCompositeTrapezoids(d, PictOpSrc, srcp.pic, t.pic,
                               XRenderFindStandardFormat(d, PictStandardA8),
                               0, 0, &tr, 1);
    CHECK(px(t, 52, 5) == 0x0000ff);
    CHECK(px(t, 65, 5) == 0x00ff00);
    CHECK(px(t, 40, 5) == 0xffffff);
    target_free(srcp);
    target_free(t);
}

/* A trapezoid's edges are lines through p1 and p2, evaluated at top and
 * bottom -- p1/p2 are generally not at top/bottom.  Right edge: through
 * (20,0) and (40,20), so x = 25.5 at y = 5.5 (the old code took p1.x/p2.x
 * as the corners and put it at 22). */
static void trapezoid_edges(void)
{
    Target t = target(60, 20);
    Picture src = XRenderCreateSolidFill(d, &red);
    XTrapezoid tr;
    tr.top = XDoubleToFixed(5);
    tr.bottom = XDoubleToFixed(10);
    tr.left.p1.x = XDoubleToFixed(0);   tr.left.p1.y = XDoubleToFixed(-10);
    tr.left.p2.x = XDoubleToFixed(0);   tr.left.p2.y = XDoubleToFixed(20);
    tr.right.p1.x = XDoubleToFixed(20); tr.right.p1.y = XDoubleToFixed(0);
    tr.right.p2.x = XDoubleToFixed(40); tr.right.p2.y = XDoubleToFixed(20);
    XRenderCompositeTrapezoids(d, PictOpOver, src, t.pic,
                               XRenderFindStandardFormat(d, PictStandardA8),
                               0, 0, &tr, 1);
    CHECK(px(t, 23, 5) == 0xff0000);
    CHECK(px(t, 27, 5) == 0xffffff);
    CHECK(px(t, 2, 4) == 0xffffff);          /* above top */
    XRenderFreePicture(d, src);
    target_free(t);
}

/* Glyph runs: the source is sampled at xSrc + (x - first element's
 * position), and a CPClipMask clips the glyphs. */
static void glyph_source_and_clip(void)
{
    Target t = target(40, 10);
    Target srcp = target(20, 10);
    XRenderColor blue = { 0, 0, 0xffff, 0xffff }, green = { 0, 0xffff, 0, 0xffff };
    XRenderFillRectangle(d, PictOpSrc, srcp.pic, &blue, 0, 0, 10, 10);
    XRenderFillRectangle(d, PictOpSrc, srcp.pic, &green, 10, 0, 10, 10);
    GlyphSet gs = XRenderCreateGlyphSet(d, XRenderFindStandardFormat(d, PictStandardA8));
    Glyph id = 'g';
    XGlyphInfo info = { 4, 4, 0, 0, 5, 0 };
    char img[16];
    memset(img, 0xff, sizeof img);
    XRenderAddGlyphs(d, gs, &id, &info, 1, img, sizeof img);
    /* first element at (20,2); xSrc 12 -> source x 12 (green) at x 20 */
    XRenderCompositeString8(d, PictOpOver, srcp.pic, t.pic, NULL, gs,
                            12, 0, 20, 2, "g", 1);
    CHECK(px(t, 21, 3) == 0x00ff00);

    /* A clip mask covering x < 32 cuts a glyph drawn at 30..33. */
    Pixmap bm = XCreatePixmap(d, root, 40, 10, 1);
    GC gc = XCreateGC(d, bm, 0, NULL);
    XSetForeground(d, gc, 0);
    XFillRectangle(d, bm, gc, 0, 0, 40, 10);
    XSetForeground(d, gc, 1);
    XFillRectangle(d, bm, gc, 0, 0, 32, 10);
    XFreeGC(d, gc);
    XRenderPictureAttributes pa;
    pa.clip_mask = bm;
    XRenderChangePicture(d, t.pic, CPClipMask, &pa);
    Picture solid = XRenderCreateSolidFill(d, &red);
    XRenderCompositeString8(d, PictOpOver, solid, t.pic, NULL, gs,
                            0, 0, 30, 2, "g", 1);
    CHECK(px(t, 31, 3) == 0xff0000);
    CHECK(px(t, 33, 3) == 0xffffff);

    /* A clip mask replaces earlier clip rectangles. */
    XRectangle r = { 0, 0, 2, 2 };
    XRenderSetPictureClipRectangles(d, t.pic, 0, 0, &r, 1);
    XRenderChangePicture(d, t.pic, CPClipMask, &pa);
    XRenderFillRectangle(d, PictOpSrc, t.pic, &blue, 0, 6, 32, 4);
    CHECK(px(t, 20, 8) == 0x0000ff);

    /* A depth-1 mask drawn through Render (coverage in alpha) clips too. */
    Pixmap bm2 = XCreatePixmap(d, root, 40, 10, 1);
    Picture a1 = XRenderCreatePicture(d, bm2,
                     XRenderFindStandardFormat(d, PictStandardA1), 0, NULL);
    XRenderColor none = { 0, 0, 0, 0 }, full = { 0, 0, 0, 0xffff };
    XRenderFillRectangle(d, PictOpSrc, a1, &none, 0, 0, 40, 10);
    XRenderFillRectangle(d, PictOpSrc, a1, &full, 0, 0, 5, 10);
    pa.clip_mask = bm2;
    XRenderChangePicture(d, t.pic, CPClipMask, &pa);
    XRenderFillRectangle(d, PictOpSrc, t.pic, &green, 0, 0, 40, 2);
    CHECK(px(t, 2, 0) == 0x00ff00);
    CHECK(px(t, 20, 0) != 0x00ff00);

    XRenderFreePicture(d, a1);
    XFreePixmap(d, bm2);
    XRenderFreePicture(d, solid);
    XFreePixmap(d, bm);
    XRenderFreeGlyphSet(d, gs);
    target_free(srcp);
    target_free(t);
}

int main(void)
{
    d = XOpenDisplay(NULL);
    if (!d) {
        fprintf(stderr, "test_render: no display\n");
        return getenv("MW_TEST_REQUIRE_DISPLAY") ? 1 : 77;
    }
    int ev, er;
    if (!XRenderQueryExtension(d, &ev, &er)) {
        fprintf(stderr, "test_render: no Render\n");
        return 1;
    }
    root = DefaultRootWindow(d);
    batched_glyphs();
    glyph_formats();
    picture_masks();
    clip_mask_pixmap();
    trapezoid_source_origin();
    trapezoid_edges();
    glyph_source_and_clip();
    XCloseDisplay(d);
    if (failures == 0) { printf("test_render: all checks passed\n"); return 0; }
    fprintf(stderr, "test_render: %d failure(s)\n", failures);
    return 1;
}
