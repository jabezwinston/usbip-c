/*
 * Copyright (C) 2026 Jabez Winston
 *
 * SPDX-License-Identifier: MIT
 *
 * Date : 04-June-2026
 */
/**
 * @file classes/dfu.h
 * @ingroup class_dfu
 * @brief USB DFU (Device Firmware Upgrade) device class, DFU 1.1.
 *
 * Presents a DFU-mode device (bInterfaceProtocol 0x02, already in dfuIDLE) that
 * dfu-util can UPLOAD from / DOWNLOAD to. One alternate setting per target. The
 * class makes NO assumption about what backs a target - a file, a flash part,
 * whatever: each target is a linear byte store described by the callbacks
 * below, supplied by the application (see examples/device/dfu_device.c for a
 * file backend). DFU is entirely EP0 control transfers, so this needs no
 * USB/IP transport support. Pairs with usbip_function_enable_winusb() (done for you when
 * opts.winusb is set) for automatic WinUSB binding on Windows.
 *
 * Built only on the public API (usbip_device.h). Mirrors python classes/device/dfu.py.
 */
#ifndef CLASSES_DFU_H
#define CLASSES_DFU_H

#include "usb_class.h"

/**
 * @addtogroup class_dfu
 * @{
 */

/**
 * One DFU interface: a single instance of the class on a device.
 *
 * Opaque - create one with dfu_add() and use the functions below. Distinct from
 * the other classes' handles, so mixing them up is a compile error rather than a
 * misread state block.
 */
typedef struct dfu_iface dfu_iface;

/** DFU bStatus codes (DFU 1.1 Sec.6.1.2), reported to the host via DFU_GETSTATUS. A
 *  target's write() returns ::DFU_STATUS_OK (0) on success, or one of these to
 *  make the device enter dfuERROR and report that status - pick the one that
 *  fits, or ::DFU_STATUS_errWRITE as a catch-all "couldn't store it". */
typedef enum {
    DFU_STATUS_OK            = 0x00,    /**< No error */
    DFU_STATUS_errTARGET     = 0x01,    /**< File is not for this device */
    DFU_STATUS_errFILE       = 0x02,    /**< File fails a vendor verification test */
    DFU_STATUS_errWRITE      = 0x03,    /**< Unable to write memory */
    DFU_STATUS_errERASE      = 0x04,    /**< Memory erase failed */
    DFU_STATUS_errCHECK_ERASED = 0x05,  /**< Memory erase check failed */
    DFU_STATUS_errPROG       = 0x06,    /**< Program memory function failed */
    DFU_STATUS_errVERIFY     = 0x07,    /**< Programmed memory failed verification */
    DFU_STATUS_errADDRESS    = 0x08,    /**< Received address is out of range */
    DFU_STATUS_errNOTDONE    = 0x09,    /**< Zero-length DNLOAD but not all data received */
    DFU_STATUS_errFIRMWARE   = 0x0A,    /**< Firmware corrupt; cannot return to run-time */
    DFU_STATUS_errVENDOR     = 0x0B,    /**< Vendor-specific error (see iString) */
    DFU_STATUS_errUSBR       = 0x0C,    /**< Unexpected USB reset */
    DFU_STATUS_errPOR        = 0x0D,    /**< Unexpected power-on reset */
    DFU_STATUS_errUNKNOWN    = 0x0E,    /**< Something went wrong, cause unknown */
    DFU_STATUS_errSTALLEDPKT = 0x0F,    /**< Device stalled an unexpected request */
} dfu_status;

/** One DFU target == one alternate setting == one app-supplied linear byte store.
 *  The class calls these with byte offsets (a multiple of wTransferSize); `ctx` is
 *  passed back to each. begin/finish/size may be NULL. */
typedef struct {
    const char *name;       /**< iInterface name shown by `dfu-util -l` (e.g. "fw0") */
    void       *ctx;        /**< Backend context, passed to every callback below */
    /**
     * Store firmware bytes at a byte offset (DOWNLOAD).
     * @param ctx   the target's @c ctx pointer.
     * @param off   byte offset (a multiple of the DFU wTransferSize).
     * @param data  the bytes to store.
     * @param len   number of bytes in @p data.
     * @return ::DFU_STATUS_OK (0) on success, or a `DFU_STATUS_*` code reported to the
     *         host via GETSTATUS (any other non-zero is treated as ::DFU_STATUS_errWRITE).
     */
    int       (*write)(void *ctx, uint32_t off, const uint8_t *data, uint32_t len);
    /**
     * Return firmware bytes at a byte offset (UPLOAD).
     * @param ctx   the target's @c ctx pointer.
     * @param off   byte offset (a multiple of the DFU wTransferSize).
     * @param buf   destination buffer.
     * @param want  maximum number of bytes to return.
     * @return bytes read; 0 signals end-of-image and terminates the upload.
     */
    int       (*read)(void *ctx, uint32_t off, uint8_t *buf, uint32_t want);
    /** A fresh download is starting - discard any old image (optional). @param ctx the target's @c ctx pointer. */
    void      (*begin)(void *ctx);
    /** Download complete - commit/flush the image (optional). @param ctx the target's @c ctx pointer. */
    void      (*finish)(void *ctx);
    /** Report bytes currently stored, for the log (optional). @param ctx the target's @c ctx pointer. @return stored byte count. */
    uint32_t  (*size)(void *ctx);
} dfu_target;

/** DFU configuration: the target table + DFU functional-descriptor parameters. */
typedef struct {
    const dfu_target *targets;  /**< Target table (one entry per alternate setting) */
    int       n_targets;        /**< Number of targets */
    uint16_t  transfer_size;    /**< DFU wTransferSize (0 -> 1024) */
    uint8_t   attributes;       /**< DFU bmAttributes  (0 -> 0x07: dnload|upload|manifest-tolerant) */
    int       winusb;           /**< Nonzero -> advertise WinUSB (Microsoft OS 1.0 + 2.0) */
    /**
     * Human-readable log hook (optional).
     * @param user  the @c user pointer.
     * @param text  a NUL-terminated description.
     */
    void    (*on_event)(void *user, const char *text);
    void     *user;             /**< Passed to on_event */
} dfu_opts;

/**
 * Add a DFU interface to a device (one alternate setting per target).
 * @param dev     the device.
 * @param opts  the target table + DFU functional-descriptor parameters (see ::dfu_opts).
 * @return the DFU handle, or `NULL` on error.
 */
dfu_iface *dfu_add(usbip_device *dev, const dfu_opts *opts);

extern const usbip_device_class usbip_device_dfu;   /**< The DFU class, for usbip_device_add_class() */

/** @} */

#endif /* CLASSES_DFU_H */
