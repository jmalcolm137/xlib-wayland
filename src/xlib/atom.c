/* atom.c — atoms, window properties, ICCCM WM hints, text properties. */
#include "internal.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/file.h>

/* Predefined atoms 1..68, indexed by value (see X11/Xatom.h). */
static const char *predef[] = {
    NULL,
    "PRIMARY", "SECONDARY", "ARC", "ATOM", "BITMAP", "CARDINAL", "COLORMAP",
    "CURSOR", "CUT_BUFFER0", "CUT_BUFFER1", "CUT_BUFFER2", "CUT_BUFFER3",
    "CUT_BUFFER4", "CUT_BUFFER5", "CUT_BUFFER6", "CUT_BUFFER7", "DRAWABLE",
    "FONT", "INTEGER", "PIXMAP", "POINT", "RECTANGLE", "RESOURCE_MANAGER",
    "RGB_COLOR_MAP", "RGB_BEST_MAP", "RGB_BLUE_MAP", "RGB_DEFAULT_MAP",
    "RGB_GRAY_MAP", "RGB_GREEN_MAP", "RGB_RED_MAP", "STRING", "VISUALID",
    "WINDOW", "WM_COMMAND", "WM_HINTS", "WM_CLIENT_MACHINE", "WM_ICON_NAME",
    "WM_ICON_SIZE", "WM_NAME", "WM_NORMAL_HINTS", "WM_SIZE_HINTS",
    "WM_ZOOM_HINTS", "MIN_SPACE", "NORM_SPACE", "MAX_SPACE", "END_SPACE",
    "SUPERSCRIPT_X", "SUPERSCRIPT_Y", "SUBSCRIPT_X", "SUBSCRIPT_Y",
    "UNDERLINE_POSITION", "UNDERLINE_THICKNESS", "STRIKEOUT_ASCENT",
    "STRIKEOUT_DESCENT", "ITALIC_ANGLE", "X_HEIGHT", "QUAD_WIDTH", "WEIGHT",
    "POINT_SIZE", "RESOLUTION", "COPYRIGHT", "NOTICE", "FONT_NAME",
    "FAMILY_NAME", "FULL_NAME", "CAP_HEIGHT", "WM_CLASS", "WM_TRANSIENT_FOR"
};

/* Custom atoms must be stable across processes: the shim gives every client its
 * own private server, so an atom id that a client receives (e.g. from a
 * workspace list) then passes to another process (dtwm, say) has to mean the
 * same name there.  A per-process sequential table makes "Three" 0x47 here and
 * "Custom Data" there.  Share a name<->id table in the runtime directory so
 * every client agrees. */
static const char *atom_file_path(void)
{
    static char path[1024];
    const char *p = getenv("XLIB_WAYLAND_ATOMS");
    if (p && *p) return p;
    const char *rt = getenv("XDG_RUNTIME_DIR");
    if (!rt || !*rt) return NULL;
    snprintf(path, sizeof path, "%s/xlib-wayland-atoms", rt);
    return path;
}

static void atom_table_ensure(Display *d, int id)
{
    XDisplayImpl *dp = MWD(d);
    if (id < dp->atoms_cap) return;
    int cap = dp->atoms_cap > 0 ? dp->atoms_cap : 512;
    while (id >= cap) cap *= 2;
    dp->atom_names = realloc(dp->atom_names, (size_t)cap * sizeof(char *));
    for (int i = dp->atoms_cap; i < cap; i++) dp->atom_names[i] = NULL;
    dp->atoms_cap = cap;
}

static void atom_file_load(Display *d)
{
    const char *path = atom_file_path();
    if (!path) return;
    FILE *f = fopen(path, "r");
    if (!f) return;
    XDisplayImpl *dp = MWD(d);
    char line[1024];
    while (fgets(line, sizeof line, f)) {
        char *sp = strchr(line, ' ');
        if (!sp) continue;
        *sp = 0;
        long id = atol(line);
        char *nm = sp + 1;
        nm[strcspn(nm, "\r\n")] = 0;
        if (id <= XA_LAST_PREDEFINED || !*nm) continue;
        atom_table_ensure(d, (int)id);
        if (!dp->atom_names[id]) dp->atom_names[id] = strdup(nm);
        if ((int)id >= dp->natoms) dp->natoms = (int)id + 1;
    }
    fclose(f);
}

/* Return the shared id for a name, or 0 if it has none.  */
static int atom_file_lookup(const char *name)
{
    const char *path = atom_file_path();
    if (!path) return 0;
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    char line[1024];
    int found = 0;
    while (fgets(line, sizeof line, f)) {
        char *sp = strchr(line, ' ');
        if (!sp) continue;
        *sp = 0;
        long id = atol(line);
        char *nm = sp + 1;
        nm[strcspn(nm, "\r\n")] = 0;
        if (strcmp(nm, name) == 0) { found = (int)id; break; }
    }
    fclose(f);
    return found;
}

/* Allocate the shared id for a name (adding it if new).  Returns 0 on failure
 * (e.g. no runtime dir), in which case the caller falls back to a local id. */
static int atom_file_add(const char *name)
{
    const char *path = atom_file_path();
    if (!path) return 0;
    int fd = open(path, O_RDWR | O_CREAT, 0600);
    if (fd < 0) return 0;
    flock(fd, LOCK_EX);
    FILE *f = fdopen(fd, "r+");
    if (!f) { close(fd); return 0; }
    char line[1024];
    int maxid = XA_LAST_PREDEFINED, found = 0;
    while (fgets(line, sizeof line, f)) {
        char *sp = strchr(line, ' ');
        if (!sp) continue;
        *sp = 0;
        long id = atol(line);
        char *nm = sp + 1;
        nm[strcspn(nm, "\r\n")] = 0;
        if (id > maxid) maxid = (int)id;
        if (strcmp(nm, name) == 0) { found = (int)id; break; }
    }
    if (!found) {
        found = maxid + 1;
        fseek(f, 0, SEEK_END);
        fprintf(f, "%d %s\n", found, name);
        fflush(f);
        fsync(fileno(f));
    }
    fclose(f);   /* releases the flock */
    return found;
}

void mw_init_atoms(Display *d)
{
    XDisplayImpl *dp = MWD(d);
    dp->atoms_cap = 512;
    dp->atom_names = calloc(dp->atoms_cap, sizeof(char *));
    for (int i = 1; i <= XA_LAST_PREDEFINED; i++)
        dp->atom_names[i] = strdup(predef[i] ? predef[i] : "");
    dp->natoms = XA_LAST_PREDEFINED + 1;
    atom_file_load(d);
}

Atom mw_intern_atom(Display *d, const char *name, Bool only_if_exists)
{
    XDisplayImpl *dp = MWD(d);
    if (!name) return None;
    for (int i = 1; i < dp->natoms; i++)
        if (dp->atom_names[i] && strcmp(dp->atom_names[i], name) == 0)
            return (Atom)i;

    /* Not known locally: consult the shared table so all clients agree. */
    {
        int gid = atom_file_lookup(name);
        if (!gid && !only_if_exists) gid = atom_file_add(name);
        if (gid) {
            atom_table_ensure(d, gid);
            if (!dp->atom_names[gid]) dp->atom_names[gid] = strdup(name);
            if (gid >= dp->natoms) dp->natoms = gid + 1;
            return (Atom)gid;
        }
    }
    if (only_if_exists) return None;
    if (dp->natoms >= dp->atoms_cap) {
        dp->atoms_cap *= 2;
        dp->atom_names = realloc(dp->atom_names, dp->atoms_cap * sizeof(char *));
        for (int i = dp->natoms; i < dp->atoms_cap; i++) dp->atom_names[i] = NULL;
    }
    int id = dp->natoms++;
    dp->atom_names[id] = strdup(name);
    return (Atom)id;
}

/* Resolve an id added by another process after this one loaded the table. */
static char *atom_file_lookup_id(int id)
{
    const char *path = atom_file_path();
    if (!path) return NULL;
    FILE *f = fopen(path, "r");
    if (!f) return NULL;
    char line[1024];
    char *result = NULL;
    while (fgets(line, sizeof line, f)) {
        char *sp = strchr(line, ' ');
        if (!sp) continue;
        *sp = 0;
        if (atoi(line) == id) {
            char *nm = sp + 1;
            nm[strcspn(nm, "\r\n")] = 0;
            result = strdup(nm);
            break;
        }
    }
    fclose(f);
    return result;
}

char *mw_atom_name(Display *d, Atom a)
{
    XDisplayImpl *dp = MWD(d);
    if ((int)a <= 0) return NULL;
    if ((int)a < dp->natoms && dp->atom_names[a]) return dp->atom_names[a];
    if ((int)a > XA_LAST_PREDEFINED) {
        char *nm = atom_file_lookup_id((int)a);
        if (nm) {
            atom_table_ensure(d, (int)a);
            if (!dp->atom_names[a]) dp->atom_names[a] = nm;
            else free(nm);
            if ((int)a >= dp->natoms) dp->natoms = (int)a + 1;
            return dp->atom_names[a];
        }
    }
    return NULL;
}

Atom XInternAtom(Display *d, _Xconst char *name, Bool only_if_exists)
{ return mw_intern_atom(d, name, only_if_exists); }

Status XInternAtoms(Display *d, char **names, int count, Bool only_if_exists,
                    Atom *atoms_return)
{
    for (int i = 0; i < count; i++)
        atoms_return[i] = mw_intern_atom(d, names[i], only_if_exists);
    (void)atoms_return;
    return 1;
}

char *XGetAtomName(Display *d, Atom a)
{
    char *n = mw_atom_name(d, a);
    return n ? strdup(n) : NULL;
}

Status XGetAtomNames(Display *d, Atom *atoms, int count, char **names_return)
{
    for (int i = 0; i < count; i++) {
        char *n = mw_atom_name(d, atoms[i]);
        if (!n) return 0;
        names_return[i] = strdup(n);
    }
    return 1;
}

/* ------------------------------------------------------------- properties */

MwProp *mw_get_prop(Display *d, MwWindow *win, Atom a)
{
    (void)d;
    for (MwProp *p = win->props; p; p = p->next)
        if (p->atom == a) return p;
    return NULL;
}

int mw_set_prop(Display *d, MwWindow *win, Atom a, Atom type, int format,
                const unsigned char *data, unsigned long nitems)
{
    MwProp *p = mw_get_prop(d, win, a);
    if (p) {
        free(p->data); p->data = NULL;
    } else {
        p = calloc(1, sizeof *p);
        p->atom = a;
        p->next = win->props;
        win->props = p;
    }
    p->type = type;
    p->format = format;
    p->nitems = nitems;
    size_t unit = format == 8 ? 1 : format == 16 ? 2 : 4;
    p->nbytes = nitems * unit;
    if (data && p->nbytes) {
        p->data = malloc(p->nbytes);
        memcpy(p->data, data, p->nbytes);
    }
    return 1;
}

int mw_del_prop(Display *d, MwWindow *win, Atom a)
{
    MwProp **pp = &win->props;
    while (*pp) {
        if ((*pp)->atom == a) {
            MwProp *dead = *pp;
            *pp = dead->next;
            free(dead->data);
            free(dead);
            (void)d;
            return 1;
        }
        pp = &(*pp)->next;
    }
    return 0;
}

static void prop_notify(Display *d, MwWindow *win, Atom a, int state)
{
    if (win->event_mask & PropertyChangeMask) {
        XPropertyEvent pe;
        memset(&pe, 0, sizeof pe);
        pe.type = PropertyNotify; pe.display = d; pe.window = win->id;
        pe.atom = a; pe.state = state; pe.time = mw_now();
        mw_put_event(d, (XEvent *)&pe);
    }
}

int XChangeProperty(Display *d, Window w, Atom property, Atom type, int format,
                    int mode, _Xconst unsigned char *data, int nelements)
{
    MwWindow *win = mw_window(d, w);
    if (!win) return 0;

    /* On a 64-bit server, format-32 property data is an array of longs: the
     * protocol carries only the low 32 bits of each one.  libX11's _XData32()
     * reads one long per element, which is why its internal Xatomtype.h warns
     * "All fields must be longs as the semantics of property routines will
     * handle conversion to and from actual 32 bit objects".  Copying the raw
     * bytes instead truncated the value and, worse, packed the two halves of
     * the first long into two elements -- Motif's clipboard lock record
     * {Window windowId; long lockLevel;} was stored as [windowId_low,
     * windowId_high], so the locked window looked like window 0 and Cut could
     * never take the clipboard lock. */
    unsigned char *packed = NULL;
    if (format == 32 && data && nelements > 0) {
        packed = malloc((size_t)nelements * 4);
        const long *src = (const long *)data;
        for (int i = 0; i < nelements; i++) {
            uint32_t v = (uint32_t)(unsigned long)src[i];
            memcpy(packed + (size_t)i * 4, &v, 4);
        }
        data = packed;
    }

    if (mode == PropModeReplace) {
        mw_set_prop(d, win, property, type, format, data, nelements);
    } else if (mode == PropModePrepend || mode == PropModeAppend) {
        MwProp *old = mw_get_prop(d, win, property);
        size_t unit = format == 8 ? 1 : format == 16 ? 2 : 4;
        unsigned long oldn = old ? old->nitems : 0;
        unsigned char *buf = malloc((oldn + nelements) * unit);
        if (mode == PropModePrepend) {
            if (data) memcpy(buf, data, nelements * unit);
            if (old && old->data) memcpy(buf + nelements * unit, old->data, oldn * unit);
        } else {
            if (old && old->data) memcpy(buf, old->data, oldn * unit);
            if (data) memcpy(buf + oldn * unit, data, nelements * unit);
        }
        mw_set_prop(d, win, property, type, format, buf, oldn + nelements);
        free(buf);
    }
    /* If this is the broker's proxy requestor window, remember what the
     * selection converter wrote; the broker forwards it to the remote client
     * (packed is still live here). */
    mw_broker_capture_prop(d, w, property, type, format, data, (unsigned long)nelements);
    free(packed);
    prop_notify(d, win, property, PropertyNewValue);
    mw_window_props_changed(d, win, property);
    return 1;
}

int XGetWindowProperty(Display *d, Window w, Atom property, long long_offset,
                       long long_length, Bool delete, Atom req_type,
                       Atom *actual_type_return, int *actual_format_return,
                       unsigned long *nitems_return, unsigned long *bytes_after_return,
                       unsigned char **prop_return)
{
    MwWindow *win = mw_window(d, w);
    if (!win) return BadWindow;
    /* The Workspace Manager's state does not live in this process: refresh the
     * synthetic WM window's workspace properties from the WSM bridge's state
     * file on every query, so a client that started earlier sees workspace
     * changes too, instead of the snapshot taken when it connected. */
    if (property == mw_intern_atom(d, "_DT_WORKSPACE_LIST", False) ||
        property == mw_intern_atom(d, "_DT_WORKSPACE_CURRENT", False))
        mw_refresh_workspace_props(d, win);
    MwProp *p = mw_get_prop(d, win, property);
    if (!p) {
        if (actual_type_return) *actual_type_return = None;
        if (actual_format_return) *actual_format_return = 0;
        if (nitems_return) *nitems_return = 0;
        if (bytes_after_return) *bytes_after_return = 0;
        if (prop_return) *prop_return = NULL;
        return Success;
    }
    if (req_type != AnyPropertyType && req_type != p->type) {
        if (actual_type_return) *actual_type_return = p->type;
        if (actual_format_return) *actual_format_return = p->format;
        if (nitems_return) *nitems_return = 0;
        if (bytes_after_return) *bytes_after_return = (unsigned long)p->nbytes;
        if (prop_return) *prop_return = NULL;
        return Success;
    }
    size_t unit = p->format == 8 ? 1 : p->format == 16 ? 2 : 4;
    /* long_offset and long_length are in 32-bit units per the X protocol. */
    long off_bytes = long_offset * 4;
    unsigned long start = off_bytes >= (long)p->nbytes ? p->nitems
                                                       : (unsigned long)(off_bytes / unit);
    unsigned long avail = p->nitems > start ? p->nitems - start : 0;
    unsigned long max_items = (long_length > 0)
        ? (unsigned long)long_length * 4 / unit
        : avail;
    unsigned long nret = avail < max_items ? avail : max_items;
    unsigned char *out = NULL;
    if (nret > 0 && prop_return) {
        if (p->format == 32) {
            /* Format-32 data is handed back as a long array, one long per
             * 32-bit value, mirroring libX11 on a 64-bit server. */
            long *lout = malloc((size_t)nret * sizeof(long));
            for (unsigned long i = 0; i < nret; i++) {
                uint32_t v;
                memcpy(&v, p->data + (start + i) * 4, 4);
                lout[i] = (long)v;
            }
            out = (unsigned char *)lout;
        } else {
            /* libX11 zero-pads the returned buffer to a 4-byte boundary, and
             * string properties are set with their trailing NUL included (a
             * window manager's _MOTIF_BINDINGS, for instance).  Allocate one
             * extra unit and zero it so a client that treats an 8-bit property
             * as a C string (Motif's _XmVirtKeysInitialize) cannot read past
             * the buffer. */
            out = calloc(1, nret * unit + unit);
            memcpy(out, p->data + start * unit, nret * unit);
        }
    }
    if (actual_type_return) *actual_type_return = p->type;
    if (actual_format_return) *actual_format_return = p->format;
    if (nitems_return) *nitems_return = nret;
    if (bytes_after_return) *bytes_after_return = (unsigned long)((avail - nret) * unit);
    if (prop_return) *prop_return = out;
    if (delete) mw_del_prop(d, win, property);
    return Success;
}

int XDeleteProperty(Display *d, Window w, Atom property)
{
    MwWindow *win = mw_window(d, w);
    if (!win) return 0;
    if (mw_del_prop(d, win, property))
        prop_notify(d, win, property, PropertyDelete);
    return 1;
}

Atom *XListProperties(Display *d, Window w, int *num_prop_return)
{
    MwWindow *win = mw_window(d, w);
    if (!win) { if (num_prop_return) *num_prop_return = 0; return NULL; }
    int n = 0;
    for (MwProp *p = win->props; p; p = p->next) n++;
    Atom *arr = n ? malloc(sizeof(Atom) * n) : NULL;
    int i = 0;
    for (MwProp *p = win->props; p; p = p->next) arr[i++] = p->atom;
    if (num_prop_return) *num_prop_return = n;
    return arr;
}

/* -------------------------------------------------------- text properties */

int XGetTextProperty(Display *d, Window w, XTextProperty *tp, Atom property)
{
    /* The reference implementation always leaves *tp in a safe state, even on
     * failure: callers that only test `rc >= Success` (Success is 0, so a
     * "not found" 0 passes) then hand *tp to XmbTextPropertyToTextList.  CDE's
     * DtWsmGetWorkspaceInfo does exactly that, so without this it read a
     * missing workspace-info property as uninitialised stack and crashed. */
    if (tp) {
        tp->value = NULL;
        tp->encoding = None;
        tp->format = 0;
        tp->nitems = 0;
    }
    Atom at; int af; unsigned long ni, ba; unsigned char *data = NULL;
    if (XGetWindowProperty(d, w, property, 0, 0x7fffffff, False,
                           AnyPropertyType, &at, &af, &ni, &ba, &data) != Success)
        return 0;
    if (!data) return 0;
    tp->value = data;
    tp->encoding = at;
    tp->format = af;
    tp->nitems = ni;
    return 1;
}

void XSetTextProperty(Display *d, Window w, XTextProperty *tp, Atom property)
{
    XChangeProperty(d, w, property, tp->encoding, tp->format,
                           PropModeReplace, tp->value, (int)tp->nitems);
}

Status XGetWMName(Display *d, Window w, XTextProperty *name)
{ return XGetTextProperty(d, w, name, XA_WM_NAME); }

Status XGetWMIconName(Display *d, Window w, XTextProperty *name)
{ return XGetTextProperty(d, w, name, XA_WM_ICON_NAME); }

Status XGetWMClientMachine(Display *d, Window w, XTextProperty *name)
{ return XGetTextProperty(d, w, name, XA_WM_CLIENT_MACHINE); }

/* ------------------------------------------------------------ WM hints */

static void set_string_prop(Display *d, Window w, Atom a, const char *s)
{
    if (s) XChangeProperty(d, w, a, XA_STRING, 8, PropModeReplace,
                           (const unsigned char *)s, (int)strlen(s));
}

void XSetWMName(Display *d, Window w, XTextProperty *tp)
{ XSetTextProperty(d, w, tp, XA_WM_NAME); }

void XSetWMIconName(Display *d, Window w, XTextProperty *tp)
{ XSetTextProperty(d, w, tp, XA_WM_ICON_NAME); }

void XSetWMClientMachine(Display *d, Window w, XTextProperty *tp)
{ XSetTextProperty(d, w, tp, XA_WM_CLIENT_MACHINE); }

int XSetIconName(Display *d, Window w, _Xconst char *icon_name)
{ set_string_prop(d, w, XA_WM_ICON_NAME, icon_name); return 1; }

Status XGetIconName(Display *d, Window w, char **icon_name)
{
    Atom at; int af; unsigned long ni, ba; unsigned char *data = NULL;
    if (XGetWindowProperty(d, w, XA_WM_ICON_NAME, 0, 1024, False, XA_STRING,
                           &at, &af, &ni, &ba, &data) == Success && data) {
        *icon_name = (char *)data;
        return 1;
    }
    *icon_name = NULL;
    return 0;
}

int XSetCommand(Display *d, Window w, char **argv, int argc)
{
    XChangeProperty(d, w, XA_WM_COMMAND, XA_STRING, 8, PropModeReplace,
                    (const unsigned char *)argv, argc);
    return 1;
}

Status XGetCommand(Display *d, Window w, char ***argv, int *argc)
{
    Atom at; int af; unsigned long ni, ba; unsigned char *data = NULL;
    if (XGetWindowProperty(d, w, XA_WM_COMMAND, 0, 0x7fffffff, False, XA_STRING,
                           &at, &af, &ni, &ba, &data) != Success || !data)
        return 0;
    /* split on NUL like Xlib */
    int n = 0;
    for (unsigned long i = 0; i < ni; i++) if (data[i] == 0) n++;
    char **v = calloc(n + 2, sizeof(char *));
    int i = 0, start = 0;
    for (unsigned long k = 0; k < ni; k++) {
        if (data[k] == 0) { v[i++] = strdup((char *)data + start); start = (int)k + 1; }
    }
    v[i] = NULL;
    *argv = v; *argc = i;
    free(data);
    return 1;
}

int XSetClassHint(Display *d, Window w, XClassHint *ch)
{
    unsigned char buf[1024];
    int n = 0;
    const char *res = ch->res_name ? ch->res_name : "";
    const char *cls = ch->res_class ? ch->res_class : "";
    int rl = (int)strlen(res), cl = (int)strlen(cls);
    memcpy(buf, res, rl); buf[rl] = 0;
    memcpy(buf + rl + 1, cls, cl); buf[rl + 1 + cl] = 0;
    n = rl + 1 + cl + 1;
    XChangeProperty(d, w, XA_WM_CLASS, XA_STRING, 8, PropModeReplace, buf, n);
    return 1;
}

Status XGetClassHint(Display *d, Window w, XClassHint *ch)
{
    Atom at; int af; unsigned long ni, ba; unsigned char *data = NULL;
    if (XGetWindowProperty(d, w, XA_WM_CLASS, 0, 1024, False, XA_STRING,
                           &at, &af, &ni, &ba, &data) != Success || !data)
        return 0;
    ch->res_name = strdup((char *)data);
    char *cls = (char *)data + strlen((char *)data) + 1;
    ch->res_class = strdup(cls);
    free(data);
    return 1;
}

XClassHint *XAllocClassHint(void) { return calloc(1, sizeof(XClassHint)); }

void XSetWMProperties(Display *d, Window w, XTextProperty *window_name,
                      XTextProperty *icon_name, char **argv, int argc,
                      XSizeHints *normal_hints, XWMHints *wm_hints,
                      XClassHint *class_hints)
{
    if (window_name) XSetWMName(d, w, window_name);
    if (icon_name) XSetWMIconName(d, w, icon_name);
    if (argv) XSetCommand(d, w, argv, argc);
    if (normal_hints) XSetWMNormalHints(d, w, normal_hints);
    if (wm_hints) XSetWMHints(d, w, wm_hints);
    if (class_hints) XSetClassHint(d, w, class_hints);
}

int XSetStandardProperties(Display *d, Window w, _Xconst char *window_name,
                            _Xconst char *icon_name, Pixmap icon_pixmap,
                            char **argv, int argc, XSizeHints *hints)
{
    XTextProperty tp;
    tp.value = (unsigned char *)(window_name ? window_name : (char *)"");
    tp.encoding = XA_STRING; tp.format = 8;
    tp.nitems = strlen((char *)tp.value);
    if (window_name) XSetWMName(d, w, &tp);
    if (icon_name) XSetIconName(d, w, icon_name);
    if (argv) XSetCommand(d, w, argv, argc);
    if (hints) XSetWMNormalHints(d, w, hints);
    return 1;
}

/* ICCCM WM_NORMAL_HINTS is 18 longs, in this order (libX11's xPropSizeHints).
 * The struct cannot be handed to XChangeProperty directly: property data is a
 * long array, and XSizeHints also holds ints. */
static void size_hints_to_longs(const XSizeHints *h, long *p)
{
    p[0]  = h->flags;
    p[1]  = h->x;            p[2]  = h->y;
    p[3]  = h->width;        p[4]  = h->height;
    p[5]  = h->min_width;    p[6]  = h->min_height;
    p[7]  = h->max_width;    p[8]  = h->max_height;
    p[9]  = h->width_inc;    p[10] = h->height_inc;
    p[11] = h->min_aspect.x; p[12] = h->min_aspect.y;
    p[13] = h->max_aspect.x; p[14] = h->max_aspect.y;
    p[15] = h->base_width;   p[16] = h->base_height;
    p[17] = h->win_gravity;
}

static void longs_to_size_hints(const long *p, unsigned long n, XSizeHints *h)
{
    memset(h, 0, sizeof *h);
    if (n > 0)  h->flags = p[0];
    if (n > 1)  h->x = (int)p[1];
    if (n > 2)  h->y = (int)p[2];
    if (n > 3)  h->width = (int)p[3];
    if (n > 4)  h->height = (int)p[4];
    if (n > 5)  h->min_width = (int)p[5];
    if (n > 6)  h->min_height = (int)p[6];
    if (n > 7)  h->max_width = (int)p[7];
    if (n > 8)  h->max_height = (int)p[8];
    if (n > 9)  h->width_inc = (int)p[9];
    if (n > 10) h->height_inc = (int)p[10];
    if (n > 11) h->min_aspect.x = (int)p[11];
    if (n > 12) h->min_aspect.y = (int)p[12];
    if (n > 13) h->max_aspect.x = (int)p[13];
    if (n > 14) h->max_aspect.y = (int)p[14];
    if (n > 15) h->base_width = (int)p[15];
    if (n > 16) h->base_height = (int)p[16];
    if (n > 17) h->win_gravity = (int)p[17];
}

void XSetWMNormalHints(Display *d, Window w, XSizeHints *hints)
{
    long prop[18];
    size_hints_to_longs(hints, prop);
    XChangeProperty(d, w, XA_WM_NORMAL_HINTS, XA_WM_SIZE_HINTS, 32,
                    PropModeReplace, (unsigned char *)prop, 18);
}

Status XGetWMNormalHints(Display *d, Window w, XSizeHints *hints,
                         long *supplied)
{
    Atom at; int af; unsigned long ni, ba; unsigned char *data = NULL;
    if (supplied) *supplied = 0;
    if (XGetWindowProperty(d, w, XA_WM_NORMAL_HINTS, 0, sizeof(XSizeHints) / 4,
                           False, XA_WM_SIZE_HINTS, &at, &af, &ni, &ba,
                           &data) != Success || !data)
        return 0;
    longs_to_size_hints((const long *)data, ni, hints);
    if (supplied) *supplied = (long)ni;
    free(data);
    return 1;
}

Status XGetNormalHints(Display *d, Window w, XSizeHints *hints)
{ long s; return XGetWMNormalHints(d, w, hints, &s); }

XSizeHints *XAllocSizeHints(void) { return calloc(1, sizeof(XSizeHints)); }

int XSetWMHints(Display *d, Window w, XWMHints *hints)
{
    /* WM_HINTS is 9 longs (libX11's xPropWMHints); only some fields are set,
     * matching the flags. */
    long prop[9] = {0};
    prop[0] = hints->flags;
    prop[1] = hints->input;
    prop[2] = hints->initial_state;
    prop[3] = (long)hints->icon_pixmap;
    prop[4] = (long)hints->icon_window;
    prop[5] = hints->icon_x;
    prop[6] = hints->icon_y;
    prop[7] = (long)hints->icon_mask;
    prop[8] = (long)hints->window_group;
    XChangeProperty(d, w, XA_WM_HINTS, XA_WM_HINTS, 32, PropModeReplace,
                    (unsigned char *)prop, 9);
    return 1;
}

XWMHints *XGetWMHints(Display *d, Window w)
{
    Atom at; int af; unsigned long ni, ba; unsigned char *data = NULL;
    if (XGetWindowProperty(d, w, XA_WM_HINTS, 0, sizeof(XWMHints) / 4, False,
                           XA_WM_HINTS, &at, &af, &ni, &ba, &data) != Success
        || !data)
        return NULL;
    const long *p = (const long *)data;
    XWMHints *h = calloc(1, sizeof(XWMHints));
    if (ni > 0) h->flags = p[0];
    if (ni > 1) h->input = (Bool)p[1];
    if (ni > 2) h->initial_state = (int)p[2];
    if (ni > 3) h->icon_pixmap = (Pixmap)p[3];
    if (ni > 4) h->icon_window = (Window)p[4];
    if (ni > 5) h->icon_x = (int)p[5];
    if (ni > 6) h->icon_y = (int)p[6];
    if (ni > 7) h->icon_mask = (Pixmap)p[7];
    if (ni > 8) h->window_group = (XID)p[8];
    free(data);
    return h;
}

XWMHints *XAllocWMHints(void) { return calloc(1, sizeof(XWMHints)); }

Status XSetWMProtocols(Display *d, Window w, Atom *protocols, int count)
{
    return XChangeProperty(d, w, mw_intern_atom(d, "WM_PROTOCOLS", False),
                           XA_ATOM, 32, PropModeReplace,
                           (unsigned char *)protocols, count);
}

Status XGetWMProtocols(Display *d, Window w, Atom **protocols, int *count)
{
    Atom at; int af; unsigned long ni, ba; unsigned char *data = NULL;
    if (XGetWindowProperty(d, w, mw_intern_atom(d, "WM_PROTOCOLS", True),
                           0, 1024, False, XA_ATOM, &at, &af, &ni, &ba,
                           &data) != Success || !data) {
        *protocols = NULL; *count = 0; return 0;
    }
    *protocols = (Atom *)data; *count = (int)ni;
    return 1;
}

int XSetTransientForHint(Display *d, Window w, Window prop_window)
{
    XChangeProperty(d, w, XA_WM_TRANSIENT_FOR, XA_WINDOW, 32, PropModeReplace,
                    (unsigned char *)&prop_window, 1);
    return 1;
}

Status XGetTransientForHint(Display *d, Window w, Window *prop_window)
{
    Atom at; int af; unsigned long ni, ba; unsigned char *data = NULL;
    if (XGetWindowProperty(d, w, XA_WM_TRANSIENT_FOR, 0, 1, False, XA_WINDOW,
                           &at, &af, &ni, &ba, &data) != Success || !data)
        return 0;
    *prop_window = *(Window *)data;
    free(data);
    return 1;
}

Status XSetWMColormapWindows(Display *d, Window w, Window *cw, int n)
{
    return XChangeProperty(d, w, mw_intern_atom(d, "WM_COLORMAP_WINDOWS", False),
                           XA_WINDOW, 32, PropModeReplace, (unsigned char *)cw, n);
}

Status XGetWMColormapWindows(Display *d, Window w, Window **cw, int *n)
{
    Atom at; int af; unsigned long ni, ba; unsigned char *data = NULL;
    if (XGetWindowProperty(d, w, mw_intern_atom(d, "WM_COLORMAP_WINDOWS", True),
                           0, 1024, False, XA_WINDOW, &at, &af, &ni, &ba,
                           &data) != Success || !data) {
        *cw = NULL; *n = 0; return 0;
    }
    *cw = (Window *)data; *n = (int)ni; return 1;
}

void XSetWMSizeHints(Display *d, Window w, XSizeHints *h, Atom a)
{
    long prop[18];
    size_hints_to_longs(h, prop);
    XChangeProperty(d, w, a, XA_WM_SIZE_HINTS, 32, PropModeReplace,
                    (unsigned char *)prop, 18);
}

Status XGetWMSizeHints(Display *d, Window w, XSizeHints *h, long *s, Atom a)
{
    Atom at; int af; unsigned long ni, ba; unsigned char *data = NULL;
    if (XGetWindowProperty(d, w, a, 0, sizeof(XSizeHints) / 4, False,
                           XA_WM_SIZE_HINTS, &at, &af, &ni, &ba, &data)
        != Success || !data) return 0;
    longs_to_size_hints((const long *)data, ni, h);
    if (s) *s = (long)ni;
    free(data);
    return 1;
}
