/**
 * Copyright (C) 2026 Jabez Winston
 *
 * SPDX-License-Identifier: MIT
 *
 * Date : 05-June-2026
 *
 * wav.c - tiny streaming WAV (RIFF/PCM) writer. See wav.h. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <usbip.h>

#include "wav.h"

struct wav {
    FILE         *fp;
    unsigned long data_bytes;
};

wav *wav_open(const char *path, int rate, int channels, int bits) {
    wav *writer = calloc(1, sizeof(*writer));
    if (!writer) return NULL;
    writer->fp = fopen(path, "wb");
    if (!writer->fp) {
        free(writer);
        return NULL;
    }

    uint32_t byte_rate = (uint32_t)rate * channels * (bits / 8);
    uint16_t block_align = (uint16_t)(channels * (bits / 8));

    /* 44-byte canonical RIFF/PCM header; the two size fields are patched on close. */
    uint8_t header[44];
    memcpy(header, "RIFF", 4);       usb_put_le32(header + 4, 36);      /* riff size (patched) */
    memcpy(header + 8, "WAVE", 4);
    memcpy(header + 12, "fmt ", 4);  usb_put_le32(header + 16, 16);     /* fmt chunk size */
    usb_put_le16(header + 20, 1);                                       /* PCM */
    usb_put_le16(header + 22, (uint16_t)channels);
    usb_put_le32(header + 24, (uint32_t)rate);
    usb_put_le32(header + 28, byte_rate);
    usb_put_le16(header + 32, block_align);
    usb_put_le16(header + 34, (uint16_t)bits);
    memcpy(header + 36, "data", 4);  usb_put_le32(header + 40, 0);      /* data size (patched) */
    fwrite(header, 1, sizeof(header), writer->fp);
    return writer;
}

void wav_write(wav *writer, const void *data, int len) {
    if (!writer || len <= 0) return;
    fwrite(data, 1, (size_t)len, writer->fp);
    writer->data_bytes += (unsigned long)len;
}

void wav_close(wav *writer) {
    if (!writer) return;
    uint8_t field[4];
    fflush(writer->fp);
    if (fseek(writer->fp, 4, SEEK_SET) == 0) {           /* patch the RIFF chunk size */
        usb_put_le32(field, (uint32_t)(36 + writer->data_bytes));
        fwrite(field, 1, 4, writer->fp);
    }
    if (fseek(writer->fp, 40, SEEK_SET) == 0) {          /* patch the data chunk size */
        usb_put_le32(field, (uint32_t)writer->data_bytes);
        fwrite(field, 1, 4, writer->fp);
    }
    fclose(writer->fp);
    free(writer);
}
