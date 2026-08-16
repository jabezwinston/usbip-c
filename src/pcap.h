/*
 * Copyright (C) 2026 Jabez Winston
 *
 * SPDX-License-Identifier: MIT
 *
 * Date : 05-June-2026
 */
/**
 * @file pcap.h
 * @ingroup pcap
 * @brief Optional USB-traffic capture to a PCAPNG file (for Wireshark).
 *
 * Off by default. Enable by setting `USBIP_PCAPNG` in the environment - a file
 * path, or a flag value ("1"/"on"/"yes"/"true"/empty) to auto-name the capture
 * after the executable (e.g. `./hid_device` -> `hid_device.pcapng`, fallback
 * `usb_traffic.pcapng`) - or by calling usbip_pcap_open(). Every USB transfer is
 * written as the Linux usbmon Submit/Complete event pair
 * (DLT_USB_LINUX_MMAPPED, 220), so Wireshark applies its USB and class
 * dissectors exactly as for a real usbmon capture. Capture only OBSERVES
 * traffic; it never alters it. Byte-compatible twin: the Python library's usbip/pcap.py.
 */
#ifndef PCAP_H
#define PCAP_H

/**
 * @addtogroup pcap
 * @{
 */

/** Begin capturing. @p path may be NULL or a flag value (auto-name), an explicit
 *  file path, or a path containing "%p" (expands to the PID). Idempotent. */
void usbip_pcap_open(const char *path);
/** Stop capturing and flush/close the capture file. */
void usbip_pcap_close(void);

/** @} */

#endif
