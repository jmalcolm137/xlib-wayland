#define _GNU_SOURCE
/* xdnd.c — bridge the XDND drag-and-drop protocol to the Wayland data device.
 *
 * XDND in one paragraph: a drag source owns the `XdndSelection` selection and
 * sends `XdndEnter` / `XdndPosition` / `XdndDrop` ClientMessages to the drop
 * site (a window advertising `XdndAware`); the site answers `XdndStatus`, pulls
 * the data from `XdndSelection` with XConvertSelection after the drop, and then
 * sends `XdndFinished` back to the source.
 *
 * None of that crosses between shim processes -- each is its own X server -- so
 * this mirrors the Motif bridge (dnd.c): the compositor routes the drag over
 * Wayland and this file speaks XDND to the X client in each process.  As the X
 * side, the shim is the XDND *source* to its own X clients: it presents the
 * Wayland offer as a synthetic XDND source window (`XdndTypeList` /
 * `XdndActionList`), drives Enter/Position/Drop, and serves `XdndSelection`
 * converts from the offer's bytes.  The X client replies Status/Finished.
 */
#include "internal.h"

#include <X11/Xatom.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdio.h>

#define XDND_VERSION 5

typedef struct MwXdnd {
    Display *d;
    Window   src;        /* synthetic XDND source window (ours) */
    Window   target;     /* X window we sent XdndEnter to */
    bool     entered;
    bool     active;
    bool     dropped;
    int      root_x, root_y;
    Atom     action;
    /* Advertised targets, and the dropped bytes stashed for XdndSelection. */
    char    **mimes;
    int       nmimes;
    unsigned char *drop_data;
    size_t    drop_len;
    bool      drop_ready;
} MwXdnd;

static int xdnd_trace(void)
{
    static int t = -1;
    if (t < 0) t = getenv("MW_TRACE") != NULL;
    return t;
}
#define TR(...) do { if (xdnd_trace()) fprintf(stderr, "MWXDND: " __VA_ARGS__); } while (0)

/* ----------------------------------------------------------------- atoms */

static Atom a_aware(Display *d)      { return mw_intern_atom(d, "XdndAware", False); }
static Atom a_sel(Display *d)        { return mw_intern_atom(d, "XdndSelection", False); }
static Atom a_typelist(Display *d)   { return mw_intern_atom(d, "XdndTypeList", False); }
static Atom a_actionlist(Display *d) { return mw_intern_atom(d, "XdndActionList", False); }
static Atom a_enter(Display *d)      { return mw_intern_atom(d, "XdndEnter", False); }
static Atom a_leave(Display *d)      { return mw_intern_atom(d, "XdndLeave", False); }
static Atom a_position(Display *d)   { return mw_intern_atom(d, "XdndPosition", False); }
static Atom a_drop(Display *d)       { return mw_intern_atom(d, "XdndDrop", False); }
static Atom a_status(Display *d)     { return mw_intern_atom(d, "XdndStatus", False); }
static Atom a_finished(Display *d)   { return mw_intern_atom(d, "XdndFinished", False); }
static Atom a_copy(Display *d)       { return mw_intern_atom(d, "XdndActionCopy", False); }
static Atom a_move(Display *d)       { return mw_intern_atom(d, "XdndActionMove", False); }
static Atom a_targets(Display *d)    { return mw_intern_atom(d, "TARGETS", False); }

Atom mw_xdnd_selection(Display *d) { return a_sel(d); }

/* -------------------------------------------------------------- life cycle */

void mw_xdnd_init(Display *d)
{
    MwXdnd *x = calloc(1, sizeof *x);
    if (!x) return;
    x->d = d;
    MWD(d)->xdnd = x;
}

void mw_xdnd_fini(Display *d)
{
    MwXdnd *x = MWD(d)->xdnd;
    if (!x) return;
    if (x->src != None) XDestroyWindow(d, x->src);
    for (int i = 0; i < x->nmimes; i++) free(x->mimes[i]);
    free(x->mimes);
    free(x->drop_data);
    free(x);
    MWD(d)->xdnd = NULL;
}

bool mw_xdnd_owns_window(Display *d, Window w)
{
    MwXdnd *x = MWD(d)->xdnd;
    return x && w != None && w == x->src;
}

/* The XdndAware property holds the protocol version the window supports. */
bool mw_xdnd_aware(Display *d, Window win)
{
    if (win == None) return false;
    Atom type = None;
    int fmt = 0;
    unsigned long n = 0, after = 0;
    unsigned char *data = NULL;
    if (XGetWindowProperty(d, win, a_aware(d), 0, 1, False, XA_ATOM,
                           &type, &fmt, &n, &after, &data) != Success)
        return false;
    bool ok = false;
    if (data && fmt == 32 && n >= 1) {
        unsigned long v = 0;
        memcpy(&v, data, 4);          /* the shim hands back wire-format longs */
        ok = v >= 3;
    }
    if (data) XFree(data);
    return ok;
}

static void ensure_src(Display *d)
{
    MwXdnd *x = MWD(d)->xdnd;
    if (!x || x->src != None) return;
    x->src = XCreateSimpleWindow(d, DefaultRootWindow(d), 0, 0, 1, 1, 0, 0, 0);
}

/* ------------------------------------------------------------- messages */

static void send_msg(Display *d, Window win, Atom msg,
                     long l0, long l1, long l2, long l3, long l4)
{
    XClientMessageEvent cm;
    memset(&cm, 0, sizeof cm);
    cm.type = ClientMessage;
    cm.display = d;
    cm.window = win;
    cm.message_type = msg;
    cm.format = 32;
    cm.data.l[0] = l0; cm.data.l[1] = l1; cm.data.l[2] = l2;
    cm.data.l[3] = l3; cm.data.l[4] = l4;
    XSendEvent(d, win, False, 0, (XEvent *)&cm);
}

/* Root coordinates of the pointer over the toplevel the drag is on. */
static void root_xy(Display *d, double sx, double sy, int *rx, int *ry)
{
    XDisplayImpl *dp = MWD(d);
    *rx = (int)(sx + 0.5);
    *ry = (int)(sy + 0.5);
    MwWindow *w = mw_window(d, dp->drag_window);
    if (!w) return;
    int ox = 0, oy = 0;
    mw_window_origin(w, &ox, &oy);
    int off = w->tl ? mw_toplevel_content_offset(w->tl) : 0;
    *rx = ox + (int)(sx + 0.5);
    *ry = oy + (int)(sy + 0.5) - off;
}

static void send_position(Display *d, MwXdnd *x)
{
    send_msg(d, x->target, a_position(d), (long)x->src, 0,
             (long)(((x->root_x & 0xffff) << 16) | (x->root_y & 0xffff)),
             (long)mw_now(), (long)x->action);
}

/* Advertise every offered MIME as an XDND target, plus the text aliases. */
static void set_type_list(Display *d, MwXdnd *x, MwWlOffer *o)
{
    long list[64];
    int n = 0;
    for (int i = 0; i < o->nmimes && n < 60; i++)
        list[n++] = (long)mw_intern_atom(d, o->mimes[i], False);
    if (o->mime) {
        list[n++] = (long)mw_intern_atom(d, "UTF8_STRING", False);
        list[n++] = (long)XA_STRING;
        list[n++] = (long)mw_intern_atom(d, "TEXT", False);
    }
    XChangeProperty(d, x->src, a_typelist(d), XA_ATOM, 32, PropModeReplace,
                    (const unsigned char *)list, n);
    Atom acts[2] = { a_copy(d), a_move(d) };
    XChangeProperty(d, x->src, a_actionlist(d), XA_ATOM, 32, PropModeReplace,
                    (const unsigned char *)acts, 2);
}

/* ---------------------------------------------------- target side (Wayland) */

void mw_xdnd_wl_enter(Display *d, struct wl_surface *s, double sx, double sy)
{
    (void)s;
    XDisplayImpl *dp = MWD(d);
    MwXdnd *x = dp->xdnd;
    if (!x) return;
    Window tgt = dp->drag_window;
    MwWlOffer *o = dp->drag_offer;
    if (tgt == None || !o) return;
    if (!mw_xdnd_aware(d, tgt)) return;

    ensure_src(d);
    set_type_list(d, x, o);
    XSetSelectionOwner(d, a_sel(d), x->src, CurrentTime);

    for (int i = 0; i < x->nmimes; i++) free(x->mimes[i]);
    free(x->mimes);
    x->mimes = calloc(o->nmimes ? o->nmimes : 1, sizeof(char *));
    x->nmimes = o->nmimes;
    for (int i = 0; i < o->nmimes; i++) x->mimes[i] = strdup(o->mimes[i]);
    free(x->drop_data);
    x->drop_data = NULL;
    x->drop_len = 0;
    x->drop_ready = false;

    x->target = tgt;
    x->action = a_copy(d);
    root_xy(d, sx, sy, &x->root_x, &x->root_y);

    long t0 = 0, t1 = 0, t2 = 0;
    if (o->nmimes > 0) t0 = (long)mw_intern_atom(d, o->mimes[0], False);
    if (o->nmimes > 1) t1 = (long)mw_intern_atom(d, o->mimes[1], False);
    if (o->nmimes > 2) t2 = (long)mw_intern_atom(d, o->mimes[2], False);
    long more = o->nmimes > 3 ? 1 : 0;
    send_msg(d, tgt, a_enter(d), (long)x->src,
             (long)((XDND_VERSION << 24) | more), t0, t1, t2);
    send_position(d, x);

    x->entered = true;
    x->active = true;
    x->dropped = false;
    TR("enter tgt=0x%lx src=0x%lx root=%d,%d mimes=%d\n",
       (unsigned long)tgt, (unsigned long)x->src, x->root_x, x->root_y, o->nmimes);
}

void mw_xdnd_wl_motion(Display *d, double sx, double sy)
{
    MwXdnd *x = MWD(d)->xdnd;
    if (!x || !x->active || x->dropped) return;
    root_xy(d, sx, sy, &x->root_x, &x->root_y);
    send_position(d, x);
}

void mw_xdnd_wl_leave(Display *d)
{
    MwXdnd *x = MWD(d)->xdnd;
    if (!x || !x->active) return;
    if (x->entered && !x->dropped && x->target != None)
        send_msg(d, x->target, a_leave(d), (long)x->src, 0, 0, 0, 0);
    x->active = false;
    x->entered = false;
    x->dropped = false;
}

void mw_xdnd_wl_drop(Display *d)
{
    MwXdnd *x = MWD(d)->xdnd;
    if (!x || !x->active || x->target == None) return;
    send_msg(d, x->target, a_drop(d), (long)x->src, 0, (long)mw_now(), 0, 0);
    x->dropped = true;
    TR("drop tgt=0x%lx\n", (unsigned long)x->target);
    /* The target now converts XdndSelection; the bytes were stashed when the
     * Wayland drop arrived (mw_xdnd_store_drop) and are served from there. */
}

/* The dropped bytes have arrived (fetched before the Wayland offer is torn down
 * with the drag); stash them and only then tell the target it may drop, so its
 * XdndSelection convert finds them ready. */
void mw_xdnd_store_drop(Display *d, const unsigned char *data, size_t len)
{
    MwXdnd *x = MWD(d)->xdnd;
    if (!x) return;
    free(x->drop_data);
    x->drop_data = NULL;
    x->drop_len = 0;
    if (data && len) {
        x->drop_data = malloc(len);
        if (x->drop_data) { memcpy(x->drop_data, data, len); x->drop_len = len; }
    }
    x->drop_ready = true;
    TR("stored drop %zu bytes mimes=%d\n", len, x->nmimes);
    mw_xdnd_wl_drop(d);
}

/* ------------------------------------------------------- XdndSelection serve */

static void post_notify(Display *d, Window requestor, Atom selection, Atom target,
                        Atom property, Time time)
{
    XSelectionEvent se;
    memset(&se, 0, sizeof se);
    se.type = SelectionNotify;
    se.display = d;
    se.requestor = requestor;
    se.selection = selection;
    se.target = target;
    se.property = property;
    se.time = time;
    mw_put_event(d, (XEvent *)&se);
}

bool mw_xdnd_xconvert(Display *d, Atom selection, Atom target, Atom property,
                      Window requestor, Time time)
{
    XDisplayImpl *dp = MWD(d);
    if (selection != a_sel(d)) return false;
    MwXdnd *x = dp->xdnd;
    if (!x) return false;

    Atom prop = property == None ? target : property;

    if (target == a_targets(d)) {
        long list[64];
        int n = 0;
        for (int i = 0; i < x->nmimes && n < 60; i++)
            list[n++] = (long)mw_intern_atom(d, x->mimes[i], False);
        XChangeProperty(d, requestor, prop, XA_ATOM, 32, PropModeReplace,
                        (const unsigned char *)list, n);
        post_notify(d, requestor, selection, target, prop, time);
        return true;
    }

    if (!x->drop_ready) return false;
    XChangeProperty(d, requestor, prop, target, 8, PropModeReplace,
                    x->drop_data, (int)x->drop_len);
    post_notify(d, requestor, selection, target, prop, time);
    TR("xconvert served %zu bytes type=%lu\n", x->drop_len, (unsigned long)target);
    return true;
}

/* --------------------------------------------------- target -> source msgs */

bool mw_xdnd_client_message(Display *d, XClientMessageEvent *cm)
{
    MwXdnd *x = MWD(d)->xdnd;
    if (!x || cm->window != x->src) return false;

    if (cm->message_type == a_status(d)) {
        TR("status accepted=%ld\n", cm->data.l[1] & 1);
        return true;
    }
    if (cm->message_type == a_finished(d)) {
        TR("finished\n");
        x->active = false;
        x->entered = false;
        x->dropped = false;
        return true;
    }
    return true;   /* any other message for our source window is ours to swallow */
}
