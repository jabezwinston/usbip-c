/*
 * Copyright (C) 2026 Jabez Winston
 *
 * SPDX-License-Identifier: MIT
 *
 * Date : 04-June-2026
 */
/**
 * @file usbip-host.h
 * @ingroup host
 * @brief Host API - write host drivers that drive USB devices over USB/IP.
 *
 * Deliberately libusb-shaped: porting libusb code is essentially
 * `s/libusb_/usbip_host_/`. The one addition is usbip_host_set_transport().
 *
 * Conventions: functions return ::USB_SUCCESS (0) on success and a negative
 * @ref errors "USB_ERROR code" on failure, unless noted (the transfer calls
 * return a byte count, and the constructors return a pointer or `NULL`).
 */
#ifndef USBIP_HOST_H
#define USBIP_HOST_H

#include "usbip.h"

/**
 * @addtogroup host
 * @{
 */

typedef struct usbip_host_context usbip_host_context;  /**< Library session / device list owner */
typedef struct usbip_host_device  usbip_host_device;   /**< An enumerated device, not yet opened */
typedef struct usbip_host_handle  usbip_host_handle;   /**< An opened device, usable for I/O     */

/**
 * Create a library context (the root object for enumeration and I/O).
 *
 * The context starts on the local transport; call usbip_host_set_transport() to point
 * it at a remote USB/IP server. Pair every successful call with usbip_host_exit().
 *
 * @param[out] ctx  receives the new context on success.
 * @retval USB_SUCCESS       context created.
 * @retval USB_ERROR_NO_MEM  allocation failed (@p *ctx is left `NULL`).
 */
int  usbip_host_init(usbip_host_context **ctx);

/**
 * Destroy a context created by usbip_host_init() and free its resources.
 * @param ctx  the context (may be `NULL`).
 */
void usbip_host_exit(usbip_host_context *ctx);

/**
 * Bind a USB/IP transport to the context - the only USB/IP-aware call.
 *
 * This side is the USB/IP **client**: the transport names the server to *connect* to,
 * which must already be serving (a program on the @ref device "device API", or a real
 * `usbipd`). Note that the wrappers' `USBIP_HOST` / `USBIP_PORT` variables do
 * not apply here - this call is how a program using the host API picks its server.
 *
 * Skip it and everything stays local. Set the transport before enumerating or
 * opening devices; the context does not take ownership (free it yourself with
 * usbip_transport_free() after usbip_host_exit()).
 *
 * @param ctx        the context.
 * @param transport  transport from usbip_transport(); `NULL` restores the local default.
 * @retval USB_SUCCESS  always.
 */
int  usbip_host_set_transport(usbip_host_context *ctx, usb_transport *transport);

/* enumerate */
/**
 * Enumerate the devices reachable on the context's transport.
 *
 * On success @p list points to a freshly allocated, `NULL`-terminated array of
 * ::usbip_host_device pointers; release it with usbip_host_free_device_list().
 *
 * @param ctx        the context.
 * @param[out] list  receives the device-list array.
 * @return the number of devices (>= 0), or a negative @ref errors "USB_ERROR code"
 *         (e.g. ::USB_ERROR_IO if the transport is unreachable, ::USB_ERROR_NO_MEM).
 */
long usbip_host_get_device_list(usbip_host_context *ctx, usbip_host_device ***list);

/**
 * Free a device list returned by usbip_host_get_device_list().
 * @param list  the array to free (may be `NULL`). Handles opened from it stay valid.
 */
void usbip_host_free_device_list(usbip_host_device **list);

/**
 * Copy an enumerated device's 18-byte device descriptor.
 * @param dev       a device from usbip_host_get_device_list().
 * @param[out] out  receives the descriptor.
 * @retval USB_SUCCESS  on success.
 * @return a negative @ref errors "USB_ERROR code" otherwise.
 */
int  usbip_host_get_device_descriptor(usbip_host_device *dev, usb_device_descriptor *out);

/* open / close */
/**
 * Open an enumerated device, yielding a handle for I/O.
 * @param dev          the device to open.
 * @param[out] handle  receives the open handle on success.
 * @retval USB_SUCCESS         opened.
 * @retval USB_ERROR_NO_DEVICE the device is gone.
 * @retval USB_ERROR_IO        the transport failed.
 */
int usbip_host_open(usbip_host_device *dev, usbip_host_handle **handle);

/**
 * Convenience: enumerate, match the first device by VID:PID, and open it.
 * @param ctx  the context.
 * @param vid  vendor ID to match.
 * @param pid  product ID to match.
 * @return an open handle, or `NULL` if no match was found or opening failed.
 */
usbip_host_handle *usbip_host_open_vid_pid(usbip_host_context *ctx, uint16_t vid, uint16_t pid);

/**
 * Close a handle from usbip_host_open() / usbip_host_open_vid_pid().
 * @param handle  the handle (may be `NULL`).
 */
void usbip_host_close(usbip_host_handle *handle);

/* config / interface */
/**
 * Select a configuration (standard SET_CONFIGURATION request).
 * @param handle  an open handle.
 * @param config  the bConfigurationValue to activate.
 * @retval USB_SUCCESS  on success; a negative @ref errors "USB_ERROR code" otherwise.
 */
int usbip_host_set_configuration(usbip_host_handle *handle, int config);

/**
 * Claim an interface.
 *
 * USB/IP has no kernel driver to detach and no exclusive-ownership concept, so
 * this is a compatibility no-op kept for libusb source parity.
 *
 * @param handle  an open handle.
 * @param iface   the interface number.
 * @retval USB_SUCCESS  always.
 */
int usbip_host_claim_interface(usbip_host_handle *handle, int iface);

/**
 * Release an interface claimed with usbip_host_claim_interface() (compatibility no-op).
 * @param handle  an open handle.
 * @param iface   the interface number.
 * @retval USB_SUCCESS  always.
 */
int usbip_host_release_interface(usbip_host_handle *handle, int iface);

/**
 * Select an interface's alternate setting (standard SET_INTERFACE request).
 * @param handle  an open handle.
 * @param iface   the interface number.
 * @param alt     the alternate setting to activate.
 * @retval USB_SUCCESS  on success; a negative @ref errors "USB_ERROR code" otherwise.
 */
int usbip_host_set_interface_alt_setting(usbip_host_handle *handle, int iface, int alt);

/**
 * Clear a halted (STALLed) endpoint - `CLEAR_FEATURE(ENDPOINT_HALT)`.
 *
 * The standard recovery after a transfer returns ::USB_ERROR_PIPE: a device
 * halts a pipe to abandon a transfer it cannot complete, and the pipe stays
 * halted until this clears it. Mass storage relies on it - a failed data phase
 * halts the bulk pipe, and the status wrapper can only be read once cleared.
 * @param handle    an open handle.
 * @param endpoint  the endpoint address, direction bit included.
 * @retval USB_SUCCESS  on success; a negative @ref errors "USB_ERROR code" otherwise.
 */
int usbip_host_clear_halt(usbip_host_handle *handle, uint8_t endpoint);

/* synchronous transfers - byte-for-byte libusb signatures */
/**
 * Perform a synchronous control transfer on endpoint 0 (libusb-shaped).
 *
 * @param handle        an open handle.
 * @param bmRequestType request type bitmask (direction in bit 7 - see ::usb_setup).
 * @param bRequest      request code.
 * @param wValue        request-specific value.
 * @param wIndex        request-specific index.
 * @param data          IN: buffer that receives up to @p wLength bytes; OUT: the
 *                      @p wLength bytes to send (may be `NULL` when @p wLength is 0).
 * @param wLength       size of the data stage in bytes.
 * @param timeout_ms    timeout in milliseconds (0 = wait indefinitely).
 * @return the number of bytes transferred (>= 0), or a negative
 *         @ref errors "USB_ERROR code" - notably ::USB_ERROR_PIPE if the device
 *         STALLed the request, or ::USB_ERROR_TIMEOUT.
 */
int usbip_host_control_transfer(usbip_host_handle *handle, uint8_t bmRequestType, uint8_t bRequest,
        uint16_t wValue, uint16_t wIndex, uint8_t *data, uint16_t wLength,
        unsigned timeout_ms);

/**
 * Perform a synchronous bulk transfer (libusb-shaped).
 *
 * @param handle           an open handle.
 * @param endpoint         endpoint address; bit 7 sets direction (0x81 = IN, 0x01 = OUT).
 * @param data             IN: receive buffer; OUT: data to send.
 * @param length           buffer size / bytes to send.
 * @param[out] transferred receives the number of bytes actually transferred (may be `NULL`).
 * @param timeout_ms       timeout in milliseconds (0 = wait indefinitely).
 * @retval USB_SUCCESS  on success (count returned via @p transferred).
 * @return a negative @ref errors "USB_ERROR code" otherwise (e.g. ::USB_ERROR_PIPE,
 *         ::USB_ERROR_TIMEOUT).
 */
int usbip_host_bulk_transfer(usbip_host_handle *handle, uint8_t endpoint, uint8_t *data, int length,
        int *transferred, unsigned timeout_ms);

/**
 * Perform a synchronous interrupt transfer (libusb-shaped). Same contract as
 * usbip_host_bulk_transfer().
 *
 * @param handle           an open handle.
 * @param endpoint         endpoint address (bit 7 = direction).
 * @param data             IN: receive buffer; OUT: data to send.
 * @param length           buffer size / bytes to send.
 * @param[out] transferred receives the byte count actually transferred (may be `NULL`).
 * @param timeout_ms       timeout in milliseconds (0 = wait indefinitely).
 * @retval USB_SUCCESS  on success; a negative @ref errors "USB_ERROR code" otherwise.
 */
int usbip_host_interrupt_transfer(usbip_host_handle *handle, uint8_t endpoint, uint8_t *data, int length,
        int *transferred, unsigned timeout_ms);

/* ---- isochronous transfers (libusb-shaped: s/libusb_/usbip_host_/) --------------
 * Same alloc/fill/submit shape as libusb, but usbip_host_submit_transfer() is
 * SYNCHRONOUS: it completes before returning. iso_packet_desc[] then holds each
 * packet's actual_length/status, and `buffer` its data at offset = Σ earlier lengths. */

/** One isochronous packet's request/result (libusb-shaped). */
typedef struct {
    unsigned int length;          /**< Requested length (set before submit)        */
    unsigned int actual_length;   /**< Bytes actually transferred (filled on done)  */
    int          status;          /**< 0 = ok, negative = error                     */
} usbip_host_iso_packet_descriptor;

/** An isochronous transfer (libusb-shaped; submitted synchronously). */
typedef struct usbip_host_transfer {
    usbip_host_handle *dev_handle;      /**< Target handle (set by usbip_host_fill_iso_transfer()) */
    uint8_t      endpoint;        /**< Endpoint address (bit 7 = direction)            */
    uint8_t      type;            /**< Transfer type - ::USB_ISO                       */
    unsigned     timeout_ms;      /**< Timeout in milliseconds (0 = indefinite)        */
    int          status;          /**< Overall status, 0 = ok                          */
    uint8_t     *buffer;          /**< Packet data, laid out contiguously per packet   */
    int          length;          /**< Total buffer length                             */
    int          actual_length;   /**< Total bytes transferred (filled on done)        */
    int          num_iso_packets; /**< Number of packets / iso_packet_desc[] entries   */
    usbip_host_iso_packet_descriptor iso_packet_desc[];   /**< Flexible array, one per packet */
} usbip_host_transfer;

/**
 * Allocate an iso transfer with room for @p num_iso_packets packet descriptors.
 * @param num_iso_packets  number of isochronous packets (>= 1).
 * @return a zeroed ::usbip_host_transfer, or `NULL` on allocation failure. Free it with
 *         usbip_host_free_transfer().
 */
usbip_host_transfer *usbip_host_alloc_transfer(int num_iso_packets);

/**
 * Free a transfer from usbip_host_alloc_transfer().
 * @param transfer  the transfer (may be `NULL`). The data @c buffer is the caller's and is
 *                  not freed here.
 */
void usbip_host_free_transfer(usbip_host_transfer *transfer);

/**
 * Populate an iso transfer's fields (libusb-shaped convenience setter).
 * @param transfer         the transfer.
 * @param handle           target handle.
 * @param endpoint         endpoint address (bit 7 = direction).
 * @param buffer           packet data buffer (caller-owned).
 * @param length           total @p buffer length.
 * @param num_iso_packets  packet count (must match usbip_host_alloc_transfer()).
 * @param timeout_ms       timeout in milliseconds (0 = indefinite).
 */
void usbip_host_fill_iso_transfer(usbip_host_transfer *transfer, usbip_host_handle *handle, uint8_t endpoint,
        uint8_t *buffer, int length, int num_iso_packets, unsigned timeout_ms);

/**
 * Set every packet's requested length to @p length (the common equal-size case).
 * @param transfer  the transfer.
 * @param length    per-packet requested length in bytes.
 */
void usbip_host_set_iso_packet_lengths(usbip_host_transfer *transfer, unsigned length);

/**
 * Pointer to packet @p packet's data within the transfer buffer (assumes
 * equal-size packets).
 * @param transfer  the transfer.
 * @param packet    zero-based packet index.
 * @return a pointer into @c t->buffer, or `NULL` if @p packet is out of range.
 */
uint8_t *usbip_host_get_iso_packet_buffer_simple(usbip_host_transfer *transfer, unsigned packet);

/**
 * Submit - and, here, synchronously complete - an isochronous transfer.
 *
 * On return, @c t->actual_length and each `iso_packet_desc[i].actual_length` /
 * `.status` are filled in.
 *
 * @param transfer  a transfer prepared with usbip_host_fill_iso_transfer().
 * @retval USB_SUCCESS  the transfer completed (check per-packet status for partials).
 * @return a negative @ref errors "USB_ERROR code" if it could not be submitted.
 */
int  usbip_host_submit_transfer(usbip_host_transfer *transfer);    /* synchronous; 0 = ok */

/** @} */

#endif /* USBIP_HOST_H */
