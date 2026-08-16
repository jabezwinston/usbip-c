/**
 * Copyright (C) 2026 Jabez Winston
 *
 * SPDX-License-Identifier: MIT
 *
 * Date : 04-June-2026
 *
 * uvc.c - USB Video Class (webcam) device class: isochronous streaming.
 *
 * Built ONLY on the public API (usbip_device.h / classes/uvc.h). Presents a VideoControl
 * + VideoStreaming interface pair (grouped by an IAD), advertises YUY2 and/or
 * MJPEG at one resolution, runs Probe/Commit negotiation, and streams UVC
 * payloads over an isochronous IN endpoint. Mirrors the Python classes/device/uvc.py.
 *
 * Spec: USB Video Class 1.1/1.5; descriptor layout follows the kernel UVC gadget
 * (drivers/usb/gadget/legacy/webcam.c) and uapi/linux/usb/video.h.
 */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "classes/uvc.h"

/* uvc_cam is this class's public handle: the function, retyped so the compiler
 * can tell it from another class's handle. */
static inline uvc_cam *handle_of(usbip_function *func) {
    return (uvc_cam *)func;
}
/* ---- UVC constants (class-specific, defined here - core stays class-free) ---- */
#define UVC_CC_VIDEO            USB_CLASS_VIDEO
#define UVC_SC_VIDEOCONTROL     0x01
#define UVC_SC_VIDEOSTREAMING   0x02
#define UVC_SC_COLLECTION       0x03
#define UVC_PC_PROTOCOL_UNDEFINED 0x00  /* bInterfaceProtocol, UVC 1.1 Sec.A.3 */
#define UVC_IAD_IFACE_COUNT     2       /* the association covers VideoControl + VideoStreaming */
#define UVC_VC_HEADER           0x01
#define UVC_VC_INPUT_TERMINAL   0x02
#define UVC_VC_OUTPUT_TERMINAL  0x03
#define UVC_VS_INPUT_HEADER     0x01
#define UVC_VS_FORMAT_UNCOMP    0x04
#define UVC_VS_FRAME_UNCOMP     0x05
#define UVC_VS_FORMAT_MJPEG     0x06
#define UVC_VS_FRAME_MJPEG      0x07
#define UVC_VS_COLORFORMAT      0x0D
#define UVC_ITT_CAMERA          0x0201
#define UVC_TT_STREAMING        0x0101
#define UVC_VER_1_00            0x0100      /* bcdUVC: video class 1.00 */

/* terminal identities (the VC topology is Camera -> USB streaming) */
#define UVC_TID_CAMERA          1
#define UVC_TID_STREAM          2

/* color matching: BT.709 primaries/transfer, BT.601 (SMPTE 170M) YCbCr matrix */
#define UVC_COLOR_PRIM_BT709        1
#define UVC_XFER_CHAR_BT709         1
#define UVC_MATRIX_COEFF_SMPTE170M  4

/* YUY2 (4:2:2 packed) geometry */
#define UVC_YUY2_BITS_PER_PIXEL 16
#define UVC_YUY2_BYTES_PER_PX   2

/* KSDATAFORMAT_SUBTYPE_YUY2, guidFormat wire order (first 4 bytes are the FourCC) */
#define UVC_GUID_YUY2 \
    'Y', 'U', 'Y', '2', 0x00, 0x00, 0x10, 0x00, \
    0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71

#define UVC_SET_CUR   0x01
#define UVC_GET_CUR   0x81
#define UVC_GET_MIN   0x82
#define UVC_GET_MAX   0x83
#define UVC_GET_RES   0x84
#define UVC_GET_LEN   0x85
#define UVC_GET_INFO  0x86
#define UVC_GET_DEF   0x87
#define UVC_VS_PROBE_CONTROL   0x01
#define UVC_VS_COMMIT_CONTROL  0x02

#define UVC_STREAM_FID  0x01
#define UVC_STREAM_EOF  0x02
#define UVC_PAYLOAD_HDR_LEN 2       /* bHeaderLength: bHeaderLength + bmHeaderInfo only */

#define UVC_PROBE_LEN   34          /* uvc_streaming_control, UVC 1.1 */
#define UVC_HINT_FRAME_INTERVAL 0x01 /* bmHint bit 0: dwFrameInterval is fixed */
#define UVC_INFO_GET_SET 0x03       /* GET_INFO capabilities: GET and SET both supported */
#define UVC_EP_IN       0x81
#define UVC_ISO_MPS     1023        /* full-speed isochronous max packet */
#define UVC_ISO_MPS_HS  1024        /* high-speed isochronous max packet */

#define UVC_CLOCK_HZ    48000000u   /* dwClockFrequency, also reported in Probe/Commit */
#define UVC_INTERVAL_HZ 10000000u   /* dwFrameInterval unit: 100 ns ticks per second */
#define UVC_DEFAULT_FPS 30
#define UVC_FRAME_MARGIN 1024       /* frame buffer slack for compressed formats */

struct uvc_state {
    uvc_opts opts;
    uint8_t  cur_format, cur_frame;     /* probed/committed indices (1-based) */
    uint32_t cur_interval;              /* dwFrameInterval (100 ns units) */
    uint32_t max_frame_size;            /* dwMaxVideoFrameSize */
    char     fmt_char[4];               /* format index -> 'Y' (YUYV) / 'M' (MJPEG) */
    int      streaming;
    uint32_t frame_index;              /* frames produced this streaming session */
    uint8_t *frame;                    /* current frame buffer (sent to host) */
    int      frame_cap;
    uint32_t frame_len, frame_pos;     /* current frame size / send cursor */
    uint8_t  fid;                      /* toggles per frame (UVC payload header) */
    uint16_t iso_mps;                  /* iso EP max packet (1023 FS / 1024 HS) */
};

/* Byte-order accessors come from usb_byteorder.h (UVC payloads are little-endian). */

static void uvc_log(struct uvc_state *st, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    usbip_class_vlog(st->opts.on_event, st->opts.user, NULL, fmt, ap);
    va_end(ap);
}

/* ---- synthetic frame source: animated SMPTE-ish color bars (YUY2) ---- */
void uvc_color_bars_yuyv(uint8_t *buf, int width, int height, uint32_t idx) {
    /* limited-range BT.601 {Y, U(Cb), V(Cr)} */
    static const uint8_t bars[8][3] = {
        {235,128,128}, {210, 16,146}, {170,166, 16}, {145, 54, 34},
        {106,202,222}, { 81, 90,240}, { 41,240,110}, { 16,128,128},
    };
    int shift = (int)(idx % (uint32_t)(width ? width : 1));
    for (int y = 0; y < height; y++) {
        uint8_t *row = buf + (size_t)y * width * 2;
        for (int x = 0; x < width; x += 2) {
            const uint8_t *c0 = bars[(((x     + shift) * 8) / width) & 7];
            const uint8_t *c1 = bars[(((x + 1 + shift) * 8) / width) & 7];
            row[x*2 + 0] = c0[0];          /* Y0 */
            row[x*2 + 1] = c0[1];          /* U  (shared by the pair) */
            row[x*2 + 2] = c1[0];          /* Y1 */
            row[x*2 + 3] = c0[2];          /* V  */
        }
    }
}

/* fill st->frame with the next frame in the committed format */
static void produce_frame(struct uvc_state *st) {
    uvc_opts *opts = &st->opts;
    char fmt = st->fmt_char[st->cur_format];
    int produced = -1;
    if (opts->next_frame)
        produced = opts->next_frame(opts->user, st->frame, st->frame_cap, fmt, st->frame_index);
    if (produced < 0) {                                   /* built-in synthetic YUYV color bars */
        uvc_color_bars_yuyv(st->frame, opts->width, opts->height, st->frame_index);
        produced = opts->width * opts->height * UVC_YUY2_BYTES_PER_PX;
    }
    st->frame_len = (uint32_t)produced;
    st->frame_pos = 0;
    uvc_log(st, "frame %c %ux%u (%dB)", fmt, opts->width, opts->height, produced);
}

/* ---- isochronous IN: pack UVC payloads (2-byte header + data) per packet ---- */
static int uvc_on_iso(usbip_function *iface, usbip_ep *ep, int npkts,
                      uint32_t *lens, uint8_t *buf) {
    (void)ep;
    struct uvc_state *st = usbip_function_state(iface);
    uint32_t off = 0;
    for (int i = 0; i < npkts; i++) {
        uint32_t req = lens[i];                    /* requested bytes for this packet */
        if (!st->streaming || req < UVC_PAYLOAD_HDR_LEN) {
            lens[i] = 0;
            continue;
        }
        if (st->frame_len == 0) produce_frame(st);

        uint32_t cap = req - UVC_PAYLOAD_HDR_LEN;  /* room after the payload header */
        uint32_t remain = st->frame_len - st->frame_pos;
        uint32_t chunk = remain < cap ? remain : cap;
        uint8_t *pkt = buf + off;
        pkt[0] = UVC_PAYLOAD_HDR_LEN;                /* bHeaderLength */
        pkt[1] = st->fid;                            /* bmHeaderInfo: FID */
        if (st->frame_pos + chunk >= st->frame_len) pkt[1] |= UVC_STREAM_EOF;
        memcpy(pkt + UVC_PAYLOAD_HDR_LEN, st->frame + st->frame_pos, chunk);
        st->frame_pos += chunk;
        lens[i] = UVC_PAYLOAD_HDR_LEN + chunk;
        off += UVC_PAYLOAD_HDR_LEN + chunk;

        if (st->frame_pos >= st->frame_len) {      /* frame done -> next starts fresh */
            st->frame_len = 0;
            st->fid ^= UVC_STREAM_FID;
            st->frame_index++;
        }
    }
    return 0;
}

/* ---- Probe/Commit negotiation ---- */
static void fill_probe(struct uvc_state *st, uint8_t *buf) {
    memset(buf, 0, UVC_PROBE_LEN);
    buf[0] = UVC_HINT_FRAME_INTERVAL;                /* bmHint: dwFrameInterval fixed */
    buf[2] = st->cur_format;                         /* bFormatIndex */
    buf[3] = st->cur_frame;                          /* bFrameIndex  */
    usb_put_le32(buf + 4,  st->cur_interval);            /* dwFrameInterval */
    usb_put_le32(buf + 18, st->max_frame_size);          /* dwMaxVideoFrameSize */
    usb_put_le32(buf + 22, st->iso_mps);                 /* dwMaxPayloadTransferSize */
    usb_put_le32(buf + 26, UVC_CLOCK_HZ);                /* dwClockFrequency */
    /* bmFramingInfo / versions left 0 */
}

static int uvc_control(usbip_function *iface, const usb_setup *setup, uint8_t *buf, uint16_t len) {
    struct uvc_state *st = usbip_function_state(iface);
    uint8_t cs = USB_U16_MSB(setup->wValue);        /* control selector */
    uint8_t rq = setup->bRequest;

    if (cs != UVC_VS_PROBE_CONTROL && cs != UVC_VS_COMMIT_CONTROL) {
        if (rq == UVC_GET_INFO) {
            buf[0] = UVC_INFO_GET_SET;
            return 1;
        }   /* GET|SET */
        return -1;                                  /* STALL other VC/VS controls */
    }

    switch (rq) {
    case UVC_GET_INFO:
        buf[0] = UVC_INFO_GET_SET;
        return 1;

    case UVC_GET_LEN:
        buf[0] = UVC_PROBE_LEN;
        buf[1] = 0;
        return 2;

    case UVC_GET_CUR:
    case UVC_GET_MIN:
    case UVC_GET_MAX:
    case UVC_GET_DEF:
    case UVC_GET_RES:
        fill_probe(st, buf);
        return UVC_PROBE_LEN;

    case UVC_SET_CUR:
        if (len >= 4) {
            if (buf[2]) st->cur_format = buf[2];
            if (buf[3]) st->cur_frame  = buf[3];
        }
        if (len >= 8) {
            uint32_t iv = usb_get_le32(buf + 4);
            if (iv) st->cur_interval = iv;
        }
        if (cs == UVC_VS_COMMIT_CONTROL)
            uvc_log(st, "COMMIT format=%c %ux%u @ %.0ffps", st->fmt_char[st->cur_format],
                    st->opts.width, st->opts.height, (double)UVC_INTERVAL_HZ / st->cur_interval);
        return 0;
    }
    return -1;
}

/* SET_INTERFACE(VS, alt) - alt 1 enables the iso endpoint i.e. starts streaming.
 * Only the VS interface has alternates, so alt is unambiguous here. */
static int uvc_set_alt(usbip_function *iface, int ifnum, int alt) {
    (void)ifnum;                                /* only the VS interface has alternates */
    struct uvc_state *st = usbip_function_state(iface);
    if (alt == 1 && !st->streaming) {
        st->streaming = 1;
        st->frame_len = 0;
        st->frame_pos = 0;
        st->fid = 0;
        st->frame_index = 0;
        uvc_log(st, "STREAM ON  %c %ux%u", st->fmt_char[st->cur_format],
                st->opts.width, st->opts.height);
    } else if (alt == 0 && st->streaming) {
        st->streaming = 0;
        uvc_log(st, "STREAM OFF (%u frames)", st->frame_index);
    }
    return 0;
}

/* ---- descriptor assembly (IAD + VC + VS alt0/alt1 + iso EP) ---- */
static int uvc_build(usbip_function *func, const void *params) {
    struct uvc_state *st = usbip_function_state(func);
    st->opts = *(const uvc_opts *)params;
    if (!st->opts.formats) st->opts.formats = UVC_HAS_YUYV;
    uvc_opts *opts = &st->opts;

    uint32_t width = opts->width, height = opts->height;
    uint32_t fps = opts->fps ? opts->fps : UVC_DEFAULT_FPS;
    uint32_t interval = UVC_INTERVAL_HZ / fps;
    uint32_t frame_bytes = width * height * UVC_YUY2_BYTES_PER_PX;
    uint32_t bitrate = frame_bytes * 8u * fps;
    st->cur_interval = interval;
    st->cur_format = 1;
    st->cur_frame = 1;
    st->max_frame_size = frame_bytes;
    /* HS iso allows 1024 B/packet (FS caps at 1023); with the 125 µs HS service
     * interval that is ~8× the FS bandwidth. */
    usbip_device *dev = usbip_function_device(func);
    int speed = usbip_device_get_speed(dev);

    st->iso_mps = (speed >= USB_SPEED_HIGH) ? UVC_ISO_MPS_HS : UVC_ISO_MPS;

    int base = usbip_function_base_ifnum(func);
    uint8_t vs_if = (uint8_t)(base + 1);          /* VC header references the VS interface */

    int nfmt = 0;
    uint8_t yuyv_idx = 0;
    uint8_t mjpeg_idx = 0;
    if (opts->formats & UVC_HAS_YUYV)  {
        yuyv_idx  = (uint8_t)(++nfmt);
        st->fmt_char[yuyv_idx]  = 'Y';
    }
    if (opts->formats & UVC_HAS_MJPEG) {
        mjpeg_idx = (uint8_t)(++nfmt);
        st->fmt_char[mjpeg_idx] = 'M';
    }

    /* class-specific VC interface total length (header + camera IT + streaming OT) */
    uint16_t vc_total = 13 + 18 + 9;                /* = 40 */

    int vs_total = 13 + nfmt;                       /* VS input header */

    if (yuyv_idx)  
        vs_total += 27 + 30 + 6;
    if (mjpeg_idx) 
        vs_total += 11 + 30 + 6;

    /* Composite video function: device triple + IAD grouping VC+VS (opt-in). */
    usbip_device_set_class(dev, USB_CLASS_MISC, USB_SUBCLASS_COMMON, USB_PROTOCOL_IAD);
    usbip_function_associate(func, UVC_IAD_IFACE_COUNT, UVC_CC_VIDEO, UVC_SC_COLLECTION, UVC_PC_PROTOCOL_UNDEFINED, 0);

    /* VideoControl interface (no endpoint) */
    usbip_interface *vc = usbip_function_add_interface(func, UVC_CC_VIDEO, UVC_SC_VIDEOCONTROL, 0);
    /* VC header: the class-specific VC interface preamble; the collection names the
     * VideoStreaming interface this VideoControl interface owns. */
    usbip_interface_add_descriptor(vc, (uint8_t[]){
        13,                             /* bLength */
        USB_DT_CS_INTERFACE,            /* bDescriptorType */
        UVC_VC_HEADER,                  /* bDescriptorSubtype */
        USB_U16LE(UVC_VER_1_00),        /* bcdUVC: video 1.00 */
        USB_U16LE(vc_total),            /* wTotalLength: header + terminals */
        USB_U32LE(UVC_CLOCK_HZ),        /* dwClockFrequency: 48 MHz */
        1,                              /* bInCollection: 1 streaming interface */
        vs_if                           /* baInterfaceNr[]: the VS interface */
    });

    /* Camera Input Terminal (ID 1) */
    usbip_interface_add_descriptor(vc, (uint8_t[]){
        18,                             /* bLength */
        USB_DT_CS_INTERFACE,            /* bDescriptorType */
        UVC_VC_INPUT_TERMINAL,          /* bDescriptorSubtype */
        UVC_TID_CAMERA,                 /* bTerminalID */
        USB_U16LE(UVC_ITT_CAMERA),      /* wTerminalType: camera sensor */
        0,                              /* bAssocTerminal: none */
        0,                              /* iTerminal */
        USB_U16LE(0),                   /* wObjectiveFocalLengthMin: no optical zoom */
        USB_U16LE(0),                   /* wObjectiveFocalLengthMax: no optical zoom */
        USB_U16LE(0),                   /* wOcularFocalLength: no optical zoom */
        3,                              /* bControlSize */
        0, 0, 0                         /* bmControls: no camera controls */
    });

    /* Output Terminal (ID 2) -> streaming, sourced from terminal 1 */
    usbip_interface_add_descriptor(vc, (uint8_t[]){
        9,                              /* bLength */
        USB_DT_CS_INTERFACE,            /* bDescriptorType */
        UVC_VC_OUTPUT_TERMINAL,         /* bDescriptorSubtype */
        UVC_TID_STREAM,                 /* bTerminalID */
        USB_U16LE(UVC_TT_STREAMING),    /* wTerminalType: USB streaming */
        0,                              /* bAssocTerminal: none */
        UVC_TID_CAMERA,                 /* bSourceID: the camera terminal */
        0                               /* iTerminal */
    });

    /* VideoStreaming interface, alt 0 (no bandwidth) + class-specific descriptors */
    usbip_interface *vs = usbip_function_add_interface(func, UVC_CC_VIDEO, UVC_SC_VIDEOSTREAMING, 0);
    /* The input header names the streaming endpoint, whose descriptor only appears in
     * alt 1 and may be relocated off UVC_EP_IN.
     * Claim the address up front: a header naming another function's pipe makes the
     * host simply not stream. */
    uint8_t vs_ep = usbip_function_reserve_endpoint(func, UVC_EP_IN);
    if (!vs_ep)  /* no IN endpoint number left on this device */
        return -1;

    /* VS input header: nfmt formats, the streaming EP, terminal link = OT(2) */
    {
        uint8_t hdr[16] = {
            (uint8_t)(13 + nfmt),       /* bLength: 13 + one bmaControls byte per format */
            USB_DT_CS_INTERFACE,        /* bDescriptorType */
            UVC_VS_INPUT_HEADER,        /* bDescriptorSubtype */
            (uint8_t)nfmt,              /* bNumFormats */
            USB_U16LE(vs_total),        /* wTotalLength: header + all format/frame descriptors */
            vs_ep,                      /* bEndpointAddress: the iso IN endpoint */
            0,                          /* bmInfo: no dynamic format change */
            UVC_TID_STREAM,             /* bTerminalLink: the streaming Output Terminal */
            0,                          /* bStillCaptureMethod: none */
            0,                          /* bTriggerSupport: no hardware trigger */
            0,                          /* bTriggerUsage */
            1                           /* bControlSize */
        };
        for (int i = 0; i < nfmt; i++)
            hdr[13 + i] = 0;            /* bmaControls[i]: no per-format controls */
        usbip_interface_add_descriptor(vs, hdr);
    }
    if (yuyv_idx) {
        /* Uncompressed (YUY2) format + frame + color matching */
        usbip_interface_add_descriptor(vs, (uint8_t[]){
            27,                         /* bLength */
            USB_DT_CS_INTERFACE,        /* bDescriptorType */
            UVC_VS_FORMAT_UNCOMP,       /* bDescriptorSubtype */
            yuyv_idx,                   /* bFormatIndex */
            1,                          /* bNumFrameDescriptors */
            UVC_GUID_YUY2,              /* guidFormat: KSDATAFORMAT_SUBTYPE_YUY2 */
            UVC_YUY2_BITS_PER_PIXEL,    /* bBitsPerPixel */
            1,                          /* bDefaultFrameIndex */
            0,                          /* bAspectRatioX: non-interlaced, unspecified */
            0,                          /* bAspectRatioY */
            0,                          /* bmInterlaceFlags: progressive */
            0                           /* bCopyProtect: duplication unrestricted */
        });
        usbip_interface_add_descriptor(vs, (uint8_t[]){
            30,                         /* bLength */
            USB_DT_CS_INTERFACE,        /* bDescriptorType */
            UVC_VS_FRAME_UNCOMP,        /* bDescriptorSubtype */
            1,                          /* bFrameIndex */
            0,                          /* bmCapabilities: no still image, fixed frame rate */
            USB_U16LE(width),               /* wWidth */
            USB_U16LE(height),               /* wHeight */
            USB_U32LE(bitrate),         /* dwMinBitRate */
            USB_U32LE(bitrate),         /* dwMaxBitRate */
            USB_U32LE(frame_bytes),     /* dwMaxVideoFrameBufferSize */
            USB_U32LE(interval),        /* dwDefaultFrameInterval (100 ns units) */
            1,                          /* bFrameIntervalType: one discrete interval */
            USB_U32LE(interval)         /* dwFrameInterval[0] */
        });
        usbip_interface_add_descriptor(vs, (uint8_t[]){
            6,                          /* bLength */
            USB_DT_CS_INTERFACE,        /* bDescriptorType */
            UVC_VS_COLORFORMAT,         /* bDescriptorSubtype */
            UVC_COLOR_PRIM_BT709,       /* bColorPrimaries: BT.709 / sRGB */
            UVC_XFER_CHAR_BT709,        /* bTransferCharacteristics: BT.709 */
            UVC_MATRIX_COEFF_SMPTE170M  /* bMatrixCoefficients: BT.601 */
        });
    }
    if (mjpeg_idx) {
        /* MJPEG format + frame + color matching */
        usbip_interface_add_descriptor(vs, (uint8_t[]){
            11,                         /* bLength */
            USB_DT_CS_INTERFACE,        /* bDescriptorType */
            UVC_VS_FORMAT_MJPEG,        /* bDescriptorSubtype */
            mjpeg_idx,                  /* bFormatIndex */
            1,                          /* bNumFrameDescriptors */
            0,                          /* bmFlags: sample size varies per frame */
            1,                          /* bDefaultFrameIndex */
            0,                          /* bAspectRatioX: non-interlaced, unspecified */
            0,                          /* bAspectRatioY */
            0,                          /* bmInterlaceFlags: progressive */
            0                           /* bCopyProtect: duplication unrestricted */
        });
        usbip_interface_add_descriptor(vs, (uint8_t[]){
            30,                         /* bLength */
            USB_DT_CS_INTERFACE,        /* bDescriptorType */
            UVC_VS_FRAME_MJPEG,         /* bDescriptorSubtype */
            1,                          /* bFrameIndex */
            0,                          /* bmCapabilities: no still image, fixed frame rate */
            USB_U16LE(width),               /* wWidth */
            USB_U16LE(height),               /* wHeight */
            USB_U32LE(bitrate),         /* dwMinBitRate */
            USB_U32LE(bitrate),         /* dwMaxBitRate */
            USB_U32LE(frame_bytes),     /* dwMaxVideoFrameBufferSize */
            USB_U32LE(interval),        /* dwDefaultFrameInterval (100 ns units) */
            1,                          /* bFrameIntervalType: one discrete interval */
            USB_U32LE(interval)         /* dwFrameInterval[0] */
        });
        usbip_interface_add_descriptor(vs, (uint8_t[]){
            6,                          /* bLength */
            USB_DT_CS_INTERFACE,        /* bDescriptorType */
            UVC_VS_COLORFORMAT,         /* bDescriptorSubtype */
            UVC_COLOR_PRIM_BT709,       /* bColorPrimaries: BT.709 / sRGB */
            UVC_XFER_CHAR_BT709,        /* bTransferCharacteristics: BT.709 */
            UVC_MATRIX_COEFF_SMPTE170M  /* bMatrixCoefficients: BT.601 */
        });
    }

    /* VideoStreaming interface, alt 1: the isochronous IN endpoint */
    usbip_interface_add_altsetting(vs, 1);
    usbip_interface_add_endpoint(vs, &(usb_endpoint_descriptor){
        .bDescriptorType = USB_DT_ENDPOINT,
        .bEndpointAddress = vs_ep,      /* the address reserved above */
        .bmAttributes = USB_ISO | USB_EP_SYNC_ASYNC | USB_EP_USAGE_DATA,
        .wMaxPacketSize = st->iso_mps,  /* 1023 FS / 1024 HS */
        .bInterval = 1                  /* every (micro)frame */
    });

    st->frame_cap = (int)frame_bytes + UVC_FRAME_MARGIN;  /* margin for compressed frames */
    st->frame = malloc((size_t)st->frame_cap);
    return st->frame ? 0 : -1;
}

static void uvc_destroy(usbip_function *iface) {
    struct uvc_state *st = usbip_function_state(iface);
    free(st->frame);
    st->frame = NULL;
}

const usbip_device_class usbip_device_uvc = {
    .name = "uvc",
    .bInterfaceClass = UVC_CC_VIDEO,
    .build = uvc_build,
    .control = uvc_control,
    .set_alt = uvc_set_alt,
    .on_iso = uvc_on_iso,
    .destroy = uvc_destroy,
    .state_size = sizeof(struct uvc_state),
};

uvc_cam *uvc_add(usbip_device *dev, const uvc_opts *opts) {
    /* device triple set by uvc_build */
    usbip_function *func = usbip_device_add_class(dev, &usbip_device_uvc, opts);

    return handle_of(func);
}
