/* wl.c — Wayland connection, registry and global binding. */
#include "internal.h"

#include <wayland-client.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <poll.h>

static void registry_global(void *data, struct wl_registry *reg, uint32_t name,
                            const char *interface, uint32_t version)
{
    Display *d = data;
    XDisplayImpl *dp = MWD(d);

    if (strcmp(interface, wl_compositor_interface.name) == 0) {
        uint32_t v = version < 5 ? version : 5;
        dp->wl_compositor = wl_registry_bind(reg, name, &wl_compositor_interface, v);
    } else if (strcmp(interface, wl_shm_interface.name) == 0) {
        dp->wl_shm = wl_registry_bind(reg, name, &wl_shm_interface, 1);
    } else if (strcmp(interface, xdg_wm_base_interface.name) == 0) {
        uint32_t v = version < 6 ? version : 6;
        dp->wm_base = wl_registry_bind(reg, name, &xdg_wm_base_interface, v);
    } else if (strcmp(interface, wl_seat_interface.name) == 0 &&
               dp->wl_seat == NULL) {
        uint32_t v = version < 7 ? version : 7;
        dp->wl_seat = wl_registry_bind(reg, name, &wl_seat_interface, v);
        /* The compositor sends wl_seat.capabilities in response to the bind,
         * during the next roundtrip.  The listener must be installed *now* or
         * the capabilities event is missed and we never bind wl_pointer or
         * wl_keyboard (no input at all). */
        mw_input_init((Display *)d, dp->wl_seat);
    } else if (strcmp(interface, wl_output_interface.name) == 0 &&
               dp->wl_output == NULL) {
        uint32_t v = version < 3 ? version : 3;
        dp->wl_output = wl_registry_bind(reg, name, &wl_output_interface, v);
    } else if (strcmp(interface, zxdg_decoration_manager_v1_interface.name) == 0) {
        dp->deco_mgr = wl_registry_bind(reg, name,
                          &zxdg_decoration_manager_v1_interface, 1);
    } else if (strcmp(interface, wp_viewporter_interface.name) == 0) {
        dp->viewporter = wl_registry_bind(reg, name,
                          &wp_viewporter_interface, version < 1 ? version : 1);
    } else if (strcmp(interface, wl_data_device_manager_interface.name) == 0) {
        dp->dnd_mgr = wl_registry_bind(reg, name,
                          &wl_data_device_manager_interface, version < 3 ? version : 3);
    }
}

static void registry_global_remove(void *data, struct wl_registry *reg, uint32_t name)
{
    (void)data; (void)reg; (void)name;
}

static const struct wl_registry_listener registry_listener = {
    .global = registry_global,
    .global_remove = registry_global_remove,
};

static void wm_base_ping(void *data, struct xdg_wm_base *base, uint32_t serial)
{
    (void)data;
    xdg_wm_base_pong(base, serial);
}

static const struct xdg_wm_base_listener wm_base_listener = {
    .ping = wm_base_ping,
};

/* ---------------------------------------------------------------- output */

static void output_geometry(void *data, struct wl_output *o, int32_t x, int32_t y,
                            int32_t pw, int32_t ph, int32_t subpixel,
                            const char *make, const char *model, int32_t transform)
{ (void)data;(void)o;(void)x;(void)y;(void)pw;(void)ph;(void)subpixel;(void)make;(void)model;(void)transform; }

static void output_mode(void *data, struct wl_output *o, uint32_t flags,
                        int32_t width, int32_t height, int32_t refresh)
{
    (void)o; (void)refresh;
    Display *d = data;
    XDisplayImpl *dp = MWD(d);
    if (flags & WL_OUTPUT_MODE_CURRENT) {
        dp->output_w = width;
        dp->output_h = height;
        if (dp->screens) {
            dp->screens[0].width = width;
            dp->screens[0].height = height;
        }
    }
}
static void output_done(void *data, struct wl_output *o) { (void)data;(void)o; }
static void output_scale(void *data, struct wl_output *o, int32_t f) { (void)data;(void)o;(void)f; }
static void output_name(void *data, struct wl_output *o, const char *n) { (void)data;(void)o;(void)n; }
static void output_description(void *data, struct wl_output *o, const char *n) { (void)data;(void)o;(void)n; }

static const struct wl_output_listener output_listener = {
    .geometry = output_geometry,
    .mode = output_mode,
    .done = output_done,
    .scale = output_scale,
    .name = output_name,
    .description = output_description,
};

int mw_wl_connect(XDisplayImpl *dp, const char *name)
{
    struct wl_display *disp = wl_display_connect(name);
    if (!disp) return -1;
    dp->wl_display = disp;
    dp->fd = wl_display_get_fd(disp);

    dp->wl_registry = wl_display_get_registry(disp);
    wl_registry_add_listener(dp->wl_registry, &registry_listener, (Display *)dp);
    wl_display_roundtrip(disp);
    wl_display_roundtrip(disp);   /* second pass for output modes etc. */

    if (!dp->wl_compositor || !dp->wl_shm || !dp->wm_base) {
        wl_display_disconnect(disp);
        dp->wl_display = NULL;
        return -1;
    }
    xdg_wm_base_add_listener(dp->wm_base, &wm_base_listener, (Display *)dp);

    if (dp->wl_output) {
        wl_output_add_listener(dp->wl_output, &output_listener, (Display *)dp);
        wl_display_roundtrip(disp);
    }

    return 0;
}

void mw_wl_disconnect(XDisplayImpl *dp)
{
    if (!dp->wl_display) return;
    mw_clipboard_fini((Display *)dp);
    if (dp->wl_seat) mw_input_fini((Display *)dp);
    if (dp->data_device) wl_data_device_destroy(dp->data_device);
    if (dp->dnd_mgr) wl_data_device_manager_destroy(dp->dnd_mgr);
    if (dp->viewporter) wp_viewporter_destroy(dp->viewporter);
    if (dp->deco_mgr) zxdg_decoration_manager_v1_destroy(dp->deco_mgr);
    if (dp->wl_output) wl_output_destroy(dp->wl_output);
    if (dp->wm_base) xdg_wm_base_destroy(dp->wm_base);
    if (dp->wl_shm) wl_shm_destroy(dp->wl_shm);
    if (dp->wl_compositor) wl_compositor_destroy(dp->wl_compositor);
    if (dp->wl_registry) wl_registry_destroy(dp->wl_registry);
    wl_display_flush(dp->wl_display);
    wl_display_disconnect(dp->wl_display);
    dp->wl_display = NULL;
}

/* Non-blocking drain of the Wayland connection followed by event translation.
 * `block` waits for at least one event. */
/* Block until at least one more Wayland event has been read and dispatched,
 * even when events are already queued.  mw_process_events(d, true) only blocks
 * on an empty queue, which is not enough for the mask-based event selectors. */
void mw_block_for_events(Display *d)
{
    XDisplayImpl *dp = MWD(d);
    if (!dp->wl_display || dp->closed) return;
    /* The client is about to go idle: paint accumulated damage first, but no
     * more than once per display refresh -- the compositor's frame callback
     * re-renders anything that was deferred, so nothing is lost. */
    mw_flush_damage_deferred(d);
    if (getenv("MW_TRACE")) fprintf(stderr, "MW: blocking for events\n");

    /* A held key repeats on its own schedule: deliver any that is due, then
     * wait at most until the next one so it fires even with nothing else
     * happening. */
    mw_kbd_repeat_pump(d);
    int rtimeout = mw_kbd_repeat_timeout(d);

    /* Normally block on the Wayland connection exactly as before.  Only when a
     * clipboard transfer is in flight, or a key repeat is pending, is it worth
     * also watching a timer -- otherwise a paste whose data arrives off the
     * event loop, or a repeat, would wait for an unrelated Wayland event. */
    int cfd = mw_clipboard_poll_fd(d);
    if (cfd < 0 && rtimeout < 0) {
        if (wl_display_dispatch(dp->wl_display) < 0)
            mw_io_error(d, "Wayland connection closed");
        return;
    }

    while (wl_display_prepare_read(dp->wl_display) != 0) {
        if (wl_display_dispatch_pending(dp->wl_display) < 0) {
            if (!dp->closed) mw_io_error(d, "Wayland connection error");
            mw_clipboard_handle_ready(d);
            return;
        }
    }
    wl_display_flush(dp->wl_display);

    struct pollfd pfd[2];
    pfd[0].fd = dp->fd; pfd[0].events = POLLIN; pfd[0].revents = 0;
    pfd[1].fd = cfd;    pfd[1].events = POLLIN; pfd[1].revents = 0;

    int timeout = rtimeout >= 0 ? rtimeout : 100;
    if (cfd >= 0 && timeout > 100) timeout = 100;
    int r = poll(pfd, cfd >= 0 ? 2 : 1, timeout);
    if (r > 0 && pfd[0].revents) {
        if (wl_display_read_events(dp->wl_display) < 0) {
            mw_io_error(d, "Wayland connection closed");
            return;
        }
    } else {
        wl_display_cancel_read(dp->wl_display);
    }
    if (wl_display_dispatch_pending(dp->wl_display) < 0 && !dp->closed)
        mw_io_error(d, "Wayland connection error");
    mw_clipboard_handle_ready(d);
    mw_kbd_repeat_pump(d);
}

void mw_process_events(Display *d, bool block)
{
    XDisplayImpl *dp = MWD(d);
    if (!dp->wl_display || dp->closed) return;

    /* Paint accumulated drawing damage only when the client is about to go
     * idle.  Rendering on every poll (XPending, XEventsQueued, ...) showed
     * half-finished repaints: Motif clears a text field and then redraws it in
     * pieces, so painting in between flashed the field blank or with only part
     * of the line.  Waiting until the client blocks coalesces the whole
     * repaint into one frame, which is what a compositor presents anyway; the
     * deferred variant additionally caps an in-progress repaint to one frame
     * per refresh so a window resize does not composite per drawing step. */
    mw_flush_damage_deferred(d);
    mw_kbd_repeat_pump(d);

    if (block && dp->qcount == 0) {
        mw_block_for_events(d);
        return;
    }

    /* canonical non-blocking read */
    if (wl_display_prepare_read(dp->wl_display) == 0) {
        wl_display_flush(dp->wl_display);
        struct pollfd pfd[2];
        int n = 0;
        pfd[n].fd = dp->fd; pfd[n].events = POLLIN; pfd[n].revents = 0; n++;
        int cfd = mw_clipboard_poll_fd(d);
        if (cfd >= 0) { pfd[n].fd = cfd; pfd[n].events = POLLIN; pfd[n].revents = 0; n++; }
        int r = poll(pfd, n, 0);
        if (r > 0 && pfd[0].revents) {
            if (wl_display_read_events(dp->wl_display) < 0) {
                mw_io_error(d, "Wayland connection closed");
                return;
            }
        } else {
            wl_display_cancel_read(dp->wl_display);
        }
    }
    if (wl_display_dispatch_pending(dp->wl_display) < 0 && !dp->closed)
        mw_io_error(d, "Wayland connection error");
    mw_clipboard_handle_ready(d);
}
