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
    /* Reuse if the same name is already loaded. */
    XDisplayImpl *dp = MWD(d);
    for (size_t i = 0; i < dp->table_cap; i++) {
        if (dp->table[i].id && dp->table[i].kind == MW_OBJ_FONT) {
            MwXFont *e = dp->table[i].obj;
            if (e && e->fs && e->fs->per_char) {
                /* match by name stored in properties? compare approximations */
            }
        }
    }
    MwFont *rf = mw_font_create(name, 0);
    if (!rf) return NULL;
    MwXFont *xf = calloc(1, sizeof *xf);
    xf->id = mw_alloc_id(d);
    xf->rfont = rf;
    xf->ascent = mw_font_ascent(rf);
    xf->descent = mw_font_descent(rf);
    xf->max_width = mw_font_max_width(rf);
    XFontStruct *fs = calloc(1, sizeof *fs);
    fs->fid = xf->id;
    fill_font_struct(d, xf, fs);
    xf->fs = fs;
    mw_register(d, xf->id, MW_OBJ_FONT, xf);
    return xf;
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
    mw_unregister(d, fs->fid);
    free(fs->per_char);
    free(fs);
    return 1;
}

int XUnloadFont(Display *d, Font font)
{
    MwXFont *xf = mw_font(d, font);
    if (!xf) return 0;
    if (xf->fs) { free(xf->fs->per_char); free(xf->fs); }
    if (xf->rfont) mw_font_destroy(xf->rfont);
    mw_unregister(d, font);
    free(xf);
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
    char **out = calloc(n, sizeof(char *));
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
    gc->font = font;
    gc->xfont = mw_font(d, font);
    return 1;
}

static int draw_glyphs(Display *d, Drawable dr, GC gc, int x, int y,
                       const unsigned char *utf8, int len, int fill_bg)
{
    int w, h;
    MwCanvas *c = mw_canvas_for_drawable(d, dr, gc, &w, &h);
    if (!c) return 0;
    if (fill_bg) {
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

int XDrawString(Display *d, Drawable dr, GC gc, int x, int y,
                _Xconst char *s, int length)
{ return draw_glyphs(d, dr, gc, x, y, (const unsigned char *)s, length, 0); }

int XDrawImageString(Display *d, Drawable dr, GC gc, int x, int y,
                     _Xconst char *s, int length)
{ return draw_glyphs(d, dr, gc, x, y, (const unsigned char *)s, length, 1); }

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
