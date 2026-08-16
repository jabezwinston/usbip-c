# USBIP - C library

Create **virtual USB devices** and write **USB host drivers** for them, in plain C.

[USB/IP](https://docs.kernel.org/usb/usbip_protocol.html) is a Linux-kernel protocol
that *exports* a computer's real USB devices, so another machine can use them over
the network. It has been in the mainline kernel for years.

But the protocol has a second, unintended power: the wire traffic is just USB
requests over TCP, so an ordinary program can answer them and *become* a USB
device - no hardware, no kernel code. A standard library for this side never
existed; this library fills that gap:

- **Device API** (`usbip-device.h`) - your program *is* a USB device: keyboard,
  serial port, disk, webcam, sound card, or anything you define yourself.
- **Host API** (`usbip-host.h`) - your program *talks to* such a device, shaped
  like libusb, with no kernel driver and no root.

See [API reference and guide](https://jabezwinston.github.io/usbip-c/) for more details.

## Use cases

- **Test automation** - drive a host app against a scripted device in CI, no hardware needed.
- **AI in the loop** - an agent can build, plug, probe and fix a device or host app in software.
- **Reverse engineering** - re-create a device from a capture until its driver accepts it.
- **Developing before the hardware exists** - test host software against a virtual model.
- **Emulating old or discontinued hardware** whose driver you still need to run.
- **Learning USB** - build devices and watch every transfer in Wireshark (`USBIP_PCAPNG=1`).

## Setup

Download a [release](https://github.com/jabezwinston/usbip-c/releases) for Windows,
32- and 64-bit in one archive: the public headers, the static and shared
`libusbip-device` / `libusbip-host` libraries, the class layer as source, the
libusb-1.0 / libusbK drop-in DLLs, and every device example as a ready-to-run
`.exe`. Everywhere else - and for Windows too - build from source (GNU make and a
C11 compiler; `mingw32-make` natively on Windows):

```bash
git clone https://github.com/jabezwinston/usbip-c && cd usbip-c && make
```

## Creating a virtual device

A complete device - one vendor interface, two bulk endpoints, echoing back whatever
the host sends. WinUSB is advertised so Windows binds a driver automatically:

```c
#include "usbip-device.h"

int main(void)
{
    usbip_device *dev = usbip_device_create(0x1209, 0x0004);
    usbip_device_set_strings(dev, "USB over IP", "My First Device", "0004");

    usbip_device_add_descriptor(dev, &(usb_interface_descriptor){
        .bDescriptorType = USB_DT_INTERFACE,
        .bInterfaceClass = 0xFF   /* vendor-specific */
    });
    usbip_ep *bulk_out = usbip_device_add_endpoint(dev, &(usb_endpoint_descriptor){
        .bDescriptorType = USB_DT_ENDPOINT,
        .bEndpointAddress = 0x01,   /* host -> device */
        .bmAttributes    = USB_BULK,
        .wMaxPacketSize   = 64
    });
    usbip_ep *bulk_in = usbip_device_add_endpoint(dev, &(usb_endpoint_descriptor){
        .bDescriptorType = USB_DT_ENDPOINT,
        .bEndpointAddress = 0x81,   /* device -> host */
        .bmAttributes    = USB_BULK,
        .wMaxPacketSize   = 64
    });
    usbip_device_enable_winusb(dev, NULL);   /* Skip driver install on Windows */

    usb_transport *transport = usbip_transport(NULL, 3240);
    usbip_device_plug(dev, transport);

    for (;;) {   /* echo everything back */
        uint8_t buffer[512];
        int n = usbip_device_read(bulk_out, buffer, sizeof buffer, 0);
        if (n > 0)
            usbip_device_write(bulk_in, buffer, n, 0);
    }
}
```

Compile and link against the static library, from a source build or from an
extracted release (shared libraries, `pkg-config` and `sudo make install` exist too):

```bash
gcc my_device.c -Iinclude src/build/libusbip-device.a -pthread -o my_device   # source build
gcc my_device.c -Iinclude lib/x64/libusbip-device.a -lws2_32 -pthread -static -o my_device.exe
```

## Attaching it

`./my_device` now serves the device on TCP port 3240 and waits - your program is the
USB/IP *server*; the OS is the *client* that imports it. Only this step differs per OS.

On Linux, the client is already in the kernel:

```bash
sudo modprobe vhci-hcd                 # once per boot
sudo usbip attach -r 127.0.0.1 -b 1-1
lsusb                 # Check if your device is listed
```

(Detach with `sudo usbip detach -p 00`.)

On Windows, install [usbip-win2](https://github.com/vadimgrn/usbip-win2) and attach
with its GUI:

![usbip-win2 GUI attaching the device](doc/img/usbip-win2-gui.png)

or from a terminal: `usbip.exe attach -r <ip> -b 1-1`. Because the device above
advertises WinUSB, no driver hunt follows - it is immediately usable from libusb apps.

On an OS without a USB/IP client (macOS has none in-box), use
[USBIP for microcontrollers](https://github.com/jabezwinston/usbip-for-uc): a small
board that attaches over the network and re-presents the device on a real USB port,
which any machine sees as plain USB.

## A COM port in a few lines

Device classes are built in, so common devices take almost no code. A serial port that
echoes what you type:

```c
#include <unistd.h>
#include "classes/cdc_acm.h"

static void on_rx(cdc_port *port, void *user, const void *data, int len)
{
    cdc_acm_send(port, data, len);   /* echo back to the host */
}

int main(void)
{
    usbip_device *dev = usbip_device_create(0x1209, 0x0001);
    cdc_acm_add(dev, &(cdc_acm_opts){ .on_rx = on_rx });

    usb_transport *transport = usbip_transport(NULL, 3240);
    usbip_device_plug(dev, transport);

    for (;;)
        pause();   /* the class handles everything */
}
```

The class layer ships as source in the repository - compile it in:

```bash
gcc my_port.c src/classes/usb_class.c src/classes/device/cdc_acm.c -Iinclude \
    src/build/libusbip-device.a -pthread -o my_port
```

Attach it and a serial port appears - `/dev/ttyACM0` on Linux, a `COMx` port on
Windows; open it with any terminal program. The other built-in classes - **HID**
(keyboard/mouse/raw), **MSC** (disk), **UVC** (webcam), **UAC** (sound card), **DFU**
(firmware upgrade), **MTP** (file transfer) and **Bluetooth** (HCI dongle) - work the
same way; [`examples/`](examples/) has a runnable program for each except Bluetooth,
and the [guide](https://jabezwinston.github.io/usbip-c/) shows what each becomes per OS.

## Writing a host driver

The host API is shaped like libusb, so the learning curve is minimal. It imports a
device and drives it from your own process - no kernel client, no root, any OS:

```c
#include "usbip-host.h"

int main(void)
{
    usbip_host_context *ctx;
    usbip_host_init(&ctx);   /* local server; set_transport() for remote */

    usbip_host_handle *handle = usbip_host_open_vid_pid(ctx, 0x1209, 0x0004);
    usbip_host_set_configuration(handle, 1);
    usbip_host_claim_interface(handle, 0);
    int sent;
    usbip_host_bulk_transfer(handle, 0x01, (uint8_t *)"hello", 5, &sent, 1000);
    return 0;
}
```

Link with `libusbip-host.a` instead (one role per binary - the libraries share internals).

## Drop-in libraries: run stock libusb / libusbK apps unmodified

[`wrapper/libusb-1.0/`](wrapper/libusb-1.0/README.md) builds an ABI-compatible
`libusb-1.0.so.0` / `libusb-1.0.dll`, and [`wrapper/libusbK/`](wrapper/libusbK/README.md)
a `libusbK.dll`, both implemented on USB/IP. Put one in front of an unmodified program -
not rewritten, not even rebuilt - and it talks to your virtual device directly, with no
kernel driver at all. `USBIP_HOST` / `USBIP_PORT` pick the server (default `127.0.0.1:3240`):

```bash
# Linux: preload it
USBIP_HOST=127.0.0.1 LD_PRELOAD=$PWD/wrapper/build/libusb-1.0.so.0 dfu-util -l
```

```bat
:: Windows: copy the DLL next to the .exe - it is found first
copy wrapper\build\libusb-1.0.dll C:\dfu-util\
set USBIP_HOST=127.0.0.1
C:\dfu-util\dfu-util.exe -l
```
