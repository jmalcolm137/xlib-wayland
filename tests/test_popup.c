/* test_popup.c — Motif-free reproduction of the menu item-selection path.
 *
 * An open menu on X is: an override-redirect top-level (the menu shell) whose
 * children are the item windows, plus an XGrabPointer on the shell with
 * owner_events=True so that presses inside an item still go to the item.
 * Motif builds exactly that, so this test exercises the same shim code
 * without needing libXt/libXm to be present:
 *
 *   1. a toplevel with a "menubar" child,
 *   2. a press on the menubar posts an override-redirect popup,
 *   3. pointer motion into an item must arm it (Enter/Leave on the item),
 *   4. a press on an item must be delivered to *that item*, with
 *      event->x/y relative to the item, not to the menu shell.
 *
 * Prints "MENU:<name>" and exits 0 when an item is selected, or
 * "MENU:TIMEOUT" and exits 1 if nothing is selected before the deadline.
 */
#include <X11/Xlib.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/time.h>

/* Height of the client-side titlebar the shim reserves for toplevels.  A
 * popup gets no titlebar, so the two coordinate spaces differ. */
#define TB_H 24

#define NITEMS 3
#define POPUP_X 16
#define POPUP_Y 24           /* just below the menubar, in content coords */
#define POPUP_W 140
#define POPUP_H 96
#define ITEM_X 4
#define ITEM_Y 4
#define ITEM_W 132
#define ITEM_H 26
#define ITEM_PITCH 30

static Display *dpy;
static GC gc;
static Window main_w, bar_w, popup_w;
static Window item[NITEMS];
static const char *names[NITEMS] = { "Alpha", "Beta", "Quit" };
static int posted;                 /* popup created */
static int verbose;                /* log every event */
static int cycles;                 /* selections to make before exiting */
static int selections;             /* selections made so far */

static double now_ms(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return tv.tv_sec * 1000.0 + tv.tv_usec / 1000.0;
}

static const char *type_name(int t)
{
    switch (t) {
    case Expose:          return "Expose";
    case EnterNotify:     return "EnterNotify";
    case LeaveNotify:     return "LeaveNotify";
    case MotionNotify:    return "MotionNotify";
    case ButtonPress:     return "ButtonPress";
    case ButtonRelease:   return "ButtonRelease";
    case KeyPress:        return "KeyPress";
    case ConfigureNotify: return "ConfigureNotify";
    case MapNotify:       return "MapNotify";
    case FocusIn:         return "FocusIn";
    default:              return "other";
    }
}

/* Which item window is this XID? */
static int item_index(Window w)
{
    for (int i = 0; i < NITEMS; i++)
        if (item[i] == w) return i;
    return -1;
}

/* Cross-check: which item *should* have been hit, given coordinates that are
 * relative to the popup shell?  Used only to explain a failure. */
static int item_index_at_shell(int x, int y)
{
    for (int i = 0; i < NITEMS; i++) {
        int iy = ITEM_Y + i * ITEM_PITCH;
        if (x >= ITEM_X && x < ITEM_X + ITEM_W &&
            y >= iy && y < iy + ITEM_H)
            return i;
    }
    return -1;
}

static void draw_item(int i, int armed)
{
    XSetForeground(dpy, gc, armed ? 0x3060a0 : 0xd8d8d0);
    XFillRectangle(dpy, item[i], gc, 0, 0, ITEM_W, ITEM_H);
    XSetForeground(dpy, gc, 0x101010);
    XDrawRectangle(dpy, item[i], gc, 0, 0, ITEM_W - 1, ITEM_H - 1);
    XDrawString(dpy, item[i], gc, 8, 18, names[i], (int)strlen(names[i]));
    XFlush(dpy);
}

static void draw_popup(void)
{
    XSetForeground(dpy, gc, 0xf0f0e8);
    XFillRectangle(dpy, popup_w, gc, 0, 0, POPUP_W, POPUP_H);
    XSetForeground(dpy, gc, 0x606060);
    XDrawRectangle(dpy, popup_w, gc, 0, 0, POPUP_W - 1, POPUP_H - 1);
    XFlush(dpy);
}

/* Build the popup shell and its item children once.  Motif does exactly this:
 * the menu shell is created with the cascade widget and reused for every
 * posting, dismissed with XUnmapWindow and re-posted with XMapWindow. */
static void create_menu(void)
{
    XSetWindowAttributes attr;
    memset(&attr, 0, sizeof attr);
    attr.override_redirect = True;
    attr.backing_pixel = 0xf0f0e8;

    popup_w = XCreateWindow(dpy, main_w, POPUP_X, POPUP_Y,
                            POPUP_W, POPUP_H, 0, CopyFromParent,
                            InputOutput, CopyFromParent,
                            CWOverrideRedirect | CWBackPixel, &attr);
    gc = XCreateGC(dpy, popup_w, 0, NULL);
    XSetFont(dpy, gc, XLoadFont(dpy, "fixed"));

    for (int i = 0; i < NITEMS; i++) {
        XSetWindowAttributes ia;
        memset(&ia, 0, sizeof ia);
        ia.backing_pixel = 0xd8d8d0;
        item[i] = XCreateWindow(dpy, popup_w, ITEM_X, ITEM_Y + i * ITEM_PITCH,
                                ITEM_W, ITEM_H, 0, CopyFromParent,
                                InputOutput, CopyFromParent,
                                CWBackPixel, &ia);
        /* Motif arms an item on EnterNotify and disarms on LeaveNotify, and
         * selects it on ButtonRelease inside it. */
        XSelectInput(dpy, item[i],
                     ExposureMask | ButtonPressMask | ButtonReleaseMask |
                     EnterWindowMask | LeaveWindowMask);
        XMapWindow(dpy, item[i]);
    }
}

/* Post the menu: map the shell, grab the pointer.  This is the Xlib-level
 * equivalent of XmMenuShell posting an XmPopupMenu. */
static void post_menu(void)
{
    XMapRaised(dpy, popup_w);

    /* owner_events=True: presses that land on an item must be delivered to
     * the item, not to the grab window. */
    XGrabPointer(dpy, popup_w, True,
                 ButtonPressMask | ButtonReleaseMask | PointerMotionMask |
                 EnterWindowMask | LeaveWindowMask,
                 GrabModeAsync, GrabModeAsync, None, None, CurrentTime);
    XGrabKeyboard(dpy, popup_w, True, GrabModeAsync, GrabModeAsync, CurrentTime);

    XSync(dpy, False);
    posted = 1;
    printf("MENU:POSTED\n");
    fflush(stdout);
}

/* Dismiss the menu, reusing the same shell window -- what Motif does on a
 * selection: ungrab, XUnmapWindow, and later XMapWindow again. */
static void unpost_menu(void)
{
    XUngrabPointer(dpy, CurrentTime);
    XUngrabKeyboard(dpy, CurrentTime);
    XUnmapWindow(dpy, popup_w);
    XSync(dpy, False);
    posted = 0;
    printf("MENU:UNPOSTED\n");
    fflush(stdout);
}

int main(int argc, char **argv)
{
    double seconds = argc > 1 ? atof(argv[1]) : 5.0;
    verbose = getenv("POPUP_VERBOSE") != NULL;
    /* Number of times the menu must be posted, selected from and dismissed.
     * More than one exercises the re-map path: after a NULL-buffer commit the
     * surface is "newly unmapped" and needs a fresh commit + configure + ack
     * before the next buffer is attached.  KWin killed NEdit with "attached
     * buffer before configure event" on that path. */
    cycles = argc > 2 ? atoi(argv[2]) : 1;
    if (cycles < 1) cycles = 1;
    int armed = -1, selected = -1;

    dpy = XOpenDisplay(NULL);
    if (!dpy) { fprintf(stderr, "cannot open display\n"); return 2; }

    main_w = XCreateWindow(dpy, RootWindow(dpy, DefaultScreen(dpy)), 0, 0,
                           300, 200, 1, CopyFromParent, InputOutput,
                           CopyFromParent, CWBackPixel,
                           &(XSetWindowAttributes){ .background_pixel = 0xffffff });
    XStoreName(dpy, main_w, "popup test");
    XSelectInput(dpy, main_w,
                 ExposureMask | ButtonPressMask | StructureNotifyMask |
                 SubstructureNotifyMask);

    bar_w = XCreateSimpleWindow(dpy, main_w, 0, 0, 300, TB_H, 0,
                                0xb0b0b0, 0xc0c0c0);
    XSelectInput(dpy, bar_w, ButtonPressMask | ExposureMask);
    XMapWindow(dpy, bar_w);
    XMapWindow(dpy, main_w);
    XSync(dpy, False);

    create_menu();

    double start = now_ms();
    for (;;) {
        if (XPending(dpy)) {
            XEvent e;
            XNextEvent(dpy, &e);

            if (verbose) {
                int is_cross = (e.type == EnterNotify || e.type == LeaveNotify);
                int is_xy    = (e.type >= ButtonPress && e.type <= MotionNotify) ||
                               is_cross;
                Window w = is_xy ? (is_cross ? e.xcrossing.window : e.xbutton.window)
                                 : e.xany.window;
                fprintf(stderr, "ev %-15s win=0x%lx %d,%d\n", type_name(e.type), w,
                        is_xy ? (is_cross ? e.xcrossing.x : e.xbutton.x) : 0,
                        is_xy ? (is_cross ? e.xcrossing.y : e.xbutton.y) : 0);
            }

            switch (e.type) {
            case Expose:
                if (posted && e.xexpose.window == popup_w) draw_popup();
                if (posted && item_index(e.xexpose.window) >= 0)
                    draw_item(item_index(e.xexpose.window),
                              item_index(e.xexpose.window) == armed);
                break;

            case UnmapNotify:
                /* The compositor can dismiss a popup on its own (a click
                 * outside), which arrives as xdg_popup.popup_done.  The shim
                 * then unmaps the X window; mirror Motif by noting that the
                 * menu is no longer posted. */
                if (e.xunmap.window == popup_w) {
                    posted = 0;
                    printf("MENU:DISMISSED\n");
                    fflush(stdout);
                }
                break;

            case EnterNotify:
                if (posted && item_index(e.xcrossing.window) >= 0) {
                    armed = item_index(e.xcrossing.window);
                    draw_item(armed, 1);
                    printf("ARM:%d\n", armed);
                    fflush(stdout);
                }
                break;

            case LeaveNotify:
                if (posted && item_index(e.xcrossing.window) == armed) {
                    draw_item(armed, 0);
                    armed = -1;
                }
                break;

            case ButtonPress:
                if (e.xbutton.button != Button1) break;
                if (e.xbutton.window == bar_w && !posted) {
                    post_menu();
                    break;
                }
                if (posted && e.xbutton.window == popup_w) {
                    /* Press landed on the shell, not on an item: the shim
                     * failed to hit-test into the item. */
                    printf("MENU:MISS shell %d,%d expected-item=%d\n",
                           e.xbutton.x, e.xbutton.y,
                           item_index_at_shell(e.xbutton.x, e.xbutton.y));
                    fflush(stdout);
                }
                if (posted && item_index(e.xbutton.window) >= 0) {
                    selected = item_index(e.xbutton.window);
                    printf("ITEM:%d at %d,%d\n", selected,
                           e.xbutton.x, e.xbutton.y);
                    fflush(stdout);
                }
                break;

            case ButtonRelease:
                if (posted && item_index(e.xbutton.window) >= 0 &&
                    item_index(e.xbutton.window) == selected) {
                    printf("MENU:%s\n", names[selected]);
                    fflush(stdout);
                    selections++;
                    if (selections >= cycles) {
                        XUngrabPointer(dpy, CurrentTime);
                        XCloseDisplay(dpy);
                        return 0;
                    }
                    /* Dismiss and start over on the same shell window, the way
                     * Motif re-posts a menu for the next cascade. */
                    unpost_menu();
                    armed = -1;
                    selected = -1;
                }
                break;

            default:
                break;
            }
            continue;
        }

        if (now_ms() - start > seconds * 1000.0) {
            printf("MENU:TIMEOUT\n");
            fflush(stdout);
            return 1;
        }
        usleep(5000);
    }
}