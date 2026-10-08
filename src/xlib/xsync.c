/* xsync.c — counter ids for the libXext facade's Sync implementation.
 *
 * The shim has no server clock to synchronize against, so counters are just
 * unique ids the client can store in a property; there is nothing to wait on. */
#include "internal.h"

unsigned long mw_xsync_new_counter(Display *d)
{
    return (unsigned long)mw_alloc_id(d);
}
