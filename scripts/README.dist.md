# USBIP C library - Windows binary package

Emulate USB devices (`libusbip-device`) or drive them from the host side
(`libusbip-host`) over the USB/IP protocol.

USB/IP inverts the usual words, so get this straight first: the side that
**provides** the device (`libusbip-device`) is the USB/IP **server** - it
*listens*, on TCP 3240 by default - and the side that **uses** it
(`libusbip-host`, or an OS importer such as `usbip attach`) is the USB/IP
**client**, which *connects*. `usbip_device_plug()` therefore starts serving and
returns; nothing enumerates until a client imports the device.

## Package contents

```
README.md
include/                    one -Iinclude covers all of this
  usbip.h                     shared by both roles
  usbip-device.h              the device (server) API
  usbip-host.h                the host (client) API
  classes/                    the class layer: usb_class.h + one header per class
  libusb-1.0/libusb.h         the drop-in APIs, to compile against without
  libusbK/libusbk.h           the upstream projects installed
src/                        source, no build system - see "Build from source"
  usbip_device.c usbip.c pcap.c    the device core, plus its private headers
  classes/usb_class.c              the class layer
  classes/device/*.c               cdc_acm, msc, mtp, hid, dfu, uvc, uac, ...
lib/x86/ , lib/x64/         libusbip-device.*, libusbip-host.* - static + shared
dropin/x86/ , dropin/x64/   libusb-1.0.dll, libusbK.dll - see "Drop-in DLLs"
bin/x86/ , bin/x64/         the device examples, one .exe each
```

Only `lib/`, `dropin/` and `bin/` depend on the architecture; each holds the same
file names under `x86` and `x64`, so the only choice is which to point at:

| directory | built with | for |
|---|---|---|
| `x86` | `i686-w64-mingw32-gcc` | 32-bit Windows programs |
| `x64` | `x86_64-w64-mingw32-gcc` | 64-bit Windows programs |

Everything here is statically linked against the mingw-w64 runtime, so no
compiler DLLs come with it: the only imports are `kernel32`, `msvcrt` and
`ws2_32`.

## Run an example

`bin/` holds the device-side examples, ready to run: each emulates a USB device
and waits for a client to import it. Every one takes `--help`.

```bat
x64\bin\cdc_acm_device.exe --verbose
```

That serves a virtual serial port on TCP 3240 and prints what the client does to
it. To use it, import it with a USB/IP client - on Windows, install
[usbip-win2](https://github.com/vadimgrn/usbip-win2) and attach from its GUI or
with `usbip.exe attach -r <ip> -b 1-1`; on Linux, `sudo usbip attach -r <ip> -b 1-1`.
The server and the client can be the same machine.

## Build against the library

```bat
gcc myusb_device.c -Iinclude -Llib\x64 -lusbip-device -lws2_32
gcc myusb_host.c   -Iinclude -Llib\x64 -lusbip-host   -lws2_32
```

`-lws2_32` is for winsock. Linking the **static** archive also wants `-pthread`
(which pulls in winpthreads); add `-static` to ship a single `.exe`. The archives
are built position-independent, so they can go into a shared library of your own
as well as into an executable.

To use the **shared** library instead, link its import library (`.dll.a`, same
`-l` spelling) and put the `.dll` beside your `.exe` - Windows searches that
directory first, so there is no `LD_LIBRARY_PATH` equivalent to set.

Link ONE role per binary: the two libraries share internal code, so a single
program linking both gets duplicate symbols.

## Build from source

`src/` is here because the **class layer has no binary form**: `usb_class.c` and
the classes beside it are compiled into each program that wants them, not into
`libusbip-device`. Add the ones you use to your own build - the prebuilt library
supplies everything under them:

```bat
gcc myusb_device.c src\classes\usb_class.c src\classes\device\hid.c ^
    -Iinclude -Llib\x64 -lusbip-device -lws2_32
```

The device core (`usbip_device.c`, `usbip.c`, `pcap.c`) is the source of
`libusbip-device` itself, for reading, stepping through, or building your own
copy. `-Iinclude` is the only flag any of it needs; the private headers beside
those files are included by name from their own directory.

No Makefiles ship here, and neither the host role nor the drop-in wrappers do.
For those, and for Linux or macOS, build the full tree from the repository.

## Drop-in DLLs

`dropin/` holds ABI-compatible replacements for two USB libraries, built on the
same client primitives as `libusbip-host`:

| file | replaces | so that |
|---|---|---|
| `libusb-1.0.dll` | libusb-1.0 | a stock libusb program (`dfu-util`, pyusb, ...) talks to a USB/IP device |
| `libusbK.dll` | libusbK | a stock libusbK program does the same |

Copy the one you want next to the program's `.exe`, replacing the real library.
No kernel driver and no attach step are involved - the program connects to the
USB/IP server directly, chosen with `USBIP_HOST` / `USBIP_PORT` below.

They keep a directory of their own here on purpose: they are named after the
libraries they replace, so putting `dropin/x64` on `PATH` would hijack every
libusb or libusbK program on the machine.

## Environment variables

These are all of them. None is required; none changes USB behaviour.

| Variable             | Read by                             | Default     | Effect |
|----------------------|-------------------------------------|-------------|--------|
| `USBIP_PCAPNG`       | both libraries                      | off         | write every transfer to a PCAPNG capture |
| `USBIP_DEBUG`        | `libusbip-device`                   | off         | log each control request + ok/STALL, and each transfer, to stderr |
| `USBIP_HOST`         | the libusb / libusbK drop-ins only  | `127.0.0.1` | USB/IP server to connect to |
| `USBIP_PORT`         | the libusb / libusbK drop-ins only  | `3240`      | its TCP port |

`USBIP_HOST` / `USBIP_PORT` are **not** read by `libusbip-host`: a program using
the host API chooses its server in code, with `usbip_host_set_transport()`.

The `VAR=value command` prefix is a POSIX-shell feature neither `cmd` nor PowerShell
has - set the variable first, as its own statement:

| Windows `cmd` | PowerShell | Linux / macOS (`sh`) |
|---|---|---|
| `set USBIP_DEBUG=1` | `$env:USBIP_DEBUG = "1"` | `USBIP_DEBUG=1 my_device` |

One `set` per line: a space before `&&` would land in the value.

### Capture USB traffic

Set `USBIP_PCAPNG=<file.pcapng>` before starting your program, then open the
capture in Wireshark - no code, no rebuild. `USBIP_PCAPNG=1` (or `on`/`yes`/
`true`/`auto`) auto-names it after the executable, in the current directory;
`%p` anywhere in the value expands to the process id. It works on both sides:
on the server it captures what the device sees, on the client what the importer
sent.

Full documentation: https://github.com/jabezwinston/usbip-c
