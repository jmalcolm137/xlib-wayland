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
#include "text-input-unstable-v3-client-protocol.h"
#include "primary-selection-unstable-v1-client-protocol.h"

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
    /* XFixesChangeCursor support: a cursor whose image was replaced. */
    Cursor                    alias_to;
    /* An ARGB cursor built from an XcursorImage (libXcursor), if any. */
    uint32_t                 *img_pixels;
    int                       img_w, img_h, img_hx, img_hy;
    struct wl_buffer         *img_buffer;
};

/* An XFIXES selection-input registration (GDK uses these to learn when the
 * clipboard owner changes, including across shim processes). */
typedef struct MwXfixesSel {
    Window                window;
    Atom                  selection;
    unsigned long         mask;
    struct MwXfixesSel   *next;
} MwXfixesSel;

/* A region handle returned by XFixesCreateRegion (opaque to clients). */
typedef struct MwXfixesRegion {
    unsigned long           id;
    struct MwXfixesRegion  *next;
} MwXfixesRegion;

/* A DAMAGE object (XDamageCreate): GDK creates one per composited window and
 * repaints from XDamageNotify. */
typedef struct MwDamage {
    unsigned long           id;
    Drawable                drawable;
    int                     level;
    struct MwDamage        *next;
} MwDamage;

/* One window's XI2 event mask (XISelectEvents), as a bit per XI event type. */
typedef struct MwXiSelect {
    Window                  win;
    uint32_t                mask[4];   /* up to 128 event types */
    struct MwXiSelect      *next;
} MwXiSelect;

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
 * Wayland client).  We keep the best text MIME (for the text paths) and the
 * full MIME list (so non-text targets -- image/png, text/uri-list, ... -- can
 * be matched and fetched). */
typedef struct MwWlOffer {
    struct wl_data_offer *offer;
    char                 *mime;      /* best text MIME, or NULL */
    char                **mimes;     /* every MIME offered */
    int                   nmimes, mimecap;
    char                 *motif_drag; /* x-motif drag payload, or NULL */
    Display              *d;         /* owning display */
    struct MwWlOffer     *next;
} MwWlOffer;

/* A data source we advertised because an X client owns a selection.  `source`
 * is a wl_data_source for CLIPBOARD or a zwp_primary_selection_source_v1 for
 * PRIMARY (the selection atom says which). */
typedef struct MwWlSource {
    void                  *source;
    Atom                   selection;
    struct MwWlSource     *next;
} MwWlSource;

/* An in-flight transfer of clipboard data *from* Wayland *to* an X client:
 * the offer's data arrives asynchronously on a pipe and becomes a
 * SelectionNotify once complete.  The same machinery carries a drag's dropped
 * bytes (is_dnd), which are handed to the Motif DnD bridge instead. */
typedef struct MwClipFetch {
    bool           active;
    bool           is_dnd;       /* a drag drop rather than a clipboard paste */
    bool           latin1;       /* serve as Latin-1 (X STRING/TEXT), not UTF-8 */
    bool           xdnd;         /* an XDND drop: stash the bytes for XdndSelection */
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
    Atom     target;             /* X target to convert for the peer */
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
    MwWindow       *popup_parent;    /* the menu this popup nests under, if any */
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
    bool                   fullscreen;       /* we (as WM) put it fullscreen */
    bool                   maximized;        /* ... or maximized */
    int                    fs_w, fs_h;       /* size to restore on unfullscreen */
    bool                   fs_csd;           /* decoration state to restore */
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
    struct wl_touch            *wl_touch;
    struct wl_surface          *touch_surface;   /* last touch-down surface */
    wl_fixed_t                  touch_lx, touch_ly;
    struct wl_output           *wl_output;
    struct zxdg_decoration_manager_v1 *deco_mgr;
    struct wp_viewporter              *viewporter;   /* may be NULL */
    struct wl_data_device_manager     *dnd_mgr;
    struct wl_data_device             *data_device;

    /* PRIMARY selection (zwp_primary_selection_v1); the X PRIMARY selection is
     * mirrored onto it and vice versa.  MwPrimOffer is defined in clipboard.c. */
    struct zwp_primary_selection_device_manager_v1 *primary_mgr;
    struct zwp_primary_selection_device_v1         *primary_device;
    struct MwPrimOffer                             *primary_offer;
    struct MwPrimOffer                             *primary_pending;

    /* Input method bridge (xim.c): the client side of zwp_text_input_v3.
     * A single text-input object per display/seat; text_input_ic points at the
     * _XIC (owned by xim.c) that currently has the input focus.  All NULL when
     * the compositor does not offer text-input, in which case XIM degrades to
     * the local pass-through. */
    struct zwp_text_input_manager_v3  *text_input_mgr;
    struct zwp_text_input_v3          *text_input;
    void                              *text_input_ic;
    bool                               text_input_entered;

    /* Cross-process selection broker (optional; see broker.c).  Opaque here. */
    struct MwBroker             *broker;

    /* Wayland clipboard bridge */
    MwWlOffer                   *wayland_offer;   /* current clipboard offer */
    MwWlOffer                   *pending_offer;   /* seen, not yet selected */
    MwWlSource                  *wl_sources;      /* sources we advertise */
    Window                       clip_window;     /* hidden requestor/owner */
    Window                       sm_window;       /* proxy for the SM window */
    MwClipFetch                  clip_fetch;
    MwClipServe                  clip_serve;

    /* Wayland drag-and-drop transport (clipboard.c).  A drag is not a
     * selection: the compositor delivers it with data_offer + enter/motion/leave
     * /drop, so it is tracked separately from the clipboard. */
    MwWlOffer                   *drag_offer;     /* offer for an incoming drag */
    struct wl_surface           *drag_surface;   /* surface the drag is over */
    double                       drag_x, drag_y; /* position within that surface */
    bool                         drag_active;    /* a drag is over one of ours */
    uint32_t                     drag_serial;    /* serial of the enter */
    Window                       drag_window;    /* X toplevel it is over */
    bool                         drag_xdnd;      /* the current drag is XDND */
    struct wl_data_source       *dnd_src;        /* our active drag source, or NULL */
    Atom                         dnd_src_sel;    /* X selection it is serving */

    /* Motif DnD bridge (xlib/dnd.c); opaque here. */
    struct MwDnd                *dnd;

    /* XDND bridge (xlib/xdnd.c); opaque here. */
    struct MwXdnd               *xdnd;

    /* XSETTINGS manager so GDK clients read theme/font/Xft settings even
     * though there is no settings daemon in this process (xlib/xsettings.c). */
    struct MwXSettings         *xsettings;

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
    Visual                      visual;       /* 24-bit TrueColor, the root */
    Visual                      visual32;     /* 32-bit ARGB, for compositing */
    Depth                       depths[2];
    ScreenFormat                formats[2];

    /* Render extension: the request built through _XGetRequest is dispatched
     * when the next request is built, when a reply is awaited, or on flush. */
    int                         render_req_pending;
    int                         render_req_type;
    size_t                      render_req_len;
    unsigned char              *render_req;
    int                         render_pending[8];  /* Render query replies, in */
    int                         render_npending;   /* request order (FIFO)      */
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
    /* Dead-key / compose sequences (e.g. dead_acute + e -> é). */
    struct xkb_compose_table   *xkb_compose_table;
    struct xkb_compose_state   *xkb_compose_state;
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
    /* The override-redirect menu currently posted, if any.  A menu mapped
     * while another is open is that menu's submenu, and must be nested under
     * it (see mw_popup_anchor). */
    MwWindow                   *open_menu;

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
    /* While a menu is open the compositor emits the whole popup chain
     * (main -> parent -> submenu) as a leave/enter burst on every motion, and
     * the ancestor enters carry stale coordinates (the parent's first item).
     * Acting on each event made GTK re-select an item in the parent menu --
     * deselecting the item whose submenu was open and popping the submenu
     * down.  Buffer the burst and, once it settles, focus only the deepest
     * popup that was entered (see mw_input_settle). */
    struct wl_surface          *ptr_defer_surface;
    MwWindow                   *ptr_defer_top;      /* window entered (surface) */
    MwWindow                   *ptr_defer_deep;     /* deepest window in it */
    int                         ptr_defer_sx, ptr_defer_sy;
    bool                        ptr_defer_active;
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
    char                       *xcursor_theme;    /* XcursorSetTheme */
    int                         xcursor_size;     /* XcursorSetDefaultSize */

    /* XFIXES (xfixes.c) */
    MwXfixesSel                *xfixes_sels;
    MwXfixesRegion             *xfixes_regions;
    unsigned long               xfixes_next_region;

    /* DAMAGE (xdamage.c) */
    MwDamage                   *damages;

    /* XInput2 event selection: per-window masks set by XISelectEvents. */
    MwXiSelect                 *xi_selects;

    /* X11 session protocol relay (session.c). */
    int                         session_fd;
    char                        session_path[192];
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
int         mw_display_scale(Display *d);   /* HiDPI device scale, >= 1 */

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
/* Process a no-reply XI request (XISelectEvents) as it is drained. */
void       mw_xi2_request(Display *d, const unsigned char *req, size_t len);
/* Whether `win` selected XI event `evtype`. */
bool       mw_xi2_selected(Display *d, Window win, int evtype);
/* Deliver an XI2 touch event (XIDeviceEvent cookie) to `win`. */
void       mw_xi2_touch(Display *d, Window win, int evtype, int touchid,
                        int deviceid, int sourceid, double x, double y,
                        Time time);
/* The id of the touch device, or 0 when the seat has no touch. */
int        mw_xi2_touch_device(Display *d);
void       mw_xi2_fini(Display *d);

/* X11 session protocol (session.c): deliver WM_SAVE_YOURSELF / WM_DELETE_WINDOW
 * to opted-in top-level windows, relayed from a session manager in another shim
 * process over a per-process Unix socket. */
void       mw_session_init(Display *d);
void       mw_session_fini(Display *d);
int        mw_session_poll_fd(Display *d);
void       mw_session_handle_ready(Display *d);

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
/* Window-manager side of EWMH _NET_WM_STATE: the client asked us to add,
 * remove or toggle a state (fullscreen today). */
void        mw_wm_net_wm_state(Display *d, XClientMessageEvent *cm);
void        mw_toplevel_set_fullscreen(Display *d, MwWindow *win, bool on);
void        mw_toplevel_set_maximized(Display *d, MwWindow *win, bool on);

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
/* Synthesise a key press/release to the focused window (used by the XIM bridge
 * to turn a Wayland text-input delete_surrounding_text into editing keys). */
void mw_key_send(Display *d, KeyCode kc, Bool press);
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
/* Coalesce a burst of wl_pointer leave/enter events into a single focus
 * change, so the spurious ancestor enters of a nested popup chain do not
 * re-select in the parent menu.  Called after each Wayland dispatch. */
void mw_input_settle(Display *d);
void mw_pointer_enter(Display *d, MwWindow *top, int x, int y);
void mw_pointer_leave(Display *d, MwWindow *top);
/* The compositor dismissed a popup (popup_done): stop routing input to a
 * modeled pointer grab on the now-dead popup (input.c). */
void mw_pointer_drop_grab(Display *d, MwWindow *win);
/* Deliver a synthetic ButtonRelease to the current grab window.  A Wayland
 * drag makes the compositor swallow the real release, so Motif's own drag
 * would otherwise never end. */
void mw_pointer_synthetic_release(Display *d, unsigned int xbutton);

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

/* Cross-process selection broker (xlib/broker.c).
 *
 * Every shim process is its own X server, so an X selection owned by one client
 * is invisible to the rest.  CDE's colour server (dtsession) owns
 * "Customize Data:<screen>"; dtstyle asks for it and, getting no answer, decides
 * the colour server is not running.  When enabled, sharing clients advertise
 * their ownership in a file under XDG_RUNTIME_DIR and accept convert requests on
 * a per-process Unix socket, servicing each through their own SelectionRequest
 * path and returning the converted property.  Opt in with
 * XLIB_WAYLAND_SHARE_SELECTIONS (comma-separated name prefixes; the default is
 * "Customize Data:"). */
void      mw_broker_init(Display *d);
void      mw_broker_fini(Display *d);
/* Service inbound/outbound broker traffic (call on every event pump). */
void      mw_broker_handle_ready(Display *d);
/* fd to include in the event-loop poll for broker traffic, or -1. */
int       mw_broker_poll_fd(Display *d);
/* Record/clear our ownership of a shared selection. */
void      mw_broker_owner_changed(Display *d, Atom selection, Window owner);
/* Forward a convert request to a remote owner.  Returns true when accepted (the
 * SelectionNotify is posted later, asynchronously). */
bool      mw_broker_convert(Display *d, Atom selection, Atom target,
                            Atom property, Window requestor, Time time);
/* Capture the property an owner writes while servicing a broker request.
 * Returns true when the write was ours (the serve proxy window). */
bool      mw_broker_capture_prop(Display *d, Window w, Atom property, Atom type,
                                 int format, const unsigned char *data,
                                 unsigned long nitems);
/* Complete a serve when the owner's SelectionNotify arrives.  Returns true when
 * the event was ours and should not be queued. */
bool      mw_broker_serve_notify(Display *d, XSelectionEvent *se);

/* Share the session manager's _DT_SM_* properties (xlib/smprops.c).  Each shim
 * process is its own X server, so the properties dtsession puts on its root and
 * top-level window (and that dtstyle's Style Manager reads) are invisible to the
 * rest.  Enabled with XLIB_WAYLAND_SHARE_PROPERTIES (comma-separated prefixes). */
void      mw_smprop_publish(Display *d, Atom prop, Atom type, int format,
                            const unsigned char *data, unsigned long nitems);
bool      mw_smprop_lookup(Display *d, Atom prop, Atom *type, int *format,
                           unsigned long *nitems, unsigned char **data);
bool      mw_smprop_is_shared(Display *d, Atom prop);
/* Hidden local window standing in for the session manager's window. */
Window    mw_smprop_proxy_window(Display *d);


/* ---------------------------------------------------------- Motif DnD bridge
 *
 * Motif drag-and-drop is an X protocol: drop sites advertise
 * `_MOTIF_DRAG_RECEIVER_INFO`, the initiator and receiver exchange
 * `_MOTIF_DRAG_AND_DROP_MESSAGE` ClientMessages, and the data moves over a
 * dynamically-named selection (the initiator's "icc handle", `_MOTIF_ATOM_n`).
 * None of that crosses between shim processes, so xlib/dnd.c bridges it onto
 * the Wayland data device: the compositor routes the drag (it alone knows the
 * pointer and the surfaces) and the bridge speaks the Motif half in each
 * process.  See dnd.c. */
void      mw_dnd_init(Display *d);
void      mw_dnd_fini(Display *d);
/* Wayland: a drag entered / moved over / left one of our surfaces. */
void      mw_dnd_wl_enter(Display *d, struct wl_surface *s, double x, double y,
                          const char *mime, const char *motif_drag);
void      mw_dnd_wl_motion(Display *d, double x, double y);
void      mw_dnd_wl_leave(Display *d);
/* The dropped bytes arrived; deliver them to the drop site under the drag. */
void      mw_dnd_wl_drop(Display *d, const unsigned char *data, size_t len);
/* Answer a convert for the synthetic initiator selection we own. */
bool      mw_dnd_xconvert(Display *d, Atom selection, Atom target, Atom property,
                          Window requestor, Time time);
/* An X client wrote _MOTIF_DRAG_INITIATOR_INFO: a Motif drag has started. */
void      mw_dnd_initiator_info(Display *d, Window src, Atom icc, int format,
                                const unsigned char *data, unsigned long nitems);
/* Whether `w` is a window this bridge created (so XSendEvent etc. treat it as
 * internal). */
bool      mw_dnd_owns_window(Display *d, Window w);
/* A Motif drag is in progress and the pointer has left its window: start the
 * Wayland drag so the compositor routes it.  Called from the pointer leave. */
void      mw_dnd_maybe_start(Display *d);
/* The Motif icc handle changed owners; a drag that no longer owns it has
 * ended. */
void      mw_dnd_selection_changed(Display *d, Atom selection, Window owner);
/* The Wayland drag finished or was cancelled: end Motif's drag (the
 * compositor swallowed the button release). */
void      mw_dnd_source_done(Display *d);
/* Deferred release of a Motif drag after its Wayland drag ends, so the broker
 * can finish relaying the transfer first (see the grace period in dnd.c). */
void      mw_dnd_pump(Display *d);
int       mw_dnd_timeout(Display *d);
/* The broker finished relaying one of a drag's conversions. */
void      mw_dnd_serve_done(Display *d);
/* Produce the bytes a Wayland drag source asked for, by converting the X
 * selection of the in-progress Motif drag.  Returns true if queued. */
bool      mw_dnd_source_send(Display *d, const char *mime, int fd);


/* cursor (cursor.c) */
void      mw_cursor_init(Display *d);
void      mw_cursor_fini(Display *d);
/* Xcursor support (used by the libXcursor facade): load a themed cursor by
 * freedesktop name, and (re)load the theme.  Returns None if unavailable. */
Cursor    mw_cursor_load_named(Display *d, const char *name);
Cursor    mw_cursor_load_shape(Display *d, unsigned int shape);
Cursor    mw_cursor_from_image(Display *d, int w, int h, int xhot, int yhot,
                               const uint32_t *pixels);
char     *mw_cursor_theme_get(Display *d);
int       mw_cursor_size_get(Display *d);
void      mw_cursor_theme_set(Display *d, const char *theme);
void      mw_cursor_size_set(Display *d, int size);

/* XFIXES (xfixes.c): the subset GDK uses for clipboard owner-change
 * notifications, regions and cursor changes.  The public entry points live in
 * the libXfixes facade (src/xfixes/xfixes.c), which calls these. */
#define MW_XFIXES_EVENT_BASE 128
#define MW_XFIXES_ERROR_BASE 128
Bool      mw_xfixes_query(int *event_base, int *error_base);
void      mw_xfixes_init(Display *d);
void      mw_xfixes_fini(Display *d);
void      mw_xfixes_select_input(Display *d, Window w, Atom selection,
                                 unsigned long mask);
void      mw_xfixes_selection_notify(Display *d, Atom selection, Window owner,
                                     Time time);
unsigned long mw_xfixes_new_region(Display *d);
void      mw_xfixes_free_region(Display *d, unsigned long region);
void      mw_xfixes_change_cursor(Display *d, Cursor image, Cursor target);
unsigned long mw_xsync_new_counter(Display *d);

/* DAMAGE (xdamage.c): GDK's composited-window path.  The shim owns
 * _NET_WM_CM_S0 (so gdk_screen_is_composited() is true) and reports DAMAGE;
 * damage objects are local and XDamageNotify is emitted from the shim's own
 * damage so GDK repaints. */
#define MW_XDAMAGE_EVENT_BASE 131
int       mw_xdamage_eventbase(void);
void      mw_xdamage_init(Display *d);
void      mw_xdamage_fini(Display *d);
unsigned long mw_xdamage_new(Display *d, Drawable drawable, int level);
void      mw_xdamage_free(Display *d, unsigned long id);
void      mw_xdamage_notify(Display *d, Drawable drawable, int x, int y,
                            int w, int h);

/* XDND (xdnd.c): bridge the XDND drag-and-drop protocol to the Wayland data
 * device, the same way the Motif bridge does for Motif DnD.  As the X side, the
 * shim is the XDND *source* to its own X clients: it sends Enter/Position/Drop
 * and serves XdndSelection from the Wayland offer, and the target replies
 * XdndStatus / XdndFinished. */
void      mw_xdnd_init(Display *d);
void      mw_xdnd_fini(Display *d);
/* A Wayland drag entered / moved over / left one of our surfaces. */
void      mw_xdnd_wl_enter(Display *d, struct wl_surface *s, double sx, double sy);
void      mw_xdnd_wl_motion(Display *d, double sx, double sy);
void      mw_xdnd_wl_leave(Display *d);
void      mw_xdnd_wl_drop(Display *d);
/* Whether the X window under a drag advertises XdndAware. */
bool      mw_xdnd_aware(Display *d, Window win);
/* Whether `w` is this bridge's synthetic XDND source window. */
bool      mw_xdnd_owns_window(Display *d, Window w);
/* Answer a convert on XdndSelection (the shim is the source to its X target). */
bool      mw_xdnd_xconvert(Display *d, Atom selection, Atom target, Atom property,
                           Window requestor, Time time);
/* A ClientMessage from the target (XdndStatus / XdndFinished) to our source
 * window.  Returns true when it was ours and should not be queued. */
/* A ClientMessage from the target (XdndStatus / XdndFinished) to our source
 * window.  Returns true when it was ours and should not be queued. */
bool      mw_xdnd_client_message(Display *d, XClientMessageEvent *cm);
/* The dropped bytes have arrived: stash them and tell the target it may drop. */
void      mw_xdnd_store_drop(Display *d, const unsigned char *data, size_t len);
/* X→Wayland: an X client took XdndSelection (a GDK drag started); drive a
 * Wayland drag for it. */
void      mw_xdnd_selection_changed(Display *d, Atom selection, Window owner);
/* A SelectionNotify to the clip window: the TARGETS reply for a drag start.
 * Returns true when it was ours and should not be queued. */
bool      mw_xdnd_notify(Display *d, XSelectionEvent *se);
/* The Wayland drag carrying an X source finished/was cancelled. */
void      mw_xdnd_source_done(Display *d);
Atom      mw_xdnd_selection(Display *d);

/* Create a Wayland data source bound to `selection` (with the clipboard's send
 * listener) and add it to the display's source list (clipboard.c). */
struct wl_data_source *mw_clipboard_make_source(Display *d, Atom selection);

/* Start fetching `mime` from a Wayland offer and serving it to an X requestor
 * as `selection`/`target` (clipboard.c). */
bool      mw_clipboard_fetch_from_offer(Display *d, struct wl_data_offer *offer,
                                         const char *mime, Atom selection, Atom target,
                                         Atom property, Window requestor, Time time,
                                         bool latin1);

/* XSETTINGS (xsettings.c): the shim is the _XSETTINGS_S<n> manager, publishing
 * theme/font/Xft settings from a config file so GDK clients see them. */
void      mw_xsettings_init(Display *d);
void      mw_xsettings_fini(Display *d);
/* Re-read the config when it changes (called from the event pump). */
void      mw_xsettings_pump(Display *d);

/* XIM (xim.c) */
void      mw_xim_init(Display *d);
/* Draw an active over-the-spot (XIMPreeditPosition) preedit into a toplevel
 * frame, after the window tree has been composited (wayland/surface.c). */
void      mw_xim_overlay(Display *d, MwToplevel *tl);

/* xrandr (xrandr.c) / shape (xshape.c) */

void      mw_shape_init(Display *d);

/* Render extension (render.c): intercepts libXrender's wire requests under the
 * major opcode XQueryExtension("RENDER") returns. */
int  mw_render_opcode(void);
void mw_render_finish(Display *d);
int  mw_render_reply(Display *d, void *rep);
int  mw_render_read(Display *d, char *data, size_t size);
void mw_render_drain(Display *d);
/* Deliver replies for Render queries the caller is not waiting for to the
 * display's async handlers (see render.c). */
void mw_render_dispatch_async(Display *d);
void mw_render_init(Display *d);
/* For the Xft Render-level entry points: the drawable behind a Picture, and
 * whether it is a solid fill (with its colour).  Return non-zero on success. */
int  mw_render_picture_drawable(Display *d, XID picture, unsigned long *drawable);
int  mw_render_picture_solid(Display *d, XID picture, unsigned short rgba[4]);

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
void          mw_dispatch_errors(Display *d);

#endif /* MW_XLIB_INTERNAL_H */
