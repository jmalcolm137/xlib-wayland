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
#include <spawn.h>
#include <sys/wait.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <errno.h>

extern char **environ;

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
    /* xdg-shell wants the ack in the commit that applies the configure, and
     * only the newest pending configure is acked (so a burst of configures
     * that arrives before we repaint is acked once).  Acking here -- in the
     * event handler, possibly several times with no commit between -- made
     * wlroots reject a repeated serial ("wrong configure serial").  Popups
     * are the exception: their reposition must follow the ack, so they ack
     * here. */
    if (tl->is_popup)
        xdg_surface_ack_configure(xs, serial);
    else
        tl->ack_serial = serial;
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
    if (MW_TRACE()) fprintf(stderr, "MW: buffer release idx=%d\n", buf->idx);
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
    b->frame_w = b->frame_h = b->frame_scale = 0;
    b->busy = false;
}

static void buffer_destroy(MwToplevel *tl)
{
    buf_free(&tl->bufs[0]);
    buf_free(&tl->bufs[1]);
}

static int buf_alloc(MwBuf *b, MwToplevel *tl, int idx,
                     int pw, int ph, int scale)
{
    XDisplayImpl *dp = MWD(tl->win->d);
    if (pw <= 0 || ph <= 0 || pw > 32768 || ph > 32768) return -1;
    uint64_t size64 = (uint64_t)pw * 4u * (uint64_t)ph;
    if (size64 > (uint64_t)INT32_MAX) return -1;
    int stride = pw * 4;
    size_t size = (size_t)size64;

    int fd = memfd_create("motif-wl-shm", MFD_CLOEXEC);
    if (fd < 0) return -1;
    if (ftruncate(fd, (off_t)size) < 0) { close(fd); return -1; }
    void *data = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (data == MAP_FAILED) { close(fd); return -1; }
    struct wl_shm_pool *pool = wl_shm_create_pool(dp->wl_shm, fd, (int32_t)size);
    close(fd);
    if (!pool) { munmap(data, size); return -1; }
    struct wl_buffer *buf = wl_shm_pool_create_buffer(pool, 0, pw, ph, stride,
                                                      WL_SHM_FORMAT_ARGB8888);
    if (!buf) {
        wl_shm_pool_destroy(pool);
        munmap(data, size);
        return -1;
    }
    b->buffer = buf;
    b->pool = pool;
    b->data = data;
    b->size = size;
    b->stride = stride;
    b->frame = NULL;
    b->frame_w = b->frame_h = b->frame_scale = 0;
    b->idx = idx;
    b->tl = tl;
    b->busy = false;
    wl_buffer_add_listener(buf, &buffer_listener, b);
    (void)scale;
    return 0;
}

/* Buffer allocation granularity.  With wp_viewporter the buffers are allocated
 * at a rounded-up size and the used area is cropped per frame, so a resize
 * inside the padding needs no new buffer at all.  Shrink when used area drops
 * below 25% of capacity so a temporary maximize does not permanently pin RAM. */
#define MW_BUF_QUANTUM 128

static int buffer_create(MwToplevel *tl, int w, int h)
{
    if (w <= 0 || h <= 0) return -1;
    XDisplayImpl *dp = MWD(tl->win->d);
    if (!dp->wl_shm) return -1;

    int sc = mw_display_scale(tl->win->d);
    int pw = w * sc, ph = h * sc;

    bool want_crop = dp->viewporter && !getenv("MW_NOVIEWPORT");
    if (!tl->viewport && want_crop && tl->surface)
        tl->viewport = wp_viewporter_get_viewport(dp->viewporter, tl->surface);
    bool crop = tl->viewport != NULL;

    bool keep = crop && tl->bufs[0].buffer && tl->buf_scale == sc &&
                tl->cap_w >= pw && tl->cap_h >= ph;
    if (keep && (tl->cap_w > MW_BUF_QUANTUM || tl->cap_h > MW_BUF_QUANTUM) &&
        (size_t)pw * (size_t)ph * 4 < (size_t)tl->cap_w * (size_t)tl->cap_h) {
        /* Shrink back down after e.g. un-maximizing from 4K. */
        keep = false;
    }
    if (!keep) {
        int cw = pw, ch = ph;
        if (crop) {
            cw = (pw + MW_BUF_QUANTUM - 1) / MW_BUF_QUANTUM * MW_BUF_QUANTUM;
            ch = (ph + MW_BUF_QUANTUM - 1) / MW_BUF_QUANTUM * MW_BUF_QUANTUM;
        }
        buffer_destroy(tl);
        if (buf_alloc(&tl->bufs[0], tl, 0, cw, ch, sc) != 0) return -1;
        if (buf_alloc(&tl->bufs[1], tl, 1, cw, ch, sc) != 0) {
            buf_free(&tl->bufs[0]);
            return -1;
        }
        tl->cap_w = cw;
        tl->cap_h = ch;
        tl->buf_scale = sc;
        tl->damage_full = true;
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
    /* The compositor's popup grab is gone; a modeled XGrabPointer on this
     * popup would otherwise strand the next press on the hidden window. */
    if (d) mw_pointer_drop_grab(d, tl->win);
}

static void popup_repositioned(void *data, struct xdg_popup *p, uint32_t token)
{ (void)data; (void)p; (void)token; }

static const struct xdg_popup_listener popup_listener = {
    .configure = popup_configure,
    .popup_done = popup_done,
    .repositioned = popup_repositioned,
};

/* ------------------------------------------------------- decorations */

/* Full _MOTIF_WM_HINTS decoding (Xm/MwmUtil.h).  Both MWM_FUNC_ALL and
 * MWM_DECOR_ALL invert the meaning of the remaining bits (listed bits are
 * excluded rather than included). */
#define MWM_HINTS_FUNCTIONS   (1u << 0)
#define MWM_HINTS_DECORATIONS (1u << 1)
#define MWM_HINTS_INPUT_MODE  (1u << 2)
#define MWM_HINTS_STATUS      (1u << 3)

#define MWM_FUNC_ALL          (1u << 0)
#define MWM_FUNC_RESIZE       (1u << 1)
#define MWM_FUNC_MOVE         (1u << 2)
#define MWM_FUNC_MINIMIZE     (1u << 3)
#define MWM_FUNC_MAXIMIZE     (1u << 4)
#define MWM_FUNC_CLOSE        (1u << 5)

#define MWM_DECOR_ALL         (1u << 0)
#define MWM_DECOR_BORDER      (1u << 1)
#define MWM_DECOR_RESIZEH     (1u << 2)
#define MWM_DECOR_TITLE       (1u << 3)
#define MWM_DECOR_MENU        (1u << 4)
#define MWM_DECOR_MINIMIZE    (1u << 5)
#define MWM_DECOR_MAXIMIZE    (1u << 6)

static bool mw_motif_decorations(Display *d, Window id, uint32_t *present)
{
    MwProp *p = mw_get_prop(d, mw_window(d, id),
                            mw_intern_atom(d, "_MOTIF_WM_HINTS", False));
    if (!p || p->format != 32 || p->nitems < 3 || !p->data)
        return false;
    uint32_t flags, decorations;
    memcpy(&flags, p->data, 4);
    memcpy(&decorations, p->data + 8, 4);
    if (!(flags & MWM_HINTS_DECORATIONS))
        return false;
    if (decorations == 0) {
        *present = 0;
        return true;
    }
    *present = (decorations & MWM_DECOR_ALL) ? ~decorations : decorations;
    return true;
}

/* True if the client asked for a window with neither border nor titlebar via
 * _MOTIF_WM_HINTS (e.g. decorations == 0, or MWM_DECOR_ALL removing both). */
static bool mw_motif_undecorated(Display *d, Window id)
{
    uint32_t present;
    return mw_motif_decorations(d, id, &present) &&
           !(present & (MWM_DECOR_BORDER | MWM_DECOR_TITLE));
}

/* Decode the functions the client allows, from _MOTIF_WM_HINTS.  Motif's
 * MWM_FUNC_ALL means the listed bits are the functions to *remove*; without it
 * the bits are the functions that are *present*.  Returns false when the
 * property is absent or says nothing about functions. */
static bool mw_motif_functions(Display *d, Window id, uint32_t *present)
{
    MwProp *p = mw_get_prop(d, mw_window(d, id),
                            mw_intern_atom(d, "_MOTIF_WM_HINTS", False));
    if (!p || p->format != 32 || p->nitems < 2 || !p->data)
        return false;
    uint32_t flags, functions;
    memcpy(&flags, p->data, 4);
    memcpy(&functions, p->data + 4, 4);
    if (!(flags & MWM_HINTS_FUNCTIONS) || functions == 0)
        return false;
    *present = (functions & MWM_FUNC_ALL) ? ~functions : functions;
    return true;
}

static bool mw_motif_no_resize(Display *d, Window id)
{
    uint32_t present;
    return mw_motif_functions(d, id, &present) && !(present & MWM_FUNC_RESIZE);
}

bool mw_motif_no_move(Display *d, Window id)
{
    uint32_t present;
    return mw_motif_functions(d, id, &present) && !(present & MWM_FUNC_MOVE);
}

/* Close is the one function the client owns under Wayland: the compositor asks
 * with xdg_toplevel.close and the client acts.  If the client disabled it, the
 * window manager's close button must not close the window, so ignore it. */
static bool mw_motif_no_close(Display *d, Window id)
{
    uint32_t present;
    return mw_motif_functions(d, id, &present) && !(present & MWM_FUNC_CLOSE);
}

static void spawn_motif_helper(const char *helper, const char *profile,
                               const char *target)
{
    char *argv[] = { (char *)helper, "decor", "-a", (char *)profile,
                     "-t", (char *)target, NULL };
    /* Double fork: the intermediate child spawns the helper and exits at
     * once, so it is reaped here and the helper is re-parented to init.
     * Spawning directly left a zombie per helper, and an application's
     * own waitpid(-1) could collect pids it never started. */
    pid_t mid = fork();
    if (mid == 0) {
        pid_t pid;
        int rc = posix_spawnp(&pid, helper, NULL, NULL, argv, environ);
        _exit(rc == 0 ? 0 : 1);
    }
    if (mid > 0)
        while (waitpid(mid, NULL, 0) < 0 && errno == EINTR) {}
}

/* Relay full _MOTIF_WM_HINTS (both decorations and functions) to the window
 * manager via sparse CoW decor profiles when CDE_MOTIF_HELPER is set, and keep
 * Wayland-side min/max size and decoration mode in sync on dynamic updates.
 *
 * Sparse profiles defined by the session (config/cow.conf):
 *   cde-decor-noborder  border.width 0
 *   cde-decor-notitle   titlebar.enabled false
 *   cde-decor-nomenu    titlebar.button.1.vector none
 *   cde-func-min        titlebar.button.0.vector none
 *   cde-func-max        titlebar.button.2.vector none
 */
void mw_apply_motif_functions(Display *d, MwWindow *win)
{
    if (!win || !win->tl || !win->tl->xdg_toplevel || win->override_redirect)
        return;

    MwToplevel *tl = win->tl;
    uint32_t fn = 0, dec = 0;
    bool has_fn = mw_motif_functions(d, win->id, &fn);
    bool has_dec = mw_motif_decorations(d, win->id, &dec);

    /* Keep Wayland-side resize and undecorated state in sync when _MOTIF_WM_HINTS
     * is updated after the toplevel has been created. */
    if (has_fn) {
        if (!(fn & MWM_FUNC_RESIZE)) {
            int cw = win->w > 0 ? win->w : tl->width;
            int ch = win->h > 0 ? win->h : tl->height;
            if (cw > 0 && ch > 0) {
                xdg_toplevel_set_min_size(tl->xdg_toplevel, cw, ch);
                xdg_toplevel_set_max_size(tl->xdg_toplevel, cw, ch);
            }
        } else {
            xdg_toplevel_set_min_size(tl->xdg_toplevel, 0, 0);
            xdg_toplevel_set_max_size(tl->xdg_toplevel, 0, 0);
        }
    }
    if (has_dec) {
        bool undec = !(dec & (MWM_DECOR_BORDER | MWM_DECOR_TITLE));
        if (undec != tl->undecorated) {
            tl->undecorated = undec;
            if (tl->deco) {
                zxdg_toplevel_decoration_v1_set_mode(tl->deco,
                    undec ? ZXDG_TOPLEVEL_DECORATION_V1_MODE_CLIENT_SIDE
                          : ZXDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);
            }
        }
    }

    const char *helper = getenv("CDE_MOTIF_HELPER");
    if (!helper || !*helper)
        return;
    if (!has_fn && !has_dec)
        return;

    uint32_t mask = 0;
    if (has_dec && !(dec & MWM_DECOR_BORDER))                      mask |= (1u << 0);
    if (has_dec && !(dec & MWM_DECOR_TITLE))                       mask |= (1u << 1);
    if (has_dec && !(dec & MWM_DECOR_MENU))                        mask |= (1u << 2);
    if ((has_fn && !(fn & MWM_FUNC_MINIMIZE)) ||
        (has_dec && !(dec & MWM_DECOR_MINIMIZE)))                  mask |= (1u << 3);
    if ((has_fn && !(fn & MWM_FUNC_MAXIMIZE)) ||
        (has_dec && !(dec & MWM_DECOR_MAXIMIZE)))                  mask |= (1u << 4);

    if (tl->motif_functions_sent && mask == tl->motif_profiles_mask)
        return;
    if (!tl->motif_functions_sent && mask == 0) {
        tl->motif_functions_sent = true;
        tl->motif_profiles_mask = 0;
        return;
    }

    if (getenv("MW_TRACE"))
        fprintf(stderr, "MW: apply_motif 0x%lx fn=0x%x dec=0x%x mask=0x%x title=%s\n",
                win->id, fn, dec, mask, tl->title ? tl->title : "(none)");

    char target[512];
    if (tl->title && tl->title[0]) {
        snprintf(target, sizeof target, "/%s", tl->title);
    } else if (tl->app_id && tl->app_id[0]) {
        snprintf(target, sizeof target, "%%%s", tl->app_id);
    } else {
        return;
    }

    static const char *const prof_names[5] = {
        "cde-decor-noborder",
        "cde-decor-notitle",
        "cde-decor-nomenu",
        "cde-func-min",
        "cde-func-max",
    };

    /* If a previously hidden element is being re-enabled, reset to default
     * first so the restored element reappears before applying the new mask. */
    if (tl->motif_functions_sent && (tl->motif_profiles_mask & ~mask) != 0)
        spawn_motif_helper(helper, "default", target);

    for (int i = 0; i < 5; i++) {
        if (mask & (1u << i))
            spawn_motif_helper(helper, prof_names[i], target);
    }
    tl->motif_functions_sent = true;
    tl->motif_profiles_mask = mask;
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

/* A window can anchor a popup if it is a Wayland toplevel of its own -- i.e.
 * it has an xdg_surface and is not itself a popup.  A popup is skipped: a
 * Motif menu shell is an override-redirect toplevel, so anchoring the next menu
 * to it -- which the old code could do when the keyboard focus was an item
 * inside the menu -- produced a popup whose parent had already been dismissed,
 * and no menu after the first one would drop down.
 *
 * Note that override_redirect is deliberately *not* part of this test.  An
 * override-redirect window that is a non-popup toplevel is an application
 * window that the window manager simply does not decorate (see mw_map_window):
 * an XForms form shown with FL_NOBORDER, say.  A client whose only window is
 * borderless still needs its menus anchored to something. */
static bool mw_anchorable(MwWindow *w)
{
    return w && w->tl && w->tl->xdg_surface && !w->tl->is_popup;
}

/* Resolve the toplevel that an override-redirect window (a Motif menu shell, say)
 * should be anchored to, or NULL if there is nothing to anchor to yet.  Tries, in
 * order:
 *
 *   1. the toplevel under the pointer,
 *   2. the toplevel owning the keyboard focus,
 *   3. the most recently mapped application toplevel,
 *   4. the X hierarchy (covers popups parented to their app shell).
 *
 * Popups are skipped and the search continues, see mw_anchorable(). */
MwWindow *mw_popup_anchor(MwWindow *win)
{
    if (!win || !win->override_redirect) return NULL;
    Display *d = win->d;
    XDisplayImpl *dp = MWD(d);

    /* A menu mapped while another menu is already open is that menu's
     * submenu, and has to be a *child popup* of it.  Anchoring it to the
     * application toplevel instead makes it a sibling, and sibling popups do
     * not nest: moving the pointer into the submenu leaves the parent menu's
     * popup subtree, and the compositor dismisses the parent out from under
     * the selection. */
    MwWindow *open = dp->open_menu;
    if (open && open != win && open->mapped && open->tl &&
        open->tl->is_popup && open->tl->xdg_surface &&
        !open->tl->popup_dismissed)
        return open;

    MwWindow *cand = dp->ptr_toplevel;

    if (!mw_anchorable(cand)) cand = dp->kbd_focus;
    while (cand && !mw_anchorable(cand)) cand = cand->parent;
    if (!mw_anchorable(cand)) cand = dp->active_toplevel;
    if (cand && !mw_anchorable(cand)) cand = NULL;
    if (!cand) {
        cand = win->parent;
        while (cand && !mw_anchorable(cand)) cand = cand->parent;
    }
    if (!mw_anchorable(cand)) return NULL;
    return cand;
}

void mw_toplevel_create(MwWindow *win)
{
    if (win->tl) return;
    Display *d = win->d;
    XDisplayImpl *dp = MWD(d);
    MwToplevel *tl = calloc(1, sizeof *tl);
    if (!tl) return;
    tl->win = win;
    win->tl = tl;
    pixman_region32_init(&tl->damage);
    pixman_region32_init(&tl->buf_damage[0]);
    pixman_region32_init(&tl->buf_damage[1]);
    tl->damage_full = true;

    int w = win->w > 0 ? win->w : 640;
    int h = win->h > 0 ? win->h : 480;

    /* An override-redirect window (Motif menus, tooltips) becomes an
     * xdg_popup anchored to the toplevel it belongs to, so the compositor
     * places it where the application asked and routes input to it.  With
     * nothing to anchor to it becomes an undecorated xdg_toplevel instead --
     * mw_map_window() only lets a visible window this far. */
    MwWindow *ptl = mw_popup_anchor(win);
    if (win->override_redirect && !ptl && MW_TRACE())
        fprintf(stderr, "MW: 0x%lx is override-redirect with no toplevel to "
                "anchor to: undecorated toplevel\n", win->id);

    tl->surface = wl_compositor_create_surface(dp->wl_compositor);
    wl_surface_set_user_data(tl->surface, win);
    tl->xdg_surface = xdg_wm_base_get_xdg_surface(dp->wm_base, tl->surface);
    xdg_surface_add_listener(tl->xdg_surface, &xdg_surface_listener, tl);

    if (ptl) {
        tl->is_popup = true;
        tl->parent_tl = ptl;
        win->popup_parent = ptl;   /* the menu this (sub)menu nests under */
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
        char sub_id[160];
        const char *app_id = "org.motif.wayland";
        if (XGetClassHint(d, win->id, &ch) && ch.res_name) {
            if (ch.res_name[0])
                app_id = ch.res_name;
            else if (ch.res_class && ch.res_class[0])
                app_id = ch.res_class;
        }
        if (ch.res_class && strcmp(ch.res_class, "Dtwm") == 0) {
            if (ch.res_name && strcmp(ch.res_name, "FrontPanel") == 0) {
                app_id = "FrontPanel";
            } else {
                snprintf(sub_id, sizeof sub_id, "FrontPanelSubpanel-%s",
                         (ch.res_name && ch.res_name[0]) ? ch.res_name : "sub");
                for (char *p = sub_id; *p; p++)
                    if (*p == ' ' || *p == '\t')
                        *p = '-';
                app_id = sub_id;
            }
        }
        xdg_toplevel_set_app_id(tl->xdg_toplevel, app_id);
        free(tl->app_id);
        tl->app_id = strdup(app_id);
        XFree(ch.res_name);
        XFree(ch.res_class);

        char *name = NULL;
        const char *tname = "Motif";
        if (XFetchName(d, win->id, &name) && name && name[0])
            tname = name;
        xdg_toplevel_set_title(tl->xdg_toplevel, tname);
        free(tl->title);
        tl->title = strdup(tname);
        XFree(name);
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

    /* A window whose _MOTIF_WM_HINTS functions drop RESIZE is fixed-size even
     * if it supplied no size hints; pin min == max to its current size. */
    if (mw_motif_no_resize(d, win->id)) {
        xdg_toplevel_set_min_size(tl->xdg_toplevel, w, h);
        xdg_toplevel_set_max_size(tl->xdg_toplevel, w, h);
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
    tl->frame = NULL;
    if (tl->frame_cb) { wl_callback_destroy(tl->frame_cb); tl->frame_cb = NULL; }
    buffer_destroy(tl);
    pixman_region32_fini(&tl->damage);
    pixman_region32_fini(&tl->buf_damage[0]);
    pixman_region32_fini(&tl->buf_damage[1]);
    if (tl->viewport) wp_viewport_destroy(tl->viewport);
    if (tl->deco) zxdg_toplevel_decoration_v1_destroy(tl->deco);
    if (tl->xdg_popup) xdg_popup_destroy(tl->xdg_popup);
    if (tl->xdg_toplevel) xdg_toplevel_destroy(tl->xdg_toplevel);
    if (tl->xdg_surface) xdg_surface_destroy(tl->xdg_surface);
    if (tl->surface) {
        wl_surface_set_user_data(tl->surface, NULL);
        wl_surface_destroy(tl->surface);
    }
    free(tl->title);
    free(tl->app_id);
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

/* ---- EWMH _NET_WM_STATE (we are the window manager) ----
 *
 * A client asking to go fullscreen sends a ClientMessage to the root window
 * (see gdk_window_fullscreen).  We are the window manager, so we translate it
 * into the xdg-shell request the compositor understands; the compositor then
 * does the real work.  Only fullscreen is implemented -- the other states are
 * accepted and ignored. */

/* Mirror the state back onto the window: GDK reads _NET_WM_STATE to answer
 * gdk_window_get_state()/get_fullscreen() and to update the application's UI,
 * and it watches the property for changes. */
static void toplevel_sync_net_wm_state(Display *d, MwWindow *win)
{
    Atom state = mw_intern_atom(d, "_NET_WM_STATE", True);
    if (!state || !win->tl) return;
    unsigned long atoms[3];
    int n = 0;
    if (win->tl->fullscreen)
        atoms[n++] = mw_intern_atom(d, "_NET_WM_STATE_FULLSCREEN", True);
    if (win->tl->maximized) {
        atoms[n++] = mw_intern_atom(d, "_NET_WM_STATE_MAXIMIZED_VERT", True);
        atoms[n++] = mw_intern_atom(d, "_NET_WM_STATE_MAXIMIZED_HORZ", True);
    }
    XChangeProperty(d, win->id, state, XA_ATOM, 32, PropModeReplace,
                    n ? (unsigned char *)atoms : NULL, n);
}

void mw_toplevel_set_fullscreen(Display *d, MwWindow *win, bool on)
{
    MwToplevel *tl = win ? win->tl : NULL;
    if (!tl || tl->is_popup || win->override_redirect) return;
    if (tl->fullscreen == on) return;

    tl->fullscreen = on;
    if (tl->xdg_toplevel) {
        if (on) xdg_toplevel_set_fullscreen(tl->xdg_toplevel, NULL);
        else    xdg_toplevel_unset_fullscreen(tl->xdg_toplevel);
    }

    /* Fullscreen covers the screen, so drop the client-side titlebar and fill
     * the screen; the compositor may adjust the geometry with its own
     * configure afterwards. */
    if (on) {
        tl->fs_w = win->w;
        tl->fs_h = win->h;
        tl->fs_csd = tl->csd;
        tl->csd = false;
        tl->tb_h = 0;
        mw_window_wm_resize(win, MWSCR(d)->width, MWSCR(d)->height);
    } else {
        bool csd = tl->fs_csd && !tl->undecorated;
        tl->csd = csd;
        tl->tb_h = csd ? MW_TITLEBAR_H : 0;
        mw_window_wm_resize(win, tl->fs_w, tl->fs_h);
    }

    if (tl->configured) {
        mw_toplevel_set_size(tl, win->w, win->h);
        mw_toplevel_damage(tl);
        mw_toplevel_render(tl);
    }
    toplevel_sync_net_wm_state(d, win);
    if (getenv("MW_TRACE"))
        fprintf(stderr, "MW: fullscreen %d win=0x%lx %dx%d csd=%d\n",
                on, win->id, win->w, win->h, tl->csd);
}

void mw_wm_net_wm_state(Display *d, XClientMessageEvent *cm)
{
    MwWindow *win = mw_window(d, cm->window);
    if (!win || !win->tl || win->tl->is_popup) return;
    Atom fs = mw_intern_atom(d, "_NET_WM_STATE_FULLSCREEN", True);
    Atom mv = mw_intern_atom(d, "_NET_WM_STATE_MAXIMIZED_VERT", True);
    Atom mh = mw_intern_atom(d, "_NET_WM_STATE_MAXIMIZED_HORZ", True);
    long action = cm->data.l[0];              /* 0 remove, 1 add, 2 toggle */
    for (int i = 0; i < 2; i++) {
        Atom a = (Atom)cm->data.l[1 + i];
        if (!a) continue;
        if (a == fs) {
            bool on = (action == 1) || (action == 2 && !win->tl->fullscreen);
            mw_toplevel_set_fullscreen(d, win, on);
        } else if (a == mv || a == mh) {
            /* xdg-shell only has a full maximize, and GDK sends both hints,
             * so either atom maximises (and a half-maximised request is
             * treated as a full one). */
            bool on = (action == 1) || (action == 2 && !win->tl->maximized);
            mw_toplevel_set_maximized(d, win, on);
        }
    }
}

/* EWMH _NET_WM_MOVERESIZE: the client asks the window manager to run an
 * interactive move or resize.  A client-side-decorated Firefox drags itself
 * this way (gdk_window_begin_move_drag); we are the window manager, so turn it
 * into an xdg-shell move/resize on the toplevel, with the serial of the press
 * that started the drag. */
void mw_wm_moveresize(Display *d, XClientMessageEvent *cm)
{
    XDisplayImpl *dp = MWD(d);
    MwWindow *win = mw_window(d, cm->window);
    if (!win || !win->tl) win = dp->active_toplevel;
    MwToplevel *tl = win ? win->tl : NULL;
    if (!tl || tl->is_popup || win->override_redirect ||
        !tl->xdg_toplevel || !dp->wl_seat)
        return;
    uint32_t serial = dp->last_press_serial ? dp->last_press_serial
                                            : dp->last_input_serial;
    if (!serial) return;
    long direction = cm->data.l[2];
    if (direction == 8) {                 /* _NET_WM_MOVERESIZE_MOVE */
        if (!mw_motif_no_move(d, win->id))
            xdg_toplevel_move(tl->xdg_toplevel, dp->wl_seat, serial);
    } else if (direction >= 0 && direction <= 7) {
        if (mw_motif_no_resize(d, win->id))
            return;
        static const uint32_t edges[8] = {
            XDG_TOPLEVEL_RESIZE_EDGE_TOP_LEFT,
            XDG_TOPLEVEL_RESIZE_EDGE_TOP,
            XDG_TOPLEVEL_RESIZE_EDGE_TOP_RIGHT,
            XDG_TOPLEVEL_RESIZE_EDGE_RIGHT,
            XDG_TOPLEVEL_RESIZE_EDGE_BOTTOM_RIGHT,
            XDG_TOPLEVEL_RESIZE_EDGE_BOTTOM,
            XDG_TOPLEVEL_RESIZE_EDGE_BOTTOM_LEFT,
            XDG_TOPLEVEL_RESIZE_EDGE_LEFT,
        };
        xdg_toplevel_resize(tl->xdg_toplevel, dp->wl_seat, serial,
                            edges[direction]);
    } else {
        return;
    }
    if (getenv("MW_TRACE"))
        fprintf(stderr, "MW: moveresize win=0x%lx dir=%ld serial=%u\n",
                win->id, direction, serial);
    if (dp->wl_display) wl_display_flush(dp->wl_display);
}

/* Maximize has no client-side geometry of its own: xdg_toplevel_set_maximized
 * lets the compositor size the window (keeping its decorations), and its
 * configure resizes the X window.  We only track the state and mirror it. */
void mw_toplevel_set_maximized(Display *d, MwWindow *win, bool on)
{
    MwToplevel *tl = win ? win->tl : NULL;
    if (!tl || tl->is_popup || win->override_redirect) return;
    if (tl->maximized == on) return;
    tl->maximized = on;
    if (tl->xdg_toplevel) {
        if (on) xdg_toplevel_set_maximized(tl->xdg_toplevel);
        else    xdg_toplevel_unset_maximized(tl->xdg_toplevel);
    }
    toplevel_sync_net_wm_state(d, win);
    if (getenv("MW_TRACE"))
        fprintf(stderr, "MW: maximize %d win=0x%lx\n", on, win->id);
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
        if (tl->xdg_popup && dp->wl_seat && serial) {   /* grab: since v1 */
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

static void toplevel_mark_dirty_list(MwToplevel *tl)
{
    tl->dirty = true;
    Display *d = tl->win->d;
    if (!d) return;
    XDisplayImpl *dp = MWD(d);
    if (tl->in_dirty_list) return;
    tl->in_dirty_list = true;
    tl->dirty_next = dp->dirty_toplevels;
    dp->dirty_toplevels = tl;
}

void mw_toplevel_damage(MwToplevel *tl)
{
    if (!tl) return;
    tl->damage_full = true;
    toplevel_mark_dirty_list(tl);
}

void mw_toplevel_damage_rect(MwToplevel *tl, int x, int y, int w, int h)
{
    if (!tl || w <= 0 || h <= 0) return;
    if (!tl->damage_full) {
        pixman_region32_union_rect(&tl->damage, &tl->damage, x, y,
                                   (unsigned)w, (unsigned)h);
        pixman_region32_union_rect(&tl->buf_damage[0], &tl->buf_damage[0], x, y,
                                   (unsigned)w, (unsigned)h);
        pixman_region32_union_rect(&tl->buf_damage[1], &tl->buf_damage[1], x, y,
                                   (unsigned)w, (unsigned)h);
        if (pixman_region32_n_rects(&tl->damage) > 16) {
            pixman_box32_t *e = pixman_region32_extents(&tl->damage);
            pixman_region32_reset(&tl->damage, e);
        }
        for (int i = 0; i < 2; i++) {
            if (pixman_region32_n_rects(&tl->buf_damage[i]) > 16) {
                pixman_box32_t *e = pixman_region32_extents(&tl->buf_damage[i]);
                pixman_region32_reset(&tl->buf_damage[i], e);
            }
        }
    }
    toplevel_mark_dirty_list(tl);
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
    Display *d = tl->win->d;
    XDisplayImpl *dp = d ? MWD(d) : NULL;
    MwFont *font = dp ? dp->csd_font : NULL;
    if (!font && dp) {
        font = mw_font_create("sans", 13);
        dp->csd_font = font;
    }

    int w = tl->width;
    MwCanvas *c = mw_canvas_begin(tl->frame, NULL, 0, 0, 0);
    if (!c) return;
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
    mw_set_line(c, 2, LineSolid, CapButt, JoinMiter, 0, NULL, 0);
    XSegment xs[2] = {
        { (short)(w - 15), 8, (short)(w - 7), 16 },
        { (short)(w - 7),  8, (short)(w - 15), 16 }
    };
    mw_segments(c, xs, 2);
    mw_stroke_path(c);
    /* title text */
    const char *title = "Motif";
    int title_len = 5;
    Atom wmname = d ? mw_intern_atom(d, "WM_NAME", True) : None;
    MwProp *p = (d && wmname) ? mw_get_prop(d, tl->win, wmname) : NULL;
    if (p && p->data && p->format == 8) {
        title = (const char *)p->data;
        title_len = (int)strnlen(title, p->nbytes);
    }
    mw_set_source_argb(c, 0xffe8e8e8);
    if (font) mw_show_utf8(c, font, title, title_len, 8, tl->tb_h - 7);
    mw_canvas_end(c);
}

/* ------------------------------------------------------------- compositor */

static void composite_rec(MwCanvas *c, MwToplevel *tl, MwWindow *win,
                          int ox, int oy, pixman_region32_t *dmg)
{
    if (!win || !win->mapped) return;
    if (dmg) {
        pixman_box32_t box = { ox, oy, ox + win->w, oy + win->h };
        if (pixman_region32_contains_rectangle(dmg, &box) == PIXMAN_REGION_OUT)
            return;
    }

    mw_canvas_save(c);
    /* Clip to this window's content box so children cannot paint outside
     * their parent (e.g. XmScrolledWindow viewports or CSD titlebar). */
    mw_canvas_clip_rect(c, ox, oy, win->w, win->h);

    if (win->has_shape) {
        int n = 0;
        pixman_box32_t *b = pixman_region32_rectangles(&win->shape, &n);
        if (n == 0) {
            mw_canvas_restore(c);
            return;
        }
        XRectangle stack_r[16];
        XRectangle *r = n <= 16 ? stack_r : malloc((size_t)n * sizeof *r);
        if (r) {
            for (int i = 0; i < n; i++) {
                r[i].x = (short)b[i].x1;
                r[i].y = (short)b[i].y1;
                r[i].width  = (unsigned short)(b[i].x2 - b[i].x1);
                r[i].height = (unsigned short)(b[i].y2 - b[i].y1);
            }
            mw_canvas_clip_rects_intersect(c, r, n, ox, oy);
            if (r != stack_r) free(r);
        }
    }

    if (!win->input_only) {
        if (win->surface && !win->surface_valid)
            mw_window_ensure_surface(win);
        if (win->surface) {
            if (win->depth >= 32) {
                mw_canvas_blit_over(c, win->surface, ox, oy, win->w, win->h);
            } else {
                mw_set_operator(c, GXcopy);
                mw_canvas_copy(c, win->surface, 0, 0, ox, oy, win->w, win->h);
            }
        } else if (win->background_pixmap != None &&
                   win->background_pixmap != ParentRelative) {
            mw_window_ensure_surface(win);
            if (win->surface) {
                if (win->depth >= 32)
                    mw_canvas_blit_over(c, win->surface, ox, oy, win->w, win->h);
                else {
                    mw_set_operator(c, GXcopy);
                    mw_canvas_copy(c, win->surface, 0, 0, ox, oy, win->w, win->h);
                }
            }
        } else if (win->background_pixmap == None && win->have_background) {
            /* Lazy backing store: solid-background containers paint directly
             * without allocating an offscreen MwSurface. */
            uint32_t argb = win->depth >= 32
                ? (uint32_t)win->background_pixel
                : (0xff000000u | (uint32_t)(win->background_pixel & 0xffffffu));
            mw_set_operator(c, GXcopy);
            mw_set_source_argb(c, argb);
            mw_rect(c, ox, oy, win->w, win->h);
            mw_fill_path(c);
        }
    }

    for (MwWindow *ch = win->children; ch; ch = ch->next_sib) {
        /* A child promoted to its own Wayland surface (popup/toplevel) is
         * presented by the compositor, not composited onto the parent. */
        if (ch->tl) continue;
        if (ch->mapped && !ch->input_only && ch->border_width > 0) {
            int bw = ch->border_width;
            int bx = ox + ch->x, by = oy + ch->y;
            uint32_t bcol = 0xff000000u | (uint32_t)(ch->border_pixel & 0xffffffu);
            mw_set_operator(c, GXcopy);
            mw_set_source_argb(c, bcol);
            mw_rect(c, bx - bw, by - bw, ch->w + 2 * bw, bw);
            mw_rect(c, bx - bw, by + ch->h, ch->w + 2 * bw, bw);
            mw_rect(c, bx - bw, by, bw, ch->h);
            mw_rect(c, bx + ch->w, by, bw, ch->h);
            mw_fill_path(c);
        }
        composite_rec(c, tl, ch, ox + ch->x, oy + ch->y, dmg);
    }

    mw_canvas_restore(c);
}

void mw_composite_window(MwToplevel *tl, MwWindow *win, int ox, int oy)
{
    if (!tl || !tl->frame || !win || !win->mapped) return;
    MwCanvas *c = mw_canvas_begin(tl->frame, NULL, 0, 0, 0);
    if (!c) return;
    composite_rec(c, tl, win, ox, oy, NULL);
    mw_canvas_end(c);
}

void mw_toplevel_render(MwToplevel *tl)
{
    if (!tl || !tl->mapped) return;
    if (!tl->configured) { tl->dirty = true; return; }

    /* The output scale can change at runtime (the window moved to another
     * monitor, or the compositor's scale changed): buffers made for the old
     * scale are too small to wrap at the new one. */
    if (tl->bufs[0].buffer && tl->buf_scale != mw_display_scale(tl->win->d))
        if (buffer_create(tl, tl->width, tl->height) != 0) return;
    int w = tl->width, h = tl->height;
    int tb = tl->tb_h;

    /* Double buffering: draw into a buffer the compositor is not reading. */
    int idx = -1;
    for (int i = 0; i < 2; i++)
        if (!tl->bufs[i].busy && tl->bufs[i].buffer) { idx = i; break; }
    if (idx < 0) {
        uint64_t now = (uint64_t)mw_now();
        if (tl->last_commit_ms && now - tl->last_commit_ms < 200) {
            toplevel_mark_dirty_list(tl);
            return;
        }
        idx = (tl->last_idx + 1) & 1;
        if (MW_TRACE()) fprintf(stderr, "MW: buffer REUSE (stalled) -> %d\n", idx);
        if (!tl->bufs[idx].buffer) return;
    }

    /* Cache the MwSurface wrapper on MwBuf across frames of the same size/scale. */
    int sc = mw_display_scale(tl->win->d);
    MwBuf *buf = &tl->bufs[idx];
    if (!buf->frame || buf->frame_w != w || buf->frame_h != h ||
        buf->frame_scale != sc) {
        if (buf->frame) mw_surface_destroy(buf->frame);
        buf->frame = mw_surface_create_for_data_scaled(buf->data, w, h,
                                                       buf->stride, sc);
        buf->frame_w = w;
        buf->frame_h = h;
        buf->frame_scale = sc;
        tl->damage_full = true;
    }
    tl->frame = buf->frame;
    if (!tl->frame) return;

    /* Compute the exact region to paint into this buffer (accounting for
     * double-buffering) and the region to report as damage to the compositor. */
    pixman_region32_t render_dmg, submit_dmg;
    pixman_region32_init(&render_dmg);
    pixman_region32_init(&submit_dmg);
    pixman_box32_t full_box = { 0, 0, w, h };

    bool full = tl->damage_full;
    if (full) {
        pixman_region32_reset(&render_dmg, &full_box);
        pixman_region32_reset(&submit_dmg, &full_box);
        /* The other buffer will also need a full repaint next time it is used. */
        pixman_region32_reset(&tl->buf_damage[1 - idx], &full_box);
        tl->damage_full = false;
    } else {
        pixman_region32_copy(&render_dmg, &tl->buf_damage[idx]);
        pixman_region32_intersect_rect(&render_dmg, &render_dmg, 0, 0,
                                       (unsigned)w, (unsigned)h);
        pixman_region32_copy(&submit_dmg, &tl->damage);
        pixman_region32_intersect_rect(&submit_dmg, &submit_dmg, 0, 0,
                                       (unsigned)w, (unsigned)h);
    }
    pixman_region32_clear(&tl->buf_damage[idx]);
    pixman_region32_clear(&tl->damage);

    uint32_t bg;
    if (tl->win->depth >= 32)
        bg = tl->win->have_background
                 ? (uint32_t)tl->win->background_pixel
                 : 0x00000000u;
    else
        bg = 0xff000000u | (uint32_t)(tl->win->background_pixel & 0xffffff);

    int nd = 0;
    pixman_box32_t *db = pixman_region32_rectangles(&render_dmg, &nd);
    if (full) {
        mw_surface_clear(tl->frame, bg);
        MwCanvas *c = mw_canvas_begin(tl->frame, NULL, 0, 0, 0);
        if (c) {
            composite_rec(c, tl, tl->win, 0, tb, NULL);
            mw_canvas_end(c);
        }
    } else if (nd > 0) {
        XRectangle stack_r[16];
        XRectangle *r = nd <= 16 ? stack_r : malloc((size_t)nd * sizeof *r);
        if (r) {
            for (int i = 0; i < nd; i++) {
                r[i].x = (short)db[i].x1;
                r[i].y = (short)db[i].y1;
                r[i].width  = (unsigned short)(db[i].x2 - db[i].x1);
                r[i].height = (unsigned short)(db[i].y2 - db[i].y1);
            }
            MwCanvas *c = mw_canvas_begin(tl->frame, r, nd, 0, 0);
            if (c) {
                mw_set_operator(c, GXcopy);
                mw_set_source_argb(c, bg);
                mw_paint(c);
                composite_rec(c, tl, tl->win, 0, tb, &render_dmg);
                mw_canvas_end(c);
            }
            if (r != stack_r) free(r);
        }
    }

    /* An active over-the-spot XIM preedit is painted on top of the composited
     * client content, so it is redrawn on every repaint (xim.c). */
    mw_xim_overlay(tl->win->d, tl);
    if (tl->csd) {
        pixman_box32_t tb_box = { 0, 0, w, tb };
        if (full || pixman_region32_contains_rectangle(&render_dmg, &tb_box) != PIXMAN_REGION_OUT)
            draw_titlebar(tl);
        if (tl->xdg_surface)
            xdg_surface_set_window_geometry(tl->xdg_surface, 0, tb, w, h - tb);
    }

    static int dump_n;
    if (MW_TRACE_DUMP() && !tl->is_popup) {
        char rdir[200];
        const char *dir = getenv("MW_DUMP_DIR");
        if (!dir || !*dir)
            dir = mw_runtime_dir(rdir, sizeof rdir) ? rdir : NULL;
        if (dir && *dir) {
            char p[256];
            int wlen = snprintf(p, sizeof p, "%s/mwroll-%02d.png", dir, (dump_n++) % 16);
            if (wlen > 0 && (size_t)wlen < sizeof p)
                mw_surface_write_png(tl->frame, p);
        }
    }
    if (MW_TRACE())
        fprintf(stderr, "MW: render 0x%lx %dx%d buf=%d full=%d\n",
                tl->win->id, w, h, idx, full);

    mw_toplevel_request_frame(tl);
    if (tl->ack_serial && tl->xdg_surface) {
        if (MW_TRACE())
            fprintf(stderr, "MW: ack_configure xs=%p serial=%u win=0x%lx\n",
                    (void *)tl->xdg_surface, tl->ack_serial,
                    (unsigned long)tl->win->id);
        xdg_surface_ack_configure(tl->xdg_surface, tl->ack_serial);
        tl->ack_serial = 0;
    }

    uint32_t sv = proxy_version(tl->surface);
    if (sv >= 3) wl_surface_set_buffer_scale(tl->surface, sc);
    wl_surface_attach(tl->surface, tl->bufs[idx].buffer, 0, 0);
    int ns = 0;
    pixman_box32_t *sb = pixman_region32_rectangles(&submit_dmg, &ns);
    if (ns == 0) {
        if (sv >= 4) wl_surface_damage_buffer(tl->surface, 0, 0, w * sc, h * sc);
        else         wl_surface_damage(tl->surface, 0, 0, w, h);
    } else {
        for (int i = 0; i < ns; i++) {
            int dx = sb[i].x1, dy = sb[i].y1;
            int dw = sb[i].x2 - sb[i].x1, dh = sb[i].y2 - sb[i].y1;
            if (sv >= 4) wl_surface_damage_buffer(tl->surface, dx * sc, dy * sc,
                                                  dw * sc, dh * sc);
            else         wl_surface_damage(tl->surface, dx, dy, dw, dh);
        }
    }
    wl_surface_commit(tl->surface);

    pixman_region32_fini(&render_dmg);
    pixman_region32_fini(&submit_dmg);

    uint64_t now = (uint64_t)mw_now();
    tl->bufs[idx].busy = true;
    tl->last_idx = idx;
    tl->last_commit_ms = now;
    tl->last_flush_ms = now;
    tl->dirty = false;
    tl->frame = NULL;

    Display *d = tl->win->d;
    if (d) {
        MWD(d)->last_flush_ms = now;
        if (MWD(d)->wl_display) wl_display_flush(MWD(d)->wl_display);
    }
}

void mw_flush_damage(Display *d)
{
    XDisplayImpl *dp = MWD(d);
    for (MwToplevel *tl = dp->dirty_toplevels; tl; tl = tl->dirty_next)
        if (tl->dirty) mw_toplevel_render(tl);
}

void mw_flush_damage_deferred(Display *d)
{
    XDisplayImpl *dp = MWD(d);
    if (!dp->table) return;
    uint64_t now = (uint64_t)mw_now();
    uint64_t frame_ms = (dp->output_refresh_mhz > 0)
        ? (uint64_t)MW_MAX(4, 1000000 / dp->output_refresh_mhz)
        : 16u;
    bool flushed = false;
    for (MwToplevel *tl = dp->dirty_toplevels; tl; tl = tl->dirty_next) {
        if (!tl->dirty) continue;
        if (tl->last_flush_ms && now - tl->last_flush_ms < frame_ms) {
            /* Too soon to composite this toplevel again.  If no frame callback
             * is already in flight, request one and commit so it takes effect. */
            if (!tl->frame_cb && tl->surface && tl->mapped && tl->configured) {
                mw_toplevel_request_frame(tl);
                wl_surface_commit(tl->surface);
                flushed = true;
            }
            continue;
        }
        mw_toplevel_render(tl);
    }
    if (flushed && dp->wl_display) wl_display_flush(dp->wl_display);
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
    (void)t;
    MwToplevel *tl = data;
    /* Track the states the compositor reports (a maximize from the window
     * chrome does not go through the client's X request) and mirror them into
     * the window's _NET_WM_STATE property, which is what GDK and the
     * application read. */
    bool maximized = false, fullscreen = false;
    uint32_t *s;
    wl_array_for_each(s, states) {
        if (*s == XDG_TOPLEVEL_STATE_MAXIMIZED)  maximized = true;
        else if (*s == XDG_TOPLEVEL_STATE_FULLSCREEN) fullscreen = true;
    }
    if (w > 0) tl->req_w = w;
    if (h > 0) tl->req_h = h;
    if (!tl->is_popup && tl->win &&
        (maximized != tl->maximized || fullscreen != tl->fullscreen)) {
        tl->maximized = maximized;
        tl->fullscreen = fullscreen;
        toplevel_sync_net_wm_state(tl->win->d, tl->win);
    }
    /* The window is now managed, so relay _MOTIF_WM_HINTS.  The WM may not
     * have registered the window's title yet; the helper retries briefly. */
    if (!tl->motif_functions_sent && tl->win)
        mw_apply_motif_functions(tl->win->d, tl->win);
    mw_toplevel_damage(tl);
}

static void tl_close(void *data, struct xdg_toplevel *t)
{
    (void)t;
    MwToplevel *tl = (MwToplevel *)data;
    if (tl && tl->win && mw_motif_no_close(tl->win->d, tl->win->id))
        return;   /* the client disabled the close function */
    mw_toplevel_close(tl);
}

static void tl_configure_bounds(void *data, struct xdg_toplevel *t,
                                int32_t w, int32_t h)
{ (void)data; (void)t; (void)w; (void)h; }
static void tl_wm_capabilities(void *data, struct xdg_toplevel *t, struct wl_array *c)
{ (void)data; (void)t; (void)c; }
