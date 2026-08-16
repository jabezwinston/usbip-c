/**
 * Copyright (C) 2026 Jabez Winston
 *
 * SPDX-License-Identifier: MIT
 *
 * Date : 05-June-2026
 *
 * main.c - virtual USB Audio (UAC1) device over USB/IP: speaker + microphone.
 *
 *   ./audio_device                       # 48 kHz/16-bit: stereo speaker + mono mic (440 Hz)
 *   ./audio_device --out played.wav      # also record what the host plays to the speaker
 *
 * The microphone streams a built-in 440 Hz tone; the speaker is a sink that meters
 * the level (and optionally writes a .wav). Volume/mute are real Feature Unit controls.
 * Then, on Linux:
 *   attach it with a USB/IP client
 *   (Linux: sudo modprobe vhci-hcd ; sudo usbip attach -r 127.0.0.1 -b 1-1)
 *   then play to / record from it with your OS's sound tools
 *   (Linux: aplay -D plughw:CARD song.wav        -> speaker sink, and --out file
 *           arecord -D plughw:CARD -d 3 mic.wav  <- microphone tone)
 *   alsamixer                            # move the volume / mute controls
 */
#define _GNU_SOURCE
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "_support/cmdline.h"
#include "_support/logging.h"
#include "classes/uac.h"
#include "wav.h"

/* carried as uac_opts.user: the optional capture file + a grouped logger */
struct app {
    wav     *out;                /* speaker capture .wav, or NULL */
    FILE    *mic_fp;             /* mono-48k-16 PCM streamed as the microphone, or NULL */
    long     mic_off;            /* offset of the WAV data chunk (loop point) */
    usbip_logger log;            /* grouped event logger */
};

static void app_spk_sink(void *user, const uint8_t *data, int len) {
    struct app *app = user;
    if (app->out) wav_write(app->out, data, len);
}

/* Microphone source: stream `frames` mono-16-bit samples from the WAV, looping. */
static int app_mic_source(void *user, uint8_t *buf, int frames, uint32_t index) {
    (void)index;
    struct app *app = user;
    int want_bytes = frames * 2;
    int got = 0;
    while (got < want_bytes) {
        size_t n_read = fread(buf + got, 1, (size_t)(want_bytes - got), app->mic_fp);
        got += (int)n_read;
        if (n_read == 0) {                          /* hit EOF -> rewind to the data chunk */
            if (fseek(app->mic_fp, app->mic_off, SEEK_SET) != 0) break;
        }
    }
    return got;
}

/* Find the PCM "data" chunk in a RIFF/WAVE file; leaves the file positioned there. */
static long wav_find_data(FILE *fp) {
    uint8_t hdr[12], chunk[8];
    if (fread(hdr, 1, 12, fp) != 12 || memcmp(hdr, "RIFF", 4) || memcmp(hdr + 8, "WAVE", 4))
        return -1;
    for (;;) {
        if (fread(chunk, 1, 8, fp) != 8) return -1;
        uint32_t size = usb_get_le32(chunk + 4);
        if (!memcmp(chunk, "data", 4)) return ftell(fp);
        if (fseek(fp, (long)((size + 1) & ~1u), SEEK_CUR) != 0) return -1;   /* skip, word-aligned */
    }
}

/* uac on_event sink: group consecutive identical events (default " (" key). */
static void app_log(void *user, const char *text) {
    usbip_log_cb(&((struct app *)user)->log, text);
}

static volatile sig_atomic_t g_stop = 0;
static void on_sig(int sig) {
    (void)sig;
    g_stop = 1;
}

struct audio_cli { const char *outfile; const char *micfile; int pace; };

static int audio_on_opt(int c, char *arg, void *u) {
    struct audio_cli *a = u;
    switch (c) {
    case 'o': a->outfile = arg; return 1;
    case 'm': a->micfile = arg; return 1;
    case 'n': a->pace = 0; return 1;
    }
    return 0;
}

int main(int argc, char **argv) {
    struct audio_cli cli = { .pace = 1 };
    static const cmdline_option options[] = {
        {"out",     CMDLINE_ARG_REQUIRED, 'o', "FILE.wav",
         "record what the host plays to the speaker"},
        {"mic-in",  CMDLINE_ARG_REQUIRED, 'm', "FILE.wav",
         "stream a mono 48 kHz 16-bit WAV as the microphone (looping)"},
        {"no-pace", CMDLINE_ARG_NONE,     'n', NULL,
         "free-run iso instead of pacing to real time"},
        {0, 0, 0, 0, 0},
    };
    cmdline_opts cli_opts = cmdline_parse(argc, argv, &(cmdline_spec){
        .vid = 0x1209,
        .pid = 0x0013,
        .high_speed_opt = 1,
        .options = options,
        .on_opt = audio_on_opt,
        .user = &cli,
    });

    const char *outfile = cli.outfile, *micfile = cli.micfile;
    int pace = cli.pace;

    struct app app;
    memset(&app, 0, sizeof(app));
    usbip_log_init(&app.log, "uac", cli_opts.verbose);
    if (outfile) {
        app.out = wav_open(outfile, 48000, 2, 16);     /* speaker is stereo 48k/16 */
        if (!app.out) {
            perror("wav_open");
            return 1;
        }
    }
    if (micfile) {
        app.mic_fp = fopen(micfile, "rb");
        if (!app.mic_fp) {
            perror("fopen mic-in");
            return 1;
        }
        app.mic_off = wav_find_data(app.mic_fp);
        if (app.mic_off < 0) {
            fprintf(stderr, "--mic-in: not a WAV (need mono 48k 16-bit)\n");
            return 1;
        }
    }

    //! [add]
    uac_opts uo = {
        .mic_source = app.mic_fp ? app_mic_source : NULL,  /* WAV file, or built-in 440 Hz tone */
        .spk_sink = app.out ? app_spk_sink : NULL,
        .on_event = app_log,
        .user = &app,
    };

    usbip_device *dev = usbip_device_create(cli_opts.vid, cli_opts.pid);
    usbip_device_set_strings(dev, "USB over IP", "USBIP Audio", "0013");
    usb_speed speed = cli_opts.high_speed ? USB_SPEED_HIGH : USB_SPEED_FULL;   /* HS = 125 µs iso */

    usbip_device_set_speed(dev, speed);
    uac_audio *audio = uac_add(dev, &uo);       /* speaker (iso OUT) + microphone (iso IN) */
    usbip_device_set_iso_pacing(dev, pace);     /* play at real time (--no-pace to free-run) */
    //! [add]

    if (!audio) {
        fprintf(stderr, "uac_add failed\n");
        return 1;
    }

    fprintf(stderr, "[uac] speaker(2ch) + mic(1ch) 48000/16  (%04x:%04x, %s, on %s:%d)  pacing=%s\n",
            cli_opts.vid, cli_opts.pid, cli_opts.high_speed ? "high speed" : "full speed",
            cli_opts.host, cli_opts.port, pace ? "real-time" : "off");
    if (outfile) fprintf(stderr, "[uac] recording speaker -> %s\n", outfile);
    if (micfile) fprintf(stderr, "[uac] microphone streams %s (looping)\n", micfile);
    fprintf(stderr, "[uac] attach: sudo usbip attach -r 127.0.0.1 -b 1-1\n");

    usb_transport *transport = usbip_transport(cli_opts.host, cli_opts.port);
    int rc = usbip_device_plug(dev, transport);

    if (rc != USB_SUCCESS) {
        fprintf(stderr, "usbip_device_plug failed (port %d in use?)\n", cli_opts.port);
        if (app.out) wav_close(app.out);
        return 1;
    }

    signal(SIGINT, on_sig);
    signal(SIGTERM, on_sig);
#ifdef _WIN32
    while (!g_stop) sleep(1);   /* no pause() on Windows; poll g_stop */
#else
    while (!g_stop) pause();
#endif

    usbip_device_unplug(dev);                               /* stop serving -> no more sink calls */
    usbip_log_flush(&app.log);
    if (app.out) {
        wav_close(app.out);
        fprintf(stderr, "[uac] wrote %s\n", outfile);
    }
    if (app.mic_fp) fclose(app.mic_fp);
    return 0;
}
