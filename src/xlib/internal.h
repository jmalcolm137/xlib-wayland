/* internal.h — private model for the Motif/Wayland Xlib implementation.
 *
 * This header is included only by our own translation units.  Public clients
 * (libXt, libXm, XV, NEdit) see the stock Xorg headers, so every struct that
 * Xlib.h defines concretely (Screen, Visual, XImage, XFontStruct, XEvent, ...)
 * is filled in exactly as the header describes.
 *
 * `Display` is opaque in the public headers (typedef struct _XDisplay Display),
 * so we are free to define its internals here — but the *prefix* must match the
 * layout that Xlib.h's accessor macros ((_XPrivDisplay)dpy)->field reach into
 * (fd, qlen, screens, default_screen, nscreens, ...).  Fields after
 * `xdefaults` are ours ("there is more to this structure, but it is private").
 */
#ifndef MW_XLIB_INTERNAL_H
#define MW_XLIB_INTERNAL_H

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <time.h>
#include <pthread.h>

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/Xatom.h>
#include <X11/Xresource.h>
#include <X11/keysym.h>
#include <X11/cursorfont.h>

#include <wayland-client.h>
#include <xkbcommon/xkbcommon.h>
#include <pixman.h>

#include "raster.h"
#include "xdg-shell-client-protocol.h"
#include "xdg-decoration-client-protocol.h"
#include "viewporter-client-protocol.h"

/* ------------------------------------------------------------------ utility */

#define MW_MIN(a, b) ((a) < (b) ? (a) : (b))
#define MW_MAX(a, b) ((a) > (b) ? (a) : (b))

/* Default synthetic screen geometry when the compositor tells us nothing. */
#define MW_DEFAULT_W 1280
#define MW_DEFAULT_H 800
#define MW_DEFAULT_DPI 96

/* XID allocation base: must not collide with None(0)/PointerRoot(1)/etc. */
#define MW_XID_BASE 0x00400000UL

/* Predefined atom count (XA_LAST_PREDEFINED is 68 in Xatom.h). */
#define MW_PREDEFINED_ATOMS (XA_LAST_PREDEFINED + 1)

/* ------------------------------------------------------------- object model */

typedef enum {
    MW_OBJ_FREE = 0,
    MW_OBJ_WINDOW,
    MW_OBJ_PIXMAP,
    MW_OBJ_GC,
    MW_OBJ_COLORMAP,
    MW_OBJ_CURSOR,
    MW_OBJ_FONT,
    MW_OBJ_PICTURE,
    MW_OBJ_GLYPHSET,
    MW_OBJ_COUNT
} MwObjKind;

typedef struct {
    XID       id;     /* 0 == empty slot */
    MwObjKind kind;
    void     *obj;
} MwEntry;

typedef struct MwProp {
    Atom              atom;
    Atom              type;
    int               format;
    unsigned long     nitems;
    unsigned char    *data;
    size_t            nbytes;
    struct MwProp    *next;
} MwProp;

typedef struct MwWindow     MwWindow;
typedef struct MwToplevel   MwToplevel;
typedef struct MwPixmap     MwPixmap;
typedef struct MwColormap   MwColormap;
typedef struct MwCursor     MwCursor;
typedef struct MwXFont      MwXFont;
typedef struct MwKeyGrab    MwKeyGrab;
typedef struct MwBtnGrab    MwBtnGrab;
typedef struct MwSelection  MwSelection;

struct MwPixmap {
    XID        id;
    int        w, h, depth;
    MwSurface *surface;
    Screen    *screen;
};

struct MwColormap {
    XID              id;
    Screen          *screen;
    Visual          *visual;
    int              alloc_count;   /* for XAllocColorCells bookkeeping */
};

struct MwCursor {
    XID                       id;
    struct wl_cursor         *wl;      /* from libwayland-cursor, may be NULL */
    struct wl_surface        *surface; /* -fallback blank */
    unsigned int              shape;   /* cursor-shape-v1 enum, 0 = none */
    uint32_t                  fg, bg;
    bool                      created_surface;
};

/* An Xlib font object: an XFontStruct plus the raster font used to draw. */
struct MwXFont {
    XID            id;
    XFontStruct   *fs;
    MwFont        *rfont;      /* from raster.h */
    int            ascent, descent, max_width;
};

/* A GC.  struct _XGC is opaque in Xlib.h (no XLIB_ILLEGAL_ACCESS). */
struct _XGC {
    XID            gid;
    int            function;
    unsigned long  plane_mask;
    unsigned long  foreground;
    unsigned long  background;
    int            line_width;
    int            line_style;
    int            cap_style;
    int            join_style;
    int            fill_style;
    int            fill_rule;
    int            arc_mode;
    Pixmap         tile;
    Pixmap         stipple;
    int            ts_x_origin;
    int            ts_y_origin;
    Font           font;
    int            subwindow_mode;
    Bool           graphics_exposures;
    int            clip_x_origin;
    int            clip_y_origin;
    Pixmap         clip_mask;
    int            dash_offset;
    char           dashes;
    /* resolved clip (drawable-relative) */
    bool           has_clip_rects;
    XRectangle    *clip_rects;
    int            nclip;
    Region         clip_region;
    MwXFont       *xfont;
};

struct MwKeyGrab {
    Window         window;
    KeyCode        keycode;
    unsigned int   modifiers;
    Bool           owner_events;
    int            pointer_mode;
    int            keyboard_mode;
    struct MwKeyGrab *next;
};

struct MwBtnGrab {
    Window         window;
    unsigned int   button;
    unsigned int   modifiers;
    Bool           owner_events;
    unsigned int   event_mask;
    int            pointer_mode;
    int            keyboard_mode;
    Cursor         cursor;
    struct MwBtnGrab *next;
};

struct MwSelection {
    Atom            selection;
    Window          owner;
    Time            time;
    struct MwSelection *next;
};

/* ------------------------------------------------- Wayland clipboard bridge */

/* A data offer advertised by the compositor (clipboard content owned by a
 * Wayland client).  We keep only the best text MIME type. */
typedef struct MwWlOffer {
    struct wl_data_offer *offer;
    char                 *mime;      /* best text MIME, or NULL */
    Display              *d;         /* owning display */
    struct MwWlOffer     *next;
} MwWlOffer;

/* A data source we advertised because an X client owns a selection. */
typedef struct MwWlSource {
    struct wl_data_source *source;
    Atom                   selection;
    struct MwWlSource     *next;
} MwWlSource;

/* An in-flight transfer of clipboard data *from* Wayland *to* an X client:
 * the offer's data arrives asynchronously on a pipe and becomes a
 * SelectionNotify once complete. */
typedef struct MwClipFetch {
    bool           active;
    int            fd;           /* read end of the pipe, -1 when none */
    unsigned char *data;
    size_t         len, cap;
    Atom           selection, target, property;
    Window         requestor;
    Time           time;
    uint64_t       deadline_ms;
} MwClipFetch;

/* An in-flight transfer of clipboard data *from* an X owner *to* a Wayland
 * client: we asked the X owner for the data and write it to the peer's pipe
 * when the SelectionNotify arrives. */
typedef struct MwClipServe {
    bool     active;
    int      fd;                 /* write end of the peer's pipe */
    bool     to_utf8;            /* translate Latin-1 to UTF-8 before sending */
    Atom     selection, property;
    uint64_t deadline_ms;
} MwClipServe;

struct MwWindow {
    XID             id;
    Display        *d;               /* owning display (convenience) */
    int             x, y;            /* relative to parent (content origin) */
    int             w, h;            /* content size, excluding border */
    int             border_width;
    int             depth;
    int             c_class;         /* InputOutput / InputOnly */
    bool            created;
    bool            mapped;
    bool            override_redirect;
    bool            input_only;
    bool            redirect_to_toplevel; /* set for top-levels */

    MwWindow       *parent;
    MwWindow       *children;        /* first child (bottom of stack) */
    MwWindow       *last_child;      /* top of stack */
    MwWindow       *next_sib;
    MwWindow       *prev_sib;

    long            event_mask;
    long            do_not_propagate_mask;
    long            all_event_masks;

    Colormap        colormap;
    Cursor          cursor;

    int             bit_gravity;
    int             win_gravity;
    int             backing_store;
    bool            save_under;
    unsigned long   backing_planes;
    unsigned long   backing_pixel;

    bool            have_background;
    unsigned long   background_pixel;
    Pixmap          background_pixmap;   /* None or ParentRelative(1) or pixmap */
    unsigned long   border_pixel;
    Pixmap          border_pixmap;       /* None or pixmap */

    int             map_state;           /* IsUnmapped/IsUnviewable/IsViewable */

    MwSurface      *surface;             /* backing store, w x h */
    bool            surface_valid;

    MwToplevel     *tl;                  /* non-NULL iff top-level */

    MwProp         *props;

    bool            has_shape;
    pixman_region32_t shape;

    bool            pointer_inside;
    Time            last_event_time;
};

/* ------------------------------------------------------------- top-levels */

typedef struct MwBuf {
    struct wl_buffer   *buffer;
    struct wl_shm_pool *pool;
    void               *data;
    size_t              size;
    int                 stride;
    MwSurface          *frame;
    bool                busy;   /* attached / awaiting release */
    int                 idx;
    struct MwToplevel  *tl;
} MwBuf;

struct MwToplevel {
    MwWindow              *win;
    struct wl_surface     *surface;
    struct xdg_surface    *xdg_surface;
    struct xdg_toplevel   *xdg_toplevel;
    struct xdg_popup      *xdg_popup;
    struct xdg_positioner *positioner;
    bool                   is_popup;
    MwWindow              *parent_tl;
    uint32_t               reposition_token;
    MwBuf                  bufs[2];
    /* Grow-only buffer allocation: with wp_viewporter the buffers stay at
     * `cap_*` and the used area is cropped per frame, so resizing does not
     * create new buffers (and the compositor does not re-import them) on every
     * step of an interactive resize. */
    int                    cap_w, cap_h;
    struct wp_viewport    *viewport;
    MwSurface             *frame;            /* current draw target */
    struct wl_callback    *frame_cb;         /* compositor frame pacing */
    struct MwToplevel     *dirty_next;       /* display dirty list */
    bool                   in_dirty_list;
    int                    last_idx;         /* buffer most recently attached */
    uint64_t               last_commit_ms;   /* for the no-release fallback */
    struct zxdg_toplevel_decoration_v1 *deco; /* may be NULL */
    int                    width, height;    /* configured (surface) size */
    int                    req_w, req_h;     /* requested */
    bool                   configured;
    uint32_t               ack_serial;      /* configure awaiting ack+commit */
    bool                   repositioned;      /* popup xdg_popup.reposition sent for the current map */
    bool                   popup_dismissed;   /* compositor dismissed the popup; rebuild before re-map */
    bool                   mapped;
    bool                   dirty;
    bool                   pending_ack;
    bool                   use_ssd;          /* server-side decoration */
    bool                   decorated;
    bool                   closed;
    bool                   csd;              /* draw client-side decorations */
    int                    tb_h;             /* titlebar height when csd */
    bool                   undecorated;      /* client asked for no decorations
                                              * (_MOTIF_WM_HINTS decorations=0) */
    char                  *title;            /* last title sent to the compositor */
    char                  *app_id;           /* app_id sent to the compositor */
    bool                   motif_functions_sent; /* _MOTIF_WM_HINTS relayed once */
};

/* ------------------------------------------------------------ display impl */

struct _XPrivate;
struct _XrmHashBucketRec;

typedef struct _XDisplayImpl {
    /* ---- MUST match Xlib.h's private struct layout, in order ---- */
    XExtData       *ext_data;
    struct _XPrivate *private1;
    int             fd;
    int             private2;
    int             proto_major_version;
    int             proto_minor_version;
    char           *vendor;
    XID             private3, private4, private5;
    int             private6;
    XID           (*resource_alloc)(Display *);
    int             byte_order;
    int             bitmap_unit;
    int             bitmap_pad;
    int             bitmap_bit_order;
    int             nformats;
    ScreenFormat   *pixmap_format;
    int             private8;
    int             release;
    struct _XPrivate *private9, *private10;
    int             qlen;
    unsigned long   last_request_read;
    unsigned long   request;
    XPointer        private11, private12, private13, private14;
    unsigned        max_request_size;
    struct _XrmHashBucketRec *db;
    int           (*private15)(Display *);
    char           *display_name;
    int             default_screen;
    int             nscreens;
    Screen         *screens;
    unsigned long   motion_buffer;
    unsigned long   private16;
    int             min_keycode;
    int             max_keycode;
    XPointer        private17, private18;
    int             private19;
    char           *xdefaults;
    /* ---- end public prefix ---- */
    /* The real Display continues past xdefaults, and libraries built with
     * XTHREADS reach into it through Xlibint.h: LockDisplay() does
     * `if (dpy->lock_fns) ...`, so the layout through the binary-compatibility
     * boundary ("things above this line should not move") has to match or a
     * library dereferences our data as a function pointer.  These fields are
     * intentionally unused (the shim needs no event_vec/lock machinery) but
     * must occupy the right offsets. */
    char           *scratch_buffer;
    unsigned long   scratch_length;
    int             ext_number;
    void           *ext_procs;
    void           *event_vec[128];
    void           *wire_vec[128];
    KeySym          lock_meaning;
    void           *lock;
    void           *async_handlers;
    unsigned long   bigreq_size;
    void           *lock_fns;              /* must stay NULL: LockDisplay checks it */
    void          (*idlist_alloc)(Display *, XID *, int);
    /* ---- everything below is private to us ---- */

    struct wl_display          *wl_display;
    struct wl_registry         *wl_registry;
    struct wl_compositor       *wl_compositor;
    struct wl_shm              *wl_shm;
    struct xdg_wm_base         *wm_base;
    struct wl_seat             *wl_seat;
    struct wl_keyboard         *wl_keyboard;
    struct wl_pointer          *wl_pointer;
    struct wl_output           *wl_output;
    struct zxdg_decoration_manager_v1 *deco_mgr;
    struct wp_viewporter              *viewporter;   /* may be NULL */
    struct wl_data_device_manager     *dnd_mgr;
    struct wl_data_device             *data_device;

    /* Wayland clipboard bridge */
    MwWlOffer                   *wayland_offer;   /* current clipboard offer */
    MwWlOffer                   *pending_offer;   /* seen, not yet selected */
    MwWlSource                  *wl_sources;      /* sources we advertise */
    Window                       clip_window;     /* hidden requestor/owner */
    MwClipFetch                  clip_fetch;
    MwClipServe                  clip_serve;

    int                         output_w, output_h;   /* logical size, pixels */
    int                         output_pw, output_ph; /* physical size, mm */
    int                         output_mode_w, output_mode_h; /* mode, physical px */
    int                         output_scale;         /* wl_output.scale, >= 1 */
    int                         seat_caps;

    /* XInput2 emulation: the last request built through _XGetRequest (so
     * _XReply can answer it) and any variable-length reply payload waiting to
     * be served by _XRead. */
    unsigned char              *xi2_req;
    int                         xi2_req_len;
    unsigned char              *xi2_data;
    size_t                      xi2_data_len;
    size_t                      xi2_data_off;

    Screen                     *screen;       /* == &screens[0] */
    Visual                      visual;
    Depth                       depths[1];
    ScreenFormat                formats[1];

    /* Render extension: the request built through _XGetRequest is dispatched
     * when the next request is built, when a reply is awaited, or on flush. */
    int                         render_req_pending;
    int                         render_req_type;
    size_t                      render_req_len;
    unsigned char              *render_req;
    int                         render_reply_pending;
    unsigned char              *render_reply_data;
    size_t                      render_reply_len;
    size_t                      render_reply_off;
    int                         render_draining;   /* re-entrancy guard */

    /* registry */
    MwEntry                    *table;
    size_t                      table_cap;    /* power of two */
    size_t                      table_used;
    XID                         next_id;

    /* Open-display list, mirroring Xlib: XOpenDisplay returns the same Display
     * for the same display name and XCloseDisplay only tears it down when the
     * last reference goes away.  Motif calls XOpenDisplay(XDisplayString(d))
     * internally (input methods, render tables), and without this the shim
     * answered with a second, fully independent Display -- a second Wayland
     * connection with its own root window and its own XID space -- so widgets
     * built through that handle ended up on a "different server". */
    int                         refcnt;
    struct _XDisplayImpl       *open_next;

    /* atoms */
    char                      **atom_names;
    int                         natoms;       /* high-water mark */
    int                         atoms_cap;
    char                       *atom_buf;     /* names of interned atoms */

    /* event queue */
    XEvent                     *queue;
    int                         qcap;
    int                         qhead;
    int                         qcount;

    /* error handling */
    XErrorHandler               error_handler;
    XIOErrorHandler             io_error_handler;
    int                         error_handler_set;
    int                         io_error_handler_set;
    XErrorEvent                *errq;
    int                         errq_cap, errq_head, errq_count;

    /* keymap */
    struct xkb_context         *xkb_ctx;
    struct xkb_keymap          *xkb_keymap;
    struct xkb_state           *xkb_state;
    XModifierKeymap            *modmap;
    int                         keymap_inited;

    /* input state */
    int                         ptr_x, ptr_y;         /* root coords */
    unsigned int                ptr_state;            /* button/mod mask */
    MwWindow                   *ptr_window;           /* toplevel under pointer */
    MwWindow                   *ptr_focus;            /* deepest window */
    MwWindow                   *kbd_focus;            /* XSetInputFocus target */
    bool                        focus_explicit;
    /* Mouse wheel: Wayland reports scroll as axis events, but X clients expect
     * the wheel buttons (4/5, 6/7).  Accumulate per axis and emit one click per
     * detent in ptr_frame (see input.c). */
    double                      scroll_acc[2];      /* residual detents */
    double                      scroll_cont[2];     /* continuous motion */
    double                      scroll_step[2];     /* discrete steps this frame */
    bool                        scroll_have_step[2];
    /* Most recently mapped ordinary (non-override, non-popup) toplevel.  A
     * Motif menu shell is parented to the *root*, so when the pointer-derived
     * candidates are unusable there is no way to find the application's
     * toplevel by walking the X hierarchy -- yet that is what a menu has to be
     * anchored to. */
    MwWindow                   *active_toplevel;

    MwWindow                   *ptr_grab_window;
    MwWindow                   *implicit_grab;   /* window that got the last button press */
    bool                        ptr_grab_owner;
    unsigned int                ptr_grab_mask;
    int                         ptr_grab_mode;
    Cursor                      ptr_grab_cursor;
    bool                        ptr_grab_active;      /* XGrabPointer in effect */
    bool                        ptr_grab_temporary;   /* established by a passive grab */
    MwBtnGrab                  *btn_grabs;
    MwKeyGrab                  *key_grabs;

    struct wl_surface          *ptr_wl_surface;
    int                         ptr_surf_x, ptr_surf_y;
    MwWindow                   *ptr_toplevel;
    uint32_t                    ptr_enter_serial;
    uint32_t                    last_input_serial;  /* any input serial */
    uint32_t                    last_press_serial;  /* serial of the last button press */
    uint32_t                    kbd_mods;         /* xkb serialized mods */
    uint32_t                    kbd_group;
    bool                        ctrl_down;

    MwWindow                   *kbd_grab_window;
    bool                        kbd_grab_owner;
    int                         kbd_grab_mode;

    /* Auto-repeat.  Wayland leaves it to the client: wl_keyboard.repeat_info
     * carries the rate and delay, and the held key has to be repeated by us. */
    int                         repeat_rate;      /* keys per second; 0 = off */
    int                         repeat_delay;     /* ms before the first repeat */
    KeyCode                     repeat_key;       /* key currently held, or 0 */
    uint64_t                    repeat_next_ms;   /* when the next repeat is due */
    int                         auto_repeat;      /* AutoRepeatModeOn/Off, shared
                                                   * with dtstyle's Keyboard panel */

    /* Wakeup plumbing (see src/wayland/wakeup.c).  libXt waits in select() on
     * the fd XConnectionNumber returns; if that is the Wayland socket the
     * client only wakes when the compositor sends something, which is far too
     * irregular to pace key repeat.  A helper thread watches the Wayland fd and
     * the repeat deadline and signals a pipe instead.  That pipe *is* the
     * public `fd' field (libXt reads it through the ConnectionNumber() macro,
     * not XConnectionNumber()), so the Wayland socket is kept separately. */
    int                         wl_fd;            /* the Wayland socket */
    int                         wake_pipe[2];     /* [0] read end, [1] write end */
    int                         wake_stop;        /* eventfd that stops the thread */
    int                         wake_cmd;         /* eventfd: deadline changed */
    pthread_t                   wake_thread;
    bool                        wake_running;
    int                         wake_active;      /* __atomic: a repeat is pending */
    uint64_t                    wake_deadline;    /* __atomic: its deadline, ms */

    /* selections */
    MwSelection                 *selections;

    /* Xlib contexts (XSaveContext/XFindContext) */
    struct MwContext            *contexts;

    /* cut buffers */
    unsigned char              *cutbuf[8];
    size_t                      cutbuf_len[8];

    /* Xrm database (also mirrored into ->db for Xlib macros) */
    XrmDatabase                 rdb;

    /* Coalescing: when the last damage flush happened, so that a client which
     * repaints in many small steps cannot force a full composite and upload
     * per step. */
    uint64_t                    last_flush_ms;

    /* Toplevels that have ever been damaged, so flushing does not have to scan
     * the whole object table (which the poll paths hit constantly). */
    struct MwToplevel          *dirty_toplevels;

    /* server state */
    bool                        syncing;
    bool                        closed;
    bool                        event_loop_dispatched;
    unsigned long               serial;
    bool                        owns_display; /* we opened it (vs Xt) */

    struct wl_cursor_theme     *cursor_theme;
    int                         cursor_theme_size;
    MwCursor                   *default_cursor;   /* left_ptr fallback */
} XDisplayImpl;

#define MWD(d)     ((XDisplayImpl *)(d))
#define MWSCR(d)   (&MWD(d)->screens[MWD(d)->default_screen])

/* wayland connection lifecycle (wayland/wl.c) */
int  mw_wl_connect(XDisplayImpl *dp, const char *name);
void mw_wl_disconnect(XDisplayImpl *dp);

/* ------------------------------------------------------------------- atoms */

Atom  mw_intern_atom(Display *d, const char *name, Bool only_if_exists);
char *mw_atom_name(Display *d, Atom a);
void  mw_init_atoms(Display *d);

/* -------------------------------------------------------------- object table */

void *mw_lookup(Display *d, XID id, MwObjKind kind);   /* kind MW_OBJ_FREE = any */
void  mw_register(Display *d, XID id, MwObjKind kind, void *obj);
void  mw_unregister(Display *d, XID id);
XID   mw_alloc_id(Display *d);
MwWindow   *mw_window(Display *d, Window w);
MwPixmap   *mw_pixmap(Display *d, Pixmap p);
MwXFont    *mw_font(Display *d, Font f);
MwColormap *mw_colormap(Display *d, Colormap c);
MwCursor   *mw_cursor(Display *d, Cursor c);

/* ------------------------------------------------------- window / compositor */

MwWindow *mw_create_window(Display *d, Window parent, int x, int y, int w, int h,
                           int border_width, int depth, unsigned int c_class,
                           Visual *visual, unsigned long valuemask,
                           XSetWindowAttributes *attr);
void      mw_destroy_window(Display *d, MwWindow *win);
void      mw_map_window(Display *d, MwWindow *win, bool raised);
void      mw_unmap_window(Display *d, MwWindow *win);
bool      mw_window_is_viewable(MwWindow *win);
void      mw_window_move_resize(MwWindow *win, int x, int y, int w, int h);
void      mw_window_wm_resize(MwWindow *win, int w, int h);
void      mw_window_ensure_surface(MwWindow *win);
MwWindow *mw_any_mapped_toplevel(Display *d, MwWindow *avoid);
void      mw_window_damage(MwWindow *win);
void      mw_window_expose(MwWindow *win, int x, int y, int w, int h);
void      mw_window_flush(MwWindow *win);
void      mw_configure_notify(Display *d, MwWindow *win);
void      mw_flush_damage(Display *d);
/* Like mw_flush_damage() but capped to roughly one frame per display refresh;
 * used for the non-blocking poll paths.  Damage is never dropped, only
 * deferred -- the client blocking (or XSync) flushes unconditionally. */
void      mw_flush_damage_deferred(Display *d);

/* xi2.c — minimal XInput2 reply surface for libXi clients. */
Bool       mw_xi2_query_extension(_Xconst char *name, int *major,
                                  int *first_event, int *first_error);
const char *mw_xi2_extension_name(void);
Bool       mw_xi2_reply(Display *d, void *repbuf);
int        mw_xi2_read(Display *d, char *data, size_t size);
void       mw_xi2_forget_request(Display *d);

/* Absolute (root) origin of a window's content. */
void mw_window_origin(MwWindow *win, int *x, int *y);
MwWindow *mw_child_at(MwWindow *parent, int x, int y);
MwWindow *mw_deepest_at(MwWindow *toplevel, int x, int y, int *cx, int *cy);

/* toplevel helpers (wayland/surface.c) */
MwToplevel *mw_toplevel_of(MwWindow *win);
void        mw_toplevel_create(MwWindow *win);
/* Relay _MOTIF_WM_HINTS functions to the window manager: hide the titlebar
 * buttons the client disabled (CoW cannot refuse commands per window).  A
 * no-op unless the session sets CDE_MOTIF_HELPER. */
void        mw_apply_motif_functions(Display *d, MwWindow *win);
MwWindow   *mw_popup_anchor(MwWindow *win);
void        mw_toplevel_destroy(MwToplevel *tl);
void        mw_toplevel_map(MwToplevel *tl);
void        mw_toplevel_unmap(MwToplevel *tl);
void        mw_toplevel_set_size(MwToplevel *tl, int w, int h);
void        mw_toplevel_commit(MwToplevel *tl);
void        mw_toplevel_damage(MwToplevel *tl);
void        mw_toplevel_render(MwToplevel *tl);
void        mw_composite_window(MwToplevel *tl, MwWindow *win, int ox, int oy);
void        mw_toplevel_close(MwToplevel *tl);
int         mw_toplevel_content_offset(MwToplevel *tl);
void        mw_toplevel_reposition(MwToplevel *tl);

/* X resource database loading (xresources.c) */
void        mw_load_resources(Display *d);

/* --------------------------------------------------------------- drawables */

/* Resolve a drawable XID to its backing MwSurface + geometry. */
MwSurface *mw_drawable_surface(Display *d, Drawable dr, int *w, int *h, int *depth);
int        mw_drawable_origin(Display *d, Drawable dr, int *x, int *y);

/* --------------------------------------------------------------------- GCs */

struct _XGC *mw_create_gc_internal(Display *d, Drawable dr, unsigned long mask,
                                   XGCValues *values);
void mw_gc_changed(Display *d, struct _XGC *gc);
MwSurface *mw_gc_stipple_surface(Display *d, struct _XGC *gc, bool *is_stipple);

/* Begin a canvas on a drawable with the GC's clip installed. */
MwCanvas *mw_canvas_for_drawable(Display *d, Drawable dr, struct _XGC *gc,
                                 int *w, int *h);
void      mw_apply_gc(MwCanvas *c, Display *d, struct _XGC *gc);

/* ------------------------------------------------------------------ events */

void mw_put_event(Display *d, XEvent *ev);
void mw_process_events(Display *d, bool block);
/* Keyboard auto-repeat: Wayland leaves it to the client, so a held key is
 * repeated from the event pump (see input.c). */
void mw_kbd_repeat_pump(Display *d);
int  mw_kbd_repeat_timeout(Display *d);
/* Session-wide auto-repeat state, shared with the Style Manager's Keyboard
 * panel (auto_repeat is AutoRepeatModeOn/Off). */
int  mw_keyboard_autorepeat_mode(Display *d);
void mw_keyboard_set_autorepeat(Display *d, int mode);
bool mw_autorepeat_enabled(Display *d);

/* Wakeup plumbing: the fd handed out by XConnectionNumber (see wakeup.c). */
int  mw_wakeup_start(XDisplayImpl *dp);
void mw_wakeup_stop(XDisplayImpl *dp);
void mw_wakeup_set(XDisplayImpl *dp, int active, uint64_t deadline_ms);
void mw_wakeup_drain(XDisplayImpl *dp);
/* Block until at least one more Wayland event has been read and dispatched,
 * even when events are already queued.  mw_process_events(d, true) only blocks
 * on an empty queue, which is not enough for the mask-based event selectors
 * (XWindowEvent/XMaskEvent/XIfEvent), so they block here instead. */
void mw_block_for_events(Display *d);
int  mw_events_queued(Display *d, int mode);
Time mw_now(void);
void mw_send_configure_notify(Display *d, MwWindow *win);
void mw_send_client_message(Display *d, MwWindow *win, Atom type,
                            long d0, long d1, long d2, long d3, long d4);

/* ------------------------------------------------------------------ input */

void mw_input_init(Display *d, struct wl_seat *seat);
void mw_input_fini(Display *d);
void mw_keyboard_key(Display *d, uint32_t key, uint32_t state);
void mw_pointer_motion(Display *d, int x, int y);
void mw_pointer_button(Display *d, uint32_t button, uint32_t state);
void mw_pointer_frame(Display *d);
void mw_set_pointer_cursor(Display *d, Cursor c);
/* Create the synthetic WM window + workspace properties CDE's DtSvc reads. */
void mw_init_wm_window(Display *d);
/* Refresh a synthetic WM window's workspace properties from the live WSM
 * state (called as DtSvc queries them, so long-running clients see changes). */
void mw_refresh_workspace_props(Display *d, MwWindow *win);
/* Apply the cursor of the window (or nearest ancestor) under the pointer.
 * Called as the pointer moves, like the server's cursor inheritance. */
void mw_pointer_update_cursor(Display *d, MwWindow *w);
void mw_pointer_enter(Display *d, MwWindow *top, int x, int y);
void mw_pointer_leave(Display *d, MwWindow *top);

/* keymap (keymap.c) */
void     mw_keymap_init(Display *d, const char *keymap_str);
void     mw_keymap_init_fd(Display *d, int fd, uint32_t size, uint32_t format);
void     mw_keymap_fini(Display *d);
KeySym   mw_keycode_to_keysym(Display *d, KeyCode kc, int index);
KeyCode  mw_keysym_to_keycode(Display *d, KeySym ks);
unsigned int mw_keysym_to_modmask(Display *d, KeySym ks);
void     mw_update_modmap(Display *d);

/* properties (property.c) */
MwProp   *mw_get_prop(Display *d, MwWindow *win, Atom a);
int       mw_set_prop(Display *d, MwWindow *win, Atom a, Atom type, int format,
                      const unsigned char *data, unsigned long nitems);
int       mw_del_prop(Display *d, MwWindow *win, Atom a);

/* selection (selection.c) */
void      mw_init_selection(Display *d);
void      mw_fini_selection(Display *d);

/* clipboard bridge between X selections and the Wayland data device
 * (wayland/clipboard.c). */
void      mw_clipboard_init(Display *d);
void      mw_clipboard_fini(Display *d);
/* Hidden window used to own selections and receive transfer properties.
 * Created on first use (see the comment on it for why it is lazy). */
Window    mw_clip_window(Display *d);
/* fd that needs watching for an in-flight fetch, or -1. */
int       mw_clipboard_poll_fd(Display *d);
/* Service the in-flight fetch/serve state (call after the fd is ready). */
void      mw_clipboard_handle_ready(Display *d);
/* Serve a selection request from the Wayland clipboard.  Returns true when it
 * accepted the request (the SelectionNotify is posted later, asynchronously). */
bool      mw_clipboard_xconvert(Display *d, Atom selection, Atom target,
                                Atom property, Window requestor, Time time);
/* Complete a pending X->Wayland transfer when its SelectionNotify arrives.
 * Returns true when the event was ours and should not be queued. */
bool      mw_clipboard_serve_notify(Display *d, XSelectionEvent *se);
/* Track X selection ownership so it can be republished to Wayland. */
void      mw_clipboard_owner_changed(Display *d, Atom selection, Window owner);
/* Convert between UTF-8 and Latin-1 where a peer needs a particular encoding. */
char     *mw_clipboard_to_utf8(const unsigned char *in, size_t inlen, size_t *outlen);
char     *mw_clipboard_from_utf8(const unsigned char *in, size_t inlen, size_t *outlen);


/* cursor (cursor.c) */
void      mw_cursor_init(Display *d);
void      mw_cursor_fini(Display *d);

/* XIM (xim.c) */
void      mw_xim_init(Display *d);

/* xrandr (xrandr.c) / shape (xshape.c) */

void      mw_shape_init(Display *d);

/* Render extension (render.c): intercepts libXrender's wire requests under the
 * major opcode XQueryExtension("RENDER") returns. */
int  mw_render_opcode(void);
void mw_render_finish(Display *d);
int  mw_render_reply(Display *d, void *rep);
int  mw_render_read(Display *d, char *data, size_t size);
void mw_render_drain(Display *d);
void mw_render_init(Display *d);

/* render marker for a window (surface.c) */
void      mw_queue_render(Display *d);

/* window property change notification used by WM_* handling */
void      mw_window_props_changed(Display *d, MwWindow *win, Atom a);

/* fatal IO error path */
int       mw_io_error(Display *d, const char *reason);

/* global error handlers (Xlib stores these process-wide) */
void          mw_set_error_handler(XErrorHandler h);
XErrorHandler mw_error_handler(void);
void          mw_set_io_error_handler(XIOErrorHandler h);
XIOErrorHandler mw_io_error_handler_get(void);
void          mw_deliver_error(Display *d, int code, int request, int minor,
                               XID resource, int type);

#endif /* MW_XLIB_INTERNAL_H */
