/* input.c — wl_seat keyboard/pointer → X events, focus and grabs. */
#define _GNU_SOURCE
#include "internal.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/file.h>

/* ------------------------------------------------------------- utilities */

static bool is_ancestor(MwWindow *a, MwWindow *w);

static MwWindow *window_for_surface(Display *d, struct wl_surface *s)
{
    XDisplayImpl *dp = MWD(d);
    if (!dp->table) return NULL;
    for (size_t i = 0; i < dp->table_cap; i++) {
        if (dp->table[i].id && dp->table[i].kind == MW_OBJ_WINDOW) {
            MwWindow *w = dp->table[i].obj;
            if (w && w->tl && w->tl->surface == s) return w;
        }
    }
    return NULL;
}

static unsigned int current_mods(Display *d)
{
    XDisplayImpl *dp = MWD(d);
    unsigned int m = 0;
    if (!dp->xkb_state) return 0;
    if (xkb_state_mod_name_is_active(dp->xkb_state, XKB_MOD_NAME_SHIFT,
                                     XKB_STATE_MODS_EFFECTIVE)) m |= ShiftMask;
    if (xkb_state_mod_name_is_active(dp->xkb_state, XKB_MOD_NAME_CAPS,
                                     XKB_STATE_MODS_EFFECTIVE)) m |= LockMask;
    if (xkb_state_mod_name_is_active(dp->xkb_state, XKB_MOD_NAME_CTRL,
                                     XKB_STATE_MODS_EFFECTIVE)) m |= ControlMask;
    if (xkb_state_mod_name_is_active(dp->xkb_state, XKB_MOD_NAME_ALT,
                                     XKB_STATE_MODS_EFFECTIVE)) m |= Mod1Mask;
    if (xkb_state_mod_name_is_active(dp->xkb_state, "Super",
                                     XKB_STATE_MODS_EFFECTIVE)) m |= Mod4Mask;
    return m;
}

/* Deliver to target, then propagate up the tree like X. */
static void deliver(Display *d, MwWindow *target, XEvent *ev, long mask)
{
    for (MwWindow *w = target; w; w = w->parent) {
        if (w->event_mask & mask) {
            if (getenv("MW_TRACE") &&
                (ev->type == EnterNotify || ev->type == LeaveNotify))
                fprintf(stderr, "MW: deliver crossing %s to 0x%lx mode=%d detail=%d\n",
                        ev->type == EnterNotify ? "Enter" : "Leave", w->id,
                        ev->xcrossing.mode, ev->xcrossing.detail);
            ev->xany.window = w->id;
            mw_put_event(d, ev);
            return;
        }
        if (w->do_not_propagate_mask & mask) return;
    }
}

/* Offset of a window's content origin from its top-level surface origin.
 * Wayland gives pointer coordinates relative to the surface, so event
 * coordinates must be computed against this, not the fake X root position
 * (which for a menu or a geometrically placed window is non-zero). */
static void origin_rel(MwWindow *w, int *ox, int *oy)
{
    int x = 0, y = 0;
    for (MwWindow *n = w; n && !n->tl; n = n->parent) { x += n->x; y += n->y; }
    *ox = x; *oy = y;
}

static void pt_event(Display *d, MwWindow *w, XEvent *ev, int type,
                     int rx, int ry, unsigned int state, int detail,
                     int mode, int kind)
{
    int wx, wy;
    origin_rel(w, &wx, &wy);
    ev->xany.type = type;
    ev->xany.display = d;
    ev->xany.serial = MWD(d)->serial++;
    ev->xany.send_event = False;
    ev->xbutton.root = MWSCR(d)->root;
    ev->xbutton.subwindow = None;
    ev->xbutton.time = mw_now();
    ev->xbutton.x = rx - wx;
    ev->xbutton.y = ry - wy;
    /* x_root/y_root must be *screen* (root-relative) coordinates, not
     * coordinates within the toplevel.  Motif's _XmGetPointVisibility()
     * translates the widget's position to root coordinates and checks that the
     * event lands inside it, so a release whose x_root misses by the window's
     * own origin is treated as "outside the widget" and the gadget's activate
     * callback is never called: dialog buttons armed but did nothing, which is
     * exactly what NEdit's File -> Exit dialog did.  NEdit positions its
     * dialogs with XtNx/XtNy (centred on the pointer), so the missing offset
     * was hundreds of pixels. */
    {
        int ax, ay;
        mw_window_origin(w, &ax, &ay);
        ev->xbutton.x_root = ax + ev->xbutton.x;
        ev->xbutton.y_root = ay + ev->xbutton.y;
    }
    ev->xbutton.state = state;
    ev->xbutton.button = (unsigned int)detail;
    ev->xbutton.same_screen = True;

    /* Mirror the event as XInput2 for windows that selected it (the XI mask is
     * independent of the core event mask).  4/5 = ButtonPress/Release,
     * 6 = Motion, 7/8 = Enter/Leave. */
    {
        int xi = 0, cross = 0;
        switch (type) {
        case MotionNotify:  xi = 6; break;
        case ButtonPress:   xi = 4; break;
        case ButtonRelease: xi = 5; break;
        case EnterNotify:   xi = 7; cross = 1; break;
        case LeaveNotify:   xi = 8; cross = 1; break;
        default: break;
        }
        if (xi) {
            if (cross)
                mw_xi2_crossing(d, w->id, xi, mode, detail, 2, 2, 0,
                                ev->xbutton.x, ev->xbutton.y, ev->xbutton.time);
            else
                mw_xi2_event(d, w->id, xi, detail, 2, 2,
                             ev->xbutton.x, ev->xbutton.y, ev->xbutton.time);
        }
    }
    (void)mode; (void)kind;
}

static MwToplevel *win_toplevel(MwWindow *w);
static bool same_toplevel(MwWindow *a, MwWindow *b);
static bool is_transient_popup(MwWindow *w);
static bool focus_moving_into_popup(Display *d, MwWindow *w);

/* ---------------------------------------------------------- keyboard */

static void kbd_keymap(void *data, struct wl_keyboard *kbd, uint32_t format,
                       int32_t fd, uint32_t size)
{
    Display *d = data;
    mw_keymap_init_fd(d, fd, size, format);
    close(fd);
}

static void kbd_enter(void *data, struct wl_keyboard *kbd, uint32_t serial,
                      struct wl_surface *surface, struct wl_array *keys)
{
    (void)kbd; (void)serial; (void)keys;
    Display *d = data;
    XDisplayImpl *dp = MWD(d);
    MwWindow *w = window_for_surface(d, surface);
    if (getenv("MW_TRACE"))
        fprintf(stderr, "MW: kbd_enter surface->win=%lx explicit=%d\n",
                w ? w->id : 0UL, dp->focus_explicit);
    /* The compositor focuses the popup surface for the duration of its
     * xdg_popup grab (KWin does).  X keeps the input focus on the application
     * toplevel and never puts it on an override-redirect menu, so ignore the
     * enter: following it made Firefox's app menu see a focus change and hide
     * itself the instant it opened. */
    if (is_transient_popup(w) &&
        (!dp->kbd_focus || !same_toplevel(dp->kbd_focus, w))) {
        if (getenv("MW_TRACE"))
            fprintf(stderr, "MW: kbd_enter popup 0x%lx ignored (focus stays 0x%lx)\n",
                    w->id, dp->kbd_focus ? dp->kbd_focus->id : 0UL);
        return;
    }
    if (w) {
        /* Work out which window inside the surface that just took the
         * keyboard should hold the X focus.  The existing kbd_focus is only
         * kept when it is still inside that surface -- usually the same
         * window.  If it belongs to a different toplevel (the main editor
         * while a dialog has just been focused) it must NOT be reused:
         * Motif updates its internal focus from the FocusIn we deliver, so
         * sending FocusIn to the old window left typing going to the main
         * window and dialog text fields could never be focused.
         *
         * The focus is the surface's own window, never the deepest window
         * under the pointer: the pointer may be resting on a child that takes
         * no keyboard input at all (xterm's scrollbar), and X never moves the
         * input focus just because the pointer is somewhere. */
        MwWindow *deep;
        if (dp->kbd_focus && same_toplevel(dp->kbd_focus, w)) {
            deep = dp->kbd_focus;
        } else {
            dp->focus_explicit = false;   /* that explicit focus was elsewhere */
            deep = w;
        }
        if (!dp->focus_explicit)
            dp->kbd_focus = deep;
        /* XI2 focus event (9 = FocusIn), driven by the XI selection rather than
         * the core FocusChangeMask. */
        mw_xi2_crossing(d, deep->id, 9, NotifyNormal, NotifyNonlinear, 3, 3, 1,
                        dp->ptr_x, dp->ptr_y, mw_now());
        if (deep->event_mask & FocusChangeMask) {
            XFocusChangeEvent fe;
            memset(&fe, 0, sizeof fe);
            fe.type = FocusIn; fe.display = d; fe.window = deep->id;
            fe.mode = NotifyNormal; fe.detail = NotifyNonlinear;
            mw_put_event(d, (XEvent *)&fe);
        }
        /* X follows a FocusIn with a KeymapNotify for windows that selected
         * KeymapStateMask; xev prints one and clients use it to resynchronise
         * modifier state that was missed while unfocused. */
        if (deep->event_mask & KeymapStateMask) {
            XKeymapEvent ke;
            memset(&ke, 0, sizeof ke);
            ke.type = KeymapNotify;
            ke.display = d;
            ke.window = deep->id;
            mw_put_event(d, (XEvent *)&ke);
        }
    }
}

static void kbd_leave(void *data, struct wl_keyboard *kbd, uint32_t serial,
                      struct wl_surface *surface)
{
    (void)kbd; (void)serial;
    Display *d = data;
    XDisplayImpl *dp = MWD(d);
    MwWindow *w = window_for_surface(d, surface);
    if (getenv("MW_TRACE"))
        fprintf(stderr, "MW: kbd_leave surface->win=%lx kbd_focus=%lx\n",
                w ? w->id : 0UL, dp->kbd_focus ? dp->kbd_focus->id : 0UL);
    /* Only focus out the window this surface actually owns.  When focus moves
     * from one toplevel to another the compositor may deliver leave(A) after
     * the FocusIn we already emitted synchronously for B; acting on it aimed a
     * FocusOut at B and deactivated the window that had just taken focus (the
     * gtester keys-events subtest failed exactly this way when it ran after
     * another subtest). */
    if (!w || (dp->kbd_focus && !same_toplevel(dp->kbd_focus, w)))
        return;
    /* Focus is moving into an open menu popup of this toplevel, not leaving
     * the application: X keeps the toplevel focused, so report nothing. */
    if (focus_moving_into_popup(d, w)) {
        if (getenv("MW_TRACE"))
            fprintf(stderr, "MW: kbd_leave 0x%lx suppressed (menu open)\n", w->id);
        return;
    }
    if (dp->kbd_focus && (dp->kbd_focus->event_mask & FocusChangeMask)) {
        XFocusChangeEvent fe;
        memset(&fe, 0, sizeof fe);
        fe.type = FocusOut; fe.display = d; fe.window = dp->kbd_focus->id;
        fe.mode = NotifyNormal; fe.detail = NotifyNonlinear;
        mw_put_event(d, (XEvent *)&fe);
    }
    /* XI2 focus event (10 = FocusOut). */
    if (dp->kbd_focus)
        mw_xi2_crossing(d, dp->kbd_focus->id, 10, NotifyNormal, NotifyNonlinear,
                        3, 3, 0, dp->ptr_x, dp->ptr_y, mw_now());
}

/* Build one key event for `kc` and deliver it to the focused window (or to a
 * passive grab that matches).  Shared by real key events and auto-repeat. */
static void deliver_key(Display *d, KeyCode kc, int type)
{
    XDisplayImpl *dp = MWD(d);
    XKeyEvent ke;
    memset(&ke, 0, sizeof ke);
    ke.display = d;
    ke.serial = dp->serial++;
    ke.send_event = False;
    ke.root = MWSCR(d)->root;
    ke.subwindow = None;
    ke.time = mw_now();
    ke.x = dp->ptr_x; ke.y = dp->ptr_y;
    ke.x_root = dp->ptr_x; ke.y_root = dp->ptr_y;
    ke.same_screen = True;
    ke.keycode = kc;
    ke.state = current_mods(d);
    ke.type = type;

    MwWindow *focus = dp->kbd_focus ? dp->kbd_focus : dp->ptr_focus;
    MwWindow *target = dp->kbd_grab_window;   /* XGrabKeyboard in effect */

    /* Passive keyboard grabs (XGrabKey / XtGrabKey).  Motif installs menu
     * accelerators this way: XtGrabKey on the top manager, where its
     * _XmRC_KeyboardInputHandler listens.  A matching key event must therefore
     * go to the grab window rather than to the focused widget.  Without this,
     * Ctrl+Z, Ctrl+A and the rest fell through to the text widget's
     * "<KeyPress>: self_insert()" translation -- undo did nothing and
     * select-all inserted an "a".  Mnemonics are registered with
     * needGrab=False, so ordinary typing is unaffected. */
    if (!target) {
        unsigned int mods = current_mods(d) & ~LockMask;
        for (MwKeyGrab *g = dp->key_grabs; g; g = g->next) {
            if (g->keycode != AnyKey && g->keycode != kc) continue;
            if ((g->modifiers & ~LockMask) != mods) continue;
            MwWindow *gw = mw_window(d, g->window);
            if (gw && focus && is_ancestor(gw, focus)) { target = gw; break; }
        }
        if (!target) target = focus;
    }
    if (getenv("MW_TRACE"))
        fprintf(stderr, "MW: kbd_key kc=%u %s target=%lx mask=%lx\n", kc,
                type == KeyPress ? "press" : "rel",
                target ? target->id : 0UL, target ? target->event_mask : 0UL);
    if (target) {
        int wx, wy;
        origin_rel(target, &wx, &wy);
        ke.x = dp->ptr_x - wx;
        ke.y = dp->ptr_y - wy;
        ke.window = target->id;
        mw_put_event(d, (XEvent *)&ke);
        /* XI2 keyboard event (2/3 = KeyPress/Release); device 3 is the master
         * keyboard. */
        mw_xi2_event(d, target->id, type == KeyPress ? 2 : 3, kc, 3, 3,
                     ke.x, ke.y, ke.time);
    }
}

/* Public wrapper: synthesise a key press/release to the focused window.  The
 * XIM bridge uses it to realise text-input delete_surrounding_text, which XIM
 * itself has no message for. */
void mw_key_send(Display *d, KeyCode kc, Bool press)
{
    if (!kc) return;
    deliver_key(d, kc, press ? KeyPress : KeyRelease);
}

/* Mirror the repeat deadline to the wakeup helper so it can wake the client's
 * select() exactly when a repeat is due, rather than waiting for unrelated
 * traffic on the Wayland connection (see src/wayland/wakeup.c). */
static void repeat_notify(Display *d)
{
    XDisplayImpl *dp = MWD(d);
    mw_wakeup_set(dp, dp->repeat_key && dp->repeat_rate > 0, dp->repeat_next_ms);
}

/* ---- shared auto-repeat state ---------------------------------------- */

/* CDE's Style Manager (dtstyle, Keyboard module) turns auto-repeat on and off
 * with XAutoRepeatOn/Off and XChangeKeyboardControl(KBAutoRepeatMode).  In CDE
 * that is a property of the whole X server, so every client has to see it, but
 * the shim gives each client its own server.  Share the setting through a file
 * in the runtime directory, like the atom table, and read it live so a change
 * made in the Style Manager reaches already-running clients.  A missing file
 * (or no runtime directory) means the default, repeat on. */
static const char *kbd_state_path(void)
{
    static char path[1024];
    const char *p = getenv("XLIB_WAYLAND_KEYBOARD");
    if (p && *p) return p;
    const char *rt = getenv("XDG_RUNTIME_DIR");
    if (!rt || !*rt) return NULL;
    snprintf(path, sizeof path, "%s/xlib-wayland-keyboard", rt);
    return path;
}

int mw_keyboard_autorepeat_mode(Display *d)
{
    XDisplayImpl *dp = MWD(d);
    const char *path = kbd_state_path();
    int mode = AutoRepeatModeOn;   /* default when there is no shared state */
    if (path) {
        FILE *f = fopen(path, "r");
        if (f) {
            char buf[16];
            if (fgets(buf, sizeof buf, f))
                mode = strncmp(buf, "off", 3) == 0
                    ? AutoRepeatModeOff : AutoRepeatModeOn;
            fclose(f);
        }
    }
    dp->auto_repeat = mode;
    return mode;
}

void mw_keyboard_set_autorepeat(Display *d, int mode)
{
    XDisplayImpl *dp = MWD(d);
    if (mode == AutoRepeatModeDefault)
        mode = AutoRepeatModeOn;   /* there is no separate "server default" */
    dp->auto_repeat = mode;

    const char *path = kbd_state_path();
    if (!path) return;
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) return;
    flock(fd, LOCK_EX);
    const char *s = (mode == AutoRepeatModeOff) ? "off\n" : "on\n";
    if (write(fd, s, strlen(s)) < 0) { /* best effort */ }
    flock(fd, LOCK_UN);
    close(fd);
}

bool mw_autorepeat_enabled(Display *d)
{
    return mw_keyboard_autorepeat_mode(d) != AutoRepeatModeOff;
}

/* Wayland leaves key repeat to the client: wl_keyboard.repeat_info supplies the
 * rate and delay, and the repeat events while a key is held have to be generated
 * here, from the event pump. */
void mw_kbd_repeat_pump(Display *d)
{
    XDisplayImpl *dp = MWD(d);
    if (!dp->repeat_key || dp->repeat_rate <= 0) return;
    /* The Style Manager's auto-repeat toggle covers every client. */
    if (!mw_autorepeat_enabled(d)) {
        dp->repeat_key = 0;
        repeat_notify(d);
        return;
    }
    int period = 1000 / dp->repeat_rate;
    if (period < 1) period = 1;
    uint64_t now = mw_now();
    if (now < dp->repeat_next_ms) return;
    /* Deliver every repeat that has come due since the last visit.  libXt blocks
     * on the connection fd itself, so the pump only runs when something else
     * arrives on the Wayland connection and wakeups are irregular; the deadline
     * is absolute, so catching up here keeps the *average* rate right instead of
     * slipping a little on every wakeup.  Cap the burst so a long stall
     * (debugger, suspend) does not fling a page of characters.  The
     * release+press pair matches an X server with detectable autorepeat off,
     * which is what Motif's text widgets and xterm handle. */
    int burst = 0;
    while (now >= dp->repeat_next_ms && burst < 8) {
        deliver_key(d, dp->repeat_key, KeyRelease);
        deliver_key(d, dp->repeat_key, KeyPress);
        dp->repeat_next_ms += (uint64_t)period;
        burst++;
    }
    if (now >= dp->repeat_next_ms)          /* long stall: resynchronise */
        dp->repeat_next_ms = now + (uint64_t)period;
    repeat_notify(d);
}

/* Milliseconds until the next repeat is due, or -1 when none is pending.  The
 * event wait uses this as its timeout so a held key repeats while the client
 * sits idle in XNextEvent. */
int mw_kbd_repeat_timeout(Display *d)
{
    XDisplayImpl *dp = MWD(d);
    if (!dp->repeat_key || dp->repeat_rate <= 0) return -1;
    if (!mw_autorepeat_enabled(d)) return -1;
    uint64_t now = mw_now();
    if (dp->repeat_next_ms <= now) return 0;
    uint64_t t = dp->repeat_next_ms - now;
    return t > 1000 ? 1000 : (int)t;
}

static void kbd_key(void *data, struct wl_keyboard *kbd, uint32_t serial,
                    uint32_t time, uint32_t key, uint32_t state)
{
    (void)kbd; (void)serial; (void)time;
    Display *d = data;
    XDisplayImpl *dp = MWD(d);
    KeyCode kc = (KeyCode)(key + 8);
    /* Protocol v10 adds a "repeated" state so a compositor may drive repeat
     * itself; that is still a press as far as X clients are concerned. */
    bool pressed = state != WL_KEYBOARD_KEY_STATE_RELEASED;
    bool ar = mw_autorepeat_enabled(d);

    /* Drop repeats the compositor generated while the session has auto-repeat
     * off (the Style Manager's Keyboard toggle). */
    if (state == WL_KEYBOARD_KEY_STATE_REPEATED && !ar) {
        dp->repeat_key = 0;
        repeat_notify(d);
        return;
    }

    deliver_key(d, kc, pressed ? KeyPress : KeyRelease);

    if (dp->xkb_state)
        xkb_state_update_key(dp->xkb_state, (xkb_keycode_t)kc,
                             pressed ? XKB_KEY_DOWN : XKB_KEY_UP);

    /* Track the held key so the pump can repeat it.  Modifiers never repeat,
     * if the session disabled auto-repeat we must not start one, and if the
     * compositor already sent a repeat we must not add our own. */
    if (state == WL_KEYBOARD_KEY_STATE_PRESSED) {
        if (ar && !mw_keysym_to_modmask(d, mw_keycode_to_keysym(d, kc, 0))) {
            dp->repeat_key = kc;
            dp->repeat_next_ms = mw_now() + (uint64_t)dp->repeat_delay;
        } else {
            dp->repeat_key = 0;
        }
    } else if (state == WL_KEYBOARD_KEY_STATE_REPEATED) {
        dp->repeat_key = 0;   /* compositor is repeating this key for us */
    } else if (dp->repeat_key == kc) {
        dp->repeat_key = 0;
    }
    repeat_notify(d);
}

static void kbd_modifiers(void *data, struct wl_keyboard *kbd, uint32_t serial,
                          uint32_t dep, uint32_t lat, uint32_t lock, uint32_t group)
{
    (void)kbd; (void)serial;
    Display *d = data;
    XDisplayImpl *dp = MWD(d);
    if (dp->xkb_state)
        xkb_state_update_mask(dp->xkb_state, dep, lat, lock, 0, 0, group);
}

static void kbd_repeat(void *data, struct wl_keyboard *kbd, int32_t rate, int32_t delay)
{
    (void)kbd;
    XDisplayImpl *dp = MWD((Display *)data);
    int r  = rate  > 0 ? rate  : 0;
    int dl = delay > 0 ? delay : 0;
    /* Compositors tend to pick a longish initial delay (400-660ms), tuned for
     * their own repeat implementation.  The repeat is synthesised here, so cap
     * the delay to keep the first repeat prompt while still honouring shorter
     * compositor values.  MW_REPEAT_DELAY / MW_REPEAT_RATE override both. */
    if (dl > 200) dl = 200;
    if (const char *e = getenv("MW_REPEAT_DELAY")) { int v = atoi(e); if (v >= 0) dl = v; }
    if (const char *e = getenv("MW_REPEAT_RATE"))  { int v = atoi(e); if (v >  0) r  = v; }
    dp->repeat_rate  = r;
    dp->repeat_delay = dl;
    if (getenv("MW_TRACE"))
        fprintf(stderr, "MW: repeat_info rate=%d delay=%d -> rate=%d delay=%d\n",
                rate, delay, dp->repeat_rate, dp->repeat_delay);
}

static const struct wl_keyboard_listener keyboard_listener = {
    .keymap = kbd_keymap,
    .enter = kbd_enter,
    .leave = kbd_leave,
    .key = kbd_key,
    .modifiers = kbd_modifiers,
    .repeat_info = kbd_repeat,
};

/* ----------------------------------------------------------- pointer */

/* Pointer position expressed in the coordinate space of the toplevel that
 * owns `w`.  A crossing event must carry coordinates relative to the window
 * it is delivered to, but the pointer's stored position (ptr_surf_x/y) is
 * relative to the current toplevel -- which is a *different* one whenever a
 * grab leaves a menu for the window that grabbed, or a menu leaves for its
 * submenu (a separate popup).  Reusing the current toplevel's coordinates put
 * the leave at bogus positions (e.g. x=216 for a pointer actually at the
 * menu's right edge, x=413); GTK feeds those to its submenu-navigation and
 * scrolling logic, which then mis-handles the menu.  Convert via the screen
 * position so pt_event() recovers the correct window- and root-relative x/y. */
static void ptr_in_win_space(Display *d, MwWindow *w, int *x, int *y)
{
    XDisplayImpl *dp = MWD(d);
    int sx = dp->ptr_surf_x, sy = dp->ptr_surf_y;
    if (dp->ptr_toplevel) {
        int ox, oy;
        int off = dp->ptr_toplevel->tl
                      ? mw_toplevel_content_offset(dp->ptr_toplevel->tl) : 0;
        mw_window_origin(dp->ptr_toplevel, &ox, &oy);
        sx = ox + dp->ptr_surf_x;
        sy = oy + dp->ptr_surf_y - off;
    }
    int wx, wy, rx, ry;
    mw_window_origin(w, &wx, &wy);
    origin_rel(w, &rx, &ry);
    *x = sx - wx + rx;
    *y = sy - wy + ry;
}

/* Nesting depth of a window's popup chain (1 for an ordinary toplevel).  Used
 * to pick the deepest popup in a compositor enter burst. */
static int win_depth(MwWindow *w)
{
    int n = 0;
    MwToplevel *tl = (w && w->tl) ? w->tl : NULL;
    while (tl) {
        n++;
        tl = (tl->parent_tl && tl->parent_tl->tl) ? tl->parent_tl->tl : NULL;
    }
    return n;
}

/* Move the pointer focus to `deep`, emitting the X crossing events. */
static void ptr_set_focus(Display *d, MwWindow *deep, int rx, int ry)
{
    XDisplayImpl *dp = MWD(d);
    if (deep == dp->ptr_focus) return;
    if (dp->ptr_focus) {
        XLeaveWindowEvent le;
        int lx, ly;
        ptr_in_win_space(d, dp->ptr_focus, &lx, &ly);
        pt_event(d, dp->ptr_focus, (XEvent *)&le, LeaveNotify, lx, ly,
                 current_mods(d), NotifyNonlinear, NotifyNormal, 0);
        le.mode = NotifyNormal; le.detail = NotifyAncestor;
        deliver(d, dp->ptr_focus, (XEvent *)&le, LeaveWindowMask);
    }
    dp->ptr_focus = deep;
    if (deep) {
        XEnterWindowEvent en;
        pt_event(d, deep, (XEvent *)&en, EnterNotify, rx, ry,
                 current_mods(d), NotifyNonlinear, NotifyNormal, 0);
        en.mode = NotifyNormal; en.detail = NotifyAncestor;
        deliver(d, deep, (XEvent *)&en, EnterWindowMask);
    }
}

/* Once a menu is open the compositor reports the whole popup chain
 * (application toplevel -> parent menu -> submenu) as a leave/enter burst on
 * every motion, and the ancestor enters carry stale coordinates -- the parent
 * menu's land on its first item.  Delivering those made GTK re-select that
 * item, which deselected the item whose submenu was open and popped the
 * submenu down (the "parent highlight jumps while I pick a submenu item"
 * symptom).  Buffer the burst and, when it settles, focus only the deepest
 * popup that was entered; the last event of a burst is not reliable, the
 * deepest popup is. */
void mw_input_settle(Display *d)
{
    XDisplayImpl *dp = MWD(d);
    if (!dp->ptr_defer_active) return;
    MwWindow *top = dp->ptr_defer_top;
    MwWindow *deep = dp->ptr_defer_deep;
    struct wl_surface *surf = dp->ptr_defer_surface;
    int sx = dp->ptr_defer_sx, sy = dp->ptr_defer_sy;
    dp->ptr_defer_active = false;
    dp->ptr_defer_top = NULL;
    dp->ptr_defer_deep = NULL;

    if (top) {
        /* Adopt the deepest popup so the crossing is reported relative to the
         * right toplevel, then move the focus there. */
        dp->ptr_toplevel = top;
        dp->ptr_wl_surface = surf;
        dp->ptr_surf_x = sx;
        dp->ptr_surf_y = sy;
        int off = top->tl ? mw_toplevel_content_offset(top->tl) : 0;
        ptr_set_focus(d, deep, sx, sy - off);
        mw_pointer_update_cursor(d, deep);
    } else {
        /* The burst left everything: no popup remained under the pointer. */
        ptr_set_focus(d, NULL, 0, 0);
        dp->ptr_toplevel = NULL;
        dp->ptr_wl_surface = NULL;
        mw_pointer_update_cursor(d, NULL);
    }
}

static void ptr_enter(void *data, struct wl_pointer *p, uint32_t serial,
                      struct wl_surface *surface, wl_fixed_t sx, wl_fixed_t sy)
{
    (void)p;
    Display *d = data;
    XDisplayImpl *dp = MWD(d);
    dp->ptr_enter_serial = serial;
    MwWindow *top = window_for_surface(d, surface);
    if (getenv("MW_TRACE"))
        fprintf(stderr, "MW: ptr_enter surface=%p -> win=0x%lx%s\n", (void*)surface,
                top ? top->id : 0UL,
                (top && top->tl && top->tl->is_popup) ? " POPUP" : "");
    int off = (top && top->tl) ? mw_toplevel_content_offset(top->tl) : 0;
    int rx = wl_fixed_to_int(sx), ry = wl_fixed_to_int(sy) - off;
    MwWindow *deep = (top && ry >= 0) ? mw_deepest_at(top, rx, ry, NULL, NULL) : NULL;

    if (dp->open_menu) {
        /* Buffer the burst; only the deepest popup is the real focus. */
        if (deep && (!dp->ptr_defer_active ||
                     win_depth(top) >= win_depth(dp->ptr_defer_top))) {
            dp->ptr_defer_surface = surface;
            dp->ptr_defer_top = top;
            dp->ptr_defer_deep = deep;
            dp->ptr_defer_sx = wl_fixed_to_int(sx);
            dp->ptr_defer_sy = wl_fixed_to_int(sy);
        }
        dp->ptr_defer_active = true;
        return;
    }

    dp->ptr_toplevel = top;
    dp->ptr_wl_surface = surface;
    dp->ptr_surf_x = wl_fixed_to_int(sx);
    dp->ptr_surf_y = wl_fixed_to_int(sy);
    ptr_set_focus(d, deep, rx, ry);
    mw_pointer_update_cursor(d, deep);
}

static void ptr_leave(void *data, struct wl_pointer *p, uint32_t serial,
                      struct wl_surface *surface)
{
    (void)p; (void)serial;
    Display *d = data;
    XDisplayImpl *dp = MWD(d);
    if (dp->open_menu) {
        /* Part of the popup-chain churn the compositor sends while a menu is
         * open; folded into the focus at mw_input_settle. */
        dp->ptr_defer_active = true;
        return;
    }
    /* Only act on a leave for the surface we currently track.  When the
     * pointer moves between two popups (parent menu -> submenu) the compositor
     * can deliver leave(old) after enter(new); acting on it cleared
     * ptr_toplevel, so subsequent motion had no window under the pointer and
     * fell back to the grab window -- the parent menu -- whose highlighted
     * item then followed the pointer as if it were still over the parent.
     * Same shape as the stale key leave fixed in a800706. */
    if (dp->ptr_wl_surface && surface != dp->ptr_wl_surface)
        return;
    if (getenv("MW_TRACE")) fprintf(stderr, "MW: ptr_leave (wl)\n");
    /* A Motif drag that leaves its window becomes a Wayland drag, so the
     * compositor routes it to whatever is under the pointer (see xlib/dnd.c).
     * A drag that stays inside this application is left native. */
    mw_dnd_maybe_start(d);
    if (dp->ptr_focus) {
        XLeaveWindowEvent le;
        pt_event(d, dp->ptr_focus, (XEvent *)&le, LeaveNotify,
                 dp->ptr_x, dp->ptr_y, current_mods(d), NotifyNonlinear,
                 NotifyNormal, 0);
        deliver(d, dp->ptr_focus, (XEvent *)&le, LeaveWindowMask);
        dp->ptr_focus = NULL;
    }
    dp->ptr_toplevel = NULL;
    dp->ptr_wl_surface = NULL;
}

/* ---- faithful pointer-grab layer ---- */

static bool is_ancestor(MwWindow *a, MwWindow *w)
{
    for (; w; w = w->parent) if (w == a) return true;
    return false;
}

/* The toplevel (Wayland surface) a window belongs to, if any.  Used to tell
 * "focus moved inside this window" from "focus moved to another window". */
static MwToplevel *win_toplevel(MwWindow *w)
{
    for (; w; w = w->parent) if (w->tl) return w->tl;
    return NULL;
}

static bool same_toplevel(MwWindow *a, MwWindow *b)
{
    return win_toplevel(a) == win_toplevel(b);
}

/* An override-redirect menu/popup window.  X does not move the input focus to
 * one: such a window is the compositor's xdg_popup, whose grab makes the
 * compositor focus the popup surface -- but that focus is not the X input
 * focus, which stays on the application's own toplevel. */
static bool is_transient_popup(MwWindow *w)
{
    return w && (w->override_redirect || (w->tl && w->tl->is_popup));
}

/* True when the pointer/keyboard focus is moving into the open menu popup that
 * `w`'s toplevel owns (rather than leaving the application). */
static bool focus_moving_into_popup(Display *d, MwWindow *w)
{
    XDisplayImpl *dp = MWD(d);
    return w && dp->open_menu && dp->open_menu->popup_parent &&
           same_toplevel(dp->open_menu->popup_parent, w);
}

/* Deepest window under the pointer, plus position relative to the toplevel. */
static MwWindow *ptr_deepest(Display *d, int *rx, int *ry)
{
    XDisplayImpl *dp = MWD(d);
    *rx = dp->ptr_surf_x;
    *ry = dp->ptr_surf_y;
    if (!dp->ptr_toplevel) return NULL;
    int off = dp->ptr_toplevel->tl ? mw_toplevel_content_offset(dp->ptr_toplevel->tl) : 0;
    *ry = dp->ptr_surf_y - off;
    if (*ry < 0) return NULL;
    return mw_deepest_at(dp->ptr_toplevel, *rx, *ry, NULL, NULL);
}

static void crossing(Display *d, MwWindow *w, int type, int mode, int detail,
                     int rx, int ry)
{
    if (!w) return;
    if (getenv("MW_TRACE"))
        fprintf(stderr, "MW: crossing %s -> 0x%lx at %d,%d mode=%d detail=%d\n",
                type == EnterNotify ? "Enter" : "Leave", w->id, rx, ry, mode, detail);
    XEvent ev;
    pt_event(d, w, &ev, type, rx, ry, current_mods(d), detail, mode, 0);
    ev.xcrossing.mode = mode;
    ev.xcrossing.detail = detail;
    deliver(d, w, &ev, type == EnterNotify ? EnterWindowMask : LeaveWindowMask);
}

static void ptr_motion(void *data, struct wl_pointer *p, uint32_t time,
                       wl_fixed_t sx, wl_fixed_t sy)
{
    (void)p; (void)time;
    Display *d = data;
    XDisplayImpl *dp = MWD(d);
    dp->ptr_surf_x = wl_fixed_to_int(sx);
    dp->ptr_surf_y = wl_fixed_to_int(sy);
    int rx, ry;
    MwWindow *deep = ptr_deepest(d, &rx, &ry);
    dp->ptr_x = rx; dp->ptr_y = ry;

    bool grabbed = dp->ptr_grab_active || dp->implicit_grab;

    /* Enter/Leave follow the real pointer position and are reported even
     * while a grab is active -- that is how X behaves, and Motif relies on it:
     * an XmMenuButton arms on EnterNotify and disarms on LeaveNotify, so
     * suppressing crossing events during the menu's XGrabPointer would leave
     * every item unarmed.  (The grab only decides which window receives the
     * button and motion events, handled below.)  Note ptr_focus was updated
     * unconditionally further down regardless of grab state, so deriving the
     * crossing events the same way keeps the two consistent. */
    bool hover_changed = (deep != dp->ptr_focus);
    if (deep && deep != dp->ptr_focus) {
        if (dp->ptr_focus) {
            int lx, ly;
            ptr_in_win_space(d, dp->ptr_focus, &lx, &ly);
            crossing(d, dp->ptr_focus, LeaveNotify, NotifyNormal, NotifyNonlinear, lx, ly);
        }
        dp->ptr_focus = deep;
        crossing(d, deep, EnterNotify, NotifyNormal, NotifyNonlinear, rx, ry);
    }
    if (deep) dp->ptr_focus = deep;
    /* Cursor inheritance follows the window under the pointer. */
    if (hover_changed) mw_pointer_update_cursor(d, deep);

    /* A grab whose window has gone away is no grab at all; without this the
     * events would be dropped and the client would look frozen. */
    if (dp->ptr_grab_active && !dp->ptr_grab_window) {
        dp->ptr_grab_active = false;
        dp->ptr_grab_temporary = false;
        dp->ptr_grab_mask = 0;
    }

    MwWindow *target;
    if (dp->ptr_grab_active)
        target = (dp->ptr_grab_owner && deep) ? deep : dp->ptr_grab_window;
    else if (dp->implicit_grab)
        target = dp->implicit_grab;
    else
        target = deep;
    if (!target) return;

    XEvent ev;
    pt_event(d, target, &ev, MotionNotify, rx, ry, current_mods(d) | dp->ptr_state,
             0, NotifyNormal, 0);
    bool want = grabbed ? (dp->ptr_grab_active ? (dp->ptr_grab_mask & PointerMotionMask)
                                               : true)
                        : (target->event_mask & PointerMotionMask);
    if (want) {
        ev.xmotion.window = target->id;
        mw_put_event(d, &ev);
    }
}

static void ptr_button(void *data, struct wl_pointer *p, uint32_t serial,
                       uint32_t time, uint32_t button, uint32_t state)
{
    (void)p; (void)time;
    Display *d = data;
    XDisplayImpl *dp = MWD(d);
    unsigned int xbutton = button;
    if (xbutton >= 0x110) xbutton = xbutton - 0x110 + 1;
    bool pressed = state == WL_POINTER_BUTTON_STATE_PRESSED;

    dp->last_input_serial = serial;
    if (pressed) dp->last_press_serial = serial;
    /* The state reported with a button event is the pointer state *just
     * before* it, so a ButtonRelease carries the mask of the button being
     * released.  Reporting only the modifier keys meant a released button
     * looked like no button at all, and Motif's push buttons (the buttons in
     * every NEdit dialog: Save / Don't Save / Cancel, OK, ...) armed on press
     * and then never fired on release. */
    /* ButtonNMask lives in the top byte of the state (Button1Mask == 1 << 8),
     * not at 1 << (button - 1).  Using the low bits meant a drag reported
     * state=1 (ShiftMask) instead of Button1Mask, so every translation that
     * depends on a held button -- NEdit's "Button1<MotionNotify>:
     * extend_adjust()" among them -- silently never matched and mouse
     * text selection did nothing. */
    unsigned int btn_mask = xbutton >= 1 ? (1u << (8 + xbutton - 1)) : 0;
    unsigned int ev_state = current_mods(d) | dp->ptr_state;
    if (pressed) dp->ptr_state |= btn_mask;
    else dp->ptr_state &= ~btn_mask;

    if (getenv("MW_TRACE"))
        fprintf(stderr, "MW: ptr_button serial=%u button=%u %s surf=(%d,%d) top=%lx\n",
                serial, button, pressed ? "press" : "release", dp->ptr_surf_x,
                dp->ptr_surf_y, dp->ptr_toplevel ? dp->ptr_toplevel->id : 0UL);

    if (!dp->ptr_grab_active && dp->ptr_toplevel && dp->ptr_toplevel->tl &&
        mw_toplevel_content_offset(dp->ptr_toplevel->tl) > 0 &&
        dp->ptr_surf_y < dp->ptr_toplevel->tl->tb_h) {
        MwToplevel *tl = dp->ptr_toplevel->tl;
        if (pressed) {
            if (dp->ptr_surf_x >= tl->width - 22) mw_toplevel_close(tl);
            else if (tl->xdg_toplevel && dp->wl_seat)
                xdg_toplevel_move(tl->xdg_toplevel, dp->wl_seat, serial);
        }
        return;
    }

    int rx, ry;
    MwWindow *deep = ptr_deepest(d, &rx, &ry);
    dp->ptr_x = rx; dp->ptr_y = ry;

    /* As in ptr_motion: a grab on a destroyed window must not keep eating
     * events, or the application becomes unresponsive to the pointer. */
    if (dp->ptr_grab_active && !dp->ptr_grab_window) {
        dp->ptr_grab_active = false;
        dp->ptr_grab_temporary = false;
        dp->ptr_grab_mask = 0;
    }

    long bit = pressed ? ButtonPressMask : ButtonReleaseMask;
    MwWindow *target = NULL;
    bool want = false;

    if (dp->ptr_grab_active) {
        /* An owner_events grab reports the event to the window under the
         * pointer when that window selected it, and otherwise to the grab
         * window through the grab's mask.  Xt registers its popup-menu action
         * that way, and it is the path by which xterm's scrollbar child
         * receives the press that drives it; checking only the grab's mask
         * dropped the event before Xt could see it. */
        if (dp->ptr_grab_owner && deep && (deep->event_mask & bit)) {
            target = deep;
            want = true;
        } else {
            target = dp->ptr_grab_window;
            want = (dp->ptr_grab_mask & bit) != 0;
        }
    } else if (dp->implicit_grab) {
        target = dp->implicit_grab;
        want = true;
    } else {
        target = deep;
        if (pressed && deep) {
            for (MwBtnGrab *g = dp->btn_grabs; g; g = g->next) {
                if (g->button != xbutton && g->button != AnyButton) continue;
                MwWindow *gw = mw_window(d, g->window);
                if (gw && (deep == gw || is_ancestor(gw, deep))) {
                    dp->ptr_grab_window = gw;
                    dp->ptr_grab_owner = g->owner_events;
                    dp->ptr_grab_mask = g->event_mask;
                    dp->ptr_grab_mode = g->pointer_mode;
                    dp->ptr_grab_active = true;
                    dp->ptr_grab_temporary = true;
                    if (g->owner_events && (deep->event_mask & bit)) {
                        target = deep;
                        want = true;
                    } else {
                        target = gw;
                        want = (g->event_mask & bit) != 0;
                    }
                    if (getenv("MW_TRACE"))
                        fprintf(stderr, "MW: passive grab 0x%lx activated\n", gw->id);
                    break;
                }
            }
        }
        if (!dp->ptr_grab_active)
            want = target && (target->event_mask & bit);
    }
    if (!target) return;
    if (getenv("MW_TRACE"))
        fprintf(stderr, "MW: button %s -> target=0x%lx mask=0x%lx want=%d at %d,%d (deep ptr=%d,%d)\n",
                pressed ? "press" : "release", target->id, target->event_mask, want,
                rx, ry, dp->ptr_x, dp->ptr_y);

    XEvent ev;
    pt_event(d, target, &ev, pressed ? ButtonPress : ButtonRelease, rx, ry,
             ev_state, (int)xbutton, NotifyNormal, 0);
    if (want) {
        ev.xbutton.window = target->id;
        mw_put_event(d, &ev);
    }

    if (pressed && !dp->ptr_grab_active && !dp->implicit_grab)
        dp->implicit_grab = target;
    if (!pressed && dp->ptr_state == 0) {
        if (dp->ptr_grab_temporary) { dp->ptr_grab_active = false; dp->ptr_grab_temporary = false; }
        dp->implicit_grab = NULL;
    }
}

/* Deliver a synthetic ButtonRelease to the window Motif's drag grabbed.  When
 * the shim hands a drag to the compositor (mw_dnd_maybe_start) the pointer is
 * routed by Wayland, so Motif never sees the real release and its drag would
 * stay active with the pointer grabbed.  This gives it the release that ends
 * the drag. */
void mw_pointer_synthetic_release(Display *d, unsigned int xbutton)
{
    XDisplayImpl *dp = MWD(d);
    unsigned int btn_mask = xbutton >= 1 ? (1u << (8 + xbutton - 1)) : 0;
    if (dp->ptr_state & btn_mask) dp->ptr_state &= ~btn_mask;

    MwWindow *target = dp->ptr_grab_window;
    if (!target) target = dp->implicit_grab;
    if (!target) target = dp->ptr_focus;
    if (!target) return;

    bool want = true;
    if (dp->ptr_grab_active && target == dp->ptr_grab_window)
        want = (dp->ptr_grab_mask & ButtonReleaseMask) != 0;

    XEvent ev;
    pt_event(d, target, &ev, ButtonRelease, dp->ptr_x, dp->ptr_y,
             current_mods(d) | dp->ptr_state, (int)xbutton, NotifyNormal, 0);
    if (want) { ev.xbutton.window = target->id; mw_put_event(d, &ev); }
    if (getenv("MW_TRACE"))
        fprintf(stderr, "MW: synthetic ButtonRelease -> 0x%lx\n", target->id);

    if (dp->ptr_grab_temporary) {
        dp->ptr_grab_active = false;
        dp->ptr_grab_temporary = false;
    }
    dp->implicit_grab = NULL;
}

/* One wheel click as the X button clients expect: 4/5 vertical, 6/7 horizontal.
 * Wayland's positive direction is down/right, which X reports as 5 and 7. */
static void wheel_click(Display *d, int axis, int dir){
    MwWindow *target = MWD(d)->ptr_focus;
    if (!target) return;
    unsigned int b;
    if (axis == WL_POINTER_AXIS_VERTICAL_SCROLL) b = dir > 0 ? 5 : 4;
    else                                         b = dir > 0 ? 7 : 6;
    XButtonEvent be;
    pt_event(d, target, (XEvent *)&be, ButtonPress, MWD(d)->ptr_x, MWD(d)->ptr_y,
             current_mods(d), (int)b, NotifyNormal, 0);
    deliver(d, target, (XEvent *)&be, ButtonPressMask);
    pt_event(d, target, (XEvent *)&be, ButtonRelease, MWD(d)->ptr_x,
             MWD(d)->ptr_y, current_mods(d), (int)b, NotifyNormal, 0);
    deliver(d, target, (XEvent *)&be, ButtonReleaseMask);
}

/* Wayland delivers scroll on the axis, in surface-local coordinates; X clients
 * instead expect wheel-button clicks.  A wheel also carries a discrete step
 * count (axis_discrete, or axis_value120 once the seat is v8), which is the
 * reliable number of detents; the continuous value is the fallback for
 * touchpads and for compositors that send only that.  Events are collected for
 * the frame and turned into clicks in ptr_frame. */
static void ptr_axis(void *data, struct wl_pointer *p, uint32_t time,
                     uint32_t axis, wl_fixed_t value)
{
    (void)p; (void)time;
    if (axis > 1) return;
    MWD((Display *)data)->scroll_cont[axis] += wl_fixed_to_double(value) / 10.0;
}

static void ptr_axis_source(void *d, struct wl_pointer *p, uint32_t s) { (void)d;(void)p;(void)s; }
static void ptr_axis_stop(void *d, struct wl_pointer *p, uint32_t t, uint32_t a) { (void)d;(void)p;(void)t;(void)a; }

static void ptr_axis_discrete(void *data, struct wl_pointer *p, uint32_t axis, int32_t v)
{
    (void)p;
    if (axis > 1) return;
    Display *d = data;
    MWD(d)->scroll_step[axis] += v;
    MWD(d)->scroll_have_step[axis] = true;
}

static void ptr_axis_value120(void *data, struct wl_pointer *p, uint32_t axis, int32_t v)
{
    (void)p;
    if (axis > 1) return;
    Display *d = data;
    MWD(d)->scroll_step[axis] += (double)v / 120.0;
    MWD(d)->scroll_have_step[axis] = true;
}

static void ptr_frame(void *data, struct wl_pointer *p)
{
    (void)p;
    Display *d = data;
    XDisplayImpl *dp = MWD(d);
    for (int a = 0; a < 2; a++) {
        double delta;
        if (!dp->scroll_cont[a] && !dp->scroll_have_step[a]) continue;
        delta = dp->scroll_have_step[a] ? dp->scroll_step[a] : dp->scroll_cont[a];
        dp->scroll_cont[a] = 0;
        dp->scroll_step[a] = 0;
        dp->scroll_have_step[a] = false;
        /* Keep the fraction so half-detent events still add up, and emit one
         * click per whole detent. */
        dp->scroll_acc[a] += delta;
        while (dp->scroll_acc[a] >= 1.0)  { wheel_click(d, a, +1); dp->scroll_acc[a] -= 1.0; }
        while (dp->scroll_acc[a] <= -1.0) { wheel_click(d, a, -1); dp->scroll_acc[a] += 1.0; }
    }
}

static const struct wl_pointer_listener pointer_listener = {
    .enter = ptr_enter,
    .leave = ptr_leave,
    .motion = ptr_motion,
    .button = ptr_button,
    .axis = ptr_axis,
    .frame = ptr_frame,
    .axis_source = ptr_axis_source,
    .axis_stop = ptr_axis_stop,
    .axis_discrete = ptr_axis_discrete,
    .axis_value120 = ptr_axis_value120,
};

/* ------------------------------------------------------------- touch */

/* Wayland touch -> XI2 touch events (XI_TouchBegin/Update/End).  The seat's
 * touch device is exposed through XIQueryDevice; here we only deliver events,
 * to the window that selected them.  Deferring to Wayland: the touch points,
 * timing and target surface all come straight from wl_touch. */
#define MW_XI_TOUCHBEGIN  18
#define MW_XI_TOUCHUPDATE 19
#define MW_XI_TOUCHEND    20

static void touch_send(Display *d, struct wl_surface *surface, int evtype,
                       int32_t id, wl_fixed_t sx, wl_fixed_t sy, uint32_t time)
{
    if (!surface) return;
    MwWindow *top = window_for_surface(d, surface);
    if (!top) return;
    int dev = mw_xi2_touch_device(d);
    if (!dev) dev = 6;
    int off = top->tl ? mw_toplevel_content_offset(top->tl) : 0;
    mw_xi2_touch(d, top->id, evtype, id, dev, dev,
                 wl_fixed_to_double(sx), wl_fixed_to_double(sy) - off, time);
}

static void touch_down(void *data, struct wl_touch *t, uint32_t serial,
                       uint32_t time, struct wl_surface *surface, int32_t id,
                       wl_fixed_t x, wl_fixed_t y)
{
    (void)t; (void)serial;
    Display *d = data;
    MWD(d)->touch_surface = surface;
    MWD(d)->touch_lx = x;
    MWD(d)->touch_ly = y;
    touch_send(d, surface, MW_XI_TOUCHBEGIN, id, x, y, time);
}

static void touch_up(void *data, struct wl_touch *t, uint32_t serial,
                     uint32_t time, int32_t id)
{
    (void)t; (void)serial;
    Display *d = data;
    touch_send(d, MWD(d)->touch_surface, MW_XI_TOUCHEND, id,
               MWD(d)->touch_lx, MWD(d)->touch_ly, time);
}

static void touch_motion(void *data, struct wl_touch *t, uint32_t time,
                         int32_t id, wl_fixed_t x, wl_fixed_t y)
{
    (void)t;
    Display *d = data;
    MWD(d)->touch_lx = x;
    MWD(d)->touch_ly = y;
    touch_send(d, MWD(d)->touch_surface, MW_XI_TOUCHUPDATE, id, x, y, time);
}

static void touch_frame(void *data, struct wl_touch *t) { (void)data; (void)t; }
static void touch_cancel(void *data, struct wl_touch *t) { (void)data; (void)t; }
static void touch_shape(void *data, struct wl_touch *t, int32_t id,
                        wl_fixed_t x, wl_fixed_t y)
{ (void)data; (void)t; (void)id; (void)x; (void)y; }
static void touch_orientation(void *data, struct wl_touch *t, int32_t id,
                              wl_fixed_t o)
{ (void)data; (void)t; (void)id; (void)o; }

static const struct wl_touch_listener touch_listener = {
    .down = touch_down,
    .up = touch_up,
    .motion = touch_motion,
    .frame = touch_frame,
    .cancel = touch_cancel,
    .shape = touch_shape,
    .orientation = touch_orientation,
};

/* ------------------------------------------------------------- seat */

static void seat_caps(void *data, struct wl_seat *seat, uint32_t caps)
{
    Display *d = data;
    XDisplayImpl *dp = MWD(d);
    dp->seat_caps = caps;
    if ((caps & WL_SEAT_CAPABILITY_KEYBOARD) && !dp->wl_keyboard) {
        dp->wl_keyboard = wl_seat_get_keyboard(seat);
        wl_keyboard_add_listener(dp->wl_keyboard, &keyboard_listener, d);
    }
    if ((caps & WL_SEAT_CAPABILITY_POINTER) && !dp->wl_pointer) {
        dp->wl_pointer = wl_seat_get_pointer(seat);
        wl_pointer_add_listener(dp->wl_pointer, &pointer_listener, d);
    }
    if ((caps & WL_SEAT_CAPABILITY_TOUCH) && !dp->wl_touch) {
        dp->wl_touch = wl_seat_get_touch(seat);
        wl_touch_add_listener(dp->wl_touch, &touch_listener, d);
    }
}

static void seat_name(void *data, struct wl_seat *seat, const char *name)
{ (void)data; (void)seat; (void)name; }

static const struct wl_seat_listener seat_listener = {
    .capabilities = seat_caps,
    .name = seat_name,
};

void mw_input_init(Display *d, struct wl_seat *seat)
{
    MWD(d)->seat_caps = 0;
    wl_seat_add_listener(seat, &seat_listener, d);
}

void mw_input_fini(Display *d)
{
    XDisplayImpl *dp = MWD(d);
    if (dp->wl_keyboard) wl_keyboard_destroy(dp->wl_keyboard);
    if (dp->wl_pointer) wl_pointer_destroy(dp->wl_pointer);
    if (dp->wl_touch) wl_touch_destroy(dp->wl_touch);
    dp->wl_keyboard = NULL;
    dp->wl_pointer = NULL;
    dp->wl_touch = NULL;
    MwBtnGrab *bg = dp->btn_grabs;
    while (bg) { MwBtnGrab *n = bg->next; free(bg); bg = n; }
    MwKeyGrab *kg = dp->key_grabs;
    while (kg) { MwKeyGrab *n = kg->next; free(kg); kg = n; }
    dp->btn_grabs = NULL; dp->key_grabs = NULL;
}

/* ------------------------------------------------------------- grabs */

int XGrabPointer(Display *d, Window grab_window, Bool owner_events,
                 unsigned int event_mask, int pointer_mode, int keyboard_mode,
                 Window confine_to, Cursor cursor, Time time)
{
    (void)confine_to; (void)time;
    XDisplayImpl *dp = MWD(d);
    dp->ptr_grab_window = mw_window(d, grab_window);
    dp->ptr_grab_owner = owner_events;
    dp->ptr_grab_mask = event_mask;
    dp->ptr_grab_mode = pointer_mode;
    dp->ptr_grab_cursor = cursor;
    dp->ptr_grab_active = true;
    dp->ptr_grab_temporary = false;
    if (cursor != None) mw_set_pointer_cursor(d, cursor);
    (void)keyboard_mode;
    if (getenv("MW_TRACE"))
        fprintf(stderr, "MW: XGrabPointer win=0x%lx owner=%d mask=0x%x\n",
                grab_window, owner_events, event_mask);
    if (dp->ptr_focus) {
        int lx, ly;
        ptr_in_win_space(d, dp->ptr_focus, &lx, &ly);
        crossing(d, dp->ptr_focus, LeaveNotify, NotifyGrab, NotifyNonlinear, lx, ly);
    }
    if (dp->ptr_grab_window) {
        int ex, ey;
        ptr_in_win_space(d, dp->ptr_grab_window, &ex, &ey);
        crossing(d, dp->ptr_grab_window, EnterNotify, NotifyGrab, NotifyNonlinear, ex, ey);
    }
    return GrabSuccess;
}

int XUngrabPointer(Display *d, Time time)
{
    (void)time;
    XDisplayImpl *dp = MWD(d);
    if (getenv("MW_TRACE"))
        fprintf(stderr, "MW: XUngrabPointer (was 0x%lx owner=%d active=%d)\n",
                dp->ptr_grab_window ? dp->ptr_grab_window->id : 0UL,
                dp->ptr_grab_owner, dp->ptr_grab_active);
    if (dp->ptr_grab_active) {
        if (dp->ptr_grab_window) {
            int lx, ly;
            ptr_in_win_space(d, dp->ptr_grab_window, &lx, &ly);
            crossing(d, dp->ptr_grab_window, LeaveNotify, NotifyUngrab,
                     NotifyNonlinear, lx, ly);
        }
        if (dp->ptr_focus) {
            int ex, ey;
            ptr_in_win_space(d, dp->ptr_focus, &ex, &ey);
            crossing(d, dp->ptr_focus, EnterNotify, NotifyUngrab, NotifyNonlinear,
                     ex, ey);
        }
    }
    dp->ptr_grab_active = false;
    dp->ptr_grab_temporary = false;
    dp->ptr_grab_window = NULL;
    dp->ptr_grab_mask = 0;
    return 1;
}

/* The compositor dismissed a popup (xdg_popup.popup_done): its Wayland grab is
 * gone, so a modeled XGrabPointer on the dead popup must not keep stranding
 * input on it.  Unlike XUngrabPointer this emits no crossing events -- the
 * popup is being unmapped anyway -- it just stops routing presses to it. */
void mw_pointer_drop_grab(Display *d, MwWindow *win)
{
    XDisplayImpl *dp = MWD(d);
    if (!win) return;
    if (dp->ptr_grab_window == win) {
        dp->ptr_grab_active = false;
        dp->ptr_grab_temporary = false;
        dp->ptr_grab_window = NULL;
        dp->ptr_grab_owner = false;
        dp->ptr_grab_mask = 0;
    }
    if (dp->implicit_grab == win) dp->implicit_grab = NULL;
}

int XGrabButton(Display *d, unsigned int button, unsigned int modifiers,
                Window grab_window, Bool owner_events, unsigned int event_mask,
                int pointer_mode, int keyboard_mode, Window confine_to,
                Cursor cursor)
{
    (void)confine_to;
    XDisplayImpl *dp = MWD(d);
    MwBtnGrab *g = calloc(1, sizeof *g);
    g->window = grab_window; g->button = button; g->modifiers = modifiers;
    g->owner_events = owner_events; g->event_mask = event_mask;
    g->pointer_mode = pointer_mode; g->keyboard_mode = keyboard_mode;
    g->cursor = cursor;
    g->next = dp->btn_grabs;
    dp->btn_grabs = g;
    return 1;
}

int XUngrabButton(Display *d, unsigned int button, unsigned int modifiers,
                  Window grab_window)
{
    XDisplayImpl *dp = MWD(d);
    MwBtnGrab **pp = &dp->btn_grabs;
    while (*pp) {
        if ((*pp)->window == grab_window &&
            ((*pp)->button == button || button == AnyButton)) {
            MwBtnGrab *dead = *pp; *pp = dead->next; free(dead);
        } else pp = &(*pp)->next;
    }
    (void)modifiers;
    return 1;
}

int XGrabKey(Display *d, int keycode, unsigned int modifiers, Window grab_window,
             Bool owner_events, int pointer_mode, int keyboard_mode)
{
    XDisplayImpl *dp = MWD(d);
    MwKeyGrab *g = calloc(1, sizeof *g);
    g->window = grab_window; g->keycode = (KeyCode)keycode; g->modifiers = modifiers;
    g->owner_events = owner_events; g->pointer_mode = pointer_mode;
    g->keyboard_mode = keyboard_mode;
    g->next = dp->key_grabs;
    dp->key_grabs = g;
    return 1;
}

int XUngrabKey(Display *d, int keycode, unsigned int modifiers, Window grab_window)
{
    XDisplayImpl *dp = MWD(d);
    MwKeyGrab **pp = &dp->key_grabs;
    while (*pp) {
        if ((*pp)->window == grab_window &&
            ((*pp)->keycode == keycode || keycode == AnyKey)) {
            MwKeyGrab *dead = *pp; *pp = dead->next; free(dead);
        } else pp = &(*pp)->next;
    }
    (void)modifiers;
    return 1;
}

int XGrabKeyboard(Display *d, Window w, Bool owner_events, int pointer_mode,
                  int keyboard_mode, Time time)
{
    (void)pointer_mode; (void)time;
    XDisplayImpl *dp = MWD(d);
    dp->kbd_grab_window = mw_window(d, w);
    dp->kbd_grab_owner = owner_events;
    dp->kbd_grab_mode = keyboard_mode;
    return GrabSuccess;
}

int XUngrabKeyboard(Display *d, Time time)
{
    (void)time;
    MWD(d)->kbd_grab_window = NULL;
    return 1;
}

int XChangeActivePointerGrab(Display *d, unsigned int event_mask, Cursor cursor,
                             Time time)
{
    (void)time;
    XDisplayImpl *dp = MWD(d);
    dp->ptr_grab_mask = event_mask;
    if (cursor != None) mw_set_pointer_cursor(d, cursor);
    return 1;
}

Bool XQueryPointer(Display *d, Window w, Window *root, Window *child,
                   int *root_x, int *root_y, int *win_x, int *win_y,
                   unsigned int *mask)
{
    XDisplayImpl *dp = MWD(d);
    MwWindow *win = mw_window(d, w);
    /* Pointer position relative to `w` (dp->ptr_x/y are toplevel-relative). */
    int ox = 0, oy = 0;
    if (win) origin_rel(win, &ox, &oy);
    int rx = dp->ptr_x - ox;
    int ry = dp->ptr_y - oy;
    if (root) *root = MWSCR(d)->root;
    if (root_x) *root_x = dp->ptr_x;
    if (root_y) *root_y = dp->ptr_y;
    if (win_x) *win_x = rx;
    if (win_y) *win_y = ry;
    if (mask) *mask = dp->ptr_state | current_mods(d);
    /* The child of `w` under the pointer, or None when the pointer is on `w`
     * itself.  GDK uses this after an implicit grab ends to decide whether the
     * pointer left the grab window; always reporting None made it synthesize a
     * Leave and un-arm buttons. */
    if (child) {
        *child = None;
        if (win) {
            MwWindow *c = mw_child_at(win, rx, ry);
            if (c) *child = c->id;
        } else if (w == MWSCR(d)->root && dp->ptr_toplevel) {
            *child = dp->ptr_toplevel->id;
        }
    }
    return True;
}

/* Move the pointer to an absolute root position and emit the crossing and
 * motion events a real server sends on a warp.  GDK (and Motif) track the
 * window under the pointer from EnterNotify; without it a client that warps
 * and then synthesises a click (gdk_test_simulate_button) never arms the
 * target, and GTK's client-side-window hit test finds no window. */
static void ptr_warp(Display *d, int root_x, int root_y)
{
    XDisplayImpl *dp = MWD(d);
    MwWindow *root = mw_window(d, MWSCR(d)->root);
    if (!root) return;
    MwWindow *top = NULL;
    for (MwWindow *c = root->children; c; c = c->next_sib) {
        if (!c->mapped || !c->tl) continue;
        int ox, oy;
        mw_window_origin(c, &ox, &oy);
        if (root_x >= ox && root_y >= oy &&
            root_x < ox + c->w && root_y < oy + c->h) { top = c; break; }
    }
    if (!top) return;
    int ox, oy;
    mw_window_origin(top, &ox, &oy);
    dp->ptr_toplevel = top;
    dp->ptr_surf_x = root_x - ox;
    dp->ptr_surf_y = root_y - oy;
    int off = top->tl ? mw_toplevel_content_offset(top->tl) : 0;
    int rx = dp->ptr_surf_x, ry = dp->ptr_surf_y - off;
    MwWindow *deep = (ry >= 0) ? mw_deepest_at(top, rx, ry, NULL, NULL) : NULL;
    if (deep != dp->ptr_focus) {
        if (dp->ptr_focus) {
            XLeaveWindowEvent le;
            int lx, ly;
            ptr_in_win_space(d, dp->ptr_focus, &lx, &ly);
            pt_event(d, dp->ptr_focus, (XEvent *)&le, LeaveNotify, lx, ly,
                     current_mods(d), NotifyNonlinear, NotifyNormal, 0);
            le.mode = NotifyNormal; le.detail = NotifyAncestor;
            deliver(d, dp->ptr_focus, (XEvent *)&le, LeaveWindowMask);
        }
        dp->ptr_focus = deep;
        if (deep) {
            XEnterWindowEvent en;
            pt_event(d, deep, (XEvent *)&en, EnterNotify, rx, ry,
                     current_mods(d), NotifyNonlinear, NotifyNormal, 0);
            en.mode = NotifyNormal; en.detail = NotifyAncestor;
            deliver(d, deep, (XEvent *)&en, EnterWindowMask);
        }
    }
    dp->ptr_x = rx; dp->ptr_y = ry;
    if (deep) {
        XEvent ev;
        pt_event(d, deep, &ev, MotionNotify, rx, ry, current_mods(d),
                 0, NotifyNormal, 0);
        ev.xmotion.window = deep->id;
        mw_put_event(d, &ev);
    }
    mw_pointer_update_cursor(d, deep);
}

int XWarpPointer(Display *d, Window src, Window dest, int sx, int sy,
                 unsigned int sw, unsigned int sh, int dx, int dy)
{
    (void)src; (void)sy; (void)sw; (void)sh;
    MwWindow *w = mw_window(d, dest);
    if (!w) return 1;
    int wx, wy;
    mw_window_origin(w, &wx, &wy);
    ptr_warp(d, wx + dx, wy + dy);
    return 1;
}

int XChangePointerControl(Display *d, Bool do_accel, Bool do_thresh,
                          int accel_num, int accel_denom, int threshold)
{ (void)d;(void)do_accel;(void)do_thresh;(void)accel_num;(void)accel_denom;(void)threshold; return 1; }

int XGetPointerControl(Display *d, int *accel_num, int *accel_denom, int *threshold)
{ (void)d; if (accel_num)*accel_num=2; if (accel_denom)*accel_denom=1; if (threshold)*threshold=4; return 1; }

int XGetPointerMapping(Display *d, unsigned char *map, int nmap)
{ (void)d; if (nmap > 0) map[0]=1; if (nmap>1) map[1]=2; if (nmap>2) map[2]=3; return 3; }

int XSetPointerMapping(Display *d, _Xconst unsigned char *map, int nmap)
{ (void)d;(void)map;(void)nmap; return MappingSuccess; }

void mw_pointer_motion(Display *d, int x, int y) { (void)d; (void)x; (void)y; }
void mw_pointer_button(Display *d, uint32_t b, uint32_t s) { (void)d; (void)b; (void)s; }
void mw_pointer_frame(Display *d) { (void)d; }
void mw_pointer_enter(Display *d, MwWindow *t, int x, int y) { (void)d;(void)t;(void)x;(void)y; }
void mw_pointer_leave(Display *d, MwWindow *t) { (void)d;(void)t; }
void mw_keyboard_key(Display *d, uint32_t k, uint32_t s) { (void)d;(void)k;(void)s; }
