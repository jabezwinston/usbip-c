/**
 *
 * In USB terms this program is the HOST; in USB/IP terms it is the CLIENT - it
 * CONNECTS to a server that is already serving a device (the device end listens).
 *
 * It lists what the server exports, opens the vendor bulk
 * device, reads its descriptor over a control transfer and round-trips a payload
 * through its loopback endpoints.
 *
 * Start the device first, then drive it:
 *   ./vendor_device &         # serves 1209:0004 on :3240
 *   ./host_drive              # ...or: ./host_drive 10.0.0.5 3240
 *
 * The same code drives a REAL device exported by a real usbipd - point it at that
 * server and change the vendor/product ids.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "usbip-host.h"

#define VENDOR_ID   0x1209 /* what vendor_device serves */
#define PRODUCT_ID  0x0004
#define EP_BULK_OUT 0x01   /* host -> device */
#define EP_BULK_IN  0x81   /* device -> host */

int main(int argc, char **argv)
{
    const char *server = (argc > 1) ? argv[1] : NULL; /* NULL = local, 127.0.0.1 */
    int port = (argc > 2) ? atoi(argv[2]) : 3240;

    /* 1. a context. It starts on the local transport; name a server to go remote. */
    usbip_host_context *ctx;
    int rc = usbip_host_init(&ctx);

    if (rc != USB_SUCCESS)
    {
        fprintf(stderr, "usbip_host_init failed: %s\n", usb_strerror(rc));
        return 1;
    }
    if (server)
    {
        usb_transport *transport = usbip_transport(server, port);

        usbip_host_set_transport(ctx, transport);
    }

    /* 2. what does the server export? (the USB/IP device list) */
    usbip_host_device **list;
    long n_devices = usbip_host_get_device_list(ctx, &list);

    if (n_devices < 0)
    {
        fprintf(stderr, "device list failed: %s (is a device being served?)\n",
                usb_strerror((int)n_devices));
        usbip_host_exit(ctx);
        return 1;
    }
    printf("exported devices: %ld\n", n_devices);

    for (long i = 0; i < n_devices; i++)
    {
        usb_device_descriptor desc;
        usbip_host_get_device_descriptor(list[i], &desc);
        printf("  [%ld] %04x:%04x\n", i, desc.idVendor, desc.idProduct);
    }
    usbip_host_free_device_list(list);

    /* 3. import one of them by vendor/product id */
    usbip_host_handle *handle = usbip_host_open_vid_pid(ctx, VENDOR_ID, PRODUCT_ID);

    if (!handle)
    {
        fprintf(stderr, "open %04x:%04x failed (is vendor_device running?)\n",
                VENDOR_ID, PRODUCT_ID);
        usbip_host_exit(ctx);
        return 1;
    }

    /* 4. a control transfer: the standard GET_DESCRIPTOR(device) */
    usb_device_descriptor desc;
    int desc_bytes = usbip_host_control_transfer(handle, USB_REQ_DIR_IN, USB_REQ_GET_DESCRIPTOR,
                                                 0x0100, 0, (uint8_t *)&desc, sizeof(desc), 1000);
    printf("opened %04x:%04x (device descriptor: %d bytes, USB %x.%02x)\n",
           desc.idVendor, desc.idProduct, desc_bytes, USB_U16_MSB(desc.bcdUSB), USB_U16_LSB(desc.bcdUSB));

    usbip_host_set_configuration(handle, 1); /* required before endpoint I/O */
    usbip_host_claim_interface(handle, 0);

    /* 5. bulk I/O: send a payload, read the device's echo back */
    //! [io]
    const char *message = "hello device";
    uint8_t echo[64];
    int n_sent = 0;
    int n_received = 0;

    rc = usbip_host_bulk_transfer(handle, EP_BULK_OUT, (uint8_t *)message,
                                  (int)strlen(message), &n_sent, 1000);
    if (rc == USB_SUCCESS)
        rc = usbip_host_bulk_transfer(handle, EP_BULK_IN, echo, sizeof(echo), &n_received, 1000);
    //! [io]

    if (rc != USB_SUCCESS)
    {
        fprintf(stderr, "bulk transfer failed: %s\n", usb_strerror(rc));
        usbip_host_close(handle);
        usbip_host_exit(ctx);
        return 1;
    }
    printf("sent %d bytes, received %d back: \"%.*s\"\n", n_sent, n_received, n_received, echo);

    /* 6. done */
    usbip_host_release_interface(handle, 0);
    usbip_host_close(handle);
    usbip_host_exit(ctx);

    int matched = (n_received == n_sent) && memcmp(echo, message, (size_t)n_sent) == 0;
    const char *verdict = matched ? "OK: loopback round-trip matched\n" : "MISMATCH\n";

    printf("%s", verdict);

    return matched ? 0 : 1;
}
