/**
 * Copyright (C) 2026 Jabez Winston
 *
 * SPDX-License-Identifier: MIT
 *
 * Date : 04-June-2026
 *
 * cdc_host.c - drive the CDC-ACM device with OUR C host (no kernel, no root).
 *
 * Mirror of the pyserial test: opens the port (line coding + DTR), sends data,
 * reads the echo back - but using usbip_host_* instead of the kernel serial stack.
 * Run cdc_acm_device first (it serves on :3240).
 */
#include <stdio.h>
#include <string.h>

#include "usbip-host.h"

int main(void)
{
    usbip_host_context *ctx;
    if (usbip_host_init(&ctx) != USB_SUCCESS) return 1;          /* default transport: 127.0.0.1:3240 */

    usbip_host_handle *handle = usbip_host_open_vid_pid(ctx, 0x1209, 0x0001);
    if (!handle) {
        fprintf(stderr, "open 1209:0001 failed (is cdc_acm_device running?)\n");
        return 1;
    }

    /* read the device descriptor over a control IN transfer */
    usb_device_descriptor device_desc;
    int desc_bytes = usbip_host_control_transfer(handle, 0x80, 0x06, 0x0100, 0,
                                           (uint8_t *)&device_desc, 18, 1000);
    printf("imported %04x:%04x (control GET_DESCRIPTOR returned %d bytes)\n",
           device_desc.idVendor, device_desc.idProduct, desc_bytes);

    usbip_host_set_configuration(handle, 1);

    /* CDC: open the "port" - set 115200 8N1, assert DTR. The SET_LINE_CODING
     * payload is dwDTERate (little-endian), bCharFormat, bParityType, bDataBits. */
    uint8_t line_coding[7] = { 0x00, 0xC2, 0x01, 0x00,  /* 115200 baud */
                               0,                        /* 1 stop bit  */
                               0,                        /* no parity   */
                               8 };                      /* 8 data bits */
    usbip_host_control_transfer(handle, 0x21, 0x20, 0, 0, line_coding, 7, 1000);  /* SET_LINE_CODING */
    usbip_host_control_transfer(handle, 0x21, 0x22, 0x0001, 0, NULL, 0, 1000);    /* SET_CONTROL_LINE_STATE DTR=1 */
    printf("opened port @115200, DTR=1\n");

    /* TX then RX (the device echoes) */
    int rc = 0;
    for (int i = 0; i < 2; i++) {
        const char *msg = i == 0 ? "ping\n" : "USBIP-cdc\n";
        int len = (int)strlen(msg);
        int n_tx = 0;
        int n_rx = 0;
        uint8_t echo[64];
        usbip_host_bulk_transfer(handle, 0x01, (uint8_t *)msg, len, &n_tx, 1000);  /* OUT */
        usbip_host_bulk_transfer(handle, 0x81, echo, sizeof(echo), &n_rx, 1000);    /* IN (echo) */
        printf("  host TX %d bytes %.*s host RX %d bytes %.*s",
               n_tx, len, msg, n_rx, n_rx, (char *)echo);
        if (n_rx != len || memcmp(echo, msg, (size_t)len) != 0) {
            rc = 1;
            printf("  MISMATCH\n");
        }
    }

    usbip_host_control_transfer(handle, 0x21, 0x22, 0x0000, 0, NULL, 0, 1000);     /* DTR=0: close */
    printf("closed port (DTR=0)\n");

    usbip_host_close(handle);
    usbip_host_exit(ctx);
    if (rc == 0) printf("OK: C host drove the C CDC-ACM device (open/baud/TX/RX/close)\n");
    return rc;
}
