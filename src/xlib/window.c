/* window.c — the in-process X window tree and window-management API. */
#include "internal.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* --------------------------------------------------------------- helpers */

static bool is_root(Display *d, MwWindow *w)
{
    return w && w->id == MWSCR(d)->root;
}

/* Resolve a window's background to either a tiling pixmap (returned in
 * *pm_out) or a solid pixel (*pix_out).  A window has one background: a
 * pixmap or a pixel.  ParentRelative (and a window that never set a
 * background) walks up the tree, matching X's CopyFromParent inheritance. */
static void effective_background(MwWindow *win, MwPixmap **pm_out,
                                 uint32_t *pix_out)
{
    uint32_t pix = MWSCR(win->d)->black_pixel;

    *pm_out = NULL;
    for (MwWindow *w = win; w; w = w->parent) {
        if (w->background_pixmap != None &&
            w->background_pixmap != ParentRelative) {
            MwPixmap *pm = mw_pixmap(w->d, w->background_pixmap);
            /* Only tile a real (multi-plane) pixmap.  A 1-bit Bitmap
             * background is a stipple whose bits are drawn with the window's
             * foreground/background by Motif, not tiled as-is; tiling it here
             * painted the whole area from the bitmap's dummy pixels. */
            if (pm && pm->surface && pm->w > 0 && pm->h > 0 && pm->depth > 1) {
                *pm_out = pm;
                return;
            }
        } else if (w->background_pixmap == ParentRelative && w->parent) {
            continue;           /* use the parent's background */
        }
        if (w->have_background) {
            pix = (uint32_t)w->background_pixel;
            break;
        }
        if (!w->parent)
            break;
    }
    *pix_out = pix;
}

/* Fill (x,y,w,h) of a window's own surface with its background, honouring a
 * tiling background pixmap (XSetWindowBackgroundPixmap / CWBackPixmap).  X
 * tiles from the window origin, so the tile phase does not depend on the
 * cleared rectangle.  Without this a window that only set a background
 * pixmap (e.g. CDE's Front-Panel handles) was filled with the default black
 * pixel instead of the pixmap. */
static void paint_background(MwWindow *win, int x, int y, int w, int h)
{
    if (!win || !win->surface || w <= 0 || h <= 0) return;

    MwPixmap *pm = NULL;
    uint32_t pix = 0;
    effective_background(win, &pm, &pix);

    XRectangle r = { (short)x, (short)y, (unsigned short)w, (unsigned short)h };
    MwCanvas *c = mw_canvas_begin(win->surface, NULL, 0, 0, 0);
    mw_clip_rects(c, &r, 1, 0, 0);
    mw_set_operator(c, GXcopy);

    if (pm) {
        int pw = pm->w, ph = pm->h;
        for (int ty = (y / ph) * ph; ty < y + h; ty += ph)
            for (int tx = (x / pw) * pw; tx < x + w; tx += pw)
                mw_canvas_copy(c, pm->surface, 0, 0, tx, ty, pw, ph);
    } else {
        mw_set_source_argb(c, 0xff000000u | (pix & 0xffffff));
        mw_paint(c);
    }
    mw_canvas_end(c);
}

void mw_window_ensure_surface(MwWindow *w)
{
    if (w->w <= 0 || w->h <= 0) return;
    if (w->surface && w->surface_valid) return;
    if (w->surface) mw_surface_destroy(w->surface);
    w->surface = mw_surface_create(w->w, w->h);
    w->surface_valid = true;
    paint_background(w, 0, 0, w->w, w->h);
}

void mw_window_origin(MwWindow *win, int *x, int *y)
{
    int ax = 0, ay = 0;
    for (MwWindow *w = win; w; w = w->parent) { ax += w->x; ay += w->y; }
    *x = ax; *y = ay;
}

bool mw_window_is_viewable(MwWindow *win)
{
    for (MwWindow *w = win; w; w = w->parent)
        if (!w->mapped) return false;
    return true;
}

static void unlink_child(MwWindow *win)
{
    if (win->prev_sib) win->prev_sib->next_sib = win->next_sib;
    else if (win->parent) win->parent->children = win->next_sib;
    if (win->next_sib) win->next_sib->prev_sib = win->prev_sib;
    else if (win->parent) win->parent->last_child = win->prev_sib;
    win->parent = NULL; win->next_sib = win->prev_sib = NULL;
}

static void link_top(MwWindow *parent, MwWindow *win)
{
    win->parent = parent;
    win->prev_sib = parent->last_child;
    win->next_sib = NULL;
    if (parent->last_child) parent->last_child->next_sib = win;
    else parent->children = win;
    parent->last_child = win;
}

void mw_window_damage(MwWindow *win)
{
    MwWindow *w = win;
    while (w && !w->tl) w = w->parent;
    /* Go through mw_toplevel_damage() so the coalescing timestamp is updated;
     * setting ->dirty directly bypassed it and the renderer then painted
     * immediately, mid-repaint, showing half-drawn text. */
    if (w && w->tl) mw_toplevel_damage(w->tl);
}

/* --------------------------------------------------- synthetic WM window */

/* Every client gets its own private X root, so a real window manager's window
 * (and the workspace properties dtwm publishes on it) is invisible to other
 * clients.  CDE's DtSvc reads those properties to answer
 * DtWsmGetWorkspaceList()/DtWsmGetCurrentWorkspace(); with none present every
 * query fails and clients stall (dtfile retries the workspace list for
 * seconds).  Synthesize the minimum the queries look for:
 *
 *   root:   _MOTIF_WM_INFO      (type _MOTIF_WM_INFO, 32, {flags, wmWindow})
 *   wmWin:  _DT_WORKSPACE_LIST  (XA_ATOM, 32, array of workspace-name atoms)
 *           _DT_WORKSPACE_CURRENT (XA_ATOM, 32, the current workspace atom)
 *
 * _DtGetMwmWindow() requires the WM window to be a direct child of the root.
 * The workspace set comes from CDE_WS_NAMES (comma separated) and
 * CDE_WS_CURRENT, defaulting to One..Four with the first current. */
/* Read the Workspace Manager state: workspace names (comma separated) and the
 * current index.  Precedence is the runtime state file written by the WSM
 * bridge, then the CDE_WS_* environment, then the CDE default of four
 * workspaces. */
static void mw_read_workspace_state(char *names, size_t namesz, int *current)
{
    snprintf(names, namesz, "ws0,ws1,ws2,ws3");
    *current = 0;

    const char *state = getenv("CDE_WSM_STATE");
    char path[1024];
    if (!state || !*state) {
        const char *rt = getenv("XDG_RUNTIME_DIR");
        snprintf(path, sizeof path, "%s/cde-wayland/workspace",
                 (rt && *rt) ? rt : "/tmp");
        state = path;
    }
    FILE *f = fopen(state, "r");
    if (f) {
        char line[512];
        while (fgets(line, sizeof line, f)) {
            if (strncmp(line, "names=", 6) == 0) {
                size_t n = strcspn(line + 6, "\r\n");
                if (n >= namesz) n = namesz - 1;
                memcpy(names, line + 6, n);
                names[n] = 0;
            } else if (strncmp(line, "current=", 8) == 0) {
                *current = atoi(line + 8);
            }
        }
        fclose(f);
    }

    const char *env = getenv("CDE_WS_NAMES");
    if (env && *env) {
        size_t n = strlen(env);
        if (n >= namesz) n = namesz - 1;
        memcpy(names, env, n);
        names[n] = 0;
    }
    env = getenv("CDE_WS_CURRENT");
    if (env && *env) *current = atoi(env);
}

/* Publish the workspace list and current workspace onto a synthetic WM
 * window. */
static void mw_set_workspace_props(Display *d, MwWindow *wm)
{
    char names_buf[512];
    int current = 0;
    mw_read_workspace_state(names_buf, sizeof names_buf, &current);

    char *names = strdup(names_buf);
    Atom ws[64];
    int nws = 0;
    for (char *tok = strtok(names, ","); tok && nws < 64; tok = strtok(NULL, ",")) {
        while (*tok == ' ' || *tok == '\t') tok++;
        if (*tok) ws[nws++] = mw_intern_atom(d, tok, False);
    }
    free(names);
    if (current < 0 || current >= nws) current = 0;
    if (nws <= 0) return;

    uint32_t list[64];
    for (int i = 0; i < nws; i++) list[i] = (uint32_t)ws[i];
    Atom pl = mw_intern_atom(d, "_DT_WORKSPACE_LIST", False);
    mw_set_prop(d, wm, pl, XA_ATOM, 32, (const unsigned char *)list,
                (unsigned long)nws);

    uint32_t cur = (uint32_t)ws[current];
    Atom pc = mw_intern_atom(d, "_DT_WORKSPACE_CURRENT", False);
    mw_set_prop(d, wm, pc, XA_ATOM, 32, (const unsigned char *)&cur, 1);
}

/* Refresh a synthetic WM window's workspace properties from the current WSM
 * state.  A client's connection snapshots the state when it starts, but the
 * workspace set/current changes while the session runs, so DtSvc's queries
 * re-read it here.  Only a window carrying the synthetic list is the WM
 * window. */
void mw_refresh_workspace_props(Display *d, MwWindow *win)
{
    Atom pl = mw_intern_atom(d, "_DT_WORKSPACE_LIST", False);
    if (mw_get_prop(d, win, pl))
        mw_set_workspace_props(d, win);
}

void mw_init_wm_window(Display *d)
{
    XDisplayImpl *dp = MWD(d);
    MwWindow *root = mw_window(d, MWSCR(d)->root);
    if (!root) return;

    /* Advertise the ToolTalk session on the root window, as a real X server's
     * session manager does.  ToolTalk clients discover the session either from
     * the TT_SESSION environment or from these root properties: the Information
     * Manager (dtinfo) uses tt_X_session(XDisplayString(display)), which reads
     * _SUN_TT_SESSION / TT_SESSION, and without them it fails with
     * TT_ERR_NOMP.  tt_XATOM_NAME and tt_CDE_XATOM_NAME in libtt. */
    {
        const char *tt = getenv("TT_SESSION");
        if (tt && *tt) {
            Atom a1 = mw_intern_atom(d, "_SUN_TT_SESSION", False);
            Atom a2 = mw_intern_atom(d, "TT_SESSION", False);
            mw_set_prop(d, root, a1, XA_STRING, 8,
                        (const unsigned char *)tt, strlen(tt));
            mw_set_prop(d, root, a2, XA_STRING, 8,
                        (const unsigned char *)tt, strlen(tt));
        }
    }

    /* The panel (dtwm) must be able to claim the screen: it checks
     * _MOTIF_WM_INFO to see whether a window manager is already running, so
     * publishing a synthetic one here would make it stand down.  The session
     * starts dtwm with CDE_NO_WM_INFO set.
     *
     * The opt-out applies to dtwm *alone*, not to the applications it launches:
     * dtwm passes its environment on, and a child that inherited the variable
     * would never get the synthetic WM window, so its DtWsmGetWorkspaceList()
     * would fail (dtfile, for one, then retries and stalls before mapping its
     * window).  So consume the variable -- unset it in this process -- after
     * honouring it once; what dtwm spawns from here on is unaffected. */
    static int no_wm_info = -1;      /* -1 unknown, 0 no, 1 yes */
    if (no_wm_info < 0) {
        no_wm_info = getenv("CDE_NO_WM_INFO") ? 1 : 0;
        if (no_wm_info)
            unsetenv("CDE_NO_WM_INFO");
    }
    if (no_wm_info) return;

    MwWindow *wm = mw_create_window(d, root->id, 0, 0, 1, 1, 0, 24,
                                    InputOutput, &dp->visual, 0, NULL);
    if (!wm) return;

    Atom mwm_info = mw_intern_atom(d, "_MOTIF_WM_INFO", False);
    uint32_t info[2];
    info[0] = 1;                  /* flags */
    info[1] = (uint32_t)wm->id;   /* wmWindow */
    mw_set_prop(d, root, mwm_info, mwm_info, 32, (const unsigned char *)info, 2);

    mw_set_workspace_props(d, wm);
}

/* ------------------------------------------------------------ creation */

MwWindow *mw_create_window(Display *d, Window parent, int x, int y, int w, int h,
                           int border_width, int depth, unsigned int c_class,
                           Visual *visual, unsigned long valuemask,
                           XSetWindowAttributes *attr)
{
    MwWindow *win = calloc(1, sizeof *win);
    win->id = mw_alloc_id(d);
    win->d = d;
    win->x = x; win->y = y;
    win->w = w; win->h = h;
    win->border_width = border_width;
    win->depth = depth;
    win->c_class = (int)c_class;
    win->input_only = (c_class == InputOnly);
    win->created = true;
    win->mapped = false;
    win->map_state = IsUnmapped;
    win->background_pixel = MWSCR(d)->black_pixel;
    win->border_pixel = MWSCR(d)->black_pixel;
    win->colormap = MWSCR(d)->cmap;
    win->cursor = None;
    win->bit_gravity = ForgetGravity;
    win->win_gravity = NorthWestGravity;
    win->backing_store = NotUseful;
    win->event_mask = 0;

    if (attr) {
        /* A window has a single background: a pixel or a pixmap.  A None
         * background pixmap does not count as "having" a background, so the
         * window still inherits its parent's (see effective_background). */
        if (valuemask & CWBackPixel)    { win->background_pixel = attr->background_pixel; win->background_pixmap = None; win->have_background = true; }
        if (valuemask & CWBackPixmap)   { win->background_pixmap = attr->background_pixmap; if (attr->background_pixmap != None) win->have_background = true; }
        if (valuemask & CWBorderPixel)  win->border_pixel = attr->border_pixel;
        if (valuemask & CWBorderPixmap) win->border_pixmap = attr->border_pixmap;
        if (valuemask & CWBitGravity)   win->bit_gravity = attr->bit_gravity;
        if (valuemask & CWWinGravity)   win->win_gravity = attr->win_gravity;
        if (valuemask & CWBackingStore) win->backing_store = attr->backing_store;
        if (valuemask & CWBackingPlanes)win->backing_planes = attr->backing_planes;
        if (valuemask & CWBackingPixel) win->backing_pixel = attr->backing_pixel;
        if (valuemask & CWSaveUnder)    win->save_under = attr->save_under;
        if (valuemask & CWEventMask)    win->event_mask = attr->event_mask;
        if (valuemask & CWDontPropagate)win->do_not_propagate_mask = attr->do_not_propagate_mask;
        if (valuemask & CWOverrideRedirect) win->override_redirect = attr->override_redirect;
        if (valuemask & CWColormap)     win->colormap = attr->colormap;
        if (valuemask & CWCursor)       win->cursor = attr->cursor;
    }
    win->all_event_masks = win->event_mask;

    mw_register(d, win->id, MW_OBJ_WINDOW, win);
    if (getenv("MW_TRACE"))
        fprintf(stderr, "MW: XCreateWindow id=0x%lx %dx%d@%d,%d parent=0x%lx class=%u override=%d\n",
                win->id, w, h, x, y, parent, c_class, win->override_redirect);

    if (parent != None) {
        MwWindow *par = mw_window(d, parent);
        if (par) link_top(par, win);
        /* X inherits the background from the parent when the caller asks for
         * CopyFromParent (the usual case, and what all of Xt's widget windows
         * do).  Without this a window that is created or recreated without an
         * explicit background is painted black instead of its parent's
         * colour -- which is what made a resized window's newly exposed area
         * come out black. */
        if (par && !win->have_background && par->have_background) {
            win->background_pixel = par->background_pixel;
            win->have_background = true;
        }
        if (par && (par->event_mask & SubstructureNotifyMask)) {
            XCreateWindowEvent ce;
            memset(&ce, 0, sizeof ce);
            ce.type = CreateNotify; ce.display = d; ce.parent = par->id;
            ce.window = win->id; ce.x = x; ce.y = y;
            ce.width = w; ce.height = h; ce.border_width = border_width;
            ce.override_redirect = win->override_redirect;
            mw_put_event(d, (XEvent *)&ce);
        }
    }
    return win;
}

/* When a menu closes, the open menu becomes the parent menu it nested under --
 * but a top-level menu's popup_parent is the ordinary toplevel it was anchored
 * to, which is not a menu.  Only keep a parent that is itself a live menu, or
 * the focus-deferral for "a menu is open" would outlive the menu. */
static MwWindow *open_menu_after(MwWindow *win)
{
    MwWindow *par = win->popup_parent;
    if (par && par->override_redirect && par->mapped &&
        par->tl && par->tl->is_popup)
        return par;
    return NULL;
}

void mw_destroy_window(Display *d, MwWindow *win)
{
    if (!win) return;
    /* destroy children */
    while (win->last_child) mw_destroy_window(d, win->last_child);

    bool was_mapped = win->mapped;
    if (win->tl) { mw_toplevel_destroy(win->tl); win->tl = NULL; }

    /* Forget this window in every piece of display state that refers to it.
     * The children have already been destroyed above, so each of them has
     * cleared its own references by the time control reaches here.
     * Missing this left dangling pointers into freed memory: a client that
     * destroys a dialog (NEdit does after File -> Open succeeds) then finds
     * the pointer/keyboard/grab state still aimed at the freed window, and
     * every later event -- menu bar clicks included -- is dispatched through
     * it instead of the live window. */
    {
        XDisplayImpl *dp = MWD(d);
        /* Hand the anchor over to another live toplevel rather than
         * clearing it: a popup posted before the pointer or focus state has
         * caught up still needs somewhere to anchor. */
        if (dp->active_toplevel == win)
            dp->active_toplevel = mw_any_mapped_toplevel(d, win);
        if (dp->open_menu      == win) dp->open_menu      = open_menu_after(win);
        if (dp->ptr_window     == win) dp->ptr_window     = NULL;
        if (dp->ptr_toplevel   == win) dp->ptr_toplevel   = NULL;
        if (dp->ptr_focus      == win) dp->ptr_focus      = NULL;
        if (dp->kbd_focus      == win) {
            /* X delivers FocusOut when the focus window is destroyed; without
             * it GDK kept pointing at the dead window and ignored the next
             * window's FocusIn. */
            if (win->event_mask & FocusChangeMask) {
                XFocusChangeEvent fe;
                memset(&fe, 0, sizeof fe);
                fe.type = FocusOut; fe.display = d; fe.window = win->id;
                fe.mode = NotifyNormal; fe.detail = NotifyNonlinear;
                mw_put_event(d, (XEvent *)&fe);
            }
            dp->kbd_focus      = NULL;
        }
        /* An active grab whose window is destroyed has to be released, not
         * just forgotten: leaving ptr_grab_active set with a NULL grab window
         * made ptr_button()/ptr_motion() drop every event on the floor, so
         * the application stopped responding to the pointer entirely.  That
         * is what happened once a modal dialog that had grabbed the pointer
         * was destroyed (NEdit destroys the whole dialog after a successful
         * File -> Open). */
        if (dp->ptr_grab_window == win) {
            dp->ptr_grab_window = NULL;
            dp->ptr_grab_active = false;
            dp->ptr_grab_temporary = false;
            dp->ptr_grab_mask = 0;
        }
        if (dp->implicit_grab  == win) dp->implicit_grab  = NULL;
        if (dp->kbd_grab_window == win) dp->kbd_grab_window = NULL;
    }

    MwWindow *par = win->parent;
    if (par && (par->event_mask & SubstructureNotifyMask)) {
        XDestroyWindowEvent de;
        memset(&de, 0, sizeof de);
        de.type = DestroyNotify; de.display = d; de.event = par->id;
        de.window = win->id;
        mw_put_event(d, (XEvent *)&de);
    }
    if (win->event_mask & StructureNotifyMask) {
        XDestroyWindowEvent de;
        memset(&de, 0, sizeof de);
        de.type = DestroyNotify; de.display = d; de.event = win->id;
        de.window = win->id;
        mw_put_event(d, (XEvent *)&de);
    }

    unlink_child(win);
    while (win->props) { MwProp *p = win->props; win->props = p->next;
                         free(p->data); free(p); }
    if (win->surface) mw_surface_destroy(win->surface);
    if (win->has_shape) pixman_region32_fini(&win->shape);
    mw_unregister(d, win->id);
    free(win);
    (void)was_mapped;
}

void mw_map_window(Display *d, MwWindow *win, bool raised)
{
    if (!win || win->mapped) return;
    win->mapped = true;
    win->map_state = IsViewable;
    if (getenv("MW_TRACE"))
        fprintf(stderr, "MW: XMapWindow id=0x%lx %dx%d toplevel=%d\n",
                win->id, win->w, win->h, win->parent && is_root(d, win->parent));

    /* A window becomes its own Wayland toplevel when its parent is the root
     * *or* when it is override-redirect.  On X an override-redirect window
     * bypasses the window manager regardless of its parent, and Motif parents
     * a menu popup to the application's toplevel shell -- not the root.  If we
     * only tested for the root, such a popup would instead be composited into
     * its parent's buffer: it would look open but would have no wl_surface,
     * no xdg_popup and no popup grab, so the compositor would keep routing
     * pointer input to the parent and the menu items would never be
     * selectable. */
    /* Only windows that are actually meant to be on screen become Wayland
     * surfaces.  An override-redirect window has to be anchored as a popup:
     * window managers do not manage those on X, but a Wayland toplevel is
     * always shown, so promoting an unanchorable one put a stray window on
     * screen next to the application window. */
    bool promotable;
    bool focus_toplevel = false;
    if (win->input_only) {
        /* An InputOnly window has no pixels at all; it exists purely to
         * receive events.  Motif uses a 10x10 InputOnly window at (-100,-100)
         * as its modal grab window, and mapping that as a Wayland surface
         * gave it the input focus -- with an xdg_popup grab on top -- so
         * every click went to the invisible helper and the real dialog never
         * saw one.  It also showed up as a stray window on screen. */
        promotable = false;
    } else if (win->override_redirect) {
        /* Only as a popup anchored to an ordinary toplevel.  There is no
         * "unmanaged but visible" toplevel on Wayland, so a helper the
         * application never expected anyone to show (Motif's 10x10 window, or
         * dtwm's full-screen workspace/backdrop overlays, which are meant to be
         * invisible root covers) must stay out of the Wayland tree rather than
         * appear as a stray window.  A legitimate override-redirect *panel* is
         * not override-redirect in this sense -- CDE's front panel window is a
         * normal root child and is promoted by the branch above. */
        /* A window that covers the whole screen is a root cover, not a popup:
         * dtwm maps its per-workspace backdrop this way, and promoting it as a
         * popup paints the desktop (and the panel) over. */
        bool fullscreen = (win->w >= MWSCR(d)->width && win->h >= MWSCR(d)->height);
        promotable = !fullscreen && (mw_popup_anchor(win) != NULL);
    } else {
        promotable = win->parent && is_root(d, win->parent);
    }

    if (promotable) {
        /* A popup the compositor dismissed has an inert xdg_popup that must
         * not be re-used: rebuild the Wayland object set for this posting.
         * Deferring it to here (rather than doing it in the popup_done
         * handler) means the X window always has a valid toplevel between
         * events, and Motif's re-post is what triggers the rebuild. */
        if (win->tl && win->tl->popup_dismissed) {
            MwToplevel *old = win->tl;
            win->tl = NULL;
            mw_toplevel_destroy(old);
        }
        if (!win->tl) mw_toplevel_create(win);
        mw_toplevel_map(win->tl);
        /* Remember this as the application toplevel a popup should anchor to
         * (Motif menu shells are parented to the root, so the X hierarchy
         * cannot be used to find it). */
        if (!win->override_redirect) {
            MWD(d)->active_toplevel = win;
            focus_toplevel = true;
        } else if (win->tl && win->tl->is_popup) {
            /* Remember the open menu so a submenu mapped next nests under it
             * rather than becoming a sibling popup (see mw_popup_anchor). */
            MWD(d)->open_menu = win;
        }
        mw_window_expose(win, 0, 0, win->w, win->h);
    } else if (win->parent) {
        /* InputOnly windows have no visible surface to build. */
        if (!win->input_only) {
            mw_window_ensure_surface(win);
            mw_window_damage(win);
        }
        mw_window_expose(win, 0, 0, win->w, win->h);
    }
    if (win->parent && (win->parent->event_mask & SubstructureNotifyMask)) {
        /* The parent's SubstructureNotify view of a newly mapped child. */
        XMapEvent me;
        memset(&me, 0, sizeof me);
        me.type = MapNotify; me.display = d; me.event = win->parent->id;
        me.window = win->id; me.override_redirect = win->override_redirect;
        mw_put_event(d, (XEvent *)&me);
    }
    if (win->event_mask & StructureNotifyMask) {
        XMapEvent me;
        memset(&me, 0, sizeof me);
        me.type = MapNotify; me.display = d; me.event = win->id; me.window = win->id;
        me.override_redirect = win->override_redirect;
        mw_put_event(d, (XEvent *)&me);
    }
    /* A window manager focuses a newly mapped toplevel.  Mirror that here,
     * after the MapNotify (GTK ignores a FocusIn for a not-yet-mapped window),
     * so the client sees the focus before its event loop returns -- otherwise
     * the compositor's later wl_keyboard.enter lands after the client has
     * already checked gtk_widget_has_focus(). */
    if (focus_toplevel) {
        MwWindow *mc = MWD(d)->kbd_focus;
        if (mc != win) {
            if (mc && (mc->event_mask & FocusChangeMask)) {
                XFocusChangeEvent fo;
                memset(&fo, 0, sizeof fo);
                fo.type = FocusOut; fo.display = d; fo.window = mc->id;
                fo.mode = NotifyNormal; fo.detail = NotifyNonlinear;
                mw_put_event(d, (XEvent *)&fo);
            }
            MWD(d)->kbd_focus = win;
            if (getenv("MW_TRACE"))
                fprintf(stderr, "MW: map-FocusIn win=0x%lx mask=0x%lx focusselmask=%d\n",
                        (unsigned long)win->id, win->event_mask,
                        (win->event_mask & FocusChangeMask) != 0);
            if (win->event_mask & FocusChangeMask) {
                XFocusChangeEvent fe;
                memset(&fe, 0, sizeof fe);
                fe.type = FocusIn; fe.display = d; fe.window = win->id;
                fe.mode = NotifyNormal; fe.detail = NotifyNonlinear;
                mw_put_event(d, (XEvent *)&fe);
            }
        }
    }
    (void)raised;
}

/* Pick a mapped ordinary toplevel to stand in when the active one goes away,
 * so a popup posted afterwards is not anchored to a window that is no longer
 * on screen (a dialog that has just been closed, for example). */
MwWindow *mw_any_mapped_toplevel(Display *d, MwWindow *avoid)
{
    MwWindow *root = mw_window(d, MWSCR(d)->root);
    if (!root) return NULL;
    for (MwWindow *c = root->children; c; c = c->next_sib)
        if (c != avoid && c->mapped && c->tl && !c->override_redirect)
            return c;
    return NULL;
}

void mw_unmap_window(Display *d, MwWindow *win)
{
    if (!win || !win->mapped) return;
    win->mapped = false;
    win->map_state = IsUnmapped;
    if (MWD(d)->active_toplevel == win)
        MWD(d)->active_toplevel = mw_any_mapped_toplevel(d, win);
    if (MWD(d)->open_menu == win)
        MWD(d)->open_menu = open_menu_after(win);
    if (win->tl) mw_toplevel_unmap(win->tl);
    if (win->parent) {
        /* The parent is told about a child being unmapped whatever role the
         * child plays: on X every window has a parent, and a menu shell
         * parented to the application's toplevel is no exception.  Skipping
         * this for windows that are also Wayland toplevels left clients that
         * track their popups through SubstructureNotifyMask (as Motif does
         * for menu shells) believing a dismissed menu was still posted. */
        if (win->parent->event_mask & SubstructureNotifyMask) {
            XUnmapEvent ue;
            memset(&ue, 0, sizeof ue);
            ue.type = UnmapNotify; ue.display = d; ue.event = win->parent->id;
            ue.window = win->id; ue.from_configure = False;
            mw_put_event(d, (XEvent *)&ue);
        }
        mw_window_damage(win->parent);
        /* siblings below become exposed */
        for (MwWindow *c = win->parent->children; c; c = c->next_sib)
            if (c->mapped) mw_window_expose(c, 0, 0, c->w, c->h);
    }
    if (win->event_mask & StructureNotifyMask) {
        XUnmapEvent ue;
        memset(&ue, 0, sizeof ue);
        ue.type = UnmapNotify; ue.display = d; ue.event = win->id; ue.window = win->id;
        mw_put_event(d, (XEvent *)&ue);
    }
}

void mw_window_expose(MwWindow *win, int x, int y, int w, int h)
{
    Display *d = win->d;
    if (!d) return;
    if (getenv("MW_TRACE"))
        fprintf(stderr, "MW: expose win=0x%lx %d,%d %dx%d\n", win->id, x, y, w, h);
    if (win->event_mask & ExposureMask) {
        XExposeEvent ee;
        memset(&ee, 0, sizeof ee);
        ee.type = Expose; ee.display = d; ee.window = win->id;
        ee.x = x; ee.y = y; ee.width = w; ee.height = h; ee.count = 0;
        mw_put_event(d, (XEvent *)&ee);
    }
}

/* Send Expose to a window and every mapped descendant, which is what the X
 * server does for the regions that become visible when a window grows.  The
 * child widgets are separate X windows and repaint only when they receive
 * their own Expose, so exposing just the top-level left the grown area blank.
 * It rendered black because a child window's backing surface is recreated on
 * resize and, having no background of its own, is cleared to black. */
/* The compositor resized one of our toplevels -- an interactive resize now
 * that windows have server-side decorations.  Resize the X window *and tell
 * the client*, exactly as an X server does after the window manager
 * reconfigures a window.  Without the notification the application never
 * learns it has more room and its contents stay at the old size, so the
 * client area appeared not to resize. */
/* Shared by the client-initiated resize path (XResizeWindow and friends) and
 * the compositor-initiated one (mw_window_wm_resize).
 *
 * Only a client-side resize is reported back with xdg_toplevel.set_size():
 * echoing the compositor's own configure would be pointless, but a client that
 * resizes itself -- Motif snaps to character cells, and re-lays out as it
 * grows -- has to tell the compositor, or the two disagree for the rest of an
 * interactive resize and the window visibly overshoots and springs back. */
static void expose_area_tree(MwWindow *win, int x, int y, int w, int h)
{
    if (w <= 0 || h <= 0) return;
    mw_window_expose(win, x, y, w, h);
    for (MwWindow *c = win->children; c; c = c->next_sib) {
        if (!c->mapped) continue;
        int x0 = c->x > x ? c->x : x;
        int y0 = c->y > y ? c->y : y;
        int x1 = (c->x + c->w) < (x + w) ? (c->x + c->w) : (x + w);
        int y1 = (c->y + c->h) < (y + h) ? (c->y + c->h) : (y + h);
        if (x1 > x0 && y1 > y0)
            expose_area_tree(c, x0 - c->x, y0 - c->y, x1 - x0, y1 - y0);
    }
}

static void window_resize(MwWindow *win, int x, int y, int w, int h)
{
    win->x = x; win->y = y;
    bool resized = (w != win->w || h != win->h);
    win->w = w; win->h = h;
    /* Resizing discards the window's contents (ForgetGravity is the default,
     * and what Motif's widgets ask for) and a server then exposes the whole
     * window.  Exposing only the newly uncovered strip looked tempting -- it
     * is what a preserving bit gravity would give -- but Motif lays widgets
     * out again after a resize and repaints only as far as the Expose directs,
     * so partial exposes left the new area (and the scroll bar) unpainted
     * until something else forced a redraw. */
    if (resized) {
        win->surface_valid = false;
        if (win->tl) mw_toplevel_set_size(win->tl, w, h);
    }
    if (win->tl) {
        if (win->tl->is_popup) mw_toplevel_reposition(win->tl);
        mw_toplevel_damage(win->tl);
    } else mw_window_damage(win);

    if (resized && win->mapped)
        expose_area_tree(win, 0, 0, w, h);
}

void mw_window_wm_resize(MwWindow *win, int w, int h)
{
    if (!win || w < 1 || h < 1) return;
    if (w == win->w && h == win->h) return;
    window_resize(win, win->x, win->y, w, h);
    if (win->tl) mw_send_configure_notify(win->d, win);
}

void mw_window_move_resize(MwWindow *win, int x, int y, int w, int h)
{
    window_resize(win, x, y, w, h);
}

/* --------------------------------------------------------- hit testing */

MwWindow *mw_child_at(MwWindow *parent, int x, int y)
{
    for (MwWindow *c = parent->last_child; c; c = c->prev_sib) {
        if (!c->mapped) continue;
        if (x >= c->x && y >= c->y && x < c->x + c->w && y < c->y + c->h)
            return c;
    }
    return NULL;
}

MwWindow *mw_deepest_at(MwWindow *top, int x, int y, int *cx, int *cy)
{
    int rx = x, ry = y;
    MwWindow *w = top;
    for (;;) {
        MwWindow *c = mw_child_at(w, rx, ry);
        if (!c) break;
        rx -= c->x; ry -= c->y;
        w = c;
    }
    if (cx) *cx = rx;
    if (cy) *cy = ry;
    return w;
}

/* ------------------------------------------------------ drawable helpers */

MwSurface *mw_drawable_surface(Display *d, Drawable dr, int *w, int *h, int *depth)
{
    MwWindow *win = mw_window(d, dr);
    if (win) {
        mw_window_ensure_surface(win);
        if (w) *w = win->w;
        if (h) *h = win->h;
        if (depth) *depth = win->depth;
        return win->surface;
    }
    MwPixmap *pm = mw_pixmap(d, dr);
    if (pm) {
        if (w) *w = pm->w;
        if (h) *h = pm->h;
        if (depth) *depth = pm->depth;
        return pm->surface;
    }
    return NULL;
}

int mw_drawable_origin(Display *d, Drawable dr, int *x, int *y)
{
    MwWindow *win = mw_window(d, dr);
    if (win) { mw_window_origin(win, x, y); return 1; }
    if (x) *x = 0;
    if (y) *y = 0;
    return 0;
}

/* ------------------------------------------------ Xlib window functions */

Window XCreateWindow(Display *d, Window parent, int x, int y,
                     unsigned int width, unsigned int height,
                     unsigned int border_width, int depth, unsigned int c_class,
                     Visual *visual, unsigned long valuemask,
                     XSetWindowAttributes *attributes)
{
    MwWindow *w = mw_create_window(d, parent, x, y, (int)width, (int)height,
                                   (int)border_width, depth, c_class, visual,
                                   valuemask, attributes);
    return w ? w->id : None;
}

Window XCreateSimpleWindow(Display *d, Window parent, int x, int y,
                           unsigned int width, unsigned int height,
                           unsigned int border_width, unsigned long border,
                           unsigned long background)
{
    XSetWindowAttributes a;
    memset(&a, 0, sizeof a);
    a.background_pixel = background;
    a.border_pixel = border;
    a.event_mask = 0;
    a.colormap = MWSCR(d)->cmap;
    return XCreateWindow(d, parent, x, y, width, height, border_width,
                         CopyFromParent, InputOutput, CopyFromParent,
                         CWBackPixel | CWBorderPixel | CWColormap, &a);
}

int XDestroyWindow(Display *d, Window w)
{
    mw_render_drain(d);
    MwWindow *win = mw_window(d, w);
    if (win) mw_destroy_window(d, win);
    return 1;
}

int XDestroySubwindows(Display *d, Window w)
{
    MwWindow *win = mw_window(d, w);
    if (!win) return 0;
    while (win->last_child) mw_destroy_window(d, win->last_child);
    return 1;
}

int XMapWindow(Display *d, Window w)
{
    MwWindow *win = mw_window(d, w);
    if (win) mw_map_window(d, win, false);
    return 1;
}

int XMapRaised(Display *d, Window w)
{
    MwWindow *win = mw_window(d, w);
    if (win) mw_map_window(d, win, true);
    return 1;
}

int XMapSubwindows(Display *d, Window w)
{
    MwWindow *win = mw_window(d, w);
    if (win) for (MwWindow *c = win->children; c; c = c->next_sib)
        mw_map_window(d, c, false);
    return 1;
}

int XUnmapWindow(Display *d, Window w)
{
    MwWindow *win = mw_window(d, w);
    if (win) mw_unmap_window(d, win);
    return 1;
}

int XUnmapSubwindows(Display *d, Window w)
{
    MwWindow *win = mw_window(d, w);
    if (win) for (MwWindow *c = win->children; c; c = c->next_sib)
        mw_unmap_window(d, c);
    return 1;
}

/* A real server sends a ConfigureNotify to a window that has selected
 * StructureNotifyMask whenever it is moved or resized, and to the parent when
 * the parent has selected SubstructureNotifyMask.  The latter used to be the
 * only case handled here (and only for non-toplevels), which broke Xt's
 * window-manager negotiation: with XtNwaitForWm set, Xt will not update a
 * shell's geometry until it sees the ConfigureNotify for the size it asked
 * for, so an XmDialogShell stayed at the 1x1 it was created with and its
 * FileSelectionBox children -- the Cancel button included -- were laid out
 * outside the window and could never be hit. */
static void dispatch_configure_notify(Display *d, MwWindow *win)
{
    if (win->tl || (win->event_mask & StructureNotifyMask))
        mw_send_configure_notify(d, win);
    if (win->parent && (win->parent->event_mask & SubstructureNotifyMask))
        mw_send_configure_notify(d, win);
}

int XReconfigureWMWindow(Display *d, Window w, int screen, unsigned int value_mask,
                         XWindowChanges *changes)
{
    /* A client would normally ask the window manager to do this with a
     * _NET_MOVERESIZE_WINDOW message.  Our windows are their own toplevels and
     * the compositor positions them, so applying the change to the window
     * directly is both correct and simpler. */
    (void)screen;
    return XConfigureWindow(d, w, value_mask, changes);
}

int XConfigureWindow(Display *d, Window w, unsigned int value_mask,
                     XWindowChanges *values)
{
    MwWindow *win = mw_window(d, w);
    if (!win) return 0;
    int x = win->x, y = win->y;
    int width = win->w, height = win->h;
    if (value_mask & CWX) x = values->x;
    if (value_mask & CWY) y = values->y;
    if (value_mask & CWWidth) width = values->width;
    if (value_mask & CWHeight) height = values->height;
    if (value_mask & CWBorderWidth) win->border_width = values->border_width;
    if (value_mask & (CWX | CWY | CWWidth | CWHeight))
        mw_window_move_resize(win, x, y, width, height);
    dispatch_configure_notify(d, win);
    mw_window_damage(win);
    return 1;
}

int XMoveWindow(Display *d, Window w, int x, int y)
{
    MwWindow *win = mw_window(d, w);
    if (!win) return 0;
    mw_window_move_resize(win, x, y, win->w, win->h);
    dispatch_configure_notify(d, win);
    return 1;
}

int XResizeWindow(Display *d, Window w, unsigned int width, unsigned int height)
{
    MwWindow *win = mw_window(d, w);
    if (!win) return 0;
    mw_window_move_resize(win, win->x, win->y, (int)width, (int)height);
    dispatch_configure_notify(d, win);
    return 1;
}

int XMoveResizeWindow(Display *d, Window w, int x, int y,
                      unsigned int width, unsigned int height)
{
    MwWindow *win = mw_window(d, w);
    if (!win) return 0;
    mw_window_move_resize(win, x, y, (int)width, (int)height);
    /* window_resize() has already exposed the window; exposing
     * the whole window here as well made every widget repaint from scratch on
     * each step of an interactive resize. */
    dispatch_configure_notify(d, win);
    return 1;
}

int XRaiseWindow(Display *d, Window w)
{
    MwWindow *win = mw_window(d, w);
    if (!win || !win->parent) return 0;
    /* unlink_child() clears win->parent, so it has to be read first --
     * dereferencing it afterwards crashed on the NULL.  NEdit calls this to
     * raise a document window as soon as a file has been loaded, so the whole
     * application died the moment an Open succeeded. */
    MwWindow *par = win->parent;
    unlink_child(win);
    link_top(par, win);
    mw_window_damage(par);
    return 1;
}

int XLowerWindow(Display *d, Window w)
{
    MwWindow *win = mw_window(d, w);
    if (!win || !win->parent) return 0;
    MwWindow *par = win->parent;
    unlink_child(win);
    win->parent = par;
    win->next_sib = par->children;
    win->prev_sib = NULL;
    if (par->children) par->children->prev_sib = win;
    par->children = win;
    if (!par->last_child) par->last_child = win;
    mw_window_damage(par);
    return 1;
}

int XReparentWindow(Display *d, Window w, Window parent, int x, int y)
{
    MwWindow *win = mw_window(d, w);
    MwWindow *par = mw_window(d, parent);
    if (!win || !par) return 0;
    unlink_child(win);
    link_top(par, win);
    win->x = x; win->y = y;
    mw_window_damage(win);
    return 1;
}

static void clear_win_rect(MwWindow *win, int x, int y, int w, int h)
{
    paint_background(win, x, y, w, h);
}

int XClearWindow(Display *d, Window w)
{
    MwWindow *win = mw_window(d, w);
    if (!win) return 0;
    mw_window_ensure_surface(win);
    clear_win_rect(win, 0, 0, win->w, win->h);
    mw_window_damage(win);
    /* XClearWindow == XClearArea(..., exposures=False): it must NOT generate
     * an Expose event.  Generating one makes clear-then-redraw clients (XV)
     * loop forever. */
    return 1;
}

int XClearArea(Display *d, Window w, int x, int y,
               unsigned int width, unsigned int height, Bool exposures)
{
    MwWindow *win = mw_window(d, w);
    if (!win) return 0;
    mw_window_ensure_surface(win);
    int cw = width ? (int)width : win->w - x;
    int ch = height ? (int)height : win->h - y;
    if (getenv("MW_TRACE"))
        fprintf(stderr, "MW: XClearArea 0x%lx %d,%d %dx%d exposures=%d\n",
                w, x, y, cw, ch, exposures);
    clear_win_rect(win, x, y, cw, ch);
    mw_window_damage(win);
    if (exposures) mw_window_expose(win, x, y, cw, ch);
    return 1;
}

Status XGetGeometry(Display *d, Drawable dr, Window *root_return,
                    int *x_return, int *y_return,
                    unsigned int *width_return, unsigned int *height_return,
                    unsigned int *border_width_return, unsigned int *depth_return)
{
    MwWindow *win = mw_window(d, dr);
    if (win) {
        if (root_return) *root_return = MWSCR(d)->root;
        if (x_return) *x_return = win->x;
        if (y_return) *y_return = win->y;
        if (width_return) *width_return = win->w;
        if (height_return) *height_return = win->h;
        if (border_width_return) *border_width_return = win->border_width;
        if (depth_return) *depth_return = win->depth;
        return 1;
    }
    MwPixmap *pm = mw_pixmap(d, dr);
    if (pm) {
        if (root_return) *root_return = MWSCR(d)->root;
        if (x_return) *x_return = 0;
        if (y_return) *y_return = 0;
        if (width_return) *width_return = pm->w;
        if (height_return) *height_return = pm->h;
        if (border_width_return) *border_width_return = 0;
        if (depth_return) *depth_return = pm->depth;
        return 1;
    }
    return 0;
}

Status XGetWindowAttributes(Display *d, Window w, XWindowAttributes *attr)
{
    MwWindow *win = mw_window(d, w);
    if (!win) return 0;
    memset(attr, 0, sizeof *attr);
    attr->x = win->x; attr->y = win->y;
    attr->width = win->w; attr->height = win->h;
    attr->border_width = win->border_width;
    attr->depth = win->depth;
    attr->visual = &MWD(d)->visual;
    attr->root = MWSCR(d)->root;
    attr->class = win->c_class;
    attr->bit_gravity = win->bit_gravity;
    attr->win_gravity = win->win_gravity;
    attr->backing_store = win->backing_store;
    attr->backing_planes = win->backing_planes;
    attr->backing_pixel = win->backing_pixel;
    attr->save_under = win->save_under;
    attr->colormap = win->colormap;
    attr->map_installed = True;
    attr->map_state = win->mapped ? IsViewable : IsUnmapped;
    attr->all_event_masks = win->all_event_masks;
    attr->your_event_mask = win->event_mask;
    attr->do_not_propagate_mask = win->do_not_propagate_mask;
    attr->override_redirect = win->override_redirect;
    attr->screen = MWSCR(d);
    return 1;
}

int XChangeWindowAttributes(Display *d, Window w, unsigned long mask,
                            XSetWindowAttributes *attr)
{
    MwWindow *win = mw_window(d, w);
    if (!win || !attr) return 0;
    if (mask & CWBackPixel)    { win->background_pixel = attr->background_pixel; win->background_pixmap = None; win->have_background = true; }
    if (mask & CWBackPixmap)   { win->background_pixmap = attr->background_pixmap; if (attr->background_pixmap != None) win->have_background = true; }
    if (mask & CWBorderPixel)  win->border_pixel = attr->border_pixel;
    if (mask & CWBorderPixmap) win->border_pixmap = attr->border_pixmap;
    if (mask & CWBitGravity)   win->bit_gravity = attr->bit_gravity;
    if (mask & CWWinGravity)   win->win_gravity = attr->win_gravity;
    if (mask & CWBackingStore) win->backing_store = attr->backing_store;
    if (mask & CWBackingPlanes)win->backing_planes = attr->backing_planes;
    if (mask & CWBackingPixel) win->backing_pixel = attr->backing_pixel;
    if (mask & CWSaveUnder)    win->save_under = attr->save_under;
    if (mask & CWEventMask)    { win->event_mask = attr->event_mask; win->all_event_masks = attr->event_mask; }
    if (mask & CWDontPropagate)win->do_not_propagate_mask = attr->do_not_propagate_mask;
    if (mask & CWOverrideRedirect) win->override_redirect = attr->override_redirect;
    if (mask & CWColormap)     win->colormap = attr->colormap;
    if (mask & CWCursor)       { win->cursor = attr->cursor; }
    return 1;
}

Status XQueryTree(Display *d, Window w, Window *root_return, Window *parent_return,
                  Window **children_return, unsigned int *nchildren_return)
{
    MwWindow *win = mw_window(d, w);
    if (!win) return 0;
    if (root_return) *root_return = MWSCR(d)->root;
    if (parent_return) *parent_return = win->parent ? win->parent->id : None;
    int n = 0;
    for (MwWindow *c = win->children; c; c = c->next_sib) n++;
    Window *arr = n ? malloc(sizeof(Window) * n) : NULL;
    int i = 0;
    for (MwWindow *c = win->children; c; c = c->next_sib) arr[i++] = c->id;
    if (children_return) *children_return = arr; else free(arr);
    if (nchildren_return) *nchildren_return = n;
    return 1;
}

Bool XTranslateCoordinates(Display *d, Window src, Window dest,
                           int src_x, int src_y, int *dest_x, int *dest_y,
                           Window *child_return)
{
    MwWindow *s = mw_window(d, src);
    MwWindow *t = mw_window(d, dest);
    if (!s || !t) return False;
    int sx, sy, tx, ty;
    mw_window_origin(s, &sx, &sy);
    mw_window_origin(t, &tx, &ty);
    int rx = src_x + sx, ry = src_y + sy;
    if (dest_x) *dest_x = rx - tx;
    if (dest_y) *dest_y = ry - ty;
    if (child_return) *child_return = None;
    return True;
}

int XSetWindowBackground(Display *d, Window w, unsigned long pixel)
{
    MwWindow *win = mw_window(d, w);
    if (win) {
        win->background_pixel = pixel;
        win->background_pixmap = None;
        win->have_background = true;
        if (win->surface) paint_background(win, 0, 0, win->w, win->h);
    }
    return 1;
}

int XSetWindowBackgroundPixmap(Display *d, Window w, Pixmap pixmap)
{
    MwWindow *win = mw_window(d, w);
    if (win) {
        win->background_pixmap = pixmap;
        win->have_background = (pixmap != None);
        if (win->surface) paint_background(win, 0, 0, win->w, win->h);
    }
    return 1;
}

int XSetWindowBorder(Display *d, Window w, unsigned long pixel)
{
    MwWindow *win = mw_window(d, w);
    if (win) win->border_pixel = pixel;
    return 1;
}

int XSetWindowBorderPixmap(Display *d, Window w, Pixmap p)
{ MwWindow *win = mw_window(d, w); if (win) win->border_pixmap = p; return 1; }
int XSetWindowBorderWidth(Display *d, Window w, unsigned int bw)
{ MwWindow *win = mw_window(d, w); if (win) win->border_width = bw; return 1; }
int XSetWindowColormap(Display *d, Window w, Colormap cmap)
{ MwWindow *win = mw_window(d, w); if (win) win->colormap = cmap; return 1; }

int XSetInputFocus(Display *d, Window w, int revert_to, Time time)
{
    (void)revert_to; (void)time;
    if (getenv("MW_TRACE"))
        fprintf(stderr, "MW: XSetInputFocus -> 0x%lx\n", w);
    MWD(d)->kbd_focus = mw_window(d, w);
    MWD(d)->focus_explicit = true;
    return 1;
}

int XGetInputFocus(Display *d, Window *focus, int *revert_to)
{
    if (focus) *focus = MWD(d)->kbd_focus ? MWD(d)->kbd_focus->id : None;
    if (revert_to) *revert_to = RevertToParent;
    return 1;
}

Status XIconifyWindow(Display *d, Window w, int screen)
{
    (void)screen;
    MwWindow *win = mw_window(d, w);
    if (win && win->tl && win->tl->xdg_toplevel)
        xdg_toplevel_set_minimized(win->tl->xdg_toplevel);
    return 1;
}

Status XWithdrawWindow(Display *d, Window w, int screen)
{
    (void)screen;
    MwWindow *win = mw_window(d, w);
    if (win) mw_unmap_window(d, win);
    return 1;
}

void mw_window_props_changed(Display *d, MwWindow *win, Atom a)
{
    MwToplevel *tl = win->tl;
    if (!tl) return;
    char *name = NULL;
    Atom wmname = mw_intern_atom(d, "WM_NAME", True);
    Atom netname = mw_intern_atom(d, "_NET_WM_NAME", True);
    Atom motif = mw_intern_atom(d, "_MOTIF_WM_HINTS", True);
    if (a == wmname || a == netname) {
        MwProp *p = (a == wmname) ? mw_get_prop(d, win, wmname)
                                  : mw_get_prop(d, win, netname);
        if (p && p->data) name = (char *)p->data;
        if (name && tl->xdg_toplevel) {
            xdg_toplevel_set_title(tl->xdg_toplevel, name);
            free(tl->title);
            tl->title = strdup(name);
            /* Re-address the window in case the title is what we match on. */
            mw_apply_motif_functions(d, win);
        }
    } else if (a == motif) {
        /* The client changed the functions it allows; update the buttons. */
        mw_apply_motif_functions(d, win);
    }
}
