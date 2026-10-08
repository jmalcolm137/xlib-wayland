/* xftrender_x.c — the Xft Render-level entry points actually draw.
 *
 * XftGlyphSpecRender/GlyphFontSpecRender composite glyphs through Render
 * Pictures (the path pangoxft takes when a caller has installed a source
 * Picture).  The shim used to drop these; this draws a glyph through a real
 * Picture and checks the pixels changed.
 */
#include <X11/Xlib.h>
#include <X11/extensions/Xrender.h>
#include <X11/Xft/Xft.h>

#include <stdio.h>
#include <string.h>
#include <unistd.h>

int main(void)
{
    setbuf(stdout, NULL);
    Display *d = XOpenDisplay(NULL);
    if (!d) { fprintf(stderr, "xftrender_x: no display\n"); return 2; }
    int scr = DefaultScreen(d);
    const int W = 64, H = 64;

    Window w = XCreateSimpleWindow(d, DefaultRootWindow(d), 0, 0, W, H, 0, 0, 0);
    XSelectInput(d, w, ExposureMask);
    XMapWindow(d, w);
    for (;;) { XEvent e; XNextEvent(d, &e); if (e.type == Expose) break; }
    fprintf(stderr, "XFT: exposed\n");

    /* A black background, then the Render path draws over it. */
    GC gc = XCreateGC(d, w, 0, NULL);
    XSetForeground(d, gc, 0x000000);
    XFillRectangle(d, w, gc, 0, 0, W, H);
    XFlush(d);

    XRenderPictFormat *fmt = XRenderFindVisualFormat(d, DefaultVisual(d, scr));
    if (!fmt) { printf("XFT:NONE fmt=%p\n", (void *)fmt); return 1; }
    Picture dst = XRenderCreatePicture(d, w, fmt, 0, NULL);
    XRenderColor red = { 0xffff, 0, 0, 0xffff };
    Picture src = XRenderCreateSolidFill(d, &red);
    fprintf(stderr, "XFT: pictures dst=%lu src=%lu\n",
            (unsigned long)dst, (unsigned long)src);

    XftFont *font = XftFontOpenName(d, scr, "monospace-24");
    if (!font) { printf("XFT:NONE font\n"); return 1; }
    fprintf(stderr, "XFT: font=%p\n", (void *)font);

    FT_UInt glyph = XftCharIndex(d, font, 'M');
    XftGlyphSpec spec;
    memset(&spec, 0, sizeof spec);
    spec.glyph = glyph;
    spec.x = 8;
    spec.y = 40;
    fprintf(stderr, "XFT: glyph=%u rendering\n", (unsigned)glyph);
    XftGlyphSpecRender(d, PictOpOver, src, font, dst, 0, 0, &spec, 1);
    XFlush(d);
    fprintf(stderr, "XFT: rendered\n");

    XImage *img = XGetImage(d, w, 0, 0, W, H, AllPlanes, ZPixmap);
    int hit = 0, nonblack = 0, maxr = 0, maxg = 0, maxb = 0;
    if (img) {
        for (int y = 0; y < H; y++)
            for (int x = 0; x < W; x++) {
                unsigned int px = XGetPixel(img, x, y);
                unsigned r = (px >> 16) & 0xff, g = (px >> 8) & 0xff, b = px & 0xff;
                if (r) maxr = r > maxr ? (int)r : maxr;
                if (g) maxg = g > maxg ? (int)g : maxg;
                if (b) maxb = b > maxb ? (int)b : maxb;
                if (px & 0xffffff) nonblack++;
                if (r > 0x40 && g < 0x40 && b < 0x40) hit = 1;
            }
        XDestroyImage(img);
    }
    printf("XFT:glyph=%u red=%d nonblack=%d maxrgb=%d,%d,%d\n",
           (unsigned)glyph, hit, nonblack, maxr, maxg, maxb);
    return (glyph && hit) ? 0 : 1;
}
