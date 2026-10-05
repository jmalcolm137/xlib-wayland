/* xim.c — X Input Method, bridged to the Wayland input-method protocol.
 *
 * There is no XIM wire protocol here (there is no X server and no separate XIM
 * server process).  We implement the *client-visible* XIM contract directly and
 * make the input method itself the compositor's: a single zwp_text_input_v3
 * object per display receives preedit_string/commit_string from whatever IME is
 * running on the compositor (ibus, fcitx5, ...), and this file turns those into
 * the XIM callbacks and XmbLookupString results the toolkit expects.
 *
 * Style support:
 *   XIMPreeditPosition    the shim draws the composing string itself, over the
 *                         spot location the client supplies (over-the-spot).
 *                         This is Motif's default (OverTheSpot).
 *   XIMPreeditCallbacks   the client draws it: we invoke XNPreeditStart/Draw/
 *                         Caret/Done.  This is what GTK2's im-xim wants.
 *   XIMPreeditNothing     no preedit display; commit only.
 *   XIMPreeditNone        no input method at all.
 *
 * Commit delivery follows libX11: a commit is attached to a synthetic KeyPress
 * with keycode 0 pushed onto the event queue.  The client calls XFilterEvent
 * (returns False), then XmbLookupString, which returns the queued UTF-8 text
 * with status XLookupChars.  Consumed keys never reach the client at all --
 * the compositor routes them to the IME and only forwards what it declines.
 */
#include "internal.h"

#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <stdio.h>
#include <wchar.h>

/* The nested-list layout produced by XVaCreateNestedList (src/xlib/xtcompat.c)
 * and by Motif's VaCopy.  Kept in sync with that file. */
typedef struct { char *name; long value; } MwArg;

/* Xlib.h defines the individual style bits but not the field masks. */
#ifndef XIM_PREEDIT_MASK
#define XIM_PREEDIT_MASK 0x00ffL
#endif
#ifndef XIM_STATUS_MASK
#define XIM_STATUS_MASK 0xff00L
#endif

/* XIM attribute names are string literals, not pointer identities. */
#define KEYEQ(a, b) (strcmp((a), (b)) == 0)

struct _XIM {
    Display     *display;
    XIMCallback *destroy_cb;   /* from XSetIMValues(XNDestroyCallback) */
};

struct _XIC {
    struct _XIM *im;
    Window       client;
    Window       focus;
    XIMStyle     input_style;

    /* Preedit/status callbacks the client registered (borrowed). */
    XIMCallback *preedit_start, *preedit_done, *preedit_draw, *preedit_caret;
    XIMCallback *status_start, *status_done, *status_draw;
    XIMCallback *string_conversion;

    /* Over-the-spot attributes. */
    XPoint        spot;
    bool          have_spot;
    XFontSet      preedit_font;
    unsigned long preedit_fg, preedit_bg;
    bool          have_preedit_fg, have_preedit_bg;
    int           line_space;
    XRectangle    preedit_area, status_area;
    bool          have_preedit_area, have_status_area;
    XIMResetState reset_state;
    XIMPreeditState preedit_state;

    /* Current preedit, UTF-8. */
    char         *preedit;
    int           preedit_len;      /* bytes */
    int           preedit_chars;    /* code points */
    int           preedit_cursor;   /* code points */
    bool          preedit_active;

    /* Committed-but-unread text (FIFO). */
    char        **commits;
    int           ncommits, commit_cap;

    MwToplevel   *tl;               /* toplevel the IC lives in, if known */
};

/* ------------------------------------------------------------- utf-8 helpers */

static int utf8_count(const char *s)
{
    int n = 0;
    for (; s && *s; s++)
        if (((unsigned char)*s & 0xC0) != 0x80) n++;
    return n;
}

static int utf8_byte_to_char(const char *s, int bytes)
{
    int c = 0;
    for (int i = 0; i < bytes && s[i]; i++)
        if (((unsigned char)s[i] & 0xC0) != 0x80) c++;
    return c;
}

/* Walk up to the window that owns a toplevel (a child has parent set; only a
 * top-level window carries ->tl). */
static MwToplevel *window_toplevel(MwWindow *w)
{
    while (w && !w->tl) w = w->parent;
    return w ? w->tl : NULL;
}

/* --------------------------------------------------------- nested-list apply */

static void ic_apply_preedit(struct _XIC *ic, const char *key, long value)
{
    if (KEYEQ(key, XNPreeditStartCallback))
        ic->preedit_start = (XIMCallback *)value;
    else if (KEYEQ(key, XNPreeditDoneCallback))
        ic->preedit_done = (XIMCallback *)value;
    else if (KEYEQ(key, XNPreeditDrawCallback))
        ic->preedit_draw = (XIMCallback *)value;
    else if (KEYEQ(key, XNPreeditCaretCallback))
        ic->preedit_caret = (XIMCallback *)value;
    else if (KEYEQ(key, XNStringConversionCallback))
        ic->string_conversion = (XIMCallback *)value;
    else if (KEYEQ(key, XNSpotLocation)) {
        if (value) { ic->spot = *(XPoint *)value; ic->have_spot = true; }
    } else if (KEYEQ(key, XNFontSet))
        ic->preedit_font = (XFontSet)value;
    else if (KEYEQ(key, XNForeground)) {
        ic->preedit_fg = (unsigned long)value; ic->have_preedit_fg = true;
    } else if (KEYEQ(key, XNBackground)) {
        ic->preedit_bg = (unsigned long)value; ic->have_preedit_bg = true;
    } else if (KEYEQ(key, XNLineSpace))
        ic->line_space = (int)value;
    else if (KEYEQ(key, XNArea)) {
        if (value) { ic->preedit_area = *(XRectangle *)value; ic->have_preedit_area = true; }
    } else if (KEYEQ(key, XNPreeditState))
        ic->preedit_state = (XIMPreeditState)value;
    /* XNFilterEvents and anything else: ignored. */
}

static void ic_apply_status(struct _XIC *ic, const char *key, long value)
{
    if (KEYEQ(key, XNStatusStartCallback))
        ic->status_start = (XIMCallback *)value;
    else if (KEYEQ(key, XNStatusDoneCallback))
        ic->status_done = (XIMCallback *)value;
    else if (KEYEQ(key, XNStatusDrawCallback))
        ic->status_draw = (XIMCallback *)value;
    else if (KEYEQ(key, XNForeground) || KEYEQ(key, XNBackground) ||
             KEYEQ(key, XNFontSet))
        ; /* status styling: accepted, the IME draws its own panel */
    else if (KEYEQ(key, XNArea)) {
        if (value) { ic->status_area = *(XRectangle *)value; ic->have_status_area = true; }
    }
}

static void ic_apply_list(struct _XIC *ic, const MwArg *list, int context);

static void ic_apply(struct _XIC *ic, const char *key, long value, int context)
{
    if (context == 1) { ic_apply_preedit(ic, key, value); return; }
    if (context == 2) { ic_apply_status(ic, key, value); return; }

    if (KEYEQ(key, XNInputStyle)) {
        ic->input_style = (XIMStyle)value;
        /* Keep only what we can actually honour. */
        if ((ic->input_style & XIM_PREEDIT_MASK) == 0)
            ic->input_style |= XIMPreeditCallbacks;
        if ((ic->input_style & XIM_STATUS_MASK) == 0)
            ic->input_style |= XIMStatusNothing;
    } else if (KEYEQ(key, XNClientWindow)) {
        ic->client = (Window)value;
        if (value) ic->tl = window_toplevel(mw_window(ic->im->display, ic->client));
    } else if (KEYEQ(key, XNFocusWindow)) {
        ic->focus = (Window)value;
        if (value) ic->tl = window_toplevel(mw_window(ic->im->display, ic->focus));
    } else if (KEYEQ(key, XNPreeditAttributes)) {
        if (value) ic_apply_list(ic, (const MwArg *)value, 1);
    } else if (KEYEQ(key, XNStatusAttributes)) {
        if (value) ic_apply_list(ic, (const MwArg *)value, 2);
    } else if (KEYEQ(key, XNVaNestedList)) {
        if (value) ic_apply_list(ic, (const MwArg *)value, 0);
    } else if (KEYEQ(key, XNResetState)) {
        ic->reset_state = (XIMResetState)value;
    }
    /* XNFilterEvents, XNResourceName/Class: ignored. */
}

static void ic_apply_list(struct _XIC *ic, const MwArg *list, int context)
{
    for (; list && list->name; list++)
        ic_apply(ic, list->name, list->value, context);
}

/* ------------------------------------------------- XGetICValues nested fills */

static void ic_fill_preedit(struct _XIC *ic, const MwArg *list)
{
    for (; list && list->name; list++) {
        void *out = (void *)(uintptr_t)list->value;
        if (!out) continue;
        if (KEYEQ(list->name, XNPreeditState))
            *(XIMPreeditState *)out = ic->preedit_active ? XIMPreeditEnable
                                                         : XIMPreeditDisable;
        else if (KEYEQ(list->name, XNSpotLocation))
            *(XPoint *)out = ic->spot;
        else if (KEYEQ(list->name, XNForeground))
            *(unsigned long *)out = ic->have_preedit_fg ? ic->preedit_fg : 0;
        else if (KEYEQ(list->name, XNBackground))
            *(unsigned long *)out = ic->have_preedit_bg ? ic->preedit_bg : 0;
        else if (KEYEQ(list->name, XNFontSet))
            *(XFontSet *)out = ic->preedit_font;
        else if (KEYEQ(list->name, XNLineSpace))
            *(int *)out = ic->line_space;
        else if (KEYEQ(list->name, XNArea))
            *(XRectangle *)out = ic->preedit_area;
    }
}

static void ic_fill_status(struct _XIC *ic, const MwArg *list)
{
    for (; list && list->name; list++) {
        void *out = (void *)(uintptr_t)list->value;
        if (!out) continue;
        if (KEYEQ(list->name, XNArea))
            *(XRectangle *)out = ic->status_area;
        else if (KEYEQ(list->name, XNForeground) ||
                 KEYEQ(list->name, XNBackground))
            *(unsigned long *)out = 0;
        else if (KEYEQ(list->name, XNFontSet))
            *(XFontSet *)out = NULL;
    }
}

/* --------------------------------------------------- preedit callback invokes */

/* XIMProc is declared with an XIM first argument, but every toolkit casts a
 * function that really takes the XIC; pointer identity is what matters. */
static void call_preedit_start(struct _XIC *ic)
{
    if (ic->preedit_start && ic->preedit_start->callback)
        ic->preedit_start->callback((XIM)ic, ic->preedit_start->client_data, NULL);
}

static void call_preedit_done(struct _XIC *ic)
{
    if (ic->preedit_done && ic->preedit_done->callback)
        ic->preedit_done->callback((XIM)ic, ic->preedit_done->client_data, NULL);
}

static void call_preedit_draw(struct _XIC *ic, const char *text,
                              int caret_chars, int old_chars)
{
    if (!ic->preedit_draw || !ic->preedit_draw->callback) return;
    int chars = text ? utf8_count(text) : 0;
    XIMText xt;
    XIMFeedback *fb = NULL;
    memset(&xt, 0, sizeof xt);
    if (chars > 0) {
        fb = calloc((size_t)chars, sizeof *fb);
        xt.length = (unsigned short)chars;
        xt.feedback = fb;
        xt.encoding_is_wchar = False;
        xt.string.multi_byte = (char *)text;
    }
    XIMPreeditDrawCallbackStruct d;
    memset(&d, 0, sizeof d);
    d.caret = caret_chars;
    d.chg_first = 0;
    d.chg_length = old_chars;
    d.text = chars > 0 ? &xt : NULL;
    ic->preedit_draw->callback((XIM)ic, ic->preedit_draw->client_data, (XPointer)&d);
    free(fb);
}

static void call_preedit_caret(struct _XIC *ic, int pos)
{
    if (!ic->preedit_caret || !ic->preedit_caret->callback) return;
    XIMPreeditCaretCallbackStruct c;
    memset(&c, 0, sizeof c);
    c.position = pos;
    c.direction = XIMAbsolutePosition;
    c.style = XIMIsPrimary;
    ic->preedit_caret->callback((XIM)ic, ic->preedit_caret->client_data, (XPointer)&c);
}

/* --------------------------------------------------------------- text-input */

static void ti_commit_event(Display *d, struct _XIC *ic, const char *text)
{
    if (!text || !*text) return;
    if (ic->ncommits == ic->commit_cap) {
        int cap = ic->commit_cap ? ic->commit_cap * 2 : 4;
        char **n = realloc(ic->commits, (size_t)cap * sizeof *n);
        if (!n) return;
        ic->commits = n;
        ic->commit_cap = cap;
    }
    ic->commits[ic->ncommits++] = strdup(text);

    /* libX11 delivers a commit as a synthetic KeyPress with keycode 0; the
     * client's XFilterEvent sees it (False) and XmbLookupString returns the
     * text.  Deliver it to the focused window so the widget's IM filter runs. */
    Window win = ic->focus ? ic->focus : ic->client;
    MwWindow *w = mw_window(d, win);
    if (!w) return;
    XKeyEvent ke;
    memset(&ke, 0, sizeof ke);
    ke.type = KeyPress;
    ke.display = d;
    ke.window = win;
    ke.root = MWSCR(d)->root;
    ke.subwindow = None;
    ke.time = mw_now();
    ke.same_screen = True;
    ke.keycode = 0;
    ke.state = 0;
    mw_put_event(d, (XEvent *)&ke);
}

static void set_preedit(struct _XIC *ic, const char *text, int cbegin, int cend)
{
    (void)cend;
    int chars = text ? utf8_count(text) : 0;
    if (chars == 0) {
        if (!ic->preedit_active) return;
        int old = ic->preedit_chars;
        ic->preedit_active = false;
        free(ic->preedit);
        ic->preedit = NULL;
        ic->preedit_len = ic->preedit_chars = ic->preedit_cursor = 0;
        call_preedit_draw(ic, NULL, 0, old);
        call_preedit_done(ic);
        return;
    }
    if (!ic->preedit_active) {
        ic->preedit_active = true;
        call_preedit_start(ic);
    }
    int old = ic->preedit_chars;
    free(ic->preedit);
    ic->preedit = strdup(text);
    ic->preedit_len = (int)strlen(text);
    ic->preedit_chars = chars;
    int caret = (cbegin < 0) ? chars : utf8_byte_to_char(text, cbegin);
    if (caret > chars) caret = chars;
    ic->preedit_cursor = caret;
    call_preedit_draw(ic, ic->preedit, caret, old);
    call_preedit_caret(ic, caret);
    if ((ic->input_style & XIM_PREEDIT_MASK) == XIMPreeditPosition && ic->tl)
        mw_toplevel_damage(ic->tl);
}

static bool ic_has_spot(const struct _XIC *ic)
{
    return ic && (ic->input_style & XIM_PREEDIT_MASK) == XIMPreeditPosition;
}

/* Frame-local top-left of the IC's spot.  Returns false when there is nowhere
 * to put it. */
static bool spot_frame_xy(Display *d, const struct _XIC *ic, int *fx, int *fy)
{
    if (!ic || !ic->tl) return false;
    MwWindow *cw = mw_window(d, ic->focus ? ic->focus : ic->client);
    if (!cw) return false;
    int wx, wy, tx, ty;
    mw_window_origin(cw, &wx, &wy);
    mw_window_origin(ic->tl->win, &tx, &ty);
    *fx = wx - tx + ic->spot.x;
    *fy = wy - ty + ic->tl->tb_h + ic->spot.y;
    return true;
}

static void send_cursor_rect(Display *d)
{
    XDisplayImpl *dp = MWD(d);
    struct _XIC *ic = dp->text_input_ic;
    if (!dp->text_input || !ic) return;
    int x = 0, y = 0, w = 1, h = 1;
    if (ic->have_spot) {
        int fx, fy;
        if (spot_frame_xy(d, ic, &fx, &fy)) { x = fx; y = fy; }
    }
    zwp_text_input_v3_set_cursor_rectangle(dp->text_input, x, y, w, h);
}

static void ti_enable(Display *d)
{
    XDisplayImpl *dp = MWD(d);
    if (!dp->text_input) return;
    zwp_text_input_v3_enable(dp->text_input);
    zwp_text_input_v3_set_content_type(dp->text_input,
                                       ZWP_TEXT_INPUT_V3_CONTENT_HINT_NONE,
                                       ZWP_TEXT_INPUT_V3_CONTENT_PURPOSE_NORMAL);
    send_cursor_rect(d);
    zwp_text_input_v3_commit(dp->text_input);
    dp->text_input_entered = false;
}

static void ti_disable(Display *d)
{
    XDisplayImpl *dp = MWD(d);
    if (!dp->text_input) return;
    zwp_text_input_v3_disable(dp->text_input);
    zwp_text_input_v3_commit(dp->text_input);
    dp->text_input_entered = false;
}

static void ti_enter(void *data, struct zwp_text_input_v3 *ti, struct wl_surface *s)
{
    (void)ti; (void)s;
    Display *d = data;
    XDisplayImpl *dp = MWD(d);
    dp->text_input_entered = true;
    /* After enter, all state is invalidated and must be resent. */
    if (dp->text_input) {
        zwp_text_input_v3_set_content_type(dp->text_input,
                                           ZWP_TEXT_INPUT_V3_CONTENT_HINT_NONE,
                                           ZWP_TEXT_INPUT_V3_CONTENT_PURPOSE_NORMAL);
        send_cursor_rect(d);
        zwp_text_input_v3_commit(dp->text_input);
    }
}

static void ti_leave(void *data, struct zwp_text_input_v3 *ti, struct wl_surface *s)
{
    (void)ti; (void)s;
    Display *d = data;
    XDisplayImpl *dp = MWD(d);
    dp->text_input_entered = false;
    if (dp->text_input_ic) set_preedit(dp->text_input_ic, NULL, 0, 0);
}

static void ti_preedit_string(void *data, struct zwp_text_input_v3 *ti,
                              const char *text, int32_t cursor_begin,
                              int32_t cursor_end)
{
    (void)ti;
    Display *d = data;
    struct _XIC *ic = MWD(d)->text_input_ic;
    if (ic) set_preedit(ic, text, cursor_begin, cursor_end);
}

static void ti_commit_string(void *data, struct zwp_text_input_v3 *ti,
                             const char *text)
{
    (void)ti;
    Display *d = data;
    struct _XIC *ic = MWD(d)->text_input_ic;
    if (!ic) return;
    /* A commit always terminates any composition. */
    set_preedit(ic, NULL, 0, 0);
    if (getenv("MW_TRACE"))
        fprintf(stderr, "MW: XIM commit '%s'\n", text ? text : "");
    ti_commit_event(d, ic, text);
}

static void ti_delete_surrounding_text(void *data, struct zwp_text_input_v3 *ti,
                                       uint32_t before_length, uint32_t after_length)
{
    (void)data; (void)ti; (void)before_length; (void)after_length;
    /* No equivalent in the XIM client contract; the toolkit's own editing is
     * left untouched. */
}

static void ti_done(void *data, struct zwp_text_input_v3 *ti, uint32_t serial)
{
    (void)data; (void)ti; (void)serial;
}

static const struct zwp_text_input_v3_listener ti_listener = {
    .enter = ti_enter,
    .leave = ti_leave,
    .preedit_string = ti_preedit_string,
    .commit_string = ti_commit_string,
    .delete_surrounding_text = ti_delete_surrounding_text,
    .done = ti_done,
};

static void text_input_ensure(Display *d)
{
    XDisplayImpl *dp = MWD(d);
    if (dp->text_input || !dp->text_input_mgr || !dp->wl_seat) return;
    dp->text_input = zwp_text_input_manager_v3_get_text_input(dp->text_input_mgr,
                                                              dp->wl_seat);
    if (dp->text_input)
        zwp_text_input_v3_add_listener(dp->text_input, &ti_listener, d);
}

void mw_xim_init(Display *d) { (void)d; }

/* --------------------------------------------------------------- XIM / XIC */

XIM XOpenIM(Display *d, XrmDatabase db, char *res_name, char *res_class)
{
    (void)db; (void)res_name; (void)res_class;
    struct _XIM *im = calloc(1, sizeof *im);
    im->display = d;
    return im;
}

Status XCloseIM(XIM im)
{
    if (!im) return 1;
    if (im->destroy_cb && im->destroy_cb->callback)
        im->destroy_cb->callback((XIM)im, im->destroy_cb->client_data, NULL);
    free(im);
    return 1;
}

/* Accessors the toolkit layers need.  libXaw dereferences XDisplayOfIM()
 * directly (via Xt), so its absence was a hard undefined-symbol failure for
 * every libXaw client -- xterm included. */
Display *XDisplayOfIM(XIM im) { return im ? im->display : NULL; }
char *XLocaleOfIM(XIM im) { (void)im; return NULL; }

/* XNQueryInputStyle hands ownership to the caller: the XIM spec says the
 * client frees the returned XIMStyles with XFree(), and that is exactly what
 * libXaw's text code does.  Allocate a fresh copy per call, with the style
 * array in its own allocation.
 *
 * Order matters: Motif's XmIm picks the first entry that matches XmNpreeditType
 * (default OverTheSpot) and wants XIMPreeditPosition; GTK2 filters out Position
 * (its ALLOWED_MASK) and takes the Callbacks entry. */
static XIMStyles *query_styles(void)
{
    XIMStyle *list = malloc(4 * sizeof *list);
    XIMStyles *styles = malloc(sizeof *styles);
    if (!list || !styles) { free(list); free(styles); return NULL; }
    list[0] = XIMPreeditPosition  | XIMStatusNothing;
    list[1] = XIMPreeditCallbacks | XIMStatusCallbacks;
    list[2] = XIMPreeditNothing   | XIMStatusNothing;
    list[3] = XIMPreeditNone      | XIMStatusNone;
    styles->count_styles = 4;
    styles->supported_styles = list;
    return styles;
}

static XIMValuesList *query_values_list(void)
{
    static const char *names[] = {
        XNInputStyle, XNClientWindow, XNFocusWindow, XNFilterEvents,
        XNStatusAttributes, XNPreeditAttributes, XNSpotLocation,
        XNFontSet, XNForeground, XNBackground, XNLineSpace,
        XNPreeditState, XNResetState,
    };
    int n = (int)(sizeof names / sizeof names[0]);
    char **vals = malloc((size_t)n * sizeof(char *));
    XIMValuesList *vl = malloc(sizeof *vl);
    if (!vals || !vl) { free(vals); free(vl); return NULL; }
    for (int i = 0; i < n; i++) vals[i] = (char *)names[i];
    vl->count_values = (unsigned short)n;
    vl->supported_values = vals;
    return vl;
}

char *XGetIMValues(XIM im, ...)
{
    if (!im) return (char *)"no input method";
    va_list ap;
    va_start(ap, im);
    const char *key;
    while ((key = va_arg(ap, const char *)) != NULL) {
        if (KEYEQ(key, XNQueryInputStyle)) {
            XIMStyles **out = va_arg(ap, XIMStyles **);
            if (out) *out = query_styles();
        } else if (KEYEQ(key, XNQueryIMValuesList) ||
                   KEYEQ(key, XNQueryICValuesList)) {
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
    while ((key = va_arg(ap, const char *)) != NULL) {
        void *value = va_arg(ap, void *);
        if (KEYEQ(key, XNDestroyCallback))
            im->destroy_cb = (XIMCallback *)value;
    }
    va_end(ap);
    return NULL;
}

XIC XCreateIC(XIM im, ...)
{
    struct _XIC *ic = calloc(1, sizeof *ic);
    if (!ic) return NULL;
    ic->im = im ? im : NULL;
    ic->input_style = XIMPreeditCallbacks | XIMStatusNothing;
    ic->reset_state = XIMInitialState;
    ic->preedit_state = XIMPreeditUnKnown;

    va_list ap;
    va_start(ap, im);
    const char *key;
    while ((key = va_arg(ap, const char *)) != NULL) {
        long value = va_arg(ap, long);
        ic_apply(ic, key, value, 0);
    }
    va_end(ap);

    if (!ic->client && ic->focus) ic->client = ic->focus;
    return ic;
}

static void ic_clear_preedit(struct _XIC *ic)
{
    if (ic->preedit_active) {
        ic->preedit_active = false;
        call_preedit_done(ic);
    }
    free(ic->preedit);
    ic->preedit = NULL;
    ic->preedit_len = ic->preedit_chars = ic->preedit_cursor = 0;
}

void XDestroyIC(XIC xic)
{
    struct _XIC *ic = xic;
    if (!ic) return;
    Display *d = ic->im ? ic->im->display : NULL;
    if (d && MWD(d)->text_input_ic == ic) {
        ti_disable(d);
        MWD(d)->text_input_ic = NULL;
    }
    ic_clear_preedit(ic);
    for (int i = 0; i < ic->ncommits; i++) free(ic->commits[i]);
    free(ic->commits);
    free(ic);
}

char *XGetICValues(XIC xic, ...)
{
    if (!xic) return (char *)"no input context";
    struct _XIC *ic = xic;
    va_list ap;
    va_start(ap, xic);
    const char *key;
    while ((key = va_arg(ap, const char *)) != NULL) {
        void *value = va_arg(ap, void *);
        if (!value) continue;
        if (KEYEQ(key, XNInputStyle))
            *(XIMStyle *)value = ic->input_style;
        else if (KEYEQ(key, XNClientWindow))
            *(Window *)value = ic->client;
        else if (KEYEQ(key, XNFocusWindow))
            *(Window *)value = ic->focus;
        else if (KEYEQ(key, XNFilterEvents))
            *(unsigned long *)value = 0;
        else if (KEYEQ(key, XNResetState))
            *(XIMResetState *)value = ic->reset_state;
        else if (KEYEQ(key, XNPreeditAttributes))
            ic_fill_preedit(ic, (const MwArg *)value);
        else if (KEYEQ(key, XNStatusAttributes))
            ic_fill_status(ic, (const MwArg *)value);
        else if (KEYEQ(key, XNVaNestedList))
            ic_apply_list(ic, (const MwArg *)value, 0);
    }
    va_end(ap);
    return NULL;
}

char *XSetICValues(XIC xic, ...)
{
    if (!xic) return (char *)"no input context";
    struct _XIC *ic = xic;
    va_list ap;
    va_start(ap, xic);
    const char *key;
    while ((key = va_arg(ap, const char *)) != NULL) {
        long value = va_arg(ap, long);
        ic_apply(ic, key, value, 0);
    }
    va_end(ap);
    /* The spot may have moved: refresh the IME's cursor rectangle. */
    if (ic->im && MWD(ic->im->display)->text_input_ic == ic) {
        text_input_ensure(ic->im->display);
        send_cursor_rect(ic->im->display);
        if (MWD(ic->im->display)->text_input)
            zwp_text_input_v3_commit(MWD(ic->im->display)->text_input);
    }
    return NULL;
}

void XSetICFocus(XIC xic)
{
    struct _XIC *ic = xic;
    if (!ic || !ic->im) return;
    Display *d = ic->im->display;
    XDisplayImpl *dp = MWD(d);

    MwWindow *w = mw_window(d, ic->focus ? ic->focus : ic->client);
    ic->tl = w ? window_toplevel(w) : NULL;

    if (dp->text_input_ic && dp->text_input_ic != ic)
        ti_disable(d);
    dp->text_input_ic = ic;
    text_input_ensure(d);
    if (dp->text_input)
        ti_enable(d);
    if (getenv("MW_TRACE"))
        fprintf(stderr, "MW: XIM focus in ic=%p tl=%p ti=%p\n",
                (void *)ic, (void *)ic->tl, (void *)dp->text_input);
}

void XUnsetICFocus(XIC xic)
{
    struct _XIC *ic = xic;
    if (!ic || !ic->im) return;
    Display *d = ic->im->display;
    XDisplayImpl *dp = MWD(d);
    if (dp->text_input_ic == ic) {
        ti_disable(d);
        dp->text_input_ic = NULL;
    }
    set_preedit(ic, NULL, 0, 0);
    ic->tl = NULL;
}

XIM XIMOfIC(XIC ic) { return ic ? ic->im : NULL; }

/* -------------------------------------------------------- input filtering */

Bool XFilterEvent(XEvent *event, Window w)
{
    (void)event; (void)w;
    /* Keys the IME consumes never reach this process: the compositor routes
     * them to the input method and forwards only what it declines.  A commit
     * (and every ordinary key) must therefore pass through untouched; the
     * commit synthesised in ti_commit_event() is read by XmbLookupString(). */
    return False;
}

/* ------------------------------------------------------ string lookup / reset */

int XmbLookupString(XIC xic, XKeyEvent *event, char *buffer, int nbytes,
                    KeySym *keysym, Status *status)
{
    struct _XIC *ic = xic;
    if (keysym) *keysym = NoSymbol;

    /* A synthetic keycode-0 KeyPress carries a queued IME commit. */
    if (event && event->type == KeyPress && event->keycode == 0 &&
        ic && ic->ncommits > 0) {
        const char *s = ic->commits[0];
        int len = (int)strlen(s);
        if (nbytes < len + 1) {
            if (status) *status = XBufferOverflow;
            return len + 1;
        }
        memcpy(buffer, s, (size_t)len);
        free(ic->commits[0]);
        memmove(&ic->commits[0], &ic->commits[1],
                (size_t)(ic->ncommits - 1) * sizeof *ic->commits);
        ic->ncommits--;
        if (status) *status = XLookupChars;
        if (getenv("MW_TRACE"))
            fprintf(stderr, "MW: XmbLookupString commit n=%d\n", len);
        return len;
    }

    if (!event || event->type != KeyPress) {
        if (status) *status = XLookupNone;
        return 0;
    }
    int n = XLookupString(event, buffer, nbytes, keysym, NULL);
    if (status)
        *status = n > 0
                  ? ((keysym && *keysym != NoSymbol) ? XLookupBoth : XLookupChars)
                  : XLookupKeySym;
    if (getenv("MW_TRACE"))
        fprintf(stderr, "MW: XmbLookupString kc=%u n=%d sym=0x%lx status=%d\n",
                event->keycode, n, keysym ? (unsigned long)*keysym : 0UL,
                status ? *status : 0);
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
    char mb[1024];
    int n = XmbLookupString(ic, event, mb, sizeof mb, keysym, status);
    if (n <= 0) return 0;
    mb[n] = 0;
    const char *p = mb;
    mbstate_t st;
    memset(&st, 0, sizeof st);
    size_t r = mbsrtowcs(buffer, &p, (size_t)nchars, &st);
    return r == (size_t)-1 ? 0 : (int)r;
}

/* Xlib's contract: reset the IC and return the string it had preedited (which
 * the caller frees with XFree).  The preedit becomes committed text; the
 * Wayland IME is reset by a disable/enable cycle. */
char *XmbResetIC(XIC xic)
{
    struct _XIC *ic = xic;
    if (!ic) return strdup("");
    char *out = NULL;
    if (ic->preedit_active && ic->preedit && ic->preedit[0])
        out = strdup(ic->preedit);
    ic_clear_preedit(ic);
    if (ic->im) {
        Display *d = ic->im->display;
        XDisplayImpl *dp = MWD(d);
        if (dp->text_input_ic == ic && dp->text_input) {
            ti_disable(d);
            ti_enable(d);
        }
    }
    return out ? out : strdup("");
}

char *Xutf8ResetIC(XIC ic) { return XmbResetIC(ic); }

wchar_t *XwcResetIC(XIC ic)
{
    char *mb = XmbResetIC(ic);
    size_t n = mbstowcs(NULL, mb, 0);
    wchar_t *w = calloc(n + 1, sizeof *w);
    if (w) mbstowcs(w, mb, n + 1);
    free(mb);
    return w;
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

/* ----------------------------------------------------- over-the-spot overlay */

/* Called from mw_toplevel_render() after the window tree has been composited,
 * so a Position-style preedit is painted on top of the client's own drawing
 * and survives every repaint (it is redrawn on each composite). */
void mw_xim_overlay(Display *d, MwToplevel *tl)
{
    XDisplayImpl *dp = MWD(d);
    struct _XIC *ic = dp->text_input_ic;
    if (!ic || !tl || tl->is_popup) return;
    if (!ic->preedit_active || !ic->preedit || !ic->preedit[0]) return;
    if (!ic_has_spot(ic)) return;
    if (ic->tl && ic->tl != tl) return;
    if (!ic->have_spot) return;

    int x, y;
    if (!spot_frame_xy(d, ic, &x, &y)) return;

    MwFont *f = NULL;
    if (ic->preedit_font) {
        MwXFont *xf = (MwXFont *)ic->preedit_font;
        f = xf->rfont;
    }
    if (!f) {
        static MwFont *fallback;
        if (!fallback) fallback = mw_font_create(NULL, 0);
        f = fallback;
    }
    if (!f) return;

    int asc = mw_font_ascent(f);
    int desc = mw_font_descent(f);
    int tw = mw_font_text_width_utf8(f, ic->preedit, ic->preedit_len);
    int th = asc + desc;
    if (tw < 1) tw = 1;

    uint32_t fg = ic->have_preedit_fg ? (0xff000000u | (ic->preedit_fg & 0xffffffu))
                                      : 0xff000000u;
    uint32_t bg = ic->have_preedit_bg ? (0xff000000u | (ic->preedit_bg & 0xffffffu))
                                      : 0xfffff0c0u;

    MwCanvas *c = mw_canvas_begin(tl->frame, NULL, 0, 0, 0);
    mw_set_operator(c, GXcopy);
    mw_set_source_argb(c, bg);
    mw_rect(c, x, y - asc, tw + 2, th);
    mw_fill_path(c);
    /* Underline the composition, as XIM preedit usually is. */
    mw_set_source_argb(c, fg);
    mw_rect(c, x, y + desc - 1, tw + 2, 1);
    mw_fill_path(c);
    mw_show_utf8(c, f, ic->preedit, ic->preedit_len, x + 1, y);
    mw_canvas_end(c);
}
