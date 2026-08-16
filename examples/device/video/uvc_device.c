/**
 * Copyright (C) 2026 Jabez Winston
 *
 * SPDX-License-Identifier: MIT
 *
 * Date : 04-June-2026
 *
 * uvc_device.c - virtual USB webcam (UVC) over USB/IP, isochronous streaming.
 *
 *   ./uvc_device                              # 320x240 YUY2 color bars, synthetic
 *   ./uvc_device --width 640 --height 480 --fps 15
 *   ./uvc_device --format mjpeg               # advertise MJPEG (needs encoder build)
 *   ./uvc_device --source file --file cap.yuyv
 *   attach it with a USB/IP client
 *   (Linux: sudo modprobe vhci-hcd ; sudo usbip attach -r 127.0.0.1 -b 1-1)
 *   then open it in any app that lists cameras
 *   (Linux: ffmpeg -f v4l2 -i /dev/videoN -frames 1 shot.png)
 */
#define _GNU_SOURCE
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "_support/cmdline.h"
#include "_support/logging.h"
#include "classes/uvc.h"
#include "jpeg.h"                /* example-local baseline JPEG encoder (MJPEG) */

/* ---- frame source + grouped logger, carried as uvc_opts.user ---- */
struct app {
    FILE    *fp;                 /* raw-YUYV file source, or NULL for synthetic */
    int      width, height;
    long     frame_bytes;        /* raw YUY2 frame size */
    uint8_t *yuv;                /* scratch YUYV before JPEG encoding */
    usbip_logger log;            /* grouped per-frame logger */
};

/* Frame source: obtain a YUYV frame (from file or the class colour-bar generator)
 * and, for MJPEG, encode it here in the app. Returns -1 for synthetic YUYV, which
 * the class then generates directly. */
static int app_next_frame(void *user, uint8_t *buf, int cap, char fmt, uint32_t idx) {
    struct app *app = user;
    const uint8_t *yuyv;
    if (app->fp) {                                  /* file: raw YUYV frames, looping */
        size_t got = fread(app->yuv, 1, (size_t)app->frame_bytes, app->fp);
        if (got < (size_t)app->frame_bytes) {
            rewind(app->fp);
            got = fread(app->yuv, 1, (size_t)app->frame_bytes, app->fp);
        }
        if (got != (size_t)app->frame_bytes) return -1;
        yuyv = app->yuv;
    } else {
        if (fmt != 'M') return -1;                  /* synthetic YUYV -> let the class do it */
        uvc_color_bars_yuyv(app->yuv, app->width, app->height, idx);
        yuyv = app->yuv;
    }
    if (fmt == 'M')
        return jpeg_encode_yuyv(yuyv, app->width, app->height, 75, buf, cap);
    if ((long)cap < app->frame_bytes) return -1;
    memcpy(buf, yuyv, (size_t)app->frame_bytes);
    return (int)app->frame_bytes;
}

/* uvc on_event sink: group consecutive identical per-frame lines (default " (" key). */
static void app_log(void *user, const char *text) {
    usbip_log_cb(&((struct app *)user)->log, text);
}

/* uvc-specific options. --height and --source are long-only: their natural short
 * letters (h, S) are reserved for the common --help / --high-speed. */
#define OPT_HEIGHT 256  /* --height: long-only option id (past the ASCII range) */
#define OPT_SOURCE 257  /* --source: long-only option id                        */

struct uvc_cli {
    int width, height, fps;
    const char *fmt, *source, *file;
};

static int uvc_on_opt(int c, char *arg, void *u) {
    struct uvc_cli *v = u;
    switch (c) {
    case 'w':        v->width  = atoi(arg); return 1;
    case OPT_HEIGHT: v->height = atoi(arg); return 1;
    case 'r':        v->fps    = atoi(arg); return 1;
    case 'F':        v->fmt    = arg;       return 1;
    case OPT_SOURCE: v->source = arg;       return 1;
    case 'f':        v->file   = arg; v->source = "file"; return 1;
    }
    return 0;
}

int main(int argc, char **argv) {
    struct uvc_cli cli = {
        .width = 320,
        .height = 240,
        .fps = 15,
        .fmt = "yuyv",
        .source = "synthetic",
    };
    static const cmdline_option options[] = {
        {"width",  CMDLINE_ARG_REQUIRED, 'w',        "N", "frame width (default 320)"},
        {"height", CMDLINE_ARG_REQUIRED, OPT_HEIGHT, "N", "frame height (default 240)"},
        {"fps",    CMDLINE_ARG_REQUIRED, 'r',        "N", "frame rate (default 15)"},
        {"format", CMDLINE_ARG_REQUIRED, 'F', "yuyv|mjpeg|both", "pixel format(s) (default yuyv)"},
        {"source", CMDLINE_ARG_REQUIRED, OPT_SOURCE, "synthetic|file", "frame source (default synthetic)"},
        {"file",   CMDLINE_ARG_REQUIRED, 'f',        "F", "raw YUYV frames (--source file)"},
        {0, 0, 0, 0, 0},
    };
    cmdline_opts cli_opts = cmdline_parse(argc, argv, &(cmdline_spec){
        .vid = 0x1209,
        .pid = 0x000e,
        .high_speed_opt = 1,
        .options = options,
        .on_opt = uvc_on_opt,
        .user = &cli,
    });

    int width = cli.width, height = cli.height, fps = cli.fps;
    const char *fmt = cli.fmt, *source = cli.source, *file = cli.file;

    unsigned formats = 0;
    if (strcmp(fmt, "yuyv") == 0)
        formats = UVC_HAS_YUYV;
    else if (strcmp(fmt, "mjpeg") == 0) 
        formats = UVC_HAS_MJPEG;
    else if (strcmp(fmt, "both") == 0)  
        formats = UVC_HAS_YUYV | UVC_HAS_MJPEG;
    else {
        fprintf(stderr, "unknown --format %s\n", fmt);
        return 2;
    }

    struct app app = {
        .width = width,
        .height = height,
        .frame_bytes = (long)width * height * 2,
    };
    usbip_log_init(&app.log, "uvc", cli_opts.verbose);

    app.yuv = malloc((size_t)app.frame_bytes);
    if (!app.yuv) {
        fprintf(stderr, "out of memory\n");
        return 1;
    }
    if (strcmp(source, "file") == 0) {
        if (!file) {
            fprintf(stderr, "--source file needs --file\n");
            return 2;
        }
        app.fp = fopen(file, "rb");
        if (!app.fp) {
            perror("fopen");
            return 1;
        }
    }

    /* next_frame always set: it encodes MJPEG and reads files; it returns -1 for
     * synthetic YUYV so the class generates color bars directly. */
    //! [add]
    uvc_opts uo = {
        .width = (uint16_t)width,
        .height = (uint16_t)height,
        .fps = (uint32_t)fps,
        .formats = formats,
        .next_frame = app_next_frame,
        .on_event = app_log,
        .user = &app,
    };

    usbip_device *dev = usbip_device_create(cli_opts.vid, cli_opts.pid);
    usbip_device_set_strings(dev, "USB over IP", "USBIP Camera", "000e");
    usb_speed speed = cli_opts.high_speed ? USB_SPEED_HIGH : USB_SPEED_FULL;   /* HS = 1024 B iso */

    usbip_device_set_speed(dev, speed);
    uvc_cam *cam = uvc_add(dev, &uo);   /* next_frame = NULL -> built-in colour bars */
    //! [add]

    if (!cam) {
        fprintf(stderr, "uvc_add failed\n");
        return 1;
    }

    fprintf(stderr, "[uvc] %s %dx%d @ %dfps, %s source  (%04x:%04x, %s, on %s:%d)\n",
            fmt, width, height, fps, source, cli_opts.vid, cli_opts.pid,
            cli_opts.high_speed ? "high speed" : "full speed", cli_opts.host, cli_opts.port);
    fprintf(stderr, "[uvc] attach: sudo usbip attach -r 127.0.0.1 -b 1-1\n");

    usb_transport *transport = usbip_transport(cli_opts.host, cli_opts.port);
    int rc = usbip_device_plug(dev, transport);

    if (rc != USB_SUCCESS) {
        fprintf(stderr, "usbip_device_plug failed (port %d in use?)\n", cli_opts.port);
        return 1;
    }
#ifdef _WIN32
    for (;;) sleep(1);          /* no pause() on Windows; idle */
#else
    for (;;) pause();
#endif
    return 0;
}
