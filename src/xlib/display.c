/* display.c — Display lifecycle, screen/visual setup, object registry. */
#include "internal.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <errno.h>
#include <pthread.h>

/* ------------------------------------------------------------- registry */

static size_t hash_id(XID id, size_t cap) { return (size_t)((id * 2654435761u) & (cap - 1)); }

static void table_grow(Display *d)
{
    XDisplayImpl *dp = MWD(d);
    size_t ncap = dp->table_cap ? dp->table_cap * 2 : 256;
    MwEntry *nt = calloc(ncap, sizeof *nt);
    for (size_t i = 0; i < dp->table_cap; i++) {
        if (dp->table[i].id) {
            size_t j = hash_id(dp->table[i].id, ncap);
            while (nt[j].id) j = (j + 1) & (ncap - 1);
            nt[j] = dp->table[i];
        }
    }
    free(dp->table);
    dp->table = nt;
    dp->table_cap = ncap;
}

void mw_register(Display *d, XID id, MwObjKind kind, void *obj)
{
    XDisplayImpl *dp = MWD(d);
    if (!dp->table) table_grow(d);
    if ((dp->table_used + 1) * 4 >= dp->table_cap * 3) table_grow(d);
    size_t j = hash_id(id, dp->table_cap);
    while (dp->table[j].id && dp->table[j].id != id) j = (j + 1) & (dp->table_cap - 1);
    if (!dp->table[j].id) dp->table_used++;
    dp->table[j].id = id;
    dp->table[j].kind = kind;
    dp->table[j].obj = obj;
}

void mw_unregister(Display *d, XID id)
{
    XDisplayImpl *dp = MWD(d);
    if (!dp->table) return;
    /* A buffered Render request may still reference this object (cairo creates
     * a pixmap, pictures it, and frees it -- all before the request reaches us),
     * so dispatch pending requests before the id disappears. */
    if (!dp->render_draining) mw_render_drain(d);
    /* Rebuild the probe table without this id.  A cluster-shifting delete is
     * error-prone (insertions can invalidate the scan and grow the table
     * mid-iteration), so we rehash cleanly. */
    MwEntry *old = dp->table;
    size_t cap = dp->table_cap;
    dp->table = calloc(cap, sizeof(MwEntry));
    dp->table_used = 0;
    for (size_t i = 0; i < cap; i++)
        if (old[i].id && old[i].id != id)
            mw_register(d, old[i].id, old[i].kind, old[i].obj);
    free(old);
}

void *mw_lookup(Display *d, XID id, MwObjKind kind)
{
    XDisplayImpl *dp = MWD(d);
    if (!dp->table || id == 0) return NULL;
    size_t j = hash_id(id, dp->table_cap);
    while (dp->table[j].id) {
        if (dp->table[j].id == id) {
            if (kind == MW_OBJ_FREE || dp->table[j].kind == kind)
                return dp->table[j].obj;
            return NULL;
        }
        j = (j + 1) & (dp->table_cap - 1);
    }
    return NULL;
}

XID mw_alloc_id(Display *d) { return MWD(d)->next_id++; }

MwWindow   *mw_window(Display *d, Window w)       { return mw_lookup(d, w, MW_OBJ_WINDOW); }
MwPixmap   *mw_pixmap(Display *d, Pixmap p)       { return mw_lookup(d, p, MW_OBJ_PIXMAP); }
MwXFont    *mw_font(Display *d, Font f)           { return mw_lookup(d, f, MW_OBJ_FONT); }
MwColormap *mw_colormap(Display *d, Colormap c)   { return mw_lookup(d, c, MW_OBJ_COLORMAP); }
MwCursor   *mw_cursor(Display *d, Cursor c)       { return mw_lookup(d, c, MW_OBJ_CURSOR); }

/* ------------------------------------------------------------- lifecycle */

/* Open displays, so that a repeated XOpenDisplay for the same display name
 * returns the same Display.  This is what Xlib does and what callers depend
 * on: Motif internally calls XOpenDisplay(XDisplayString(d)) for input methods
 * and render tables.  Returning a fresh Display there would hand Motif a
 * second X "server" with its own root window, XID space and Wayland
 * connection, so the widgets built from that handle (a file selection dialog,
 * for example) would be unreachable from the rest of the application. */
static XDisplayImpl *open_displays;
static pthread_mutex_t display_list_lock = PTHREAD_MUTEX_INITIALIZER;

/* XAllocID is the macro (*dpy->resource_alloc)(dpy).  Ecosystem libraries --
 * libXrender above all -- allocate their object ids through it, so the field
 * must hold a real function or they call through NULL. */
static XID mw_display_resource_alloc(Display *d) { return mw_alloc_id(d); }

Display *XOpenDisplay(_Xconst char *display_name)
{
    if (mw_raster_init() != 0) return NULL;

    /* Xlib reports an X display string ("host:display.screen") through
     * XDisplayString() and through the Display's display_name field.  The
     * name is informational here -- we always connect via WAYLAND_DISPLAY --
     * but it must be X-shaped: clients parse it.  CDE's DtSvc, for example,
     * splits it on ':' and dereferences the remainder, so exposing the raw
     * Wayland socket name ("wayland-0") made it crash in GetDisplayName().
     * Derive the name from XOpenDisplay's argument, then $DISPLAY, then a
     * local ":0", and use it as the open-display key so repeated opens of the
     * same display still return the same Display. */
    const char *env = getenv("DISPLAY");
    const char *resolved = display_name && *display_name ? display_name
                          : (env && *env ? env : ":0");

    pthread_mutex_lock(&display_list_lock);
    for (XDisplayImpl *e = open_displays; e; e = e->open_next) {
        if (e->display_name && strcmp(e->display_name, resolved) == 0) {
            e->refcnt++;
            pthread_mutex_unlock(&display_list_lock);
            return (Display *)e;
        }
    }
    pthread_mutex_unlock(&display_list_lock);

    XDisplayImpl *dp = calloc(1, sizeof *dp);
    Display *d = (Display *)dp;

    dp->proto_major_version = 11;
    dp->proto_minor_version = 0;
    dp->vendor = strdup("Motif/Wayland");
    dp->release = 1;
    dp->byte_order = LSBFirst;
    dp->bitmap_unit = 32;
    dp->bitmap_pad = 32;
    dp->bitmap_bit_order = LSBFirst;
    dp->max_request_size = 65535;
    dp->resource_alloc = mw_display_resource_alloc;
    dp->idlist_alloc = NULL;
    /* Output buffer.  The public prefix names these private11..private14, but
     * they are Xlib's last_req/buffer/bufptr/bufmax: extension libraries
     * (libXrender) build requests directly into this buffer through the
     * Xlibint.h macros, so it must be real memory. */
    {
        char *ob = calloc(1, 1u << 20);
        dp->private11 = ob;              /* last_req */
        dp->private12 = ob;              /* buffer  */
        dp->private13 = ob;              /* bufptr  */
        dp->private14 = ob ? ob + (1u << 20) : NULL;   /* bufmax */
    }
    dp->default_screen = 0;
    dp->nscreens = 1;
    dp->min_keycode = 8;
    dp->max_keycode = 255;
    dp->next_id = MW_XID_BASE;
    dp->motion_buffer = 0;
    dp->fd = -1;
    dp->wl_fd = -1;
    dp->auto_repeat = AutoRepeatModeOn;
    dp->display_name = strdup(resolved);

    /* Bind to the compositor.  Xt passes the X DISPLAY name (":0") here; there
     * is no X server, so the name is informational only — we always connect
     * through WAYLAND_DISPLAY / the default Wayland socket. */
    if (mw_wl_connect(dp, NULL) != 0) {
        free(dp->vendor);
        free(dp->display_name);
        free(dp);
        return NULL;
    }

    /* Screen + visual (single TrueColor 24-bit screen). */
    dp->screens = calloc(1, sizeof(Screen));
    Screen *scr = &dp->screens[0];
    dp->screen = scr;

    int sw = dp->output_w > 0 ? dp->output_w : MW_DEFAULT_W;
    int sh = dp->output_h > 0 ? dp->output_h : MW_DEFAULT_H;

    dp->visual.ext_data = NULL;
    dp->visual.visualid = 0x21;
    dp->visual.class = TrueColor;
    dp->visual.red_mask   = 0x00ff0000;
    dp->visual.green_mask = 0x0000ff00;
    dp->visual.blue_mask  = 0x000000ff;
    dp->visual.bits_per_rgb = 8;
    dp->visual.map_entries = 256;

    dp->depths[0].depth = 24;
    dp->depths[0].nvisuals = 1;
    dp->depths[0].visuals = &dp->visual;

    dp->formats[0].ext_data = NULL;
    dp->formats[0].depth = 24;
    dp->formats[0].bits_per_pixel = 32;
    dp->formats[0].scanline_pad = 32;
    dp->nformats = 1;
    dp->pixmap_format = dp->formats;

    scr->ext_data = NULL;
    scr->display = d;
    scr->width = sw;
    scr->height = sh;
    scr->mwidth = (int)(sw * 25.4 / MW_DEFAULT_DPI);
    scr->mheight = (int)(sh * 25.4 / MW_DEFAULT_DPI);
    scr->ndepths = 1;
    scr->depths = dp->depths;
    scr->root_depth = 24;
    scr->root_visual = &dp->visual;
    scr->white_pixel = 0x00ffffff;
    scr->black_pixel = 0x00000000;
    scr->max_maps = 256;
    scr->min_maps = 1;
    scr->backing_store = NotUseful;
    scr->save_unders = False;
    scr->root_input_mask = 0;

    mw_init_atoms(d);

    /* Root window. */
    XSetWindowAttributes rattr;
    memset(&rattr, 0, sizeof rattr);
    rattr.background_pixel = scr->black_pixel;
    rattr.event_mask = 0;
    MwWindow *root = mw_create_window(d, None, 0, 0, sw, sh, 0, 24,
                                      InputOutput, &dp->visual,
                                      CWBackPixel | CWEventMask, &rattr);
    scr->root = root->id;

    /* Default colormap + GC. */
    scr->cmap = XCreateColormap(d, root->id, &dp->visual, AllocNone);
    scr->default_gc = XCreateGC(d, root->id, 0, NULL);

    /* Synthetic WM window + workspace properties (CDE Workspace Manager). */
    mw_init_wm_window(d);

    /* Resources, keymap, selections, cursors, IMs, extensions. */
    XrmInitialize();
    dp->xdefaults = NULL;
    mw_load_resources(d);
    mw_keymap_init(d, NULL);
    mw_init_selection(d);
    mw_clipboard_init(d);
    mw_cursor_init(d);
    mw_xim_init(d);
    mw_xrandr_init(d);
    mw_shape_init(d);

    /* The keymap comes from the compositor, but keymap-aware clients read the
     * layout names off the root window -- `setxkbmap -query` prints these. */
    {
        Atom xkbrules = XInternAtom(d, "_XKB_RULES_NAMES", False);
        static const char rules[] = "evdev\0pc105\0us\0\0";
        XChangeProperty(d, root->id, xkbrules, XA_STRING, 8, PropModeReplace,
                        (const unsigned char *)rules, (int)sizeof rules);
    }

    dp->error_handler = NULL;
    dp->io_error_handler = NULL;
    dp->ptr_x = sw / 2;
    dp->ptr_y = sh / 2;
    dp->owns_display = true;
    dp->serial = 1;
    dp->refcnt = 1;

    /* Publish in the open-display list so a later XOpenDisplay for the same
     * name returns this very Display instead of a second, independent one. */
    pthread_mutex_lock(&display_list_lock);
    dp->open_next = open_displays;
    open_displays = dp;
    pthread_mutex_unlock(&display_list_lock);

    return d;
}

int XCloseDisplay(Display *d)
{
    if (!d) return 1;
    XDisplayImpl *dp = MWD(d);

    /* Xlib semantics: closing a shared display just drops a reference.  The
     * connection is only torn down once the last reference goes away. */
    pthread_mutex_lock(&display_list_lock);
    if (dp->refcnt > 1) {
        dp->refcnt--;
        pthread_mutex_unlock(&display_list_lock);
        return 1;
    }
    pthread_mutex_unlock(&display_list_lock);

    /* Tear down remaining windows (top-levels first, then children). */
    for (size_t i = 0; i < dp->table_cap; i++) {
        if (dp->table[i].id && dp->table[i].kind == MW_OBJ_WINDOW) {
            MwWindow *w = dp->table[i].obj;
            if (w && w->tl) mw_toplevel_destroy(w->tl);
            w->tl = NULL;
        }
    }
    for (size_t i = 0; i < dp->table_cap; i++) {
        MwEntry e = dp->table[i];
        if (!e.id) continue;
        switch (e.kind) {
        case MW_OBJ_WINDOW: {
            MwWindow *w = e.obj;
            if (w) { if (w->surface) mw_surface_destroy(w->surface);
                     if (w->has_shape) pixman_region32_fini(&w->shape);
                     free(w); }
            break; }
        case MW_OBJ_PIXMAP: {
            MwPixmap *p = e.obj;
            if (p) { if (p->surface) mw_surface_destroy(p->surface); free(p); }
            break; }
        case MW_OBJ_GC: free(e.obj); break;
        case MW_OBJ_FONT: {
            MwXFont *f = e.obj;
            if (f) {
                if (f->fs) { free(f->fs->per_char); free(f->fs); }
                if (f->rfont) mw_font_destroy(f->rfont);
                free(f);
            }
            break; }
        case MW_OBJ_COLORMAP: free(e.obj); break;
        case MW_OBJ_CURSOR: free(e.obj); break;
        default: break;
        }
    }

    /* Last reference: take it out of the open-display list. */
    pthread_mutex_lock(&display_list_lock);
    for (XDisplayImpl **pp = &open_displays; *pp; pp = &(*pp)->open_next) {
        if (*pp == dp) { *pp = dp->open_next; break; }
    }
    pthread_mutex_unlock(&display_list_lock);

    mw_cursor_fini(d);
    mw_fini_selection(d);
    mw_keymap_fini(d);
    mw_wl_disconnect(dp);

    if (dp->atom_names) {
        for (int i = 0; i < dp->natoms; i++) free(dp->atom_names[i]);
        free(dp->atom_names);
    }
    free(dp->atom_buf);
    free(dp->queue);
    free(dp->table);
    free(dp->screens);
    free(dp->vendor);
    free(dp->display_name);
    free(dp->xdefaults);
    for (int i = 0; i < 8; i++) free(dp->cutbuf[i]);
    free(dp);
    return 1;
}

/* --------------------------------------------------------- misc server ops */

char *XDisplayName(_Xconst char *string)
{
    if (string && *string) return (char *)string;
    char *w = getenv("WAYLAND_DISPLAY");
    if (w) return w;
    return getenv("DISPLAY");
}

int XSetCloseDownMode(Display *d, int mode) { (void)d; (void)mode; return 0; }

int XScreenNumberOfScreen(Screen *screen) { (void)screen; return 0; }
Screen *XScreenOfDisplay(Display *d, int scr) { (void)scr; return MWSCR(d); }
Display *XDisplayOfScreen(Screen *s) { return s->display; }
VisualID XVisualIDFromVisual(Visual *v) { return v->visualid; }

/* How many motion events the server buffers for this client.  There is no
 * server-side event queue here, so report the value carried in the display
 * structure (zero, i.e. motion is not buffered); xdpyinfo prints it. */
unsigned long XDisplayMotionBufferSize(Display *d) { return MWD(d)->motion_buffer; }

int XFlush(Display *d)
{
    XDisplayImpl *dp = MWD(d);
    /* Dispatch any buffered extension requests (Render) before the commit. */
    mw_render_drain(d);
    /* XFlush makes pending drawing visible, so commit accumulated damage
     * before flushing the connection.  The commit is capped to one frame per
     * refresh (and arms a frame callback when it defers), so a repaint built
     * from many drawing calls still presents as a single frame.
     *
     * Without this, drawing triggered by a source that never reaches the
     * shim's own event pump was never committed: xterm repaints in response to
     * pty output and then calls XFlush "before waiting", so on a compositor
     * that sends no further traffic the window stayed frozen even though the
     * application was alive and still reading its pty. */
    mw_flush_damage_deferred(d);
    if (dp->wl_display) wl_display_flush(dp->wl_display);
    return 1;
}

int XSync(Display *d, Bool discard)
{
    XDisplayImpl *dp = MWD(d);
    (void)discard;
    mw_render_drain(d);
    if (getenv("MW_TRACE")) fprintf(stderr, "MW: XSync\n");
    if (dp->wl_display) {
        wl_display_flush(dp->wl_display);
        mw_flush_damage(d);
        wl_display_dispatch_pending(dp->wl_display);
        wl_display_roundtrip(dp->wl_display);
    }
    return 1;
}

int (*XSynchronize(Display *d, Bool on)) (Display *)
{
    XDisplayImpl *dp = MWD(d);
    dp->syncing = on;
    return NULL;
}

int (*XSetErrorHandler(int (*handler)(Display *, XErrorEvent *)))(Display *, XErrorEvent *)
{
    XErrorHandler old = mw_error_handler();
    mw_set_error_handler(handler);
    return old;
}

int (*XSetIOErrorHandler(int (*handler)(Display *)))(Display *)
{
    XIOErrorHandler old = mw_io_error_handler_get();
    mw_set_io_error_handler(handler);
    return old;
}

int XGetErrorText(Display *d, int code, char *buffer, int length)
{
    (void)d;
    snprintf(buffer, length, "X error %d", code);
    return 0;
}

int XGetErrorDatabaseText(Display *d, _Xconst char *name, _Xconst char *msg,
                          _Xconst char *def, char *buf, int len)
{
    (void)d; (void)name; (void)msg;
    snprintf(buf, len, "%s", def ? def : "");
    return 0;
}

char *XResourceManagerString(Display *d) { return MWD(d)->xdefaults; }
char *XScreenResourceString(Screen *screen) { (void)screen; return NULL; }

long XMaxRequestSize(Display *d) { return (long)MWD(d)->max_request_size; }
long XExtendedMaxRequestSize(Display *d) { (void)d; return 0; }
unsigned long XLastKnownRequestProcessed(Display *d) { return MWD(d)->last_request_read; }
unsigned long XNextRequest(Display *d) { return ++MWD(d)->request; }

int XBell(Display *d, int percent) { (void)d; (void)percent; return 0; }
int XKillClient(Display *d, XID resource) { (void)d; (void)resource; return 0; }
int XUngrabServer(Display *d) { (void)d; return 1; }
int XGrabServer(Display *d) { (void)d; return 1; }

int XAddConnectionWatch(Display *d, XConnectionWatchProc proc, XPointer data)
{ (void)d; (void)proc; (void)data; return 1; }
void XRemoveConnectionWatch(Display *d, XConnectionWatchProc proc, XPointer data)
{ (void)d; (void)proc; (void)data; }
void XProcessInternalConnection(Display *d, int fd) { (void)d; (void)fd; }
int XInternalConnectionNumbers(Display *d, int **fd, int *n)
{ (void)d; if (fd) *fd = NULL; if (n) *n = 0; return 1; }
