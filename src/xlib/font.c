/* font.c — X core font emulation over FreeType/fontconfig + text drawing. */
#include "internal.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

static void fill_font_struct(Display *d, MwXFont *xf, XFontStruct *fs);

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
    fill_font_struct(d, xf, fs);
    xf->fs = fs;
    mw_register(d, xf->id, MW_OBJ_FONT, xf);
    return xf;
}

static void destroy_xfont(Display *d, MwXFont *xf)
{
    mw_unregister(d, xf->id);
    if (xf->fs) { free(xf->fs->per_char); free(xf->fs); }
    if (xf->rfont) mw_font_destroy(xf->rfont);
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

static void fill_font_struct(Display *d, MwXFont *xf, XFontStruct *fs)
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
    fs->ascent = xf->ascent;
    fs->descent = xf->descent;

    int minl = 0, maxr = 0, maxw = 0, maxasc = 0, maxdesc = 0;
    XCharStruct *pc = fs->per_char;
    for (int c = 0; c < 256; c++) {
        int w = mw_font_char_width(xf->rfont, (unsigned)c);
        int lb = mw_font_char_lbearing(xf->rfont, (unsigned)c);
        int rb = mw_font_char_rbearing(xf->rfont, (unsigned)c);
        pc[c].lbearing = (short)lb;
        pc[c].rbearing = (short)(lb + rb);
        pc[c].width = (short)w;
        pc[c].ascent = (short)xf->ascent;
        pc[c].descent = (short)xf->descent;
        pc[c].attributes = 0;
        if (c == 'x') { minl = lb; maxr = pc[c].rbearing; maxw = w;
                        maxasc = xf->ascent; maxdesc = xf->descent; }
    }
    fs->min_bounds = pc['x'];
    fs->max_bounds = pc['x'];
    fs->max_bounds.lbearing = (short)minl;
    fs->min_bounds.lbearing = (short)minl;
    fs->max_bounds.rbearing = (short)maxr;
    fs->max_bounds.width = (short)maxw;
    fs->max_bounds.ascent = (short)maxasc;
    fs->max_bounds.descent = (short)maxdesc;
    fs->min_bounds.width = (short)mw_font_char_width(xf->rfont, ' ');
    fs->min_bounds.lbearing = 0;
    fs->min_bounds.rbearing = fs->min_bounds.width;
    fs->min_bounds.ascent = 0;
    fs->min_bounds.descent = 0;
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

int XTextExtents(XFontStruct *fs, _Xconst char *s, int n, int *dir,
                 int *font_ascent, int *font_descent, XCharStruct *overall)
{
    if (!fs) return 0;
    if (dir) *dir = FontLeftToRight;
    if (font_ascent) *font_ascent = fs->ascent;
    if (font_descent) *font_descent = fs->descent;
    if (overall) {
        memset(overall, 0, sizeof *overall);
        int w = 0, maxr = 0;
        for (int i = 0; i < n; i++) {
            unsigned char c = (unsigned char)s[i];
            XCharStruct *pc = fs->per_char ? &fs->per_char[c] : &fs->max_bounds;
            w += pc->width;
            if (pc->rbearing > maxr) maxr = pc->rbearing;
            if (pc->ascent > overall->ascent) overall->ascent = pc->ascent;
            if (pc->descent > overall->descent) overall->descent = pc->descent;
        }
        overall->width = (short)w;
        overall->rbearing = (short)maxr;
    }
    return 1;
}

int XTextExtents16(XFontStruct *fs, _Xconst XChar2b *s, int n, int *dir,
                   int *font_ascent, int *font_descent, XCharStruct *overall)
{
    (void)s; (void)n;
    if (dir) *dir = FontLeftToRight;
    if (font_ascent) *font_ascent = fs ? fs->ascent : 0;
    if (font_descent) *font_descent = fs ? fs->descent : 0;
    if (overall) memset(overall, 0, sizeof *overall);
    return 1;
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
    if (fill_bg && gc->xfont && gc->xfont->rfont) {
        int tw = mw_font_text_width_utf8(gc->xfont->rfont, (const char *)utf8, len);
        if (tw < 1) tw = 1;
        mw_set_source_argb(c, 0xff000000u | (uint32_t)(gc->background & 0xffffff));
        mw_rect(c, x - 1, y - gc->xfont->ascent - 1,
                tw + 2, gc->xfont->ascent + gc->xfont->descent + 2);
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
        if (tw < 1) tw = 1;
        mw_set_source_argb(c, 0xff000000u | (uint32_t)(gc->background & 0xffffff));
        mw_rect(c, x - 1, y - (gc->xfont ? gc->xfont->ascent : 0) - 1,
                tw + 2, (gc->xfont ? gc->xfont->ascent + gc->xfont->descent : 0) + 2);
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

int XDrawString16(Display *d, Drawable dr, GC gc, int x, int y,
                  _Xconst XChar2b *s, int length)
{
    char *buf = calloc((size_t)length * 4 + 1, 1);
    int o = 0;
    for (int i = 0; i < length; i++) {
        unsigned int c = ((unsigned)s[i].byte1 << 8) | s[i].byte2;
        char tmp[8]; int n;
        char2b_to_utf8(c, tmp, &n);
        memcpy(buf + o, tmp, n); o += n;
    }
    int r = draw_glyphs(d, dr, gc, x, y, (unsigned char *)buf, o, 0);
    free(buf);
    return r;
}

int XDrawImageString16(Display *d, Drawable dr, GC gc, int x, int y,
                       _Xconst XChar2b *s, int length)
{
    char *buf = calloc((size_t)length * 4 + 1, 1);
    int o = 0;
    for (int i = 0; i < length; i++) {
        unsigned int c = ((unsigned)s[i].byte1 << 8) | s[i].byte2;
        char tmp[8]; int n;
        char2b_to_utf8(c, tmp, &n);
        memcpy(buf + o, tmp, n); o += n;
    }
    int r = draw_glyphs(d, dr, gc, x, y, (unsigned char *)buf, o, 1);
    free(buf);
    return r;
}

int XDrawText16(Display *d, Drawable dr, GC gc, int x, int y,
                XTextItem16 *items, int nitems)
{
    for (int i = 0; i < nitems; i++) {
        if (items[i].nchars > 0)
            XDrawString16(d, dr, gc, x, y, items[i].chars, items[i].nchars);
        x += XTextWidth16(gc->xfont ? gc->xfont->fs : NULL,
                          items[i].chars, items[i].nchars) + items[i].delta;
    }
    return 1;
}

/* ------------------------------------------------------------- font sets */

XFontSet XCreateFontSet(Display *d, _Xconst char *base_font_name_list,
                        char ***missing_charset_list, int *missing_charset_count,
                        char **def_string)
{
    MwXFont *xf = load_xfont(d, base_font_name_list, true);
    if (missing_charset_list) *missing_charset_list = NULL;
    if (missing_charset_count) *missing_charset_count = 0;
    if (def_string) *def_string = NULL;
    return (XFontSet)xf;
}

void XFreeFontSet(Display *d, XFontSet fs)
{
    (void)d; (void)fs;
}

int XFontsOfFontSet(XFontSet fs, XFontStruct ***font_struct_list,
                    char ***font_name_list)
{
    MwXFont *xf = (MwXFont *)fs;
    XFontStruct **s = malloc(sizeof(XFontStruct *));
    s[0] = xf ? xf->fs : NULL;
    if (font_struct_list) *font_struct_list = s;
    if (font_name_list) *font_name_list = NULL;
    return 1;
}

char *XBaseFontNameListOfFontSet(XFontSet fs) { (void)fs; return NULL; }

XFontSetExtents *XExtentsOfFontSet(XFontSet fs)
{
    static XFontSetExtents ext;
    MwXFont *xf = (MwXFont *)fs;
    ext.max_logical_extent.x = 0;
    ext.max_logical_extent.y = (short)(xf ? -xf->ascent : 0);
    ext.max_logical_extent.width = (unsigned short)(xf ? xf->max_width : 0);
    ext.max_logical_extent.height = (unsigned short)(xf ? xf->ascent + xf->descent : 0);
    ext.max_ink_extent = ext.max_logical_extent;
    return &ext;
}
