/**
 * Copyright (C) 2026 Jabez Winston
 *
 * SPDX-License-Identifier: MIT
 *
 * Date : 04-June-2026
 *
 * usbip_host.c - host API (USB/IP client), libusb-shaped. Thin facade over the client
 * primitives in usbip.c; CLASS-FREE. Importing a device gives a usbip_host_handle whose
 * transfers map 1:1 onto CMD_SUBMIT/RET_SUBMIT.
 */
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "usbip_device_internal.h" /* for usbip_* client primitives + usbip_devinfo */
#include "usbip-host.h"

struct usbip_host_context
{
    usb_transport *t;
    int owns_t;
};
struct usbip_host_device
{
    usbip_host_context *ctx;
    struct usbip_devinfo info;
};

struct usbip_host_handle
{
    int fd;
    uint32_t devid;
    uint32_t seq;
    usb_device_descriptor desc;
};

int usbip_host_init(usbip_host_context **ctx)
{
    *ctx = calloc(1, sizeof(**ctx));
    return *ctx ? USB_SUCCESS : USB_ERROR_NO_MEM;
}

void usbip_host_exit(usbip_host_context *ctx)
{
    if (!ctx)
        return;
    if (ctx->owns_t && ctx->t)
        usbip_transport_free(ctx->t);
    free(ctx);
}

int usbip_host_set_transport(usbip_host_context *ctx, usb_transport *transport)
{
    if (ctx->owns_t && ctx->t)
        usbip_transport_free(ctx->t);
    ctx->t = transport;
    ctx->owns_t = 0;
    return USB_SUCCESS;
}

static usb_transport *ctx_transport(usbip_host_context *ctx)
{
    if (!ctx->t)
    {
        ctx->t = usbip_transport(NULL, 0);
        ctx->owns_t = 1;
    }
    return ctx->t;
}

/* ---- enumeration ------------------------------------------------------- */
long usbip_host_get_device_list(usbip_host_context *ctx, usbip_host_device ***list)
{
    usb_transport *transport = ctx_transport(ctx);
    int fd = usbip_connect(transport);

    if (fd < 0)
        return USB_ERROR_IO;

    struct usbip_devinfo infos[16];
    int count = usbip_client_devlist(fd, infos, 16);
    close(fd);
    if (count < 0)
        return count;

    usbip_host_device **arr = calloc((size_t)count + 1, sizeof(*arr));
    if (!arr)
        return USB_ERROR_NO_MEM;
    for (int i = 0; i < count; i++)
    {
        usbip_host_device *device = calloc(1, sizeof(*device));
        device->ctx = ctx;
        device->info = infos[i];
        arr[i] = device;
    }
    *list = arr;
    return count;
}

void usbip_host_free_device_list(usbip_host_device **list)
{
    if (!list)
        return;
    for (int i = 0; list[i]; i++)
        free(list[i]);
    free(list);
}

int usbip_host_get_device_descriptor(usbip_host_device *dev, usb_device_descriptor *out)
{
    struct usbip_devinfo *in = &dev->info;
    memset(out, 0, sizeof(*out));
    out->bLength = 18;
    out->bDescriptorType = USB_DT_DEVICE;
    out->bcdUSB = 0x0200;
    out->bDeviceClass = in->dclass;
    out->bDeviceSubClass = in->dsub;
    out->bDeviceProtocol = in->dproto;
    out->bMaxPacketSize0 = 64;
    out->idVendor = in->vid;
    out->idProduct = in->pid;
    out->bcdDevice = in->bcdDevice;
    out->bNumConfigurations = in->n_cfg;
    return USB_SUCCESS;
}

/* ---- open / close ------------------------------------------------------ */
int usbip_host_open(usbip_host_device *dev, usbip_host_handle **handle)
{
    usb_transport *transport = ctx_transport(dev->ctx);
    int fd = usbip_connect(transport);

    if (fd < 0)
        return USB_ERROR_IO;

    struct usbip_devinfo info;
    if (usbip_client_import(fd, dev->info.busid, &info) != USB_SUCCESS)
    {
        close(fd);
        return USB_ERROR_NO_DEVICE;
    }
    usbip_host_handle *opened = calloc(1, sizeof(*opened));
    opened->fd = fd;
    opened->devid = (info.busnum << 16) | info.devnum;
    usbip_host_get_device_descriptor(dev, &opened->desc);
    *handle = opened;
    return USB_SUCCESS;
}

usbip_host_handle *usbip_host_open_vid_pid(usbip_host_context *ctx, uint16_t vid, uint16_t pid)
{
    usbip_host_device **list;
    long count = usbip_host_get_device_list(ctx, &list);
    if (count < 0)
        return NULL;

    usbip_host_handle *handle = NULL;
    for (long i = 0; i < count; i++)
    {
        if (list[i]->info.vid == vid && list[i]->info.pid == pid)
        {
            if (usbip_host_open(list[i], &handle) != USB_SUCCESS)
                handle = NULL;
            break;
        }
    }
    usbip_host_free_device_list(list);
    return handle;
}

void usbip_host_close(usbip_host_handle *handle)
{
    if (!handle)
        return;
    if (handle->fd >= 0)
        close(handle->fd);
    free(handle);
}

/* ---- config / interface ----------------------------------------------- */
int usbip_host_set_configuration(usbip_host_handle *handle, int config)
{
    int rc = usbip_host_control_transfer(handle, 0x00, 0x09, (uint16_t)config, 0, NULL, 0, 1000);
    return rc < 0 ? rc : USB_SUCCESS;
}

/* USB/IP imports the whole device into userspace; there is no kernel driver to
 * detach on this side, so claim/release are no-ops that succeed. */
int usbip_host_claim_interface(usbip_host_handle *handle, int iface)
{
    (void)handle;
    (void)iface;
    return USB_SUCCESS;
}
int usbip_host_release_interface(usbip_host_handle *handle, int iface)
{
    (void)handle;
    (void)iface;
    return USB_SUCCESS;
}

int usbip_host_set_interface_alt_setting(usbip_host_handle *handle, int iface, int alt)
{
    int rc = usbip_host_control_transfer(handle, 0x01, 0x0B, (uint16_t)alt, (uint16_t)iface, NULL, 0, 1000);
    return rc < 0 ? rc : USB_SUCCESS;
}

int usbip_host_clear_halt(usbip_host_handle *handle, uint8_t endpoint)
{
    int rc = usbip_host_control_transfer(handle, 0x02, USB_REQ_CLEAR_FEATURE, USB_FEATURE_ENDPOINT_HALT, endpoint, NULL, 0, 1000);
    return rc < 0 ? rc : USB_SUCCESS;
}

/* ---- transfers (libusb signatures) ------------------------------------ */
int usbip_host_control_transfer(usbip_host_handle *handle, uint8_t bmRequestType, uint8_t bRequest,
                                uint16_t wValue, uint16_t wIndex, uint8_t *data,
                                uint16_t wLength, unsigned timeout_ms)
{
    (void)timeout_ms;
    uint8_t setup[8] = {USB_SETUP_BYTES(bmRequestType, bRequest, wValue, wIndex, wLength)};
    int dir = (bmRequestType & 0x80) ? USB_IN : USB_OUT;
    int actual = 0;
    int rc = usbip_client_submit(handle->fd, handle->devid, &handle->seq, dir, 0, setup, 0, data, wLength, &actual);
    return rc < 0 ? rc : actual; /* control returns bytes transferred */
}

static int data_transfer(usbip_host_handle *handle, uint8_t endpoint, uint8_t *data, int length,
                         int *transferred, int interval)
{
    int dir = (endpoint & 0x80) ? USB_IN : USB_OUT;
    int actual = 0;
    int rc = usbip_client_submit(handle->fd, handle->devid, &handle->seq, dir, endpoint & 0x0f, NULL, interval, data, length, &actual);
    if (transferred)
        *transferred = actual;
    return rc < 0 ? rc : USB_SUCCESS; /* bulk/intr return 0 on success */
}

int usbip_host_bulk_transfer(usbip_host_handle *handle, uint8_t endpoint, uint8_t *data, int length,
                             int *transferred, unsigned timeout_ms)
{
    (void)timeout_ms;
    return data_transfer(handle, endpoint, data, length, transferred, 0);
}

int usbip_host_interrupt_transfer(usbip_host_handle *handle, uint8_t endpoint, uint8_t *data, int length,
                                  int *transferred, unsigned timeout_ms)
{
    (void)timeout_ms;
    return data_transfer(handle, endpoint, data, length, transferred, 1);
}

/* ---- isochronous transfers (libusb-shaped, synchronous) ---------------- */
usbip_host_transfer *usbip_host_alloc_transfer(int num_iso_packets)
{
    usbip_host_transfer *transfer = calloc(1, sizeof(*transfer) + (size_t)num_iso_packets * sizeof(usbip_host_iso_packet_descriptor));
    if (transfer)
    {
        transfer->num_iso_packets = num_iso_packets;
        transfer->type = USB_ISO;
    }
    return transfer;
}

void usbip_host_free_transfer(usbip_host_transfer *transfer)
{
    free(transfer);
}

void usbip_host_fill_iso_transfer(usbip_host_transfer *transfer, usbip_host_handle *handle, uint8_t endpoint,
                                  uint8_t *buffer, int length, int num_iso_packets,
                                  unsigned timeout_ms)
{
    transfer->dev_handle = handle;
    transfer->endpoint = endpoint;
    transfer->type = USB_ISO;
    transfer->buffer = buffer;
    transfer->length = length;
    transfer->num_iso_packets = num_iso_packets;
    transfer->timeout_ms = timeout_ms;
}

void usbip_host_set_iso_packet_lengths(usbip_host_transfer *transfer, unsigned length)
{
    for (int i = 0; i < transfer->num_iso_packets; i++)
        transfer->iso_packet_desc[i].length = length;
}

uint8_t *usbip_host_get_iso_packet_buffer_simple(usbip_host_transfer *transfer, unsigned packet)
{
    if ((int)packet >= transfer->num_iso_packets || transfer->num_iso_packets == 0)
        return NULL;
    return transfer->buffer + packet * (transfer->length / transfer->num_iso_packets);
}

int usbip_host_submit_transfer(usbip_host_transfer *transfer)
{
    usbip_host_handle *handle = transfer->dev_handle;
    int np = transfer->num_iso_packets;
    int dir = (transfer->endpoint & 0x80) ? USB_IN : USB_OUT;

    struct iso_pkt *pkts = calloc((size_t)np, sizeof(*pkts));
    if (!pkts)
        return USB_ERROR_NO_MEM;
    uint32_t off = 0;
    for (int i = 0; i < np; i++)
    { /* slot offset = Σ earlier lengths */
        pkts[i].offset = off;
        pkts[i].length = transfer->iso_packet_desc[i].length;
        off += pkts[i].length;
    }
    int total = 0;
    int rc = usbip_client_submit_iso(handle->fd, handle->devid, &handle->seq, dir, transfer->endpoint & 0x0f,
                                     0, transfer->buffer, pkts, np, &total);
    for (int i = 0; i < np; i++)
    {
        transfer->iso_packet_desc[i].actual_length = pkts[i].actual_length;
        transfer->iso_packet_desc[i].status = pkts[i].status;
    }
    free(pkts);
    transfer->actual_length = total;
    transfer->status = rc < 0 ? rc : 0;
    return rc < 0 ? rc : USB_SUCCESS;
}

/* ---- host driver registry (mirror of usbip_device_class; auto-bind is a follow-up) */
#define MAX_DRIVERS 32
static const usbip_host_driver *g_drivers[MAX_DRIVERS];
static int g_driver_count;

void usbip_host_register_driver(const usbip_host_driver *drv)
{
    if (g_driver_count < MAX_DRIVERS)
        g_drivers[g_driver_count++] = drv;
}
