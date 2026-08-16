/**
 * Copyright (C) 2026 Jabez Winston
 *
 * SPDX-License-Identifier: MIT
 *
 * Date : 05-June-2026
 *
 * uac.c - USB Audio Class 1.0 (UAC1) device class: speaker + microphone.
 *
 * Built ONLY on the public API (usbip_device.h / classes/uac.h). Presents an AudioControl
 * interface (a USB->Speaker playback chain and a Microphone->USB capture chain,
 * each with a master mute+volume Feature Unit) plus two AudioStreaming interfaces:
 * a stereo speaker over an isochronous OUT endpoint and a mono microphone over an
 * isochronous IN endpoint, both 48 kHz / 16-bit PCM. Mirrors the Python
 * classes/device/uac.py.
 *
 * Spec: USB Device Class Definition for Audio Devices 1.0; descriptor layout follows
 * the kernel UAC1 gadget (drivers/usb/gadget/function/f_uac1.c) and the UAC1 spec
 * (Audio Data Formats / Feature Unit). Full-speed, Type I PCM, no feedback endpoint.
 */
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "classes/uac.h"


/* uac_audio is this class's public handle: the function, retyped so the compiler
 * can tell it from another class's handle. */
static inline uac_audio *handle_of(usbip_function *func) {
    return (uac_audio *)func;
}
/* ---- UAC1 constants (class-specific, defined here - core stays class-free) ---- */
#define UAC_CC_AUDIO            USB_CLASS_AUDIO
#define UAC_SC_AUDIOCONTROL     0x01
#define UAC_SC_AUDIOSTREAMING   0x02

/* AC interface descriptor subtypes */
#define UAC_AC_HEADER           0x01
#define UAC_AC_INPUT_TERMINAL   0x02
#define UAC_AC_OUTPUT_TERMINAL  0x03
#define UAC_AC_FEATURE_UNIT     0x06

/* AS interface descriptor subtypes */
#define UAC_AS_GENERAL          0x01
#define UAC_AS_FORMAT_TYPE      0x02

/* AS endpoint descriptor subtype */
#define UAC_EP_GENERAL          0x01

/* terminal types */
#define UAC_TT_USB_STREAMING    0x0101
#define UAC_TT_MICROPHONE       0x0201
#define UAC_TT_SPEAKER          0x0301

/* format */
#define UAC_FORMAT_TYPE_I       0x01
#define UAC_FORMAT_PCM          0x0001

/* bcdADC: audio device class 1.00 */
#define UAC_ADC_1_00            0x0100

/* wChannelConfig: spatial positions of the channels in a cluster */
#define UAC_CHCFG_NONE          0x0000  /* mono: no spatial position */
#define UAC_CHCFG_STEREO        0x0003  /* front left + front right */

/* Feature Unit bmaControls bits (one entry per channel, master first) */
#define UAC_FU_BM_NONE          0x00
#define UAC_FU_BM_MUTE          0x01
#define UAC_FU_BM_VOLUME        0x02

/* class requests (audio 1.0) */
#define UAC_SET_CUR   0x01
#define UAC_GET_CUR   0x81
#define UAC_GET_MIN   0x82
#define UAC_GET_MAX   0x83
#define UAC_GET_RES   0x84

/* Feature Unit control selectors */
#define UAC_FU_MUTE     0x01
#define UAC_FU_VOLUME   0x02

/* identities + format */
#define UAC_IT_PLAY   1     /* USB streaming -> (playback) */
#define UAC_FU_PLAY   2     /* playback Feature Unit (mute+vol) */
#define UAC_OT_SPK    3     /* Speaker */
#define UAC_IT_MIC    4     /* Microphone */
#define UAC_FU_CAP    5     /* capture Feature Unit (mute+vol) */
#define UAC_OT_USB    6     /* -> USB streaming (capture) */

#define UAC_RATE      48000
#define UAC_SPK_CH    2
#define UAC_MIC_CH    1
#define UAC_EP_SPK    0x01      /* iso OUT (playback) */
#define UAC_EP_MIC    0x82      /* iso IN  (capture)  */

/* PCM sample geometry: 16-bit signed samples, so bSubframeSize is 2 bytes. */
#define UAC_SUBFRAME_SIZE   2
#define UAC_BIT_RESOLUTION  16

/* 440 Hz tone: 32-bit phase accumulator, top 8 bits index a 256-entry sine table.
 * PHASE_INC = round(440 / 48000 * 2^32). Pure integer -> byte-identical to Python. */
#define UAC_PHASE_INC  39370534u
/* default volume controls: 1/256 dB, signed */
#define UAC_VOL_CUR  ((int16_t)0xF600)   /* -10 dB */
#define UAC_VOL_MIN  ((int16_t)0xC100)   /* -63 dB */
#define UAC_VOL_MAX  ((int16_t)0x0000)   /*   0 dB */
#define UAC_VOL_RES  ((int16_t)0x0100)   /*   1 dB */

static const int16_t SINE256[256] = {
         0,    393,    785,   1177,   1568,   1959,   2348,   2735,
      3121,   3506,   3888,   4267,   4645,   5019,   5390,   5758,
      6123,   6484,   6841,   7194,   7542,   7886,   8226,   8560,
      8889,   9213,   9531,   9844,  10150,  10451,  10745,  11033,
     11314,  11588,  11855,  12115,  12368,  12614,  12851,  13081,
     13304,  13518,  13724,  13921,  14111,  14292,  14464,  14627,
     14782,  14928,  15065,  15192,  15311,  15420,  15521,  15611,
     15693,  15764,  15827,  15880,  15923,  15957,  15981,  15995,
     16000,  15995,  15981,  15957,  15923,  15880,  15827,  15764,
     15693,  15611,  15521,  15420,  15311,  15192,  15065,  14928,
     14782,  14627,  14464,  14292,  14111,  13921,  13724,  13518,
     13304,  13081,  12851,  12614,  12368,  12115,  11855,  11588,
     11314,  11033,  10745,  10451,  10150,   9844,   9531,   9213,
      8889,   8560,   8226,   7886,   7542,   7194,   6841,   6484,
      6123,   5758,   5390,   5019,   4645,   4267,   3888,   3506,
      3121,   2735,   2348,   1959,   1568,   1177,    785,    393,
         0,   -393,   -785,  -1177,  -1568,  -1959,  -2348,  -2735,
     -3121,  -3506,  -3888,  -4267,  -4645,  -5019,  -5390,  -5758,
     -6123,  -6484,  -6841,  -7194,  -7542,  -7886,  -8226,  -8560,
     -8889,  -9213,  -9531,  -9844, -10150, -10451, -10745, -11033,
    -11314, -11588, -11855, -12115, -12368, -12614, -12851, -13081,
    -13304, -13518, -13724, -13921, -14111, -14292, -14464, -14627,
    -14782, -14928, -15065, -15192, -15311, -15420, -15521, -15611,
    -15693, -15764, -15827, -15880, -15923, -15957, -15981, -15995,
    -16000, -15995, -15981, -15957, -15923, -15880, -15827, -15764,
    -15693, -15611, -15521, -15420, -15311, -15192, -15065, -14928,
    -14782, -14627, -14464, -14292, -14111, -13921, -13724, -13518,
    -13304, -13081, -12851, -12614, -12368, -12115, -11855, -11588,
    -11314, -11033, -10745, -10451, -10150,  -9844,  -9531,  -9213,
     -8889,  -8560,  -8226,  -7886,  -7542,  -7194,  -6841,  -6484,
     -6123,  -5758,  -5390,  -5019,  -4645,  -4267,  -3888,  -3506,
     -3121,  -2735,  -2348,  -1959,  -1568,  -1177,   -785,   -393,
};

struct uac_state {
    uac_opts opts;
    uint8_t  spk_if, mic_if;            /* AudioStreaming interface numbers */
    int      spk_on, mic_on;           /* alt-1 selected? (streaming) */
    uint32_t mic_samples;              /* running mono-sample counter (tone phase) */
    unsigned long spk_bytes;           /* total playback bytes received */
    int      spk_peak;                 /* peak |sample| seen on playback */
    uint8_t  mute[2];                  /* [0]=playback FU, [1]=capture FU */
    int16_t  volume[2];
};

static void uac_log(struct uac_state *st, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    usbip_class_vlog(st->opts.on_event, st->opts.user, NULL, fmt, ap);
    va_end(ap);
}

/* ---- synthetic microphone source: 440 Hz sine, mono 16-bit LE ---- */
void uac_tone(uint8_t *buf, int frames, uint32_t index)
{
    for (int k = 0; k < frames; k++)
    {
        uint32_t phase = (uint32_t)((uint64_t)(index + (uint32_t)k) * UAC_PHASE_INC);
        int16_t sample = SINE256[(phase >> 24) & 0xff];
        usb_put_le16(buf + k * 2, (uint16_t)sample);
    }
}

/* ---- isochronous IN (microphone): fill each packet with tone/app samples ---- */
static int uac_in_iso(usbip_function *iface, usbip_ep *ep, int npkts, uint32_t *lens, uint8_t *buf)
{
    (void)ep;
    struct uac_state *st = usbip_function_state(iface);
    uint32_t off = 0;
    for (int i = 0; i < npkts; i++)
    {
        int frames = (int)(lens[i] / 2); /* mono 16-bit; lens[i] = requested bytes */
        if (!st->mic_on || frames <= 0)
        {
            lens[i] = 0;
            continue;
        }
        if (st->mute[1])
        { /* muted -> silence */
            memset(buf + off, 0, (size_t)frames * 2);
        }
        else
        {
            int produced = -1;
            if (st->opts.mic_source)
                produced = st->opts.mic_source(st->opts.user, buf + off, frames, st->mic_samples);
            if (produced < 0)
            {
                uac_tone(buf + off, frames, st->mic_samples);
                produced = frames * 2;
            }
            frames = produced / 2;
        }
        lens[i] = (uint32_t)frames * 2;
        st->mic_samples += (uint32_t)frames;
        off += (uint32_t)frames * 2;
    }
    if (off)
        uac_log(st, "mic tone (%uB)", off);
    return 0;
}

/* ---- isochronous OUT (speaker): meter the level + hand PCM to the app ---- */
static int uac_out_iso(usbip_function *iface, usbip_ep *ep, int npkts,
                       const uint32_t *lens, const uint8_t *buf)
{
    (void)ep;
    struct uac_state *st = usbip_function_state(iface);
    uint32_t off = 0, total = 0;
    for (int i = 0; i < npkts; i++)
    {
        uint32_t len = lens[i];
        for (uint32_t k = 0; k + 1 < len; k += 2)
        {
            int16_t sample = (int16_t)usb_get_le16(buf + off + k);
            int amp = sample < 0 ? -sample : sample;
            if (amp > st->spk_peak)
                st->spk_peak = amp;
        }
        if (len && st->opts.spk_sink && !st->mute[0])
            st->opts.spk_sink(st->opts.user, buf + off, (int)len);
        off += len;
        total += len;
    }
    st->spk_bytes += total;
    if (total)
        uac_log(st, "spk recv peak=%d (%uB)", st->spk_peak, total);
    return 0;
}

/* ---- the vtable's single iso callback: mic (IN) or speaker (OUT) by direction ---- */
static int uac_on_iso(usbip_function *iface, usbip_ep *ep, int npkts, uint32_t *lens, uint8_t *buf)
{
    if (usbip_endpoint_address(ep) & 0x80)
        return uac_in_iso(iface, ep, npkts, lens, buf);
    return uac_out_iso(iface, ep, npkts, lens, buf);
}

/* ---- Feature Unit controls (mute + volume), recipient = AudioControl iface ---- */
static int uac_control(usbip_function *iface, const usb_setup *setup, uint8_t *buf, uint16_t len)
{
    struct uac_state *st = usbip_function_state(iface);
    uint8_t cs = USB_U16_MSB(setup->wValue);   /* control selector */
    uint8_t unit = USB_U16_MSB(setup->wIndex); /* Feature Unit ID */
    int idx = -1;

    if (unit == UAC_FU_PLAY)
        idx = 0;
    else if (unit == UAC_FU_CAP)
        idx = 1;

    if (idx < 0)
        return -1;

    if (cs == UAC_FU_MUTE)
    {
        switch (setup->bRequest)
        {
        case UAC_SET_CUR:
            if (len >= 1)
                st->mute[idx] = buf[0] ? 1 : 0;
            return 0;

        case UAC_GET_CUR:
            buf[0] = st->mute[idx];
            return 1;

        default:
            return -1;
        }
    }
    if (cs == UAC_FU_VOLUME)
    {
        int16_t value;
        switch (setup->bRequest)
        {
        case UAC_SET_CUR:
            if (len >= 2)
                st->volume[idx] = (int16_t)usb_get_le16(buf);
            return 0;

        case UAC_GET_CUR:
            value = st->volume[idx];
            break;

        case UAC_GET_MIN:
            value = UAC_VOL_MIN;
            break;

        case UAC_GET_MAX:
            value = UAC_VOL_MAX;
            break;

        case UAC_GET_RES:
            value = UAC_VOL_RES;
            break;

        default:
            return -1;
        }
        usb_put_le16(buf, (uint16_t)value);
        return 2;
    }
    return -1;
}

/* SET_INTERFACE(alt) on a streaming interface - alt 1 enables its iso endpoint. */
static int uac_set_alt(usbip_function *iface, int ifnum, int alt)
{
    struct uac_state *st = usbip_function_state(iface);
    if (ifnum == st->spk_if)
    {
        if (alt == 1 && !st->spk_on)
        {
            st->spk_on = 1;
            st->spk_bytes = 0;
            st->spk_peak = 0;
            uac_log(st, "SPK ON  48000/16/2");
        }
        else if (alt == 0 && st->spk_on)
        {
            st->spk_on = 0;
            uac_log(st, "SPK OFF (%lu bytes, peak %d)", st->spk_bytes, st->spk_peak);
        }
    }
    else if (ifnum == st->mic_if)
    {
        if (alt == 1 && !st->mic_on)
        {
            st->mic_on = 1;
            st->mic_samples = 0;
            uac_log(st, "MIC ON  48000/16/1");
        }
        else if (alt == 0 && st->mic_on)
        {
            st->mic_on = 0;
            uac_log(st, "MIC OFF (%u samples)", st->mic_samples);
        }
    }
    return 0;
}

/* ---- descriptor assembly (AC topology + 2 AS interfaces, alt0/alt1 + iso EPs) ---- */
static int uac_build(usbip_function *func, const void *params)
{
    struct uac_state *st = usbip_function_state(func);
    st->opts = *(const uac_opts *)params;
    st->volume[0] = st->volume[1] = UAC_VOL_CUR;

    int base = usbip_function_base_ifnum(func);
    uint8_t ac = (uint8_t)base;
    uint8_t spk = (uint8_t)(base + 1);
    uint8_t mic = (uint8_t)(base + 2);
    st->spk_if = spk;
    st->mic_if = mic;

    /* The iso service interval is one (micro)frame: 1 ms at FS, 125 µs at HS, so HS
     * packets carry 1/8 the PCM.
     * bInterval stays 1; the host and pacer reinterpret it for the speed. */
    usbip_device *dev = usbip_function_device(func);
    int speed = usbip_device_get_speed(dev);
    int frames_per_sec = (speed >= USB_SPEED_HIGH) ? 8000 : 1000;

    uint16_t spk_mps = (uint16_t)(UAC_RATE / frames_per_sec * UAC_SPK_CH * UAC_SUBFRAME_SIZE); /* 192 FS / 24 HS */
    uint16_t mic_mps = (uint16_t)(UAC_RATE / frames_per_sec * UAC_MIC_CH * UAC_SUBFRAME_SIZE); /* 96  FS / 12 HS */

    /* class-specific AC interface total length (header + 6 terminals/units) */
    uint16_t ac_total = 10 + 12 + 10 + 9 + 12 + 9 + 9; /* = 71 */

    /* AudioControl interface (no endpoints); (void)ac silences unused when base==0 */
    (void)ac;
    usbip_interface *acif = usbip_function_add_interface(func, UAC_CC_AUDIO, UAC_SC_AUDIOCONTROL, 0);
    /* AC header: the class-specific AC interface preamble; the collection lists the
     * AudioStreaming interfaces this AudioControl interface owns. */
    usbip_interface_add_descriptor(acif, (uint8_t[]){
        10,                             /* bLength */
        USB_DT_CS_INTERFACE,            /* bDescriptorType */
        UAC_AC_HEADER,                  /* bDescriptorSubtype */
        USB_U16LE(UAC_ADC_1_00),        /* bcdADC: audio 1.00 */
        USB_U16LE(ac_total),            /* wTotalLength: header + all units/terminals */
        2,                              /* bInCollection: 2 streaming interfaces */
        spk, mic                        /* baInterfaceNr[]: speaker, microphone */
    });

    /* playback: Input Terminal (USB streaming, stereo) -> Feature Unit -> Speaker */
    usbip_interface_add_descriptor(acif, (uint8_t[]){
        12,                             /* bLength */
        USB_DT_CS_INTERFACE,            /* bDescriptorType */
        UAC_AC_INPUT_TERMINAL,          /* bDescriptorSubtype */
        UAC_IT_PLAY,                    /* bTerminalID */
        USB_U16LE(UAC_TT_USB_STREAMING),/* wTerminalType: USB streaming */
        0,                              /* bAssocTerminal: none */
        UAC_SPK_CH,                     /* bNrChannels */
        USB_U16LE(UAC_CHCFG_STEREO),    /* wChannelConfig: front left + front right */
        0,                              /* iChannelNames */
        0                               /* iTerminal */
    });

    usbip_interface_add_descriptor(acif, (uint8_t[]){
        10,                             /* bLength */
        USB_DT_CS_INTERFACE,            /* bDescriptorType */
        UAC_AC_FEATURE_UNIT,            /* bDescriptorSubtype */
        UAC_FU_PLAY,                    /* bUnitID */
        UAC_IT_PLAY,                    /* bSourceID: the USB streaming terminal */
        1,                              /* bControlSize: 1 byte per channel */
        UAC_FU_BM_MUTE | UAC_FU_BM_VOLUME, /* bmaControls[0]: master */
        UAC_FU_BM_NONE,                 /* bmaControls[1]: left  - no per-channel control */
        UAC_FU_BM_NONE,                 /* bmaControls[2]: right - no per-channel control */
        0                               /* iFeature */
    });

    usbip_interface_add_descriptor(acif, (uint8_t[]){
        9,                              /* bLength */
        USB_DT_CS_INTERFACE,            /* bDescriptorType */
        UAC_AC_OUTPUT_TERMINAL,         /* bDescriptorSubtype */
        UAC_OT_SPK,                     /* bTerminalID */
        USB_U16LE(UAC_TT_SPEAKER),      /* wTerminalType: speaker */
        0,                              /* bAssocTerminal: none */
        UAC_FU_PLAY,                    /* bSourceID: the playback Feature Unit */
        0                               /* iTerminal */
    });

    /* capture: Microphone (mono) -> Feature Unit -> Output Terminal (USB streaming) */
    usbip_interface_add_descriptor(acif, (uint8_t[]){
        12,                             /* bLength */
        USB_DT_CS_INTERFACE,            /* bDescriptorType */
        UAC_AC_INPUT_TERMINAL,          /* bDescriptorSubtype */
        UAC_IT_MIC,                     /* bTerminalID */
        USB_U16LE(UAC_TT_MICROPHONE),   /* wTerminalType: microphone */
        0,                              /* bAssocTerminal: none */
        UAC_MIC_CH,                     /* bNrChannels */
        USB_U16LE(UAC_CHCFG_NONE),      /* wChannelConfig: mono, no spatial position */
        0,                              /* iChannelNames */
        0                               /* iTerminal */
    });

    usbip_interface_add_descriptor(acif, (uint8_t[]){
        9,                              /* bLength */
        USB_DT_CS_INTERFACE,            /* bDescriptorType */
        UAC_AC_FEATURE_UNIT,            /* bDescriptorSubtype */
        UAC_FU_CAP,                     /* bUnitID */
        UAC_IT_MIC,                     /* bSourceID: the microphone terminal */
        1,                              /* bControlSize: 1 byte per channel */
        UAC_FU_BM_MUTE | UAC_FU_BM_VOLUME, /* bmaControls[0]: master */
        UAC_FU_BM_NONE,                 /* bmaControls[1]: mono - no per-channel control */
        0                               /* iFeature */
    });

    usbip_interface_add_descriptor(acif, (uint8_t[]){
        9,                              /* bLength */
        USB_DT_CS_INTERFACE,            /* bDescriptorType */
        UAC_AC_OUTPUT_TERMINAL,         /* bDescriptorSubtype */
        UAC_OT_USB,                     /* bTerminalID */
        USB_U16LE(UAC_TT_USB_STREAMING),/* wTerminalType: USB streaming */
        0,                              /* bAssocTerminal: none */
        UAC_FU_CAP,                     /* bSourceID: the capture Feature Unit */
        0                               /* iTerminal */
    });

    /* AudioStreaming - playback (speaker): alt 0 (idle) + alt 1 (iso OUT) */
    usbip_interface *spkif = usbip_function_add_interface(func, UAC_CC_AUDIO, UAC_SC_AUDIOSTREAMING, 0);
    usbip_interface_add_altsetting(spkif, 1);

    usbip_interface_add_descriptor(spkif, (uint8_t[]){
        7,                              /* bLength */
        USB_DT_CS_INTERFACE,            /* bDescriptorType */
        UAC_AS_GENERAL,                 /* bDescriptorSubtype */
        UAC_IT_PLAY,                    /* bTerminalLink: the playback Input Terminal */
        1,                              /* bDelay: 1 frame */
        USB_U16LE(UAC_FORMAT_PCM)       /* wFormatTag: PCM */
    });

    usbip_interface_add_descriptor(spkif, (uint8_t[]){
        11,                             /* bLength */
        USB_DT_CS_INTERFACE,            /* bDescriptorType */
        UAC_AS_FORMAT_TYPE,             /* bDescriptorSubtype */
        UAC_FORMAT_TYPE_I,              /* bFormatType: type I */
        UAC_SPK_CH,                     /* bNrChannels */
        UAC_SUBFRAME_SIZE,              /* bSubframeSize: bytes per sample */
        UAC_BIT_RESOLUTION,             /* bBitResolution */
        1,                              /* bSamFreqType: one discrete rate */
        USB_U24LE(UAC_RATE)             /* tSamFreq[0]: 48 kHz */
    });

    /* 9-byte UAC iso data EP descriptor (adds bRefresh + bSynchAddress) */
    usbip_interface_add_endpoint(spkif, (uint8_t[]){
        9,                              /* bLength */
        USB_DT_ENDPOINT,                /* bDescriptorType */
        UAC_EP_SPK,                     /* bEndpointAddress: iso OUT */
        USB_ISO | USB_EP_SYNC_ADAPTIVE | USB_EP_USAGE_DATA, /* bmAttributes */
        USB_U16LE(spk_mps),             /* wMaxPacketSize: one service interval of PCM */
        1,                              /* bInterval: every (micro)frame */
        0,                              /* bRefresh */
        0                               /* bSynchAddress: no feedback endpoint */
    });

    usbip_interface_add_descriptor(spkif, (uint8_t[]){
        7,                              /* bLength */
        USB_DT_CS_ENDPOINT,             /* bDescriptorType */
        UAC_EP_GENERAL,                 /* bDescriptorSubtype */
        0,                              /* bmAttributes: no sampling-freq control */
        0,                              /* bLockDelayUnits */
        USB_U16LE(0)                    /* wLockDelay */
    });

    /* AudioStreaming - capture (microphone): alt 0 (idle) + alt 1 (iso IN) */
    usbip_interface *micif = usbip_function_add_interface(func, UAC_CC_AUDIO, UAC_SC_AUDIOSTREAMING, 0);
    usbip_interface_add_altsetting(micif, 1);

    usbip_interface_add_descriptor(micif, (uint8_t[]){
        7,                              /* bLength */
        USB_DT_CS_INTERFACE,            /* bDescriptorType */
        UAC_AS_GENERAL,                 /* bDescriptorSubtype */
        UAC_OT_USB,                     /* bTerminalLink: the capture Output Terminal */
        1,                              /* bDelay: 1 frame */
        USB_U16LE(UAC_FORMAT_PCM)       /* wFormatTag: PCM */
    });

    usbip_interface_add_descriptor(micif, (uint8_t[]){
        11,                             /* bLength */
        USB_DT_CS_INTERFACE,            /* bDescriptorType */
        UAC_AS_FORMAT_TYPE,             /* bDescriptorSubtype */
        UAC_FORMAT_TYPE_I,              /* bFormatType: type I */
        UAC_MIC_CH,                     /* bNrChannels */
        UAC_SUBFRAME_SIZE,              /* bSubframeSize: bytes per sample */
        UAC_BIT_RESOLUTION,             /* bBitResolution */
        1,                              /* bSamFreqType: one discrete rate */
        USB_U24LE(UAC_RATE)             /* tSamFreq[0]: 48 kHz */
    });

    usbip_interface_add_endpoint(micif, (uint8_t[]){
        9,                              /* bLength */
        USB_DT_ENDPOINT,                /* bDescriptorType */
        UAC_EP_MIC,                     /* bEndpointAddress: iso IN */
        USB_ISO | USB_EP_SYNC_ASYNC | USB_EP_USAGE_DATA,    /* bmAttributes */
        USB_U16LE(mic_mps),             /* wMaxPacketSize: one service interval of PCM */
        1,                              /* bInterval: every (micro)frame */
        0,                              /* bRefresh */
        0                               /* bSynchAddress: no feedback endpoint */
    });

    usbip_interface_add_descriptor(micif, (uint8_t[]){
        7,                              /* bLength */
        USB_DT_CS_ENDPOINT,             /* bDescriptorType */
        UAC_EP_GENERAL,                 /* bDescriptorSubtype */
        0,                              /* bmAttributes: no sampling-freq control */
        0,                              /* bLockDelayUnits */
        USB_U16LE(0)                    /* wLockDelay */
    });

    return 0;
}

const usbip_device_class usbip_device_uac = {
    .name = "uac",
    .bInterfaceClass = UAC_CC_AUDIO,
    .build = uac_build,
    .control    = uac_control,
    .set_alt    = uac_set_alt,
    .on_iso     = uac_on_iso,
    .state_size = sizeof(struct uac_state),
};

uac_audio *uac_add(usbip_device *dev, const uac_opts *opts)
{
    usbip_function *func = usbip_device_add_class(dev, &usbip_device_uac, opts);

    return handle_of(func);
}
