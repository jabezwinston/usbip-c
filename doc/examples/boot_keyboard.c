/**
 * boot_keyboard.c - the documentation's first example: a USB Boot-protocol HID
 * keyboard served over USB/IP, written on the DEVICE CORE ALONE (usbip-device.h).
 * Every descriptor is authored by hand and every class request answered in one
 * callback - no class layer, no classes/hid.h. It is the whole library surface a
 * simple device needs: create, describe, plug, write.
 *
 *   ./boot_keyboard                 # serve 1209:0011 on :3240, type "hello"
 *   ./boot_keyboard 4000 "world"    # ...on another port, typing another string
 *
 * Roles are inverted from USB's: in USB terms this program is the DEVICE, but in
 * USB/IP terms it is the SERVER - usbip_device_plug() listens on TCP 3240 and waits.
 * The USB host end is the USB/IP CLIENT, and it connects.
 *
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "usbip-device.h"

/* identity + wiring */
#define VENDOR_ID  0x1209 /* pid.codes, the free VID for open hardware */
#define PRODUCT_ID 0x0011
#define EP_HID_IN  0x81   /* device -> host: the 8-byte key reports */

/* HID 1.11 - the handful of constants this example needs */
#define HID_DT_HID       0x21 /* the HID descriptor, inside the configuration */
#define HID_DT_REPORT    0x22 /* the Report descriptor, fetched separately    */
#define HID_GET_REPORT   0x01
#define HID_GET_IDLE     0x02
#define HID_GET_PROTOCOL 0x03
#define HID_SET_REPORT   0x09
#define HID_SET_IDLE     0x0A
#define HID_SET_PROTOCOL 0x0B

#define HID_SUBCLASS_BOOT     0x01 /* bInterfaceSubClass: boot interface  */
#define HID_PROTOCOL_KEYBOARD 0x01 /* bInterfaceProtocol: keyboard        */
#define KEY_REPORT_LEN        8    /* modifiers, reserved, 6 key codes    */

/* The standard boot-keyboard Report descriptor: an 8-byte Input report
 * (modifiers, reserved, 6 key codes) plus a 1-byte LED Output report. Raw
 * bytes in wire order - nothing beyond the core is used here. */
static const uint8_t KEYBOARD_REPORT_DESC[] = {
    0x05, 0x01, /* Usage Page (Generic Desktop)       */
    0x09, 0x06, /* Usage (Keyboard)                   */
    0xA1, 0x01, /* Collection (Application)           */
    0x05, 0x07, /*   Usage Page (Keyboard/Keypad)     */
    0x19, 0xE0, /*   Usage Minimum (Left Control)     */
    0x29, 0xE7, /*   Usage Maximum (Right GUI)        */
    0x15, 0x00, /*   Logical Minimum (0)              */
    0x25, 0x01, /*   Logical Maximum (1)              */
    0x75, 0x01, /*   Report Size (1)                  */
    0x95, 0x08, /*   Report Count (8)                 */
    0x81, 0x02, /*   Input (Data,Var,Abs) - modifiers */
    0x95, 0x01, /*   Report Count (1)                 */
    0x75, 0x08, /*   Report Size (8)                  */
    0x81, 0x03, /*   Input (Const) - reserved byte    */
    0x95, 0x05, /*   Report Count (5)                 */
    0x75, 0x01, /*   Report Size (1)                  */
    0x05, 0x08, /*   Usage Page (LEDs)                */
    0x19, 0x01, /*   Usage Minimum (Num Lock)         */
    0x29, 0x05, /*   Usage Maximum (Kana)             */
    0x91, 0x02, /*   Output (Data,Var,Abs) - LEDs     */
    0x95, 0x01, /*   Report Count (1)                 */
    0x75, 0x03, /*   Report Size (3)                  */
    0x91, 0x03, /*   Output (Const) - padding         */
    0x95, 0x06, /*   Report Count (6)                 */
    0x75, 0x08, /*   Report Size (8)                  */
    0x15, 0x00, /*   Logical Minimum (0)              */
    0x25, 0x65, /*   Logical Maximum (101)            */
    0x05, 0x07, /*   Usage Page (Keyboard/Keypad)     */
    0x19, 0x00, /*   Usage Minimum (0)                */
    0x29, 0x65, /*   Usage Maximum (101)              */
    0x81, 0x00, /*   Input (Data,Array) - 6 key codes */
    0xC0,       /* End Collection                     */
};

/* The HID class descriptor (HID 1.11 6.2.1). Class-specific descriptor structs
 * are not in usbip.h - the core is class-agnostic - so the example declares its
 * own and appends it as a typed descriptor. */
typedef struct USB_PACKED
{
    uint8_t bLength, bDescriptorType;
    uint16_t bcdHID;
    uint8_t bCountryCode, bNumDescriptors, bReportType;
    uint16_t wReportLength;
} hid_descriptor;

/* ---- the one control handler this device needs -------------------------- */
/* The core answers every standard request itself, except GET_DESCRIPTOR for a
 * class-specific descriptor type - that is the Report descriptor below. Class
 * requests (the HID_* codes) are ours too. Returning < 0 STALLs the pipe. */
static int hid_control(void *ctx, const usb_setup *s, uint8_t *buf, uint16_t len)
{
    (void)ctx;

    if (USB_REQ_TYPE(s->bmRequestType) == USB_STANDARD && s->bRequest == USB_REQ_GET_DESCRIPTOR)
    {
        if (USB_U16_MSB(s->wValue) != HID_DT_REPORT)
            return -1;

        int n = (int)sizeof(KEYBOARD_REPORT_DESC) < len ? (int)sizeof(KEYBOARD_REPORT_DESC) : len;
        memcpy(buf, KEYBOARD_REPORT_DESC, (size_t)n);
        return n;
    }

    if (USB_REQ_TYPE(s->bmRequestType) != USB_CLASS)
        return -1;

    switch (s->bRequest)
    {
        case HID_GET_REPORT:
            if (len > KEY_REPORT_LEN)
                len = KEY_REPORT_LEN;
            memset(buf, 0, len); /* no key is held right now */
            return len;

        case HID_SET_REPORT:
        { /* the Num/Caps/Scroll Lock LED report, sent on endpoint 0 */
            uint8_t leds = len > 0 ? buf[0] : 0;
            const char *num_lock    = (leds & 0x01) ? "on" : "off";
            const char *caps_lock   = (leds & 0x02) ? "on" : "off";
            const char *scroll_lock = (leds & 0x04) ? "on" : "off";

            fprintf(stderr, "[kbd] LEDs: NumLock=%s CapsLock=%s ScrollLock=%s\n",
                    num_lock, caps_lock, scroll_lock);

            return 0;
        }

        case HID_SET_IDLE:
        case HID_SET_PROTOCOL:
            return 0;

        case HID_GET_IDLE:
            buf[0] = 0;
            return len >= 1 ? 1 : 0;

        case HID_GET_PROTOCOL:
            buf[0] = 1; /* report protocol */
            return len >= 1 ? 1 : 0;
    }
    return -1;
}

/* ---- typing ------------------------------------------------------------- */
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
    if (ch == '\n')
    {
        *code = 0x28;
        return 1;
    }
    return 0;
}

/* One key press is two reports on the interrupt IN endpoint: the key down, then
 * the key up. usbip_device_write() hands the report to a waiting IN transfer, or
 * queues it until the host asks for one. */
static void type_string(usbip_ep *hid_in, const char *text)
{
    for (const char *cursor = text; *cursor; cursor++)
    {
        uint8_t mod;
        uint8_t code;

        if (!key_for(*cursor, &mod, &code))
            continue;

        uint8_t press[KEY_REPORT_LEN] = {mod, 0, code, 0, 0, 0, 0, 0};
        uint8_t release[KEY_REPORT_LEN] = {0};
        usbip_device_write(hid_in, press, sizeof(press), 0); /* key down */
        usleep(20000);
        usbip_device_write(hid_in, release, sizeof(release), 0); /* key up   */
        usleep(20000);
    }
    fprintf(stderr, "[kbd] typed \"%s\"\n", text);
}

static void wait_here(void)
{
#ifdef _WIN32
    for (;;)
        sleep(1); /* no pause() on Windows; idle */
#else
    for (;;)
        pause();
#endif
}

int main(int argc, char **argv)
{
    int port = (argc > 1) ? atoi(argv[1]) : 3240; /* the USB/IP port to serve on */
    const char *text = (argc > 2) ? argv[2] : "hello";

    /* 1. the device: vendor + product id, and the strings the host displays */
    usbip_device *dev = usbip_device_create(VENDOR_ID, PRODUCT_ID);

    if (!dev)
    {
        fprintf(stderr, "usbip_device_create failed\n");
        return 1;
    }
    usbip_device_set_strings(dev, "USB over IP", "USBIP Boot Keyboard", "0011");

    /* 2. its descriptors, appended to the configuration in wire order:
     *    interface -> HID class descriptor -> endpoint */
    //! [descriptors]
    usbip_device_add_descriptor(dev, &(usb_interface_descriptor){
        .bDescriptorType    = USB_DT_INTERFACE,
        .bInterfaceNumber   = 0,
        .bNumEndpoints      = 0, /* auto-counted as endpoints are added */
        .bInterfaceClass    = USB_CLASS_HID,
        .bInterfaceSubClass = HID_SUBCLASS_BOOT,
        .bInterfaceProtocol = HID_PROTOCOL_KEYBOARD
    });

    usbip_device_add_descriptor(dev, &(hid_descriptor){
        .bLength         = 9,
        .bDescriptorType = HID_DT_HID,
        .bcdHID          = 0x0111, /* HID 1.11 */
        .bNumDescriptors = 1,
        .bReportType     = HID_DT_REPORT,
        .wReportLength   = sizeof(KEYBOARD_REPORT_DESC)
    });

    usbip_ep *hid_in = usbip_device_add_endpoint(dev, &(usb_endpoint_descriptor){
        .bDescriptorType  = USB_DT_ENDPOINT,
        .bEndpointAddress = EP_HID_IN,
        .bmAttributes     = USB_INTR,
        .wMaxPacketSize   = KEY_REPORT_LEN,
        .bInterval        = 10 /* poll every 10 ms */
    });

    /* 3. the requests the core cannot answer for us */
    usbip_device_on_control(dev, 0, hid_control, NULL);
    //! [descriptors]

    if (!hid_in)
    {
        fprintf(stderr, "usbip_device_add_endpoint failed\n");
        return 1;
    }

    /* 4. plug it in: serve USB/IP on every interface, on `port` */
    usb_transport *transport = usbip_transport(NULL, port);
    int rc = usbip_device_plug(dev, transport);

    if (rc != USB_SUCCESS)
    {
        fprintf(stderr, "usbip_device_plug failed: %s\n", usb_strerror(rc));
        return 1;
    }
    fprintf(stderr, "[kbd] serving %04x:%04x on :%d\n", VENDOR_ID, PRODUCT_ID, port);
    fprintf(stderr, "[kbd] attach it: sudo usbip attach -r 127.0.0.1 -b 1-1\n");

    /* 5. use it. The reports wait in the endpoint queue until a host attaches. */
    type_string(hid_in, text);

    wait_here();
    return 0;
}
