/* selection.c — selections, cut buffers, and inter-client transfer stubs. */
#include "internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void mw_init_selection(Display *d)
{
    XDisplayImpl *dp = MWD(d);
    dp->selections = NULL;
    for (int i = 0; i < 8; i++) { dp->cutbuf[i] = NULL; dp->cutbuf_len[i] = 0; }
}

void mw_fini_selection(Display *d)
{
    XDisplayImpl *dp = MWD(d);
    MwSelection *s = dp->selections;
    while (s) { MwSelection *n = s->next; free(s); s = n; }
    dp->selections = NULL;
}

static MwSelection *find_sel(Display *d, Atom selection)
{
    for (MwSelection *s = MWD(d)->selections; s; s = s->next)
        if (s->selection == selection) return s;
    return NULL;
}

int XSetSelectionOwner(Display *d, Atom selection, Window owner, Time time)
{
    XDisplayImpl *dp = MWD(d);
    MwSelection *s = find_sel(d, selection);
    if (!s) {
        s = calloc(1, sizeof *s);
        s->selection = selection;
        s->next = dp->selections;
        dp->selections = s;
    }
    Window old = s->owner;
    s->owner = owner;
    s->time = time;
    mw_clipboard_owner_changed(d, selection, owner);
    mw_broker_owner_changed(d, selection, owner);
    mw_dnd_selection_changed(d, selection, owner);
    mw_xdnd_selection_changed(d, selection, owner);
    mw_xfixes_selection_notify(d, selection, owner, time);
    if (old != None && old != owner) {
        MwWindow *w = mw_window(d, old);
        if (w) {
            XSelectionClearEvent ce;
            memset(&ce, 0, sizeof ce);
            ce.type = SelectionClear; ce.display = d; ce.window = old;
            ce.selection = selection; ce.time = time;
            mw_put_event(d, (XEvent *)&ce);
        }
    }
    return 1;
}

Window XGetSelectionOwner(Display *d, Atom selection)
{
    MwSelection *s = find_sel(d, selection);
    return s ? s->owner : None;
}

int XConvertSelection(Display *d, Atom selection, Atom target, Atom property,
                      Window requestor, Time time)
{
    MwSelection *s = find_sel(d, selection);
    Window owner = s ? s->owner : None;

    if (getenv("MW_TRACE")) {
        const char *sn = XGetAtomName(d, selection);
        if (sn && strncmp(sn, "_MOTIF_ATOM_", 12) == 0) {
            const char *tn = XGetAtomName(d, target);
            fprintf(stderr, "MW: XConvertSelection %s target=%s owner=0x%lx\n",
                    sn, tn ? tn : "?", (unsigned long)owner);
            if (tn) XFree((char *)tn);
        }
        if (sn) XFree((char *)sn);
    }

    /* No X client owns it, or the owner is our proxy window standing in for
     * the compositor clipboard: serve the request from the Wayland offer. */
    if (owner == None || owner == MWD(d)->clip_window) {
        /* A shared selection may be owned by another shim process (e.g. the
         * colour server in dtsession); ask its broker for the transfer. */
        if (owner == None &&
            mw_broker_convert(d, selection, target, property, requestor, time))
            return 1;
        if (mw_clipboard_xconvert(d, selection, target, property, requestor, time))
            return 1;   /* SelectionNotify posted once the transfer completes */
        XSelectionEvent se;
        memset(&se, 0, sizeof se);
        se.type = SelectionNotify; se.display = d; se.requestor = requestor;
        se.selection = selection; se.target = target; se.property = None;
        se.time = time;
        mw_put_event(d, (XEvent *)&se);
        return 1;
    }
    /* A Motif drag's icc handle, standing in for a remote initiator: serve the
     * dropped bytes to the drop site. */
    if (owner != None && mw_dnd_owns_window(d, owner) &&
        mw_dnd_xconvert(d, selection, target, property, requestor, time))
        return 1;
    /* Our synthetic XDND source window: the drop site converts XdndSelection. */
    if (owner != None && mw_xdnd_owns_window(d, owner) &&
        mw_xdnd_xconvert(d, selection, target, property, requestor, time))
        return 1;
    MwWindow *owner_win = mw_window(d, owner);
    if (!owner_win) return 0;
    XSelectionRequestEvent re;
    memset(&re, 0, sizeof re);
    re.type = SelectionRequest;
    re.display = d;
    re.owner = owner;
    re.requestor = requestor;
    re.selection = selection;
    re.target = target;
    re.property = property == None ? target : property;
    re.time = time;
    mw_put_event(d, (XEvent *)&re);
    return 1;
}

/* ----------------------------------------------------------- cut buffers */

int XStoreBytes(Display *d, _Xconst char *bytes, int nbytes)
{ return XStoreBuffer(d, bytes, nbytes, 0); }

int XStoreBuffer(Display *d, _Xconst char *bytes, int nbytes, int buffer)
{
    XDisplayImpl *dp = MWD(d);
    if (buffer < 0 || buffer > 7) return 0;
    free(dp->cutbuf[buffer]);
    if (nbytes < 0) nbytes = bytes ? (int)strlen(bytes) : 0;
    dp->cutbuf[buffer] = malloc((size_t)nbytes + 1);
    if (bytes) memcpy(dp->cutbuf[buffer], bytes, nbytes);
    dp->cutbuf[buffer][nbytes] = 0;
    dp->cutbuf_len[buffer] = (size_t)nbytes;
    return 1;
}

char *XFetchBytes(Display *d, int *nbytes)
{ return XFetchBuffer(d, nbytes, 0); }

char *XFetchBuffer(Display *d, int *nbytes, int buffer)
{
    XDisplayImpl *dp = MWD(d);
    if (buffer < 0 || buffer > 7) { if (nbytes) *nbytes = 0; return NULL; }
    if (!dp->cutbuf[buffer]) { if (nbytes) *nbytes = 0; return NULL; }
    if (nbytes) *nbytes = (int)dp->cutbuf_len[buffer];
    char *out = malloc(dp->cutbuf_len[buffer] + 1);
    memcpy(out, dp->cutbuf[buffer], dp->cutbuf_len[buffer] + 1);
    return out;
}

int XRotateBuffers(Display *d, int rotate)
{
    XDisplayImpl *dp = MWD(d);
    if (rotate == 0) return 1;
    rotate = ((rotate % 8) + 8) % 8;
    unsigned char *buf[8]; size_t len[8];
    for (int i = 0; i < 8; i++) { buf[i] = dp->cutbuf[i]; len[i] = dp->cutbuf_len[i]; }
    for (int i = 0; i < 8; i++) {
        dp->cutbuf[(i + rotate) % 8] = buf[i];
        dp->cutbuf_len[(i + rotate) % 8] = len[i];
    }
    return 1;
}
