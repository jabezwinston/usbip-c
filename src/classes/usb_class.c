/**
 * Copyright (C) 2026 Jabez Winston
 *
 * SPDX-License-Identifier: MIT
 *
 * Date : 29-July-2026
 *
 * usb_class.c - the class layer: functions, interfaces, and the
 * usbip_device_class vtable. Built entirely on the PUBLIC device-core API
 * (descriptor groups + per-interface/per-endpoint callback registration);
 * it never touches core internals, so the core links on its own
 * (libusbip-device) and this layer sits on top.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "classes/usb_class.h"

/* One USB interface == one bInterfaceNumber; the class triple is remembered so
 * alternate settings inherit it, and cur_alt tracks the append cursor. */
struct usbip_interface {
    usbip_function *func;
    usbip_device *dev;
    uint8_t ifnum;                      /* bInterfaceNumber */
    uint8_t icls, isub, iproto;         /* class triple, inherited by every alt setting */
    uint8_t cur_alt;                    /* append cursor (last alt added) */
};

/* A function == one class instance: owns the vtable/state and >=1 interface. On
 * the wire it is one descriptor group; the core keeps only that. */
struct usbip_function {
    usbip_device *dev;
    int group;                      /* the core descriptor group backing this function */
    int base_ifnum;                 /* bInterfaceNumber of this function's first interface,
                                     * fixed at creation so it stays valid after interfaces
                                     * are added (see usbip_function_base_ifnum). */
    const usbip_device_class *cls;
    usbip_function_control_fn control_cb;     /* for bare functions */
    void *state;
    usbip_ep *eps[16];
    int n_eps;
    struct usbip_interface *ifaces[8];  /* child interfaces, in add order */
    int n_ifaces;
    struct usbip_interface *cur_iface;  /* append cursor (last interface added) */
};

/* Per-device function registry: powers usbip_function_instance() and the
 * device-level control fallback, without any core support. */
struct uc_registry {
    usbip_device *dev;
    usbip_function *funcs[8];
    int n;
};
static struct uc_registry uc_regs[16];

static struct uc_registry *uc_reg_for(usbip_device *dev)
{
    struct uc_registry *empty = NULL;
    for (int i = 0; i < (int)(sizeof(uc_regs) / sizeof(uc_regs[0])); i++)
    {
        if (uc_regs[i].dev == dev)
            return &uc_regs[i];
        if (!uc_regs[i].dev && !empty)
            empty = &uc_regs[i];
    }
    if (empty)
        empty->dev = dev;
    return empty;
}

/* ---- trampolines: core (ctx-based) -> vtable (usbip_function*-based) ---- */
static int uc_ctrl_tramp(void *ctx, const usb_setup *setup, uint8_t *buf, uint16_t len)
{
    usbip_function *func = ctx;
    if (func->cls && func->cls->control)
        return func->cls->control(func, setup, buf, len);
    if (func->control_cb)
        return func->control_cb(func, setup, buf, len);
    return -1; /* STALL */
}

static int uc_set_alt_tramp(void *ctx, int ifnum, int alt)
{
    usbip_function *func = ctx;
    return func->cls->set_alt(func, ifnum, alt);
}

static void uc_out_tramp(void *ctx, usbip_ep *ep, const void *data, int len)
{
    usbip_function *func = ctx;
    func->cls->on_out(func, ep, data, len);
}

static int uc_iso_tramp(void *ctx, usbip_ep *ep, int npkts, uint32_t *lens, uint8_t *buf)
{
    usbip_function *func = ctx;
    return func->cls->on_iso(func, ep, npkts, lens, buf);
}

/* Find func's child interface for ifnum, creating it -- and registering the
 * function's trampolines -- on first sight. Alt settings reuse the object. */
static usbip_interface *uc_iface_for(usbip_function *func, uint8_t ifnum)
{
    for (int i = 0; i < func->n_ifaces; i++)
        if (func->ifaces[i]->ifnum == ifnum)
            return func->ifaces[i];
    if (func->n_ifaces >= (int)(sizeof(func->ifaces) / sizeof(func->ifaces[0])))
        return NULL;
    usbip_interface *iface = calloc(1, sizeof(*iface));
    if (!iface)
        return NULL;
    iface->func = func;
    iface->dev = func->dev;
    iface->ifnum = ifnum;
    func->ifaces[func->n_ifaces++] = iface;
    usbip_device_on_control(func->dev, ifnum, uc_ctrl_tramp, func);
    if (func->cls && func->cls->set_alt)
        usbip_device_on_set_alt(func->dev, ifnum, uc_set_alt_tramp, func);
    return iface;
}

usbip_function *usbip_device_add_function(usbip_device *dev)
{
    struct uc_registry *reg = uc_reg_for(dev);
    if (!reg || reg->n >= (int)(sizeof(reg->funcs) / sizeof(reg->funcs[0])))
    {
        fprintf(stderr, "[usbip_device] function table full (%d); function not added\n",
                reg ? reg->n : 0);
        return NULL;
    }
    int group = usbip_device_group_begin(dev);
    if (group < 0)
        return NULL;
    usbip_function *func = calloc(1, sizeof(*func));
    if (!func)
    {
        usbip_device_group_abort(dev);
        return NULL;
    }
    func->dev = dev;
    func->group = group;
    func->base_ifnum = usbip_device_get_num_interfaces(dev); /* what its first interface will be numbered */
    if (reg->n == 0) /* the first function is the device-level control fallback */
        usbip_device_on_control(dev, -1, uc_ctrl_tramp, func);
    reg->funcs[reg->n++] = func;
    return func;
}

/* Undo a function whose build failed: roll the core back to the group mark, then free
 * the class-layer objects. Only ever the last function, before any host attaches. */
static void uc_function_free(usbip_device *dev, usbip_function *func)
{
    usbip_device_group_abort(dev);
    for (int i = 0; i < func->n_ifaces; i++)
        free(func->ifaces[i]);
    struct uc_registry *reg = uc_reg_for(dev);
    if (reg)
        for (int i = 0; i < reg->n; i++)
            if (reg->funcs[i] == func)
            {
                memmove(&reg->funcs[i], &reg->funcs[i + 1],
                        (size_t)(reg->n - i - 1) * sizeof(reg->funcs[0]));
                reg->n--;
                break;
            }
    free(func->state);
    free(func);
}

void usbip_function_on_control(usbip_function *func, usbip_function_control_fn cb)
{
    func->control_cb = cb;
}

int usbip_function_add_descriptor(usbip_function *func, const void *descriptor)
{
    const uint8_t *desc = descriptor;
    usbip_device *dev = func->dev;

    if (desc[1] == USB_DT_ENDPOINT)
    { /* keep the pipe: the function's endpoint lookups and callbacks need it */
        if (func->n_eps >= (int)(sizeof(func->eps) / sizeof(func->eps[0])))
        {
            fprintf(stderr, "[usbip_device] endpoint table full (%d); endpoint dropped\n",
                    func->n_eps);
            return USB_ERROR_NO_MEM;
        }
        usbip_ep *ep = usbip_device_add_endpoint(dev, descriptor);
        if (!ep)
            return USB_ERROR_NO_MEM;
        func->eps[func->n_eps++] = ep;
        if (func->cls)
        { /* only the callbacks the vtable declares, so the defaults stay live */
            if (func->cls->on_out)
                usbip_ep_on_out(ep, uc_out_tramp, func);
            if (func->cls->on_iso)
                usbip_ep_on_iso(ep, uc_iso_tramp, func);
        }
        return USB_SUCCESS;
    }

    int rc = usbip_device_add_descriptor(dev, descriptor);
    if (rc != USB_SUCCESS)
        return rc;
    if (desc[1] == USB_DT_INTERFACE)
    { /* desc[2]=bInterfaceNumber, desc[3]=bAlternateSetting */
        usbip_interface *iface = uc_iface_for(func, desc[2]);
        if (iface)
        {
            func->cur_iface = iface;
            iface->cur_alt = desc[3];
        }
    }
    return USB_SUCCESS;
}

usbip_ep *usbip_function_add_endpoint(usbip_function *func, const void *ep_descriptor)
{
    const uint8_t *desc = ep_descriptor;
    usb_dir dir = (desc[2] & 0x80) ? USB_IN : USB_OUT;
    if (usbip_function_add_descriptor(func, ep_descriptor) != USB_SUCCESS)
        return NULL;
    /* the endpoint just appended is this function's last -- return it directly,
     * since its address may differ from the one requested */
    usbip_ep *ep = func->n_eps ? func->eps[func->n_eps - 1] : NULL;
    if (!ep)
        return NULL;
    usb_dir got = (usbip_endpoint_address(ep) & 0x80) ? USB_IN : USB_OUT;
    return got == dir ? ep : NULL;
}

usbip_ep *usbip_function_endpoint(usbip_function *func, uint8_t addr)
{
    /* Resolve within this function, not device-globally: several functions hold
     * endpoints, and one may have been relocated off the address it asked for. */
    int number = addr & 0x0f;
    uint8_t dirbit = addr & 0x80;
    for (int i = 0; i < func->n_eps; i++)
    {
        uint8_t ep_addr = usbip_endpoint_address(func->eps[i]);
        if ((ep_addr & 0x0f) == number && (ep_addr & 0x80) == dirbit)
            return func->eps[i];
    }
    return NULL;
}

uint8_t usbip_function_reserve_endpoint(usbip_function *func, uint8_t want_addr)
{
    if (!func || !func->cur_iface) /* no interface yet: nothing can own the claim */
        return 0;
    return usbip_device_reserve_endpoint(func->dev, want_addr);
}

int usbip_function_enable_msos(usbip_function *func, const char *compatible, const char *guid)
{
    if (!func)
        return USB_ERROR_INVALID_PARAM;
    return usbip_device_enable_msos_group(func->dev, func->group, compatible, guid);
}

int usbip_function_enable_winusb(usbip_function *func, const char *guid)
{
    const char *device_guid = guid ? guid : USBIP_WINUSB_DEFAULT_GUID;

    return usbip_function_enable_msos(func, "WINUSB", device_guid);
}

void usbip_class_vlog(void (*emit)(void *user, const char *text), void *user,
                      const char *prefix, const char *fmt, va_list ap)
{
    if (!emit)
        return;

    char buf[160];
    int off = 0;
    if (prefix)
        off = snprintf(buf, sizeof(buf), "%s", prefix);

    if (off < 0 || off >= (int)sizeof(buf))
        off = 0;

    vsnprintf(buf + off, sizeof(buf) - (size_t)off, fmt, ap);
    emit(user, buf);
}

/* ---- class instantiation (generic; the class is passed by pointer) ------ */
usbip_function *usbip_device_add_class(usbip_device *dev, const usbip_device_class *cls,
                                       const void *params)
{
    if (!cls)
        return NULL;

    usbip_function *func = usbip_device_add_function(dev);
    if (!func)
        return NULL;
    func->cls = cls;
    if (cls->state_size)
    {
        func->state = calloc(1, cls->state_size);
        if (!func->state)
        {
            uc_function_free(dev, func);
            return NULL;
        }
    }
    if (cls->build && cls->build(func, params) != 0)
    { /* class declares its descriptors */
        const char *name = cls->name ? cls->name : "?";

        fprintf(stderr, "[usbip_device] class '%s' failed to build; function removed\n", name);
        if (cls->destroy)
            cls->destroy(func);
        uc_function_free(dev, func);
        return NULL;
    }
    return func;
}

void *usbip_function_state(usbip_function *func)
{
    return func->state;
}

usbip_device *usbip_function_device(usbip_function *func)
{
    return func->dev;
}

int usbip_function_instance(usbip_function *func)
{
    struct uc_registry *reg = uc_reg_for(func->dev);
    int index = 0;
    for (int i = 0; reg && i < reg->n; i++)
    {
        usbip_function *other = reg->funcs[i];
        if (other == func)
            return index;
        if (other->cls && other->cls == func->cls)
            index++;
    }
    return index;
}

int usbip_function_base_ifnum(usbip_function *func)
{
    return func->base_ifnum;
}

/* ---- two-level builder API (function -> interface -> alt setting) ------- */
usbip_interface *usbip_function_add_interface(usbip_function *func, uint8_t cls, uint8_t sub, uint8_t proto)
{
    uint8_t ifnum = (uint8_t)usbip_device_get_num_interfaces(func->dev); /* next free bInterfaceNumber */
    usb_interface_descriptor desc = {
        .bDescriptorType = USB_DT_INTERFACE,
        .bInterfaceNumber = ifnum,
        .bAlternateSetting = 0,
        .bInterfaceClass = cls,
        .bInterfaceSubClass = sub,
        .bInterfaceProtocol = proto,
    };
    usbip_function_add_descriptor(func, &desc);
    func->cur_iface->icls = cls; /* remembered so alt settings inherit the triple */
    func->cur_iface->isub = sub;
    func->cur_iface->iproto = proto;
    return func->cur_iface;
}

int usbip_interface_add_altsetting(usbip_interface *iface, uint8_t alt)
{
    usb_interface_descriptor desc = {
        .bDescriptorType = USB_DT_INTERFACE,
        .bInterfaceNumber = iface->ifnum,
        .bAlternateSetting = alt,
        .bInterfaceClass = iface->icls,
        .bInterfaceSubClass = iface->isub,
        .bInterfaceProtocol = iface->iproto,
    };
    return usbip_function_add_descriptor(iface->func, &desc);
}

int usbip_interface_add_descriptor(usbip_interface *iface, const void *descriptor)
{
    return usbip_function_add_descriptor(iface->func, descriptor);
}

usbip_ep *usbip_interface_add_endpoint(usbip_interface *iface, const void *ep_descriptor)
{
    /* Return the object rather than looking it back up by address: the allocator may
     * have relocated it. bNumEndpoints is auto-counted by the core append path. */
    return usbip_function_add_endpoint(iface->func, ep_descriptor);
}

int usbip_interface_number(usbip_interface *iface)
{
    return iface->ifnum;
}

void usbip_interface_set_string(usbip_interface *iface, uint8_t istr)
{
    usbip_device_set_interface_string(iface->dev, iface->ifnum, iface->cur_alt, istr);
}

void usbip_function_associate(usbip_function *func, uint8_t count, uint8_t cls,
                              uint8_t sub, uint8_t proto, uint8_t iFunction)
{
    /* Emit the 8-byte Interface Association Descriptor now, so it precedes the
     * function's first interface descriptor in the blob (USB-IF IAD ECN). */
    uint8_t first = (uint8_t)usbip_device_get_num_interfaces(func->dev);
    uint8_t iad[8] = {8, USB_DT_INTERFACE_ASSOCIATION, first, count, cls, sub, proto, iFunction};
    usbip_function_add_descriptor(func, iad);
}
