/* xfixes.c — the XFIXES surface GDK uses.
 *
 * The shim is an in-process X server, so there is no extension opcode; the
 * libXfixes facade (src/xfixes/xfixes.c) calls straight into these helpers.
 * What matters for GTK2 is XFixesSelectSelectionInput + XFixesSelectionNotify:
 * GDK registers interest in the CLIPBOARD (and PRIMARY) atoms on its leader
 * window and updates the paste UI from the notifications.  Emitting them from
 * XSetSelectionOwner means a change made by another shim process (relayed
 * through the broker into our own X selection) reaches the app too.
 */
#include "internal.h"

#include <X11/extensions/Xfixes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

Bool mw_xfixes_query(int *event_base, int *error_base)
{
    if (event_base) *event_base = MW_XFIXES_EVENT_BASE;
    if (error_base) *error_base = MW_XFIXES_ERROR_BASE;
    return True;
}

void mw_xfixes_init(Display *d)
{
    XDisplayImpl *dp = MWD(d);
    dp->xfixes_sels = NULL;
    dp->xfixes_regions = NULL;
    dp->xfixes_next_region = 1;
}

void mw_xfixes_fini(Display *d)
{
    XDisplayImpl *dp = MWD(d);
    MwXfixesSel *s = dp->xfixes_sels;
    while (s) { MwXfixesSel *n = s->next; free(s); s = n; }
    dp->xfixes_sels = NULL;
    MwXfixesRegion *r = dp->xfixes_regions;
    while (r) { MwXfixesRegion *n = r->next; free(r); r = n; }
    dp->xfixes_regions = NULL;
}

void mw_xfixes_select_input(Display *d, Window w, Atom selection,
                            unsigned long mask)
{
    XDisplayImpl *dp = MWD(d);
    for (MwXfixesSel *s = dp->xfixes_sels; s; s = s->next)
        if (s->window == w && s->selection == selection) { s->mask = mask; return; }
    MwXfixesSel *s = calloc(1, sizeof *s);
    if (!s) return;
    s->window = w;
    s->selection = selection;
    s->mask = mask;
    s->next = dp->xfixes_sels;
    dp->xfixes_sels = s;
}

void mw_xfixes_selection_notify(Display *d, Atom selection, Window owner,
                                Time time)
{
    XDisplayImpl *dp = MWD(d);
    if (!dp->xfixes_sels) return;
    if (time == CurrentTime) time = mw_now();

    for (MwXfixesSel *s = dp->xfixes_sels; s; s = s->next) {
        if (s->selection != selection) continue;
        if (!(s->mask & XFixesSetSelectionOwnerNotifyMask)) continue;

        XFixesSelectionNotifyEvent ev;
        memset(&ev, 0, sizeof ev);
        ev.type = MW_XFIXES_EVENT_BASE + XFixesSelectionNotify;
        ev.window = s->window;
        ev.subtype = XFixesSetSelectionOwnerNotify;
        ev.owner = owner;
        ev.selection = selection;
        ev.timestamp = time;
        ev.selection_timestamp = time;
        mw_put_event(d, (XEvent *)&ev);
        if (getenv("MW_TRACE"))
            fprintf(stderr, "MW: XFixesSelectionNotify win=0x%lx sel=%lu owner=0x%lx\n",
                    (unsigned long)s->window, (unsigned long)selection,
                    (unsigned long)owner);
    }
}

unsigned long mw_xfixes_new_region(Display *d)
{
    XDisplayImpl *dp = MWD(d);
    MwXfixesRegion *r = calloc(1, sizeof *r);
    if (!r) return 0;
    r->id = dp->xfixes_next_region++;
    r->next = dp->xfixes_regions;
    dp->xfixes_regions = r;
    return r->id;
}

void mw_xfixes_free_region(Display *d, unsigned long region)
{
    XDisplayImpl *dp = MWD(d);
    for (MwXfixesRegion **pp = &dp->xfixes_regions; *pp; pp = &(*pp)->next) {
        if ((*pp)->id == region) {
            MwXfixesRegion *dead = *pp;
            *pp = dead->next;
            free(dead);
            return;
        }
    }
}

/* XFixesChangeCursor(image, target): GDK loads a new themed cursor `image` and
 * asks that everything currently using `target` show it instead, so a theme
 * change updates the cursors already set on windows.  Follow the alias when a
 * cursor is applied. */
void mw_xfixes_change_cursor(Display *d, Cursor image, Cursor target)
{
    MwCursor *c = mw_cursor(d, target);
    if (c) c->alias_to = image;
    /* The alias is followed the next time a cursor is applied (the pointer
     * moving over a window re-evaluates it), which is enough for a theme
     * change. */
}
