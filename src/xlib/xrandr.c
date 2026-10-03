/* xrandr.c — a single-output XRandR facade over wl_output. */
#include "internal.h"

#include <X11/extensions/Xrandr.h>
#include <stdlib.h>
#include <string.h>

void mw_xrandr_init(Display *d) { (void)d; }

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
    o->mm_width = (unsigned)(MWD(d)->output_w * 25.4 / MW_DEFAULT_DPI);
    o->mm_height = (unsigned)(MWD(d)->output_h * 25.4 / MW_DEFAULT_DPI);
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
