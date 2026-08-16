/*
 * Copyright (C) 2026 Jabez Winston
 *
 * SPDX-License-Identifier: MIT
 *
 * Date : 04-June-2026
 */
/**
 * @file classes/mtp.h
 * @ingroup class_mtp
 * @brief USB MTP (Media Transfer Protocol) v1.1 device class.
 *
 * Exports one or more host directory trees as MTP storages: Windows Explorer,
 * libmtp (mtp-detect/mtp-files) and gphoto2 can browse them, download files and -
 * unless read-only - upload, delete, rename, move/copy and edit. MTP rides the
 * Still Image interface (class 0x06/0x01/0x01) and a bulk container protocol
 * (command -> optional data -> response) with an interrupt IN endpoint for events.
 *
 * This class implements the MTP protocol and object model (device-global handles,
 * the per-storage tree, properties, the Android partial/edit extensions) but does
 * **no filesystem access** - all storage I/O is delegated to the backend callbacks
 * below, one per storage context (see examples/device/mtp_device.c). Pairs with
 * usbip_function_enable_msos(func, "MTP", NULL) (done when opts.winusb is set) so Windows binds
 * its MTP/WPD driver with no INF. Mirrors python classes/device/mtp.py.
 */
#ifndef CLASSES_MTP_H
#define CLASSES_MTP_H

#include "usb_class.h"

/**
 * @addtogroup class_mtp
 * @{
 */

/**
 * One MTP function: a single instance of the class on a device.
 *
 * Opaque - create one with mtp_add() and use the functions below. Distinct from
 * the other classes' handles, so mixing them up is a compile error rather than a
 * misread state block.
 */
typedef struct mtp_func mtp_func;

#define MTP_MAX_STORAGES 8      /**< Maximum number of storages a device may expose */

/**
 * Callback used by ::mtp_opts::listdir to report one directory entry.
 * @param ctx   the opaque pointer that listdir() was handed.
 * @param name  the entry's name (a single path component, not a full path).
 */
typedef void (*mtp_emit)(void *ctx, const char *name);

/** MTP configuration: device identity + one or more storage backends.
 *
 *  The class implements the MTP protocol and object model but performs **no**
 *  filesystem access - every storage operation is delegated to the callbacks below.
 *  Each callback's first argument is the per-storage context (one of @c stores[]).
 *  Paths are relative and "/"-separated; the empty string `""` is that storage's
 *  root. Unless noted, the `int` callbacks return 0 on success and a negative value
 *  on failure. */
typedef struct {
    int         read_only;      /**< Nonzero -> reject host writes/deletes/edits */
    const char *name;           /**< Model / device friendly name */
    const char *manufacturer;   /**< DeviceInfo Manufacturer (may be NULL) */
    const char *serial;         /**< 32 hex chars (NULL -> a built-in default) */
    int         winusb;         /**< Nonzero -> advertise the Microsoft OS "MTP" Compatible ID */

    int         n_stores;                   /**< Number of storages exposed (entries used in @c stores) */
    void       *stores[MTP_MAX_STORAGES];   /**< Per-storage backend contexts (e.g. a directory root) */

    /* ---- storage backend (the app implements these) ---- */
    /**
     * List a directory: call @p emit once per entry it contains.
     * @param store  the per-storage context.
     * @param path   directory path (`""` = storage root).
     * @param emit   sink for entry names (see ::mtp_emit).
     * @param ctx    opaque pointer to pass back to @p emit.
     */
    void        (*listdir)(void *store, const char *path, mtp_emit emit, void *ctx);
    /**
     * Stat one object.
     * @param store        the per-storage context.
     * @param path         object path.
     * @param[out] is_dir  set non-zero for a directory, 0 for a file.
     * @param[out] size    set to the file size in bytes.
     * @param[out] mtime   set to the modification time (Unix epoch seconds).
     * @return 0 if the object exists, < 0 if it is missing.
     */
    int         (*get_info)(void *store, const char *path, int *is_dir,
                            uint64_t *size, uint32_t *mtime);
    /**
     * Read from a file.
     * @param store  the per-storage context.
     * @param path   file path.
     * @param off    byte offset to read from.
     * @param buf    destination buffer.
     * @param want   maximum number of bytes to read.
     * @return bytes read (0 at end of file), or < 0 on error.
     */
    int         (*read)(void *store, const char *path, long off, uint8_t *buf, int want);
    /**
     * Create or overwrite a file with the given contents (whole-file write).
     * @param store  the per-storage context.
     * @param path   file path.
     * @param data   the file contents.
     * @param len    number of bytes in @p data.
     * @return 0 on success, < 0 on error.
     */
    int         (*write)(void *store, const char *path, const uint8_t *data, int len);
    /**
     * Create a directory.
     * @param store  the per-storage context.
     * @param path   directory path to create.
     * @return 0 on success, < 0 on error.
     */
    int         (*make_dir)(void *store, const char *path);
    /**
     * Delete a file or empty directory.
     * @param store  the per-storage context.
     * @param path   object path to remove.
     * @return 0 on success, < 0 on error.
     */
    int         (*remove)(void *store, const char *path);
    /**
     * Rename or move an object.
     * @param store    the per-storage context.
     * @param path     existing object path.
     * @param newpath  new path.
     * @return 0 on success, < 0 on error.
     */
    int         (*rename)(void *store, const char *path, const char *newpath);
    /**
     * Partial in-place write (Android edit extension; may be `NULL`).
     * @param store  the per-storage context.
     * @param path   file path.
     * @param off    byte offset to write at.
     * @param data   the bytes to write.
     * @param len    number of bytes in @p data.
     * @return 0 on success, < 0 on error.
     */
    int         (*pwrite)(void *store, const char *path, long off, const uint8_t *data, int len);
    /**
     * Truncate a file to a new length (Android edit extension; may be `NULL`).
     * @param store  the per-storage context.
     * @param path   file path.
     * @param size   new length in bytes.
     * @return 0 on success, < 0 on error.
     */
    int         (*truncate)(void *store, const char *path, uint64_t size);
    /**
     * Report the storage's capacity.
     * @param store        the per-storage context.
     * @param[out] total   set to total bytes.
     * @param[out] freeb   set to free bytes.
     */
    void        (*disk_usage)(void *store, uint64_t *total, uint64_t *freeb);
    /**
     * Storage label shown to the host (may be `NULL`).
     * @param store  the per-storage context.
     * @return a NUL-terminated label, or `NULL` for a built-in default.
     */
    const char *(*description)(void *store);

    /**
     * Human-readable log hook for MTP activity (may be `NULL`).
     * @param user  the @c user pointer.
     * @param text  a NUL-terminated description.
     */
    void  (*on_event)(void *user, const char *text);
    void   *user;               /**< Opaque pointer passed to @c on_event */
} mtp_opts;

/**
 * Add an MTP interface to a device.
 * @param dev     the device.
 * @param opts  device identity + one or more storage backends (see ::mtp_opts).
 * @return the new function, or `NULL` on error.
 */
mtp_func *mtp_add(usbip_device *dev, const mtp_opts *opts);

extern const usbip_device_class usbip_device_mtp;   /**< The MTP class, for usbip_device_add_class() */

/** @} */

#endif /* CLASSES_MTP_H */
