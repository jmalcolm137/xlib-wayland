/* drag_target_wl.c — a Wayland drop target for the XDND X->Wayland path.
 *
 * Maps an xdg_toplevel (so the compositor can route a drag to it), accepts the
 * first offered MIME on data_device.enter, and on drop receives the data and
 * prints "WLDRAG:GOT <data>".  The drag source is an X client behind the shim,
 * so this proves the whole chain: X drag -> shim Wayland drag -> this target ->
 * XConvertSelection back to the X source.
 */
#include <wayland-client.h>
#include <xdg-shell-client-protocol.h>

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>

static struct wl_compositor *compositor;
static struct xdg_wm_base *wm_base;
static struct wl_shm *shm;
static struct wl_seat *seat;
static struct wl_data_device_manager *dd_mgr;

static void ptr_enter(void *d, struct wl_pointer *p, uint32_t s, struct wl_surface *surf, wl_fixed_t x, wl_fixed_t y)
{ (void)d; (void)p; (void)s; (void)surf; (void)x; (void)y; }
static void ptr_leave(void *d, struct wl_pointer *p, uint32_t s, struct wl_surface *surf)
{ (void)d; (void)p; (void)s; (void)surf; }
static void ptr_motion(void *d, struct wl_pointer *p, uint32_t t, wl_fixed_t x, wl_fixed_t y)
{ (void)d; (void)p; (void)t; (void)x; (void)y; }
static void ptr_button(void *d, struct wl_pointer *p, uint32_t s, uint32_t t, uint32_t b, uint32_t st)
{ (void)d; (void)p; (void)s; (void)t; (void)b; (void)st; }
static void ptr_axis(void *d, struct wl_pointer *p, uint32_t t, uint32_t a, wl_fixed_t v)
{ (void)d; (void)p; (void)t; (void)a; (void)v; }
static void ptr_frame(void *d, struct wl_pointer *p) { (void)d; (void)p; }
static void ptr_axis_source(void *d, struct wl_pointer *p, uint32_t s) { (void)d; (void)p; (void)s; }
static void ptr_axis_stop(void *d, struct wl_pointer *p, uint32_t t, uint32_t a)
{ (void)d; (void)p; (void)t; (void)a; }
static void ptr_axis_discrete(void *d, struct wl_pointer *p, uint32_t a, int32_t v)
{ (void)d; (void)p; (void)a; (void)v; }
static const struct wl_pointer_listener ptr_listener = {
    .enter = ptr_enter, .leave = ptr_leave, .motion = ptr_motion,
    .button = ptr_button, .axis = ptr_axis, .frame = ptr_frame,
    .axis_source = ptr_axis_source, .axis_stop = ptr_axis_stop,
    .axis_discrete = ptr_axis_discrete,
};

static struct wl_data_offer *offer;
static char *offer_mime;
static int drop_read_fd = -1;

static struct wl_surface *g_surf;
static struct wl_buffer *g_buf;
static int g_attached;
static struct wl_display *g_disp;

static void reg_global(void *data, struct wl_registry *reg, uint32_t name,
                       const char *iface, uint32_t ver)
{
    (void)data;
    if (!strcmp(iface, wl_compositor_interface.name))
        compositor = wl_registry_bind(reg, name, &wl_compositor_interface, ver < 4 ? ver : 4);
    else if (!strcmp(iface, wl_shm_interface.name))
        shm = wl_registry_bind(reg, name, &wl_shm_interface, 1);
    else if (!strcmp(iface, wl_seat_interface.name))
        seat = wl_registry_bind(reg, name, &wl_seat_interface, ver < 7 ? ver : 7);
    else if (!strcmp(iface, wl_data_device_manager_interface.name))
        dd_mgr = wl_registry_bind(reg, name, &wl_data_device_manager_interface, ver < 3 ? ver : 3);
    else if (!strcmp(iface, xdg_wm_base_interface.name))
        wm_base = wl_registry_bind(reg, name, &xdg_wm_base_interface, ver < 6 ? ver : 6);
}
static void reg_global_remove(void *d, struct wl_registry *r, uint32_t n)
{ (void)d; (void)r; (void)n; }
static const struct wl_registry_listener reg_listener = { reg_global, reg_global_remove };

static void wm_ping(void *d, struct xdg_wm_base *b, uint32_t serial)
{ (void)d; xdg_wm_base_pong(b, serial); }
static const struct xdg_wm_base_listener wm_listener = { .ping = wm_ping };

static void offer_ev(void *data, struct wl_data_offer *o, const char *mime)
{
    (void)data; (void)o;
    if (!offer_mime) offer_mime = strdup(mime);
}
static const struct wl_data_offer_listener offer_listener = { .offer = offer_ev };

static void dd_data_offer(void *data, struct wl_data_device *dd, struct wl_data_offer *o)
{
    (void)data; (void)dd;
    offer = o;
    wl_data_offer_add_listener(o, &offer_listener, NULL);
}
static void dd_enter(void *data, struct wl_data_device *dd, uint32_t serial,
                     struct wl_surface *s, wl_fixed_t x, wl_fixed_t y,
                     struct wl_data_offer *o)
{
    (void)data; (void)dd; (void)s; (void)x; (void)y; (void)o;
    if (offer && offer_mime) wl_data_offer_accept(offer, serial, offer_mime);
}
static void dd_leave(void *d, struct wl_data_device *dd) { (void)d; (void)dd; }
static void dd_motion(void *d, struct wl_data_device *dd, uint32_t t, wl_fixed_t x, wl_fixed_t y)
{ (void)d; (void)dd; (void)t; (void)x; (void)y; }
static void dd_drop(void *data, struct wl_data_device *dd)
{
    (void)data; (void)dd;
    if (!offer || !offer_mime) { printf("WLDRAG:NONE\n"); fflush(stdout); return; }
    int fds[2];
    if (pipe(fds) != 0) { printf("WLDRAG:NONE\n"); fflush(stdout); return; }
    drop_read_fd = fds[0];
    wl_data_offer_receive(offer, offer_mime, fds[1]);
    /* The fd is sent with the request; flush before closing the write end, or
     * the sendmsg would reference a closed descriptor. */
    wl_display_flush(g_disp);
    close(fds[1]);
}
static void dd_selection(void *d, struct wl_data_device *dd, struct wl_data_offer *o)
{ (void)d; (void)dd; (void)o; }
static const struct wl_data_device_listener dd_listener = {
    .data_offer = dd_data_offer, .enter = dd_enter, .leave = dd_leave,
    .motion = dd_motion, .drop = dd_drop, .selection = dd_selection,
};

static void xs_configure(void *data, struct xdg_surface *xs, uint32_t serial)
{
    (void)data;
    xdg_surface_ack_configure(xs, serial);
    if (!g_attached && g_buf && g_surf) {
        wl_surface_attach(g_surf, g_buf, 0, 0);
        wl_surface_damage(g_surf, 0, 0, 200, 150);
        wl_surface_commit(g_surf);
        g_attached = 1;
    }
}
static const struct xdg_surface_listener xs_listener = { .configure = xs_configure };

static int make_shm_file(int size)
{
    char name[] = "/wl-drag-XXXXXX";
    int fd = shm_open(name, O_RDWR | O_CREAT | O_EXCL, 0600);
    if (fd < 0) return -1;
    shm_unlink(name);
    if (ftruncate(fd, size) < 0) { close(fd); return -1; }
    return fd;
}

int main(void)
{
    setbuf(stdout, NULL);
    struct wl_display *disp = wl_display_connect(NULL);
    if (!disp) { fprintf(stderr, "drag_target_wl: no display\n"); return 2; }
    g_disp = disp;
    struct wl_registry *reg = wl_display_get_registry(disp);
    wl_registry_add_listener(reg, &reg_listener, NULL);
    wl_display_roundtrip(disp);
    wl_display_roundtrip(disp);
    if (!compositor || !wm_base || !shm || !seat || !dd_mgr) {
        fprintf(stderr, "drag_target_wl: missing globals\n"); return 2;
    }
    xdg_wm_base_add_listener(wm_base, &wm_listener, NULL);

    const int W = 200, H = 150;
    struct wl_surface *surf = wl_compositor_create_surface(compositor);
    struct xdg_surface *xs = xdg_wm_base_get_xdg_surface(wm_base, surf);
    struct xdg_toplevel *tl = xdg_surface_get_toplevel(xs);
    xdg_toplevel_set_title(tl, "drag target");

    int fd = make_shm_file(W * H * 4);
    void *data = mmap(NULL, W * H * 4, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    struct wl_shm_pool *pool = wl_shm_create_pool(shm, fd, W * H * 4);
    g_buf = wl_shm_pool_create_buffer(pool, 0, W, H, W * 4,
                                      WL_SHM_FORMAT_XRGB8888);
    wl_shm_pool_destroy(pool);
    close(fd);
    memset(data, 0x40, W * H * 4);
    xdg_surface_set_window_geometry(xs, 0, 0, W, H);

    g_surf = surf;
    xdg_surface_add_listener(xs, &xs_listener, NULL);

    wl_surface_commit(surf);
    wl_display_roundtrip(disp);

    struct wl_data_device *dd = wl_data_device_manager_get_data_device(dd_mgr, seat);
    wl_data_device_add_listener(dd, &dd_listener, NULL);
    struct wl_pointer *ptr = wl_seat_get_pointer(seat);
    wl_pointer_add_listener(ptr, &ptr_listener, NULL);
    wl_display_flush(disp);
    printf("WLDRAG:READY\n");

    for (int i = 0; i < 250; i++) {
        struct pollfd pfd = { wl_display_get_fd(disp), POLLIN, 0 };
        int pr = poll(&pfd, 1, 40);
        if (pr > 0) {
            if (wl_display_dispatch(disp) < 0) break;
        } else {
            wl_display_dispatch_pending(disp);
        }
        if (drop_read_fd >= 0) {
            struct pollfd rp = { drop_read_fd, POLLIN, 0 };
            if (poll(&rp, 1, 0) > 0) {
                char buf2[4096];
                ssize_t n = read(drop_read_fd, buf2, sizeof buf2 - 1);
                if (n > 0) {
                    buf2[n] = 0;
                    printf("WLDRAG:GOT %s\n", buf2);
                    fflush(stdout);
                    return 0;
                }
            }
        }
    }
    printf("WLDRAG:TIMEOUT\n");
    return 1;
}
