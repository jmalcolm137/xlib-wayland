/* event.c — XEvent queue, event retrieval, error delivery, synthetic events. */
#include "internal.h"

#include <X11/extensions/Xfixes.h>
#include <X11/extensions/Xdamage.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <time.h>

/* ------------------------------------------------------------ error handlers */

static XErrorHandler  g_error_handler;
static XIOErrorHandler g_io_handler;

void mw_set_error_handler(XErrorHandler h) { g_error_handler = h; }
XErrorHandler mw_error_handler(void) { return g_error_handler; }
void mw_set_io_error_handler(XIOErrorHandler h) { g_io_handler = h; }
XIOErrorHandler mw_io_error_handler_get(void) { return g_io_handler; }

int mw_io_error(Display *d, const char *reason)
{
    XDisplayImpl *dp = MWD(d);
    dp->closed = true;
    if (g_io_handler) return g_io_handler(d);
    /* Match Xlib's default IO error behaviour: the connection is gone, so
     * continuing is not meaningful. */
    fprintf(stderr, "XIO: fatal IO error: %s (Motif/Wayland)\n",
            reason ? reason : "connection closed");
    exit(1);
}

void mw_deliver_error(Display *d, int code, int request, int minor,
                      XID resource, int type)
{
    XDisplayImpl *dp = MWD(d);
    if (!dp->errq) {
        dp->errq_cap = 16;
        dp->errq = calloc(dp->errq_cap, sizeof *dp->errq);
    }
    XErrorEvent *e = &dp->errq[(dp->errq_head + dp->errq_count) % dp->errq_cap];
    memset(e, 0, sizeof *e);
    e->type = type ? type : 0;
    e->display = d;
    e->resourceid = resource;
    e->serial = dp->serial;
    e->error_code = (unsigned char)code;
    e->request_code = (unsigned char)request;
    e->minor_code = (unsigned char)minor;
    if (dp->errq_count < dp->errq_cap) dp->errq_count++;
    else dp->errq_head = (dp->errq_head + 1) % dp->errq_cap;
    /* Xlib reports errors even if no handler: default handler prints/aborts. */
    if (!g_error_handler) {
        dp->errq_head = (dp->errq_head + 1) % dp->errq_cap;
        if (dp->errq_count) dp->errq_count--;
    }
}

/* -------------------------------------------------------------- event queue */

static size_t mw_event_size(int type)
{
    switch (type) {
    case KeyPress: case KeyRelease:            return sizeof(XKeyEvent);
    case ButtonPress: case ButtonRelease:      return sizeof(XButtonEvent);
    case MotionNotify:                         return sizeof(XMotionEvent);
    case EnterNotify: case LeaveNotify:        return sizeof(XCrossingEvent);
    case FocusIn: case FocusOut:               return sizeof(XFocusChangeEvent);
    case KeymapNotify:                         return sizeof(XKeymapEvent);
    case Expose:                               return sizeof(XExposeEvent);
    case GraphicsExpose:                       return sizeof(XGraphicsExposeEvent);
    case NoExpose:                             return sizeof(XNoExposeEvent);
    case VisibilityNotify:                     return sizeof(XVisibilityEvent);
    case CreateNotify:                         return sizeof(XCreateWindowEvent);
    case DestroyNotify:                        return sizeof(XDestroyWindowEvent);
    case UnmapNotify:                          return sizeof(XUnmapEvent);
    case MapNotify:                            return sizeof(XMapEvent);
    case MapRequest:                           return sizeof(XMapRequestEvent);
    case ReparentNotify:                       return sizeof(XReparentEvent);
    case ConfigureNotify:                      return sizeof(XConfigureEvent);
    case ConfigureRequest:                     return sizeof(XConfigureRequestEvent);
    case GravityNotify:                        return sizeof(XGravityEvent);
    case ResizeRequest:                        return sizeof(XResizeRequestEvent);
    case CirculateNotify:                      return sizeof(XCirculateEvent);
    case CirculateRequest:                     return sizeof(XCirculateRequestEvent);
    case PropertyNotify:                       return sizeof(XPropertyEvent);
    case SelectionClear:                       return sizeof(XSelectionClearEvent);
    case SelectionRequest:                     return sizeof(XSelectionRequestEvent);
    case SelectionNotify:                      return sizeof(XSelectionEvent);
    case ColormapNotify:                       return sizeof(XColormapEvent);
    case ClientMessage:                        return sizeof(XClientMessageEvent);
    case MappingNotify:                        return sizeof(XMappingEvent);
    case GenericEvent:                         return sizeof(XGenericEventCookie);
    /* The XFIXES selection-notify event is larger than XAnyEvent; copy it in
     * full so its owner/selection/timestamp fields survive. */
    case MW_XFIXES_EVENT_BASE:                 return sizeof(XFixesSelectionNotifyEvent);
    case MW_XDAMAGE_EVENT_BASE:                return sizeof(XDamageNotifyEvent);
    default:                                   return sizeof(XAnyEvent);
    }
}

void mw_put_event(Display *d, XEvent *ev)
{
    XDisplayImpl *dp = MWD(d);
    /* A SelectionNotify for our hidden clipboard window belongs to an
     * in-flight X->Wayland transfer; completing it consumes the event. */
    if (ev->type == SelectionNotify && mw_xdnd_notify(d, &ev->xselection))
        return;
    if (ev->type == SelectionNotify && dp->clip_window != None &&
        ev->xselection.requestor == dp->clip_window &&
        mw_clipboard_serve_notify(d, &ev->xselection))
        return;
    /* Likewise, a SelectionNotify for the broker's proxy requestor window
     * belongs to a transfer we are servicing for another shim process. */
    if (ev->type == SelectionNotify &&
        mw_broker_serve_notify(d, &ev->xselection))
        return;
    static int trace = -1;
    static long evtotal;
    static long evcount[64];
    if (trace < 0) trace = getenv("MW_TRACE") != NULL;
    if (trace) {
        if (ev->xany.type < 64) evcount[ev->xany.type]++;
        if (++evtotal % 20000 == 0) {
            fprintf(stderr, "MW: events total=%ld:", evtotal);
            for (int i = 0; i < 64; i++) if (evcount[i])
                fprintf(stderr, " t%d=%ld", i, evcount[i]);
            fprintf(stderr, "\n");
            for (int i = 0; i < 64; i++) evcount[i] = 0;
        }
    }
    XEvent tmp;
    memset(&tmp, 0, sizeof tmp);
    memcpy(&tmp, ev, mw_event_size(ev->xany.type));
    tmp.xany.display = d;
    /* Serials must increase monotonically: clients compare them.  In
     * particular libXt only believes a SelectionClear (and so stops treating
     * itself as the selection owner and forwards the paste to the real owner)
     * when the event's serial is at least the one it recorded; ours carried 0,
     * so Xt kept serving locally and the Wayland clipboard never arrived. */
    if (tmp.xany.serial == 0)
        tmp.xany.serial = ++dp->serial;

    /* Coalesce the notifications a resize produces.  A drag delivers a stream
     * of sizes; queueing a ConfigureNotify and an Expose for every one of them
     * made the client re-lay out and repaint its whole window that many times,
     * which is what kept the window busy for a second or two after the drag
     * ended.  Only the newest size matters, and one Expose covering the union
     * of the reported areas repaints the same thing. */
    if (tmp.type == ConfigureNotify || tmp.type == Expose) {
        for (int i = 0; i < dp->qcount; i++) {
            XEvent *q = &dp->queue[(dp->qhead + i) % dp->qcap];
            if (q->type != tmp.type || q->xany.window != tmp.xany.window)
                continue;
            if (tmp.type == ConfigureNotify) {
                /* Keep the serial the client may already have observed and
                 * take the latest geometry. */
                unsigned long serial = q->xconfigure.serial;
                *q = tmp;
                q->xconfigure.serial = serial;
            } else if (tmp.xexpose.width >= q->xexpose.width &&
                       tmp.xexpose.height >= q->xexpose.height) {
                q->xexpose = tmp.xexpose;
            }
            dp->qlen = dp->qcount;
            return;
        }
    }
    if (!dp->queue) {
        dp->qcap = 256;
        dp->queue = calloc(dp->qcap, sizeof(XEvent));
    }
    if (dp->qcount == dp->qcap) {
        int ncap = dp->qcap * 2;
        XEvent *nq = calloc(ncap, sizeof(XEvent));
        for (int i = 0; i < dp->qcount; i++)
            nq[i] = dp->queue[(dp->qhead + i) % dp->qcap];
        free(dp->queue);
        dp->queue = nq;
        dp->qcap = ncap;
        dp->qhead = 0;
    }
    dp->queue[(dp->qhead + dp->qcount) % dp->qcap] = tmp;
    dp->qcount++;
    dp->qlen = dp->qcount;
}

Time mw_now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (Time)(ts.tv_sec * 1000ULL + ts.tv_nsec / 1000000ULL);
}

int mw_events_queued(Display *d, int mode)
{
    XDisplayImpl *dp = MWD(d);
    if (mode == QueuedAfterFlush) XFlush(d);
    if (mode != QueuedAlready) mw_process_events(d, false);
    (void)dp;
    return dp->qcount;
}

int XPending(Display *d)
{
    XDisplayImpl *dp = MWD(d);
    XFlush(d);
    mw_process_events(d, false);
    return dp->qcount;
}

int XEventsQueued(Display *d, int mode) { return mw_events_queued(d, mode); }

static XErrorEvent *pop_error(Display *d)
{
    XDisplayImpl *dp = MWD(d);
    if (dp->errq_count <= 0) return NULL;
    XErrorEvent *e = &dp->errq[dp->errq_head];
    dp->errq_head = (dp->errq_head + 1) % dp->errq_cap;
    dp->errq_count--;
    return e;
}

/* Deliver any queued X errors to the error handler, as a synchronous Xlib call
 * (XSync) does. */
void mw_dispatch_errors(Display *d)
{
    if (!g_error_handler) return;
    XErrorEvent *ee;
    while ((ee = pop_error(d)))
        g_error_handler(d, ee);
}

static bool pop_event(Display *d, XEvent *out)
{
    XDisplayImpl *dp = MWD(d);
    if (dp->qcount <= 0) return false;
    *out = dp->queue[dp->qhead];
    dp->qhead = (dp->qhead + 1) % dp->qcap;
    dp->qcount--;
    dp->qlen = dp->qcount;
    return true;
}

static void ensure_event(Display *d)
{
    XDisplayImpl *dp = MWD(d);
    while (dp->qcount == 0 && dp->errq_count == 0 && !dp->closed)
        mw_process_events(d, true);
}

int XNextEvent(Display *d, XEvent *event)
{
    XDisplayImpl *dp = MWD(d);
    for (;;) {
        ensure_event(d);
        XErrorEvent *ee = pop_error(d);
        if (ee) {
            if (g_error_handler) { g_error_handler(d, ee); continue; }
            continue;
        }
        if (pop_event(d, event)) {
            if (getenv("MW_TRACE"))
                fprintf(stderr, "MW: XNextEvent type=%d win=0x%lx send=%d (to GDK)\n",
                        event->xany.type, (unsigned long)event->xany.window,
                        event->xany.send_event);
            /* Xlib's XNextEvent returns 0, and clients loop on
             * `while (XNextEvent(dpy, &ev) == 0)` (rendercheck does). */
            return 0;
        }
        if (dp->closed) { memset(event, 0, sizeof *event); return 0; }
    }
}

int XPeekEvent(Display *d, XEvent *event)
{
    XDisplayImpl *dp = MWD(d);
    ensure_event(d);
    XErrorEvent *ee = pop_error(d);
    if (ee) { if (g_error_handler) { g_error_handler(d, ee); } }
    if (dp->qcount > 0) { *event = dp->queue[dp->qhead]; return 1; }
    memset(event, 0, sizeof *event);
    return 0;
}

int XPutBackEvent(Display *d, XEvent *event)
{
    XDisplayImpl *dp = MWD(d);
    if (!dp->queue) { mw_put_event(d, event); return 1; }
    if (dp->qcount == dp->qcap) mw_put_event(d, event);   /* grow then move */
    int idx = (dp->qhead - 1 + dp->qcap) % dp->qcap;
    dp->queue[idx] = *event;
    dp->qhead = idx;
    dp->qcount++;
    dp->qlen = dp->qcount;
    return 1;
}

/* ---------------------------------------------------------- event matching */

static bool matches_mask(XEvent *e, long mask)
{
    if (mask == 0) return true;
    /* Xlib answers mask-based requests through a type-to-mask table, because
     * the event type number is not the mask bit: a PropertyNotify is type 28
     * but is selected by PropertyChangeMask (bit 22).  Shifting the type only
     * coincided for a couple of events, and that stalled Motif's clipboard:
     * ClipboardGetCurrentTime() appends an empty property to the root window
     * and waits for the PropertyNotify, so XWindowEvent(root,
     * PropertyChangeMask) never matched the event XChangeProperty had just
     * delivered and Cut spun forever.  Table from libX11's evtomask.c. */
    static const long tbl[LASTEvent] = {
        [KeyPress]          = KeyPressMask,
        [KeyRelease]        = KeyReleaseMask,
        [ButtonPress]       = ButtonPressMask,
        [ButtonRelease]     = ButtonReleaseMask,
        [MotionNotify]      = PointerMotionMask | PointerMotionHintMask |
                              Button1MotionMask | Button2MotionMask |
                              Button3MotionMask | Button4MotionMask |
                              Button5MotionMask | ButtonMotionMask,
        [EnterNotify]       = EnterWindowMask,
        [LeaveNotify]       = LeaveWindowMask,
        [FocusIn]           = FocusChangeMask,
        [FocusOut]          = FocusChangeMask,
        [KeymapNotify]      = KeymapStateMask,
        [Expose]            = ExposureMask,
        [GraphicsExpose]    = ExposureMask,
        [NoExpose]          = ExposureMask,
        [VisibilityNotify]  = VisibilityChangeMask,
        [CreateNotify]      = SubstructureNotifyMask,
        [DestroyNotify]     = StructureNotifyMask | SubstructureNotifyMask,
        [UnmapNotify]       = StructureNotifyMask | SubstructureNotifyMask,
        [MapNotify]         = StructureNotifyMask | SubstructureNotifyMask,
        [MapRequest]        = SubstructureRedirectMask,
        [ReparentNotify]    = StructureNotifyMask | SubstructureNotifyMask,
        [ConfigureNotify]   = StructureNotifyMask | SubstructureNotifyMask,
        [ConfigureRequest]  = SubstructureRedirectMask,
        [GravityNotify]     = StructureNotifyMask | SubstructureNotifyMask,
        [ResizeRequest]     = ResizeRedirectMask,
        [CirculateNotify]   = StructureNotifyMask | SubstructureNotifyMask,
        [CirculateRequest]  = SubstructureRedirectMask,
        [PropertyNotify]    = PropertyChangeMask,
        [ColormapNotify]    = ColormapChangeMask,
    };
    long m = (e->xany.type > 0 && e->xany.type < LASTEvent)
                 ? tbl[e->xany.type] : 0;
    if (!(m & mask)) return false;
    /* A MotionNotify is also answered by a ButtonNMotionMask, but only when the
     * event's state reports that button held (libX11 masks it the same way). */
    if (e->xany.type == MotionNotify) {
        const long all_pointers = PointerMotionMask | PointerMotionHintMask |
                                  ButtonMotionMask;
        const long all_buttons = Button1MotionMask | Button2MotionMask |
                                 Button3MotionMask | Button4MotionMask |
                                 Button5MotionMask;
        if (!(mask & all_pointers) && !(mask & all_buttons & e->xmotion.state))
            return false;
    }
    return true;
}

int XMaskEvent(Display *d, long mask, XEvent *event)
{
    XDisplayImpl *dp = MWD(d);
    for (;;) {
        ensure_event(d);
        for (int i = 0; i < dp->qcount; i++) {
            XEvent *e = &dp->queue[(dp->qhead + i) % dp->qcap];
            if (matches_mask(e, mask)) {
                *event = *e;
                for (int j = i; j > 0; j--)
                    dp->queue[(dp->qhead + j) % dp->qcap] =
                        dp->queue[(dp->qhead + j - 1) % dp->qcap];
                dp->qhead = (dp->qhead + 1) % dp->qcap;
                dp->qcount--;
                return 1;
            }
        }
        XErrorEvent *ee = pop_error(d);
        if (ee) { if (g_error_handler) g_error_handler(d, ee); }
        if (dp->closed) { memset(event, 0, sizeof *event); return 0; }
        mw_block_for_events(d);
    }
}

int XCheckMaskEvent(Display *d, long mask, XEvent *event)
{
    XDisplayImpl *dp = MWD(d);
    mw_process_events(d, false);
    for (int i = 0; i < dp->qcount; i++) {
        XEvent *e = &dp->queue[(dp->qhead + i) % dp->qcap];
        if (matches_mask(e, mask)) {
            *event = *e;
            for (int j = i; j > 0; j--)
                dp->queue[(dp->qhead + j) % dp->qcap] =
                    dp->queue[(dp->qhead + j - 1) % dp->qcap];
            dp->qhead = (dp->qhead + 1) % dp->qcap;
            dp->qcount--;
            return 1;
        }
    }
    return 0;
}

static bool ev_is(XEvent *e, Window w, long mask, int type)
{
    if (type != 0 && e->xany.type != type) return false;
    if (w != None && e->xany.window != w) return false;
    if (mask && !matches_mask(e, mask)) return false;
    return true;
}

static int take_first(Display *d, Window w, long mask, int type, XEvent *out)
{
    XDisplayImpl *dp = MWD(d);
    for (int i = 0; i < dp->qcount; i++) {
        XEvent *e = &dp->queue[(dp->qhead + i) % dp->qcap];
        if (ev_is(e, w, mask, type)) {
            *out = *e;
            for (int j = i; j > 0; j--)
                dp->queue[(dp->qhead + j) % dp->qcap] =
                    dp->queue[(dp->qhead + j - 1) % dp->qcap];
            dp->qhead = (dp->qhead + 1) % dp->qcap;
            dp->qcount--;
            return 1;
        }
    }
    return 0;
}

/* XInput2 delivers its events as "cookies": the event carries a small header
 * and XGetEventData() fetches the detail, XFreeEventData() releases it.  The
 * shim generates no generic events, so there is never anything to fetch, but
 * libXi resolves both symbols at load time and every client calls them for the
 * generic events it receives. */
Bool XGetEventData(Display *d, XGenericEventCookie *cookie)
{
    (void)d;
    if (!cookie || cookie->type != GenericEvent) return False;
    return cookie->data != NULL;
}

void XFreeEventData(Display *d, XGenericEventCookie *cookie)
{
    (void)d;
    if (!cookie) return;
    free(cookie->data);
    cookie->data = NULL;
}

int XWindowEvent(Display *d, Window w, long mask, XEvent *event)
{
    for (;;) {
        ensure_event(d);
        if (take_first(d, w, mask, 0, event)) return 1;
        XErrorEvent *ee = pop_error(d);
        if (ee) { if (g_error_handler) g_error_handler(d, ee); continue; }
        if (MWD(d)->closed) { memset(event, 0, sizeof *event); return 0; }
        /* Nothing queued matches, but the queue may hold events for other
         * windows or masks.  ensure_event() only blocks on an empty queue, so
         * without this the loop spun at 100% CPU instead of waiting for the
         * event being asked for (Xlib leaves the other events queued). */
        mw_block_for_events(d);
    }
}

int XCheckWindowEvent(Display *d, Window w, long mask, XEvent *event)
{
    mw_process_events(d, false);
    return take_first(d, w, mask, 0, event);
}

int XCheckTypedWindowEvent(Display *d, Window w, int type, XEvent *event)
{
    mw_process_events(d, false);
    return take_first(d, w, 0, type, event);
}

int XCheckTypedEvent(Display *d, int type, XEvent *event)
{
    mw_process_events(d, false);
    return take_first(d, None, 0, type, event);
}

Bool XIfEvent(Display *d, XEvent *event, Bool (*pred)(), XPointer arg)
{
    for (;;) {
        ensure_event(d);
        for (int i = 0; i < MWD(d)->qcount; i++) {
            XEvent *e = &MWD(d)->queue[(MWD(d)->qhead + i) % MWD(d)->qcap];
            if (pred(d, e, arg)) {
                *event = *e;
                for (int j = i; j > 0; j--)
                    MWD(d)->queue[(MWD(d)->qhead + j) % MWD(d)->qcap] =
                        MWD(d)->queue[(MWD(d)->qhead + j - 1) % MWD(d)->qcap];
                MWD(d)->qhead = (MWD(d)->qhead + 1) % MWD(d)->qcap;
                MWD(d)->qcount--;
                return True;
            }
        }
        XErrorEvent *ee = pop_error(d);
        if (ee) { if (g_error_handler) g_error_handler(d, ee); }
        if (MWD(d)->closed) return False;
        mw_block_for_events(d);
    }
}

Bool XCheckIfEvent(Display *d, XEvent *event, Bool (*pred)(), XPointer arg)
{    mw_process_events(d, false);
    for (int i = 0; i < MWD(d)->qcount; i++) {
        XEvent *e = &MWD(d)->queue[(MWD(d)->qhead + i) % MWD(d)->qcap];
        if (pred(d, e, arg)) {
            *event = *e;
            for (int j = i; j > 0; j--)
                MWD(d)->queue[(MWD(d)->qhead + j) % MWD(d)->qcap] =
                    MWD(d)->queue[(MWD(d)->qhead + j - 1) % MWD(d)->qcap];
            MWD(d)->qhead = (MWD(d)->qhead + 1) % MWD(d)->qcap;
            MWD(d)->qcount--;
            return True;
        }
    }
    return False;
}

Bool XPeekIfEvent(Display *d, XEvent *event, Bool (*pred)(), XPointer arg)
{
    for (;;) {
        for (int i = 0; i < MWD(d)->qcount; i++) {
            XEvent *e = &MWD(d)->queue[(MWD(d)->qhead + i) % MWD(d)->qcap];
            if (pred(d, e, arg)) { *event = *e; return True; }
        }
        if (MWD(d)->closed) return False;
        mw_block_for_events(d);
        if (MWD(d)->closed && MWD(d)->qcount == 0) return False;
    }
}

int XSendEvent(Display *d, Window w, Bool propagate, long event_mask, XEvent *event)
{
    MwWindow *win = mw_window(d, w);
    if (getenv("MW_TRACE"))
        fprintf(stderr, "MW: XSendEvent win=0x%lx prop=%d mask=0x%lx type=%d win_valid=%d\n",
                (unsigned long)w, propagate, event_mask, event->xany.type, win != NULL);
    if (!win) return 0;
    /* A window the DnD bridge created stands in for a client in another
     * process, so an event addressed to it has left this process: do not loop
     * it back to the sender.  Motif's drop receiver sends its
     * receiver-to-initiator messages (DROP_SITE_ENTER, DROP_FINISH, ...) to the
     * initiator's window; with that window here they came straight back and
     * Motif processed its own messages as an initiator, corrupting the drop
     * transfer (the dropped file list never got requested). */
    if (mw_dnd_owns_window(d, w)) return 1;
    /* Our synthetic XDND source window: the drop site's XdndStatus/XdndFinished
     * comes here; consume it rather than looping it back. */
    if (mw_xdnd_owns_window(d, w)) {
        if (event->type == ClientMessage)
            mw_xdnd_client_message(d, &event->xclient);
        return 1;
    }
    /* A client asking the window manager to change a window state sends an
     * EWMH _NET_WM_STATE ClientMessage to the root window; we *are* the
     * window manager, so consume it and act on it rather than handing it
     * back to the client. */
    if (event->type == ClientMessage && w == MWSCR(d)->root &&
        event->xclient.message_type ==
            mw_intern_atom(d, "_NET_WM_STATE", True)) {
        mw_wm_net_wm_state(d, &event->xclient);
        return 1;
    }
    /* Likewise _NET_WM_MOVERESIZE: a client-side-decorated window asks us to
     * start an interactive move/resize (this is how a Firefox title-bar drag
     * reaches the compositor). */
    if (event->type == ClientMessage && w == MWSCR(d)->root &&
        event->xclient.message_type ==
            mw_intern_atom(d, "_NET_WM_MOVERESIZE", True)) {
        mw_wm_moveresize(d, &event->xclient);
        return 1;
    }
    event->xany.send_event = True;
    event->xany.display = d;
    if (win->event_mask & event_mask || event_mask == 0 ||
        (propagate && (win->event_mask & 0)))
        mw_put_event(d, event);
    else
        mw_put_event(d, event);   /* deliver anyway: this is an in-process server */
    return 1;
}

int XAllowEvents(Display *d, int mode, Time time)
{ (void)d; (void)mode; (void)time; return 1; }

/* ------------------------------------------------------- synthetic events */

void mw_send_configure_notify(Display *d, MwWindow *win)
{
    int ox = 0, oy = 0;
    mw_window_origin(win, &ox, &oy);
    XConfigureEvent ce;
    memset(&ce, 0, sizeof ce);
    ce.type = ConfigureNotify;
    ce.display = d;
    ce.event = win->id;
    ce.window = win->id;
    ce.x = win->x;
    ce.y = win->y;
    ce.width = win->w;
    ce.height = win->h;
    ce.border_width = win->border_width;
    ce.above = None;
    ce.override_redirect = win->override_redirect;
    ce.serial = MWD(d)->serial++;
    mw_put_event(d, (XEvent *)&ce);
}

void mw_send_client_message(Display *d, MwWindow *win, Atom type,
                            long d0, long d1, long d2, long d3, long d4)
{
    XClientMessageEvent cm;
    memset(&cm, 0, sizeof cm);
    cm.type = ClientMessage;
    cm.display = d;
    cm.window = win->id;
    cm.message_type = type;
    cm.format = 32;
    cm.data.l[0] = d0; cm.data.l[1] = d1; cm.data.l[2] = d2;
    cm.data.l[3] = d3; cm.data.l[4] = d4;
    cm.serial = MWD(d)->serial++;
    mw_put_event(d, (XEvent *)&cm);
}
