/**
 * Copyright (C) 2026 Jabez Winston
 *
 * SPDX-License-Identifier: MIT
 *
 * Date : 05-June-2026
 *
 * hid_device.c - virtual USB HID device over USB/IP, built on the generic hid class.
 *
 *   ./hid_device                          # mouse (default): nudges the pointer
 *   ./hid_device --profile keyboard --type "hello"   # types a string once
 *   ./hid_device --profile raw           # vendor in/out reports (no class driver)
 *   ./hid_device --vid 0x1209 --pid 0x0011 --host 0.0.0.0 --port 3240
 *   attach it with a USB/IP client
 *   (Linux: sudo modprobe vhci-hcd ; sudo usbip attach -r 127.0.0.1 -b 1-1)
 *
 * One generic hid_add() drives every profile - only the Report descriptor and the
 * report-sending loop differ. The "raw" profile declares an interrupt-OUT endpoint
 * and an Output report, so the host can send data back (logged here).
 */
#define _GNU_SOURCE
#include <getopt.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "classes/hid.h"

/* ---- Report descriptors (built with the HID item macros; mirror python
 *      classes/device/hid.py, byte-for-byte) ------------------------------- */
static const uint8_t RD_MOUSE[] = {                  /* 4-byte report: buttons, dx, dy, wheel */
    HID_USAGE_PAGE(0x01), HID_USAGE(0x02),           /* Generic Desktop, Mouse */
    HID_COLLECTION(0x01),                            /*   Application */
      HID_USAGE(0x01),                               /*   Pointer */
      HID_COLLECTION(0x00),                          /*     Physical */
        HID_USAGE_PAGE(0x09),                        /*     Buttons */
        HID_USAGE_MIN(1), HID_USAGE_MAX(3),
        HID_LOGICAL_MIN(0), HID_LOGICAL_MAX(1),
        HID_REPORT_COUNT(3), HID_REPORT_SIZE(1), HID_INPUT(0x02),    /* 3 buttons */
        HID_REPORT_COUNT(1), HID_REPORT_SIZE(5), HID_INPUT(0x03),    /* 5 bits padding */
        HID_USAGE_PAGE(0x01),                        /*     Generic Desktop */
        HID_USAGE(0x30), HID_USAGE(0x31), HID_USAGE(0x38),           /* X, Y, Wheel */
        HID_LOGICAL_MIN(-127), HID_LOGICAL_MAX(127),
        HID_REPORT_SIZE(8), HID_REPORT_COUNT(3), HID_INPUT(0x06),    /* relative X/Y/wheel */
    HID_END_COLLECTION,
    HID_END_COLLECTION,
};

static const uint8_t RD_KEYBOARD[] = {               /* 8-byte Input + 1-byte LED Output */
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

static const uint8_t RD_VENDOR[] = {                 /* vendor page 0xFF00: 8-byte in + 8-byte out */
    HID_USAGE_PAGE16(0xFF00), HID_USAGE(0x01),
    HID_COLLECTION(0x01),                            /*   Application */
      HID_LOGICAL_MIN(0), HID_LOGICAL_MAX16(255),
    HID_REPORT_SIZE(8),
      HID_USAGE(0x01), HID_REPORT_COUNT(8), HID_INPUT(0x02),         /* 8-byte Input */
      HID_USAGE(0x01), HID_REPORT_COUNT(8), HID_OUTPUT(0x02),        /* 8-byte Output */
    HID_END_COLLECTION,
};

enum profile
{
    P_MOUSE,
    P_KEYBOARD,
    P_RAW
};

static int verbose;

static void on_output(hid_iface *func, void *user, const void *data, int len)
{
    (void)func;
    (void)user;
    const uint8_t *bytes = data;
    fprintf(stderr, "[hid] output report (%dB):", len);

    for (int i = 0; i < len && i < 16; i++)
        fprintf(stderr, " %02x", bytes[i]);

    fprintf(stderr, "\n");
}

/* --echo: bounce every interrupt-OUT report back on the interrupt-IN endpoint, so a
 * host can measure round-trip latency through the proxy path. */
static void echo_output(hid_iface *func, void *user, const void *data, int len)
{
    (void)user;
    hid_send_report(func, data, len);

    if (verbose)
        fprintf(stderr, "[hid] echo %dB\n", len);
}

/* Keyboard LED Output report (HID LED usage page 0x08): bit0 NumLock, bit1 CapsLock,
 * bit2 ScrollLock, bit3 Compose, bit4 Kana. Sent via SET_REPORT(Output) on EP0 --
 * boot keyboards have no interrupt-OUT endpoint. */
static void keyboard_leds(hid_iface *func, void *user, const void *data, int len)
{
    (void)func;
    (void)user;
    uint8_t leds = len > 0 ? ((const uint8_t *)data)[0] : 0;
    const char *num_lock    = (leds & 0x01) ? "on" : "off";
    const char *caps_lock   = (leds & 0x02) ? "on" : "off";
    const char *scroll_lock = (leds & 0x04) ? "on" : "off";
    const char *compose     = (leds & 0x08) ? " Compose=on" : "";
    const char *kana        = (leds & 0x10) ? " Kana=on" : "";

    fprintf(stderr, "[hid] LEDs: NumLock=%s CapsLock=%s ScrollLock=%s%s%s\n",
            num_lock, caps_lock, scroll_lock, compose, kana);
}

/* minimal ASCII -> HID Usage (Keyboard/Keypad page) for --type */
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
    if (ch == '\n')
    {
        *code = 0x28;
        return 1;
    }
    return 0;
}

static void type_string(hid_iface *func, const char *text)
{
    for (const char *cursor = text; *cursor; cursor++)
    {
        uint8_t mod;
        uint8_t code;

        if (!key_for(*cursor, &mod, &code))
            continue;

        uint8_t press[8] = {mod, 0, code, 0, 0, 0, 0, 0};
        uint8_t release[8] = {0};
        hid_send_report(func, press, 8); /* key down */
        usleep(20000);
        hid_send_report(func, release, 8); /* key up   */
        usleep(20000);
    }
    fprintf(stderr, "[hid] typed \"%s\"\n", text);
}

static void wait_here()
{
#ifdef _WIN32
    for (;;)  sleep(1); /* no pause() on Windows; idle */
#else
    for (;;)  pause();
#endif
}

int main(int argc, char **argv)
{
    const char *host = "0.0.0.0";
    const char *text = NULL;
    int port = 3240;
    int profile = P_MOUSE;
    int echo = 0;
    int high_speed = 0;
    uint16_t vid = 0x1209;
    uint16_t pid = 0x0011;

    static struct option opts[] = {
        {"profile", 1, 0, 'r'},
        {"type", 1, 0, 't'},
        {"echo", 0, 0, 'e'},
        {"high-speed", 0, 0, 'S'},
        {"verbose", 0, 0, 'v'},
        {"vid", 1, 0, 'V'},
        {"pid", 1, 0, 'P'},
        {"host", 1, 0, 'H'},
        {"port", 1, 0, 'p'},
        {"help", 0, 0, 'h'},
        {0, 0, 0, 0},
    };
    int opt;
    while ((opt = getopt_long(argc, argv, "r:t:evSV:P:H:p:h", opts, NULL)) != -1)
    {
        switch (opt)
        {
            case 'r':
                if (!strcmp(optarg, "mouse"))
                    profile = P_MOUSE;
                else if (!strcmp(optarg, "keyboard"))
                    profile = P_KEYBOARD;
                else if (!strcmp(optarg, "raw"))
                    profile = P_RAW;
                else
                {
                    fprintf(stderr, "bad --profile '%s' (mouse|keyboard|raw)\n", optarg);
                    return 2;
                }
                break;
            case 't':
                text = optarg;
                break;
            case 'e':
                echo = 1;
                break;
            case 'S':
                high_speed = 1;
                break;
            case 'v':
                verbose = 1;
                break;
            case 'V':
                vid = (uint16_t)strtol(optarg, NULL, 0);
                break;
            case 'P':
                pid = (uint16_t)strtol(optarg, NULL, 0);
                break;
            case 'H':
                host = optarg;
                break;
            case 'p':
                port = atoi(optarg);
                break;
            case 'h':
            default:
                fprintf(stderr,
                        "usage: %s [--profile mouse|keyboard|raw] [--type TEXT] [--echo]\n"
                        "          [--high-speed] [--verbose] [--vid V] [--pid P] [--host H] [--port N]\n"
                        "  --echo   raw profile: bounce interrupt-OUT reports back on IN\n"
                        "           (round-trip latency test; host drives the report rate)\n",
                        argv[0]);
                return opt == 'h' ? 0 : 2;
        }
    }

    hid_opts hopts = {
        .in_ep = 0x81,
        .in_mps = 8,
        .in_interval = 10
    };

    const char *pname;
    switch (profile)
    {
        //! [keyboard]
        case P_KEYBOARD:
            hopts.report_desc = RD_KEYBOARD;
            hopts.report_desc_len = sizeof(RD_KEYBOARD);
            hopts.subclass = HID_SUBCLASS_BOOT;
            hopts.protocol = HID_PROTOCOL_KEYBOARD;
            hopts.name = "USBIP Keyboard";
            hopts.on_output = keyboard_leds; /* Num/Caps/Scroll Lock LED reports */
            pname = "keyboard";
            break;
        //! [keyboard]

        case P_RAW:
            hopts.report_desc = RD_VENDOR;
            hopts.report_desc_len = sizeof(RD_VENDOR);
            hopts.in_mps = 8;
            hopts.out_ep = 0x01;
            hopts.out_mps = 8;
            hopts.on_output = echo ? echo_output : on_output;
            hopts.name = "USBIP Raw HID";
            pname = "raw";
            break;

        default:
            hopts.report_desc = RD_MOUSE;
            hopts.report_desc_len = sizeof(RD_MOUSE);
            hopts.subclass = HID_SUBCLASS_BOOT;
            hopts.protocol = HID_PROTOCOL_MOUSE;
            hopts.name = "USBIP Mouse";
            pname = "mouse";
            break;
    }

    //! [add]
    usbip_device *dev = usbip_device_create(vid, pid);
    usb_speed speed = high_speed ? USB_SPEED_HIGH : USB_SPEED_FULL;

    usbip_device_set_speed(dev, speed);
    usbip_device_set_strings(dev, "USB over IP", "USBIP HID", "0011");
    hid_iface *func = hid_add(dev, &hopts);     /* any Report descriptor: keyboard, mouse, raw */
    //! [add]

    if (!func)
    {
        fprintf(stderr, "hid_add failed\n");
        return 1;
    }

    fprintf(stderr, "[hid] profile %s  (%04x:%04x on %s:%d)\n", pname, vid, pid, host, port);
    fprintf(stderr, "[hid] attach: sudo usbip attach -r 127.0.0.1 -b 1-1\n");

    usb_transport *transport = usbip_transport(host, port);
    int rc = usbip_device_plug(dev, transport);

    if (rc != USB_SUCCESS)
    {
        fprintf(stderr, "usbip_device_plug failed (port %d in use?)\n", port);
        return 1;
    }

    if (profile == P_KEYBOARD)
    {
        if (text)
            type_string(func, text);

        wait_here();
    }
    else if (profile == P_RAW)
    {
        if (echo)
        { /* host drives the rate via OUT reports */
            wait_here();
        }
        uint8_t report[8] = {0};
        for (;;)
        { /* push an incrementing counter */
            report[0]++;
            hid_send_report(func, report, sizeof(report));
            if (verbose)
                fprintf(stderr, "[hid] input report %u\n", report[0]);
            sleep(1);
        }
    }
    else
    { /* mouse: trace a small square */
        const int8_t dx[] = {12, 0, -12, 0};
        const int8_t dy[] = {0, 12, 0, -12};
        for (unsigned i = 0;; i++)
        {
            uint8_t report[4] = {0, (uint8_t)dx[i & 3], (uint8_t)dy[i & 3], 0};
            hid_send_report(func, report, sizeof(report));
            sleep(1);
        }
    }
    return 0;
}
