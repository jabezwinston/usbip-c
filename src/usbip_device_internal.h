/**
 * Copyright (C) 2026 Jabez Winston
 *
 * SPDX-License-Identifier: MIT
 *
 * Date : 04-June-2026
 *
 * Internal definitions shared by usbip_device.c, usbip.c, and the built-in classes.
 * Not installed; the public opaque types are completed here.
 *
 * Byte order: descriptor structs are serialized by memcpy, which is correct on any
 * host because USB_PACKED stores them little-endian - see usbip.h. Descriptors
 * built as raw byte arrays (uac.c, uvc.c) spell out their 16-bit fields with
 * USB_U16LE() instead, to the same effect.
 */
#ifndef USBIP_DEVICE_INTERNAL_H
#define USBIP_DEVICE_INTERNAL_H

#include <pthread.h>
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <time.h>

#include "usbip-device.h"
#include "usbip-host.h"

/* ---- host portability ---------------------------------------------------
 * The non-socket OS differences; os_compat.h holds the sockets/Winsock ones and
 * is included only by the two files that do networking. */

/* A monotonic microsecond clock, and the condvar calls that wait on deadlines
 * expressed in it. The three belong together: hand a usbip_mono_us() deadline to
 * a condvar waiting on some other clock and it expires immediately (the epochs
 * are ~boot vs. 1970 apart) - a silent bug, so nothing here calls
 * pthread_cond_timedwait() directly. POSIX pins the clock with
 * pthread_condattr_setclock(); macOS has no such call - its condvars are always
 * CLOCK_REALTIME - but does offer a relative wait, which is what a deadline wait
 * reduces to anyway. */
static inline uint64_t usbip_mono_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000u + (uint64_t)ts.tv_nsec / 1000u;
}

static inline void usbip_cond_init_mono(pthread_cond_t *cond)
{
#ifdef __APPLE__
    pthread_cond_init(cond, NULL);
#else
    pthread_condattr_t ca;
    pthread_condattr_init(&ca);
    pthread_condattr_setclock(&ca, CLOCK_MONOTONIC);
    pthread_cond_init(cond, &ca);
    pthread_condattr_destroy(&ca);
#endif
}

/* wait on cond until deadline_us, as read by usbip_mono_us() */
static inline int usbip_cond_timedwait_mono(pthread_cond_t *cond, pthread_mutex_t *lock, uint64_t deadline_us)
{
#ifdef __APPLE__
    uint64_t now = usbip_mono_us();
    uint64_t wait_us = deadline_us > now ? deadline_us - now : 0;
    struct timespec ts = {.tv_sec = (time_t)(wait_us / 1000000u),
                          .tv_nsec = (long)(wait_us % 1000000u) * 1000};
    return pthread_cond_timedwait_relative_np(cond, lock, &ts);
#else
    struct timespec ts = {.tv_sec = (time_t)(deadline_us / 1000000u),
                          .tv_nsec = (long)(deadline_us % 1000000u) * 1000};
    return pthread_cond_timedwait(cond, lock, &ts);
#endif
}

#if !defined(_WIN32) && !defined(__APPLE__)
extern char *program_invocation_short_name; /* glibc: the argv[0] basename */
#endif

/* argv[0] as the host hands it back, or NULL: a bare basename everywhere but
 * Windows, which has only the full exe path - the caller strips what it does not
 * want. glibc keeps it in a global, BSD/macOS behind getprogname(). */
static inline const char *usbip_progname(void)
{
#ifdef _WIN32
    return (__argv && __argv[0]) ? __argv[0] : NULL;
#elif defined(__APPLE__)
    return getprogname();
#else
    return program_invocation_short_name;
#endif
}

/* USB/IP commands */
#define USBIP_CMD_SUBMIT 1
#define USBIP_CMD_UNLINK 2
#define USBIP_RET_SUBMIT 3
#define USBIP_RET_UNLINK 4

/* ---- USB/IP wire layout (kernel tools/usb/usbip, usbip_common.h) --------
 * Every multi-byte field below is BIG-endian; use usb_get_be32()/usb_put_be32().
 * The offsets are shared by the server (usbip.c) and the client half, and by
 * pack_usbip_device() -- the packer and the parser have to agree, so
 * they must read the layout from one place. */
#define USBIP_PROTO_VERSION      0x0111  /* the protocol version both ends announce
                                          * (NOT version.h's USBIP_VERSION, the library's) */
#define USBIP_DEFAULT_PORT       3240
#define USBIP_LISTEN_BACKLOG     4

/* op_common: version(2) + code(2) + status(4) */
#define USBIP_OP_HDR_LEN         8
#define USBIP_OP_CODE_OFF        2
#define USBIP_OP_STATUS_OFF      4
#define USBIP_OP_STATUS_OK       0
#define USBIP_OP_STATUS_ERROR    1       /* generic refusal, e.g. IMPORT of an unknown busid */
#define USBIP_OP_REQ_IMPORT      0x8003
#define USBIP_OP_REP_IMPORT      0x0003
#define USBIP_OP_REQ_DEVLIST     0x8005
#define USBIP_OP_REP_DEVLIST     0x0005

/* usbip_usb_device: the fixed 312-byte device record */
#define USBIP_DEV_LEN            312
#define USBIP_DEV_PATH_OFF       0
#define USBIP_DEV_PATH_LEN       256
#define USBIP_DEV_BUSID_OFF      256
#define USBIP_DEV_BUSID_LEN      32
#define USBIP_DEV_BUSNUM_OFF     288
#define USBIP_DEV_DEVNUM_OFF     292
#define USBIP_DEV_SPEED_OFF      296
#define USBIP_DEV_VID_OFF        300
#define USBIP_DEV_PID_OFF        302
#define USBIP_DEV_BCDDEVICE_OFF  304
#define USBIP_DEV_CLASS_OFF      306
#define USBIP_DEV_SUBCLASS_OFF   307
#define USBIP_DEV_PROTOCOL_OFF   308
#define USBIP_DEV_CFGVALUE_OFF   309
#define USBIP_DEV_NUM_CFG_OFF    310
#define USBIP_DEV_NUM_IFACE_OFF  311
#define USBIP_IFACE_TUPLE_LEN    4       /* class, subclass, protocol, padding */
#define USBIP_DEV_BUSNUM         1       /* every exported device sits on virtual bus 1 */
#define USBIP_DEV_DEVNUM         2       /* the first device's address; +1 per device after it */
#define USBIP_MAX_DEVICES        16      /* devices one listener can export */

/* usbip_header: a fixed 48-byte basic+command header. Offsets 20/24/32/36 carry
 * different fields depending on the command, hence the separate names. */
#define USBIP_HDR_LEN            48
#define USBIP_HDR_COMMAND_OFF    0
#define USBIP_HDR_SEQNUM_OFF     4
#define USBIP_HDR_DEVID_OFF      8
#define USBIP_HDR_DIRECTION_OFF  12
#define USBIP_HDR_EP_OFF         16
#define USBIP_CMD_FLAGS_OFF      20      /* CMD_SUBMIT: transfer_flags */
#define USBIP_CMD_LENGTH_OFF     24      /* CMD_SUBMIT: transfer_buffer_length */
#define USBIP_CMD_NUMPKTS_OFF    32      /* CMD_SUBMIT: number_of_packets */
#define USBIP_CMD_INTERVAL_OFF   36      /* CMD_SUBMIT: interval */
#define USBIP_CMD_SETUP_OFF      40      /* CMD_SUBMIT: the 8-byte SETUP packet */
#define USBIP_RET_STATUS_OFF     20      /* RET_SUBMIT: status */
#define USBIP_RET_ACTUAL_OFF     24      /* RET_SUBMIT: actual_length */
#define USBIP_RET_NUMPKTS_OFF    32      /* RET_SUBMIT: number_of_packets */
#define USBIP_RET_ERRCOUNT_OFF   36      /* RET_SUBMIT: error_count */
#define USBIP_UNLINK_SEQNUM_OFF  20      /* CMD_UNLINK: the seqnum being cancelled */

/* usbip_iso_packet_descriptor, one per packet, appended after the payload */
#define USBIP_ISO_DESC_LEN       16
#define USBIP_ISO_OFFSET_OFF     0
#define USBIP_ISO_LENGTH_OFF     4
#define USBIP_ISO_ACTUAL_OFF     8
#define USBIP_ISO_STATUS_OFF     12

/* usbmon transfer_type numbering, as pcap wants it (NOT the USB bmAttributes order) */
#define USBMON_XFER_ISO          0
#define USBMON_XFER_INTR         1
#define USBMON_XFER_CTRL         2
#define USBMON_XFER_BULK         3
#define USBMON_EVENT_SUBMIT      'S'
#define USBMON_EVENT_COMPLETE    'C'
#define USBMON_STATUS_PENDING    (-115)  /* -EINPROGRESS: submitted, not yet completed */

/* The RET_SUBMIT status that STALLs a control transfer: the Linux errno vhci expects
 * on the wire (-EPIPE), NOT the library's own USB_ERROR_PIPE (-9). Never substitute. */
#define USBIP_STATUS_STALL       (-32)

struct dbuf {                       /* a queued data chunk */
    struct dbuf *next;
    int len;
    int off;                        /* bytes already consumed by partial IN reads */
    uint8_t data[];
};

struct pending {                    /* a parked IN URB awaiting data */
    struct pending *next;
    struct conn *conn;
    uint32_t seqnum, devid, direction, ep;
    uint32_t length;                /* the URB's requested length (honor short reads) */
};

#define USBIP_EP_NO_IFNUM 0xFF          /* ep->ifnum when no interface owned the append cursor */

struct usbip_ep {
    uint8_t addr, number;
    usb_dir dir;
    uint8_t type;
    uint16_t mps;
    uint8_t interval;
    usbip_device *dev;                  /* owning device (link speed for iso pacing) */
    uint8_t ifnum;                      /* owning bInterfaceNumber, or USBIP_EP_NO_IFNUM */
    usbip_ep_out_fn on_out;             /* host->device data callback, or NULL to enqueue */
    void *on_out_ctx;
    usbip_ep_iso_fn on_iso;             /* iso producer/consumer by ep direction, or NULL:
                                         * IN completes with zero-length packets, OUT drops the data */
    void *on_iso_ctx;
    pthread_mutex_t lock;
    pthread_cond_t cond;
    struct dbuf *in_head, *in_tail;     /* data device->host, waiting for an IN URB */
    struct dbuf *out_head, *out_tail;   /* data host->device, waiting for usbip_device_read */
    struct pending *pend_head, *pend_tail;  /* IN URBs parked until data exists */
    int halted;                         /* STALL every URB until CLEAR_FEATURE(ENDPOINT_HALT) */
    uint64_t iso_deadline_us;           /* iso pacing: monotonic time the next frame is due */
};

/* One Microsoft-OS-descriptor advertisement; a device may carry several. Each
 * function on a composite binds its own driver, so the claim has to be
 * "interfaces 2..3 are WinUSB", never "this device is". */
struct msos_entry {
    int group;                  /* the descriptor group it describes; -1 = the whole device */
    char *compatible;           /* Compatible ID, e.g. "WINUSB" or "MTP" */
    char *guid;                 /* DeviceInterfaceGUID(s) to advertise, or NULL for none */
};

/* per-ifnum control / SET_INTERFACE registration (see usbip_device_on_control) */
struct ctrl_slot {
    usbip_device_control_fn cb;
    void *ctx;
};
struct set_alt_slot {
    usbip_device_set_alt_fn cb;
    void *ctx;
};

/* One (bInterfaceNumber, bAlternateSetting) descriptor in the config blob. The offset
 * is kept so bNumEndpoints (off+4) auto-counts and iInterface (off+8) can be patched. */
struct ifalt {
    uint8_t ifnum, alt;
    int off;                        /* blob offset of the 9-byte interface descriptor */
};

/* A descriptor group: the class-free remnant of "one function" the core keeps, for
 * the MS-OS fan-out and the plug-time IAD diagnostic. Blob bookkeeping only. */
struct descr_group {
    uint8_t first_ifnum;            /* bInterfaceNumber of the group's first interface */
    int n_ifs;                      /* distinct bInterfaceNumbers registered by the group */
    int descr_off;                  /* where the group's bytes start in the config blob */
};

struct usbip_device {
    uint16_t vid, pid, bcdDevice, bcdUSB;
    usb_speed speed;                         /* reported in OP_REP_DEVLIST/IMPORT (default FULL) */
    uint8_t dev_class, dev_sub, dev_proto;   /* device descriptor class (0 = per-interface) */
    char *strings[16];              /* [0] unused; 1=mfr 2=product 3=serial; 4+ = usbip_device_add_string */
    int n_strings;                  /* number of allocated string slots (>=4) */
    uint8_t i_mfr, i_prod, i_ser;
    /* Microsoft OS descriptors (see usbip_device_enable_msos / usbip_device_enable_msos_group) */
    struct msos_entry msos[8];      /* one per advertised group (or one device-wide) */
    int n_msos;                     /* 0 = no MS OS descriptors at all */
    uint8_t msos1_vendor, msos2_vendor;  /* vendor request codes for the two mechanisms */
    /* WebUSB (see usbip_device_enable_webusb): a second BOS platform capability + GET_URL */
    int webusb;                     /* emit a WebUSB BOS platform capability */
    uint8_t webusb_vendor;          /* bVendorCode for the WebUSB GET_URL request */
    char *webusb_url;               /* landing-page URL (full, scheme included) */
    int iso_paced;                  /* delay iso completions to real time (see usbip_device_set_iso_pacing) */
    int composite;                  /* see usbip_device_set_composite: multi-interface classes
                                     * emit an IAD so Windows' usbccgp groups them correctly */
    uint8_t descr[2048];            /* THE config blob: everything after the 9-byte config header */
    int descr_len;
    struct ifalt ifalts[64];        /* (ifnum,alt) -> interface-descriptor offset, in append order */
    int n_ifalts;
    int cur_ifnum, cur_alt;         /* append cursor: the last interface descriptor seen (-1 = none) */
    uint8_t if_present[16];         /* bInterfaceNumbers registered so far */
    int n_ifnums;                   /* highest bInterfaceNumber+1 seen -> bNumInterfaces on the wire */
    int config;
    struct usbip_ep *eps[128];      /* every endpoint object, in add order */
    int n_eps;
    struct usbip_ep *ep_map[16][2];   /* [number][dir] */
    /* Endpoint numbers claimed before their descriptor exists, for a class descriptor
     * that must name an address declared later -- UVC's VS input header. Value is the
     * owning bInterfaceNumber + 1 (0 = free); cleared when the endpoint is added. */
    uint8_t ep_reserved[16][2];
    struct ctrl_slot ctrl[16];      /* per-ifnum class/vendor control handlers */
    struct ctrl_slot ctrl_default;  /* device-level fallback (usbip_device_on_control ifnum -1) */
    struct set_alt_slot set_alt_tab[16];  /* per-ifnum SET_INTERFACE handlers */
    struct descr_group groups[8];
    int n_groups;
    /* usbip_device_group_begin snapshot, for the pre-plug usbip_device_group_abort rollback */
    struct {
        int descr_len, n_ifalts, n_eps, n_msos, n_ifnums, cur_ifnum, cur_alt;
        uint8_t if_present[16];
        uint8_t ep_reserved[16][2];
        struct ctrl_slot ctrl[16], ctrl_default;
        struct set_alt_slot set_alt_tab[16];
    } gmark;
    /* Serving. One listener can carry several devices, the importer picking by busid
     * as usbipd exports a bus. Listeners are shared by address, so a second plug on
     * one host:port joins the first's socket instead of binding. */
    struct usbip_server *server;    /* the listener serving this device (NULL = unplugged) */
    char busid[USBIP_DEV_BUSID_LEN];/* wire busid, e.g. "1-1"; auto-assigned at plug time */
    uint32_t busnum, devnum;        /* wire address; devnum is unique per listener */
    char host[64];
    int port;
};

struct paced_ret;                   /* iso completion queued for real-time delivery (usbip_device.c) */

struct conn {                       /* one attached host connection */
    int fd;
    usbip_device *dev;
    pthread_mutex_t wlock;          /* serialize socket writes */
    /* isochronous pacing (see usbip_device_set_iso_pacing): a per-connection thread
     * sends completions at each endpoint's deadline, so the serve thread never blocks. */
    pthread_t pacer_thread;
    int pace_started, pace_stop;
    struct paced_ret *pace_head;
    pthread_mutex_t pace_lock;
    pthread_cond_t pace_cond;
};

/* ---- usbip.c (wire I/O + serve loop) ---- */
struct iso_pkt {                    /* one isochronous packet descriptor (wire: 4×BE u32) */
    uint32_t offset, length, actual_length, status;
};
struct urb {
    uint32_t command, seqnum, devid, direction, ep;
    uint32_t flags, unlink_seqnum;
    int32_t length, interval;
    int32_t number_of_packets;      /* >0 only for isochronous transfers */
    struct iso_pkt *iso;            /* [number_of_packets], malloc'd, or NULL */
    uint8_t setup[8];
    uint8_t *data;                  /* OUT payload (malloc'd) */
    int data_len;
};
int  usbip_read_cmd(int fd, struct urb *urb, usbip_device *dev);
int  usbip_send_ret(struct conn *conn, uint32_t seqnum, uint32_t devid,
                    uint32_t direction, uint32_t ep, int32_t status,
                    const void *data, int actual);
/* isochronous RET: header(actual=Σpkt) + de-padded data + the packet descriptors.
 * urb->iso[i].actual_length/status must be filled; offset/length are echoed. */
int  usbip_send_ret_iso(struct conn *conn, struct urb *urb, const void *data);
int  usbip_send_ret_unlink(struct conn *conn, uint32_t seqnum, int32_t status);
int  usbip_serve_start(usbip_device *dev);  /* bind/listen/accept thread */
void usbip_serve_stop(usbip_device *dev);
void usbip_transport_params(usb_transport *transport, const char **host, int *port);

/* ---- usbip.c (client side, used by usbip_host.c) ---- */
struct usbip_devinfo {
    char     busid[32];
    uint32_t busnum, devnum, speed;
    uint16_t vid, pid, bcdDevice;
    uint8_t  dclass, dsub, dproto, n_cfg, n_ifaces;
    uint8_t  iclass[16], isub[16], iproto[16];
};
int usbip_connect(usb_transport *transport);                                    /* -> fd or -1 */
int usbip_client_import(int fd, const char *busid, struct usbip_devinfo *out);
int usbip_client_devlist(int fd, struct usbip_devinfo *list, int max);  /* -> count */
int usbip_client_submit(int fd, uint32_t devid, uint32_t *seqctr,
                        int dir, int ep, const uint8_t setup[8], int interval,
                        uint8_t *buf, int len, int *actual);            /* 0 ok, else USB_ERROR_* */
/* isochronous submit: pkts[i].offset/length are inputs (offset = slot in buf);
 * on return actual_length/status are filled and, for IN, buf[offset..] holds each
 * packet's data. *total_actual gets the summed bytes. */
int usbip_client_submit_iso(int fd, uint32_t devid, uint32_t *seqctr,
                            int dir, int ep, int interval, uint8_t *buf,
                            struct iso_pkt *pkts, int npkts, int *total_actual);

/* ---- pcap.c (optional USB-traffic capture; no-op unless USBIP_PCAPNG set) ---- */
int  usbip_pcap_enabled(void);          /* cheap gate so callers skip building args */
/* one usbmon event: event 'S'(submit)/'C'(complete); xfer_type in usbmon numbering
 * (iso0 intr1 ctrl2 bulk3); ep_addr includes the direction bit; setup is the 8-byte
 * control SETUP or NULL; data is the OUT (submit) / IN (complete) payload or NULL. */
void usbip_pcap_packet(uint32_t devid, uint32_t seqnum, int xfer_type, char event,
                       int ep_addr, int32_t status, const uint8_t *setup,
                       const uint8_t *data, int data_len, int urb_len);

/* ---- usbip_device.c (URB dispatch + standard requests) ---- */
void usbip_device_dispatch(struct conn *conn, struct urb *urb);
void usbip_device_handle_unlink(struct conn *conn, struct urb *urb);
void usbip_device_pacer_stop(struct conn *conn);   /* drain + stop the iso pacer when a conn closes */
int  pack_usbip_device(usbip_device *dev, uint8_t out[USBIP_DEV_LEN]);

/* ---- retired public API (kept linkable; no longer in the shipped headers,
 * and hidden from the packaged libraries' exports) ---- */
void usbip_ep_clear_halt(usbip_ep *ep);
int  usbip_ep_is_halted(usbip_ep *ep);

/* dead feature: registered drivers are never read back, kept only so the
 * definition in usbip_host.c retains a prototype */
typedef struct {
    const char *name;
    int  bInterfaceClass;
    int  (*probe)(usbip_host_handle *handle, int iface, void **drvdata);
    void (*disconnect)(usbip_host_handle *handle, void *drvdata);
} usbip_host_driver;
void usbip_host_register_driver(const usbip_host_driver *drv);

#endif /* USBIP_DEVICE_INTERNAL_H */
