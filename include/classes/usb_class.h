/*
 * Copyright (C) 2026 Jabez Winston
 *
 * SPDX-License-Identifier: MIT
 *
 * Date : 29-July-2026
 */
/**
 * @file usb_class.h
 * @ingroup usb_class
 * @brief Class layer - functions, interfaces, and the ::usbip_device_class vtable.
 *
 * Composes a device out of **functions** (one class instance each, owning one or
 * more interfaces), mirroring the Linux gadget composite framework. Built entirely
 * on the public device-core API (usbip_device.h): descriptor groups, per-interface
 * control/set-alt registration, and per-endpoint data callbacks. The core never
 * references this layer, so a device can also be authored against the core alone.
 *
 * Conventions: functions return ::USB_SUCCESS (0) on success and a negative
 * @ref errors "USB_ERROR code" on failure; constructors return a pointer or `NULL`.
 * The prefix names the receiver of the first parameter (`usbip_function_*`,
 * `usbip_interface_*`).
 */
#ifndef USBIP_USB_CLASS_H
#define USBIP_USB_CLASS_H

#include <stdarg.h>

#include "usbip-device.h"

/**
 * @addtogroup usb_class
 * @{
 */

typedef struct usbip_function   usbip_function;   /**< A function: one class instance (>=1 iface) */
typedef struct usbip_interface  usbip_interface;  /**< One USB interface (one bInterfaceNumber)  */

/** Class-specific descriptor type codes - CDC, UAC and UVC all use these two values. */
#define USB_DT_CS_INTERFACE 0x24 /**< class-specific interface descriptor */
#define USB_DT_CS_ENDPOINT  0x25 /**< class-specific endpoint descriptor */

/**
 * Low-level: add a bare function you configure yourself, descriptor by descriptor.
 *
 * Most code uses a ready-made class (e.g. hid_add()) instead. The interface class
 * triple lives in the interface descriptors you append - the function itself
 * carries none.
 * @param dev  the device.
 * @return the new function, or `NULL` when the function table (8) is full or
 *         memory ran out.
 */
usbip_function *usbip_device_add_function(usbip_device *dev);

/**
 * Append a typed descriptor (interface / endpoint / HID / class-specific) to a
 * function, in wire order.
 *
 * `bNumEndpoints` is auto-counted: leave it 0 in interface descriptors - every
 * endpoint appended afterwards bumps the count in its owning alternate setting.
 *
 * The bytes are appended as they are, so declare the descriptor ::USB_PACKED and
 * its little-endian layout is handled for you (::USB_U16LE for a raw byte array) -
 * see usbip_device_add_descriptor().
 * @param func        the function.
 * @param descriptor  pointer to a ::USB_PACKED descriptor whose first byte is its length.
 * @retval USB_SUCCESS      appended.
 * @retval USB_ERROR_NO_MEM the device's descriptor buffer is full, or (for an
 *                          endpoint descriptor) no endpoint number was free in
 *                          its direction.
 */
int     usbip_function_add_descriptor(usbip_function *func, const void *descriptor);

/**
 * Append an endpoint descriptor and return the pipe it created.
 *
 * Prefer this over usbip_function_add_descriptor() for endpoints: `bEndpointAddress` is only a
 * *preference*. It is honoured when the number is free, but if another interface
 * already holds it the endpoint is relocated to the lowest free number in the same
 * direction - so a second instance of a class, or a composite device, gets working
 * pipes instead of a descriptor in which two interfaces claim one address. Pass a
 * zero endpoint number (`0x00` OUT / `0x80` IN) to say "any".
 *
 * Because the address may change, keep the returned pipe rather than looking it up
 * again by the address you asked for.
 * @param func          the function.
 * @param ep_descriptor a packed ::usb_endpoint_descriptor.
 * @return the endpoint, or `NULL` if it could not be added (no free number, or the
 *         device's descriptor buffer is full).
 */
usbip_ep *usbip_function_add_endpoint(usbip_function *func, const void *ep_descriptor);

/**
 * Claim an endpoint address now, for an endpoint declared later.
 *
 * Needed when a **class-specific descriptor has to name an endpoint that has not
 * been declared yet** - UVC's VideoStreaming input header carries a
 * `bEndpointAddress`, but must be emitted before the endpoint descriptor, which
 * lives in a later alternate setting. Writing the requested address into that
 * header would be wrong the moment the allocator relocates the endpoint (see
 * usbip_function_add_endpoint()), leaving the class descriptor pointing at some
 * other function's pipe.
 *
 * Returns the address the endpoint will actually get. Put *that* in the class
 * descriptor, then pass it as the `bEndpointAddress` of the endpoint descriptor
 * when you add it: the reservation makes the allocator hand back the same number.
 * Call it after adding the interface that will own the endpoint - the reservation
 * belongs to that interface, so its alternate settings can all use it.
 *
 * @param func       the function.
 * @param want_addr  the preferred address (direction bit included), e.g. 0x81.
 * @return the reserved address, or 0 if no number was free in that direction -
 *         or if the function has no interface yet to own the claim.
 */
uint8_t   usbip_function_reserve_endpoint(usbip_function *func, uint8_t want_addr);

/**
 * Fetch one of this function's endpoint pipes by address.
 *
 * The lookup is scoped to @p func, so each function in a composite device resolves
 * its own pipes. Note that an endpoint relocated by the allocator (see
 * usbip_function_add_endpoint()) no longer answers to the address originally
 * requested - hold on to the pipe you were given instead.
 * @param func  the function.
 * @param addr  endpoint address (e.g. 0x81 for IN endpoint 1).
 * @return the endpoint, or `NULL` if this function has none with that address.
 */
usbip_ep *usbip_function_endpoint(usbip_function *func, uint8_t addr);

/**
 * Class/vendor control handler.
 *
 * For an IN request, fill @p buf with up to @p len bytes and return the count; for
 * an OUT request, @p buf holds @p len bytes and you return >= 0; return a negative
 * value to STALL. Standard requests are handled automatically by the core.
 * @param func    the function.
 * @param setup   the 8-byte SETUP packet.
 * @param buf     data buffer (IN: to fill; OUT: incoming data).
 * @param len     IN: capacity of @p buf; OUT: number of bytes in @p buf.
 * @return bytes produced (IN) / consumed-OK (OUT, >= 0), or < 0 to STALL.
 */
typedef int (*usbip_function_control_fn)(usbip_function *func, const usb_setup *setup,
                               uint8_t *buf, uint16_t len);
/**
 * Install the class/vendor control handler for an interface.
 * @param func   the function.
 * @param cb     the handler (see ::usbip_function_control_fn), or `NULL` to remove it.
 */
void usbip_function_on_control(usbip_function *func, usbip_function_control_fn cb);

/**
 * Advertise Microsoft OS descriptors for **one function** of a composite device.
 *
 * On a composite, Windows loads `usbccgp` and gives each function its own devnode,
 * each binding its own driver. A device-wide Compatible ID cannot express that:
 * it would tell Windows the whole device is (say) WinUSB, taking the COM port of a
 * neighbouring CDC function down with it. This form names the function, so
 * Microsoft OS 1.0 emits a section with the right `bFirstInterfaceNumber` and
 * Microsoft OS 2.0 wraps the feature in a Function Subset.
 *
 * Call it from the class's `build()` or right after adding the class; the interface
 * numbers are resolved later, when the host asks for the descriptors. Bumps bcdUSB
 * to 0x0210 so the host fetches the BOS.
 *
 * A single-function device may call this too - the emitted bytes are then identical
 * to the device-wide form.
 * @param func        the function to describe.
 * @param compatible  the Compatible ID string (e.g. "WINUSB", "MTP").
 * @param guid        DeviceInterfaceGUID to add, or `NULL` to omit it.
 * @retval USB_SUCCESS            recorded.
 * @retval USB_ERROR_NO_MEM       the advertisement table is full or memory ran out.
 * @retval USB_ERROR_INVALID_PARAM @p func is `NULL`.
 */
int         usbip_function_enable_msos(usbip_function *func, const char *compatible, const char *guid);

/**
 * usbip_function_enable_msos() with the "WINUSB" Compatible ID.
 * @param func  the function to describe.
 * @param guid  DeviceInterfaceGUID to advertise, or `NULL` for a built-in default.
 * @retval USB_SUCCESS            recorded.
 * @retval USB_ERROR_NO_MEM       the advertisement table is full or memory ran out.
 * @retval USB_ERROR_INVALID_PARAM @p func is `NULL`.
 */
int         usbip_function_enable_winusb(usbip_function *func, const char *guid);

/* ---- reusable device classes (the extension mechanism) ----------------- */
/** A reusable **device class**: the descriptors it declares, the callbacks that
 *  drive it, and the size of its per-instance state. Declare one statically, then
 *  instantiate it on a device with usbip_device_add_class() (the built-in classes
 *  also expose a convenience `*_add()` helper).
 *
 *  Every callback receives the ::usbip_function being served - reach its per-instance
 *  state with usbip_function_state() and its device with usbip_function_device().
 *  Only @c build is required; any other callback may be `NULL` (that event is then
 *  simply not handled). */
typedef struct {
    const char *name;            /**< The class's name, e.g. "hid" - used in diagnostics. */
    uint8_t bInterfaceClass;     /**< Default bInterfaceClass for the interface. */
    uint8_t bInterfaceSubClass;  /**< Default bInterfaceSubClass. */
    uint8_t bInterfaceProtocol;  /**< Default bInterfaceProtocol. */

    /**
     * Build the function (**required**). Called once when the function is
     * instantiated - via usbip_device_add_class() or a class's `*_add()` helper:
     * declare the interface/endpoint/class descriptors with
     * usbip_function_add_descriptor() and keep the endpoint pipes it returns.
     * @param func    the function being created.
     * @param params  the class-specific parameters (a `*_add()` helper passes its
     *                opts struct through).
     * @return ::USB_SUCCESS on success, or a negative @ref errors "USB_ERROR code"
     *         to abort instantiation.
     */
    int  (*build)(usbip_function *func, const void *params);

    /**
     * Handle a class/vendor control request on endpoint 0 (optional). The core
     * answers standard requests; only class/vendor ones reach here. Same contract
     * as ::usbip_function_control_fn.
     * @param func    the function.
     * @param setup   the 8-byte SETUP packet.
     * @param buf     data buffer - IN: fill with up to @p len bytes; OUT: holds @p len bytes.
     * @param len     IN: capacity of @p buf; OUT: number of bytes received.
     * @return bytes produced (IN) or consumed-OK (OUT, >= 0), or < 0 to STALL.
     */
    int  (*control)(usbip_function *func, const usb_setup *setup, uint8_t *buf, uint16_t len);

    /**
     * A bulk/interrupt OUT packet arrived from the host (optional).
     * @param func   the function.
     * @param ep     the OUT endpoint it arrived on.
     * @param data   the received bytes.
     * @param len    number of bytes in @p data.
     */
    void (*on_out)(usbip_function *func, usbip_ep *ep, const void *data, int len);

    /**
     * The host issued SET_INTERFACE (optional). Lets an interface that spans several
     * USB interfaces (e.g. audio's two streaming interfaces) tell them apart.
     * @param func   the function.
     * @param ifnum  the interface whose alternate setting changed.
     * @param alt    the newly selected alternate setting.
     * @return ignored.
     */
    int  (*set_alt)(usbip_function *func, int ifnum, int alt);

    /**
     * Isochronous data (optional). Same contract as ::usbip_ep_iso_fn: @p buf
     * holds @p npkts packets back-to-back, packet @c i being @c lens[i] bytes.
     * On an OUT endpoint the packets arrive filled in - read them. On an IN
     * endpoint @c lens[i] arrives as the size the host asked for - fill @p buf
     * and set each @c lens[i] to the bytes you wrote. A function serving both
     * directions (e.g. audio) tells them apart with
     * `usbip_endpoint_address(ep) & 0x80` (set = IN).
     * @param func   the function.
     * @param ep     the isochronous endpoint.
     * @param npkts  number of packets.
     * @param lens   the per-packet sizes.
     * @param buf    the packet bytes, back-to-back.
     * @return ::USB_SUCCESS (0) on success.
     */
    int  (*on_iso)(usbip_function *func, usbip_ep *ep, int npkts,
                   uint32_t *lens, uint8_t *buf);

    /**
     * Release per-instance state on teardown (optional). Called when a failed
     * `build` unwinds; free anything @c build allocated.
     * @param func   the function being destroyed.
     */
    void (*destroy)(usbip_function *func);

    size_t state_size;   /**< Bytes of per-instance state - zero-initialised and
                          *   reachable from any callback via usbip_function_state(). */
} usbip_device_class;

/**
 * Instantiate a reusable device class onto a device, adding one function.
 * @param dev     the device.
 * @param cls     the class to instantiate - a statically-allocated class descriptor,
 *                kept by reference (e.g. `&usbip_device_hid`).
 * @param params  class-specific build parameters, passed to ::usbip_device_class::build.
 * @return the new function, or `NULL` if @p cls is `NULL` or its build failed.
 */
usbip_function *usbip_device_add_class(usbip_device *dev, const usbip_device_class *cls,
                                       const void *params);
/**
 * The function's per-instance state block (sized by ::usbip_device_class::state_size).
 * @param func  the function.
 * @return a pointer to the zero-initialised state, or `NULL` if the class declared none.
 */
void         *usbip_function_state(usbip_function *func);
/**
 * The device a function belongs to - for device-level setup from a class build
 * (e.g. usbip_device_add_string() / usbip_device_enable_winusb()).
 * @param func  the function.
 * @return the owning device.
 */
usbip_device   *usbip_function_device(usbip_function *func);
/**
 * The bInterfaceNumber of this function's **first** interface.
 *
 * Fixed when the function is created, so it reads the same before and after the
 * function's interfaces are added. A multi-interface class uses it to name its own
 * interfaces from inside a descriptor it emits earlier in the block - a CDC Union
 * or a UVC VideoControl header has to state the interface number of a sibling that
 * does not exist yet, and the function's interfaces are numbered consecutively from
 * this base. Once an interface exists, usbip_interface_number() reports it directly.
 * @param func  the function.
 * @return the bInterfaceNumber of the function's first interface.
 */
int           usbip_function_base_ifnum(usbip_function *func);
/**
 * Which instance of its own class this function is, counting from 0 in add order.
 *
 * Lets a class label its instances without a global counter - the second
 * cdc_acm_add() on a device answers 1, and the count restarts per device, so
 * several devices in one process stay independent.
 * @param func  the function.
 * @return the 0-based instance number among same-class functions on this device.
 */
int           usbip_function_instance(usbip_function *func);
/* ---- two-level builder API (function -> interface -> alt setting) ------- *
 * A function owns interfaces (one bInterfaceNumber each), an interface owns alt
 * settings, an alt owns endpoints. The flat usbip_function_add_descriptor() still
 * works and derives the same tree. */

/**
 * Add a USB interface (a fresh bInterfaceNumber, alternate setting 0) to a function.
 * @param func   the function.
 * @param cls    bInterfaceClass.
 * @param sub    bInterfaceSubClass.
 * @param proto  bInterfaceProtocol.
 * @return the new interface.
 */
usbip_interface *usbip_function_add_interface(usbip_function *func, uint8_t cls, uint8_t sub, uint8_t proto);
/**
 * Add an alternate setting to an interface (same bInterfaceNumber, new bAlternateSetting).
 *
 * Subsequent usbip_interface_add_endpoint() / usbip_interface_add_descriptor()
 * calls land in this alternate setting.
 * @param iface  the interface.
 * @param alt    the bAlternateSetting value.
 * @retval USB_SUCCESS      added.
 * @retval USB_ERROR_NO_MEM the device's descriptor buffer is full.
 */
int              usbip_interface_add_altsetting(usbip_interface *iface, uint8_t alt);
/**
 * Append a class-specific descriptor to an interface's current alternate setting.
 * @param iface       the interface.
 * @param descriptor  a packed descriptor whose first byte is its length.
 * @return ::USB_SUCCESS, or a negative @ref errors "USB_ERROR code".
 */
int              usbip_interface_add_descriptor(usbip_interface *iface, const void *descriptor);
/**
 * Append an endpoint to an interface's current alternate setting.
 * @param iface          the interface.
 * @param ep_descriptor  a packed ::usb_endpoint_descriptor.
 * @return the new endpoint pipe, or `NULL` on failure.
 */
usbip_ep        *usbip_interface_add_endpoint(usbip_interface *iface, const void *ep_descriptor);
/**
 * An interface's bInterfaceNumber.
 * @param iface  the interface.
 * @return its bInterfaceNumber.
 */
int              usbip_interface_number(usbip_interface *iface);
/**
 * Set the iInterface string index on the interface's current alternate setting.
 * Does nothing if the interface has no alternate setting yet - call it after
 * usbip_function_add_interface().
 * @param iface  the interface.
 * @param istr   string descriptor index (from usbip_device_add_string()).
 */
void             usbip_interface_set_string(usbip_interface *iface, uint8_t istr);
/**
 * Emit an Interface Association Descriptor grouping this function's interfaces (opt-in).
 *
 * Call before adding the function's interfaces. Needed only for true composite
 * functions (e.g. UVC); most multi-interface classes (CDC, Bluetooth, audio) group
 * their interfaces by class-specific means and must NOT emit an IAD. Pair with
 * usbip_device_set_class(dev, ::USB_CLASS_MISC, ::USB_SUBCLASS_COMMON,
 * ::USB_PROTOCOL_IAD) - or let usbip_device_set_composite() do it for the device.
 * @param func       the function.
 * @param count      bInterfaceCount (number of interfaces in the association).
 * @param cls        bFunctionClass.
 * @param sub        bFunctionSubClass.
 * @param proto      bFunctionProtocol.
 * @param iFunction  iFunction string index (0 for none).
 */
void  usbip_function_associate(usbip_function *func, uint8_t count, uint8_t cls,
                               uint8_t sub, uint8_t proto, uint8_t iFunction);
/**
 * Format a log line and hand it to a class module's on_event-style callback.
 *
 * The one implementation of the "format into a small buffer, emit, or do
 * nothing when no callback is set" idiom every class module needs.
 * @param emit    the module's event callback, or `NULL` (then nothing happens).
 * @param user    the callback's user pointer.
 * @param prefix  text placed before the formatted message, or `NULL` for none.
 * @param fmt     printf format for the message.
 * @param ap      its arguments.
 */
void  usbip_class_vlog(void (*emit)(void *user, const char *text), void *user,
                       const char *prefix, const char *fmt, va_list ap);

/* Each built-in class declares its usbip_device_class object in its own header
 * under classes/ (e.g. ::usbip_device_hid in classes/hid.h). */

/** @} */

#endif /* USBIP_USB_CLASS_H */
