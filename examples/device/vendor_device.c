/**
 * Copyright (C) 2026 Jabez Winston
 *
 * SPDX-License-Identifier: MIT
 *
 * Date : 05-June-2026
 *
 * vendor_device.c - a vendor-defined USB device with one Bulk IN and one Bulk OUT,
 * served over USB/IP. Self-contained: it uses only the public device API (usbip_device.h)
 * and builds the descriptors inline - there is NO class code behind it.
 *
 * Behaviour: a loopback. Whatever the host sends on Bulk OUT is echoed straight
 * back on Bulk IN. That makes it trivial to exercise from any USB stack.
 *
 *   ./vendor_device            # serve 1209:0004 on :3240
 *   ./vendor_device 4000       # ...or on a port of your choice
 *
 * Drive it with the libusb wrapper (no kernel needed):
 *   USBIP_HOST=127.0.0.1 USBIP_PORT=3240 LD_PRELOAD=build/libusb-1.0.so.0 your_app
 * or attach it to the local kernel:
 *   attach it with a USB/IP client
 *   (Linux: sudo modprobe vhci-hcd ; sudo usbip attach -r 127.0.0.1 -b 1-1)
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "usbip-device.h"

/* identity + wiring (a vendor device defines its own class 0xFF) */
#define VENDOR_ID   0x1209
#define PRODUCT_ID  0x0004
#define EP_BULK_OUT 0x01        /* host -> device */
#define EP_BULK_IN  0x81        /* device -> host */
#define MAX_PACKET  64

int main(int argc, char **argv)
{
    int port = (argc > 1) ? atoi(argv[1]) : 3240;       /* default USB/IP port */

    usbip_device *dev = usbip_device_create(VENDOR_ID, PRODUCT_ID);
    usbip_device_set_strings(dev, "USB over IP", "USBIP Vendor Bulk", "0004");

    /* One vendor interface (class 0xFF) with a bulk OUT and a bulk IN endpoint,
     * declared straight on the device core - no class layer. */
    //! [descriptors]
    usbip_device_add_descriptor(dev, &(usb_interface_descriptor){
        .bDescriptorType    = USB_DT_INTERFACE,
        .bInterfaceNumber   = 0,
        .bNumEndpoints      = 0,                /* auto-counted as endpoints are added */
        .bInterfaceClass    = 0xFF,
        .bInterfaceSubClass = 0x00,
        .bInterfaceProtocol = 0x00
    });
    usbip_ep *bulk_out = usbip_device_add_endpoint(dev, &(usb_endpoint_descriptor){
        .bDescriptorType  = USB_DT_ENDPOINT,
        .bEndpointAddress = EP_BULK_OUT,
        .bmAttributes     = USB_BULK,
        .wMaxPacketSize   = MAX_PACKET
    });
    usbip_ep *bulk_in = usbip_device_add_endpoint(dev, &(usb_endpoint_descriptor){
        .bDescriptorType  = USB_DT_ENDPOINT,
        .bEndpointAddress = EP_BULK_IN,
        .bmAttributes     = USB_BULK,
        .wMaxPacketSize   = MAX_PACKET
    });
    //! [descriptors]

    usb_transport *transport = usbip_transport(NULL, port);
    int rc = usbip_device_plug(dev, transport);

    if (rc != USB_SUCCESS) {
        fprintf(stderr, "usbip_device_plug failed (port %d in use?)\n", port);
        return 1;
    }
    fprintf(stderr, "[vendor] serving %04x:%04x on :%d (bulk OUT 0x%02x, bulk IN 0x%02x) - "
                    "echoing OUT back on IN\n", VENDOR_ID, PRODUCT_ID, port, EP_BULK_OUT, EP_BULK_IN);

    /* Loopback loop: each usbip_device_read() returns one host OUT transfer; echo it back. */
    //! [io]
    uint8_t buffer[8192];
    for (;;) {
        int n_received = usbip_device_read(bulk_out, buffer, sizeof(buffer), 0);  /* blocks until the host sends */

        if (n_received <= 0)
            continue;

        fprintf(stderr, "[vendor] RX %d bytes -> echo\n", n_received);
        usbip_device_write(bulk_in, buffer, n_received, 0);                      /* device -> host */
    }
    //! [io]
    return 0;
}
