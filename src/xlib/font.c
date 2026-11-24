/* font.c — X core font emulation over FreeType/fontconfig + text drawing. */
#include "internal.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

static bool fill_font_struct(Display *d, MwXFont *xf, XFontStruct *fs);

static MwXFont *load_xfont(Display *d, const char *name, bool query)
{
    (void)query;
    if (!name) name = "fixed";
    MwFont *rf = mw_font_create(name, 0);
    if (!rf) return NULL;
    MwXFont *xf = calloc(1, sizeof *xf);
    XFontStruct *fs = calloc(1, sizeof *fs);
    if (!xf || !fs) { free(xf); free(fs); mw_font_destroy(rf); return NULL; }
    xf->id = mw_alloc_id(d);
    xf->rfont = rf;
    xf->ascent = mw_font_ascent(rf);
    xf->descent = mw_font_descent(rf);
    xf->max_width = mw_font_max_width(rf);
    fs->fid = xf->id;
    if (!fill_font_struct(d, xf, fs)) {
        free(xf); free(fs); mw_font_destroy(rf); return NULL;
    }
    xf->fs = fs;
    mw_register(d, xf->id, MW_OBJ_FONT, xf);
    return xf;
}

static void destroy_xfont(Display *d, MwXFont *xf)
{
    mw_unregister(d, xf->id);
    if (xf->fs) { free(xf->fs->per_char); free(xf->fs); }
    if (xf->rfont) mw_font_destroy(xf->rfont);
    free(xf->fs_base_name);
    free(xf);
}

void mw_font_unref(Display *d, MwXFont *xf)
{
    if (!xf) return;
    if (--xf->refs <= 0 && xf->released) destroy_xfont(d, xf);
}

/* Point a GC at a font, holding a reference so the font survives its ID
 * being released while the GC still draws with it. */
void mw_gc_set_xfont(Display *d, GC gc, MwXFont *xf)
{
    if (xf) xf->refs++;
    MwXFont *old = gc->xfont;
    gc->xfont = xf;
    gc->font = xf ? xf->id : None;
    mw_font_unref(d, old);
}

/* The font a GC gets when none is given: one per display, not one per GC
 * (each load measures 256 glyphs three times).  The display holds a
 * reference; if the client released its ID, a fresh one is loaded. */
MwXFont *mw_default_font(Display *d)
{
    XDisplayImpl *dp = MWD(d);
    if (dp->default_font && dp->default_font->released) {
        mw_font_unref(d, dp->default_font);
        dp->default_font = NULL;
    }
    if (!dp->default_font) {
        dp->default_font = load_xfont(d, "fixed", false);
        if (dp->default_font) dp->default_font->refs++;
    }
    return dp->default_font;
}

static void release_font_id(Display *d, Font id)
{
    MwXFont *xf = mw_lookup(d, id, MW_OBJ_FONT);
    if (!xf || xf->released) return;
    xf->released = true;
    if (xf->refs <= 0) destroy_xfont(d, xf);
}

static bool fill_font_struct(Display *d, MwXFont *xf, XFontStruct *fs)
{
    (void)d;
    fs->ext_data = NULL;
    fs->direction = FontLeftToRight;
    fs->min_byte1 = 0;
    fs->max_byte1 = 0;
    fs->min_char_or_byte2 = 0;
    fs->max_char_or_byte2 = 255;
    fs->all_chars_exist = True;
    fs->default_char = '?';
    fs->n_properties = 0;
    fs->properties = NULL;
    fs->per_char = calloc(256, sizeof(XCharStruct));
    if (!fs->per_char) return false;
    fs->ascent = xf->ascent;
    fs->descent = xf->descent;

    XCharStruct *pc = fs->per_char;
    for (int c = 0; c < 256; c++) {
        int w = 0, lb = 0, rsb = 0;
        mw_font_char_metrics(xf->rfont, (unsigned)c, &w, &lb, &rsb);
        pc[c].lbearing = (short)lb;
        /* X's rbearing is where the ink ends, measured from the origin:
         * the advance less the right side bearing. */
        pc[c].rbearing = (short)(w - rsb);
        if (pc[c].rbearing < pc[c].lbearing) pc[c].rbearing = pc[c].lbearing;
        pc[c].width = (short)w;
        pc[c].ascent = (short)xf->ascent;
        pc[c].descent = (short)xf->descent;
        pc[c].attributes = 0;
    }
    /* The bounds are the per-field minimum and maximum over every
     * character (they used to be copied from 'x'). */
    fs->min_bounds = pc[0];
    fs->max_bounds = pc[0];
    for (int c = 1; c < 256; c++) {
#define MW_BOUND(f) do { \
        if (pc[c].f < fs->min_bounds.f) fs->min_bounds.f = pc[c].f; \
        if (pc[c].f > fs->max_bounds.f) fs->max_bounds.f = pc[c].f; } while (0)
        MW_BOUND(lbearing); MW_BOUND(rbearing); MW_BOUND(width);
        MW_BOUND(ascent); MW_BOUND(descent);
#undef MW_BOUND
    }
    return true;
}

Font XLoadFont(Display *d, _Xconst char *name)
{
    MwXFont *xf = load_xfont(d, name, false);
    return xf ? xf->id : None;
}

XFontStruct *XLoadQueryFont(Display *d, _Xconst char *name)
{
    MwXFont *xf = load_xfont(d, name, true);
    return xf ? xf->fs : NULL;
}

XFontStruct *XQueryFont(Display *d, XID font_ID)
{
    MwXFont *xf = mw_font(d, font_ID);
    /* As on a real server, a GContext (XGContextFromGC) names the GC's
     * font. */
    if (!xf) {
        struct _XGC *gc = mw_lookup(d, font_ID, MW_OBJ_GC);
        if (gc) xf = gc->xfont;
    }
    return xf ? xf->fs : NULL;
}

int XFreeFont(Display *d, XFontStruct *fs)
{
    if (!fs) return 0;
    release_font_id(d, fs->fid);
    return 1;
}

int XUnloadFont(Display *d, Font font)
{
    if (!mw_font(d, font)) return 0;
    release_font_id(d, font);
    return 1;
}

char **XListFonts(Display *d, _Xconst char *pattern, int maxnames, int *count)
{
    (void)d; (void)pattern;
    const char *names[] = {
        "-misc-fixed-medium-r-normal--13-120-75-75-c-70-iso8859-1",
        "-misc-fixed-medium-r-normal--14-130-75-75-c-70-iso8859-1",
        "-adobe-helvetica-medium-r-normal--12-120-75-75-p-67-iso8859-1",
        "fixed",
    };
    int n = (int)(sizeof(names) / sizeof(names[0]));
    if (maxnames > 0 && n > maxnames) n = maxnames;
    /* NULL-terminated: XFreeFontNames walks the list to its NULL. */
    char **out = calloc((size_t)n + 1, sizeof(char *));
    if (!out) { *count = 0; return NULL; }
    for (int i = 0; i < n; i++) out[i] = strdup(names[i]);
    *count = n;
    return out;
}

int XFreeFontNames(char **list)
{
    if (!list) return 0;
    for (char **p = list; *p; p++) free(*p);
    free(list);
    return 1;
}

char **XListFontsWithInfo(Display *d, _Xconst char *pattern, int maxnames,
                          int *count, XFontStruct **info)
{
    (void)d; (void)pattern; (void)maxnames; (void)info;
    if (count) *count = 0;
    return NULL;
}

Bool XGetFontProperty(XFontStruct *fs, Atom atom, unsigned long *value)
{
    (void)atom;
    if (!fs) return False;
    if (value) *value = 0;
    return False;
}

/* --------------------------------------------------------------- metrics */

int XTextWidth(XFontStruct *fs, _Xconst char *s, int n)
{
    if (!fs) return 0;
    int total = 0;
    for (int i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        total += fs->per_char ? fs->per_char[c].width : fs->max_bounds.width;
    }
    return total;
}

int XTextWidth16(XFontStruct *fs, _Xconst XChar2b *s, int n)
{
    if (!fs) return 0;
    int total = 0;
    for (int i = 0; i < n; i++) {
        unsigned int c = ((unsigned)s[i].byte1 << 8) | s[i].byte2;
        total += (c < 256 && fs->per_char) ? fs->per_char[c].width
                                           : fs->max_bounds.width;
    }
    return total;
}

/* The overall extents of a run, as libX11 computes them: each character
 * sits at its pen position, so lbearing/rbearing are the extreme ink edges
 * over the run, width the sum of the advances. */
static XCharStruct *char_info(XFontStruct *fs, unsigned int c)
{
    if (fs->per_char && c >= fs->min_char_or_byte2 && c <= fs->max_char_or_byte2)
        return &fs->per_char[c - fs->min_char_or_byte2];
    return &fs->max_bounds;
}

static void run_extents(XFontStruct *fs, const unsigned int *cs, int n,
                        XCharStruct *overall)
{
    memset(overall, 0, sizeof *overall);
    int x = 0;
    for (int i = 0; i < n; i++) {
        XCharStruct *pc = char_info(fs, cs[i]);
        if (i == 0 || x + pc->lbearing < overall->lbearing)
            overall->lbearing = (short)(x + pc->lbearing);
        if (i == 0 || x + pc->rbearing > overall->rbearing)
            overall->rbearing = (short)(x + pc->rbearing);
        if (i == 0 || pc->ascent > overall->ascent) overall->ascent = pc->ascent;
        if (i == 0 || pc->descent > overall->descent) overall->descent = pc->descent;
        x += pc->width;
    }
    overall->width = (short)x;
}

static int text_extents(XFontStruct *fs, const unsigned int *cs, int n, int *dir,
                        int *font_ascent, int *font_descent, XCharStruct *overall)
{
    if (dir) *dir = FontLeftToRight;
    if (font_ascent) *font_ascent = fs ? fs->ascent : 0;
    if (font_descent) *font_descent = fs ? fs->descent : 0;
    if (overall) {
        if (fs) run_extents(fs, cs, n, overall);
        else memset(overall, 0, sizeof *overall);
    }
    return 1;
}

int XTextExtents(XFontStruct *fs, _Xconst char *s, int n, int *dir,
                 int *font_ascent, int *font_descent, XCharStruct *overall)
{
    if (n <= 0 || !s) return text_extents(fs, NULL, 0, dir, font_ascent, font_descent, overall);
    unsigned int stackbuf[256], *cs = n <= 256 ? stackbuf : malloc(sizeof *cs * (size_t)n);
    if (!cs) return 0;
    for (int i = 0; i < n; i++) cs[i] = (unsigned char)s[i];
    int r = text_extents(fs, cs, n, dir, font_ascent, font_descent, overall);
    if (cs != stackbuf) free(cs);
    return r;
}

/* The core fonts here cover one byte of characters: a 16-bit character
 * with a non-zero first byte measures as the font's default (max) cell. */
int XTextExtents16(XFontStruct *fs, _Xconst XChar2b *s, int n, int *dir,
                   int *font_ascent, int *font_descent, XCharStruct *overall)
{
    if (n <= 0 || !s) return text_extents(fs, NULL, 0, dir, font_ascent, font_descent, overall);
    unsigned int stackbuf[256], *cs = n <= 256 ? stackbuf : malloc(sizeof *cs * (size_t)n);
    if (!cs) return 0;
    for (int i = 0; i < n; i++)
        cs[i] = s[i].byte1 ? 0x10000u : s[i].byte2;
    int r = text_extents(fs, cs, n, dir, font_ascent, font_descent, overall);
    if (cs != stackbuf) free(cs);
    return r;
}

/* --------------------------------------------------------------- drawing */

int XSetFont(Display *d, GC gc, Font font)
{
    mw_gc_set_xfont(d, gc, mw_font(d, font));
    return 1;
}

static int draw_glyphs(Display *d, Drawable dr, GC gc, int x, int y,
                       const unsigned char *utf8, int len, int fill_bg)
{
    int w, h;
    MwCanvas *c = mw_canvas_for_drawable(d, dr, gc, &w, &h);
    if (!c) return 0;
    /* Image text always draws with GXcopy and a solid fill (only the plane
     * mask applies), and its background is exactly the text's cell box:
     * [x, x + width) x [y - ascent, y + descent). */
    if (fill_bg) mw_set_operator(c, GXcopy);
    if (fill_bg && gc->xfont && gc->xfont->rfont) {
        int tw = mw_font_text_width_utf8(gc->xfont->rfont, (const char *)utf8, len);
        mw_set_source_argb(c, 0xff000000u | (uint32_t)(gc->background & 0xffffff));
        mw_rect(c, x, y - gc->xfont->ascent,
                tw, gc->xfont->ascent + gc->xfont->descent);
        mw_fill_path(c);
        mw_set_source_argb(c, 0xff000000u | (uint32_t)(gc->foreground & 0xffffff));
    } else if (fill_bg) {
        /* XDrawImageString paints the string's background over its full text
         * extent before drawing the glyphs.  The extent is a pixel width, not
         * the byte count: using the byte count under-filled badly ("a" erased
         * 3px of a 9px cell), and Motif relies on this to erase the old text
         * cursor -- so typing left the cursor's remains behind as the
         * insertion point advanced. */
        int tw = 0;
        if (gc->xfont && gc->xfont->fs) {
            XFontStruct *fs = gc->xfont->fs;
            for (int i = 0; i < len; i++)
                tw += fs->per_char
                    ? fs->per_char[(unsigned char)utf8[i]].width
                    : fs->max_bounds.width;
        }
        mw_set_source_argb(c, 0xff000000u | (uint32_t)(gc->background & 0xffffff));
        mw_rect(c, x, y - (gc->xfont ? gc->xfont->ascent : 0),
                tw, gc->xfont ? gc->xfont->ascent + gc->xfont->descent : 0);
        mw_fill_path(c);
        mw_set_source_argb(c, 0xff000000u | (uint32_t)(gc->foreground & 0xffffff));
    }
    if (getenv("MW_TRACE"))
        fprintf(stderr, "MW: core-draw %d bytes drawable=%dx%d '%.*s'\n",
                len, w, h, len > 12 ? 12 : len, (const char *)utf8);
    if (gc->xfont && gc->xfont->rfont)
        mw_show_utf8(c, gc->xfont->rfont, (const char *)utf8, len, x, y);
    mw_canvas_end(c);
    MwWindow *win = mw_window(d, dr);
    if (win) mw_window_damage(win);
    return 1;
}

static const char *char2b_to_utf8(unsigned int c, char *buf, int *out)
{
    if (c < 0x80) { buf[0] = (char)c; *out = 1; }
    else if (c < 0x800) {
        buf[0] = (char)(0xC0 | (c >> 6)); buf[1] = (char)(0x80 | (c & 0x3F));
        *out = 2;
    } else {
        buf[0] = (char)(0xE0 | (c >> 12));
        buf[1] = (char)(0x80 | ((c >> 6) & 0x3F));
        buf[2] = (char)(0x80 | (c & 0x3F));
        *out = 3;
    }
    buf[*out] = 0;
    return buf;
}

/* Core 8-bit text is Latin-1 (the core fonts are ISO 8859-1): convert it to
 * UTF-8 for the raster, or a byte like 0xE9 (é) would be taken as the lead of
 * a multibyte sequence and swallow the characters after it. */
static int draw_latin1(Display *d, Drawable dr, GC gc, int x, int y,
                       const char *s, int length, int fill_bg)
{
    if (length <= 0) return 1;
    unsigned char stackbuf[512], *buf = stackbuf;
    if ((size_t)length * 2 > sizeof stackbuf) {
        buf = malloc((size_t)length * 2);
        if (!buf) return 0;
    }
    int o = 0;
    for (int i = 0; i < length; i++) {
        unsigned char ch = (unsigned char)s[i];
        if (ch < 0x80) buf[o++] = ch;
        else { buf[o++] = (unsigned char)(0xc0 | (ch >> 6));
               buf[o++] = (unsigned char)(0x80 | (ch & 0x3f)); }
    }
    int r = draw_glyphs(d, dr, gc, x, y, buf, o, fill_bg);
    if (buf != stackbuf) free(buf);
    return r;
}

/* UTF-8 text for the Xmb/Xutf8 entry points. */
int mw_draw_utf8(Display *d, Drawable dr, GC gc, int x, int y,
                 const char *s, int length, int fill_bg)
{
    return draw_glyphs(d, dr, gc, x, y, (const unsigned char *)s, length, fill_bg);
}

int XDrawString(Display *d, Drawable dr, GC gc, int x, int y,
                _Xconst char *s, int length)
{ return draw_latin1(d, dr, gc, x, y, s, length, 0); }

int XDrawImageString(Display *d, Drawable dr, GC gc, int x, int y,
                     _Xconst char *s, int length)
{ return draw_latin1(d, dr, gc, x, y, s, length, 1); }

static int draw_string16_common(Display *d, Drawable dr, GC gc, int x, int y,
                                _Xconst XChar2b *s, int length, int fill_bg)
{
    if (!s || length <= 0) return 0;
    char stack_buf[256];
    char *buf = (size_t)length * 4 + 1 <= sizeof stack_buf
        ? stack_buf : malloc((size_t)length * 4 + 1);
    if (!buf) return 0;
    int o = 0;
    for (int i = 0; i < length; i++) {
        unsigned int c = ((unsigned)s[i].byte1 << 8) | s[i].byte2;
        char tmp[8]; int n;
        char2b_to_utf8(c, tmp, &n);
        memcpy(buf + o, tmp, (size_t)n); o += n;
    }
    buf[o] = 0;
    int r = draw_glyphs(d, dr, gc, x, y, (unsigned char *)buf, o, fill_bg);
    if (buf != stack_buf) free(buf);
    return r;
}

int XDrawString16(Display *d, Drawable dr, GC gc, int x, int y,
                  _Xconst XChar2b *s, int length)
{ return draw_string16_common(d, dr, gc, x, y, s, length, 0); }

int XDrawImageString16(Display *d, Drawable dr, GC gc, int x, int y,
                       _Xconst XChar2b *s, int length)
{ return draw_string16_common(d, dr, gc, x, y, s, length, 1); }

int XDrawText16(Display *d, Drawable dr, GC gc, int x, int y,
                XTextItem16 *items, int nitems)
{
    if (!items || nitems <= 0) return 1;
    for (int i = 0; i < nitems; i++) {
        if (items[i].font != None) XSetFont(d, gc, items[i].font);
        x += items[i].delta;
        if (items[i].nchars > 0) {
            XDrawString16(d, dr, gc, x, y, items[i].chars, items[i].nchars);
            x += XTextWidth16(gc->xfont ? gc->xfont->fs : NULL,
                              items[i].chars, items[i].nchars);
        }
    }
    return 1;
}

/* ------------------------------------------------------------- font sets */

XFontSet XCreateFontSet(Display *d, _Xconst char *base_font_name_list,
                        char ***missing_charset_list, int *missing_charset_count,
                        char **def_string)
{
    if (missing_charset_list) *missing_charset_list = NULL;
    if (missing_charset_count) *missing_charset_count = 0;
    if (def_string) *def_string = (char *)"";
    MwXFont *xf = load_xfont(d, base_font_name_list, true);
    if (!xf) return NULL;
    /* One font serves the whole set; its accessors return storage kept here,
     * as libX11's do (the caller must not free them). */
    xf->fs_base_name = strdup(base_font_name_list ? base_font_name_list : "fixed");
    xf->fs_names[0] = xf->fs_base_name;
    xf->fs_structs[0] = xf->fs;
    XRectangle r = { 0, (short)-xf->ascent, (unsigned short)xf->max_width,
                     (unsigned short)(xf->ascent + xf->descent) };
    xf->fs_extents.max_logical_extent = r;
    xf->fs_extents.max_ink_extent = r;
    return (XFontSet)xf;
}

/* The set's font goes like any other: when nothing (no GC borrowing it in
 * the middle of a draw) refers to it any more. */
void XFreeFontSet(Display *d, XFontSet fs)
{
    MwXFont *xf = (MwXFont *)fs;
    if (xf) release_font_id(d, xf->id);
}

int XFontsOfFontSet(XFontSet fs, XFontStruct ***font_struct_list,
                    char ***font_name_list)
{
    MwXFont *xf = (MwXFont *)fs;
    if (!xf) {
        if (font_struct_list) *font_struct_list = NULL;
        if (font_name_list) *font_name_list = NULL;
        return 0;
    }
    if (font_struct_list) *font_struct_list = xf->fs_structs;
    if (font_name_list) *font_name_list = xf->fs_names;
    return 1;
}

char *XBaseFontNameListOfFontSet(XFontSet fs)
{
    MwXFont *xf = (MwXFont *)fs;
    return xf ? xf->fs_base_name : NULL;
}

XFontSetExtents *XExtentsOfFontSet(XFontSet fs)
{
    MwXFont *xf = (MwXFont *)fs;
    return xf ? &xf->fs_extents : NULL;
}
