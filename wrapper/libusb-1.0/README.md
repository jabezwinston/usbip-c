# libusb-1.0

An **ABI- and symbol-compatible** `libusb-1.0.so.0` built on the USBIP C library's low-level
USB/IP client primitives. Any program written against the real `<libusb.h>` can load
this library instead of the system libusb and transparently drive a **USBIP virtual
USB device over USB/IP** - no kernel module, no root, no `vhci`.

## Build

Both artifacts land in `wrapper/build/`.

**Linux / macOS**

<div class="lang-sh"></div>

```bash
make libusb                 # -> wrapper/build/libusb-1.0.so.0 (+ a libusb-1.0.so symlink)
make OS=Windows_NT libusb   # cross-build for Windows -> wrapper/build/libusb-1.0.dll
```

**Windows** - from `cmd`, with a mingw-w64 `gcc` on `PATH` (`mingw32-make`, not MSYS2's
`make`):

<div class="lang-sh"></div>

```bat
mingw32-make libusb
```

## Use

The library never sees a transport call from the program, so it learns where the USB/IP
server is from the environment (read once in `libusb_init`):

| variable         | default       | meaning                          |
|------------------|---------------|----------------------------------|
| `USBIP_HOST`  | `127.0.0.1`   | USB/IP server host               |
| `USBIP_PORT`  | `3240`        | USB/IP server TCP port           |

Start a USBIP virtual DFU device on port 4000 in one terminal
(`examples/build/dfu_device --port 4000`), then point a stock, already-compiled libusb
program at it in another.

**Linux** - `LD_PRELOAD` puts this library ahead of the system one:

<div class="lang-sh"></div>

```bash
USBIP_HOST=127.0.0.1 USBIP_PORT=4000 LD_PRELOAD=/path/to/libusb-1.0.so.0 dfu-util -l
USBIP_HOST=127.0.0.1 USBIP_PORT=4000 LD_PRELOAD=/path/to/libusb-1.0.so.0 lsusb
```

**Windows** - there is no `LD_PRELOAD`. Copy the DLL into the program's own directory,
which Windows searches first, and set the variables one `set` per line (a space before
`&&` would land in the value). `lsusb` has no Windows build.

<div class="lang-sh"></div>

```bat
copy wrapper\build\libusb-1.0.dll C:\dfu-util\

set USBIP_HOST=127.0.0.1
set USBIP_PORT=4000
C:\dfu-util\dfu-util.exe -l
```

To build your own program against it, compile against this header and link the library:

<div class="lang-sh"></div>

```bash
# Linux / macOS
cc myapp.c -Iwrapper/libusb-1.0/include /path/to/libusb-1.0.so.0 -Wl,-rpath,/path/to
```

<div class="lang-sh"></div>

On Windows (mingw-w64) link the import library and ship the DLL beside the `.exe`:

```bat
gcc myapp.c -Iwrapper\libusb-1.0\include wrapper\build\libusb-1.0.dll.a -o myapp.exe
```

pyusb works too, on any OS, and is the one route that needs neither preloading nor
copying - point its libusb1 backend straight at the file:

```python
import usb.core, usb.backend.libusb1
be = usb.backend.libusb1.get_backend(find_library=lambda _: "/path/to/libusb-1.0.so.0")
dev = usb.core.find(idVendor=0x1209, idProduct=0x0001, backend=be)
```

## Fidelity

The synchronous *and* asynchronous libusb APIs are implemented. What USB/IP cannot
honestly provide is stubbed rather than faked, and listed.

| Call | What it does here | Why |
|------|-------------------|-----|
| `libusb_has_capability(LIBUSB_CAP_HAS_HOTPLUG)` | returns 0 | USB/IP has no arrival/removal events, so well-written programs fall back to manual enumeration |
| `libusb_hotplug_register_callback` | `LIBUSB_ERROR_NOT_SUPPORTED` | as above |
| `libusb_kernel_driver_active` | 0 | USB/IP imports the whole device; no kernel driver is bound on this side |
| `libusb_detach_kernel_driver` / `libusb_attach_kernel_driver` / `libusb_set_auto_detach_kernel_driver` | succeed, doing nothing | as above |
| `libusb_reset_device` | succeeds, doing nothing | there is no device-reset path over an import; `libusb_clear_halt` *is* real, and is what a stuck endpoint needs |
| `libusb_get_parent` | returns `NULL` | there is no hub topology over an import. The symbol is exported because pyusb resolves it at load time |
| transfer `timeout` | accepted, never enforced | the underlying primitives block |
| `libusb_cancel_transfer` | exact while still queued (completes `CANCELLED`), best-effort once in flight | a synchronous call already running cannot be interrupted |
| `libusb_get_device_descriptor` | string-index fields read 0 | it is synthesized from the USB/IP enumeration record. The string descriptors themselves work once the device is open, and `bus` / `address` / `speed` are the real advertised values |
