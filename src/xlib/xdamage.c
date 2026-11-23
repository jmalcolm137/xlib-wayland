/* xdamage.c — the DAMAGE extension, tracked locally.
 *
 * The X composite/damage model lets a compositor redirect a window and be told
 * when its contents change.  Under Wayland the compositor is the Wayland
 * compositor, so there is nothing to redirect; but GDK's composited-window path
 * (used for RGBA/transparent windows) creates a damage on its own window and
 * repaints from the notifications.  We keep the damage objects and emit
 * XDamageNotify whenever the shim damages that drawable, so that path stays
 * live.
 */
#include "internal.h"

#include <X11/extensions/Xdamage.h>
#include <stdlib.h>
#include <string.h>

int mw_xdamage_eventbase(void) { return MW_XDAMAGE_EVENT_BASE; }
int mw_xdamage_errorbase(void) { return MW_XDAMAGE_ERROR_BASE; }

void mw_xdamage_init(Display *d)
{
    MWD(d)->damages = NULL;
}

static void damage_free(MwDamage *dm)
{
    if (dm->damage) XDestroyRegion(dm->damage);
    free(dm);
}

void mw_xdamage_fini(Display *d)
{
    XDisplayImpl *dp = MWD(d);
    MwDamage *dm = dp->damages;
    while (dm) { MwDamage *n = dm->next; damage_free(dm); dm = n; }
    dp->damages = NULL;
}

unsigned long mw_xdamage_new(Display *d, Drawable drawable, int level)
{
    XDisplayImpl *dp = MWD(d);
    MwDamage *dm = calloc(1, sizeof *dm);
    if (!dm) return 0;
    dm->damage = XCreateRegion();
    if (!dm->damage) { free(dm); return 0; }
    dm->id = mw_alloc_id(d);
    dm->drawable = drawable;
    dm->level = level;
    dm->next = dp->damages;
    dp->damages = dm;
    return dm->id;
}

void mw_xdamage_free(Display *d, unsigned long id)
{
    XDisplayImpl *dp = MWD(d);
    for (MwDamage **pp = &dp->damages; *pp; pp = &(*pp)->next) {
        if ((*pp)->id == id) {
            MwDamage *dead = *pp;
            *pp = dead->next;
            damage_free(dead);
            return;
        }
    }
}

/* As the server does, a damage object goes away with its drawable. */
void mw_xdamage_drawable_gone(Display *d, Drawable drawable)
{
    XDisplayImpl *dp = MWD(d);
    for (MwDamage **pp = &dp->damages; *pp; ) {
        if ((*pp)->drawable == drawable) {
            MwDamage *dead = *pp;
            *pp = dead->next;
            damage_free(dead);
        } else {
            pp = &(*pp)->next;
        }
    }
}

static MwDamage *find_damage(Display *d, unsigned long id)
{
    for (MwDamage *dm = MWD(d)->damages; dm; dm = dm->next)
        if (dm->id == id) return dm;
    return NULL;
}

static void send_notify(Display *d, MwDamage *dm, XRectangle area)
{
    XDamageNotifyEvent ev;
    memset(&ev, 0, sizeof ev);
    ev.type = MW_XDAMAGE_EVENT_BASE + XDamageNotify;
    ev.display = d;
    ev.drawable = dm->drawable;
    ev.damage = dm->id;
    ev.level = dm->level;
    ev.more = False;
    ev.timestamp = mw_now();
    ev.area = area;
    /* geometry is the drawable's, not the damaged area's. */
    MwWindow *win = mw_window(d, dm->drawable);
    if (win) {
        ev.geometry.x = (short)win->x;
        ev.geometry.y = (short)win->y;
        ev.geometry.width = (unsigned short)win->w;
        ev.geometry.height = (unsigned short)win->h;
    } else {
        MwPixmap *pm = mw_pixmap(d, dm->drawable);
        if (pm) {
            ev.geometry.x = 0;
            ev.geometry.y = 0;
            ev.geometry.width = (unsigned short)pm->w;
            ev.geometry.height = (unsigned short)pm->h;
        } else {
            ev.geometry = area;
        }
    }
    mw_put_event(d, (XEvent *)&ev);
}

/* Add a rectangle to one damage object and report it as its level asks:
 * Raw every rectangle, Delta the part that is new, BoundingBox when the box
 * grows, NonEmpty when the damage stops being empty. */
static void damage_add(Display *d, MwDamage *dm, XRectangle r)
{
    if (r.width == 0 || r.height == 0) return;
    bool was_empty = XEmptyRegion(dm->damage);
    XRectangle before;
    XClipBox(dm->damage, &before);
    bool fresh = XRectInRegion(dm->damage, r.x, r.y, r.width, r.height) != RectangleIn;
    XUnionRectWithRegion(&r, dm->damage, dm->damage);
    XRectangle after;
    XClipBox(dm->damage, &after);
    switch (dm->level) {
    case XDamageReportRawRectangles:
        send_notify(d, dm, r);
        break;
    case XDamageReportDeltaRectangles:
        if (fresh) send_notify(d, dm, r);
        break;
    case XDamageReportBoundingBox:
        if (was_empty || memcmp(&before, &after, sizeof before) != 0)
            send_notify(d, dm, after);
        break;
    case XDamageReportNonEmpty:
    default:
        if (was_empty) send_notify(d, dm, after);
        break;
    }
}

void mw_xdamage_notify(Display *d, Drawable drawable, int x, int y, int w, int h)
{
    XDisplayImpl *dp = MWD(d);
    XRectangle r = { (short)x, (short)y, (unsigned short)w, (unsigned short)h };
    for (MwDamage *dm = dp->damages; dm; dm = dm->next)
        if (dm->drawable == drawable) damage_add(d, dm, r);
}

void mw_xdamage_add(Display *d, Drawable drawable, unsigned long region)
{
    Region src = mw_xfixes_region(d, region);
    if (!src) return;
    XRectangle *rects = NULL;
    int n = mw_region_rectangles(src, &rects);
    for (MwDamage *dm = MWD(d)->damages; dm; dm = dm->next)
        if (dm->drawable == drawable)
            for (int i = 0; i < n; i++) damage_add(d, dm, rects[i]);
    free(rects);
}

/* XDamageSubtract: repair None takes all the damage, otherwise the part in
 * `repair`; `parts` (if given) receives what was taken.  Damage that is left
 * is reported again, so the client knows to repaint it. */
void mw_xdamage_subtract(Display *d, unsigned long damage, unsigned long repair,
                         unsigned long parts)
{
    MwDamage *dm = find_damage(d, damage);
    if (!dm) return;
    Region out = parts ? mw_xfixes_region(d, parts) : NULL;
    Region rep = repair ? mw_xfixes_region(d, repair) : NULL;
    if (repair && !rep) return;
    if (!rep) {
        if (out) {
            XSubtractRegion(out, out, out);
            XUnionRegion(dm->damage, out, out);
        }
        XSubtractRegion(dm->damage, dm->damage, dm->damage);
        return;
    }
    if (out) XIntersectRegion(dm->damage, rep, out);
    XSubtractRegion(dm->damage, rep, dm->damage);
    if (!XEmptyRegion(dm->damage)) {
        XRectangle box;
        XClipBox(dm->damage, &box);
        send_notify(d, dm, box);
    }
}
