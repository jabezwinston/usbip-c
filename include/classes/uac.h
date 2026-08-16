/*
 * Copyright (C) 2026 Jabez Winston
 *
 * SPDX-License-Identifier: MIT
 *
 * Date : 05-June-2026
 */
/**
 * @file classes/uac.h
 * @ingroup class_uac
 * @brief USB Audio Class 1.0 (UAC1) device class: speaker + microphone, app API.
 *
 * Presents a full-speed UAC1 audio device: an AudioControl interface (two
 * playback/capture terminal chains, each with a mute+volume Feature Unit) plus two
 * AudioStreaming interfaces - a stereo speaker (isochronous OUT) and a mono
 * microphone (isochronous IN), both 48 kHz / 16-bit PCM. The microphone streams a
 * built-in 440 Hz synthetic tone (or app-supplied frames); the speaker is a sink
 * that meters the level and hands the PCM to the app. Built on usbip_device.h only.
 */
#ifndef UAC_H
#define UAC_H

#include "usb_class.h"

/**
 * @addtogroup class_uac
 * @{
 */

/**
 * One USB Audio function: a single instance of the class on a device.
 *
 * Opaque - create one with uac_add() and use the functions below. Distinct from
 * the other classes' handles, so mixing them up is a compile error rather than a
 * misread state block.
 */
typedef struct uac_audio uac_audio;

/** UAC1 configuration: microphone source + speaker sink callbacks. */
typedef struct {
    /**
     * Microphone source (optional). Fill @p buf with mono 16-bit-LE capture samples;
     * return `< 0` to use the built-in 440 Hz synthetic tone.
     * @param user    the @c user pointer.
     * @param buf     output buffer (@p frames × 2 bytes).
     * @param frames  number of mono samples requested.
     * @param index   running mono-sample counter (the phase origin).
     * @return bytes written, or < 0 for the built-in tone.
     */
    int  (*mic_source)(void *user, uint8_t *buf, int frames, uint32_t index);

    /**
     * Speaker sink (optional). Receives stereo 16-bit-LE playback audio the host
     * pushed to the speaker (once per isochronous OUT transfer).
     * @param user  the @c user pointer.
     * @param data  the PCM bytes.
     * @param len   number of bytes in @p data.
     */
    void (*spk_sink)(void *user, const uint8_t *data, int len);

    /**
     * Human-readable log hook (optional).
     * @param user  the @c user pointer.
     * @param text  a NUL-terminated description.
     */
    void (*on_event)(void *user, const char *text);
    void *user;                     /**< Opaque pointer passed to the callbacks above */
} uac_opts;

/**
 * Add a UAC1 audio interface (speaker + microphone) to a device.
 *
 * Leaves the device class triple at 0/0/0 (UAC1 predates the IAD; the AudioControl
 * header collects the streaming interfaces).
 * @param dev     the device.
 * @param opts  microphone source + speaker sink callbacks (see ::uac_opts).
 * @return the new function, or `NULL` on error.
 */
uac_audio *uac_add(usbip_device *dev, const uac_opts *opts);

/**
 * Fill a buffer with the built-in synthetic microphone tone.
 *
 * Writes @p frames mono 16-bit-LE samples of a 440 Hz sine (48 kHz) starting at
 * sample @p index. Pure integer, so it is byte-identical to the Python
 * `uac.tone()` - exposed for cross-language parity tests.
 * @param buf     output buffer (>= `frames * 2` bytes).
 * @param frames  number of mono samples to write.
 * @param index   running sample counter (the phase origin).
 */
void uac_tone(uint8_t *buf, int frames, uint32_t index);

extern const usbip_device_class usbip_device_uac;   /**< The UAC1 audio class, for usbip_device_add_class() */

/** @} */

#endif /* UAC_H */
