/* geometry.c — XParseGeometry / XWMGeometry. */
#include "internal.h"

#include <stdlib.h>
#include <string.h>
#include <ctype.h>

int XParseGeometry(_Xconst char *string, int *x, int *y,
                   unsigned int *width, unsigned int *height)
{
    int mask = NoValue;
    const char *str = string;
    char *p;
    long v;

    if (!str) return NoValue;
    p = (char *)str;

    /* [=][<width>{xX}<height>][{+-}<xoffset>{+-}<yoffset>] */
    if (*p == '=') p++;
    if (isdigit((unsigned char)*p)) {
        v = strtol(p, &p, 10);
        if (width) *width = (unsigned int)v;
        mask |= WidthValue;
        if (*p == 'x' || *p == 'X') {
            p++;
            v = strtol(p, &p, 10);
            if (height) *height = (unsigned int)v;
            mask |= HeightValue;
        }
    }
    if (*p == '+' || *p == '-') {
        int neg = (*p == '-');
        p++;
        v = strtol(p, &p, 10);
        if (x) *x = (int)v;
        mask |= neg ? XNegative : XValue;
        if (*p == '+' || *p == '-') {
            neg = (*p == '-');
            p++;
            v = strtol(p, &p, 10);
            if (y) *y = (int)v;
            mask |= neg ? YNegative : YValue;
        }
    }
    return mask;
}

int XWMGeometry(Display *d, int screen, _Xconst char *user_geom,
                _Xconst char *def_geom, unsigned int bwidth,
                XSizeHints *hints, int *x_ret, int *y_ret,
                int *width_ret, int *height_ret, int *gravity_ret)
{
    Screen *scr = (d && screen >= 0) ? MWSCR(d) : NULL;
    int sw = scr ? scr->width : MW_DEFAULT_W;
    int sh = scr ? scr->height : MW_DEFAULT_H;

    int x = 0, y = 0;
    unsigned int w = 0, h = 0;
    int mask = XParseGeometry(user_geom ? user_geom : "", &x, &y, &w, &h);
    if (!(mask & WidthValue) || !(mask & HeightValue)) {
        int dx = 0, dy = 0; unsigned int dw = 0, dh = 0;
        XParseGeometry(def_geom ? def_geom : "", &dx, &dy, &dw, &dh);
        if (!(mask & WidthValue) && (dw)) { w = dw; }
        if (!(mask & HeightValue) && (dh)) { h = dh; }
        if (!(mask & XValue) && !(mask & XNegative)) { x = dx; mask |= XValue; }
        if (!(mask & YValue) && !(mask & YNegative)) { y = dy; mask |= YValue; }
    }
    if (w == 0) w = hints && (hints->flags & PSize) ? (unsigned)hints->width : 200;
    if (h == 0) h = hints && (hints->flags & PSize) ? (unsigned)hints->height : 200;

    /* Apply user size hints if the request was not explicit. */
    if (hints && (hints->flags & PSize)) {
        if (!(mask & WidthValue)) w = (unsigned)hints->width;
        if (!(mask & HeightValue)) h = (unsigned)hints->height;
        if (hints->flags & PMinSize) {
            if ((int)w < hints->min_width) w = (unsigned)hints->min_width;
            if ((int)h < hints->min_height) h = (unsigned)hints->min_height;
        }
        if (hints->flags & PMaxSize) {
            if ((int)w > hints->max_width) w = (unsigned)hints->max_width;
            if ((int)h > hints->max_height) h = (unsigned)hints->max_height;
        }
    }

    int grav = NorthWestGravity;
    if (mask & XNegative) x = sw - (int)w - x - 2 * (int)bwidth;
    if (mask & YNegative) y = sh - (int)h - y - 2 * (int)bwidth;

    if (x_ret) *x_ret = x;
    if (y_ret) *y_ret = y;
    if (width_ret) *width_ret = (int)w;
    if (height_ret) *height_ret = (int)h;
    if (gravity_ret) *gravity_ret = grav;
    return mask | WidthValue | HeightValue;
}
