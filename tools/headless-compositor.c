/*
 * headless-compositor.c — a minimal, wlroots-free Wayland compositor.
 *
 * Purpose
 * -------
 * CI needs to exercise the Motif/Wayland Xlib (libX11) backend without a
 * real desktop session.  This program is a single-file compositor built
 * directly on libwayland-server.  It implements just enough of the core
 * protocol for an Xlib backend to connect, create an xdg-shell toplevel,
 * render into a wl_shm buffer, receive synthetic input, and be captured to
 * a PNG for golden-image comparison.
 *
 * Advertised globals
 * ------------------
 *   wl_compositor   v5   create_surface / create_region
 *   wl_shm          v2   create_pool; ARGB8888 + XRGB8888
 *   wl_seat         v7   get_pointer / get_keyboard (+ real xkb keymap)
 *   wl_output       v3   geometry / mode / scale
 *   xdg_wm_base     v1   toplevels are sized to --size (default 800x600)
 *
 * Behaviour
 * ---------
 *   * Prints "READY <socket>" on stdout once the socket is listening.
 *   * After --timeout seconds (default 2), or as soon as a line arrives on
 *     stdin, it captures the largest / most recently mapped toplevel's
 *     attached wl_shm buffer to --output (default frame.png) as PNG and
 *     exits 0.
 *   * --input FILE replays a tiny line-based input script (see usage()).
 *
 * Build
 * -----
 *   wayland-scanner server-header  <xdg-shell.xml> xdg-shell-server.h
 *   wayland-scanner private-code   <xdg-shell.xml> xdg-shell-protocol.c
 *   gcc -std=gnu11 -Wall -o headless-compositor \
 *       headless-compositor.c xdg-shell-protocol.c \
 *       $(pkg-config --cflags --libs wayland-server) \
 *       $(pkg-config --cflags --libs xkbcommon) -lpng -lm
 *
 * The generated header is looked up as "xdg-shell-server.h" first (the name
 * meson would use with an include path), then under <xdg-shell-server.h>,
 * then at the /tmp path used by the build command in the task description.
 */

#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include <png.h>
#include <wayland-server.h>
#include <xkbcommon/xkbcommon.h>

#if defined(__has_include)
#  if __has_include("xdg-shell-server.h")
#    include "xdg-shell-server.h"
#  elif __has_include(<xdg-shell-server.h>)
#    include <xdg-shell-server.h>
#  elif __has_include("/tmp/xdg-shell-server.h")
#    include "/tmp/xdg-shell-server.h"
#  else
#    error "xdg-shell-server.h not found: generate it with wayland-scanner server-header"
#  endif
#else
#  include "xdg-shell-server.h"
#endif

#if defined(__has_include)
#  if __has_include("xdg-decoration-server.h")
#    include "xdg-decoration-server.h"
#  elif __has_include(<xdg-decoration-server.h>)
#    include <xdg-decoration-server.h>
#  endif
#endif

#if defined(__has_include)
#  if __has_include("viewporter-server.h")
#    include "viewporter-server.h"
#  elif __has_include(<viewporter-server.h>)
#    include <viewporter-server.h>
#  endif
#endif

#if defined(__has_include)
#  if __has_include("text-input-unstable-v3-server-protocol.h")
#    include "text-input-unstable-v3-server-protocol.h"
#  elif __has_include(<text-input-unstable-v3-server-protocol.h>)
#    include <text-input-unstable-v3-server-protocol.h>
#  endif
#endif

#if defined(__has_include)
#  if __has_include("primary-selection-unstable-v1-server-protocol.h")
#    include "primary-selection-unstable-v1-server-protocol.h"
#  elif __has_include(<primary-selection-unstable-v1-server-protocol.h>)
#    include <primary-selection-unstable-v1-server-protocol.h>
#  endif
#endif

#if defined(__BYTE_ORDER__) && defined(__ORDER_LITTLE_ENDIAN__) && \
    (__BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__)
#  define MW_LITTLE_ENDIAN 1
#else
#  define MW_LITTLE_ENDIAN 0
#endif

#define DEFAULT_SOCKET   "wayland-mwtest"
#define DEFAULT_WIDTH    800
#define DEFAULT_HEIGHT   600
#define DEFAULT_TIMEOUT  2
#define DEFAULT_OUTPUT   "frame.png"
#define FRAME_INTERVAL_MS 16
#define SCRIPT_START_MS  400
#define SCRIPT_STEP_MS   60
/* How long to keep waiting for the client to map a window before delivering the
 * script anyway, and how often to re-check while waiting. */
#define SCRIPT_BACKSTOP_MS 3000
#define SCRIPT_GATE_POLL_MS 50

#ifndef MIN
#  define MIN(a, b) ((a) < (b) ? (a) : (b))
#endif

struct mw_compositor;
struct mw_surface;
struct mw_xdg_surface;
struct mw_xdg_toplevel;

/* ------------------------------------------------------------------ */
/* Data model                                                          */
/* ------------------------------------------------------------------ */

struct mw_buffer {
	struct wl_resource *resource;
	struct mw_compositor *comp;
	int fd;              /* dup() of the pool fd (the client's fd is closed) */
	int32_t offset;
	int32_t width;
	int32_t height;
	int32_t stride;
	uint32_t format;
};

struct mw_pool {
	struct wl_resource *resource;
	struct mw_compositor *comp;
	int fd;              /* dup() of the received fd */
	int32_t size;
};

struct mw_frame_cb {
	struct wl_resource *resource;
	struct wl_list link;
};

struct mw_surface {
	struct wl_resource *resource;
	struct mw_compositor *comp;
	struct wl_list link;              /* comp->surfaces */

	struct wl_resource *pending_buffer;
	int pending_attach;
	struct wl_resource *buffer;       /* committed buffer */
	int32_t buf_w, buf_h;
	int32_t buffer_scale;             /* wl_surface.set_buffer_scale, >= 1 */

	int32_t geom_x, geom_y, geom_w, geom_h;

	/* wp_viewport: the client may attach a larger buffer and crop it. */
	struct wl_resource *viewport;
	int32_t vp_sx, vp_sy, vp_sw, vp_sh;   /* source rect, buffer coords */
	int32_t vp_dw, vp_dh;                 /* destination size */
	int has_vp_source;

	int is_toplevel;
	int is_popup;
	int mapped;
	int enter_sent;
	uint64_t map_seq;

	struct mw_xdg_surface *xdg_surface;
	struct wl_list frame_callbacks;   /* mw_frame_cb */
};

struct mw_xdg_surface {
	struct wl_resource *resource;
	struct mw_compositor *comp;
	struct mw_surface *surface;
	struct mw_xdg_toplevel *toplevel;
	struct mw_popup *popup;
	/* xdg-shell configure bookkeeping.  A client may only attach a buffer
	 * after it has acked a configure, and committing a NULL buffer puts the
	 * surface back into the "newly unmapped" state, which requires a fresh
	 * commit + configure + ack before the next buffer.  Real compositors
	 * (KWin, wlroots) enforce this and kill the client, so the test
	 * compositor does too -- otherwise these bugs hide until someone runs the
	 * app on a real session. */
	int configured;   /* a configure has been sent */
	int acked;        /* the client acked the latest configure */
};

struct mw_xdg_toplevel {
	struct wl_resource *resource;
	struct mw_compositor *comp;
	struct mw_xdg_surface *xdg_surface;
	char *title;
	char *app_id;
	int configured;
	struct wl_resource *deco;
};

struct mw_popup {
	struct wl_resource *resource;
	struct mw_compositor *comp;
	struct mw_xdg_surface *xdg_surface;
	int32_t w, h;            /* size taken from the positioner */
};

struct mw_positioner {
	int32_t width;
	int32_t height;
};

/* Generic wrapper for the objects we keep in per-type lists. */
struct mw_device {
	struct wl_resource *resource;
	struct wl_list link;
	int entered;
	struct mw_surface *entered_surface;
};

enum mw_action_kind {
	ACT_MOTION,
	ACT_BUTTON,
	ACT_KEY,
	ACT_FRAME,
	ACT_SLEEP,
	ACT_SCREENSHOT,
	ACT_POPUPDONE,
	ACT_RESIZE,
	ACT_AXIS,
	ACT_TOUCH,
};

struct mw_action {
	enum mw_action_kind kind;
	int x, y;
	uint32_t state;   /* pressed/released */
	uint32_t code;    /* button or evdev keycode */
	int value;        /* ACT_AXIS: signed wheel detents */
	int delay_ms;
	int has_xy;       /* motion only: the coordinates are meaningful */
};

struct mw_compositor {
	struct wl_display *display;
	struct wl_event_loop *loop;

	char *socket_name;
	int width;
	int height;
	int scale;                 /* wl_output scale advertised to clients */
	int timeout;
	const char *output_path;
	const char *input_path;

	/* Optional periodic capture: with --capture-prefix P --capture-every MS,
	 * write P0001.png, P0002.png, ... every MS while the run continues, so a
	 * long-lived session can be turned into a frame sequence / animation
	 * instead of a single screenshot. */
	const char *capture_prefix;
	int capture_every_ms;
	int capture_count;
	struct wl_event_source *capture_timer;

	struct wl_list surfaces;          /* mw_surface */
	struct wl_list pointers;          /* mw_device */
	struct wl_list keyboards;         /* mw_device */
	struct wl_list touches;           /* mw_device */
	struct wl_list outputs;           /* mw_device */
	struct wl_list data_devices;      /* hc_data_device */
	struct hc_data_source *data_selection; /* compositor clipboard owner */
	struct wl_list primary_devices;   /* hc_data_device (primary) */
	struct hc_data_source *primary_selection; /* primary selection owner */

	/* Minimal drag-and-drop: the source sets a data source, the pointer
	 * entering a surface gets a drag offer, and a button release drops. */
	struct hc_data_source *drag_source;
	struct mw_surface     *drag_surface;
	struct wl_resource    *drag_device;
	struct wl_resource    *drag_offer;
	int                    drag_active;

	struct mw_surface *last_toplevel; /* last mapped toplevel */
	struct mw_surface *input_surface; /* input focus target */
	struct mw_surface *popup_surface; /* currently mapped xdg_popup, if any */
	int ptr_x, ptr_y;                 /* last motion, for coordinate-less buttons */
	uint64_t map_seq;

	struct xkb_context *xkb_context;
	struct xkb_keymap *xkb_keymap;
	char *xkb_keymap_string;
	size_t xkb_keymap_size;

	struct wl_list pending_frame_cbs; /* mw_frame_cb awaiting done */
	struct wl_event_source *frame_timer;

	struct wl_event_source *timeout_timer;
	int captured;

	struct wl_event_source *stdin_source;

	struct mw_action *actions;
	int action_count;
	int action_index;
	struct wl_event_source *script_timer;   /* drives the actions */
	struct wl_event_source *script_gate;    /* waits for a surface to exist */
	int script_started;
	uint64_t script_gate_start_ms;
};

/* ------------------------------------------------------------------ */
/* Small helpers                                                       */
/* ------------------------------------------------------------------ */

static uint32_t now_ms(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint32_t)((uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u);
}

/*
 * Anonymous, sealable-enough file for the keyboard keymap.  memfd_create is
 * the clean path; shm_open is the fallback when memfd is unavailable.
 */
static int create_anon_file(size_t size)
{
	static unsigned counter;
	int fd = memfd_create("mw-keymap", MFD_CLOEXEC);

	if (fd < 0) {
		char name[64];
		snprintf(name, sizeof name, "/mw-keymap-%d-%u", (int)getpid(), counter++);
		fd = shm_open(name, O_RDWR | O_CREAT | O_EXCL, 0600);
		if (fd >= 0)
			shm_unlink(name);
	}
	if (fd < 0)
		return -1;
	if (ftruncate(fd, (off_t)size) < 0) {
		close(fd);
		return -1;
	}
	return fd;
}

static struct mw_device *device_for_client(struct wl_list *list,
					   struct wl_client *client)
{
	struct mw_device *dev;

	wl_list_for_each(dev, list, link) {
		if (wl_resource_get_client(dev->resource) == client)
			return dev;
	}
	return NULL;
}

static struct mw_device *first_device(struct wl_list *list)
{
	struct mw_device *dev;

	wl_list_for_each(dev, list, link)
		return dev;
	return NULL;
}

static struct wl_resource *output_for_client(struct mw_compositor *comp,
					     struct wl_client *client)
{
	struct mw_device *dev = device_for_client(&comp->outputs, client);
	return dev ? dev->resource : NULL;
}

/* ------------------------------------------------------------------ */
/* PNG output                                                          */
/* ------------------------------------------------------------------ */

static int write_png(const char *path, const uint8_t *rgba, int w, int h)
{
	FILE *fp;
	png_structp png;
	png_infop info;
	png_bytep *rows;
	int y, rc = -1;

	if (w <= 0 || h <= 0)
		return -1;

	fp = fopen(path, "wb");
	if (!fp) {
		fprintf(stderr, "headless-compositor: cannot open %s: %s\n",
			path, strerror(errno));
		return -1;
	}
	png = png_create_write_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
	if (!png)
		goto out_fp;
	info = png_create_info_struct(png);
	if (!info)
		goto out_png;
	if (setjmp(png_jmpbuf(png)))
		goto out_info;

	png_init_io(png, fp);
	png_set_IHDR(png, info, (png_uint_32)w, (png_uint_32)h, 8,
		     PNG_COLOR_TYPE_RGBA, PNG_INTERLACE_NONE,
		     PNG_COMPRESSION_TYPE_DEFAULT, PNG_FILTER_TYPE_DEFAULT);
	png_write_info(png, info);

	rows = malloc(sizeof(png_bytep) * (size_t)h);
	if (!rows)
		goto out_info;
	for (y = 0; y < h; y++)
		rows[y] = (png_bytep)(rgba + (size_t)y * (size_t)w * 4u);
	png_write_image(png, rows);
	png_write_end(png, NULL);
	free(rows);
	rc = 0;

out_info:
	png_destroy_write_struct(&png, &info);
	goto out_fp;
out_png:
	png_destroy_write_struct(&png, NULL);
out_fp:
	fclose(fp);
	return rc;
}

static uint8_t unpremultiply(uint8_t c, uint8_t a)
{
	unsigned v;
	if (a == 0)
		return 0;
	if (a == 255)
		return c;
	v = ((unsigned)c * 255u + (unsigned)a / 2u) / (unsigned)a;
	return v > 255u ? 255u : (uint8_t)v;
}

/*
 * Convert a wl_shm buffer to tightly packed RGBA8888 for PNG.  Returns 0 on
 * success and hands ownership of *out to the caller.
 */
static int buffer_to_rgba(struct mw_buffer *buf, uint8_t **out, int *out_w,
			  int *out_h)
{
	int w, h, x, y;
	int is_argb;
	size_t need, maplen;
	struct stat st;
	void *base;
	uint8_t *rgba;

	if (!buf || buf->fd < 0 || buf->stride <= 0 || buf->offset < 0)
		return -1;
	w = buf->width;
	h = buf->height;
	if (w <= 0 || h <= 0)
		return -1;
	/* Never read past a caller-supplied short stride. */
	if ((size_t)w * 4u > (size_t)buf->stride)
		w = buf->stride / 4;
	if (w <= 0)
		return -1;

	if (fstat(buf->fd, &st) < 0)
		return -1;
	need = (size_t)buf->offset + (size_t)buf->stride * (size_t)h;
	if ((size_t)st.st_size < need)
		return -1;
	maplen = (size_t)st.st_size;

	base = mmap(NULL, maplen, PROT_READ, MAP_SHARED, buf->fd, 0);
	if (base == MAP_FAILED)
		return -1;

	rgba = malloc((size_t)w * (size_t)h * 4u);
	if (!rgba) {
		munmap(base, maplen);
		return -1;
	}

	is_argb = (buf->format == WL_SHM_FORMAT_ARGB8888);
	for (y = 0; y < h; y++) {
		const uint8_t *row = (const uint8_t *)base + buf->offset +
				     (size_t)y * (size_t)buf->stride;
		uint8_t *dst = rgba + (size_t)y * (size_t)w * 4u;

		for (x = 0; x < w; x++) {
			/* ARGB8888/XRGB8888 are 32-bit words in host byte order. */
			const uint8_t *p = row + (size_t)x * 4u;
			uint8_t r, g, b, a;
#if MW_LITTLE_ENDIAN
			b = p[0]; g = p[1]; r = p[2]; a = p[3];
#else
			a = p[0]; r = p[1]; g = p[2]; b = p[3];
#endif
			if (is_argb) {
				r = unpremultiply(r, a);
				g = unpremultiply(g, a);
				b = unpremultiply(b, a);
			} else {
				a = 255;
			}
			dst[0] = r;
			dst[1] = g;
			dst[2] = b;
			dst[3] = a;
			dst += 4;
		}
	}

	munmap(base, maplen);
	*out = rgba;
	*out_w = w;
	*out_h = h;
	return 0;
}

/*
 * "largest / last-mapped" toplevel: greatest buffer area, breaking ties (and
 * falling back) by most recent map.
 */
static struct mw_surface *pick_screenshot_surface(struct mw_compositor *comp)
{
	struct mw_surface *s, *best = NULL;
	int64_t best_area = -1;
	uint64_t best_seq = 0;
	int pass;

	int smallest = getenv("HC_SMALL") != NULL;

	for (pass = 0; pass < 2; pass++) {
		wl_list_for_each(s, &comp->surfaces, link) {
			int64_t area;

			if (!s->buffer)
				continue;
			if (pass == 0 && !s->is_toplevel)
				continue;
			area = (int64_t)s->buf_w * (int64_t)s->buf_h;
			/* Tiny helper windows are not interesting; skip them. */
			if (smallest && (s->buf_w < 40 || s->buf_h < 40))
				continue;
			if ((smallest && (best_area < 0 || area < best_area)) ||
			    (!smallest &&
			     (area > best_area ||
			      (area == best_area && s->map_seq > best_seq)))) {
				best = s;
				best_area = area;
				best_seq = s->map_seq;
			}
		}
		if (best)
			return best;
	}
	return best;
}

static void save_screenshot_path(struct mw_compositor *comp, const char *path)
{
	struct mw_surface *s = pick_screenshot_surface(comp);
	uint8_t *rgba = NULL;
	int w = 0, h = 0;

	if (s && s->buffer) {
		struct mw_buffer *buf = wl_resource_get_user_data(s->buffer);

		if (buf && buffer_to_rgba(buf, &rgba, &w, &h) == 0) {
			/* A viewport crops the (possibly larger) buffer down to
			 * the window: present only the source rectangle.  The
			 * source rect is in surface-local coordinates, so it is
			 * scaled by the surface's buffer scale to reach buffer
			 * (physical) pixels. */
			int bfs = s->buffer_scale > 0 ? s->buffer_scale : 1;
			int vsx = s->vp_sx * bfs, vsy = s->vp_sy * bfs;
			int vsw = s->vp_sw * bfs, vsh = s->vp_sh * bfs;
			if (s->has_vp_source && s->vp_sw > 0 && s->vp_sh > 0 &&
			    (vsw < w || vsh < h || vsx || vsy)) {
				int cw = vsw, ch = vsh, y;
				if (cw > w - vsx) cw = w - vsx;
				if (ch > h - vsy) ch = h - vsy;
				uint8_t *crop = malloc((size_t)cw * (size_t)ch * 4u);
				if (crop) {
					for (y = 0; y < ch; y++)
						memcpy(crop + (size_t)y * cw * 4u,
						       rgba + ((size_t)(y + vsy) * w +
						               vsx) * 4u,
						       (size_t)cw * 4u);
					free(rgba);
					rgba = crop;
					w = cw;
					h = ch;
				}
			}
			if (write_png(path, rgba, w, h) == 0)
				fprintf(stderr,
					"headless-compositor: captured %dx%d -> %s\n",
					w, h, path);
		} else {
			fprintf(stderr,
				"headless-compositor: could not read the mapped buffer\n");
		}
	} else {
		fprintf(stderr,
			"headless-compositor: no mapped surface; writing a blank frame\n");
	}

	if (!rgba) {
		size_t i, n;
		w = comp->width > 0 ? comp->width : 1;
		h = comp->height > 0 ? comp->height : 1;
		n = (size_t)w * (size_t)h;
		rgba = malloc(n * 4u);
		if (rgba) {
			for (i = 0; i < n; i++) {
				rgba[i * 4u + 0] = 0;
				rgba[i * 4u + 1] = 0;
				rgba[i * 4u + 2] = 0;
				rgba[i * 4u + 3] = 255;
			}
			write_png(path, rgba, w, h);
		}
	}
	free(rgba);
}

static void save_screenshot(struct mw_compositor *comp)
{
	save_screenshot_path(comp, comp->output_path);
}

static void capture_and_exit(struct mw_compositor *comp)
{
	if (comp->captured)
		return;
	comp->captured = 1;
	save_screenshot(comp);
	wl_display_terminate(comp->display);
}

/* ------------------------------------------------------------------ */
/* Frame callbacks (throttled ~60 Hz)                                  */
/* ------------------------------------------------------------------ */

/* Defined with the script machinery further down; called when a surface first
 * becomes visible so a queued input script starts as soon as there is a target. */
static void script_gate_kick(struct mw_compositor *comp);

static void arm_frame_timer(struct mw_compositor *comp)
{
	if (comp->frame_timer)
		wl_event_source_timer_update(comp->frame_timer, FRAME_INTERVAL_MS);
}

static void frame_cb_destroyed(struct wl_resource *resource)
{
	struct mw_frame_cb *cb = wl_resource_get_user_data(resource);

	if (!cb)
		return;
	wl_list_remove(&cb->link);
	free(cb);
}

static int frame_timer_func(void *data)
{
	struct mw_compositor *comp = data;
	struct mw_frame_cb *cb, *tmp;
	uint32_t t = now_ms();

	wl_list_for_each_safe(cb, tmp, &comp->pending_frame_cbs, link) {
		wl_callback_send_done(cb->resource, t);
		wl_resource_destroy(cb->resource);
	}
	return 0;
}

static void fire_frame_callbacks(struct mw_surface *surface)
{
	struct mw_compositor *comp = surface->comp;
	struct mw_frame_cb *cb, *tmp;

	if (wl_list_empty(&surface->frame_callbacks))
		return;

	wl_list_for_each_safe(cb, tmp, &surface->frame_callbacks, link) {
		wl_list_remove(&cb->link);
		wl_list_insert(comp->pending_frame_cbs.prev, &cb->link);
	}
	arm_frame_timer(comp);
}

/* ------------------------------------------------------------------ */
/* wl_compositor / wl_surface / wl_region                              */
/* ------------------------------------------------------------------ */

static void pointer_leave(struct mw_device *dev, struct mw_surface *surface);

/* The ordinary toplevel to fall back to when the focused surface goes away:
 * the most recently mapped one that is still mapped.  Keeping a pointer to a
 * surface that has since been unmapped (a closed dialog, say) would route
 * every later event into a dead window. */
static struct mw_surface *fallback_toplevel(struct mw_compositor *comp,
					    struct mw_surface *avoid)
{
	struct mw_surface *s, *best = NULL;
	uint64_t best_seq = 0;

	wl_list_for_each(s, &comp->surfaces, link) {
		if (!s->is_toplevel || !s->mapped || s == avoid)
			continue;
		if (!best || s->map_seq >= best_seq) {
			best = s;
			best_seq = s->map_seq;
		}
	}
	return best;
}

/* A surface that stops being mapped must not keep the input focus: a real
 * compositor sends wl_pointer.leave and routes subsequent input elsewhere.
 * Without this, a dismissed menu would go on receiving presses, so a client
 * that unmaps and re-maps a popup -- which is exactly what Motif does when a
 * menu is dismissed and then re-posted -- could never be clicked again. */
static void drop_input_focus(struct mw_surface *surface)
{
	struct mw_compositor *comp = surface->comp;
	struct mw_device *dev;

	if (comp->input_surface == surface)
		comp->input_surface = fallback_toplevel(comp, surface);
	if (comp->last_toplevel == surface)
		comp->last_toplevel = fallback_toplevel(comp, surface);
	wl_list_for_each(dev, &comp->pointers, link) {
		if (dev->entered_surface == surface)
			pointer_leave(dev, surface);
	}
	wl_list_for_each(dev, &comp->keyboards, link) {
		if (dev->entered_surface == surface) {
			dev->entered_surface = NULL;
			dev->entered = 0;
		}
	}
}

static void surface_destroyed(struct wl_resource *resource)
{
	struct mw_surface *surface = wl_resource_get_user_data(resource);
	struct mw_frame_cb *cb, *tmp;

	if (!surface)
		return;

	if (surface->xdg_surface) {
		surface->xdg_surface->surface = NULL;
		surface->xdg_surface = NULL;
	}
	wl_list_for_each_safe(cb, tmp, &surface->frame_callbacks, link)
		wl_resource_destroy(cb->resource);

	if (surface->comp->last_toplevel == surface)
		surface->comp->last_toplevel = NULL;
	drop_input_focus(surface);
	wl_list_remove(&surface->link);
	free(surface);
}

static void surface_destroy(struct wl_client *client,
			    struct wl_resource *resource)
{
	(void)client;
	wl_resource_destroy(resource);
}

static void surface_attach(struct wl_client *client,
			   struct wl_resource *resource,
			   struct wl_resource *buffer, int32_t x, int32_t y)
{
	struct mw_surface *surface = wl_resource_get_user_data(resource);
	(void)client;
	(void)x;
	(void)y;
	if (!surface)
		return;
	surface->pending_buffer = buffer;
	surface->pending_attach = 1;
}

static void surface_damage(struct wl_client *client,
			   struct wl_resource *resource,
			   int32_t x, int32_t y, int32_t width, int32_t height)
{
	(void)client; (void)resource; (void)x; (void)y; (void)width; (void)height;
}

static void surface_frame(struct wl_client *client,
			  struct wl_resource *resource, uint32_t callback)
{
	struct mw_surface *surface = wl_resource_get_user_data(resource);
	struct mw_frame_cb *cb;

	if (!surface)
		return;
	cb = calloc(1, sizeof *cb);
	if (!cb)
		return;
	cb->resource = wl_resource_create(client, &wl_callback_interface, 1, callback);
	wl_resource_set_implementation(cb->resource, NULL, cb, frame_cb_destroyed);
	wl_list_insert(&surface->frame_callbacks, &cb->link);
}

static void surface_set_opaque_region(struct wl_client *client,
				      struct wl_resource *resource,
				      struct wl_resource *region)
{
	(void)client; (void)resource; (void)region;
}

static void surface_set_input_region(struct wl_client *client,
				     struct wl_resource *resource,
				     struct wl_resource *region)
{
	(void)client; (void)resource; (void)region;
}

static void send_enter_if_needed(struct mw_surface *surface)
{
	struct wl_resource *output;

	if (surface->enter_sent)
		return;
	output = output_for_client(surface->comp,
				   wl_resource_get_client(surface->resource));
	if (!output)
		return;
	wl_surface_send_enter(surface->resource, output);
	wl_output_send_done(output);
	surface->enter_sent = 1;
}

static void send_xdg_configure(struct mw_surface *surface)
{
	struct mw_xdg_surface *xdg = surface->xdg_surface;
	struct mw_compositor *comp = surface->comp;
	struct wl_array states;
	uint32_t serial;

	if (!xdg || xdg->configured)
		return;

	if (xdg->toplevel) {
		wl_array_init(&states);
		/* xdg_toplevel.configure(width, height, states).  Zero means
		 * "no preference": like a floating window manager, let the
		 * application keep the size it asked for.  An explicit size is
		 * sent by the "resize" script action instead. */
		xdg_toplevel_send_configure(xdg->toplevel->resource, 0, 0, &states);
		wl_array_release(&states);
	} else if (xdg->popup) {
		xdg_popup_send_configure(xdg->popup->resource, 0, 0,
					 xdg->popup->w, xdg->popup->h);
	}

	serial = wl_display_next_serial(comp->display);
	xdg_surface_send_configure(xdg->resource, serial);

	send_enter_if_needed(surface);
	if (xdg->toplevel)
		xdg->toplevel->configured = 1;
	xdg->configured = 1;
}

static void surface_commit(struct wl_client *client,
			   struct wl_resource *resource)
{
	struct mw_surface *surface = wl_resource_get_user_data(resource);
	struct mw_compositor *comp;
	struct mw_xdg_surface *xdg;

	(void)client;
	if (!surface)
		return;
	comp = surface->comp;
	xdg = surface->xdg_surface;

	if (surface->pending_attach) {
		struct mw_buffer *buf;
		int is_null = (surface->pending_buffer == NULL);

		surface->pending_attach = 0;

		/* Attaching a buffer before the configure has been acked is a
		 * protocol error.  KWin words it "attached a buffer before
		 * configure event"; match that so failures are recognisable. */
		if (!is_null && xdg && !xdg->acked) {
			wl_resource_post_error(xdg->resource,
					       XDG_SURFACE_ERROR_UNCONFIGURED_BUFFER,
					       "attached a buffer before configure event");
			return;
		}

		/* Release the buffer we were displaying, as a real compositor
		 * does once it has a replacement.  Without this a client can
		 * never learn its buffer is free, so it ends up writing into a
		 * buffer the compositor is still showing (tearing, or a frame
		 * that looks blank/stale). */
		/* HC_NORELEASE mimics a compositor that is slow to release buffers
		 * (KWin holds them for a while), to exercise the client's fallback. */
		if (!getenv("HC_NORELEASE") &&
		    surface->buffer && surface->buffer != surface->pending_buffer)
			wl_buffer_send_release(surface->buffer);

		surface->buffer = surface->pending_buffer;
		surface->pending_buffer = NULL;

		buf = surface->buffer ?
			wl_resource_get_user_data(surface->buffer) : NULL;
		if (surface->buffer && buf) {
			surface->buf_w = buf->width;
			surface->buf_h = buf->height;
			/* Bump the sequence only on the unmapped -> mapped
			 * transition.  Doing it on every commit made "most
			 * recently mapped" mean "most recently painted", so a
			 * window that merely repainted stole the input from the
			 * dialog that was actually on top. */
			int was_mapped = surface->mapped;
			if (!was_mapped)
				surface->map_seq = ++comp->map_seq;
			surface->mapped = 1;
			if (surface->is_popup) {
				comp->popup_surface = surface;
			} else if (surface->is_toplevel) {
				/* A toplevel becoming visible takes the input,
				 * dismissing any open popup -- which is what a
				 * compositor does when another window is
				 * activated.  Without this the menu popup kept
				 * the focus forever and a dialog opened from a
				 * menu item could not be clicked at all. */
				if (!was_mapped) comp->popup_surface = NULL;
				comp->last_toplevel = surface;
			}
			/* The input target is derived from the popup and the most
			 * recently mapped toplevel at input time (see
			 * execute_action).  Updating it here as well meant an
			 * ordinary window that merely repainted -- the main
			 * window redrawing while a dialog was up, say -- took
			 * the input away from the dialog. */
			if (!was_mapped && comp->action_count > 0)
				script_gate_kick(comp);
		} else {
			/* A NULL commit unmaps the surface.  Whether the
			 * compositor owes it a fresh configure when it comes
			 * back differs in practice: wlroots re-sends one, KWin
			 * does not (a popup that was configured once stays
			 * configured across an unmap/re-map).  HC_KWIN_REMAP
			 * reproduces KWin, so the shim cannot rely on getting a
			 * second configure. */
			surface->mapped = 0;
			surface->buf_w = surface->buf_h = 0;
			if (comp->popup_surface == surface)
				comp->popup_surface = NULL;
			drop_input_focus(surface);
			if (xdg && !(getenv("HC_KWIN_REMAP") && surface->is_popup)) {
				xdg->acked = 0;
				xdg->configured = 0;
			}
		}
	}

	if (xdg)
		send_xdg_configure(surface);

	send_enter_if_needed(surface);
	fire_frame_callbacks(surface);
}

static void surface_set_buffer_transform(struct wl_client *client,
					 struct wl_resource *resource,
					 int32_t transform)
{
	(void)client; (void)resource; (void)transform;
}

static void surface_set_buffer_scale(struct wl_client *client,
				     struct wl_resource *resource,
				     int32_t scale)
{
	struct mw_surface *surface = wl_resource_get_user_data(resource);
	(void)client;
	if (scale > 0 && surface)
		surface->buffer_scale = scale;
}

static void surface_damage_buffer(struct wl_client *client,
				  struct wl_resource *resource,
				  int32_t x, int32_t y, int32_t width,
				  int32_t height)
{
	(void)client; (void)resource; (void)x; (void)y; (void)width; (void)height;
}

static void surface_offset(struct wl_client *client,
			   struct wl_resource *resource, int32_t x, int32_t y)
{
	(void)client; (void)resource; (void)x; (void)y;
}

static void surface_get_release(struct wl_client *client,
				struct wl_resource *resource,
				uint32_t callback)
{
	struct mw_surface *surface = wl_resource_get_user_data(resource);
	struct mw_frame_cb *cb;

	if (!surface)
		return;
	cb = calloc(1, sizeof *cb);
	if (!cb)
		return;
	cb->resource = wl_resource_create(client, &wl_callback_interface, 1, callback);
	wl_resource_set_implementation(cb->resource, NULL, cb, frame_cb_destroyed);
	wl_list_insert(surface->comp->pending_frame_cbs.prev, &cb->link);
	arm_frame_timer(surface->comp);
}

static const struct wl_surface_interface surface_implementation = {
	.destroy = surface_destroy,
	.attach = surface_attach,
	.damage = surface_damage,
	.frame = surface_frame,
	.set_opaque_region = surface_set_opaque_region,
	.set_input_region = surface_set_input_region,
	.commit = surface_commit,
	.set_buffer_transform = surface_set_buffer_transform,
	.set_buffer_scale = surface_set_buffer_scale,
	.damage_buffer = surface_damage_buffer,
	.offset = surface_offset,
	.get_release = surface_get_release,
};

static void region_destroy(struct wl_client *client,
			   struct wl_resource *resource)
{
	(void)client;
	wl_resource_destroy(resource);
}

static void region_add(struct wl_client *client, struct wl_resource *resource,
		       int32_t x, int32_t y, int32_t width, int32_t height)
{
	(void)client; (void)resource; (void)x; (void)y; (void)width; (void)height;
}

static void region_subtract(struct wl_client *client,
			    struct wl_resource *resource,
			    int32_t x, int32_t y, int32_t width, int32_t height)
{
	(void)client; (void)resource; (void)x; (void)y; (void)width; (void)height;
}

static const struct wl_region_interface region_implementation = {
	.destroy = region_destroy,
	.add = region_add,
	.subtract = region_subtract,
};

static void compositor_create_surface(struct wl_client *client,
				      struct wl_resource *resource,
				      uint32_t id)
{
	struct mw_compositor *comp = wl_resource_get_user_data(resource);
	struct mw_surface *surface = calloc(1, sizeof *surface);

	if (!surface)
		return;
	surface->comp = comp;
	surface->buffer_scale = 1;
	wl_list_init(&surface->frame_callbacks);
	wl_list_insert(&comp->surfaces, &surface->link);

	surface->resource = wl_resource_create(client, &wl_surface_interface,
					       wl_resource_get_version(resource), id);
	wl_resource_set_implementation(surface->resource, &surface_implementation,
				       surface, surface_destroyed);
}

static void compositor_create_region(struct wl_client *client,
				     struct wl_resource *resource, uint32_t id)
{
	struct wl_resource *region = wl_resource_create(client, &wl_region_interface,
						wl_resource_get_version(resource), id);
	wl_resource_set_implementation(region, &region_implementation, NULL, NULL);
}

static void compositor_release(struct wl_client *client,
			       struct wl_resource *resource)
{
	(void)client;
	wl_resource_destroy(resource);
}

static const struct wl_compositor_interface compositor_implementation = {
	.create_surface = compositor_create_surface,
	.create_region = compositor_create_region,
	.release = compositor_release,
};

static void bind_compositor(struct wl_client *client, void *data,
			    uint32_t version, uint32_t id)
{
	struct wl_resource *resource = wl_resource_create(client,
			&wl_compositor_interface, MIN(version, 5), id);
	wl_resource_set_implementation(resource, &compositor_implementation, data, NULL);
}

/* ------------------------------------------------------------------ */
/* wl_shm                                                              */
/* ------------------------------------------------------------------ */

static void buffer_destroyed(struct wl_resource *resource)
{
	struct mw_buffer *buf = wl_resource_get_user_data(resource);
	struct mw_surface *surface;

	if (!buf)
		return;
	/* Drop any surface references so we never touch a dead buffer. */
	if (buf->comp) {
		wl_list_for_each(surface, &buf->comp->surfaces, link) {
			if (surface->pending_buffer == resource) {
				surface->pending_buffer = NULL;
				surface->pending_attach = 0;
			}
			if (surface->buffer == resource) {
				surface->buffer = NULL;
				surface->mapped = 0;
				surface->buf_w = surface->buf_h = 0;
			}
		}
	}
	if (buf->fd >= 0)
		close(buf->fd);
	free(buf);
}

static void buffer_destroy(struct wl_client *client,
			   struct wl_resource *resource)
{
	(void)client;
	wl_resource_destroy(resource);
}

static const struct wl_buffer_interface buffer_implementation = {
	.destroy = buffer_destroy,
};

static void shm_pool_create_buffer(struct wl_client *client,
				   struct wl_resource *resource, uint32_t id,
				   int32_t offset, int32_t width, int32_t height,
				   int32_t stride, uint32_t format)
{
	struct mw_pool *pool = wl_resource_get_user_data(resource);
	struct mw_buffer *buf;

	if (!pool)
		return;
	buf = calloc(1, sizeof *buf);
	if (!buf)
		return;
	buf->fd = dup(pool->fd);
	buf->comp = pool->comp;
	buf->offset = offset;
	buf->width = width;
	buf->height = height;
	buf->stride = stride;
	buf->format = format;

	buf->resource = wl_resource_create(client, &wl_buffer_interface, 1, id);
	wl_resource_set_implementation(buf->resource, &buffer_implementation,
				       buf, buffer_destroyed);
}

static void shm_pool_resize(struct wl_client *client,
			    struct wl_resource *resource, int32_t size)
{
	struct mw_pool *pool = wl_resource_get_user_data(resource);
	(void)client;
	if (pool)
		pool->size = size;
}

static void shm_pool_destroyed(struct wl_resource *resource)
{
	struct mw_pool *pool = wl_resource_get_user_data(resource);

	if (!pool)
		return;
	if (pool->fd >= 0)
		close(pool->fd);
	free(pool);
}

static void shm_pool_destroy(struct wl_client *client,
			     struct wl_resource *resource)
{
	(void)client;
	wl_resource_destroy(resource);
}

static const struct wl_shm_pool_interface shm_pool_implementation = {
	.create_buffer = shm_pool_create_buffer,
	.destroy = shm_pool_destroy,
	.resize = shm_pool_resize,
};

static void shm_create_pool(struct wl_client *client,
			    struct wl_resource *resource, uint32_t id,
			    int32_t fd, int32_t size)
{
	struct mw_compositor *comp = wl_resource_get_user_data(resource);
	struct mw_pool *pool = calloc(1, sizeof *pool);

	if (!pool) {
		close(fd);
		return;
	}
	/* libwayland closes the received fd once this handler returns. */
	pool->fd = dup(fd);
	pool->comp = comp;
	pool->size = size;
	pool->resource = wl_resource_create(client, &wl_shm_pool_interface, 1, id);
	wl_resource_set_implementation(pool->resource, &shm_pool_implementation,
				       pool, shm_pool_destroyed);
}

static void shm_release(struct wl_client *client, struct wl_resource *resource)
{
	(void)client;
	wl_resource_destroy(resource);
}

static const struct wl_shm_interface shm_implementation = {
	.create_pool = shm_create_pool,
	.release = shm_release,
};

static void bind_shm(struct wl_client *client, void *data,
		     uint32_t version, uint32_t id)
{
	struct wl_resource *resource = wl_resource_create(client,
			&wl_shm_interface, MIN(version, 2), id);
	wl_resource_set_implementation(resource, &shm_implementation, data, NULL);
	wl_shm_send_format(resource, WL_SHM_FORMAT_ARGB8888);
	wl_shm_send_format(resource, WL_SHM_FORMAT_XRGB8888);
}

/* ------------------------------------------------------------------ */
/* wl_seat / wl_pointer / wl_keyboard                                  */
/* ------------------------------------------------------------------ */

static void device_destroyed(struct wl_resource *resource)
{
	struct mw_device *dev = wl_resource_get_user_data(resource);

	if (!dev)
		return;
	wl_list_remove(&dev->link);
	free(dev);
}

static void send_keymap(struct mw_compositor *comp, struct wl_resource *keyboard)
{
	int fd;
	ssize_t written;

	if (!comp->xkb_keymap_string)
		return;
	fd = create_anon_file(comp->xkb_keymap_size);
	if (fd < 0)
		return;
	written = write(fd, comp->xkb_keymap_string, comp->xkb_keymap_size);
	if (written == (ssize_t)comp->xkb_keymap_size)
		wl_keyboard_send_keymap(keyboard,
					WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1,
					fd, (uint32_t)comp->xkb_keymap_size);
	close(fd);
}

static const struct wl_pointer_interface pointer_implementation;
static const struct wl_keyboard_interface keyboard_implementation;
static const struct wl_touch_interface touch_implementation;

static void seat_get_pointer(struct wl_client *client,
			     struct wl_resource *resource, uint32_t id)
{
	struct mw_compositor *comp = wl_resource_get_user_data(resource);
	struct mw_device *dev = calloc(1, sizeof *dev);

	if (!dev)
		return;
	dev->resource = wl_resource_create(client, &wl_pointer_interface,
					   wl_resource_get_version(resource), id);
	wl_resource_set_implementation(dev->resource, &pointer_implementation,
				       dev, device_destroyed);
	wl_list_insert(&comp->pointers, &dev->link);
}

static void seat_get_keyboard(struct wl_client *client,
			      struct wl_resource *resource, uint32_t id)
{
	struct mw_compositor *comp = wl_resource_get_user_data(resource);
	struct mw_device *dev = calloc(1, sizeof *dev);

	if (!dev)
		return;
	dev->resource = wl_resource_create(client, &wl_keyboard_interface,
					   wl_resource_get_version(resource), id);
	wl_resource_set_implementation(dev->resource, &keyboard_implementation,
				       dev, device_destroyed);
	wl_list_insert(&comp->keyboards, &dev->link);

	send_keymap(comp, dev->resource);
	wl_keyboard_send_repeat_info(dev->resource, 25, 400);
}

static void seat_get_touch(struct wl_client *client,
			   struct wl_resource *resource, uint32_t id)
{
	struct mw_compositor *comp = wl_resource_get_user_data(resource);
	struct mw_device *dev = calloc(1, sizeof *dev);

	if (!dev)
		return;
	dev->resource = wl_resource_create(client, &wl_touch_interface,
					   wl_resource_get_version(resource), id);
	wl_resource_set_implementation(dev->resource, &touch_implementation,
				       dev, device_destroyed);
	wl_list_insert(&comp->touches, &dev->link);
}

static void seat_release(struct wl_client *client, struct wl_resource *resource)
{
	(void)client;
	wl_resource_destroy(resource);
}

static const struct wl_seat_interface seat_implementation = {
	.get_pointer = seat_get_pointer,
	.get_keyboard = seat_get_keyboard,
	.get_touch = seat_get_touch,
	.release = seat_release,
};

static void bind_seat(struct wl_client *client, void *data,
		      uint32_t version, uint32_t id)
{
	struct wl_resource *resource = wl_resource_create(client,
			&wl_seat_interface, MIN(version, 7), id);
	wl_resource_set_implementation(resource, &seat_implementation, data, NULL);
	wl_seat_send_capabilities(resource,
			WL_SEAT_CAPABILITY_POINTER | WL_SEAT_CAPABILITY_KEYBOARD |
			WL_SEAT_CAPABILITY_TOUCH);
	if (wl_resource_get_version(resource) >= 2)
		wl_seat_send_name(resource, "seat0");
}

static void pointer_set_cursor(struct wl_client *client,
			       struct wl_resource *resource, uint32_t serial,
			       struct wl_resource *surface,
			       int32_t hotspot_x, int32_t hotspot_y)
{
	(void)client; (void)resource; (void)serial; (void)surface;
	(void)hotspot_x; (void)hotspot_y;
}

static void pointer_release(struct wl_client *client,
			    struct wl_resource *resource)
{
	(void)client;
	wl_resource_destroy(resource);
}

static const struct wl_pointer_interface pointer_implementation = {
	.set_cursor = pointer_set_cursor,
	.release = pointer_release,
};

static void keyboard_release(struct wl_client *client,
			     struct wl_resource *resource)
{
	(void)client;
	wl_resource_destroy(resource);
}

static const struct wl_keyboard_interface keyboard_implementation = {
	.release = keyboard_release,
};

static void touch_release(struct wl_client *client,
			  struct wl_resource *resource)
{
	(void)client;
	wl_resource_destroy(resource);
}

static const struct wl_touch_interface touch_implementation = {
	.release = touch_release,
};

/* ------------------------------------------------------------------ */
/* wp_viewporter / wp_viewport                                         */
/*                                                                      */
/* A client may attach a buffer larger than the window and crop it with */
/* a viewport, so resizing does not have to recreate the buffer.        */
/* ------------------------------------------------------------------ */

static void viewport_destroy(struct wl_client *client, struct wl_resource *resource)
{ (void)client; wl_resource_destroy(resource); }

static void viewport_set_source(struct wl_client *client, struct wl_resource *resource,
				wl_fixed_t x, wl_fixed_t y, wl_fixed_t w, wl_fixed_t h)
{
	struct mw_surface *surface = wl_resource_get_user_data(resource);
	(void)client;
	if (!surface)
		return;
	if (w < 0 || h < 0) {            /* NULL source: no crop */
		surface->has_vp_source = 0;
		return;
	}
	surface->vp_sx = wl_fixed_to_int(x);
	surface->vp_sy = wl_fixed_to_int(y);
	surface->vp_sw = wl_fixed_to_int(w);
	surface->vp_sh = wl_fixed_to_int(h);
	surface->has_vp_source = 1;
}

static void viewport_set_destination(struct wl_client *client,
				     struct wl_resource *resource,
				     int32_t w, int32_t h)
{
	struct mw_surface *surface = wl_resource_get_user_data(resource);
	(void)client;
	if (!surface)
		return;
	surface->vp_dw = w;
	surface->vp_dh = h;
}

static const struct wp_viewport_interface viewport_implementation = {
	.destroy = viewport_destroy,
	.set_source = viewport_set_source,
	.set_destination = viewport_set_destination,
};

static void viewport_destroyed(struct wl_resource *resource)
{
	struct mw_surface *surface = wl_resource_get_user_data(resource);
	if (surface && surface->viewport == resource) {
		surface->viewport = NULL;
		surface->has_vp_source = 0;
		surface->vp_dw = surface->vp_dh = 0;
	}
}

static void viewporter_get_viewport(struct wl_client *client,
				    struct wl_resource *resource,
				    uint32_t id, struct wl_resource *surface_resource)
{
	struct mw_surface *surface = wl_resource_get_user_data(surface_resource);
	struct wl_resource *vp;

	if (!surface)
		return;
	vp = wl_resource_create(client, &wp_viewport_interface,
				wl_resource_get_version(resource), id);
	surface->viewport = vp;
	surface->has_vp_source = 0;
	surface->vp_dw = surface->vp_dh = 0;
	wl_resource_set_implementation(vp, &viewport_implementation, surface,
				       viewport_destroyed);
}

static void viewporter_destroy(struct wl_client *client, struct wl_resource *resource)
{ (void)client; wl_resource_destroy(resource); }

static const struct wp_viewporter_interface viewporter_implementation = {
	.destroy = viewporter_destroy,
	.get_viewport = viewporter_get_viewport,
};

static void bind_viewporter(struct wl_client *client, void *data,
			    uint32_t version, uint32_t id)
{
	struct wl_resource *resource = wl_resource_create(client,
			&wp_viewporter_interface, MIN(version, 1), id);
	(void)data;
	wl_resource_set_implementation(resource, &viewporter_implementation, NULL,
				       NULL);
}

/* ------------------------------------------------------------------ */
/* zwp_text_input_manager_v3 — a scripted input method                  */
/*                                                                      */
/* The shim binds this to implement XIM.  There is no real IME here:     */
/* once a text-input is enabled and committed, the compositor sends      */
/* enter, then a canned preedit, then (a moment later) a canned commit,  */
/* so the whole XIM path can be exercised without ibus.  The strings are */
/* overridable with HC_IME_PREEDIT / HC_IME_COMMIT.                      */
/* ------------------------------------------------------------------ */

struct hc_text_input {
	struct wl_resource     *resource;
	struct mw_compositor   *comp;
	int                     enabled;
	int                     entered;
	int                     sent;
	struct wl_event_source *commit_timer;
};

/* A surface of the text-input's own client to deliver enter to. */
static struct mw_surface *ti_target_surface(struct hc_text_input *ti)
{
	struct wl_client *client = wl_resource_get_client(ti->resource);
	struct mw_surface *s;

	if (ti->comp->input_surface &&
	    wl_resource_get_client(ti->comp->input_surface->resource) == client)
		return ti->comp->input_surface;
	wl_list_for_each(s, &ti->comp->surfaces, link) {
		if (s->is_toplevel &&
		    wl_resource_get_client(s->resource) == client)
			return s;
	}
	return NULL;
}

static int ti_commit_timer_cb(void *data)
{
	struct hc_text_input *ti = data;
	const char *text = getenv("HC_IME_COMMIT");
	if (!text) text = "\xe4\xbd\xa0\xe5\xa5\xbd";  /* 你好 */
	/* HC_IME_COMMIT="" holds the preedit (no commit), so a Position-style
	 * overlay can be captured; otherwise send the commit. */
	if (ti->resource && text[0]) {
		zwp_text_input_v3_send_commit_string(ti->resource, text);
		zwp_text_input_v3_send_done(ti->resource,
			wl_display_next_serial(ti->comp->display));
	}
	if (ti->commit_timer) {
		wl_event_source_remove(ti->commit_timer);
		ti->commit_timer = NULL;
	}
	return 0;
}

static void ti_send_preedit(struct hc_text_input *ti)
{
	const char *pre = getenv("HC_IME_PREEDIT");
	if (!pre) pre = "ni";
	zwp_text_input_v3_send_preedit_string(ti->resource, pre, -1, -1);
	zwp_text_input_v3_send_done(ti->resource,
		wl_display_next_serial(ti->comp->display));
	ti->commit_timer = wl_event_loop_add_timer(ti->comp->loop,
			ti_commit_timer_cb, ti);
	if (ti->commit_timer)
		wl_event_source_timer_update(ti->commit_timer, 60);
}

static void text_input_destroy(struct wl_client *client, struct wl_resource *r)
{
	(void)client;
	wl_resource_destroy(r);
}

static void text_input_enable(struct wl_client *client, struct wl_resource *r)
{
	(void)client;
	struct hc_text_input *ti = wl_resource_get_user_data(r);
	ti->enabled = 1;
}

static void text_input_disable(struct wl_client *client, struct wl_resource *r)
{
	(void)client;
	struct hc_text_input *ti = wl_resource_get_user_data(r);
	ti->enabled = 0;
	ti->entered = 0;
	ti->sent = 0;
}

static void text_input_set_surrounding_text(struct wl_client *client,
		struct wl_resource *r, const char *text, int32_t cursor, int32_t anchor)
{ (void)client; (void)r; (void)text; (void)cursor; (void)anchor; }

static void text_input_set_text_change_cause(struct wl_client *client,
		struct wl_resource *r, uint32_t cause)
{ (void)client; (void)r; (void)cause; }

static void text_input_set_content_type(struct wl_client *client,
		struct wl_resource *r, uint32_t hint, uint32_t purpose)
{ (void)client; (void)r; (void)hint; (void)purpose; }

static void text_input_set_cursor_rectangle(struct wl_client *client,
		struct wl_resource *r, int32_t x, int32_t y, int32_t w, int32_t h)
{ (void)client; (void)r; (void)x; (void)y; (void)w; (void)h; }

static void text_input_commit(struct wl_client *client, struct wl_resource *r)
{
	(void)client;
	struct hc_text_input *ti = wl_resource_get_user_data(r);
	if (!ti->enabled) return;
	if (!ti->entered) {
		struct mw_surface *s = ti_target_surface(ti);
		if (!s) return;              /* retry on a later commit */
		zwp_text_input_v3_send_enter(r, s->resource);
		ti->entered = 1;
		return;                      /* the client re-commits in response */
	}
	if (ti->sent == 0) {
		ti->sent = 1;
		ti_send_preedit(ti);
	}
}

static const struct zwp_text_input_v3_interface text_input_implementation = {
	.destroy = text_input_destroy,
	.enable = text_input_enable,
	.disable = text_input_disable,
	.set_surrounding_text = text_input_set_surrounding_text,
	.set_text_change_cause = text_input_set_text_change_cause,
	.set_content_type = text_input_set_content_type,
	.set_cursor_rectangle = text_input_set_cursor_rectangle,
	.commit = text_input_commit,
};

static void text_input_destroyed(struct wl_resource *r)
{
	struct hc_text_input *ti = wl_resource_get_user_data(r);
	if (!ti) return;
	if (ti->commit_timer) {
		wl_event_source_remove(ti->commit_timer);
		ti->commit_timer = NULL;
	}
	free(ti);
}

static void text_input_manager_get_text_input(struct wl_client *client,
		struct wl_resource *manager, uint32_t id, struct wl_resource *seat)
{
	(void)seat;
	struct hc_text_input *ti = calloc(1, sizeof *ti);
	if (!ti) return;
	ti->comp = wl_resource_get_user_data(manager);
	ti->resource = wl_resource_create(client, &zwp_text_input_v3_interface, 1, id);
	if (!ti->resource) { free(ti); return; }
	wl_resource_set_implementation(ti->resource, &text_input_implementation,
				       ti, text_input_destroyed);
}

static const struct zwp_text_input_manager_v3_interface
text_input_manager_implementation = {
	.get_text_input = text_input_manager_get_text_input,
};

static void bind_text_input_manager(struct wl_client *client, void *data,
				    uint32_t version, uint32_t id)
{
	struct wl_resource *resource = wl_resource_create(client,
			&zwp_text_input_manager_v3_interface, MIN(version, 1), id);
	wl_resource_set_implementation(resource, &text_input_manager_implementation,
				       data, NULL);
}

/* ------------------------------------------------------------------ */
/* wl_data_device_manager / wl_data_source / wl_data_offer              */
/*                                                                      */
/* One compositor-wide clipboard.  A client sets it with a data source; */
/* every data device gets a data offer describing the current source,   */
/* and a receive() on that offer is forwarded to the source's send().   */
/* ------------------------------------------------------------------ */

struct hc_data_source {
	struct wl_resource    *resource;
	struct wl_array        mimes;      /* char * */
	struct mw_compositor  *comp;
	struct wl_list         offers;     /* hc_data_offer referencing us */
	int                    primary;    /* a zwp_primary_selection_source_v1 */
};

struct hc_data_device {
	struct wl_resource   *resource;
	struct wl_list        link;
	struct mw_compositor *comp;
	int                   primary;
};

struct hc_data_offer {
	struct hc_data_source *source;     /* NULL once the source is gone */
	struct wl_list         link;
	int                    primary;
};

static void broadcast_selection(struct mw_compositor *comp);

static void data_source_destroyed(struct wl_resource *resource)
{
	struct hc_data_source *src = wl_resource_get_user_data(resource);
	struct hc_data_offer *offer;
	char **mime;

	if (!src)
		return;
	/* Existing offers must stop pointing at us or a later receive() would
	 * touch freed memory. */
	wl_list_for_each(offer, &src->offers, link)
		offer->source = NULL;
	if (src->comp && src->primary && src->comp->primary_selection == src) {
		src->comp->primary_selection = NULL;
		broadcast_selection(src->comp);
	} else if (src->comp && src->comp->data_selection == src) {
		src->comp->data_selection = NULL;
		broadcast_selection(src->comp);
	}
	wl_array_for_each(mime, &src->mimes)
		free(*mime);
	wl_array_release(&src->mimes);
	free(src);
}

static void data_source_offer(struct wl_client *client,
			      struct wl_resource *resource, const char *mime)
{
	struct hc_data_source *src = wl_resource_get_user_data(resource);
	char **slot;
	(void)client;
	if (!src)
		return;
	slot = wl_array_add(&src->mimes, sizeof(char *));
	if (slot)
		*slot = strdup(mime);
}

static void data_source_destroy(struct wl_client *client,
				struct wl_resource *resource)
{
	(void)client;
	wl_resource_destroy(resource);
}

static void data_source_set_actions(struct wl_client *client,
				    struct wl_resource *resource,
				    uint32_t actions)
{
	(void)client; (void)resource; (void)actions;
}

static const struct wl_data_source_interface data_source_implementation = {
	.offer = data_source_offer,
	.destroy = data_source_destroy,
	.set_actions = data_source_set_actions,
};

static void data_offer_accept(struct wl_client *client,
			      struct wl_resource *resource,
			      uint32_t serial, const char *mime_type)
{
	(void)client; (void)resource; (void)serial; (void)mime_type;
}

/* The receiver hands us the write end of a pipe; forward it to the source,
 * which writes the data and closes it. */
static void data_offer_receive(struct wl_client *client,
			       struct wl_resource *resource,
			       const char *mime_type, int32_t fd)
{
	struct hc_data_offer *offer = wl_resource_get_user_data(resource);
	(void)client;

	if (offer && offer->source && offer->source->resource) {
		if (getenv("HC_TRACE"))
			fprintf(stderr, "HC: offer_receive mime=%s src=%p ver=%u\n",
				mime_type, (void*)offer->source->resource,
				wl_resource_get_version(offer->source->resource));
		if (offer->primary)
			zwp_primary_selection_source_v1_send_send(offer->source->resource,
								  mime_type, fd);
		else
			wl_data_source_send_send(offer->source->resource, mime_type, fd);
	}
	close(fd);
}

static void data_offer_destroy(struct wl_client *client,
			       struct wl_resource *resource)
{
	(void)client;
	wl_resource_destroy(resource);
}

static void data_offer_finish(struct wl_client *client,
			      struct wl_resource *resource)
{
	(void)client; (void)resource;
}

static void data_offer_set_actions(struct wl_client *client,
				   struct wl_resource *resource,
				   uint32_t actions, uint32_t preferred)
{
	(void)client; (void)resource; (void)actions; (void)preferred;
}

static const struct wl_data_offer_interface data_offer_implementation = {
	.accept = data_offer_accept,
	.receive = data_offer_receive,
	.destroy = data_offer_destroy,
	.finish = data_offer_finish,
	.set_actions = data_offer_set_actions,
};

static void data_offer_destroyed(struct wl_resource *resource)
{
	struct hc_data_offer *offer = wl_resource_get_user_data(resource);
	if (!offer)
		return;
	wl_list_remove(&offer->link);
	free(offer);
}

/* --- primary selection (no drag-and-drop) --- */

static void primary_device_set_selection(struct wl_client *client,
					 struct wl_resource *resource,
					 struct wl_resource *source, uint32_t serial)
{
	struct hc_data_device *dev = wl_resource_get_user_data(resource);
	struct mw_compositor *comp = dev ? dev->comp : NULL;
	struct hc_data_source *sel = source
		? wl_resource_get_user_data(source) : NULL;
	(void)client; (void)serial;
	if (!comp) return;

	if (comp->primary_selection && comp->primary_selection != sel &&
	    comp->primary_selection->resource)
		zwp_primary_selection_source_v1_send_cancelled(
			comp->primary_selection->resource);

	comp->primary_selection = sel;
	broadcast_selection(comp);
}

static void primary_device_destroy(struct wl_client *client,
				   struct wl_resource *resource)
{
	(void)client;
	wl_resource_destroy(resource);
}

static const struct zwp_primary_selection_device_v1_interface
primary_device_implementation = {
	.set_selection = primary_device_set_selection,
	.destroy = primary_device_destroy,
};

static const struct zwp_primary_selection_source_v1_interface
primary_source_implementation = {
	.offer = data_source_offer,
	.destroy = data_source_destroy,
};

static const struct zwp_primary_selection_offer_v1_interface
primary_offer_implementation = {
	.receive = data_offer_receive,
	.destroy = data_offer_destroy,
};

static void send_offer_to(struct wl_resource *device, int primary,
			  struct hc_data_source *src)
{
	struct wl_client *client = wl_resource_get_client(device);
	uint32_t version = wl_resource_get_version(device);
	struct wl_resource *offer_res;
	struct hc_data_offer *offer;
	char **mime;

	if (!src) {
		if (primary) zwp_primary_selection_device_v1_send_selection(device, NULL);
		else         wl_data_device_send_selection(device, NULL);
		return;
	}

	offer = calloc(1, sizeof *offer);
	if (!offer) {
		if (primary) zwp_primary_selection_device_v1_send_selection(device, NULL);
		else         wl_data_device_send_selection(device, NULL);
		return;
	}
	offer->source = src;
	offer->primary = primary;
	wl_list_insert(&src->offers, &offer->link);

	/* id 0 lets libwayland allocate a server-side id; the client learns it
	 * from the selection event. */
	offer_res = wl_resource_create(client,
			primary ? (const struct wl_interface *)&zwp_primary_selection_offer_v1_interface
				: &wl_data_offer_interface, version, 0);
	wl_resource_set_implementation(offer_res,
			primary ? (const void *)&primary_offer_implementation
				: (const void *)&data_offer_implementation,
			offer, data_offer_destroyed);

	/* The data_offer event introduces the object; only then may the offer's
	 * MIME types and the selection event reference it. */
	if (primary) {
		zwp_primary_selection_device_v1_send_data_offer(device, offer_res);
		wl_array_for_each(mime, &src->mimes)
			zwp_primary_selection_offer_v1_send_offer(offer_res, *mime);
		zwp_primary_selection_device_v1_send_selection(device, offer_res);
	} else {
		wl_data_device_send_data_offer(device, offer_res);
		wl_array_for_each(mime, &src->mimes)
			wl_data_offer_send_offer(offer_res, *mime);
		wl_data_device_send_selection(device, offer_res);
	}
}

static void broadcast_selection(struct mw_compositor *comp)
{
	struct hc_data_device *dev;

	wl_list_for_each(dev, &comp->data_devices, link)
		send_offer_to(dev->resource, 0, comp->data_selection);
	wl_list_for_each(dev, &comp->primary_devices, link)
		send_offer_to(dev->resource, 1, comp->primary_selection);
}

/* --- minimal drag-and-drop --- */

static struct wl_resource *data_device_for_client(struct mw_compositor *comp,
						  struct wl_client *client)
{
	struct hc_data_device *dev;
	wl_list_for_each(dev, &comp->data_devices, link)
		if (wl_resource_get_client(dev->resource) == client)
			return dev->resource;
	return NULL;
}

static void drag_leave(struct mw_compositor *comp)
{
	if (comp->drag_device)
		wl_data_device_send_leave(comp->drag_device);
	if (comp->drag_offer)
		wl_resource_destroy(comp->drag_offer);
	comp->drag_device = NULL;
	comp->drag_offer = NULL;
	comp->drag_surface = NULL;
}

static void drag_enter(struct mw_compositor *comp, struct mw_surface *surface,
		       int x, int y)
{
	if (!comp->drag_source || !surface)
		return;
	if (getenv("HC_TRACE"))
		fprintf(stderr, "HC: drag_enter surface=%p\n", (void*)surface);
	struct wl_resource *dev = data_device_for_client(comp,
			wl_resource_get_client(surface->resource));
	if (!dev)
		return;

	struct hc_data_offer *offer = calloc(1, sizeof *offer);
	if (!offer)
		return;
	offer->source = comp->drag_source;
	offer->primary = 0;
	wl_list_insert(&comp->drag_source->offers, &offer->link);

	struct wl_resource *offer_res = wl_resource_create(
			wl_resource_get_client(surface->resource),
			&wl_data_offer_interface, wl_resource_get_version(dev), 0);
	wl_resource_set_implementation(offer_res, &data_offer_implementation,
				       offer, data_offer_destroyed);

	wl_data_device_send_data_offer(dev, offer_res);
	char **mime;
	wl_array_for_each(mime, &comp->drag_source->mimes)
		wl_data_offer_send_offer(offer_res, *mime);
	wl_data_device_send_enter(dev, wl_display_next_serial(comp->display),
				  surface->resource, wl_fixed_from_int(x),
				  wl_fixed_from_int(y), offer_res);

	comp->drag_device = dev;
	comp->drag_offer = offer_res;
	comp->drag_surface = surface;
}

static void drag_motion(struct mw_compositor *comp, int x, int y)
{
	if (!comp->drag_device)
		return;
	wl_data_device_send_motion(comp->drag_device, now_ms(),
				   wl_fixed_from_int(x), wl_fixed_from_int(y));
}

static void drag_drop(struct mw_compositor *comp)
{
	if (getenv("HC_TRACE"))
		fprintf(stderr, "HC: drag_drop device=%p source=%p\n",
			(void*)comp->drag_device, (void*)comp->drag_source);
	if (comp->drag_device)
		wl_data_device_send_drop(comp->drag_device);
	if (comp->drag_source && comp->drag_source->resource)
		wl_data_source_send_dnd_finished(comp->drag_source->resource);
	comp->drag_active = 0;
	comp->drag_source = NULL;
	comp->drag_device = NULL;
	comp->drag_surface = NULL;
	/* Keep comp->drag_offer and its source alive: the target fetches the data
	 * with wl_data_offer.receive after the drop, so the offer must outlive the
	 * drop (as it does on a real compositor).  It is freed when the client
	 * destroys it, or leaks until the next drag replaces it. */
}

static void data_device_start_drag(struct wl_client *client,
				   struct wl_resource *resource,
				   struct wl_resource *source,
				   struct wl_resource *origin,
				   struct wl_resource *icon, uint32_t serial)
{
	(void)client; (void)resource; (void)origin; (void)icon; (void)serial;
	struct hc_data_device *dev = wl_resource_get_user_data(resource);
	struct mw_compositor *comp = dev ? dev->comp : NULL;
	if (!comp) return;
	comp->drag_source = source ? wl_resource_get_user_data(source) : NULL;
	comp->drag_active = comp->drag_source != NULL;
	comp->drag_surface = NULL;
	comp->drag_device = NULL;
	comp->drag_offer = NULL;
	if (getenv("HC_TRACE"))
		fprintf(stderr, "HC: start_drag source=%p active=%d serial=%u\n",
			(void*)source, comp->drag_active, serial);
}

static void data_device_set_selection(struct wl_client *client,
				      struct wl_resource *resource,
				      struct wl_resource *source,
				      uint32_t serial)
{
	struct hc_data_device *dev = wl_resource_get_user_data(resource);
	struct mw_compositor *comp = dev ? dev->comp : NULL;
	struct hc_data_source *sel = source
		? wl_resource_get_user_data(source) : NULL;
	(void)client; (void)serial;
	if (!comp) return;

	/* Replacing the selection must cancel the previous owner, just as a real
	 * compositor does; the client relies on it to learn it lost the clipboard. */
	if (comp->data_selection && comp->data_selection != sel &&
	    comp->data_selection->resource)
		wl_data_source_send_cancelled(comp->data_selection->resource);

	comp->data_selection = sel;
	broadcast_selection(comp);
}

static void data_device_release(struct wl_client *client,
				struct wl_resource *resource)
{
	(void)client;
	wl_resource_destroy(resource);
}

static const struct wl_data_device_interface data_device_implementation = {
	.start_drag = data_device_start_drag,
	.set_selection = data_device_set_selection,
	.release = data_device_release,
};

static void data_device_destroyed(struct wl_resource *resource)
{
	struct hc_data_device *dev = wl_resource_get_user_data(resource);
	if (!dev)
		return;
	wl_list_remove(&dev->link);
	free(dev);
}

static void manager_get_data_device(struct wl_client *client,
				    struct wl_resource *resource,
				    uint32_t id, struct wl_resource *seat)
{
	struct mw_compositor *comp = wl_resource_get_user_data(resource);
	struct hc_data_device *dev = calloc(1, sizeof *dev);
	(void)seat;

	if (!dev)
		return;
	dev->comp = comp;
	dev->resource = wl_resource_create(client, &wl_data_device_interface,
					   wl_resource_get_version(resource), id);
	/* user_data is the device record, whose destructor frees it; `comp`
	 * lives on the stack and must never be handed to free(). */
	wl_resource_set_implementation(dev->resource, &data_device_implementation,
				       dev, data_device_destroyed);
	wl_list_insert(&comp->data_devices, &dev->link);

	/* A device joining after the clipboard was set still needs the offer. */
	if (comp->data_selection)
		send_offer_to(dev->resource, 0, comp->data_selection);
}

static void manager_create_data_source(struct wl_client *client,
				       struct wl_resource *resource,
				       uint32_t id)
{
	struct hc_data_source *src = calloc(1, sizeof *src);
	if (!src)
		return;
	wl_array_init(&src->mimes);
	wl_list_init(&src->offers);
	src->comp = wl_resource_get_user_data(resource);
	src->resource = wl_resource_create(client, &wl_data_source_interface,
					   wl_resource_get_version(resource), id);
	wl_resource_set_implementation(src->resource, &data_source_implementation,
				       src, data_source_destroyed);
}

static const struct wl_data_device_manager_interface manager_implementation = {
	.create_data_source = manager_create_data_source,
	.get_data_device = manager_get_data_device,
};

static void bind_data_device_manager(struct wl_client *client, void *data,
				     uint32_t version, uint32_t id)
{
	struct wl_resource *resource = wl_resource_create(client,
			&wl_data_device_manager_interface, MIN(version, 3), id);
	wl_resource_set_implementation(resource, &manager_implementation, data,
				       NULL);
}

/* --- primary selection manager --- */

static void primary_manager_get_device(struct wl_client *client,
				       struct wl_resource *resource,
				       uint32_t id, struct wl_resource *seat)
{
	struct mw_compositor *comp = wl_resource_get_user_data(resource);
	struct hc_data_device *dev = calloc(1, sizeof *dev);
	(void)seat;
	if (!dev)
		return;
	dev->comp = comp;
	dev->primary = 1;
	dev->resource = wl_resource_create(client,
			&zwp_primary_selection_device_v1_interface,
			wl_resource_get_version(resource), id);
	wl_resource_set_implementation(dev->resource, &primary_device_implementation,
				       dev, data_device_destroyed);
	wl_list_insert(&comp->primary_devices, &dev->link);

	if (comp->primary_selection)
		send_offer_to(dev->resource, 1, comp->primary_selection);
}

static void primary_manager_create_source(struct wl_client *client,
					  struct wl_resource *resource,
					  uint32_t id)
{
	struct hc_data_source *src = calloc(1, sizeof *src);
	if (!src)
		return;
	wl_array_init(&src->mimes);
	wl_list_init(&src->offers);
	src->comp = wl_resource_get_user_data(resource);
	src->primary = 1;
	src->resource = wl_resource_create(client,
			&zwp_primary_selection_source_v1_interface,
			wl_resource_get_version(resource), id);
	wl_resource_set_implementation(src->resource,
				       &primary_source_implementation, src,
				       data_source_destroyed);
}

static const struct zwp_primary_selection_device_manager_v1_interface
primary_manager_implementation = {
	.create_source = primary_manager_create_source,
	.get_device = primary_manager_get_device,
};

static void bind_primary_manager(struct wl_client *client, void *data,
				 uint32_t version, uint32_t id)
{
	struct wl_resource *resource = wl_resource_create(client,
			&zwp_primary_selection_device_manager_v1_interface,
			MIN(version, 1), id);
	wl_resource_set_implementation(resource, &primary_manager_implementation,
				       data, NULL);
}

/* ------------------------------------------------------------------ */
/* wl_output                                                           */
/* ------------------------------------------------------------------ */

static void output_release(struct wl_client *client,
			   struct wl_resource *resource)
{
	(void)client;
	wl_resource_destroy(resource);
}

static const struct wl_output_interface output_implementation = {
	.release = output_release,
};

static void bind_output(struct wl_client *client, void *data,
			uint32_t version, uint32_t id)
{
	struct mw_compositor *comp = data;
	uint32_t v = MIN(version, 3);
	struct mw_device *dev = calloc(1, sizeof *dev);

	if (!dev)
		return;
	dev->resource = wl_resource_create(client, &wl_output_interface, v, id);
	wl_resource_set_implementation(dev->resource, &output_implementation,
				       dev, device_destroyed);
	wl_list_insert(&comp->outputs, &dev->link);

	wl_output_send_geometry(dev->resource, 0, 0, 0, 0,
				WL_OUTPUT_SUBPIXEL_UNKNOWN, "MW", "Headless",
				WL_OUTPUT_TRANSFORM_NORMAL);
	if (v >= 2)
		wl_output_send_name(dev->resource, "MW-1");
	wl_output_send_mode(dev->resource,
			    WL_OUTPUT_MODE_CURRENT | WL_OUTPUT_MODE_PREFERRED,
			    comp->width, comp->height, 60000);
	wl_output_send_scale(dev->resource, comp->scale > 0 ? comp->scale : 1);
	wl_output_send_done(dev->resource);
}

/* ------------------------------------------------------------------ */
/* xdg_wm_base / xdg_positioner / xdg_surface / xdg_toplevel / popup   */
/* ------------------------------------------------------------------ */

static void positioner_destroyed(struct wl_resource *resource)
{
	free(wl_resource_get_user_data(resource));
}

static void positioner_destroy(struct wl_client *client,
			       struct wl_resource *resource)
{
	(void)client;
	wl_resource_destroy(resource);
}

static void positioner_set_size(struct wl_client *client,
				struct wl_resource *resource,
				int32_t width, int32_t height)
{
	struct mw_positioner *pos = wl_resource_get_user_data(resource);
	(void)client;
	if (pos) {
		pos->width = width;
		pos->height = height;
	}
}

static void positioner_set_anchor_rect(struct wl_client *client,
				       struct wl_resource *resource,
				       int32_t x, int32_t y, int32_t width,
				       int32_t height)
{
	(void)client; (void)resource; (void)x; (void)y; (void)width; (void)height;
}

static void positioner_set_anchor(struct wl_client *client,
				  struct wl_resource *resource, uint32_t anchor)
{
	(void)client; (void)resource; (void)anchor;
}

static void positioner_set_gravity(struct wl_client *client,
				   struct wl_resource *resource, uint32_t gravity)
{
	(void)client; (void)resource; (void)gravity;
}

static void positioner_set_constraint_adjustment(struct wl_client *client,
						 struct wl_resource *resource,
						 uint32_t adjustment)
{
	(void)client; (void)resource; (void)adjustment;
}

static void positioner_set_offset(struct wl_client *client,
				  struct wl_resource *resource,
				  int32_t x, int32_t y)
{
	(void)client; (void)resource; (void)x; (void)y;
}

static void positioner_set_reactive(struct wl_client *client,
				    struct wl_resource *resource)
{
	(void)client; (void)resource;
}

static void positioner_set_parent_size(struct wl_client *client,
				       struct wl_resource *resource,
				       int32_t parent_width,
				       int32_t parent_height)
{
	(void)client; (void)resource; (void)parent_width; (void)parent_height;
}

static void positioner_set_parent_configure(struct wl_client *client,
					    struct wl_resource *resource,
					    uint32_t serial)
{
	(void)client; (void)resource; (void)serial;
}

static const struct xdg_positioner_interface positioner_implementation = {
	.destroy = positioner_destroy,
	.set_size = positioner_set_size,
	.set_anchor_rect = positioner_set_anchor_rect,
	.set_anchor = positioner_set_anchor,
	.set_gravity = positioner_set_gravity,
	.set_constraint_adjustment = positioner_set_constraint_adjustment,
	.set_offset = positioner_set_offset,
	.set_reactive = positioner_set_reactive,
	.set_parent_size = positioner_set_parent_size,
	.set_parent_configure = positioner_set_parent_configure,
};

static void wm_base_destroy(struct wl_client *client,
			    struct wl_resource *resource)
{
	(void)client;
	wl_resource_destroy(resource);
}

static void wm_base_create_positioner(struct wl_client *client,
				      struct wl_resource *resource, uint32_t id)
{
	struct mw_positioner *pos = calloc(1, sizeof *pos);
	struct wl_resource *res;

	if (!pos)
		return;
	res = wl_resource_create(client, &xdg_positioner_interface,
				 wl_resource_get_version(resource), id);
	wl_resource_set_implementation(res, &positioner_implementation, pos,
				       positioner_destroyed);
}

/*
 * The xdg_surface / xdg_toplevel / xdg_popup implementation tables are
 * defined further down, after their handler functions.  Tentative
 * declarations here let the wm_base request handlers install them.
 */
static const struct xdg_surface_interface xdg_surface_implementation;
static const struct xdg_toplevel_interface toplevel_implementation;
static const struct xdg_popup_interface popup_implementation;
static void xdg_surface_destroyed(struct wl_resource *resource);
static void toplevel_destroyed(struct wl_resource *resource);
static void popup_destroyed(struct wl_resource *resource);

static void wm_base_get_xdg_surface(struct wl_client *client,
				    struct wl_resource *resource, uint32_t id,
				    struct wl_resource *surface_resource)
{
	struct mw_compositor *comp = wl_resource_get_user_data(resource);
	struct mw_surface *surface = wl_resource_get_user_data(surface_resource);
	struct mw_xdg_surface *xdg = calloc(1, sizeof *xdg);

	if (!xdg)
		return;
	xdg->comp = comp;
	xdg->surface = surface;
	xdg->resource = wl_resource_create(client, &xdg_surface_interface,
					   wl_resource_get_version(resource), id);
	wl_resource_set_implementation(xdg->resource, &xdg_surface_implementation,
				       xdg, xdg_surface_destroyed);
	if (surface)
		surface->xdg_surface = xdg;
}

/* The xdg_surface handlers referenced by that table are defined below. */
static void xdg_surface_destroy(struct wl_client *client,
				struct wl_resource *resource);
static void xdg_surface_get_toplevel(struct wl_client *client,
				     struct wl_resource *resource, uint32_t id);
static void xdg_surface_get_popup(struct wl_client *client,
				  struct wl_resource *resource, uint32_t id,
				  struct wl_resource *parent,
				  struct wl_resource *positioner);
static void xdg_surface_set_window_geometry(struct wl_client *client,
					    struct wl_resource *resource,
					    int32_t x, int32_t y,
					    int32_t width, int32_t height);
static void xdg_surface_ack_configure(struct wl_client *client,
				      struct wl_resource *resource,
				      uint32_t serial);

static const struct xdg_surface_interface xdg_surface_implementation = {
	.destroy = xdg_surface_destroy,
	.get_toplevel = xdg_surface_get_toplevel,
	.get_popup = xdg_surface_get_popup,
	.set_window_geometry = xdg_surface_set_window_geometry,
	.ack_configure = xdg_surface_ack_configure,
};

static void wm_base_pong(struct wl_client *client, struct wl_resource *resource,
			 uint32_t serial)
{
	(void)client; (void)resource; (void)serial;
}

static const struct xdg_wm_base_interface wm_base_implementation = {
	.destroy = wm_base_destroy,
	.create_positioner = wm_base_create_positioner,
	.get_xdg_surface = wm_base_get_xdg_surface,
	.pong = wm_base_pong,
};

/* ------------------------------------------------------------------ */
/* zxdg_decoration_manager_v1                                          */
/*                                                                     */
/* A modern compositor -- KWin included -- draws the frame, the        */
/* titlebar and owns interactive resizing, and tells the client so     */
/* through this protocol.  The shim asks for server-side decorations   */
/* and then leaves the whole frame to us.                              */
/* ------------------------------------------------------------------ */

static void deco_destroy(struct wl_client *client, struct wl_resource *resource)
{
	(void)client;
	wl_resource_destroy(resource);
}

static void deco_set_mode(struct wl_client *client, struct wl_resource *resource,
			  uint32_t mode)
{
	struct mw_xdg_toplevel *toplevel = wl_resource_get_user_data(resource);
	uint32_t answer;
	(void)client;

	/* We can decorate: always answer server-side.  A client that asked
	 * for client-side is told so and falls back to drawing its own. */
	answer = ZXDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE;
	(void)mode;
	if (toplevel)
		toplevel->deco = resource;
	zxdg_toplevel_decoration_v1_send_configure(resource, answer);
}

static const struct zxdg_toplevel_decoration_v1_interface deco_implementation = {
	.destroy = deco_destroy,
	.set_mode = deco_set_mode,
};

static void deco_manager_destroy(struct wl_client *client,
				 struct wl_resource *resource)
{
	(void)client;
	wl_resource_destroy(resource);
}

static void deco_manager_get(struct wl_client *client,
			     struct wl_resource *resource, uint32_t id,
			     struct wl_resource *toplevel_resource)
{
	struct mw_xdg_toplevel *toplevel =
		wl_resource_get_user_data(toplevel_resource);
	struct wl_resource *deco = wl_resource_create(client,
			&zxdg_toplevel_decoration_v1_interface, 1, id);

	if (!deco)
		return;
	wl_resource_set_implementation(deco, &deco_implementation, toplevel,
				       NULL);
	if (toplevel)
		toplevel->deco = deco;
}

static const struct zxdg_decoration_manager_v1_interface deco_manager_implementation = {
	.destroy = deco_manager_destroy,
	.get_toplevel_decoration = deco_manager_get,
};

static void bind_deco_manager(struct wl_client *client, void *data,
			      uint32_t version, uint32_t id)
{
	struct wl_resource *resource = wl_resource_create(client,
			&zxdg_decoration_manager_v1_interface, MIN(version, 1), id);
	wl_resource_set_implementation(resource, &deco_manager_implementation,
				       data, NULL);
	(void)version;
}

static void bind_wm_base(struct wl_client *client, void *data,
			 uint32_t version, uint32_t id)
{
	struct wl_resource *resource = wl_resource_create(client,
			&xdg_wm_base_interface, MIN(version, 6), id);
	wl_resource_set_implementation(resource, &wm_base_implementation, data, NULL);
}

static void xdg_surface_destroyed(struct wl_resource *resource)
{
	struct mw_xdg_surface *xdg = wl_resource_get_user_data(resource);

	if (!xdg)
		return;
	if (xdg->surface)
		xdg->surface->xdg_surface = NULL;
	if (xdg->toplevel)
		xdg->toplevel->xdg_surface = NULL;
	free(xdg);
}

static void xdg_surface_destroy(struct wl_client *client,
				struct wl_resource *resource)
{
	(void)client;
	wl_resource_destroy(resource);
}

static void xdg_surface_get_toplevel(struct wl_client *client,
				     struct wl_resource *resource, uint32_t id)
{
	struct mw_xdg_surface *xdg = wl_resource_get_user_data(resource);
	struct mw_xdg_toplevel *toplevel = calloc(1, sizeof *toplevel);

	if (!xdg || !toplevel)
		return;
	toplevel->comp = xdg->comp;
	toplevel->xdg_surface = xdg;
	toplevel->resource = wl_resource_create(client, &xdg_toplevel_interface,
				wl_resource_get_version(resource), id);
	wl_resource_set_implementation(toplevel->resource, &toplevel_implementation,
				       toplevel, toplevel_destroyed);
	xdg->toplevel = toplevel;
	if (xdg->surface)
		xdg->surface->is_toplevel = 1;
}

static void xdg_surface_get_popup(struct wl_client *client,
				  struct wl_resource *resource, uint32_t id,
				  struct wl_resource *parent,
				  struct wl_resource *positioner)
{
	struct mw_xdg_surface *xdg = wl_resource_get_user_data(resource);
	struct mw_positioner *pos = positioner ?
		wl_resource_get_user_data(positioner) : NULL;
	struct mw_popup *popup = calloc(1, sizeof *popup);
	int32_t w, h;
	(void)parent;

	if (!xdg || !popup)
		return;
	popup->comp = xdg->comp;
	popup->xdg_surface = xdg;
	if (xdg->surface) xdg->surface->is_popup = 1;
	popup->resource = wl_resource_create(client, &xdg_popup_interface,
				wl_resource_get_version(resource), id);
	wl_resource_set_implementation(popup->resource, &popup_implementation,
				       popup, popup_destroyed);
	xdg->popup = popup;

	w = pos && pos->width > 0 ? pos->width : 1;
	h = pos && pos->height > 0 ? pos->height : 1;
	popup->w = w;
	popup->h = h;
	/* No configure here: like every xdg-shell role, the client must first
	 * commit with no buffer attached.  send_xdg_configure() runs from
	 * surface_commit(). */

	if (xdg->surface)
		send_enter_if_needed(xdg->surface);
}

static void xdg_surface_set_window_geometry(struct wl_client *client,
					    struct wl_resource *resource,
					    int32_t x, int32_t y,
					    int32_t width, int32_t height)
{
	struct mw_xdg_surface *xdg = wl_resource_get_user_data(resource);
	(void)client;
	if (!xdg || !xdg->surface)
		return;
	xdg->surface->geom_x = x;
	xdg->surface->geom_y = y;
	xdg->surface->geom_w = width;
	xdg->surface->geom_h = height;
}

static void xdg_surface_ack_configure(struct wl_client *client,
				      struct wl_resource *resource,
				      uint32_t serial)
{
	struct mw_xdg_surface *xdg = wl_resource_get_user_data(resource);
	(void)client; (void)serial;
	if (xdg)
		xdg->acked = 1;
}

static void toplevel_destroyed(struct wl_resource *resource)
{
	struct mw_xdg_toplevel *toplevel = wl_resource_get_user_data(resource);

	if (!toplevel)
		return;
	if (toplevel->xdg_surface)
		toplevel->xdg_surface->toplevel = NULL;
	free(toplevel->title);
	free(toplevel->app_id);
	free(toplevel);
}

static void toplevel_destroy(struct wl_client *client,
			     struct wl_resource *resource)
{
	(void)client;
	wl_resource_destroy(resource);
}

static void toplevel_set_parent(struct wl_client *client,
				struct wl_resource *resource,
				struct wl_resource *parent)
{
	(void)client; (void)resource; (void)parent;
}

static void toplevel_set_title(struct wl_client *client,
			       struct wl_resource *resource,
			       const char *title)
{
	struct mw_xdg_toplevel *toplevel = wl_resource_get_user_data(resource);
	(void)client;
	if (!toplevel)
		return;
	free(toplevel->title);
	toplevel->title = title ? strdup(title) : NULL;
}

static void toplevel_set_app_id(struct wl_client *client,
				struct wl_resource *resource,
				const char *app_id)
{
	struct mw_xdg_toplevel *toplevel = wl_resource_get_user_data(resource);
	(void)client;
	if (!toplevel)
		return;
	free(toplevel->app_id);
	toplevel->app_id = app_id ? strdup(app_id) : NULL;
}

static void toplevel_show_window_menu(struct wl_client *client,
				      struct wl_resource *resource,
				      struct wl_resource *seat, uint32_t serial,
				      int32_t x, int32_t y)
{
	(void)client; (void)resource; (void)seat; (void)serial; (void)x; (void)y;
}

static void toplevel_move(struct wl_client *client,
			  struct wl_resource *resource,
			  struct wl_resource *seat, uint32_t serial)
{
	(void)client; (void)resource; (void)seat; (void)serial;
}

static void toplevel_resize(struct wl_client *client,
			    struct wl_resource *resource,
			    struct wl_resource *seat, uint32_t serial,
			    uint32_t edges)
{
	(void)client; (void)resource; (void)seat; (void)serial; (void)edges;
}

static void toplevel_set_max_size(struct wl_client *client,
				  struct wl_resource *resource,
				  int32_t width, int32_t height)
{
	(void)client; (void)resource; (void)width; (void)height;
}

static void toplevel_set_min_size(struct wl_client *client,
				  struct wl_resource *resource,
				  int32_t width, int32_t height)
{
	(void)client; (void)resource; (void)width; (void)height;
}

static void toplevel_set_maximized(struct wl_client *client,
				   struct wl_resource *resource)
{
	(void)client; (void)resource;
}

static void toplevel_unset_maximized(struct wl_client *client,
				     struct wl_resource *resource)
{
	(void)client; (void)resource;
}

static void toplevel_set_fullscreen(struct wl_client *client,
				    struct wl_resource *resource,
				    struct wl_resource *output)
{
	(void)client; (void)resource; (void)output;
}

static void toplevel_unset_fullscreen(struct wl_client *client,
				      struct wl_resource *resource)
{
	(void)client; (void)resource;
}

static void toplevel_set_minimized(struct wl_client *client,
				   struct wl_resource *resource)
{
	(void)client; (void)resource;
}

static const struct xdg_toplevel_interface toplevel_implementation = {
	.destroy = toplevel_destroy,
	.set_parent = toplevel_set_parent,
	.set_title = toplevel_set_title,
	.set_app_id = toplevel_set_app_id,
	.show_window_menu = toplevel_show_window_menu,
	.move = toplevel_move,
	.resize = toplevel_resize,
	.set_max_size = toplevel_set_max_size,
	.set_min_size = toplevel_set_min_size,
	.set_maximized = toplevel_set_maximized,
	.unset_maximized = toplevel_unset_maximized,
	.set_fullscreen = toplevel_set_fullscreen,
	.unset_fullscreen = toplevel_unset_fullscreen,
	.set_minimized = toplevel_set_minimized,
};

static void popup_destroyed(struct wl_resource *resource)
{
	struct mw_popup *popup = wl_resource_get_user_data(resource);

	if (!popup)
		return;
	if (popup->xdg_surface)
		popup->xdg_surface->popup = NULL;
	free(popup);
}

static void popup_destroy(struct wl_client *client,
			  struct wl_resource *resource)
{
	(void)client;
	wl_resource_destroy(resource);
}

static void popup_grab(struct wl_client *client, struct wl_resource *resource,
		       struct wl_resource *seat, uint32_t serial)
{
	(void)client; (void)resource; (void)seat; (void)serial;
}

static void popup_reposition(struct wl_client *client,
			     struct wl_resource *resource,
			     struct wl_resource *positioner, uint32_t token)
{
	(void)client; (void)resource; (void)positioner;
	xdg_popup_send_repositioned(resource, token);
}

static const struct xdg_popup_interface popup_implementation = {
	.destroy = popup_destroy,
	.grab = popup_grab,
	.reposition = popup_reposition,
};

/* ------------------------------------------------------------------ */
/* Scripted input                                                      */
/* ------------------------------------------------------------------ */

static uint32_t parse_press_state(const char *s)
{
	if (!s)
		return 0;
	if (!strcasecmp(s, "press") || !strcmp(s, "1") || !strcasecmp(s, "pressed"))
		return WL_POINTER_BUTTON_STATE_PRESSED;
	return WL_POINTER_BUTTON_STATE_RELEASED;
}

static uint32_t parse_button_code(const char *s)
{
	if (!s)
		return 0;
	if (!strcasecmp(s, "left") || !strcasecmp(s, "btn_left"))
		return 0x110;
	if (!strcasecmp(s, "right") || !strcasecmp(s, "btn_right"))
		return 0x111;
	if (!strcasecmp(s, "middle") || !strcasecmp(s, "btn_middle"))
		return 0x112;
	if (!strcasecmp(s, "back") || !strcasecmp(s, "btn_side"))
		return 0x116;
	if (!strcasecmp(s, "forward") || !strcasecmp(s, "btn_extra"))
		return 0x115;
	return (uint32_t)strtoul(s, NULL, 0);
}

static void parse_script(struct mw_compositor *comp, const char *path)
{
	FILE *fp = fopen(path, "r");
	char line[256];
	int cap = 16;
	int pending_delay = -1;     /* set by "sleep", applies to the next action */

	if (!fp) {
		fprintf(stderr, "headless-compositor: cannot read %s: %s\n",
			path, strerror(errno));
		return;
	}

	comp->actions = malloc((size_t)cap * sizeof *comp->actions);
	if (!comp->actions) {
		fclose(fp);
		return;
	}

	while (fgets(line, sizeof line, fp)) {
		char *save = NULL, *tok, *hash = strchr(line, '#');
		struct mw_action a;

		if (hash)
			*hash = '\0';
		tok = strtok_r(line, " \t\r\n", &save);
		if (!tok)
			continue;

		if (!strcmp(tok, "sleep")) {
			char *ms = strtok_r(NULL, " \t\r\n", &save);
			pending_delay = ms ? atoi(ms) : 100;
			continue;
		}

		memset(&a, 0, sizeof a);
		a.delay_ms = pending_delay >= 0 ? pending_delay : SCRIPT_STEP_MS;
		pending_delay = -1;

		if (!strcmp(tok, "motion")) {
			char *x = strtok_r(NULL, " \t\r\n", &save);
			char *y = strtok_r(NULL, " \t\r\n", &save);
			if (!x || !y)
				continue;
			a.kind = ACT_MOTION;
			a.x = atoi(x);
			a.y = atoi(y);
			a.has_xy = 1;
		} else if (!strcmp(tok, "button")) {
			char *st = strtok_r(NULL, " \t\r\n", &save);
			char *btn = strtok_r(NULL, " \t\r\n", &save);
			if (!st || !btn)
				continue;
			a.kind = ACT_BUTTON;
			a.state = parse_press_state(st);
			a.code = parse_button_code(btn);
		} else if (!strcmp(tok, "key")) {
			char *st = strtok_r(NULL, " \t\r\n", &save);
			char *code = strtok_r(NULL, " \t\r\n", &save);
			if (!st || !code)
				continue;
			a.kind = ACT_KEY;
			a.state = parse_press_state(st);
			a.code = (uint32_t)strtoul(code, NULL, 0);
		} else if (!strcmp(tok, "axis")) {
			/* axis vertical|horizontal NOTCHES: a wheel detent, positive
			 * meaning down/right, as a compositor would deliver it. */
			char *ax = strtok_r(NULL, " \t\r\n", &save);
			char *n = strtok_r(NULL, " \t\r\n", &save);
			if (!ax || !n)
				continue;
			a.kind = ACT_AXIS;
			a.code = (!strcasecmp(ax, "horizontal") ||
				  !strcmp(ax, "h")) ? 1 : 0;
			a.value = atoi(n);
		} else if (!strcmp(tok, "touch")) {
			/* A complete tap: down, a small motion, up. */
			char *x = strtok_r(NULL, " \t\r\n", &save);
			char *y = strtok_r(NULL, " \t\r\n", &save);
			if (!x || !y)
				continue;
			a.kind = ACT_TOUCH;
			a.x = atoi(x);
			a.y = atoi(y);
			a.has_xy = 1;
		} else if (!strcmp(tok, "frame")) {
			a.kind = ACT_FRAME;
		} else if (!strcmp(tok, "screenshot")) {
			a.kind = ACT_SCREENSHOT;
		} else if (!strcmp(tok, "popupdone")) {
			/* Dismiss the active popup the way a compositor does. */
			a.kind = ACT_POPUPDONE;
		} else if (!strcmp(tok, "resize")) {
			/* Compositor-driven resize of the focused toplevel. */
			char *x = strtok_r(NULL, " \t\r\n", &save);
			char *y = strtok_r(NULL, " \t\r\n", &save);
			if (!x || !y)
				continue;
			a.kind = ACT_RESIZE;
			a.x = atoi(x);
			a.y = atoi(y);
			a.has_xy = 1;
		} else {
			continue;
		}

		if (comp->action_count == cap) {
			struct mw_action *grown;
			cap *= 2;
			grown = realloc(comp->actions,
					(size_t)cap * sizeof *comp->actions);
			if (!grown) {
				comp->action_count = 0;
				break;
			}
			comp->actions = grown;
		}
		comp->actions[comp->action_count++] = a;
	}
	fclose(fp);
}

static struct mw_device *pointer_for_surface(struct mw_compositor *comp,
					     struct mw_surface *surface)
{
	struct mw_device *dev;

	if (surface)
		dev = device_for_client(&comp->pointers,
					wl_resource_get_client(surface->resource));
	else
		dev = NULL;
	return dev ? dev : first_device(&comp->pointers);
}

static struct mw_device *touch_for_surface(struct mw_compositor *comp,
					   struct mw_surface *surface)
{
	struct mw_device *dev;

	if (surface)
		dev = device_for_client(&comp->touches,
					wl_resource_get_client(surface->resource));
	else
		dev = NULL;
	return dev ? dev : first_device(&comp->touches);
}

static struct mw_device *keyboard_for_surface(struct mw_compositor *comp,
					      struct mw_surface *surface)
{
	struct mw_device *dev;

	if (surface)
		dev = device_for_client(&comp->keyboards,
					wl_resource_get_client(surface->resource));
	else
		dev = NULL;
	return dev ? dev : first_device(&comp->keyboards);
}

static void pointer_enter(struct mw_device *dev, struct mw_surface *surface,
			  int x, int y)
{
	uint32_t serial = wl_display_next_serial(surface->comp->display);
	wl_pointer_send_enter(dev->resource, serial, surface->resource,
			      wl_fixed_from_int(x), wl_fixed_from_int(y));
	dev->entered = 1;
}

static void pointer_leave(struct mw_device *dev, struct mw_surface *surface)
{
	if (!dev || !surface) return;
	wl_pointer_send_leave(dev->resource,
			      wl_display_next_serial(surface->comp->display),
			      surface->resource);
	dev->entered = 0;
	dev->entered_surface = NULL;
}

/* Move pointer focus to `surface`, sending leave/enter as a compositor does
 * when the pointer crosses between surfaces (e.g. into an open popup). */
static void pointer_focus(struct mw_device *dev, struct mw_surface *surface,
			  int x, int y)
{
	if (!dev || !surface) return;
	if (dev->entered_surface == surface) return;
	if (dev->entered_surface) pointer_leave(dev, dev->entered_surface);
	pointer_enter(dev, surface, x, y);
	dev->entered_surface = surface;
}

static void keyboard_enter(struct mw_device *dev, struct mw_surface *surface)
{
	struct wl_array keys;
	uint32_t serial = wl_display_next_serial(surface->comp->display);

	wl_array_init(&keys);
	wl_keyboard_send_enter(dev->resource, serial, surface->resource, &keys);
	wl_array_release(&keys);
	wl_keyboard_send_modifiers(dev->resource,
				   wl_display_next_serial(surface->comp->display),
				   0, 0, 0, 0);
	dev->entered = 1;
}

static void execute_action(struct mw_compositor *comp, struct mw_action *a)
{
	/* The open popup, if any, keeps the focus; otherwise the most recently
	 * mapped ordinary toplevel is the active window. */
	struct mw_surface *surface = comp->popup_surface ?
		comp->popup_surface : fallback_toplevel(comp, NULL);
	if (getenv("HC_TRACE"))
		fprintf(stderr, "HC: action kind=%d surface=%p %dx%d\n",
			a->kind, (void*)surface, surface ? surface->buf_w : 0,
			surface ? surface->buf_h : 0);

	switch (a->kind) {
	case ACT_MOTION: {
		struct mw_device *dev = pointer_for_surface(comp, surface);
		if (!dev || !surface)
			return;
		comp->ptr_x = a->x;
		comp->ptr_y = a->y;
		pointer_focus(dev, surface, a->x, a->y);
		wl_pointer_send_motion(dev->resource, now_ms(),
				       wl_fixed_from_int(a->x),
				       wl_fixed_from_int(a->y));
		wl_pointer_send_frame(dev->resource);
		if (comp->drag_active) {
			if (surface != comp->drag_surface) {
				drag_leave(comp);
				drag_enter(comp, surface, a->x, a->y);
			} else {
				drag_motion(comp, a->x, a->y);
			}
		}
		break;
	}
	case ACT_BUTTON: {
		struct mw_device *dev = pointer_for_surface(comp, surface);
		int bx, by;

		if (!dev || !surface)
			return;
		if (comp->drag_active && a->state == WL_POINTER_BUTTON_STATE_RELEASED) {
			drag_drop(comp);
			break;
		}
		/* Clicking outside an open popup dismisses it: the compositor
		 * sends xdg_popup.popup_done and the client is expected to take
		 * the popup down.  Without this the popup would stay up forever
		 * and a client that handles popup_done (as Motif consumers do)
		 * would never be exercised. */
		if (comp->popup_surface && a->state == WL_POINTER_BUTTON_STATE_PRESSED &&
		    (comp->popup_surface != surface ||
		     a->x < 0 || a->y < 0 ||
		     a->x >= comp->popup_surface->buf_w ||
		     a->y >= comp->popup_surface->buf_h)) {
			struct mw_xdg_surface *pxdg = comp->popup_surface->xdg_surface;
			if (pxdg && pxdg->popup) {
				xdg_popup_send_popup_done(pxdg->popup->resource);
				comp->popup_surface = NULL;
				comp->input_surface = comp->last_toplevel;
			}
		}
		/* A button with no coordinates of its own acts where the pointer
		 * already is, so scripts read as "move there, then click". */
		bx = a->has_xy ? a->x : comp->ptr_x;
		by = a->has_xy ? a->y : comp->ptr_y;
		comp->ptr_x = bx;
		comp->ptr_y = by;
		pointer_focus(dev, surface, bx, by);
		wl_pointer_send_button(dev->resource,
				       wl_display_next_serial(comp->display),
				       now_ms(), a->code, a->state);
		wl_pointer_send_frame(dev->resource);
		break;
	}
	case ACT_KEY: {
		struct mw_device *dev = keyboard_for_surface(comp, surface);
		if (!dev || !surface)
			return;
		/* Follow the input target: when the active window or popup
		 * changes (a dialog opened from a menu, say), send the old
		 * surface a leave and enter the new one, so the client routes
		 * keys to the window that is actually in front. */
		if (!dev->entered || dev->entered_surface != surface) {
			if (dev->entered_surface)
				wl_keyboard_send_leave(dev->resource,
						       wl_display_next_serial(comp->display),
						       dev->entered_surface->resource);
			keyboard_enter(dev, surface);
			dev->entered_surface = surface;
			comp->input_surface = surface;
		}
		wl_keyboard_send_key(dev->resource,
				     wl_display_next_serial(comp->display),
				     now_ms(), a->code, a->state);
		break;
	}
	case ACT_FRAME: {
		struct mw_device *dev = pointer_for_surface(comp, surface);
		if (dev)
			wl_pointer_send_frame(dev->resource);
		break;
	}
	case ACT_AXIS: {
		struct mw_device *dev = pointer_for_surface(comp, surface);
		int ver;
		if (!dev || !surface)
			return;
		pointer_focus(dev, surface, comp->ptr_x, comp->ptr_y);
		ver = wl_resource_get_version(dev->resource);
		/* A wheel detent as a compositor delivers it: the continuous axis
		 * value, the discrete step count (axis_value120 on v8, the older
		 * axis_discrete below that), then the frame that batches them. */
		if (ver >= 5)
			wl_pointer_send_axis_source(dev->resource,
						    WL_POINTER_AXIS_SOURCE_WHEEL);
		wl_pointer_send_axis(dev->resource, now_ms(), a->code,
				     wl_fixed_from_int(a->value * 10));
		if (ver >= 8)
			wl_pointer_send_axis_value120(dev->resource, a->code,
						      a->value * 120);
		else if (ver >= 5)
			wl_pointer_send_axis_discrete(dev->resource, a->code,
						      a->value);
		wl_pointer_send_frame(dev->resource);
		break;
	}
	case ACT_TOUCH: {
		struct mw_device *dev = touch_for_surface(comp, surface);
		uint32_t serial, t;
		if (!dev || !surface)
			return;
		serial = wl_display_next_serial(comp->display);
		t = now_ms();
		/* down, a small motion, up: a complete tap. */
		wl_touch_send_down(dev->resource, serial, t, surface->resource, 0,
				   wl_fixed_from_int(a->x),
				   wl_fixed_from_int(a->y));
		wl_touch_send_frame(dev->resource);
		wl_touch_send_motion(dev->resource, t + 1, 0,
				     wl_fixed_from_int(a->x + 5),
				     wl_fixed_from_int(a->y + 5));
		wl_touch_send_frame(dev->resource);
		wl_touch_send_up(dev->resource, serial, t + 2, 0);
		wl_touch_send_frame(dev->resource);
		break;
	}
	case ACT_SLEEP:
		break;
	case ACT_SCREENSHOT:
		capture_and_exit(comp);
		break;
	case ACT_RESIZE: {
		/* A compositor-driven resize: send a fresh xdg_toplevel.configure
		 * carrying the new size, then the xdg_surface.configure that
		 * completes it.  Only a toplevel can be resized this way. */
		struct mw_xdg_surface *xdg = surface ? surface->xdg_surface : NULL;
		if (xdg && xdg->toplevel) {
			struct wl_array states;
			wl_array_init(&states);
			xdg_toplevel_send_configure(xdg->toplevel->resource,
						    a->x, a->y, &states);
			wl_array_release(&states);
			xdg_surface_send_configure(xdg->resource,
				wl_display_next_serial(comp->display));
			xdg->configured = 1;
		}
		break;
	}
	case ACT_POPUPDONE:
		if (comp->popup_surface) {
			struct mw_xdg_surface *pxdg = comp->popup_surface->xdg_surface;
			if (pxdg && pxdg->popup)
				xdg_popup_send_popup_done(pxdg->popup->resource);
			comp->popup_surface = NULL;
			comp->input_surface = comp->last_toplevel;
		}
		break;
	}
}

static int script_timer_func(void *data)
{
	struct mw_compositor *comp = data;

	if (comp->captured)
		return 0;
	if (comp->action_index >= comp->action_count)
		return 0;

	execute_action(comp, &comp->actions[comp->action_index++]);

	if (comp->captured)
		return 0;
	if (comp->action_index < comp->action_count) {
		int delay = comp->actions[comp->action_index].delay_ms;
		if (delay <= 0)
			delay = SCRIPT_STEP_MS;
		wl_event_source_timer_update(comp->script_timer, delay);
	}
	return 0;
}

static void start_script(struct mw_compositor *comp)
{
	int delay;

	if (comp->script_started || comp->action_count == 0)
		return;
	comp->script_started = 1;
	if (getenv("HC_TRACE"))
		fprintf(stderr, "HC: script start\n");
	delay = comp->actions[0].delay_ms;
	if (delay <= 0)
		delay = SCRIPT_STEP_MS;
	wl_event_source_timer_update(comp->script_timer, delay);
}

/* Is there anything an input action could be delivered to?  Delivering motion or
 * a button to no surface is a silent no-op, so a script that starts before the
 * client has mapped its first toplevel loses those actions -- which looks
 * exactly like a client that ignored the input.  A slow starter (XForms spends
 * its first few hundred milliseconds in Xrm, the font list and the colormap
 * before it maps anything) needs the script to wait for the window. */
static int script_has_target(struct mw_compositor *comp)
{
	return comp->popup_surface != NULL ||
	       fallback_toplevel(comp, NULL) != NULL;
}

static int script_gate_func(void *data)
{
	struct mw_compositor *comp = data;

	/* Wait for a window to exist before delivering input, but not forever:
	 * a client that maps late still gets its script, and one that never
	 * maps at all does not hang the run past the backstop. */
	if (script_has_target(comp) ||
	    now_ms() - comp->script_gate_start_ms >= SCRIPT_BACKSTOP_MS)
		start_script(comp);
	else
		wl_event_source_timer_update(comp->script_gate,
					     SCRIPT_GATE_POLL_MS);
	return 0;
}

/* Called when a surface becomes visible: start the script right away instead of
 * waiting for the next poll, so the first action lands as soon as possible. */
static void script_gate_kick(struct mw_compositor *comp)
{
	if (!comp->script_started && comp->script_gate)
		script_gate_func(comp);
}

/* ------------------------------------------------------------------ */
/* Timers                                                              */
/* ------------------------------------------------------------------ */

static int timeout_func(void *data)
{
	capture_and_exit(data);
	return 0;
}

/* Write P0001.png, P0002.png, ... at a fixed interval without ending the run,
 * so a long-lived client can be recorded as a frame sequence. */
static int capture_timer_func(void *data)
{
	struct mw_compositor *comp = data;
	char path[1024];

	if (comp->captured || !comp->capture_prefix)
		return 0;
	snprintf(path, sizeof path, "%s%04d.png", comp->capture_prefix,
		 ++comp->capture_count);
	save_screenshot_path(comp, path);
	wl_event_source_timer_update(comp->capture_timer, comp->capture_every_ms);
	return 0;
}

static int stdin_func(int fd, uint32_t mask, void *data)
{
	struct mw_compositor *comp = data;
	char buf[512];
	ssize_t n;

	(void)mask;
	n = read(fd, buf, sizeof buf);
	if (n > 0) {
		capture_and_exit(comp);
	} else if (n == 0) {
		if (comp->stdin_source) {
			wl_event_source_remove(comp->stdin_source);
			comp->stdin_source = NULL;
		}
	}
	return 0;
}

static int signal_func(int signal_number, void *data)
{
	struct mw_compositor *comp = data;
	(void)signal_number;
	wl_display_terminate(comp->display);
	return 0;
}

/* ------------------------------------------------------------------ */
/* main                                                                */
/* ------------------------------------------------------------------ */

static void usage(const char *argv0)
{
	fprintf(stderr,
		"usage: %s [--socket NAME] [--size WxH] [--scale N] [--timeout SECONDS]\n"
		"          [--output FILE] [--input FILE]\n"
		"\n"
		"  --socket NAME   Wayland socket in $XDG_RUNTIME_DIR\n"
		"                  (default: $WAYLAND_DISPLAY or \"%s\")\n"
		"  --size WxH      toplevel configure size (default %dx%d)\n"
		"  --scale N       advertise wl_output scale N (HiDPI); default 1\n"
		"  --timeout SEC   seconds before the screenshot (default %d)\n"
		"  --output FILE   PNG path (default %s)\n"
		"  --capture-prefix P   also write P0001.png every --capture-every ms\n"
		"  --capture-every MS   periodic capture interval (default 0 = off)\n"
		"  --input FILE    replay a script before the screenshot:\n"
		"                    motion X Y\n"
		"                    button PRESS|RELEASE BUTTON\n"
		"                    key PRESS|RELEASE KEYCODE\n"
		"                    axis vertical|horizontal NOTCHES\n"
		"                    frame\n"
		"                    sleep MS\n"
		"                    screenshot\n",
		argv0, DEFAULT_SOCKET, DEFAULT_WIDTH, DEFAULT_HEIGHT,
		DEFAULT_TIMEOUT, DEFAULT_OUTPUT);
}

int main(int argc, char **argv)
{
	struct mw_compositor comp;
	const char *socket_name = NULL;
	const char *input_path = NULL;
	int i;

	memset(&comp, 0, sizeof comp);
	comp.width = DEFAULT_WIDTH;
	comp.height = DEFAULT_HEIGHT;
	comp.scale = 1;
	comp.timeout = DEFAULT_TIMEOUT;
	comp.output_path = DEFAULT_OUTPUT;

	for (i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--socket") && i + 1 < argc) {
			socket_name = argv[++i];
		} else if (!strcmp(argv[i], "--size") && i + 1 < argc) {
			int w, h;
			if (sscanf(argv[++i], "%dx%d", &w, &h) != 2 || w <= 0 || h <= 0) {
				fprintf(stderr, "headless-compositor: bad --size\n");
				return 2;
			}
			comp.width = w;
			comp.height = h;
		} else if (!strcmp(argv[i], "--timeout") && i + 1 < argc) {
			comp.timeout = atoi(argv[++i]);
			if (comp.timeout < 0)
				comp.timeout = 0;
		} else if (!strcmp(argv[i], "--scale") && i + 1 < argc) {
			comp.scale = atoi(argv[++i]);
			if (comp.scale < 1)
				comp.scale = 1;
		} else if (!strcmp(argv[i], "--output") && i + 1 < argc) {
			comp.output_path = argv[++i];
		} else if (!strcmp(argv[i], "--input") && i + 1 < argc) {
			input_path = argv[++i];
		} else if (!strcmp(argv[i], "--capture-prefix") && i + 1 < argc) {
			comp.capture_prefix = argv[++i];
		} else if (!strcmp(argv[i], "--capture-every") && i + 1 < argc) {
			comp.capture_every_ms = atoi(argv[++i]);
		} else if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h")) {
			usage(argv[0]);
			return 0;
		} else {
			fprintf(stderr, "headless-compositor: unknown argument '%s'\n",
				argv[i]);
			usage(argv[0]);
			return 2;
		}
	}
	comp.input_path = input_path;

	if (!socket_name || !*socket_name)
		socket_name = getenv("WAYLAND_DISPLAY");
	if (!socket_name || !*socket_name)
		socket_name = DEFAULT_SOCKET;
	comp.socket_name = strdup(socket_name);
	if (!comp.socket_name)
		return 1;

	signal(SIGPIPE, SIG_IGN);

	wl_list_init(&comp.surfaces);
	wl_list_init(&comp.pointers);
	wl_list_init(&comp.keyboards);
	wl_list_init(&comp.touches);
	wl_list_init(&comp.outputs);
	wl_list_init(&comp.data_devices);
	wl_list_init(&comp.primary_devices);
	wl_list_init(&comp.pending_frame_cbs);

	comp.display = wl_display_create();
	if (!comp.display) {
		fprintf(stderr, "headless-compositor: wl_display_create failed\n");
		return 1;
	}
	comp.loop = wl_display_get_event_loop(comp.display);

	/* A real, compiled xkb keymap for wl_keyboard.keymap. */
	comp.xkb_context = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
	if (comp.xkb_context) {
		struct xkb_rule_names names;
		memset(&names, 0, sizeof names);
		comp.xkb_keymap = xkb_keymap_new_from_names(comp.xkb_context,
				&names, XKB_KEYMAP_COMPILE_NO_FLAGS);
	}
	if (comp.xkb_keymap) {
		comp.xkb_keymap_string = xkb_keymap_get_as_string(comp.xkb_keymap,
						XKB_KEYMAP_FORMAT_TEXT_V1);
		if (comp.xkb_keymap_string)
			comp.xkb_keymap_size =
				strlen(comp.xkb_keymap_string) + 1;
	}
	if (!comp.xkb_keymap_string)
		fprintf(stderr,
			"headless-compositor: warning: no xkb keymap available\n");

	wl_global_create(comp.display, &wl_compositor_interface, 5, &comp,
			 bind_compositor);
	wl_global_create(comp.display, &wl_shm_interface, 2, &comp, bind_shm);
	wl_global_create(comp.display, &wl_seat_interface, 7, &comp, bind_seat);
	wl_global_create(comp.display, &wl_data_device_manager_interface, 3,
			 &comp, bind_data_device_manager);
	if (!getenv("HC_NO_PRIMARY"))
		wl_global_create(comp.display,
				 &zwp_primary_selection_device_manager_v1_interface,
				 1, &comp, bind_primary_manager);
	wl_global_create(comp.display, &wp_viewporter_interface, 1, &comp,
			 bind_viewporter);
	/* Text-input v3 (the shim's XIM bridge).  HC_NO_TEXTINPUT mimics a
	 * compositor without it, so the local XIM fallback stays testable. */
	if (!getenv("HC_NO_TEXTINPUT"))
		wl_global_create(comp.display, &zwp_text_input_manager_v3_interface,
				 1, &comp, bind_text_input_manager);
	wl_global_create(comp.display, &wl_output_interface, 3, &comp, bind_output);
	/* HC_NO_DECO mimics a compositor without xdg-decoration (GNOME/Mutter,
	 * for instance), so the client-side fallback path stays testable. */
	if (!getenv("HC_NO_DECO"))
		wl_global_create(comp.display, &zxdg_decoration_manager_v1_interface,
				 1, &comp, bind_deco_manager);
	wl_global_create(comp.display, &xdg_wm_base_interface, 6, &comp,
			 bind_wm_base);

	if (wl_display_add_socket(comp.display, comp.socket_name) < 0) {
		fprintf(stderr,
			"headless-compositor: cannot create socket '%s': %s "
			"(is XDG_RUNTIME_DIR set?)\n",
			comp.socket_name, strerror(errno));
		return 1;
	}

	if (comp.input_path)
		parse_script(&comp, comp.input_path);

	comp.timeout_timer = wl_event_loop_add_timer(comp.loop, timeout_func, &comp);
	if (comp.timeout_timer)
		wl_event_source_timer_update(comp.timeout_timer, comp.timeout * 1000);
	comp.frame_timer = wl_event_loop_add_timer(comp.loop, frame_timer_func, &comp);
	comp.script_timer = wl_event_loop_add_timer(comp.loop, script_timer_func, &comp);
	if (comp.capture_every_ms > 0 && comp.capture_prefix) {
		comp.capture_timer = wl_event_loop_add_timer(comp.loop,
				capture_timer_func, &comp);
		if (comp.capture_timer)
			wl_event_source_timer_update(comp.capture_timer,
					comp.capture_every_ms);
	}
	if (comp.action_count > 0) {
		comp.script_gate_start_ms = now_ms();
		comp.script_gate = wl_event_loop_add_timer(comp.loop,
							   script_gate_func, &comp);
		if (comp.script_gate)
			wl_event_source_timer_update(comp.script_gate,
						     SCRIPT_START_MS);
	}

	/* A line on stdin triggers an early screenshot. */
	comp.stdin_source = wl_event_loop_add_fd(comp.loop, STDIN_FILENO,
			WL_EVENT_READABLE, stdin_func, &comp);

	/* Exit cleanly (and unlink the socket) on the usual CI signals. */
	wl_event_loop_add_signal(comp.loop, SIGINT, signal_func, &comp);
	wl_event_loop_add_signal(comp.loop, SIGTERM, signal_func, &comp);

	printf("READY %s\n", comp.socket_name);
	fflush(stdout);

	wl_display_run(comp.display);

	free(comp.actions);
	free(comp.xkb_keymap_string);
	if (comp.xkb_keymap)
		xkb_keymap_unref(comp.xkb_keymap);
	if (comp.xkb_context)
		xkb_context_unref(comp.xkb_context);
	wl_display_destroy_clients(comp.display);
	wl_display_destroy(comp.display);
	free(comp.socket_name);
	return 0;
}
