/**
 * Copyright (C) 2026 Jabez Winston
 *
 * SPDX-License-Identifier: MIT
 *
 * Date : 29-July-2026
 *
 * cdc_hid_device.c - a composite CDC-ACM serial port + HID consumer control, built
 * on the ready-made CLASSES: cdc_acm_add() and hid_add() author the descriptors,
 * emit the Interface Association Descriptor that groups each function, and answer
 * the class requests, so this file is behaviour only. Compare cdc_acm_device.c and
 * hid_device.c, which are the same two functions on devices of their own.
 *
 * Behaviour: the serial port is a console that drives the media keys. Type a key
 * name on it and the HID function taps that consumer key on the host, so one
 * action is visible through both functions:
 *
 *   ./cdc_hid_device                 # serve 1209:0013 on :3240
 *   attach it with a USB/IP client
 *   (Linux: sudo modprobe vhci-hcd ; sudo usbip attach -r 127.0.0.1 -b 1-1)
 *   open the serial port it creates and type, e.g.:
 *     v+        raise the volume one step
 *     v- v- v-  lower it three steps (a line may carry several keys)
 *     m         mute
 *     b+ / b-   screen brightness
 *     help      list every key
 */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "classes/cdc_acm.h"
#include "classes/hid.h"

/* identity */
#define VENDOR_ID    0x1209
#define PRODUCT_ID   0x0013

#define LINE_MAX_LEN 64

/* A Consumer Control (Usage Page 0x0C) with one momentary bit per key: a 2-byte
 * Input report whose low 10 bits are the keys below, in this order, padded out to
 * the byte boundary. A bitmap rather than the keyboard's array of key codes, so a
 * report says "this key is down" directly; sending an all-zero report releases it.
 * Every Usage here is a single byte, which is what keeps the item macros readable -
 * usages above 0xFF (AC Home, AL Calculator...) need a 2-byte Usage item. */
static const uint8_t RD_CONSUMER[] = {
    HID_USAGE_PAGE(0x0C), HID_USAGE(0x01),           /* Consumer, Consumer Control */
    HID_COLLECTION(0x01),                            /*   Application */
      HID_LOGICAL_MIN(0), HID_LOGICAL_MAX(1),
      HID_REPORT_SIZE(1), HID_REPORT_COUNT(10),
      HID_USAGE(0xE9), HID_USAGE(0xEA), HID_USAGE(0xE2),  /* Volume +/-, Mute      */
      HID_USAGE(0x6F), HID_USAGE(0x70),                   /* Brightness +/-        */
      HID_USAGE(0xCD), HID_USAGE(0xB7),                   /* Play/Pause, Stop      */
      HID_USAGE(0xB5), HID_USAGE(0xB6),                   /* Next, Previous track  */
      HID_USAGE(0xB8),                                    /* Eject                 */
      HID_INPUT(0x02),                                    /* 10 momentary bits     */
      HID_REPORT_COUNT(6), HID_INPUT(0x03),               /* 6 bits padding        */
    HID_END_COLLECTION,
};

/* The console vocabulary: one token per bit of RD_CONSUMER, same order. */
static const struct consumer_key {
    const char *token;
    uint16_t    bit;
    const char *what;
} KEYS[] = {
    {"v+",    1u << 0, "volume up"},
    {"v-",    1u << 1, "volume down"},
    {"m",     1u << 2, "mute"},
    {"b+",    1u << 3, "brightness up"},
    {"b-",    1u << 4, "brightness down"},
    {"play",  1u << 5, "play/pause"},
    {"stop",  1u << 6, "stop"},
    {"next",  1u << 7, "next track"},
    {"prev",  1u << 8, "previous track"},
    {"eject", 1u << 9, "eject"},
};

struct app {
    cdc_port  *console;
    hid_iface *consumer;
    char       line[LINE_MAX_LEN];
    int        line_len;
    int        overflow;
};

/* ---- console output ------------------------------------------------------ */
static void cdc_acm_printf(struct app *app, const char *fmt, ...)
#if defined(__GNUC__)
    __attribute__((format(printf, 2, 3)))
#endif
    ;

static void cdc_acm_printf(struct app *app, const char *fmt, ...)
{
    if (!app->console)
        return;
    char b[128];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(b, sizeof(b) - 2, fmt, ap);
    va_end(ap);
    if (n < 0)
        return;
    if (n > (int)sizeof(b) - 3)
        n = (int)sizeof(b) - 3;
    b[n++] = '\r'; /* terminals opened raw want both */
    b[n++] = '\n';
    cdc_acm_send(app->console, b, n);
}

/* ---- the HID function: tap one consumer key ------------------------------ */
/* Press and release, because a consumer key is momentary: the host acts on the
 * 0 -> 1 edge and repeats while the bit stays set. */
static void tap(struct app *app, uint16_t bits)
{
    uint8_t press[2] = {(uint8_t)(bits & 0xFF), (uint8_t)(bits >> 8)};
    uint8_t release[2] = {0, 0};

    hid_send_report(app->consumer, press, sizeof(press));
    usleep(20000);
    hid_send_report(app->consumer, release, sizeof(release));
    usleep(20000);
}

/* ---- console commands ---------------------------------------------------- */
static void cmd_help(struct app *app)
{
    cdc_acm_printf(app, "consumer keys -- several per line, e.g. 'v+ v+ m':");
    for (size_t i = 0; i < sizeof(KEYS) / sizeof(KEYS[0]); i++)
        cdc_acm_printf(app, "  %-6s %s", KEYS[i].token, KEYS[i].what);
}

/* One word: send its key. The point of the example -- typing on one function is
 * felt through the other, sharing only `user`. */
static void handle_word(struct app *app, const char *word)
{
    if (strcmp(word, "help") == 0 || strcmp(word, "?") == 0)
    {
        cmd_help(app);
        return;
    }
    for (size_t i = 0; i < sizeof(KEYS) / sizeof(KEYS[0]); i++)
    {
        if (strcmp(word, KEYS[i].token) == 0)
        {
            fprintf(stderr, "[cdc_hid] %s -> %s\n", word, KEYS[i].what);
            cdc_acm_printf(app, "%s", KEYS[i].what);
            tap(app, KEYS[i].bit);
            return;
        }
    }
    cdc_acm_printf(app, "unknown key '%s' -- try 'help'", word);
}

/* A line is a list of key names: split it in place and send each in turn, so
 * 'v+ v+ m' is three keys. */
static void handle_line(struct app *app, char *line)
{
    char *p = line;
    int any = 0;

    for (;;)
    {
        while (*p == ' ' || *p == '\t')
            p++;
        if (!*p)
            break;
        char *word = p;
        while (*p && *p != ' ' && *p != '\t')
            p++;
        if (*p)
            *p++ = 0;
        handle_word(app, word);
        any = 1;
    }
    if (!any) /* bare Enter: the greeting, on demand */
        cdc_acm_printf(app, "USBIP composite CDC+HID consumer control -- type 'help'");
}

/* ---- CDC callbacks: the class answers line coding and DTR itself -------- */
static void on_open(cdc_port *port, void *user, const cdc_line_coding *coding)
{
    struct app *app = user;
    (void)port;
    fprintf(stderr, "[cdc_hid] port OPENED @ %u baud, %u data bits\n", coding->baud, coding->data_bits);
    app->line_len = 0;
    app->overflow = 0;
}

static void on_close(cdc_port *port, void *user)
{
    (void)port;
    (void)user;
    fprintf(stderr, "[cdc_hid] port closed\n");
}

/* Serial input: echo it so typing is visible, and act on each completed line. The
 * class delivers this on the serve thread. */
static void on_rx(cdc_port *port, void *user, const void *data, int len)
{
    struct app *app = user;
    const char *d = data;

    for (int i = 0; i < len; i++)
    {
        char c = d[i];
        cdc_acm_send(port, &c, 1); /* echo, so typing is visible */
        if (c == '\r' || c == '\n')
        {
            cdc_acm_send(port, "\n", 1);
            if (app->overflow)
            {
                cdc_acm_printf(app, "line too long -- ignored");
            }
            else
            {
                app->line[app->line_len] = 0;
                handle_line(app, app->line);
            }
            app->line_len = 0;
            app->overflow = 0;
        }
        else if (app->line_len < LINE_MAX_LEN - 1)
        {
            app->line[app->line_len++] = c;
        }
        else
        {
            app->overflow = 1; /* drop the whole line, not just its tail */
        }
    }
}

int main(int argc, char **argv)
{
    int port = (argc > 1) ? atoi(argv[1]) : 3240;                         /* default USB/IP port */

    struct app app;
    memset(&app, 0, sizeof(app));

    usbip_device *dev = usbip_device_create(VENDOR_ID, PRODUCT_ID);
    usbip_device_set_strings(dev, "USB over IP", "USBIP CDC+HID", "0013");
    //! [composite]
    usbip_device_set_composite(dev); /* EF/02/01: usbccgp splits per IAD */

    /* --- function 1: CDC-ACM (interfaces 0+1), grouped by the class's IAD --- */
    cdc_acm_opts copts = {
        .on_rx = on_rx,
        .on_open = on_open,
        .on_close = on_close,
        .user = &app
    };
    app.console = cdc_acm_add(dev, &copts);
    //! [composite]

    //! [second_function]
    /* --- function 2: HID consumer control (interface 2) --------------------
     * No subclass or protocol: only a keyboard or a mouse can be a boot device. */
    hid_opts hopts = {
        .report_desc     = RD_CONSUMER,
        .report_desc_len = sizeof(RD_CONSUMER),
        .in_mps          = 2,
        .user            = &app
    };
    app.consumer = hid_add(dev, &hopts);
    //! [second_function]

    if (!app.console || !app.consumer)
    {
        fprintf(stderr, "failed to add the CDC port or the HID consumer control\n");
        return 1;
    }

    usb_transport *transport = usbip_transport(NULL, port);
    int rc = usbip_device_plug(dev, transport);

    if (rc != USB_SUCCESS)
    {
        fprintf(stderr, "usbip_device_plug failed (port %d in use?)\n", port);
        return 1;
    }
    fprintf(stderr, "[cdc_hid] serving %04x:%04x on :%d - CDC on interfaces %d+%d, consumer control on interface %d\n",
            VENDOR_ID, PRODUCT_ID, port, app.console->interface_number, app.console->interface_number + 1,
            app.consumer->interface_number);
    fprintf(stderr, "[cdc_hid] type 'help' on the serial port for the key names\n");

    /* Both functions run from their callbacks; nothing left for this thread. */
#ifdef _WIN32
    for (;;)
        sleep(1); /* no pause() on Windows; idle */
#else
    for (;;)
        pause();
#endif
    return 0;
}
