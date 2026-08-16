/**
 * Copyright (C) 2026 Jabez Winston
 *
 * SPDX-License-Identifier: MIT
 *
 * Date : 04-June-2026
 *
 * msc.c - USB Mass Storage device class: Bulk-Only Transport + SCSI (SBC/MMC).
 *
 * Built ONLY on the public API (usbip_device.h / classes/msc.h). Mirrors the Python
 * classes/device/msc.py. Per USB MSC BOT 1.0 and SCSI SPC/SBC.
 */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "classes/msc.h"


/* One endpoint NUMBER per direction (0x82 IN, not 0x81): WCH USBFS cores only
 * double-buffer a number used in ONE direction. A preference; composites relocate. */
/* Interface subclass/protocol (USB MSC overview 1.4 Sec.2). The base class is
 * ::USB_CLASS_MSC. */
#define MSC_SUBCLASS_SCSI 0x06   /* SCSI transparent command set */
#define MSC_SUBCLASS_UFI  0x04   /* UFI (SFF-8070i) - what a real USB floppy reports */
#define MSC_PROTOCOL_BOT  0x50   /* Bulk-Only Transport */

#define MSC_EP_IN  0x82
#define MSC_EP_OUT 0x01

/* ---- SCSI opcodes (SPC/SBC/MMC). Same names as the Python classes/device/msc.py. ----
 * Listing an opcode here IS defining and naming it: this one line gives the constant
 * below its value and scsi_name() its log text, so an opcode is never spelled twice. */
#define SCSI_OP_LIST(X)                                                                         \
    X(TEST_UNIT_READY,        0x00)  /* "are you there?" -- no data phase */                    \
    X(REZERO_UNIT,            0x01)  /* UFI: seek to track 0 -- a no-op here */                 \
    X(REQUEST_SENSE,          0x03)  /* fetch (and clear) the sense left by the last failure */ \
    X(FORMAT_UNIT,            0x04)  /* UFI: format the medium -- accepted, nothing to do */    \
    X(INQUIRY,                0x12)  /* who are you: vendor / product / revision */             \
    X(MODE_SENSE_6,           0x1A)  /* mode parameters, 6-byte CDB (write-protect bit) */      \
    X(START_STOP_UNIT,        0x1B)  /* spin up/down or eject -- a no-op here */                \
    X(PREVENT_ALLOW,          0x1E)  /* lock/unlock the medium -- a no-op here */               \
    X(READ_FORMAT_CAPACITIES, 0x23)  /* capacity list; Windows asks removable media for it */   \
    X(READ_CAPACITY_10,       0x25)  /* last LBA + block size, 32-bit */                        \
    X(READ_10,                0x28)  /* read N blocks from an LBA */                            \
    X(WRITE_10,               0x2A)  /* write N blocks; data arrives in later OUT packets */    \
    X(SEEK_10,                0x2B)  /* UFI: position the heads -- a no-op here */              \
    X(VERIFY_10,              0x2F)  /* UFI: verify blocks -- a no-op, nothing can rot here */  \
    X(SYNC_CACHE_10,          0x35)  /* flush -- a no-op, this disk has no write cache */       \
    X(READ_TOC,               0x43)  /* MMC: table of contents (CD-ROM only) */                 \
    X(GET_CONFIGURATION,      0x46)  /* MMC: current profile (CD-ROM only) */                   \
    X(GET_EVENT_STATUS,       0x4A)  /* MMC: media-change polling */                            \
    X(MODE_SENSE_10,          0x5A)  /* mode parameters, 10-byte CDB */                         \
    X(SERVICE_ACTION_IN_16,   0x9E)  /* 16-byte CDB whose real command is the service action */ \
    X(READ_12,                0xA8)  /* UFI: read, 12-byte CDB with a 4-byte block count */     \
    X(WRITE_12,               0xAA)  /* UFI: write, 12-byte CDB with a 4-byte block count */

/* The only enum in this file, and it declares nothing: it exists so the list above can
 * hand each opcode its value. Everything else here stays a #define. */
enum {
#define SCSI_OP_DEFINE(sym, code) sym = code,
    SCSI_OP_LIST(SCSI_OP_DEFINE)
#undef SCSI_OP_DEFINE
};

#define SAI_MASK             0x1F   /* service action lives in cdb[1] & SAI_MASK */
#define SAI_READ_CAPACITY_16 0x10   /* the only service action handled: READ CAPACITY(16) */

/* Sense data as (key, asc, ascq) - each expands to THREE arguments, so it drops
 * straight into msc_fail()/set_sense(), mirroring the Python tuples. */
#define SENSE_OK               0x00, 0x00, 0x00
#define SENSE_UNRECOVERED_READ 0x03, 0x11, 0x00   /* MEDIUM ERROR / unrecovered read */
#define SENSE_INVALID_COMMAND  0x05, 0x20, 0x00
#define SENSE_LBA_RANGE        0x05, 0x21, 0x00
#define SENSE_INVALID_FIELD    0x05, 0x24, 0x00
#define SENSE_LUN_NOT_SUPPORTED 0x05, 0x25, 0x00  /* ILLEGAL REQUEST / logical unit not supported */
#define SENSE_WRITE_PROTECT    0x07, 0x27, 0x00

/* REQUEST SENSE response (SPC fixed format) */
#define SENSE_LEN               18   /* bytes we return: 8-byte header + 10 more */
#define SENSE_RESPONSE_CURRENT  0x70 /* response code: current error, fixed format */
#define SENSE_ADDITIONAL_LEN    10   /* SENSE_LEN - 8, as the header must declare it */
#define SENSE_KEY_OFF           2    /* sense key, low nibble */
#define SENSE_ADDL_LEN_OFF      7    /* where SENSE_ADDITIONAL_LEN goes */
#define SENSE_ASC_OFF           12   /* additional sense code */
#define SENSE_ASCQ_OFF          13   /* additional sense code qualifier */

/* CDB fields shared by the 10-byte commands (READ(10) / WRITE(10)) */
#define CDB10_LBA_OFF           2    /* 4-byte big-endian logical block address */
#define CDB10_LEN_OFF           7    /* 2-byte big-endian transfer length, in blocks */

/* The 12-byte commands (READ(12) / WRITE(12)) keep the LBA where the 10-byte ones
 * do and widen the transfer length to four bytes. */
#define CDB12_LEN_OFF           6    /* 4-byte big-endian transfer length, in blocks */

/* ---- Bulk-Only Transport (USB MSC BOT 1.0) ---- */
#define BOT_GET_MAX_LUN     0xFE   /* class request: how many LUNs, minus one */
#define BOT_RESET           0xFF   /* class request: Bulk-Only Mass Storage Reset */
#define BOT_TAG_LEN         4      /* the tag echoed from each CBW back into its CSW */
#define BOT_SIG_LEN         4      /* length of both the CBW and CSW signatures */
#define BOT_CBW_SIG         "USBC" /* Command Block Wrapper, host -> device */
#define BOT_CBW_LEN         31     /* a CBW is exactly this long; anything else is junk */
#define BOT_CBW_TAG_OFF     4
#define BOT_CBW_DATALEN_OFF 8      /* dCBWDataTransferLength, little-endian */
#define BOT_CBW_FLAGS_OFF   12     /* bmCBWFlags; bit 7 set = the data phase is IN */
#define BOT_CBW_FLAG_IN     0x80
#define BOT_CBW_LUN_OFF     13     /* bCBWLUN: which logical unit the command is for */
#define BOT_CBW_LUN_MASK    0x0F   /* only the low four bits of that byte are the LUN */
#define BOT_CBW_CDB_OFF     15     /* the SCSI command itself starts here */
#define BOT_CSW_SIG         "USBS" /* Command Status Wrapper, device -> host */
#define BOT_CSW_LEN         13
#define BOT_CSW_TAG_OFF     4
#define BOT_CSW_RESIDUE_OFF 8      /* bytes promised but not sent, little-endian */
#define BOT_CSW_STATUS_OFF  12
#define BOT_STATUS_PASSED   0      /* command succeeded */
#define BOT_STATUS_FAILED   1      /* CHECK CONDITION: the host should REQUEST SENSE */

/* ---- INQUIRY standard data (SPC-2) ---- */
#define INQ_LEN              36     /* the standard INQUIRY data we return */
#define INQ_EVPD             0x01   /* cdb[1] bit 0: vital product data page requested */
#define INQ_PDT_DIRECT       0x00   /* peripheral device type: direct-access block device */
#define INQ_PDT_CDROM        0x05   /* peripheral device type: CD/DVD */
#define INQ_RMB_REMOVABLE    0x80   /* removable medium bit -- lets the desktop eject it */
#define INQ_VERSION_SPC2     0x04   /* the SCSI standard claimed */
#define INQ_RESPONSE_FORMAT  0x02   /* the only format modern hosts accept */
#define INQ_ADDITIONAL_LEN   31     /* INQ_LEN - 5, as the header must declare it */
#define INQ_VENDOR_OFF       8      /* the three identity strings are space-padded, */
#define INQ_VENDOR_LEN       8      /* not NUL-terminated (see set_str) */
#define INQ_PRODUCT_OFF      16
#define INQ_PRODUCT_LEN      16
#define INQ_REVISION_OFF     32
#define INQ_REVISION_LEN     4

/* ---- INQUIRY vital product data (EVPD) pages ----
 * Windows asks every removable device for the serial number page; Linux never does.
 * Header(4): device type, page code, reserved, page length. */
#define VPD_SUPPORTED_PAGES  0x00   /* the list of pages we answer */
#define VPD_UNIT_SERIAL      0x80   /* the medium's serial number, as text */
#define VPD_PAGE_CODE_OFF    1
#define VPD_LENGTH_OFF       3      /* the page length, excluding this header */
#define VPD_DATA_OFF         4
#define VPD_SERIAL_MAX       20     /* longer serials are truncated, not overflowed */
#define VPD_MAX_LEN          (VPD_DATA_OFF + VPD_SERIAL_MAX)

/* ---- READ FORMAT CAPACITIES (SFF-8070i / UFI 4.9) ----
 * The other command Windows sends to removable media and Linux does not.
 *
 * Reply: capacity list header(4), the current-capacity descriptor (block count(4),
 * type byte, block length(3)), then FORMATTABLE descriptors of the same shape with a
 * reserved type byte. A floppy lists the standard formats there, which is what lets
 * Windows offer a capacity to format to. */
#define RFC_DESC_LEN         8
#define RFC_LEN              (4 + RFC_DESC_LEN)
#define RFC_LIST_LEN_OFF     3      /* bytes of descriptor following the header */
#define RFC_DESC_OFF         4
#define RFC_TYPE_OFF         4      /* descriptor byte 4: what the capacity describes */
#define RFC_BLOCKLEN_OFF     5      /* 3-byte big-endian block length */
#define RFC_TYPE_FORMATTED   0x02   /* the medium is present and formatted */
#define RFC_TYPE_FORMATTABLE 0x00   /* reserved in a formattable descriptor */

/* ---- MODE SENSE page selection (SPC / UFI 4.5) ----
 * The page code has to be read: a floppy's geometry lives in a page, where every
 * other medium gets the bare mode parameter header. */
#define MODE_PAGE_MASK       0x3F   /* cdb[2] low bits: which page is wanted */
#define MODE_PAGE_FLEX_DISK  0x05   /* Flexible Disk: the floppy's geometry */
#define MODE_PAGE_ALL        0x3F   /* every page this device has */
#define MODE6_HDR_LEN        4      /* length, medium type, device-specific, block-desc len */
#define MODE10_HDR_LEN       8      /* 2-byte length, medium type, device-specific, ... */
#define MODE6_MEDIUM_OFF     1      /* where the medium type code sits in each header */
#define MODE10_MEDIUM_OFF    2
#define MODE6_DEV_SPEC_OFF   2      /* device-specific parameter: the write-protect bit */
#define MODE10_DEV_SPEC_OFF  3

/* ---- Flexible Disk page (UFI 4.5, page code 0x05) ----
 * 32 bytes: page header(2) then the drive geometry. Head settle and motor delays
 * stay zero -- no host reads them off a device with no motor. */
#define FLEX_PAGE_LEN        32
#define FLEX_PAGE_DATA_LEN   0x1E   /* FLEX_PAGE_LEN - 2, as the page header declares it */
#define FLEX_RATE_OFF        2      /* 2-byte transfer rate, kbit/s */
#define FLEX_HEADS_OFF       4
#define FLEX_SECTORS_OFF     5      /* sectors per track */
#define FLEX_SECTOR_SIZE_OFF 6      /* 2-byte data bytes per sector */
#define FLEX_CYLINDERS_OFF   8      /* 2-byte cylinder count */
#define FLEX_ROTATION_OFF    28     /* 2-byte medium rotation rate, rpm */
#define FLOPPY_RPM           300    /* what every 3.5" drive spins at */

/* The standard floppy formats, 512 bytes per sector; a medium takes the geometry of
 * the entry matching its block count.
 * Medium type codes are UFI table 17, which names only these three. */
#define FLOPPY_BLOCK_SIZE    512
static const struct {
    uint32_t    blocks;
    uint8_t     medium_type;
    uint8_t     cylinders, heads, sectors;
    uint16_t    rate_kbps;
    int         format;             /* the MSC_FLOPPY_* code this entry is */
} FLOPPY_FORMATS[] = {
    { 5760, 0x00, 80, 2, 36, 1000, MSC_FLOPPY_2_88M },
    { 2880, 0x94, 80, 2, 18,  500, MSC_FLOPPY_1_44M },
    { 2400, 0x93, 80, 2, 15,  500, MSC_FLOPPY_1_2M  },
    { 1440, 0x1E, 80, 2,  9,  250, MSC_FLOPPY_720K  },
    {  720, 0x00, 40, 2,  9,  250, MSC_FLOPPY_360K  },
};
#define FLOPPY_FORMAT_COUNT  (sizeof(FLOPPY_FORMATS) / sizeof(FLOPPY_FORMATS[0]))
#define FLOPPY_DEFAULT       1      /* index of 1.44M: the geometry an odd size borrows */

/* Room for the current capacity plus every format a floppy can be formatted to. */
#define RFC_MAX_LEN          (4 + RFC_DESC_LEN * (1 + (int)FLOPPY_FORMAT_COUNT))

/* Shown by the host as the drive's identity when the caller sets none. */
#define MSC_DEFAULT_VENDOR         "USB-IP"
#define MSC_DEFAULT_PRODUCT        "DISK"
#define MSC_DEFAULT_CDROM_PRODUCT  "CD-ROM"   /* used instead per opts.medium */
#define MSC_DEFAULT_FLOPPY_PRODUCT "FLOPPY"
#define MSC_DEFAULT_REVISION       "0001"
#define MSC_DEFAULT_SERIAL         "0123456789ABCDEF"

/* ---- misc response sizes / bits ---- */
#define READ_CAP_10_LEN     8       /* 4-byte last LBA + 4-byte block length */
#define READ_CAP_16_LEN     32      /* 8-byte last LBA + 4-byte block length, zero-padded */
#define MODE_WRITE_PROTECT  0x80    /* device-specific parameter: write protected */
#define MMC_PROFILE_CDROM   0x08    /* GET CONFIGURATION current profile */
#define MMC_TOC_ADR_CONTROL 0x14    /* ADR=1 (track position), control=4 (data track) */
#define MMC_TOC_LEADOUT     0xAA    /* the lead-out is reported as track 0xAA */
#define MMC_EVENT_NO_CHANGE 0x80    /* notification class header: nothing to report */

/* READ CAPACITY(16) parameter data: an 8-byte last-LBA then a 4-byte block length.
 * Only the low half of the LBA is populated -- these disks are far below 2^32 blocks. */
#define RC16_LBA_LO_OFF     4
#define RC16_BLOCKLEN_OFF   8

/* One logical unit, msc_lun overrides already resolved against msc_opts in lun_init,
 * so the SCSI handlers read one row.
 * Sense is per unit: one LUN's failure must not answer another's REQUEST SENSE. */
struct lun_state {
    void       *user;               /* backend context for num_blocks/block_size/read/write */
    int         medium;             /* one of the MSC_MEDIUM_* codes */
    int         read_only;
    const char *product, *serial;   /* never NULL once resolved */
    uint8_t     sense_key, asc, ascq;
};

struct msc_state {
    msc_disk pub;                   /* MUST be first: a msc_disk* IS a msc_state* */
    msc_opts opts;
    struct lun_state lun[MSC_MAX_LUNS];
    int      n_luns;                /* how many of them are in use (at least one) */
    /* The pipes this disk was assigned. A composite relocates them off MSC_EP_*, so
     * keep the objects rather than looking them back up by address. */
    usbip_ep *in, *out;
    int      cbw_in;                /* the current command's data phase is device->host */
    /* One pair of pipes shared by every unit, one command at a time, so the write in
     * flight names the unit it lands on. */
    int      writing;               /* mid WRITE(10)/WRITE(12) data phase */
    uint8_t  wop;                   /* which of the two, for the log line */
    uint8_t  wlun;                  /* the unit that CBW addressed */
    uint8_t  wtag[BOT_TAG_LEN];
    uint32_t wlba, wcount, wneed, wgot;
    uint8_t *wbuf;
};

/* The public msc_disk is the first member of the private state, so converting
 * between them is free in both directions. */
static inline msc_disk *disk_of(usbip_function *func)
{
    return &((struct msc_state *)usbip_function_state(func))->pub;
}

/* ---- helpers ---- */
/* Byte-order accessors come from usb_byteorder.h; SCSI is big-endian on the wire,
 * the BOT wrappers around it are little-endian. */

/* Copy an identity string into a fixed-width INQUIRY field. SCSI SPACE-pads these
 * and does not NUL-terminate, so memcpy + memset, never strcpy. */
static void set_str(uint8_t *dst, const char *src, int cap) {
    size_t len = src ? strlen(src) : 0;
    if (len > (size_t)cap)
        len = (size_t)cap;                /* longer names are truncated, not overflowed */
    if (len)
        memcpy(dst, src, len);
    memset(dst + len, ' ', (size_t)cap - len);
}

/* Log one command. Several units name the one being addressed up front, so a grouped
 * logger keys per unit; a lone unit says nothing about LUNs. */
static void msc_log(struct msc_state *st, int lun, const char *fmt, ...) {
    char lun_text[16];
    const char *prefix = NULL;

    if (st->n_luns > 1)
    {
        snprintf(lun_text, sizeof(lun_text), "lun%d ", lun);
        prefix = lun_text;
    }

    va_list ap;
    va_start(ap, fmt);
    usbip_class_vlog(st->opts.on_command, st->opts.user, prefix, fmt, ap);
    va_end(ap);
}

/* The single place an opcode becomes text: every log line and CHECK CONDITION label
 * goes through here. The log text is the SCSI_OP_LIST symbol itself with its
 * underscores spaced, so an opcode is never spelled out twice. Mirrors _NAMES in
 * the Python classes/device/msc.py. */
static struct {
    uint8_t op;
    char    name[24];   /* holds the stringized macro; a longer one fails to compile */
} SCSI_NAMES[] = {
#define SCSI_OP_ENTRY(sym, code) { code, #sym },
    SCSI_OP_LIST(SCSI_OP_ENTRY)
#undef SCSI_OP_ENTRY
};

static const char *scsi_name(uint8_t op) {
    for (size_t i = 0; i < sizeof(SCSI_NAMES) / sizeof(SCSI_NAMES[0]); i++) {
        if (SCSI_NAMES[i].op != op)
            continue;
        for (char *ch = SCSI_NAMES[i].name; *ch; ch++)
            if (*ch == '_')
                *ch = ' ';          /* idempotent, so re-spacing a spaced name is fine */
        return SCSI_NAMES[i].name;
    }
    return "SCSI";                      /* an opcode we do not implement */
}

/* The bulk IN pipe as assigned at build time, NOT looked up by MSC_EP_IN: once
 * another function holds 0x82 this disk moves, and every CSW is silently dropped. */
static usbip_ep *in_ep(usbip_function *iface)
{
    return ((struct msc_state *)usbip_function_state(iface))->in;
}

static void msc_csw(usbip_function *iface, const uint8_t tag[BOT_TAG_LEN], uint32_t residue, uint8_t status) {
    uint8_t csw[BOT_CSW_LEN];
    memcpy(csw, BOT_CSW_SIG, BOT_SIG_LEN);
    memcpy(csw + BOT_CSW_TAG_OFF, tag, BOT_TAG_LEN);
    usb_put_le32(csw + BOT_CSW_RESIDUE_OFF, residue);
    csw[BOT_CSW_STATUS_OFF] = status;
    usbip_device_write(in_ep(iface), csw, BOT_CSW_LEN, 0);
}

/* Abandon the data phase of a command that produced none of its promised data.
 *
 * Sending only the CSW hands it to the outstanding data URB, leaving the read that
 * wants the CSW unanswered until Windows' 20 s timeout.
 *
 * Halting ends the phase per BOT 1.0 Sec.6.7 case 4 ("Hi > Dn"). A short-but-nonempty
 * phase is fine -- the data completes the URB. */
static void msc_stall_data_phase(struct msc_state *st, uint32_t dlen) {
    if (!dlen)
        return;   /* nothing was promised, so there is no data phase to end */

    usbip_ep_stall(st->cbw_in ? st->in : st->out);
}

static void msc_data_in(usbip_function *iface, const uint8_t tag[BOT_TAG_LEN], uint32_t dlen,
                        const uint8_t *data, uint32_t datalen) {
    struct msc_state *st = usbip_function_state(iface);
    uint32_t send = datalen < dlen ? datalen : dlen;

    if (send)
        usbip_device_write(in_ep(iface), data, (int)send, 0);
    else
        msc_stall_data_phase(st, dlen);

    msc_csw(iface, tag, dlen - send, BOT_STATUS_PASSED);
}

static void set_sense(struct lun_state *lun, uint8_t key, uint8_t asc, uint8_t ascq) {
    lun->sense_key = key;
    lun->asc = asc;
    lun->ascq = ascq;
}

static void msc_fail(usbip_function *iface, struct msc_state *st, int lun,
                     const uint8_t tag[BOT_TAG_LEN], uint32_t dlen,
                     uint8_t key, uint8_t asc, uint8_t ascq, const char *label) {
    set_sense(&st->lun[lun], key, asc, ascq);
    msc_log(st, lun, "%s -> CHECK CONDITION %#04x/%#04x", label, asc, ascq);
    msc_stall_data_phase(st, dlen);
    msc_csw(iface, tag, dlen, BOT_STATUS_FAILED);
}

/* The SCSI peripheral device type. A floppy is a direct-access device like any
 * other disk - only its geometry page and its subclass say otherwise. */
static uint8_t inq_pdt(const struct lun_state *lun) {
    return lun->medium == MSC_MEDIUM_CDROM ? INQ_PDT_CDROM : INQ_PDT_DIRECT;
}

/* The product string defaults to the medium type, so INQUIRY and its log line agree. */
static const char *default_product(int medium) {
    if (medium == MSC_MEDIUM_CDROM)
        return MSC_DEFAULT_CDROM_PRODUCT;

    if (medium == MSC_MEDIUM_FLOPPY)
        return MSC_DEFAULT_FLOPPY_PRODUCT;

    return MSC_DEFAULT_PRODUCT;
}

/* Resolve one unit: its own field where set, else the device-wide one. Done once at
 * build time, so nothing downstream repeats the rule. */
static void lun_init(struct lun_state *lun, const msc_lun *src, const msc_opts *opts) {
    lun->user      = src ? src->user : opts->user;
    lun->medium    = src && src->medium ? src->medium : opts->medium;
    lun->read_only = (src && src->read_only) || opts->read_only;
    lun->product   = src && src->product ? src->product : opts->product;
    lun->serial    = src && src->serial ? src->serial : opts->serial;

    if (lun->medium == MSC_MEDIUM_CDROM)
        lun->read_only = 1;               /* a CD-ROM is read-only by definition */

    if (!lun->product)
        lun->product = default_product(lun->medium);

    if (!lun->serial)
        lun->serial = MSC_DEFAULT_SERIAL;

    set_sense(lun, SENSE_OK);
}

/* Index in FLOPPY_FORMATS of the standard format a `blocks` x `bs` medium is, or -1.
 * The caller then borrows FLOPPY_DEFAULT's geometry with the real cylinder count,
 * so an odd image still enumerates. */
static int floppy_format_index(uint32_t blocks, uint32_t bs) {
    if (bs != FLOPPY_BLOCK_SIZE)
        return -1;

    for (size_t i = 0; i < FLOPPY_FORMAT_COUNT; i++)
        if (FLOPPY_FORMATS[i].blocks == blocks)
            return (int)i;

    return -1;
}

/* The medium type code for the mode parameter header (UFI table 17). */
static uint8_t floppy_medium_type(uint32_t blocks, uint32_t bs) {
    int i = floppy_format_index(blocks, bs);
    return i < 0 ? 0x00 : FLOPPY_FORMATS[i].medium_type;
}

/* The Flexible Disk page for the current medium. */
static void build_flex_page(uint8_t buf[FLEX_PAGE_LEN], uint32_t blocks, uint32_t bs) {
    int i = floppy_format_index(blocks, bs);
    uint8_t heads = FLOPPY_FORMATS[FLOPPY_DEFAULT].heads;
    uint8_t sectors = FLOPPY_FORMATS[FLOPPY_DEFAULT].sectors;
    uint16_t rate = FLOPPY_FORMATS[FLOPPY_DEFAULT].rate_kbps;
    uint32_t cylinders;

    if (i >= 0) {
        heads = FLOPPY_FORMATS[i].heads;
        sectors = FLOPPY_FORMATS[i].sectors;
        rate = FLOPPY_FORMATS[i].rate_kbps;
        cylinders = FLOPPY_FORMATS[i].cylinders;
    }
    else
        cylinders = blocks / ((uint32_t)heads * sectors);   /* an odd image: keep the capacity honest */

    if (cylinders > 0xFFFF)
        cylinders = 0xFFFF;

    memset(buf, 0, FLEX_PAGE_LEN);
    buf[0] = MODE_PAGE_FLEX_DISK;
    buf[1] = FLEX_PAGE_DATA_LEN;
    usb_put_be16(buf + FLEX_RATE_OFF, rate);
    buf[FLEX_HEADS_OFF] = heads;
    buf[FLEX_SECTORS_OFF] = sectors;
    usb_put_be16(buf + FLEX_SECTOR_SIZE_OFF, (uint16_t)bs);
    usb_put_be16(buf + FLEX_CYLINDERS_OFF, (uint16_t)cylinders);
    usb_put_be16(buf + FLEX_ROTATION_OFF, FLOPPY_RPM);
}

/* One READ FORMAT CAPACITIES descriptor: block count, type, 24-bit block length. */
static void put_capacity_desc(uint8_t *desc, uint32_t blocks, uint32_t bs, uint8_t type) {
    usb_put_be32(desc, blocks);
    desc[RFC_TYPE_OFF] = type;
    usb_put_be24(desc + RFC_BLOCKLEN_OFF, bs); /* the block length is 24-bit */
}

/* The whole capacity list. Returns its length. */
static int build_format_capacities(const struct lun_state *lun, uint8_t buf[RFC_MAX_LEN],
                                   uint32_t blocks, uint32_t bs) {
    int off = RFC_DESC_OFF;
    memset(buf, 0, RFC_MAX_LEN);
    put_capacity_desc(buf + off, blocks, bs, RFC_TYPE_FORMATTED);
    off += RFC_DESC_LEN;

    if (lun->medium == MSC_MEDIUM_FLOPPY) {
        /* the formats this drive could be asked to format to, largest first */
        for (size_t i = 0; i < FLOPPY_FORMAT_COUNT; i++) {
            put_capacity_desc(buf + off, FLOPPY_FORMATS[i].blocks, FLOPPY_BLOCK_SIZE,
                              RFC_TYPE_FORMATTABLE);
            off += RFC_DESC_LEN;
        }
    }
    buf[RFC_LIST_LEN_OFF] = (uint8_t)(off - RFC_DESC_OFF);
    return off;
}

static void build_inquiry(struct msc_state *st, const struct lun_state *lun, uint8_t buf[INQ_LEN]) {
    msc_opts *opts = &st->opts;
    const char *vendor = opts->vendor ? opts->vendor : MSC_DEFAULT_VENDOR;
    const char *revision = opts->revision ? opts->revision : MSC_DEFAULT_REVISION;

    memset(buf, 0, INQ_LEN);
    buf[0] = inq_pdt(lun);
    buf[1] = INQ_RMB_REMOVABLE;
    buf[2] = INQ_VERSION_SPC2;
    buf[3] = INQ_RESPONSE_FORMAT;
    buf[4] = INQ_ADDITIONAL_LEN;
    set_str(buf + INQ_VENDOR_OFF,   vendor, INQ_VENDOR_LEN);
    set_str(buf + INQ_PRODUCT_OFF,  lun->product, INQ_PRODUCT_LEN);
    set_str(buf + INQ_REVISION_OFF, revision, INQ_REVISION_LEN);
}

/* Build an INQUIRY EVPD page. Returns its length, or -1 for a page we do not publish
 * -- the caller fails those, as page 0x00 tells the host to expect. */
static int build_vpd(const struct lun_state *lun, uint8_t page, uint8_t buf[VPD_MAX_LEN]) {
    memset(buf, 0, VPD_MAX_LEN);
    buf[0] = inq_pdt(lun);
    buf[VPD_PAGE_CODE_OFF] = page;

    if (page == VPD_SUPPORTED_PAGES) {
        buf[VPD_DATA_OFF]     = VPD_SUPPORTED_PAGES;   /* in ascending page order */
        buf[VPD_DATA_OFF + 1] = VPD_UNIT_SERIAL;
        buf[VPD_LENGTH_OFF]   = 2;
        return VPD_DATA_OFF + 2;
    }
    if (page == VPD_UNIT_SERIAL) {
        const char *serial = lun->serial;
        size_t len = strlen(serial);
        if (len > VPD_SERIAL_MAX)
            len = VPD_SERIAL_MAX;
        memcpy(buf + VPD_DATA_OFF, serial, len);
        buf[VPD_LENGTH_OFF] = (uint8_t)len;
        return VPD_DATA_OFF + (int)len;
    }
    return -1;
}

static void dispatch(usbip_function *iface, struct msc_state *st, int lun_index,
                     const uint8_t tag[BOT_TAG_LEN], uint32_t dlen, const uint8_t *cdb) {
    msc_opts *opts = &st->opts;
    struct lun_state *lun = &st->lun[lun_index];
    uint32_t bs = opts->block_size(lun->user);
    uint32_t nb = opts->num_blocks(lun->user);
    uint8_t op = cdb[0];
    const char *op_name = scsi_name(op);   /* the log text for this opcode, looked up once */

    switch (op) {
    case TEST_UNIT_READY:
        msc_log(st, lun_index, "%s -> OK", op_name);
        msc_csw(iface, tag, 0, BOT_STATUS_PASSED);
        break;

    case INQUIRY: {
        if (cdb[1] & INQ_EVPD) {
            uint8_t page = cdb[2];
            uint8_t vpd[VPD_MAX_LEN];
            int built = build_vpd(lun, page, vpd);
            if (built < 0) {
                msc_fail(iface, st, lun_index, tag, dlen, SENSE_INVALID_FIELD, op_name);
                break;
            }
            msc_log(st, lun_index, "%s page %#04x -> %d B", op_name, page, built);
            msc_data_in(iface, tag, dlen, vpd, (uint32_t)built);
            break;
        }
        uint8_t inq[INQ_LEN];
        build_inquiry(st, lun, inq);
        msc_log(st, lun_index, "%s -> %s %s", op_name,
                opts->vendor ? opts->vendor : MSC_DEFAULT_VENDOR, lun->product);
        msc_data_in(iface, tag, dlen, inq, INQ_LEN);
        break;
    }

    case READ_FORMAT_CAPACITIES: {
        uint8_t rfc[RFC_MAX_LEN];
        int built = build_format_capacities(lun, rfc, nb, bs);
        msc_log(st, lun_index, "%s -> %u x %u", op_name, nb, bs);
        msc_data_in(iface, tag, dlen, rfc, (uint32_t)built);
        break;
    }

    case READ_CAPACITY_10: {
        uint8_t cap[READ_CAP_10_LEN];
        usb_put_be32(cap, nb - 1);
        usb_put_be32(cap + 4, bs);
        msc_log(st, lun_index, "%s -> %u x %u (%u KiB)", op_name, nb, bs,
                (unsigned)((uint64_t)nb * bs / 1024));
        msc_data_in(iface, tag, dlen, cap, READ_CAP_10_LEN);
        break;
    }

    case SERVICE_ACTION_IN_16:
        if ((cdb[1] & SAI_MASK) == SAI_READ_CAPACITY_16) {
            uint8_t cap[READ_CAP_16_LEN];
            memset(cap, 0, sizeof(cap));
            usb_put_be32(cap + RC16_LBA_LO_OFF, nb - 1);
            usb_put_be32(cap + RC16_BLOCKLEN_OFF, bs);
            msc_log(st, lun_index, "%s -> %u x %u", op_name, nb, bs);
            msc_data_in(iface, tag, dlen, cap, sizeof(cap));
        } else msc_fail(iface, st, lun_index, tag, dlen, SENSE_INVALID_COMMAND, op_name);
        break;

    case READ_10:
    case READ_12: {
        uint32_t lba = usb_get_be32(cdb + CDB10_LBA_OFF);
        uint32_t count = usb_get_be16(cdb + CDB10_LEN_OFF);

        if (op == READ_12)
            count = usb_get_be32(cdb + CDB12_LEN_OFF);

        if ((uint64_t)lba + count > nb) {
            msc_fail(iface, st, lun_index, tag, dlen, SENSE_LBA_RANGE, op_name);
            break;
        }
        size_t rlen = (size_t)count * bs;
        uint8_t *buf = malloc(rlen ? rlen : 1);
        if (opts->read(lun->user, lba, count, buf) == 0) {
            msc_log(st, lun_index, "%s lba=%u count=%u -> OK (%uB)", op_name, lba, count, count * bs);
            msc_data_in(iface, tag, dlen, buf, count * bs);
        } else msc_fail(iface, st, lun_index, tag, dlen, SENSE_UNRECOVERED_READ, op_name);
        free(buf);
        break;
    }

    case WRITE_10:
    case WRITE_12: {
        uint32_t lba = usb_get_be32(cdb + CDB10_LBA_OFF);
        uint32_t count = usb_get_be16(cdb + CDB10_LEN_OFF);

        if (op == WRITE_12)
            count = usb_get_be32(cdb + CDB12_LEN_OFF);

        if (lun->read_only) {
            msc_fail(iface, st, lun_index, tag, dlen, SENSE_WRITE_PROTECT, op_name);
            break;
        }
        if ((uint64_t)lba + count > nb) {
            msc_fail(iface, st, lun_index, tag, dlen, SENSE_LBA_RANGE, op_name);
            break;
        }
        st->writing = 1;
        st->wop = op;
        st->wlun = (uint8_t)lun_index;
        memcpy(st->wtag, tag, BOT_TAG_LEN);
        st->wlba = lba;
        st->wcount = count;
        st->wneed = count * bs;
        st->wgot = 0;
        st->wbuf = malloc(st->wneed ? st->wneed : 1);
        break;                         /* data arrives in subsequent on_out calls */
    }

    case REQUEST_SENSE: {
        uint8_t sense[SENSE_LEN];
        memset(sense, 0, sizeof(sense));
        sense[0] = SENSE_RESPONSE_CURRENT;
        sense[SENSE_KEY_OFF]      = lun->sense_key;
        sense[SENSE_ADDL_LEN_OFF] = SENSE_ADDITIONAL_LEN;
        sense[SENSE_ASC_OFF]      = lun->asc;
        sense[SENSE_ASCQ_OFF]     = lun->ascq;
        set_sense(lun, SENSE_OK);                        /* sense is cleared once reported */
        msc_log(st, lun_index, "%s", op_name);
        msc_data_in(iface, tag, dlen, sense, sizeof(sense));
        break;
    }

    case MODE_SENSE_6:
    case MODE_SENSE_10: {
        /* The mode parameter header, optionally followed by the pages asked for.
         * header(6):  length, medium type, device-specific, block-descriptor length
         * header(10): 2-byte length, medium type, device-specific, ...
         * The header's own length field counts everything after it. */
        int hdr = op == MODE_SENSE_10 ? MODE10_HDR_LEN : MODE6_HDR_LEN;
        uint8_t page = cdb[2] & MODE_PAGE_MASK;
        uint8_t mode[MODE10_HDR_LEN + FLEX_PAGE_LEN];
        int built = hdr;

        memset(mode, 0, sizeof(mode));
        mode[op == MODE_SENSE_10 ? MODE10_DEV_SPEC_OFF : MODE6_DEV_SPEC_OFF] =
            (uint8_t)(lun->read_only ? MODE_WRITE_PROTECT : 0);

        if (lun->medium == MSC_MEDIUM_FLOPPY) {
            mode[op == MODE_SENSE_10 ? MODE10_MEDIUM_OFF : MODE6_MEDIUM_OFF] =
                floppy_medium_type(nb, bs);
            if (page == MODE_PAGE_FLEX_DISK || page == MODE_PAGE_ALL) {
                build_flex_page(mode + built, nb, bs);
                built += FLEX_PAGE_LEN;
            }
        }
        if (op == MODE_SENSE_10)
            usb_put_be16(mode, (uint16_t)(built - 2));
        else
            mode[0] = (uint8_t)(built - 1);

        msc_log(st, lun_index, "%s page %#04x -> %d B", op_name, page, built);
        msc_data_in(iface, tag, dlen, mode, (uint32_t)built);
        break;
    }

    case FORMAT_UNIT:
        /* Nothing to lay down: the blocks exist and the host writes the filesystem.
         * Refuse on a write-protected medium, where "success" would be a lie. */
        if (lun->read_only)
            msc_fail(iface, st, lun_index, tag, dlen, SENSE_WRITE_PROTECT, op_name);
        else {
            msc_log(st, lun_index, "%s -> OK", op_name);
            msc_csw(iface, tag, 0, BOT_STATUS_PASSED);
        }
        break;

    case PREVENT_ALLOW:
    case START_STOP_UNIT:
    case SYNC_CACHE_10:
    case REZERO_UNIT:
    case SEEK_10:
    case VERIFY_10:
        msc_log(st, lun_index, "%s -> OK", op_name);
        msc_csw(iface, tag, 0, BOT_STATUS_PASSED);
        break;

    case READ_TOC:
        /* CD-ROM only */
        if (lun->medium == MSC_MEDIUM_CDROM) {
            /* TOC header (data length 18, first/last track 1) then two descriptors:
             * track 1 at LBA 0, and the lead-out. */
            uint8_t toc[20] = { 0x00, 18, 0x01, 0x01,
                                0x00, MMC_TOC_ADR_CONTROL, 0x01, 0, 0, 0, 0, 0,
                                0x00, MMC_TOC_ADR_CONTROL, MMC_TOC_LEADOUT, 0, 0, 0, 0, 0 };
            msc_log(st, lun_index, "%s", op_name);
            msc_data_in(iface, tag, dlen, toc, sizeof(toc));
        }
        else
            msc_fail(iface, st, lun_index, tag, dlen, SENSE_INVALID_COMMAND, op_name);
        break;

    case GET_CONFIGURATION:
        /* CD-ROM only */
        if (lun->medium == MSC_MEDIUM_CDROM) {
            uint8_t cfg[8] = { 0,0,0,0, 0,0,0, MMC_PROFILE_CDROM };
            msc_log(st, lun_index, "%s", op_name);
            msc_data_in(iface, tag, dlen, cfg, sizeof(cfg));
        }
        else
            msc_fail(iface, st, lun_index, tag, dlen, SENSE_INVALID_COMMAND, op_name);
        break;

    case GET_EVENT_STATUS: {
        /* no event: event data length 6, notification class 0 with the "no change" bit */
        uint8_t ev[8] = { 0, 6, MMC_EVENT_NO_CHANGE, 0, 0, 0, 0, 0 };
        msc_data_in(iface, tag, dlen, ev, sizeof(ev));
        break;
    }
    default:
        msc_fail(iface, st, lun_index, tag, dlen, SENSE_INVALID_COMMAND, op_name);
    }
}

/* ---- usbip_device_class vtable ---- */
static int msc_build(usbip_function *func, const void *params) {
    struct msc_state *st = usbip_function_state(func);
    st->opts = *(const msc_opts *)params;
    st->writing = 0;
    st->wbuf = NULL;

    if (st->opts.n_luns > MSC_MAX_LUNS)
        return -1;                      /* more units than the transport can address */

    /* No units declared is the everyday case: one, backed by opts.user. */
    st->n_luns = st->opts.n_luns > 0 ? st->opts.n_luns : 1;
    for (int i = 0; i < st->n_luns; i++)
    {
        const msc_lun *declared = st->opts.n_luns > 0 ? &st->opts.luns[i] : NULL;

        lun_init(&st->lun[i], declared, &st->opts);
    }

    usbip_device *dev = usbip_function_device(func);
    int istr = st->opts.name ? usbip_device_add_string(dev, st->opts.name) : 0;

    st->pub.index  = usbip_function_instance(func);
    st->pub.n_luns = st->n_luns;
    st->pub.medium = st->lun[0].medium;
    st->pub.name  = st->opts.name ? st->opts.name : "";
    st->pub.user  = st->opts.user;
    st->pub.dev   = dev;
    st->pub.func  = func;

    int speed = usbip_device_get_speed(dev);
    uint16_t bulk_mps = (speed >= USB_SPEED_HIGH) ? 512 : 64;  /* HS bulk = 512 */
    uint8_t subclass = st->opts.ufi ? MSC_SUBCLASS_UFI : MSC_SUBCLASS_SCSI;
    usbip_interface *iface = usbip_function_add_interface(func, USB_CLASS_MSC, subclass, MSC_PROTOCOL_BOT);
    st->pub.interface_number = usbip_interface_number(iface);
    usbip_interface_set_string(iface, (uint8_t)istr);

    st->in = usbip_interface_add_endpoint(iface, &(usb_endpoint_descriptor){
        .bDescriptorType = USB_DT_ENDPOINT,
        .bEndpointAddress = MSC_EP_IN,
        .bmAttributes = USB_BULK,
        .wMaxPacketSize = bulk_mps 
    });
    st->out = usbip_interface_add_endpoint(iface, &(usb_endpoint_descriptor){
        .bDescriptorType = USB_DT_ENDPOINT,
        .bEndpointAddress = MSC_EP_OUT,
        .bmAttributes = USB_BULK,
        .wMaxPacketSize = bulk_mps 
    });
    st->pub.in_ep  = usbip_endpoint_address(st->in);
    st->pub.out_ep = usbip_endpoint_address(st->out);
    return 0;
}

static int msc_control(usbip_function *iface, const usb_setup *setup, uint8_t *buf, uint16_t len) {
    (void)len;
    struct msc_state *st = usbip_function_state(iface);
    if (setup->bRequest == BOT_GET_MAX_LUN) {
        buf[0] = (uint8_t)(st->n_luns - 1);   /* the answer is the LAST LUN's number */
        return 1;
    }
    if (setup->bRequest == BOT_RESET) {
        st->writing = 0;
        free(st->wbuf);
        st->wbuf = NULL;
        return 0;
    }
    return -1;
}

static void msc_on_out(usbip_function *iface, usbip_ep *ep, const void *data, int len) {
    (void)ep;
    struct msc_state *st = usbip_function_state(iface);
    const uint8_t *bytes = data;

    if (st->writing) {
        uint32_t take = (uint32_t)len;
        if (st->wgot + take > st->wneed)
            take = st->wneed - st->wgot;

        memcpy(st->wbuf + st->wgot, bytes, take);
        st->wgot += take;
        if (st->wgot >= st->wneed) {
            int rc = st->opts.write(st->lun[st->wlun].user, st->wlba, st->wcount, st->wbuf);
            const char *op_name = scsi_name(st->wop);
            uint8_t status = (rc == 0) ? BOT_STATUS_PASSED : BOT_STATUS_FAILED;

            msc_log(st, st->wlun, "%s lba=%u count=%u -> OK (%uB)", op_name, st->wlba, st->wcount,
                    st->wneed);
            msc_csw(iface, st->wtag, 0, status);
            free(st->wbuf);
            st->wbuf = NULL;
            st->writing = 0;
        }
        return;
    }
    if (len != BOT_CBW_LEN || memcmp(bytes, BOT_CBW_SIG, BOT_SIG_LEN) != 0)
        return;   /* invalid CBW: drop */

    uint8_t tag[BOT_TAG_LEN];
    memcpy(tag, bytes + BOT_CBW_TAG_OFF, BOT_TAG_LEN);
    uint32_t dlen = usb_get_le32(bytes + BOT_CBW_DATALEN_OFF);
    int lun = bytes[BOT_CBW_LUN_OFF] & BOT_CBW_LUN_MASK;
    st->cbw_in = (bytes[BOT_CBW_FLAGS_OFF] & BOT_CBW_FLAG_IN) != 0;

    if (lun >= st->n_luns) {
        /* No such unit. The sense goes on unit 0 -- the one addressed does not exist
         * to hold it, and unit 0 is where a host looks after this CSW. */
        char label[32];
        const char *op_name = scsi_name(bytes[BOT_CBW_CDB_OFF]);

        snprintf(label, sizeof(label), "lun%d %s", lun, op_name);
        msc_fail(iface, st, 0, tag, dlen, SENSE_LUN_NOT_SUPPORTED, label);
        return;
    }
    dispatch(iface, st, lun, tag, dlen, bytes + BOT_CBW_CDB_OFF);
}

const usbip_device_class usbip_device_msc = {
    .name = "msc",
    .bInterfaceClass = USB_CLASS_MSC,
    .build = msc_build,
    .control = msc_control,
    .on_out = msc_on_out,
    .state_size = sizeof(struct msc_state),
};

uint64_t msc_floppy_format_size(int format) {
    for (size_t i = 0; i < FLOPPY_FORMAT_COUNT; i++)
        if (FLOPPY_FORMATS[i].format == format)
            return (uint64_t)FLOPPY_FORMATS[i].blocks * FLOPPY_BLOCK_SIZE;

    return 0;
}

msc_disk *msc_add(usbip_device *dev, const msc_opts *opts) {
    usbip_function *func = usbip_device_add_class(dev, &usbip_device_msc, opts);
    return func ? disk_of(func) : NULL;
}
