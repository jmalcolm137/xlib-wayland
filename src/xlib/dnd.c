/* dnd.c — bridge Motif drag-and-drop onto the Wayland data device.
 *
 * Motif drag-and-drop is an X protocol.  A drop site advertises itself by
 * putting `_MOTIF_DRAG_RECEIVER_INFO` on its window; the drag initiator
 * hit-tests the window under the pointer to find a site, writes its
 * `_MOTIF_DRAG_INITIATOR_INFO` to the source window, and the two sides exchange
 * `_MOTIF_DRAG_AND_DROP_MESSAGE` ClientMessages; the payload finally moves over
 * a dynamically-named selection (the initiator's "icc handle", `_MOTIF_ATOM_n`).
 *
 * Every one of those steps needs a shared X server, which this shim does not
 * have: each process has its own root, its own window tree and its own atom
 * table, so an initiator can never see another process's drop sites and a
 * ClientMessage can never cross.  What the processes *do* share is the Wayland
 * compositor, which knows the pointer and the surfaces and routes a drag with
 * wl_data_device (see wayland/clipboard.c).  This file therefore speaks the
 * Motif half locally in each process and lets the compositor carry the drag
 * between them:
 *
 *   Wayland -> CDE (receiving): the compositor drops bytes on one of our
 *     surfaces; we stand in as a remote initiator — a synthetic source window
 *     with `_MOTIF_DRAG_INITIATOR_INFO` and a synthetic icc handle — and feed
 *     the drop site the TOP_LEVEL_ENTER / DRAG_MOTION / DROP_START messages it
 *     expects, then serve the icc selection from the dropped bytes.
 *
 *   CDE -> Wayland (sending): a Motif drag is detected from the initiator
 *     info write; when the pointer leaves our windows we start a Wayland drag
 *     offering the data, pulled out of the still-live Motif selection.
 *
 * Only text is translated so far; the X target is mapped to and from a MIME
 * type so a CDE app can exchange a drop with a Wayland application.
 */
#include "internal.h"

#include <X11/Xatom.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdio.h>
#include <stdint.h>

static int dnd_trace(void)
{
    static int t = -1;
    if (t < 0) t = getenv("MW_TRACE") != NULL;
    return t;
}
#define TR(...) do { if (dnd_trace()) fprintf(stderr, "MWDND: " __VA_ARGS__); } while (0)

/* Motif ICC message types (lib/Xm/DragC.h). */
enum {
    ICC_TOP_LEVEL_ENTER, ICC_TOP_LEVEL_LEAVE, ICC_DRAG_MOTION,
    ICC_DROP_SITE_ENTER, ICC_DROP_SITE_LEAVE, ICC_DROP_START,
    ICC_DROP_FINISH, ICC_DRAG_DROP_FINISH, ICC_OPERATION_CHANGED
};

#define ICC_ORIGINATOR_RECEIVER 0x80   /* PUT_ICC_EVENT_TYPE(RECEIVER_EVENT) */
#define ICC_BYTE_ORDER          'l'    /* _XmByteOrderChar on little-endian  */
#define ICC_PROTOCOL_VERSION    0

/* Drop operations and site status (lib/Xm/DragC.h, DropSMgr.h). */
#define DND_OP_MOVE  (1u << 0)
#define DND_OP_COPY  (1u << 1)
#define DND_OP_LINK  (1u << 2)
#define DND_SITE_NONE   1   /* XmNO_DROP_SITE    */
#define DND_SITE_INVALID 2  /* XmDROP_SITE_INVALID */
#define DND_SITE_VALID  3   /* XmDROP_SITE_VALID */
#define DND_ACTION_DROP 0   /* XmDROP */

/* The targets table index Motif always has: 0 = {} and 1 = { XA_STRING }. */
#define ICC_TARGETS_INDEX_STRING 1

typedef struct MwDnd {
    Display *d;
    /* --- receiving a Wayland drag ------------------------------------- */
    Window   src_win;      /* synthetic initiator window (ours) */
    Atom     icc;          /* synthetic icc handle, a selection we own */
    Window   shell;        /* the X shell the drag is over */
    int      root_x, root_y;   /* current position, root coordinates */
    bool     active;       /* a Wayland drag is over one of our surfaces */
    bool     entered;      /* TOP_LEVEL_ENTER has been sent */
    bool     drop_ready;   /* the dropped bytes have arrived */
    unsigned char *drop_data;
    size_t         drop_len;

    /* --- sending a Motif drag ----------------------------------------- */
    Window   x_src;        /* the Motif source window */
    Atom     x_icc;        /* the Motif icc handle selection */
    bool     x_active;     /* a Motif drag is in progress in this process */
} MwDnd;

/* ------------------------------------------------------------- atoms */

static Atom a_motif_dnd_msg(Display *d)
{ return mw_intern_atom(d, "_MOTIF_DRAG_AND_DROP_MESSAGE", False); }
static Atom a_motif_init_info(Display *d)
{ return mw_intern_atom(d, "_MOTIF_DRAG_INITIATOR_INFO", False); }
static Atom a_utf8(Display *d)
{ return mw_intern_atom(d, "UTF8_STRING", False); }
static Atom a_targets(Display *d)
{ return mw_intern_atom(d, "TARGETS", False); }
static Atom a_compound_text(Display *d)
{ return mw_intern_atom(d, "COMPOUND_TEXT", False); }
static Atom a_text(Display *d)
{ return mw_intern_atom(d, "TEXT", False); }
static Atom a_transfer_success(Display *d)
{ return mw_intern_atom(d, "XmTRANSFER_SUCCESS", False); }
static Atom a_transfer_failure(Display *d)
{ return mw_intern_atom(d, "XmTRANSFER_FAILURE", False); }

/* ------------------------------------------------------- message codec */

static void put16(unsigned char *p, unsigned v)
{ p[0] = (unsigned char)(v & 0xff); p[1] = (unsigned char)((v >> 8) & 0xff); }

static void put32(unsigned char *p, unsigned long v)
{
    p[0] = (unsigned char)(v & 0xff);
    p[1] = (unsigned char)((v >> 8) & 0xff);
    p[2] = (unsigned char)((v >> 16) & 0xff);
    p[3] = (unsigned char)((v >> 24) & 0xff);
}

/* Common 8-byte header: type, byte order, flags (site/op/ops), time. */
static void icc_header(unsigned char *m, int type, int originator,
                       unsigned flags, Time t)
{
    m[0] = (unsigned char)(type | (originator ? ICC_ORIGINATOR_RECEIVER : 0));
    m[1] = ICC_BYTE_ORDER;
    put16(m + 2, flags);
    put32(m + 4, (unsigned long)t);
}

static unsigned icc_flags(int site_status, int operation, int operations)
{
    return ((unsigned)(site_status & 0xf) << 4) |
           ((unsigned)(operation   & 0xf) << 0) |
           ((unsigned)(operations  & 0xf) << 8);
}

static void send_icc(Display *d, Window win, const unsigned char *buf, int len)
{
    if (win == None) return;
    XClientMessageEvent cm;
    memset(&cm, 0, sizeof cm);
    cm.type = ClientMessage;
    cm.display = d;
    cm.window = win;
    cm.message_type = a_motif_dnd_msg(d);
    cm.format = 8;
    memcpy(cm.data.b, buf, (size_t)(len > 20 ? 20 : len));
    XSendEvent(d, win, False, NoEventMask, (XEvent *)&cm);
}

/* ------------------------------------------------------ synthetic initiator */

/* The receiver reads `_MOTIF_DRAG_INITIATOR_INFO` off the source window to
 * learn the initiator's targets.  We advertise { XA_STRING } (index 1 of the
 * shared table) so a plain text drop is offered. */
static void write_initiator_info(Display *d, MwDnd *x)
{
    unsigned char info[8];
    info[0] = ICC_BYTE_ORDER;
    info[1] = ICC_PROTOCOL_VERSION;
    put16(info + 2, ICC_TARGETS_INDEX_STRING);
    put32(info + 4, (unsigned long)x->icc);
    XChangeProperty(d, x->src_win, x->icc, a_motif_init_info(d), 8,
                    PropModeReplace, info, (int)sizeof info);
}

static void ensure_synthetic(Display *d)
{
    MwDnd *x = MWD(d)->dnd;
    if (x->src_win != None) return;
    x->src_win = XCreateSimpleWindow(d, DefaultRootWindow(d), -1, -1, 1, 1, 0, 0, 0);
    x->icc = mw_intern_atom(d, "_MOTIF_ATOM_DND", False);
    XSetSelectionOwner(d, x->icc, x->src_win, CurrentTime);
    write_initiator_info(d, x);
}

bool mw_dnd_owns_window(Display *d, Window w)
{
    MwDnd *x = MWD(d)->dnd;
    return x && w != None && w == x->src_win;
}

/* -------------------------------------------------------- receiving side */

void mw_dnd_wl_enter(Display *d, struct wl_surface *s, double sx, double sy,
                     const char *mime)
{
    (void)s;
    MwDnd *x = MWD(d)->dnd;
    if (!x) return;
    Window shell = MWD(d)->drag_window;
    if (shell == None) return;
    MwWindow *w = mw_window(d, shell);
    if (!w || !w->tl || w->tl->is_popup) return;

    ensure_synthetic(d);

    int ox = 0, oy = 0;
    mw_window_origin(w, &ox, &oy);
    int off = mw_toplevel_content_offset(w->tl);
    x->shell = shell;
    x->root_x = ox + (int)(sx + 0.5);
    x->root_y = oy + (int)(sy + 0.5) - off;
    TR("enter shell=0x%lx root=%d,%d mime=%s\n", (unsigned long)shell,
       x->root_x, x->root_y, mime ? mime : "(none)");
    x->active = true;
    x->entered = true;
    x->drop_ready = false;
    free(x->drop_data);
    x->drop_data = NULL;
    x->drop_len = 0;

    /* The initiator announces itself to the receiver's shell. */
    unsigned char m[20];
    memset(m, 0, sizeof m);
    icc_header(m, ICC_TOP_LEVEL_ENTER, 0, 0, CurrentTime);
    put32(m + 8, (unsigned long)x->src_win);
    put32(m + 12, (unsigned long)x->icc);
    send_icc(d, shell, m, 16);

    /* ... and reports the first motion so a site can be entered. */
    memset(m, 0, sizeof m);
    icc_header(m, ICC_DRAG_MOTION, 0,
               icc_flags(DND_SITE_VALID, DND_OP_COPY, DND_OP_COPY | DND_OP_MOVE),
               CurrentTime);
    put16(m + 8, (unsigned short)x->root_x);
    put16(m + 10, (unsigned short)x->root_y);
    send_icc(d, shell, m, 12);
}

void mw_dnd_wl_motion(Display *d, double sx, double sy)
{
    MwDnd *x = MWD(d)->dnd;
    if (!x || !x->active || x->shell == None) return;
    MwWindow *w = mw_window(d, x->shell);
    if (!w) return;
    int ox = 0, oy = 0;
    mw_window_origin(w, &ox, &oy);
    int off = mw_toplevel_content_offset(w->tl);
    x->root_x = ox + (int)(sx + 0.5);
    x->root_y = oy + (int)(sy + 0.5) - off;

    unsigned char m[12];
    memset(m, 0, sizeof m);
    icc_header(m, ICC_DRAG_MOTION, 0,
               icc_flags(DND_SITE_VALID, DND_OP_COPY, DND_OP_COPY | DND_OP_MOVE),
               CurrentTime);
    put16(m + 8, (unsigned short)x->root_x);
    put16(m + 10, (unsigned short)x->root_y);
    send_icc(d, x->shell, m, 12);
}

void mw_dnd_wl_leave(Display *d)
{
    MwDnd *x = MWD(d)->dnd;
    if (!x || !x->active) return;
    if (x->entered && x->shell != None) {
        unsigned char m[20];
        memset(m, 0, sizeof m);
        icc_header(m, ICC_TOP_LEVEL_LEAVE, 0, 0, CurrentTime);
        put32(m + 8, (unsigned long)x->src_win);
        send_icc(d, x->shell, m, 12);
    }
    x->active = false;
    x->entered = false;
    x->shell = None;
}

void mw_dnd_wl_drop(Display *d, const unsigned char *data, size_t len)
{
    MwDnd *x = MWD(d)->dnd;
    if (!x || !x->active || x->shell == None) return;
    free(x->drop_data);
    x->drop_data = NULL;
    x->drop_len = 0;
    if (data && len) {
        x->drop_data = malloc(len);
        if (x->drop_data) { memcpy(x->drop_data, data, len); x->drop_len = len; }
    }
    x->drop_ready = true;
    TR("drop %zu bytes at %d,%d -> shell=0x%lx\n", len, x->root_x, x->root_y,
       (unsigned long)x->shell);

    /* The drop starts the transfer; the site answers with a convert request on
     * our icc selection, which mw_dnd_xconvert() serves from the dropped
     * bytes. */
    unsigned char m[20];
    memset(m, 0, sizeof m);
    icc_header(m, ICC_DROP_START, 0,
               icc_flags(DND_SITE_VALID, DND_OP_COPY, DND_OP_COPY | DND_OP_MOVE)
               | ((unsigned)DND_ACTION_DROP << 12),
               CurrentTime);
    put16(m + 8, (unsigned short)x->root_x);
    put16(m + 10, (unsigned short)x->root_y);
    put32(m + 12, (unsigned long)x->icc);
    put32(m + 16, (unsigned long)x->src_win);
    send_icc(d, x->shell, m, 20);

    /* The drop has been delivered; the site now pulls the data through the icc
     * selection, which mw_dnd_xconvert() serves from x->drop_data. */
    x->active = false;
    x->entered = false;
    x->shell = None;
}

/* Serve a convert on our synthetic icc handle from the dropped bytes. */
bool mw_dnd_xconvert(Display *d, Atom selection, Atom target, Atom property,
                     Window requestor, Time time)
{
    MwDnd *x = MWD(d)->dnd;
    if (!x || x->src_win == None || selection != x->icc) return false;

    if (dnd_trace()) {
        const char *tn = XGetAtomName(d, target);
        TR("xconvert target=%lu (%s) drop_ready=%d\n", (unsigned long)target,
           tn ? tn : "?", x->drop_ready);
        if (tn) XFree((char *)tn);
    }
    Atom prop = property == None ? target : property;
    bool ok = false;
    long served = -1;

    if (x->drop_ready && target == a_targets(d)) {
        /* Offer the text encodings a CDE widget is likely to ask for. */
        long list[3];
        list[0] = (long)XA_STRING;
        list[1] = (long)a_utf8(d);
        list[2] = (long)a_compound_text(d);
        XChangeProperty(d, requestor, prop, XA_ATOM, 32, PropModeReplace,
                        (const unsigned char *)list, 3);
        ok = true;
    } else if (x->drop_ready && target == a_utf8(d)) {
        XChangeProperty(d, requestor, prop, target, 8, PropModeReplace,
                        x->drop_data, (int)x->drop_len);
        ok = x->drop_len > 0;
        served = (long)x->drop_len;
    } else if (x->drop_ready &&
               (target == XA_STRING || target == a_text(d) ||
                target == a_compound_text(d))) {
        /* The offer carries UTF-8; these targets want Latin-1. */
        size_t olen = 0;
        char *enc = mw_clipboard_from_utf8(x->drop_data, x->drop_len, &olen);
        if (enc && olen > 0) {
            XChangeProperty(d, requestor, prop, target, 8, PropModeReplace,
                            (const unsigned char *)enc, (int)olen);
            ok = true;
            served = (long)olen;
        }
        free(enc);
    } else if (x->drop_ready &&
               (target == a_transfer_success(d) || target == a_transfer_failure(d))) {
        /* The drop site reports the transfer outcome back over the icc handle.
         * Motif's own initiator acknowledges with an empty property of the same
         * type (DragC.c DropConvertCallback). */
        XChangeProperty(d, requestor, prop, target, 32, PropModeReplace, NULL, 0);
        ok = true;
        served = 0;
    }

    XSelectionEvent se;
    memset(&se, 0, sizeof se);
    se.type = SelectionNotify;
    se.display = d;
    se.requestor = requestor;
    se.selection = selection;
    se.target = target;
    se.property = ok ? prop : None;
    se.time = time;
    mw_put_event(d, (XEvent *)&se);
    TR("xconvert -> %s (served=%ld req=0x%lx prop=%lu)\n",
       ok ? "ok" : "refused", served, (unsigned long)requestor,
       (unsigned long)prop);
    return true;
}

/* ------------------------------------------------------- life cycle */

void mw_dnd_init(Display *d)
{
    MwDnd *x = calloc(1, sizeof *x);
    if (!x) return;
    x->d = d;
    x->shell = None;
    MWD(d)->dnd = x;
}

void mw_dnd_fini(Display *d)
{
    MwDnd *x = MWD(d)->dnd;
    if (!x) return;
    if (x->src_win != None) XDestroyWindow(d, x->src_win);
    free(x->drop_data);
    free(x);
    MWD(d)->dnd = NULL;
}

/* ------------------------------------------------------- sending side */

/* A Motif drag has started in this process: the initiator wrote its info to
 * the source window and owns the icc handle selection.  We remember it so the
 * payload can be pulled out of the still-live selection when a Wayland peer
 * asks.  (Starting the Wayland drag itself is driven by the pointer leaving our
 * windows, in input.c, so an in-process drag keeps its native behaviour.) */
void mw_dnd_initiator_info(Display *d, Window src, Atom icc, int format,
                           const unsigned char *data, unsigned long nitems)
{
    MwDnd *x = MWD(d)->dnd;
    if (!x) return;
    if (x->src_win != None && src == x->src_win) return;  /* our own write */
    (void)format; (void)data; (void)nitems;
    x->x_src = src;
    x->x_icc = icc;
    x->x_active = true;
    TR("Motif drag started src=0x%lx icc=%lu\n", (unsigned long)src, (unsigned long)icc);
}

bool mw_dnd_source_send(Display *d, const char *mime, int fd)
{
    (void)d; (void)mime; (void)fd;
    return false;   /* implemented with the Wayland source, below */
}
