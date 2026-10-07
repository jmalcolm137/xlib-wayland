/* clipboard.c — bridge between X selections and the Wayland data device.
 *
 * X clients (Motif, NEdit) use the CLIPBOARD selection.  Wayland clients
 * (mousepad, Konsole, LibreOffice) use wl_data_device.  This file connects the
 * two so text can be cut in one and pasted in the other:
 *
 *   X -> Wayland:  when an X client becomes the CLIPBOARD owner we create a
 *                  wl_data_source and set it as the compositor selection;
 *                  wl_data_source.send() pulls the bytes back out of the X
 *                  owner with XConvertSelection() (which must be asynchronous:
 *                  send() runs inside the owner's own event dispatch).
 *
 *   Wayland -> X:  we take the X CLIPBOARD selection ourselves while a Wayland
 *                  client owns the compositor clipboard, so X clients see an
 *                  owner; their XConvertSelection() is answered by fetching the
 *                  offer's bytes over a pipe and posting a SelectionNotify once
 *                  the transfer finishes.
 */
#include "internal.h"

#include <wayland-client.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdio.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <time.h>

#define CLIP_FETCH_TIMEOUT_MS 3000
#define CLIP_SERVE_TIMEOUT_MS 5000

/* The X toplevel whose surface this is, or NULL.  (There is no reverse map on
 * the toplevel; the object table is small enough to scan.) */
static MwWindow *drag_window_for_surface(Display *d, struct wl_surface *s)
{
    XDisplayImpl *dp = MWD(d);
    if (!dp->table || !s) return NULL;
    for (size_t i = 0; i < dp->table_cap; i++) {
        if (dp->table[i].id && dp->table[i].kind == MW_OBJ_WINDOW) {
            MwWindow *w = dp->table[i].obj;
            if (w && w->tl && w->tl->surface == s) return w;
        }
    }
    return NULL;
}

/* CLIPBOARD is not one of the predefined atoms (those stop at WM_TRANSIENT_FOR),
 * so it has to be interned like any other name. */
static Atom clipboard_atom(Display *d)
{
    return mw_intern_atom(d, "CLIPBOARD", False);
}

/* The hidden window used to own selections on behalf of Wayland and to receive
 * the property during X->Wayland transfers.  It is created on first use so that
 * clipboard support does not perturb the window ids an application sees. */
Window mw_clip_window(Display *d)
{
    XDisplayImpl *dp = MWD(d);
    if (dp->clip_window == None) {
        Window root = XDefaultRootWindow(d);
        dp->clip_window = XCreateSimpleWindow(d, root, 0, 0, 1, 1, 0, 0, 0);
    }
    return dp->clip_window;
}

static uint64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* ------------------------------------------------------------- encodings */

/* MIME types we are willing to take clipboard text from, best first. */
static const char *mime_priority[] = {
    "text/plain;charset=utf-8",
    "UTF8_STRING",
    "text/plain;charset=UTF-8",
    "text/plain",
    "STRING",
    "TEXT",
    NULL
};

static int mime_rank(const char *m)
{
    for (int i = 0; mime_priority[i]; i++)
        if (strcasecmp(m, mime_priority[i]) == 0) return i;
    return -1;
}

static bool mime_wants_utf8(const char *m)
{
    if (!m) return true;
    if (strcasecmp(m, "UTF8_STRING") == 0) return true;
    if (strcasestr(m, "utf-8") || strcasestr(m, "utf8")) return true;
    return false;
}

char *mw_clipboard_to_utf8(const unsigned char *in, size_t inlen, size_t *outlen)
{
    /* Each Latin-1 byte outside ASCII becomes a two-byte UTF-8 sequence. */
    char *out = malloc(inlen * 2 + 1);
    size_t o = 0;
    for (size_t i = 0; i < inlen; i++) {
        unsigned char c = in[i];
        if (c < 0x80) {
            out[o++] = (char)c;
        } else {
            out[o++] = (char)(0xc0 | (c >> 6));
            out[o++] = (char)(0x80 | (c & 0x3f));
        }
    }
    out[o] = 0;
    if (outlen) *outlen = o;
    return out;
}

char *mw_clipboard_from_utf8(const unsigned char *in, size_t inlen, size_t *outlen)
{
    /* Decode UTF-8, substituting '?' for anything that will not fit Latin-1.
     * Versions of this text already in Latin-1 pass through unchanged. */
    char *out = malloc(inlen + 1);
    size_t i = 0, o = 0;
    while (i < inlen) {
        unsigned char c = in[i];
        unsigned long cp = c;
        int extra = 0;
        if (c >= 0xf0) { cp = c & 0x07; extra = 3; }
        else if (c >= 0xe0) { cp = c & 0x0f; extra = 2; }
        else if (c >= 0xc0) { cp = c & 0x1f; extra = 1; }
        bool valid = true;
        for (int k = 0; k < extra; k++) {
            if (i + 1 + k >= inlen || (in[i + 1 + k] & 0xc0) != 0x80) { valid = false; break; }
            cp = (cp << 6) | (in[i + 1 + k] & 0x3f);
        }
        if (!valid) { out[o++] = (char)c; i++; continue; }
        out[o++] = cp < 0x100 ? (char)cp : '?';
        i += 1 + extra;
    }
    out[o] = 0;
    if (outlen) *outlen = o;
    return out;
}

static void sync_x_owner(Display *d);
static void sync_x_owner_primary(Display *d);

/* Non-text MIME types we advertise on behalf of an X selection owner, so a
 * Wayland peer can paste an image, a file list or HTML the owner can serve.
 * source_send() converts the matching X target; if the owner cannot serve it,
 * the peer simply receives nothing. */
static const char *extra_mime[] = {
    "image/png",
    "image/jpeg",
    "image/bmp",
    "image/tiff",
    "image/x-xpixmap",
    "text/html",
    "text/uri-list",
    "application/x-color",
    NULL
};

static Atom a_targets(Display *d) { return mw_intern_atom(d, "TARGETS", False); }

static void post_selection_notify(Display *d, Window requestor, Atom selection,
                                  Atom target, Atom property, Time time)
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

static void targets_add(Display *d, Atom **list, int *n, int *cap, Atom a)
{
    (void)d;
    for (int i = 0; i < *n; i++) if ((*list)[i] == a) return;
    if (*n == *cap) {
        int c = *cap ? *cap * 2 : 8;
        Atom *nl = realloc(*list, (size_t)c * sizeof(Atom));
        if (!nl) return;
        *list = nl;
        *cap = c;
    }
    (*list)[(*n)++] = a;
}

/* The X targets a Wayland offer can serve: every advertised MIME interned by
 * name, plus the text aliases when a text MIME is present. */
static Atom *targets_from_mimes(Display *d, char **mimes, int nmimes,
                                const char *best, int *out_n)
{
    Atom *list = NULL;
    int n = 0, cap = 0;
    bool text = best != NULL;
    for (int i = 0; i < nmimes; i++) {
        targets_add(d, &list, &n, &cap, mw_intern_atom(d, mimes[i], False));
        if (mime_rank(mimes[i]) >= 0) text = true;
    }
    if (text) {
        targets_add(d, &list, &n, &cap, mw_intern_atom(d, "UTF8_STRING", False));
        targets_add(d, &list, &n, &cap, XA_STRING);
        targets_add(d, &list, &n, &cap, mw_intern_atom(d, "TEXT", False));
        targets_add(d, &list, &n, &cap, mw_intern_atom(d, "COMPOUND_TEXT", False));
    }
    *out_n = n;
    return list;
}

/* Map an X target to an offered MIME.  *latin1 is set when the requestor wants
 * Latin-1 (X STRING/TEXT/COMPOUND_TEXT) rather than the offer's UTF-8. */
static const char *mime_for_target(Display *d, char **mimes, int nmimes,
                                   const char *best, Atom target, bool *latin1)
{
    *latin1 = false;
    char *name = XGetAtomName(d, target);
    if (!name) return NULL;

    for (int i = 0; i < nmimes; i++)
        if (strcasecmp(mimes[i], name) == 0) {
            const char *m = mimes[i];
            XFree(name);
            return m;
        }

    bool is_utf8 = strcasecmp(name, "UTF8_STRING") == 0;
    bool is_latin1 = strcasecmp(name, "STRING") == 0 ||
                     strcasecmp(name, "TEXT") == 0 ||
                     strcasecmp(name, "COMPOUND_TEXT") == 0;
    XFree(name);
    if (!is_utf8 && !is_latin1) return NULL;

    if (is_utf8)
        for (int i = 0; i < nmimes; i++)
            if (mime_wants_utf8(mimes[i])) return mimes[i];
    if (best) { *latin1 = is_latin1; return best; }
    for (int i = 0; i < nmimes; i++)
        if (mime_rank(mimes[i]) >= 0) { *latin1 = is_latin1; return mimes[i]; }
    return NULL;
}

/* ------------------------------------------- PRIMARY selection transport */

/* PRIMARY uses its own protocol (zwp_primary_selection_v1): the same shape as
 * the clipboard offer/source, but a different interface without drag-and-drop. */
typedef struct MwPrimOffer MwPrimOffer;

struct MwPrimOffer {
    struct zwp_primary_selection_offer_v1 *offer;
    char  *mime;
    char **mimes;
    int    nmimes, mimecap;
    Display *d;
};

static void prim_add(MwPrimOffer *o, const char *mime)
{
    for (int i = 0; i < o->nmimes; i++)
        if (strcasecmp(o->mimes[i], mime) == 0) return;
    if (o->nmimes == o->mimecap) {
        int cap = o->mimecap ? o->mimecap * 2 : 8;
        char **n = realloc(o->mimes, (size_t)cap * sizeof *n);
        if (!n) return;
        o->mimes = n;
        o->mimecap = cap;
    }
    o->mimes[o->nmimes++] = strdup(mime);
}

static void prim_offer_free(MwPrimOffer *o)
{
    if (!o) return;
    if (o->offer) zwp_primary_selection_offer_v1_destroy(o->offer);
    free(o->mime);
    for (int i = 0; i < o->nmimes; i++) free(o->mimes[i]);
    free(o->mimes);
    free(o);
}

static void prim_offer_mime(void *data, struct zwp_primary_selection_offer_v1 *offer,
                            const char *mime)
{
    (void)offer;
    MwPrimOffer *o = data;
    prim_add(o, mime);
    int r = mime_rank(mime);
    if (r >= 0 && r < (o->mime ? mime_rank(o->mime) : 1000)) {
        free(o->mime);
        o->mime = strdup(mime);
    }
    if (o->d && o == MWD(o->d)->primary_offer)
        sync_x_owner_primary(o->d);
}

static const struct zwp_primary_selection_offer_v1_listener prim_offer_listener = {
    .offer = prim_offer_mime,
};

static void pd_data_offer(void *data, struct zwp_primary_selection_device_v1 *dd,
                          struct zwp_primary_selection_offer_v1 *id)
{
    (void)dd;
    Display *d = data;
    XDisplayImpl *dp = MWD(d);
    MwPrimOffer *o = calloc(1, sizeof *o);
    if (!o) return;
    o->offer = id;
    o->d = d;
    zwp_primary_selection_offer_v1_add_listener(id, &prim_offer_listener, o);
    prim_offer_free(dp->primary_pending);
    dp->primary_pending = o;
}

static void pd_selection(void *data, struct zwp_primary_selection_device_v1 *dd,
                         struct zwp_primary_selection_offer_v1 *id)
{
    (void)dd;
    Display *d = data;
    XDisplayImpl *dp = MWD(d);
    if (id && dp->primary_pending && dp->primary_pending->offer == id) {
        prim_offer_free(dp->primary_offer);
        dp->primary_offer = dp->primary_pending;
        dp->primary_pending = NULL;
    } else {
        prim_offer_free(dp->primary_pending);
        dp->primary_pending = NULL;
        prim_offer_free(dp->primary_offer);
        dp->primary_offer = NULL;
    }
    sync_x_owner_primary(d);
}

static const struct zwp_primary_selection_device_v1_listener primary_device_listener = {
    .data_offer = pd_data_offer,
    .selection = pd_selection,
};

/* ------------------------------------------------------------- wl offers */

static void offer_free(MwWlOffer *o)
{
    if (!o) return;
    if (o->offer) wl_data_offer_destroy(o->offer);
    free(o->mime);
    for (int i = 0; i < o->nmimes; i++) free(o->mimes[i]);
    free(o->mimes);
    free(o->motif_drag);
    free(o);
}

/* Remember every MIME an offer advertises, so non-text targets can be matched
 * later (the `mime` field keeps only the best text type for the text paths). */
static void offer_add_mime(MwWlOffer *o, const char *mime)
{
    for (int i = 0; i < o->nmimes; i++)
        if (strcasecmp(o->mimes[i], mime) == 0) return;
    if (o->nmimes == o->mimecap) {
        int cap = o->mimecap ? o->mimecap * 2 : 8;
        char **n = realloc(o->mimes, (size_t)cap * sizeof *n);
        if (!n) return;
        o->mimes = n;
        o->mimecap = cap;
    }
    o->mimes[o->nmimes++] = strdup(mime);
}

/* Bridge payload advertised by another shim on a Motif drag: the initiator's
 * icc handle selection name and the targets it can serve, hex-encoded (see
 * xlib/dnd.c). */
#define MOTIF_DRAG_MIME_PREFIX "application/x-motif-drag;"

static void offer_mime(void *data, struct wl_data_offer *offer, const char *mime)
{
    MwWlOffer *o = data;
    (void)offer;
    if (strncmp(mime, MOTIF_DRAG_MIME_PREFIX, strlen(MOTIF_DRAG_MIME_PREFIX)) == 0) {
        free(o->motif_drag);
        o->motif_drag = strdup(mime + strlen(MOTIF_DRAG_MIME_PREFIX));
        return;
    }
    offer_add_mime(o, mime);
    int r = mime_rank(mime);
    if (r >= 0 && r < (o->mime ? mime_rank(o->mime) : 1000)) {
        free(o->mime);
        o->mime = strdup(mime);
    }
    /* A compositor is permitted to send the selection before the MIME list, or
     * the selection may be non-text; re-evaluate ownership either way so X
     * clients see an owner they can paste from. */
    if (o->d && o == MWD(o->d)->wayland_offer)
        sync_x_owner(o->d);
}

static void dnd_offer_source_actions(void *data, struct wl_data_offer *offer,
                                     uint32_t actions)
{
    (void)data; (void)offer; (void)actions;
    /* The source's allowed actions (v3).  We accept whatever it offers. */
}

static void dnd_offer_action(void *data, struct wl_data_offer *offer,
                             uint32_t action)
{
    (void)data; (void)offer; (void)action;
    /* The action the compositor selected; only copy matters to us. */
}

static const struct wl_data_offer_listener offer_listener = {
    .offer = offer_mime,
    /* v3 handlers must exist or libwayland aborts the client when a drag
     * offer arrives (River sends source_actions). */
    .source_actions = dnd_offer_source_actions,
    .action = dnd_offer_action,
};

/* ------------------------------------------------------------- wl source */

static MwWlSource *source_find(Display *d, Atom selection)
{
    for (MwWlSource *s = MWD(d)->wl_sources; s; s = s->next)
        if (s->selection == selection) return s;
    return NULL;
}

static void source_free(Display *d, MwWlSource *s)
{
    MwWlSource **pp = &MWD(d)->wl_sources;
    while (*pp && *pp != s) pp = &(*pp)->next;
    if (*pp) *pp = s->next;
    if (s->source) {
        if (s->selection == XA_PRIMARY)
            zwp_primary_selection_source_v1_destroy(s->source);
        else
            wl_data_source_destroy(s->source);
    }
    free(s);
}

static MwWlSource *source_by_proxy(Display *d, void *proxy)
{
    for (MwWlSource *s = MWD(d)->wl_sources; s; s = s->next)
        if (s->source == proxy) return s;
    return NULL;
}

static void source_offer(MwWlSource *s, const char *mime)
{
    if (s->selection == XA_PRIMARY)
        zwp_primary_selection_source_v1_offer(s->source, mime);
    else
        wl_data_source_offer(s->source, mime);
}

/* A peer asked a source of ours for `mime`.  XConvertSelection() cannot be
 * answered synchronously here (the X owner only handles the SelectionRequest on
 * its next trip round the event loop), so queue the transfer and finish it when
 * the owner's SelectionNotify arrives. */
static void source_handle_send(Display *d, void *proxy, const char *mime, int32_t fd)
{
    MwWlSource *s = source_by_proxy(d, proxy);
    if (!s) { close(fd); return; }

    MwClipServe *sv = &MWD(d)->clip_serve;
    if (sv->active) { close(fd); return; }
    sv->active = true;
    sv->fd = fd;
    sv->selection = s->selection;
    sv->property = mw_intern_atom(d, "MW_CLIP_DATA", False);
    if (mime_rank(mime) >= 0 || mime_wants_utf8(mime)) {
        /* A text request: pull X STRING (Latin-1) and convert to UTF-8 when the
         * peer asked for UTF-8. */
        sv->target = XA_STRING;
        sv->to_utf8 = mime_wants_utf8(mime);
    } else {
        /* Non-text: convert the X target atom named by the MIME itself. */
        sv->target = mw_intern_atom(d, mime, False);
        sv->to_utf8 = false;
    }
    sv->deadline_ms = now_ms() + CLIP_SERVE_TIMEOUT_MS;

    XConvertSelection(d, s->selection, sv->target, sv->property,
                      mw_clip_window(d), CurrentTime);
    wl_display_flush(MWD(d)->wl_display);
}

static void source_handle_cancel(Display *d, void *proxy)
{
    MwWlSource *s = source_by_proxy(d, proxy);
    if (s) source_free(d, s);
    /* Losing our source means some other client took the selection;
     * re-evaluate who should own the X selection(s). */
    sync_x_owner(d);
    sync_x_owner_primary(d);
}

static void source_target(void *data, struct wl_data_source *src, const char *mime)
{ (void)data; (void)src; (void)mime; }

static void source_send(void *data, struct wl_data_source *src, const char *mime, int32_t fd)
{ source_handle_send((Display *)data, src, mime, fd); }

static void source_cancelled(void *data, struct wl_data_source *src)
{ source_handle_cancel((Display *)data, src); }

static void source_drop(void *data, struct wl_data_source *src)
{ (void)data; (void)src; }
static void source_finished(void *data, struct wl_data_source *src)
{ (void)data; (void)src; }
static void source_action(void *data, struct wl_data_source *src, uint32_t a)
{ (void)data; (void)src; (void)a; }

static const struct wl_data_source_listener source_listener = {
    .target = source_target,
    .send = source_send,
    .cancelled = source_cancelled,
    .dnd_drop_performed = source_drop,
    .dnd_finished = source_finished,
    .action = source_action,
};

static void prim_source_send(void *data, struct zwp_primary_selection_source_v1 *src,
                             const char *mime, int32_t fd)
{ source_handle_send((Display *)data, src, mime, fd); }

static void prim_source_cancelled(void *data, struct zwp_primary_selection_source_v1 *src)
{ source_handle_cancel((Display *)data, src); }

static const struct zwp_primary_selection_source_v1_listener prim_source_listener = {
    .send = prim_source_send,
    .cancelled = prim_source_cancelled,
};

/* ----------------------------------------------------------- data device */

/* Make the X CLIPBOARD owner reflect who really owns the compositor clipboard.
 *
 * Motif decides whether to serve a paste from its own stored data or to ask
 * the selection owner by comparing the owner it recorded when it took the
 * selection against the real one (WeOwnSelection()).  So if we leave a stale X
 * owner in place, NEdit keeps pasting its own old text and never asks us --
 * which is exactly what happened after cutting in NEdit and then copying in a
 * Wayland application. */
static void sync_x_owner(Display *d)
{
    XDisplayImpl *dp = MWD(d);
    Atom clip = clipboard_atom(d);
    bool has_data = dp->wayland_offer &&
                    (dp->wayland_offer->mime || dp->wayland_offer->nmimes > 0);
    bool we_published = source_find(d, clip) != NULL;

    if (has_data && !we_published) {
        /* A Wayland client owns the compositor clipboard: X clients must see
         * us as the owner so their paste is routed through the bridge. */
        Window want = mw_clip_window(d);
        if (XGetSelectionOwner(d, clip) != want) {
            XSetSelectionOwner(d, clip, want, CurrentTime);
        }
        return;
    }

    /* We published an X client's selection (leave it alone), or the Wayland
     * clipboard is empty: if we are only proxying, give the selection up. */
    if (!we_published && dp->clip_window != None &&
        XGetSelectionOwner(d, clip) == dp->clip_window)
        XSetSelectionOwner(d, clip, None, CurrentTime);
}

/* The same, for the X PRIMARY selection and the Wayland primary offer. */
static void sync_x_owner_primary(Display *d)
{
    XDisplayImpl *dp = MWD(d);
    bool has_data = dp->primary_offer &&
                    (dp->primary_offer->mime || dp->primary_offer->nmimes > 0);
    bool we_published = source_find(d, XA_PRIMARY) != NULL;

    if (has_data && !we_published) {
        Window want = mw_clip_window(d);
        if (XGetSelectionOwner(d, XA_PRIMARY) != want)
            XSetSelectionOwner(d, XA_PRIMARY, want, CurrentTime);
        return;
    }
    if (!we_published && dp->clip_window != None &&
        XGetSelectionOwner(d, XA_PRIMARY) == dp->clip_window)
        XSetSelectionOwner(d, XA_PRIMARY, None, CurrentTime);
}

static void dd_data_offer(void *data, struct wl_data_device *dd, struct wl_data_offer *id)
{
    Display *d = data;
    XDisplayImpl *dp = MWD(d);
    (void)dd;
    MwWlOffer *o = calloc(1, sizeof *o);
    o->offer = id;
    o->d = d;
    wl_data_offer_add_listener(id, &offer_listener, o);
    /* selected offers are promoted in dd_selection(); anything left here when
     * the next offer arrives was not selected. */
    offer_free(dp->pending_offer);
    dp->pending_offer = o;
}

static void dd_selection(void *data, struct wl_data_device *dd, struct wl_data_offer *id)
{
    Display *d = data;
    XDisplayImpl *dp = MWD(d);
    (void)dd;

    if (id && dp->pending_offer && dp->pending_offer->offer == id) {
        dp->wayland_offer = dp->pending_offer;
        dp->pending_offer = NULL;
    } else {
        offer_free(dp->pending_offer);
        dp->pending_offer = NULL;
        offer_free(dp->wayland_offer);
        dp->wayland_offer = NULL;
    }

    /* Make X clients see an owner for the compositor clipboard so that a paste
     * request reaches us instead of nobody. */
    sync_x_owner(d);
}

static void dd_enter(void *data, struct wl_data_device *dd, uint32_t serial,
                     struct wl_surface *s, wl_fixed_t x, wl_fixed_t y,
                     struct wl_data_offer *offer)
{
    Display *d = data;
    XDisplayImpl *dp = MWD(d);
    (void)dd;

    /* A drag (not a clipboard selection) entered one of our surfaces.  The
     * compositor sends data_offer first, so claim it from pending_offer. */
    MwWlOffer *o = NULL;
    if (dp->pending_offer && dp->pending_offer->offer == offer) {
        o = dp->pending_offer;
        dp->pending_offer = NULL;
    } else {
        o = calloc(1, sizeof *o);
        o->offer = offer;
        o->d = d;
        wl_data_offer_add_listener(offer, &offer_listener, o);
    }
    offer_free(dp->drag_offer);
    dp->drag_offer = o;
    dp->drag_serial = serial;
    dp->drag_surface = s;
    dp->drag_x = wl_fixed_to_double(x);
    dp->drag_y = wl_fixed_to_double(y);
    dp->drag_active = true;
    MwWindow *w = drag_window_for_surface(d, s);
    dp->drag_window = w ? w->id : None;

    /* Tell the compositor which type we would take and that we are a copy
     * target, so it keeps sending motion and a drop. */
    if (o->mime) wl_data_offer_accept(offer, serial, o->mime);
    if (wl_proxy_get_version((struct wl_proxy *)offer) >= 3)
        wl_data_offer_set_actions(offer,
            WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY |
            WL_DATA_DEVICE_MANAGER_DND_ACTION_MOVE,
            WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY);
    wl_display_flush(dp->wl_display);

    double fx = wl_fixed_to_double(x), fy = wl_fixed_to_double(y);
    dp->drag_xdnd = false;
    if (o->motif_drag) {
        mw_dnd_wl_enter(d, s, fx, fy, o->mime, o->motif_drag);
    } else if (mw_xdnd_aware(d, dp->drag_window)) {
        /* The X window under the drag accepts XDND; present the Wayland offer as
         * an XDND source to it. */
        dp->drag_xdnd = true;
        mw_xdnd_wl_enter(d, s, fx, fy);
    } else {
        mw_dnd_wl_enter(d, s, fx, fy, o->mime, o->motif_drag);
    }
}

static void dd_leave(void *data, struct wl_data_device *dd)
{
    (void)dd;
    Display *d = data;
    XDisplayImpl *dp = MWD(d);
    if (!dp->drag_active) return;
    dp->drag_active = false;
    dp->drag_surface = NULL;
    dp->drag_window = None;
    offer_free(dp->drag_offer);
    dp->drag_offer = NULL;
    if (dp->drag_xdnd) {
        if (!(dp->clip_fetch.active && dp->clip_fetch.is_dnd))
            mw_xdnd_wl_leave(d);
        dp->drag_xdnd = false;
    } else if (!(dp->clip_fetch.active && dp->clip_fetch.is_dnd)) {
        mw_dnd_wl_leave(d);
    }
}

static void dd_motion(void *data, struct wl_data_device *dd, uint32_t time,
                      wl_fixed_t x, wl_fixed_t y)
{
    (void)dd; (void)time;
    Display *d = data;
    XDisplayImpl *dp = MWD(d);
    if (!dp->drag_active) return;
    dp->drag_x = wl_fixed_to_double(x);
    dp->drag_y = wl_fixed_to_double(y);
    if (dp->drag_offer && dp->drag_offer->mime)
        wl_data_offer_accept(dp->drag_offer->offer, dp->drag_serial,
                             dp->drag_offer->mime);
    if (dp->drag_xdnd)
        mw_xdnd_wl_motion(d, dp->drag_x, dp->drag_y);
    else
        mw_dnd_wl_motion(d, dp->drag_x, dp->drag_y);
}

static void dd_drop(void *data, struct wl_data_device *dd)
{
    (void)dd;
    Display *d = data;
    XDisplayImpl *dp = MWD(d);

    /* A Motif-to-Motif drag carries no data over Wayland: the transfer is the
     * initiator's icc handle selection, relayed between the two processes by
     * the selection broker.  Just start the drop at the site. */
    if (dp->drag_offer && dp->drag_offer->motif_drag) {
        mw_dnd_wl_drop(d, NULL, 0);
        return;
    }

    /* XDND: fetch the dropped bytes now, before the Wayland offer is torn down
     * with the drag, and tell the target it may drop once they are in hand. */
    if (dp->drag_xdnd) {
        MwWlOffer *o = dp->drag_offer;
        const char *m = o ? (o->mime ? o->mime : (o->nmimes ? o->mimes[0] : NULL)) : NULL;
        if (!m || dp->clip_fetch.active || !o || !o->offer) {
            mw_xdnd_wl_drop(d);
            return;
        }
        int fds[2];
        if (pipe(fds) != 0) { mw_xdnd_wl_drop(d); return; }
        fcntl(fds[0], F_SETFL, fcntl(fds[0], F_GETFL) | O_NONBLOCK);
        fcntl(fds[1], F_SETFD, FD_CLOEXEC);
        fcntl(fds[0], F_SETFD, FD_CLOEXEC);
        wl_data_offer_receive(o->offer, m, fds[1]);
        close(fds[1]);
        wl_display_flush(dp->wl_display);

        MwClipFetch *f = &dp->clip_fetch;
        f->active = true;
        f->is_dnd = true;
        f->latin1 = false;
        f->xdnd = true;
        f->fd = fds[0];
        f->data = NULL;
        f->len = f->cap = 0;
        f->selection = None;
        f->target = None;
        f->property = None;
        f->requestor = None;
        f->time = CurrentTime;
        f->deadline_ms = now_ms() + CLIP_FETCH_TIMEOUT_MS;
        return;
    }

    if (!dp->drag_active || !dp->drag_offer || !dp->drag_offer->mime) {
        if (dp->drag_offer && wl_proxy_get_version((struct wl_proxy *)dp->drag_offer->offer) >= 3)
            wl_data_offer_finish(dp->drag_offer->offer);
        dd_leave(data, dd);
        return;
    }

    /* Fetch the dropped bytes on a pipe, exactly as a clipboard paste does. */
    if (dp->clip_fetch.active) { dd_leave(data, dd); return; }
    int fds[2];
    if (pipe(fds) != 0) { dd_leave(data, dd); return; }
    fcntl(fds[0], F_SETFL, fcntl(fds[0], F_GETFL) | O_NONBLOCK);
    fcntl(fds[0], F_SETFD, FD_CLOEXEC);
    fcntl(fds[1], F_SETFD, FD_CLOEXEC);

    wl_data_offer_receive(dp->drag_offer->offer, dp->drag_offer->mime, fds[1]);
    close(fds[1]);
    if (wl_proxy_get_version((struct wl_proxy *)dp->drag_offer->offer) >= 3)
        wl_data_offer_finish(dp->drag_offer->offer);
    wl_display_flush(dp->wl_display);

    MwClipFetch *f = &dp->clip_fetch;
    f->active = true;
    f->is_dnd = true;
    f->latin1 = false;
    f->fd = fds[0];
    f->data = NULL;
    f->len = f->cap = 0;
    f->selection = None;
    f->target = None;
    f->property = None;
    f->requestor = None;
    f->time = CurrentTime;
    f->deadline_ms = now_ms() + CLIP_FETCH_TIMEOUT_MS;
}

static const struct wl_data_device_listener device_listener = {
    .data_offer = dd_data_offer,
    .enter = dd_enter,
    .leave = dd_leave,
    .motion = dd_motion,
    .drop = dd_drop,
    .selection = dd_selection,
};

/* -------------------------------------------------------------- lifecycle */

void mw_clipboard_init(Display *d)
{
    XDisplayImpl *dp = MWD(d);
    dp->clip_fetch.fd = -1;
    dp->clip_fetch.is_dnd = false;
    dp->clip_serve.fd = -1;

    if (dp->dnd_mgr && dp->wl_seat && !dp->data_device) {
        dp->data_device = wl_data_device_manager_get_data_device(dp->dnd_mgr, dp->wl_seat);
        wl_data_device_add_listener(dp->data_device, &device_listener, d);
    }
    if (dp->primary_mgr && dp->wl_seat && !dp->primary_device) {
        dp->primary_device = zwp_primary_selection_device_manager_v1_get_device(
                                 dp->primary_mgr, dp->wl_seat);
        zwp_primary_selection_device_v1_add_listener(dp->primary_device,
                                                     &primary_device_listener, d);
    }
    mw_dnd_init(d);
}

void mw_clipboard_fini(Display *d)
{
    XDisplayImpl *dp = MWD(d);
    mw_dnd_fini(d);
    if (dp->clip_fetch.active) {
        if (dp->clip_fetch.fd >= 0) close(dp->clip_fetch.fd);
        free(dp->clip_fetch.data);
        dp->clip_fetch.active = false;
        dp->clip_fetch.fd = -1;
    }
    if (dp->clip_serve.active) {
        if (dp->clip_serve.fd >= 0) close(dp->clip_serve.fd);
        dp->clip_serve.active = false;
        dp->clip_serve.fd = -1;
    }
    offer_free(dp->wayland_offer);
    offer_free(dp->pending_offer);
    offer_free(dp->drag_offer);
    dp->wayland_offer = dp->pending_offer = dp->drag_offer = NULL;
    prim_offer_free(dp->primary_offer);
    prim_offer_free(dp->primary_pending);
    dp->primary_offer = dp->primary_pending = NULL;
    while (dp->wl_sources)
        source_free(d, dp->wl_sources);
    if (dp->primary_device) {
        zwp_primary_selection_device_v1_destroy(dp->primary_device);
        dp->primary_device = NULL;
    }
    if (dp->data_device) { wl_data_device_destroy(dp->data_device); dp->data_device = NULL; }
}

/* -------------------------------------------------------------- polling */

int mw_clipboard_poll_fd(Display *d)
{
    XDisplayImpl *dp = MWD(d);
    if (dp->clip_fetch.active && dp->clip_fetch.fd >= 0) return dp->clip_fetch.fd;
    return -1;
}

static void fetch_finish(Display *d)
{
    XDisplayImpl *dp = MWD(d);
    MwClipFetch *f = &dp->clip_fetch;

    unsigned char *out = f->data;
    size_t outlen = f->len;
    unsigned char *conv = NULL;
    Atom type = f->target;

    if (f->is_dnd) {
        /* A dropped drag: hand the bytes to the Motif DnD bridge, or stash them
         * for the XDND target to convert. */
        if (f->fd >= 0) close(f->fd);
        f->fd = -1;
        if (f->xdnd) mw_xdnd_store_drop(d, out, outlen);
        else         mw_dnd_wl_drop(d, out, outlen);
        free(f->data);
        f->data = NULL;
        f->len = f->cap = 0;
        f->active = false;
        f->is_dnd = false;
        f->xdnd = false;
        return;
    }

    if (f->latin1) {
        /* The offer's bytes are UTF-8; the requestor asked for STRING/TEXT. */
        conv = (unsigned char *)mw_clipboard_from_utf8(f->data, f->len, &outlen);
        out = conv;
    }

    if (f->fd >= 0) close(f->fd);
    f->fd = -1;

    if (out && outlen > 0)
        XChangeProperty(d, f->requestor, f->property, type, 8,
                        PropModeReplace, out, (int)outlen);

    XSelectionEvent se;
    memset(&se, 0, sizeof se);
    se.type = SelectionNotify;
    se.display = d;
    se.requestor = f->requestor;
    se.selection = f->selection;
    se.target = f->target;
    se.property = (out && outlen > 0) ? f->property : None;
    se.time = f->time;
    mw_put_event(d, (XEvent *)&se);

    free(conv);
    free(f->data);
    f->data = NULL;
    f->len = f->cap = 0;
    f->active = false;
}

void mw_clipboard_handle_ready(Display *d)
{
    XDisplayImpl *dp = MWD(d);
    MwClipFetch *f = &dp->clip_fetch;

    if (f->active) {
        for (;;) {
            if (f->len + 65536 > f->cap) {
                f->cap = f->cap ? f->cap * 2 : 65536;
                if (f->cap < f->len + 65536) f->cap = f->len + 65536;
                f->data = realloc(f->data, f->cap);
            }
            ssize_t n = read(f->fd, f->data + f->len, 65536);
            if (n > 0) { f->len += (size_t)n; continue; }
            if (n == 0) { fetch_finish(d); break; }              /* EOF */
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                if (now_ms() > f->deadline_ms) fetch_finish(d);
                break;
            }
            fetch_finish(d);                                      /* error */
            break;
        }
    }

    /* A stale serve (the X owner never answered) must not leak the pipe. */
    if (dp->clip_serve.active && now_ms() > dp->clip_serve.deadline_ms) {
        if (dp->clip_serve.fd >= 0) close(dp->clip_serve.fd);
        dp->clip_serve.fd = -1;
        dp->clip_serve.active = false;
    }
}

/* ----------------------------------------------------- Wayland -> X serve */

/* Start fetching `mime` from a Wayland offer and serving it to an X requestor. */
bool mw_clipboard_fetch_from_offer(Display *d, struct wl_data_offer *offer,
                                   const char *mime, Atom selection, Atom target,
                                   Atom property, Window requestor, Time time,
                                   bool latin1)
{
    XDisplayImpl *dp = MWD(d);
    if (!offer || !mime) return false;
    if (dp->clip_fetch.active) return false;

    int fds[2];
    if (pipe(fds) != 0) return false;
    fcntl(fds[0], F_SETFL, fcntl(fds[0], F_GETFL) | O_NONBLOCK);
    fcntl(fds[1], F_SETFD, FD_CLOEXEC);
    fcntl(fds[0], F_SETFD, FD_CLOEXEC);

    wl_data_offer_receive(offer, mime, fds[1]);
    close(fds[1]);
    wl_display_flush(dp->wl_display);

    MwClipFetch *f = &dp->clip_fetch;
    f->active = true;
    f->is_dnd = false;
    f->latin1 = latin1;
    f->fd = fds[0];
    f->data = NULL;
    f->len = f->cap = 0;
    f->selection = selection;
    f->target = target;
    f->property = property;
    f->requestor = requestor;
    f->time = time;
    f->deadline_ms = now_ms() + CLIP_FETCH_TIMEOUT_MS;
    return true;
}

/* Serve an X PRIMARY convert from the current Wayland primary offer. */
static bool prim_xconvert(Display *d, Atom target, Atom property,
                          Window requestor, Time time)
{
    XDisplayImpl *dp = MWD(d);
    MwPrimOffer *o = dp->primary_offer;
    if (!o || (!o->mime && o->nmimes == 0)) return false;
    if (dp->primary_device == NULL) return false;

    Atom prop = property == None ? target : property;

    if (target == a_targets(d)) {
        int n = 0;
        Atom *list = targets_from_mimes(d, o->mimes, o->nmimes, o->mime, &n);
        XChangeProperty(d, requestor, prop, XA_ATOM, 32, PropModeReplace,
                        (const unsigned char *)list, n);
        free(list);
        post_selection_notify(d, requestor, XA_PRIMARY, target, prop, time);
        return true;
    }

    if (dp->clip_fetch.active) return false;

    bool latin1 = false;
    const char *m = mime_for_target(d, o->mimes, o->nmimes, o->mime, target, &latin1);
    if (!m) return false;

    int fds[2];
    if (pipe(fds) != 0) return false;
    fcntl(fds[0], F_SETFL, fcntl(fds[0], F_GETFL) | O_NONBLOCK);
    fcntl(fds[1], F_SETFD, FD_CLOEXEC);
    fcntl(fds[0], F_SETFD, FD_CLOEXEC);

    zwp_primary_selection_offer_v1_receive(o->offer, m, fds[1]);
    close(fds[1]);
    wl_display_flush(dp->wl_display);

    MwClipFetch *f = &dp->clip_fetch;
    f->active = true;
    f->is_dnd = false;
    f->latin1 = latin1;
    f->fd = fds[0];
    f->data = NULL;
    f->len = f->cap = 0;
    f->selection = XA_PRIMARY;
    f->target = target;
    f->property = prop;
    f->requestor = requestor;
    f->time = time;
    f->deadline_ms = now_ms() + CLIP_FETCH_TIMEOUT_MS;
    return true;
}

bool mw_clipboard_xconvert(Display *d, Atom selection, Atom target,
                           Atom property, Window requestor, Time time)
{
    XDisplayImpl *dp = MWD(d);
    if (selection == XA_PRIMARY)
        return prim_xconvert(d, target, property, requestor, time);

    MwWlOffer *o = dp->wayland_offer;
    if (selection != clipboard_atom(d)) return false;
    if (!o || (!o->mime && o->nmimes == 0)) return false;
    if (dp->data_device == NULL) return false;

    Atom prop = property == None ? target : property;

    /* TARGETS: answer from the offer's MIME list; nothing to fetch. */
    if (target == a_targets(d)) {
        int n = 0;
        Atom *list = targets_from_mimes(d, o->mimes, o->nmimes, o->mime, &n);
        XChangeProperty(d, requestor, prop, XA_ATOM, 32, PropModeReplace,
                        (const unsigned char *)list, n);
        free(list);
        post_selection_notify(d, requestor, selection, target, prop, time);
        return true;
    }

    if (dp->clip_fetch.active) return false;

    bool latin1 = false;
    const char *m = mime_for_target(d, o->mimes, o->nmimes, o->mime, target, &latin1);
    if (!m) return false;

    int fds[2];
    if (pipe(fds) != 0) return false;
    fcntl(fds[0], F_SETFL, fcntl(fds[0], F_GETFL) | O_NONBLOCK);
    fcntl(fds[1], F_SETFD, FD_CLOEXEC);
    fcntl(fds[0], F_SETFD, FD_CLOEXEC);

    /* The compositor hands the write end to the source and we read the data.
     * receive() takes ownership of the fd, so we must not close it ourselves. */
    wl_data_offer_receive(o->offer, m, fds[1]);
    close(fds[1]);
    wl_display_flush(dp->wl_display);

    MwClipFetch *f = &dp->clip_fetch;
    f->active = true;
    f->is_dnd = false;
    f->latin1 = latin1;
    f->fd = fds[0];
    f->data = NULL;
    f->len = f->cap = 0;
    f->selection = selection;
    f->target = target;
    f->property = prop;
    f->requestor = requestor;
    f->time = time;
    f->deadline_ms = now_ms() + CLIP_FETCH_TIMEOUT_MS;
    return true;
}

/* ----------------------------------------------------- X -> Wayland serve */

bool mw_clipboard_serve_notify(Display *d, XSelectionEvent *se)
{
    XDisplayImpl *dp = MWD(d);
    MwClipServe *sv = &dp->clip_serve;
    if (!sv->active || dp->clip_window == None ||
        se->requestor != dp->clip_window) return false;

    Atom type = None;
    int format = 0;
    unsigned long n = 0, ba = 0;
    unsigned char *data = NULL;
    if (se->property != None)
        XGetWindowProperty(d, dp->clip_window, se->property, 0, 0x7fffffff, True,
                           AnyPropertyType, &type, &format, &n, &ba, &data);

    if (data && sv->fd >= 0) {
        if (sv->to_utf8) {
            size_t olen = 0;
            char *enc = mw_clipboard_to_utf8(data, n, &olen);
            ssize_t off = 0;
            while (off < (ssize_t)olen) {
                ssize_t w = write(sv->fd, enc + off, (size_t)(olen - off));
                if (w <= 0) break;
                off += w;
            }
            free(enc);
        } else {
            ssize_t off = 0;
            while (off < (ssize_t)n) {
                ssize_t w = write(sv->fd, data + off, (size_t)(n - off));
                if (w <= 0) break;
                off += w;
            }
        }
    }
    if (data) XFree(data);
    if (sv->fd >= 0) close(sv->fd);
    sv->fd = -1;
    sv->active = false;
    return true;
}

/* ----------------------------------------------- X ownership -> Wayland */

void mw_clipboard_owner_changed(Display *d, Atom selection, Window owner)
{
    XDisplayImpl *dp = MWD(d);
    bool is_prim = (selection == XA_PRIMARY);
    if (!is_prim && selection != clipboard_atom(d)) return;
    if (dp->clip_window != None && owner == dp->clip_window)
        return;   /* our own proxy ownership */
    if (is_prim) {
        if (!dp->primary_mgr || !dp->primary_device) return;
    } else {
        if (!dp->data_device || !dp->dnd_mgr) return;
    }

    /* Drop any previous source for this selection. */
    MwWlSource *old = source_find(d, selection);
    if (old) source_free(d, old);

    if (owner == None) {
        if (is_prim) sync_x_owner_primary(d); else sync_x_owner(d);
        return;
    }

    MwWlSource *s = calloc(1, sizeof *s);
    s->selection = selection;
    if (is_prim) {
        s->source = zwp_primary_selection_device_manager_v1_create_source(dp->primary_mgr);
        zwp_primary_selection_source_v1_add_listener(s->source, &prim_source_listener, d);
    } else {
        s->source = wl_data_device_manager_create_data_source(dp->dnd_mgr);
        wl_data_source_add_listener(s->source, &source_listener, d);
    }
    for (int i = 0; mime_priority[i]; i++) source_offer(s, mime_priority[i]);
    /* Non-text types too: source_send() converts the matching X target, and an
     * owner that cannot serve one simply yields nothing. */
    for (int i = 0; extra_mime[i]; i++) source_offer(s, extra_mime[i]);
    s->next = dp->wl_sources;
    dp->wl_sources = s;

    /* set_selection() is only honoured with a serial from recent input; the
     * user has just selected or pressed the paste shortcut. */
    if (is_prim)
        zwp_primary_selection_device_v1_set_selection(dp->primary_device, s->source,
                                                      dp->last_input_serial);
    else
        wl_data_device_set_selection(dp->data_device, s->source, dp->last_input_serial);
    wl_display_flush(dp->wl_display);
    if (is_prim) sync_x_owner_primary(d); else sync_x_owner(d);
}
