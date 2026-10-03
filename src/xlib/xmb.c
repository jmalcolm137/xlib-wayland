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

static int utf8_len(const char *s, int n)
{
    int count = 0;
    for (int i = 0; i < n; ) {
        unsigned char c = (unsigned char)s[i];
        int adv = c < 0x80 ? 1 : (c & 0xE0) == 0xC0 ? 2 : (c & 0xF0) == 0xE0 ? 3 : 4;
        i += adv; count++;
    }
    return count;
}

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
    (void)fs;
    if (image) XDrawImageString(d, dr, gc, x, y, str, n);
    else       XDrawString(d, dr, gc, x, y, str, n);
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
    if (ink) { ink->x = 0; ink->y = (short)-asc; ink->width = (unsigned short)w; ink->height = (unsigned short)(asc + desc); }
    if (logical) *logical = *ink;
    return w;
}

int XmbTextPerCharExtents(XFontSet fs, _Xconst char *s, int n, XRectangle *ink,
                          XRectangle *logical, int nink, int *num,
                          XRectangle *ink_ret, XRectangle *logical_ret)
{
    MwXFont *f = fs_font(fs);
    int asc = f ? f->ascent : 10, desc = f ? f->descent : 3;
    int count = 0, xoff = 0;
    const char *p = s, *end = s + (n < 0 ? (int)strlen(s) : n);
    float scale = nink > 0 ? (float)nink / (utf8_len(s, (int)(end - s)) ? utf8_len(s, (int)(end - s)) : 1) : 1.0f;
    while (p < end) {
        unsigned int ch = utf8_next(&p, end);
        int cw = f && f->rfont ? mw_font_char_width(f->rfont, ch) : 6;
        if (ink && count < nink) { ink[count].x = (short)xoff; ink[count].y = (short)-asc; ink[count].width = (unsigned short)cw; ink[count].height = (unsigned short)(asc+desc); }
        if (logical && count < nink) logical[count] = ink[count];
        if (ink_ret && count < nink) { ink_ret[count].x = (short)((short)xoff); ink_ret[count].width = (unsigned short)cw; }
        if (logical_ret && count < nink) { logical_ret[count].x = (short)xoff; logical_ret[count].width = (unsigned short)cw; }
        xoff += cw; count++;
    }
    (void)scale;
    if (num) *num = count;
    return count;
}

/* ------------------------------------------------------------------ Xwc */

void XwcDrawString(Display *d, Drawable dr, XFontSet fs, GC gc, int x, int y,
                   _Xconst wchar_t *s, int n)
{
    (void)fs;
    /* Convert to UTF-8 and reuse the core text path. */
    int cap = n * 4 + 1, o = 0;
    char *buf = malloc(cap);
    for (int i = 0; i < n; i++) {
        unsigned int c = (unsigned int)s[i];
        if (c < 0x80) buf[o++] = (char)c;
        else if (c < 0x800) { buf[o++] = (char)(0xC0|(c>>6)); buf[o++] = (char)(0x80|(c&0x3F)); }
        else if (c < 0x10000) { buf[o++] = (char)(0xE0|(c>>12)); buf[o++] = (char)(0x80|((c>>6)&0x3F)); buf[o++] = (char)(0x80|(c&0x3F)); }
        else { buf[o++] = (char)(0xF0|(c>>18)); buf[o++] = (char)(0x80|((c>>12)&0x3F)); buf[o++] = (char)(0x80|((c>>6)&0x3F)); buf[o++] = (char)(0x80|(c&0x3F)); }
    }
    XDrawString(d, dr, gc, x, y, buf, o);
    free(buf);
}

void XwcDrawImageString(Display *d, Drawable dr, XFontSet fs, GC gc, int x, int y,
                        _Xconst wchar_t *s, int n)
{
    (void)fs;
    int cap = n * 4 + 1, o = 0;
    char *buf = malloc(cap);
    for (int i = 0; i < n; i++) {
        unsigned int c = (unsigned int)s[i];
        if (c < 0x80) buf[o++] = (char)c;
        else if (c < 0x800) { buf[o++] = (char)(0xC0|(c>>6)); buf[o++] = (char)(0x80|(c&0x3F)); }
        else { buf[o++] = (char)(0xE0|(c>>12)); buf[o++] = (char)(0x80|((c>>6)&0x3F)); buf[o++] = (char)(0x80|(c&0x3F)); }
    }
    XDrawImageString(d, dr, gc, x, y, buf, o);
    free(buf);
}

int XwcTextEscapement(XFontSet fs, _Xconst wchar_t *s, int n)
{ return wc_text_width(fs, s, n); }

int XwcTextExtents(XFontSet fs, _Xconst wchar_t *s, int n,
                   XRectangle *ink, XRectangle *logical)
{
    int w = wc_text_width(fs, s, n);
    MwXFont *f = fs_font(fs);
    int asc = f ? f->ascent : 10, desc = f ? f->descent : 3;
    if (ink) { ink->x = 0; ink->y = (short)-asc; ink->width = (unsigned short)w; ink->height = (unsigned short)(asc+desc); }
    if (logical) *logical = *ink;
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
        if (ink && i < nink) { ink[i].x = (short)xoff; ink[i].y = (short)-asc; ink[i].width = (unsigned short)cw; ink[i].height = (unsigned short)(asc+desc); }
        if (logical && i < nink) logical[i] = ink[i];
        if (ink_ret && i < nink) { ink_ret[i].x = (short)xoff; ink_ret[i].width = (unsigned short)cw; }
        if (logical_ret && i < nink) { logical_ret[i].x = (short)xoff; logical_ret[i].width = (unsigned short)cw; }
        xoff += cw;
    }
    if (num) *num = n;
    return n;
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

/* Xlib's contract: reset the input context and return the string it had
 * committed, which the caller frees with XFree.  There is no preedit state
 * here, so the answer is an empty string -- and non-NULL, because callers
 * treat NULL as a failure. */
char *XmbResetIC(XIC ic) { (void)ic; return strdup(""); }
char *Xutf8ResetIC(XIC ic) { (void)ic; return strdup(""); }
wchar_t *XwcResetIC(XIC ic) { (void)ic; return calloc(1, sizeof(wchar_t)); }

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
