/* xi2.c — a minimal XInput2 surface so libXi clients can run.
 *
 * The shim speaks no wire protocol, so an extension library cannot be answered
 * by the server.  libXi builds its requests through _XGetRequest and then waits
 * in _XReply, so we recognise its major opcode there and synthesise the reply
 * the library expects.  The query path is what a client such as xinput needs:
 * the extension version, XIQueryVersion, XIQueryDevice and the property
 * listings.  It reports one master pointer/keyboard pair plus an XTEST slave
 * pair -- the shape a client expects from a real server -- with no device
 * classes, which is all the query code reads.
 */
#include "internal.h"

#include <X11/extensions/XInput2.h>
#include <stdlib.h>
#include <string.h>

#define MW_XI_NAME        "XInputExtension"
#define MW_XI_OPCODE      131
#define MW_XI_FIRST_EVENT 70
#define MW_XI_FIRST_ERROR 137

/* Minor request codes (XInput2). */
#define MW_XI_GET_EXT_VERSION 1
#define MW_XI_QUERY_VERSION   47
#define MW_XI_QUERY_DEVICE    48
#define MW_XI_LIST_PROPERTIES 56
#define MW_XI_GET_PROPERTY    59

/* XInput2 device uses.  These are the values the protocol actually carries
 * (checked against a real server): XIMasterPointer is 1, not 0. */
#define MW_XI_MASTER_POINTER  1
#define MW_XI_MASTER_KEYBOARD 2
#define MW_XI_SLAVE_POINTER   3
#define MW_XI_SLAVE_KEYBOARD  4

/* ------------------------------------------------------------ device info */

/* XI2 class types and valuator modes (from XI2.h). */
#define MW_XI_KEY_CLASS      0
#define MW_XI_BUTTON_CLASS   1
#define MW_XI_VALUATOR_CLASS 2
#define MW_XI_TOUCH_CLASS    8
#define MW_XI_MODE_ABSOLUTE  1
#define MW_XI_DIRECT_TOUCH   1
#define MW_XI_TOUCH_ID       6        /* the touch device, when present */

/* A reply is 32 bytes: type, subtype, sequence, length, then 24 bytes of
 * payload.  Writing at explicit offsets keeps the layout obvious. */
static void put16(unsigned char *p, int off, unsigned v)
{ unsigned short x = (unsigned short)v; memcpy(p + off, &x, 2); }
static void put32(unsigned char *p, int off, unsigned v)
{ unsigned int x = v; memcpy(p + off, &x, 4); }

Bool mw_xi2_query_extension(_Xconst char *name, int *major,
                            int *first_event, int *first_error)
{
    if (!name || strcmp(name, MW_XI_NAME) != 0) return False;
    if (major)       *major = MW_XI_OPCODE;
    if (first_event) *first_event = MW_XI_FIRST_EVENT;
    if (first_error) *first_error = MW_XI_FIRST_ERROR;
    return True;
}

const char *mw_xi2_extension_name(void) { return MW_XI_NAME; }

/* A device in the synthetic set.  kind: 1 pointer, 2 keyboard, 3 touch. */
struct xi_dev {
    int id, use, attach, kind;
    const char *name;
};

/* A growable wire buffer for one XIQueryDevice reply. */
struct xibuf { unsigned char *p; size_t len, cap; };

static void xb_reserve(struct xibuf *b, size_t n)
{
    if (b->len + n <= b->cap) return;
    size_t nc = b->cap ? b->cap : 256;
    while (nc < b->len + n) nc *= 2;
    unsigned char *np = realloc(b->p, nc);
    if (!np) return;
    b->p = np;
    b->cap = nc;
}
static void xb_u8(struct xibuf *b, unsigned v)
{ xb_reserve(b, 1); if (b->p) b->p[b->len++] = (unsigned char)v; }
static void xb_u16(struct xibuf *b, unsigned v)
{
    xb_reserve(b, 2);
    if (!b->p) return;
    b->p[b->len++] = (unsigned char)v;
    b->p[b->len++] = (unsigned char)(v >> 8);
}
static void xb_u32(struct xibuf *b, unsigned v)
{
    xb_reserve(b, 4);
    if (!b->p) return;
    for (int i = 0; i < 4; i++) b->p[b->len++] = (unsigned char)(v >> (8 * i));
}
static void xb_bytes(struct xibuf *b, const void *d, size_t n)
{ xb_reserve(b, n); if (!b->p) return; memcpy(b->p + b->len, d, n); b->len += n; }
/* FP3232: a 32.32 fixed-point value (integral int32, fractional uint32). */
static void xb_fp3232(struct xibuf *b, double v)
{
    int32_t in = (int32_t)v;
    uint32_t fr = (uint32_t)((v - (double)in) * 4294967296.0);
    xb_u32(b, (unsigned)in);
    xb_u32(b, fr);
}

/* One class struct.  `length` fields are in 4-byte units. */
static void add_valuator(struct xibuf *b, int devid, int number,
                         double min, double max, int mode)
{
    xb_u16(b, MW_XI_VALUATOR_CLASS); xb_u16(b, 11);
    xb_u16(b, devid); xb_u16(b, number);
    xb_u32(b, 0);                       /* label atom */
    xb_fp3232(b, min); xb_fp3232(b, max); xb_fp3232(b, 0.0);
    xb_u32(b, 0);                       /* resolution */
    xb_u16(b, mode); xb_u16(b, 0);      /* mode + pad */
}

static void add_buttons(struct xibuf *b, int devid, int nbuttons)
{
    int maskbytes = ((nbuttons + 7) / 8 + 3) & ~3;
    xb_u16(b, MW_XI_BUTTON_CLASS); xb_u16(b, (unsigned)(8 + maskbytes + 4 * nbuttons) / 4);
    xb_u16(b, devid); xb_u16(b, nbuttons);
    for (int i = 0; i < maskbytes; i++) xb_u8(b, 0);   /* current button state */
    for (int i = 0; i < nbuttons; i++) xb_u32(b, 0);   /* labels */
}

static void add_keys(struct xibuf *b, int devid, int first, int last)
{
    int n = last - first + 1;
    xb_u16(b, MW_XI_KEY_CLASS); xb_u16(b, (unsigned)(8 + 4 * n) / 4);
    xb_u16(b, devid); xb_u16(b, n);
    for (int k = first; k <= last; k++) xb_u32(b, (unsigned)k);
}

static void add_touch(struct xibuf *b, int devid, int ntouch)
{
    xb_u16(b, MW_XI_TOUCH_CLASS); xb_u16(b, 2);
    xb_u16(b, devid); xb_u8(b, MW_XI_DIRECT_TOUCH); xb_u8(b, ntouch);
}

static void add_device(struct xibuf *b, int id, int use, int attach,
                       const char *name, int nclasses)
{
    int nl = (int)strlen(name);
    xb_u16(b, id); xb_u16(b, use); xb_u16(b, attach);
    xb_u16(b, nclasses); xb_u16(b, nl); xb_u8(b, 1); xb_u8(b, 0);
    xb_bytes(b, name, (size_t)nl);
    for (int i = nl; (i & 3); i++) xb_u8(b, 0);
}

/* The device set reflects the Wayland seat: the pointer/keyboard pair is
 * always there, and a touch device appears only when the seat advertises the
 * touch capability.  Classes give clients the axes/buttons/keys they expect. */
static void build_device_info(Display *d, int filter, int *count, size_t *len)
{
    XDisplayImpl *dp = MWD(d);
    int have_touch = (dp->seat_caps & WL_SEAT_CAPABILITY_TOUCH) != 0;
    int sw = MWSCR(d)->width > 0 ? MWSCR(d)->width : 1024;
    int sh = MWSCR(d)->height > 0 ? MWSCR(d)->height : 768;

    struct xi_dev devs[6];
    int nd = 0;
    devs[nd++] = (struct xi_dev){2, MW_XI_MASTER_POINTER, 3, 1, "Virtual core pointer"};
    devs[nd++] = (struct xi_dev){3, MW_XI_MASTER_KEYBOARD, 2, 2, "Virtual core keyboard"};
    devs[nd++] = (struct xi_dev){4, MW_XI_SLAVE_POINTER, 2, 1, "Virtual core XTEST pointer"};
    devs[nd++] = (struct xi_dev){5, MW_XI_SLAVE_KEYBOARD, 3, 2, "Virtual core XTEST keyboard"};
    if (have_touch)
        devs[nd++] = (struct xi_dev){MW_XI_TOUCH_ID, MW_XI_SLAVE_POINTER, 2, 3, "Wayland touch"};

    struct xibuf b = {0};
    int n = 0;
    for (int i = 0; i < nd; i++) {
        if (filter && devs[i].id != filter) continue;
        int nclasses = devs[i].kind == 1 ? 3 : devs[i].kind == 2 ? 1 : 2;
        add_device(&b, devs[i].id, devs[i].use, devs[i].attach, devs[i].name, nclasses);
        switch (devs[i].kind) {
        case 1:  /* pointer: two absolute axes + three buttons */
            add_valuator(&b, devs[i].id, 0, 0, sw, MW_XI_MODE_ABSOLUTE);
            add_valuator(&b, devs[i].id, 1, 0, sh, MW_XI_MODE_ABSOLUTE);
            add_buttons(&b, devs[i].id, 3);
            break;
        case 2:  /* keyboard */
            add_keys(&b, devs[i].id, 8, 255);
            break;
        default: /* touch: direct mode, normalised x/y */
            add_touch(&b, devs[i].id, 10);
            add_valuator(&b, devs[i].id, 0, 0, 1, MW_XI_MODE_ABSOLUTE);
            add_valuator(&b, devs[i].id, 1, 0, 1, MW_XI_MODE_ABSOLUTE);
            break;
        }
        n++;
    }
    *count = n;
    *len = b.len;
    dp->xi2_data = b.p;
    dp->xi2_data_len = b.len;
    dp->xi2_data_off = 0;
}

Bool mw_xi2_reply(Display *d, void *repbuf)
{
    XDisplayImpl *dp = MWD(d);
    unsigned char *req = dp->xi2_req;
    if (!req || req[0] != MW_XI_OPCODE) return False;
    unsigned char *rep = repbuf;
    memset(rep, 0, 32);
    unsigned minor = req[1];
    rep[0] = 1;              /* X_Reply */
    rep[1] = (unsigned char)minor;
    dp->xi2_req = NULL;      /* answered */
    if (getenv("MW_TRACE"))
        fprintf(stderr, "MW: XI2 request minor=%u -> reply\n", minor);

    switch (minor) {
    case MW_XI_GET_EXT_VERSION:      /* legacy XInput version query */
        put16(rep, 8, 2);            /* major_version */
        put16(rep, 10, 4);           /* minor_version */
        rep[12] = 1;                 /* present */
        return True;
    case MW_XI_QUERY_VERSION:
        put16(rep, 8, 2);
        put16(rep, 10, 4);
        return True;
    case MW_XI_QUERY_DEVICE: {
        /* xXIQueryDeviceReq: reqType, ReqType, length, then deviceid. */
        int filter = (int)req[4] | ((int)req[5] << 8);
        int count = 0; size_t len = 0;
        build_device_info(d, filter, &count, &len);
        if (getenv("MW_TRACE"))
            fprintf(stderr, "MW: XIQueryDevice filter=%d -> %d devices, %zu bytes\n",
                    filter, count, len);
        put16(rep, 8, count);                 /* num_devices */
        put32(rep, 4, (unsigned)(len / 4));   /* reply length */
        return True;
    }
    case MW_XI_LIST_PROPERTIES:
        put16(rep, 8, 0);                     /* num_properties */
        put32(rep, 4, 0);
        return True;
    case MW_XI_GET_PROPERTY:
        put32(rep, 4, 0);                     /* no such property */
        return True;
    default:
        return True;                          /* empty reply, no data */
    }
}

/* Serve the variable-length payload of a synthesised reply.  Returns -1 when
 * the pending data is not ours, so the caller can fall back. */
int mw_xi2_read(Display *d, char *data, size_t size)
{
    XDisplayImpl *dp = MWD(d);
    if (!dp->xi2_data) return -1;
    size_t avail = dp->xi2_data_len - dp->xi2_data_off;
    size_t n = size < avail ? size : avail;
    if (n) memcpy(data, dp->xi2_data + dp->xi2_data_off, n);
    if (n < size) memset(data + n, 0, size - n);
    dp->xi2_data_off += n;
    if (dp->xi2_data_off >= dp->xi2_data_len) {
        free(dp->xi2_data);
        dp->xi2_data = NULL;
        dp->xi2_data_len = dp->xi2_data_off = 0;
    }
    return (int)size;
}

void mw_xi2_forget_request(Display *d)
{
    XDisplayImpl *dp = MWD(d);
    dp->xi2_req = NULL;
    if (dp->xi2_data) {
        free(dp->xi2_data);
        dp->xi2_data = NULL;
        dp->xi2_data_len = dp->xi2_data_off = 0;
    }
}

/* --------------------------------------------------- events / selection */

int mw_xi2_touch_device(Display *d)
{
    return (MWD(d)->seat_caps & WL_SEAT_CAPABILITY_TOUCH) ? MW_XI_TOUCH_ID : 0;
}

void mw_xi2_fini(Display *d)
{
    XDisplayImpl *dp = MWD(d);
    MwXiSelect *s = dp->xi_selects;
    while (s) { MwXiSelect *n = s->next; free(s); s = n; }
    dp->xi_selects = NULL;
}

static MwXiSelect *xi_select_find(Display *d, Window win, bool create)
{
    XDisplayImpl *dp = MWD(d);
    for (MwXiSelect *s = dp->xi_selects; s; s = s->next)
        if (s->win == win) return s;
    if (!create) return NULL;
    MwXiSelect *s = calloc(1, sizeof *s);
    if (!s) return NULL;
    s->win = win;
    s->next = dp->xi_selects;
    dp->xi_selects = s;
    return s;
}

/* XISelectEvents (minor 46): store the union of the requested event masks.  We
 * do not distinguish devices; touch is what matters here. */
void mw_xi2_request(Display *d, const unsigned char *req, size_t len)
{
    if (len < 4 || req[0] != MW_XI_OPCODE) return;
    if (req[1] != 46 /* X_XISelectEvents */) return;
    if (len < 12) return;

    Window win;
    unsigned short num;
    memcpy(&win, req + 4, 4);
    memcpy(&num, req + 8, 2);

    MwXiSelect *s = xi_select_find(d, win, true);
    if (!s) return;
    memset(s->mask, 0, sizeof s->mask);

    size_t off = 12;
    for (unsigned i = 0; i < num && off + 4 <= len; i++) {
        unsigned short mwords;
        memcpy(&mwords, req + off + 2, 2);
        off += 4;
        size_t bytes = (size_t)mwords * 4;
        if (off + bytes > len) break;
        for (size_t b = 0; b < bytes; b++) {
            for (int k = 0; k < 8; k++) {
                if (!(req[off + b] & (1 << k))) continue;
                unsigned ev = (unsigned)b * 8 + (unsigned)k;
                if (ev < 128) s->mask[ev / 32] |= 1u << (ev % 32);
            }
        }
        off += bytes;
    }
    if (getenv("MW_TRACE"))
        fprintf(stderr, "MW: XISelectEvents win=0x%lx masks=%u now=%08x\n",
                (unsigned long)win, num, s->mask[0]);
}

static bool xi_mask_has(const MwXiSelect *s, int evtype)
{
    if (!s || evtype < 0 || evtype >= 128) return false;
    return (s->mask[evtype / 32] >> (evtype % 32)) & 1u;
}

bool mw_xi2_selected(Display *d, Window win, int evtype)
{
    return xi_mask_has(xi_select_find(d, win, false), evtype);
}

/* The window that selected `evtype`, searching from `win` up through its
 * ancestors (XI2 events propagate up the hierarchy). */
static Window xi_event_target(Display *d, Window win, int evtype)
{
    for (MwWindow *w = mw_window(d, win); w; w = w->parent)
        if (xi_mask_has(xi_select_find(d, w->id, false), evtype))
            return w->id;
    return None;
}

/* A touch event is an XIDeviceEvent delivered as a GenericEvent cookie.  The
 * masks/values are packed with the struct so XFreeEventData's single free
 * releases everything. */
struct mw_xi_dev_event {
    XIDeviceEvent  ev;
    unsigned char  button_mask[4];
    unsigned char  valuator_mask[4];
    double         valuator_values[2];
};

/* A generic XI2 device event (motion, button, key, touch).  Coordinates are
 * window-relative. */
void mw_xi2_event(Display *d, Window win, int evtype, int detail,
                  int deviceid, int sourceid, double x, double y, Time time)
{
    Window target = xi_event_target(d, win, evtype);
    if (target == None) return;

    struct mw_xi_dev_event *e = calloc(1, sizeof *e);
    if (!e) return;
    XIDeviceEvent *ev = &e->ev;
    ev->type = GenericEvent;
    ev->display = d;
    ev->extension = MW_XI_OPCODE;
    ev->evtype = evtype;
    ev->time = time ? time : mw_now();
    ev->deviceid = deviceid;
    ev->sourceid = sourceid;
    ev->detail = detail;
    ev->root = MWSCR(d)->root;
    ev->event = target;
    ev->child = None;
    ev->root_x = x;
    ev->root_y = y;
    ev->event_x = x;
    ev->event_y = y;
    ev->flags = 0;
    ev->buttons.mask_len = 0;
    ev->buttons.mask = NULL;
    ev->valuators.mask_len = 1;
    ev->valuators.mask = e->valuator_mask;
    e->valuator_mask[0] = 0x3;            /* valuators 0 (x) and 1 (y) */
    ev->valuators.values = e->valuator_values;
    e->valuator_values[0] = x;
    e->valuator_values[1] = y;
    memset(&ev->mods, 0, sizeof ev->mods);
    memset(&ev->group, 0, sizeof ev->group);

    XEvent xev;
    memset(&xev, 0, sizeof xev);
    XGenericEventCookie *c = &xev.xcookie;
    c->type = GenericEvent;
    c->extension = MW_XI_OPCODE;
    c->evtype = evtype;
    c->data = e;
    mw_put_event(d, &xev);

    if (getenv("MW_TRACE"))
        fprintf(stderr, "MW: XI event evtype=%d detail=%d win=0x%lx (%.0f,%.0f)\n",
                evtype, detail, (unsigned long)target, x, y);
}

void mw_xi2_touch(Display *d, Window win, int evtype, int touchid, int deviceid,
                  int sourceid, double x, double y, Time time)
{
    mw_xi2_event(d, win, evtype, touchid, deviceid, sourceid, x, y, time);
}

/* XI_Enter/Leave/FocusIn/FocusOut use xXIEnterEvent (an XIEnterEvent cookie),
 * not the device-event layout. */
void mw_xi2_crossing(Display *d, Window win, int evtype, int mode, int detail,
                     int deviceid, int sourceid, int focus, double x, double y,
                     Time time)
{
    Window target = xi_event_target(d, win, evtype);
    if (target == None) return;

    struct { XIEnterEvent ev; unsigned char bm[4]; } *e = calloc(1, sizeof *e);
    if (!e) return;
    XIEnterEvent *ev = &e->ev;
    ev->type = GenericEvent;
    ev->display = d;
    ev->extension = MW_XI_OPCODE;
    ev->evtype = evtype;
    ev->time = time ? time : mw_now();
    ev->deviceid = deviceid;
    ev->sourceid = sourceid;
    ev->mode = mode;
    ev->detail = detail;
    ev->root = MWSCR(d)->root;
    ev->event = target;
    ev->child = None;
    ev->root_x = x;
    ev->root_y = y;
    ev->event_x = x;
    ev->event_y = y;
    ev->focus = focus ? True : False;
    ev->same_screen = True;
    ev->buttons.mask_len = 0;
    ev->buttons.mask = NULL;
    memset(&ev->mods, 0, sizeof ev->mods);
    memset(&ev->group, 0, sizeof ev->group);

    XEvent xev;
    memset(&xev, 0, sizeof xev);
    XGenericEventCookie *c = &xev.xcookie;
    c->type = GenericEvent;
    c->extension = MW_XI_OPCODE;
    c->evtype = evtype;
    c->data = e;
    mw_put_event(d, &xev);

    if (getenv("MW_TRACE"))
        fprintf(stderr, "MW: XI crossing evtype=%d mode=%d detail=%d win=0x%lx\n",
                evtype, mode, detail, (unsigned long)target);
}
