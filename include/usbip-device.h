/*
 * Copyright (C) 2026 Jabez Winston
 *
 * SPDX-License-Identifier: MIT
 *
 * Date : 04-June-2026
 */
/**
 * @file usbip-device.h
 * @ingroup device
 * @brief Device core API - descriptors, standard requests, endpoints, dispatch.
 *
 * Build a virtual device from typed descriptors and serve it over USB/IP. The
 * core knows only generic USB: the config blob it assembles, the standard
 * requests it answers, and the per-interface/per-endpoint callbacks it routes
 * to. Reusable device classes (functions, interfaces, the ::usbip_device_class
 * vtable) are a separate layer on top - see classes/usb_class.h. I/O is
 * blocking; the library serves each attached host from its own thread.
 *
 * Conventions: functions return ::USB_SUCCESS (0) on success and a negative
 * @ref errors "USB_ERROR code" on failure; constructors return a pointer or `NULL`.
 * The prefix names the receiver of the first parameter; `usbip_device_*` covers
 * the device itself plus the device-side data path
 * (usbip_device_read()/usbip_device_write()), mirroring the host-side `usbip_host_*`.
 */
#ifndef USBIP_DEVICE_H
#define USBIP_DEVICE_H

#include "usbip.h"

/**
 * @addtogroup device
 * @{
 */

typedef struct usbip_device     usbip_device;     /**< The virtual device                       */
typedef struct usbip_ep         usbip_ep;         /**< An endpoint (byte pipe)                   */

/* ---- build ------------------------------------------------------------- */
/**
 * Create a virtual device with the given vendor/product IDs.
 * @param vid  idVendor.
 * @param pid  idProduct.
 * @return the new device, or `NULL` on allocation failure. Destroy it implicitly
 *         via usbip_device_unplug() / process exit.
 */
usbip_device *usbip_device_create(uint16_t vid, uint16_t pid);

/**
 * Set the manufacturer / product / serial string descriptors (indices 1-3).
 * @param dev      the device.
 * @param mfr      iManufacturer string (may be `NULL`).
 * @param product  iProduct string (may be `NULL`).
 * @param serial   iSerialNumber string (may be `NULL`).
 */
void        usbip_device_set_strings(usbip_device *dev, const char *mfr,
                             const char *product, const char *serial);
/**
 * Set the device-descriptor class triple.
 *
 * Default ::USB_CLASS_PER_INTERFACE (0/0/0) means "class defined per interface".
 * Multi-interface devices (e.g. UVC) use ::USB_CLASS_MISC / ::USB_SUBCLASS_COMMON /
 * ::USB_PROTOCOL_IAD (0xEF/0x02/0x01) so the host honours the Interface
 * Association Descriptor.
 * @param dev    the device.
 * @param cls    bDeviceClass.
 * @param sub    bDeviceSubClass.
 * @param proto  bDeviceProtocol.
 */
void        usbip_device_set_class(usbip_device *dev, uint8_t cls, uint8_t sub, uint8_t proto);

/**
 * Set the device release number (bcdDevice). Default 0x0100.
 *
 * Some hosts bind a driver only on an exact match (e.g. Windows + a CSR Bluetooth
 * radio's 0x8891/0x0c5c).
 * @param dev  the device.
 * @param bcd  bcdDevice (BCD, e.g. 0x0100 = "1.00").
 */
void        usbip_device_set_bcd_device(usbip_device *dev, uint16_t bcd);

/**
 * Report a link speed (default ::USB_SPEED_FULL).
 *
 * USB/IP is URB-level, so "high speed" is just the reported speed + 512-byte bulk
 * endpoints - no companion descriptors. Call BEFORE adding interfaces so
 * speed-aware classes (e.g. CDC-ACM) size their bulk endpoints accordingly.
 * @param dev    the device.
 * @param speed  the reported ::usb_speed.
 */
void        usbip_device_set_speed(usbip_device *dev, usb_speed speed);

/**
 * The device's reported link speed (for speed-aware class code; a class reaches
 * its device with usbip_function_device() from classes/usb_class.h).
 * @param dev  the device.
 * @return the reported ::usb_speed.
 */
usb_speed   usbip_device_get_speed(usbip_device *dev);

/**
 * Register an extra string descriptor (e.g. an interface name).
 *
 * Indices 1-3 are reserved for the manufacturer/product/serial set by
 * usbip_device_set_strings().
 * @param dev  the device.
 * @param str  the string to add.
 * @return the new descriptor's index, for use as an iInterface/iProduct/… value.
 */
int         usbip_device_add_string(usbip_device *dev, const char *str);

/**
 * Declare the device composite: several independent class functions on one device.
 *
 * Call **before** adding any class. Sets the device-descriptor triple to
 * 0xEF/0x02/0x01 (Miscellaneous / Common Class / Interface Association) and asks
 * every multi-interface class to emit an Interface Association Descriptor grouping
 * its own interfaces.
 *
 * Linux does not need this - `cdc_acm` and friends group their interfaces from the
 * class-specific descriptors (CDC's Union descriptor). Windows does: `usbccgp` splits
 * a composite device into one child devnode per *function*, and without an IAD it
 * splits per *interface*, handing CDC's data interface to a separate devnode from its
 * communications interface, so the COM port never forms.
 *
 * A single-function device must NOT set this - it would advertise an association that
 * describes the whole device.
 *
 * The triple is pinned from here on: a class that sets its own device triple
 * (e.g. Bluetooth's 0xE0/0x01/0x01) is ignored with a diagnostic, because
 * overwriting 0xEF/0x02/0x01 would silently unmake the composite.
 * @param dev  the device.
 */
void        usbip_device_set_composite(usbip_device *dev);

/** The DeviceInterfaceGUID advertised when enable_winusb() is passed `NULL`. */
#define USBIP_WINUSB_DEFAULT_GUID "{D2A45C1E-7B3F-4A6E-9C2D-1E5F8B0A3C7D}"

/**
 * Advertise WinUSB via BOTH Microsoft OS 1.0 and Microsoft OS 2.0 descriptors.
 *
 * Windows then auto-installs the WinUSB driver, so libusb apps like dfu-util work
 * without Zadig. Bumps bcdUSB to 0x0210 so the host fetches the BOS.
 * @param dev   the device.
 * @param guid  DeviceInterfaceGUID to advertise, or `NULL` for
 *              #USBIP_WINUSB_DEFAULT_GUID.
 * @retval USB_SUCCESS      recorded.
 * @retval USB_ERROR_NO_MEM the advertisement table is full or memory ran out.
 */
int         usbip_device_enable_winusb(usbip_device *dev, const char *guid);

/**
 * Generalized Microsoft OS descriptor advertisement with any Compatible ID.
 *
 * Use "WINUSB" (libusb/dfu-util) or "MTP" (Media Transfer Protocol).
 *
 * This form describes the **whole device**, which is what a single-function device
 * wants. On a composite, scope the advertisement to one function instead - see
 * usbip_function_enable_msos().
 * @param dev         the device.
 * @param compatible  the Compatible ID string (e.g. "WINUSB", "MTP").
 * @param guid        DeviceInterfaceGUID to add, or `NULL` to omit it (MTP needs none).
 * @retval USB_SUCCESS      recorded.
 * @retval USB_ERROR_NO_MEM the advertisement table is full or memory ran out.
 */
int         usbip_device_enable_msos(usbip_device *dev, const char *compatible, const char *guid);

/**
 * Advertise WebUSB so a capable browser can surface and open the device.
 *
 * Adds a WebUSB platform-capability descriptor to the BOS and answers the
 * bVendorCode/GET_URL (wIndex 0x02) vendor request with @p url. Bumps bcdUSB to
 * 0x0210 (BOS present). Pair with usbip_device_enable_winusb() so the device also binds
 * WinUSB on Windows.
 * @param dev          the device.
 * @param vendor_code  the bVendorCode for the WebUSB request; pick a value distinct
 *                     from WinUSB's 0x20/0x21 if both are enabled.
 * @param url          the landing-page URL.
 */
void        usbip_device_enable_webusb(usbip_device *dev, uint8_t vendor_code, const char *url);

/**
 * Pace isochronous completions to real wall-clock time.
 *
 * USB/IP has no SOF clock, so by default iso transfers complete instantly and the
 * host's audio/video engine free-runs (a UAC speaker plays many times too fast).
 * When enabled, each iso transfer is held until the time its packet schedule would
 * really take. Pacing blocks the per-connection serve thread, so it best suits a
 * single iso stream (e.g. an audio speaker). Off by default.
 * @param dev      the device.
 * @param enabled  non-zero to enable pacing, 0 to disable.
 */
void        usbip_device_set_iso_pacing(usbip_device *dev, int enabled);

/* ---- descriptor build: the device-level config blob --------------------- */
/**
 * Append a typed descriptor (interface / endpoint / HID / class-specific) to the
 * device's configuration, in wire order.
 *
 * The core parses two standard types as it appends: an **interface** descriptor
 * registers its bInterfaceNumber and becomes the append cursor; an **endpoint**
 * descriptor gets its number allocated (the requested address is a preference -
 * see usbip_device_add_endpoint()) and bumps `bNumEndpoints` in the owning
 * interface descriptor, which should therefore be left 0.
 *
 * The descriptor's bytes are appended as they are, so its 16-bit fields have to be
 * little-endian already. Declaring it ::USB_PACKED is what arranges that - assign
 * the fields in host order and the layout takes care of itself, on any host. A
 * descriptor built as a raw byte array wants ::USB_U16LE instead.
 * @param dev         the device.
 * @param descriptor  pointer to a ::USB_PACKED descriptor whose first byte is its length.
 * @retval USB_SUCCESS      appended.
 * @retval USB_ERROR_NO_MEM the configuration blob is full, or (for an endpoint
 *                          descriptor) no endpoint number was free in its direction.
 */
int       usbip_device_add_descriptor(usbip_device *dev, const void *descriptor);

/**
 * Append an endpoint descriptor and return the pipe it created.
 *
 * Prefer this over usbip_device_add_descriptor() for endpoints: `bEndpointAddress`
 * is only a *preference*. It is honoured when the number is free, but if another
 * interface already holds it the endpoint is relocated to the lowest free number in
 * the same direction. Pass a zero endpoint number (`0x00` OUT / `0x80` IN) to say
 * "any". Because the address may change, keep the returned pipe rather than looking
 * it up again by the address you asked for.
 * @param dev           the device.
 * @param ep_descriptor a packed ::usb_endpoint_descriptor.
 * @return the endpoint, or `NULL` if it could not be added.
 */
usbip_ep *usbip_device_add_endpoint(usbip_device *dev, const void *ep_descriptor);

/**
 * Claim an endpoint address now, for an endpoint declared later.
 *
 * Needed when a class-specific descriptor has to name an endpoint that has not been
 * declared yet (UVC's VideoStreaming input header). The claim belongs to the
 * current interface (the last interface descriptor appended), so its alternate
 * settings can all use it; the allocator hands the same number back when the real
 * endpoint descriptor arrives.
 * @param dev        the device.
 * @param want_addr  the preferred address (direction bit included), e.g. 0x81.
 * @return the reserved address, or 0 if no number was free in that direction - or
 *         if no interface has been appended yet to own the claim.
 */
uint8_t   usbip_device_reserve_endpoint(usbip_device *dev, uint8_t want_addr);

/**
 * How many interface numbers the device has registered so far - i.e. the next free
 * bInterfaceNumber, and the `bNumInterfaces` the host will see.
 * @param dev  the device.
 * @return highest bInterfaceNumber + 1, or 0 when no interface exists yet.
 */
int       usbip_device_get_num_interfaces(usbip_device *dev);

/**
 * Patch `iInterface` in the (ifnum, alt) interface descriptor already appended.
 * Does nothing if no such interface descriptor exists.
 * @param dev    the device.
 * @param ifnum  the bInterfaceNumber.
 * @param alt    the bAlternateSetting.
 * @param istr   string descriptor index (from usbip_device_add_string()).
 */
void      usbip_device_set_interface_string(usbip_device *dev, uint8_t ifnum,
                                            uint8_t alt, uint8_t istr);

/* ---- request routing: per-interface callbacks --------------------------- */
/**
 * Class/vendor control handler (core form).
 *
 * For an IN request, fill @p buf with up to @p len bytes and return the count; for
 * an OUT request, @p buf holds @p len bytes and you return >= 0; return a negative
 * value to STALL. Standard requests are handled by the core before this is called.
 * @param ctx      the context registered with usbip_device_on_control().
 * @param setup    the 8-byte SETUP packet.
 * @param buf      data buffer (IN: to fill; OUT: incoming data).
 * @param len      IN: capacity of @p buf; OUT: number of bytes in @p buf.
 * @return bytes produced (IN) / consumed-OK (OUT, >= 0), or < 0 to STALL.
 */
typedef int (*usbip_device_control_fn)(void *ctx, const usb_setup *setup,
                                       uint8_t *buf, uint16_t len);
/**
 * Register the class/vendor control handler for one interface.
 *
 * Interface-recipient requests are routed to their bInterfaceNumber's handler.
 * @p ifnum -1 registers the **device-level fallback**: it receives every
 * non-interface-recipient request, and interface-recipient requests whose
 * bInterfaceNumber has no handler of its own.
 * @param dev    the device.
 * @param ifnum  the bInterfaceNumber (0-15), or -1 for the device-level fallback.
 * @param cb     the handler, or `NULL` to remove it.
 * @param ctx    passed back to @p cb.
 */
void usbip_device_on_control(usbip_device *dev, int ifnum,
                             usbip_device_control_fn cb, void *ctx);

/**
 * SET_INTERFACE handler: the host selected alternate setting @p alt on @p ifnum.
 * @param ctx    the context registered with usbip_device_on_set_alt().
 * @param ifnum  the interface whose alternate setting changed.
 * @param alt    the newly selected alternate setting.
 * @return ignored.
 */
typedef int (*usbip_device_set_alt_fn)(void *ctx, int ifnum, int alt);
/**
 * Register the SET_INTERFACE handler for one interface.
 * @param dev    the device.
 * @param ifnum  the bInterfaceNumber (0-15).
 * @param cb     the handler, or `NULL` to remove it.
 * @param ctx    passed back to @p cb.
 */
void usbip_device_on_set_alt(usbip_device *dev, int ifnum,
                             usbip_device_set_alt_fn cb, void *ctx);

/* ---- per-endpoint data callbacks ---------------------------------------- */
/**
 * A bulk/interrupt OUT packet arrived from the host.
 * @param ctx    the context registered with usbip_ep_on_out().
 * @param ep     the OUT endpoint it arrived on.
 * @param data   the received bytes.
 * @param len    number of bytes in @p data.
 */
typedef void (*usbip_ep_out_fn)(void *ctx, usbip_ep *ep, const void *data, int len);
/**
 * Register the OUT-data callback for an endpoint. Without one, OUT data is queued
 * for usbip_device_read().
 * @param ep   the OUT endpoint.
 * @param cb   the callback, or `NULL` to go back to queueing.
 * @param ctx  passed back to @p cb.
 */
void usbip_ep_on_out(usbip_ep *ep, usbip_ep_out_fn cb, void *ctx);

/**
 * The isochronous data callback: one call handles one transfer's packets.
 *
 * @p buf and @p lens always describe the same layout: @p npkts packets laid
 * back-to-back in @p buf, packet @c i being @c lens[i] bytes (no padding between
 * packets). The endpoint's direction decides who fills them in:
 *
 * - **OUT endpoint** - the host sent data. The packets arrive filled in; read
 *   them. The buffer is discarded on return.
 * - **IN endpoint** - the host wants data. On entry @c lens[i] holds the size
 *   the host asked for, and @p buf has room for all of it. Write each packet's
 *   data and set @c lens[i] to the bytes you wrote (anything from 0, an empty
 *   packet, up to the requested size).
 *
 * An endpoint's direction never changes, so a given callback only ever sees one
 * of the two cases.
 * @param ctx    the context registered with usbip_ep_on_iso().
 * @param ep     the isochronous endpoint.
 * @param npkts  number of packets.
 * @param lens   the per-packet sizes (see above).
 * @param buf    the packet bytes, back-to-back (see above).
 * @return ::USB_SUCCESS (0) on success.
 */
typedef int (*usbip_ep_iso_fn)(void *ctx, usbip_ep *ep, int npkts,
                               uint32_t *lens, uint8_t *buf);
/**
 * Register the isochronous data callback for an endpoint (either direction).
 * Without one, iso IN transfers complete with zero-length packets and iso OUT
 * data is dropped (the transfers still complete normally).
 * @param ep   the isochronous endpoint.
 * @param cb   the callback, or `NULL` to remove it.
 * @param ctx  passed back to @p cb.
 */
void usbip_ep_on_iso(usbip_ep *ep, usbip_ep_iso_fn cb, void *ctx);

/* ---- descriptor groups --------------------------------------------------- */
/**
 * Open a new **descriptor group**: a run of descriptors that form one function of
 * the device (what an IAD groups, and what one Microsoft OS advertisement covers).
 * Everything appended until the next usbip_device_group_begin() belongs to it.
 * Groups exist for descriptor bookkeeping only - the Microsoft OS emitters fan out
 * one section per group, and the plug-time IAD diagnostic warns per group. A simple
 * device need not create any: it is then treated as one implicit group.
 * @param dev  the device.
 * @return the group id (>= 0), or a negative @ref errors "USB_ERROR code" when the
 *         group table (8) is full.
 */
int  usbip_device_group_begin(usbip_device *dev);

/**
 * Roll back everything appended since the last usbip_device_group_begin():
 * descriptors, endpoints, interface registrations, reservations, control/set-alt
 * registrations, and the group's Microsoft OS advertisements. For unwinding a failed
 * build before the device is plugged; never call it on a served device.
 * @param dev  the device.
 */
void usbip_device_group_abort(usbip_device *dev);

/**
 * Advertise Microsoft OS descriptors scoped to one descriptor group.
 *
 * On a composite, Windows loads `usbccgp` and gives each function its own devnode;
 * a device-wide Compatible ID cannot express "only this function is WinUSB". This
 * form names the group, so Microsoft OS 1.0 emits a section with the right
 * `bFirstInterfaceNumber` and Microsoft OS 2.0 wraps the feature in a Function
 * Subset.
 * Interface numbers are resolved when the host asks, so it may be called before
 * the group's interfaces exist. Bumps bcdUSB to 0x0210 (BOS present).
 * @param dev         the device.
 * @param group       the group id from usbip_device_group_begin(), or -1 for the
 *                    whole device (what usbip_device_enable_msos() passes).
 * @param compatible  the Compatible ID string (e.g. "WINUSB", "MTP").
 * @param guid        DeviceInterfaceGUID to add, or `NULL` to omit it.
 * @retval USB_SUCCESS      recorded.
 * @retval USB_ERROR_NO_MEM the advertisement table is full or memory ran out.
 */
int  usbip_device_enable_msos_group(usbip_device *dev, int group,
                                    const char *compatible, const char *guid);

/**
 * An endpoint's `bEndpointAddress`, as it appears in the descriptor the host reads.
 *
 * Worth asking for after the fact: the allocator may have moved the endpoint off
 * the address that was requested (see usbip_function_add_endpoint()).
 * @param ep  the endpoint.
 * @return the address (direction bit included), or 0 if @p ep is `NULL`.
 */
uint8_t   usbip_endpoint_address(usbip_ep *ep);

/* ---- data: flat + symmetric to libusb, returns bytes or negative ------- */
/**
 * Read host-to-device (OUT) data from an endpoint, blocking up to @p timeout_ms.
 * @param ep          the OUT endpoint (from usbip_function_endpoint()).
 * @param buf         buffer to receive data.
 * @param len         buffer capacity in bytes.
 * @param timeout_ms  timeout in milliseconds (0 = wait indefinitely).
 * @return bytes read (>= 0), or a negative @ref errors "USB_ERROR code"
 *         (::USB_ERROR_NO_DEVICE if @p ep is `NULL`, ::USB_ERROR_TIMEOUT on timeout).
 */
int  usbip_device_read (usbip_ep *ep, void *buf, int len, unsigned timeout_ms);       /* host->dev */
/**
 * Write device-to-host (IN) data to an endpoint, blocking up to @p timeout_ms.
 * @param ep          the IN endpoint (from usbip_function_endpoint()).
 * @param buf         data to send.
 * @param len         number of bytes to send.
 * @param timeout_ms  timeout in milliseconds (0 = wait indefinitely).
 * @return bytes written (>= 0), or a negative @ref errors "USB_ERROR code".
 */
int  usbip_device_write(usbip_ep *ep, const void *buf, int len, unsigned timeout_ms); /* dev->host */
/* STALL a *control* request by returning a negative value from the
 * ::usbip_function_control_fn handler; non-control endpoints halt instead --
 * see usbip_ep_stall(). */

/* ---- endpoint halt ----------------------------------------------------- */
/**
 * Halt (STALL) a non-control endpoint.
 *
 * Every URB on it is completed with a STALL until the host issues
 * `CLEAR_FEATURE(ENDPOINT_HALT)`, which the core answers itself. Any IN URB the
 * host already has outstanding is STALLed immediately, so this is how a class
 * says "the data you asked for is not coming" without leaving the host to sit
 * out its own timeout.
 *
 * Data already queued with usbip_device_write() survives the halt and is
 * delivered once the host clears it. Mass storage relies on that: it halts the
 * bulk-IN endpoint to abandon a failed data phase, then queues the CSW.
 * @param ep  the endpoint (ignored if `NULL`).
 */
void usbip_ep_stall(usbip_ep *ep);

/* ---- lifecycle --------------------------------------------------------- */
/**
 * Plug the device onto a transport and start serving it.
 *
 * This side is the USB/IP **server**: the call starts *listening* (on TCP 3240 by
 * default) and returns immediately. Nothing enumerates the device until a USB/IP
 * client - the OS importer (`usbip attach`), or a program on the
 * @ref host "host API" - connects and imports it. Data written before that is queued
 * on its endpoint.
 *
 * **Several devices, one process.** Plugging another device onto the same address
 * exports it alongside the first rather than failing to bind: the listener is shared,
 * and each device is named on the wire by its own busid - `1-1`, `1-2`, ... in plug
 * order, unless usbip_device_set_busid() named it. The importer picks one
 * (`usbip attach -b 1-2`, or by busid through the host API), and every imported
 * device gets its own connection. This is separate from a *composite* device
 * (usbip_device_set_composite()), which is one device with several functions.
 *
 * @param dev        the device.
 * @param transport  transport from usbip_transport() / usbip_loopback(), or `NULL` for the
 *                   local default.
 * @retval USB_SUCCESS  serving started (e.g. the port is now accepting imports).
 * @return a negative @ref errors "USB_ERROR code" otherwise (e.g. the port is busy).
 */
int  usbip_device_plug  (usbip_device *dev, usb_transport *transport /* NULL = local */);
/**
 * Stop serving a device previously passed to usbip_device_plug().
 *
 * When other devices still share the listener it keeps running for them; the last
 * device unplugged closes it.
 * @param dev  the device.
 */
void usbip_device_unplug(usbip_device *dev);

/**
 * Name this device on the wire, instead of the `1-<n>` assigned at plug time.
 *
 * Only useful when a process exports several devices and the importer needs stable
 * names regardless of plug order. Call before usbip_device_plug().
 * @param dev    the device.
 * @param busid  the busid, e.g. `"1-4"`. Truncated to 31 characters.
 */
void        usbip_device_set_busid(usbip_device *dev, const char *busid);

/**
 * The busid this device is exported under - what `usbip attach -b` takes.
 *
 * Assigned by usbip_device_plug() unless usbip_device_set_busid() set it; reads
 * `"1-1"` before either.
 * @param dev  the device.
 * @return the busid, owned by the device.
 */
const char *usbip_device_get_busid(usbip_device *dev);

/**
 * Whether the app declared this device composite (see usbip_device_set_composite()).
 *
 * A multi-interface class consults this from its `build()` to decide whether to emit
 * an IAD grouping its interfaces.
 * @param dev  the device.
 * @return non-zero when composite.
 */
int             usbip_device_is_composite(usbip_device *dev);

/** @} */

#endif /* USBIP_DEVICE_H */
