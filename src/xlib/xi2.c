/* xi2.c — a minimal XInput2 surface so libXi clients can run.
 *
 * The shim speaks no wire protocol, so an extension library cannot be answered
 * by the server.  libXi builds its requests through _XGetRequest and then waits
 * in _XReply, so we recognise its major opcode there and synthesise the reply
 * the library expects.  The query path is what a client such as xinput needs:
 * the extension version, XIQueryVersion, XIQueryDevice and the property
 * listings.  It reports one master pointer/keyboard pair plus an XTEST slave
 * pair -- the shape a client expects from a real server -- with no device
 * classes, which is all the query code reads.
 */
#include "internal.h"

#include <stdlib.h>
#include <string.h>

#define MW_XI_NAME        "XInputExtension"
#define MW_XI_OPCODE      131
#define MW_XI_FIRST_EVENT 70
#define MW_XI_FIRST_ERROR 137

/* Minor request codes (XInput2). */
#define MW_XI_GET_EXT_VERSION 1
#define MW_XI_QUERY_VERSION   47
#define MW_XI_QUERY_DEVICE    48
#define MW_XI_LIST_PROPERTIES 56
#define MW_XI_GET_PROPERTY    59

/* XInput2 device uses.  These are the values the protocol actually carries
 * (checked against a real server): XIMasterPointer is 1, not 0. */
#define MW_XI_MASTER_POINTER  1
#define MW_XI_MASTER_KEYBOARD 2
#define MW_XI_SLAVE_POINTER   3
#define MW_XI_SLAVE_KEYBOARD  4

struct mw_xi_device {
    unsigned short id;
    unsigned short use;
    unsigned short attachment;
    unsigned char  enabled;
    const char    *name;
};

static const struct mw_xi_device xi_devices[] = {
    { 2, MW_XI_MASTER_POINTER,  3, 1, "Virtual core pointer"    },
    { 3, MW_XI_MASTER_KEYBOARD, 2, 1, "Virtual core keyboard"   },
    { 4, MW_XI_SLAVE_POINTER,   2, 1, "Virtual core XTEST pointer"  },
    { 5, MW_XI_SLAVE_KEYBOARD,  3, 1, "Virtual core XTEST keyboard" },
};
#define MW_XI_NDEVICES ((int)(sizeof xi_devices / sizeof xi_devices[0]))

/* A reply is 32 bytes: type, subtype, sequence, length, then 24 bytes of
 * payload.  Writing at explicit offsets keeps the layout obvious. */
static void put16(unsigned char *p, int off, unsigned v)
{ unsigned short x = (unsigned short)v; memcpy(p + off, &x, 2); }
static void put32(unsigned char *p, int off, unsigned v)
{ unsigned int x = v; memcpy(p + off, &x, 4); }

Bool mw_xi2_query_extension(_Xconst char *name, int *major,
                            int *first_event, int *first_error)
{
    if (!name || strcmp(name, MW_XI_NAME) != 0) return False;
    if (major)       *major = MW_XI_OPCODE;
    if (first_event) *first_event = MW_XI_FIRST_EVENT;
    if (first_error) *first_error = MW_XI_FIRST_ERROR;
    return True;
}

const char *mw_xi2_extension_name(void) { return MW_XI_NAME; }

static void build_device_info(Display *d, int filter, int *count, size_t *len)
{
    XDisplayImpl *dp = MWD(d);
    size_t total = 0;
    int n = 0;
    for (int i = 0; i < MW_XI_NDEVICES; i++) {
        if (filter && xi_devices[i].id != filter) continue;
        total += 12 + ((strlen(xi_devices[i].name) + 3) & ~(size_t)3);
        n++;
    }
    unsigned char *buf = calloc(1, total ? total : 1);
    size_t o = 0;
    if (buf) {
        for (int i = 0; i < MW_XI_NDEVICES; i++) {
            if (filter && xi_devices[i].id != filter) continue;
            int nl = (int)strlen(xi_devices[i].name);
            put16(buf, (int)o + 0, xi_devices[i].id);
            put16(buf, (int)o + 2, xi_devices[i].use);
            put16(buf, (int)o + 4, xi_devices[i].attachment);
            put16(buf, (int)o + 6, 0);      /* num_classes */
            put16(buf, (int)o + 8, nl);     /* name_len */
            buf[o + 10] = xi_devices[i].enabled;
            o += 12;                        /* sizeof xXIDeviceInfo */
            memcpy(buf + o, xi_devices[i].name, nl);
            o += (nl + 3) & ~(size_t)3;
        }
    }
    *count = n;
    *len = total;
    dp->xi2_data = buf;
    dp->xi2_data_len = total;
    dp->xi2_data_off = 0;
}

Bool mw_xi2_reply(Display *d, void *repbuf)
{
    XDisplayImpl *dp = MWD(d);
    unsigned char *req = dp->xi2_req;
    if (!req || req[0] != MW_XI_OPCODE) return False;
    unsigned char *rep = repbuf;
    memset(rep, 0, 32);
    unsigned minor = req[1];
    rep[0] = 1;              /* X_Reply */
    rep[1] = (unsigned char)minor;
    dp->xi2_req = NULL;      /* answered */
    if (getenv("MW_TRACE"))
        fprintf(stderr, "MW: XI2 request minor=%u -> reply\n", minor);

    switch (minor) {
    case MW_XI_GET_EXT_VERSION:      /* legacy XInput version query */
        put16(rep, 8, 2);            /* major_version */
        put16(rep, 10, 4);           /* minor_version */
        rep[12] = 1;                 /* present */
        return True;
    case MW_XI_QUERY_VERSION:
        put16(rep, 8, 2);
        put16(rep, 10, 4);
        return True;
    case MW_XI_QUERY_DEVICE: {
        /* xXIQueryDeviceReq: reqType, ReqType, length, then deviceid. */
        int filter = (int)req[4] | ((int)req[5] << 8);
        int count = 0; size_t len = 0;
        build_device_info(d, filter, &count, &len);
        if (getenv("MW_TRACE"))
            fprintf(stderr, "MW: XIQueryDevice filter=%d -> %d devices, %zu bytes\n",
                    filter, count, len);
        put16(rep, 8, count);                 /* num_devices */
        put32(rep, 4, (unsigned)(len / 4));   /* reply length */
        return True;
    }
    case MW_XI_LIST_PROPERTIES:
        put16(rep, 8, 0);                     /* num_properties */
        put32(rep, 4, 0);
        return True;
    case MW_XI_GET_PROPERTY:
        put32(rep, 4, 0);                     /* no such property */
        return True;
    default:
        return True;                          /* empty reply, no data */
    }
}

/* Serve the variable-length payload of a synthesised reply.  Returns -1 when
 * the pending data is not ours, so the caller can fall back. */
int mw_xi2_read(Display *d, char *data, size_t size)
{
    XDisplayImpl *dp = MWD(d);
    if (!dp->xi2_data) return -1;
    size_t avail = dp->xi2_data_len - dp->xi2_data_off;
    size_t n = size < avail ? size : avail;
    if (n) memcpy(data, dp->xi2_data + dp->xi2_data_off, n);
    if (n < size) memset(data + n, 0, size - n);
    dp->xi2_data_off += n;
    if (dp->xi2_data_off >= dp->xi2_data_len) {
        free(dp->xi2_data);
        dp->xi2_data = NULL;
        dp->xi2_data_len = dp->xi2_data_off = 0;
    }
    return (int)size;
}

void mw_xi2_forget_request(Display *d)
{
    XDisplayImpl *dp = MWD(d);
    dp->xi2_req = NULL;
    if (dp->xi2_data) {
        free(dp->xi2_data);
        dp->xi2_data = NULL;
        dp->xi2_data_len = dp->xi2_data_off = 0;
    }
}
