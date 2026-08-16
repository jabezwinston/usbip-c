/**
 * Copyright (C) 2026 Jabez Winston
 *
 * SPDX-License-Identifier: MIT
 *
 * Date : 04-June-2026
 *
 * cdc_acm_device.c - a virtual CDC-ACM serial port served over USB/IP.
 *
 * The CDC class is generic; THIS app decides what the port does, via callbacks:
 * it logs open/close/baud and echoes received bytes back to the host.
 *
 *   ./cdc_acm_device
 *   attach it with a USB/IP client
 *   (Linux: sudo modprobe vhci-hcd ; sudo usbip attach -r 127.0.0.1 -b 1-1)
 */
#include <stdio.h>
#include <unistd.h>

#include "_support/cmdline.h"
#include "classes/cdc_acm.h"

static void on_open(cdc_port *port, void *user, const cdc_line_coding *line_coding)
{
    (void)port;
    (void)user;
    fprintf(stderr, "[app] port OPENED @ %u baud, %u data bits\n", line_coding->baud, line_coding->data_bits);
}

static void on_close(cdc_port *port, void *user)
{
    (void)port;
    (void)user;
    fprintf(stderr, "[app] port CLOSED\n");
}

static void on_line_coding(cdc_port *port, void *user, const cdc_line_coding *line_coding)
{
    (void)port;
    (void)user;
    fprintf(stderr, "[app] line coding -> %u baud, %u data bits, parity %u, stop %u\n", line_coding->baud, line_coding->data_bits, line_coding->parity, line_coding->stop_bits);
}

static void on_rx(cdc_port *port, void *user, const void *data, int len)
{
    (void)user;
    fprintf(stderr, "[app] RX %d bytes: %.*s\n", len, len, (const char *)data);
    cdc_acm_send(port, data, len); /* echo back to the host */
    fprintf(stderr, "[app] TX %d bytes (echo)\n", len);
}

int main(int argc, char **argv)
{
    cmdline_opts cli_opts = cmdline_parse(argc, argv, &(cmdline_spec){
        .vid = 0x1209,
        .pid = 0x0001,
        .high_speed_opt = 1,
    });

    usbip_device *dev = usbip_device_create(cli_opts.vid, cli_opts.pid);
    usbip_device_set_strings(dev, "USB over IP", "USBIP CDC-ACM", "0001");
    usb_speed speed = cli_opts.high_speed ? USB_SPEED_HIGH : USB_SPEED_FULL;

    usbip_device_set_speed(dev, speed);

    //! [add]
    cdc_acm_opts ops = {
        .on_open  = on_open,
        .on_close = on_close,
        .on_line_coding = on_line_coding,
        .on_rx = on_rx,
    };
    cdc_port *port = cdc_acm_add(dev, &ops);        /* the host exposes a serial port */
    //! [add]

    if (!port)
    {
        fprintf(stderr, "cdc_acm_add failed\n");
        return 1;
    }

    usb_transport *transport = usbip_transport(cli_opts.host, cli_opts.port);
    int rc = usbip_device_plug(dev, transport);

    if (rc != USB_SUCCESS)
    {
        fprintf(stderr, "usbip_device_plug failed (port %d in use?)\n", cli_opts.port);
        return 1;
    }

    fprintf(stderr, "[app] serving CDC-ACM (%04x:%04x, %s) on %s:%d - "
                    "attach: sudo usbip attach -r 127.0.0.1 -b 1-1\n",
            cli_opts.vid, cli_opts.pid, 
            cli_opts.high_speed ? "high speed" : "full speed",
            cli_opts.host, cli_opts.port);

#ifdef _WIN32
    for (;;)
        sleep(1); /* no pause() on Windows; idle */
#else
    for (;;)
        pause();
#endif
    return 0;
}
