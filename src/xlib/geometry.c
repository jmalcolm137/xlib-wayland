/* geometry.c — XParseGeometry / XWMGeometry. */
#include "internal.h"

#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdbool.h>

/* libX11's ParseGeom.c.  Offsets come back signed: "-10" is x == -10 with
 * both XValue and XNegative set, which is what clients (and XGeometry and
 * XWMGeometry below) test.  Anything left over makes the whole string
 * invalid (0). */
static int read_integer(const char *s, const char **next)
{
    int sign = 1, v = 0;
    if (*s == '+') s++;
    else if (*s == '-') { s++; sign = -1; }
    /* Saturate rather than overflow: no geometry needs more than 16 bits. */
    for (; *s >= '0' && *s <= '9'; s++)
        if (v < 100000) v = v * 10 + (*s - '0');
    *next = s;
    return sign * v;
}

int XParseGeometry(_Xconst char *string, int *x, int *y,
                   unsigned int *width, unsigned int *height)
{
    int mask = NoValue;
    unsigned int tw = 0, th = 0;
    int tx = 0, ty = 0;
    const char *p, *next;

    if (!string || !*string) return NoValue;
    if (*string == '=') string++;
    p = string;
    if (*p != '+' && *p != '-' && *p != 'x' && *p != 'X') {
        if (!isdigit((unsigned char)*p)) return 0;
        tw = (unsigned int)read_integer(p, &next);
        if (p == next) return 0;
        p = next;
        mask |= WidthValue;
    }
    if (*p == 'x' || *p == 'X') {
        p++;
        if (!isdigit((unsigned char)*p)) return 0;
        th = (unsigned int)read_integer(p, &next);
        if (p == next) return 0;
        p = next;
        mask |= HeightValue;
    }
    if (*p == '+' || *p == '-') {
        if (*p == '-') mask |= XNegative;
        bool neg = *p == '-';
        p++;
        if (!isdigit((unsigned char)*p)) return 0;
        tx = read_integer(p, &next);
        if (p == next) return 0;
        if (neg) tx = -tx;
        p = next;
        mask |= XValue;
        if (*p == '+' || *p == '-') {
            if (*p == '-') mask |= YNegative;
            neg = *p == '-';
            p++;
            if (!isdigit((unsigned char)*p)) return 0;
            ty = read_integer(p, &next);
            if (p == next) return 0;
            if (neg) ty = -ty;
            p = next;
            mask |= YValue;
        }
    }
    if (*p != '\0') return 0;
    if ((mask & XValue) && x) *x = tx;
    if ((mask & YValue) && y) *y = ty;
    if ((mask & WidthValue) && width) *width = tw;
    if ((mask & HeightValue) && height) *height = th;
    return mask;
}

/* Return the pixel geometry a client should use, combining a user-specified
 * geometry with a fully-qualified default.  This is the reference algorithm
 * from Xlib's Geom.c: fwidth/fheight are the size of one position unit (1 for
 * the pixel units every toolkit passes), and a negative offset is measured
 * from the far edge of the screen. */
int XGeometry(Display *dpy, int screen, _Xconst char *pos, _Xconst char *def,
              unsigned int bwidth, unsigned int fwidth, unsigned int fheight,
              int xadd, int yadd, int *x, int *y, int *width, int *height)
{
    int px = 0, py = 0, dx = 0, dy = 0;
    unsigned int pwidth = 0, pheight = 0, dwidth = 0, dheight = 0;
    int pmask = XParseGeometry(pos, &px, &py, &pwidth, &pheight);
    int dmask = XParseGeometry(def, &dx, &dy, &dwidth, &dheight);

    Screen *scr = (dpy && screen >= 0 && screen < ScreenCount(dpy))
                  ? ScreenOfDisplay(dpy, screen) : NULL;
    int sw = scr ? scr->width : MW_DEFAULT_W;
    int sh = scr ? scr->height : MW_DEFAULT_H;

    *x = (dmask & XNegative)
         ? sw + dx - (int)(dwidth * fwidth) - 2 * (int)bwidth - xadd : dx;
    *y = (dmask & YNegative)
         ? sh + dy - (int)(dheight * fheight) - 2 * (int)bwidth - yadd : dy;
    *width = (int)dwidth;
    *height = (int)dheight;

    if (pmask & WidthValue)  *width = (int)pwidth;
    if (pmask & HeightValue) *height = (int)pheight;

    if (pmask & XValue)
        *x = (pmask & XNegative)
             ? sw + px - *width * (int)fwidth - 2 * (int)bwidth - xadd : px;
    if (pmask & YValue)
        *y = (pmask & YNegative)
             ? sh + py - *height * (int)fheight - 2 * (int)bwidth - yadd : py;

    return pmask;
}

/* libX11's WMGeom.c.  Sizes are counted in resize increments over the base
 * size (Xt's Shell passes "80x24" for an 80-column window), and a negative
 * position is measured from the far edge and picks the matching gravity.
 * Only the user's geometry sets XValue/YValue in the result; a default
 * position contributes just its XNegative/YNegative. */
int XWMGeometry(Display *d, int screen, _Xconst char *user_geom,
                _Xconst char *def_geom, unsigned int bwidth,
                XSizeHints *hints, int *x_ret, int *y_ret,
                int *width_ret, int *height_ret, int *gravity_ret)
{
    Screen *scr = (d && screen >= 0 && screen < ScreenCount(d))
                  ? ScreenOfDisplay(d, screen) : NULL;
    int sw = scr ? scr->width : MW_DEFAULT_W;
    int sh = scr ? scr->height : MW_DEFAULT_H;
    XSizeHints none;
    if (!hints) { memset(&none, 0, sizeof none); hints = &none; }

    /* ICCCM 4.1.2.3: base and minimum sizes default to each other. */
    int base_w = 0, base_h = 0, min_w = 0, min_h = 0, inc_w = 1, inc_h = 1;
    if (hints->flags & PBaseSize) {
        base_w = hints->base_width;  base_h = hints->base_height;
    } else if (hints->flags & PMinSize) {
        base_w = hints->min_width;   base_h = hints->min_height;
    }
    if (hints->flags & PMinSize) {
        min_w = hints->min_width;    min_h = hints->min_height;
    } else if (hints->flags & PBaseSize) {
        min_w = hints->base_width;   min_h = hints->base_height;
    }
    if (hints->flags & PResizeInc) {
        inc_w = hints->width_inc;    inc_h = hints->height_inc;
    }

    int ux = 0, uy = 0, dx = 0, dy = 0;
    unsigned int uw = 0, uh = 0, dw = 0, dh = 0;
    int umask = XParseGeometry(user_geom, &ux, &uy, &uw, &uh);
    int dmask = XParseGeometry(def_geom, &dx, &dy, &dw, &dh);
    int rmask = umask;

    int rw = (int)((umask & WidthValue) ? uw : (dmask & WidthValue) ? dw : 1)
             * inc_w + base_w;
    int rh = (int)((umask & HeightValue) ? uh : (dmask & HeightValue) ? dh : 1)
             * inc_h + base_h;
    if (rw < min_w) rw = min_w;
    if (rh < min_h) rh = min_h;
    if (hints->flags & PMaxSize) {
        if (rw > hints->max_width)  rw = hints->max_width;
        if (rh > hints->max_height) rh = hints->max_height;
    }

    int rx, ry;
    if (umask & XValue) {
        rx = (umask & XNegative) ? sw + ux - rw - 2 * (int)bwidth : ux;
    } else if (dmask & XValue) {
        if (dmask & XNegative) {
            rx = sw + dx - rw - 2 * (int)bwidth;
            rmask |= XNegative;
        } else {
            rx = dx;
        }
    } else {
        rx = 0;
    }
    if (umask & YValue) {
        ry = (umask & YNegative) ? sh + uy - rh - 2 * (int)bwidth : uy;
    } else if (dmask & YValue) {
        if (dmask & YNegative) {
            ry = sh + dy - rh - 2 * (int)bwidth;
            rmask |= YNegative;
        } else {
            ry = dy;
        }
    } else {
        ry = 0;
    }

    int grav;
    if (rmask & XNegative)
        grav = (rmask & YNegative) ? SouthEastGravity : NorthEastGravity;
    else
        grav = (rmask & YNegative) ? SouthWestGravity : NorthWestGravity;

    if (x_ret) *x_ret = rx;
    if (y_ret) *y_ret = ry;
    if (width_ret) *width_ret = rw;
    if (height_ret) *height_ret = rh;
    if (gravity_ret) *gravity_ret = grav;
    return rmask;
}
