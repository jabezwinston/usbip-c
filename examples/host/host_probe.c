/**
 * Copyright (C) 2026 Jabez Winston
 *
 * SPDX-License-Identifier: MIT
 *
 * Date : 04-June-2026
 *
 * host_probe.c - list + import devices from a USB/IP server with the C host.
 *
 * Used to drive a REAL kernel gadget (usbip-vudc + usbipd -e): enumerate, read
 * device + config descriptors, and issue a class control request if it's MSC.
 * Default server is 127.0.0.1:3240; pass a host as argv[1].
 */
#include <stdio.h>
#include "usbip-host.h"
#include "usbip.h"

int main(int argc, char **argv)
{
    usbip_host_context *ctx;
    if (usbip_host_init(&ctx) != USB_SUCCESS) return 1;
    if (argc > 1)
    {
        usb_transport *transport = usbip_transport(argv[1], 3240);

        usbip_host_set_transport(ctx, transport);
    }

    usbip_host_device **list;
    long n_devices = usbip_host_get_device_list(ctx, &list);
    if (n_devices < 0) {
        fprintf(stderr, "devlist failed (%s)\n", usb_strerror((int)n_devices));
        return 1;
    }
    printf("exported devices: %ld\n", n_devices);
    for (long i = 0; i < n_devices; i++) {
        usb_device_descriptor device_desc;
        usbip_host_get_device_descriptor(list[i], &device_desc);
        printf("  [%ld] %04x:%04x  (%d configuration(s))\n",
               i, device_desc.idVendor, device_desc.idProduct, device_desc.bNumConfigurations);
    }
    if (n_devices == 0) {
        usbip_host_free_device_list(list);
        usbip_host_exit(ctx);
        return 1;
    }

    /* Open the first device and read its descriptors over the control endpoint. */
    usbip_host_handle *handle;
    if (usbip_host_open(list[0], &handle) != USB_SUCCESS) {
        fprintf(stderr, "open failed\n");
        return 1;
    }
    usbip_host_free_device_list(list);

    usb_device_descriptor device_desc;
    int dev_bytes = usbip_host_control_transfer(handle, 0x80, 0x06, 0x0100, 0,
                                          (uint8_t *)&device_desc, 18, 1000);
    printf("opened [0]: GET_DESCRIPTOR(device) -> %d bytes, %04x:%04x\n",
           dev_bytes, device_desc.idVendor, device_desc.idProduct);

    /* Read the config descriptor: first 9 bytes give wTotalLength, then refetch the lot. */
    uint8_t cfg[256];
    usbip_host_control_transfer(handle, 0x80, 0x06, 0x0200, 0, cfg, 9, 1000);
    int total_len = usb_get_le16(cfg + 2);
    uint16_t want = (uint16_t)((total_len > 256) ? 256 : total_len);
    int cfg_bytes = usbip_host_control_transfer(handle, 0x80, 0x06, 0x0200, 0, cfg, want, 1000);
    printf("GET_DESCRIPTOR(config) -> %d bytes (wTotalLength=%d)\n", cfg_bytes, total_len);

    usbip_host_set_configuration(handle, cfg[5] ? cfg[5] : 1);   /* required before class requests */

    /* Walk the config blob descriptor-by-descriptor to find the first interface's class. */
    int iface_class = -1;
    int offset = 0;

    while (offset + 1 < cfg_bytes) {
        int blen = cfg[offset];
        int type = cfg[offset + 1];

        if (type == 0x04 && offset + 5 < cfg_bytes) {   /* INTERFACE */
            iface_class = cfg[offset + 5];
            break;                                      /* the *first* one, as printed */
        }
        offset += blen ? blen : 1;                      /* a zero bLength would not advance */
    }
    printf("first interface class: 0x%02x\n", iface_class);

    int rc = (device_desc.idVendor != 0) ? 0 : 1;
    if (iface_class == 0x08) {                            /* mass storage: GET_MAX_LUN */
        uint8_t max_lun = 0xff;
        int lun_bytes = usbip_host_control_transfer(handle, 0xA1, 0xFE, 0, 0, &max_lun, 1, 1000);
        printf("GET_MAX_LUN (class control) -> %d (rc=%d)\n", max_lun, lun_bytes);
        if (lun_bytes != 1) rc = 1;
    }

    usbip_host_close(handle);
    usbip_host_exit(ctx);
    if (rc == 0) printf("OK: C host enumerated + drove the device\n");
    return rc;
}
