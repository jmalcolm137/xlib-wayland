/* xmb.c — the internationalized text and output-method entry points of
 * libX11 (Xmb/Xwc/Xutf8 drawing, metrics, and XOM/XOC stubs).  Motif's text
 * widgets call these, so they must exist and behave sensibly; we back them
 * with the same font objects and drawing code as the core Xlib API.
 */
#include "internal.h"

#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <wchar.h>

struct _XOM { Display *display; };
struct _XOC { struct _XOM *om; };

static MwXFont *fs_font(XFontSet fs) { return (MwXFont *)fs; }

static unsigned int utf8_next(const char **sp, const char *end)
{
    const unsigned char *s = (const unsigned char *)*sp;
    if (s >= (const unsigned char *)end) return 0;
    unsigned int c = *s++;
    if (c >= 0xF0 && s + 2 < (const unsigned char *)end)
        c = ((c & 0x07) << 18) | ((s[0] & 0x3F) << 12) | ((s[1] & 0x3F) << 6) | (s[2] & 0x3F), s += 3;
    else if (c >= 0xE0 && s + 1 < (const unsigned char *)end)
        c = ((c & 0x0F) << 12) | ((s[0] & 0x3F) << 6) | (s[1] & 0x3F), s += 2;
    else if (c >= 0xC0)
        c = ((c & 0x1F) << 6) | (s[0] & 0x3F), s += 1;
    *sp = (const char *)s;
    return c;
}

static int utf8_text_width(XFontSet fs, _Xconst char *s, int n)
{
    MwXFont *f = fs_font(fs);
    if (!f || !f->rfont) return n * 6;
    if (n < 0) n = (int)strlen(s);
    return mw_font_text_width_utf8(f->rfont, s, n);
}

static int wc_text_width(XFontSet fs, _Xconst wchar_t *s, int n)
{
    MwXFont *f = fs_font(fs);
    if (!f || !f->rfont) return n * 6;
    int total = 0;
    for (int i = 0; i < n; i++) total += mw_font_char_width(f->rfont, (unsigned)s[i]);
    return total;
}

static void mb_draw(Display *d, Drawable dr, XFontSet fs, GC gc,
                    int x, int y, _Xconst char *str, int n, int image)
{
    /* The fontset names the font, and XDrawString derives everything -- the
     * advance, XTextWidth, the caller's layout -- from the GC's font.  Motif
     * lays a widget's line out from the fontset (its fontList) but draws it
     * with XmbDrawString while the GC still carries some other font, so the
     * two disagreed: the drawn advance came out wider than the measured width,
     * and the widget then erased everything past the measured extent, wiping
     * the tail of every line that ran longer than the (too small) measurement.
     * Borrow the fontset's font for the duration of the draw so the pixels and
     * the metrics always describe the same font. */
    MwXFont *ff = gc ? fs_font(fs) : NULL;
    Font     of = 0;
    MwXFont *ox = NULL;
    if (ff && ff->rfont) {
        of = gc->font; ox = gc->xfont;
        gc->font = ff->id; gc->xfont = ff;
    }
    mw_draw_utf8(d, dr, gc, x, y, str, n, image);
    if (ff && ff->rfont) { gc->font = of; gc->xfont = ox; }
}

/* ------------------------------------------------------------------ Xmb */

void XmbDrawString(Display *d, Drawable dr, XFontSet fs, GC gc, int x, int y,
                   _Xconst char *s, int n)
{ mb_draw(d, dr, fs, gc, x, y, s, n, 0); }

void XmbDrawImageString(Display *d, Drawable dr, XFontSet fs, GC gc, int x, int y,
                        _Xconst char *s, int n)
{ mb_draw(d, dr, fs, gc, x, y, s, n, 1); }

int XmbTextEscapement(XFontSet fs, _Xconst char *s, int n)
{ return utf8_text_width(fs, s, n); }

int XmbTextExtents(XFontSet fs, _Xconst char *s, int n,
                   XRectangle *ink, XRectangle *logical)
{
    int w = utf8_text_width(fs, s, n);
    MwXFont *f = fs_font(fs);
    int asc = f ? f->ascent : 10, desc = f ? f->descent : 3;
    /* ink and logical are both optional; CDE's DtTerm calls this with a NULL
     * ink and a non-NULL logical, so they must be filled independently. */
    XRectangle e = { 0, (short)-asc, (unsigned short)w,
                     (unsigned short)(asc + desc) };
    if (ink) *ink = e;
    if (logical) *logical = e;
    return w;
}

/* Shared tail of the PerCharExtents calls.  The per-character arrays hold
 * nink entries; the overall ink/logical returns are single rectangles.  As in
 * libX11, a buffer too small for every character fails with 0. */
static void per_char_put(XRectangle *ink, XRectangle *logical, int nink,
                         int i, int xoff, int cw, int asc, int desc)
{
    if (i >= nink) return;
    XRectangle r = { (short)xoff, (short)-asc, (unsigned short)cw,
                     (unsigned short)(asc + desc) };
    if (ink) ink[i] = r;
    if (logical) logical[i] = r;
}

static int per_char_finish(int count, int nink, int width, int asc, int desc,
                           int *num, XRectangle *ink_ret,
                           XRectangle *logical_ret)
{
    if (num) *num = count;
    XRectangle all = { 0, (short)-asc, (unsigned short)width,
                       (unsigned short)(asc + desc) };
    if (ink_ret) *ink_ret = all;
    if (logical_ret) *logical_ret = all;
    return count > nink ? 0 : 1;
}

int XmbTextPerCharExtents(XFontSet fs, _Xconst char *s, int n, XRectangle *ink,
                          XRectangle *logical, int nink, int *num,
                          XRectangle *ink_ret, XRectangle *logical_ret)
{
    MwXFont *f = fs_font(fs);
    int asc = f ? f->ascent : 10, desc = f ? f->descent : 3;
    int count = 0, xoff = 0;
    const char *p = s, *end = s + (n < 0 ? (int)strlen(s) : n);
    while (p < end) {
        unsigned int ch = utf8_next(&p, end);
        int cw = f && f->rfont ? mw_font_char_width(f->rfont, ch) : 6;
        per_char_put(ink, logical, nink, count, xoff, cw, asc, desc);
        xoff += cw; count++;
    }
    return per_char_finish(count, nink, xoff, asc, desc, num, ink_ret,
                           logical_ret);
}

/* ------------------------------------------------------------------ Xwc */

/* Wide text goes through the same UTF-8 path as Xmb, so it is drawn with
 * the fontset's font -- the one XwcTextEscapement measures with. */
static void wc_draw(Display *d, Drawable dr, XFontSet fs, GC gc, int x, int y,
                    _Xconst wchar_t *s, int n, int image)
{
    if (n <= 0) return;
    char *buf = malloc((size_t)n * 4 + 1);
    if (!buf) return;
    int o = 0;
    for (int i = 0; i < n; i++) {
        unsigned int c = (unsigned int)s[i];
        if (c > 0x10FFFF || (c >= 0xD800 && c <= 0xDFFF)) c = 0xFFFD;
        if (c < 0x80) buf[o++] = (char)c;
        else if (c < 0x800) { buf[o++] = (char)(0xC0|(c>>6)); buf[o++] = (char)(0x80|(c&0x3F)); }
        else if (c < 0x10000) { buf[o++] = (char)(0xE0|(c>>12)); buf[o++] = (char)(0x80|((c>>6)&0x3F)); buf[o++] = (char)(0x80|(c&0x3F)); }
        else { buf[o++] = (char)(0xF0|(c>>18)); buf[o++] = (char)(0x80|((c>>12)&0x3F)); buf[o++] = (char)(0x80|((c>>6)&0x3F)); buf[o++] = (char)(0x80|(c&0x3F)); }
    }
    mb_draw(d, dr, fs, gc, x, y, buf, o, image);
    free(buf);
}

void XwcDrawString(Display *d, Drawable dr, XFontSet fs, GC gc, int x, int y,
                   _Xconst wchar_t *s, int n)
{ wc_draw(d, dr, fs, gc, x, y, s, n, 0); }

void XwcDrawImageString(Display *d, Drawable dr, XFontSet fs, GC gc, int x, int y,
                        _Xconst wchar_t *s, int n)
{ wc_draw(d, dr, fs, gc, x, y, s, n, 1); }

int XwcTextEscapement(XFontSet fs, _Xconst wchar_t *s, int n)
{ return wc_text_width(fs, s, n); }

int XwcTextExtents(XFontSet fs, _Xconst wchar_t *s, int n,
                   XRectangle *ink, XRectangle *logical)
{
    int w = wc_text_width(fs, s, n);
    MwXFont *f = fs_font(fs);
    int asc = f ? f->ascent : 10, desc = f ? f->descent : 3;
    XRectangle e = { 0, (short)-asc, (unsigned short)w,
                     (unsigned short)(asc+desc) };
    if (ink) *ink = e;
    if (logical) *logical = e;
    return w;
}

int XwcTextPerCharExtents(XFontSet fs, _Xconst wchar_t *s, int n, XRectangle *ink,
                          XRectangle *logical, int nink, int *num,
                          XRectangle *ink_ret, XRectangle *logical_ret)
{
    MwXFont *f = fs_font(fs);
    int asc = f ? f->ascent : 10, desc = f ? f->descent : 3, xoff = 0;
    for (int i = 0; i < n; i++) {
        int cw = f && f->rfont ? mw_font_char_width(f->rfont, (unsigned)s[i]) : 6;
        per_char_put(ink, logical, nink, i, xoff, cw, asc, desc);
        xoff += cw;
    }
    return per_char_finish(n, nink, xoff, asc, desc, num, ink_ret,
                           logical_ret);
}

/* ---------------------------------------------------------------- Xutf8 */

void Xutf8DrawString(Display *d, Drawable dr, XFontSet fs, GC gc, int x, int y,
                     _Xconst char *s, int n)
{ mb_draw(d, dr, fs, gc, x, y, s, n, 0); }

void Xutf8DrawImageString(Display *d, Drawable dr, XFontSet fs, GC gc,
                          int x, int y, _Xconst char *s, int n)
{ mb_draw(d, dr, fs, gc, x, y, s, n, 1); }

int Xutf8TextEscapement(XFontSet fs, _Xconst char *s, int n)
{ return utf8_text_width(fs, s, n); }

int Xutf8TextExtents(XFontSet fs, _Xconst char *s, int n,
                     XRectangle *ink, XRectangle *logical)
{ return XmbTextExtents(fs, s, n, ink, logical); }

int Xutf8TextPerCharExtents(XFontSet fs, _Xconst char *s, int n, XRectangle *ink,
                            XRectangle *logical, int nink, int *num,
                            XRectangle *ink_ret, XRectangle *logical_ret)
{ return XmbTextPerCharExtents(fs, s, n, ink, logical, nink, num, ink_ret, logical_ret); }

/* XmbResetIC/Xutf8ResetIC/XwcResetIC live in xim.c: resetting an input context
 * also has to tear down the Wayland preedit and flush it as committed text. */

/* ------------------------------------------------------------- OM / OC */

XOM XOpenOM(Display *d, struct _XrmHashBucketRec *db, _Xconst char *res_name,
            _Xconst char *res_class)
{
    (void)db; (void)res_name; (void)res_class;
    struct _XOM *om = calloc(1, sizeof *om);
    om->display = d;
    return om;
}

Status XCloseOM(XOM om) { free(om); return 1; }
Display *XDisplayOfOM(XOM om) { return om ? om->display : NULL; }
char *XLocaleOfOM(XOM om) { (void)om; return (char *)"C"; }
char *XGetOMValues(XOM om, ...) { (void)om; return NULL; }
char *XSetOMValues(XOM om, ...) { (void)om; return NULL; }

XOC XCreateOC(XOM om, ...)
{
    struct _XOC *oc = calloc(1, sizeof *oc);
    oc->om = om;
    return oc;
}
void XDestroyOC(XOC oc) { free(oc); }
XOM XOMOfOC(XOC oc) { return oc ? oc->om : NULL; }
char *XGetOCValues(XOC oc, ...) { (void)oc; return NULL; }
char *XSetOCValues(XOC oc, ...) { (void)oc; return NULL; }

/* --------------------------------------------------- wc text properties */

int XwcTextListToTextProperty(Display *d, wchar_t **list, int count,
                              XICCEncodingStyle style, XTextProperty *tp)
{
    (void)d; (void)style;
    size_t total = 0;
    for (int i = 0; i < count; i++) total += wcslen(list[i]) * 4 + 1;
    unsigned char *buf = calloc(total + 1, 1);
    size_t o = 0;
    for (int i = 0; i < count; i++) {
        for (wchar_t *w = list[i]; *w; w++) {
            unsigned int c = (unsigned int)*w;
            if (c < 0x80) buf[o++] = (unsigned char)c;
            else if (c < 0x800) { buf[o++] = (unsigned char)(0xC0|(c>>6)); buf[o++] = (unsigned char)(0x80|(c&0x3F)); }
            else { buf[o++] = (unsigned char)(0xE0|(c>>12)); buf[o++] = (unsigned char)(0x80|((c>>6)&0x3F)); buf[o++] = (unsigned char)(0x80|(c&0x3F)); }
        }
        buf[o++] = 0;
    }
    tp->value = buf; tp->encoding = XA_STRING; tp->format = 8; tp->nitems = o;
    return Success;
}

int XwcTextPropertyToTextList(Display *d, const XTextProperty *tp,
                              wchar_t ***list, int *count)
{
    (void)d;
    int n = 0;
    for (unsigned long i = 0; i < tp->nitems; i++) if (tp->value[i] == 0) n++;
    if (n == 0) n = 1;
    wchar_t **v = calloc(n + 1, sizeof(wchar_t *));
    int i = 0, start = 0;
    for (unsigned long k = 0; k < tp->nitems; k++) {
        if (tp->value[k] == 0) {
            int len = (int)k - start;
            wchar_t *w = calloc(len + 1, sizeof(wchar_t));
            for (int j = 0; j < len; j++) w[j] = (wchar_t)tp->value[start + j];
            v[i++] = w;
            start = (int)k + 1;
        }
    }
    v[i] = NULL;
    *list = v; *count = i;
    return Success;
}

void XwcFreeStringList(wchar_t **list)
{
    if (!list) return;
    for (wchar_t **p = list; *p; p++) free(*p);
    free(list);
}
