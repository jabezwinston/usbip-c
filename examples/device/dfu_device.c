/**
 * Copyright (C) 2026 Jabez Winston
 *
 * SPDX-License-Identifier: MIT
 *
 * Date : 04-June-2026
 *
 * dfu_device.c - virtual USB DFU device over USB/IP (file-backed targets).
 *
 *   ./dfu_device                                  # alt0 ./dfu_fw0.bin + alt1 ./dfu_fw1.bin
 *   ./dfu_device --alt app:app.bin                # one target named "app"
 *   ./dfu_device --alt fw:firmware.bin --transfer-size 4096
 *   ./dfu_device --no-winusb --verbose
 *   attach it with a USB/IP client
 *   (Linux: sudo modprobe vhci-hcd ; sudo usbip attach -r 127.0.0.1 -b 1-1)
 *       dfu-util -l            # lists the alternate settings
 *       dfu-util -a 0 -D fw.bin   /   dfu-util -a 0 -U out.bin
 *
 * WinUSB MS-OS descriptors are advertised by default so dfu-util works on
 * Windows without installing a driver via Zadig.
 */
#define _GNU_SOURCE
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "_support/cmdline.h"
#include "_support/logging.h"
#include "classes/dfu.h"

#define MAX_TARGETS 16
#define DEFAULT_TARGET0 "dfu_fw0.bin"
#define DEFAULT_TARGET1 "dfu_fw1.bin"

/* ---- DFU target backend: the class is backend-agnostic, the file specifics live
 *      here. A target is a linear byte store tracking how much is stored, so
 *      UPLOAD knows where the image ends. ---- */

/* File: read/write/seek a stdio stream; finish truncates it to the image size */
struct file_backing
{
    FILE *fp;
    uint32_t length;
};

static int file_write(void *ctx, uint32_t off, const uint8_t *data, uint32_t len)
{
    struct file_backing *fb = ctx;

    if (!fb->fp || fseek(fb->fp, (long)off, SEEK_SET) != 0)
        return DFU_STATUS_errWRITE;

    if (fwrite(data, 1, len, fb->fp) != len) /* I/O error / disk full */
        return DFU_STATUS_errWRITE;          /* -> dfu-util reports "unable to write memory" */

    if (off + len > fb->length)
        fb->length = off + len;

    return DFU_STATUS_OK;
}
static int file_read(void *ctx, uint32_t off, uint8_t *buf, uint32_t want)
{
    struct file_backing *fb = ctx;

    if (off >= fb->length)
        return 0;

    if (want > fb->length - off)
        want = fb->length - off;

    if (!fb->fp || fseek(fb->fp, (long)off, SEEK_SET) != 0)
        return 0;

    return (int)fread(buf, 1, want, fb->fp);
}
static void file_begin(void *ctx) {
    ((struct file_backing *)ctx)->length = 0; 
}

static void file_finish(void *ctx)
{
    struct file_backing *fb = ctx;
    if (!fb->fp)
        return;

    fflush(fb->fp);

    if (ftruncate(fileno(fb->fp), (off_t)fb->length) != 0)
    { /* best-effort */
    }
    fflush(fb->fp);
}
static uint32_t file_size(void *ctx) { return ((struct file_backing *)ctx)->length; }

/* ---- target constructor (allocate a backing + wire the callbacks) ---- */
static void make_file(dfu_target *t, const char *name, const char *path)
{
    struct file_backing *fb = calloc(1, sizeof(*fb));
    fb->fp = fopen(path, "r+b"); /* keep existing contents if any */

    if (!fb->fp)
        fb->fp = fopen(path, "w+b");

    if (fb->fp)
    {
        fseek(fb->fp, 0, SEEK_END);
        long sz = ftell(fb->fp);
        fb->length = sz > 0 ? (uint32_t)sz : 0;
    }

    *t = (dfu_target){
        .name   = name,
        .ctx    = fb,
        .write  = file_write,
        .read   = file_read,
        .begin  = file_begin,
        .finish = file_finish,
        .size   = file_size,
    };
}

/* parse "NAME:PATH" into a target (spec is mutated; must outlive use). Split at the
 * FIRST colon only, so a Windows path keeps its drive letter. */
static int parse_spec(char *spec, dfu_target *t)
{
    char *path = strchr(spec, ':');

    if (!path || !path[1])
        return -1;

    *path++ = 0;
    make_file(t, spec, path);
    return 0;
}

/* dfu-specific options (the common ones live in _support/cmdline). The --alt specs
 * are collected here and turned into targets in main(). */
struct dfu_cli
{
    const char *specs[MAX_TARGETS];
    int n_specs;
    uint16_t transfer_size;
    int winusb;
};

static int dfu_on_opt(int c, char *arg, void *u)
{
    struct dfu_cli *d = u;
    switch (c)
    {
        case 'a':
            if (d->n_specs >= MAX_TARGETS)
            {
                fprintf(stderr, "too many --alt targets\n");
                exit(2);
            }
            d->specs[d->n_specs++] = arg;
            return 1;

        case 't':
            d->transfer_size = (uint16_t)strtol(arg, NULL, 0);
            return 1;

        case 'w':
            d->winusb = 0;
            return 1;
    }
    return 0;
}

int main(int argc, char **argv)
{
    struct dfu_cli cli = {
        .transfer_size = 1024,
        .winusb = 1,
    };

    static const cmdline_option options[] = {
        {"alt", CMDLINE_ARG_REQUIRED, 'a', "NAME:PATH", "add a file-backed target (repeatable; default:\n fw0 -> " DEFAULT_TARGET0 ", fw1 -> " DEFAULT_TARGET1 ")"},
        {"transfer-size", CMDLINE_ARG_REQUIRED, 't', "N", "DFU transfer block size (default 1024)"},
        {"no-winusb", CMDLINE_ARG_NONE, 'w', NULL, "don't advertise WinUSB MS-OS descriptors"},
        {0, 0, 0, 0, 0},
    };
    cmdline_opts cli_opts = cmdline_parse(argc, argv, &(cmdline_spec){
        .vid = 0x1209,
        .pid = 0x000f,
        .options = options,
        .on_opt = dfu_on_opt,
        .user = &cli,
    });

    /* build the targets (their file backend lives above, in this example) */
    dfu_target targets[MAX_TARGETS];
    char descr[MAX_TARGETS][1100];
    int n_targets = 0;

    if (cli.n_specs == 0)
    { /* two targets, so `dfu-util -a N` has something to choose between */
        make_file(&targets[n_targets], "fw0", DEFAULT_TARGET0);
        snprintf(descr[n_targets], sizeof(descr[0]), "fw0 (file %s)", DEFAULT_TARGET0);
        n_targets++;
        make_file(&targets[n_targets], "fw1", DEFAULT_TARGET1);
        snprintf(descr[n_targets], sizeof(descr[0]), "fw1 (file %s)", DEFAULT_TARGET1);
        n_targets++;
    }
    else
    {
        for (int i = 0; i < cli.n_specs; i++)
        {
            if (parse_spec(strdup(cli.specs[i]), &targets[n_targets]) != 0)
            {
                fprintf(stderr, "bad --alt '%s' (use NAME:PATH)\n", cli.specs[i]);
                return 2;
            }
            snprintf(descr[n_targets], sizeof(descr[0]), "%s", cli.specs[i]);
            n_targets++;
        }
    }

    usbip_logger logger;
    usbip_log_init(&logger, "dfu", cli_opts.verbose);

    //! [add]
    dfu_opts dopts = {
        .targets = targets,
        .n_targets = n_targets,
        .transfer_size = cli.transfer_size,
        .attributes = 0, /* 0 -> default 0x07 */
        .winusb = cli.winusb,
        .on_event = usbip_log_cb,
        .user = &logger,
    };

    usbip_device *dev = usbip_device_create(cli_opts.vid, cli_opts.pid);
    usbip_device_set_strings(dev, "USB over IP", "USBIP DFU", "000f");
    dfu_iface *dfu = dfu_add(dev, &dopts);          /* dfu-util -a <n> -U / -D */
    //! [add]

    if (!dfu)
    {
        fprintf(stderr, "dfu_add failed\n");
        return 1;
    }

    const char *winusb = cli.winusb ? "on" : "off";

    fprintf(stderr, "[dfu] %d target(s), transfer-size %u, WinUSB %s  (%04x:%04x on %s:%d)\n",
            n_targets, cli.transfer_size, winusb,
            cli_opts.vid, cli_opts.pid, cli_opts.host, cli_opts.port);

    for (int i = 0; i < n_targets; i++)
        fprintf(stderr, "[dfu]   alt %d: %s\n", i, descr[i]);

    fprintf(stderr, "[dfu] attach: sudo usbip attach -r 127.0.0.1 -b 1-1\n");
    fprintf(stderr, "[dfu] then:   dfu-util -l  |  dfu-util -a 0 -D fw.bin  |  dfu-util -a 0 -U out.bin\n");

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
