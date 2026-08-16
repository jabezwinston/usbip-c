/**
 * Copyright (C) 2026 Jabez Winston
 *
 * SPDX-License-Identifier: MIT
 *
 * Date : 05-June-2026
 *
 * hid.c - USB HID (Human Interface Device) device class, HID 1.11.
 *
 * Built ONLY on the public API (usbip_device.h / classes/hid.h). Mirrors the Python
 * classes/device/hid.py. A generic HID interface: descriptors come from the
 * caller-supplied Report descriptor; EP0 answers the full HID request set
 * (GET/SET_REPORT, GET/SET_IDLE, GET/SET_PROTOCOL); the required interrupt IN
 * carries Input reports and an optional interrupt OUT carries Output reports.
 */
#include <string.h>

#include "classes/hid.h"

/* class-specific requests (HID 1.11 Sec.7.2) */
#define HID_GET_REPORT    0x01   /* host pulls a report over ep0 instead of the IN pipe */
#define HID_GET_IDLE      0x02   /* current idle rate */
#define HID_GET_PROTOCOL  0x03   /* boot or report protocol */
#define HID_SET_REPORT    0x09   /* host pushes a report (e.g. keyboard LEDs) */
#define HID_SET_IDLE      0x0A   /* how often to resend an unchanged report */
#define HID_SET_PROTOCOL  0x0B   /* BIOS switches a keyboard to boot protocol */

/* class descriptor types (HID 1.11 Sec.7.1.1) */
#define HID_DT_HID        0x21   /* the HID descriptor itself, inside the interface */
#define HID_DT_REPORT     0x22   /* the report descriptor, fetched separately */

#define HID_BCD_VERSION        0x0111   /* HID 1.11, as bcdHID */
#define HID_LAST_INPUT_MAX     64       /* biggest Input report we echo to GET_REPORT */

struct hid_state {
    hid_iface pub;                             /* MUST be first: hid_iface* IS this */
    hid_opts opts;
    usbip_ep *in, *out;                        /* the pipes actually assigned    */
    uint8_t  last_input[HID_LAST_INPUT_MAX];   /* for GET_REPORT(Input) echo     */
    int      last_input_len;
};

/* The public hid_iface is the first member of the private state, so converting
 * between them is free in both directions. */
static inline struct hid_state *state_of(hid_iface *iface) { 
    return (struct hid_state *)iface; 
}

static inline hid_iface *iface_of(usbip_function *func)
{
    return &((struct hid_state *)usbip_function_state(func))->pub;
}

static usb_hid_descriptor hid_desc(const struct hid_state *st) {
    return (usb_hid_descriptor){
        .bLength = sizeof(usb_hid_descriptor),
        .bDescriptorType = HID_DT_HID,
        .bcdHID = HID_BCD_VERSION,
        .bCountryCode = st->opts.country,
        .bNumDescriptors = 1,
        .bReportType = HID_DT_REPORT,
        .wReportLength = st->opts.report_desc_len 
    };
}

/* bInterval is speed-dependent: FS/LS counts 1 ms frames, HS counts 2^(bInterval-1)
 * microframes of 125 us.
 * opts.*_interval is milliseconds of intent, so passing an FS value through at HS
 * turns a "10 ms" endpoint into a 64 ms one. */
static uint8_t hid_encode_interval(usbip_function *func, uint8_t ms) {
    if (usbip_device_get_speed(usbip_function_device(func)) < USB_SPEED_HIGH || ms == 0)
        return ms;

    uint32_t target_us = (uint32_t)ms * 1000u;
    uint8_t best = 1;
    uint32_t best_err = UINT32_MAX;

    for (uint8_t candidate = 1; candidate <= 16; candidate++) {
        uint32_t us = 125u << (candidate - 1);
        uint32_t err = us > target_us ? us - target_us : target_us - us;
        if (err < best_err) {
            best_err = err;
            best = candidate;
        }
    }
    return best;
}

/* ---- usbip_device_class vtable ---- */
static int hid_build(usbip_function *func, const void *params) {
    const hid_opts *opts = params;
    struct hid_state *st = usbip_function_state(func);
    memset(st, 0, sizeof(*st));
    st->opts = *opts;

    if (!st->opts.in_ep)
        st->opts.in_ep = 0x81;

    if (!st->opts.in_mps)
        st->opts.in_mps = 64;

    if (!st->opts.in_interval)
        st->opts.in_interval = 10;

    if (st->opts.out_ep && !st->opts.out_mps)
        st->opts.out_mps = 64;

    if (st->opts.out_ep && !st->opts.out_interval)
        st->opts.out_interval = 10;

    st->pub.protocol = HID_REPORT_PROTOCOL;        /* what the host has after enumeration */

    usbip_device *dev = usbip_function_device(func);
    int istr = st->opts.name ? usbip_device_add_string(dev, st->opts.name) : 0;

    st->pub.index = usbip_function_instance(func);
    st->pub.name  = st->opts.name ? st->opts.name : "";
    st->pub.user  = st->opts.user;
    st->pub.dev   = usbip_function_device(func);
    st->pub.func  = func;
    //! [build]
    usbip_interface *iface = usbip_function_add_interface(func, USB_CLASS_HID, st->opts.subclass, st->opts.protocol);
    st->pub.interface_number = usbip_interface_number(iface);
    usbip_interface_set_string(iface, (uint8_t)istr);
    usb_hid_descriptor hd = hid_desc(st);      /* interleaved between iface and eps */
    usbip_interface_add_descriptor(iface, &hd);

    st->in = usbip_interface_add_endpoint(iface, &(usb_endpoint_descriptor){
        .bDescriptorType = USB_DT_ENDPOINT,
        .bEndpointAddress = st->opts.in_ep,
        .bmAttributes = USB_INTR,
        .wMaxPacketSize = st->opts.in_mps,
        .bInterval = hid_encode_interval(func, st->opts.in_interval)
    });
    //! [build]

    if (st->opts.out_ep)
        st->out = usbip_interface_add_endpoint(iface, &(usb_endpoint_descriptor){
            .bDescriptorType = USB_DT_ENDPOINT,
            .bEndpointAddress = st->opts.out_ep,
            .bmAttributes = USB_INTR,
            .wMaxPacketSize = st->opts.out_mps,
            .bInterval = hid_encode_interval(func, st->opts.out_interval) 
        });
    return 0;
}

static int hid_control(usbip_function *iface, const usb_setup *setup, uint8_t *buf, uint16_t len) {
    struct hid_state *st = usbip_function_state(iface);
    uint8_t type = USB_REQ_TYPE(setup->bmRequestType);

    /* standard GET_DESCRIPTOR for the Report / HID class descriptors */
    if (type == USB_STANDARD && setup->bRequest == USB_REQ_GET_DESCRIPTOR) {
        uint8_t dt = USB_U16_MSB(setup->wValue);
        if (dt == HID_DT_REPORT) {
            int count = st->opts.report_desc_len < len ? st->opts.report_desc_len : len;
            memcpy(buf, st->opts.report_desc, (size_t)count);
            return count;
        }
        if (dt == HID_DT_HID) {
            usb_hid_descriptor hd = hid_desc(st);
            int count = (int)sizeof(hd) < len ? (int)sizeof(hd) : len;
            memcpy(buf, &hd, (size_t)count);
            return count;
        }
        return -1;                             /* e.dev. Physical: none */
    }
    if (type != USB_CLASS)
        return -1;

    uint8_t rtype = USB_U16_MSB(setup->wValue);
    uint8_t rid = USB_U16_LSB(setup->wValue);
    switch (setup->bRequest) {
    case HID_GET_REPORT: {
        if (st->opts.get_report) {
            int got = st->opts.get_report(iface_of(iface), st->opts.user, rtype, rid, buf, len);
            if (got >= 0)
                return got;
        }
        int count = st->last_input_len < len ? st->last_input_len : len;    /* echo last Input */
        memcpy(buf, st->last_input, (size_t)count);
        return count;
    }

    case HID_SET_REPORT:
        if (st->opts.set_report)
            st->opts.set_report(iface_of(iface), st->opts.user, rtype, rid, buf, len);
        else if (st->opts.on_output)
            st->opts.on_output(iface_of(iface), st->opts.user, buf, len);
        return 0;

    case HID_GET_IDLE:
        buf[0] = st->pub.idle;
        return 1;

    case HID_SET_IDLE:
        st->pub.idle = USB_U16_MSB(setup->wValue);
        return 0;

    case HID_GET_PROTOCOL:
        buf[0] = st->pub.protocol;
        return 1;

    case HID_SET_PROTOCOL:
        st->pub.protocol = USB_U16_LSB(setup->wValue);
        return 0;
    }

    return -1;                                 /* STALL */
}

static void hid_on_out(usbip_function *iface, usbip_ep *ep, const void *data, int len) {
    (void)ep;
    struct hid_state *st = usbip_function_state(iface);

    if (st->opts.on_output) 
        st->opts.on_output(iface_of(iface), st->opts.user, data, len);
}

const usbip_device_class usbip_device_hid = {
    .name = "hid",
    .bInterfaceClass = USB_CLASS_HID,
    .build   = hid_build,
    .control = hid_control,
    .on_out  = hid_on_out,
    .state_size = sizeof(struct hid_state),
};

hid_iface *hid_add(usbip_device *dev, const hid_opts *opts) {
    usbip_function *func = usbip_device_add_class(dev, &usbip_device_hid, opts);
    return func ? iface_of(func) : NULL;
}

int hid_send_report(hid_iface *iface, const void *data, int len) {
    struct hid_state *st = state_of(iface);
    if (!st || !st->in) 
        return USB_ERROR_NO_DEVICE;

    int kept = len < HID_LAST_INPUT_MAX ? len : HID_LAST_INPUT_MAX;

    if (kept > 0) {
        memcpy(st->last_input, data, (size_t)kept);
        st->last_input_len = kept;
    }
    return usbip_device_write(st->in, data, len, 0);
}
