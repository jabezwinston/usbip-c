/*
 * Copyright (C) 2026 Jabez Winston
 *
 * SPDX-License-Identifier: MIT
 *
 * Compile check: one translation unit may include BOTH public role headers
 * (their shared usbip.h content is guarded), and the version string agrees with
 * its numeric macros (the Windows resource is built from the numbers, so drift
 * would ship a .dll whose FileVersion contradicts its own header). Referenced
 * by `make check`.
 */
#include "usbip-device.h"
#include "usbip-host.h"

#define HDR_STR_(x) #x
#define HDR_STR(x)  HDR_STR_(x)
_Static_assert(__builtin_strcmp(USBIP_VERSION,
                                HDR_STR(USBIP_VERSION_MAJOR) "." HDR_STR(USBIP_VERSION_MINOR) "."
                                HDR_STR(USBIP_VERSION_MICRO)) == 0,
               "USBIP_VERSION and USBIP_VERSION_MAJOR/MINOR/MICRO have drifted apart");

const char *(*hdr_both_strerror)(int) = usb_strerror;
usbip_device *(*hdr_both_create)(uint16_t, uint16_t) = usbip_device_create;
int (*hdr_both_init)(usbip_host_context **) = usbip_host_init;
