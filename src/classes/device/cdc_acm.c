/**
 * Copyright (C) 2026 Jabez Winston
 *
 * SPDX-License-Identifier: MIT
 *
 * Date : 04-June-2026
 *
 * cdc_acm.c - CDC-ACM (virtual serial) DEVICE class.
 *
 * Built ONLY on the public API (usbip_device.h / classes/cdc_acm.h): no core internals.
 * The class speaks the CDC protocol and surfaces events to the app via
 * cdc_acm_opts; it has no opinion about logging or echo - that's the app's job
 * (see examples/device/cdc_acm_device.c).
 */
#include "classes/cdc_acm.h"

/* ---- CDC functional descriptors (typed, so build() reads like a spec table) ---- */

/* Functional descriptor subtypes (USB CDC 1.1 Table 25) */
#define CDC_SUBTYPE_HEADER    0x00   /* bcdCDC */
#define CDC_SUBTYPE_CALL_MGMT 0x01   /* where call management lives */
#define CDC_SUBTYPE_ACM       0x02   /* which ACM requests are supported */
#define CDC_SUBTYPE_UNION     0x06   /* binds the Comm interface to its Data interface */

typedef struct USB_PACKED {
    uint8_t bFunctionLength, bDescriptorType, bDescriptorSubtype;
    uint16_t bcdCDC;
} cdc_header_desc;

typedef struct USB_PACKED {
    uint8_t bFunctionLength, bDescriptorType, bDescriptorSubtype, 
            bmCapabilities, bDataInterface;
} cdc_call_mgmt_desc;

typedef struct USB_PACKED {
    uint8_t bFunctionLength, bDescriptorType, bDescriptorSubtype, bmCapabilities;
} cdc_acm_func_desc;

typedef struct USB_PACKED {
    uint8_t bFunctionLength, bDescriptorType, bDescriptorSubtype,
            bControlInterface, bSubordinateInterface0;
} cdc_union_desc;

/* Interface numbers within the function; the pair must stay adjacent for the Union. */

/* Preferred endpoint addresses -- on a composite the allocator may relocate them. */
#define CDC_EP_NOTIFY 0x82   /* interrupt IN: serial state notifications */
#define CDC_EP_IN     0x81   /* bulk IN:  device -> host */
#define CDC_EP_OUT    0x01   /* bulk OUT: host -> device */

/* Interface class codes (USB CDC 1.1 Sec.4.2-4.5). The Communications triple doubles as
 * the IAD's function triple, since the association describes the same ACM function. */
#define CDC_CLASS_COMM     USB_CLASS_CDC        /* Communications and CDC Control */
#define CDC_SUBCLASS_ACM   0x02                 /* Abstract Control Model */
#define CDC_PROTOCOL_AT    0x01                 /* AT commands (V.250) */
#define CDC_CLASS_DATA     USB_CLASS_CDC_DATA   /* the bulk data interface */
#define CDC_IAD_IFACE_COUNT 2    /* the association covers Communications + Data */

/* CDC class control requests (USB CDC 1.1 Table 45) */
#define CDC_SET_LINE_CODING        0x20   /* baud / parity / stop bits */
#define CDC_GET_LINE_CODING        0x21   /* read them back */
#define CDC_SET_CONTROL_LINE_STATE 0x22   /* DTR / RTS */
#define CDC_CTRL_DTR               0x01   /* wValue bit 0: the host opened the port */

struct cdc_state {
    cdc_port        pub;            /* MUST be first: a cdc_port* IS a cdc_state* */
    cdc_acm_opts    opts;
    /* The pipes this port was assigned. A second port is relocated off the default
     * addresses, so keep the objects rather than looking them up by CDC_EP_*. */
    usbip_ep       *notify, *in, *out;
};

/* The public cdc_port is the first member of the private state, so converting
 * between them is free in both directions. */
static inline struct cdc_state *state_of(cdc_port *port) {
    return (struct cdc_state *)port;
}
static inline cdc_port *port_of(usbip_function *func)
{
    return &((struct cdc_state *)usbip_function_state(func))->pub;
}

/* ---- line coding (7-byte wire format) <-> struct ---- */
static void coding_from_wire(const uint8_t *wire, cdc_line_coding *lc)
{
    lc->baud = usb_get_le32(wire);          /* dwDTERate */
    lc->stop_bits = wire[4];
    lc->parity    = wire[5];
    lc->data_bits = wire[6];
}

static void coding_to_wire(const cdc_line_coding *lc, uint8_t *wire)
{
    usb_put_le32(wire, lc->baud);           /* dwDTERate */
    wire[4] = lc->stop_bits;
    wire[5] = lc->parity;
    wire[6] = lc->data_bits;
}

/* ---- usbip_device_class vtable ---- */
static int cdc_build(usbip_function *func, const void *params)
{
    struct cdc_state *st = usbip_function_state(func);
    st->opts    = params ? *(const cdc_acm_opts *)params : (cdc_acm_opts){0};
    st->pub.line_coding = (cdc_line_coding){ 
        .baud = 9600,
        .data_bits = 8 
    };
    st->pub.is_open = 0;

    /* Communications interface: notification endpoint + CDC functional descriptors */
    usbip_device *dev = usbip_function_device(func);
    int istr = st->opts.name ? usbip_device_add_string(dev, st->opts.name) : 0;

    st->pub.index = usbip_function_instance(func);
    st->pub.name  = st->opts.name ? st->opts.name : "";
    st->pub.user  = st->opts.user;
    st->pub.dev   = dev;
    st->pub.func  = func;
    /* Declare the comm+data pair as ONE function, or usbccgp splits it per INTERFACE
     * and no COM port forms: Communications cannot start (Code 10), Data binds
     * nothing (Code 28). Linux pairs them from the Union descriptor either way.
     * How we say "one function" depends on whether we are alone on the device:
     *   composite -> an IAD grouping our two interfaces (the device triple is
     *                pinned to EF/02/01 and describes the device, not us);
     *   alone     -> the device triple itself, which also keeps bDeviceClass
     *                non-zero so usbccgp never loads in the first place. */
    if (usbip_device_is_composite(dev))
        usbip_function_associate(func, CDC_IAD_IFACE_COUNT, CDC_CLASS_COMM, CDC_SUBCLASS_ACM, CDC_PROTOCOL_AT, (uint8_t)istr);
    else
        usbip_device_set_class(dev, CDC_CLASS_COMM, CDC_SUBCLASS_ACM, CDC_PROTOCOL_AT);


    usbip_interface *comm = usbip_function_add_interface(func, CDC_CLASS_COMM, CDC_SUBCLASS_ACM, CDC_PROTOCOL_AT);
    st->pub.interface_number = usbip_interface_number(comm);
    usbip_interface_set_string(comm, (uint8_t)istr);   /* iInterface, so the host shows the name */
    uint8_t comm_if = (uint8_t)usbip_interface_number(comm);
    uint8_t data_if = comm_if + 1;

    usbip_interface_add_descriptor(comm, &(cdc_header_desc){
        .bFunctionLength = sizeof(cdc_header_desc),
        .bDescriptorType = USB_DT_CS_INTERFACE,
        .bDescriptorSubtype = CDC_SUBTYPE_HEADER,
        .bcdCDC = 0x0110 
    });
    usbip_interface_add_descriptor(comm, &(cdc_call_mgmt_desc){
        .bFunctionLength = sizeof(cdc_call_mgmt_desc),
        .bDescriptorType = USB_DT_CS_INTERFACE,
        .bDescriptorSubtype = CDC_SUBTYPE_CALL_MGMT,
        .bmCapabilities = 0x00,
        .bDataInterface = data_if 
    });
    usbip_interface_add_descriptor(comm, &(cdc_acm_func_desc){
        .bFunctionLength = sizeof(cdc_acm_func_desc),
        .bDescriptorType = USB_DT_CS_INTERFACE,
        .bDescriptorSubtype = CDC_SUBTYPE_ACM,
        .bmCapabilities = 0x02
    });  /* line coding */
    usbip_interface_add_descriptor(comm, &(cdc_union_desc){
        .bFunctionLength = sizeof(cdc_union_desc),
        .bDescriptorType = USB_DT_CS_INTERFACE,
        .bDescriptorSubtype = CDC_SUBTYPE_UNION,
        .bControlInterface = comm_if,
        .bSubordinateInterface0 = data_if
    });
    st->notify = usbip_interface_add_endpoint(comm, &(usb_endpoint_descriptor){
        .bDescriptorType = USB_DT_ENDPOINT,
        .bEndpointAddress = CDC_EP_NOTIFY,
        .bmAttributes = USB_INTR,
        .wMaxPacketSize = 16,
        .bInterval = 9
    });

    /* Data interface: bulk IN + bulk OUT. HS bulk must be 512 B (USB 2.0 Sec.5.8.3); FS is 64. */
    int speed = usbip_device_get_speed(dev);
    uint16_t bulk_mps = (speed >= USB_SPEED_HIGH) ? 512 : 64;

    usbip_interface *data = usbip_function_add_interface(func, CDC_CLASS_DATA, 0, 0);
    st->in = usbip_interface_add_endpoint(data, &(usb_endpoint_descriptor){
        .bDescriptorType = USB_DT_ENDPOINT,
        .bEndpointAddress = CDC_EP_IN,
        .bmAttributes = USB_BULK,
        .wMaxPacketSize = bulk_mps 
    });
    st->out = usbip_interface_add_endpoint(data, &(usb_endpoint_descriptor){
        .bDescriptorType = USB_DT_ENDPOINT,
        .bEndpointAddress = CDC_EP_OUT,
        .bmAttributes = USB_BULK,
        .wMaxPacketSize = bulk_mps
    });
    st->pub.in_ep  = usbip_endpoint_address(st->in);
    st->pub.out_ep = usbip_endpoint_address(st->out);
    return 0;
}

static int cdc_control(usbip_function *iface, const usb_setup *setup, uint8_t *buf, uint16_t len)
{
    struct cdc_state *st = usbip_function_state(iface);
    switch (setup->bRequest) {
    case CDC_SET_LINE_CODING:
        if (len >= 7) {
            coding_from_wire(buf, &st->pub.line_coding);
            if (st->opts.on_line_coding)
                st->opts.on_line_coding(port_of(iface), st->opts.user, &st->pub.line_coding);
        }
        return 0;

    case CDC_GET_LINE_CODING:
        coding_to_wire(&st->pub.line_coding, buf);
        return 7;

    case CDC_SET_CONTROL_LINE_STATE: {
        int dtr = (setup->wValue & CDC_CTRL_DTR) != 0;
        if (dtr && !st->pub.is_open) {                         /* rising edge -> opened */
            st->pub.is_open = 1;
            if (st->opts.on_open) st->opts.on_open(port_of(iface), st->opts.user, &st->pub.line_coding);
        } else if (!dtr && st->pub.is_open) {                  /* falling edge -> closed */
            st->pub.is_open = 0;
            if (st->opts.on_close) st->opts.on_close(port_of(iface), st->opts.user);
        }
        return 0;
    }
    default:
        return -1;                                      /* STALL unsupported requests */
    }
}

static void cdc_on_out(usbip_function *iface, usbip_ep *ep, const void *data, int len)
{
    (void)ep;
    struct cdc_state *st = usbip_function_state(iface);
    if (st->opts.on_rx) st->opts.on_rx(port_of(iface), st->opts.user, data, len);
}

const usbip_device_class usbip_device_cdc_acm = {
    .name = "cdc_acm",
    .bInterfaceClass = USB_CLASS_CDC,
    .build   = cdc_build,
    .control = cdc_control,
    .on_out  = cdc_on_out,
    .state_size = sizeof(struct cdc_state),
};

/* ---- app-facing helpers ---- */
cdc_port *cdc_acm_add(usbip_device *dev, const cdc_acm_opts *opts)
{
    usbip_function *func = usbip_device_add_class(dev, &usbip_device_cdc_acm, opts);
    return func ? port_of(func) : NULL;
}

int cdc_acm_send(cdc_port *port, const void *data, int len)
{
    struct cdc_state *st = state_of(port);
    if (!st || !st->in)
        return USB_ERROR_NO_DEVICE;
    return usbip_device_write(st->in, data, len, 0);
}
