/* cursor.c — X cursors mapped onto the Wayland cursor API. */
#define _GNU_SOURCE
#include "internal.h"

#include <wayland-cursor.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>

struct cursor_name { unsigned int glyph; const char *name; };

/* A small subset of the X cursor font glyphs → freedesktop cursor names. */
static const struct cursor_name names[] = {
    { XC_left_ptr,    "left_ptr" },
    { XC_xterm,       "xterm" },
    { XC_watch,       "watch" },
    { XC_hand1,       "hand1" },
    { XC_hand2,       "hand2" },
    { XC_X_cursor,    "X_cursor" },
    { XC_crosshair,   "crosshair" },
    { XC_sb_h_double_arrow, "sb_h_double_arrow" },
    { XC_sb_v_double_arrow, "sb_v_double_arrow" },
    { XC_fleur,       "fleur" },
    { XC_question_arrow, "question_arrow" },
    { XC_coffee_mug,  "coffee_mug" },
    { XC_pirate,      "pirate" },
    { XC_plus,        "plus" },
    { XC_sizing,      "sizing" },
};

static const char *cursor_name_for(unsigned int glyph)
{
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++)
        if (names[i].glyph == glyph) return names[i].name;
    return "left_ptr";
}

void mw_cursor_init(Display *d)
{
    XDisplayImpl *dp = MWD(d);
    dp->cursor_theme = NULL;
    dp->cursor_theme_size = 24;
    dp->default_cursor = NULL;
    dp->xcursor_theme = NULL;
    dp->xcursor_size = 24;
    if (!dp->wl_shm) return;
    dp->cursor_theme = wl_cursor_theme_load(NULL, dp->cursor_theme_size,
                                            dp->wl_shm);
}

void mw_cursor_fini(Display *d)
{
    XDisplayImpl *dp = MWD(d);
    if (dp->cursor_theme) wl_cursor_theme_destroy(dp->cursor_theme);
    dp->cursor_theme = NULL;
}

Cursor XCreateFontCursor(Display *d, unsigned int shape)
{
    MwCursor *c = calloc(1, sizeof *c);
    c->id = mw_alloc_id(d);
    c->shape = shape;
    c->fg = 0xff000000;
    c->bg = 0xffffffff;
    XDisplayImpl *dp = MWD(d);
    if (dp->cursor_theme) {
        struct wl_cursor *wc = wl_cursor_theme_get_cursor(dp->cursor_theme,
                                                          cursor_name_for(shape));
        c->wl = wc;
    }
    mw_register(d, c->id, MW_OBJ_CURSOR, c);
    return c->id;
}

Cursor XCreatePixmapCursor(Display *d, Pixmap source, Pixmap mask,
                           XColor *foreground, XColor *background,
                           unsigned int xhot, unsigned int yhot)
{
    (void)source; (void)mask; (void)xhot; (void)yhot;
    MwCursor *c = calloc(1, sizeof *c);
    c->id = mw_alloc_id(d);
    if (foreground) c->fg = 0xff000000u | (uint32_t)(foreground->pixel & 0xffffff);
    if (background) c->bg = 0xff000000u | (uint32_t)(background->pixel & 0xffffff);
    mw_register(d, c->id, MW_OBJ_CURSOR, c);
    return c->id;
}

Cursor XCreateGlyphCursor(Display *d, Font source_font, Font mask_font,
                          unsigned int source_char, unsigned int mask_char,
                          const XColor *foreground, const XColor *background)
{
    (void)source_font; (void)mask_font; (void)mask_char;
    MwCursor *c = calloc(1, sizeof *c);
    c->id = mw_alloc_id(d);
    c->shape = source_char;
    if (foreground) c->fg = 0xff000000u | (uint32_t)(foreground->pixel & 0xffffff);
    if (background) c->bg = 0xff000000u | (uint32_t)(background->pixel & 0xffffff);
    XDisplayImpl *dp = MWD(d);
    if (dp->cursor_theme) {
        struct wl_cursor *wc = wl_cursor_theme_get_cursor(dp->cursor_theme,
                                                          cursor_name_for(source_char));
        c->wl = wc;
    }
    mw_register(d, c->id, MW_OBJ_CURSOR, c);
    return c->id;
}

int XFreeCursor(Display *d, Cursor cursor)
{
    MwCursor *c = mw_cursor(d, cursor);
    if (c) {
        if (c->img_buffer) wl_buffer_destroy(c->img_buffer);
        free(c->img_pixels);
        mw_unregister(d, cursor);
        free(c);
    }
    return 1;
}

/* An ARGB cursor buffer for an XcursorImage (no libwayland-cursor theme). */
static struct wl_buffer *make_argb_buffer(Display *d, const uint32_t *pix,
                                          int w, int h)
{
    XDisplayImpl *dp = MWD(d);
    if (!dp->wl_shm || w <= 0 || h <= 0) return NULL;
    size_t size = (size_t)w * h * 4;
    int fd = memfd_create("mw-cursor", MFD_CLOEXEC);
    if (fd < 0) return NULL;
    if (ftruncate(fd, (off_t)size) < 0) { close(fd); return NULL; }
    void *map = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (map == MAP_FAILED) { close(fd); return NULL; }
    memcpy(map, pix, size);
    struct wl_shm_pool *pool = wl_shm_create_pool(dp->wl_shm, fd, (int32_t)size);
    struct wl_buffer *buf = wl_shm_pool_create_buffer(pool, 0, w, h, w * 4,
                                                      WL_SHM_FORMAT_ARGB8888);
    wl_shm_pool_destroy(pool);
    munmap(map, size);
    close(fd);
    return buf;
}

static void apply_cursor(Display *d, MwCursor *c)
{
    XDisplayImpl *dp = MWD(d);
    /* Resolve an XFixesChangeCursor alias (a theme change replaced this
     * cursor's image). */
    while (c && c->alias_to != None) {
        MwCursor *n = mw_cursor(d, c->alias_to);
        if (!n || n == c) break;
        c = n;
    }
    if (!dp->wl_pointer) return;
    if (c && c->img_pixels) {
        if (!c->img_buffer)
            c->img_buffer = make_argb_buffer(d, c->img_pixels, c->img_w, c->img_h);
        if (c->img_buffer) {
            if (!c->surface)
                c->surface = wl_compositor_create_surface(dp->wl_compositor);
            wl_surface_attach(c->surface, c->img_buffer, 0, 0);
            wl_surface_damage(c->surface, 0, 0, c->img_w, c->img_h);
            wl_surface_commit(c->surface);
            wl_pointer_set_cursor(dp->wl_pointer, dp->ptr_enter_serial,
                                  c->surface, c->img_hx, c->img_hy);
            return;
        }
    }
    if ((!c || !c->wl) && dp->cursor_theme) {
        /* A cursor the theme does not provide, or a pixmap cursor (which we
         * cannot render), must not blank the pointer: fall back to the default
         * arrow. */
        if (!dp->default_cursor) {
            struct wl_cursor *def = wl_cursor_theme_get_cursor(dp->cursor_theme,
                                                               "left_ptr");
            if (def) {
                MwCursor *dc = calloc(1, sizeof *dc);
                dc->id = mw_alloc_id(d);
                dc->wl = def;
                mw_register(d, dc->id, MW_OBJ_CURSOR, dc);
                dp->default_cursor = dc;
            }
        }
        if (dp->default_cursor) c = dp->default_cursor;
    }
    if (!c || !c->wl) {
        wl_pointer_set_cursor(dp->wl_pointer, dp->ptr_enter_serial, NULL, 0, 0);
        return;
    }
    struct wl_cursor_image *img = c->wl->images[0];
    if (!c->surface)
        c->surface = wl_compositor_create_surface(dp->wl_compositor);
    struct wl_buffer *buf = wl_cursor_image_get_buffer(img);
    wl_surface_attach(c->surface, buf, 0, 0);
    wl_surface_damage(c->surface, 0, 0, (int32_t)img->width, (int32_t)img->height);
    wl_surface_commit(c->surface);
    wl_pointer_set_cursor(dp->wl_pointer, dp->ptr_enter_serial, c->surface,
                          img->hotspot_x, img->hotspot_y);
}

int XDefineCursor(Display *d, Window w, Cursor cursor)
{
    MwWindow *win = mw_window(d, w);
    if (!win) return 0;
    win->cursor = cursor;
    apply_cursor(d, mw_cursor(d, cursor));
    return 1;
}

int XUndefineCursor(Display *d, Window w)
{
    MwWindow *win = mw_window(d, w);
    if (win) win->cursor = None;
    apply_cursor(d, NULL);
    return 1;
}

int XRecolorCursor(Display *d, Cursor cursor, XColor *foreground, XColor *background)
{
    MwCursor *c = mw_cursor(d, cursor);
    if (c) {
        if (foreground) c->fg = 0xff000000u | (uint32_t)(foreground->pixel & 0xffffff);
        if (background) c->bg = 0xff000000u | (uint32_t)(background->pixel & 0xffffff);
    }
    return 1;
}

int XQueryBestCursor(Display *d, Drawable dr, unsigned int width,
                     unsigned int height, unsigned int *w_ret, unsigned int *h_ret)
{
    (void)d; (void)dr;
    if (w_ret) *w_ret = width;
    if (h_ret) *h_ret = height;
    return 1;
}

void mw_set_pointer_cursor(Display *d, Cursor cursor)
{
    apply_cursor(d, mw_cursor(d, cursor));
}

/* Apply the cursor of the window under the pointer, or of its nearest ancestor
 * that has one.  X applies a window's cursor whenever the pointer is inside it,
 * so this has to be re-evaluated as the pointer moves; the shim previously only
 * applied a cursor when the client called XDefineCursor, so a pointer moving
 * from one widget to another kept the first widget's cursor. */
void mw_pointer_update_cursor(Display *d, MwWindow *w)
{
    MwCursor *c = NULL;
    for (MwWindow *n = w; n; n = n->parent) {
        if (n->cursor != None) { c = mw_cursor(d, n->cursor); break; }
    }
    apply_cursor(d, c);
}

/* ------------------------------------------------------ Xcursor (libXcursor) */

static void cursor_theme_reload(Display *d)
{
    XDisplayImpl *dp = MWD(d);
    if (dp->default_cursor) {
        mw_unregister(d, dp->default_cursor->id);
        if (dp->default_cursor->img_buffer)
            wl_buffer_destroy(dp->default_cursor->img_buffer);
        free(dp->default_cursor->img_pixels);
        free(dp->default_cursor);
        dp->default_cursor = NULL;
    }
    if (dp->cursor_theme) {
        wl_cursor_theme_destroy(dp->cursor_theme);
        dp->cursor_theme = NULL;
    }
    if (!dp->wl_shm) return;
    int size = dp->xcursor_size > 0 ? dp->xcursor_size : 24;
    const char *theme = (dp->xcursor_theme && *dp->xcursor_theme)
                            ? dp->xcursor_theme : NULL;
    dp->cursor_theme = wl_cursor_theme_load(theme, size, dp->wl_shm);
    dp->cursor_theme_size = size;
}

Cursor mw_cursor_load_named(Display *d, const char *name)
{
    XDisplayImpl *dp = MWD(d);
    if (!dp->cursor_theme || !name || !*name) return None;
    struct wl_cursor *wc = wl_cursor_theme_get_cursor(dp->cursor_theme, name);
    if (!wc) return None;
    MwCursor *c = calloc(1, sizeof *c);
    if (!c) return None;
    c->id = mw_alloc_id(d);
    c->wl = wc;
    mw_register(d, c->id, MW_OBJ_CURSOR, c);
    return c->id;
}

/* GDK passes a GdkCursorType, whose values are the X cursor-font glyph
 * numbers, so reuse the glyph table. */
Cursor mw_cursor_load_shape(Display *d, unsigned int shape)
{
    return mw_cursor_load_named(d, cursor_name_for(shape));
}

Cursor mw_cursor_from_image(Display *d, int w, int h, int xhot, int yhot,
                            const uint32_t *pixels)
{
    if (w <= 0 || h <= 0 || !pixels) return None;
    MwCursor *c = calloc(1, sizeof *c);
    if (!c) return None;
    c->id = mw_alloc_id(d);
    c->img_w = w; c->img_h = h; c->img_hx = xhot; c->img_hy = yhot;
    c->img_pixels = malloc((size_t)w * h * 4);
    if (!c->img_pixels) { free(c); return None; }
    memcpy(c->img_pixels, pixels, (size_t)w * h * 4);
    mw_register(d, c->id, MW_OBJ_CURSOR, c);
    return c->id;
}

char *mw_cursor_theme_get(Display *d)
{
    XDisplayImpl *dp = MWD(d);
    return dp->xcursor_theme ? strdup(dp->xcursor_theme) : NULL;
}

int mw_cursor_size_get(Display *d)
{
    XDisplayImpl *dp = MWD(d);
    return dp->xcursor_size > 0 ? dp->xcursor_size : 24;
}

void mw_cursor_theme_set(Display *d, const char *theme)
{
    XDisplayImpl *dp = MWD(d);
    free(dp->xcursor_theme);
    dp->xcursor_theme = (theme && *theme) ? strdup(theme) : NULL;
    cursor_theme_reload(d);
}

void mw_cursor_size_set(Display *d, int size)
{
    XDisplayImpl *dp = MWD(d);
    if (size <= 0) return;
    dp->xcursor_size = size;
    cursor_theme_reload(d);
}
