/* surface.c — X top-level windows as xdg_toplevel + wl_shm, and the
 * window-tree compositor that paints them into a Wayland buffer.
 *
 * When the compositor offers no server-side decorations we draw a minimal
 * client-side titlebar (drag to move, close button) and inset the X window's
 * content below it.
 */
#define _GNU_SOURCE
#include "internal.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <errno.h>

static uint32_t proxy_version(void *p) {
    return p ? wl_proxy_get_version((struct wl_proxy *)p) : 0;
}

#define MW_TITLEBAR_H 24
#define MW_CLOSE_W    22

static int buffer_create(MwToplevel *tl, int w, int h);

/* ------------------------------------------------------------- listeners */

static void tl_configure(void *data, struct xdg_toplevel *t, int32_t w, int32_t h,
                         struct wl_array *states);
static void tl_close(void *data, struct xdg_toplevel *t);
static void tl_configure_bounds(void *data, struct xdg_toplevel *t, int32_t w, int32_t h);
static void tl_wm_capabilities(void *data, struct xdg_toplevel *t, struct wl_array *c);

static const struct xdg_toplevel_listener toplevel_listener = {
    .configure = tl_configure,
    .close = tl_close,
    .configure_bounds = tl_configure_bounds,
    .wm_capabilities = tl_wm_capabilities,
};

static void xs_configure(void *data, struct xdg_surface *xs, uint32_t serial)
{
    MwToplevel *tl = data;
    Display *d = tl->win->d;
    xdg_surface_ack_configure(xs, serial);
    tl->configured = true;
    if (getenv("MW_TRACE"))
        fprintf(stderr, "MW: xdg configure serial=%u suggested=%dx%d keep=%dx%d%s\n",
                serial, tl->req_w, tl->req_h, tl->win->w, tl->win->h,
                tl->csd ? " (csd)" : "");

    /* With server-side decorations the compositor owns interactive resize:
     * a configure carrying a size is a resize request the window has to
     * adopt, which is the xdg-shell equivalent of the window manager resizing
     * the X window.  A configure of 0 means "no preference" -- keep the
     * client's own size, the normal case for a floating window. */
    if (!tl->is_popup && tl->req_w > 0 && tl->req_h > 0) {
        int want_w = tl->req_w;
        int want_h = tl->req_h - tl->tb_h;
        if (want_h < 1) want_h = 1;
        if (want_w != tl->win->w || want_h != tl->win->h)
            mw_window_wm_resize(tl->win, want_w, want_h);
    }

    tl->req_w = tl->win->w;
    tl->req_h = tl->win->h + tl->tb_h;
    if (tl->width != tl->req_w || tl->height != tl->req_h)
        buffer_create(tl, tl->req_w, tl->req_h);
    /* Now that the configure is acked, the popup is allowed to reposition. */
    if (tl->is_popup && !tl->repositioned) {
        tl->repositioned = true;
        mw_toplevel_reposition(tl);
    }

    /* Acknowledge the configure by committing a frame: without a commit the
     * surface stays unmapped.  Resizing already sent Expose to the window and
     * its children (mw_window_move_resize), so the client repaints and the
     * next damage flush presents the real contents; this frame is just the
     * empty/background one that satisfies the configure handshake. */
    mw_toplevel_damage(tl);
    mw_toplevel_render(tl);
    (void)d;
}

static const struct xdg_surface_listener xdg_surface_listener = {
    .configure = xs_configure,
};

static void buf_release(void *data, struct wl_buffer *b)
{
    (void)b;
    MwBuf *buf = data;
    buf->busy = false;
    if (getenv("MW_TRACE")) fprintf(stderr, "MW: buffer release idx=%d\n", buf->idx);
    /* Paint pending damage through the capped path.  Rendering here directly
     * defeated the frame pacing: the compositor releases buffers as fast as we
     * commit them, so a repaint that keeps marking damage turned into a
     * composite per release (~180/s) and the window stayed busy for a second
     * or two after a resize ended. */
    if (buf->tl && buf->tl->dirty && buf->tl->win->d)
        mw_flush_damage_deferred(buf->tl->win->d);
}

static const struct wl_buffer_listener buffer_listener = {
    .release = buf_release,
};

/* Request one frame callback per toplevel: after a commit, and when damage has
 * to wait for the next display refresh.  Requesting it without committing is
 * valid Wayland and does not tear. */
static void mw_toplevel_request_frame(MwToplevel *tl);

/* wl_surface.frame: the compositor tells us when a new frame can be presented.
 * Without it the only thing pacing us is the client happening to block, and a
 * big repaint (which a resize triggers) made the client composite and upload a
 * whole window hundreds of times a second. */
static void frame_done(void *data, struct wl_callback *cb, uint32_t t)
{
    (void)t;
    MwToplevel *tl = data;
    wl_callback_destroy(cb);
    tl->frame_cb = NULL;
    /* The compositor calls this at most once per refresh, so it is already the
     * natural pace: flush even when the 16ms limiter would defer us.  Letting
     * the limiter drop this one stranded the final frame -- xterm's scroll
     * ended a frame short on a real compositor, while the headless test passed
     * purely because its buffer-release timing did not hit the window. */
    if (tl->dirty && tl->win->d) mw_flush_damage(tl->win->d);
}

static const struct wl_callback_listener frame_listener = {
    .done = frame_done,
};

static void mw_toplevel_request_frame(MwToplevel *tl)
{
    if (!tl || tl->frame_cb || !tl->surface) return;
    tl->frame_cb = wl_surface_frame(tl->surface);
    if (tl->frame_cb)
        wl_callback_add_listener(tl->frame_cb, &frame_listener, tl);
}

/* --------------------------------------------------------------- buffers */

static void buf_free(MwBuf *b)
{
    if (b->buffer) { wl_buffer_destroy(b->buffer); b->buffer = NULL; }
    if (b->pool)   { wl_shm_pool_destroy(b->pool);   b->pool = NULL; }
    if (b->frame)  { mw_surface_destroy(b->frame);   b->frame = NULL; }
    if (b->data)   { munmap(b->data, b->size);       b->data = NULL; }
    b->size = 0;
    b->busy = false;
}

static void buffer_destroy(MwToplevel *tl)
{
    buf_free(&tl->bufs[0]);
    buf_free(&tl->bufs[1]);
}

static int buf_alloc(MwBuf *b, MwToplevel *tl, int idx, int w, int h)
{
    XDisplayImpl *dp = MWD(tl->win->d);
    int stride = w * 4;
    size_t size = (size_t)stride * h;

    int fd = memfd_create("motif-wl-shm", MFD_CLOEXEC);
    if (fd < 0) return -1;
    if (ftruncate(fd, (off_t)size) < 0) { close(fd); return -1; }
    void *data = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (data == MAP_FAILED) { close(fd); return -1; }
    struct wl_shm_pool *pool = wl_shm_create_pool(dp->wl_shm, fd, (int32_t)size);
    close(fd);
    struct wl_buffer *buf = wl_shm_pool_create_buffer(pool, 0, w, h, stride,
                                                      WL_SHM_FORMAT_ARGB8888);
    b->buffer = buf;
    b->pool = pool;
    b->data = data;
    b->size = size;
    b->stride = stride;
    b->frame = mw_surface_create_for_data(data, w, h, stride);
    b->idx = idx;
    b->tl = tl;
    b->busy = false;
    wl_buffer_add_listener(buf, &buffer_listener, b);
    return 0;
}

/* Buffer allocation granularity.  With wp_viewporter the buffers are allocated
 * at a rounded-up size and the used area is cropped per frame, so a resize
 * inside the padding needs no new buffer at all.  Without it the buffers have
 * to match the window exactly. */
#define MW_BUF_QUANTUM 128

static int buffer_create(MwToplevel *tl, int w, int h)
{
    if (w <= 0 || h <= 0) return -1;
    XDisplayImpl *dp = MWD(tl->win->d);
    if (!dp->wl_shm) return -1;

    /* A viewport lets the buffers be larger than the window, so an interactive
     * resize no longer makes the compositor import a fresh buffer for every
     * step of the drag. */
    bool want_crop = dp->viewporter && !getenv("MW_NOVIEWPORT");
    if (!tl->viewport && want_crop && tl->surface)
        tl->viewport = wp_viewporter_get_viewport(dp->viewporter, tl->surface);
    bool crop = tl->viewport != NULL;

    bool keep = crop && tl->bufs[0].buffer &&
                tl->cap_w >= w && tl->cap_h >= h;
    if (!keep) {
        int cw = w, ch = h;
        if (crop) {
            cw = (w + MW_BUF_QUANTUM - 1) / MW_BUF_QUANTUM * MW_BUF_QUANTUM;
            ch = (h + MW_BUF_QUANTUM - 1) / MW_BUF_QUANTUM * MW_BUF_QUANTUM;
        }
        buffer_destroy(tl);
        if (buf_alloc(&tl->bufs[0], tl, 0, cw, ch) != 0) return -1;
        if (buf_alloc(&tl->bufs[1], tl, 1, cw, ch) != 0) return -1;
        tl->cap_w = cw;
        tl->cap_h = ch;
    }
    tl->width = w;
    tl->height = h;

    if (crop) {
        wp_viewport_set_source(tl->viewport, wl_fixed_from_int(0),
                               wl_fixed_from_int(0),
                               wl_fixed_from_int(w), wl_fixed_from_int(h));
        wp_viewport_set_destination(tl->viewport, w, h);
    }
    return 0;
}

/* ------------------------------------------------------------- popups */

static void popup_configure(void *data, struct xdg_popup *p, int32_t x, int32_t y,
                            int32_t w, int32_t h)
{
    (void)p;
    MwToplevel *tl = data;
    if (getenv("MW_TRACE"))
        fprintf(stderr, "MW: popup configure placed x=%d y=%d %dx%d\n", x, y, w, h);
    if (w > 0) tl->req_w = w;
    if (h > 0) tl->req_h = h;
    mw_toplevel_damage(tl);
}

static void popup_done(void *data, struct xdg_popup *p)
{
    (void)p;
    MwToplevel *tl = data;
    Display *d = tl->win->d;
    if (getenv("MW_TRACE"))
        fprintf(stderr, "MW: popup_done 0x%lx (dismissed)\n", tl->win->id);

    /* A dismissed xdg_popup is inert and must not be re-used, so the Wayland
     * object set has to be rebuilt before the menu can be posted again.
     * Flags are not destroyed here, though: this runs from the Wayland event
     * dispatch, possibly while the application still holds the X window, and
     * Motif re-posts a dismissed menu by mapping the same X window again.
     * The teardown is deferred to mw_toplevel_map() so it happens at a
     * well-defined point, just before the re-post. */
    tl->popup_dismissed = true;
    if (d) mw_unmap_window(d, tl->win);   /* dismissed: hidden, like X */
}

static void popup_repositioned(void *data, struct xdg_popup *p, uint32_t token)
{ (void)data; (void)p; (void)token; }

static const struct xdg_popup_listener popup_listener = {
    .configure = popup_configure,
    .popup_done = popup_done,
    .repositioned = popup_repositioned,
};

/* ------------------------------------------------------- decorations */

/* True if the client asked for a window with no decorations via
 * _MOTIF_WM_HINTS (flags bit MWM_HINTS_DECORATIONS set and decorations == 0).
 * Motif wants such a window to be a bare content area -- neither the compositor
 * nor the fallback titlebar may draw chrome on it. */
#define MWM_HINTS_DECORATIONS (1u << 1)
static bool mw_motif_undecorated(Display *d, Window id)
{
    MwProp *p = mw_get_prop(d, mw_window(d, id),
                            mw_intern_atom(d, "_MOTIF_WM_HINTS", False));
    if (!p || p->format != 32 || p->nitems < 3 || !p->data)
        return false;
    uint32_t flags, decorations;
    memcpy(&flags, p->data, 4);
    memcpy(&decorations, p->data + 8, 4);
    return (flags & MWM_HINTS_DECORATIONS) && decorations == 0;
}

static void deco_configure(void *data, struct zxdg_toplevel_decoration_v1 *d,
                           uint32_t mode)
{
    (void)d;
    MwToplevel *tl = data;
    if (getenv("MW_TRACE"))
        fprintf(stderr, "MW: deco configure 0x%lx mode=%u (undecorated=%d)\n",
                tl->win ? tl->win->id : 0, mode, tl->undecorated);
    bool want_csd = (mode != ZXDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE)
                    && !tl->undecorated;
    if (want_csd != tl->csd) {
        tl->csd = want_csd;
        tl->tb_h = want_csd ? MW_TITLEBAR_H : 0;
        mw_toplevel_set_size(tl, tl->win->w, tl->win->h);
        mw_toplevel_damage(tl);
        if (tl->configured) mw_toplevel_render(tl);
    }
}

static const struct zxdg_toplevel_decoration_v1_listener deco_listener = {
    .configure = deco_configure,
};

/* --------------------------------------------------------------- create */

/* Resolve the ordinary toplevel that an override-redirect window (a Motif
 * menu shell, say) should be anchored to, or NULL if there is nothing to
 * anchor to yet.  Tries, in order:
 *
 *   1. the toplevel under the pointer,
 *   2. the toplevel owning the keyboard focus,
 *   3. the most recently mapped application toplevel,
 *   4. the X hierarchy (covers popups parented to their app shell).
 *
 * Any candidate that is itself override-redirect or a popup is skipped: a
 * Motif menu shell is an override-redirect toplevel, so anchoring the next
 * menu to it -- which the old code could do when the keyboard focus was an
 * item inside the menu -- produced a popup whose parent had already been
 * dismissed, and no menu after the first one would drop down. */
MwWindow *mw_popup_anchor(MwWindow *win)
{
    if (!win || !win->override_redirect) return NULL;
    Display *d = win->d;
    XDisplayImpl *dp = MWD(d);
    MwWindow *cand = dp->ptr_toplevel;

    if (!cand || cand->override_redirect) cand = dp->kbd_focus;
    while (cand && (cand->override_redirect || !cand->tl))
        cand = cand->parent;
    if (!cand) cand = dp->active_toplevel;
    if (cand && (cand->override_redirect || !cand->tl)) cand = NULL;
    if (!cand) {
        cand = win->parent;
        while (cand && (cand->override_redirect || !cand->tl))
            cand = cand->parent;
    }
    if (!cand || !cand->tl || !cand->tl->xdg_surface) return NULL;
    return cand;
}

void mw_toplevel_create(MwWindow *win)
{
    if (win->tl) return;
    Display *d = win->d;
    XDisplayImpl *dp = MWD(d);
    MwToplevel *tl = calloc(1, sizeof *tl);
    tl->win = win;
    win->tl = tl;

    int w = win->w > 0 ? win->w : 640;
    int h = win->h > 0 ? win->h : 480;

    /* An override-redirect window (Motif menus, tooltips) becomes an
     * xdg_popup anchored to the toplevel it belongs to, so the compositor
     * places it where the application asked and routes input to it. */
    MwWindow *ptl = mw_popup_anchor(win);
    if (win->override_redirect && !ptl && getenv("MW_TRACE"))
        fprintf(stderr, "MW: 0x%lx is override-redirect with no toplevel to "
                "anchor to\n", win->id);

    tl->surface = wl_compositor_create_surface(dp->wl_compositor);
    tl->xdg_surface = xdg_wm_base_get_xdg_surface(dp->wm_base, tl->surface);
    xdg_surface_add_listener(tl->xdg_surface, &xdg_surface_listener, tl);

    if (ptl) {
        tl->is_popup = true;
        tl->parent_tl = ptl;
        int rx = 0, ry = 0, tx = 0, ty = 0;
        mw_window_origin(win, &rx, &ry);
        mw_window_origin(ptl, &tx, &ty);
        int relx = rx - tx, rely = ry - ty;
        if (rely < 0) rely = 0;
        tl->positioner = xdg_wm_base_create_positioner(dp->wm_base);
        xdg_positioner_set_size(tl->positioner, w, h);
        xdg_positioner_set_anchor_rect(tl->positioner, relx, rely, 1, 1);
        xdg_positioner_set_anchor(tl->positioner, XDG_POSITIONER_ANCHOR_TOP_LEFT);
        xdg_positioner_set_gravity(tl->positioner, XDG_POSITIONER_GRAVITY_BOTTOM_RIGHT);
        xdg_positioner_set_constraint_adjustment(tl->positioner,
            XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_SLIDE_X |
            XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_SLIDE_Y |
            XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_FLIP_Y);
        tl->xdg_popup = xdg_surface_get_popup(tl->xdg_surface,
                                              ptl->tl->xdg_surface, tl->positioner);
        xdg_popup_add_listener(tl->xdg_popup, &popup_listener, tl);
        xdg_positioner_destroy(tl->positioner);
        tl->positioner = NULL;
        if (getenv("MW_TRACE"))
            fprintf(stderr, "MW: popup for 0x%lx rel=(%d,%d) %dx%d anchored to 0x%lx\n",
                    win->id, relx, rely, w, h, ptl->id);
        buffer_create(tl, w, h);
        return;
    }

    tl->xdg_toplevel = xdg_surface_get_toplevel(tl->xdg_surface);
    xdg_toplevel_add_listener(tl->xdg_toplevel, &toplevel_listener, tl);

    /* Report the application's identity to the compositor.  The WM_CLASS
     * instance (falling back to the class) becomes the Wayland app_id, and
     * WM_NAME the title.  Without this every window was "org.motif.wayland",
     * so a window manager such as CoW could not tell one application from
     * another -- nor dtwm's front panel from its workspace window. */
    {
        XClassHint ch = { NULL, NULL };
        const char *app_id = "org.motif.wayland";
        if (XGetClassHint(d, win->id, &ch) && ch.res_name) {
            if (ch.res_name[0])
                app_id = ch.res_name;
            else if (ch.res_class && ch.res_class[0])
                app_id = ch.res_class;
        }
        /* dtwm (the Front Panel) creates the panel and each sub-panel as a
         * separate toplevel, all with WM_CLASS class "Dtwm" but a per-window
         * instance (the panel itself is "FrontPanel", a sub-panel is its
         * label).  Report them as "FrontPanel" and "FrontPanelSubpanel" so the
         * window manager can treat sub-panels as part of the panel --
         * undecorated and docked above it -- rather than as ordinary windows. */
        if (ch.res_class && strcmp(ch.res_class, "Dtwm") == 0) {
            if (ch.res_name && strcmp(ch.res_name, "FrontPanel") == 0) {
                app_id = "FrontPanel";
            } else {
                /* A sub-panel: encode its label into the app_id (spaces to
                 * '-') so the window manager can style and place each one,
                 * e.g. "FrontPanelSubpanel-Personal-Applications".  app_id is
                 * set before the toplevel is placed and the title may not have
                 * arrived yet, so a title selector would miss it. */
                static char sub_id[160];
                snprintf(sub_id, sizeof sub_id, "FrontPanelSubpanel-%s",
                         (ch.res_name && ch.res_name[0]) ? ch.res_name : "sub");
                for (char *p = sub_id; *p; p++)
                    if (*p == ' ' || *p == '\t')
                        *p = '-';
                app_id = sub_id;
            }
        }
        xdg_toplevel_set_app_id(tl->xdg_toplevel, app_id);
        XFree(ch.res_name);
        XFree(ch.res_class);

        char *name = NULL;
        if (XFetchName(d, win->id, &name) && name && name[0]) {
            xdg_toplevel_set_title(tl->xdg_toplevel, name);
            XFree(name);
        } else {
            XFree(name);
            xdg_toplevel_set_title(tl->xdg_toplevel, "Motif");
        }
    }

    /* Ask for server-side decorations.  That is what an X client expects:
     * the window manager owns the frame, the titlebar and interactive
     * resizing, and the application just fills the content area.  The
     * decoration listener still runs, so if the compositor answers that it
     * only does client-side decorations we fall back to drawing one
     * ourselves. */
    /* Tell the compositor the dialog's parent (WM_TRANSIENT_FOR), so the window
     * manager can keep a dialog above its parent and place it there, as
     * CDE/MWM does.  Only same-process toplevels have a Wayland parent here;
     * the transient target may be a child, so walk up to its toplevel. */
    {
        Window tf = None;
        if (XGetTransientForHint(d, win->id, &tf) && tf != None) {
            MwWindow *pw = mw_window(d, tf);
            while (pw && !pw->tl && pw->parent) pw = pw->parent;
            if (pw && pw->tl && pw->tl->xdg_toplevel)
                xdg_toplevel_set_parent(tl->xdg_toplevel, pw->tl->xdg_toplevel);
        }
    }

    /* Forward the ICCCM size constraints (WM_NORMAL_HINTS) so the compositor
     * enforces the same limits the client asked for -- fixed-size CDE dialogs,
     * a terminal's min/max rows, and so on.  Without this every window could be
     * resized to anything the user dragged. */
    {
        XSizeHints h;
        memset(&h, 0, sizeof h);
        long supplied = 0;
        if (XGetWMNormalHints(d, win->id, &h, &supplied)) {
            if (h.flags & PMinSize)
                xdg_toplevel_set_min_size(tl->xdg_toplevel, h.min_width,
                                          h.min_height);
            if (h.flags & PMaxSize)
                xdg_toplevel_set_max_size(tl->xdg_toplevel, h.max_width,
                                          h.max_height);
        }
    }

    /* A Motif client that set _MOTIF_WM_HINTS decorations=0 wants no chrome at
     * all; ask for client-side decorations so the compositor keeps its hands
     * off, and mark the toplevel so the fallback titlebar is suppressed too. */
    tl->undecorated = win->override_redirect || mw_motif_undecorated(d, win->id);
    if (getenv("MW_TRACE"))
        fprintf(stderr, "MW: toplevel 0x%lx undecorated=%d deco_mgr=%p\n",
                win->id, tl->undecorated, (void *)dp->deco_mgr);
    if (dp->deco_mgr) {
        tl->deco = zxdg_decoration_manager_v1_get_toplevel_decoration(dp->deco_mgr,
                                                                      tl->xdg_toplevel);
        zxdg_toplevel_decoration_v1_add_listener(tl->deco, &deco_listener, tl);
        zxdg_toplevel_decoration_v1_set_mode(tl->deco,
            tl->undecorated ? ZXDG_TOPLEVEL_DECORATION_V1_MODE_CLIENT_SIDE
                            : ZXDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);
    }
    /* Assume the compositor will decorate us whenever it can; deco_configure()
     * corrects this if it turns out to be a client-side-decoration
     * compositor.  Override-redirect windows are never decorated: window
     * managers do not manage them, and drawing chrome on one (Motif's 10x10
     * helper, say) put a spurious little window on screen beside the real
     * one. */
    tl->csd = tl->undecorated ? false : (dp->deco_mgr == NULL);
    tl->tb_h = tl->csd ? MW_TITLEBAR_H : 0;

    tl->req_w = w;
    tl->req_h = h + tl->tb_h;
    buffer_create(tl, w, h + tl->tb_h);
    if (getenv("MW_TRACE"))
        fprintf(stderr, "MW: toplevel created for 0x%lx %dx%d%s\n", win->id, w, h,
                tl->csd ? " (csd)" : "");
}

void mw_toplevel_destroy(MwToplevel *tl)
{
    if (!tl) return;
    if (tl->win && tl->win->d) {
        XDisplayImpl *dp = MWD(tl->win->d);
        MwToplevel **pp = &dp->dirty_toplevels;
        while (*pp) {
            if (*pp == tl) { *pp = tl->dirty_next; tl->in_dirty_list = false; break; }
            pp = &(*pp)->dirty_next;
        }
    }
    if (tl->frame) { mw_surface_destroy(tl->frame); tl->frame = NULL; }
    if (tl->frame_cb) { wl_callback_destroy(tl->frame_cb); tl->frame_cb = NULL; }
    buffer_destroy(tl);
    if (tl->viewport) wp_viewport_destroy(tl->viewport);
    if (tl->deco) zxdg_toplevel_decoration_v1_destroy(tl->deco);
    if (tl->xdg_popup) xdg_popup_destroy(tl->xdg_popup);
    if (tl->xdg_toplevel) xdg_toplevel_destroy(tl->xdg_toplevel);
    if (tl->xdg_surface) xdg_surface_destroy(tl->xdg_surface);
    if (tl->surface) wl_surface_destroy(tl->surface);
    free(tl);
}

void mw_toplevel_set_size(MwToplevel *tl, int w, int h)
{
    if (!tl) return;
    tl->req_w = w;
    tl->req_h = h + tl->tb_h;
    if (w != tl->width || (h + tl->tb_h) != tl->height) {
        buffer_create(tl, w, h + tl->tb_h);
        mw_toplevel_damage(tl);
    }
}

void mw_toplevel_map(MwToplevel *tl)
{
    if (!tl) return;
    if (getenv("MW_TRACE"))
        fprintf(stderr, "MW: map id=0x%lx %dx%d override=%d csd=%d\n",
                tl->win->id, tl->win->w, tl->win->h, tl->win->override_redirect, tl->csd);
    tl->mapped = true;
    if (tl->req_w != tl->width || tl->req_h != tl->height)
        buffer_create(tl, tl->req_w, tl->req_h);
    if (tl->is_popup) {
        /* xdg_popup.reposition is not legal until the popup has been
         * configured and acked, so it is sent from xs_configure() instead. */
        tl->repositioned = false;
        /* xdg_popup.grab gives the popup the implicit pointer grab so it
         * receives input and dismisses on outside clicks.  KWin enforces
         * this; without it the menu is inert. */
        XDisplayImpl *dp = MWD(tl->win->d);
        uint32_t serial = dp->last_press_serial ? dp->last_press_serial
                                                : dp->last_input_serial;
        if (tl->xdg_popup && dp->wl_seat && serial && proxy_version(tl->xdg_popup) >= 3) {
            xdg_popup_grab(tl->xdg_popup, dp->wl_seat, serial);
            if (getenv("MW_TRACE"))
                fprintf(stderr, "MW: xdg_popup_grab 0x%lx serial=%u\n", tl->win->id, serial);
        }
    }
    mw_toplevel_damage(tl);
    /* The xdg-shell contract: commit with no buffer first, then ack the
     * resulting configure before attaching anything.  Attaching here would
     * raise "attached a buffer before configure event". */
    wl_surface_commit(tl->surface);
    if (tl->win->d) wl_display_flush(MWD(tl->win->d)->wl_display);
}

void mw_toplevel_unmap(MwToplevel *tl)
{
    if (!tl) return;
    tl->mapped = false;
    /* A popup is rebuilt for the next posting rather than re-used.  A popup
     * that has been configured once stays configured as far as KWin is
     * concerned: it does not send a second xdg_popup.configure when the same
     * surface is mapped again.  Waiting for one (which is what clearing
     * tl->configured here asks for) meant the re-mapped menu was never
     * rendered -- the X window was there and clicks reached it, but nothing
     * was ever painted, so no menu appeared to drop down.  Building a fresh
     * xdg_popup per posting, as GTK and Qt do, always gets a configure. */
    /* River closes its river_window_v1 when a toplevel's surface is unmapped,
     * so the same wl_surface cannot simply be re-mapped: the compositor would
     * never announce a window again (this broke any X client that withdraws and
     * re-maps a toplevel, dtwm sub-panels among them).  Mark it for rebuild so
     * mw_toplevel_map() -- actually mw_map_window() -- makes a fresh
     * xdg_toplevel on the next map.  Popups already worked this way. */
    tl->popup_dismissed = true;
    tl->configured = false;
    tl->dirty = false;
    wl_surface_attach(tl->surface, NULL, 0, 0);
    wl_surface_commit(tl->surface);
    Display *d = tl->win->d;
    if (d) {
        MWD(d)->last_flush_ms = (uint64_t)mw_now();
        if (MWD(d)->wl_display) wl_display_flush(MWD(d)->wl_display);
    }
}

void mw_toplevel_damage(MwToplevel *tl)
{
    if (!tl) return;
    tl->dirty = true;
    Display *d = tl->win->d;
    if (!d) return;
    XDisplayImpl *dp = MWD(d);
    /* Record the toplevel once (the list never needs removal: flushing skips
     * entries that are no longer dirty). */
    if (tl->in_dirty_list) return;
    tl->in_dirty_list = true;
    tl->dirty_next = dp->dirty_toplevels;
    dp->dirty_toplevels = tl;
}

void mw_toplevel_reposition(MwToplevel *tl)
{
    if (!tl || !tl->is_popup || !tl->xdg_popup || !tl->parent_tl) return;
    /* reposition is only legal on a configured, acked popup. */
    if (!tl->configured) return;
    MwToplevel *ptl = tl->parent_tl->tl;
    if (!ptl) return;
    Display *d = tl->win->d;
    if (!d) return;
    XDisplayImpl *dp = MWD(d);
    int rx = 0, ry = 0, tx = 0, ty = 0;
    mw_window_origin(tl->win, &rx, &ry);
    mw_window_origin(tl->parent_tl, &tx, &ty);
    int relx = rx - tx;
    int rely = ry - ty;
    if (rely < 0) rely = 0;
    struct xdg_positioner *pos = xdg_wm_base_create_positioner(dp->wm_base);
    xdg_positioner_set_size(pos, tl->win->w, tl->win->h);
    xdg_positioner_set_anchor_rect(pos, relx, rely, 1, 1);
    xdg_positioner_set_anchor(pos, XDG_POSITIONER_ANCHOR_TOP_LEFT);
    xdg_positioner_set_gravity(pos, XDG_POSITIONER_GRAVITY_BOTTOM_RIGHT);
    xdg_positioner_set_constraint_adjustment(pos,
        XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_SLIDE_X |
        XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_SLIDE_Y |
        XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_FLIP_Y);
    if (proxy_version(tl->xdg_popup) >= 3)
        xdg_popup_reposition(tl->xdg_popup, pos, ++tl->reposition_token);
    xdg_positioner_destroy(pos);
    if (getenv("MW_TRACE"))
        fprintf(stderr, "MW: popup reposition 0x%lx rel=(%d,%d)\n", tl->win->id, relx, rely);
}

int mw_toplevel_content_offset(MwToplevel *tl)
{
    return (tl && tl->csd && !tl->is_popup) ? tl->tb_h : 0;
}

void mw_toplevel_close(MwToplevel *tl)
{
    if (!tl) return;
    Display *d = tl->win->d;
    if (!d) return;
    Atom wmproto = mw_intern_atom(d, "WM_PROTOCOLS", True);
    Atom delwin = mw_intern_atom(d, "WM_DELETE_WINDOW", True);
    MwProp *p = mw_get_prop(d, tl->win, wmproto);
    bool supports = false;
    if (p && p->format == 32 && p->data) {
        /* mw_get_prop keeps format-32 values packed as 32-bit words, while
         * Atom is a 64-bit unsigned long, so read each item out explicitly. */
        for (unsigned long i = 0; i < p->nitems; i++) {
            uint32_t v;
            memcpy(&v, p->data + i * 4, 4);
            if ((Atom)v == delwin) supports = true;
        }
    }
    if (supports)
        mw_send_client_message(d, tl->win, wmproto, (long)delwin,
                               (long)mw_now(), 0, 0, 0);
    else
        mw_unmap_window(d, tl->win);
}

/* ------------------------------------------------------- client-side deco */

static void draw_titlebar(MwToplevel *tl)
{
    if (!tl->csd || !tl->frame) return;
    static MwFont *font;
    if (!font) font = mw_font_create("sans", 13);

    int w = tl->width;
    MwCanvas *c = mw_canvas_begin(tl->frame, NULL, 0, 0, 0);
    mw_set_operator(c, GXcopy);
    /* titlebar background */
    mw_set_source_argb(c, 0xff3b3f46);
    mw_rect(c, 0, 0, w, tl->tb_h);
    mw_fill_path(c);
    /* bottom border */
    mw_set_source_argb(c, 0xff20232a);
    mw_rect(c, 0, tl->tb_h - 1, w, 1);
    mw_fill_path(c);
    /* close button */
    mw_set_source_argb(c, 0xffb0433a);
    mw_rect(c, w - MW_CLOSE_W + 3, 3, MW_CLOSE_W - 6, tl->tb_h - 6);
    mw_fill_path(c);
    mw_set_source_argb(c, 0xffffffff);
    mw_rect(c, w - 14, 8, 8, 1); mw_fill_path(c);
    mw_rect(c, w - 11, 8, 1, 8); mw_fill_path(c);
    /* title text */
    Display *d = tl->win->d;
    char *title = NULL;
    Atom wmname = d ? mw_intern_atom(d, "WM_NAME", True) : None;
    MwProp *p = (d && wmname) ? mw_get_prop(d, tl->win, wmname) : NULL;
    if (p && p->data && p->format == 8) title = (char *)p->data;
    if (!title) title = (char *)"Motif";
    mw_set_source_argb(c, 0xffe8e8e8);
    mw_show_utf8(c, font, title, (int)strlen(title), 8, tl->tb_h - 7);
    mw_canvas_end(c);
}

/* ------------------------------------------------------------- compositor */

void mw_composite_window(MwToplevel *tl, MwWindow *win, int ox, int oy)
{
    if (!win || !win->mapped) return;
    /* A window that has been resized since it last painted still holds its
     * old, smaller offscreen surface; bring it up to date first (it is
     * recreated and filled with the window's background) so the whole window
     * is covered instead of only the region that existed before. */
    if (!win->input_only) mw_window_ensure_surface(win);
    if (win->surface && !win->input_only) {
        MwCanvas *c = mw_canvas_begin(tl->frame, NULL, 0, 0, 0);
        mw_set_operator(c, GXcopy);
        mw_canvas_copy(c, win->surface, 0, 0, ox, oy, win->w, win->h);
        mw_canvas_end(c);
    }
    for (MwWindow *ch = win->children; ch; ch = ch->next_sib)
        mw_composite_window(tl, ch, ox + ch->x, oy + ch->y);
}

void mw_toplevel_render(MwToplevel *tl)
{
    if (!tl || !tl->mapped) return;
    if (!tl->configured) { tl->dirty = true; return; }

    int w = tl->width, h = tl->height;
    int tb = tl->tb_h;

    /* Double buffering: draw into a buffer the compositor is not reading. */
    int idx = -1;
    for (int i = 0; i < 2; i++)
        if (!tl->bufs[i].busy && tl->bufs[i].frame) { idx = i; break; }
    if (idx < 0) {
        /* Both buffers are attached.  Normally wait for wl_buffer.release and
         * repaint then (re-using a displayed buffer tears).  Only if the
         * compositor has not released for a while do we steal a buffer, to
         * avoid deadlocking with compositors that never send release. */
        uint64_t now = (uint64_t)mw_now();
        if (tl->last_commit_ms && now - tl->last_commit_ms < 200) {
            mw_toplevel_damage(tl);
            return;
        }
        idx = (tl->last_idx + 1) & 1;
        if (getenv("MW_TRACE")) fprintf(stderr, "MW: buffer REUSE (stalled) -> %d\n", idx);
        if (!tl->bufs[idx].buffer) return;
    }
    /* The buffer may be larger than the window (see buffer_create), so wrap
     * just the used w x h region with the buffer's stride. */
    tl->frame = mw_surface_create_for_data(tl->bufs[idx].data, w, h,
                                           tl->bufs[idx].stride);
    if (!tl->frame) return;

    mw_surface_clear(tl->frame, 0xff000000u |
                     (tl->win->background_pixel & 0xffffff));
    mw_composite_window(tl, tl->win, 0, tb);
    if (tl->csd) {
        draw_titlebar(tl);
        if (tl->xdg_surface)
            xdg_surface_set_window_geometry(tl->xdg_surface, 0, tb, w, h - tb);
    }

    static int dump_n;
    if (getenv("MW_DUMP") && !tl->is_popup) {
        /* Debug frame dumps (MW_DUMP=1) go to $MW_DUMP_DIR, else $TMPDIR. */
        const char *dir = getenv("MW_DUMP_DIR");
        if (!dir || !*dir) dir = getenv("TMPDIR");
        if (!dir || !*dir) dir = "/tmp";
        char p[256];
        snprintf(p, sizeof p, "%s/mwroll-%02d.png", dir, (dump_n++) % 16);
        mw_surface_write_png(tl->frame, p);
    }
    if (getenv("MW_TRACE"))
        fprintf(stderr, "MW: render 0x%lx %dx%d buf=%d\n", tl->win->id, w, h, idx);
    /* Request the frame callback once.  Adding the listener again on a proxy
     * that already has one makes libwayland warn ("proxy already has a
     * listener") and registers frame_done repeatedly, so a single compositor
     * frame triggered a cascade of extra composites -- which is what made the
     * window keep repainting for a second or two after a resize ended. */
    mw_toplevel_request_frame(tl);
    wl_surface_attach(tl->surface, tl->bufs[idx].buffer, 0, 0);
    wl_surface_damage_buffer(tl->surface, 0, 0, w, h);
    wl_surface_commit(tl->surface);
    tl->bufs[idx].busy = true;
    tl->last_idx = idx;
    tl->last_commit_ms = (uint64_t)mw_now();
    tl->dirty = false;
    if (tl->frame) { mw_surface_destroy(tl->frame); tl->frame = NULL; }

    Display *d = tl->win->d;
    if (d) {
        MWD(d)->last_flush_ms = (uint64_t)mw_now();
        if (MWD(d)->wl_display) wl_display_flush(MWD(d)->wl_display);
    }
}

void mw_flush_damage(Display *d)
{
    XDisplayImpl *dp = MWD(d);
    for (MwToplevel *tl = dp->dirty_toplevels; tl; tl = tl->dirty_next)
        if (tl->dirty) mw_toplevel_render(tl);
}

/* Repaint at most once per display refresh unless the client is going idle.  A
 * client that draws in many small steps -- Motif repaints a resized window in a
 * dozen separate flushes -- would otherwise composite and upload the whole
 * window once per step, which makes an interactive resize crawl.  The damage
 * stays pending, so nothing is lost: mw_flush_damage() runs unconditionally
 * when the client blocks or asks for a round trip. */
#define MW_FRAME_MS 16
void mw_flush_damage_deferred(Display *d)
{
    XDisplayImpl *dp = MWD(d);
    if (!dp->table) return;
    uint64_t now = (uint64_t)mw_now();
    if (dp->last_flush_ms && now - dp->last_flush_ms < MW_FRAME_MS) {
        /* Too soon to composite again.  Arm a frame callback on every dirty
         * toplevel so the pending damage is presented at the next refresh
         * instead of waiting for the client to happen to draw again. */
        for (MwToplevel *tl = dp->dirty_toplevels; tl; tl = tl->dirty_next)
            if (tl->dirty) mw_toplevel_request_frame(tl);
        return;
    }

    bool any = false;
    for (MwToplevel *tl = dp->dirty_toplevels; tl && !any; tl = tl->dirty_next)
        if (tl->dirty) any = true;
    if (!any) return;

    mw_flush_damage(d);
    dp->last_flush_ms = now;
}

void mw_queue_render(Display *d)
{
    XDisplayImpl *dp = MWD(d);
    if (!dp->table) return;
    for (size_t i = 0; i < dp->table_cap; i++)
        if (dp->table[i].id && dp->table[i].kind == MW_OBJ_WINDOW) {
            MwWindow *w = dp->table[i].obj;
            if (w && w->tl) mw_toplevel_damage(w->tl);
        }
}

/* ------------------------------------------------------------- listeners */

static void tl_configure(void *data, struct xdg_toplevel *t, int32_t w, int32_t h,
                         struct wl_array *states)
{
    (void)t; (void)states;
    MwToplevel *tl = data;
    if (w > 0) tl->req_w = w;
    if (h > 0) tl->req_h = h;
    mw_toplevel_damage(tl);
}

static void tl_close(void *data, struct xdg_toplevel *t)
{
    (void)t;
    mw_toplevel_close((MwToplevel *)data);
}

static void tl_configure_bounds(void *data, struct xdg_toplevel *t,
                                int32_t w, int32_t h)
{ (void)data; (void)t; (void)w; (void)h; }
static void tl_wm_capabilities(void *data, struct xdg_toplevel *t, struct wl_array *c)
{ (void)data; (void)t; (void)c; }
