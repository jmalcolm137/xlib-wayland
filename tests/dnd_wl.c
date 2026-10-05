/* dnd_wl.c — a Wayland drag source used to test the Motif DnD bridge.
 *
 * Creates a small toplevel; a left-button press on it starts a drag offering
 * "text/plain;charset=utf-8" with the text given on the command line (default
 * DND-HELLO).  Then a CDE application can be the drop target, exercised with
 * the virtual-pointer tools (VCLICK: press, move, release).
 *
 * Links only wayland-client + the generated xdg-shell protocol: it is the
 * "other app" a real compositor routes a drag from.
 */
#define _GNU_SOURCE
#include <wayland-client.h>
#include <xdg-shell-client-protocol.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/mman.h>

static struct wl_compositor *compositor;
static struct wl_shm *shm;
static struct wl_seat *seat;
static struct wl_pointer *pointer;
static struct xdg_wm_base *wm_base;
static struct wl_data_device_manager *dnd_mgr;
static struct wl_data_device *dnd_dev;

static const char *payload = "DND-HELLO";
static struct wl_display *g_disp;

static void seat_bind(struct wl_seat *s);

static void reg_global(void *data, struct wl_registry *reg, uint32_t name,
                       const char *iface, uint32_t version)
{
    (void)data;
    if (strcmp(iface, wl_compositor_interface.name) == 0)
        compositor = wl_registry_bind(reg, name, &wl_compositor_interface,
                                      version < 4 ? version : 4);
    else if (strcmp(iface, wl_shm_interface.name) == 0)
        shm = wl_registry_bind(reg, name, &wl_shm_interface, 1);
    else if (strcmp(iface, wl_seat_interface.name) == 0 && !seat) {
        seat = wl_registry_bind(reg, name, &wl_seat_interface,
                                version < 7 ? version : 7);
        seat_bind(seat);
    }
    else if (strcmp(iface, xdg_wm_base_interface.name) == 0)
        wm_base = wl_registry_bind(reg, name, &xdg_wm_base_interface,
                                   version < 6 ? version : 6);
    else if (strcmp(iface, wl_data_device_manager_interface.name) == 0 && !dnd_mgr)
        dnd_mgr = wl_registry_bind(reg, name, &wl_data_device_manager_interface,
                                   version < 3 ? version : 3);
}
static void reg_remove(void *data, struct wl_registry *reg, uint32_t name)
{ (void)data;(void)reg;(void)name; }
static const struct wl_registry_listener reg_listener = { reg_global, reg_remove };

/* ----------------------------------------------------------- data source */

static void src_target(void *d, struct wl_data_source *s, const char *m)
{ (void)d;(void)s; fprintf(stderr, "dnd_wl: target %s\n", m); }

static void src_send(void *d, struct wl_data_source *s, const char *mime, int32_t fd)
{
    (void)d; (void)s;
    fprintf(stderr, "dnd_wl: send %s\n", mime);
    size_t len = strlen(payload), off = 0;
    while (off < len) {
        ssize_t w = write(fd, payload + off, len - off);
        if (w <= 0) break;
        off += (size_t)w;
    }
    close(fd);
}

static void src_cancelled(void *d, struct wl_data_source *s)
{ (void)d;(void)s; fprintf(stderr, "dnd_wl: cancelled\n"); }
static void src_drop(void *d, struct wl_data_source *s)
{ (void)d;(void)s; fprintf(stderr, "dnd_wl: drop-performed\n"); }
static void src_finished(void *d, struct wl_data_source *s)
{ (void)d;(void)s; fprintf(stderr, "dnd_wl: finished\n"); }
static void src_action(void *d, struct wl_data_source *s, uint32_t a)
{ (void)d;(void)s; fprintf(stderr, "dnd_wl: action %u\n", a); }

static const struct wl_data_source_listener src_listener = {
    .target = src_target, .send = src_send, .cancelled = src_cancelled,
    .dnd_drop_performed = src_drop, .dnd_finished = src_finished,
    .action = src_action,
};

/* --------------------------------------------------------------- pointer */

static struct wl_surface *surface;

static void ptr_enter(void *d, struct wl_pointer *p, uint32_t serial,
                      struct wl_surface *s, wl_fixed_t x, wl_fixed_t y)
{ (void)d;(void)p;(void)serial;(void)s;(void)x;(void)y;
  fprintf(stderr, "dnd_wl: pointer enter\n"); }
static void ptr_leave(void *d, struct wl_pointer *p, uint32_t serial,
                      struct wl_surface *s)
{ (void)d;(void)p;(void)serial;(void)s; }
static void ptr_motion(void *d, struct wl_pointer *p, uint32_t t, wl_fixed_t x, wl_fixed_t y)
{ (void)d;(void)p;(void)t;(void)x;(void)y; }

static void ptr_button(void *d, struct wl_pointer *p, uint32_t serial,
                       uint32_t time, uint32_t button, uint32_t state)
{
    (void)d; (void)p; (void)time;
    fprintf(stderr, "dnd_wl: button 0x%x state %u\n", button, state);
    if (button != 0x110 /* BTN_LEFT */ || state != WL_POINTER_BUTTON_STATE_PRESSED)
        return;
    if (!dnd_mgr) { fprintf(stderr, "dnd_wl: no dnd manager\n"); return; }

    struct wl_data_source *src = wl_data_device_manager_create_data_source(dnd_mgr);
    wl_data_source_add_listener(src, &src_listener, NULL);
    wl_data_source_offer(src, "text/plain;charset=utf-8");
    wl_data_source_offer(src, "text/plain");
    wl_data_source_offer(src, "UTF8_STRING");
    wl_data_source_offer(src, "STRING");
    wl_data_source_set_actions(src,
        WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY |
        WL_DATA_DEVICE_MANAGER_DND_ACTION_MOVE);
    wl_data_device_start_drag(dnd_dev, src, surface, NULL, serial);
    wl_display_flush(g_disp);
    fprintf(stderr, "dnd_wl: drag started (serial %u)\n", serial);
}

static void ptr_axis(void *d, struct wl_pointer *p, uint32_t t, uint32_t a, wl_fixed_t v)
{ (void)d;(void)p;(void)t;(void)a;(void)v; }
static void ptr_frame(void *d, struct wl_pointer *p) { (void)d;(void)p; }
static void ptr_axis_source(void *d, struct wl_pointer *p, uint32_t s) { (void)d;(void)p;(void)s; }
static void ptr_axis_stop(void *d, struct wl_pointer *p, uint32_t t, uint32_t a) { (void)d;(void)p;(void)t;(void)a; }
static void ptr_axis_discrete(void *d, struct wl_pointer *p, uint32_t a, int32_t v) { (void)d;(void)p;(void)a;(void)v; }

static const struct wl_pointer_listener ptr_listener = {
    .enter = ptr_enter, .leave = ptr_leave, .motion = ptr_motion,
    .button = ptr_button, .axis = ptr_axis, .frame = ptr_frame,
    .axis_source = ptr_axis_source, .axis_stop = ptr_axis_stop,
    .axis_discrete = ptr_axis_discrete,
};

static void seat_caps(void *d, struct wl_seat *s, uint32_t caps)
{
    (void)d;
    if ((caps & WL_SEAT_CAPABILITY_POINTER) && !pointer) {
        pointer = wl_seat_get_pointer(s);
        wl_pointer_add_listener(pointer, &ptr_listener, NULL);
    }
}
static void seat_name(void *d, struct wl_seat *s, const char *n) { (void)d;(void)s;(void)n; }
static const struct wl_seat_listener seat_listener = { seat_caps, seat_name };

static void seat_bind(struct wl_seat *s)
{
    wl_seat_add_listener(s, &seat_listener, NULL);
}

/* ------------------------------------------------------------ xdg shell */

static void wm_ping(void *d, struct xdg_wm_base *b, uint32_t serial)
{ (void)d; xdg_wm_base_pong(b, serial); }
static const struct xdg_wm_base_listener wm_listener = { wm_ping };

static int configured;
static void xdg_surf_configure(void *d, struct xdg_surface *s, uint32_t serial)
{ (void)d; xdg_surface_ack_configure(s, serial); configured = 1; }
static const struct xdg_surface_listener xdg_surf_listener = { xdg_surf_configure };

static void xdg_top_configure(void *d, struct xdg_toplevel *t, int32_t w, int32_t h,
                              struct wl_array *states)
{ (void)d;(void)t;(void)w;(void)h;(void)states; }
static void xdg_top_close(void *d, struct xdg_toplevel *t) { (void)d;(void)t; exit(0); }
static void xdg_top_bounds(void *d, struct xdg_toplevel *t, int32_t w, int32_t h)
{ (void)d;(void)t;(void)w;(void)h; }
static void xdg_top_caps(void *d, struct xdg_toplevel *t, struct wl_array *c)
{ (void)d;(void)t;(void)c; }
static const struct xdg_toplevel_listener xdg_top_listener = {
    .configure = xdg_top_configure, .close = xdg_top_close,
    .configure_bounds = xdg_top_bounds, .wm_capabilities = xdg_top_caps,
};

/* A shm buffer so the surface is actually mapped and receives pointer events. */
static struct wl_buffer *make_buffer(int w, int h)
{
    int stride = w * 4, size = stride * h;
    int fd = memfd_create("dnd_wl", MFD_CLOEXEC);
    if (fd < 0) return NULL;
    if (ftruncate(fd, size) != 0) { close(fd); return NULL; }
    void *data = mmap(NULL, (size_t)size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (data == MAP_FAILED) { close(fd); return NULL; }
    uint32_t *px = data;
    for (int i = 0; i < w * h; i++) px[i] = 0xff3355cc;
    struct wl_shm_pool *pool = wl_shm_create_pool(shm, fd, size);
    struct wl_buffer *buf = wl_shm_pool_create_buffer(pool, 0, w, h, stride,
                                                      WL_SHM_FORMAT_XRGB8888);
    wl_shm_pool_destroy(pool);
    close(fd);
    return buf;
}

int main(int argc, char **argv)
{
    setbuf(stderr, NULL);
    if (argc > 1) payload = argv[1];

    struct wl_display *disp = wl_display_connect(NULL);
    if (!disp) { fprintf(stderr, "dnd_wl: cannot connect\n"); return 2; }
    g_disp = disp;
    struct wl_registry *reg = wl_display_get_registry(disp);
    wl_registry_add_listener(reg, &reg_listener, NULL);
    wl_display_roundtrip(disp);
    wl_display_roundtrip(disp);
    if (!compositor || !wm_base) { fprintf(stderr, "dnd_wl: no compositor/shell\n"); return 2; }
    if (seat) { wl_seat_add_listener(seat, &seat_listener, NULL); wl_display_roundtrip(disp); }
    if (dnd_mgr)
        dnd_dev = wl_data_device_manager_get_data_device(dnd_mgr, seat);
    xdg_wm_base_add_listener(wm_base, &wm_listener, NULL);

    surface = wl_compositor_create_surface(compositor);
    struct xdg_surface *xs = xdg_wm_base_get_xdg_surface(wm_base, surface);
    xdg_surface_add_listener(xs, &xdg_surf_listener, NULL);
    struct xdg_toplevel *top = xdg_surface_get_toplevel(xs);
    xdg_toplevel_add_listener(top, &xdg_top_listener, NULL);
    xdg_toplevel_set_title(top, "dnd_wl");
    xdg_toplevel_set_app_id(top, "dnd_wl");
    wl_surface_commit(surface);
    while (!configured) {
        if (wl_display_dispatch(disp) < 0) return 2;
    }
    if (shm) {
        struct wl_buffer *buf = make_buffer(240, 140);
        if (buf) {
            wl_surface_attach(surface, buf, 0, 0);
            wl_surface_damage_buffer(surface, 0, 0, 240, 140);
            wl_surface_commit(surface);
        }
    }
    wl_display_roundtrip(disp);
    fprintf(stderr, "dnd_wl: ready (payload \"%s\")\n", payload);

    for (;;) {
        if (wl_display_dispatch(disp) < 0) break;
    }
    return 0;
}
