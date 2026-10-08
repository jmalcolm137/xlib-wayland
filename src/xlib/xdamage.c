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

void mw_xdamage_init(Display *d)
{
    MWD(d)->damages = NULL;
}

void mw_xdamage_fini(Display *d)
{
    XDisplayImpl *dp = MWD(d);
    MwDamage *dm = dp->damages;
    while (dm) { MwDamage *n = dm->next; free(dm); dm = n; }
    dp->damages = NULL;
}

unsigned long mw_xdamage_new(Display *d, Drawable drawable, int level)
{
    XDisplayImpl *dp = MWD(d);
    MwDamage *dm = calloc(1, sizeof *dm);
    if (!dm) return 0;
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
            free(dead);
            return;
        }
    }
}

void mw_xdamage_notify(Display *d, Drawable drawable, int x, int y, int w, int h)
{
    XDisplayImpl *dp = MWD(d);
    if (!dp->damages) return;
    int any = 0;
    for (MwDamage *dm = dp->damages; dm; dm = dm->next)
        if (dm->drawable == drawable) { any = 1; break; }
    if (!any) return;

    for (MwDamage *dm = dp->damages; dm; dm = dm->next) {
        if (dm->drawable != drawable) continue;
        XDamageNotifyEvent ev;
        memset(&ev, 0, sizeof ev);
        ev.type = MW_XDAMAGE_EVENT_BASE;   /* + XDamageNotify (0) */
        ev.drawable = drawable;
        ev.damage = dm->id;
        ev.level = dm->level;
        ev.more = False;
        ev.timestamp = mw_now();
        ev.area.x = (short)x;
        ev.area.y = (short)y;
        ev.area.width = (unsigned short)w;
        ev.area.height = (unsigned short)h;
        ev.geometry = ev.area;
        mw_put_event(d, (XEvent *)&ev);
    }
}
