/* render_extra_x.c — the Render/clip additions that have no other test:
 * a depth-1 core clip mask, a Render CPClipMask, a masked composite onto a
 * transformed destination picture, and XIGetSelectedEvents. */
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/extensions/Xrender.h>
#include <X11/extensions/XInput2.h>
#include <X11/Xft/Xft.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static Display *d;
static Window root;

/* Core XSetClipMask: a depth-1 mask with set bits for x in [16,48) must clip a
 * red fill to exactly that band. */
static int core_clip_mask(void)
{
    Pixmap out = XCreatePixmap(d, root, 64, 64, 24);
    Pixmap mask = XCreatePixmap(d, root, 64, 64, 1);
    GC gc = XCreateGC(d, out, 0, NULL);

    XSetForeground(d, gc, 1);
    XFillRectangle(d, mask, gc, 16, 0, 32, 64);
    XSetForeground(d, gc, 0xffffff);
    XFillRectangle(d, out, gc, 0, 0, 64, 64);

    XSetClipMask(d, gc, mask);
    XSetForeground(d, gc, 0xff0000);
    XFillRectangle(d, out, gc, 0, 0, 64, 64);
    XSync(d, 0);

    XImage *im = XGetImage(d, out, 0, 0, 64, 64, ~0UL, ZPixmap);
    int in = 0, outred = 0;
    for (int y = 0; y < 64; y++)
        for (int x = 0; x < 64; x++) {
            unsigned long p = XGetPixel(im, x, y) & 0xffffff;
            if (x >= 16 && x < 48) { if (p == 0xff0000) in++; }
            else if (p == 0xff0000) outred++;
        }
    XDestroyImage(im);
    XFreeGC(d, gc);
    XFreePixmap(d, out);
    XFreePixmap(d, mask);
    return in == 2048 && outred == 0;
}

/* Render CPClipMask: an A8 clip mask covering the left half. */
static int render_clip_mask(void)
{
    Pixmap outp = XCreatePixmap(d, root, 64, 64, 24);
    XRenderPictFormat *rgb = XRenderFindStandardFormat(d, PictStandardRGB24);
    Picture out = XRenderCreatePicture(d, outp, rgb, 0, NULL);

    Pixmap maskp = XCreatePixmap(d, root, 64, 64, 8);
    XRenderPictFormat *a8 = XRenderFindStandardFormat(d, PictStandardA8);
    Picture mask = XRenderCreatePicture(d, maskp, a8, 0, NULL);

    XRenderColor white = { 0, 0, 0, 0xffff };
    XRenderFillRectangle(d, PictOpSrc, mask, &white, 0, 0, 32, 64);
    XRenderColor bg = { 0xffff, 0xffff, 0xffff, 0xffff };
    XRenderFillRectangle(d, PictOpSrc, out, &bg, 0, 0, 64, 64);

    XRenderPictureAttributes pa;
    pa.clip_mask = mask;
    XRenderChangePicture(d, out, CPClipMask, &pa);
    XRenderColor red = { 0xffff, 0, 0, 0xffff };
    XRenderFillRectangle(d, PictOpSrc, out, &red, 0, 0, 64, 64);
    XSync(d, 0);

    XImage *im = XGetImage(d, outp, 0, 0, 64, 64, ~0UL, ZPixmap);
    int in = 0, outred = 0;
    for (int y = 0; y < 64; y++)
        for (int x = 0; x < 64; x++) {
            unsigned long p = XGetPixel(im, x, y) & 0xffffff;
            if (x < 32) { if (p == 0xff0000) in++; }
            else if (p == 0xff0000) outred++;
        }
    XDestroyImage(im);
    return in == 2048 && outred == 0;
}

/* A masked Over onto a destination translated by +10 in x: the red must land
 * at x in [10,64), leaving 0..9 white. */
static int dst_transform(void)
{
    Pixmap outp = XCreatePixmap(d, root, 64, 64, 24);
    XRenderPictFormat *rgb = XRenderFindStandardFormat(d, PictStandardRGB24);
    Picture out = XRenderCreatePicture(d, outp, rgb, 0, NULL);

    XRenderColor white = { 0xffff, 0xffff, 0xffff, 0xffff };
    XRenderFillRectangle(d, PictOpSrc, out, &white, 0, 0, 64, 64);

    Pixmap mkp = XCreatePixmap(d, root, 1, 1, 32);
    XRenderPictFormat *argb = XRenderFindStandardFormat(d, PictStandardARGB32);
    Picture mask = XRenderCreatePicture(d, mkp, argb, 0, NULL);
    XRenderFillRectangle(d, PictOpSrc, mask, &white, 0, 0, 1, 1);
    XRenderPictureAttributes mpa;
    mpa.repeat = RepeatNormal;
    XRenderChangePicture(d, mask, CPRepeat, &mpa);

    XRenderColor red = { 0xffff, 0, 0, 0xffff };
    Picture src = XRenderCreateSolidFill(d, &red);

    XTransform t;
    memset(&t, 0, sizeof t);
    t.matrix[0][0] = t.matrix[1][1] = t.matrix[2][2] = 1 << 16;
    t.matrix[0][2] = 10 << 16;
    XRenderSetPictureTransform(d, out, &t);

    XRenderComposite(d, PictOpOver, src, mask, out, 0, 0, 0, 0, 0, 0, 64, 64);
    XSync(d, 0);

    XImage *im = XGetImage(d, outp, 0, 0, 64, 64, ~0UL, ZPixmap);
    int red_left = 0, red_right = 0, white_left = 0;
    for (int y = 0; y < 64; y++)
        for (int x = 0; x < 64; x++) {
            unsigned long p = XGetPixel(im, x, y) & 0xffffff;
            if (x < 10) { if (p == 0xff0000) red_left++; else if (p == 0xffffff) white_left++; }
            else if (p == 0xff0000) red_right++;
        }
    XDestroyImage(im);
    return red_left == 0 && red_right == 54 * 64 && white_left == 10 * 64;
}

/* Xft's Render-level glyph entry points (what pangoxft uses when it has a
 * source Picture): XftDefaultHasRender must report Render, and a glyph run must
 * leave ink on the destination. */
static int xft_render(void)
{
    int scr = DefaultScreen(d);
    if (!XftDefaultHasRender(d)) return 0;

    Pixmap outp = XCreatePixmap(d, root, 200, 60, 24);
    XRenderPictFormat *rgb = XRenderFindStandardFormat(d, PictStandardRGB24);
    Picture dst = XRenderCreatePicture(d, outp, rgb, 0, NULL);
    XRenderColor w = { 0xffff, 0xffff, 0xffff, 0xffff };
    XRenderColor k = { 0x0000, 0x0000, 0x0000, 0xffff };
    XRenderFillRectangle(d, PictOpSrc, dst, &w, 0, 0, 200, 60);
    Picture black = XRenderCreateSolidFill(d, &k);

    XftFont *font = XftFontOpenName(d, scr, "DejaVu Sans-24");
    if (!font) return 0;

    XftGlyphSpec specs[16];
    int n = 0, x = 4;
    for (const char *p = "Hello"; *p; p++) {
        specs[n].glyph = XftCharIndex(d, font, (unsigned char)*p);
        specs[n].x = x;
        specs[n].y = 40;
        x += 16;
        n++;
    }
    XftGlyphSpecRender(d, PictOpOver, black, font, dst, 0, 0, specs, n);
    XSync(d, 0);

    XImage *im = XGetImage(d, outp, 0, 0, 200, 60, ~0UL, ZPixmap);
    int ink = 0;
    for (int yy = 0; yy < 60; yy++)
        for (int xx = 0; xx < 200; xx++)
            if ((XGetPixel(im, xx, yy) & 0xffffff) != 0xffffff) ink++;
    XDestroyImage(im);
    XFreePixmap(d, outp);
    return ink > 100;
}

static int xi_selected(void)
{
    int maj = 2, min = 0;
    if (XIQueryVersion(d, &maj, &min) != Success || maj < 2) return 0;
    Window w = DefaultRootWindow(d);

    unsigned char bits[1] = { 0 };
    XISetMask(bits, XI_Motion);
    XIEventMask m;
    m.deviceid = XIAllMasterDevices;
    m.mask_len = 1;
    m.mask = bits;
    XISelectEvents(d, w, &m, 1);
    XFlush(d);

    int n = 0;
    XIEventMask *r = XIGetSelectedEvents(d, w, &n);
    if (!r) return 0;
    int motion = 0;
    for (int i = 0; i < n; i++)
        for (int b = 0; b < r[i].mask_len; b++)
            for (int k = 0; k < 8; k++)
                if (r[i].mask[b] & (1 << k)) {
                    int ev = b * 8 + k;
                    if (ev == XI_Motion) motion = 1;
                }
    free(r);
    return n >= 1 && motion;
}

int main(void)
{
    d = XOpenDisplay(NULL);
    if (!d) { printf("RENDERX:NODISPLAY\n"); return 2; }
    root = DefaultRootWindow(d);

    int a = core_clip_mask();
    int b = render_clip_mask();
    int c = dst_transform();
    int e = xi_selected();
    int f = xft_render();
    printf("RENDERX:RESULT clipcore=%d cliprender=%d dstxform=%d xisel=%d xftrender=%d\n",
           a, b, c, e, f);
    return (a && b && c && e && f) ? 0 : 1;
}
