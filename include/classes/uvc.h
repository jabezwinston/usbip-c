/*
 * Copyright (C) 2026 Jabez Winston
 *
 * SPDX-License-Identifier: MIT
 *
 * Date : 04-June-2026
 */
/**
 * @file classes/uvc.h
 * @ingroup class_uvc
 * @brief USB Video Class (webcam) device class, app API.
 *
 * Presents an isochronous UVC camera: a VideoControl + VideoStreaming interface
 * pair (grouped by an IAD) advertising uncompressed (YUY2) and/or MJPEG formats
 * at one resolution. The class runs Probe/Commit negotiation and streams UVC
 * payloads over an isochronous IN endpoint; the app supplies frame pixels (or
 * uses the built-in synthetic color-bar generator). Built on usbip_device.h only.
 */
#ifndef UVC_H
#define UVC_H

#include "usb_class.h"

/**
 * @addtogroup class_uvc
 * @{
 */

/**
 * One UVC camera function: a single instance of the class on a device.
 *
 * Opaque - create one with uvc_add() and use the functions below. Distinct from
 * the other classes' handles, so mixing them up is a compile error rather than a
 * misread state block.
 */
typedef struct uvc_cam uvc_cam;

#define UVC_HAS_YUYV  1  /**< opts.formats bit: advertise packed YUYV */
#define UVC_HAS_MJPEG 2  /**< opts.formats bit: advertise MJPEG */

/** UVC configuration: frame geometry, advertised formats, and a frame source. */
typedef struct {
    uint16_t width, height;         /**< Frame size, e.g. 320x240 */
    uint32_t fps;                   /**< Nominal frame rate */
    unsigned formats;               /**< UVC_HAS_YUYV | UVC_HAS_MJPEG (0 -> YUYV) */

    /**
     * Frame source (optional). Fill @p buf with ONE frame and return its length;
     * return `< 0` to use the built-in synthetic YUYV color-bar generator.
     *
     * The class only synthesizes YUYV; to advertise MJPEG (::UVC_HAS_MJPEG) you must
     * supply 'M' frames here (MJPEG encoding is application code - see the example).
     * @param user   the @c user pointer.
     * @param buf    output buffer.
     * @param cap    capacity of @p buf in bytes.
     * @param fmt    requested format: 'Y' = packed YUYV (width×height×2 bytes),
     *               'M' = a JPEG/MJPEG frame.
     * @param index  frame counter since streaming started.
     * @return the frame length in bytes, or < 0 for the built-in generator.
     */
    int (*next_frame)(void *user, uint8_t *buf, int cap, char fmt, uint32_t index);

    /**
     * Human-readable log hook (optional).
     * @param user  the @c user pointer.
     * @param text  a NUL-terminated description.
     */
    void (*on_event)(void *user, const char *text);
    void *user;                     /**< Opaque pointer passed to the callbacks above */
} uvc_opts;

/**
 * Add a UVC camera interface to a device. Sets the device class to
 * ::USB_CLASS_MISC / ::USB_SUBCLASS_COMMON / ::USB_PROTOCOL_IAD (0xEF/0x02/0x01).
 * @param dev     the device.
 * @param opts  frame geometry, advertised formats and a frame source (see ::uvc_opts).
 * @return the new function, or `NULL` on error.
 */
uvc_cam *uvc_add(usbip_device *dev, const uvc_opts *opts);

/**
 * Render one frame of the built-in animated color-bar test pattern (packed YUYV).
 *
 * Exposed so an app's `next_frame` can build pixels to encode (e.g. to MJPEG).
 * @param buf     output buffer (>= `width * height * 2` bytes).
 * @param width   frame width in pixels.
 * @param height  frame height in pixels.
 * @param index   running frame counter (animates the bars).
 */
void uvc_color_bars_yuyv(uint8_t *buf, int width, int height, uint32_t index);

extern const usbip_device_class usbip_device_uvc;   /**< The UVC video class, for usbip_device_add_class() */

/** @} */

#endif /* UVC_H */
