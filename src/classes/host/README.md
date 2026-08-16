# Host class drivers

Host-side class drivers go here - built on the public `usbip_host.h` API and the
`usbip_host_driver` registry (`usbip_host_register_driver`), mirroring how device classes in
`../device/` are built on `usbip_device.h` + `usbip_device_class`.

None are implemented in C yet (the host API currently drives devices directly via
`usbip_host_control_transfer` / `usbip_host_bulk_transfer`). The Python library already has host
class drivers under its `usbip/classes/host/` (HID, CDC-ACM, MSC) to mirror.
