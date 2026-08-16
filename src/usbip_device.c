/**
 * Copyright (C) 2026 Jabez Winston
 *
 * SPDX-License-Identifier: MIT
 *
 * Date : 04-June-2026
 *
 * usbip_device.c - device core. CLASS-FREE: knows only generic USB (descriptors, the
 * three standard descriptor types, standard requests, endpoints, URB dispatch)
 * and a class-agnostic vtable. It never branches on bInterfaceClass. Anything
 * class-specific lives in src/class_*.c, built on this public API.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <stddef.h>

#include "usbip_device_internal.h"

/* MS OS 1.0 (the 0xEE string + two vendor requests) */
#define MSOS1_BCD_VERSION      0x0100  /* bcdVersion in every MS OS 1.0 header */
#define MSOS1_IDX_COMPAT_ID    0x0004  /* wIndex: Extended Compat ID feature */
#define MSOS1_IDX_EXT_PROPS    0x0005  /* wIndex: Extended Properties feature */
#define MSOS1_COMPATID_HDR_LEN 16      /* header before the per-function sections */
#define MSOS1_COMPATID_SEC_LEN 24      /* one Extended Compat ID section */
#define MSOS1_COMPATID_MAX     8       /* compatibleID is a fixed 8-byte field */
#define MSOS1_EXTPROP_HDR_LEN  10      /* dwLength + bcdVersion + wIndex + wCount */
#define MSOS1_PROP_REG_SZ      1       /* dwPropertyDataType: NUL-terminated string */
#define MSOS1_STRING_INDEX     0xEE    /* the string descriptor that advertises MS OS 1.0 */
#define MSOS1_VENDOR_DEFAULT   0x20    /* bRequest we ask Windows to use, if unset */

/* MS OS 2.0 (a BOS platform capability + one vendor request) */
#define MSOS2_IDX_DESCRIPTOR      0x0007  /* wIndex: fetch the descriptor set */
#define MSOS2_VENDOR_DEFAULT      0x21
#define MSOS2_SET_HDR_LEN         10      /* descriptor-set header */
#define MSOS2_SET_HEADER          0x0000
#define MSOS2_SUBSET_CONFIG       0x0001
#define MSOS2_SUBSET_FUNCTION     0x0002
#define MSOS2_FEATURE_COMPAT      0x0003  /* Compatible ID feature */
#define MSOS2_FEATURE_REG_PROP    0x0004  /* Registry Property feature */
#define MSOS2_CONFIG_SUBSET_LEN   8
#define MSOS2_FUNCTION_SUBSET_LEN 8
#define MSOS2_COMPAT_LEN          20         /* wLength + wType + 8 compat + 8 sub-compat */
#define MSOS2_REGPROP_HDR_LEN     8          /* wLength + wType + wDataType + wNameLength */
#define MSOS2_REG_MULTI_SZ        0x0007     /* wPropertyDataType: REG_MULTI_SZ */
#define MSOS2_WINDOWS_8_1         0x06030000 /* dwWindowsVersion: 8.1 and later */

/* BOS + platform capability (USB 3.0 Sec.9.6.2) */
#define USB_BOS_HDR_LEN          5
#define USB_DT_BOS               0x0F
#define USB_DT_DEVICE_CAPABILITY 0x10
#define USB_CAP_TYPE_PLATFORM    0x05
#define USB_PLATFORM_CAP_HDR     4     /* bLength + bDescriptorType + bDevCapabilityType + reserved */
#define USB_PLATFORM_UUID_LEN    16

/* WebUSB platform capability + GET_URL */
#define WEBUSB_CAP_LEN         24      /* platform header + UUID + bcdVersion + vendor code + landing */
#define MSOS2_CAP_LEN          28      /* platform header + UUID + version + length + vendor + alt */
#define WEBUSB_BCD_VERSION     0x0100
#define WEBUSB_URL_HDR_LEN     3       /* bLength + bDescriptorType + bScheme */
#define WEBUSB_URL_DESC_TYPE   3
#define WEBUSB_SCHEME_HTTP     0
#define WEBUSB_SCHEME_HTTPS    1
#define WEBUSB_SCHEME_NONE     255     /* the URL carries its own scheme */
#define WEBUSB_IDX_GET_URL     0x02    /* wIndex on the WebUSB vendor request */

/* ---- isochronous pacing -------------------------------------------------
 * There is no SOF over USB/IP, so unpaced iso acks instantly and the host's engine
 * free-runs -- a UAC speaker plays ~20x too fast.
 *
 * Pacing (usbip_device_set_iso_pacing, opt-in) schedules each completion on a
 * per-endpoint deadline clock, delivered by a PACER thread. */
struct paced_ret
{
    struct paced_ret *next;
    uint64_t deadline_us;
    uint32_t seqnum, devid, direction, ep;
    int npkts;
    struct iso_pkt *iso; /* copy of u->iso[0..npkts) */
    uint8_t *data;       /* copy of IN data (de-padded), or NULL for OUT */
    uint32_t data_len;
};

/* advance an endpoint's deadline clock by npkts service intervals; return the new deadline */
static uint64_t iso_deadline(usbip_ep *ep, int npkts)
{
    uint8_t bint = ep->interval ? ep->interval : 1;
    /* FS/LS counts bInterval whole 1 ms frames; HS counts 2^(bInterval-1) of 125 µs
     * microframes. So bInterval=1 polls every 1 ms at FS, every 125 µs at HS. */
    int hs = ep->dev && ep->dev->speed >= USB_SPEED_HIGH;
    uint64_t frame_us = hs ? (125u << (bint - 1)) : (uint64_t)bint * 1000u;
    uint64_t now = usbip_mono_us();

    if (ep->iso_deadline_us < now)
        ep->iso_deadline_us = now; /* first transfer / recover from a gap */

    ep->iso_deadline_us += frame_us * (uint64_t)npkts;
    return ep->iso_deadline_us;
}

static void *pacer_run(void *arg)
{
    struct conn *conn = arg;
    pthread_mutex_lock(&conn->pace_lock);
    for (;;)
    {
        while (!conn->pace_stop && !conn->pace_head)
            pthread_cond_wait(&conn->pace_cond, &conn->pace_lock);
        if (!conn->pace_head)
        {
            if (conn->pace_stop)
                break;
            continue;
        }

        struct paced_ret **minpp = &conn->pace_head; /* earliest-deadline entry */
        for (struct paced_ret **pp = &conn->pace_head; *pp; pp = &(*pp)->next)
            if ((*pp)->deadline_us < (*minpp)->deadline_us)
                minpp = pp;
        struct paced_ret *entry = *minpp;

        if (!conn->pace_stop && entry->deadline_us > usbip_mono_us())
        { /* not due yet: wait (re-scan on wake) */
            usbip_cond_timedwait_mono(&conn->pace_cond, &conn->pace_lock, entry->deadline_us);
            continue;
        }
        *minpp = entry->next; /* due (or draining on stop): send it */
        pthread_mutex_unlock(&conn->pace_lock);
        struct urb tu;
        memset(&tu, 0, sizeof(tu));
        tu.seqnum = entry->seqnum;
        tu.devid = entry->devid;
        tu.direction = entry->direction;
        tu.ep = entry->ep;
        tu.number_of_packets = entry->npkts;
        tu.iso = entry->iso;
        usbip_send_ret_iso(conn, &tu, entry->data);
        free(entry->iso);
        free(entry->data);
        free(entry);
        pthread_mutex_lock(&conn->pace_lock);
    }
    pthread_mutex_unlock(&conn->pace_lock);
    return NULL;
}

/* lazily start the pacer thread on first paced iso (only the serve thread calls this) */
static void pacer_init(struct conn *conn)
{
    pthread_mutex_init(&conn->pace_lock, NULL);
    usbip_cond_init_mono(&conn->pace_cond);
    conn->pace_head = NULL;
    conn->pace_stop = 0;
    conn->pace_started = 1;
    pthread_create(&conn->pacer_thread, NULL, pacer_run, conn);
}

/* queue an iso completion for delivery at `deadline` (copies the descriptors + IN data) */
static void pace_submit(struct conn *conn, struct urb *urb, uint64_t deadline,
                        const uint8_t *data, uint32_t data_len)
{
    if (!conn->pace_started)
        pacer_init(conn);

    struct paced_ret *entry = calloc(1, sizeof(*entry));
    struct iso_pkt *iso = malloc((size_t)urb->number_of_packets * sizeof(*iso));
    uint8_t *dcopy = (data && data_len) ? malloc(data_len) : NULL;

    if (!entry || !iso || (data && data_len && !dcopy))
    { /* OOM: just send now, unpaced */
        free(entry);
        free(iso);
        free(dcopy);
        usbip_send_ret_iso(conn, urb, data);
        return;
    }
    memcpy(iso, urb->iso, (size_t)urb->number_of_packets * sizeof(*iso));

    if (dcopy)
        memcpy(dcopy, data, data_len);

    entry->deadline_us = deadline;
    entry->seqnum = urb->seqnum;
    entry->devid = urb->devid;
    entry->direction = urb->direction;
    entry->ep = urb->ep;
    entry->npkts = urb->number_of_packets;
    entry->iso = iso;
    entry->data = dcopy;
    entry->data_len = data_len;
    pthread_mutex_lock(&conn->pace_lock);
    entry->next = conn->pace_head;
    conn->pace_head = entry;
    pthread_cond_signal(&conn->pace_cond);
    pthread_mutex_unlock(&conn->pace_lock);
}

void usbip_device_pacer_stop(struct conn *conn)
{
    if (!conn->pace_started)
        return;

    pthread_mutex_lock(&conn->pace_lock);
    conn->pace_stop = 1; /* pacer drains remaining, then exits */
    pthread_cond_signal(&conn->pace_cond);
    pthread_mutex_unlock(&conn->pace_lock);
    pthread_join(conn->pacer_thread, NULL);
    pthread_mutex_destroy(&conn->pace_lock);
    pthread_cond_destroy(&conn->pace_cond);
    conn->pace_started = 0;
}

void usbip_device_set_iso_pacing(usbip_device *dev, int enabled) { dev->iso_paced = enabled; }

const char *usb_strerror(int code)
{
    switch (code)
    {
    case USB_SUCCESS:
        return "success";
    case USB_ERROR_IO:
        return "I/O error";
    case USB_ERROR_NO_DEVICE:
        return "no device";
    case USB_ERROR_NOT_FOUND:
        return "not found";
    case USB_ERROR_TIMEOUT:
        return "timeout";
    case USB_ERROR_PIPE:
        return "stall";
    case USB_ERROR_NO_MEM:
        return "out of memory";
    default:
        return "other error";
    }
}

/* ---- small queues ------------------------------------------------------ */
static void enq_dbuf(struct dbuf **head, struct dbuf **tail, const void *data, int len)
{
    struct dbuf *dbuf = malloc(sizeof(*dbuf) + len);
    if (!dbuf)
        return;

    dbuf->next = NULL;
    dbuf->len = len;
    dbuf->off = 0;

    if (len)
        memcpy(dbuf->data, data, len);

    if (*tail)
        (*tail)->next = dbuf;
    else
        *head = dbuf;

    *tail = dbuf;
}

static struct dbuf *deq_dbuf(struct dbuf **head, struct dbuf **tail)
{
    struct dbuf *dbuf = *head;
    if (dbuf)
    {
        *head = dbuf->next;
        if (!*head)
            *tail = NULL;
    }
    return dbuf;
}

/* ---- build / configure ------------------------------------------------- */
usbip_device *usbip_device_create(uint16_t vid, uint16_t pid)
{
    usbip_device *dev = calloc(1, sizeof(*dev));

    if (!dev)
        return NULL;

    dev->vid = vid;
    dev->pid = pid;
    dev->bcdDevice = USB_BCD_DEVICE_DEFAULT;
    dev->bcdUSB = USB_BCD_USB_2_0;
    dev->speed = USB_SPEED_FULL; /* default; usbip_device_set_speed bumps to HIGH (512 B bulk) */
    dev->n_strings = 4;          /* slots 1-3 reserved for mfr/product/serial */
    dev->cur_ifnum = -1;         /* no interface descriptor appended yet */
    dev->cur_alt = -1;
    return dev;
}

void usbip_device_set_strings(usbip_device *dev, const char *mfr, const char *product, const char *serial)
{
    if (mfr)
    {
        dev->strings[1] = strdup(mfr);
        dev->i_mfr = 1;
    }
    if (product)
    {
        dev->strings[2] = strdup(product);
        dev->i_prod = 2;
    }
    if (serial)
    {
        dev->strings[3] = strdup(serial);
        dev->i_ser = 3;
    }
}

/* Is this the 0xEF/0x02/0x01 triple a composite device must advertise? */
static int is_iad_triple(uint8_t cls, uint8_t sub, uint8_t proto)
{
    return cls == USB_CLASS_MISC && sub == USB_SUBCLASS_COMMON && proto == USB_PROTOCOL_IAD;
}

void usbip_device_set_class(usbip_device *dev, uint8_t cls, uint8_t sub, uint8_t proto)
{
    /* 0xEF/0x02/0x01 is what makes Windows load usbccgp. Letting a later class
     * overwrite it would silently unmake the composite. */
    if (dev->composite && !is_iad_triple(cls, sub, proto))
    {
        fprintf(stderr, "[usbip_device] device class %02x/%02x/%02x ignored: "
                        "composite pins the triple to EF/02/01\n", cls, sub, proto);
        return;
    }
    dev->dev_class = cls;
    dev->dev_sub = sub;
    dev->dev_proto = proto;
}

/* bcdDevice. Some hosts key driver binding off the exact value - Windows binds a
 * CSR Bluetooth radio only on the real firmware's 0x8891. Default 0x0100. */
void usbip_device_set_bcd_device(usbip_device *dev, uint16_t bcd)
{
    dev->bcdDevice = bcd;
}

/* Report a link speed to the importer. USB/IP is URB-level, so "high speed" is just
 * speed=HIGH plus 512 B bulk endpoints in the descriptors. Set it BEFORE adding
 * interfaces, so speed-aware classes size their endpoints. */
void usbip_device_set_speed(usbip_device *dev, usb_speed speed)
{
    dev->speed = speed;
}

/* The device's reported speed - lets a class pick endpoint sizes at build time. */
usb_speed usbip_device_get_speed(usbip_device *dev)
{
    return dev->speed;
}

int usbip_device_add_string(usbip_device *dev, const char *str)
{
    if (dev->n_strings >= (int)(sizeof(dev->strings) / sizeof(dev->strings[0])))
        return 0;

    int idx = dev->n_strings++;
    dev->strings[idx] = strdup(str);
    return idx;
}

void usbip_device_set_composite(usbip_device *dev)
{
    dev->composite = 1;
    /* Miscellaneous / Common Class / Interface Association: the triple that tells
     * Windows to load usbccgp and split the device by IAD-declared function. */
    usbip_device_set_class(dev, USB_CLASS_MISC, USB_SUBCLASS_COMMON, USB_PROTOCOL_IAD);
}

int usbip_device_is_composite(usbip_device *dev)
{
    return dev ? dev->composite : 0;
}

/* Record (or replace) one MS-OS advertisement; `group` -1 means the whole device.
 * The same scope overwrites in place, so two calls cannot leak strings. */
static struct msos_entry *msos_add(usbip_device *dev, int group,
                                   const char *compatible, const char *guid)
{
    char *comp_copy = strdup(compatible ? compatible : "WINUSB");
    char *guid_copy = guid ? strdup(guid) : NULL;
    if (!comp_copy || (guid && !guid_copy))
    {
        free(comp_copy);
        free(guid_copy);
        return NULL;
    }

    struct msos_entry *entry = NULL;
    for (int i = 0; i < dev->n_msos; i++)
        if (dev->msos[i].group == group)
        {
            entry = &dev->msos[i];
            break;
        }

    if (!entry)
    {
        if (dev->n_msos >= (int)(sizeof(dev->msos) / sizeof(dev->msos[0])))
        {
            fprintf(stderr, "[usbip_device] MS OS table full (%d entries); advertisement dropped\n", dev->n_msos);
            free(comp_copy);
            free(guid_copy);
            return NULL;
        }
        entry = &dev->msos[dev->n_msos++];
        memset(entry, 0, sizeof(*entry));
    }
    free(entry->compatible);
    free(entry->guid);
    entry->group = group;
    entry->compatible = comp_copy;
    entry->guid = guid_copy;
    dev->bcdUSB = 0x0210; /* USB 2.1: signals a BOS descriptor is present */

    if (!dev->msos1_vendor)
        dev->msos1_vendor = MSOS1_VENDOR_DEFAULT;

    if (!dev->msos2_vendor)
        dev->msos2_vendor = MSOS2_VENDOR_DEFAULT;

    return entry;
}

int usbip_device_enable_msos_group(usbip_device *dev, int group,
                                   const char *compatible, const char *guid)
{
    return msos_add(dev, group, compatible, guid) ? USB_SUCCESS : USB_ERROR_NO_MEM;
}

int usbip_device_enable_msos(usbip_device *dev, const char *compatible, const char *guid)
{
    return msos_add(dev, -1, compatible, guid) ? USB_SUCCESS : USB_ERROR_NO_MEM;
}

int usbip_device_enable_winusb(usbip_device *dev, const char *guid)
{
    const char *device_guid = guid ? guid : USBIP_WINUSB_DEFAULT_GUID;

    return usbip_device_enable_msos(dev, "WINUSB", device_guid);
}

void usbip_device_enable_webusb(usbip_device *dev, uint8_t vendor_code, const char *url)
{
    dev->webusb = 1;
    dev->webusb_vendor = vendor_code;
    dev->webusb_url = strdup(url ? url : "");
    dev->bcdUSB = 0x0210; /* USB 2.1: signals a BOS descriptor is present */
}

static int desc_len(const uint8_t *desc)
{
    if (desc[0]) /* explicit bLength */
        return desc[0];
    switch (desc[1])
    { /* else infer for known types */
    case USB_DT_DEVICE:
        return 18;
    case USB_DT_CONFIG:
        return 9;
    case USB_DT_INTERFACE:
        return 9;
    case USB_DT_ENDPOINT:
        return 7;
    default: /* class descriptors carry their own bLength */
        return desc[0];
    }
}

/* the (ifnum, alt) record of an interface descriptor already in the config blob */
static struct ifalt *ifalt_for(usbip_device *dev, uint8_t ifnum, uint8_t alt)
{
    for (int i = 0; i < dev->n_ifalts; i++)
        if (dev->ifalts[i].ifnum == ifnum && dev->ifalts[i].alt == alt)
            return &dev->ifalts[i];

    return NULL;
}

/* Register a bInterfaceNumber on first sight: bNumInterfaces bookkeeping, plus
 * attribution to the open descriptor group. Alt settings reuse the number. */
static void register_ifnum(usbip_device *dev, uint8_t ifnum)
{
    if (ifnum + 1 > dev->n_ifnums)
        dev->n_ifnums = ifnum + 1;

    if (ifnum >= 16 || dev->if_present[ifnum])
        return;

    dev->if_present[ifnum] = 1;

    if (dev->n_groups)
    { /* everything appended since group_begin belongs to the latest group */
        struct descr_group *group = &dev->groups[dev->n_groups - 1];
        if (!group->n_ifs)
            group->first_ifnum = ifnum;
        group->n_ifs++;
    }
}

/* Is `ep_num` either unreserved or reserved by `owner` itself? Reservations are stored
 * as ifnum + 1 so that 0 can mean "none", which is what the +1 here undoes. */
static int ep_reservation_allows(usbip_device *dev, uint8_t owner, int ep_num, int dir)
{
    uint8_t res = dev->ep_reserved[ep_num][dir];

    return !res || res == (uint8_t)(owner + 1);
}

/* Pick the endpoint number for a new endpoint, in `dir`.
 *
 * The request is a *preference*: honoured when free, else relocated to the lowest
 * free number. 0 means "any".
 *
 * `owner` is USBIP_EP_NO_IFNUM when no interface exists yet. */
static int ep_alloc_number(usbip_device *dev, uint8_t owner, int want, int dir)
{
    if (want)
    {
        /* The preferred number may already be held, so long as the holder is this same
         * interface: alt settings of one interface reuse an address legitimately
         * (USB 2.0 Sec.9.6.6). */
        struct usbip_ep *held = dev->ep_map[want][dir];
        if ((!held || held->ifnum == owner) && ep_reservation_allows(dev, owner, want, dir))
            return want;
    }
    /* A relocation has no such licence: it must land on a number nothing holds. */
    for (int ep_num = 1; ep_num < 16; ep_num++)
        if (!dev->ep_map[ep_num][dir] && ep_reservation_allows(dev, owner, ep_num, dir))
            return ep_num;

    return -1; /* all 15 numbers in this direction taken */
}

int usbip_device_add_descriptor(usbip_device *dev, const void *descriptor)
{
    const uint8_t *desc = descriptor;
    int blen = desc_len(desc);

    if (dev->descr_len + blen > (int)sizeof(dev->descr))
        return USB_ERROR_NO_MEM;

    uint8_t *dst = dev->descr + dev->descr_len;
    memcpy(dst, desc, blen);
    dst[0] = (uint8_t)blen; /* normalize bLength */
    dev->descr_len += blen;

    if (desc[1] == USB_DT_INTERFACE)
    { /* desc[2]=bInterfaceNumber, desc[3]=bAlternateSetting */
        register_ifnum(dev, desc[2]);
        dev->cur_ifnum = desc[2];
        dev->cur_alt = desc[3];
        struct ifalt *ia = ifalt_for(dev, desc[2], desc[3]);
        if (!ia && dev->n_ifalts < (int)(sizeof(dev->ifalts) / sizeof(dev->ifalts[0])))
        {
            ia = &dev->ifalts[dev->n_ifalts++];
            ia->ifnum = desc[2];
            ia->alt = desc[3];
        }
        if (ia)
            ia->off = (int)(dst - dev->descr);
    }
    else if (desc[1] == USB_DT_ENDPOINT)
    {
        if (dev->n_eps >= (int)(sizeof(dev->eps) / sizeof(dev->eps[0])))
        {
            fprintf(stderr, "[usbip_device] endpoint table full (%d); endpoint dropped\n",
                    dev->n_eps);
            dev->descr_len -= blen;
            return USB_ERROR_NO_MEM;
        }
        uint8_t owner = dev->cur_ifnum >= 0 ? (uint8_t)dev->cur_ifnum : USBIP_EP_NO_IFNUM;
        int dir = (desc[2] & 0x80) ? USB_IN : USB_OUT;
        int number = ep_alloc_number(dev, owner, desc[2] & 0x0f, dir);
        if (number < 0)
        {
            dev->descr_len -= blen;
            return USB_ERROR_NO_MEM;
        }

        struct usbip_ep *ep = calloc(1, sizeof(*ep));
        if (!ep)
        {
            dev->descr_len -= blen;
            return USB_ERROR_NO_MEM;
        }

        ep->number = (uint8_t)number;
        ep->dir = (usb_dir)dir;
        ep->addr = (uint8_t)(number | (dir == USB_IN ? 0x80 : 0x00));
        dst[2] = ep->addr; /* patch the wire descriptor to match */
        ep->type = desc[3] & 0x03;
        ep->mps = usb_get_le16(desc + 4);
        ep->interval = desc[6];
        ep->dev = dev;
        ep->ifnum = owner;
        pthread_mutex_init(&ep->lock, NULL);
        pthread_cond_init(&ep->cond, NULL);
        dev->eps[dev->n_eps++] = ep;
        dev->ep_map[ep->number][ep->dir] = ep;      /* device-global: a URB carries only ep+dir */
        dev->ep_reserved[ep->number][ep->dir] = 0;  /* the reservation is now the real thing */
        /* Auto-count bNumEndpoints in the owning alt: leave the field 0 when
         * declaring the interface. A hand-maintained count goes stale. */
        struct ifalt *ia = NULL;

        if (dev->cur_ifnum >= 0)
            ia = ifalt_for(dev, owner, (uint8_t)dev->cur_alt);

        if (ia)
            dev->descr[ia->off + 4]++;
    }
    return USB_SUCCESS;
}

usbip_ep *usbip_device_add_endpoint(usbip_device *dev, const void *ep_descriptor)
{
    const uint8_t *desc = ep_descriptor;
    usb_dir dir = (desc[2] & 0x80) ? USB_IN : USB_OUT;

    if (usbip_device_add_descriptor(dev, ep_descriptor) != USB_SUCCESS)
        return NULL;

    /* the endpoint just appended is the device's last -- return it directly,
     * since its address may differ from the one requested */
    usbip_ep *ep = dev->n_eps ? dev->eps[dev->n_eps - 1] : NULL;
    return (ep && ep->dir == dir) ? ep : NULL;
}

uint8_t usbip_endpoint_address(usbip_ep *ep)
{
    return ep ? ep->addr : 0;
}

/* Look an endpoint up by its bEndpointAddress, the form a SETUP packet's wIndex
 * carries. Returns NULL for an address no endpoint claims. */
static usbip_ep *ep_by_addr(usbip_device *dev, uint8_t addr)
{
    uint8_t number = addr & 0x0f;
    usb_dir dir = (addr & 0x80) ? USB_IN : USB_OUT;
    return dev ? dev->ep_map[number][dir] : NULL;
}

/* ---- endpoint halt ------------------------------------------------------
 * A halted endpoint STALLs every URB until CLEAR_FEATURE(ENDPOINT_HALT). Queued
 * data survives: MSC STALLs a failed data phase, then queues the CSW. */
void usbip_ep_stall(usbip_ep *ep)
{
    if (!ep)
        return;

    pthread_mutex_lock(&ep->lock);
    ep->halted = 1;
    /* An IN URB may be parked waiting for data that is never coming -- complete it
     * now, or the host sits on the read until its own timeout fires. */
    struct pending *pend = ep->pend_head;
    ep->pend_head = NULL;
    ep->pend_tail = NULL;
    pthread_mutex_unlock(&ep->lock);
    while (pend)
    {
        struct pending *next = pend->next;
        usbip_send_ret(pend->conn, pend->seqnum, pend->devid, pend->direction, pend->ep, USBIP_STATUS_STALL, NULL, 0);
        free(pend);
        pend = next;
    }
}

void usbip_ep_clear_halt(usbip_ep *ep)
{
    if (!ep)
        return;

    pthread_mutex_lock(&ep->lock);
    ep->halted = 0;
    pthread_mutex_unlock(&ep->lock);
}

int usbip_ep_is_halted(usbip_ep *ep)
{
    if (!ep)
        return 0;

    pthread_mutex_lock(&ep->lock);
    int halted = ep->halted;
    pthread_mutex_unlock(&ep->lock);
    return halted;
}

uint8_t usbip_device_reserve_endpoint(usbip_device *dev, uint8_t want_addr)
{
    if (!dev || dev->cur_ifnum < 0) /* no interface yet: nothing can own the claim */
        return 0;

    usb_dir dir = (want_addr & 0x80) ? USB_IN : USB_OUT;
    uint8_t owner = (uint8_t)dev->cur_ifnum;
    int ep_num = ep_alloc_number(dev, owner, want_addr & 0x0f, dir);

    if (ep_num < 0)
        return 0;

    dev->ep_reserved[ep_num][dir] = (uint8_t)(owner + 1);
    return (uint8_t)(ep_num | (dir == USB_IN ? 0x80 : 0x00));
}

int usbip_device_get_num_interfaces(usbip_device *dev)
{
    return dev->n_ifnums;
}

void usbip_device_set_interface_string(usbip_device *dev, uint8_t ifnum, uint8_t alt, uint8_t istr)
{
    struct ifalt *ia = ifalt_for(dev, ifnum, alt);
    if (ia)
        dev->descr[ia->off + 8] = istr; /* iInterface */
}

/* ---- request routing registration --------------------------------------- */
void usbip_device_on_control(usbip_device *dev, int ifnum, usbip_device_control_fn cb, void *ctx)
{
    if (ifnum < 0)
    {
        dev->ctrl_default.cb = cb;
        dev->ctrl_default.ctx = ctx;
        return;
    }
    if (ifnum < 16)
    {
        dev->ctrl[ifnum].cb = cb;
        dev->ctrl[ifnum].ctx = ctx;
    }
}

void usbip_device_on_set_alt(usbip_device *dev, int ifnum, usbip_device_set_alt_fn cb, void *ctx)
{
    if (ifnum >= 0 && ifnum < 16)
    {
        dev->set_alt_tab[ifnum].cb = cb;
        dev->set_alt_tab[ifnum].ctx = ctx;
    }
}

void usbip_ep_on_out(usbip_ep *ep, usbip_ep_out_fn cb, void *ctx)
{
    if (!ep)
        return;

    ep->on_out = cb;
    ep->on_out_ctx = ctx;
}

void usbip_ep_on_iso(usbip_ep *ep, usbip_ep_iso_fn cb, void *ctx)
{
    if (!ep)
        return;

    ep->on_iso = cb;
    ep->on_iso_ctx = ctx;
}

/* ---- descriptor groups --------------------------------------------------- */
int usbip_device_group_begin(usbip_device *dev)
{
    if (dev->n_groups >= (int)(sizeof(dev->groups) / sizeof(dev->groups[0])))
        return USB_ERROR_NO_MEM;

    dev->gmark.descr_len = dev->descr_len;
    dev->gmark.n_ifalts = dev->n_ifalts;
    dev->gmark.n_eps = dev->n_eps;
    dev->gmark.n_msos = dev->n_msos;
    dev->gmark.n_ifnums = dev->n_ifnums;
    dev->gmark.cur_ifnum = dev->cur_ifnum;
    dev->gmark.cur_alt = dev->cur_alt;
    memcpy(dev->gmark.if_present, dev->if_present, sizeof(dev->if_present));
    memcpy(dev->gmark.ep_reserved, dev->ep_reserved, sizeof(dev->ep_reserved));
    memcpy(dev->gmark.ctrl, dev->ctrl, sizeof(dev->ctrl));
    dev->gmark.ctrl_default = dev->ctrl_default;
    memcpy(dev->gmark.set_alt_tab, dev->set_alt_tab, sizeof(dev->set_alt_tab));
    struct descr_group *group = &dev->groups[dev->n_groups];
    group->first_ifnum = 0;
    group->n_ifs = 0;
    group->descr_off = dev->descr_len;
    return dev->n_groups++;
}

void usbip_device_group_abort(usbip_device *dev)
{
    if (!dev->n_groups)
        return;
    int gid = --dev->n_groups;
    for (int i = dev->gmark.n_eps; i < dev->n_eps; i++)
    {
        struct usbip_ep *ep = dev->eps[i];
        if (dev->ep_map[ep->number][ep->dir] == ep)
            dev->ep_map[ep->number][ep->dir] = NULL;
        pthread_mutex_destroy(&ep->lock);
        pthread_cond_destroy(&ep->cond);
        free(ep);
    }

    dev->n_eps = dev->gmark.n_eps;

    for (int i = 0; i < dev->n_msos;)
    { /* only the aborted group's advertisements; device-wide entries survive */
        if (dev->msos[i].group == gid)
        {
            free(dev->msos[i].compatible);
            free(dev->msos[i].guid);
            dev->msos[i] = dev->msos[dev->n_msos - 1];
            memset(&dev->msos[--dev->n_msos], 0, sizeof(dev->msos[0]));
        }
        else
            i++;
    }

    dev->descr_len = dev->gmark.descr_len;
    dev->n_ifalts = dev->gmark.n_ifalts;
    dev->n_ifnums = dev->gmark.n_ifnums;
    dev->cur_ifnum = dev->gmark.cur_ifnum;
    dev->cur_alt = dev->gmark.cur_alt;
    memcpy(dev->if_present, dev->gmark.if_present, sizeof(dev->if_present));
    memcpy(dev->ep_reserved, dev->gmark.ep_reserved, sizeof(dev->ep_reserved));
    memcpy(dev->ctrl, dev->gmark.ctrl, sizeof(dev->ctrl));
    dev->ctrl_default = dev->gmark.ctrl_default;
    memcpy(dev->set_alt_tab, dev->gmark.set_alt_tab, sizeof(dev->set_alt_tab));
}

/* ---- USBIP_DEBUG tracing ----------------------------------------------- */
/* Set USBIP_DEBUG for one stderr line per transfer. Read once, so a release
 * build pays a single getenv. */
static int dbg_on(void)
{
    static int on = -1;
    if (on < 0)
        on = getenv("USBIP_DEBUG") ? 1 : 0;
    return on;
}

/* One line per transfer: direction, address, type, bytes moved - traffic without a
 * PCAP capture. @p type is a usb_xfer_type, or -1 when the endpoint is unknown. */
static void dbg_xfer(int dir, uint8_t epnum, int type, int len, const char *note)
{
    /* the same type names the Python core prints, so the two logs read alike */
    static const char *const type_name[] = {"control", "iso", "bulk", "interrupt"};
    if (!dbg_on())
        return;
    fprintf(stderr, "[usbip_device] %s ep=0x%02x %-9s len=%d%s%s\n",
            dir == USB_IN ? "IN " : "OUT",
            (unsigned)(epnum | (dir == USB_IN ? 0x80 : 0)),
            (type >= 0 && type <= 3) ? type_name[type] : "?",
            len,
            note ? " " : "",
            note ? note : "");
}

/* ---- data path --------------------------------------------------------- */
int usbip_device_write(usbip_ep *ep, const void *buf, int len, unsigned timeout_ms)
{
    (void)timeout_ms;
    if (!ep)
        return USB_ERROR_NO_DEVICE;

    pthread_mutex_lock(&ep->lock);
    const uint8_t *src = buf;
    int remaining = len;
    /* feed parked IN URBs first, honouring each URB's requested length: one big write
     * spreads across several short reads rather than overflowing a single URB */
    while (remaining > 0 && ep->pend_head)
    {
        struct pending *pend = ep->pend_head;
        ep->pend_head = pend->next;
        if (!ep->pend_head)
            ep->pend_tail = NULL;

        int chunk = remaining;
        if (pend->length && chunk > (int)pend->length)
            chunk = (int)pend->length;

        pthread_mutex_unlock(&ep->lock);
        usbip_send_ret(pend->conn, pend->seqnum, pend->devid, pend->direction, pend->ep, 0, src, chunk);
        dbg_xfer(USB_IN, pend->ep, ep->type, chunk, "parked URB");
        free(pend);
        src += chunk;
        remaining -= chunk;
        pthread_mutex_lock(&ep->lock);
    }

    if (remaining > 0) /* queue the rest for future IN URBs */
        enq_dbuf(&ep->in_head, &ep->in_tail, src, remaining);

    pthread_mutex_unlock(&ep->lock);
    return len;
}

int usbip_device_read(usbip_ep *ep, void *buf, int len, unsigned timeout_ms)
{
    (void)timeout_ms;
    if (!ep)
        return USB_ERROR_NO_DEVICE;

    pthread_mutex_lock(&ep->lock);
    while (!ep->out_head)
        pthread_cond_wait(&ep->cond, &ep->lock);

    struct dbuf *dbuf = deq_dbuf(&ep->out_head, &ep->out_tail);
    pthread_mutex_unlock(&ep->lock);
    int count = dbuf->len < len ? dbuf->len : len;
    memcpy(buf, dbuf->data, count);
    free(dbuf);
    return count;
}

/* ---- Microsoft OS descriptors (WinUSB auto-binding) ------------------- */
/* MS OS 2.0 platform-capability UUID {D8DD60DF-4589-4CC7-9CD2-659D9E648A9F}, in GUID mixed-endian order */
static const uint8_t MSOS20_UUID[16] = {
    0xDF, 0x60, 0xDD, 0xD8,             /* D8DD60DF */
    0x89, 0x45,                         /* 4589 */
    0xC7, 0x4C,                         /* 4CC7 */
    0x9C, 0xD2,                         /* 9CD2 */
    0x65, 0x9D, 0x9E, 0x64, 0x8A, 0x9F, /* 659D9E648A9F */
};

/* WebUSB platform-capability UUID {3408b638-09a9-47a0-8bfd-a0768815b665}, in the
 * GUID's mixed-endian wire order (per the WebUSB spec) */
static const uint8_t WEBUSB_UUID[16] = {
    0x38, 0xB6, 0x08, 0x34,             /* 3408b638 */
    0xA9, 0x09,                         /* 09a9 */
    0xA0, 0x47,                         /* 47a0 */    
    0x8B, 0xFD,                         /* 8bfd */
    0xA0, 0x76, 0x88, 0x15, 0xB6, 0x65, /* a0768815b665 */
};

/* write an ASCII string as UTF-16LE plus `n_nul` trailing 16-bit NULs; returns bytes */
static int put_utf16(uint8_t *buf, const char *str, int n_nul)
{
    int off = 0;
    for (; *str; str++)
    {
        buf[off++] = (uint8_t)*str;
        buf[off++] = 0;
    }
    while (n_nul-- > 0)
    {
        buf[off++] = 0;
        buf[off++] = 0;
    }
    return off;
}

/* the 0xEE OS String Descriptor: "MSFT100" + the MS-OS-1.0 vendor request code */
static int msos1_string(uint8_t vendor, uint8_t *buf)
{
    buf[0] = 0x12;
    buf[1] = USB_DT_STRING;
    int off = 2 + put_utf16(buf + 2, "MSFT100", 0); /* -> 16 */
    buf[off++] = vendor;
    buf[off++] = 0x00; /* -> 18 */
    return off;
}

/* How many descriptor groups the MS-OS emitters fan out over. A flat-authored
 * device is one implicit group covering everything. */
static int msos_n_groups(usbip_device *dev)
{
    return dev->n_groups ? dev->n_groups : 1;
}

/* Which bInterfaceNumber a group starts at. Resolved at emit time: a class enables
 * MS OS descriptors from its build(), before its interfaces exist. */
static uint8_t group_first_ifnum(usbip_device *dev, int group)
{
    if (group >= dev->n_groups || !dev->groups[group].n_ifs)
        return 0;
    return dev->groups[group].first_ifnum;
}

/* The entry describing a group: group-scoped wins, else the device-wide entry fans
 * out, else the group is not advertised and binds by class. */
static const struct msos_entry *msos_entry_for_group(usbip_device *dev, int group)
{
    for (int i = 0; i < dev->n_msos; i++)
        if (dev->msos[i].group == group)
            return &dev->msos[i];

    for (int i = 0; i < dev->n_msos; i++)
        if (dev->msos[i].group < 0)
            return &dev->msos[i];

    return NULL;
}

/* Whether the MS OS 2.0 set must scope its features per function. On a composite,
 * a feature directly under the set header applies device-wide and would bind one
 * driver over every usbccgp child. A lone function keeps the flat form. */
static int msos_needs_subsets(usbip_device *dev)
{
    return dev->n_groups > 1 && dev->n_msos > 0;
}

/* The entry for a bInterfaceNumber: exact group match wins, else device-wide. NULL
 * when nothing covers it, so the caller cannot leak another function's GUID. A
 * group's interfaces run consecutively from its first, so a range test suffices. */
static const struct msos_entry *msos_entry_for_interface(usbip_device *dev, int ifnum)
{
    for (int i = 0; i < dev->n_msos; i++)
    {
        int group = dev->msos[i].group;
        if (group < 0 || group >= dev->n_groups)
            continue;

        if (ifnum >= dev->groups[group].first_ifnum &&
            ifnum < dev->groups[group].first_ifnum + dev->groups[group].n_ifs)
            return &dev->msos[i];
    }
    for (int i = 0; i < dev->n_msos; i++)
        if (dev->msos[i].group < 0)
            return &dev->msos[i];

    return NULL;
}

/* MS OS 1.0 Compatible-ID feature descriptor: one 24-byte section per covered group,
 * each naming the interface it starts at. A device-wide entry fans out to all. */
static int msos1_compatid(usbip_device *dev, uint8_t *buf)
{
    int count = 0;
    struct
    {
        uint8_t first;
        const struct msos_entry *entry;
    } sec[8];

    const int n_groups = msos_n_groups(dev);
    const int max_sections = (int)(sizeof(sec) / sizeof(sec[0]));

    for (int i = 0; i < n_groups && count < max_sections; i++)
    {
        const struct msos_entry *entry = msos_entry_for_group(dev, i);
        if (!entry)
            continue;
        sec[count].first = group_first_ifnum(dev, i);
        sec[count].entry = entry;
        count++;
    }
    int total = MSOS1_COMPATID_HDR_LEN + MSOS1_COMPATID_SEC_LEN * count;
    memset(buf, 0, (size_t)total);
    usb_put_le32(buf, (uint32_t)total);
    usb_put_le16(buf + 4, MSOS1_BCD_VERSION);
    usb_put_le16(buf + 6, MSOS1_IDX_COMPAT_ID);
    buf[8] = (uint8_t)count; /* bCount */
    for (int i = 0; i < count; i++)
    {
        uint8_t *section = buf + MSOS1_COMPATID_HDR_LEN + MSOS1_COMPATID_SEC_LEN * i;
        section[0] = sec[i].first; /* bFirstInterfaceNumber */
        section[1] = 1;            /* bReserved */
        size_t cl = strlen(sec[i].entry->compatible);
        if (cl > MSOS1_COMPATID_MAX)
            cl = MSOS1_COMPATID_MAX;
        memcpy(section + 2, sec[i].entry->compatible, cl); /* compatibleID, space-free fixed field */
    }
    return total;
}

/* UTF-16LE size of an ASCII string with n_nul trailing 16-bit NULs (put_utf16's output) */
static int utf16_len(const char *str, int n_nul)
{
    return 2 * ((int)strlen(str) + n_nul);
}

/* Exact byte size msos1_extprops(guid, ...) will write. */
static int msos1_extprops_size(const char *guid)
{
    if (!guid)
        return MSOS1_EXTPROP_HDR_LEN;

    return MSOS1_EXTPROP_HDR_LEN + 10 + utf16_len("DeviceInterfaceGUID", 1) + 4 + utf16_len(guid, 1);
}

/* MS OS 1.0 Extended-Properties feature descriptor: one REG_SZ DeviceInterfaceGUID,
 * or an empty set (0 properties) when guid is NULL (e.g. MTP) */
static int msos1_extprops(const char *guid, uint8_t *buf)
{
    if (!guid)
    {
        usb_put_le32(buf, MSOS1_EXTPROP_HDR_LEN);
        usb_put_le16(buf + 4, MSOS1_BCD_VERSION);
        usb_put_le16(buf + 6, MSOS1_IDX_EXT_PROPS);
        usb_put_le16(buf + 8, 0); /* wCount: no properties */
        return MSOS1_EXTPROP_HDR_LEN;
    }
    int str = MSOS1_EXTPROP_HDR_LEN; /* sections follow the header */
    int name_off = str + 10;
    int nlen = put_utf16(buf + name_off, "DeviceInterfaceGUID", 1);
    int data_len_off = name_off + nlen;
    int data_off = data_len_off + 4;
    int dlen = put_utf16(buf + data_off, guid, 1);
    int seclen = 10 + nlen + 4 + dlen;
    usb_put_le32(buf + str, seclen);
    usb_put_le32(buf + str + 4, MSOS1_PROP_REG_SZ);
    usb_put_le16(buf + str + 8, nlen);
    usb_put_le32(buf + data_len_off, dlen);
    int total = MSOS1_EXTPROP_HDR_LEN + seclen;
    usb_put_le32(buf, total);
    usb_put_le16(buf + 4, MSOS1_BCD_VERSION);
    usb_put_le16(buf + 6, MSOS1_IDX_EXT_PROPS);
    usb_put_le16(buf + 8, 1); /* wCount: one property */
    return total;
}

/* Exact byte size msos2_feature(compatible, guid, ...) will write. */
static int msos2_feature_size(const char *guid)
{
    int total = 20; /* the Compatible-ID feature */
    if (guid)
        total += 8 + utf16_len("DeviceInterfaceGUIDs", 1) + 2 + utf16_len(guid, 2);
    return total;
}

/* One MS OS 2.0 feature block: a Compatible-ID, plus a REG_MULTI_SZ property when
 * guid is non-NULL (omitted for MTP, which needs no DeviceInterfaceGUID). */
static int msos2_feature(const char *compatible, const char *guid, uint8_t *buf)
{
    usb_put_le16(buf, 0x0014);
    usb_put_le16(buf + 2, 0x0003); /* Compatible-ID feature */
    memset(buf + 4, 0, 16);
    size_t cl = strlen(compatible);

    if (cl > 8)
        cl = 8;

    memcpy(buf + 4, compatible, cl);
    int total = 20;
    if (guid)
    { /* optional registry-property feature */
        uint8_t *prop = buf + total;
        int name_off = 8;
        int nlen = put_utf16(prop + name_off, "DeviceInterfaceGUIDs", 1);
        int data_len_off = name_off + nlen;
        int data_off = data_len_off + 2;
        int dlen = put_utf16(prop + data_off, guid, 2); /* REG_MULTI_SZ: double NUL */
        int reglen = 8 + nlen + 2 + dlen;
        usb_put_le16(prop, reglen);
        usb_put_le16(prop + 2, 0x0004);
        usb_put_le16(prop + 4, 0x0007);
        usb_put_le16(prop + 6, nlen);
        usb_put_le16(prop + data_len_off, dlen);
        total += reglen;
    }
    return total;
}

/* MS OS 2.0 descriptor set.
 *
 * Lone function: set header + the feature, device-wide.
 *
 * Composite: set header + a Configuration Subset wrapping one Function Subset per
 * advertised function, each naming its bFirstInterface, so the Compatible ID lands
 * on that devnode rather than the whole device. */
/* Exact byte size msos2_set() will write; keep the two in lockstep. The BOS platform
 * capability advertises this length, and a mismatch makes Windows reject the set. */
static int msos2_set_size(usbip_device *dev)
{
    int off = 10;
    if (!msos_needs_subsets(dev))
    {
        const struct msos_entry *entry = msos_entry_for_group(dev, 0);
        if (!entry && dev->n_msos)
            entry = &dev->msos[0];
        if (entry)
            off += msos2_feature_size(entry->guid);
    }
    else
    {
        int coff = 8;
        for (int i = 0; i < msos_n_groups(dev); i++)
        {
            const struct msos_entry *entry = msos_entry_for_group(dev, i);
            if (entry)
                coff += 8 + msos2_feature_size(entry->guid);
        }
        off += coff;
    }
    return off;
}

static int msos2_set(usbip_device *dev, uint8_t *buf)
{
    int off = 10; /* after the 10-byte set header */
    if (!msos_needs_subsets(dev))
    {
        const struct msos_entry *entry = msos_entry_for_group(dev, 0);
        if (!entry && dev->n_msos)
            entry = &dev->msos[0];
        if (entry)
            off += msos2_feature(entry->compatible, entry->guid, buf + off);
    }
    else
    {
        uint8_t *cfg = buf + off;
        int coff = 8; /* after the configuration subset header */
        for (int i = 0; i < msos_n_groups(dev); i++)
        {
            const struct msos_entry *entry = msos_entry_for_group(dev, i);
            if (!entry)
                continue;
            uint8_t *fs = cfg + coff;
            int foff = 8; /* after the function subset header */
            foff += msos2_feature(entry->compatible, entry->guid, fs + foff);
            usb_put_le16(fs, 8);
            usb_put_le16(fs + 2, 0x0002);        /* Function Subset */
            fs[4] = group_first_ifnum(dev, i);   /* bFirstInterface */
            fs[5] = 0;
            usb_put_le16(fs + 6, (uint16_t)foff); /* wSubsetLength */
            coff += foff;
        }
        usb_put_le16(cfg, 8);
        usb_put_le16(cfg + 2, 0x0001); /* Configuration Subset */
        cfg[4] = 0;                    /* bConfigurationValue: an INDEX, so 0 = the first config */
        cfg[5] = 0;
        usb_put_le16(cfg + 6, (uint16_t)coff); /* wTotalLength */
        off += coff;
    }
    usb_put_le16(buf, 0x000A);
    usb_put_le16(buf + 2, 0x0000);
    usb_put_le32(buf + 4, 0x06030000);
    usb_put_le16(buf + 8, (uint16_t)off);
    return off;
}

/* WebUSB URL descriptor: bLength, bDescriptorType(0x03 URL), bScheme, UTF-8 URL with
 * the scheme prefix stripped (0=http:// 1=https:// 255=URL carries its own scheme) */
static int build_webusb_url(const char *url, uint8_t *buf)
{
    uint8_t scheme = 255;
    if (strncmp(url, "https://", 8) == 0)
    {
        scheme = WEBUSB_SCHEME_HTTPS;
        url += 8;
    }
    else if (strncmp(url, "http://", 7) == 0)
    {
        scheme = WEBUSB_SCHEME_HTTP;
        url += 7;
    }
    int url_len = (int)strlen(url);
    buf[0] = (uint8_t)(WEBUSB_URL_HDR_LEN + url_len);
    buf[1] = WEBUSB_URL_DESC_TYPE;
    buf[2] = scheme; /* the prefix is stripped, not sent */
    memcpy(buf + WEBUSB_URL_HDR_LEN, url, url_len);
    return WEBUSB_URL_HDR_LEN + url_len;
}

/* BOS descriptor: a WebUSB platform-capability descriptor (when WebUSB is enabled)
 * followed by an MS OS 2.0 platform-capability descriptor (when WinUSB is enabled) */
static int build_bos_desc(usbip_device *dev, uint8_t *buf)
{
    int off = USB_BOS_HDR_LEN, ncaps = 0; /* caps start after the BOS header */
    if (dev->webusb)
    { /* WebUSB platform capability (24 bytes) */
        uint8_t *cap = buf + off;
        cap[0] = WEBUSB_CAP_LEN;
        cap[1] = USB_DT_DEVICE_CAPABILITY;
        cap[2] = USB_CAP_TYPE_PLATFORM;
        cap[3] = 0; /* bReserved */
        memcpy(cap + USB_PLATFORM_CAP_HDR, WEBUSB_UUID, USB_PLATFORM_UUID_LEN);
        usb_put_le16(cap + 20, WEBUSB_BCD_VERSION);
        cap[22] = dev->webusb_vendor;                              /* bVendorCode (GET_URL) */
        cap[23] = (dev->webusb_url && dev->webusb_url[0]) ? 1 : 0; /* iLandingPage */
        off += WEBUSB_CAP_LEN;
        ncaps++;
    }
    if (dev->n_msos)
    {                                   /* MS OS 2.0 platform capability (28 bytes) */
        int slen = msos2_set_size(dev); /* length only; the set itself is served on
                                         * the vendor request (wIndex 0x0007) */
        uint8_t *cap = buf + off;
        cap[0] = MSOS2_CAP_LEN;
        cap[1] = USB_DT_DEVICE_CAPABILITY;
        cap[2] = USB_CAP_TYPE_PLATFORM;
        cap[3] = 0; /* bReserved */
        memcpy(cap + USB_PLATFORM_CAP_HDR, MSOS20_UUID, USB_PLATFORM_UUID_LEN);
        usb_put_le32(cap + 20, MSOS2_WINDOWS_8_1);
        usb_put_le16(cap + 24, slen);
        cap[26] = dev->msos2_vendor; /* bAltEnumCode below */
        cap[27] = 0;
        off += MSOS2_CAP_LEN;
        ncaps++;
    }
    buf[0] = USB_BOS_HDR_LEN;
    buf[1] = USB_DT_BOS;
    usb_put_le16(buf + 2, off);
    buf[4] = (uint8_t)ncaps;
    return off;
}

/* ---- descriptors (standard only) -------------------------------------- */
/* Written as one array in wire order: USB_U16LE spells out the 16-bit fields, where
 * a memcpy of the USB_PACKED struct would put them in HOST order. The _Static_asserts
 * tie the array lengths to the typed structs. */
_Static_assert(sizeof(usb_device_descriptor) == 18, "device descriptor must be 18 bytes");
_Static_assert(sizeof(usb_config_descriptor) == 9, "config descriptor must be 9 bytes");

static int build_device_desc(usbip_device *dev, uint8_t *buf)
{
    const uint8_t desc[] = {
        sizeof(usb_device_descriptor), USB_DT_DEVICE,
        USB_U16LE(dev->bcdUSB),
        dev->dev_class, dev->dev_sub, dev->dev_proto,
        USB_EP0_MAX_PACKET,
        USB_U16LE(dev->vid),
        USB_U16LE(dev->pid),
        USB_U16LE(dev->bcdDevice),
        dev->i_mfr, dev->i_prod, dev->i_ser,
        USB_NUM_CONFIGURATIONS,
    };
    _Static_assert(sizeof(desc) == sizeof(usb_device_descriptor), "device descriptor size");
    memcpy(buf, desc, sizeof(desc));
    return (int)sizeof(desc);
}

static int build_config_desc(usbip_device *dev, uint8_t *buf, int cap)
{
    int total = (int)sizeof(usb_config_descriptor) + dev->descr_len;
    if (total > cap)
        total = cap;

    const uint8_t cfg[] = {
        sizeof(usb_config_descriptor), USB_DT_CONFIG,
        USB_U16LE(total),
        (uint8_t)dev->n_ifnums, USB_CONFIG_VALUE, 0 /* iConfiguration */,
        USB_CONFIG_ATTR_BUS_POWERED, USB_CONFIG_MAX_POWER_100MA,
    };
    _Static_assert(sizeof(cfg) == sizeof(usb_config_descriptor), "config descriptor size");
    memcpy(buf, cfg, sizeof(cfg));

    int off = (int)sizeof(cfg);
    if (off < total)
        memcpy(buf + off, dev->descr, (size_t)(total - off));
    return total;
}

static int build_string_desc(usbip_device *dev, uint8_t idx, uint8_t *buf, int *status)
{
    if (idx == 0)
    {
        buf[0] = 4;
        buf[1] = USB_DT_STRING;
        buf[2] = 0x09;
        buf[3] = 0x04;
        return 4;
    }

    if (idx == MSOS1_STRING_INDEX && dev->n_msos) /* MS OS 1.0 */
        return msos1_string(dev->msos1_vendor, buf);

    const char *str = (idx < dev->n_strings) ? dev->strings[idx] : NULL;
    if (!str)
    {
        *status = USBIP_STATUS_STALL;
        return 0;
    }

    int len = (int)strlen(str);
    buf[0] = (uint8_t)(2 + 2 * len);
    buf[1] = USB_DT_STRING;
    for (int i = 0; i < len; i++)
    {
        buf[2 + 2 * i] = (uint8_t)str[i];
        buf[3 + 2 * i] = 0;
    }
    return 2 + 2 * len;
}

int pack_usbip_device(usbip_device *dev, uint8_t out[USBIP_DEV_LEN])
{
    /* The offsets come from usbip_device_internal.h so this packer and the importer's
     * parse_devinfo() in usbip.c cannot drift apart. Everything here is big-endian. */
    memset(out, 0, USBIP_DEV_LEN);
    /* busid/busnum/devnum are assigned when the device joins a listener; before that
     * - a direct call from a test - fall back to the lone-device values. */
    const char *busid = dev->busid[0] ? dev->busid : "1-1";
    snprintf((char *)out + USBIP_DEV_PATH_OFF, USBIP_DEV_PATH_LEN, "/sys/devices/platform/vhci_hcd.0/usb1/%s", busid);
    snprintf((char *)out + USBIP_DEV_BUSID_OFF, USBIP_DEV_BUSID_LEN, "%s", busid);

    /* map usb_speed (LOW/FULL/HIGH/SUPER) -> kernel usb_device_speed wire value */
    static const uint32_t wire_speed[] = {1, 2, 3, 5};
    uint32_t speed = wire_speed[(dev->speed <= USB_SPEED_SUPER) ? dev->speed : USB_SPEED_FULL];

    uint32_t busnum = dev->busnum ? dev->busnum : USBIP_DEV_BUSNUM;
    uint32_t devnum = dev->devnum ? dev->devnum : USBIP_DEV_DEVNUM;

    usb_put_be32(out + USBIP_DEV_BUSNUM_OFF, busnum);
    usb_put_be32(out + USBIP_DEV_DEVNUM_OFF, devnum);
    usb_put_be32(out + USBIP_DEV_SPEED_OFF, speed);
    usb_put_be16(out + USBIP_DEV_VID_OFF, dev->vid);
    usb_put_be16(out + USBIP_DEV_PID_OFF, dev->pid);
    usb_put_be16(out + USBIP_DEV_BCDDEVICE_OFF, dev->bcdDevice);
    out[USBIP_DEV_CLASS_OFF]     = dev->dev_class;
    out[USBIP_DEV_SUBCLASS_OFF]  = dev->dev_sub;
    out[USBIP_DEV_PROTOCOL_OFF]  = dev->dev_proto;
    out[USBIP_DEV_CFGVALUE_OFF]  = 1;
    out[USBIP_DEV_NUM_CFG_OFF]   = 1;
    out[USBIP_DEV_NUM_IFACE_OFF] = (uint8_t)dev->n_ifnums;

    return USBIP_DEV_LEN;
}

/* ---- control routing (class-agnostic) --------------------------------- */
/* Interface-recipient requests go to their bInterfaceNumber's registered handler;
 * everything else (and an unregistered ifnum) goes to the device-level fallback. */
static const struct ctrl_slot *route(usbip_device *dev, const usb_setup *setup)
{
    if ((setup->bmRequestType & 0x1f) == 1)
    { /* interface recipient */
        uint8_t ifnum = USB_U16_LSB(setup->wIndex);
        if (ifnum < 16 && dev->ctrl[ifnum].cb)
            return &dev->ctrl[ifnum];
    }
    return dev->ctrl_default.cb ? &dev->ctrl_default : NULL;
}

/* opt-in (USBIP_DEBUG=1): log every control request + whether we answered or
 * STALLed it - used to catch anything a host (e.g. Windows) sends unhandled. */
static void dbg_ctrl(const usb_setup *setup, int status)
{
    if (!dbg_on())
        return;
    fprintf(stderr, "[usbip_device] ctrl type=0x%02x req=0x%02x val=0x%04x idx=0x%04x len=%u -> %s\n",
            setup->bmRequestType, setup->bRequest, setup->wValue, setup->wIndex, setup->wLength,
            status ? "STALL" : "ok");
}

static void handle_control(struct conn *conn, struct urb *urb)
{
    usbip_device *dev = conn->dev;
    usb_setup setup;
    memcpy(&setup, urb->setup, sizeof(setup));   /* USB_PACKED stores LE, so this is the wire form */

    uint8_t buf[4096]; /* control IN scratch: fits DFU UPLOAD (wTransferSize) + BOS/config */
    int reply_len = 0, status = 0, core_handled = 0;
    int is_in = (setup.bmRequestType & USB_REQ_DIR_IN) != 0;

    if (USB_REQ_TYPE(setup.bmRequestType) == USB_STANDARD)
    {
        switch (setup.bRequest)
        {
        case USB_REQ_GET_DESCRIPTOR:
        {
            uint8_t dt = USB_U16_MSB(setup.wValue);
            if (dt == USB_DT_DEVICE)
            {
                reply_len = build_device_desc(dev, buf);
                core_handled = 1;
            }
            else if (dt == USB_DT_CONFIG)
            {
                reply_len = build_config_desc(dev, buf, sizeof(buf));
                core_handled = 1;
            }
            else if (dt == USB_DT_STRING)
            {
                uint8_t index = USB_U16_LSB(setup.wValue);

                reply_len = build_string_desc(dev, index, buf, &status);
                core_handled = 1;
            }
            else if (dt == USB_DT_BOS && (dev->n_msos || dev->webusb))
            {
                reply_len = build_bos_desc(dev, buf);
                core_handled = 1;
            } /* BOS (WebUSB / MS OS 2.0) */
            else if (dt < USB_DT_CLASS_SPECIFIC_BASE)
            {
                status = USBIP_STATUS_STALL;
                core_handled = 1;
            } /* unsupported standard desc (e.g. device qualifier) -> STALL */
            /* else: a class-specific descriptor type -> fall through to the interface */
            break;
        }

        case USB_REQ_SET_CONFIGURATION:
            dev->config = USB_U16_LSB(setup.wValue);
            for (int i = 0; i < dev->n_eps; i++) /* (re)configure clears every halt */
                usbip_ep_clear_halt(dev->eps[i]);
            core_handled = 1;
            break;

        case USB_REQ_GET_CONFIGURATION:
            buf[0] = (uint8_t)dev->config;
            reply_len = 1;
            core_handled = 1;
            break;

        case USB_REQ_GET_INTERFACE:
            buf[0] = 0; /* only alt 0 is reported; SET_INTERFACE is passed to the class */
            reply_len = 1;
            core_handled = 1;
            break;

        case USB_REQ_SET_INTERFACE:
        {
            uint8_t ifnum = USB_U16_LSB(setup.wIndex), alt = USB_U16_LSB(setup.wValue);
            if (ifnum < 16 && dev->set_alt_tab[ifnum].cb)
                dev->set_alt_tab[ifnum].cb(dev->set_alt_tab[ifnum].ctx, ifnum, alt);
            core_handled = 1;
            break;
        }

        case USB_REQ_GET_STATUS:
        {
            usbip_ep *ep = ep_by_addr(dev, USB_U16_LSB(setup.wIndex));
            int for_endpoint = (USB_REQ_RECIP(setup.bmRequestType) == USB_RECIP_ENDPOINT);

            buf[0] = 0; /* device: not self-powered, no remote wakeup */
            buf[1] = 0;
            if (for_endpoint && usbip_ep_is_halted(ep))
                buf[0] = USB_STATUS_ENDPOINT_HALT;

            reply_len = 2;
            core_handled = 1;
            break;
        }

        /* The halt is ours to track: the host clears it to recover a stalled pipe,
         * as MSC does after a failed data phase. Other features are vhci's. */
        case USB_REQ_CLEAR_FEATURE:
        case USB_REQ_SET_FEATURE:
            if (USB_REQ_RECIP(setup.bmRequestType) == USB_RECIP_ENDPOINT &&
                setup.wValue == USB_FEATURE_ENDPOINT_HALT)
            {
                uint8_t addr = USB_U16_LSB(setup.wIndex);
                usbip_ep *ep = ep_by_addr(dev, addr);

                if (setup.bRequest == USB_REQ_CLEAR_FEATURE)
                    usbip_ep_clear_halt(ep);
                else
                    usbip_ep_stall(ep);
            }
            core_handled = 1;
            break;

        /* vhci assigns the address itself; accept and ignore. */
        case USB_REQ_SET_ADDRESS:
            core_handled = 1;
            break;

        default:
            break;
        }
    }

    /* MS-OS (WinUSB) + WebUSB vendor requests are core, not class. Each branch is
     * gated on its own flag, so a stray request can't match a zero vendor code. */
    if (!core_handled && is_in && USB_REQ_TYPE(setup.bmRequestType) == USB_VENDOR)
    {
        if (dev->n_msos && setup.bRequest == dev->msos1_vendor && setup.wIndex == MSOS1_IDX_COMPAT_ID)
        {
            reply_len = msos1_compatid(dev, buf);
            core_handled = 1;
        }
        /* Extended properties are per-interface: the recipient interface is in wValue's
         * high byte, so a composite answers each function with its own GUID. */
        else if (dev->n_msos && setup.bRequest == dev->msos1_vendor && setup.wIndex == MSOS1_IDX_EXT_PROPS)
        {
            uint8_t ifnum = USB_U16_MSB(setup.wValue);
            const struct msos_entry *entry = msos_entry_for_interface(dev, ifnum);
            const char *guid = entry ? entry->guid : NULL;

            if (msos1_extprops_size(guid) <= (int)sizeof(buf))
                reply_len = msos1_extprops(guid, buf);
            else
                status = USBIP_STATUS_STALL; /* GUID too long for the scratch buffer */

            core_handled = 1;
        }
        else if (dev->n_msos && setup.bRequest == dev->msos2_vendor && setup.wIndex == MSOS2_IDX_DESCRIPTOR)
        {
            if (msos2_set_size(dev) <= (int)sizeof(buf))
                reply_len = msos2_set(dev, buf);
            else
                status = USBIP_STATUS_STALL; /* descriptor set outgrew the scratch buffer */
            core_handled = 1;
        }
        else if (dev->webusb && setup.bRequest == dev->webusb_vendor && setup.wIndex == WEBUSB_IDX_GET_URL)
        {
            reply_len = build_webusb_url(dev->webusb_url, buf);
            core_handled = 1;
        }
    }

    if (!core_handled)
    { /* delegate to the registered handler (generic) */
        const struct ctrl_slot *slot = route(dev, &setup);
        if (!slot)
            status = USBIP_STATUS_STALL;
        else if (is_in)
        {
            int rc = slot->cb(slot->ctx, &setup, buf, setup.wLength);
            if (rc < 0)
                status = USBIP_STATUS_STALL;
            else
                reply_len = rc;
        }
        else
        {
            int rc = slot->cb(slot->ctx, &setup, urb->data, (uint16_t)urb->data_len);
            if (rc < 0)
                status = USBIP_STATUS_STALL;
        }
    }

    dbg_ctrl(&setup, status);
    if (status != 0)
        usbip_send_ret(conn, urb->seqnum, urb->devid, urb->direction, urb->ep, status, NULL, 0);
    else if (is_in)
    {
        if (reply_len > setup.wLength)
            reply_len = setup.wLength;
        usbip_send_ret(conn, urb->seqnum, urb->devid, urb->direction, urb->ep, 0, buf, reply_len);
    }
    else
        usbip_send_ret(conn, urb->seqnum, urb->devid, urb->direction, urb->ep, 0, NULL, urb->data_len);
}

void usbip_device_dispatch(struct conn *conn, struct urb *urb)
{
    usbip_device *dev = conn->dev;
    if (urb->ep == 0)
    {
        handle_control(conn, urb);
        return;
    }

    usbip_ep *ep = (urb->ep < 16) ? dev->ep_map[urb->ep][urb->direction] : NULL;
    if (!ep)
    {
        usbip_send_ret(conn, urb->seqnum, urb->devid, urb->direction, urb->ep, -19, NULL, 0);
        dbg_xfer(urb->direction, urb->ep, -1, urb->length, "-> no such endpoint");
        return;
    }

    if (usbip_ep_is_halted(ep))
    { /* every transfer STALLs until CLEAR_FEATURE(ENDPOINT_HALT) clears it */
        usbip_send_ret(conn, urb->seqnum, urb->devid, urb->direction, urb->ep, USBIP_STATUS_STALL, NULL, 0);
        dbg_xfer(urb->direction, urb->ep, ep->type, urb->length, "-> STALL");
        return;
    }

    /* isochronous IN: ask the class to fill each packet, then de-padded RET */
    if (ep->type == USB_ISO && urb->direction == USB_IN && urb->number_of_packets > 0)
    {
        int np = urb->number_of_packets;
        uint32_t total_req = 0;

        for (int i = 0; i < np; i++)
            total_req += urb->iso[i].length;

        uint8_t *buf = malloc(total_req ? total_req : 1);
        uint32_t *lens = malloc((size_t)np * sizeof(uint32_t));
        if (buf && lens)
        {
            for (int i = 0; i < np; i++) /* requested lengths in, produced lengths out */
                lens[i] = urb->iso[i].length;
            if (ep->on_iso)
                ep->on_iso(ep->on_iso_ctx, ep, np, lens, buf);
            else
                memset(lens, 0, (size_t)np * sizeof(uint32_t)); /* zero-length packets */

            uint32_t actual = 0;
            for (int i = 0; i < np; i++)
            {
                urb->iso[i].actual_length = lens[i];
                urb->iso[i].status = 0;
                actual += lens[i];
            }

            if (dev->iso_paced)
            {
                uint64_t deadline = iso_deadline(ep, np); /* deliver at real time */

                pace_submit(conn, urb, deadline, buf, actual);
            }
            else
                usbip_send_ret_iso(conn, urb, buf);
            if (dbg_on())
            {
                char note[32];
                snprintf(note, sizeof(note), "%d pkts", np);
                dbg_xfer(USB_IN, urb->ep, ep->type, (int)actual, note);
            }
        }
        free(buf);
        free(lens);
        return;
    }

    /* isochronous OUT: de-pad the host's packets and hand them to the class */
    if (ep->type == USB_ISO && urb->direction == USB_OUT && urb->number_of_packets > 0)
    {
        int np = urb->number_of_packets;
        uint32_t total = 0;
        for (int i = 0; i < np; i++)
            total += urb->iso[i].length;
        uint8_t *buf = malloc(total ? total : 1);
        uint32_t *lens = malloc((size_t)np * sizeof(uint32_t));
        if (buf && lens)
        {
            uint32_t off = 0;
            for (int i = 0; i < np; i++)
            { /* de-pad: pack packets back-to-back */
                uint32_t len = urb->iso[i].length;
                if (urb->data && urb->iso[i].offset + len <= (uint32_t)urb->data_len)
                    memcpy(buf + off, urb->data + urb->iso[i].offset, len);
                lens[i] = len;
                off += len;
                urb->iso[i].actual_length = len;
                urb->iso[i].status = 0;
            }

            if (ep->on_iso)
                ep->on_iso(ep->on_iso_ctx, ep, np, lens, buf);

            if (dev->iso_paced)
            {
                uint64_t deadline = iso_deadline(ep, np); /* deliver at real time */
                pace_submit(conn, urb, deadline, NULL, 0);
            }
            else
                usbip_send_ret_iso(conn, urb, NULL); /* OUT RET: descriptors only, no data */
            if (dbg_on())
            {
                char note[32];
                snprintf(note, sizeof(note), "%d pkts", np);
                dbg_xfer(USB_OUT, urb->ep, ep->type, (int)total, note);
            }
        }
        free(buf);
        free(lens);
        return;
    }

    if (urb->direction == USB_IN)
    {
        pthread_mutex_lock(&ep->lock);
        struct dbuf *dbuf = ep->in_head;
        if (dbuf)
        { /* serve a slice; keep the remainder queued */
            int avail = dbuf->len - dbuf->off;
            int chunk = avail;
            if (urb->length && chunk > urb->length)
                chunk = urb->length;
            uint8_t *slice = dbuf->data + dbuf->off;
            int drained = (dbuf->off + chunk >= dbuf->len);
            if (drained)
            {
                ep->in_head = dbuf->next;
                if (!ep->in_head)
                    ep->in_tail = NULL;
            }
            else
                dbuf->off += chunk;
            pthread_mutex_unlock(&ep->lock);
            usbip_send_ret(conn, urb->seqnum, urb->devid, urb->direction, urb->ep, 0, slice, chunk);
            dbg_xfer(USB_IN, urb->ep, ep->type, chunk, NULL);
            if (drained)
                free(dbuf);
        }
        else
        { /* park until usbip_device_write() feeds it */
            struct pending *pend = malloc(sizeof(*pend));
            pend->next = NULL;
            pend->conn = conn;
            pend->seqnum = urb->seqnum;
            pend->devid = urb->devid;
            pend->direction = urb->direction;
            pend->ep = urb->ep;
            pend->length = urb->length;
            if (ep->pend_tail)
                ep->pend_tail->next = pend;
            else
                ep->pend_head = pend;
            ep->pend_tail = pend;
            pthread_mutex_unlock(&ep->lock);
            dbg_xfer(USB_IN, urb->ep, ep->type, urb->length, "-> parked, no data yet");
        }
    }
    else
    { /* OUT: deliver to the endpoint's callback, or queue for usbip_device_read */
        if (ep->on_out)
            ep->on_out(ep->on_out_ctx, ep, urb->data, urb->data_len);
        else
        {
            pthread_mutex_lock(&ep->lock);
            enq_dbuf(&ep->out_head, &ep->out_tail, urb->data, urb->data_len);
            pthread_cond_signal(&ep->cond);
            pthread_mutex_unlock(&ep->lock);
        }
        usbip_send_ret(conn, urb->seqnum, urb->devid, urb->direction, urb->ep, 0, NULL, urb->data_len);
        dbg_xfer(USB_OUT, urb->ep, ep->type, urb->data_len, NULL);
    }
}

void usbip_device_handle_unlink(struct conn *conn, struct urb *urb)
{
    usbip_device *dev = conn->dev;
    /* drop the parked IN URB whose seqnum is being unlinked, if we have it */
    for (int ep_num = 0; ep_num < 16; ep_num++)
    {
        for (int dir = 0; dir < 2; dir++)
        {
            usbip_ep *ep = dev->ep_map[ep_num][dir];
            if (!ep)
                continue;
            pthread_mutex_lock(&ep->lock);
            struct pending **pp = &ep->pend_head;
            while (*pp)
            {
                if ((*pp)->seqnum == urb->unlink_seqnum)
                {
                    struct pending *dead = *pp;
                    *pp = dead->next;
                    if (ep->pend_tail == dead)
                        ep->pend_tail = NULL;
                    free(dead);
                }
                else
                    pp = &(*pp)->next;
            }
            pthread_mutex_unlock(&ep->lock);
        }
    }
    /* drop any paced iso completion for the unlinked URB: no late RET for a URB the
     * host already cancelled */
    if (conn->pace_started)
    {
        pthread_mutex_lock(&conn->pace_lock);
        struct paced_ret **pp = &conn->pace_head;
        while (*pp)
        {
            if ((*pp)->seqnum == urb->unlink_seqnum)
            {
                struct paced_ret *dead = *pp;
                *pp = dead->next;
                free(dead->iso);
                free(dead->data);
                free(dead);
            }
            else
                pp = &(*pp)->next;
        }
        pthread_mutex_unlock(&conn->pace_lock);
    }
    usbip_send_ret_unlink(conn, urb->seqnum, 0);
}

/* ---- lifecycle --------------------------------------------------------- */
/* Reject a config where two interfaces claim one endpoint address. The allocator
 * prevents this, so a failure here means a hand-built descriptor blob bypassed it.
 * Alt settings of one interface share addresses legitimately. */
static int check_endpoint_conflicts(usbip_device *dev)
{
    for (int i = 0; i < dev->n_eps; i++)
    {
        usbip_ep *ep = dev->eps[i];
        usbip_ep *held = dev->ep_map[ep->number][ep->dir];
        if (held && held->ifnum != ep->ifnum)
        {
            fprintf(stderr,
                    "usbip: endpoint 0x%02X claimed by two interfaces (%d and %d) -- "
                    "the device descriptor is invalid\n",
                    ep->addr,
                    ep->ifnum != USBIP_EP_NO_IFNUM ? ep->ifnum : -1,
                    held->ifnum != USBIP_EP_NO_IFNUM ? held->ifnum : -1);
            return USB_ERROR_INVALID_PARAM;
        }
    }
    return USB_SUCCESS;
}

/* Warn about a multi-interface function with no IAD, found by scanning the group's
 * slice of the config blob.
 *
 * A class emits its IAD at build time, so none is emitted if
 * usbip_device_set_composite() came late or never. usbccgp then splits the function
 * per interface and no COM port forms. Linux pairs them anyway, hence unnoticed. */
static void check_composite_iads(usbip_device *dev)
{
    /* Testing n_groups, not just the flag, catches the commonest slip: several
     * classes added and usbip_device_set_composite() never called. */
    if (!dev->composite && dev->n_groups < 2)
        return;
    for (int group = 0; group < dev->n_groups; group++)
    {
        if (dev->groups[group].n_ifs <= 1)
            continue;
        int end = (group + 1 < dev->n_groups) ? dev->groups[group + 1].descr_off : dev->descr_len;
        int has_iad = 0;
        for (int off = dev->groups[group].descr_off; off + 1 < end && dev->descr[off];
             off += dev->descr[off])
            if (dev->descr[off + 1] == USB_DT_INTERFACE_ASSOCIATION)
                has_iad = 1;
        if (!has_iad)
            fprintf(stderr,
                    "usbip: composite function at interface %d has %d interfaces but no "
                    "IAD -- call usbip_device_set_composite() BEFORE adding classes\n",
                    group_first_ifnum(dev, group), dev->groups[group].n_ifs);
    }
}

void usbip_device_set_busid(usbip_device *dev, const char *busid)
{
    snprintf(dev->busid, sizeof(dev->busid), "%s", busid ? busid : "");
}

const char *usbip_device_get_busid(usbip_device *dev)
{
    return dev->busid[0] ? dev->busid : "1-1";
}

int usbip_device_plug(usbip_device *dev, usb_transport *transport)
{
    const char *host = NULL;
    int port = 3240;
    int rc = check_endpoint_conflicts(dev);
    if (rc != USB_SUCCESS)
        return rc;
    check_composite_iads(dev);
    if (transport)
    {
        usbip_transport_params(transport, &host, &port);
    }
    snprintf(dev->host, sizeof(dev->host), "%s", host ? host : "0.0.0.0");
    dev->port = port;

    return usbip_serve_start(dev);
}

void usbip_device_unplug(usbip_device *dev) {
    usbip_serve_stop(dev);
}
