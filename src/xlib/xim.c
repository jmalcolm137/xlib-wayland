/* xim.c — a local X Input Method sufficient for Motif text widgets.
 *
 * There is no XIM protocol here; the point is to honour the *client-visible*
 * contract that Motif's XmIm.c depends on: a non-NULL XIM advertising the
 * XIMPreeditNothing|XIMStatusNothing style, ICs bound to a client window, and
 * XGetIMValues/XGetICValues that fill the requested return slots instead of
 * leaving them untouched.
 */
#include "internal.h"

#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <wchar.h>

struct _XIM { Display *display; };
struct _XIC {
    struct _XIM *im;
    Window       client;
    Window       focus;
    XIMStyle     input_style;
};

void mw_xim_init(Display *d) { (void)d; }

XIM XOpenIM(Display *d, XrmDatabase db, char *res_name, char *res_class)
{
    (void)db; (void)res_name; (void)res_class;
    struct _XIM *im = calloc(1, sizeof *im);
    im->display = d;
    return im;
}

Status XCloseIM(XIM im) { free(im); return 1; }

/* Accessors the toolkit layers need.  libXaw dereferences XDisplayOfIM()
 * directly (via Xt), so its absence was a hard undefined-symbol failure for
 * every libXaw client -- xterm included. */
Display *XDisplayOfIM(XIM im) { return im ? im->display : NULL; }
char *XLocaleOfIM(XIM im) { (void)im; return NULL; }

/* XNQueryInputStyle hands ownership to the caller: the XIM spec says the
 * client frees the returned XIMStyles with XFree(), and that is exactly what
 * libXaw's text code does.  Returning a pointer to static storage therefore
 * aborted every libXaw client (xmessage, xterm, ...) in free().  Allocate a
 * fresh copy per call, and keep the style array in its own allocation so a
 * caller that frees the array as well as the struct is still valid. */
static XIMStyles *query_styles(void)
{
    XIMStyle *list = malloc(3 * sizeof *list);
    XIMStyles *styles = malloc(sizeof *styles);
    if (!list || !styles) { free(list); free(styles); return NULL; }
    list[0] = XIMPreeditNothing | XIMStatusNothing;
    list[1] = XIMPreeditNone    | XIMStatusNone;
    list[2] = XIMPreeditNothing | XIMStatusNone;
    styles->count_styles = 3;
    styles->supported_styles = list;
    return styles;
}

/* Same ownership rule for the value-name lists. */
static XIMValuesList *query_values_list(void)
{
    XIMValuesList *vl = malloc(sizeof *vl);
    if (vl) { vl->count_values = 0; vl->supported_values = NULL; }
    return vl;
}

char *XGetIMValues(XIM im, ...)
{
    if (!im) return (char *)"no input method";
    va_list ap;
    va_start(ap, im);
    const char *key;
    while ((key = va_arg(ap, const char *)) != NULL) {
        if (strcmp(key, XNQueryInputStyle) == 0) {
            XIMStyles **out = va_arg(ap, XIMStyles **);
            if (out) *out = query_styles();
        } else if (strcmp(key, XNQueryIMValuesList) == 0) {
            XIMValuesList **out = va_arg(ap, XIMValuesList **);
            if (out) *out = query_values_list();
        } else if (strcmp(key, XNQueryICValuesList) == 0) {
            XIMValuesList **out = va_arg(ap, XIMValuesList **);
            if (out) *out = query_values_list();
        } else {
            (void)va_arg(ap, void *);
        }
    }
    va_end(ap);
    return NULL;
}

char *XSetIMValues(XIM im, ...)
{
    if (!im) return (char *)"no input method";
    va_list ap;
    va_start(ap, im);
    const char *key;
    while ((key = va_arg(ap, const char *)) != NULL) (void)va_arg(ap, void *);
    va_end(ap);
    return NULL;
}

XIC XCreateIC(XIM im, ...)
{
    struct _XIC *ic = calloc(1, sizeof *ic);
    ic->im = im;
    ic->input_style = XIMPreeditNothing | XIMStatusNothing;
    va_list ap;
    va_start(ap, im);
    const char *key;
    while ((key = va_arg(ap, const char *)) != NULL) {
        if (strcmp(key, XNInputStyle) == 0)
            ic->input_style = va_arg(ap, XIMStyle);
        else if (strcmp(key, XNClientWindow) == 0)
            ic->client = va_arg(ap, Window);
        else if (strcmp(key, XNFocusWindow) == 0)
            ic->focus = va_arg(ap, Window);
        else if (strcmp(key, XNResourceName) == 0 ||
                 strcmp(key, XNResourceClass) == 0)
            (void)va_arg(ap, char *);
        else
            break;   /* unknown key: arity unclear, stop parsing safely */
    }
    va_end(ap);
    return ic;
}

void XDestroyIC(XIC ic) { free(ic); }

char *XGetICValues(XIC ic, ...)
{
    if (!ic) return (char *)"no input context";
    va_list ap;
    va_start(ap, ic);
    const char *key;
    while ((key = va_arg(ap, const char *)) != NULL) {
        if (strcmp(key, XNInputStyle) == 0) {
            XIMStyle *out = va_arg(ap, XIMStyle *);
            if (out) *out = ic->input_style;
        } else if (strcmp(key, XNFilterEvents) == 0) {
            unsigned long *out = va_arg(ap, unsigned long *);
            if (out) *out = 0;
        } else if (strcmp(key, XNStatusAttributes) == 0 ||
                   strcmp(key, XNPreeditAttributes) == 0 ||
                   strcmp(key, XNVaNestedList) == 0) {
            XPointer *out = va_arg(ap, XPointer *);
            if (out) *out = NULL;
        } else if (strcmp(key, XNResetState) == 0) {
            XIMResetState *out = va_arg(ap, XIMResetState *);
            if (out) *out = (XIMResetState)XIMInitialState;
        } else {
            (void)va_arg(ap, void *);
        }
    }
    va_end(ap);
    return NULL;
}

char *XSetICValues(XIC ic, ...)
{
    if (!ic) return (char *)"no input context";
    va_list ap;
    va_start(ap, ic);
    const char *key;
    while ((key = va_arg(ap, const char *)) != NULL) (void)va_arg(ap, void *);
    va_end(ap);
    return NULL;
}

void XSetICFocus(XIC ic) { if (ic) ic->focus = ic->client; }
void XUnsetICFocus(XIC ic) { (void)ic; }

XIM XIMOfIC(XIC ic) { return ic ? ic->im : NULL; }

Bool XFilterEvent(XEvent *event, Window w)
{ (void)event; (void)w; return False; }

int XmbLookupString(XIC ic, XKeyEvent *event, char *buffer, int nbytes,
                    KeySym *keysym, Status *status)
{
    (void)ic;
    if (event->type != KeyPress) {
        if (status) *status = XLookupNone;
        return 0;
    }
    int n = XLookupString(event, buffer, nbytes, keysym, NULL);
    /* The status matters: Motif only inserts when it says characters were
     * produced (XLookupChars / XLookupBoth), not merely a keysym. */
    if (status)
        *status = n > 0
                  ? ((keysym && *keysym != NoSymbol) ? XLookupBoth : XLookupChars)
                  : XLookupKeySym;
    if (getenv("MW_TRACE"))
        fprintf(stderr, "MW: XmbLookupString kc=%u n=%d sym=0x%lx status=%d '%c'\n",
                event->keycode, n, keysym ? (unsigned long)*keysym : 0UL,
                status ? *status : 0, n > 0 ? buffer[0] : '?');
    return n;
}

int Xutf8LookupString(XIC ic, XKeyEvent *event, char *buffer, int nbytes,
                      KeySym *keysym, Status *status)
{
    return XmbLookupString(ic, event, buffer, nbytes, keysym, status);
}

int XwcLookupString(XIC ic, XKeyEvent *event, wchar_t *buffer, int nchars,
                    KeySym *keysym, Status *status)
{
    char mb[256];
    int n = XmbLookupString(ic, event, mb, sizeof mb, keysym, status);
    if (n <= 0) return 0;
    mb[n] = 0;
    const char *p = mb;
    mbstate_t st;
    memset(&st, 0, sizeof st);
    size_t r = mbsrtowcs(buffer, &p, nchars, &st);
    return r == (size_t)-1 ? 0 : (int)r;
}

Bool XRegisterIMInstantiateCallback(Display *d, XrmDatabase db, char *res,
                                    char *cls, XIDProc callback, XPointer client)
{
    (void)d; (void)db; (void)res; (void)cls; (void)callback; (void)client;
    return True;
}

Bool XUnregisterIMInstantiateCallback(Display *d, XrmDatabase db, char *res,
                                      char *cls, XIDProc callback, XPointer client)
{
    (void)d; (void)db; (void)res; (void)cls; (void)callback; (void)client;
    return True;
}
