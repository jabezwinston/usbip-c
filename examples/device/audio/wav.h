/**
 * Copyright (C) 2026 Jabez Winston
 *
 * SPDX-License-Identifier: MIT
 *
 * Date : 05-June-2026
 *
 * wav.h - tiny streaming WAV (RIFF/PCM) writer for the audio example.
 *
 * Captures the PCM the host plays to the virtual speaker into a .wav file. The
 * header is written with placeholder sizes on open and patched on close, so the
 * file is valid even though the total length isn't known until streaming stops.
 */
#ifndef EXAMPLE_WAV_H
#define EXAMPLE_WAV_H

typedef struct wav wav;

/* Open `path` for writing `channels`-channel, `rate` Hz, `bits`-bit PCM. NULL on error. */
wav *wav_open(const char *path, int rate, int channels, int bits);
/* Append `len` bytes of interleaved little-endian PCM. */
void wav_write(wav *w, const void *data, int len);
/* Patch the header sizes and close. */
void wav_close(wav *w);

#endif /* EXAMPLE_WAV_H */
