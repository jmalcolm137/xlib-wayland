/* test_xft.c — Xft text, pictures and Region clipping.
 *
 * The client matrix only exercises Xft through xclock (its clock face).  This
 * covers what Xft's other entry points need, and in particular the two paths
 * that have broken:
 *
 *   1. Xft text: XftFontOpenName + XftDrawString8 go through the Render glyph
 *      sets.  Upstream libXft decides which source to use from
 *      XRenderQueryVersion, which only reaches it through Xlib's async handler
 *      for the pipelined QueryVersion reply; getting that wrong made Xft fall
 *      back to a 1x1 pixmap source and draw nothing.
 *
 *   2. XftDrawPicture()/XftDrawSrcPicture() plus XRenderSetPictureClipRegion():
 *      libXrender reads the private Region layout as BOX { x1, x2, y1, y2 }.
 *      Storing the mirror as XRectangle { x, y, width, height } turned a
 *      full clip into a degenerate one and blanked the drawing.
 *
 * The harness checks the captured frame: text ink, a red XftDrawRect, and a
 * triangle drawn through the clipped picture that must appear only on the
 * clipped side.
 */
#include <X11/Xlib.h>
#include <X11/Xft/Xft.h>
#include <X11/extensions/Xrender.h>

#include <stdio.h>
#include <unistd.h>

int main(void)
{
    Display *d = XOpenDisplay(NULL);
    if (!d) { printf("XFT:NODISPLAY\n"); return 2; }

    int s = DefaultScreen(d);
    Window w = XCreateSimpleWindow(d, RootWindow(d, s), 0, 0, 320, 220, 0,
                                   BlackPixel(d, s), WhitePixel(d, s));
    XSelectInput(d, w, ExposureMask);
    XMapWindow(d, w);
    XFlush(d);

    /* XNextEvent returns 0 in Xlib; clients loop on that. */
    for (;;) {
        XEvent e;
        if (XNextEvent(d, &e) != 0) { printf("XFT:NEXTEVENT-RET\n"); return 2; }
        if (e.type == Expose && e.xexpose.count == 0)
            break;
    }

    XftDraw *draw = XftDrawCreate(d, w, DefaultVisual(d, s), DefaultColormap(d, s));
    XftFont *font = XftFontOpenName(d, s, "DejaVu Sans-24");
    XftColor black, red;
    if (!draw || !font ||
        !XftColorAllocName(d, DefaultVisual(d, s), DefaultColormap(d, s),
                           "black", &black) ||
        !XftColorAllocName(d, DefaultVisual(d, s), DefaultColormap(d, s),
                           "red", &red)) {
        printf("XFT:SETUP-FAILED\n");
        return 2;
    }

    XGlyphInfo gi;
    XftTextExtents8(d, font, (FcChar8 *)"Hello", 5, &gi);

    /* 1. text through the glyph path */
    XftDrawString8(draw, &black, font, 10, 40, (FcChar8 *)"Hello Xft", 9);

    /* 2. an Xft-filled rectangle, red */
    XftDrawRect(draw, &red, 10, 60, 80, 20);

    /* 3. a triangle through Xft's pictures, clipped to x < 150 by a Region */
    Picture dst = XftDrawPicture(draw);
    Picture src = XftDrawSrcPicture(draw, &black);
    if (dst && src) {
        Region r = XCreateRegion();
        XRectangle clip = { 0, 0, 150, 220 };
        XUnionRectWithRegion(&clip, r, r);
        XRenderSetPictureClipRegion(d, dst, r);
        XDestroyRegion(r);

        XRenderColor c = { 0, 0, 0, 0xffff };
        Picture solid = XRenderCreateSolidFill(d, &c);
        XTriangle t;
        t.p1.x = 0;        t.p1.y = 100 << 16;
        t.p2.x = 300 << 16; t.p2.y = 100 << 16;
        t.p3.x = 150 << 16; t.p3.y = 170 << 16;
        /* Over, not Src: Render applies the operator across the whole clipped
         * drawable with the triangle as a mask, so Src would clear everything
         * inside the clip (that is what rendercheck's triangle tests require).
         * Over paints only the triangle and leaves the text and red rectangle
         * alone. */
        XRenderCompositeTriangles(d, PictOpOver, solid, dst, NULL, 0, 0, &t, 1);
        XRenderFreePicture(d, solid);
    } else {
        printf("XFT:NOPICTURE\n");
    }

    printf("XFT:DRAWN extents=%d\n", gi.xOff);
    fflush(stdout);
    XFlush(d);

    /* Stay up until the compositor takes its frame. */
    for (;;) {
        while (XPending(d)) { XEvent e; XNextEvent(d, &e); }
        usleep(20000);
    }
    return 0;
}
