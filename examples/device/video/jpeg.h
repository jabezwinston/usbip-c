/**
 * Copyright (C) 2026 Jabez Winston
 *
 * SPDX-License-Identifier: MIT
 *
 * Date : 04-June-2026
 *
 * jpeg.h - minimal self-contained baseline JPEG encoder (YCbCr 4:4:4).
 *
 * Encodes a packed-YUYV frame into a baseline JFIF JPEG. This is EXAMPLE/app
 * code (used by uvc_device.c to synthesize MJPEG frames) - the UVC class itself
 * only deals in YUYV and takes MJPEG frames from the app. Not a general image
 * library, just enough for the example camera.
 */
#ifndef JPEG_H
#define JPEG_H

#include <stdint.h>

/* Encode width*height*2 bytes of packed YUYV into `out` (capacity out_cap), quality
 * 1..100. Returns the JPEG length, or <0 if it does not fit. */
int jpeg_encode_yuyv(const uint8_t *yuyv, int width, int height, int quality,
                     uint8_t *out, int out_cap);

#endif /* JPEG_H */
