/* image.c — XImage objects and pixmaps (including bitmaps). */
#include "internal.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* ---------------------------------------------------------- XImage funcs */

/* As the server lays images out: XY formats are one bit per pixel per
 * plane, a depth-1 ZPixmap is one bit, deeper ones 8/16/32. */
static int img_bpp(int depth, int format)
{
    if (format == XYBitmap || format == XYPixmap || depth == 1) return 1;
    if (depth <= 8) return 8;
    if (depth <= 16) return 16;
    return 32;
}

/* An XYPixmap holds `depth` bit planes of bytes_per_line * height bytes each,
 * most significant plane first. */
static bool img_planar(const XImage *img)
{
    return img->format == XYPixmap && img->depth > 1;
}

static bool img_in_bounds(const XImage *img, int x, int y)
{
    return img && img->data && x >= 0 && y >= 0 &&
           x < img->width && y < img->height;
}

static unsigned long img_get_pixel(XImage *img, int x, int y)
{
    if (!img_in_bounds(img, x, y)) return 0;
    long off = (long)y * img->bytes_per_line;
    if (img_planar(img)) {
        size_t plane = (size_t)img->bytes_per_line * img->height;
        unsigned long v = 0;
        for (int p = 0; p < img->depth; p++) {
            unsigned char b = (unsigned char)img->data[plane * p + off + (x >> 3)];
            v = (v << 1) | ((b >> (x & 7)) & 1);
        }
        return v;
    }
    switch (img->bits_per_pixel) {
    case 32: return ((uint32_t *)(img->data + off))[x];
    case 16: return ((uint16_t *)(img->data + off))[x];
    case 8:  return ((uint8_t  *)(img->data + off))[x];
    case 1:  return (unsigned long)((img->data[off + (x >> 3)] >> (x & 7)) & 1);
    default: return 0;
    }
}

static int img_put_pixel(XImage *img, int x, int y, unsigned long v)
{
    if (!img_in_bounds(img, x, y)) return 0;
    long off = (long)y * img->bytes_per_line;
    if (img_planar(img)) {
        size_t plane = (size_t)img->bytes_per_line * img->height;
        for (int p = 0; p < img->depth; p++) {
            char *b = &img->data[plane * p + off + (x >> 3)];
            if ((v >> (img->depth - 1 - p)) & 1) *b |= (char)(1 << (x & 7));
            else                                 *b &= (char)~(1 << (x & 7));
        }
        return 1;
    }
    switch (img->bits_per_pixel) {
    case 32: ((uint32_t *)(img->data + off))[x] = (uint32_t)v; break;
    case 16: ((uint16_t *)(img->data + off))[x] = (uint16_t)v; break;
    case 8:  ((uint8_t  *)(img->data + off))[x] = (uint8_t)v; break;
    case 1:
        if (v & 1) img->data[off + (x >> 3)] |= (char)(1 << (x & 7));
        else       img->data[off + (x >> 3)] &= (char)~(1 << (x & 7));
        break;
    default: return 0;
    }
    return 1;
}

static int img_add_pixel(XImage *img, long v)
{
    if (img->bits_per_pixel == 32) {
        /* unused */
    }
    (void)v;
    return 1;
}

static int img_destroy(XImage *img)
{
    if (!img) return 0;
    free(img->data);
    free(img);
    return 1;
}

static XImage *img_sub_image(XImage *img, int x, int y, unsigned int w,
                             unsigned int h);
static XImage *img_create_image(struct _XDisplay *d, Visual *v, unsigned int depth,
                                int format, int offset, char *data,
                                unsigned int width, unsigned int height,
                                int bitmap_pad, int bytes_per_line);

static void set_funcs(XImage *img)
{
    img->f.create_image = img_create_image;
    img->f.destroy_image = img_destroy;
    img->f.get_pixel = img_get_pixel;
    img->f.put_pixel = img_put_pixel;
    img->f.sub_image = img_sub_image;
    img->f.add_pixel = img_add_pixel;
}

XImage *XCreateImage(Display *d, Visual *visual, unsigned int depth, int format,
                     int offset, char *data, unsigned int width,
                     unsigned int height, int bitmap_pad, int bytes_per_line)
{
    /* Reject geometry the pixel accessors cannot address safely: the X
     * protocol caps sizes at 16 bits, depth must be a real pixel depth (the
     * planar path shifts by depth-1), and a pad below one byte would make
     * bytes_per_line zero. */
    if (width > 65535 || height > 65535 || depth < 1 || depth > 32)
        return NULL;
    if (bitmap_pad != 0 && bitmap_pad != 8 && bitmap_pad != 16 &&
        bitmap_pad != 32)
        return NULL;
    if (bytes_per_line < 0) return NULL;
    XImage *img = calloc(1, sizeof *img);
    if (!img) return NULL;
    img->width = (int)width;
    img->height = (int)height;
    img->xoffset = offset;
    img->format = format;
    img->depth = (int)depth;
    img->bits_per_pixel = img_bpp((int)depth, format);
    img->byte_order = LSBFirst;
    img->bitmap_unit = 32;
    img->bitmap_bit_order = LSBFirst;
    img->bitmap_pad = bitmap_pad ? bitmap_pad : 32;
    Visual *v = visual;
    if (!v && d) v = &MWD(d)->visual;
    img->red_mask = v ? v->red_mask : 0x00ff0000;
    img->green_mask = v ? v->green_mask : 0x0000ff00;
    img->blue_mask = v ? v->blue_mask : 0x000000ff;
    long bits = (long)width * img->bits_per_pixel;
    int pad = img->bitmap_pad;
    long min_bpl = ((bits + pad - 1) / pad) * (pad / 8);
    if (bytes_per_line > 0) {
        /* A caller-supplied pitch shorter than a row would let the accessors
         * index past each row into the next and off the end of the buffer. */
        if (bytes_per_line < min_bpl) { free(img); return NULL; }
        img->bytes_per_line = bytes_per_line;
    } else {
        img->bytes_per_line = (int)min_bpl;
    }
    /* As in libX11, the pixels are the caller's: NULL stays NULL. */
    img->data = data;
    img->obdata = NULL;
    set_funcs(img);
    return img;
}

/* An image with its own zeroed pixels (XGetImage, XSubImage, MIT-SHM). */
XImage *mw_create_image_alloc(Display *d, Visual *visual, unsigned int depth,
                              int format, unsigned int width,
                              unsigned int height, int bitmap_pad)
{
    XImage *img = XCreateImage(d, visual, depth, format, 0, NULL, width,
                               height, bitmap_pad, 0);
    if (!img) return NULL;
    size_t planes = (format == XYPixmap && depth > 1) ? depth : 1;
    img->data = calloc((size_t)img->bytes_per_line * (height ? height : 1) * planes, 1);
    if (!img->data) { free(img); return NULL; }
    return img;
}

static XImage *img_create_image(struct _XDisplay *d, Visual *v, unsigned int depth,
                                int format, int offset, char *data,
                                unsigned int width, unsigned int height,
                                int bitmap_pad, int bytes_per_line)
{
    return XCreateImage((Display *)d, v, depth, format, offset, data,
                        width, height, bitmap_pad, bytes_per_line);
}

static XImage *img_sub_image(XImage *img, int x, int y, unsigned int w,
                             unsigned int h)
{
    /* As in libX11, the result owns a copy of the pixels (callers hand it to
     * XDestroyImage), and the part of the rectangle outside the source is
     * left zero. */
    if (!img) return NULL;
    XImage *sub = mw_create_image_alloc(NULL, NULL, img->depth, img->format,
                                        w, h, img->bitmap_pad);
    if (!sub || !sub->data) { if (sub) img_destroy(sub); return NULL; }
    sub->red_mask = img->red_mask;
    sub->green_mask = img->green_mask;
    sub->blue_mask = img->blue_mask;
    if (x < 0 || y < 0) return sub;
    int cw = (int)w, ch = (int)h;
    if (x + cw > img->width)  cw = img->width - x;
    if (y + ch > img->height) ch = img->height - y;
    for (int r = 0; r < ch; r++)
        for (int c = 0; c < cw; c++)
            img_put_pixel(sub, c, r, img_get_pixel(img, x + c, y + r));
    return sub;
}

/* ---------------------------------------------------------- get/put image */

XImage *XGetImage(Display *d, Drawable dr, int x, int y,
                  unsigned int width, unsigned int height,
                  unsigned long plane_mask, int format)
{
    if (width == 0 || height == 0 || width > 32767 || height > 32767)
        return NULL;
    mw_render_drain(d);
    int dw, dh, depth;
    MwSurface *s = mw_drawable_surface(d, dr, &dw, &dh, &depth);
    if (!s) return NULL;
    Visual *vis = depth >= 32 ? &MWD(d)->visual32 : &MWD(d)->visual;
    XImage *img = mw_create_image_alloc(d, vis, (unsigned int)depth,
                                        format == XYPixmap ? ZPixmap : format,
                                        width, height, 32);
    if (!img) return NULL;
    uint32_t *tmp = calloc((size_t)width * height, 4);
    if (!tmp) { XDestroyImage(img); return NULL; }
    mw_surface_get(s, x, y, (int)width, (int)height, tmp, (int)width);
    for (unsigned int j = 0; j < height; j++)
        for (unsigned int i = 0; i < width; i++) {
            uint32_t px = tmp[(size_t)j * width + i];
            unsigned long out;
            if (depth == 8)       out = (px >> 24) & 0xff;
            else if (depth >= 32) out = px;
            else if (depth == 1)  out = px & 1u;          /* bitmaps: bit values */
            else                  out = px & 0xffffffu;
            img_put_pixel(img, (int)i, (int)j, out & plane_mask);
        }
    free(tmp);
    return img;
}

/* XGetSubImage is XGetImage blitted into a caller-supplied XImage at
 * (dest_x,dest_y): any part that falls outside the destination is clipped, and
 * the destination image is returned.  GDK reaches it through its image path. */
XImage *XGetSubImage(Display *d, Drawable dr, int x, int y,
                     unsigned int width, unsigned int height,
                     unsigned long plane_mask, int format,
                     XImage *dest_image, int dest_x, int dest_y)
{
    if (!dest_image) return NULL;
    XImage *src = XGetImage(d, dr, x, y, width, height, plane_mask, format);
    if (!src) return NULL;
    for (unsigned int j = 0; j < height; j++) {
        for (unsigned int i = 0; i < width; i++) {
            int dx = dest_x + (int)i;
            int dy = dest_y + (int)j;
            if (dx < 0 || dy < 0 ||
                dx >= dest_image->width || dy >= dest_image->height)
                continue;
            img_put_pixel(dest_image, dx, dy, img_get_pixel(src, (int)i, (int)j));
        }
    }
    XDestroyImage(src);
    return dest_image;
}

int XPutImage(Display *d, Drawable dr, GC gc, XImage *img,
              int src_x, int src_y, int dest_x, int dest_y,
              unsigned int width, unsigned int height)
{
    if (!img) return 0;
    /* As in libX11, the source rectangle is clipped to the image; a negative
     * source offset moves the destination with it.  Work in long long so the
     * arithmetic cannot overflow on hostile offsets. */
    if (width > 65535 || height > 65535) return 0;
    long long sx = src_x, sy = src_y, dx = dest_x, dy = dest_y;
    long long lw = width, lh = height;
    if (sx < 0) { dx -= sx; lw += sx; sx = 0; }
    if (sy < 0) { dy -= sy; lh += sy; sy = 0; }
    if (sx + lw > img->width)  lw = img->width - sx;
    if (sy + lh > img->height) lh = img->height - sy;
    if (lw <= 0 || lh <= 0) return 1;
    if (dx < INT_MIN / 2 || dx > INT_MAX / 2 ||
        dy < INT_MIN / 2 || dy > INT_MAX / 2) return 1;
    src_x = (int)sx; src_y = (int)sy; dest_x = (int)dx; dest_y = (int)dy;
    int w = (int)lw, h = (int)lh;
    width = (unsigned int)w;
    height = (unsigned int)h;
    /* A 1-bit image (XYBitmap, or a depth-1 XImage) carries bit values, not
     * pixels: the X server draws set bits in the GC foreground and clear bits
     * in the GC background.  Treating the 0/1 values as pixels made every
     * bitmap image (Motif/XPM depth-1 graphics, the help viewer's bitonal
     * TIFF art, window icons, ...) come out solid black. */
    int onebit = (img->format == XYBitmap || img->depth == 1);
    uint32_t fg = 0xff000000u | ((uint32_t)(gc ? gc->foreground : 0) & 0xffffff);
    uint32_t bg = 0xff000000u | ((uint32_t)(gc ? gc->background : 0) & 0xffffff);
    /* A depth-32 drawable keeps the image's alpha: cairo uploads premultiplied
     * ARGB icons this way, and forcing alpha to 0xff made every transparent
     * icon pixel opaque black.  A 24-bit drawable has no alpha, so those stay
     * opaque. */
    int dw = 0, dh = 0, ddepth = 0;
    MwSurface *dsurf = mw_drawable_surface(d, dr, &dw, &dh, &ddepth);
    if (!dsurf) return 0;
    int keep_alpha = (ddepth >= 32);
    int alpha_only = (ddepth == 8);

    /* Fast path: unclipped 32-bit ZPixmap GXcopy AllPlanes on a 1x surface
     * writes straight into the destination surface without a temp allocation. */
    if (!onebit && !alpha_only && img->format == ZPixmap &&
        img->bits_per_pixel == 32 && img->data &&
        img->bytes_per_line >= img->width * 4 &&
        mw_surface_scale(dsurf) == 1 && mw_surface_data(dsurf) &&
        (!gc || (gc->function == GXcopy &&
                 (gc->plane_mask & 0xfffffful) == 0xfffffful &&
                 !gc->has_clip_rects && gc->clip_mask == None))) {
        mw_render_drain(d);
        int cx = dest_x, cy = dest_y, csx = src_x, csy = src_y;
        int cw = (int)width, ch = (int)height;
        if (cx < 0) { csx -= cx; cw += cx; cx = 0; }
        if (cy < 0) { csy -= cy; ch += cy; cy = 0; }
        if (cx + cw > dw) cw = dw - cx;
        if (cy + ch > dh) ch = dh - cy;
        if (cw > 0 && ch > 0) {
            uint32_t *dst = (uint32_t *)mw_surface_data(dsurf);
            int dstride = mw_surface_stride(dsurf) / 4;
            for (int j = 0; j < ch; j++) {
                const uint32_t *srow = (const uint32_t *)(img->data +
                                        (size_t)(csy + j) * img->bytes_per_line) + csx;
                uint32_t *drow = dst + (size_t)(cy + j) * dstride + cx;
                if (keep_alpha) {
                    memcpy(drow, srow, (size_t)cw * 4);
                } else {
                    for (int i = 0; i < cw; i++)
                        drow[i] = 0xff000000u | (srow[i] & 0xffffffu);
                }
            }
            mw_surface_mark_dirty(dsurf);
        }
        MwWindow *win = mw_window(d, dr);
        if (win) mw_window_damage_rect(win, dest_x, dest_y, (int)width, (int)height);
        return 1;
    }

    MwSurface *tmp = mw_surface_create((int)width, (int)height);
    uint32_t *td = tmp ? (uint32_t *)mw_surface_data(tmp) : NULL;
    if (!td) { mw_surface_destroy(tmp); return 0; }
    int tstride = mw_surface_stride(tmp) / 4;
    for (unsigned int j = 0; j < height; j++)
        for (unsigned int i = 0; i < width; i++) {
            unsigned long px = img_get_pixel(img, src_x + (int)i, src_y + (int)j);
            td[(size_t)j * tstride + i] = onebit
                ? (px ? fg : bg)
                : (alpha_only ? (((uint32_t)px & 0xffu) << 24)
                              : (keep_alpha ? (uint32_t)px
                                            : (0xff000000u | ((uint32_t)px & 0xffffffu))));
        }
    mw_surface_mark_dirty(tmp);
    MwCanvas *c = mw_canvas_for_drawable(d, dr, gc, NULL, NULL);
    if (c) {
        mw_canvas_put_image(c, tmp, dest_x, dest_y, 0, 0, (int)width, (int)height);
        mw_canvas_end(c);
    }
    mw_surface_destroy(tmp);
    MwWindow *win = mw_window(d, dr);
    if (win) mw_window_damage_rect(win, dest_x, dest_y, (int)width, (int)height);
    return 1;
}

/* ---------------------------------------------------------------- pixmaps */

Pixmap XCreatePixmap(Display *d, Drawable dr, unsigned int width,
                     unsigned int height, unsigned int depth)
{
    (void)dr;
    MwPixmap *pm = calloc(1, sizeof *pm);
    if (!pm) return None;
    pm->id = mw_alloc_id(d);
    pm->w = (int)width; pm->h = (int)height;
    pm->depth = (int)depth;
    pm->screen = MWSCR(d);
    pm->surface = mw_surface_create((int)width, (int)height);
    if (!pm->surface) { free(pm); return None; }
    mw_surface_clear(pm->surface, 0x00000000);
    mw_register(d, pm->id, MW_OBJ_PIXMAP, pm);
    if (MW_TRACE_RENDER())
        fprintf(stderr, "MW: XCreatePixmap id=0x%lx %ux%u depth=%u lookup=%p\n",
                (unsigned long)pm->id, width, height, depth,
                (void *)mw_pixmap(d, pm->id));
    return pm->id;
}

int XFreePixmap(Display *d, Pixmap p)
{
    /* Dispatch buffered Render requests that may still target this pixmap
     * while its surface is alive. */
    mw_render_drain(d);
    MwPixmap *pm = mw_pixmap(d, p);
    if (pm) {
        if (pm->surface) mw_surface_destroy(pm->surface);
        if (MWD(d)->damages) mw_xdamage_drawable_gone(d, p);
        mw_unregister(d, p);
        free(pm);
    }
    return 1;
}

Pixmap XCreateBitmapFromData(Display *d, Drawable dr, _Xconst char *data,
                             unsigned int width, unsigned int height)
{
    Pixmap p = XCreatePixmap(d, dr, width, height, 1);
    MwPixmap *pm = mw_pixmap(d, p);
    if (!pm || !pm->surface) return None;
    uint32_t *dst = (uint32_t *)mw_surface_data(pm->surface);
    int dstride = mw_surface_stride(pm->surface) / 4;
    if (!dst || !data) return p;
    int stride = (int)((width + 7) / 8);
    for (unsigned int y = 0; y < height; y++) {
        uint32_t *row = dst + (size_t)y * dstride;
        const unsigned char *src = (const unsigned char *)data + (size_t)y * stride;
        for (unsigned int x = 0; x < width; x++)
            row[x] = ((src[x >> 3] >> (x & 7)) & 1) ? 0xffffffffu : 0x00000000u;
    }
    mw_surface_mark_dirty(pm->surface);
    return p;
}

Pixmap XCreatePixmapFromBitmapData(Display *d, Drawable dr, char *data,
                                   unsigned int width, unsigned int height,
                                   unsigned long fg, unsigned long bg,
                                   unsigned int depth)
{
    Pixmap p = XCreatePixmap(d, dr, width, height, depth);
    MwPixmap *pm = mw_pixmap(d, p);
    if (!pm || !pm->surface) return None;
    uint32_t *dst = (uint32_t *)mw_surface_data(pm->surface);
    int dstride = mw_surface_stride(pm->surface) / 4;
    if (!dst || !data) return p;
    uint32_t fgpx = 0xff000000u | (uint32_t)(fg & 0xffffffu);
    uint32_t bgpx = 0xff000000u | (uint32_t)(bg & 0xffffffu);
    int stride = (int)((width + 7) / 8);
    for (unsigned int y = 0; y < height; y++) {
        uint32_t *row = dst + (size_t)y * dstride;
        const unsigned char *src = (const unsigned char *)data + (size_t)y * stride;
        for (unsigned int x = 0; x < width; x++)
            row[x] = ((src[x >> 3] >> (x & 7)) & 1) ? fgpx : bgpx;
    }
    mw_surface_mark_dirty(pm->surface);
    return p;
}

/* XPM-less helper used by some Motif code paths */
Pixmap XCreatePixmapFromData(Display *d, Drawable dr, char *data,
                             unsigned int width, unsigned int height,
                             unsigned long fg, unsigned long bg, unsigned int depth)
{ return XCreatePixmapFromBitmapData(d, dr, data, width, height, fg, bg, depth); }

Pixmap XCreateDataFromPixmap(Display *d, Pixmap p, int *w, int *h, int *depth)
{
    (void)d;
    MwPixmap *pm = mw_pixmap(d, p);
    if (pm) { if (w) *w = pm->w; if (h) *h = pm->h; if (depth) *depth = pm->depth; }
    return p;
}

/* -------------------------------------------------------- XBM file I/O */

static int parse_xbm(_Xconst char *data, int *w, int *h, char **bits_out)
{
    const char *p = data;
    int bw = 0, bh = 0, nbytes = 0;
    /* find _width / _height defines */
    const char *l = p;
    while (l && *l) {
        const char *nl = strchr(l, '\n');
        int len = nl ? (int)(nl - l) : (int)strlen(l);
        char line[512];
        int n = len < 511 ? len : 511;
        memcpy(line, l, n); line[n] = 0;
        int v;
        if (sscanf(line, "#define %*s _width %d", &v) == 1 ||
            (strstr(line, "_width") && sscanf(strstr(line, "_width") + 6, "%d", &v) == 1))
            bw = v;
        else if (sscanf(line, "#define %*s _height %d", &v) == 1 ||
                 (strstr(line, "_height") && sscanf(strstr(line, "_height") + 7, "%d", &v) == 1))
            bh = v;
        (void)bits_out; (void)nbytes;
        if (!nl) break;
        l = nl + 1;
    }
    if (bw <= 0 || bh <= 0 || bw > 32767 || bh > 32767) return -1;
    nbytes = (bw + 7) / 8 * bh;
    char *bits = calloc((size_t)nbytes, 1);
    if (!bits) return -1;
    /* collect hex bytes from the braces */
    int idx = 0;
    int in_braces = 0;
    for (const char *q = data; *q; q++) {
        if (*q == '{') { in_braces = 1; continue; }
        if (*q == '}') { in_braces = 0; continue; }
        if (!in_braces) continue;
        if (q[0] == '0' && (q[1] == 'x' || q[1] == 'X')) {
            char *end = NULL;
            unsigned long byte = strtoul(q, &end, 16);
            if (end > q + 2 && idx < nbytes) {
                bits[idx++] = (char)(byte & 0xffu);
                q = end - 1;
            }
        }
    }
    *w = bw; *h = bh; *bits_out = bits;
    return 0;
}

int XReadBitmapFileData(_Xconst char *filename, unsigned int *width,
                        unsigned int *height, unsigned char **data,
                        int *x_hot, int *y_hot)
{
    FILE *f = fopen(filename, "rb");
    if (!f) return BitmapOpenFailed;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return BitmapFileInvalid; }
    long sz = ftell(f);
    /* Even a 32767x32767 XBM in hex text is well under 1 GiB; cap far lower
     * so a hostile path (/dev/zero, a huge file) cannot exhaust memory. */
    if (sz < 0 || sz > (64L << 20)) { fclose(f); return BitmapFileInvalid; }
    fseek(f, 0, SEEK_SET);
    char *buf = malloc((size_t)sz + 1);
    if (!buf) { fclose(f); return BitmapNoMemory; }
    size_t rd = fread(buf, 1, (size_t)sz, f);
    buf[rd] = 0;
    fclose(f);
    int w = 0, h = 0; char *bits = NULL;
    int r = parse_xbm(buf, &w, &h, &bits);
    free(buf);
    if (r != 0) return BitmapFileInvalid;
    *width = w; *height = h;
    *data = (unsigned char *)bits;
    if (x_hot) *x_hot = -1;
    if (y_hot) *y_hot = -1;
    return BitmapSuccess;
}

int XReadBitmapFile(Display *d, Drawable dr, _Xconst char *filename,
                    unsigned int *width, unsigned int *height, Pixmap *bitmap,
                    int *x_hot, int *y_hot)
{
    unsigned char *data = NULL;
    int r = XReadBitmapFileData(filename, width, height, &data, x_hot, y_hot);
    if (r != BitmapSuccess) return r;
    *bitmap = XCreateBitmapFromData(d, dr, (char *)data, *width, *height);
    free(data);
    return BitmapSuccess;
}

int XWriteBitmapFile(Display *d, _Xconst char *filename, Pixmap bitmap,
                     unsigned int width, unsigned int height, int x_hot, int y_hot)
{
    (void)d;
    FILE *f = fopen(filename, "w");
    if (!f) return BitmapOpenFailed;
    fprintf(f, "#define image_width %u\n#define image_height %u\n", width, height);
    fprintf(f, "static unsigned char image_bits[] = {\n");
    MwPixmap *pm = mw_pixmap(d, bitmap);
    int stride = (int)((width + 7) / 8);
    unsigned char *row = calloc(stride ? stride : 1, 1);
    if (!row) { fclose(f); return BitmapNoMemory; }
    for (unsigned int y = 0; y < height; y++) {
        memset(row, 0, stride);
        for (unsigned int x = 0; x < width; x++) {
            uint32_t px = 0;
            if (pm && pm->surface) {
                uint32_t *s = (uint32_t *)mw_surface_data(pm->surface);
                int ss = mw_surface_stride(pm->surface) / 4;
                if ((int)x < pm->w && (int)y < pm->h)
                    px = s[(size_t)y * ss + x];
            }
            if (px & 0xffffff) row[x >> 3] |= 1 << (x & 7);
        }
        for (int i = 0; i < stride; i++) fprintf(f, "0x%02x, ", row[i]);
        fprintf(f, "\n");
    }
    free(row);
    fprintf(f, "};\n");
    if (x_hot >= 0) fprintf(f, "/* hot: %d,%d */\n", x_hot, y_hot);
    fclose(f);
    return BitmapSuccess;
}
