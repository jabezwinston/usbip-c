/*
 * Copyright (C) 2026 Jabez Winston
 *
 * SPDX-License-Identifier: MIT
 *
 * Date : 04-June-2026
 */
/**
 * @file usbip.h
 * @ingroup transport
 * @brief Shared, role-neutral types and the transport seam.
 *
 * Everything both roles need in one header: the USB constants and packed
 * descriptor structs, the unaligned little/big-endian accessors, the `USB_*`
 * error codes, and the ::usb_transport handle under every host/device call.
 * Include it through `usbip-device.h` or `usbip-host.h`.
 *
 * Prefixes: `usb_*` (shared types) · `usbip_host_*` (host side) · `usbip_*` (wire /
 * transport). On the device side the prefix names the receiver of the first
 * parameter - `usbip_device_*`, `usbip_function_*`, `usbip_interface_*`,
 * `usbip_endpoint_*` - with `usbip_device_*` also covering the data path
 * (usbip_device_read()/usbip_device_write()), mirroring `usbip_host_*`.
 */
#ifndef USBIP_H
#define USBIP_H

#include <stdint.h>
#include <stddef.h>
#include <string.h>

/** USBIP release version, as a string literal. Keep in sync with python `usbip.__version__`. */
#define USBIP_VERSION "0.7.0"

/* Numeric parts, for `#if` checks. `make check` keeps them in sync with the string
 * above; the Windows VERSIONINFO resource is built from them. */
/** Major part of ::USBIP_VERSION. */
#define USBIP_VERSION_MAJOR 0
/** Minor part of ::USBIP_VERSION. */
#define USBIP_VERSION_MINOR 7
/** Micro (patch) part of ::USBIP_VERSION. */
#define USBIP_VERSION_MICRO 0

/* Is the host big-endian? Decides which way a swap runs, and whether a descriptor
 * struct needs one at all. */
#if defined(__BYTE_ORDER__) && defined(__ORDER_BIG_ENDIAN__)
#  define USB_HOST_BIG_ENDIAN (__BYTE_ORDER__ == __ORDER_BIG_ENDIAN__)
#elif defined(__BIG_ENDIAN__)
#  define USB_HOST_BIG_ENDIAN 1        /* older GCC/Clang, and Apple's on BE targets */
#elif defined(_MSC_VER)
#  define USB_HOST_BIG_ENDIAN 0        /* every target MSVC supports is little-endian */
#else
#  define USB_HOST_BIG_ENDIAN 0
#endif

/**
 * Give a descriptor struct the wire's layout: no padding, and little-endian
 * storage. Apply it to every @c struct whose bytes go out on the bus.
 *
 * USB descriptors are little-endian by definition, so that is a property of the
 * type rather than something each emitter has to remember. Fields are still read
 * and written in host order - `.wMaxPacketSize = 512`, `s->wValue` - and the
 * compiler swaps on access, so a descriptor can be serialized (and a SETUP packet
 * parsed) with a plain @c memcpy on any host. On a little-endian build nothing is
 * emitted at all; the attribute costs nothing there.
 *
 * The one restriction, and it is a useful one: you cannot take the address of a
 * multi-byte field (@c &desc->wLength). GCC rejects it outright on a big-endian
 * build rather than handing out a pointer to bytes in the wrong order.
 */
#if defined(__GNUC__) && !defined(__clang__) && __GNUC__ >= 6
#  define USB_PACKED __attribute__((packed, scalar_storage_order("little-endian")))
#elif !USB_HOST_BIG_ENDIAN
#  define USB_PACKED __attribute__((packed))   /* host order is already the wire order */
#else
#  error "big-endian build needs GCC 6+ for scalar_storage_order; see USB_PACKED in usbip.h"
#endif

/* ---------------------------------------------------------------------------
 * Byte order - one vocabulary for every multi-byte field on the wire. Nothing
 * outside this section shifts and masks bytes by hand. Pick by where the field lives:
 *
 *   - a struct field  -> declare the struct ::USB_PACKED and assign in host order;
 *   - a byte at an offset in a buffer -> usb_get_le16() / usb_put_be32() and their
 *     16/24/32/64-bit siblings, which take a @c void* so any offset is safe;
 *   - a literal byte array -> ::USB_U16LE and friends, which expand to the bytes
 *     themselves so a whole descriptor reads in wire order.
 *
 * None depend on the host's own byte order; the swap uses the toolchain intrinsic
 * where there is one.
 * ------------------------------------------------------------------------- */

/* ---- byte swap: toolchain intrinsic where available ---- */
#if defined(__GNUC__) || defined(__clang__)
#  define USB_BSWAP16(x) __builtin_bswap16(x)
#  define USB_BSWAP32(x) __builtin_bswap32(x)
#  define USB_BSWAP64(x) __builtin_bswap64(x)
#elif defined(_MSC_VER)
#  include <stdlib.h>
#  define USB_BSWAP16(x) _byteswap_ushort(x)
#  define USB_BSWAP32(x) _byteswap_ulong(x)
#  define USB_BSWAP64(x) _byteswap_uint64(x)
#else
#  define USB_BSWAP16(x) ((uint16_t)(((uint16_t)(x) >> 8) | ((uint16_t)(x) << 8)))
#  define USB_BSWAP32(x) ((uint32_t)(((uint32_t)(x) >> 24) | \
                                     (((uint32_t)(x) >> 8) & 0x0000FF00u) | \
                                     (((uint32_t)(x) << 8) & 0x00FF0000u) | \
                                     ((uint32_t)(x) << 24)))
#  define USB_BSWAP64(x) ((uint64_t)USB_BSWAP32((uint32_t)((x) >> 32)) | \
                          ((uint64_t)USB_BSWAP32((uint32_t)(x)) << 32))
#endif

/* Host order to wire order and back - the same operation either way. Private to
 * the accessors below, and #undef'd once they are built. */
#if USB_HOST_BIG_ENDIAN
#  define USB_CONV_LE16(x) USB_BSWAP16(x)
#  define USB_CONV_LE32(x) USB_BSWAP32(x)
#  define USB_CONV_LE64(x) USB_BSWAP64(x)
#  define USB_CONV_BE16(x) (x)
#  define USB_CONV_BE32(x) (x)
#  define USB_CONV_BE64(x) (x)
#else
#  define USB_CONV_LE16(x) (x)
#  define USB_CONV_LE32(x) (x)
#  define USB_CONV_LE64(x) (x)
#  define USB_CONV_BE16(x) USB_BSWAP16(x)
#  define USB_CONV_BE32(x) USB_BSWAP32(x)
#  define USB_CONV_BE64(x) USB_BSWAP64(x)
#endif

/* memcpy through a local is the portable unaligned access; compilers turn these
 * into one load/store plus a bswap. */
#define USB_BYTEORDER_GET(name, type, conv)                       \
    static inline type name(const void *src) {                    \
        type value;                                               \
        memcpy(&value, src, sizeof(value));                        \
        return conv(value);                                       \
    }
#define USB_BYTEORDER_PUT(name, type, conv)                       \
    static inline void name(void *dst, type value) {              \
        type raw = conv(value);                                   \
        memcpy(dst, &raw, sizeof(raw));                            \
    }

/* Little-endian accessors: USB descriptors, UVC probe blocks, PTP/MTP containers. */
USB_BYTEORDER_GET(usb_get_le16, uint16_t, USB_CONV_LE16)
USB_BYTEORDER_GET(usb_get_le32, uint32_t, USB_CONV_LE32)
USB_BYTEORDER_GET(usb_get_le64, uint64_t, USB_CONV_LE64)
USB_BYTEORDER_PUT(usb_put_le16, uint16_t, USB_CONV_LE16)
USB_BYTEORDER_PUT(usb_put_le32, uint32_t, USB_CONV_LE32)
USB_BYTEORDER_PUT(usb_put_le64, uint64_t, USB_CONV_LE64)

/* Big-endian accessors: SCSI CDBs and parameter data. */
USB_BYTEORDER_GET(usb_get_be16, uint16_t, USB_CONV_BE16)
USB_BYTEORDER_GET(usb_get_be32, uint32_t, USB_CONV_BE32)
USB_BYTEORDER_GET(usb_get_be64, uint64_t, USB_CONV_BE64)
USB_BYTEORDER_PUT(usb_put_be16, uint16_t, USB_CONV_BE16)
USB_BYTEORDER_PUT(usb_put_be32, uint32_t, USB_CONV_BE32)
USB_BYTEORDER_PUT(usb_put_be64, uint64_t, USB_CONV_BE64)

#undef USB_BYTEORDER_GET
#undef USB_BYTEORDER_PUT
#undef USB_CONV_LE16
#undef USB_CONV_LE32
#undef USB_CONV_LE64
#undef USB_CONV_BE16
#undef USB_CONV_BE32
#undef USB_CONV_BE64

/* 24-bit fields have no C type to memcpy, so spell the three bytes out. A UAC
 * sample rate (LE) and a SCSI block length (BE); carried in a uint32_t, top byte zero. */
static inline uint32_t usb_get_le24(const void *src)
{
    const uint8_t *bytes = (const uint8_t *)src;
    return (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8) | ((uint32_t)bytes[2] << 16);
}

static inline uint32_t usb_get_be24(const void *src)
{
    const uint8_t *bytes = (const uint8_t *)src;
    return ((uint32_t)bytes[0] << 16) | ((uint32_t)bytes[1] << 8) | (uint32_t)bytes[2];
}

static inline void usb_put_le24(void *dst, uint32_t value)
{
    uint8_t *bytes = (uint8_t *)dst;
    bytes[0] = (uint8_t)value;
    bytes[1] = (uint8_t)(value >> 8);
    bytes[2] = (uint8_t)(value >> 16);
}

static inline void usb_put_be24(void *dst, uint32_t value)
{
    uint8_t *bytes = (uint8_t *)dst;
    bytes[0] = (uint8_t)(value >> 16);
    bytes[1] = (uint8_t)(value >> 8);
    bytes[2] = (uint8_t)value;
}

/**
 * A 16-bit value as its two little-endian bytes, for descriptor initialisers.
 *
 * The expansion is a comma-separated byte pair rather than a single value, so a
 * whole descriptor can be written as one array in wire order:
 * @code
 *   uint8_t d[] = { 18, USB_DT_DEVICE, USB_U16LE(bcdUSB), ... };
 * @endcode
 * Endian-safe by construction - the shifts do not depend on host byte order.
 * The argument is expanded once per byte, so it must not have side effects.
 * @param x  the value to emit.
 */
#define USB_U16LE(x) (uint8_t)((x) & 0xff), (uint8_t)(((x) >> 8) & 0xff)
/** A 24-bit value as its three little-endian bytes (a UAC sample rate, say).
 *  See ::USB_U16LE.
 *  @param x  the value to emit. */
#define USB_U24LE(x) (uint8_t)((x) & 0xff), (uint8_t)(((x) >> 8) & 0xff), \
                     (uint8_t)(((x) >> 16) & 0xff)
/** A 32-bit value as its four little-endian bytes. See ::USB_U16LE.
 *  @param x  the value to emit. */
#define USB_U32LE(x) (uint8_t)((x) & 0xff), (uint8_t)(((x) >> 8) & 0xff), \
                     (uint8_t)(((x) >> 16) & 0xff), (uint8_t)(((x) >> 24) & 0xff)

/** A 16-bit value as its two big-endian bytes, for the class payloads that are
 *  big-endian on the wire (SCSI, CTAPHID, PTP status words). See ::USB_U16LE.
 *  @param x  the value to emit. */
#define USB_U16BE(x) (uint8_t)(((x) >> 8) & 0xff), (uint8_t)((x) & 0xff)
/** A 24-bit value as its three big-endian bytes. See ::USB_U16BE.
 *  @param x  the value to emit. */
#define USB_U24BE(x) (uint8_t)(((x) >> 16) & 0xff), (uint8_t)(((x) >> 8) & 0xff), \
                     (uint8_t)((x) & 0xff)
/** A 32-bit value as its four big-endian bytes. See ::USB_U16BE.
 *  @param x  the value to emit. */
#define USB_U32BE(x) (uint8_t)(((x) >> 24) & 0xff), (uint8_t)(((x) >> 16) & 0xff), \
                     (uint8_t)(((x) >> 8) & 0xff), (uint8_t)((x) & 0xff)

/** The least significant byte of a 16-bit value - the index half of a @c wValue,
 *  an interface number out of a @c wIndex.
 *  @param x  the 16-bit value to take the byte from. */
#define USB_U16_LSB(x) (uint8_t)((x) & 0xff)
/** The most significant byte of a 16-bit value - the type half of a @c wValue, a
 *  control selector, a report type.
 *  @param x  the 16-bit value to take the byte from. */
#define USB_U16_MSB(x) (uint8_t)(((x) >> 8) & 0xff)

/* ---------------------------------------------------------------------------
 * USB constants and enums (shared, role-neutral).
 * ------------------------------------------------------------------------- */

/**
 * @addtogroup desc
 * @{
 */

#define USB_BCD_USB_2_0        0x0200  /**< bcdUSB: USB 2.0 */
#define USB_BCD_DEVICE_DEFAULT 0x0100  /**< bcdDevice: device release 1.00, unless set */

/** @c bEndpointAddress is a 4-bit endpoint number plus a direction bit. */
#define USB_EP_ADDR_NUM_MASK  0x0F   /**< Endpoint number within bEndpointAddress */
#define USB_EP_ADDR_DIR_IN    0x80   /**< Direction bit set = IN (device to host) */

/** Transfer direction (matches bit 7 of an endpoint address / @c bmRequestType). */
typedef enum {
    USB_OUT = 0,    /**< Host → device */
    USB_IN  = 1,    /**< Device → host */
} usb_dir;

/** Endpoint transfer type (the transfer-type field of @c bmAttributes). */
typedef enum {
    USB_CONTROL = 0,    /**< Control - setup/config/commands (endpoint 0) */
    USB_ISO     = 1,    /**< Isochronous - streaming media, timed, may drop data */
    USB_BULK    = 2,    /**< Bulk - large data, reliable, no timing guarantee */
    USB_INTR    = 3,    /**< Interrupt - small periodic data, bounded latency */
} usb_xfer_type;

/** Isochronous synchronisation type - bits 3:2 of @c bmAttributes. */
#define USB_EP_SYNC_NONE      0x00  /**< No synchronisation */
#define USB_EP_SYNC_ASYNC     0x04  /**< Asynchronous - free-running source, the host adapts */
#define USB_EP_SYNC_ADAPTIVE  0x08  /**< Adaptive - the endpoint follows the host's rate */
#define USB_EP_SYNC_SYNC      0x0C  /**< Synchronous - locked to the bus SOF clock */

/** Isochronous usage type - bits 5:4 of @c bmAttributes. */
#define USB_EP_USAGE_DATA     0x00  /**< Data endpoint */
#define USB_EP_USAGE_FEEDBACK 0x10  /**< Feedback endpoint (reports the sink's rate) */

/** Control-request type - bits 6:5 of the SETUP packet's @c bmRequestType. */
typedef enum {
    USB_STANDARD = 0,   /**< A standard request defined by the USB spec */
    USB_CLASS    = 1,   /**< A class-specific request (handled by a device class) */
    USB_VENDOR   = 2,   /**< A vendor-specific request */
} usb_req_type;

/**
 * @name Standard request codes
 * @c bRequest values for ::USB_STANDARD requests (USB 2.0 Table 9-4). The core
 * answers these itself; a class only ever sees ::USB_CLASS / ::USB_VENDOR ones,
 * plus GET_DESCRIPTOR for its own class-specific descriptor types.
 * @{
 */
#define USB_REQ_GET_STATUS         0x00  /**< Read the recipient's 2-byte status word (see ::USB_STATUS_ENDPOINT_HALT) */
#define USB_REQ_CLEAR_FEATURE      0x01  /**< Clear a feature on the recipient (see ::USB_FEATURE_ENDPOINT_HALT) */
#define USB_REQ_SET_FEATURE        0x03  /**< Set a feature on the recipient */
#define USB_REQ_SET_ADDRESS        0x05  /**< Assign the device its bus address (handled by the host stack) */
#define USB_REQ_GET_DESCRIPTOR     0x06  /**< Fetch a descriptor; @c wValue is type:index (see the @c USB_DT_* codes) */
#define USB_REQ_SET_DESCRIPTOR     0x07  /**< Write a descriptor - optional, this library does not implement it */
#define USB_REQ_GET_CONFIGURATION  0x08  /**< Read the active @c bConfigurationValue */
#define USB_REQ_SET_CONFIGURATION  0x09  /**< Select a configuration; only ::USB_CONFIG_VALUE exists here */
#define USB_REQ_GET_INTERFACE      0x0A  /**< Read an interface's current alternate setting */
#define USB_REQ_SET_INTERFACE      0x0B  /**< Select an interface's alternate setting (how streaming classes start/stop) */
#define USB_REQ_SYNCH_FRAME        0x0C  /**< Report an isochronous endpoint's synchronization frame */
/** @} */

#define USB_REQ_DIR_IN   0x80   /**< @c bmRequestType bit 7: device-to-host */
/** Extract the ::usb_req_type from a @c bmRequestType (bits 6:5). */
#define USB_REQ_TYPE(bmRequestType)  (((bmRequestType) >> 5) & 3)

/**
 * @name Request recipients
 * Bits 4:0 of @c bmRequestType (USB 2.0 Table 9-2).
 * @{
 */
#define USB_RECIP_DEVICE     0  /**< The device as a whole */
#define USB_RECIP_INTERFACE  1  /**< One interface, named by the low byte of @c wIndex */
#define USB_RECIP_ENDPOINT   2  /**< One endpoint, named by the low byte of @c wIndex */
/** @} */
/** Extract the recipient from a @c bmRequestType (bits 4:0). */
#define USB_REQ_RECIP(bmRequestType) ((bmRequestType) & 0x1f)

/** @c wValue for CLEAR_FEATURE / SET_FEATURE on an endpoint recipient (USB 2.0 Table 9-6). */
#define USB_FEATURE_ENDPOINT_HALT  0
/** GET_STATUS on an endpoint recipient returns this bit set while the endpoint is halted. */
#define USB_STATUS_ENDPOINT_HALT   0x0001

/** Descriptor types below this are standard; from here up they are class-specific
 *  (0x21 HID, 0x24 CS_INTERFACE, ...) and belong to a class, not the core. */
#define USB_DT_CLASS_SPECIFIC_BASE 0x21

/** Fixed fields of the device and configuration descriptors this library emits. */
#define USB_EP0_MAX_PACKET          64    /**< bMaxPacketSize0 for a full/high-speed device */
#define USB_NUM_CONFIGURATIONS      1     /**< Exactly one configuration is exposed */
#define USB_CONFIG_VALUE            1     /**< Its bConfigurationValue */
#define USB_CONFIG_ATTR_BUS_POWERED 0x80  /**< bmAttributes bit 7: reserved, always set */
#define USB_CONFIG_MAX_POWER_100MA  50    /**< bMaxPower, counted in 2 mA units */

/** Reported link speed (select with usbip_device_set_speed()). */
typedef enum {
    USB_SPEED_LOW,      /**< Low speed, 1.5 Mbit/s */
    USB_SPEED_FULL,     /**< Full speed, 12 Mbit/s (the default) */
    USB_SPEED_HIGH,     /**< High speed, 480 Mbit/s (512-byte bulk endpoints) */
    USB_SPEED_SUPER,    /**< Super speed, 5 Gbit/s */
} usb_speed;

/** Standard descriptor type codes (the @c bDescriptorType field, Sec.9.6). */
#define USB_DT_DEVICE                 1     /**< Device descriptor */
#define USB_DT_CONFIG                 2     /**< Configuration descriptor */
#define USB_DT_STRING                 3     /**< String descriptor */
#define USB_DT_INTERFACE              4     /**< Interface descriptor */
#define USB_DT_ENDPOINT               5     /**< Endpoint descriptor */
#define USB_DT_INTERFACE_ASSOCIATION  0x0B  /**< IAD - groups interfaces into one function */

/** USB-IF base class codes, as they appear in `bInterfaceClass` (and in
 *  `bDeviceClass` for the few that are device-defining). Subclass and protocol codes
 *  are specific to each class specification and live with that class's code. */
#define USB_CLASS_PER_INTERFACE 0x00  /**< bDeviceClass 0: class defined per interface */
#define USB_CLASS_AUDIO         0x01  /**< Audio (UAC) */
#define USB_CLASS_CDC           0x02  /**< Communications and CDC Control */
#define USB_CLASS_HID           0x03  /**< Human Interface Device */
#define USB_CLASS_IMAGE         0x06  /**< Still Imaging (PTP; MTP rides on it) */
#define USB_CLASS_MSC           0x08  /**< Mass Storage */
#define USB_CLASS_CDC_DATA      0x0A  /**< CDC Data */
#define USB_CLASS_VIDEO         0x0E  /**< Video (UVC) */
#define USB_CLASS_WIRELESS      0xE0  /**< Wireless Controller (Bluetooth radio) */
#define USB_CLASS_MISC          0xEF  /**< Miscellaneous (see the IAD triple below) */
#define USB_CLASS_APP_SPEC      0xFE  /**< Application Specific (DFU) */
#define USB_CLASS_VENDOR_SPEC   0xFF  /**< Vendor Specific */

/** The device-descriptor class triple that declares a device composite: its
 *  functions are described by Interface Association Descriptors rather than by a
 *  single device-wide class (USB-IF IAD ECN). Set by usbip_device_set_composite();
 *  a class that is itself multi-interface (e.g. UVC) declares it too. */
#define USB_SUBCLASS_COMMON 0x02  /**< bDeviceSubClass: Common Class (with ::USB_CLASS_MISC) */
#define USB_PROTOCOL_IAD    0x01  /**< bDeviceProtocol: Interface Association Descriptor */

/** @} */

/**
 * @addtogroup errors
 * @{
 */

/** Error/status codes - same values as libusb, so ported constants keep working.
 *  Render any of them with usb_strerror(). */
#define USB_SUCCESS               0    /**< No error */
#define USB_ERROR_IO            (-1)   /**< Input/output error on the transport */
#define USB_ERROR_INVALID_PARAM (-2)   /**< An argument was invalid */
#define USB_ERROR_ACCESS        (-3)   /**< Insufficient permissions */
#define USB_ERROR_NO_DEVICE     (-4)   /**< The device has been disconnected */
#define USB_ERROR_NOT_FOUND     (-5)   /**< Entity not found (no matching device/endpoint) */
#define USB_ERROR_BUSY          (-6)   /**< Resource busy */
#define USB_ERROR_TIMEOUT       (-7)   /**< The operation timed out */
#define USB_ERROR_PIPE          (-9)   /**< Endpoint STALLed / pipe error */
#define USB_ERROR_NO_MEM        (-11)  /**< Out of memory */
#define USB_ERROR_OTHER         (-99)  /**< Other / unspecified error */
/**
 * Map a `USB_*` status/error code to a human-readable string.
 *
 * @param code  a value from the @ref errors "USB_* codes" (0 is ::USB_SUCCESS).
 * @return a constant, NUL-terminated description (never `NULL`; do not free).
 *         Codes without a dedicated message render as `"other error"`.
 */
const char *usb_strerror(int code);

/** @} */

/**
 * @addtogroup desc
 * @{
 */

/** The 8-byte SETUP packet that begins every control transfer (host byte order in
 *  the API). @c bmRequestType is a bitmask: bit 7 = direction (0 OUT, 1 IN), bits 6-5 =
 *  type (0 standard, 1 class, 2 vendor), bits 4-0 = recipient (0 device, 1 interface,
 *  2 endpoint) - e.g. 0x80 = IN/standard/device, 0x21 = OUT/class/interface. The core
 *  answers standard requests; a class/vendor handler (usbip_function_on_control()) sees the rest. */
typedef struct USB_PACKED {
    uint8_t  bmRequestType;   /**< Direction | type | recipient bitmask (see above) */
    uint8_t  bRequest;        /**< Request code (e.g. GET_DESCRIPTOR = 0x06)         */
    uint16_t wValue;          /**< Request-specific parameter (often a type/index)   */
    uint16_t wIndex;          /**< Request-specific parameter (often iface/endpoint) */
    uint16_t wLength;         /**< Number of data bytes in the (optional) data stage  */
} usb_setup;

/**
 * The same SETUP packet as the eight bytes that go on the wire, for the client
 * side, which submits it as a byte array rather than a struct:
 * @code
 *   uint8_t setup[8] = { USB_SETUP_BYTES(0x80, USB_REQ_GET_DESCRIPTOR, 0x0100, 0, 18) };
 * @endcode
 * @param type    @c bmRequestType.
 * @param request @c bRequest.
 * @param value   @c wValue, in host order.
 * @param index   @c wIndex, in host order.
 * @param length  @c wLength, in host order.
 */
#define USB_SETUP_BYTES(type, request, value, index, length) \
    (uint8_t)(type), (uint8_t)(request), USB_U16LE(value), USB_U16LE(index), USB_U16LE(length)

/* ---- typed descriptors (the device side fills these; lib serializes) ---- */

/** Standard USB device descriptor (Sec.9.6.1). */
typedef struct USB_PACKED {
    uint8_t  bLength, bDescriptorType;
    uint16_t bcdUSB;
    uint8_t  bDeviceClass, bDeviceSubClass, bDeviceProtocol, bMaxPacketSize0;
    uint16_t idVendor, idProduct, bcdDevice;
    uint8_t  iManufacturer, iProduct, iSerialNumber, bNumConfigurations;
} usb_device_descriptor;

/** Standard configuration descriptor (Sec.9.6.3). */
typedef struct USB_PACKED {
    uint8_t  bLength, bDescriptorType;
    uint16_t wTotalLength;
    uint8_t  bNumInterfaces, bConfigurationValue, iConfiguration, bmAttributes, bMaxPower;
} usb_config_descriptor;

/** Standard interface descriptor (Sec.9.6.5). */
typedef struct USB_PACKED {
    uint8_t bLength, bDescriptorType, bInterfaceNumber, bAlternateSetting, bNumEndpoints;
    uint8_t bInterfaceClass, bInterfaceSubClass, bInterfaceProtocol, iInterface;
} usb_interface_descriptor;

/** Standard endpoint descriptor (Sec.9.6.6). */
typedef struct USB_PACKED {
    uint8_t  bLength, bDescriptorType, bEndpointAddress, bmAttributes;
    uint16_t wMaxPacketSize;
    uint8_t  bInterval;
} usb_endpoint_descriptor;

/* Class-specific descriptor structs (HID, CDC functional, …) are NOT here:
 * the core is class-agnostic. Each class/example defines its own (see the
 * HID example and class_cdc_acm). */

/** @} */

/* ---------------------------------------------------------------------------
 * Transport - the seam under every host/device call. Each takes an optional
 * ::usb_transport *; `NULL` means the default local USB/IP transport.
 * ------------------------------------------------------------------------- */

/**
 * @addtogroup transport
 * @{
 */

/** Opaque handle to a USB/IP transport (the wire under host/device calls). */
typedef struct usb_transport usb_transport;

/**
 * Create a real USB/IP transport over TCP.
 *
 * Hand the result to usbip_device_plug() (device side) or usbip_host_set_transport() (host
 * side). Ownership stays with the caller: release it with usbip_transport_free()
 * once the device is unplugged / the host context is exited.
 *
 * @param host  Peer/bind address. On the host side, the USB/IP server to connect
 *              to; on the device side, the local address to serve on. Pass `NULL`
 *              for the local default (connect to `127.0.0.1`, serve on `0.0.0.0`).
 * @param port  TCP port; the USB/IP well-known port is 3240.
 * @return a transport handle, or `NULL` on allocation failure.
 */
usb_transport *usbip_transport(const char *host, int port);

/**
 * Create an in-process transport for tests - no sockets, no kernel, no root.
 *
 * A device plugged onto a loopback transport can be driven by a host built on the
 * same handle, entirely within one process; the test suite uses this for
 * cross-language verification.
 *
 * @return a transport handle, or `NULL` on allocation failure.
 */
usb_transport *usbip_loopback(void);

/**
 * Release a transport created by usbip_transport() / usbip_loopback().
 *
 * The transport must no longer be in use - unplug the device (usbip_device_unplug()) and
 * exit any host context (usbip_host_exit()) first.
 *
 * @param transport  the transport to free.
 */
void usbip_transport_free(usb_transport *transport);

/** @} */

#endif /* USBIP_H */
