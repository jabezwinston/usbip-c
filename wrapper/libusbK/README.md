# libusbK

An **ABI- and symbol-compatible `libusbK.dll`** built on the USBIP C library's low-level USB/IP
client primitives. A Windows program written against Travis Robinson's `<libusbk.h>`
can load this DLL instead of the real libusbK and transparently drive a **USBIP
virtual USB device over USB/IP** - no `libusbK.sys` kernel driver, no device install,
no `vhci`.

```
include/libusbk.h   clean-room, ABI-compatible public header (only what we implement)
libusbk.c           the implementation, on usbip_connect / usbip_client_* (NOT usbip_host_*)
libusbK.def         export list: undecorated __stdcall names (UsbK_Init, ...)
```

This is the libusbK sibling of the `../libusb-1.0` wrapper: same backend, same
`USBIP_HOST`/`USBIP_PORT` model, the *other* major Windows USB API.

## Build

libusbK is a Windows-only API (`HANDLE` / `OVERLAPPED` / `__stdcall`), so the DLL is
built only for Windows - natively on Windows, or cross-built from Linux or macOS.

**Windows**, from `cmd`, with a mingw-w64 `gcc` on `PATH` (`mingw32-make`, not MSYS2's
`make`):

<div class="lang-sh"></div>

```bat
mingw32-make libusbk
```

**Linux / macOS**, cross-building with mingw-w64:

<div class="lang-sh"></div>

```bash
make OS=Windows_NT libusbk      # -> wrapper/build/libusbK.dll
```

The DLL is linked `-static`, so it carries no mingw runtime dependency and runs under
wine and on a bare Windows box. Exports are **undecorated** (`UsbK_Init`, not
`_UsbK_Init@8`), so `GetProcAddress` and `LibK_LoadDriverAPI` resolve exactly as
against the real `libusbK.dll`.

## Use

Copy `libusbK.dll` into the program's own directory, which Windows searches first - an
already-compiled libusbK program then loads this one with nothing else to configure.

The library learns where the USB/IP server is from the environment (read on each
`LstK_Init` / `UsbK_Init`):

| variable         | default       | meaning                          |
|------------------|---------------|----------------------------------|
| `USBIP_HOST`  | `127.0.0.1`   | USB/IP server host               |
| `USBIP_PORT`  | `3240`        | USB/IP server TCP port           |

<div class="lang-sh"></div>

```bat
copy wrapper\build\libusbK.dll C:\path\to\your-program\

set USBIP_HOST=127.0.0.1
set USBIP_PORT=4000
C:\path\to\your-program\your-program.exe
```

One `set` per line: a space before `&&` would land in the value. PowerShell spells it
`$env:USBIP_PORT = "4000"`.

Both libusbK entry styles work:

```c
/* (1) direct exports */
KLST_HANDLE list;  LstK_Init(&list, KLST_FLAG_NONE);
KLST_DEVINFO_HANDLE dev;  LstK_FindByVidPid(list, 0x1209, 0x0001, &dev);
KUSB_HANDLE h;  UsbK_Init(&h, dev);
UsbK_WritePipe(h, 0x01, buf, len, &sent, NULL);

/* (2) the driver-API function table */
KUSB_DRIVER_API api;  LibK_LoadDriverAPI(&api, KUSB_DRVID_LIBUSBK);
api.Init(&h, dev);
api.ReadPipe(h, 0x81, buf, sizeof buf, &got, NULL);
```

`ReadPipe` / `WritePipe` / `ControlTransfer` / `IsoReadPipe` are **synchronous** when
`Overlapped` is `NULL`, and **asynchronous** when given an `LPOVERLAPPED` (the call
returns `FALSE` + `ERROR_IO_PENDING` and completes on a worker thread). The `OvlK_*`
overlapped pool and `UsbK_GetOverlappedResult` work as usual:

```c
KOVL_POOL_HANDLE pool;  OvlK_Init(&pool, h, 4, KOVL_POOL_FLAG_NONE);
KOVL_HANDLE ovl;        OvlK_Acquire(&ovl, pool);
UsbK_ReadPipe(h, 0x81, buf, sizeof buf, NULL, (LPOVERLAPPED)ovl);   /* -> ERROR_IO_PENDING */
UINT got;  OvlK_Wait(ovl, 1000, KOVL_WAIT_FLAG_RELEASE_ON_SUCCESS, &got);
```

## Fidelity

The full public surface is implemented - every symbol the real `libusbK.dll` exports.
What USB/IP cannot honestly provide is stubbed rather than faked, and listed.

Two things worth knowing:

- `WinUsb_*` is declared in `winusb.h`, not `libusbk.h` - calling one without a
  prototype yields a cdecl call to a stdcall export and corrupts the stack.
- `LstK` device-info handles are stable : an info handle keeps its address across
  attach/detach, so handles the caller is holding stay valid.

Below are API limitations,

| Call | What it does here | Why |
|------|-------------------|-----|
| `HotK_Init` | fires `OnHotPlug` once per currently-present match, then never again | that is an initial enumeration; USB/IP has no asynchronous arrival/removal |
| `UsbK_Set/GetPipePolicy`, `UsbK_Set/GetPowerPolicy` | accepted and stored, and read back unchanged | there is nothing below to enforce a timeout or manage power - the underlying submit blocks |
| `UsbK_ResetDevice` | succeeds, doing nothing | there is no device-reset path over an import |
| `UsbK_AbortPipe` / `UsbK_FlushPipe` | succeed, doing nothing | a submit already running cannot be interrupted, and there is no driver-side queue to flush |
| `UsbK_ResetPipe` | issues a real `CLEAR_FEATURE(ENDPOINT_HALT)` | it is a device operation, not a driver one, so it is done for real |
| `UsbK_GetSuperSpeedPipeCompanionDescriptor` | reports "not present" | these are FS/HS devices |
| `UsbK_Initialize` | `ERROR_NOT_SUPPORTED` | it starts from a Windows device file handle, and there is no device node over USB/IP |
| `UsbK_GetProperty(KUSB_PROPERTY_DEVICE_FILE_HANDLE)` | returns the USB/IP socket | the nearest true answer |
| transfer / pipe-policy `timeout` | accepted, never enforced | the primitives block |
| in-flight cancellation | not available | the model is synchronous underneath, one worker thread per handle |
| the `DriverID` of `LibK_LoadDriverAPI` / `LibK_CopyDriverAPI` | ignored; `KUSB_DRVID_LIBUSBK` is advertised | there is a single backend |
