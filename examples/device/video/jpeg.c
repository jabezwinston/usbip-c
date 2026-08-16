/**
 * Copyright (C) 2026 Jabez Winston
 *
 * SPDX-License-Identifier: MIT
 *
 * Date : 04-June-2026
 *
 * jpeg.c - minimal baseline JPEG encoder (YCbCr 4:4:4), self-contained.
 *
 * Standard Annex K quant + Huffman tables; a plain (slow but correct) float DCT.
 * Enough to synthesize MJPEG webcam payloads. See jpeg.h.
 */
#include <math.h>
#include <string.h>

#include "jpeg.h"

/* Annex K.1 quantization tables (natural / row-major order) */
static const uint8_t QY[64] = {
    16,11,10,16,24,40,51,61, 12,12,14,19,26,58,60,55,
    14,13,16,24,40,57,69,56, 14,17,22,29,51,87,80,62,
    18,22,37,56,68,109,103,77, 24,35,55,64,81,104,113,92,
    49,64,78,87,103,121,120,101, 72,92,95,98,112,100,103,99 };
static const uint8_t QC[64] = {
    17,18,24,47,99,99,99,99, 18,21,26,66,99,99,99,99,
    24,26,56,99,99,99,99,99, 47,66,99,99,99,99,99,99,
    99,99,99,99,99,99,99,99, 99,99,99,99,99,99,99,99,
    99,99,99,99,99,99,99,99, 99,99,99,99,99,99,99,99 };

static const uint8_t ZZ[64] = {
     0, 1, 8,16, 9, 2, 3,10, 17,24,32,25,18,11, 4, 5,
    12,19,26,33,40,48,41,34, 27,20,13, 6, 7,14,21,28,
    35,42,49,56,57,50,43,36, 29,22,15,23,30,37,44,51,
    58,59,52,45,38,31,39,46, 53,60,61,54,47,55,62,63 };

/* Annex K.3 standard Huffman tables: bits[1..16] counts, then the symbol values */
static const uint8_t DCL_BITS[17] = {0, 0,1,5,1,1,1,1,1,1,0,0,0,0,0,0,0};
static const uint8_t DCL_VAL[12]  = {0,1,2,3,4,5,6,7,8,9,10,11};
static const uint8_t DCC_BITS[17] = {0, 0,3,1,1,1,1,1,1,1,1,1,0,0,0,0,0};
static const uint8_t DCC_VAL[12]  = {0,1,2,3,4,5,6,7,8,9,10,11};
static const uint8_t ACL_BITS[17] = {0, 0,2,1,3,3,2,4,3,5,5,4,4,0,0,1,0x7d};
static const uint8_t ACL_VAL[162] = {
    0x01,0x02,0x03,0x00,0x04,0x11,0x05,0x12,0x21,0x31,0x41,0x06,0x13,0x51,0x61,0x07,
    0x22,0x71,0x14,0x32,0x81,0x91,0xa1,0x08,0x23,0x42,0xb1,0xc1,0x15,0x52,0xd1,0xf0,
    0x24,0x33,0x62,0x72,0x82,0x09,0x0a,0x16,0x17,0x18,0x19,0x1a,0x25,0x26,0x27,0x28,
    0x29,0x2a,0x34,0x35,0x36,0x37,0x38,0x39,0x3a,0x43,0x44,0x45,0x46,0x47,0x48,0x49,
    0x4a,0x53,0x54,0x55,0x56,0x57,0x58,0x59,0x5a,0x63,0x64,0x65,0x66,0x67,0x68,0x69,
    0x6a,0x73,0x74,0x75,0x76,0x77,0x78,0x79,0x7a,0x83,0x84,0x85,0x86,0x87,0x88,0x89,
    0x8a,0x92,0x93,0x94,0x95,0x96,0x97,0x98,0x99,0x9a,0xa2,0xa3,0xa4,0xa5,0xa6,0xa7,
    0xa8,0xa9,0xaa,0xb2,0xb3,0xb4,0xb5,0xb6,0xb7,0xb8,0xb9,0xba,0xc2,0xc3,0xc4,0xc5,
    0xc6,0xc7,0xc8,0xc9,0xca,0xd2,0xd3,0xd4,0xd5,0xd6,0xd7,0xd8,0xd9,0xda,0xe1,0xe2,
    0xe3,0xe4,0xe5,0xe6,0xe7,0xe8,0xe9,0xea,0xf1,0xf2,0xf3,0xf4,0xf5,0xf6,0xf7,0xf8,
    0xf9,0xfa };
static const uint8_t ACC_BITS[17] = {0, 0,2,1,2,4,4,3,4,7,5,4,4,0,1,2,0x77};
static const uint8_t ACC_VAL[162] = {
    0x00,0x01,0x02,0x03,0x11,0x04,0x05,0x21,0x31,0x06,0x12,0x41,0x51,0x07,0x61,0x71,
    0x13,0x22,0x32,0x81,0x08,0x14,0x42,0x91,0xa1,0xb1,0xc1,0x09,0x23,0x33,0x52,0xf0,
    0x15,0x62,0x72,0xd1,0x0a,0x16,0x24,0x34,0xe1,0x25,0xf1,0x17,0x18,0x19,0x1a,0x26,
    0x27,0x28,0x29,0x2a,0x35,0x36,0x37,0x38,0x39,0x3a,0x43,0x44,0x45,0x46,0x47,0x48,
    0x49,0x4a,0x53,0x54,0x55,0x56,0x57,0x58,0x59,0x5a,0x63,0x64,0x65,0x66,0x67,0x68,
    0x69,0x6a,0x73,0x74,0x75,0x76,0x77,0x78,0x79,0x7a,0x82,0x83,0x84,0x85,0x86,0x87,
    0x88,0x89,0x8a,0x92,0x93,0x94,0x95,0x96,0x97,0x98,0x99,0x9a,0xa2,0xa3,0xa4,0xa5,
    0xa6,0xa7,0xa8,0xa9,0xaa,0xb2,0xb3,0xb4,0xb5,0xb6,0xb7,0xb8,0xb9,0xba,0xc2,0xc3,
    0xc4,0xc5,0xc6,0xc7,0xc8,0xc9,0xca,0xd2,0xd3,0xd4,0xd5,0xd6,0xd7,0xd8,0xd9,0xda,
    0xe2,0xe3,0xe4,0xe5,0xe6,0xe7,0xe8,0xe9,0xea,0xf2,0xf3,0xf4,0xf5,0xf6,0xf7,0xf8,
    0xf9,0xfa };

/* Huffman code lookup built from a (bits, values) table: sym -> (code, length) */
typedef struct { uint16_t code[256]; uint8_t size[256]; } huff;

static void huff_build(huff *h, const uint8_t *bits, const uint8_t *vals) {
    memset(h, 0, sizeof(*h));
    uint16_t code = 0;
    int k = 0;
    for (int len = 1; len <= 16; len++) {
        for (int i = 0; i < bits[len]; i++) {
            uint8_t sym = vals[k++];
            h->code[sym] = code;
            h->size[sym] = (uint8_t)len;
            code++;
        }
        code <<= 1;
    }
}

/* output + bit buffer */
typedef struct {
    uint8_t *out; int cap, len; int ok;
    uint32_t acc; int nbits;
} bw;

static void put_byte(bw *b, uint8_t value) {
    if (b->len < b->cap) b->out[b->len] = value; else b->ok = 0;
    b->len++;
}
static void put_bytes(bw *b, const uint8_t *p, int n) { for (int i = 0; i < n; i++) put_byte(b, p[i]); }
static void put_word(bw *b, uint16_t value) {
    put_byte(b, value >> 8);
    put_byte(b, value & 0xff);
}

static void put_bits(bw *b, uint16_t code, int size) {
    b->acc |= (uint32_t)(code & ((1u << size) - 1)) << (24 - b->nbits - size);
    b->nbits += size;
    while (b->nbits >= 8) {
        uint8_t byte = (uint8_t)(b->acc >> 16);
        put_byte(b, byte);
        if (byte == 0xFF) put_byte(b, 0x00);          /* byte stuffing */
        b->acc <<= 8;
        b->nbits -= 8;
    }
}
static void bit_flush(bw *b) {
    if (b->nbits > 0) {                                /* pad to a byte boundary with 1s */
        int pad = 8 - b->nbits;
        put_bits(b, (uint16_t)((1u << pad) - 1), pad);
    }
}

/* number of magnitude bits and the JPEG-coded value for v */
static int magnitude(int value, uint16_t *bits_out) {
    int abs_val = value < 0 ? -value : value;
    int n_bits = 0;
    while (abs_val) {
        abs_val >>= 1;
        n_bits++;
    }
    *bits_out = (uint16_t)(value < 0 ? (value - 1) : value);   /* one's-complement for negatives */
    return n_bits;
}

static void fdct(double *blk) {                        /* in-place 8x8 DCT-II, normalized */
    static double C[8][8];
    static int init = 0;
    if (!init) {
        for (int u = 0; u < 8; u++)
            for (int x = 0; x < 8; x++)
                C[u][x] = cos((2 * x + 1) * u * M_PI / 16.0) * (u == 0 ? 0.353553390593 : 0.5);
        init = 1;
    }
    double tmp[64];
    for (int y = 0; y < 8; y++)                         /* rows */
        for (int u = 0; u < 8; u++) {
            double sum = 0;
            for (int x = 0; x < 8; x++) sum += C[u][x] * blk[y * 8 + x];
            tmp[y * 8 + u] = sum;
        }
    for (int x = 0; x < 8; x++)                         /* cols */
        for (int v = 0; v < 8; v++) {
            double sum = 0;
            for (int y = 0; y < 8; y++) sum += C[v][y] * tmp[y * 8 + x];
            blk[v * 8 + x] = sum;
        }
}

static void scale_quant(const uint8_t *base, int quality, uint8_t *q) {
    int scale = quality < 50 ? 5000 / quality : 200 - quality * 2;
    for (int i = 0; i < 64; i++) {
        int value = (base[i] * scale + 50) / 100;

        if (value < 1)
            value = 1;
        if (value > 255)
            value = 255;

        q[i] = (uint8_t)value;
    }
}

/* encode one 8x8 block: level-shifted samples in `s` -> quantized Huffman output */
static void encode_block(bw *b, const double *s, const uint8_t *q,
                         const huff *dc, const huff *ac, int *prev_dc) {
    double blk[64];
    for (int i = 0; i < 64; i++) blk[i] = s[i];
    fdct(blk);

    int coef[64];
    for (int i = 0; i < 64; i++) {
        double v = blk[i] / q[i];
        coef[i] = (int)floor(v + 0.5);
    }

    /* DC: Huffman the magnitude category of the difference from the previous block */
    int diff = coef[0] - *prev_dc;
    *prev_dc = coef[0];
    uint16_t magnitude_bits;
    int category = magnitude(diff, &magnitude_bits);
    put_bits(b, dc->code[category], dc->size[category]);
    if (category) put_bits(b, magnitude_bits, category);

    /* AC in zig-zag order: (run-of-zeros, magnitude-category) symbols */
    int run = 0;
    for (int k = 1; k < 64; k++) {
        int coef_val = coef[ZZ[k]];
        if (coef_val == 0) {
            run++;
            continue;
        }
        while (run > 15) {                            /* ZRL: 16 zeros */
            put_bits(b, ac->code[0xF0], ac->size[0xF0]);
            run -= 16;
        }
        int ac_category = magnitude(coef_val, &magnitude_bits);
        int sym = (run << 4) | ac_category;
        put_bits(b, ac->code[sym], ac->size[sym]);
        put_bits(b, magnitude_bits, ac_category);
        run = 0;
    }
    if (run > 0) put_bits(b, ac->code[0x00], ac->size[0x00]);   /* EOB */
}

static void write_dht(bw *b, uint8_t id, const uint8_t *bits, const uint8_t *vals, int nval) {
    put_word(b, 0xFFC4);
    put_word(b, (uint16_t)(3 + 16 + nval));
    put_byte(b, id);
    put_bytes(b, bits + 1, 16);
    put_bytes(b, vals, nval);
}

int jpeg_encode_yuyv(const uint8_t *yuyv, int width, int height, int quality,
                     uint8_t *out, int out_cap) {
    if (quality < 1) quality = 1;
    if (quality > 100) quality = 100;
    uint8_t qy[64];
    uint8_t qc[64];
    scale_quant(QY, quality, qy);
    scale_quant(QC, quality, qc);
    huff dcl, dcc, acl, acc;
    huff_build(&dcl, DCL_BITS, DCL_VAL);
    huff_build(&dcc, DCC_BITS, DCC_VAL);
    huff_build(&acl, ACL_BITS, ACL_VAL);
    huff_build(&acc, ACC_BITS, ACC_VAL);

    bw b = { out, out_cap, 0, 1, 0, 0 };

    put_word(&b, 0xFFD8);                               /* SOI */
    /* APP0 / JFIF */
    put_word(&b, 0xFFE0); put_word(&b, 16);
    put_bytes(&b, (const uint8_t *)"JFIF\0", 5);
    put_byte(&b, 1); put_byte(&b, 1); put_byte(&b, 0);
    put_word(&b, 1); put_word(&b, 1); put_byte(&b, 0); put_byte(&b, 0);
    /* DQT (luma id 0, chroma id 1), stored in zig-zag order */
    put_word(&b, 0xFFDB); put_word(&b, 67); put_byte(&b, 0x00);
    for (int i = 0; i < 64; i++) put_byte(&b, qy[ZZ[i]]);
    put_word(&b, 0xFFDB); put_word(&b, 67); put_byte(&b, 0x01);
    for (int i = 0; i < 64; i++) put_byte(&b, qc[ZZ[i]]);
    /* SOF0: 3 components, all 1x1 sampling (4:4:4) */
    put_word(&b, 0xFFC0); put_word(&b, 17); put_byte(&b, 8);
    put_word(&b, (uint16_t)height); put_word(&b, (uint16_t)width); put_byte(&b, 3);
    put_byte(&b, 1); put_byte(&b, 0x11); put_byte(&b, 0);
    put_byte(&b, 2); put_byte(&b, 0x11); put_byte(&b, 1);
    put_byte(&b, 3); put_byte(&b, 0x11); put_byte(&b, 1);
    /* Huffman tables */
    write_dht(&b, 0x00, DCL_BITS, DCL_VAL, 12);
    write_dht(&b, 0x10, ACL_BITS, ACL_VAL, 162);
    write_dht(&b, 0x01, DCC_BITS, DCC_VAL, 12);
    write_dht(&b, 0x11, ACC_BITS, ACC_VAL, 162);
    /* SOS */
    put_word(&b, 0xFFDA); put_word(&b, 12); put_byte(&b, 3);
    put_byte(&b, 1); put_byte(&b, 0x00);
    put_byte(&b, 2); put_byte(&b, 0x11);
    put_byte(&b, 3); put_byte(&b, 0x11);
    put_byte(&b, 0); put_byte(&b, 63); put_byte(&b, 0);

    int prev_dc_y = 0;
    int prev_dc_u = 0;
    int prev_dc_v = 0;
    for (int my = 0; my < height; my += 8) {              /* one 8x8 macroblock at a time */
        for (int mx = 0; mx < width; mx += 8) {
            double Y[64];
            double Cb[64];
            double Cr[64];
            for (int j = 0; j < 8; j++) {
                int y = my + j; if (y >= height) y = height - 1;        /* clamp at edges */
                for (int i = 0; i < 8; i++) {
                    int x = mx + i; if (x >= width) x = width - 1;
                    const uint8_t *pair = yuyv + ((size_t)y * width + (x & ~1)) * 2;
                    int luma = yuyv[((size_t)y * width + x) * 2];       /* YUYV: Y per pixel */
                    int cb = pair[1];                                  /* Cb/Cr shared per pair */
                    int cr = pair[3];
                    Y[j * 8 + i]  = luma - 128;                        /* level-shift to [-128,127] */
                    Cb[j * 8 + i] = cb - 128;
                    Cr[j * 8 + i] = cr - 128;
                }
            }
            encode_block(&b, Y,  qy, &dcl, &acl, &prev_dc_y);
            encode_block(&b, Cb, qc, &dcc, &acc, &prev_dc_u);
            encode_block(&b, Cr, qc, &dcc, &acc, &prev_dc_v);
        }
    }
    bit_flush(&b);
    put_word(&b, 0xFFD9);                               /* EOI */
    return b.ok ? b.len : -1;
}
