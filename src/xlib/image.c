/* image.c — XImage objects and pixmaps (including bitmaps). */
#include "internal.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* ---------------------------------------------------------- XImage funcs */

static int img_bpp(int depth, int format)
{
    if (format == XYBitmap || format == XYPixmap) return depth;
    if (depth <= 8) return 8;
    if (depth <= 16) return 16;
    return 32;
}

static unsigned long img_get_pixel(XImage *img, int x, int y)
{    long off = (long)y * img->bytes_per_line;
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
    long off = (long)y * img->bytes_per_line;
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
    XImage *img = calloc(1, sizeof *img);
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
    if (bytes_per_line > 0) img->bytes_per_line = bytes_per_line;
    else {
        int bits = (int)(width * img->bits_per_pixel);
        int pad = img->bitmap_pad;
        if (pad <= 0) pad = 8;
        img->bytes_per_line = ((bits + pad - 1) / pad) * (pad / 8);
    }
    if (data) img->data = data;
    else img->data = calloc((size_t)img->bytes_per_line * (height ? height : 1), 1);
    img->obdata = NULL;
    set_funcs(img);
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
    if (!img) return NULL;
    int bpp = img->bits_per_pixel;
    if (bpp < 8) return NULL;   /* bit offsets unsupported */
    int bytes = bpp / 8;
    char *base = img->data + (long)y * img->bytes_per_line + (long)x * bytes;
    XImage *sub = XCreateImage(NULL, NULL, img->depth, img->format, 0, NULL,
                               w, h, img->bitmap_pad, img->bytes_per_line);
    free(sub->data);
    sub->data = base;             /* borrowed; caller must not destroy naively */
    sub->obdata = NULL;
    return sub;
}

/* ---------------------------------------------------------- get/put image */

XImage *XGetImage(Display *d, Drawable dr, int x, int y,
                  unsigned int width, unsigned int height,
                  unsigned long plane_mask, int format)
{
    (void)plane_mask;
    /* Render requests are buffered until something reads the drawable: apply
     * them before reading it back, or the image misses everything the client
     * has just drawn through Render (rendercheck draws with Render and reads
     * back with XGetImage). */
    mw_render_drain(d);
    int dw, dh, depth;
    MwSurface *s = mw_drawable_surface(d, dr, &dw, &dh, &depth);
    if (!s) return NULL;
    XImage *img = XCreateImage(d, &MWD(d)->visual, (unsigned int)depth,
                               format == XYPixmap ? ZPixmap : format,
                               0, NULL, width, height, 32, 0);
    uint32_t *tmp = calloc((size_t)width * height, 4);
    mw_surface_get(s, x, y, (int)width, (int)height, tmp, (int)width);
    for (unsigned int j = 0; j < height; j++)
        for (unsigned int i = 0; i < width; i++) {
            uint32_t px = tmp[(size_t)j * width + i];
            /* A depth-32 drawable carries alpha; masking it off lost the
             * channel entirely (rendercheck reads ARGB destinations back to
             * check alpha, and every gradient looked transparent).  Shallower
             * drawables have no alpha to keep. */
            img_put_pixel(img, (int)i, (int)j,
                          depth >= 32 ? px : (px & 0xffffffu));
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
    /* A 1-bit image (XYBitmap, or a depth-1 XImage) carries bit values, not
     * pixels: the X server draws set bits in the GC foreground and clear bits
     * in the GC background.  Treating the 0/1 values as pixels made every
     * bitmap image (Motif/XPM depth-1 graphics, the help viewer's bitonal
     * TIFF art, window icons, ...) come out solid black. */
    int onebit = (img->bits_per_pixel == 1);
    uint32_t fg = 0xff000000u | ((uint32_t)(gc ? gc->foreground : 0) & 0xffffff);
    uint32_t bg = 0xff000000u | ((uint32_t)(gc ? gc->background : 0) & 0xffffff);
    /* A depth-32 drawable keeps the image's alpha: cairo uploads premultiplied
     * ARGB icons this way, and forcing alpha to 0xff made every transparent
     * icon pixel opaque black.  A 24-bit drawable has no alpha, so those stay
     * opaque. */
    int ddepth = 0;
    mw_drawable_surface(d, dr, NULL, NULL, &ddepth);
    int keep_alpha = (ddepth >= 32);
    MwSurface *tmp = mw_surface_create((int)width, (int)height);
    uint32_t *td = (uint32_t *)mw_surface_data(tmp);
    int tstride = mw_surface_stride(tmp) / 4;
    for (unsigned int j = 0; j < height; j++)
        for (unsigned int i = 0; i < width; i++) {
            unsigned long px = img_get_pixel(img, src_x + (int)i, src_y + (int)j);
            td[(size_t)j * tstride + i] = onebit
                ? (px ? fg : bg)
                : (keep_alpha ? (uint32_t)px
                              : (0xff000000u | ((uint32_t)px & 0xffffff)));
        }
    mw_surface_mark_dirty(tmp);
    MwCanvas *c = mw_canvas_for_drawable(d, dr, gc, NULL, NULL);
    if (c) {
        mw_canvas_put_image(c, tmp, dest_x, dest_y, 0, 0, (int)width, (int)height);
        mw_canvas_end(c);
    }
    mw_surface_destroy(tmp);
    MwWindow *win = mw_window(d, dr);
    if (win) mw_window_damage(win);
    return 1;
}

/* ---------------------------------------------------------------- pixmaps */

Pixmap XCreatePixmap(Display *d, Drawable dr, unsigned int width,
                     unsigned int height, unsigned int depth)
{
    (void)dr;
    MwPixmap *pm = calloc(1, sizeof *pm);
    pm->id = mw_alloc_id(d);
    pm->w = (int)width; pm->h = (int)height;
    pm->depth = (int)depth;
    pm->screen = MWSCR(d);
    pm->surface = mw_surface_create((int)width, (int)height);
    mw_surface_clear(pm->surface, 0x00000000);
    mw_register(d, pm->id, MW_OBJ_PIXMAP, pm);
    if (getenv("MW_TRACE_RENDER"))
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
    if (!pm) return None;
    int stride = (int)((width + 7) / 8);
    uint32_t *px = malloc(sizeof(uint32_t) * width * height);
    for (unsigned int y = 0; y < height; y++)
        for (unsigned int x = 0; x < width; x++) {
            unsigned char b = (unsigned char)data[y * stride + (x >> 3)];
            int set = (b >> (x & 7)) & 1;
            px[y * width + x] = set ? 0xffffffffu : 0x00000000u;
        }
    MwCanvas *c = mw_canvas_begin(pm->surface, NULL, 0, 0, 0);
    for (unsigned int y = 0; y < height; y++)
        for (unsigned int x = 0; x < width; x++) {
            mw_set_source_argb(c, px[y * width + x]);
            mw_set_operator(c, GXcopy);
            mw_rect(c, x, y, 1, 1);
            mw_fill_path(c);
        }
    mw_canvas_end(c);
    free(px);
    return p;
}

Pixmap XCreatePixmapFromBitmapData(Display *d, Drawable dr, char *data,
                                   unsigned int width, unsigned int height,
                                   unsigned long fg, unsigned long bg,
                                   unsigned int depth)
{
    Pixmap p = XCreatePixmap(d, dr, width, height, depth);
    MwPixmap *pm = mw_pixmap(d, p);
    if (!pm) return None;
    int stride = (int)((width + 7) / 8);
    MwCanvas *c = mw_canvas_begin(pm->surface, NULL, 0, 0, 0);
    for (unsigned int y = 0; y < height; y++)
        for (unsigned int x = 0; x < width; x++) {
            unsigned char b = (unsigned char)data[y * stride + (x >> 3)];
            int set = (b >> (x & 7)) & 1;
            mw_set_source_argb(c, 0xff000000u |
                               (uint32_t)((set ? fg : bg) & 0xffffff));
            mw_rect(c, x, y, 1, 1);
            mw_fill_path(c);
        }
    mw_canvas_end(c);
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
    if (bw <= 0 || bh <= 0) return -1;
    nbytes = (bw + 7) / 8 * bh;
    char *bits = calloc(nbytes, 1);
    /* collect hex bytes from the braces */
    int idx = 0;
    int in_braces = 0;
    for (const char *q = data; *q; q++) {
        if (*q == '{') { in_braces = 1; continue; }
        if (*q == '}') { in_braces = 0; continue; }
        if (!in_braces) continue;
        if (q[0] == '0' && (q[1] == 'x' || q[1] == 'X')) {
            unsigned int byte;
            if (sscanf(q, "0x%2x", &byte) == 1 && idx < nbytes) {
                bits[idx++] = (char)byte;
                q += 3;
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
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = malloc(sz + 1);
    size_t rd = fread(buf, 1, sz, f);
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
    unsigned char *row = calloc(stride, 1);
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
