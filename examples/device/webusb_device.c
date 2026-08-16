/**
 * Copyright (C) 2026 Jabez Winston
 *
 * SPDX-License-Identifier: MIT
 *
 * Date : 05-June-2026
 *
 * webusb_device.c - a WebUSB device over USB/IP. Self-contained: it uses only the
 * public device API (usbip_device.h) and builds the descriptors inline - there is NO class
 * code behind it. It is a vendor device (class 0xFF) with one Bulk IN and one Bulk
 * OUT that echoes whatever it receives back with the case of ASCII letters swapped,
 * dressed up so a WebUSB-capable browser can find and open it.
 *
 * To be a WebUSB device (per the WebUSB spec) it advertises, via the core:
 *   - a BOS descriptor carrying a WebUSB platform-capability descriptor, and
 *   - a vendor request (bVendorCode + wIndex=GET_URL) that returns its landing-page
 *     URL  -- usbip_device_enable_webusb() does both.
 * It also co-advertises WinUSB (MS OS 2.0) so the interface auto-binds WinUSB on
 * Windows, which Chrome requires there -- usbip_device_enable_winusb().
 *
 *   ./webusb_device                                   # serve 1209:0012 on :3240
 *   ./webusb_device 4000                              # ...or on a port of your choice
 *   ./webusb_device 3240 https://example.com/app      # ...with a custom landing page
 *   attach it with a USB/IP client
 *   (Linux: sudo modprobe vhci-hcd ; sudo usbip attach -r 127.0.0.1 -b 1-1)
 * and open it from a WebUSB page served over https (or http://localhost):
 *   const dev = await navigator.usb.requestDevice({filters:[{vendorId:0x1209}]});
 *   await dev.open(); await dev.selectConfiguration(1); await dev.claimInterface(0);
 *   await dev.transferOut(1, new TextEncoder().encode("hi"));
 *   const r = await dev.transferIn(1, 64);   // -> "HI" (echoed, case swapped)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "usbip-device.h"

/* identity + wiring (a vendor device defines its own class 0xFF) */
#define VENDOR_ID    0x1209
#define PRODUCT_ID   0x0012
#define EP_BULK_OUT  0x01       /* host -> device */
#define EP_BULK_IN   0x81       /* device -> host */
#define MAX_PACKET   64

/* the bRequest the browser uses for the WebUSB GET_URL request. Kept distinct from
 * WinUSB's MS-OS codes (0x20/0x21) so the two mechanisms never collide. */
#define WEBUSB_VENDOR_CODE  0x22
#define LANDING_PAGE        "https://jabezwinston.github.io/web-apps/webusb-test.html"

int main(int argc, char **argv)
{
    int port = (argc > 1) ? atoi(argv[1]) : 3240;           /* default USB/IP port */
    const char *url = (argc > 2) ? argv[2] : LANDING_PAGE;   /* landing-page URL */

    usbip_device *dev = usbip_device_create(VENDOR_ID, PRODUCT_ID);
    usbip_device_set_strings(dev, "USB over IP", "USBIP WebUSB", "0012");
    usbip_device_enable_webusb(dev, WEBUSB_VENDOR_CODE, url);  /* BOS WebUSB cap + GET_URL */
    usbip_device_enable_winusb(dev, NULL);                     /* MS OS 2.0 -> WinUSB on Windows */

    /* One vendor interface (class 0xFF) with a bulk OUT and a bulk IN endpoint,
     * declared straight on the device core - no class layer. */
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

    usb_transport *transport = usbip_transport(NULL, port);
    int rc = usbip_device_plug(dev, transport);

    if (rc != USB_SUCCESS) {
        fprintf(stderr, "usbip_device_plug failed (port %d in use?)\n", port);
        return 1;
    }

    fprintf(stderr, "[webusb] serving %04x:%04x on :%d (bulk OUT 0x%02x, bulk IN 0x%02x)\n", VENDOR_ID, PRODUCT_ID, port, EP_BULK_OUT, EP_BULK_IN);
    fprintf(stderr, "[webusb] landing page %s  (GET_URL vendor code 0x%02x), WinUSB on\n", url, WEBUSB_VENDOR_CODE);

    /* Loopback: each usbip_device_read() returns one host OUT transfer; swap the case
     * of its ASCII letters and echo it back, so the transform shows at the host. */
    uint8_t buffer[8192];
    for (;;) {
        int n_received = usbip_device_read(bulk_out, buffer, sizeof(buffer), 0);  /* blocks until the host sends */

        if (n_received <= 0)
            continue;

        for (int i = 0; i < n_received; i++) {              /* swap case (letters only) */
            uint8_t ch = buffer[i];
            if ((ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z'))
                buffer[i] = ch ^ 0x20;
        }

        fprintf(stderr, "[webusb] RX %d bytes -> echo (case swapped)\n", n_received);
        usbip_device_write(bulk_in, buffer, n_received, 0);         /* device -> host */
    }
    return 0;
}
