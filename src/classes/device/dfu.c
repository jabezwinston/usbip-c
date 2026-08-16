/**
 * Copyright (C) 2026 Jabez Winston
 *
 * SPDX-License-Identifier: MIT
 *
 * Date : 04-June-2026
 *
 * dfu.c - USB DFU (Device Firmware Upgrade) device class, DFU 1.1.
 *
 * Built ONLY on the public API (usbip_device.h / classes/dfu.h). Mirrors the Python
 * classes/device/dfu.py. All traffic is EP0 control: DNLOAD/UPLOAD move the
 * image in wTransferSize chunks; GETSTATUS drives the little state machine.
 */
#include <stdarg.h>
#include <stdio.h>

#include "classes/dfu.h"

/* dfu_iface is this class's public handle: the function, retyped so the compiler
 * can tell it from another class's handle. */
static inline dfu_iface *handle_of(usbip_function *func)
{
    return (dfu_iface *)func;
}
/* class-specific requests (DFU 1.1 Table 3.2) */
#define DFU_DETACH     0   /* run-time: please re-enumerate in DFU mode */
#define DFU_DNLOAD     1   /* host -> device firmware block */
#define DFU_UPLOAD     2   /* device -> host firmware block */
#define DFU_GETSTATUS  3   /* status + state + poll timeout */
#define DFU_CLRSTATUS  4   /* leave dfuERROR */
#define DFU_GETSTATE   5   /* state alone, without clearing anything */
#define DFU_ABORT      6   /* give up the current transfer, back to dfuIDLE */

/* states (DFU 1.1 Table 4.2 - the subset we use) */
#define dfuIDLE          2   /* ready for a new transfer */
#define dfuDNLOAD_SYNC   3   /* block taken; the next GETSTATUS reports it */
#define dfuDNLOAD_IDLE   5   /* mid-download, ready for the next block */
#define dfuMANIFEST_SYNC 6   /* download finished; the image is being committed */
#define dfuUPLOAD_IDLE   9   /* mid-upload, more to send */
#define dfuERROR        10   /* stuck until the host sends CLRSTATUS */

/* bStatus codes (dfu_status, DFU 1.1 Sec.6.1.2) are public - see classes/dfu.h. */

#define DFU_FUNCTIONAL_DESC 0x21

/* Interface subclass/protocol (DFU 1.1 Sec.4.2). The base class is ::USB_CLASS_APP_SPEC. */
#define DFU_SUBCLASS            0x01   /* Device Firmware Upgrade */
#define DFU_PROTOCOL_DFU_MODE   0x02   /* already in DFU mode (not run-time) */
#define MAX_TARGETS 16

typedef struct USB_PACKED
{
    uint8_t  bLength, bDescriptorType, bmAttributes;
    uint16_t wDetachTimeOut, wTransferSize, bcdDFUVersion;
} dfu_functional_descriptor;

struct dfu_state
{
    uint16_t transfer_size;
    uint8_t  attributes;
    int      n_targets;
    dfu_target tgt[MAX_TARGETS];  /* copies of the app's target descriptors (callbacks + ctx) */
    int      cur;
    uint8_t  state, status;
    void   (*on_event)(void *user, const char *text);
    void    *user;
};

static void dfu_log(struct dfu_state *st, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    usbip_class_vlog(st->on_event, st->user, NULL, fmt, ap);
    va_end(ap);
}

/* ---- usbip_device_class vtable ---- */
static int dfu_build(usbip_function *func, const void *params)
{
    const dfu_opts *opts = params;
    struct dfu_state *st = usbip_function_state(func);

    st->transfer_size = opts->transfer_size ? opts->transfer_size : 1024;

    if (st->transfer_size > 4096)
        st->transfer_size = 4096; /* core control buffer cap */

    st->attributes = opts->attributes ? opts->attributes : 0x07;
    st->on_event = opts->on_event;
    st->user = opts->user;
    st->state = dfuIDLE;
    st->status = DFU_STATUS_OK;
    st->cur = 0;
    st->n_targets = opts->n_targets > MAX_TARGETS ? MAX_TARGETS : opts->n_targets;

    usbip_device *dev = usbip_function_device(func);
    /* Scoped to THIS function: a device-wide WINUSB Compatible ID would bind WinUSB
     * over everything, and the neighbour's driver would never load.
     * On a lone DFU device the emitted bytes are identical either way. */
    if (opts->winusb)
        usbip_function_enable_winusb(func, NULL);

    /* one interface (class 0xFE/0x01/0x02); each target is an alternate setting */
    usbip_interface *dfu_if = NULL;
    for (int i = 0; i < st->n_targets; i++)
    {
        st->tgt[i] = opts->targets[i]; /* copy the app's descriptor (callbacks + ctx) */
        const char *name = st->tgt[i].name ? st->tgt[i].name : "target";
        int istr = usbip_device_add_string(dev, name);

        if (i == 0)
            dfu_if = usbip_function_add_interface(func, USB_CLASS_APP_SPEC, DFU_SUBCLASS, DFU_PROTOCOL_DFU_MODE);
        else
            usbip_interface_add_altsetting(dfu_if, (uint8_t)i);

        usbip_interface_set_string(dfu_if, (uint8_t)istr);
    }
    /* one DFU functional descriptor after all the alternate-setting interfaces */
    dfu_functional_descriptor fd = {
        .bLength = 9,
        .bDescriptorType = DFU_FUNCTIONAL_DESC,
        .bmAttributes = st->attributes,
        .wDetachTimeOut = 1000,
        .wTransferSize = st->transfer_size,
        .bcdDFUVersion = 0x0110
    };

    if (dfu_if)
        usbip_interface_add_descriptor(dfu_if, &fd);
    else
        usbip_function_add_descriptor(func, &fd);

    return 0;
}

static int dfu_control(usbip_function *iface, const usb_setup *setup, uint8_t *buf, uint16_t len)
{
    if (USB_REQ_TYPE(setup->bmRequestType) != USB_CLASS)
        return -1; /* DFU uses class requests only */

    struct dfu_state *st = usbip_function_state(iface);
    dfu_target *target = &st->tgt[st->cur];

    switch (setup->bRequest)
    {
    case DFU_DNLOAD:
        if (len == 0)
        { 
            /* zero-length block = end of download */
            if (target->finish)
                target->finish(target->ctx);

            st->state = dfuMANIFEST_SYNC;
            st->status = DFU_STATUS_OK;

            uint32_t written = target->size ? target->size(target->ctx) : 0;

            dfu_log(st, "DOWNLOAD done %s (%uB)", target->name, written);

            return 0;
        }

        if (setup->wValue == 0 && target->begin)
            target->begin(target->ctx); /* first block = fresh image */

        int rc = target->write(target->ctx, (uint32_t)setup->wValue * st->transfer_size, buf, (uint32_t)len);
        if (rc != DFU_STATUS_OK)
        { 
            /* backend reported a DFU status */
            int is_dfu_status = (rc > DFU_STATUS_OK && rc <= DFU_STATUS_errSTALLEDPKT);

            st->state = dfuERROR;
            st->status = is_dfu_status ? (uint8_t)rc : DFU_STATUS_errWRITE;
            dfu_log(st, "DNLOAD error: status 0x%02x", st->status);
            return 0; /* ACK the block; the host learns the error from GETSTATUS (dfuERROR) */
        }
        st->state = dfuDNLOAD_SYNC;
        st->status = DFU_STATUS_OK;
        dfu_log(st, "DNLOAD (%dB)", len);
        return 0;

    case DFU_UPLOAD:
    {
        uint32_t want = st->transfer_size;
        if (want > len)
            want = len;
        int got = target->read(target->ctx, (uint32_t)setup->wValue * st->transfer_size, buf, want);
        if (got)
        {
            st->state = dfuUPLOAD_IDLE;
            dfu_log(st, "UPLOAD (%dB)", got);
        }
        else
        {
            st->state = dfuIDLE;
            dfu_log(st, "UPLOAD done %s", target->name);
        }
        st->status = DFU_STATUS_OK;
        return got;
    }

    case DFU_GETSTATUS:
        if (st->state == dfuDNLOAD_SYNC)
            st->state = dfuDNLOAD_IDLE;
        else if (st->state == dfuMANIFEST_SYNC)
            st->state = dfuIDLE; /* manifest-tolerant */
        buf[0] = st->status;
        buf[1] = 0;
        buf[2] = 0;
        buf[3] = 0; /* bwPollTimeout = 0 */
        buf[4] = st->state;
        buf[5] = 0;
        return 6;

    case DFU_GETSTATE:
        buf[0] = st->state;
        return 1;

    case DFU_CLRSTATUS:
    case DFU_ABORT:
        st->state = dfuIDLE;
        st->status = DFU_STATUS_OK;
        return 0;

    case DFU_DETACH:
        return 0; /* no-op in DFU mode */
    }
    return -1; /* STALL */
}

static int dfu_set_alt(usbip_function *iface, int ifnum, int alt)
{
    (void)ifnum; /* DFU has a single interface */
    struct dfu_state *st = usbip_function_state(iface);
    if (alt >= 0 && alt < st->n_targets)
    {
        st->cur = alt;
        st->state = dfuIDLE;
        st->status = DFU_STATUS_OK;
        dfu_log(st, "SELECT %s (alt %d)", st->tgt[alt].name, alt);
    }
    return 0;
}

const usbip_device_class usbip_device_dfu = {
    .name = "dfu",
    .bInterfaceClass = USB_CLASS_APP_SPEC,
    .build = dfu_build,
    .control = dfu_control,
    .set_alt = dfu_set_alt,
    .state_size = sizeof(struct dfu_state),
};

dfu_iface *dfu_add(usbip_device *dev, const dfu_opts *opts)
{
    usbip_function *func = usbip_device_add_class(dev, &usbip_device_dfu, opts);

    return handle_of(func);
}
