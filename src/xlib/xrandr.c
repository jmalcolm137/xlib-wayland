/* xrandr.c — a single-output XRandR facade over wl_output. */
#include "internal.h"

#include <X11/extensions/Xrandr.h>
#include <stdlib.h>
#include <string.h>

Bool XRRQueryExtension(Display *d, int *event_base, int *error_base)
{
    (void)d;
    if (event_base) *event_base = 90;
    if (error_base) *error_base = 90;
    return True;
}

Status XRRQueryVersion(Display *d, int *major, int *minor)
{
    (void)d;
    if (major) *major = 1;
    if (minor) *minor = 4;
    return 1;
}

static void fill_mode(Display *d, XRRModeInfo *m)
{
    XDisplayImpl *dp = MWD(d);
    memset(m, 0, sizeof *m);
    m->id = 1;
    m->width = (unsigned)(dp->output_w > 0 ? dp->output_w : MW_DEFAULT_W);
    m->height = (unsigned)(dp->output_h > 0 ? dp->output_h : MW_DEFAULT_H);
    m->dotClock = 60000;
    m->hSyncStart = m->width;
    m->hSyncEnd = m->width;
    m->hTotal = m->width;
    m->vSyncStart = m->height;
    m->vSyncEnd = m->height;
    m->vTotal = m->height;
    m->name = NULL;
    m->nameLength = 0;
    m->modeFlags = RR_HSyncPositive | RR_VSyncPositive;
}

XRRScreenResources *XRRGetScreenResources(Display *d, Window w)
{
    (void)w;
    XRRScreenResources *r = calloc(1, sizeof *r);
    r->timestamp = mw_now();
    r->configTimestamp = r->timestamp;
    r->ncrtc = 1;
    r->crtcs = malloc(sizeof(RRCrtc));
    r->crtcs[0] = 2;
    r->noutput = 1;
    r->outputs = malloc(sizeof(RROutput));
    r->outputs[0] = 3;
    r->nmode = 1;
    r->modes = malloc(sizeof(XRRModeInfo));
    fill_mode(d, &r->modes[0]);
    return r;
}

XRRScreenResources *XRRGetScreenResourcesCurrent(Display *d, Window w)
{ return XRRGetScreenResources(d, w); }

void XRRFreeScreenResources(XRRScreenResources *r)
{
    if (!r) return;
    free(r->crtcs); free(r->outputs); free(r->modes);
    free(r);
}

void XRRFreeModeInfo(XRRModeInfo *m) { free(m); }

XRROutputInfo *XRRGetOutputInfo(Display *d, XRRScreenResources *res, RROutput out)
{
    (void)res;
    XRROutputInfo *o = calloc(1, sizeof *o);
    o->timestamp = mw_now();
    o->crtc = out == 3 ? 2 : 0;
    o->name = strdup("WAYLAND-1");
    o->nameLen = (int)strlen(o->name);
    /* Prefer the compositor's physical size (wl_output.geometry); fall back to
     * a 96-dpi guess only when it does not report one.  GDK turns pixel size
     * plus millimetres into a real DPI, which applications such as GIMP use. */
    o->mm_width  = MWD(d)->output_pw > 0
                   ? (unsigned)MWD(d)->output_pw
                   : (unsigned)(MWD(d)->output_w * 25.4 / MW_DEFAULT_DPI);
    o->mm_height = MWD(d)->output_ph > 0
                   ? (unsigned)MWD(d)->output_ph
                   : (unsigned)(MWD(d)->output_h * 25.4 / MW_DEFAULT_DPI);
    o->connection = RR_Connected;
    o->subpixel_order = SubPixelUnknown;
    o->ncrtc = 1;
    o->crtcs = malloc(sizeof(RRCrtc));
    o->crtcs[0] = 2;
    o->nclone = 0;
    o->nmode = 1;
    o->npreferred = 1;
    o->modes = malloc(sizeof(RRMode));
    o->modes[0] = 1;
    return o;
}

void XRRFreeOutputInfo(XRROutputInfo *o)
{
    if (!o) return;
    free(o->name); free(o->crtcs); free(o->clones); free(o->modes);
    free(o);
}

XRRCrtcInfo *XRRGetCrtcInfo(Display *d, XRRScreenResources *res, RRCrtc crtc)
{
    (void)res; (void)crtc;
    XRRCrtcInfo *c = calloc(1, sizeof *c);
    XDisplayImpl *dp = MWD(d);
    c->timestamp = mw_now();
    c->x = 0; c->y = 0;
    c->width = (unsigned)(dp->output_w > 0 ? dp->output_w : MW_DEFAULT_W);
    c->height = (unsigned)(dp->output_h > 0 ? dp->output_h : MW_DEFAULT_H);
    c->mode = 1;
    c->rotation = RR_Rotate_0;
    c->noutput = 1;
    c->outputs = malloc(sizeof(RROutput));
    c->outputs[0] = 3;
    c->rotations = RR_Rotate_0;
    c->npossible = 1;
    c->possible = malloc(sizeof(RROutput));
    c->possible[0] = 3;
    return c;
}

void XRRFreeCrtcInfo(XRRCrtcInfo *c)
{
    if (!c) return;
    free(c->outputs); free(c->possible);
    free(c);
}

RROutput XRRGetOutputPrimary(Display *d, Window w)
{ (void)d; (void)w; return 3; }

void XRRSelectInput(Display *d, Window w, int mask) { (void)d; (void)w; (void)mask; }

Bool XRRUpdateConfiguration(XEvent *event) { (void)event; return True; }

int XRRRootToScreen(Display *d, Window root)
{
    (void)d; (void)root;
    return 0;
}

Status XRRGetScreenSizeRange(Display *d, Window w, int *minWidth, int *minHeight,
                             int *maxWidth, int *maxHeight)
{
    (void)w;
    XDisplayImpl *dp = MWD(d);
    *minWidth = *minHeight = 1;
    *maxWidth = dp->output_w > 0 ? dp->output_w : MW_DEFAULT_W;
    *maxHeight = dp->output_h > 0 ? dp->output_h : MW_DEFAULT_H;
    return 1;
}

void XRRSetScreenSize(Display *d, Window w, int width, int height,
                      int mmWidth, int mmHeight)
{
    (void)w; (void)mmWidth; (void)mmHeight;
    XDisplayImpl *dp = MWD(d);
    dp->output_w = width;
    dp->output_h = height;
    if (dp->screens) {
        dp->screens[0].width = width;
        dp->screens[0].height = height;
    }
}

int XRRGetScreenResourcesRefCount(XRRScreenResources *r) { (void)r; return 0; }

/* RandR 1.5 monitors.  We advertise 1.4, so GDK takes the 1.3 path
 * (XRRGetOutputInfo); these exist so a program that links libXrandr finds the
 * symbols, and so advertising 1.5 later is a one-line change. */
XRRMonitorInfo *XRRGetMonitors(Display *d, Window w, Bool get_active,
                               int *nmonitors)
{
    (void)w; (void)get_active;
    XDisplayImpl *dp = MWD(d);
    XRRMonitorInfo *m = calloc(1, sizeof *m);
    if (!m) { if (nmonitors) *nmonitors = 0; return NULL; }
    m->name = None;
    m->primary = True;
    m->automatic = False;
    m->noutput = 1;
    m->x = 0;
    m->y = 0;
    m->width  = dp->output_w > 0 ? dp->output_w : MW_DEFAULT_W;
    m->height = dp->output_h > 0 ? dp->output_h : MW_DEFAULT_H;
    m->mwidth  = dp->output_pw > 0 ? dp->output_pw
                 : (int)(m->width * 25.4 / MW_DEFAULT_DPI);
    m->mheight = dp->output_ph > 0 ? dp->output_ph
                 : (int)(m->height * 25.4 / MW_DEFAULT_DPI);
    m->outputs = malloc(sizeof(RROutput));
    if (m->outputs) m->outputs[0] = 3;
    else m->noutput = 0;
    if (nmonitors) *nmonitors = 1;
    return m;
}

void XRRFreeMonitors(XRRMonitorInfo *monitors)
{
    if (!monitors) return;
    free(monitors[0].outputs);
    free(monitors);
}

/* The remaining RandR entry points a GTK2 application may link.  There is one
 * output whose configuration the compositor owns, so gamma and mode-setting
 * requests are accepted and not carried, and output properties report "not
 * found".  They exist so nothing fails to resolve at load time. */

XRRCrtcGamma *XRRAllocGamma(int size)
{
    XRRCrtcGamma *g = malloc(sizeof *g);
    if (!g) return NULL;
    g->size = size;
    g->red   = calloc((size_t)size, sizeof(unsigned short));
    g->green = calloc((size_t)size, sizeof(unsigned short));
    g->blue  = calloc((size_t)size, sizeof(unsigned short));
    if (!g->red || !g->green || !g->blue) { XRRFreeGamma(g); return NULL; }
    return g;
}

void XRRFreeGamma(XRRCrtcGamma *gamma)
{
    if (!gamma) return;
    free(gamma->red);
    free(gamma->green);
    free(gamma->blue);
    free(gamma);
}

int XRRGetCrtcGammaSize(Display *d, RRCrtc crtc) { (void)d; (void)crtc; return 0; }

XRRCrtcGamma *XRRGetCrtcGamma(Display *d, RRCrtc crtc)
{ (void)d; (void)crtc; return NULL; }

void XRRSetCrtcGamma(Display *d, RRCrtc crtc, XRRCrtcGamma *gamma)
{ (void)d; (void)crtc; (void)gamma; }

int XRRGetOutputProperty(Display *dpy, RROutput output, Atom property,
                         long offset, long length, Bool delete, Bool pending,
                         Atom req_type, Atom *actual_type, int *actual_format,
                         unsigned long *nitems, unsigned long *bytes_after,
                         unsigned char **prop)
{
    (void)dpy; (void)output; (void)property; (void)offset; (void)length;
    (void)delete; (void)pending; (void)req_type;
    if (actual_type)   *actual_type = None;
    if (actual_format) *actual_format = 0;
    if (nitems)        *nitems = 0;
    if (bytes_after)   *bytes_after = 0;
    if (prop)          *prop = NULL;
    return BadName;
}

Status XRRSetCrtcConfig(Display *dpy, XRRScreenResources *res, RRCrtc crtc,
                        Time timestamp, int x, int y, RRMode mode,
                        Rotation rotation, RROutput *outputs, int noutputs)
{
    (void)dpy; (void)res; (void)crtc; (void)timestamp; (void)x; (void)y;
    (void)mode; (void)rotation; (void)outputs; (void)noutputs;
    return Success;
}

void XRRSetOutputPrimary(Display *dpy, Window window, RROutput output)
{ (void)dpy; (void)window; (void)output; }

/* RandR 1.4 providers.  The compositor owns the single output, so there is no
 * display-provider indirection to report: hand back an empty provider list.
 * Firefox's GL probe reads nproviders and skips the provider lookup at zero. */
XRRProviderResources *XRRGetProviderResources(Display *d, Window w)
{
    (void)d; (void)w;
    XRRProviderResources *r = calloc(1, sizeof *r);
    if (!r) return NULL;
    r->timestamp = mw_now();
    r->nproviders = 0;
    r->providers = NULL;
    return r;
}

void XRRFreeProviderResources(XRRProviderResources *r)
{
    if (!r) return;
    free(r->providers);
    free(r);
}

XRRProviderInfo *XRRGetProviderInfo(Display *d, XRRScreenResources *res,
                                    RRProvider provider)
{
    (void)d; (void)res; (void)provider;
    return NULL;
}

void XRRFreeProviderInfo(XRRProviderInfo *info)
{
    if (!info) return;
    free(info->crtcs);
    free(info->outputs);
    free(info->name);
    free(info->associated_providers);
    free(info->associated_capability);
    free(info);
}
