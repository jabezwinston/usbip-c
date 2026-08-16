/*
 * Copyright (C) 2026 Jabez Winston
 *
 * SPDX-License-Identifier: MIT
 *
 * Date : 05-June-2026
 */
/**
 * @file classes/bluetooth.h
 * @ingroup class_bt
 * @brief USB Bluetooth (class 0xE0/0x01/0x01) DEVICE class.
 *
 * A minimal HCI transport: it carries HCI commands (EP0 class control OUT),
 * HCI events (interrupt IN) and ACL data (bulk OUT/IN) between the USB host and
 * a controller the application implements. No HCI logic lives here - the class
 * never inspects a command or synthesizes an event. Mirrors the Python library's
 * usbip/classes/device/bluetooth.py.
 *
 * Bluetooth USB Transport Layer (Core spec Vol 4 Part B):
 *   - device class 0xE0 / subclass 0x01 (RF) / protocol 0x01 (Bluetooth)
 *   - interface 0: interrupt-IN (events) + bulk-OUT (ACL out) + bulk-IN (ACL in)
 *   - interface 1: SCO isochronous (6 alt settings) - descriptor-only / no-op
 */
#ifndef CLASSES_BLUETOOTH_H
#define CLASSES_BLUETOOTH_H

#include "usb_class.h"

/**
 * @addtogroup class_bt
 * @{
 */

/**
 * One Bluetooth HCI transport: a single instance of the class on a device.
 *
 * Opaque - create one with bluetooth_add() and use the functions below. Distinct from
 * the other classes' handles, so mixing them up is a compile error rather than a
 * misread state block.
 */
typedef struct bt_hci bt_hci;

/** Options passed to bluetooth_add(). The callbacks fire on the USB serving
 *  thread; keep them short and hand work off to the controller. */
typedef struct {
    /* host -> controller */
    /**
     * An HCI command arrived from the host on endpoint 0 (host -> controller).
     * @param iface    the Bluetooth handle.
     * @param cmd  the HCI command packet.
     * @param len  number of bytes in @p cmd.
     */
    void (*on_command)(bt_hci *iface, const uint8_t *cmd, int len);
    /**
     * An ACL data PDU arrived from the host on the bulk-OUT endpoint (host -> controller).
     * @param iface    the Bluetooth handle.
     * @param pdu  the ACL PDU (4-byte header + payload).
     * @param len  number of bytes in @p pdu.
     */
    void (*on_acl)(bt_hci *iface, const uint8_t *pdu, int len);
    int   with_sco;     /**< Declare the SCO isochronous interface (no-op, fidelity) */
    void *user;         /**< Opaque app pointer, retrievable via bluetooth_user()    */
} bt_opts;

/**
 * Add a Bluetooth dongle (HCI transport) to a device. Sets the device class to
 * ::USB_CLASS_WIRELESS / 0x01 / 0x01 (Wireless Controller / RF / Bluetooth).
 * @param dev     the device.
 * @param opts  host->controller callbacks + options (see ::bt_opts).
 * @return the new function (use it with bt_send_event() / bt_send_acl()), or `NULL`.
 */
bt_hci *bluetooth_add(usbip_device *dev, const bt_opts *opts);

/* controller -> host */
/**
 * Send an HCI event to the host on the interrupt-IN endpoint (controller -> host).
 * @param iface    the interface from bluetooth_add().
 * @param evt  the HCI event packet.
 * @param len  event length in bytes.
 * @return bytes sent (>= 0), or a negative @ref errors "USB_ERROR code".
 */
int bt_send_event(bt_hci *iface, const void *evt, int len);
/**
 * Send an ACL data PDU to the host on the bulk-IN endpoint (controller -> host).
 * @param iface    the interface from bluetooth_add().
 * @param pdu  the ACL PDU (4-byte header + payload).
 * @param len  PDU length in bytes.
 * @return bytes sent (>= 0), or a negative @ref errors "USB_ERROR code".
 */
int bt_send_acl(bt_hci *iface, const void *pdu, int len);

/**
 * Retrieve the opaque application pointer passed as ::bt_opts::user.
 * @param iface  the interface from bluetooth_add().
 * @return the stored `user` pointer.
 */
void *bluetooth_user(bt_hci *iface);

extern const usbip_device_class usbip_device_bluetooth;  /**< The Bluetooth class, for usbip_device_add_class() */

/** @} */

#endif /* CLASSES_BLUETOOTH_H */
