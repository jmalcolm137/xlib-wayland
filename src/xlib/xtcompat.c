/* xtcompat.c — small compatibility symbols that upstream libraries expect
 * but modern upstream no longer provides.
 *
 * `XVaCreateNestedList` was an Xlib/Xt helper (an X11R5-era entry point) that
 * built an `ArgList` from varargs.  It was removed from modern libXt, but Open
 * Motif's XmIm.c still calls it.  We provide it here so that Motif's libraries
 * link and run against stock libXt.  The returned list must be freed by the
 * caller (Xm does free it).
 */
#include "internal.h"

#include <stdlib.h>
#include <stdarg.h>

/* Matches Xt's Arg: { String name; XtArgVal value; } with XtArgVal == long. */
typedef struct { char *name; long value; } MwArg;

void *XVaCreateNestedList(int dummy, ...)
{
    (void)dummy;
    va_list ap;
    va_start(ap, dummy);

    size_t cap = 8, n = 0;
    MwArg *list = malloc(cap * sizeof(MwArg));
    char *name;
    while ((name = va_arg(ap, char *)) != NULL) {
        long value = va_arg(ap, long);
        if (n + 2 > cap) {
            cap *= 2;
            list = realloc(list, cap * sizeof(MwArg));
        }
        list[n].name = name;
        list[n].value = value;
        n++;
    }
    va_end(ap);

    if (n + 1 > cap) list = realloc(list, (n + 1) * sizeof(MwArg));
    list[n].name = NULL;
    list[n].value = 0;
    return list;
}
