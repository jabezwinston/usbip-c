/**
 * Copyright (C) 2026 Jabez Winston
 *
 * SPDX-License-Identifier: MIT
 *
 * Date : 05-June-2026
 *
 * pcap.c - optional USB-traffic capture as PCAPNG (DLT_USB_LINUX_MMAPPED, 220).
 *
 * The byte-for-byte twin of the Python library's usbip/pcap.py: each USB transfer is logged as
 * the Linux usbmon Submit ('S') / Complete ('C') event pair, so Wireshark's USB
 * and class dissectors light up exactly as they would for a real usbmon trace.
 * The Section Header / Interface blocks are stamped with identifying options
 * (shb_userappl = "USBIP <version>", shb_os/shb_hardware from uname,
 * if_name/if_description) so Wireshark/capinfos show context.
 * Lazily enabled from USBIP_PCAPNG (see pcap.h). Observes only; never alters.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h> /* strcasecmp */
#include <stdint.h>
#include <time.h>
#include <unistd.h>
#include <pthread.h>
#ifndef _WIN32
#include <sys/utsname.h>
#endif

#include "pcap.h"
#include "usbip_device_internal.h"

#define LINKTYPE_USB_LINUX_MMAPPED 220

static FILE *g_fp;
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static int g_checked; /* have we consulted USBIP_PCAPNG? */

/* ---- pcapng blocks (g_lock held by the caller) ------------------------ */
static void write_block(uint32_t btype, const uint8_t *body, int body_len)
{
    uint8_t hdr[8], trailer[4];
    uint32_t total = (uint32_t)body_len + 12; /* type + len + body + len */
    usb_put_le32(hdr, btype);
    usb_put_le32(hdr + 4, total);
    usb_put_le32(trailer, total);
    fwrite(hdr, 1, 8, g_fp);
    fwrite(body, 1, (size_t)body_len, g_fp);
    fwrite(trailer, 1, 4, g_fp);
}

/* one option TLV: u16 code, u16 length, value, padded to 4 bytes. Returns new off.
 * SHB option codes: shb_hardware=2, shb_os=3, shb_userappl=4.
 * IDB option codes: if_name=2, if_description=3, if_tsresol=9. */
static int put_opt(uint8_t *buf, int off, uint16_t code, const void *val, int len)
{
    int pad = (-len) & 3;
    usb_put_le16(buf + off, code);
    usb_put_le16(buf + off + 2, (uint16_t)len);
    if (len)
        memcpy(buf + off + 4, val, (size_t)len);
    memset(buf + off + 4 + len, 0, (size_t)pad);
    return off + 4 + len + pad;
}

static int put_stropt(uint8_t *buf, int off, uint16_t code, const char *str)
{
    return put_opt(buf, off, code, str, (int)strlen(str));
}

static void write_shb(void) /* Section Header Block */
{
    uint8_t body[512];
    int off;
#ifndef _WIN32
    struct utsname uts;
#endif
    char osbuf[256], app[256];

    usb_put_le32(body, 0x1A2B3C4D); /* byte-order magic (LE) */
    usb_put_le16(body + 4, 1);
    usb_put_le16(body + 6, 0); /* version 1.0 */
    usb_put_le32(body + 8, 0xFFFFFFFF);
    usb_put_le32(body + 12, 0xFFFFFFFF); /* section length = -1 */
    off = 16;

#ifdef _WIN32
    snprintf(osbuf, sizeof(osbuf), "Windows"); /* shb_os (no uname on Windows) */
#else
    uname(&uts);
    snprintf(osbuf, sizeof(osbuf), "%s %s", uts.sysname, uts.release);
#endif
    snprintf(app, sizeof(app), "USBIP %s (github.com/jabezwinston/usbip-c)", USBIP_VERSION);
#ifdef _WIN32
    off = put_stropt(body, off, 2, "x86"); /* shb_hardware */
#else
    off = put_stropt(body, off, 2, uts.machine); /* shb_hardware (e.g. x86_64) */
#endif
    off = put_stropt(body, off, 3, osbuf); /* shb_os       (e.g. Linux 6.x) */
    off = put_stropt(body, off, 4, app);   /* shb_userappl */
    usb_put_le16(body + off, 0);
    usb_put_le16(body + off + 2, 0);
    off += 4; /* opt_endofopt */

    write_block(0x0A0D0D0A, body, off);
}

static void write_idb(void) /* Interface Description Block */
{
    uint8_t body[256], tsresol = 6;
    int off;
    usb_put_le16(body, LINKTYPE_USB_LINUX_MMAPPED);
    usb_put_le16(body + 2, 0);
    usb_put_le32(body + 4, 0); /* snaplen 0 */
    off = 8;
    off = put_stropt(body, off, 2, "usbmon");               /* if_name */
    off = put_stropt(body, off, 3, "Virtual USB (USB/IP)"); /* if_description */
    off = put_opt(body, off, 9, &tsresol, 1);               /* if_tsresol = 10^-6 s */
    usb_put_le16(body + off, 0);
    usb_put_le16(body + off + 2, 0);
    off += 4; /* opt_endofopt */
    write_block(0x00000001, body, off);
}

static void write_epb(const uint8_t *data, int caplen, uint64_t ts_usec) /* Enhanced Packet Block */
{
    int pad = (-caplen) & 3;
    int body_len = 20 + caplen + pad;
    uint8_t *body = malloc((size_t)body_len);
    if (!body)
        return;
    usb_put_le32(body, 0);                             /* interface id 0 */
    usb_put_le32(body + 4, (uint32_t)(ts_usec >> 32)); /* timestamp high */
    usb_put_le32(body + 8, (uint32_t)ts_usec);         /* timestamp low */
    usb_put_le32(body + 12, (uint32_t)caplen);         /* captured len */
    usb_put_le32(body + 16, (uint32_t)caplen);         /* original len */
    memcpy(body + 20, data, (size_t)caplen);
    memset(body + 20 + caplen, 0, (size_t)pad);
    write_block(0x00000006, body, body_len);
    free(body);
}

/* ---- enable / disable ------------------------------------------------- */
static int is_flag(const char *str) /* "turn it on" without a path */
{
    return !str || !*str || !strcmp(str, "1") || !strcasecmp(str, "on") || !strcasecmp(str, "yes") || !strcasecmp(str, "true") || !strcasecmp(str, "auto");
}

static void default_name(char *out, size_t cap)
{
    const char *prog = usbip_progname();
#ifdef _WIN32
    if (prog)
    { /* Windows hands back the full exe path: strip dir, then ".exe" */
        const char *sep;
        if ((sep = strrchr(prog, '\\')))
            prog = sep + 1;
        if ((sep = strrchr(prog, '/')))
            prog = sep + 1;
    }
    char stem[256];
    if (prog && *prog)
    {
        snprintf(stem, sizeof(stem), "%s", prog);
        char *dot = strrchr(stem, '.');
        if (dot && !strcasecmp(dot, ".exe"))
            *dot = '\0';
        prog = stem;
    }
#endif
    if (!prog || !*prog)
        prog = "usb_traffic";
    snprintf(out, cap, "%s.pcapng", prog);
}

void usbip_pcap_open(const char *path)
{
    char name[512];
    pthread_mutex_lock(&g_lock);
    g_checked = 1;
    if (g_fp)
    {
        pthread_mutex_unlock(&g_lock);
        return;
    }
    if (is_flag(path))
    {
        default_name(name, sizeof(name));
    }
    else
    {
        const char *pc = strstr(path, "%p"); /* expand %p -> pid */
        if (pc)
            snprintf(name, sizeof(name), "%.*s%d%s", (int)(pc - path), path, (int)getpid(), pc + 2);
        else
            snprintf(name, sizeof(name), "%s", path);
    }
    g_fp = fopen(name, "wb");
    if (g_fp)
    {
        write_shb();
        write_idb();
        fflush(g_fp);
    }
    pthread_mutex_unlock(&g_lock);
}

void usbip_pcap_close(void)
{
    pthread_mutex_lock(&g_lock);
    if (g_fp)
    {
        fclose(g_fp);
        g_fp = NULL;
    }
    pthread_mutex_unlock(&g_lock);
}

static void ensure(void) /* honor USBIP_PCAPNG once */
{
    if (g_checked)
        return;
    const char *env = getenv("USBIP_PCAPNG");
    if (env)
        usbip_pcap_open(env); /* sets g_checked */
    else
        g_checked = 1;
}

int usbip_pcap_enabled(void)
{
    ensure();
    return g_fp != NULL;
}

/* ---- one usbmon event ------------------------------------------------- */
void usbip_pcap_packet(uint32_t devid, uint32_t seqnum, int xfer_type, char event,
                       int ep_addr, int32_t status, const uint8_t *setup,
                       const uint8_t *data, int data_len, int urb_len)
{
    ensure();
    if (!g_fp)
        return;
    if (data_len < 0)
        data_len = 0;

    struct timespec tspec;
    clock_gettime(CLOCK_REALTIME, &tspec);
    int64_t sec = (int64_t)tspec.tv_sec;
    int32_t usec = (int32_t)(tspec.tv_nsec / 1000);

    uint8_t hdr[64]; /* usbmon mmapped header */
    memset(hdr, 0, sizeof(hdr));
    usb_put_le64(hdr, seqnum);                                  /* id (pairs S/C) */
    hdr[8] = (uint8_t)event;                                    /* 'S' or 'C' */
    hdr[9] = (uint8_t)xfer_type;                                /* usbmon: iso0 intr1 ctrl2 bulk3 */
    hdr[10] = (uint8_t)ep_addr;                                 /* incl. direction bit */
    hdr[11] = (uint8_t)(devid & 0xFF);                          /* device_address */
    usb_put_le16(hdr + 12, (uint16_t)((devid >> 16) & 0xFFFF)); /* bus_id */
    hdr[14] = setup ? 0 : 0x2D;                                 /* setup_flag (0 = present) */
    hdr[15] = data_len > 0 ? 0 : 0x2D;                          /* data_flag  (0 = present) */
    usb_put_le64(hdr + 16, (uint64_t)sec);                      /* ts_sec (s64) */
    usb_put_le32(hdr + 24, (uint32_t)usec);                     /* ts_usec */
    usb_put_le32(hdr + 28, (uint32_t)status);
    usb_put_le32(hdr + 32, (uint32_t)urb_len);                  /* urb_len */
    usb_put_le32(hdr + 36, (uint32_t)data_len);                 /* data_len */
    if (setup)
        memcpy(hdr + 40, setup, 8); /* setup[8] (else zero) */
    /* hdr+48 interval, +52 start_frame, +56 xfer_flags, +60 ndesc: all zero */

    int caplen = 64 + data_len;
    uint8_t *rec = malloc((size_t)caplen);
    if (!rec)
        return;
    memcpy(rec, hdr, 64);
    if (data_len > 0 && data)
        memcpy(rec + 64, data, (size_t)data_len);

    uint64_t ts_usec = (uint64_t)sec * 1000000ull + (uint64_t)usec;
    pthread_mutex_lock(&g_lock);
    if (g_fp)
    {
        write_epb(rec, caplen, ts_usec);
        fflush(g_fp);
    }
    pthread_mutex_unlock(&g_lock);
    free(rec);
}
