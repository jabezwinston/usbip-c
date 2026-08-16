/**
 * Copyright (C) 2026 Jabez Winston
 *
 * SPDX-License-Identifier: MIT
 *
 * Date : 31-July-2026
 *
 * multi_device.c - TWO independent USB devices served from ONE process: a HID
 * keyboard and a CDC-ACM serial port, each with its own VID/PID, its own
 * descriptors and its own busid.
 *
 * This is the counterpart to cdc_hid_device.c, which puts the same two classes
 * on ONE composite device. Here they are separate devices: the host enumerates
 * two, attaches them independently, and either can be detached without touching
 * the other. All it takes is plugging both onto the same transport - the listener
 * is shared and each device is named by its busid.
 *
 *   ./multi_device                                  # serve both on :3240
 *   usbip list -r 127.0.0.1                         # 1-1 keyboard, 1-2 serial
 *   attach BOTH busids with a USB/IP client -- they are two separate devices
 *   (Linux: sudo modprobe vhci-hcd ; sudo usbip attach -r 127.0.0.1 -b 1-1  and  -b 1-2)
 *   then write to the serial port: what you send is echoed back AND typed
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "_support/cmdline.h"
#include "classes/cdc_acm.h"
#include "classes/hid.h"

/* Two devices, two identities. The serial port takes the pid after the keyboard's. */
#define KBD_VID   0x1209
#define KBD_PID   0x0014
#define SER_VID   0x1209
#define SER_PID   0x0015

/* 8-byte Input (modifiers, reserved, 6 key codes) + 1-byte LED Output: the boot
 * keyboard report descriptor, same bytes as hid_device.c's keyboard profile. */
static const uint8_t RD_KEYBOARD[] = {
    HID_USAGE_PAGE(0x01), HID_USAGE(0x06),           /* Generic Desktop, Keyboard */
    HID_COLLECTION(0x01),                            /*   Application */
      HID_USAGE_PAGE(0x07),                          /*   Keyboard/Keypad */
      HID_USAGE_MIN(0xE0), HID_USAGE_MAX(0xE7),      /*   Left Control .. Right GUI */
      HID_LOGICAL_MIN(0), HID_LOGICAL_MAX(1),
      HID_REPORT_SIZE(1), HID_REPORT_COUNT(8), HID_INPUT(0x02),      /* 8 modifier bits */
      HID_REPORT_COUNT(1), HID_REPORT_SIZE(8), HID_INPUT(0x03),      /* reserved byte */
      HID_REPORT_COUNT(5), HID_REPORT_SIZE(1),
      HID_USAGE_PAGE(0x08), HID_USAGE_MIN(1), HID_USAGE_MAX(5),      /* LEDs: Num .. Kana */
      HID_OUTPUT(0x02),                                              /* 5 LED bits */
      HID_REPORT_COUNT(1), HID_REPORT_SIZE(3), HID_OUTPUT(0x03),     /* 3 bits padding */
      HID_REPORT_COUNT(6), HID_REPORT_SIZE(8),
      HID_LOGICAL_MIN(0), HID_LOGICAL_MAX(101),
      HID_USAGE_PAGE(0x07), HID_USAGE_MIN(0), HID_USAGE_MAX(101),
      HID_INPUT(0x00),                                               /* 6 key codes (array) */
    HID_END_COLLECTION,
};

static hid_iface *keyboard;

/* minimal ASCII -> HID Usage (Keyboard/Keypad page) */
static int key_for(char ch, uint8_t *mod, uint8_t *code)
{
    *mod = 0;
    if (ch >= 'a' && ch <= 'z')
    {
        *code = (uint8_t)(0x04 + (ch - 'a'));
        return 1;
    }
    if (ch >= 'A' && ch <= 'Z')
    {
        *mod = 0x02; /* Left Shift */
        *code = (uint8_t)(0x04 + (ch - 'A'));
        return 1;
    }
    if (ch >= '1' && ch <= '9')
    {
        *code = (uint8_t)(0x1E + (ch - '1'));
        return 1;
    }
    if (ch == '0')
    {
        *code = 0x27;
        return 1;
    }
    if (ch == ' ')
    {
        *code = 0x2C;
        return 1;
    }
    if (ch == '\n' || ch == '\r')
    {
        *code = 0x28;
        return 1;
    }
    return 0;
}

static void type_char(char ch)
{
    uint8_t mod;
    uint8_t code;

    if (!key_for(ch, &mod, &code))
        return;

    uint8_t press[8] = {mod, 0, code, 0, 0, 0, 0, 0};
    uint8_t release[8] = {0};
    hid_send_report(keyboard, press, sizeof(press)); /* key down */
    usleep(20000);
    hid_send_report(keyboard, release, sizeof(release)); /* key up */
    usleep(20000);
}

/* Independent on the wire, not in the app: bytes arriving on the serial port are
 * echoed there AND typed on the keyboard, so one process is visibly driving both. */
static void on_rx(cdc_port *port, void *user, const void *data, int len)
{
    (void)user;
    const char *bytes = data;
    fprintf(stderr, "[serial] RX %d bytes -> echo + type on the keyboard\n", len);
    cdc_acm_send(port, data, len);

    for (int i = 0; i < len; i++)
        type_char(bytes[i]);
}

static void on_open(cdc_port *port, void *user, const cdc_line_coding *line_coding)
{
    (void)port;
    (void)user;
    fprintf(stderr, "[serial] port OPENED @ %u baud\n", line_coding->baud);
}

static void on_close(cdc_port *port, void *user)
{
    (void)port;
    (void)user;
    fprintf(stderr, "[serial] port CLOSED\n");
}

static void keyboard_leds(hid_iface *iface, void *user, const void *data, int len)
{
    (void)iface;
    (void)user;
    uint8_t leds = len > 0 ? ((const uint8_t *)data)[0] : 0;
    const char *num_lock    = (leds & 0x01) ? "on" : "off";
    const char *caps_lock   = (leds & 0x02) ? "on" : "off";
    const char *scroll_lock = (leds & 0x04) ? "on" : "off";

    fprintf(stderr, "[keyboard] LEDs: NumLock=%s CapsLock=%s ScrollLock=%s\n",
            num_lock, caps_lock, scroll_lock);
}

int main(int argc, char **argv)
{
    cmdline_opts cli_opts = cmdline_parse(argc, argv, &(cmdline_spec){
        .vid = KBD_VID,
        .pid = KBD_PID,
        .high_speed_opt = 1,
        .notes = "serves a HID keyboard on busid 1-1 and a CDC-ACM port on 1-2",
    });

    usb_speed speed = cli_opts.high_speed ? USB_SPEED_HIGH : USB_SPEED_FULL;

    /* --- device 1: the HID keyboard ------------------------------------- */
    usbip_device *kbd = usbip_device_create(cli_opts.vid, cli_opts.pid);
    usbip_device_set_speed(kbd, speed);
    usbip_device_set_strings(kbd, "USB over IP", "USBIP Keyboard", "0014");

    hid_opts hopts = {
        .report_desc = RD_KEYBOARD,
        .report_desc_len = sizeof(RD_KEYBOARD),
        .subclass = HID_SUBCLASS_BOOT,
        .protocol = HID_PROTOCOL_KEYBOARD,
        .in_ep = 0x81,
        .in_mps = 8,
        .in_interval = 10,
        .on_output = keyboard_leds
    };
    keyboard = hid_add(kbd, &hopts);
    if (!keyboard)
    {
        fprintf(stderr, "hid_add failed\n");
        return 1;
    }

    /* --- device 2: the CDC-ACM serial port -------------------------------
     * A second usbip_device_create(), NOT a second function on the first: separate
     * descriptors, endpoint space (both use 0x81) and attach. */
    usbip_device *ser = usbip_device_create(SER_VID, SER_PID);
    usbip_device_set_speed(ser, speed);
    usbip_device_set_strings(ser, "USB over IP", "USBIP Serial", "0015");

    cdc_acm_opts copts = {
        .on_open = on_open,
        .on_close = on_close,
        .on_rx = on_rx,
        .name = "USBIP Serial"
    };
    if (!cdc_acm_add(ser, &copts))
    {
        fprintf(stderr, "cdc_acm_add failed\n");
        return 1;
    }

    /* --- serve both on one listener --------------------------------------
     * Same transport, two plugs. The first device takes busid 1-1, the second
     * 1-2; usbip_device_set_busid() before plugging would pin either. */
    usb_transport *transport = usbip_transport(cli_opts.host, cli_opts.port);
    int rc = usbip_device_plug(kbd, transport);

    if (rc != USB_SUCCESS)
    {
        fprintf(stderr, "usbip_device_plug(keyboard) failed (port %d in use?)\n", cli_opts.port);
        return 1;
    }

    rc = usbip_device_plug(ser, transport);

    if (rc != USB_SUCCESS)
    {
        fprintf(stderr, "usbip_device_plug(serial) failed\n");
        return 1;
    }

    const char *kbd_busid = usbip_device_get_busid(kbd);
    const char *ser_busid = usbip_device_get_busid(ser);

    fprintf(stderr, "[multi] serving 2 devices on %s:%d\n", cli_opts.host, cli_opts.port);
    fprintf(stderr, "[multi]   %s  %04x:%04x  HID keyboard\n", kbd_busid, cli_opts.vid, cli_opts.pid);
    fprintf(stderr, "[multi]   %s  %04x:%04x  CDC-ACM serial port\n", ser_busid, SER_VID, SER_PID);
    fprintf(stderr, "[multi] attach: sudo usbip attach -r 127.0.0.1 -b %s   (and -b %s)\n",
            kbd_busid, ser_busid);

    for (;;)
        sleep(1);   /* both devices are served by their own threads */
    return 0;
}
