/*
 * Copyright (C) 2026 Jabez Winston
 *
 * SPDX-License-Identifier: MIT
 *
 * Date : 05-June-2026
 */
/**
 * @file classes/hid.h
 * @ingroup class_hid
 * @brief USB HID (Human Interface Device) device class, HID 1.11.
 *
 * A *generic* HID interface: hand it any Report descriptor and it presents a
 * proper HID device - keyboard, mouse, consumer control, or a vendor-defined raw
 * device. It answers the full HID 1.11 request set on EP0 (GET/SET_REPORT,
 * GET/SET_IDLE, GET/SET_PROTOCOL) plus the required interrupt IN and an optional
 * interrupt OUT for Output reports (HID 1.11 Sec.4.4, Sec.7.2, Appendix G).
 *
 * Built only on the public API (usbip_device.h). Mirrors python classes/device/hid.py.
 */
#ifndef CLASSES_HID_H
#define CLASSES_HID_H

#include "usb_class.h"

/**
 * @addtogroup class_hid
 * @{
 */

/**
 * One generic HID interface: a single instance of the class on a device.
 *
 * Create one with hid_add(); it stays valid for the life of the device. The fields
 * are maintained by the class and are **read-only** to the app - they are here so a
 * callback can tell which interface fired and how it is configured without keeping
 * its own table. `protocol` and `idle` track what the host last set.
 *
 * Distinct from the other classes' handles, so mixing them up is a compile error
 * rather than a misread state block. A device may carry several: call hid_add()
 * once each.
 */
typedef struct hid_iface {
    int             index;            /**< 0-based HID instance on this device, in
                                       *   hid_add() order                          */
    const char     *name;             /**< ::hid_opts::name, or `""` (never NULL)    */
    void           *user;             /**< ::hid_opts::user, passed straight through */
    int             interface_number; /**< This interface's bInterfaceNumber         */
    uint8_t         in_ep;            /**< Interrupt IN address actually assigned (a
                                       *   second HID interface is relocated off the
                                       *   default 0x81)                             */
    uint8_t         out_ep;           /**< Interrupt OUT address assigned, 0 if none  */
    uint8_t         protocol;         /**< ::HID_REPORT_PROTOCOL/::HID_BOOT_PROTOCOL  */
    uint8_t         idle;             /**< Current idle duration, in 4 ms units       */
    usbip_device   *dev;              /**< The owning device                          */
    usbip_function *func;             /**< The underlying function - escape hatch to
                                       *   the core usbip_device.h API                */
} hid_iface;

/** Report types (high byte of wValue in Get/Set_Report), HID 1.11 Sec.7.2.1. */
#define HID_REPORT_INPUT   1  /**< Input report (device -> host) */
#define HID_REPORT_OUTPUT  2  /**< Output report (host -> device) */
#define HID_REPORT_FEATURE 3  /**< Feature report (either direction, control only) */

/** ::hid_opts::subclass - bInterfaceSubClass, HID 1.11 Sec.4.2. */
#define HID_SUBCLASS_NONE 0  /**< No subclass: a plain HID interface */
#define HID_SUBCLASS_BOOT 1  /**< Boot interface: the fixed report layout a BIOS parses */

/** ::hid_opts::protocol - bInterfaceProtocol, HID 1.11 Sec.4.3. Only meaningful with
 *  ::HID_SUBCLASS_BOOT; a report-protocol-only interface leaves it ::HID_PROTOCOL_NONE. */
#define HID_PROTOCOL_NONE     0  /**< Neither of the two boot devices */
#define HID_PROTOCOL_KEYBOARD 1  /**< Boot keyboard: 8-byte Input report */
#define HID_PROTOCOL_MOUSE    2  /**< Boot mouse: 3-byte Input report */

/** ::hid_iface::protocol - Get/Set_Protocol values, HID 1.11 Sec.7.2.5. Which report
 *  layout the host has selected, distinct from the bInterfaceProtocol above. */
#define HID_BOOT_PROTOCOL   0  /**< Host wants the boot layout */
#define HID_REPORT_PROTOCOL 1  /**< Host wants the Report descriptor's layout (default) */

/**
 * @name Report-descriptor item helpers (HID 1.11 Sec.6.2.2 short items)
 * Each macro expands to one short item (prefix byte + data byte(s)), so a Report
 * descriptor reads like the spec when written as a `static const uint8_t[]`:
 * @code
 *   static const uint8_t mouse[] = {
 *       HID_USAGE_PAGE(0x01), HID_USAGE(0x02),
 *       HID_COLLECTION(0x01),
 *       ... 
 *       HID_END_COLLECTION
 *   };
 * @endcode
 *
 * The three item classes behave differently, and mixing them up is the usual reason
 * a descriptor parses but describes the wrong device:
 * - **Global** (Usage Page, Logical Min/Max, Report Size/Count/ID) stays in force for
 *   every item that follows, until changed again.
 * - **Local** (Usage, Usage Min/Max) applies only to the *next* Main item, then is
 *   discarded.
 * - **Main** (Input, Output, Feature, Collection) is what actually declares fields in
 *   a report, out of the Global and Local state standing at that point.
 * @{
 */

/** Global: Usage Page in force for the Usages that follow - 1-byte page id, e.g.
 *  0x01 Generic Desktop, 0x07 Keyboard/Keypad, 0x08 LED, 0x0C Consumer (Sec.6.2.2.7). */
#define HID_USAGE_PAGE(x)     0x05, (uint8_t)(x)
/** Global: ::HID_USAGE_PAGE with a 2-byte page id, for pages above 0xFF - notably the
 *  vendor-defined 0xFF00..0xFFFF range used by raw devices. */
#define HID_USAGE_PAGE16(x)   0x06, USB_U16LE(x)
/** Local: Usage for the next Main item, within the current Usage Page - e.g. 0x02
 *  Mouse, 0x06 Keyboard, 0x30 X, 0x31 Y (Sec.6.2.2.8). */
#define HID_USAGE(x)          0x09, (uint8_t)(x)
/** Local: first Usage of a consecutive run applied to the next Main item. Pairs with
 *  ::HID_USAGE_MAX. */
#define HID_USAGE_MIN(x)      0x19, (uint8_t)(x)
/** Local: last Usage of that run - `HID_USAGE_MIN(0xE0), HID_USAGE_MAX(0xE7)` hands the
 *  eight keyboard modifier Usages to the eight fields of the next Main item, in order. */
#define HID_USAGE_MAX(x)      0x29, (uint8_t)(x)
/** Global: smallest value a field may report, as one **signed** byte (-128..127). Sets
 *  how the host scales and interprets the raw bits (Sec.6.2.2.7). */
#define HID_LOGICAL_MIN(x)    0x15, (uint8_t)(x)
/** Global: largest value a field may report, as one **signed** byte - so it tops out at
 *  127. Use ::HID_LOGICAL_MAX16 for anything above that. */
#define HID_LOGICAL_MAX(x)    0x25, (uint8_t)(x)
/** Global: ::HID_LOGICAL_MAX with 2 little-endian data bytes, for ranges a signed byte
 *  cannot hold - `HID_LOGICAL_MAX16(255)` for a byte-wide raw field, or a digitiser's
 *  absolute coordinate range. */
#define HID_LOGICAL_MAX16(x)  0x26, USB_U16LE(x)
/** Global: width of one field, in **bits** (not bytes). */
#define HID_REPORT_SIZE(x)    0x75, (uint8_t)(x)
/** Global: how many fields of that width the next Main item declares. Size x Count is
 *  the item's total bit width; a report must end on a byte boundary, so pad the
 *  remainder with a constant `HID_INPUT(0x01)`. */
#define HID_REPORT_COUNT(x)   0x95, (uint8_t)(x)
/** Global: start a numbered report. From here on every report on this interface carries
 *  this 1-byte ID as its first byte, in **both** directions and on EP0 as well - so
 *  hid_send_report() must send it too. Omit the item entirely when the interface has a
 *  single report. */
#define HID_REPORT_ID(x)      0x85, (uint8_t)(x)
/** Main: open a collection - 0x00 Physical, 0x01 Application, 0x02 Logical (Sec.6.2.2.6).
 *  Close it with ::HID_END_COLLECTION. Each top-level thing the device is must be an
 *  Application collection. */
#define HID_COLLECTION(x)     0xA1, (uint8_t)(x)
/** Main: close the innermost open collection. The one item here that takes no data. */
#define HID_END_COLLECTION    0xC0
/** Main: declare device -> host fields, sent on the interrupt IN pipe. @p x is the data
 *  bitmap of Sec.6.2.2.5 - 0x02 Data,Variable,Absolute (buttons, LEDs, levels), 0x06
 *  Data,Variable,Relative (mouse deltas), 0x00 Data,Array,Absolute (keycode slots),
 *  0x01 Constant (padding to a byte boundary). */
#define HID_INPUT(x)          0x81, (uint8_t)(x)
/** Main: declare host -> device fields, taken from the interrupt OUT pipe or a
 *  SET_REPORT(Output) - keyboard LEDs are the classic case. Same @p x bitmap as
 *  ::HID_INPUT. */
#define HID_OUTPUT(x)         0x91, (uint8_t)(x)
/** Main: declare fields the host reads and writes over EP0 only, with
 *  GET_REPORT/SET_REPORT(Feature) - configuration and state rather than live data.
 *  Same @p x bitmap as ::HID_INPUT. */
#define HID_FEATURE(x)        0xB1, (uint8_t)(x)
/** @} */

/** The 9-byte HID descriptor that points at the Report descriptor (Sec.6.2.1). */
typedef struct USB_PACKED {
    uint8_t  bLength, bDescriptorType;
    uint16_t bcdHID;
    uint8_t  bCountryCode, bNumDescriptors, bReportType;
    uint16_t wReportLength;
} usb_hid_descriptor;

/** Per-instance configuration for a generic HID interface. Zero-fields take the
 *  documented defaults. Any callback may be NULL. */
typedef struct {
    const uint8_t *report_desc;     /**< The Report descriptor bytes (required)        */
    uint16_t       report_desc_len; /**< Length of report_desc                         */
    uint8_t        subclass;        /**< ::HID_SUBCLASS_NONE or ::HID_SUBCLASS_BOOT    */
    uint8_t        protocol;        /**< ::HID_PROTOCOL_NONE/HID_PROTOCOL_KEYBOARD/HID_PROTOCOL_MOUSE (boot)   */
    uint8_t        in_ep;           /**< Interrupt IN address (0 -> 0x81)              */
    uint16_t       in_mps;          /**< 0 -> 64                                       */
    uint8_t        in_interval;     /**< 0 -> 10                                       */
    uint8_t        out_ep;          /**< Interrupt OUT address (0 = none, e.g. 0x01)   */
    uint16_t       out_mps;         /**< 0 -> 64                                       */
    uint8_t        out_interval;    /**< 0 -> 10                                       */
    uint8_t        country;         /**< bCountryCode (usually 0)                      */
    const char    *name;            /**< iInterface string (optional)                 */
    /**
     * Answer a host GET_REPORT (optional).
     * @param iface     the HID interface.
     * @param user  the @c user pointer.
     * @param type  report type - 1 Input, 2 Output, 3 Feature (see ::HID_REPORT_INPUT).
     * @param id    report ID (0 when report IDs are not used).
     * @param buf   buffer to fill with the report (up to @p len bytes).
     * @param len   capacity of @p buf.
     * @return bytes written, or < 0 to fall back to echoing the last Input report sent.
     */
    int  (*get_report)(hid_iface *iface, void *user, uint8_t type, uint8_t id,
                       uint8_t *buf, uint16_t len);
    /**
     * Receive a host SET_REPORT on endpoint 0 (optional). If `NULL` while @c on_output
     * is set, Output reports are delivered to @c on_output instead.
     * @param iface     the HID interface.
     * @param user  the @c user pointer.
     * @param type  report type (1 Input, 2 Output, 3 Feature).
     * @param id    report ID.
     * @param data  the report bytes from the host.
     * @param len   number of bytes in @p data.
     */
    void (*set_report)(hid_iface *iface, void *user, uint8_t type, uint8_t id,
                       const void *data, int len);
    /**
     * Receive an Output report (optional) - from the interrupt-OUT pipe, or via
     * SET_REPORT(Output) when @c set_report is `NULL` (e.g. keyboard LED reports).
     * @param iface     the HID interface.
     * @param user  the @c user pointer.
     * @param data  the Output report bytes.
     * @param len   number of bytes in @p data.
     */
    void (*on_output)(hid_iface *iface, void *user, const void *data, int len);
    void *user;                     /**< Opaque pointer passed to every callback above */
} hid_opts;

/**
 * Add a generic HID interface to a device.
 * @param dev     the device.
 * @param opts  HID configuration - Report descriptor, endpoints and callbacks (see ::hid_opts).
 * @return the HID handle (pass it to hid_send_report()), or `NULL` on error.
 */
hid_iface *hid_add(usbip_device *dev, const hid_opts *opts);

/**
 * Send one Input report to the host on the interrupt-IN pipe (device -> host).
 *
 * The report is also cached as the value returned for a host GET_REPORT(Input).
 * @param iface     the HID interface from hid_add().
 * @param data  the report bytes.
 * @param len   report length in bytes.
 * @return bytes sent (>= 0), or a negative @ref errors "USB_ERROR code".
 */
int hid_send_report(hid_iface *iface, const void *data, int len);


extern const usbip_device_class usbip_device_hid;   /**< The HID class, for usbip_device_add_class() */

/** @} */

#endif /* CLASSES_HID_H */
