/**
 * Copyright (C) 2026 Jabez Winston
 *
 * SPDX-License-Identifier: MIT
 *
 * Date : 04-June-2026
 *
 * uvc_host.c - drive the UVC device with OUR C host iso API (no kernel, no root).
 *
 * Demonstrates the libusb-shaped isochronous API (usbip_host_alloc_transfer /
 * usbip_host_fill_iso_transfer / usbip_host_submit_transfer / iso_packet_desc[]): import the
 * camera, negotiate Probe/Commit, SET_INTERFACE(alt 1), then reassemble UVC
 * payloads into frames. Run uvc_device first (it serves on :3240).
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "usbip-host.h"

#define EP_DEFAULT 0x81
#define PROBE_LEN  34

/* find the VideoStreaming interface number + its isochronous IN endpoint */
static int parse_config(const uint8_t *cfg, int len, int *vs_iface, uint8_t *iso_ep) {
    int offset = 0;
    int cur_iface = -1;
    int in_videostreaming = 0;
    int found = 0;
    while (offset + 2 <= len) {
        const uint8_t *desc = cfg + offset;
        int desc_len = desc[0] ? desc[0] : 2;
        if (desc[1] == 0x04 && offset + 9 <= len) {                  /* interface */
            cur_iface = desc[2];
            in_videostreaming = (desc[5] == 0x0E && desc[6] == 0x02);/* video, streaming */
            if (in_videostreaming) *vs_iface = cur_iface;
        } else if (desc[1] == 0x05 && in_videostreaming && offset + 7 <= len) {  /* endpoint */
            if ((desc[3] & 0x03) == 0x01) {                          /* isochronous */
                *iso_ep = desc[2];
                found = 1;
            }
        }
        offset += desc_len;
    }
    return found;
}

int main(int argc, char **argv) {
    int want_frames = argc > 1 ? atoi(argv[1]) : 5;

    usbip_host_context *ctx;
    if (usbip_host_init(&ctx) != USB_SUCCESS) return 1;          /* default: 127.0.0.1:3240 */
    usbip_host_handle *handle = usbip_host_open_vid_pid(ctx, 0x1209, 0x000e);
    if (!handle) {
        fprintf(stderr, "open 1209:000e failed (is uvc_device running?)\n");
        return 1;
    }
    usbip_host_set_configuration(handle, 1);

    uint8_t cfg[512];
    int cfg_len = usbip_host_control_transfer(handle, 0x80, 0x06, 0x0200, 0, cfg, sizeof(cfg), 1000);
    int vs_iface = 1;
    uint8_t iso_ep = EP_DEFAULT;
    if (cfg_len < 0 || !parse_config(cfg, cfg_len, &vs_iface, &iso_ep)) {
        fprintf(stderr, "no UVC isochronous endpoint in config descriptor\n");
        return 1;
    }
    printf("imported UVC camera: VideoStreaming iface %d, iso EP %#04x\n", vs_iface, iso_ep);

    /* Probe/Commit: ask for format 1, frame 1 */
    uint8_t probe[PROBE_LEN] = {0};
    probe[2] = 1; probe[3] = 1;
    usbip_host_control_transfer(handle, 0x21, 0x01, 0x0100, (uint16_t)vs_iface, probe, PROBE_LEN, 1000); /* SET PROBE */
    usbip_host_control_transfer(handle, 0xA1, 0x81, 0x0100, (uint16_t)vs_iface, probe, PROBE_LEN, 1000); /* GET PROBE */
    uint32_t max_frame = (uint32_t)probe[18] | (probe[19]<<8) | (probe[20]<<16) | ((uint32_t)probe[21]<<24);
    usbip_host_control_transfer(handle, 0x21, 0x01, 0x0200, (uint16_t)vs_iface, probe, PROBE_LEN, 1000); /* SET COMMIT */
    usbip_host_set_interface_alt_setting(handle, vs_iface, 1);   /* start streaming */
    printf("negotiated: dwMaxVideoFrameSize=%u, streaming on (alt 1)\n", max_frame);

    /* isochronous IN: reassemble payloads into frames */
    const int n_packets = 32;
    const int packet_size = 1023;
    usbip_host_transfer *transfer = usbip_host_alloc_transfer(n_packets);
    uint8_t *buffer = malloc((size_t)n_packets * packet_size);
    uint8_t *frame = malloc(max_frame ? max_frame : 1);
    usbip_host_fill_iso_transfer(transfer, handle, iso_ep, buffer, n_packets * packet_size, n_packets, 1000);
    usbip_host_set_iso_packet_lengths(transfer, packet_size);

    int frames = 0;
    int frame_pos = 0;
    int rc = 1;
    uint8_t first_pixel = 0;
    for (int iter = 0; iter < 2000 && frames < want_frames; iter++) {
        if (usbip_host_submit_transfer(transfer) != USB_SUCCESS) {
            fprintf(stderr, "iso submit failed\n");
            break;
        }
        for (int i = 0; i < n_packets; i++) {
            int actual_len = (int)transfer->iso_packet_desc[i].actual_length;
            if (actual_len < 2) continue;
            uint8_t *packet = usbip_host_get_iso_packet_buffer_simple(transfer, i);
            int header_len = packet[0];
            uint8_t info = packet[1];
            int payload_len = actual_len - header_len;
            if (payload_len > 0 && frame_pos + payload_len <= (int)max_frame) {
                memcpy(frame + frame_pos, packet + header_len, payload_len);
                frame_pos += payload_len;
            }
            if (info & 0x02) {                            /* EOF: frame complete */
                if (frames == 0) first_pixel = frame[0];
                frames++;
                frame_pos = 0;
            }
        }
    }
    if (frames >= want_frames) {
        rc = 0;
        printf("OK: reassembled %d frames over iso; frame size %u, first Y=%u (expect ~235 white bar)\n",
               frames, max_frame, first_pixel);
    } else {
        fprintf(stderr, "only %d/%d frames\n", frames, want_frames);
    }

    usbip_host_free_transfer(transfer); free(buffer); free(frame);
    usbip_host_set_interface_alt_setting(handle, vs_iface, 0);
    usbip_host_close(handle); usbip_host_exit(ctx);
    return rc;
}
