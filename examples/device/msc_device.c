/**
 * Copyright (C) 2026 Jabez Winston
 *
 * SPDX-License-Identifier: MIT
 *
 * Date : 04-June-2026
 *
 * msc_device.c - virtual USB Mass Storage device over USB/IP, backed by image files.
 *
 *   ./msc_device                         # 64 MiB ./msc_device.img, read-write
 *   ./msc_device --size 128M --file disk.img
 *   ./msc_device --read-only
 *   ./msc_device --cdrom --file image.iso
 *   ./msc_device --floppy                # 1.44 MB floppy, ./msc_device.img
 *   ./msc_device --floppy 720K --ufi     # ... as a real USB floppy drive would
 *
 *   Several images = several logical units (LUNs) behind one interface, each its
 *   own drive on the host; a tag on an entry says what that one is:
 *   ./msc_device --file a.img,b.img --size 8M,64M
 *   ./msc_device --file disk.img,install.iso:cdrom,key.img:ro
 *
 *   attach it with a USB/IP client
 *   (Linux: sudo modprobe vhci-hcd ; sudo usbip attach -r 127.0.0.1 -b 1-1)
 */
#define _GNU_SOURCE
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "_support/cmdline.h"
#include "_support/logging.h"
#include "classes/msc.h"

#define DEFAULT_IMAGE "msc_device.img"

/* ---- one backing image = one logical unit, carried as that unit's msc_lun.user ---- */
struct disk
{
    FILE *fp;         /* the backing image */
    const char *path;
    int writable;     /* 0 if the image itself is read-only (e.g. a distro ISO) */
    int medium;       /* MSC_MEDIUM_DISK / _CDROM / _FLOPPY */
    int read_only;    /* write-protect this unit */
    uint32_t num_blocks, block_size;
};

/* The grouped SCSI-command logger, carried as msc_opts.user: it belongs to the
 * device, and every unit's log line arrives through it. */
struct app
{
    usbip_logger log;
};

/* Seek to / measure a byte offset. fseek() and ftell() work in long, 32-bit on
 * Windows, so an image past 2 GiB needs the 64-bit spellings. */
static int seek_to(FILE *fp, uint64_t off)
{
#ifdef _WIN32
    return _fseeki64(fp, (long long)off, SEEK_SET);
#else
    return fseeko(fp, (off_t)off, SEEK_SET);
#endif
}

static uint64_t image_size(FILE *fp)
{
#ifdef _WIN32
    if (_fseeki64(fp, 0, SEEK_END) != 0)
        return 0;

    return (uint64_t)_ftelli64(fp);
#else
    if (fseeko(fp, 0, SEEK_END) != 0)
        return 0;

    return (uint64_t)ftello(fp);
#endif
}

/* The block backend. Every callback is handed the unit it is about, so one set of
 * them serves however many images the command line named. */
static uint32_t disk_num_blocks(void *user)
{
    return ((struct disk *)user)->num_blocks;
}

static uint32_t disk_block_size(void *user)
{
    return ((struct disk *)user)->block_size;
}

static int disk_read(void *user, uint32_t lba, uint32_t count, uint8_t *buf)
{
    struct disk *d = user;
    size_t n_bytes = (size_t)count * d->block_size;

    if (seek_to(d->fp, (uint64_t)lba * d->block_size) != 0)
        return -1;

    return fread(buf, 1, n_bytes, d->fp) == n_bytes ? 0 : -1;
}

static int disk_write(void *user, uint32_t lba, uint32_t count, const uint8_t *buf)
{
    struct disk *d = user;
    size_t n_bytes = (size_t)count * d->block_size;

    if (!d->writable)
        return -1;

    if (seek_to(d->fp, (uint64_t)lba * d->block_size) != 0)
        return -1;

    return fwrite(buf, 1, n_bytes, d->fp) == n_bytes ? 0 : -1;
}

/* Open the backing image. An existing file is only ever grown to `size`, never
 * shortened, so --file cannot discard a prepared image; a missing one is created at
 * `size`, and an unwritable one is served read-only. Unbuffered, because the SCSI
 * handlers alternate reads and writes on the same stream. */
static FILE *open_image(const char *path, uint64_t size, int *writable)
{
    FILE *fp = fopen(path, "r+b");
    *writable = 1;
    if (!fp)
    {
        fp = fopen(path, "rb"); /* there but not writable -> read-only medium */
        if (fp)
        {
            *writable = 0;
            setvbuf(fp, NULL, _IONBF, 0);
            return fp;
        }
        fp = fopen(path, "w+b"); /* not there at all -> create it */
        if (!fp)
            return NULL;
    }
    setvbuf(fp, NULL, _IONBF, 0);

    if (image_size(fp) < size)
    { /* grow: writing the last byte zero-fills the gap */
        seek_to(fp, size - 1);
        fputc(0, fp);
    }
    return fp;
}

/* msc on_command sink: skip status polls and group reads/writes by command name
 * (the text up to " lba=" / " -> "), so consecutive block traffic collapses. */
static void msc_log(void *user, const char *text)
{
    struct app *app = user;
    if (app->log.verbose)
    { /* verbose: print every command */
        usbip_log_emit(&app->log, text, text);
        return;
    }

    if (strncmp(text, "TEST UNIT READY", 15) == 0)
        return; /* status poll, skip when grouping */

    const char *lba = strstr(text, " lba=");
    const char *arrow = strstr(text, " -> ");
    const char *end = text + strlen(text);

    if (lba)
        end = lba;

    if (arrow && arrow < end)
        end = arrow;

    char key[96];
    size_t key_len = (size_t)(end - text);

    if (key_len >= sizeof(key))
        key_len = sizeof(key) - 1;

    memcpy(key, text, key_len);
    key[key_len] = 0;
    usbip_log_emit(&app->log, key, text);
}

/* msc-specific command-line options (the common ones live in _support/cmdline).
 * --file, --size and --block-size stay text: each may be a comma-separated list,
 * one entry per logical unit. */
struct msc_cli
{
    char *size;
    char *block_size;
    char *file;
    int read_only, cdrom, ufi;
    const char *floppy; /* the format name, or NULL when --floppy was not given */
};

#define DEFAULT_FLOPPY_FORMAT "1.44M"

/* --floppy names its format on the command line; the class API takes a code. */
static const struct
{
    const char *name;
    int format;
} FLOPPY_NAMES[] = {
    {"2.88M", MSC_FLOPPY_2_88M},
    {"1.44M", MSC_FLOPPY_1_44M},
    {"1.2M", MSC_FLOPPY_1_2M},
    {"720K", MSC_FLOPPY_720K},
    {"360K", MSC_FLOPPY_360K},
};

/* Case-insensitive compare, so "1.44m" and "1.44M" name the same format.
 * strcasecmp() is POSIX and _stricmp() is the Windows spelling; neither is C. */
static int name_eq(const char *a, const char *b)
{
    for (; *a && *b; a++, b++)
    {
        char ca = *a >= 'a' && *a <= 'z' ? (char)(*a - 'a' + 'A') : *a;
        char cb = *b >= 'a' && *b <= 'z' ? (char)(*b - 'a' + 'A') : *b;
        if (ca != cb)
            return 0;
    }

    return *a == *b;
}

/* The MSC_FLOPPY_* code a format name stands for, or -1 if it names none. */
static int floppy_format_of(const char *name)
{
    for (size_t i = 0; i < sizeof(FLOPPY_NAMES) / sizeof(FLOPPY_NAMES[0]); i++)
        if (name_eq(name, FLOPPY_NAMES[i].name))
            return FLOPPY_NAMES[i].format;

    return -1;
}

/* Cut "a,b,c" into its pieces in place. Returns the count, or -1 past `max` -- a
 * device cannot address more units than that. */
static int split_commas(char *s, char **out, int max)
{
    int n = 0;

    while (s)
    {
        char *comma = strchr(s, ',');

        if (comma)
            *comma = 0;

        if (*s)
        {
            if (n == max)
                return -1;

            out[n++] = s;
        }
        s = comma ? comma + 1 : NULL;
    }

    return n;
}

/* Strip the ":cdrom" / ":floppy" / ":disk" / ":ro" tags off one --file entry into
 * *medium and *read_only. A colon counts only when the text after it names a tag, so
 * a Windows path keeps its own ("C:\disks\a.img", even "C:\disks\a.img:cdrom"). */
static void parse_file_tags(char *entry, int *medium, int *read_only)
{
    for (;;)
    {
        char *colon = strrchr(entry, ':');

        if (!colon)
            return;

        if (name_eq(colon + 1, "cdrom"))
            *medium = MSC_MEDIUM_CDROM;
        else if (name_eq(colon + 1, "floppy"))
            *medium = MSC_MEDIUM_FLOPPY;
        else if (name_eq(colon + 1, "disk"))
            *medium = MSC_MEDIUM_DISK;
        else if (name_eq(colon + 1, "ro") || name_eq(colon + 1, "read-only"))
            *read_only = 1;
        else
            return;                 /* not a tag: it is part of the path */

        *colon = 0;
    }
}

/* What to call a unit on the console. */
static const char *medium_name(const struct disk *d, const char *floppy_format)
{
    if (d->medium == MSC_MEDIUM_CDROM)
        return "CD-ROM";

    if (d->medium == MSC_MEDIUM_FLOPPY)
        return floppy_format;

    return d->read_only ? "read-only" : "read-write";
}

/* The INQUIRY product id: what the host shows for this unit. */
static const char *product_of(const struct disk *d)
{
    if (d->medium == MSC_MEDIUM_CDROM)
        return "CD-ROM";

    if (d->medium == MSC_MEDIUM_FLOPPY)
        return "FLOPPY";

    return d->read_only ? "DISK-RO" : "DISK";
}

static int msc_on_opt(int c, char *arg, void *u)
{
    struct msc_cli *m = u;
    switch (c)
    {
        case 's':
            m->size = arg;
            return 1;

        case 'b':
            m->block_size = arg;
            return 1;

        case 'f':
            m->file = arg;
            return 1;

        case 'r':
            m->read_only = 1;
            return 1;

        case 'c':
            m->cdrom = 1;
            return 1;

        case 'F':
            m->floppy = arg ? arg : DEFAULT_FLOPPY_FORMAT; /* --floppy alone means 1.44M */
            return 1;

        case 'U':
            m->ufi = 1;
            return 1;
    }

    return 0;
}

int main(int argc, char **argv)
{
    struct msc_cli cli;
    memset(&cli, 0, sizeof(cli));

    static const cmdline_option options[] = {
        {"size",       CMDLINE_ARG_REQUIRED, 's', "N[K|M|G][,N...]", "disk size (default 64M); one value, or one per image"},
        {"block-size", CMDLINE_ARG_REQUIRED, 'b', "N[,N...]",  "bytes per block (default 512, 2048 for a CD-ROM)"},
        {"file", CMDLINE_ARG_REQUIRED, 'f', "IMG[,IMG...]",
             "image file(s) to back the disk (default " DEFAULT_IMAGE ",\ncreated at --size; an existing image keeps its contents).\n"
             "Each extra image is another logical unit, and may carry a\n tag saying what it is: IMG:cdrom, IMG:floppy, IMG:ro"},

        {"read-only", CMDLINE_ARG_NONE, 'r', NULL, "present a write-protected medium"},
        {"cdrom", CMDLINE_ARG_NONE, 'c', NULL, "present a read-only CD-ROM (pair with --file image.iso)"},

         /* --floppy or --floppy=720K */
        {"floppy", CMDLINE_ARG_OPTIONAL, 'F', "[=FORMAT]", "present a floppy: 2.88M, 1.44M (default), 1.2M, 720K, 360K\n"
                                                           "(sets --size, so an image of that size is created)"},

        {"ufi", CMDLINE_ARG_NONE, 'U', NULL, "declare the UFI command set (subclass 0x04) rather than\nSCSI transparent - what a real USB floppy drive reports"},
        {0, 0, 0, 0, 0},
    };
    cmdline_opts cli_opts = cmdline_parse(argc, argv, &(cmdline_spec){
        .vid = 0x1209,
        .pid = 0x0008,
        .high_speed_opt = 1,
        .options = options,
        .on_opt = msc_on_opt,
        .user = &cli,
    });

    int cdrom = cli.cdrom;
    int medium = MSC_MEDIUM_DISK;

    if (cdrom)
        medium = MSC_MEDIUM_CDROM;
    else if (cli.floppy)
        medium = MSC_MEDIUM_FLOPPY;

    const char *floppy_name = cli.floppy ? cli.floppy : DEFAULT_FLOPPY_FORMAT;
    int floppy_format = floppy_format_of(floppy_name);

    if (cdrom && cli.floppy)
    {
        fprintf(stderr, "--cdrom and --floppy are different media; pick one\n");
        return 1;
    }

    if (floppy_format < 0)
    {
        fprintf(stderr, "unknown floppy format '%s' (2.88M, 1.44M, 1.2M, 720K, 360K)\n", floppy_name);
        return 1;
    }

    /* One image per logical unit. The lists are cut up in copies, because splitting
     * writes NULs into them and argv is not ours to shred. */
    char *file_arg = strdup(cli.file ? cli.file : DEFAULT_IMAGE);
    char *size_arg = strdup(cli.size ? cli.size : "");
    char *block_arg = strdup(cli.block_size ? cli.block_size : "");
    char *files[MSC_MAX_LUNS], *sizes[MSC_MAX_LUNS], *blocks[MSC_MAX_LUNS];
    int n_luns = split_commas(file_arg, files, MSC_MAX_LUNS);
    int n_sizes = split_commas(size_arg, sizes, MSC_MAX_LUNS);
    int n_blocks = split_commas(block_arg, blocks, MSC_MAX_LUNS);

    if (n_luns < 0)
    {
        fprintf(stderr, "at most %d images: one per logical unit\n", MSC_MAX_LUNS);
        return 1;
    }

    if (n_luns == 0)
    {
        fprintf(stderr, "--file names no image\n");
        return 1;
    }

    if ((n_sizes > 1 && n_sizes != n_luns) || (n_blocks > 1 && n_blocks != n_luns))
    {
        fprintf(stderr, "--size / --block-size take one value, or one per image (%d)\n", n_luns);
        return 1;
    }

    struct app app;
    memset(&app, 0, sizeof(app));
    usbip_log_init(&app.log, "msc", cli_opts.verbose);

    struct disk disks[MSC_MAX_LUNS];
    memset(disks, 0, sizeof(disks));

    for (int i = 0; i < n_luns; i++)
    {
        struct disk *d = &disks[i];
        uint64_t size = 64ULL * 1024 * 1024;

        d->medium = medium;                 /* the global flags, then this entry's tags */
        d->read_only = cli.read_only;
        parse_file_tags(files[i], &d->medium, &d->read_only);
        d->path = files[i];

        /* one value applies to every unit; otherwise it is one per unit, in order */
        if (n_sizes)
            size = cmdline_parse_size(sizes[n_sizes == 1 ? 0 : i]);

        uint32_t block_size = 512;

        if (d->medium == MSC_MEDIUM_CDROM)
            block_size = 2048;

        if (n_blocks)
            block_size = (uint32_t)atoi(blocks[n_blocks == 1 ? 0 : i]);
        d->block_size = block_size;

        if (d->medium == MSC_MEDIUM_FLOPPY)
        { /* the format fixes the capacity: a floppy is the size it is */
            size = msc_floppy_format_size(floppy_format);
            d->block_size = 512;
        }

        d->fp = open_image(d->path, size, &d->writable);
        if (!d->fp)
        {
            fprintf(stderr, "cannot open image %s\n", d->path);
            return 1;
        }
        /* the medium is as big as the image actually is: an existing one may be larger */
        d->num_blocks = (uint32_t)(image_size(d->fp) / d->block_size);
        if (d->medium == MSC_MEDIUM_FLOPPY)
            d->num_blocks = (uint32_t)(size / d->block_size); /* a floppy is the size its format says */

        if (d->num_blocks == 0)
            d->num_blocks = 1;

        if (!d->writable || d->medium == MSC_MEDIUM_CDROM)
            d->read_only = 1;
    }

    //! [add]
    msc_opts ops = {
        .num_blocks = disk_num_blocks,
        .block_size = disk_block_size,
        .read       = disk_read,
        .write      = disk_write,
        .ufi        = cli.ufi,
        .vendor     = "USB-IP",
        .revision   = "0001",
        .n_luns     = n_luns,        /* one logical unit per image, each its own drive */
        .on_command = msc_log,
        .user       = &app,          /* the on_command context; the units bring their own */
    };

    for (int i = 0; i < n_luns; i++)
    {
        ops.luns[i].user      = &disks[i];
        ops.luns[i].medium    = disks[i].medium;
        ops.luns[i].read_only = disks[i].read_only;
        ops.luns[i].product   = product_of(&disks[i]);
    }

    usbip_device *dev = usbip_device_create(cli_opts.vid, cli_opts.pid);
    usbip_device_set_strings(dev, "USB over IP", "USBIP MSC", "0008");
    usb_speed speed = cli_opts.high_speed ? USB_SPEED_HIGH : USB_SPEED_FULL; /* HS = 512 B bulk */

    usbip_device_set_speed(dev, speed);
    msc_disk *disk = msc_add(dev, &ops);    /* SCSI / Bulk-Only Transport over the callbacks */
    //! [add]

    if (!disk)
    {
        fprintf(stderr, "msc_add failed\n");
        return 1;
    }

    if (n_luns == 1)
        fprintf(stderr, "[msc] %s%s %s: %u x %uB = %.1f MiB  (%04x:%04x, %s, on %s:%d)\n",
                medium_name(&disks[0], floppy_name), cli.ufi ? " (UFI)" : "", disks[0].path,
                disks[0].num_blocks, disks[0].block_size,
                disks[0].num_blocks * (double)disks[0].block_size / (1024 * 1024),
                cli_opts.vid, cli_opts.pid, cli_opts.high_speed ? "high speed" : "full speed", cli_opts.host, cli_opts.port);
    else
    {
        fprintf(stderr, "[msc] %d logical units%s  (%04x:%04x, %s, on %s:%d)\n",
                n_luns, cli.ufi ? " (UFI)" : "",
                cli_opts.vid, cli_opts.pid, cli_opts.high_speed ? "high speed" : "full speed", cli_opts.host, cli_opts.port);

        for (int i = 0; i < n_luns; i++)
            fprintf(stderr, "[msc]   lun %d: %-10s %s: %u x %uB = %.1f MiB\n",
                    i, medium_name(&disks[i], floppy_name), disks[i].path,
                    disks[i].num_blocks, disks[i].block_size,
                    disks[i].num_blocks * (double)disks[i].block_size / (1024 * 1024));
    }

    fprintf(stderr, "[msc] attach: sudo usbip attach -r 127.0.0.1 -b 1-1\n");

    usb_transport *transport = usbip_transport(cli_opts.host, cli_opts.port);
    int rc = usbip_device_plug(dev, transport);

    if (rc != USB_SUCCESS)
    {
        fprintf(stderr, "usbip_device_plug failed (port %d in use?)\n", cli_opts.port);
        return 1;
    }
#ifdef _WIN32
    for (;;)
        sleep(1); /* no pause() on Windows; idle */
#else
    for (;;)
        pause();
#endif
    return 0;
}
