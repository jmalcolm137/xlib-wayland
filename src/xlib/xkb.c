/* xkb.c — the XKB API surface that clients and libxkbfile resolve against.
 *
 * The shim has no XKB protocol.  Keycodes and keysyms come from the
 * compositor's xkbcommon keymap (see keymap.c); there is no server to carry
 * keymap, indicator, controls or geometry requests.  XkbQueryExtension()
 * therefore reports the extension as absent and clients fall back to core X11
 * -- a path every one of them already has.  Advertising an extension whose
 * requests go nowhere would be worse than not having it.
 *
 * The functions below still have to exist, because anything that links them
 * must resolve them at load time: xterm references the indicator and keysym
 * entry points directly, and xclock links libxkbfile, which references the
 * whole allocator family.  Where a function has a sensible local meaning it is
 * implemented (keysym lookup, the bell, virtual-modifier translation,
 * allocation); where it needs a server it reports failure rather than
 * pretending a request was answered.
 */
#include "internal.h"

#include <X11/XKBlib.h>
#include <X11/extensions/XKBgeom.h>
#include <stdlib.h>
#include <string.h>

/* ---------------------------------------------------------- negotiation */

Bool XkbLibraryVersion(int *libMajorRtrn, int *libMinorRtrn)
{
    /* The caller passes the version it was compiled against and expects the
     * library to confirm compatibility; we are that library. */
    if (libMajorRtrn) *libMajorRtrn = XkbMajorVersion;
    if (libMinorRtrn) *libMinorRtrn = XkbMinorVersion;
    return True;
}

Bool XkbQueryExtension(Display *dpy, int *opcodeReturn, int *eventBaseReturn,
                       int *errorBaseReturn, int *majorRtrn, int *minorRtrn)
{
    (void)dpy;
    if (opcodeReturn)    *opcodeReturn = 0;
    if (eventBaseReturn) *eventBaseReturn = 0;
    if (errorBaseReturn) *errorBaseReturn = 0;
    if (majorRtrn)       *majorRtrn = XkbMajorVersion;
    if (minorRtrn)       *minorRtrn = XkbMinorVersion;
    return False;
}

/* ------------------------------------------------------------- keysyms */

KeySym XkbKeycodeToKeysym(Display *dpy, KeyCode kc, int group, int level)
{
    XDisplayImpl *dp = MWD(dpy);
    if (!dp->xkb_keymap) return NoSymbol;
    const xkb_keysym_t *syms = NULL;
    int n = xkb_keymap_key_get_syms_by_level(dp->xkb_keymap, (xkb_keycode_t)kc,
                                             (xkb_layout_index_t)group,
                                             (xkb_level_index_t)level, &syms);
    return n > 0 ? (KeySym)syms[0] : NoSymbol;
}

Bool XkbVirtualModsToReal(XkbDescPtr xkb, unsigned int virtual_mask,
                          unsigned int *mask_rtrn)
{
    if (!xkb || !xkb->server || !mask_rtrn) return False;
    *mask_rtrn = 0;
    for (int i = 0; i < XkbNumVirtualMods; i++)
        if (virtual_mask & (1u << i))
            *mask_rtrn |= xkb->server->vmods[i];
    return True;
}

/* --------------------------------------------------------------- bell */

Bool XkbBell(Display *dpy, Window win, int percent, Atom name)
{
    (void)win; (void)name;
    XBell(dpy, percent);
    return True;
}

Bool XkbBellEvent(Display *dpy, Window win, int percent, Atom name)
{
    (void)win; (void)name;
    XBell(dpy, percent);
    /* The BellNotify event is part of the extension we do not carry. */
    return False;
}

/* --------------------------------------------------------- indicators */

Bool XkbGetNamedIndicator(Display *dpy, Atom name, int *pNdxRtrn,
                          Bool *pStateRtrn, XkbIndicatorMapPtr pMapRtrn,
                          Bool *pRealRtrn)
{
    (void)dpy; (void)name;
    /* No indicator maps: tell the caller the indicator is unknown so it can
     * carry on (xterm simply will not reflect a LED). */
    if (pNdxRtrn)   *pNdxRtrn = -1;
    if (pStateRtrn) *pStateRtrn = False;
    if (pMapRtrn)   memset(pMapRtrn, 0, sizeof *pMapRtrn);
    if (pRealRtrn)  *pRealRtrn = False;
    return False;
}

Bool XkbSetNamedIndicator(Display *dpy, Atom name, Bool changeState, Bool state,
                          Bool createNewMap, XkbIndicatorMapPtr pMap)
{
    (void)dpy; (void)name; (void)changeState; (void)state; (void)createNewMap;
    (void)pMap;
    return False;
}

/* -------------------------------------------------------- allocation */

XkbDescPtr XkbAllocKeyboard(void)
{
    return calloc(1, sizeof(XkbDescRec));
}

Status XkbAllocClientMap(XkbDescPtr xkb, unsigned int which, unsigned int nTypes)
{
    (void)which; (void)nTypes;
    if (!xkb) return BadAlloc;
    if (!xkb->map) xkb->map = calloc(1, sizeof(XkbClientMapRec));
    return xkb->map ? Success : BadAlloc;
}

Status XkbAllocServerMap(XkbDescPtr xkb, unsigned int which, unsigned int nActions)
{
    (void)which; (void)nActions;
    if (!xkb) return BadAlloc;
    if (!xkb->server) xkb->server = calloc(1, sizeof(XkbServerMapRec));
    return xkb->server ? Success : BadAlloc;
}

Status XkbAllocCompatMap(XkbDescPtr xkb, unsigned int which, unsigned int nInterpret)
{
    (void)which; (void)nInterpret;
    if (!xkb) return BadAlloc;
    if (!xkb->compat) xkb->compat = calloc(1, sizeof(XkbCompatMapRec));
    return xkb->compat ? Success : BadAlloc;
}

Status XkbAllocIndicatorMaps(XkbDescPtr xkb)
{
    if (!xkb) return BadAlloc;
    if (!xkb->indicators) xkb->indicators = calloc(1, sizeof(XkbIndicatorRec));
    return xkb->indicators ? Success : BadAlloc;
}

Status XkbAllocControls(XkbDescPtr xkb, unsigned int which)
{
    (void)which;
    if (!xkb) return BadAlloc;
    if (!xkb->ctrls) xkb->ctrls = calloc(1, sizeof(XkbControlsRec));
    return xkb->ctrls ? Success : BadAlloc;
}

Status XkbAllocNames(XkbDescPtr xkb, unsigned int which, int nTotalRG,
                     int nTotalAliases)
{
    (void)which; (void)nTotalRG; (void)nTotalAliases;
    if (!xkb) return BadAlloc;
    if (!xkb->names) xkb->names = calloc(1, sizeof(XkbNamesRec));
    return xkb->names ? Success : BadAlloc;
}

Status XkbAllocGeometry(XkbDescPtr xkb, XkbGeometrySizesPtr sizes)
{
    (void)sizes;
    if (!xkb) return BadAlloc;
    if (!xkb->geom) xkb->geom = calloc(1, sizeof(XkbGeometryRec));
    return xkb->geom ? Success : BadAlloc;
}

/* Geometry is not modelled: the shim never loads a keyboard description, so
 * these are unreachable in practice and returning NULL is the documented
 * "could not allocate" result. */
XkbColorPtr     XkbAddGeomColor(XkbGeometryPtr geom, _Xconst char *spec, unsigned int pixel)
{ (void)geom; (void)spec; (void)pixel; return NULL; }
XkbDoodadPtr    XkbAddGeomDoodad(XkbGeometryPtr geom, XkbSectionPtr section, Atom name)
{ (void)geom; (void)section; (void)name; return NULL; }
XkbKeyPtr       XkbAddGeomKey(XkbRowPtr row)
{ (void)row; return NULL; }
XkbOutlinePtr   XkbAddGeomOutline(XkbShapePtr shape, int sz_points)
{ (void)shape; (void)sz_points; return NULL; }
XkbOverlayPtr   XkbAddGeomOverlay(XkbSectionPtr section, Atom name, int sz_rows)
{ (void)section; (void)name; (void)sz_rows; return NULL; }
XkbOverlayRowPtr XkbAddGeomOverlayRow(XkbOverlayPtr overlay, int row_under, int sz_keys)
{ (void)overlay; (void)row_under; (void)sz_keys; return NULL; }
XkbPropertyPtr  XkbAddGeomProperty(XkbGeometryPtr geom, _Xconst char *name, _Xconst char *value)
{ (void)geom; (void)name; (void)value; return NULL; }
XkbRowPtr       XkbAddGeomRow(XkbSectionPtr section, int sz_keys)
{ (void)section; (void)sz_keys; return NULL; }
XkbSectionPtr   XkbAddGeomSection(XkbGeometryPtr geom, Atom name, int sz_rows,
                                  int sz_doodads, int sz_overlays)
{ (void)geom; (void)name; (void)sz_rows; (void)sz_doodads; (void)sz_overlays; return NULL; }
XkbShapePtr     XkbAddGeomShape(XkbGeometryPtr geom, Atom name, int sz_outlines)
{ (void)geom; (void)name; (void)sz_outlines; return NULL; }

/* ----------------------------------------------------- map / names I/O */

XkbDescPtr XkbGetMap(Display *dpy, unsigned int which, unsigned int deviceSpec)
{
    (void)dpy; (void)which;
    XkbDescPtr xkb = XkbAllocKeyboard();
    if (xkb) xkb->device_spec = (unsigned short)deviceSpec;
    return xkb;
}

Status XkbGetUpdatedMap(Display *dpy, unsigned int which, XkbDescPtr desc)
{
    (void)dpy; (void)which;
    if (!desc) return BadAlloc;
    return XkbAllocClientMap(desc, 0, 0);
}

Status XkbGetNames(Display *dpy, unsigned int which, XkbDescPtr desc)
{
    (void)dpy; (void)which;
    if (!desc) return BadAlloc;
    return XkbAllocNames(desc, 0, 0, 0);
}

Status XkbGetCompatMap(Display *dpy, unsigned int which, XkbDescPtr xkb)
{
    (void)dpy; (void)which;
    if (!xkb) return BadAlloc;
    return XkbAllocCompatMap(xkb, 0, 0);
}

Status XkbGetGeometry(Display *dpy, XkbDescPtr xkb)
{
    (void)dpy;
    if (!xkb) return BadAlloc;
    return XkbAllocGeometry(xkb, NULL);
}

Status XkbGetIndicatorMap(Display *dpy, unsigned long which, XkbDescPtr desc)
{
    (void)dpy; (void)which;
    if (!desc) return BadAlloc;
    return XkbAllocIndicatorMaps(desc);
}

Bool XkbSetMap(Display *dpy, unsigned int which, XkbDescPtr desc)
{ (void)dpy; (void)which; (void)desc; return False; }

Bool XkbSetNames(Display *dpy, unsigned int which, unsigned int firstType,
                 unsigned int nTypes, XkbDescPtr desc)
{ (void)dpy; (void)which; (void)firstType; (void)nTypes; (void)desc; return False; }

Bool XkbSetCompatMap(Display *dpy, unsigned int which, XkbDescPtr xkb,
                     Bool updateActions)
{ (void)dpy; (void)which; (void)xkb; (void)updateActions; return False; }

Status XkbSetGeometry(Display *dpy, unsigned deviceSpec, XkbGeometryPtr geom)
{ (void)dpy; (void)deviceSpec; (void)geom; return BadAlloc; }

Bool XkbSetIndicatorMap(Display *dpy, unsigned long which, XkbDescPtr desc)
{ (void)dpy; (void)which; (void)desc; return False; }

KeySym *XkbResizeKeySyms(XkbDescPtr desc, int forKey, int symsNeeded)
{ (void)desc; (void)forKey; (void)symsNeeded; return NULL; }

XkbAction *XkbResizeKeyActions(XkbDescPtr desc, int forKey, int actsNeeded)
{ (void)desc; (void)forKey; (void)actsNeeded; return NULL; }
