/**
 * Copyright (C) 2026 Jabez Winston
 *
 * SPDX-License-Identifier: MIT
 *
 * Date : 04-June-2026
 *
 * mtp.c - USB MTP (Media Transfer Protocol) v1.1 device class.
 *
 * Built ONLY on the public API (usbip_device.h / classes/mtp.h). Mirrors the Python
 * classes/device/mtp.py. Implements the PTP bulk container protocol (command ->
 * optional data -> response) + an interrupt IN for events, and the MTP object
 * model: device-global ObjectHandles over one or more storages (one per backend),
 * object properties, rename/move/copy and the Android partial/edit extensions. All
 * storage I/O is delegated to the per-storage backend callbacks in mtp_opts - this
 * file does NO filesystem access (see examples/device/mtp_device.c).
 */
#include <ctype.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "classes/mtp.h"

/* mtp_func is this class's public handle: the function, retyped so the compiler
 * can tell it from another class's handle. */
static inline mtp_func *handle_of(usbip_function *func)
{
    return (mtp_func *)func;
}

/* Interface subclass/protocol (USB Still Image Capture 1.0 Sec.3), on the
 * ::USB_CLASS_IMAGE PTP triple. Only the MS-OS "MTP" Compatible ID tells them apart. */
#define PTP_SUBCLASS 0x01 /* Still Image Capture Device */
#define PTP_PROTOCOL 0x01 /* Bulk-Only / PIMA 15740 */

#define MTP_EP_OUT  0x01
#define MTP_EP_IN   0x81
#define MTP_EP_INTR 0x82

/* Class-specific control requests (PIMA 15740 / USB Still Image Sec.5.2) */
#define MTP_REQ_CANCEL            0x64  /* abort the transfer in flight */
#define MTP_REQ_DEVICE_RESET      0x66  /* drop the session and any partial transfer */
#define MTP_REQ_GET_DEVICE_STATUS 0x67  /* how are you: answered with the OK code below */

/* Device Status response: a 4-byte wLength + wCode pair, both little-endian. */
#define MTP_STATUS_LEN 4       /* wLength: the whole response, including itself */
/* wCode of the Device Status response: RC_OK, i.e. nothing pending */

/* container types */
#define CT_COMMAND   1
#define CT_DATA      2
#define CT_RESPONSE  3
#define CT_EVENT     4

/* Operation codes: every operation we implement, listed once. Listing one here IS
 * defining and naming it -- this one line gives the constant below its value, the
 * supported-operations array GetDeviceInfo advertises its entry (in this order, so
 * the order is part of the wire), and op_name() its log text. Mirrors _OP_NAMES in
 * the Python classes/device/mtp.py. */
#define MTP_OP_LIST(X)                       \
    X(OP_GET_DEVICE_INFO,            0x1001) \
    X(OP_OPEN_SESSION,               0x1002) \
    X(OP_CLOSE_SESSION,              0x1003) \
    X(OP_GET_STORAGE_IDS,            0x1004) \
    X(OP_GET_STORAGE_INFO,           0x1005) \
    X(OP_GET_NUM_OBJECTS,            0x1006) \
    X(OP_GET_OBJECT_HANDLES,         0x1007) \
    X(OP_GET_OBJECT_INFO,            0x1008) \
    X(OP_GET_OBJECT,                 0x1009) \
    X(OP_DELETE_OBJECT,              0x100B) \
    X(OP_SEND_OBJECT_INFO,           0x100C) \
    X(OP_SEND_OBJECT,                0x100D) \
    X(OP_GET_PARTIAL_OBJECT,         0x101B) \
    X(OP_GET_DEVICE_PROP_DESC,       0x1014) \
    X(OP_GET_DEVICE_PROP_VALUE,      0x1015) \
    X(OP_SET_DEVICE_PROP_VALUE,      0x1016) \
    X(OP_GET_OBJECT_PROPS_SUPPORTED, 0x9801) \
    X(OP_GET_OBJECT_PROP_DESC,       0x9802) \
    X(OP_GET_OBJECT_PROP_VALUE,      0x9803) \
    X(OP_GET_OBJECT_PROP_LIST,       0x9805) \
    X(OP_GET_OBJECT_REFERENCES,      0x9810) \
    X(OP_MOVE_OBJECT,                0x1019) \
    X(OP_COPY_OBJECT,                0x101A) \
    X(OP_SET_OBJECT_PROP_VALUE,      0x9804) \
    X(OP_GET_PARTIAL_OBJECT_64,      0x95C1) \
    X(OP_SEND_PARTIAL_OBJECT,        0x95C2) \
    X(OP_TRUNCATE_OBJECT,            0x95C3) \
    X(OP_BEGIN_EDIT_OBJECT,          0x95C4) \
    X(OP_END_EDIT_OBJECT,            0x95C5)

/* The only enum in this file, and it declares nothing: it exists so the list above
 * can hand each opcode its value. Everything else here stays a #define. */
enum
{
#define MTP_OP_DEFINE(sym, code) sym = code,
    MTP_OP_LIST(MTP_OP_DEFINE)
#undef MTP_OP_DEFINE
};

/* response codes */
#define RC_OK                         0x2001
#define RC_GENERAL_ERROR              0x2002
#define RC_SESSION_NOT_OPEN           0x2003
#define RC_OPERATION_NOT_SUPPORTED    0x2005
#define RC_INVALID_STORAGE_ID         0x2008
#define RC_INVALID_OBJECT_HANDLE      0x2009
#define RC_DEVICEPROP_NOT_SUPPORTED   0x200A
#define RC_STORE_READ_ONLY            0x200E
#define RC_INVALID_PARENT             0x201A
#define RC_INVALID_PARAMETER          0x201D
#define RC_SESSION_ALREADY_OPEN       0x201E
#define RC_NO_VALID_OBJECT_INFO       0x2015
#define RC_INVALID_OBJECT_PROP_CODE   0xA801
#define RC_OBJECT_PROP_NOT_SUPPORTED  0xA80A

/* events pushed to the host over the interrupt IN */
#define EV_OBJECT_ADDED    0x4002
#define EV_OBJECT_REMOVED  0x4003
#define EV_STORE_ADDED     0x4004

/* object formats */
#define FMT_UNDEFINED    0x3000   /* a plain file */
#define FMT_ASSOCIATION  0x3001   /* a directory */
#define FMT_TEXT         0x3004

/* property datatype codes */
#define T_U8    0x0002
#define T_U16   0x0004
#define T_U32   0x0006
#define T_U64   0x0008
#define T_U128  0x000A
#define T_STR   0xFFFF            /* length-prefixed UTF-16 string */

/* device property codes */
#define DPC_SYNC_PARTNER   0xD401
#define DPC_FRIENDLY_NAME  0xD402

/* object property codes */
#define OPC_STORAGE_ID     0xDC01
#define OPC_OBJECT_FORMAT  0xDC02
#define OPC_PROTECTION     0xDC03
#define OPC_OBJECT_SIZE    0xDC04
#define OPC_FILENAME       0xDC07
#define OPC_DATE_MODIFIED  0xDC09
#define OPC_PARENT         0xDC0B
#define OPC_PUID           0xDC41
#define OPC_NAME           0xDC44

#define ALL 0xFFFFFFFFu
#define STORAGE_ID(si) (((uint32_t)((si) + 1) << 16) | 0x0001u)

static const uint16_t PROP_LIST[] = {
    OPC_STORAGE_ID,
    OPC_OBJECT_FORMAT,
    OPC_PROTECTION,
    OPC_OBJECT_SIZE,
    OPC_FILENAME,
    OPC_DATE_MODIFIED,
    OPC_PARENT,
    OPC_PUID,
    OPC_NAME,
};
#define N_PROPS ((int)(sizeof(PROP_LIST) / sizeof(PROP_LIST[0])))

/* The supported-operations array GetDeviceInfo advertises, in MTP_OP_LIST order. */
static const uint16_t DEV_OPS[] = {
#define MTP_OP_ENTRY(sym, code) code,
    MTP_OP_LIST(MTP_OP_ENTRY)
#undef MTP_OP_ENTRY
};

/* The op -> log text table: the text is the MTP_OP_LIST symbol with OP_ stripped and
 * the words CamelCased, PTP's own spelling ("GetDeviceInfo"). */
static struct
{
    uint16_t op;
    char name[32];   /* holds the stringized macro; a longer one fails to compile */
} OP_NAMES[] = {
#define MTP_OP_ENTRY(sym, code) { code, #sym },
    MTP_OP_LIST(MTP_OP_ENTRY)
#undef MTP_OP_ENTRY
};

static const char *op_name(uint16_t op)
{
    for (size_t i = 0; i < sizeof(OP_NAMES) / sizeof(OP_NAMES[0]); i++)
    {
        char *name = OP_NAMES[i].name;
        if (OP_NAMES[i].op != op)
            continue;
        if (strncmp(name, "OP_", 3) == 0)
        {
            /* CamelCase the stringized macro in place, dropping the prefix:
             * OP_GET_DEVICE_INFO -> GetDeviceInfo. The result is shorter than the
             * source, so the write cursor never overtakes the read cursor. A
             * converted name no longer starts with OP_, which is what the guard
             * above tests -- the conversion itself is not repeatable. */
            char *dst = name;
            for (const char *src = name + 3; *src; src++)
            {
                if (*src == '_')
                    continue;
                int word_start = src == name + 3 || src[-1] == '_';
                int upper = *src >= 'A' && *src <= 'Z';
                *dst++ = !word_start && upper ? (char)(*src - 'A' + 'a') : *src;
            }
            *dst = '\0';
        }
        return name;
    }
    return "?";   /* every caller passes an op from the list above */
}

/* ---- object table: device-global handles over n storages ---- */
struct obj {
    uint32_t handle, parent;
    uint64_t size;
    uint32_t mtime;
    uint16_t fmt;
    int      is_dir, alive;
    int      si;                /* store index */
    uint32_t storage;
    char     path[4096];        /* relative to its store's root; "" = that root */
    char     name[256];
};

struct mtp_state {
    mtp_opts ops;               /* backend callbacks + per-storage contexts + flags */
    int      read_only;
    char     model[64], manufacturer[64], serial[40], friendly[64], sync_partner[64];
    struct obj *objs;
    int      n_objs, cap_objs;
    uint32_t next_handle;
    uint32_t send_target;       /* handle reserved by SendObjectInfo (0 = none) */
    uint32_t session;
    /* pending data-out phase */
    int      rx_active;
    uint16_t rx_code;
    uint32_t rx_txid, rx_p[5];
    uint8_t *rx_buf;
    size_t   rx_len, rx_cap;
    long     rx_need;
    /* The pipes this function was assigned. A composite relocates them off MTP_EP_*,
     * so keep the objects: a stale lookup silently drops every response. */
    usbip_ep *in, *out, *intr;
};

static void *store_ctx(struct mtp_state *st, int si)
{
    return st->ops.stores[si];
}

static int si_of(struct mtp_state *st, uint32_t storage_id)
{
    for (int i = 0; i < st->ops.n_stores; i++)
        if (STORAGE_ID(i) == storage_id)
            return i;
    return -1;
}

static void mtp_log(struct mtp_state *st, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    usbip_class_vlog(st->ops.on_event, st->ops.user, NULL, fmt, ap);
    va_end(ap);
}

static uint16_t fmt_of(const char *name)
{
    const char *dot = strrchr(name, '.');
    if (!dot)
        return FMT_UNDEFINED;
    char ext[8];
    int i = 0;
    for (const char *cursor = dot + 1; *cursor && i < 7; cursor++)
        ext[i++] = (char)tolower((unsigned char)*cursor);
    ext[i] = 0;
    static const struct
    {
        const char *ext;
        uint16_t fmt;
    } map[] = {
        {"txt", FMT_TEXT},
        {"htm", 0x3005},
        {"html", 0x3005},
        {"wav", 0x3008},
        {"mp3", 0x3009},
        {"avi", 0x300A},
        {"mpg", 0x300B},
        {"mpeg", 0x300B},
        {"jpg", 0x3801},
        {"jpeg", 0x3801},
        {"bmp", 0x3804},
        {"gif", 0x3807},
        {"png", 0x380B},
        {"tif", 0x380D},
        {"tiff", 0x380D},
        {"wma", 0xB901},
        {"ogg", 0xB902},
        {"aac", 0xB903},
        {"flac", 0xB906},
        {"wmv", 0xB981},
        {"mp4", 0xB982},
        {"m4a", 0xB982},
        {"3gp", 0xB984},
        {"xml", 0xBA82},
    };
    for (size_t k = 0; k < sizeof(map) / sizeof(map[0]); k++)
        if (strcmp(ext, map[k].ext) == 0)
            return map[k].fmt;
    return FMT_UNDEFINED;
}

static void puid_of(int si, const char *path, uint8_t out[16])
{
    uint64_t h1 = 1469598103934665603ULL ^ (uint64_t)si;
    for (const char *path_bytes = path; *path_bytes; path_bytes++)
    {
        h1 ^= (uint8_t)*path_bytes;
        h1 *= 1099511628211ULL;
    }
    uint64_t h2 = h1 ^ 0x9e3779b97f4a7c15ULL;
    for (const char *path_bytes = path; *path_bytes; path_bytes++)
        h2 = (h2 << 5) + h2 + (uint8_t)*path_bytes;
    usb_put_le64(out, h1);        /* fixed byte order, so a PUID does not depend */
    usb_put_le64(out + 8, h2);    /* on the host the device happens to run on */
}

static struct obj *obj_by_path(struct mtp_state *st, int si, const char *path)
{
    for (int i = 0; i < st->n_objs; i++)
        if (st->objs[i].si == si && strcmp(st->objs[i].path, path) == 0)
            return &st->objs[i];
    return NULL;
}
static struct obj *obj_by_handle(struct mtp_state *st, uint32_t handle)
{
    for (int i = 0; i < st->n_objs; i++)
        if (st->objs[i].alive && st->objs[i].handle == handle)
            return &st->objs[i];
    return NULL;
}

/* dir + "/" + name -> dst; an entry under the store root joins without a slash */
static void path_join(char *dst, size_t sz, const char *dir, const char *name)
{
    if (dir[0])
        snprintf(dst, sz, "%s/%s", dir, name);
    else
        snprintf(dst, sz, "%s", name);
}

/* Parse a PTP length-prefixed UTF-16LE string (count byte, then wide chars) into
 * ASCII: low bytes only, stopping at the count, a NUL, the output size or the
 * `n` input bytes available. */
static void ptp_str_ascii(const uint8_t *raw, int len, char *out, size_t outsz)
{
    int nc = len > 0 ? raw[0] : 0;
    int j = 0;
    for (int i = 0; i < nc && j < (int)outsz - 1 && 1 + 2 * i < len; i++)
    {
        uint8_t lo = raw[1 + 2 * i];
        if (!lo)
            break;
        out[j++] = (char)lo;
    }
    out[j] = 0;
}

static struct obj *obj_upsert(struct mtp_state *st, int si, const char *full, const char *name,
                              uint32_t parent, int is_dir, uint64_t size, uint32_t mtime)
{
    struct obj *obj = obj_by_path(st, si, full);
    if (!obj)
    {
        if (st->n_objs == st->cap_objs)
        {
            st->cap_objs = st->cap_objs ? st->cap_objs * 2 : 64;
            st->objs = realloc(st->objs, (size_t)st->cap_objs * sizeof(*st->objs));
        }
        obj = &st->objs[st->n_objs++];
        memset(obj, 0, sizeof(*obj));
        obj->handle = st->next_handle++;
        obj->si = si;
        obj->storage = STORAGE_ID(si);
        snprintf(obj->path, sizeof(obj->path), "%s", full);
    }
    snprintf(obj->name, sizeof(obj->name), "%s", name);
    obj->parent = parent;
    obj->is_dir = is_dir;
    obj->size = is_dir ? 0 : size;
    obj->mtime = mtime;
    obj->fmt = is_dir ? FMT_ASSOCIATION : fmt_of(name);
    obj->alive = 1;
    return obj;
}

/* collect listdir names (via the backend's emit callback) for sorting */
struct namelist
{
    char (*names)[256];
    int n, cap;
};

static void collect_name(void *ctx, const char *name)
{
    struct namelist *nl = ctx;
    if (nl->n == nl->cap)
    {
        nl->cap = nl->cap ? nl->cap * 2 : 32;
        nl->names = realloc(nl->names, (size_t)nl->cap * 256);
    }
    snprintf(nl->names[nl->n++], 256, "%s", name);
}

static int cmp_name(const void *lhs, const void *rhs)
{
    return strcmp(lhs, rhs);
}

static void scan_dir(struct mtp_state *st, int si, const char *relpath, uint32_t parent)
{
    struct namelist nl = {0};
    st->ops.listdir(store_ctx(st, si), relpath, collect_name, &nl);
    qsort(nl.names, (size_t)nl.n, 256, cmp_name);
    for (int i = 0; i < nl.n; i++)
    {
        char full[4096]; /* same size as obj.path */
        path_join(full, sizeof(full), relpath, nl.names[i]);
        int is_dir;
        uint64_t size;
        uint32_t mtime;
        if (st->ops.get_info(store_ctx(st, si), full, &is_dir, &size, &mtime) != 0)
            continue;
        struct obj *obj = obj_upsert(st, si, full, nl.names[i], parent, is_dir, size, mtime);
        if (is_dir)
            scan_dir(st, si, full, obj->handle);
    }
    free(nl.names);
}

static void rescan(struct mtp_state *st)
{
    for (int i = 0; i < st->n_objs; i++)
        st->objs[i].alive = 0;
    for (int si = 0; si < st->ops.n_stores; si++)
        scan_dir(st, si, "", 0);
}

/* resolve a write target (store_index + dir relpath) under (storage_id, parent) */
static const char *target(struct mtp_state *st, uint32_t storage_id, uint32_t parent, int *out_si)
{
    if (parent == 0 || parent == ALL)
    {
        int si = si_of(st, storage_id);
        *out_si = si >= 0 ? si : 0;
        return "";
    }
    struct obj *obj = obj_by_handle(st, parent);
    if (obj && obj->is_dir)
    {
        *out_si = obj->si;
        return obj->path;
    }
    return NULL;
}

static int is_descendant(struct mtp_state *st, uint32_t handle, uint32_t ancestor)
{
    struct obj *obj = obj_by_handle(st, handle);
    while (obj && obj->parent)
    {
        if (obj->parent == ancestor)
            return 1;
        obj = obj_by_handle(st, obj->parent);
    }
    return 0;
}

/* ---- growable byte buffer for datasets ---- */
struct buf
{
    uint8_t *p;
    size_t len, cap;
};

static void bput(struct buf *buf, const void *data, size_t len)
{
    if (buf->len + len > buf->cap)
    {
        buf->cap = (buf->len + len) * 2 + 64;
        buf->p = realloc(buf->p, buf->cap);
    }
    memcpy(buf->p + buf->len, data, len);
    buf->len += len;
}

static void b8(struct buf *buf, uint8_t value)
{
    bput(buf, &value, 1);
}

/* PTP containers are little-endian throughout. */
static void b16(struct buf *buf, uint16_t value)
{
    uint8_t raw[2];
    usb_put_le16(raw, value);
    bput(buf, raw, sizeof(raw));
}

static void b32(struct buf *buf, uint32_t value)
{
    uint8_t raw[4];
    usb_put_le32(raw, value);
    bput(buf, raw, sizeof(raw));
}

static void b64(struct buf *buf, uint64_t value)
{
    uint8_t raw[8];
    usb_put_le64(raw, value);
    bput(buf, raw, sizeof(raw));
}

static void bstr(struct buf *buf, const char *str)
{ /* PTP string (ASCII -> UTF-16LE) */
    size_t len = str ? strlen(str) : 0;
    if (len > 254)
        len = 254;
    b8(buf, (uint8_t)(len ? len + 1 : 0));
    for (size_t i = 0; i < len; i++)
    {
        b8(buf, (uint8_t)str[i]);
        b8(buf, 0);
    }
    if (len)
        b16(buf, 0); /* terminating NUL char */
}

static void barr16(struct buf *buf, const uint16_t *values, int count)
{
    b32(buf, (uint32_t)count);
    for (int i = 0; i < count; i++)
        b16(buf, values[i]);
}

/* ---- container output ---- */
/* One of this function's IN pipes as assigned at build time, NOT looked up by
 * MTP_EP_*: a composite relocates them, and every response is silently dropped. */
static usbip_ep *mtp_in_ep(usbip_function *iface, uint8_t which)
{
    struct mtp_state *st = usbip_function_state(iface);
    return which == MTP_EP_INTR ? st->intr : st->in;
}

static void send_one(usbip_function *iface, uint8_t ep, uint16_t type, uint16_t code, uint32_t txid, const uint8_t *payload, int plen)
{
    int total = 12 + plen;
    uint8_t *out = malloc((size_t)total);

    if (!out)
        return;

    usb_put_le32(out, (uint32_t)total);
    usb_put_le16(out + 4, type);
    usb_put_le16(out + 6, code);
    usb_put_le32(out + 8, txid);

    if (plen)
        memcpy(out + 12, payload, (size_t)plen);

    usbip_device_write(mtp_in_ep(iface, ep), out, total, 0);
    free(out);
}

static void send_data(usbip_function *iface, uint16_t code, uint32_t txid, const uint8_t *payload, int len)
{
    send_one(iface, MTP_EP_IN, CT_DATA, code, txid, payload, len);
}

static void respond(usbip_function *iface, uint16_t rc, uint32_t txid, const uint32_t *values, int np)
{
    uint8_t params[20] = {0}; /* np is trusted to be <= 5; zero the tail either way */
    for (int i = 0; i < np && i < 5; i++)
        usb_put_le32(params + 4 * i, values[i]);
    send_one(iface, MTP_EP_IN, CT_RESPONSE, rc, txid, params, np * 4);
}

static void send_event(usbip_function *iface, uint16_t code, uint32_t param)
{
    uint8_t params[4] = {USB_U32LE(param)};
    send_one(iface, MTP_EP_INTR, CT_EVENT, code, ALL, params, 4);
}

/* The two guards the dispatch repeats for almost every object operation. Each
 * answers the host itself on failure, so the caller just breaks out. */
static struct obj *require_obj(usbip_function *iface, struct mtp_state *st, uint32_t handle, uint32_t txid, int need_file)
{
    struct obj *obj = obj_by_handle(st, handle);
    if (!obj || (need_file && obj->is_dir))
    {
        respond(iface, RC_INVALID_OBJECT_HANDLE, txid, NULL, 0);
        return NULL;
    }
    return obj;
}

static int require_writable(usbip_function *iface, struct mtp_state *st, uint32_t txid)
{
    if (!st->read_only)
        return 1;

    respond(iface, RC_STORE_READ_ONLY, txid, NULL, 0);
    return 0;
}

/* send a built dataset as the data phase, then an OK response, then free it */
static void data_ok(usbip_function *iface, uint16_t code, uint32_t txid, struct buf *buf)
{
    send_data(iface, code, txid, buf->p, (int)buf->len);
    respond(iface, RC_OK, txid, NULL, 0);
    free(buf->p);
    buf->p = NULL;
    buf->len = buf->cap = 0;
}

/* ---- dataset builders ---- */
static void build_device_info(struct mtp_state *st, struct buf *buf)
{
    static const uint16_t events[] = {
        EV_OBJECT_ADDED,
        EV_OBJECT_REMOVED,
        EV_STORE_ADDED
    };
    static const uint16_t dprops[] = {
        DPC_FRIENDLY_NAME,
        DPC_SYNC_PARTNER
    };
    static const uint16_t playback[] = {
        FMT_UNDEFINED,
        FMT_ASSOCIATION,
        FMT_TEXT,
        0x3009,
        0x3801,
        0x380B,
        0xB982
    };
    b16(buf, 100);
    b32(buf, 0x00000006);
    b16(buf, 100);
    bstr(buf, "microsoft.com: 1.0; android.com: 1.0;");
    b16(buf, 0); /* functional mode */
    barr16(buf, DEV_OPS, (int)(sizeof(DEV_OPS) / sizeof(DEV_OPS[0])));
    barr16(buf, events, 3);
    barr16(buf, dprops, 2);
    barr16(buf, NULL, 0); /* capture formats */
    barr16(buf, playback, (int)(sizeof(playback) / sizeof(playback[0])));
    bstr(buf, st->manufacturer);
    bstr(buf, st->model);
    bstr(buf, "1.0");
    bstr(buf, st->serial);
}

static void build_storage_info(struct mtp_state *st, uint32_t storage_id, struct buf *buf)
{
    int si = si_of(st, storage_id);
    uint64_t total = 0;
    uint64_t freeb = 0;
    if (si >= 0 && st->ops.disk_usage)
        st->ops.disk_usage(store_ctx(st, si), &total, &freeb);
    const char *desc = "USB over IP";
    if (si >= 0 && st->ops.description)
    {
        const char *from_ops = st->ops.description(store_ctx(st, si));
        if (from_ops)
            desc = from_ops;
    }
    char vol[24];
    snprintf(vol, sizeof(vol), "vol-%08x", storage_id);
    b16(buf, 0x0003);
    b16(buf, 0x0002);
    b16(buf, st->read_only ? 0x0001 : 0x0000); /* fixed RAM, hierarchical */
    b64(buf, total);
    b64(buf, freeb);
    b32(buf, ALL);
    bstr(buf, desc);
    bstr(buf, vol);
}

static void build_object_info(struct obj *obj, struct buf *buf)
{
    uint32_t size = obj->size > ALL ? ALL : (uint32_t)obj->size;
    b32(buf, obj->storage);
    b16(buf, obj->fmt);
    b16(buf, 0);
    b32(buf, size);
    b16(buf, 0);
    b32(buf, 0);
    b32(buf, 0);
    b32(buf, 0); /* thumb format/size/w/h */
    b32(buf, 0);
    b32(buf, 0);
    b32(buf, 0); /* image w/h/depth */
    b32(buf, obj->parent);
    b16(buf, obj->is_dir ? 0x0001 : 0x0000);
    b32(buf, 0);
    b32(buf, 0);
    bstr(buf, obj->name);
    char dt[24];
    time_t mtime = (time_t)obj->mtime;
    strftime(dt, sizeof(dt), "%Y%m%dT%H%M%S", localtime(&mtime));
    bstr(buf, dt);
    bstr(buf, dt);
    bstr(buf, "");
}

/* one object property -> append (datatype, value); return 0 if unsupported */
static int build_prop(struct mtp_state *st, struct obj *obj, uint16_t pc, struct buf *buf)
{
    switch (pc)
    {
    case OPC_STORAGE_ID:
        b16(buf, T_U32);
        b32(buf, obj->storage);
        return 1;

    case OPC_OBJECT_FORMAT:
        b16(buf, T_U16);
        b16(buf, obj->fmt);
        return 1;

    case OPC_PROTECTION:
        b16(buf, T_U16);
        b16(buf, st->read_only ? 0x0001 : 0);
        return 1;

    case OPC_OBJECT_SIZE:
        b16(buf, T_U64);
        b64(buf, obj->size);
        return 1;

    case OPC_PARENT:
        b16(buf, T_U32);
        b32(buf, obj->parent);
        return 1;

    case OPC_PUID:
    {
        uint8_t id[16];
        puid_of(obj->si, obj->path, id);
        b16(buf, T_U128);
        bput(buf, id, 16);
        return 1;
    }
    case OPC_FILENAME:
    case OPC_NAME:
        b16(buf, T_STR);
        bstr(buf, obj->name);
        return 1;

    case OPC_DATE_MODIFIED:
    {
        char dt[24];
        time_t mtime = (time_t)obj->mtime;
        strftime(dt, sizeof(dt), "%Y%m%dT%H%M%S", localtime(&mtime));
        b16(buf, T_STR);
        bstr(buf, dt);
        return 1;
    }
    default:
        return 0;
    }
}

/* value-only (GetObjectPropValue): like build_prop but without the datatype prefix */
static int build_prop_value(struct mtp_state *st, struct obj *obj, uint16_t pc, struct buf *buf)
{
    struct buf tmp = {0};
    if (!build_prop(st, obj, pc, &tmp))
    {
        free(tmp.p);
        return 0;
    }
    bput(buf, tmp.p + 2, tmp.len - 2); /* drop the 2-byte datatype */
    free(tmp.p);
    return 1;
}

static int datatype_of(uint16_t pc)
{
    switch (pc)
    {
        case OPC_OBJECT_FORMAT:
        case OPC_PROTECTION:
            return T_U16;

        case OPC_STORAGE_ID:
        case OPC_PARENT:
            return T_U32;

        case OPC_OBJECT_SIZE:
            return T_U64;

        case OPC_PUID:
            return T_U128;

        default:
            return T_STR;
    }
}

static void build_prop_desc(uint16_t pc, struct buf *buf)
{
    int dt = datatype_of(pc);
    int getset = (pc == OPC_FILENAME || pc == OPC_NAME) ? 1 : 0; /* name is settable (rename) */
    b16(buf, pc);
    b16(buf, (uint16_t)dt);
    b8(buf, (uint8_t)getset);
    switch (dt)
    { /* factory default value */
    case T_U16:
        b16(buf, 0);
        break;

    case T_U32:
        b32(buf, 0);
        break;

    case T_U64:
        b64(buf, 0);
        break;

    case T_U128:
    {
        uint8_t zeros[16] = {0};
        bput(buf, zeros, 16);
        break;
    }
    default:
        b8(buf, 0);
        break; /* empty string */
    }
    b32(buf, 0);
    b8(buf, 0); /* group code, form flag = none */
}

static void build_prop_list(struct mtp_state *st, const uint32_t *params, struct buf *buf)
{
    uint32_t handle = params[0];
    uint32_t fmt = params[1];
    uint32_t propcode = params[2];
    uint32_t depth = params[4];
    int all_props = (propcode == 0 || propcode == ALL);
    uint32_t count = 0;
    struct buf body = {0};
    for (int i = 0; i < st->n_objs; i++)
    {
        struct obj *obj = &st->objs[i];
        if (!obj->alive)
            continue;
        if (handle == ALL)
        { /* all objects */
        }
        else if (handle == 0)
        {
            if (obj->parent != 0)
                continue;
        }
        else if (obj->handle != handle && !(depth == ALL && is_descendant(st, obj->handle, handle)))
            continue;
        if (fmt && obj->fmt != fmt)
            continue;
        for (int k = 0; k < N_PROPS; k++)
        {
            uint16_t pc = all_props ? PROP_LIST[k] : (uint16_t)propcode;
            b32(&body, obj->handle);
            b16(&body, pc);
            if (!build_prop(st, obj, pc, &body))
            {
                body.len -= 6;
            } /* unsupported: roll back */
            else
                count++;
            if (!all_props)
                break;
        }
    }
    b32(buf, count);
    bput(buf, body.p, body.len);
    free(body.p);
}

/* ---- list handles for (storage_id, parent), optionally filtered by format ---- */
static int handle_matches(struct obj *obj, uint32_t storage_id, uint32_t fmt, uint32_t parent)
{
    if (!obj->alive)
        return 0;

    if (storage_id != 0 && storage_id != ALL && obj->storage != storage_id)
        return 0;

    if (parent == ALL)
    {
        if (obj->parent != 0)
            return 0;
    }                                            /* ROOT -> top level */
    else if (parent != 0 && obj->parent != parent) /* parent 0 -> all on the store(s) */
        return 0;
    if (fmt && obj->fmt != fmt)
        return 0;
    return 1;
}
static void list_handles(struct mtp_state *st, uint32_t storage_id, uint32_t fmt, uint32_t parent, struct buf *buf)
{
    uint32_t count = 0;
    struct buf body = {0};
    for (int i = 0; i < st->n_objs; i++)
    {
        if (handle_matches(&st->objs[i], storage_id, fmt, parent))
        {
            b32(&body, st->objs[i].handle);
            count++;
        }
    }
    b32(buf, count);
    bput(buf, body.p, body.len);
    free(body.p);
}
static uint32_t count_handles(struct mtp_state *st, uint32_t storage_id, uint32_t fmt, uint32_t parent)
{
    uint32_t count = 0;
    for (int i = 0; i < st->n_objs; i++)
        if (handle_matches(&st->objs[i], storage_id, fmt, parent))
            count++;
    return count;
}

/* read an object's bytes (whole when want<0, or a [off,want) slice) via its backend */
static uint8_t *read_object(struct mtp_state *st, struct obj *obj, long off, long want, int *out_len)
{
    long avail = (long)obj->size - off;
    if (avail < 0)
        avail = 0;
    if (want < 0 || want > avail)
        want = avail;
    uint8_t *buf = malloc(want ? (size_t)want : 1);
    int got = st->ops.read(store_ctx(st, obj->si), obj->path, off, buf, (int)want);
    *out_len = got;
    return buf;
}

/* after a backend rename within store `si` (old -> neu), repoint the table */
static void retag_paths(struct mtp_state *st, int si, const char *old, const char *neu)
{
    size_t ol = strlen(old);
    for (int i = 0; i < st->n_objs; i++)
    {
        struct obj *obj = &st->objs[i];
        if (obj->si != si)
            continue;
        if (strcmp(obj->path, old) == 0)
        {
            snprintf(obj->path, sizeof(obj->path), "%s", neu);
        }
        else if (strncmp(obj->path, old, ol) == 0 && obj->path[ol] == '/')
        {
            char tmp[4096];
            snprintf(tmp, sizeof(tmp), "%s%s", neu, obj->path + ol);
            snprintf(obj->path, sizeof(obj->path), "%s", tmp);
        }
    }
}

/* recursively copy a file or directory tree into store dest_si; out = new relpath */
static void copy_tree(struct mtp_state *st, struct obj *obj, int dest_si,
                      const char *dest, const char *name, char *out, size_t outsz)
{
    char neu[8192]; /* room for nested dest + name */
    path_join(neu, sizeof(neu), dest, name);
    if (obj->is_dir)
    {
        st->ops.make_dir(store_ctx(st, dest_si), neu);
        for (int i = 0; i < st->n_objs; i++)
        {
            struct obj *child = &st->objs[i];
            if (child->alive && child->parent == obj->handle)
                copy_tree(st, child, dest_si, neu, child->name, NULL, 0);
        }
    }
    else
    {
        int got = 0;
        uint8_t *buf = read_object(st, obj, 0, -1, &got);
        st->ops.write(store_ctx(st, dest_si), neu, buf, got);
        free(buf);
    }
    if (out)
        snprintf(out, outsz, "%s", neu);
}

/* ---- data-out phase completion ---- */
static void recv_object_info(usbip_function *iface, struct mtp_state *st, const uint8_t *payload, int len)
{
    uint32_t txid = st->rx_txid;
    if (len < 53)
    {
        respond(iface, RC_GENERAL_ERROR, txid, NULL, 0);
        return;
    }
    uint16_t fmt = usb_get_le16(payload + 4);
    char name[256]; /* the Filename string starts at offset 52 */
    ptp_str_ascii(payload + 52, len - 52, name, sizeof(name));
    int si = 0;
    const char *pdir = target(st, st->rx_p[0], st->rx_p[1], &si);
    if (!pdir || !name[0])
    {
        respond(iface, RC_INVALID_PARENT, txid, NULL, 0);
        return;
    }
    char full[4400]; /* room for pdir (<=4095) + '/' + name */
    path_join(full, sizeof(full), pdir, name);
    if (fmt == FMT_ASSOCIATION)
    {
        st->ops.make_dir(store_ctx(st, si), full);
        st->send_target = 0;
    }
    else
    {
        st->ops.write(store_ctx(st, si), full, (const uint8_t *)"", 0); /* create empty */
    }
    rescan(st);
    struct obj *obj = obj_by_path(st, si, full);
    uint32_t handle = (obj && obj->alive) ? obj->handle : 0;
    if (fmt != FMT_ASSOCIATION)
        st->send_target = handle;
    mtp_log(st, "%s %s", op_name(OP_SEND_OBJECT_INFO), name);
    uint32_t rp[3] = {
        STORAGE_ID(si),
        st->rx_p[1],
        handle};
    respond(iface, RC_OK, txid, rp, 3);
    if (handle)
        send_event(iface, EV_OBJECT_ADDED, handle);
}

static void recv_object(usbip_function *iface, struct mtp_state *st, const uint8_t *payload, int len)
{
    uint32_t txid = st->rx_txid;
    uint32_t handle = st->send_target;
    st->send_target = 0;
    struct obj *obj = obj_by_handle(st, handle);
    if (!obj)
    {
        respond(iface, RC_NO_VALID_OBJECT_INFO, txid, NULL, 0);
        return;
    }
    st->ops.write(store_ctx(st, obj->si), obj->path, payload, len);
    rescan(st);
    mtp_log(st, "%s (%dB)", op_name(OP_SEND_OBJECT), len);
    respond(iface, RC_OK, txid, NULL, 0);
    send_event(iface, EV_OBJECT_ADDED, handle);
}

static void recv_device_prop(usbip_function *iface, struct mtp_state *st, const uint8_t *payload, int len)
{
    char val[64];
    ptp_str_ascii(payload, len, val, sizeof(val));
    if (st->rx_p[0] == DPC_FRIENDLY_NAME)
        snprintf(st->friendly, sizeof(st->friendly), "%s", val);
    else if (st->rx_p[0] == DPC_SYNC_PARTNER)
        snprintf(st->sync_partner, sizeof(st->sync_partner), "%s", val);
    respond(iface, RC_OK, st->rx_txid, NULL, 0);
}

static void recv_set_object_prop(usbip_function *iface, struct mtp_state *st, const uint8_t *payload, int len)
{
    uint32_t txid = st->rx_txid;
    uint32_t handle = st->rx_p[0];
    uint32_t propcode = st->rx_p[1];
    struct obj *obj = require_obj(iface, st, handle, txid, 0);
    if (!obj)
        return;
    if (propcode != OPC_FILENAME && propcode != OPC_NAME)
    {
        respond(iface, RC_OBJECT_PROP_NOT_SUPPORTED, txid, NULL, 0);
        return;
    }
    char name[256]; /* parse the PTP string value */
    ptp_str_ascii(payload, len, name, sizeof(name));
    if (!name[0])
    {
        respond(iface, RC_GENERAL_ERROR, txid, NULL, 0);
        return;
    }
    struct obj *par = (obj->parent == 0) ? NULL : obj_by_handle(st, obj->parent);
    const char *pdir = par ? par->path : "";
    int si = obj->si;
    char old[4096];
    snprintf(old, sizeof(old), "%s", obj->path);
    char neu[4400]; /* room for pdir (<=4095) + '/' + name */
    path_join(neu, sizeof(neu), pdir, name);
    if (st->ops.rename(store_ctx(st, si), old, neu) == 0)
    {
        retag_paths(st, si, old, neu);
        rescan(st);
        mtp_log(st, "Rename -> %s", name);
        respond(iface, RC_OK, txid, NULL, 0);
    }
    else
        respond(iface, RC_GENERAL_ERROR, txid, NULL, 0);
}

static void recv_send_partial(usbip_function *iface, struct mtp_state *st, const uint8_t *payload, int len)
{
    uint32_t txid = st->rx_txid;
    uint32_t handle = st->rx_p[0];
    long off = (long)((uint64_t)st->rx_p[1] | ((uint64_t)st->rx_p[2] << 32));
    struct obj *obj = require_obj(iface, st, handle, txid, 1);
    if (!obj)
        return;
    st->ops.pwrite(store_ctx(st, obj->si), obj->path, off, payload, len);
    rescan(st);
    mtp_log(st, "%s (@%ld %dB)", op_name(OP_SEND_PARTIAL_OBJECT), off, len);
    uint32_t got = (uint32_t)len;
    respond(iface, RC_OK, txid, &got, 1);
}

static void complete_rx(usbip_function *iface, struct mtp_state *st)
{
    const uint8_t *payload = st->rx_buf + 12;
    int plen = (int)(st->rx_need - 12);

    if (plen < 0)
        plen = 0;

    if (st->rx_code == OP_SEND_OBJECT_INFO)
        recv_object_info(iface, st, payload, plen);
    else if (st->rx_code == OP_SEND_OBJECT)
        recv_object(iface, st, payload, plen);
    else if (st->rx_code == OP_SET_DEVICE_PROP_VALUE)
        recv_device_prop(iface, st, payload, plen);
    else if (st->rx_code == OP_SET_OBJECT_PROP_VALUE)
        recv_set_object_prop(iface, st, payload, plen);
    else if (st->rx_code == OP_SEND_PARTIAL_OBJECT)
        recv_send_partial(iface, st, payload, plen);
}

static void begin_rx(struct mtp_state *st, uint16_t code, uint32_t txid, const uint32_t *params)
{
    st->rx_active = 1;
    st->rx_code = code;
    st->rx_txid = txid;
    memcpy(st->rx_p, params, sizeof(st->rx_p));
    st->rx_len = 0;
    st->rx_need = -1;
}

/* ---- command dispatch ---- */
static void dispatch(usbip_function *iface, struct mtp_state *st, const uint8_t *data, int len)
{
    if (len < 12)
        return;

    uint16_t code = usb_get_le16(data + 6);
    uint32_t txid = usb_get_le32(data + 8);
    uint32_t params[5] = { 0, 0, 0, 0, 0};
    int np = (len - 12) / 4;
    if (np > 5)
        np = 5;
    for (int i = 0; i < np; i++)
        params[i] = usb_get_le32(data + 12 + 4 * i);

    if (code != OP_GET_DEVICE_INFO && code != OP_OPEN_SESSION && !st->session)
    {
        respond(iface, RC_SESSION_NOT_OPEN, txid, NULL, 0);
        return;
    }
    struct buf buf = {0};
    switch (code)
    {
    case OP_GET_DEVICE_INFO:
        mtp_log(st, "%s", op_name(code));
        build_device_info(st, &buf);
        data_ok(iface, code, txid, &buf);
        break;

    case OP_OPEN_SESSION:
        if (st->session)
        {
            uint32_t rp = st->session;
            respond(iface, RC_SESSION_ALREADY_OPEN, txid, &rp, 1);
        }
        else if (params[0] == 0)
            respond(iface, RC_INVALID_PARAMETER, txid, NULL, 0);
        else
        {
            st->session = params[0];
            mtp_log(st, "%s", op_name(code));
            respond(iface, RC_OK, txid, NULL, 0);
        }
        break;

    case OP_CLOSE_SESSION:
        st->session = 0;
        respond(iface, RC_OK, txid, NULL, 0);
        break;

    case OP_GET_STORAGE_IDS:
    {
        b32(&buf, (uint32_t)st->ops.n_stores);
        for (int si = 0; si < st->ops.n_stores; si++)
            b32(&buf, STORAGE_ID(si));
        data_ok(iface, code, txid, &buf);
        break;
    }
    case OP_GET_STORAGE_INFO:
        if (si_of(st, params[0]) < 0)
            respond(iface, RC_INVALID_STORAGE_ID, txid, NULL, 0);
        else
        {
            build_storage_info(st, params[0], &buf);
            data_ok(iface, code, txid, &buf);
        }
        break;

    case OP_GET_NUM_OBJECTS:
    {
        uint32_t count = count_handles(st, params[0], params[1], params[2]);
        respond(iface, RC_OK, txid, &count, 1);
        break;
    }
    case OP_GET_OBJECT_HANDLES:
        mtp_log(st, "%s", op_name(code));
        list_handles(st, params[0], params[1], params[2], &buf);
        data_ok(iface, code, txid, &buf);
        break;

    case OP_GET_OBJECT_INFO:
    {
        struct obj *obj = require_obj(iface, st, params[0], txid, 0);
        if (obj)
        {
            build_object_info(obj, &buf);
            data_ok(iface, code, txid, &buf);
        }
        break;
    }
    case OP_GET_OBJECT:
    {
        struct obj *obj = require_obj(iface, st, params[0], txid, 1);
        if (!obj)
            break;

        int got = 0;
        uint8_t *fd = read_object(st, obj, 0, -1, &got);
        mtp_log(st, "%s %s (%dB)", op_name(code), obj->name, got);
        send_data(iface, code, txid, fd, got);
        respond(iface, RC_OK, txid, NULL, 0);
        free(fd);
        break;
    }
    case OP_GET_PARTIAL_OBJECT:
    case OP_GET_PARTIAL_OBJECT_64:
    {
        struct obj *obj = require_obj(iface, st, params[0], txid, 1);
        if (!obj)
            break;

        long off;
        long want;
        if (code == OP_GET_PARTIAL_OBJECT_64)
        {
            off = (long)((uint64_t)params[1] | ((uint64_t)params[2] << 32));
            want = (long)params[3];
        }
        else
        {
            off = (long)params[1];
            want = (params[2] == ALL) ? -1 : (long)params[2];
        }
        int n_read = 0;
        uint8_t *fd = read_object(st, obj, off, want, &n_read);
        send_data(iface, code, txid, fd, n_read);
        uint32_t got = (uint32_t)n_read;
        respond(iface, RC_OK, txid, &got, 1);
        free(fd);
        break;
    }
    case OP_DELETE_OBJECT:
    {
        if (!require_writable(iface, st, txid))
            break;

        struct obj *obj = require_obj(iface, st, params[0], txid, 0);
        if (!obj)
            break;

        if (st->ops.remove(store_ctx(st, obj->si), obj->path) == 0)
        {
            rescan(st);
            respond(iface, RC_OK, txid, NULL, 0);
            send_event(iface, EV_OBJECT_REMOVED, params[0]);
        }
        else
            respond(iface, RC_GENERAL_ERROR, txid, NULL, 0);
        break;
    }
    case OP_SEND_OBJECT_INFO:
        if (require_writable(iface, st, txid))
            begin_rx(st, code, txid, params);
        break;

    case OP_SEND_OBJECT:
        if (!require_writable(iface, st, txid))
            break;

        if (!st->send_target)
            respond(iface, RC_NO_VALID_OBJECT_INFO, txid, NULL, 0);
        else
            begin_rx(st, code, txid, params);
        break;

    case OP_MOVE_OBJECT:
    {
        if (!require_writable(iface, st, txid))
            break;

        struct obj *obj = require_obj(iface, st, params[0], txid, 0);
        int dsi = 0;
        const char *dest = target(st, params[1], params[2], &dsi);
        if (!obj)
            break;

        if (!dest)
        {
            respond(iface, RC_INVALID_PARENT, txid, NULL, 0);
            break;
        }
        char neu[4400];
        path_join(neu, sizeof(neu), dest, obj->name);
        if (dsi == obj->si)
        { /* same-storage move = rename */
            char old[4096];
            snprintf(old, sizeof(old), "%s", obj->path);
            if (st->ops.rename(store_ctx(st, obj->si), old, neu) == 0)
            {
                retag_paths(st, obj->si, old, neu);
                rescan(st);
                respond(iface, RC_OK, txid, NULL, 0);
            }
            else
                respond(iface, RC_GENERAL_ERROR, txid, NULL, 0);
        }
        else
        { /* cross-storage move = copy + delete */
            int src_si = obj->si;
            char old[4096];
            snprintf(old, sizeof(old), "%s", obj->path);
            copy_tree(st, obj, dsi, dest, obj->name, NULL, 0);
            st->ops.remove(store_ctx(st, src_si), old);
            rescan(st);
            respond(iface, RC_OK, txid, NULL, 0);
        }
        break;
    }
    case OP_COPY_OBJECT:
    {
        if (!require_writable(iface, st, txid))
            break;

        struct obj *obj = require_obj(iface, st, params[0], txid, 0);
        int dsi = 0;
        const char *dest = target(st, params[1], params[2], &dsi);
        if (!obj)
            break;

        if (!dest)
        {
            respond(iface, RC_INVALID_PARENT, txid, NULL, 0);
            break;
        }
        char neu[8192];
        copy_tree(st, obj, dsi, dest, obj->name, neu, sizeof(neu));
        rescan(st);
        struct obj *no = obj_by_path(st, dsi, neu);
        uint32_t nh = (no && no->alive) ? no->handle : 0;
        respond(iface, RC_OK, txid, &nh, 1);
        break;
    }
    case OP_SET_OBJECT_PROP_VALUE:
        if (require_writable(iface, st, txid))
            begin_rx(st, code, txid, params);
        break;

    case OP_BEGIN_EDIT_OBJECT:
    case OP_END_EDIT_OBJECT:
        respond(iface, obj_by_handle(st, params[0]) ? RC_OK : RC_INVALID_OBJECT_HANDLE, txid, NULL, 0);
        break;

    case OP_SEND_PARTIAL_OBJECT:
    {
        if (!require_writable(iface, st, txid))
            break;

        if (require_obj(iface, st, params[0], txid, 1))
            begin_rx(st, code, txid, params);
        break;
    }
    case OP_TRUNCATE_OBJECT:
    {
        if (!require_writable(iface, st, txid))
            break;

        struct obj *obj = require_obj(iface, st, params[0], txid, 1);
        uint64_t size = (uint64_t)params[1] | ((uint64_t)params[2] << 32);
        if (!obj)
            break;

        if (st->ops.truncate(store_ctx(st, obj->si), obj->path, size) == 0)
        {
            rescan(st);
            respond(iface, RC_OK, txid, NULL, 0);
        }
        else
            respond(iface, RC_GENERAL_ERROR, txid, NULL, 0);
        break;
    }
    case OP_GET_DEVICE_PROP_DESC:
        if (params[0] == DPC_FRIENDLY_NAME || params[0] == DPC_SYNC_PARTNER)
        {
            const char *cur = params[0] == DPC_FRIENDLY_NAME ? st->friendly : st->sync_partner;
            b16(&buf, (uint16_t)params[0]);
            b16(&buf, T_STR);
            b8(&buf, 1); /* get/set */
            bstr(&buf, "");
            bstr(&buf, cur);
            b8(&buf, 0);
            data_ok(iface, code, txid, &buf);
        }
        else
            respond(iface, RC_DEVICEPROP_NOT_SUPPORTED, txid, NULL, 0);
        break;

    case OP_GET_DEVICE_PROP_VALUE:
        if (params[0] == DPC_FRIENDLY_NAME)
        {
            bstr(&buf, st->friendly);
            data_ok(iface, code, txid, &buf);
        }
        else if (params[0] == DPC_SYNC_PARTNER)
        {
            bstr(&buf, st->sync_partner);
            data_ok(iface, code, txid, &buf);
        }
        else
            respond(iface, RC_DEVICEPROP_NOT_SUPPORTED, txid, NULL, 0);
        break;

    case OP_SET_DEVICE_PROP_VALUE:
        begin_rx(st, code, txid, params);
        break;

    case OP_GET_OBJECT_PROPS_SUPPORTED:
        barr16(&buf, PROP_LIST, N_PROPS);
        data_ok(iface, code, txid, &buf);
        break;

    case OP_GET_OBJECT_PROP_DESC:
    {
        int ok = 0;
        for (int k = 0; k < N_PROPS; k++)
            if (PROP_LIST[k] == params[0])
                ok = 1;
        if (!ok)
            respond(iface, RC_INVALID_OBJECT_PROP_CODE, txid, NULL, 0);
        else
        {
            build_prop_desc((uint16_t)params[0], &buf);
            data_ok(iface, code, txid, &buf);
        }
        break;
    }
    case OP_GET_OBJECT_PROP_VALUE:
    {
        struct obj *obj = require_obj(iface, st, params[0], txid, 0);
        if (!obj)
            break;

        if (!build_prop_value(st, obj, (uint16_t)params[1], &buf))
            respond(iface, RC_OBJECT_PROP_NOT_SUPPORTED, txid, NULL, 0);
        else
            data_ok(iface, code, txid, &buf);
        break;
    }
    case OP_GET_OBJECT_PROP_LIST:
        mtp_log(st, "%s", op_name(code));
        build_prop_list(st, params, &buf);
        data_ok(iface, code, txid, &buf);
        break;

    case OP_GET_OBJECT_REFERENCES:
        b32(&buf, 0);
        data_ok(iface, code, txid, &buf);
        break;

    default:
        mtp_log(st, "unhandled op 0x%04X", code);
        respond(iface, RC_OPERATION_NOT_SUPPORTED, txid, NULL, 0);
    }
    free(buf.p);
}

/* ---- usbip_device_class vtable ---- */
static int mtp_build(usbip_function *func, const void *params)
{
    const mtp_opts *opts = params;
    struct mtp_state *st = usbip_function_state(func);
    st->ops = *opts;
    if (st->ops.n_stores < 1)
        st->ops.n_stores = 1;
    if (st->ops.n_stores > MTP_MAX_STORAGES)
        st->ops.n_stores = MTP_MAX_STORAGES;
    st->read_only = opts->read_only;
    st->next_handle = 1;
    st->session = 0;
    st->send_target = 0;
    st->rx_active = 0;
    snprintf(st->model, sizeof(st->model), "%s", opts->name ? opts->name : "USBIP MTP");
    snprintf(st->friendly, sizeof(st->friendly), "%s", st->model);
    snprintf(st->manufacturer, sizeof(st->manufacturer), "%s", opts->manufacturer ? opts->manufacturer : "USB over IP");
    snprintf(st->serial, sizeof(st->serial), "%s", opts->serial && strlen(opts->serial) == 32 ? opts->serial : "JW30");
    rescan(st);

    /* Scoped to THIS function (see dfu.c): the "MTP" Compatible ID must name the MTP
     * interface, or Windows loads WPD over whichever function sits at interface 0. */
    if (opts->winusb)
        usbip_function_enable_msos(func, "MTP", NULL);

    uint16_t bulk_mps = usbip_device_get_speed(usbip_function_device(func)) >= USB_SPEED_HIGH ? 512 : 64; /* HS bulk = 512 */
    usbip_interface *iface = usbip_function_add_interface(func, USB_CLASS_IMAGE, PTP_SUBCLASS, PTP_PROTOCOL);

    st->out = usbip_interface_add_endpoint(iface, &(usb_endpoint_descriptor){
        .bDescriptorType = USB_DT_ENDPOINT,
        .bEndpointAddress = MTP_EP_OUT,
        .bmAttributes = USB_BULK,
        .wMaxPacketSize = bulk_mps
    });
    st->in = usbip_interface_add_endpoint(iface, &(usb_endpoint_descriptor){
        .bDescriptorType = USB_DT_ENDPOINT,
        .bEndpointAddress = MTP_EP_IN,
        .bmAttributes = USB_BULK,
        .wMaxPacketSize = bulk_mps
    });
    st->intr = usbip_interface_add_endpoint(iface, &(usb_endpoint_descriptor){
        .bDescriptorType = USB_DT_ENDPOINT,
        .bEndpointAddress = MTP_EP_INTR,
        .bmAttributes = USB_INTR,
        .wMaxPacketSize = 28,
        .bInterval = 6
    });
    return 0;
}

static int mtp_control(usbip_function *iface, const usb_setup *setup, uint8_t *buf, uint16_t len)
{
    (void)len;
    if (USB_REQ_TYPE(setup->bmRequestType) != USB_CLASS)
        return -1;
    struct mtp_state *st = usbip_function_state(iface);
    switch (setup->bRequest)
    {
        case MTP_REQ_GET_DEVICE_STATUS:
            usb_put_le16(buf, MTP_STATUS_LEN);
            usb_put_le16(buf + 2, RC_OK);
            return MTP_STATUS_LEN;

        case MTP_REQ_DEVICE_RESET:
            st->session = 0;
            st->rx_active = 0;
            st->send_target = 0;
            return 0;

        case MTP_REQ_CANCEL:
            return 0;
    }
    return -1;
}

static void mtp_on_out(usbip_function *iface, usbip_ep *ep, const void *data, int len)
{
    (void)ep;
    struct mtp_state *st = usbip_function_state(iface);
    const uint8_t *bytes = data;
    if (!st->rx_active)
    {
        dispatch(iface, st, bytes, len);
        return;
    }
    if (st->rx_len + (size_t)len > st->rx_cap)
    {
        st->rx_cap = (st->rx_len + (size_t)len) * 2 + 64;
        st->rx_buf = realloc(st->rx_buf, st->rx_cap);
    }
    memcpy(st->rx_buf + st->rx_len, bytes, (size_t)len);
    st->rx_len += (size_t)len;
    if (st->rx_need < 0 && st->rx_len >= 12)
        st->rx_need = (long)usb_get_le32(st->rx_buf);
    if (st->rx_need >= 0 && (long)st->rx_len >= st->rx_need)
    {
        st->rx_active = 0;
        complete_rx(iface, st);
    }
}

const usbip_device_class usbip_device_mtp = {
    .name = "mtp",
    .bInterfaceClass = USB_CLASS_IMAGE,
    .build   = mtp_build,
    .control = mtp_control,
    .on_out  = mtp_on_out,
    .state_size = sizeof(struct mtp_state),
};

mtp_func *mtp_add(usbip_device *dev, const mtp_opts *opts)
{
    return handle_of(usbip_device_add_class(dev, &usbip_device_mtp, opts));
}
