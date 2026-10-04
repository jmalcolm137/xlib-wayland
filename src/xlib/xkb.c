/* xkb.c — the XKB API surface that clients and libxkbfile resolve against.
 *
 * Keycodes and keysyms come from the compositor's xkbcommon keymap (see
 * keymap.c); there is no server to carry keymap, indicator, controls or
 * geometry requests.  The extension is nevertheless reported present: the
 * keymap is real, and clients that ask for XKB -- setxkbmap to read the
 * layout, xterm for its keycode translation -- otherwise refuse to run at
 * all.  Requests that would change the keymap are simply not carried, and the
 * functions that would need a server report failure rather than pretending.
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

Bool XkbUseExtension(Display *dpy, int *major_rtrn, int *minor_rtrn)
{
    /* The client is asking whether the XKB extension is usable.  It is, to the
     * extent the shim implements it; report our version. */
    (void)dpy;
    if (major_rtrn) *major_rtrn = XkbMajorVersion;
    if (minor_rtrn) *minor_rtrn = XkbMinorVersion;
    return True;
}

Bool XkbQueryExtension(Display *dpy, int *opcodeReturn, int *eventBaseReturn,
                       int *errorBaseReturn, int *majorRtrn, int *minorRtrn)
{
    /* The extension is reported present: keycodes and keysyms do come from a
     * real xkbcommon keymap, and clients that ask for it -- setxkbmap to read
     * the layout, xterm for its indicators -- otherwise refuse to run at all.
     * The bases are in unused ranges so nothing is ever mistaken for an XKB
     * event, and requests that would change the keymap are not carried. */
    (void)dpy;
    if (opcodeReturn)    *opcodeReturn = 129;
    if (eventBaseReturn) *eventBaseReturn = 110;
    if (errorBaseReturn) *errorBaseReturn = 150;
    if (majorRtrn)       *majorRtrn = XkbMajorVersion;
    if (minorRtrn)       *minorRtrn = XkbMinorVersion;
    return True;
}

Display *XkbOpenDisplay(_Xconst char *display_name, int *event_rtrn,
                        int *error_rtrn, int *major_in_out, int *minor_in_out,
                        int *reason_rtrn)
{
    int major = major_in_out ? *major_in_out : XkbMajorVersion;
    int minor = minor_in_out ? *minor_in_out : XkbMinorVersion;
    Display *dpy = XOpenDisplay(display_name);
    if (!dpy) {
        if (reason_rtrn) *reason_rtrn = XkbOD_ConnectionRefused;
        return NULL;
    }
    if (!XkbLibraryVersion(&major, &minor)) {
        if (reason_rtrn) *reason_rtrn = XkbOD_BadLibraryVersion;
        XCloseDisplay(dpy);
        return NULL;
    }
    if (!XkbQueryExtension(dpy, NULL, event_rtrn, error_rtrn, &major, &minor)) {
        if (reason_rtrn) *reason_rtrn = XkbOD_NonXkbServer;
        XCloseDisplay(dpy);
        return NULL;
    }
    if (major_in_out) *major_in_out = major;
    if (minor_in_out) *minor_in_out = minor;
    if (reason_rtrn) *reason_rtrn = XkbOD_Success;
    return dpy;
}

/* Loading a keyboard from the XKB configuration files and uploading it is not
 * something an in-process shim can do; report failure rather than pretend. */
XkbDescPtr XkbGetKeyboardByName(Display *dpy, unsigned int deviceSpec,
                                XkbComponentNamesPtr names, unsigned int want,
                                unsigned int need, Bool load)
{
    (void)dpy; (void)deviceSpec; (void)names; (void)want; (void)need; (void)load;
    return NULL;
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

Bool XkbForceDeviceBell(Display *dpy, int deviceSpec, int bellClass,
                        int bellID, int percent)
{ (void)deviceSpec; (void)bellClass; (void)bellID; XBell(dpy, percent); return True; }

Bool XkbForceBell(Display *dpy, int percent)
{ XBell(dpy, percent); return True; }

Bool XkbDeviceBell(Display *dpy, Window win, int deviceSpec, int bellClass,
                   int bellID, int percent, Atom name)
{ (void)win; (void)deviceSpec; (void)bellClass; (void)bellID; (void)name;
  XBell(dpy, percent); return True; }

Bool XkbDeviceBellEvent(Display *dpy, Window win, int deviceSpec, int bellClass,
                        int bellID, int percent, Atom name)
{ (void)dpy; (void)win; (void)deviceSpec; (void)bellClass; (void)bellID;
  (void)percent; (void)name; return False; }

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

/* Build a valid client/server/names description from the compositor's
 * xkbcommon keymap.  There is no XKB wire protocol, so a client that asks for
 * the map (GDK's keyboard handling is the demanding one) would otherwise get a
 * descriptor whose map/server/names are NULL and crash dereferencing them.
 * The keysyms are real: they come from the same keymap XkbKeycodeToKeysym()
 * reads.  Modifier maps (server->vmods, names->vmods) are left zeroed, which
 * the core modifier state does not depend on. */
static Status xkb_build_map(Display *dpy, XkbDescPtr xkb, unsigned int deviceSpec)
{
    XDisplayImpl *dp = MWD(dpy);
    struct xkb_keymap *km = dp ? dp->xkb_keymap : NULL;

    KeyCode min = 8, max = 255;
    if (km) {
        KeyCode kmin = (KeyCode)xkb_keymap_min_keycode(km);
        KeyCode kmax = (KeyCode)xkb_keymap_max_keycode(km);
        if (kmin >= 8) min = kmin;
        if (kmax <= 255 && kmax >= min) max = kmax;
    }

    xkb->dpy = dpy;
    xkb->device_spec = (unsigned short)deviceSpec;
    xkb->min_key_code = min;
    xkb->max_key_code = max;

    /* Size the per-key arrays to the full X keycode range (0..255), not just
     * up to the xkbcommon keymap's maximum: clients iterate to the range
     * XDisplayKeycodes() reports, which can exceed it, and would read past the
     * end of a shorter array.  Keycodes outside [min,max] simply have a zeroed
     * entry (no groups, no symbols) and are skipped. */
    int nkeys = 256;

    XkbClientMapRec *map = calloc(1, sizeof *map);
    if (!map) return BadAlloc;
    xkb->map = map;
    map->key_sym_map = calloc((size_t)nkeys, sizeof(XkbSymMapRec));
    map->modmap      = calloc((size_t)nkeys, 1);
    map->size_types  = 1;
    map->num_types   = 1;
    map->types       = calloc(1, sizeof(XkbKeyTypeRec));
    if (!map->key_sym_map || !map->modmap || !map->types) return BadAlloc;
    /* One key type, no modifier-map entries: clients fall back to the plain
     * "symbol at (group,level)" lookup, which is what our keysyms model. */
    map->types[0].num_levels = XkbNumKbdGroups;
    map->types[0].map = NULL;
    map->types[0].map_count = 0;

    size_t nsyms = 0;
    for (KeyCode k = min; k <= max; k++) {
        int ng = 1, width = 1;
        if (km) {
            ng = xkb_keymap_num_layouts_for_key(km, k);
            if (ng < 1) ng = 1;
            if (ng > XkbNumKbdGroups) ng = XkbNumKbdGroups;
            for (int g = 0; g < ng; g++) {
                int nl = xkb_keymap_num_levels_for_key(km, k, g);
                if (nl > width) width = nl;
            }
        }
        if (width < 1) width = 1;
        if (width > 4) width = 4;
        nsyms += (size_t)ng * (size_t)width;
    }
    if (nsyms < 1) nsyms = 1;
    map->syms = calloc(nsyms, sizeof(KeySym));
    if (!map->syms) return BadAlloc;
    map->size_syms = map->num_syms = (unsigned short)nsyms;

    size_t off = 0;
    for (KeyCode k = min; k <= max; k++) {
        int ng = 1, width = 1;
        if (km) {
            ng = xkb_keymap_num_layouts_for_key(km, k);
            if (ng < 1) ng = 1;
            if (ng > XkbNumKbdGroups) ng = XkbNumKbdGroups;
            for (int g = 0; g < ng; g++) {
                int nl = xkb_keymap_num_levels_for_key(km, k, g);
                if (nl > width) width = nl;
            }
        }
        if (width < 1) width = 1;
        if (width > 4) width = 4;

        map->key_sym_map[k].width = (unsigned char)width;
        map->key_sym_map[k].group_info =
            (unsigned char)(((width & 0x03) << 4) | (ng & 0x0f));
        map->key_sym_map[k].offset = (unsigned short)off;
        for (int g = 0; g < XkbNumKbdGroups; g++)
            map->key_sym_map[k].kt_index[g] = 0;

        if (km) {
            for (int g = 0; g < ng; g++) {
                for (int l = 0; l < width; l++) {
                    const xkb_keysym_t *syms = NULL;
                    int n = xkb_keymap_key_get_syms_by_level(km, k, g, l, &syms);
                    map->syms[off + (size_t)g * width + l] =
                        (n > 0 && syms) ? (KeySym)syms[0] : NoSymbol;
                }
            }
        }
        off += (size_t)ng * (size_t)width;
    }

    if (!xkb->server) xkb->server = calloc(1, sizeof(XkbServerMapRec));
    if (!xkb->server) return BadAlloc;
    xkb->server->vmodmap = calloc((size_t)nkeys, sizeof(unsigned short));

    if (!xkb->names) xkb->names = calloc(1, sizeof(XkbNamesRec));
    if (!xkb->names) return BadAlloc;

    return Success;
}

XkbDescPtr XkbGetMap(Display *dpy, unsigned int which, unsigned int deviceSpec)
{
    (void)which;
    XkbDescPtr xkb = XkbAllocKeyboard();
    if (!xkb) return NULL;
    if (xkb_build_map(dpy, xkb, deviceSpec) != Success) {
        XkbFreeKeyboard(xkb, XkbAllComponentsMask, True);
        return NULL;
    }
    return xkb;
}

Status XkbGetUpdatedMap(Display *dpy, unsigned int which, XkbDescPtr desc)
{
    (void)which;
    if (!desc) return BadAlloc;
    if (desc->map && desc->server && desc->names) return Success;
    return xkb_build_map(dpy, desc, desc->device_spec);
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

/* ----------------------------------------------------------- state / controls
 *
 * GDK's X11 backend drives its keyboard handling through XKB: it registers for
 * state/map notifications with XkbSelectEvents, reads the current modifier and
 * group state with XkbGetState, reads the autorepeat controls with
 * XkbGetControls, and toggles detectable autorepeat with
 * XkbSetDetectableAutoRepeat.  These four were missing from the shim's XKB
 * surface; anything linking libgdk-x11-2.0 therefore failed to resolve them
 * (the first was gtk-query-immodules-2.0).
 *
 * There is no XKB wire protocol here, but the keymap is real (xkbcommon; see
 * keymap.c), so the state and controls have honest local answers.  Event
 * registration is accepted and not carried: XKB state/map *events* are not
 * synthesised, but every key event still carries its core modifier state, so a
 * client that falls back to reading XKeyEvent.state behaves correctly.
 */

Status XkbGetState(Display *dpy, unsigned int deviceSpec, XkbStatePtr rtrn)
{
    (void)deviceSpec;
    if (!rtrn) return BadValue;
    memset(rtrn, 0, sizeof *rtrn);

    XDisplayImpl *dp = MWD(dpy);
    if (dp && dp->xkb_state) {
        struct xkb_state *st = dp->xkb_state;

        xkb_mod_mask_t eff = xkb_state_serialize_mods(st, XKB_STATE_MODS_EFFECTIVE);
        xkb_mod_mask_t dep = xkb_state_serialize_mods(st, XKB_STATE_MODS_DEPRESSED);
        xkb_mod_mask_t lat = xkb_state_serialize_mods(st, XKB_STATE_MODS_LATCHED);
        xkb_mod_mask_t lck = xkb_state_serialize_mods(st, XKB_STATE_MODS_LOCKED);
        xkb_layout_index_t grp  =
            xkb_state_serialize_layout(st, XKB_STATE_LAYOUT_EFFECTIVE);
        xkb_layout_index_t lgrp =
            xkb_state_serialize_layout(st, XKB_STATE_LAYOUT_LOCKED);

        rtrn->group             = (unsigned char)grp;
        rtrn->base_group        = (unsigned short)grp;
        rtrn->locked_group      = (unsigned char)lgrp;
        rtrn->mods              = (unsigned char)eff;
        rtrn->base_mods         = (unsigned char)dep;
        rtrn->latched_mods      = (unsigned char)lat;
        rtrn->locked_mods       = (unsigned char)lck;
        rtrn->compat_state      = (unsigned char)eff;
        rtrn->grab_mods         = (unsigned char)eff;
        rtrn->compat_grab_mods  = (unsigned char)eff;
        rtrn->lookup_mods       = (unsigned char)eff;
        rtrn->compat_lookup_mods= (unsigned char)eff;
    }
    return Success;
}

Status XkbGetControls(Display *dpy, unsigned long which, XkbDescPtr desc)
{
    (void)dpy; (void)which;
    if (!desc) return BadAlloc;
    if (!desc->ctrls) {
        desc->ctrls = calloc(1, sizeof(XkbControlsRec));
        if (!desc->ctrls) return BadAlloc;
    }
    /* Keyboard autorepeat is handled by the shim itself (it synthesises
     * KeyRelease events on the repeat deadline and already honours the CDE
     * Style Manager's toggle).  Report it enabled so GDK does not install a
     * competing repeat timer. */
    desc->ctrls->enabled_ctrls   = XkbRepeatKeysMask;
    desc->ctrls->repeat_delay    = 660;
    desc->ctrls->repeat_interval = 25;
    desc->ctrls->num_groups      = 1;
    return Success;
}

Bool XkbSelectEvents(Display *dpy, unsigned int deviceSpec,
                     unsigned int affect, unsigned int values)
{
    (void)dpy; (void)deviceSpec; (void)affect; (void)values;
    /* Accepted.  XKB notifications are not synthesised; core key events carry
     * the modifier state. */
    return True;
}

Bool XkbSelectEventDetails(Display *dpy, unsigned int deviceSpec,
                           unsigned int eventType, unsigned long affect,
                           unsigned long details)
{
    (void)dpy; (void)deviceSpec; (void)eventType; (void)affect; (void)details;
    return True;
}

Bool XkbSetDetectableAutoRepeat(Display *dpy, Bool detectable,
                                Bool *supported_rtrn)
{
    (void)dpy; (void)detectable;
    /* The shim already reports press/release pairs and do/do-not-repeat via
     * the core protocol, so detectable autorepeat is supported. */
    if (supported_rtrn) *supported_rtrn = True;
    return True;
}

void XkbFreeKeyboard(XkbDescPtr xkb, unsigned int which, Bool freeDesc)
{
    (void)which;
    if (!xkb) return;
    if (xkb->map) {
        free(xkb->map->types);
        free(xkb->map->syms);
        free(xkb->map->key_sym_map);
        free(xkb->map->modmap);
        free(xkb->map);
    }
    if (xkb->server) {
        free(xkb->server->acts);
        free(xkb->server->behaviors);
        free(xkb->server->key_acts);
        free(xkb->server->explicit);
        free(xkb->server->vmodmap);
        free(xkb->server);
    }
    if (xkb->names) {
        free(xkb->names->keys);
        free(xkb->names->key_aliases);
        free(xkb->names->radio_groups);
        free(xkb->names);
    }
    free(xkb->compat);
    free(xkb->indicators);
    free(xkb->ctrls);
    free(xkb->geom);
    if (freeDesc) free(xkb);
}

/* ------------------------------------------------ keymap control / queries
 *
 * The broader slice of XKB that libmatekbd (through libxklavier) uses.  Getters
 * answer from the real keymap; requests that would change server state are
 * accepted and not carried, matching the rest of the XKB surface. */

XkbDescPtr XkbGetKeyboard(Display *dpy, unsigned int which, unsigned int deviceSpec)
{
    return XkbGetMap(dpy, which, deviceSpec);
}

unsigned int XkbKeysymToModifiers(Display *dpy, KeySym ks)
{
    /* No modifier-key mapping is modelled; callers fall back to the core
     * modifier map. */
    (void)dpy; (void)ks;
    return 0;
}

Bool XkbTranslateKeyCode(XkbDescPtr xkb, KeyCode keycode, unsigned int modifiers,
                         unsigned int *modifiers_return, KeySym *keysym_return)
{
    if (modifiers_return) *modifiers_return = modifiers;
    if (!xkb || !XkbKeycodeInRange(xkb, keycode)) {
        if (keysym_return) *keysym_return = NoSymbol;
        return False;
    }
    int ng = XkbKeyNumGroups(xkb, keycode);
    if (ng < 1) { if (keysym_return) *keysym_return = NoSymbol; return False; }
    int width = XkbKeyGroupsWidth(xkb, keycode);
    if (width < 1) width = 1;
    int group = XkbGroupForCoreState(modifiers);
    if (group >= ng) group %= ng;
    int level = (modifiers & ShiftMask) ? 1 : 0;   /* core shift -> level 1 */
    if (level >= width) level = 0;
    KeySym ks = XkbKeySymEntry(xkb, keycode, level, group);
    if (keysym_return) *keysym_return = ks;
    return ks != NoSymbol;
}

Status XkbGetIndicatorState(Display *dpy, unsigned int deviceSpec,
                            unsigned int *pStateRtrn)
{
    (void)dpy; (void)deviceSpec;
    if (pStateRtrn) *pStateRtrn = 0;
    return Success;
}

Bool XkbLockGroup(Display *dpy, unsigned int deviceSpec, unsigned int group)
{ (void)dpy; (void)deviceSpec; (void)group; return True; }

Bool XkbLatchGroup(Display *dpy, unsigned int deviceSpec, unsigned int group)
{ (void)dpy; (void)deviceSpec; (void)group; return True; }

Bool XkbLockModifiers(Display *dpy, unsigned int deviceSpec,
                      unsigned int affect, unsigned int values)
{ (void)dpy; (void)deviceSpec; (void)affect; (void)values; return True; }

Bool XkbLatchModifiers(Display *dpy, unsigned int deviceSpec,
                       unsigned int affect, unsigned int values)
{ (void)dpy; (void)deviceSpec; (void)affect; (void)values; return True; }

Bool XkbSetControls(Display *dpy, unsigned long which, XkbDescPtr desc)
{ (void)dpy; (void)which; (void)desc; return True; }

Bool XkbChangeEnabledControls(Display *dpy, unsigned int deviceSpec,
                              unsigned int affect, unsigned int values)
{ (void)dpy; (void)deviceSpec; (void)affect; (void)values; return True; }
