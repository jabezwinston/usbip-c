/*
 * Copyright (C) 2026 Jabez Winston
 *
 * SPDX-License-Identifier: MIT
 *
 * Date : 04-June-2026
 */
/**
 * @file classes/cdc_acm.h
 * @ingroup class_cdc
 * @brief CDC-ACM (virtual serial) device class, app-facing API.
 *
 * The class implements the CDC protocol; your app supplies callbacks for the
 * events it cares about (port opened/closed, line coding changed, bytes received)
 * and uses cdc_acm_send() to transmit. Built entirely on the public usbip_device.h API.
 *
 * A device may carry several ports - call cdc_acm_add() once per port. Each call
 * returns its own ::cdc_port, and every callback is handed the port it belongs to,
 * so one set of callbacks can serve them all.
 */
#ifndef CDC_ACM_H
#define CDC_ACM_H

#include "usb_class.h"

/**
 * @addtogroup class_cdc
 * @{
 */

/** Serial line parameters (USB CDC "line coding"). */
typedef struct {
    uint32_t baud;        /**< Baud rate */
    uint8_t  stop_bits;   /**< 0 = 1 stop bit, 1 = 1.5, 2 = 2 */
    uint8_t  parity;      /**< 0 none, 1 odd, 2 even, 3 mark, 4 space */
    uint8_t  data_bits;   /**< 5, 6, 7, 8, or 16 */
} cdc_line_coding;

/**
 * One CDC-ACM serial port: a single instance of the class on a device.
 *
 * Create one with cdc_acm_add(); it stays valid for the life of the device. The
 * fields are maintained by the class and are **read-only** to the app - they are
 * here so a callback can tell which port fired and how it is configured without
 * having to keep its own table. `line_coding` and `is_open` track the host.
 *
 * It is a distinct type from the other classes' handles, so passing (say) a
 * ::hid_iface to cdc_acm_send() is a compile error, not a misread state block.
 */
typedef struct cdc_port {
    int             index;            /**< 0-based port number on this device, in
                                       *   cdc_acm_add() order - the same order the
                                       *   host enumerates `/dev/ttyACM<n>`         */
    const char     *name;             /**< ::cdc_acm_opts::name, or `""` (never NULL) */
    void           *user;             /**< ::cdc_acm_opts::user, passed straight through */
    int             interface_number; /**< The Communications interface's bInterfaceNumber */
    uint8_t         in_ep;            /**< Bulk IN address actually assigned (a second
                                       *   port is relocated off the default 0x81)  */
    uint8_t         out_ep;           /**< Bulk OUT address actually assigned        */
    cdc_line_coding line_coding;      /**< The line coding the host last set         */
    int             is_open;          /**< Non-zero while the host asserts DTR       */
    usbip_device   *dev;              /**< The owning device                         */
    usbip_function *func;             /**< The underlying function - escape hatch to
                                       *   the core usbip_device.h API               */
} cdc_port;

/** App callbacks and per-port settings. Any callback may be NULL. `port` identifies
 *  which port fired (use it with cdc_acm_send() and read its fields); `user` is your
 *  context pointer, passed straight through. */
typedef struct {
    /**
     * The host opened the port - asserted DTR (optional).
     * @param port    the port that was opened.
     * @param user    the @c user pointer.
     * @param coding  the line coding in effect at open time.
     */
    void (*on_open)(cdc_port *port, void *user, const cdc_line_coding *coding);
    /**
     * The host closed the port - cleared DTR (optional).
     * @param port  the port that was closed.
     * @param user  the @c user pointer.
     */
    void (*on_close)(cdc_port *port, void *user);
    /**
     * The host changed the line coding - baud/parity/etc. (optional).
     * @param port    the port whose settings changed.
     * @param user    the @c user pointer.
     * @param coding  the new line coding.
     */
    void (*on_line_coding)(cdc_port *port, void *user, const cdc_line_coding *coding);
    /**
     * Bytes arrived from the host (optional). Transmit back with cdc_acm_send().
     * @param port  the port the bytes arrived on.
     * @param user  the @c user pointer.
     * @param data  the received bytes.
     * @param len   number of bytes in @p data.
     */
    void (*on_rx)(cdc_port *port, void *user, const void *data, int len);
    /** Optional iInterface name for this port, e.g. "Console" - shown by the host
     *  (Linux: `/sys/bus/usb/devices/.../interface`; Windows: the device name). */
    const char *name;
    void *user;           /**< Opaque pointer passed to every callback above */
} cdc_acm_opts;

/**
 * Add a CDC-ACM serial port to a device. Call it once per port.
 * @param dev  the device.
 * @param opts  app callbacks and settings (see ::cdc_acm_opts), or `NULL` for a silent port.
 * @return the port handle, or `NULL` on error.
 */
cdc_port *cdc_acm_add(usbip_device *dev, const cdc_acm_opts *opts);

/**
 * Transmit bytes to the host (device -> host).
 * @param port  the port.
 * @param data  the bytes to send.
 * @param len   number of bytes.
 * @return the number of bytes queued/sent (>= 0), or a negative @ref errors "USB_ERROR code".
 */
int cdc_acm_send(cdc_port *port, const void *data, int len);

extern const usbip_device_class usbip_device_cdc_acm;  /**< The CDC-ACM class, for usbip_device_add_class() */

/** @} */

#endif /* CDC_ACM_H */
