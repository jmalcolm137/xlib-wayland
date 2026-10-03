/* compat.c — the long tail of Xlib entry points. */
#include "internal.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <ctype.h>

/* ----------------------------------------------------- selection input */

int XSelectInput(Display *d, Window w, long event_mask)
{
    MwWindow *win = mw_window(d, w);
    if (!win) return 0;
    if (getenv("MW_TRACE"))
        fprintf(stderr, "MW: XSelectInput 0x%lx mask=0x%lx\n", w, event_mask);
    win->event_mask = event_mask;
    win->all_event_masks |= event_mask;
    return 1;
}

int XSetNormalHints(Display *d, Window w, XSizeHints *hints)
{ XSetWMNormalHints(d, w, hints); return 1; }

void XSetRGBColormaps(Display *d, Window w, XStandardColormap *cmaps, int count,
                      Atom property)
{ XChangeProperty(d, w, property, XA_RGB_COLOR_MAP, 32, PropModeReplace,
                         (unsigned char *)cmaps,
                         count * (int)(sizeof(XStandardColormap) / sizeof(long))); }

Status XGetRGBColormaps(Display *d, Window w, XStandardColormap **cmaps,
                        int *count, Atom property)
{
    Atom at; int af; unsigned long ni, ba; unsigned char *data = NULL;
    if (XGetWindowProperty(d, w, property, 0, 0x7fffffff, False, XA_RGB_COLOR_MAP,
                           &at, &af, &ni, &ba, &data) != Success || !data) {
        *cmaps = NULL; *count = 0; return 0;
    }
    *cmaps = (XStandardColormap *)data;
    *count = (int)(ni / (sizeof(XStandardColormap) / sizeof(long)));
    return 1;
}

/* ------------------------------------------------------- text properties */

Status XmbTextListToTextProperty(Display *d, char **list, int count,
                                 XICCEncodingStyle style, XTextProperty *tp)
{
    (void)d; (void)style;
    size_t total = 0;
    for (int i = 0; i < count; i++) total += strlen(list[i]) + 1;
    unsigned char *buf = calloc(total + 1, 1);
    size_t o = 0;
    for (int i = 0; i < count; i++) {
        size_t n = strlen(list[i]);
        memcpy(buf + o, list[i], n); o += n; buf[o++] = 0;
    }
    tp->value = buf;
    tp->encoding = XA_STRING;
    tp->format = 8;
    tp->nitems = o;
    return Success;
}

Status Xutf8TextListToTextProperty(Display *d, char **list, int count,
                                   XICCEncodingStyle style, XTextProperty *tp)
{ return XmbTextListToTextProperty(d, list, count, style, tp); }

int XmbTextPropertyToTextList(Display *d, const XTextProperty *tp, char ***list,
                                 int *count)
{
    (void)d;
    int n = 0;
    for (unsigned long i = 0; i < tp->nitems; i++) if (tp->value[i] == 0) n++;
    if (n == 0) n = 1;
    char **v = calloc(n + 1, sizeof(char *));
    int i = 0, start = 0;
    for (unsigned long k = 0; k < tp->nitems; k++)
        if (tp->value[k] == 0) { v[i++] = strdup((char *)tp->value + start); start = (int)k + 1; }
    if (i == 0) v[i++] = strdup("");
    v[i] = NULL;
    *list = v; *count = i;
    return Success;
}

int Xutf8TextPropertyToTextList(Display *d, const XTextProperty *tp, char ***list,
                                   int *count)
{ return XmbTextPropertyToTextList(d, tp, list, count); }

/* The STRING-encoding pair.  Unlike the Xmb... variants these take no
 * Display, so STRING is the only encoding they can recognise -- exactly the
 * limitation the reference implementation has.  xterm uses the pair to parse
 * text it has just read back off a property. */
Status XTextPropertyToStringList(XTextProperty *tp, char ***list_rtrn,
                                 int *count_rtrn)
{
    if (!list_rtrn || !count_rtrn) return 0;
    *list_rtrn = NULL;
    *count_rtrn = 0;
    if (!tp || tp->format != 8 || tp->encoding != XA_STRING || !tp->value)
        return 0;
    int count = 0;
    for (unsigned long i = 0; i < tp->nitems; i++)
        if (tp->value[i] == 0) count++;
    char **v = calloc((size_t)count + 1, sizeof(char *));
    if (!v) return 0;
    int n = 0;
    unsigned long start = 0;
    for (unsigned long i = 0; i < tp->nitems; i++) {
        if (tp->value[i] != 0) continue;
        size_t len = i - start;
        v[n] = malloc(len + 1);
        if (!v[n]) { XFreeStringList(v); return 0; }
        memcpy(v[n], tp->value + start, len);
        v[n][len] = 0;
        n++;
        start = i + 1;
    }
    v[n] = NULL;
    *list_rtrn = v;
    *count_rtrn = n;
    return Success;
}

Status XStringListToTextProperty(char **list, int count, XTextProperty *tp)
{
    size_t total = 0;
    for (int i = 0; i < count; i++)
        total += (list[i] ? strlen(list[i]) : 0) + 1;
    unsigned char *buf = calloc(total + 1, 1);
    if (!buf) return 0;
    size_t o = 0;
    for (int i = 0; i < count; i++) {
        size_t n = list[i] ? strlen(list[i]) : 0;
        if (n) memcpy(buf + o, list[i], n);
        o += n;
        buf[o++] = 0;
    }
    tp->value = buf;
    tp->encoding = XA_STRING;
    tp->format = 8;
    tp->nitems = o;
    return Success;
}

void XFreeStringList(char **list)
{
    if (!list) return;
    for (char **p = list; *p; p++) free(*p);
    free(list);
}

void XmbSetWMProperties(Display *d, Window w, _Xconst char *window_name,
                          _Xconst char *icon_name, char **argv, int argc,
                          XSizeHints *normal_hints, XWMHints *wm_hints,
                          XClassHint *class_hints)
{
    if (window_name) {
        XTextProperty tp;
        tp.value = (unsigned char *)window_name;
        tp.encoding = XA_STRING; tp.format = 8;
        tp.nitems = strlen(window_name);
        XSetWMName(d, w, &tp);
    }
    XSetWMProperties(d, w, NULL, NULL, argv, argc, normal_hints, wm_hints, class_hints);
    if (icon_name) XSetIconName(d, w, icon_name);
}

void Xutf8SetWMProperties(Display *d, Window w, _Xconst char *wn,
                            _Xconst char *in, char **argv, int argc,
                            XSizeHints *nh, XWMHints *wh, XClassHint *ch)
{ XmbSetWMProperties(d, w, wn, in, argv, argc, nh, wh, ch); }

/* --------------------------------------------------------------- names */

Status XFetchName(Display *d, Window w, char **name)
{
    Atom at; int af; unsigned long ni, ba; unsigned char *data = NULL;
    if (XGetWindowProperty(d, w, XA_WM_NAME, 0, 1024, False, AnyPropertyType,
                           &at, &af, &ni, &ba, &data) != Success || !data) {
        *name = NULL; return 0;
    }
    *name = (char *)data;
    return 1;
}

void XSetName(Display *d, Window w, _Xconst char *name)
{
    XChangeProperty(d, w, XA_WM_NAME, XA_STRING, 8, PropModeReplace,
                    (const unsigned char *)name, (int)strlen(name));
}

/* ------------------------------------------------------------- contexts */

struct MwContext { XID rid; XContext ctx; XPointer data; struct MwContext *next; };

int XSaveContext(Display *d, XID rid, XContext context, _Xconst char *data)
{
    XDisplayImpl *dp = MWD(d);
    for (struct MwContext *c = dp->contexts; c; c = c->next)
        if (c->rid == rid && c->ctx == context) { c->data = (XPointer)data; return 0; }
    struct MwContext *c = calloc(1, sizeof *c);
    c->rid = rid; c->ctx = context; c->data = (XPointer)data;
    c->next = dp->contexts; dp->contexts = c;
    return 0;
}

int XFindContext(Display *d, XID rid, XContext context, XPointer *data)
{
    for (struct MwContext *c = MWD(d)->contexts; c; c = c->next)
        if (c->rid == rid && c->ctx == context) { if (data) *data = c->data; return 0; }
    return XCNOENT;
}

int XDeleteContext(Display *d, XID rid, XContext context)
{
    XDisplayImpl *dp = MWD(d);
    struct MwContext **pp = &dp->contexts;
    while (*pp) {
        if ((*pp)->rid == rid && (*pp)->ctx == context) {
            struct MwContext *dead = *pp; *pp = dead->next; free(dead); return 0;
        }
        pp = &(*pp)->next;
    }
    return XCNOENT;
}

/* ----------------------------------------------------------- Xrm default */

char *XGetDefault(Display *d, _Xconst char *prog, _Xconst char *name)
{
    char *res = MWD(d)->xdefaults;
    static char buf[512];
    if (!res || !prog || !name) return NULL;
    snprintf(buf, sizeof buf, "%s.%s:", prog, name);
    char *p = strstr(res, buf);
    if (!p) return NULL;
    p += strlen(buf);
    while (*p == ' ' || *p == '\t') p++;
    static char val[512];
    int i = 0;
    while (*p && *p != '\n' && i < 511) val[i++] = *p++;
    val[i] = 0;
    return val;
}

/* --------------------------------------------------------- extensions */

Bool XQueryExtension(Display *d, _Xconst char *name, int *major_opcode,
                     int *first_event, int *first_error)
{
    (void)d;
    if (getenv("MW_TRACE"))
        fprintf(stderr, "MW: XQueryExtension \"%s\"\n", name ? name : "(null)");
    if (mw_xi2_query_extension(name, major_opcode, first_event, first_error))
        return True;
    if (name && (strcmp(name, "SHAPE") == 0 || strcmp(name, "RANDR") == 0)) {
        if (major_opcode) *major_opcode = strcmp(name, "SHAPE") ? 140 : 139;
        if (first_event) *first_event = strcmp(name, "SHAPE") ? 90 : 64;
        if (first_error) *first_error = 64;
        return True;
    }
    if (major_opcode) *major_opcode = 0;
    if (first_event) *first_event = 0;
    if (first_error) *first_error = 0;
    return False;
}

XExtCodes *XInitExtension(Display *d, _Xconst char *name)
{
    /* XextAddDisplay (libXext) calls this to learn an extension's codes, and a
     * NULL return means "the server does not have this extension": libraries
     * built on libXext -- libXi above all -- then refuse to run at all.  Report
     * the codes of the extensions the shim does provide. */
    int major, first_event, first_error;
    if (!XQueryExtension(d, name, &major, &first_event, &first_error))
        return NULL;
    XExtCodes *e = calloc(1, sizeof *e);
    if (!e) return NULL;
    static int next_extension = 1;   /* XAddExtension uses 128 and up */
    e->extension = next_extension++;
    e->major_opcode = major;
    e->first_event = first_event;
    e->first_error = first_error;
    return e;
}

char **XListExtensions(Display *d, int *nextensions)
{
    (void)d;
    char **e = calloc(4, sizeof(char *));
    e[0] = strdup("SHAPE");
    e[1] = strdup("RANDR");
    e[2] = strdup(mw_xi2_extension_name());
    e[3] = NULL;
    if (nextensions) *nextensions = 3;
    return e;
}

int XFreeExtensionList(char **list)
{
    if (list) { for (char **p = list; *p; p++) free(*p); free(list); }
    return 1;
}

XExtCodes *XAddExtension(Display *d)
{
    /* Real Xlib allocates a client-side extension record with a unique
     * extension number.  Motif (ColorObj) stores dpy here and reads
     * xExt->extension for XESetCloseDisplay. */
    (void)d;
    static int next_extension = 128;
    XExtCodes *e = calloc(1, sizeof *e);
    e->extension = next_extension++;
    e->major_opcode = 0;
    e->first_event = 0;
    e->first_error = 0;
    return e;
}

XExtData **XEHeadOfExtensionList(XEDataObject object)
{ (void)object; return NULL; }
XExtData *XFindOnExtensionList(XExtData **head, int number)
{ (void)head; (void)number; return NULL; }

int (*XESetCloseDisplay(Display *d, int extension,
                        int (*proc)(Display *, XExtCodes *)))(Display *, XExtCodes *)
{ (void)d; (void)extension; (void)proc; return NULL; }

/* ------------------------------------------------------- keyboard/pointer */

int XChangeKeyboardControl(Display *d, unsigned long mask, XKeyboardControl *v)
{ (void)d; (void)mask; (void)v; return 1; }

int XGetKeyboardControl(Display *d, XKeyboardState *s)
{
    (void)d;
    memset(s, 0, sizeof *s);
    s->bell_percent = 50;
    s->global_auto_repeat = AutoRepeatModeOn;
    memset(s->auto_repeats, 0xff, sizeof s->auto_repeats);
    return 1;
}

int XQueryKeymap(Display *d, char keys[32])
{
    (void)d;
    memset(keys, 0, 32);
    return 1;
}

int XAutoRepeatOn(Display *d) { (void)d; return 1; }
int XAutoRepeatOff(Display *d) { (void)d; return 1; }

XTimeCoord *XGetMotionEvents(Display *d, Window w, Time start, Time stop,
                             int *nevents)
{
    (void)d; (void)w; (void)start; (void)stop;
    if (nevents) *nevents = 0;
    return NULL;
}

int XQueryBestSize(Display *d, int class, Drawable dr, unsigned int width,
                   unsigned int height, unsigned int *w, unsigned int *h)
{ (void)d; (void)class; (void)dr; if (w) *w = width; if (h) *h = height; return 1; }

int XQueryBestTile(Display *d, Drawable dr, unsigned int width,
                   unsigned int height, unsigned int *w, unsigned int *h)
{ return XQueryBestSize(d, 0, dr, width, height, w, h); }

int XQueryBestStipple(Display *d, Drawable dr, unsigned int width,
                      unsigned int height, unsigned int *w, unsigned int *h)
{ return XQueryBestSize(d, 0, dr, width, height, w, h); }

int XSetScreenSaver(Display *d, int timeout, int interval, int prefer_blank,
                    int allow_exposures)
{ (void)d; (void)timeout; (void)interval; (void)prefer_blank; (void)allow_exposures; return 1; }

int XGetScreenSaver(Display *d, int *timeout, int *interval, int *prefer_blank,
                    int *allow_exposures)
{
    (void)d;
    if (timeout) *timeout = 600;
    if (interval) *interval = 600;
    if (prefer_blank) *prefer_blank = PreferBlanking;
    if (allow_exposures) *allow_exposures = AllowExposures;
    return 1;
}

int XForceScreenSaver(Display *d, int mode) { (void)d; (void)mode; return 1; }
int XActivateScreenSaver(Display *d) { (void)d; return 1; }
int XResetScreenSaver(Display *d) { (void)d; return 1; }

int XNoOp(Display *d) { (void)d; return 1; }

int (*XSetAfterFunction(Display *d, int (*proc)(Display *)))(Display *)
{ (void)d; (void)proc; return NULL; }

int XAllocColorPlanes(Display *d, Colormap cmap, Bool contig, unsigned long *pixels,
                      int ncolors, int nreds, int ngreens, int nblues,
                      unsigned long *rmask, unsigned long *gmask, unsigned long *bmask)
{
    (void)d; (void)cmap; (void)contig; (void)pixels; (void)ncolors;
    (void)nreds; (void)ngreens; (void)nblues;
    if (rmask) *rmask = 0x00ff0000;
    if (gmask) *gmask = 0x0000ff00;
    if (bmask) *bmask = 0x000000ff;
    return 1;
}

int XStoreNamedColor(Display *d, Colormap cmap, _Xconst char *color,
                     unsigned long pixel, int flags)
{
    (void)d; (void)cmap; (void)color; (void)pixel; (void)flags;
    return 1;
}

XIconSize *XAllocIconSize(void) { return calloc(1, sizeof(XIconSize)); }

int XSetIconSizes(Display *d, Window w, XIconSize *size_list, int count)
{
    /* XIconSize holds ints, but format-32 property data must be a long array,
     * so convert rather than passing the struct. */
    long *prop = malloc((size_t)count * 6 * sizeof(long));
    for (int i = 0; i < count; i++) {
        prop[i * 6 + 0] = size_list[i].min_width;
        prop[i * 6 + 1] = size_list[i].min_height;
        prop[i * 6 + 2] = size_list[i].max_width;
        prop[i * 6 + 3] = size_list[i].max_height;
        prop[i * 6 + 4] = size_list[i].width_inc;
        prop[i * 6 + 5] = size_list[i].height_inc;
    }
    int r = XChangeProperty(d, w, XA_WM_ICON_SIZE, XA_WM_ICON_SIZE, 32,
                            PropModeReplace, (unsigned char *)prop, count * 6);
    free(prop);
    return r;
}

int XGetIconSizes(Display *d, Window w, XIconSize **size_list, int *count)
{
    Atom at; int af; unsigned long ni, ba; unsigned char *data = NULL;
    if (XGetWindowProperty(d, w, XA_WM_ICON_SIZE, 0, 0x7fffffff, False,
                           XA_WM_ICON_SIZE, &at, &af, &ni, &ba, &data) != Success
        || !data) { *size_list = NULL; *count = 0; return 0; }
    /* Property data comes back as a long array; XIconSize holds ints. */
    int n = (int)(ni / 6);
    XIconSize *out = n ? malloc((size_t)n * sizeof(XIconSize)) : NULL;
    const long *p = (const long *)data;
    for (int i = 0; i < n; i++) {
        out[i].min_width  = (int)p[i * 6 + 0];
        out[i].min_height = (int)p[i * 6 + 1];
        out[i].max_width  = (int)p[i * 6 + 2];
        out[i].max_height = (int)p[i * 6 + 3];
        out[i].width_inc  = (int)p[i * 6 + 4];
        out[i].height_inc = (int)p[i * 6 + 5];
    }
    free(data);
    *size_list = out;
    *count = n;
    return 1;
}

int XSetAccessControl(Display *d, int mode) { (void)d; (void)mode; return 1; }
int XAddHost(Display *d, XHostAddress *h) { (void)d; (void)h; return 1; }
int XRemoveHost(Display *d, XHostAddress *h) { (void)d; (void)h; return 1; }
int XEnableAccessControl(Display *d) { (void)d; return 1; }
int XDisableAccessControl(Display *d) { (void)d; return 1; }
XHostAddress *XListHosts(Display *d, int *nhosts, Bool *state)
{ (void)d; if (nhosts) *nhosts = 0; if (state) *state = True; return NULL; }

int XAddToSaveSet(Display *d, Window w) { (void)d; (void)w; return 1; }
int XRemoveFromSaveSet(Display *d, Window w) { (void)d; (void)w; return 1; }
int XChangeSaveSet(Display *d, Window w, int mode) { (void)d; (void)w; (void)mode; return 1; }
