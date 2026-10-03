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

/* ------------------------------------------------------------- wl offers */

static void offer_free(MwWlOffer *o)
{
    if (!o) return;
    if (o->offer) wl_data_offer_destroy(o->offer);
    free(o->mime);
    free(o);
}

static void offer_mime(void *data, struct wl_data_offer *offer, const char *mime)
{
    MwWlOffer *o = data;
    (void)offer;
    int r = mime_rank(mime);
    if (r < 0) return;
    int cur = o->mime ? mime_rank(o->mime) : 1000;
    if (r < cur) {
        free(o->mime);
        o->mime = strdup(mime);
        /* A compositor is permitted to send the selection before the MIME
         * list; once we know this offer is text, re-evaluate ownership. */
        if (o->d && o == MWD(o->d)->wayland_offer)
            sync_x_owner(o->d);
    }
}

static const struct wl_data_offer_listener offer_listener = {
    .offer = offer_mime,
    /* v3 source/dnd actions: not used for clipboard. */
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
    if (s->source) wl_data_source_destroy(s->source);
    free(s);
}

static void source_target(void *data, struct wl_data_source *src, const char *mime)
{ (void)data; (void)src; (void)mime; }

static void source_send(void *data, struct wl_data_source *src, const char *mime, int32_t fd)
{
    Display *d = data;
    MwWlSource *s = source_find(d, MWD(d)->clip_serve.selection);
    /* Find the source this request belongs to (there is at most one per
     * selection atom). */
    if (!s || s->source != src) {
        for (s = MWD(d)->wl_sources; s; s = s->next)
            if (s->source == src) break;
    }
    if (!s) { close(fd); return; }

    /* XConvertSelection() cannot be answered synchronously here: it posts a
     * SelectionRequest that the X owner only handles on its next trip round
     * Xt's event loop, and we are inside that loop right now.  Queue the
     * transfer and finish it when the owner's SelectionNotify arrives. */
    MwClipServe *sv = &MWD(d)->clip_serve;
    if (sv->active) { close(fd); return; }
    sv->active = true;
    sv->fd = fd;
    sv->to_utf8 = mime_wants_utf8(mime);
    sv->selection = s->selection;
    sv->property = mw_intern_atom(d, "MW_CLIP_DATA", False);
    sv->deadline_ms = now_ms() + CLIP_SERVE_TIMEOUT_MS;

    XConvertSelection(d, s->selection, XA_STRING, sv->property,
                      mw_clip_window(d), CurrentTime);
    wl_display_flush(MWD(d)->wl_display);
}

static void source_cancelled(void *data, struct wl_data_source *src)
{
    Display *d = data;
    for (MwWlSource *s = MWD(d)->wl_sources; s; s = s->next)
        if (s->source == src) { source_free(d, s); break; }
    /* Losing our source means some other client took the compositor
     * clipboard; re-evaluate who should own the X selection.  This also covers
     * a compositor that delivers `selection` before `cancelled`. */
    sync_x_owner(d);
}

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
    bool has_text = dp->wayland_offer && dp->wayland_offer->mime;
    bool we_published = source_find(d, clip) != NULL;

    if (has_text && !we_published) {
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
{ (void)data;(void)dd;(void)serial;(void)s;(void)x;(void)y;(void)offer; }
static void dd_leave(void *data, struct wl_data_device *dd)
{ (void)data;(void)dd; }
static void dd_motion(void *data, struct wl_data_device *dd, uint32_t t,
                      wl_fixed_t x, wl_fixed_t y)
{ (void)data;(void)dd;(void)t;(void)x;(void)y; }
static void dd_drop(void *data, struct wl_data_device *dd)
{ (void)data;(void)dd; }

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
    dp->clip_serve.fd = -1;

    if (dp->dnd_mgr && dp->wl_seat && !dp->data_device) {
        dp->data_device = wl_data_device_manager_get_data_device(dp->dnd_mgr, dp->wl_seat);
        wl_data_device_add_listener(dp->data_device, &device_listener, d);
    }

}

void mw_clipboard_fini(Display *d)
{
    XDisplayImpl *dp = MWD(d);
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
    dp->wayland_offer = dp->pending_offer = NULL;
    while (dp->wl_sources) {
        MwWlSource *s = dp->wl_sources;
        dp->wl_sources = s->next;
        if (s->source) wl_data_source_destroy(s->source);
        free(s);
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

    if (f->target == XA_STRING) {
        /* The offer's bytes are UTF-8; the requestor asked for STRING. */
        conv = (unsigned char *)mw_clipboard_from_utf8(f->data, f->len, &outlen);
        out = conv;
        type = XA_STRING;
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

bool mw_clipboard_xconvert(Display *d, Atom selection, Atom target,
                           Atom property, Window requestor, Time time)
{
    XDisplayImpl *dp = MWD(d);
    if (selection != clipboard_atom(d)) return false;
    if (!dp->wayland_offer || !dp->wayland_offer->mime) return false;
    if (dp->clip_fetch.active) return false;
    if (dp->data_device == NULL) return false;

    int fds[2];
    if (pipe(fds) != 0) return false;
    fcntl(fds[0], F_SETFL, fcntl(fds[0], F_GETFL) | O_NONBLOCK);
    fcntl(fds[1], F_SETFD, FD_CLOEXEC);
    fcntl(fds[0], F_SETFD, FD_CLOEXEC);

    /* The compositor hands the write end to the source and we read the data.
     * receive() takes ownership of the fd, so we must not close it ourselves. */
    wl_data_offer_receive(dp->wayland_offer->offer, dp->wayland_offer->mime, fds[1]);
    close(fds[1]);
    wl_display_flush(dp->wl_display);

    MwClipFetch *f = &dp->clip_fetch;
    f->active = true;
    f->fd = fds[0];
    f->data = NULL;
    f->len = f->cap = 0;
    f->selection = selection;
    f->target = target;
    f->property = property == None ? target : property;
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
    if (selection != clipboard_atom(d)) return;
    if (dp->clip_window != None && owner == dp->clip_window)
        return;   /* our own proxy ownership */
    if (!dp->data_device || !dp->dnd_mgr) return;

    /* Drop any previous source for this selection. */
    MwWlSource *old = source_find(d, selection);
    if (old) source_free(d, old);

    if (owner == None) return;

    MwWlSource *s = calloc(1, sizeof *s);
    s->selection = selection;
    s->source = wl_data_device_manager_create_data_source(dp->dnd_mgr);
    wl_data_source_add_listener(s->source, &source_listener, d);
    for (int i = 0; mime_priority[i]; i++)
        wl_data_source_offer(s->source, mime_priority[i]);
    s->next = dp->wl_sources;
    dp->wl_sources = s;

    /* set_selection() is only honoured with a serial from recent input; the
     * user has just clicked a menu or pressed Ctrl+C. */
    wl_data_device_set_selection(dp->data_device, s->source, dp->last_input_serial);
    wl_display_flush(dp->wl_display);
    sync_x_owner(d);
}
