/* dnd.c — bridge Motif drag-and-drop onto the Wayland data device.
 *
 * Motif drag-and-drop is an X protocol.  A drop site advertises itself by
 * putting `_MOTIF_DRAG_RECEIVER_INFO` on its window; the drag initiator
 * hit-tests the window under the pointer to find a site, writes its
 * `_MOTIF_DRAG_INITIATOR_INFO` to the source window, and the two sides exchange
 * `_MOTIF_DRAG_AND_DROP_MESSAGE` ClientMessages; the payload finally moves over
 * a dynamically-named selection (the initiator's "icc handle", `_MOTIF_ATOM_n`)
 * with a target such as FILE_NAME (files), TEXT/STRING (text).
 *
 * Every one of those steps needs a shared X server, which this shim does not
 * have: each process has its own root, window tree and atom table, so an
 * initiator can never see another process's drop sites and a ClientMessage can
 * never cross.  What the processes do share is the Wayland compositor, which
 * knows the pointer and the surfaces, and the selection broker, which already
 * relays selections between shim processes.
 *
 * So: the compositor routes the *drag* (wl_data_device), and the broker carries
 * the *transfer*.  The source shim starts a Wayland drag offering a private MIME
 * that names the initiator's icc handle and the targets it can serve; the
 * destination shim plays remote initiator to its drop site -- a synthetic
 * source window with `_MOTIF_DRAG_INITIATOR_INFO`, the same icc handle, and a
 * target list registered in the shared `_MOTIF_DRAG_TARGETS` table -- then feeds
 * it TOP_LEVEL_ENTER / DRAG_MOTION / DROP_START.  The site's convert requests
 * land on the (not locally owned) icc selection and the broker relays them to
 * the real initiator, exactly as a selection paste would.
 *
 * A drag from a plain Wayland application has no X initiator: there the
 * destination shim owns the icc handle itself and serves it from the dropped
 * bytes (STRING/TEXT/UTF8).
 */
#include "internal.h"

#include <wayland-client.h>
#include <X11/Xatom.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdio.h>
#include <stdint.h>
#include <time.h>
#include <unistd.h>

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
#define DND_SITE_VALID  3   /* XmDROP_SITE_VALID */
#define DND_ACTION_DROP 0   /* XmDROP */

/* The targets table index Motif always has: 0 = {} and 1 = { XA_STRING }. */
#define ICC_TARGETS_INDEX_STRING 1

/* How long to keep the initiator's selection owned after the Wayland drag ends,
 * so the destination's convert request (relayed by the broker) can complete
 * before Motif disowns it. */
#define DND_RELEASE_GRACE_MS 4000

#define MOTIF_DRAG_MIME_PREFIX "application/x-motif-drag;"

typedef struct MwDnd {
    Display *d;

    /* --- receiving a drag --------------------------------------------- */
    Window   src_win;      /* synthetic initiator window (ours) */
    Atom     icc;          /* transfer selection (our own, or the source's) */
    bool     remote;       /* the icc selection names a remote initiator */
    unsigned targets_index; /* our entry in the shared targets table */
    Window   shell;        /* the X shell the drag is over */
    int      root_x, root_y;   /* current position, root coordinates */
    bool     active;       /* a drag is over one of our surfaces */
    bool     entered;      /* TOP_LEVEL_ENTER has been sent */
    bool     drop_ready;   /* the dropped bytes have arrived (Wayland source) */
    unsigned char *drop_data;
    size_t         drop_len;

    /* --- sending a Motif drag ----------------------------------------- */
    Window   x_src;        /* the Motif source window */
    Atom     x_icc;        /* the Motif icc handle selection */
    bool     x_active;     /* a Motif drag is in progress in this process */
    unsigned x_targets_index;
    char    *payload;      /* hex of "icc\0target\0..." to advertise */
    bool     release_pending;
    uint64_t release_at;
} MwDnd;

static int dnd_trace(void)
{
    static int t = -1;
    if (t < 0) t = getenv("MW_TRACE") != NULL || getenv("MW_DND_TRACE") != NULL;
    return t;
}
#define TR(...) do { if (dnd_trace()) { fprintf(stderr, "MWDND[%d] ", (int)getpid()); fprintf(stderr, __VA_ARGS__); } } while (0)

static uint64_t dnd_now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)(ts.tv_nsec / 1000000);
}

/* ------------------------------------------------------------- atoms */

static Atom a_motif_dnd_msg(Display *d)
{ return mw_intern_atom(d, "_MOTIF_DRAG_AND_DROP_MESSAGE", False); }
static Atom a_motif_init_info(Display *d)
{ return mw_intern_atom(d, "_MOTIF_DRAG_INITIATOR_INFO", False); }
static Atom a_motif_targets(Display *d)
{ return mw_intern_atom(d, "_MOTIF_DRAG_TARGETS", False); }
static Atom a_motif_window(Display *d)
{ return mw_intern_atom(d, "_MOTIF_DRAG_WINDOW", False); }
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
static Atom a_host_name(Display *d)
{ return mw_intern_atom(d, "HOST_NAME", False); }
static Atom a_sun_host(Display *d)
{ return mw_intern_atom(d, "_SUN_FILE_HOST_NAME", False); }

/* ------------------------------------------------------------- hex */

static void hex_encode(const unsigned char *in, size_t n, char *out)
{
    static const char *h = "0123456789abcdef";
    for (size_t i = 0; i < n; i++) {
        out[i * 2] = h[in[i] >> 4];
        out[i * 2 + 1] = h[in[i] & 0xf];
    }
    out[n * 2] = 0;
}

static int hex_val(int c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static size_t hex_decode(const char *in, unsigned char *out, size_t outsz)
{
    size_t n = 0;
    while (in[0] && in[1] && n < outsz) {
        int h = hex_val(in[0]), l = hex_val(in[1]);
        if (h < 0 || l < 0) break;
        out[n++] = (unsigned char)((h << 4) | l);
        in += 2;
    }
    return n;
}

/* ------------------------------------------------- Motif tables */

/* Motif keeps a persistent drag window (an InputOnly child of the root) that
 * holds the shared atom and targets tables. */
static Window motif_window(Display *d)
{
    Atom prop = a_motif_window(d);
    Atom type; int fmt; unsigned long n = 0, after = 0;
    unsigned char *data = NULL;
    Window w = None;
    if (XGetWindowProperty(d, DefaultRootWindow(d), prop, 0, 1, False, XA_WINDOW,
                           &type, &fmt, &n, &after, &data) == Success && data) {
        if (fmt == 32 && n >= 1) {
            uint32_t v;
            memcpy(&v, data, 4);
            w = (Window)v;
        }
        XFree(data);
    }
    return w;
}

/* Read the targets table property.  Returns a malloc'd buffer and its length,
 * or NULL.  *count is the number of target lists. */
static unsigned char *targets_read(Display *d, Window mw, unsigned long *len,
                                   unsigned *count)
{
    Atom prop = a_motif_targets(d);
    Atom type; int fmt; unsigned long n = 0, after = 0;
    unsigned char *data = NULL;
    if (mw == None) return NULL;
    if (XGetWindowProperty(d, mw, prop, 0, 0x7fffffff, False, prop,
                           &type, &fmt, &n, &after, &data) != Success)
        return NULL;
    if (!data || fmt != 8 || n < 8) { if (data) XFree(data); return NULL; }
    *len = n;
    *count = (unsigned)data[2] | ((unsigned)data[3] << 8);
    return data;
}

/* Find (or append) a target list; returns its index, or -1. */
static int targets_index(Display *d, Window mw, const Atom *list, int n)
{
    unsigned long len = 0;
    unsigned count = 0;
    unsigned char *data = targets_read(d, mw, &len, &count);
    if (!data) return -1;

    /* search for an existing identical list */
    size_t off = 8;
    for (unsigned i = 0; i < count && off + 2 <= len; i++) {
        int num = data[off] | (data[off + 1] << 8);
        size_t lob = (size_t)num * 4;
        if (off + 2 + lob > len) break;
        if (num == n) {
            int same = 1;
            for (int j = 0; j < n; j++) {
                uint32_t v;
                memcpy(&v, data + off + 2 + (size_t)j * 4, 4);
                if (v != (uint32_t)list[j]) { same = 0; break; }
            }
            if (same) { XFree(data); return (int)i; }        }
        off += 2 + lob;
    }

    /* append a new list */
    size_t add = 2 + (size_t)n * 4;
    unsigned char *buf = malloc(len + add);
    if (!buf) { XFree(data); return -1; }
    memcpy(buf, data, len);
    buf[len] = (unsigned char)(n & 0xff);
    buf[len + 1] = (unsigned char)((n >> 8) & 0xff);
    for (int j = 0; j < n; j++) {
        uint32_t v = (uint32_t)list[j];
        memcpy(buf + len + 2 + (size_t)j * 4, &v, 4);
    }
    unsigned newcount = count + 1;
    buf[2] = (unsigned char)(newcount & 0xff);
    buf[3] = (unsigned char)((newcount >> 8) & 0xff);
    uint32_t heap = (uint32_t)(len + add);
    memcpy(buf + 4, &heap, 4);
    XChangeProperty(d, mw, a_motif_targets(d), a_motif_targets(d), 8,
                    PropModeReplace, buf, (int)(len + add));
    TR("targets append index %u (width %u, %d targets)\n", count, newcount, n);
    free(buf);
    XFree(data);
    return (int)count;
}

/* The atoms of the target list at `idx`, or 0. */
static int targets_at(Display *d, Window mw, unsigned idx, Atom **out)
{
    unsigned long len = 0;
    unsigned count = 0;
    unsigned char *data = targets_read(d, mw, &len, &count);
    if (!data) return 0;
    int rc = 0;
    size_t off = 8;
    for (unsigned i = 0; i < count && off + 2 <= len; i++) {
        int num = data[off] | (data[off + 1] << 8);
        size_t lob = (size_t)num * 4;
        if (off + 2 + lob > len) break;
        if (i == idx) {
            Atom *v = calloc((size_t)num + 1, sizeof(Atom));
            if (v) {
                for (int j = 0; j < num; j++) {
                    uint32_t a;
                    memcpy(&a, data + off + 2 + (size_t)j * 4, 4);
                    v[j] = (Atom)a;
                }
                *out = v;
                rc = num;
            }
            break;
        }
        off += 2 + lob;
    }
    XFree(data);
    return rc;
}

/* ------------------------------------------------ message codec */

static void put16(unsigned char *p, unsigned v)
{ p[0] = (unsigned char)(v & 0xff); p[1] = (unsigned char)((v >> 8) & 0xff); }

static void put32(unsigned char *p, unsigned long v)
{
    p[0] = (unsigned char)(v & 0xff);
    p[1] = (unsigned char)((v >> 8) & 0xff);
    p[2] = (unsigned char)((v >> 16) & 0xff);
    p[3] = (unsigned char)((v >> 24) & 0xff);
}

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

/* --------------------------------------- synthetic initiator (destination) */

/* Write `_MOTIF_DRAG_INITIATOR_INFO` (property named after the icc handle) so
 * the drop site can learn the initiator's targets. */
static void write_initiator_info(Display *d, MwDnd *x, unsigned targets_index)
{
    unsigned char info[8];
    info[0] = ICC_BYTE_ORDER;
    info[1] = ICC_PROTOCOL_VERSION;
    put16(info + 2, targets_index);
    put32(info + 4, (unsigned long)x->icc);
    XChangeProperty(d, x->src_win, x->icc, a_motif_init_info(d), 8,
                    PropModeReplace, info, (int)sizeof info);
}

bool mw_dnd_owns_window(Display *d, Window w)
{
    MwDnd *x = MWD(d)->dnd;
    return x && w != None && w == x->src_win;
}

static void ensure_src_window(Display *d)
{
    MwDnd *x = MWD(d)->dnd;
    if (x->src_win == None)
        x->src_win = XCreateSimpleWindow(d, DefaultRootWindow(d), -1, -1, 1, 1,
                                         0, 0, 0);
}

/* -------------------------------------------------------- receiving side */

/* Interpret the bridge payload another shim advertised: "icc\0target\0...". */
static void dnd_apply_payload(Display *d, MwDnd *x, const char *hexpayload)
{
    unsigned char raw[4096];
    size_t n = hex_decode(hexpayload, raw, sizeof raw);
    if (n == 0) return;
    raw[sizeof raw - 1] = 0;

    /* first NUL-terminated string is the icc handle name */
    const char *icc = (const char *)raw;
    size_t icclen = strnlen(icc, n);
    if (icclen >= n) return;

    /* the rest are target names */
    Atom list[64];
    int nl = 0;
    const char *p = (const char *)raw + icclen + 1;
    const char *end = (const char *)raw + n;
    while (p < end && *p && nl < 64) {
        size_t l = strnlen(p, (size_t)(end - p));
        list[nl++] = mw_intern_atom(d, p, False);
        TR("  want target %s\n", p);
        p += l + 1;
    }

    ensure_src_window(d);
    x->icc = mw_intern_atom(d, icc, False);
    x->remote = true;

    Window mw = motif_window(d);
    int idx = (mw != None && nl > 0) ? targets_index(d, mw, list, nl) : -1;
    if (idx < 0) idx = ICC_TARGETS_INDEX_STRING;
    x->targets_index = (unsigned)idx;
    /* Own the icc handle locally so the drop site's convert requests reach us:
     * we answer the protocol-level targets (HOST_NAME, TARGETS, the outcome
     * handshake) ourselves and relay the real payload to the initiator. */
    XSetSelectionOwner(d, x->icc, x->src_win, CurrentTime);
    write_initiator_info(d, x, (unsigned)idx);
    TR("payload: icc=%s targets=%d index=%d\n", icc, nl, idx);
}

void mw_dnd_wl_enter(Display *d, struct wl_surface *s, double sx, double sy,
                     const char *mime, const char *motif_drag)
{
    (void)s; (void)mime;
    MwDnd *x = MWD(d)->dnd;
    if (!x) return;
    /* Our own drag coming back: the compositor also delivers enter to the
     * source surface (the pointer starts on it).  Do not stand in as the
     * initiator here -- our Motif still owns the icc handle and its own drag
     * handles the transfer; taking the selection over broke it. */
    if (MWD(d)->dnd_src) {
        TR("enter: our own drag, ignoring\n");
        return;
    }
    Window shell = MWD(d)->drag_window;
    if (shell == None) return;
    MwWindow *w = mw_window(d, shell);
    if (!w || !w->tl || w->tl->is_popup) return;

    if (motif_drag) {
        dnd_apply_payload(d, x, motif_drag);
    } else {
        /* A plain Wayland drag: we own the icc handle ourselves. */
        ensure_src_window(d);
        x->icc = mw_intern_atom(d, "_MOTIF_ATOM_DND", False);
        XSetSelectionOwner(d, x->icc, x->src_win, CurrentTime);
        x->remote = false;
        write_initiator_info(d, x, ICC_TARGETS_INDEX_STRING);
    }

    int ox = 0, oy = 0;
    mw_window_origin(w, &ox, &oy);
    int off = mw_toplevel_content_offset(w->tl);
    x->shell = shell;
    x->root_x = ox + (int)(sx + 0.5);
    x->root_y = oy + (int)(sy + 0.5) - off;
    x->active = true;
    x->entered = true;
    x->drop_ready = false;
    free(x->drop_data);
    x->drop_data = NULL;
    x->drop_len = 0;
    TR("enter shell=0x%lx root=%d,%d remote=%d\n", (unsigned long)shell,
       x->root_x, x->root_y, x->remote);

    unsigned char m[20];
    memset(m, 0, sizeof m);
    icc_header(m, ICC_TOP_LEVEL_ENTER, 0, 0, CurrentTime);
    put32(m + 8, (unsigned long)x->src_win);
    put32(m + 12, (unsigned long)x->icc);
    send_icc(d, shell, m, 16);

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
    if (!x || !x->shell || x->shell == None) return;
    free(x->drop_data);
    x->drop_data = NULL;
    x->drop_len = 0;
    if (data && len) {
        x->drop_data = malloc(len);
        if (x->drop_data) { memcpy(x->drop_data, data, len); x->drop_len = len; }
    }
    x->drop_ready = true;
    TR("drop %zu bytes at %d,%d -> shell=0x%lx remote=%d\n", len, x->root_x,
       x->root_y, (unsigned long)x->shell, x->remote);

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

    /* The drop has been delivered.  Do NOT send TOP_LEVEL_LEAVE afterwards:
     * Motif's ReceiverShellExternalSourceHandler destroys the external
     * DragContext on a leave that is not accompanied by a DROP_START in the
     * same batch, and the transfer machinery still holds that context -- the
     * next XtGetValues on it is a use-after-free.  Mark the drag finished so
     * the compositor's leave (which comes right after the drop) is ignored. */
    x->active = false;
    x->entered = false;
    x->shell = None;
}

/* Serve a convert on a synthetic icc handle we own (a Wayland-origin drag). */
static void dnd_notify(Display *d, Atom selection, Atom target, Window requestor,
                       Time time, Atom prop, bool ok)
{
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
}

bool mw_dnd_xconvert(Display *d, Atom selection, Atom target, Atom property,
                     Window requestor, Time time)
{
    MwDnd *x = MWD(d)->dnd;
    if (!x || x->src_win == None || selection != x->icc)
        return false;

    if (dnd_trace()) {
        const char *tn = XGetAtomName(d, target);
        TR("xconvert target=%lu (%s) drop_ready=%d remote=%d\n",
           (unsigned long)target, tn ? tn : "?", x->drop_ready, x->remote);
        if (tn) XFree((char *)tn);
    }

    Atom prop = property == None ? target : property;
    bool ok = false;
    long served = -1;

    if (x->remote) {
        /* HOST_NAME is the Dt file protocol's preflight: the receiver asks the
         * initiator for its host to choose between FILE_NAME and _DT_NETFILE.
         * We are the initiator here, so answer it directly. */
        if (target == a_host_name(d) || target == a_sun_host(d)) {
            char host[256] = "";
            if (gethostname(host, sizeof host - 1) != 0) host[0] = 0;
            XChangeProperty(d, requestor, prop, XA_STRING, 8, PropModeReplace,
                            (const unsigned char *)host, (int)strlen(host) + 1);
            dnd_notify(d, selection, target, requestor, time, prop, true);
            TR("remote HOST_NAME -> '%s' (requestor=0x%lx prop=%lu)\n", host,
               (unsigned long)requestor, (unsigned long)prop);
            return true;
        }
        if (target == a_targets(d)) {
            Window mw = motif_window(d);
            Atom *list = NULL;
            int n = (mw != None) ? targets_at(d, mw, x->targets_index, &list) : 0;
            if (n > 0) {
                long vals[64];
                int cnt = n > 64 ? 64 : n;
                for (int i = 0; i < cnt; i++) vals[i] = (long)list[i];
                XChangeProperty(d, requestor, prop, XA_ATOM, 32, PropModeReplace,
                                (const unsigned char *)vals, cnt);
            }
            free(list);
            dnd_notify(d, selection, target, requestor, time, prop, n > 0);
            return true;
        }
        if (target == a_transfer_success(d) || target == a_transfer_failure(d)) {
            XChangeProperty(d, requestor, prop, target, 32, PropModeReplace, NULL, 0);
            dnd_notify(d, selection, target, requestor, time, prop, true);
            return true;
        }
        /* The real payload (FILE_NAME, TEXT, ...): relay to the initiator. */
        if (mw_broker_convert(d, selection, target, property, requestor, time))
            return true;
        TR("remote relay refused target=%lu\n", (unsigned long)target);
        dnd_notify(d, selection, target, requestor, time, prop, false);
        return true;
    }

    if (x->drop_ready && target == a_targets(d)) {
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
        XChangeProperty(d, requestor, prop, target, 32, PropModeReplace, NULL, 0);
        ok = true;
        served = 0;
    }

    dnd_notify(d, selection, target, requestor, time, prop, ok);
    TR("xconvert -> %s (served=%ld)\n", ok ? "ok" : "refused", served);
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
    free(x->payload);
    free(x);
    MWD(d)->dnd = NULL;
}

/* ------------------------------------------------------- sending side */

static void dnd_release_now(Display *d)
{
    MwDnd *x = MWD(d)->dnd;
    if (!x || !x->release_pending) return;
    x->release_pending = false;
    TR("releasing Motif drag\n");
    mw_pointer_synthetic_release(d, 1);
}

void mw_dnd_source_done(Display *d)
{
    XDisplayImpl *dp = MWD(d);
    if (dp->dnd_src) { wl_data_source_destroy(dp->dnd_src); dp->dnd_src = NULL; }
    MwDnd *x = dp->dnd;
    if (!x) return;
    /* Keep the initiator's selection owned a moment longer: the destination's
     * transfer is relayed by the broker and has to complete before Motif
     * disowns it. */
    x->release_pending = true;
    x->release_at = dnd_now_ms() + DND_RELEASE_GRACE_MS;
}

int mw_dnd_timeout(Display *d)
{
    MwDnd *x = MWD(d)->dnd;
    if (!x || !x->release_pending) return -1;
    uint64_t now = dnd_now_ms();
    if (now >= x->release_at) return 0;
    return (int)(x->release_at - now);
}

void mw_dnd_pump(Display *d)
{
    MwDnd *x = MWD(d)->dnd;
    if (x && x->release_pending && dnd_now_ms() >= x->release_at)
        dnd_release_now(d);
}

/* The broker has finished relaying a conversion for a Motif drag's icc handle:
 * the destination now has the data, so Motif's own drag can end. */
void mw_dnd_serve_done(Display *d)
{
    MwDnd *x = MWD(d)->dnd;
    if (x && x->release_pending) dnd_release_now(d);
}

static void dnd_src_target(void *data, struct wl_data_source *src, const char *mime)
{ (void)data; (void)src; TR("source target %s\n", mime); }

static void dnd_src_send(void *data, struct wl_data_source *src, const char *mime,
                         int32_t fd)
{
    (void)src;
    Display *d = data;
    if (!mw_dnd_source_send(d, mime, fd)) close(fd);
}

static void dnd_src_cancelled(void *data, struct wl_data_source *src)
{ (void)src; TR("source cancelled\n"); mw_dnd_source_done((Display *)data); }

static void dnd_src_drop(void *data, struct wl_data_source *src)
{ (void)data; (void)src; }

static void dnd_src_finished(void *data, struct wl_data_source *src)
{ (void)src; TR("source finished\n"); mw_dnd_source_done((Display *)data); }

static void dnd_src_action(void *data, struct wl_data_source *src, uint32_t a)
{ (void)data; (void)src; (void)a; }

static const struct wl_data_source_listener dnd_source_listener = {
    .target = dnd_src_target,
    .send = dnd_src_send,
    .cancelled = dnd_src_cancelled,
    .dnd_drop_performed = dnd_src_drop,
    .dnd_finished = dnd_src_finished,
    .action = dnd_src_action,
};

/* Build the "<<hex>>" payload naming the icc handle and the initiator's
 * targets, so the destination shim can present the same offer to its drop
 * site. */
static void build_payload(Display *d, MwDnd *x)
{
    free(x->payload);
    x->payload = NULL;

    char icc[256] = "";
    char *nm = XGetAtomName(d, x->x_icc);
    if (nm) { snprintf(icc, sizeof icc, "%s", nm); XFree(nm); }
    if (!icc[0]) return;

    unsigned char raw[2048];
    size_t rl = 0;
    size_t l = strlen(icc) + 1;
    if (l > sizeof raw) return;
    memcpy(raw, icc, l);
    rl = l;

    Atom *list = NULL;
    Window mw = motif_window(d);
    int n = (mw != None) ? targets_at(d, mw, x->x_targets_index, &list) : 0;
    for (int i = 0; i < n && rl + 256 < sizeof raw; i++) {
        char *tn = XGetAtomName(d, list[i]);
        if (!tn) continue;
        TR("  target %s\n", tn);
        size_t tl = strlen(tn) + 1;
        if (rl + tl > sizeof raw) { XFree(tn); continue; }
        memcpy(raw + rl, tn, tl);
        rl += tl;
        XFree(tn);
    }
    free(list);

    char *hex = malloc(rl * 2 + 1);
    if (!hex) return;
    hex_encode(raw, rl, hex);
    x->payload = hex;
    TR("built payload %zu targets\n", (size_t)n);
}

/* A Motif drag has started in this process.  Record the initiator's selection
 * and targets; the Wayland drag itself starts when the pointer leaves our
 * windows, so a drag that stays inside this application keeps its native
 * behaviour. */
void mw_dnd_initiator_info(Display *d, Window src, Atom icc, int format,
                           const unsigned char *data, unsigned long nitems)
{
    MwDnd *x = MWD(d)->dnd;
    if (!x) return;
    if (x->src_win != None && src == x->src_win) return;  /* our own write */
    (void)format;
    x->x_src = src;
    x->x_icc = icc;
    x->x_active = true;
    if (data && nitems >= 8)
        x->x_targets_index = (unsigned)data[2] | ((unsigned)data[3] << 8);
    build_payload(d, x);
    TR("Motif drag started src=0x%lx icc=%lu\n", (unsigned long)src,
       (unsigned long)icc);

    /* Start the Wayland drag now, while the pointer is still on the source
     * surface and the button press serial is fresh: the compositor rejects a
     * start_drag issued after the pointer has already left.  The drag icon no
     * longer follows the pointer (the compositor shows its own), and an
     * in-process drag is handled by the same bridge as a cross-process one. */
    mw_dnd_maybe_start(d);
}

void mw_dnd_selection_changed(Display *d, Atom selection, Window owner)
{
    MwDnd *x = MWD(d)->dnd;
    if (!x || x->x_icc == None || selection != x->x_icc) return;
    if (owner == None) { x->x_active = false; x->release_pending = false; }
}

void mw_dnd_maybe_start(Display *d)
{
    XDisplayImpl *dp = MWD(d);
    MwDnd *x = dp->dnd;
    if (!x || !x->x_active || x->x_src == None) return;
    if (dp->dnd_src) return;                         /* already dragging */
    if (!dp->data_device || !dp->dnd_mgr) return;

    MwWindow *sw = mw_window(d, x->x_src);
    while (sw && !sw->tl) sw = sw->parent;
    if (!sw || !sw->tl || !sw->tl->surface) return;

    struct wl_data_source *src =
        wl_data_device_manager_create_data_source(dp->dnd_mgr);
    if (!src) return;
    wl_data_source_add_listener(src, &dnd_source_listener, d);

    /* The private MIME names the initiator's selection and targets; the text
     * MIMEs let a plain Wayland application accept a text drag too. */
    char mime[4096];
    if (x->payload) {
        snprintf(mime, sizeof mime, "%s%s", MOTIF_DRAG_MIME_PREFIX, x->payload);
        wl_data_source_offer(src, mime);
    }
    wl_data_source_offer(src, "text/plain;charset=utf-8");
    wl_data_source_offer(src, "UTF8_STRING");
    wl_data_source_offer(src, "text/plain");
    wl_data_source_offer(src, "STRING");
    wl_data_source_set_actions(src,
        WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY |
        WL_DATA_DEVICE_MANAGER_DND_ACTION_MOVE);
    dp->dnd_src = src;
    wl_data_device_start_drag(dp->data_device, src, sw->tl->surface, NULL,
                              dp->last_press_serial);
    wl_display_flush(dp->wl_display);
    TR("started Wayland drag from 0x%lx serial=%u\n", (unsigned long)x->x_src,
       dp->last_press_serial);
}

bool mw_dnd_source_send(Display *d, const char *mime, int fd)
{
    XDisplayImpl *dp = MWD(d);
    MwDnd *x = dp->dnd;
    if (!x || x->x_icc == None) return false;
    MwClipServe *sv = &dp->clip_serve;
    if (sv->active) return false;
    (void)mime;   /* everything we offer is served as UTF-8 text */

    sv->active = true;
    sv->fd = fd;
    sv->to_utf8 = false;     /* Motif returns UTF-8 for UTF8_STRING */
    sv->selection = x->x_icc;
    sv->property = mw_intern_atom(d, "MW_DND_DATA", False);
    sv->deadline_ms = dnd_now_ms() + 5000;
    XConvertSelection(d, x->x_icc, a_utf8(d), sv->property, mw_clip_window(d),
                      CurrentTime);
    wl_display_flush(dp->wl_display);
    return true;
}
