/* clip_wl.c — a Wayland clipboard peer used to test the X<->Wayland bridge.
 *
 *   clip_wl receive        read the compositor clipboard and print GOT:<data>
 *   clip_wl offer <text>   offer <text> on the compositor clipboard, serving
 *                          receive() requests for a short while
 *
 * Links only wayland-client: it is the "other app" (mousepad/Konsole/...).
 */
#include <wayland-client.h>
#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static struct wl_seat *seat;
static struct wl_data_device_manager *mgr;

static struct wl_data_offer *cur_offer;
static char *offer_mime;
static const char *want_mime;   /* receive-mime: which MIME to look for */
static int selected;

static void reg_global(void *data, struct wl_registry *reg, uint32_t name,
                       const char *iface, uint32_t version)
{
    (void)data;
    if (strcmp(iface, wl_seat_interface.name) == 0 && !seat)
        seat = wl_registry_bind(reg, name, &wl_seat_interface,
                                version < 7 ? version : 7);
    else if (strcmp(iface, wl_data_device_manager_interface.name) == 0 && !mgr)
        mgr = wl_registry_bind(reg, name, &wl_data_device_manager_interface,
                               version < 3 ? version : 3);
}

static void reg_global_remove(void *data, struct wl_registry *reg, uint32_t name)
{ (void)data; (void)reg; (void)name; }

static const struct wl_registry_listener reg_listener = {
    .global = reg_global,
    .global_remove = reg_global_remove,
};

static int is_text_mime(const char *m)
{
    return !strncasecmp(m, "text/", 5) || !strcasecmp(m, "UTF8_STRING") ||
           !strcasecmp(m, "STRING") || !strcasecmp(m, "TEXT");
}

static void offer_mime_ev(void *data, struct wl_data_offer *offer, const char *mime)
{
    (void)data; (void)offer;
    if (want_mime) {
        if (!offer_mime && strcasecmp(mime, want_mime) == 0)
            offer_mime = strdup(mime);
    } else if (!offer_mime && is_text_mime(mime)) {
        offer_mime = strdup(mime);
    }
}

static const struct wl_data_offer_listener offer_listener = {
    .offer = offer_mime_ev,
};

static void dd_data_offer(void *data, struct wl_data_device *dd, struct wl_data_offer *id)
{
    (void)data; (void)dd;
    wl_data_offer_add_listener(id, &offer_listener, NULL);
    cur_offer = id;
}

static void dd_selection(void *data, struct wl_data_device *dd, struct wl_data_offer *id)
{
    (void)data; (void)dd;
    selected = 1;
    if (id != cur_offer) { cur_offer = id; offer_mime = NULL; }
}

static void dd_enter(void *d, struct wl_data_device *dd, uint32_t s, struct wl_surface *su,
                     wl_fixed_t x, wl_fixed_t y, struct wl_data_offer *o)
{ (void)d;(void)dd;(void)s;(void)su;(void)x;(void)y;(void)o; }
static void dd_leave(void *d, struct wl_data_device *dd) { (void)d;(void)dd; }
static void dd_motion(void *d, struct wl_data_device *dd, uint32_t t, wl_fixed_t x, wl_fixed_t y)
{ (void)d;(void)dd;(void)t;(void)x;(void)y; }
static void dd_drop(void *d, struct wl_data_device *dd) { (void)d;(void)dd; }

static const struct wl_data_device_listener dd_listener = {
    .data_offer = dd_data_offer,
    .enter = dd_enter,
    .leave = dd_leave,
    .motion = dd_motion,
    .drop = dd_drop,
    .selection = dd_selection,
};

/* ------------------------------------------------------------- offer mode */

static const char *offer_text;
static int served;

static void src_target(void *d, struct wl_data_source *s, const char *m)
{ (void)d;(void)s;(void)m; }

static void src_send(void *d, struct wl_data_source *s, const char *mime, int32_t fd)
{
    (void)d; (void)s; (void)mime;
    size_t len = strlen(offer_text);
    ssize_t off = 0;
    while (off < (ssize_t)len) {
        ssize_t w = write(fd, offer_text + off, (size_t)(len - off));
        if (w <= 0) break;
        off += w;
    }
    close(fd);
    served = 1;
}

static void src_cancelled(void *d, struct wl_data_source *s) { (void)d;(void)s; }
static void src_drop(void *d, struct wl_data_source *s) { (void)d;(void)s; }
static void src_finished(void *d, struct wl_data_source *s) { (void)d;(void)s; }
static void src_action(void *d, struct wl_data_source *s, uint32_t a) { (void)d;(void)s;(void)a; }

static const struct wl_data_source_listener src_listener = {
    .target = src_target,
    .send = src_send,
    .cancelled = src_cancelled,
    .dnd_drop_performed = src_drop,
    .dnd_finished = src_finished,
    .action = src_action,
};

/* ------------------------------------------------------------------ */

static int dispatch_timeout(struct wl_display *disp, int ms)
{
    while (wl_display_prepare_read(disp) != 0) {
        if (wl_display_dispatch_pending(disp) < 0) return -1;
    }
    wl_display_flush(disp);
    struct pollfd pfd = { wl_display_get_fd(disp), POLLIN, 0 };
    int r = poll(&pfd, 1, ms);
    if (r > 0) {
        if (wl_display_read_events(disp) < 0) return -1;
    } else {
        wl_display_cancel_read(disp);
    }
    if (wl_display_dispatch_pending(disp) < 0) return -1;
    return r;
}

int main(int argc, char **argv)
{
    setbuf(stdout, NULL);
    if (argc < 2) { fprintf(stderr, "usage: clip_wl receive|offer <text>\n"); return 2; }

    struct wl_display *disp = wl_display_connect(NULL);
    if (!disp) { fprintf(stderr, "clip_wl: cannot connect\n"); return 2; }
    struct wl_registry *reg = wl_display_get_registry(disp);
    wl_registry_add_listener(reg, &reg_listener, NULL);
    wl_display_roundtrip(disp);
    wl_display_roundtrip(disp);
    if (!seat || !mgr) { fprintf(stderr, "clip_wl: no seat/manager\n"); return 2; }

    struct wl_data_device *dev =
        wl_data_device_manager_get_data_device(mgr, seat);
    wl_data_device_add_listener(dev, &dd_listener, NULL);
    wl_display_roundtrip(disp);

    if (strcmp(argv[1], "receive") == 0 || strcmp(argv[1], "receive-mime") == 0) {
        if (strcmp(argv[1], "receive-mime") == 0 && argc >= 3) want_mime = argv[2];
        /* Wait for the compositor to hand us the clipboard offer. */
        for (int i = 0; i < 200 && !selected; i++)
            dispatch_timeout(disp, 50);
        if (!cur_offer || !offer_mime) { printf("GOT:<none>\n"); return 1; }

        int fds[2];
        if (pipe(fds) != 0) return 1;
        wl_data_offer_receive(cur_offer, offer_mime, fds[1]);
        close(fds[1]);
        wl_display_flush(disp);

        char buf[4096];
        size_t len = 0;
        for (int i = 0; i < 200; i++) {
            struct pollfd pfd = { fds[0], POLLIN, 0 };
            if (poll(&pfd, 1, 50) > 0) {
                ssize_t n = read(fds[0], buf + len, sizeof buf - 1 - len);
                if (n <= 0) break;
                len += (size_t)n;
                if (len >= sizeof buf - 1) break;
            }
        }
        close(fds[0]);
        buf[len] = 0;
        printf("GOT:%s\n", buf);
        return 0;
    }

    if (strcmp(argv[1], "offer") == 0) {
        if (argc < 3) return 2;
        offer_text = argv[2];
        struct wl_data_source *src = wl_data_device_manager_create_data_source(mgr);
        wl_data_source_add_listener(src, &src_listener, NULL);
        wl_data_source_offer(src, "text/plain;charset=utf-8");
        wl_data_source_offer(src, "UTF8_STRING");
        wl_data_source_offer(src, "text/plain");
        wl_data_source_offer(src, "STRING");
        wl_data_device_set_selection(dev, src, 0);
        wl_display_flush(disp);
        printf("OFFERED\n");

        /* Stay alive long enough for the X side to paste (and to answer any
         * repeated requests the toolkit makes). */
        for (int i = 0; i < 200; i++)
            dispatch_timeout(disp, 50);
        return 0;
    }

    if (strcmp(argv[1], "offer-mime") == 0) {
        if (argc < 4) return 2;
        offer_text = argv[3];
        struct wl_data_source *src = wl_data_device_manager_create_data_source(mgr);
        wl_data_source_add_listener(src, &src_listener, NULL);
        wl_data_source_offer(src, argv[2]);
        wl_data_device_set_selection(dev, src, 0);
        wl_display_flush(disp);
        printf("OFFERED\n");
        for (int i = 0; i < 200; i++)
            dispatch_timeout(disp, 50);
        return 0;
    }

    fprintf(stderr, "clip_wl: unknown mode %s\n", argv[1]);
    return 2;
}
