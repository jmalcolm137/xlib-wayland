/* xdamage.c — a libXdamage facade over the Wayland Xlib shim.
 *
 * Installed as libXdamage.so.1.  DAMAGE objects live in the shim's libX11
 * (src/xlib/xdamage.c), which also emits XDamageNotify from its own damage, so
 * GDK's composited-window repaint path stays live.
 */
#include <X11/Xlib.h>
#include <X11/extensions/Xdamage.h>

extern unsigned long mw_xdamage_new(Display *, Drawable, int);
extern void          mw_xdamage_free(Display *, unsigned long);
extern int           mw_xdamage_eventbase(void);
extern int           mw_xdamage_errorbase(void);
extern void          mw_xdamage_subtract(Display *, unsigned long, unsigned long,
                                         unsigned long);
extern void          mw_xdamage_add(Display *, Drawable, unsigned long);

Bool XDamageQueryExtension(Display *dpy, int *event_base_return,
                           int *error_base_return)
{
    (void)dpy;
    if (event_base_return) *event_base_return = mw_xdamage_eventbase();
    if (error_base_return) *error_base_return = mw_xdamage_errorbase();
    return True;
}

Status XDamageQueryVersion(Display *dpy, int *major_version_return,
                           int *minor_version_return)
{
    (void)dpy;
    if (major_version_return) *major_version_return = 1;
    if (minor_version_return) *minor_version_return = 1;
    return 1;
}

Damage XDamageCreate(Display *dpy, Drawable drawable, int level)
{
    return (Damage)mw_xdamage_new(dpy, drawable, level);
}

void XDamageDestroy(Display *dpy, Damage damage)
{
    mw_xdamage_free(dpy, (unsigned long)damage);
}

/* Repair damage: all of it (repair None) or the part in `repair`, with what
 * was repaired stored in `parts`. */
void XDamageSubtract(Display *dpy, Damage damage, XserverRegion repair,
                     XserverRegion parts)
{
    mw_xdamage_subtract(dpy, (unsigned long)damage, (unsigned long)repair,
                        (unsigned long)parts);
}

/* Report a region of a drawable as damaged (to every damage object on it). */
void XDamageAdd(Display *dpy, Drawable drawable, XserverRegion region)
{
    mw_xdamage_add(dpy, drawable, (unsigned long)region);
}
