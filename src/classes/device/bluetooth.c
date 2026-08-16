/**
 * Copyright (C) 2026 Jabez Winston
 *
 * SPDX-License-Identifier: MIT
 *
 * Date : 05-June-2026
 *
 * bluetooth.c - USB Bluetooth (class 0xE0) DEVICE class: a minimal HCI transport.
 *
 * Built only on the public API (usbip_device.h / classes/bluetooth.h): no core internals.
 * It moves HCI commands/events and ACL data between the USB host and an
 * app-supplied controller; it has no HCI knowledge of its own. Mirrors the Python
 * library's usbip/classes/device/bluetooth.py.
 */
#include <string.h>

#include "classes/bluetooth.h"

/* bt_hci is this class's public handle: the function, retyped so the compiler
 * can tell it from another class's handle. */
static inline usbip_function *fn(bt_hci *hci)
{
    return (usbip_function *)hci;
}

static inline bt_hci *handle_of(usbip_function *func)
{
    return (bt_hci *)func;
}

/* Bluetooth Programming Interface (USB Bluetooth 1.2 Sec.2.1): the base class is
 * ::USB_CLASS_WIRELESS, subclass RF Controller, protocol Bluetooth. */
#define BT_CLASS USB_CLASS_WIRELESS /* Wireless Controller */
#define BT_SUBCLASS 0x01            /* RF Controller */
#define BT_PROTOCOL 0x01            /* Bluetooth Programming Interface */

/* Preferred endpoint addresses -- on a composite the allocator may relocate them. */
#define EP_EVENT   0x81   /* interrupt IN: HCI events */
#define EP_ACL_OUT 0x02   /* bulk OUT: ACL data, host -> controller */
#define EP_ACL_IN  0x82   /* bulk IN:  ACL data, controller -> host */
#define EP_SCO_OUT 0x03   /* isochronous OUT: SCO audio */
#define EP_SCO_IN  0x83   /* isochronous IN:  SCO audio */

#define BT_ACL_REASM_MAX 1024

struct bt_state
{
    bt_opts opts;
    uint8_t acl_rx[BT_ACL_REASM_MAX]; /* ACL OUT reassembly (across 64-byte URBs) */
    int acl_have;
    /* The IN pipes this controller was assigned. A composite relocates them off EP_*,
     * so keep the objects: a stale lookup silently drops every event and ACL. */
    usbip_ep *event, *acl_in;
};

/* SCO isochronous alt-setting packet sizes (alt 0 = zero-bandwidth placeholder) */
static const uint16_t sco_alt_sizes[] = {0, 9, 17, 25, 33, 49};

/* ---- usbip_device_class vtable ---- */
static int bt_build(usbip_function *func, const void *params)
{
    struct bt_state *st = usbip_function_state(func);
    usbip_device *dev = usbip_function_device(func);

    st->opts = params ? *(const bt_opts *)params : (bt_opts){0};
    st->acl_have = 0;

    usbip_device_set_class(dev, BT_CLASS, BT_SUBCLASS, BT_PROTOCOL);

    /* Interface 0: interrupt-IN (events) + bulk-OUT (ACL out) + bulk-IN (ACL in) */
    usbip_interface *hci = usbip_function_add_interface(func, BT_CLASS, BT_SUBCLASS, BT_PROTOCOL);
    st->event = usbip_interface_add_endpoint(hci, &(usb_endpoint_descriptor){
        .bDescriptorType = USB_DT_ENDPOINT,
        .bEndpointAddress = EP_EVENT,
        .bmAttributes = USB_INTR,
        .wMaxPacketSize = 16,
        .bInterval = 1
    });
    usbip_interface_add_endpoint(hci, &(usb_endpoint_descriptor){
        .bDescriptorType = USB_DT_ENDPOINT,
        .bEndpointAddress = EP_ACL_OUT,
        .bmAttributes = USB_BULK,
        .wMaxPacketSize = 64
    });
    st->acl_in = usbip_interface_add_endpoint(hci, &(usb_endpoint_descriptor){
        .bDescriptorType = USB_DT_ENDPOINT,
        .bEndpointAddress = EP_ACL_IN,
        .bmAttributes = USB_BULK,
        .wMaxPacketSize = 64
    });

    /* Interface 1: SCO isochronous, 6 alt settings, for dongle fidelity only. The iso
     * endpoints never carry data; the host keeps zero-bandwidth alt 0 selected. */
    if (st->opts.with_sco)
    {
        usbip_interface *sco = NULL;
        for (uint8_t alt = 0; alt < (uint8_t)(sizeof(sco_alt_sizes) / sizeof(sco_alt_sizes[0])); alt++)
        {
            if (alt == 0)
                sco = usbip_function_add_interface(func, BT_CLASS, BT_SUBCLASS, BT_PROTOCOL);
            else
                usbip_interface_add_altsetting(sco, alt);

            usbip_interface_add_endpoint(sco, &(usb_endpoint_descriptor){
                .bDescriptorType = USB_DT_ENDPOINT,
                .bEndpointAddress = EP_SCO_OUT,
                .bmAttributes = USB_ISO,
                .wMaxPacketSize = sco_alt_sizes[alt],
                .bInterval = 1
            });
            usbip_interface_add_endpoint(sco, &(usb_endpoint_descriptor){
                .bDescriptorType = USB_DT_ENDPOINT,
                .bEndpointAddress = EP_SCO_IN,
                .bmAttributes = USB_ISO,
                .wMaxPacketSize = sco_alt_sizes[alt],
                .bInterval = 1
            });
        }
    }
    return 0;
}

static int bt_control(usbip_function *iface, const usb_setup *setup, uint8_t *buf, uint16_t len)
{
    struct bt_state *st = usbip_function_state(iface);
    /* HCI command = class control OUT (bmRequestType 0x20). */
    if (USB_REQ_TYPE(setup->bmRequestType) == USB_CLASS && 
        (setup->bmRequestType & USB_REQ_DIR_IN) == 0)
    {
        if (st->opts.on_command)
            st->opts.on_command(handle_of(iface), buf, len);
        return 0;
    }
    return -1; /* nothing else to answer -> STALL */
}

static void bt_on_out(usbip_function *iface, usbip_ep *ep, const void *data, int len)
{
    (void)ep;
    struct bt_state *st = usbip_function_state(iface);

    if (len <= 0)
        return;
    if (st->acl_have + len > BT_ACL_REASM_MAX)
        st->acl_have = 0; /* resync on overflow */

    memcpy(st->acl_rx + st->acl_have, data, (size_t)len);
    st->acl_have += len;
    /* An ACL PDU may span several bulk-OUT URBs; release it once the 4-byte
     * header plus its little-endian data length (offset 2) are in hand. */
    while (st->acl_have >= 4)
    {
        int total = 4 + usb_get_le16(st->acl_rx + 2);
        if (st->acl_have < total)
            break;
        if (st->opts.on_acl)
            st->opts.on_acl(handle_of(iface), st->acl_rx, total);
        memmove(st->acl_rx, st->acl_rx + total, (size_t)(st->acl_have - total));
        st->acl_have -= total;
    }
}

const usbip_device_class usbip_device_bluetooth = {
    .name = "bluetooth",
    .bInterfaceClass = BT_CLASS,
    .bInterfaceSubClass = BT_SUBCLASS,
    .bInterfaceProtocol = BT_PROTOCOL,
    .build   = bt_build,
    .control = bt_control,
    .on_out  = bt_on_out,
    .state_size = sizeof(struct bt_state),
};

/* ---- app-facing helpers ---- */
bt_hci *bluetooth_add(usbip_device *dev, const bt_opts *opts)
{
    usbip_function *func = usbip_device_add_class(dev, &usbip_device_bluetooth, opts);

    return handle_of(func);
}

int bt_send_event(bt_hci *iface, const void *evt, int len)
{
    struct bt_state *st = usbip_function_state(fn(iface));
    return usbip_device_write(st->event, evt, len, 0);
}

int bt_send_acl(bt_hci *iface, const void *pdu, int len)
{
    struct bt_state *st = usbip_function_state(fn(iface));
    return usbip_device_write(st->acl_in, pdu, len, 0);
}

void *bluetooth_user(bt_hci *iface)
{
    struct bt_state *st = usbip_function_state(fn(iface));

    return st->opts.user;
}
