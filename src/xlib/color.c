/* color.c — visuals, colormaps and colour allocation. */
#include "internal.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <ctype.h>

/* ------------------------------------------------------------- colormaps */

Colormap XCreateColormap(Display *d, Window w, Visual *visual, int alloc)
{
    (void)w; (void)alloc;
    MwColormap *cm = calloc(1, sizeof *cm);
    cm->id = mw_alloc_id(d);
    cm->screen = MWSCR(d);
    cm->visual = visual ? visual : &MWD(d)->visual;
    mw_register(d, cm->id, MW_OBJ_COLORMAP, cm);
    return cm->id;
}

int XFreeColormap(Display *d, Colormap cmap)
{
    MwColormap *cm = mw_colormap(d, cmap);
    if (cm) { mw_unregister(d, cmap); free(cm); }
    return 1;
}

Colormap XCopyColormapAndFree(Display *d, Colormap cmap)
{
    MwColormap *cm = mw_colormap(d, cmap);
    return XCreateColormap(d, MWSCR(d)->root,
                           cm ? cm->visual : &MWD(d)->visual, AllocNone);
}

int XInstallColormap(Display *d, Colormap cmap) { (void)d; (void)cmap; return 1; }
int XUninstallColormap(Display *d, Colormap cmap) { (void)d; (void)cmap; return 1; }

Colormap *XListInstalledColormaps(Display *d, Window w, int *nret)
{
    (void)w;
    Colormap *cmaps = malloc(sizeof(Colormap));
    cmaps[0] = MWSCR(d)->cmap;
    if (nret) *nret = 1;
    return cmaps;
}

/* ---------------------------------------------------------------- colours */

static unsigned short c8to16(unsigned int v) { return (unsigned short)(v * 257); }

int XQueryColor(Display *d, Colormap cmap, XColor *def)
{
    (void)d; (void)cmap;
    unsigned long px = def->pixel;
    def->red   = c8to16((px >> 16) & 0xff);
    def->green = c8to16((px >>  8) & 0xff);
    def->blue  = c8to16(px & 0xff);
    def->flags = DoRed | DoGreen | DoBlue;
    return 1;
}

int XQueryColors(Display *d, Colormap cmap, XColor *defs, int ncolors)
{
    for (int i = 0; i < ncolors; i++) XQueryColor(d, cmap, &defs[i]);
    return 1;
}

int XStoreColor(Display *d, Colormap cmap, XColor *c) { (void)d; (void)cmap; (void)c; return 1; }
int XStoreColors(Display *d, Colormap cmap, XColor *c, int n) { (void)d; (void)cmap; (void)c; (void)n; return 1; }

int XAllocColor(Display *d, Colormap cmap, XColor *def)
{
    (void)d; (void)cmap;
    def->pixel = ((unsigned long)(def->red   >> 8) << 16)
               | ((unsigned long)(def->green >> 8) <<  8)
               | ((unsigned long)(def->blue  >> 8));
    def->flags = DoRed | DoGreen | DoBlue;
    return 1;
}

int XAllocColorCells(Display *d, Colormap cmap, Bool contig,
                     unsigned long *planes, unsigned int nplanes,
                     unsigned long *pixels, unsigned int ncolors)
{
    (void)contig; (void)planes; (void)nplanes;
    MwColormap *cm = mw_colormap(d, cmap);
    int base = cm ? cm->alloc_count : 0;
    for (unsigned int i = 0; i < ncolors; i++) pixels[i] = (unsigned long)(base + i);
    if (cm) cm->alloc_count += (int)ncolors;
    return 1;
}

int XFreeColors(Display *d, Colormap cmap, unsigned long *pixels,
                int npixels, unsigned long planes)
{ (void)d; (void)cmap; (void)pixels; (void)npixels; (void)planes; return 1; }

/* ----------------------------------------------------------- rgb.txt names */

typedef struct { char *name; unsigned char r, g, b; } NamedColor;
static NamedColor *g_named;
static int g_nnamed;

/* The standard X11 colour database (rgb.txt), embedded at build time because
 * it is not installed on all systems. */
static const struct { const char *n; int r, g, b; } rgb_table[] = {
#include "rgb_table.h"
};

static void load_rgb(void)
{
    if (g_named || g_nnamed) return;
    const char *paths[] = { "/usr/share/X11/rgb.txt", "/etc/X11/rgb.txt", NULL };
    FILE *f = NULL;
    for (int i = 0; paths[i]; i++) { f = fopen(paths[i], "r"); if (f) break; }
    if (!f) { g_nnamed = -1; return; }
    char line[256];
    int cap = 1024;
    g_named = calloc(cap, sizeof(NamedColor));
    while (fgets(line, sizeof line, f)) {
        if (line[0] == '!' || line[0] == '#') continue;
        int r, g, b;
        char name[200];
        if (sscanf(line, "%d %d %d %199[^\n]", &r, &g, &b, name) == 4) {
            char *p = name;
            while (*p == ' ' || *p == '\t') p++;
            if (g_nnamed >= cap) break;
            g_named[g_nnamed].name = strdup(p);
            g_named[g_nnamed].r = (unsigned char)r;
            g_named[g_nnamed].g = (unsigned char)g;
            g_named[g_nnamed].b = (unsigned char)b;
            g_nnamed++;
        }
    }
    fclose(f);
}

static int lookup_named(const char *spec, int *r, int *g, int *b)
{
    static const struct { const char *n; int r, g, b; } builtin[] = {
        {"white",255,255,255},{"black",0,0,0},{"red",255,0,0},{"green",0,255,0},
        {"blue",0,0,255},{"yellow",255,255,0},{"cyan",0,255,255},
        {"magenta",255,0,255},{"gray",190,190,190},{"grey",190,190,190},
        {"darkgray",169,169,169},{"lightgray",211,211,211},
        {"lightgrey",211,211,211},{"orange",255,165,0},{"purple",160,32,240},
        {"brown",165,42,42},{"pink",255,192,203},{"gold",255,215,0},
        {"navy",0,0,128},{"teal",0,128,128},{"olive",128,128,0},
        {"maroon",176,48,96},{"silver",192,192,192},{"lime",0,255,0},
        {"aqua",0,255,255},{"fuchsia",255,0,255},{"tan",210,180,140},
        {"beige",245,245,220},{"ivory",255,255,240},{"khaki",240,230,140},
        {"salmon",250,128,114},{"turquoise",64,224,208},{"violet",238,130,238},
        {"indigo",75,0,130},{"chocolate",210,105,30},{"coral",255,127,80},
        {"crimson",220,20,60},{"orchid",218,112,214},{"plum",221,160,221},
        {"wheat",245,222,179},{"snow",255,250,250},{"azure",240,255,255},
        {"ghostwhite",248,248,255},{"whitesmoke",245,245,245},
        {"gainsboro",220,220,220},{"dimGray",105,105,105},
        {"dimgray",105,105,105},{"lightblue",173,216,230},
        {"lightgreen",144,238,144},{"skyblue",135,206,235},
        {"royalblue",65,105,225},{"steelblue",70,130,180},
        {"midnightblue",25,25,112},{"seagreen",46,139,87},
        {"forestgreen",34,139,34},{"darkgreen",0,100,0},{"darkred",139,0,0},
        {"darkblue",0,0,139},{"darkcyan",0,139,139},
        {"darkmagenta",139,0,139},{"darkorange",255,140,0},
        {"goldenrod",218,165,32},{"sienna",160,82,45},{"peru",205,133,63},
        {"blueviolet",138,43,226},{"mediumpurple",147,112,219},
    };
    for (size_t i = 0; i < sizeof(rgb_table)/sizeof(rgb_table[0]); i++)
        if (strcasecmp(rgb_table[i].n, spec) == 0) {
            *r = rgb_table[i].r; *g = rgb_table[i].g; *b = rgb_table[i].b;
            return 1;
        }
    for (size_t i = 0; i < sizeof(builtin)/sizeof(builtin[0]); i++)
        if (strcasecmp(builtin[i].n, spec) == 0) {
            *r = builtin[i].r; *g = builtin[i].g; *b = builtin[i].b;
            return 1;
        }
    load_rgb();
    for (int i = 0; i < g_nnamed; i++)
        if (strcasecmp(g_named[i].name, spec) == 0) {
            *r = g_named[i].r; *g = g_named[i].g; *b = g_named[i].b;
            return 1;
        }
    return 0;
}

static int hexval(int c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static int parse_rgb_spec(const char *spec, unsigned short out[3])
{
    const char *p = spec + 4;   /* past "rgb:" */
    for (int i = 0; i < 3; i++) {
        while (*p == ' ' || *p == '\t') p++;
        unsigned long v = 0;
        int n = 0;
        while (isxdigit((unsigned char)*p) && n < 5) {
            v = v * 16 + (unsigned)hexval((unsigned char)*p);
            p++; n++;
        }
        if (n < 1 || n > 4) return 0;
        v <<= (4 - n) * 4;      /* left-justify to 16 bits, as Xlib does */
        out[i] = (unsigned short)v;
        if (i < 2) {
            while (*p == ' ' || *p == '\t') p++;
            if (*p != '/') return 0;
            p++;
        }
    }
    return 1;
}

static int parse_rgbi_spec(const char *spec, unsigned short out[3])
{
    const char *p = spec + 5;   /* past "rgbi:" */
    float f[3];
    for (int i = 0; i < 3; i++) {
        char *end = NULL;
        f[i] = strtof(p, &end);
        if (end == p) return 0;
        p = end;
        if (i < 2) { if (*p != '/') return 0; p++; }
    }
    for (int i = 0; i < 3; i++) {
        if (f[i] < 0) f[i] = 0;
        if (f[i] > 1) f[i] = 1;
        out[i] = (unsigned short)(f[i] * 65535.0f + 0.5f);
    }
    return 1;
}

int XParseColor(Display *d, Colormap cmap, _Xconst char *spec, XColor *def)
{
    (void)d; (void)cmap;
    if (!spec) return 0;
    int r = 0, g = 0, b = 0;
    if (spec[0] == '#') {
        int n = (int)strlen(spec + 1);
        if (n % 3) return 0;
        int per = n / 3;
        if (per < 1 || per > 4) return 0;
        unsigned v[3] = {0,0,0};
        for (int i = 0; i < n; i++) {
            int h = hexval(spec[1 + i]);
            if (h < 0) return 0;
            v[i / per] = (v[i / per] << 4) | (unsigned)h;
        }
        if (per == 1)      { r = v[0]*17; g = v[1]*17; b = v[2]*17; }
        else if (per == 2) { r = v[0];    g = v[1];    b = v[2]; }
        else if (per == 3) { r = v[0]>>4; g = v[1]>>4; b = v[2]>>4; }
        else               { r = v[0]>>8; g = v[1]>>8; b = v[2]>>8; }
        def->red = c8to16(r); def->green = c8to16(g); def->blue = c8to16(b);
        def->flags = DoRed | DoGreen | DoBlue;
        return 1;
    }
    if (spec[0] == 'r' && spec[1] == 'g' && spec[2] == 'b' && spec[3] == ':' &&
        spec[4] != 'i') {
        unsigned short v[3];
        if (parse_rgb_spec(spec, v)) {
            def->red = v[0]; def->green = v[1]; def->blue = v[2];
            def->flags = DoRed | DoGreen | DoBlue;
            return 1;
        }
    }
    if (strncasecmp(spec, "rgbi:", 5) == 0) {
        unsigned short v[3];
        if (parse_rgbi_spec(spec, v)) {
            def->red = v[0]; def->green = v[1]; def->blue = v[2];
            def->flags = DoRed | DoGreen | DoBlue;
            return 1;
        }
    }
    if (lookup_named(spec, &r, &g, &b)) {
        def->red = c8to16(r); def->green = c8to16(g); def->blue = c8to16(b);
        def->flags = DoRed | DoGreen | DoBlue;
        return 1;
    }
    /* last resorts */
    if (strcasecmp(spec, "black") == 0) { def->red=def->green=def->blue=0; def->flags=7; return 1; }
    if (strcasecmp(spec, "white") == 0) { def->red=def->green=def->blue=65535; def->flags=7; return 1; }
    return 0;
}

int XLookupColor(Display *d, Colormap cmap, _Xconst char *spec, XColor *exact,
                 XColor *screen)
{
    if (!XParseColor(d, cmap, spec, exact)) return 0;
    *screen = *exact;
    screen->pixel = ((unsigned long)(exact->red >> 8) << 16)
                  | ((unsigned long)(exact->green >> 8) << 8)
                  | (unsigned long)(exact->blue >> 8);
    return 1;
}

int XAllocNamedColor(Display *d, Colormap cmap, _Xconst char *spec,
                     XColor *screen, XColor *exact)
{
    if (!XParseColor(d, cmap, spec, exact)) return 0;
    XLookupColor(d, cmap, spec, exact, screen);
    return 1;
}

/* --------------------------------------------------------------- visuals */

XVisualInfo *XGetVisualInfo(Display *d, long vinfo_mask, XVisualInfo *vinfo,
                            int *nitems)
{
    XDisplayImpl *dp = MWD(d);
    Visual *vs[2] = { &dp->visual, &dp->visual32 };
    int vdepths[2] = { 24, 32 };
    XVisualInfo *out = calloc(2, sizeof *out);
    if (!out) { *nitems = 0; return NULL; }
    int n = 0;
    for (int i = 0; i < 2; i++) {
        Visual *v = vs[i];
        if ((vinfo_mask & VisualIDMask) && vinfo->visualid != v->visualid)
            continue;
        if ((vinfo_mask & VisualDepthMask) && vinfo->depth != vdepths[i])
            continue;
        if ((vinfo_mask & VisualClassMask) && vinfo->class != v->class)
            continue;
        out[n].visual = v;
        out[n].visualid = v->visualid;
        out[n].screen = 0;
        out[n].depth = vdepths[i];
        out[n].class = v->class;
        out[n].red_mask = v->red_mask;
        out[n].green_mask = v->green_mask;
        out[n].blue_mask = v->blue_mask;
        out[n].colormap_size = 256;
        out[n].bits_per_rgb = 8;
        n++;
    }
    if (!n) { free(out); out = NULL; }
    *nitems = n;
    return out;
}

Status XMatchVisualInfo(Display *d, int screen, int depth, int class,
                        XVisualInfo *vinfo)
{
    (void)screen;
    if ((class != TrueColor && class != 0)) return 0;
    XDisplayImpl *dp = MWD(d);
    Visual *v;
    if (depth == 24) v = &dp->visual;
    else if (depth == 32) v = &dp->visual32;
    else return 0;
    memset(vinfo, 0, sizeof *vinfo);
    vinfo->visual = v;
    vinfo->visualid = v->visualid;
    vinfo->depth = depth;
    vinfo->class = TrueColor;
    vinfo->red_mask = v->red_mask;
    vinfo->green_mask = v->green_mask;
    vinfo->blue_mask = v->blue_mask;
    vinfo->bits_per_rgb = 8;
    vinfo->colormap_size = 256;
    return 1;
}

int *XListDepths(Display *d, int scr, int *count)
{
    (void)d; (void)scr;
    int *a = malloc(2 * sizeof(int));
    a[0] = 24; a[1] = 32; *count = 2;
    return a;
}

XPixmapFormatValues *XListPixmapFormats(Display *d, int *count)
{
    (void)d;
    XPixmapFormatValues *a = malloc(2 * sizeof(XPixmapFormatValues));
    a[0].depth = 24; a[0].bits_per_pixel = 32; a[0].scanline_pad = 32;
    a[1].depth = 32; a[1].bits_per_pixel = 32; a[1].scanline_pad = 32;
    *count = 2;
    return a;
}
