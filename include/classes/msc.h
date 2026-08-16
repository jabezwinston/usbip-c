/*
 * Copyright (C) 2026 Jabez Winston
 *
 * SPDX-License-Identifier: MIT
 *
 * Date : 04-June-2026
 */
/**
 * @file classes/msc.h
 * @ingroup class_msc
 * @brief USB Mass Storage (Bulk-Only Transport + SCSI) device class, app API.
 *
 * The class runs the BOT state machine and a SCSI command set; the app supplies
 * a block-storage backend and a logging hook via ::msc_opts. Built on usbip_device.h only.
 */
#ifndef MSC_H
#define MSC_H

#include "usb_class.h"

/**
 * @addtogroup class_msc
 * @{
 */

/** Most logical units one mass-storage interface can carry. The Bulk-Only
 *  Transport addresses a unit with the four-bit @c bCBWLUN field of the CBW, so
 *  sixteen is the transport's own ceiling, not a chosen one. */
#define MSC_MAX_LUNS       16

/** @name Medium type
 *  What the disk presents itself as, set in ::msc_opts::medium. The default (0) is
 *  a plain removable disk, so a zero-initialised ::msc_opts needs no medium field.
 *  @{ */
#define MSC_MEDIUM_DISK    0   /**< Removable direct-access disk (SCSI type 0x00) */
#define MSC_MEDIUM_CDROM   1   /**< Read-only CD-ROM (SCSI type 0x05), MMC commands answered */
#define MSC_MEDIUM_FLOPPY  2   /**< Floppy: direct-access with a Flexible Disk geometry page */
/** @} */

/** @name Floppy format
 *  The standard formats a ::MSC_MEDIUM_FLOPPY disk can be, passed to
 *  msc_floppy_format_size(). All of them are 512 bytes per sector.
 *  @{ */
#define MSC_FLOPPY_2_88M   0   /**< 2.88 MB - 80 cylinders, 2 heads, 36 sectors */
#define MSC_FLOPPY_1_44M   1   /**< 1.44 MB - 80 cylinders, 2 heads, 18 sectors */
#define MSC_FLOPPY_1_2M    2   /**< 1.2 MB - 80 cylinders, 2 heads, 15 sectors */
#define MSC_FLOPPY_720K    3   /**< 720 KB - 80 cylinders, 2 heads, 9 sectors */
#define MSC_FLOPPY_360K    4   /**< 360 KB - 40 cylinders, 2 heads, 9 sectors */
/** @} */

/**
 * One mass-storage function: a single instance of the class on a device.
 *
 * Create one with msc_add(); it stays valid for the life of the device. The fields
 * are maintained by the class and are **read-only** to the app - they are here so
 * a callback (or a second function sharing the same app state) can tell which disk
 * it is looking at and how it was wired up, without keeping its own table.
 *
 * It is a distinct type from the other classes' handles, so passing (say) a
 * ::cdc_port where a ::msc_disk is wanted is a compile error, not a misread state
 * block.
 */
typedef struct msc_disk {
    int             index;            /**< 0-based instance number on this device, in
                                       *   msc_add() order                            */
    const char     *name;             /**< ::msc_opts::name, or `""` (never NULL)       */
    void           *user;             /**< ::msc_opts::user, passed straight through    */
    int             n_luns;           /**< Logical units this disk carries: 1 unless
                                       *   ::msc_opts::luns asked for more             */
    int             medium;           /**< Unit 0's medium, one of the `MSC_MEDIUM_*` codes */
    int             interface_number; /**< The mass-storage interface's bInterfaceNumber */
    uint8_t         in_ep;            /**< Bulk IN address actually assigned (on a
                                       *   composite this is relocated off 0x82)      */
    uint8_t         out_ep;           /**< Bulk OUT address actually assigned          */
    usbip_device   *dev;              /**< The owning device                           */
    usbip_function *func;             /**< The underlying function - escape hatch to
                                       *   the core usbip_device.h API                 */
} msc_disk;

/**
 * One logical unit: a backend context, plus what this unit alone presents itself as.
 *
 * A mass-storage interface can carry several units - a card reader's slots, a disk
 * next to its install CD - and the host addresses them separately (Linux gives each
 * one its own `/dev/sd*`, Windows its own drive letter). They share one interface
 * and one pair of bulk pipes; only the backend and the fields below differ.
 *
 * Every field except @c user is an **override**: left zero or `NULL` it takes the
 * device-wide value from ::msc_opts. Since ::MSC_MEDIUM_DISK is itself 0, a unit
 * cannot override a device-wide ::MSC_MEDIUM_CDROM back to a plain disk - leave
 * ::msc_opts::medium unset and name the medium on each unit instead.
 */
typedef struct {
    void       *user;       /**< Backend context, handed to @c num_blocks / @c block_size /
                             *   @c read / @c write for this unit                          */
    int         medium;     /**< One of the `MSC_MEDIUM_*` codes; 0 = ::msc_opts::medium    */
    int         read_only;  /**< Write-protect this unit; a device-wide
                             *   ::msc_opts::read_only still protects every unit, and a
                             *   CD-ROM is read-only whatever this says                    */
    const char *product;    /**< INQUIRY product id; `NULL` = ::msc_opts::product           */
    const char *serial;     /**< INQUIRY EVPD page 0x80 serial; `NULL` = ::msc_opts::serial  */
} msc_lun;

/** Mass-storage configuration: a block-storage backend + presentation options.
 *
 *  The backend addresses the medium in fixed-size blocks. With one logical unit -
 *  the default - every callback receives the opaque @c user pointer below; with
 *  several (see @c luns) they receive that unit's ::msc_lun::user instead, and
 *  @c user is left to @c on_command. */
typedef struct {
    /**
     * Report the medium's capacity, in blocks.
     * @param user  the unit's backend context.
     * @return the number of addressable blocks (capacity = this × block_size()).
     */
    uint32_t (*num_blocks)(void *user);
    /**
     * Report the block size in bytes (commonly 512).
     * @param user  the unit's backend context.
     * @return bytes per block.
     */
    uint32_t (*block_size)(void *user);
    /**
     * Read @p count blocks starting at logical block @p lba.
     * @param user   the unit's backend context.
     * @param lba    first logical block address.
     * @param count  number of blocks to read.
     * @param buf    destination, @p count × block_size() bytes.
     * @return 0 on success, non-zero to fail the SCSI command.
     */
    int (*read)(void *user, uint32_t lba, uint32_t count, uint8_t *buf);
    /**
     * Write @p count blocks starting at logical block @p lba.
     * @param user   the unit's backend context.
     * @param lba    first logical block address.
     * @param count  number of blocks to write.
     * @param buf    source, @p count × block_size() bytes.
     * @return 0 on success, non-zero to fail the SCSI command.
     */
    int (*write)(void *user, uint32_t lba, uint32_t count, const uint8_t *buf);

    /** Logical units this interface carries: entries used in @c luns. Zero - the
     *  usual case - means one unit, backed by @c user, described by the fields
     *  below; anything above ::MSC_MAX_LUNS is refused by msc_add(). */
    int     n_luns;
    msc_lun luns[MSC_MAX_LUNS];     /**< The units, when @c n_luns says there are several */

    int read_only;                  /**< Present a write-protected medium (every unit) */
    /** What the medium is: one of ::MSC_MEDIUM_DISK (the default), ::MSC_MEDIUM_CDROM
     *  or ::MSC_MEDIUM_FLOPPY. A CD-ROM is always read-only; a floppy answers MODE
     *  SENSE page 0x05 with the geometry of the standard format its capacity matches. */
    int medium;                     /**< Overridden per unit by ::msc_lun::medium */
    /**
     * Declare the UFI command set (bInterfaceSubClass 0x04, SFF-8070i) instead of
     * SCSI transparent (0x06) - the subclass a real USB floppy drive reports, and
     * what makes Windows show the drive as a floppy rather than a removable disk.
     * The transport stays Bulk-Only (bInterfaceProtocol 0x50); UFI-over-CBI is not
     * offered. Intended for ::MSC_MEDIUM_FLOPPY, but not restricted to it. The UFI
     * commands themselves (FORMAT UNIT, READ(12)/WRITE(12), VERIFY, SEEK, REZERO
     * UNIT) are answered whatever the subclass, as they are legal SCSI too.
     */
    int ufi;
    /** INQUIRY strings (may be NULL). The vendor and revision are the device's;
     *  the product id is overridden per unit by ::msc_lun::product. */
    const char *vendor, *product, *revision;
    /** Serial number returned by INQUIRY EVPD page 0x80, which Windows asks every
     *  removable device for (may be `NULL` for a default). Truncated past 20 chars.
     *  Overridden per unit by ::msc_lun::serial. */
    const char *serial;

    /**
     * Human-readable log hook, called with a description of each SCSI command
     * handled (may be `NULL`). On a multi-unit disk each line names its unit.
     * @param user  the @c user pointer.
     * @param text  a NUL-terminated description.
     */
    void (*on_command)(void *user, const char *text);
    /** Optional iInterface name for this disk, e.g. "Storage" - shown by the host
     *  (Linux: `/sys/bus/usb/devices/.../interface`; Windows: the device name). */
    const char *name;
    /** Opaque pointer passed to @c on_command - and, on a single-unit disk, to the
     *  block callbacks above. With @c n_luns set, each unit brings its own
     *  ::msc_lun::user for those instead. */
    void *user;
} msc_opts;

/**
 * Add a mass-storage function to a device: one interface carrying one logical
 * unit, or ::msc_opts::n_luns of them.
 * @param dev    the device.
 * @param opts  the block-storage backend and presentation options (see ::msc_opts).
 * @return the disk handle, or `NULL` on error.
 */
msc_disk *msc_add(usbip_device *dev, const msc_opts *opts);

/**
 * Byte size of a standard floppy format, for sizing the backing image.
 *
 * A ::MSC_MEDIUM_FLOPPY disk takes its geometry from the capacity it is given, so
 * an app that wants a real floppy sizes its image with this. A capacity matching
 * none of the formats still works - the class reports the true block count with
 * 1.44M's head/sector layout - but no host will call it a standard floppy.
 *
 * @param format  one of the `MSC_FLOPPY_*` codes.
 * @return the size in bytes, or 0 if @p format is not one of them.
 */
uint64_t msc_floppy_format_size(int format);

extern const usbip_device_class usbip_device_msc;   /**< The mass-storage class, for usbip_device_add_class() */

/** @} */

#endif /* MSC_H */
