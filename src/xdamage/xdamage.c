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

Bool XDamageQueryExtension(Display *dpy, int *event_base_return,
                           int *error_base_return)
{
    (void)dpy;
    if (event_base_return) *event_base_return = mw_xdamage_eventbase();
    if (error_base_return) *error_base_return = mw_xdamage_eventbase();
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

/* The damage has already been delivered as events; there is no server-side
 * accumulation to subtract. */
void XDamageSubtract(Display *dpy, Damage damage, XserverRegion repair,
                     XserverRegion parts)
{
    (void)dpy; (void)damage; (void)repair; (void)parts;
}

void XDamageAdd(Display *dpy, Drawable drawable, XserverRegion region)
{
    (void)dpy; (void)drawable; (void)region;
}
