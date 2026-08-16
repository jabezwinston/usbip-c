/**
 * Copyright (C) 2026 Jabez Winston
 *
 * SPDX-License-Identifier: MIT
 *
 * Date : 05-June-2026
 *
 * libusb.c - a drop-in libusb-1.0 implemented on the USBIP C library's low-level USB/IP
 * client primitives (usbip_connect / usbip_client_devlist / usbip_client_import /
 * usbip_client_submit[_iso], declared in usbip_device_internal.h). NOT built on usbip_host_*.
 *
 * A program compiled against the real <libusb.h> can LD_PRELOAD this .so and drive
 * a usbip virtual device over USB/IP - no kernel, no root, no vhci. The USB/IP
 * server is chosen by env: USBIP_HOST / USBIP_PORT (default 127.0.0.1:3240),
 * read once in libusb_init(). There are no non-libusb exported symbols.
 *
 * The USBIP primitives are synchronous; the async API (submit/handle_events/
 * callbacks) is layered on top via one worker thread + a completion queue + a
 * self-pipe per context (see "asynchronous I/O" below).
 */
#define _GNU_SOURCE
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include "os_compat.h" /* winsock; the poll()/pipe self-wakeup is emulated below */
#undef interface       /* windows.h defines `interface`; libusb uses it as a field name */
#else
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>
#define sock_close close
#endif
#include <pthread.h>
#include <sys/time.h>

#include "usbip_device_internal.h" /* usbip_* client primitives, struct usbip_devinfo, iso_pkt */
#include "libusb.h"

#ifdef _WIN32
/* WSAPoll only polls SOCKETs and there is no pipe()/fcntl(O_NONBLOCK) on Windows,
 * so emulate the self-pipe with a connected non-blocking loopback socket pair. */
static int usbip_socketpair(int fds[2])
{
    usbip_net_startup();
    SOCKET listener = socket(AF_INET, SOCK_STREAM, 0);
    if (listener == INVALID_SOCKET)
        return -1;
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    int len = (int)sizeof(addr);
    SOCKET cli = INVALID_SOCKET, srv = INVALID_SOCKET;
    if (bind(listener, (struct sockaddr *)&addr, sizeof(addr)) ||
        listen(listener, 1) ||
        getsockname(listener, (struct sockaddr *)&addr, &len))
        goto fail;
    cli = socket(AF_INET, SOCK_STREAM, 0);
    if (cli == INVALID_SOCKET)
        goto fail;
    if (connect(cli, (struct sockaddr *)&addr, sizeof(addr)))
        goto fail;
    srv = accept(listener, NULL, NULL);
    if (srv == INVALID_SOCKET)
        goto fail;
    closesocket(listener);
    u_long nb = 1;
    ioctlsocket(srv, FIONBIO, &nb);
    ioctlsocket(cli, FIONBIO, &nb);
    fds[0] = (int)srv; /* read end  */
    fds[1] = (int)cli; /* write end */
    return 0;
fail:
    if (listener != INVALID_SOCKET)
        closesocket(listener);
    if (cli != INVALID_SOCKET)
        closesocket(cli);
    return -1;
}
#endif

/* ===================================================================== */
/* internal types                                                        */
/* ===================================================================== */
struct itransfer
{ /* private header placed BEFORE the public transfer */
    struct itransfer *next;
    struct libusb_context *ctx;
    int state; /* ST_* */
    int cancelled;
};
#define ST_IDLE     0 /* allocated, never submitted           */
#define ST_QUEUED   1 /* on the submit queue                  */
#define ST_INFLIGHT 2 /* the worker is running it             */
#define ST_DONE     3 /* on the done queue, awaiting callback */

#define PUB_OF(it) ((struct libusb_transfer *)((unsigned char *)(it) + sizeof(struct itransfer)))
#define IT_OF(pub) ((struct itransfer *)((unsigned char *)(pub) - sizeof(struct itransfer)))

struct libusb_context
{
    usb_transport *t;
    /* async event machinery */
    pthread_mutex_t lock;                      /* protects the two queues + worker flags */
    pthread_cond_t qcond;                      /* worker waits here for submissions */
    struct itransfer *subq_head, *subq_tail;   /* submitted, not yet run */
    struct itransfer *doneq_head, *doneq_tail; /* run, awaiting handle_events */
    pthread_t worker;
    int worker_started;
    int shutting;
    int pipe_r, pipe_w;     /* self-pipe: one byte per completion */
    pthread_mutex_t evlock; /* libusb_lock_events() */
    libusb_pollfd_added_cb pfd_added;
    libusb_pollfd_removed_cb pfd_removed;
    void *pfd_user;
};

struct libusb_device
{
    struct libusb_context *ctx;
    int refcnt;
    struct usbip_devinfo info;
    uint8_t *cfg_raw; /* cached config-descriptor blob (lazy) */
    int cfg_len;
};

struct libusb_device_handle
{
    struct libusb_device *dev;
    int fd;
    uint32_t devid;
    uint32_t seq;
    pthread_mutex_t iolock; /* serialize socket I/O (sync API + worker) */
};

/* default context for libusb_init(NULL)/calls with ctx==NULL */
static libusb_context *g_default;
static int g_default_rc;
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER; /* guards default ctx + refcounts */

static libusb_context *resolve_ctx(libusb_context *ctx)
{
    return ctx ? ctx : g_default;
}

/* ===================================================================== */
/* low-level control helper (over usbip_client_submit)                   */
/* ===================================================================== */
static int ctrl_raw(int fd, uint32_t devid, uint32_t *seq, pthread_mutex_t *lock,
                    uint8_t bmReq, uint8_t bReq, uint16_t wValue, uint16_t wIndex,
                    uint8_t *data, uint16_t wLen, int *actual)
{
    uint8_t setup[8] = {USB_SETUP_BYTES(bmReq, bReq, wValue, wIndex, wLen)};
    int dir = (bmReq & 0x80) ? USB_IN : USB_OUT;
    int act = 0;
    if (lock)
        pthread_mutex_lock(lock);
    int rc = usbip_client_submit(fd, devid, seq, dir, 0, setup, 0, data, wLen, &act);
    if (lock)
        pthread_mutex_unlock(lock);
    if (actual)
        *actual = act;
    return rc;
}

static int ctrl_io(struct libusb_device_handle *handle, uint8_t bmReq, uint8_t bReq,
                   uint16_t wValue, uint16_t wIndex, uint8_t *data, uint16_t wLen, int *actual)
{
    return ctrl_raw(handle->fd, handle->devid, &handle->seq, &handle->iolock, bmReq, bReq, wValue, wIndex,
                    data, wLen, actual);
}

static enum libusb_transfer_status map_status(int rc)
{
    switch (rc)
    {
    case USB_SUCCESS:
        return LIBUSB_TRANSFER_COMPLETED;
    case USB_ERROR_PIPE:
        return LIBUSB_TRANSFER_STALL;
    case USB_ERROR_NO_DEVICE:
        return LIBUSB_TRANSFER_NO_DEVICE;
    case USB_ERROR_TIMEOUT:
        return LIBUSB_TRANSFER_TIMED_OUT;
    default:
        return LIBUSB_TRANSFER_ERROR;
    }
}

/* ===================================================================== */
/* library / context                                                     */
/* ===================================================================== */
static libusb_context *ctx_new(void)
{
    libusb_context *ctx = calloc(1, sizeof(*ctx));
    if (!ctx)
        return NULL;
    const char *host = getenv("USBIP_HOST");
    const char *ports = getenv("USBIP_PORT");
    int port = ports ? atoi(ports) : 0; /* 0 -> usbip_transport picks 3240 */
    ctx->t = usbip_transport(host, port); /* host NULL -> 127.0.0.1 */
    pthread_mutex_init(&ctx->lock, NULL);
    pthread_mutex_init(&ctx->evlock, NULL);
    pthread_cond_init(&ctx->qcond, NULL);
    ctx->pipe_r = ctx->pipe_w = -1;
    int pf[2];
#ifdef _WIN32
    if (usbip_socketpair(pf) == 0)
    { /* already non-blocking */
        ctx->pipe_r = pf[0];
        ctx->pipe_w = pf[1];
    }
#else
    if (pipe(pf) == 0)
    {
        fcntl(pf[0], F_SETFL, O_NONBLOCK);
        fcntl(pf[1], F_SETFL, O_NONBLOCK);
        ctx->pipe_r = pf[0];
        ctx->pipe_w = pf[1];
    }
#endif
    return ctx;
}

static void *worker_main(void *arg);

static void ctx_free(libusb_context *ctx)
{
    if (!ctx)
        return;
    if (ctx->worker_started)
    {
        pthread_mutex_lock(&ctx->lock);
        ctx->shutting = 1;
        pthread_cond_signal(&ctx->qcond);
        pthread_mutex_unlock(&ctx->lock);
        pthread_join(ctx->worker, NULL);
    }
    if (ctx->pipe_r >= 0)
        sock_close(ctx->pipe_r);
    if (ctx->pipe_w >= 0)
        sock_close(ctx->pipe_w);
    pthread_mutex_destroy(&ctx->lock);
    pthread_mutex_destroy(&ctx->evlock);
    pthread_cond_destroy(&ctx->qcond);
    if (ctx->t)
        usbip_transport_free(ctx->t);
    free(ctx);
}

int LIBUSB_CALL libusb_init(libusb_context **ctx)
{
    if (ctx)
    {
        libusb_context *created = ctx_new();
        if (!created)
            return LIBUSB_ERROR_NO_MEM;
        *ctx = created;
        return LIBUSB_SUCCESS;
    }
    /* default context (libusb_init(NULL)) */
    pthread_mutex_lock(&g_lock);
    if (!g_default)
        g_default = ctx_new();
    int ok = g_default != NULL;
    if (ok)
        g_default_rc++;
    pthread_mutex_unlock(&g_lock);
    return ok ? LIBUSB_SUCCESS : LIBUSB_ERROR_NO_MEM;
}

void LIBUSB_CALL libusb_exit(libusb_context *ctx)
{
    if (!ctx)
    {
        pthread_mutex_lock(&g_lock);
        if (g_default_rc > 0 && --g_default_rc == 0)
        {
            ctx_free(g_default);
            g_default = NULL;
        }
        pthread_mutex_unlock(&g_lock);
        return;
    }
    ctx_free(ctx);
}

void LIBUSB_CALL libusb_set_debug(libusb_context *ctx, int level)
{
    (void)ctx;
    (void)level;
}
int LIBUSB_CALLV libusb_set_option(libusb_context *ctx, enum libusb_option option, ...)
{
    (void)ctx;
    (void)option;
    return LIBUSB_SUCCESS;
}
void LIBUSB_CALL libusb_set_log_cb(libusb_context *ctx, libusb_log_cb cb, int mode)
{
    (void)ctx;
    (void)cb;
    (void)mode;
}
int LIBUSB_CALL libusb_setlocale(const char *locale)
{
    (void)locale;
    return LIBUSB_SUCCESS;
}

const struct libusb_version *LIBUSB_CALL libusb_get_version(void)
{
    static const struct libusb_version version = {1, 0, 27, 0, "", "USB over IP"};
    return &version;
}

int LIBUSB_CALL libusb_has_capability(uint32_t capability)
{
    switch (capability)
    {
    case LIBUSB_CAP_HAS_CAPABILITY:
        return 1;
    case LIBUSB_CAP_HAS_HID_ACCESS:
        return 1; /* HID is just control/interrupt */
    case LIBUSB_CAP_HAS_HOTPLUG:
        return 0; /* honest: no hotplug events */
    case LIBUSB_CAP_SUPPORTS_DETACH_KERNEL_DRIVER:
        return 0;
    default:
        return 0;
    }
}

const char *LIBUSB_CALL libusb_error_name(int errcode)
{
    switch (errcode)
    {
    case LIBUSB_SUCCESS:
        return "LIBUSB_SUCCESS";
    case LIBUSB_ERROR_IO:
        return "LIBUSB_ERROR_IO";
    case LIBUSB_ERROR_INVALID_PARAM:
        return "LIBUSB_ERROR_INVALID_PARAM";
    case LIBUSB_ERROR_ACCESS:
        return "LIBUSB_ERROR_ACCESS";
    case LIBUSB_ERROR_NO_DEVICE:
        return "LIBUSB_ERROR_NO_DEVICE";
    case LIBUSB_ERROR_NOT_FOUND:
        return "LIBUSB_ERROR_NOT_FOUND";
    case LIBUSB_ERROR_BUSY:
        return "LIBUSB_ERROR_BUSY";
    case LIBUSB_ERROR_TIMEOUT:
        return "LIBUSB_ERROR_TIMEOUT";
    case LIBUSB_ERROR_OVERFLOW:
        return "LIBUSB_ERROR_OVERFLOW";
    case LIBUSB_ERROR_PIPE:
        return "LIBUSB_ERROR_PIPE";
    case LIBUSB_ERROR_INTERRUPTED:
        return "LIBUSB_ERROR_INTERRUPTED";
    case LIBUSB_ERROR_NO_MEM:
        return "LIBUSB_ERROR_NO_MEM";
    case LIBUSB_ERROR_NOT_SUPPORTED:
        return "LIBUSB_ERROR_NOT_SUPPORTED";
    case LIBUSB_ERROR_OTHER:
        return "LIBUSB_ERROR_OTHER";
    default:
        return "LIBUSB_ERROR_UNKNOWN";
    }
}

/* values match usb.h, so usb_strerror() messages line up */
const char *LIBUSB_CALL libusb_strerror(int errcode)
{
    return usb_strerror(errcode);
}

/* ===================================================================== */
/* enumeration + refcount                                                */
/* ===================================================================== */
ssize_t LIBUSB_CALL libusb_get_device_list(libusb_context *ctx, libusb_device ***list)
{
    ctx = resolve_ctx(ctx);
    if (!ctx)
        return LIBUSB_ERROR_INVALID_PARAM;

    int fd = usbip_connect(ctx->t);
    if (fd < 0)
        return LIBUSB_ERROR_IO;
    struct usbip_devinfo infos[16];
    int count = usbip_client_devlist(fd, infos, 16);
    sock_close(fd);
    if (count < 0)
        return count;

    libusb_device **arr = calloc((size_t)count + 1, sizeof(*arr));
    if (!arr)
        return LIBUSB_ERROR_NO_MEM;
    for (int i = 0; i < count; i++)
    {
        libusb_device *device = calloc(1, sizeof(*device));
        if (!device)
        {
            for (int j = 0; j < i; j++)
                free(arr[j]);
            free(arr);
            return LIBUSB_ERROR_NO_MEM;
        }
        device->ctx = ctx;
        device->refcnt = 1;
        device->info = infos[i];
        arr[i] = device;
    }
    *list = arr;
    return count;
}

static void device_free(libusb_device *dev)
{
    if (dev)
    {
        free(dev->cfg_raw);
        free(dev);
    }
}

libusb_device *LIBUSB_CALL libusb_ref_device(libusb_device *dev)
{
    pthread_mutex_lock(&g_lock);
    dev->refcnt++;
    pthread_mutex_unlock(&g_lock);
    return dev;
}

void LIBUSB_CALL libusb_unref_device(libusb_device *dev)
{
    if (!dev)
        return;
    pthread_mutex_lock(&g_lock);
    int last_ref = (--dev->refcnt <= 0);
    pthread_mutex_unlock(&g_lock);
    if (last_ref)
        device_free(dev);
}

void LIBUSB_CALL libusb_free_device_list(libusb_device **list, int unref_devices)
{
    if (!list)
        return;
    if (unref_devices)
        for (int i = 0; list[i]; i++)
            libusb_unref_device(list[i]);
    free(list);
}

/* ---- per-device getters ---------------------------------------------- */
uint8_t LIBUSB_CALL libusb_get_bus_number(libusb_device *dev) { return (uint8_t)dev->info.busnum; }
uint8_t LIBUSB_CALL libusb_get_device_address(libusb_device *dev) { return (uint8_t)dev->info.devnum; }
uint8_t LIBUSB_CALL libusb_get_port_number(libusb_device *dev)
{
    (void)dev;
    return 1;
}
int LIBUSB_CALL libusb_get_port_numbers(libusb_device *dev, uint8_t *pn, int len)
{
    (void)dev;
    if (len > 0)
    {
        pn[0] = 1;
        return 1;
    }
    return LIBUSB_ERROR_OVERFLOW;
}

int LIBUSB_CALL libusb_get_device_speed(libusb_device *dev)
{
    switch (dev->info.speed)
    { /* kernel usb_device_speed -> libusb_speed */
    case 1:
        return LIBUSB_SPEED_LOW;
    case 2:
        return LIBUSB_SPEED_FULL;
    case 3:
        return LIBUSB_SPEED_HIGH;
    case 5:
        return LIBUSB_SPEED_SUPER;
    case 6:
        return LIBUSB_SPEED_SUPER_PLUS;
    default:
        return LIBUSB_SPEED_UNKNOWN;
    }
}

libusb_device *LIBUSB_CALL libusb_get_device(libusb_device_handle *handle)
{
    return handle ? handle->dev : NULL;
}

libusb_device *LIBUSB_CALL libusb_get_parent(libusb_device *dev)
{
    (void)dev;
    return NULL;
} /* no hub topology over USB/IP */

/* ===================================================================== */
/* open / close / config / interface                                     */
/* ===================================================================== */
int LIBUSB_CALL libusb_open(libusb_device *dev, libusb_device_handle **out)
{
    if (!dev || !out)
        return LIBUSB_ERROR_INVALID_PARAM;
    int fd = usbip_connect(dev->ctx->t);
    if (fd < 0)
        return LIBUSB_ERROR_IO;
    struct usbip_devinfo imp;
    if (usbip_client_import(fd, dev->info.busid, &imp) != USB_SUCCESS)
    {
        sock_close(fd);
        return LIBUSB_ERROR_NO_DEVICE;
    }

    libusb_device_handle *handle = calloc(1, sizeof(*handle));
    if (!handle)
    {
        sock_close(fd);
        return LIBUSB_ERROR_NO_MEM;
    }
    handle->dev = libusb_ref_device(dev);
    handle->fd = fd;
    handle->devid = (imp.busnum << 16) | imp.devnum;
    pthread_mutex_init(&handle->iolock, NULL);
    *out = handle;
    return LIBUSB_SUCCESS;
}

libusb_device_handle *LIBUSB_CALL libusb_open_device_with_vid_pid(libusb_context *ctx, uint16_t vid, uint16_t pid)
{
    libusb_device **list;
    ssize_t count = libusb_get_device_list(ctx, &list);
    if (count < 0)
        return NULL;
    libusb_device_handle *handle = NULL;
    for (ssize_t i = 0; i < count; i++)
    {
        if (list[i]->info.vid == vid && list[i]->info.pid == pid)
        {
            if (libusb_open(list[i], &handle) != LIBUSB_SUCCESS)
                handle = NULL;
            break;
        }
    }
    libusb_free_device_list(list, 1); /* handle keeps its own ref on the matched dev */
    return handle;
}

void LIBUSB_CALL libusb_close(libusb_device_handle *handle)
{
    if (!handle)
        return;
    if (handle->fd >= 0)
        sock_close(handle->fd);
    pthread_mutex_destroy(&handle->iolock);
    libusb_unref_device(handle->dev);
    free(handle);
}

int LIBUSB_CALL libusb_set_configuration(libusb_device_handle *handle, int config)
{
    if (!handle)
        return LIBUSB_ERROR_NO_DEVICE;
    int rc = ctrl_io(handle, 0x00, LIBUSB_REQUEST_SET_CONFIGURATION, (uint16_t)config, 0, NULL, 0, NULL);
    return rc < 0 ? rc : LIBUSB_SUCCESS;
}

int LIBUSB_CALL libusb_get_configuration(libusb_device_handle *handle, int *config)
{
    if (!handle || !config)
        return LIBUSB_ERROR_INVALID_PARAM;
    uint8_t cfg_byte = 1;
    int rc = ctrl_io(handle, 0x80, LIBUSB_REQUEST_GET_CONFIGURATION, 0, 0, &cfg_byte, 1, NULL);
    *config = (rc < 0) ? 1 : cfg_byte; /* default to 1 if the device doesn't answer */
    return LIBUSB_SUCCESS;
}

/* USB/IP imports the whole device into userspace: there is no kernel driver bound
 * on this side, so claim/release are no-ops that succeed (mirrors usbip_host.c). */
int LIBUSB_CALL libusb_claim_interface(libusb_device_handle *handle, int iface)
{
    (void)iface;
    return handle ? LIBUSB_SUCCESS : LIBUSB_ERROR_NO_DEVICE;
}
int LIBUSB_CALL libusb_release_interface(libusb_device_handle *handle, int iface)
{
    (void)iface;
    return handle ? LIBUSB_SUCCESS : LIBUSB_ERROR_NO_DEVICE;
}

int LIBUSB_CALL libusb_set_interface_alt_setting(libusb_device_handle *handle, int iface, int alt)
{
    if (!handle)
        return LIBUSB_ERROR_NO_DEVICE;
    int rc = ctrl_io(handle, 0x01, LIBUSB_REQUEST_SET_INTERFACE, (uint16_t)alt, (uint16_t)iface, NULL, 0, NULL);
    return rc < 0 ? rc : LIBUSB_SUCCESS;
}

int LIBUSB_CALL libusb_clear_halt(libusb_device_handle *handle, unsigned char endpoint)
{
    if (!handle)
        return LIBUSB_ERROR_NO_DEVICE;
    int rc = ctrl_io(handle, 0x02, LIBUSB_REQUEST_CLEAR_FEATURE, USB_FEATURE_ENDPOINT_HALT, endpoint, NULL, 0, NULL);
    return rc < 0 ? rc : LIBUSB_SUCCESS;
}

int LIBUSB_CALL libusb_reset_device(libusb_device_handle *handle)
{
    return handle ? LIBUSB_SUCCESS : LIBUSB_ERROR_NO_DEVICE;
}

/* kernel-driver calls are meaningless over USB/IP import; report "nothing bound". */
int LIBUSB_CALL libusb_kernel_driver_active(libusb_device_handle *handle, int iface)
{
    (void)handle;
    (void)iface;
    return 0;
}

int LIBUSB_CALL libusb_detach_kernel_driver(libusb_device_handle *handle, int iface)
{
    (void)handle;
    (void)iface;
    return LIBUSB_SUCCESS;
}

int LIBUSB_CALL libusb_attach_kernel_driver(libusb_device_handle *handle, int iface)
{
    (void)handle;
    (void)iface;
    return LIBUSB_SUCCESS;
}

int LIBUSB_CALL libusb_set_auto_detach_kernel_driver(libusb_device_handle *handle, int enable)
{
    (void)handle;
    (void)enable;
    return LIBUSB_SUCCESS;
}

/* ===================================================================== */
/* descriptors                                                           */
/* ===================================================================== */
int LIBUSB_CALL libusb_get_device_descriptor(libusb_device *dev, struct libusb_device_descriptor *desc)
{
    if (!dev || !desc)
        return LIBUSB_ERROR_INVALID_PARAM;
    struct usbip_devinfo *in = &dev->info;
    memset(desc, 0, sizeof(*desc));
    desc->bLength = LIBUSB_DT_DEVICE_SIZE;
    desc->bDescriptorType = LIBUSB_DT_DEVICE;
    desc->bcdUSB = 0x0200;
    desc->bDeviceClass = in->dclass;
    desc->bDeviceSubClass = in->dsub;
    desc->bDeviceProtocol = in->dproto;
    desc->bMaxPacketSize0 = 64;
    desc->idVendor = in->vid;
    desc->idProduct = in->pid;
    desc->bcdDevice = in->bcdDevice;
    desc->bNumConfigurations = in->n_cfg ? in->n_cfg : 1;
    return LIBUSB_SUCCESS;
}

/* fetch (and cache) the raw config-descriptor blob via a short-lived import */
static int fetch_config_blob(libusb_device *dev)
{
    if (dev->cfg_raw)
        return LIBUSB_SUCCESS;
    int fd = usbip_connect(dev->ctx->t);
    if (fd < 0)
        return LIBUSB_ERROR_IO;
    struct usbip_devinfo imp;
    if (usbip_client_import(fd, dev->info.busid, &imp) != USB_SUCCESS)
    {
        sock_close(fd);
        return LIBUSB_ERROR_NO_DEVICE;
    }
    uint32_t devid = (imp.busnum << 16) | imp.devnum, seq = 0;

    uint8_t hdr[9];
    int act = 0;
    int rc = ctrl_raw(fd, devid, &seq, NULL, 0x80, LIBUSB_REQUEST_GET_DESCRIPTOR,
                      (LIBUSB_DT_CONFIG << 8) | 0, 0, hdr, sizeof(hdr), &act);
    if (rc < 0 || act < 4)
    {
        sock_close(fd);
        return LIBUSB_ERROR_IO;
    }
    int total = usb_get_le16(hdr + 2);
    if (total < 9)
        total = 9;
    uint8_t *buf = malloc((size_t)total);
    if (!buf)
    {
        sock_close(fd);
        return LIBUSB_ERROR_NO_MEM;
    }
    rc = ctrl_raw(fd, devid, &seq, NULL, 0x80, LIBUSB_REQUEST_GET_DESCRIPTOR,
                  (LIBUSB_DT_CONFIG << 8) | 0, 0, buf, (uint16_t)total, &act);
    sock_close(fd);
    if (rc < 0 || act < 9)
    {
        free(buf);
        return LIBUSB_ERROR_IO;
    }
    dev->cfg_raw = buf;
    dev->cfg_len = act;
    return LIBUSB_SUCCESS;
}

/* --- config-blob -> nested libusb tree -------------------------------- */
struct tmp_alt
{
    struct libusb_interface_descriptor d;
    struct libusb_endpoint_descriptor *eps;
    int neps, epcap;
    uint8_t *extra;
    int extra_len, extra_cap;
};

static void tmp_extra_push(uint8_t **buf, int *len, int *cap, const uint8_t *data, int add_len)
{
    if (*len + add_len > *cap)
    {
        int nc = (*cap ? *cap * 2 : 64);
        while (nc < *len + add_len)
            nc *= 2;
        uint8_t *nb = realloc(*buf, (size_t)nc);
        if (!nb)
            return;
        *buf = nb;
        *cap = nc;
    }
    memcpy(*buf + *len, data, (size_t)add_len);
    *len += add_len;
}

static struct libusb_config_descriptor *parse_config(const uint8_t *buf, int len)
{
    if (len < 9 || buf[1] != LIBUSB_DT_CONFIG)
        return NULL;
    struct libusb_config_descriptor *cfg = calloc(1, sizeof(*cfg));
    if (!cfg)
        return NULL;
    cfg->bLength = buf[0];
    cfg->bDescriptorType = buf[1];
    cfg->wTotalLength = usb_get_le16(buf + 2);
    cfg->bNumInterfaces = buf[4];
    cfg->bConfigurationValue = buf[5];
    cfg->iConfiguration = buf[6];
    cfg->bmAttributes = buf[7];
    cfg->MaxPower = buf[8];

    struct tmp_alt *alts = NULL;
    int nalt = 0, acap = 0;
    struct tmp_alt *cur = NULL;
    uint8_t *cfg_extra = NULL;
    int cfg_extra_len = 0, cfg_extra_cap = 0;

    int pos = buf[0] ? buf[0] : 9; /* skip the config header */
    while (pos + 2 <= len)
    {
        int blen = buf[pos], btype = buf[pos + 1];
        if (blen == 0 || pos + blen > len)
            break;
        if (btype == LIBUSB_DT_INTERFACE && blen >= 9)
        {
            if (nalt == acap)
            {
                acap = acap ? acap * 2 : 4;
                alts = realloc(alts, (size_t)acap * sizeof(*alts));
            }
            cur = &alts[nalt++];
            memset(cur, 0, sizeof(*cur));
            cur->d.bLength = buf[pos];
            cur->d.bDescriptorType = buf[pos + 1];
            cur->d.bInterfaceNumber = buf[pos + 2];
            cur->d.bAlternateSetting = buf[pos + 3];
            cur->d.bNumEndpoints = buf[pos + 4];
            cur->d.bInterfaceClass = buf[pos + 5];
            cur->d.bInterfaceSubClass = buf[pos + 6];
            cur->d.bInterfaceProtocol = buf[pos + 7];
            cur->d.iInterface = buf[pos + 8];
        }
        else if (btype == LIBUSB_DT_ENDPOINT && blen >= 7 && cur)
        {
            if (cur->neps == cur->epcap)
            {
                cur->epcap = cur->epcap ? cur->epcap * 2 : 4;
                cur->eps = realloc(cur->eps, (size_t)cur->epcap * sizeof(*cur->eps));
            }
            struct libusb_endpoint_descriptor *ep = &cur->eps[cur->neps++];
            memset(ep, 0, sizeof(*ep));
            ep->bLength = buf[pos];
            ep->bDescriptorType = buf[pos + 1];
            ep->bEndpointAddress = buf[pos + 2];
            ep->bmAttributes = buf[pos + 3];
            ep->wMaxPacketSize = usb_get_le16(buf + pos + 4);
            ep->bInterval = buf[pos + 6];
            if (blen >= 9)
            {
                ep->bRefresh = buf[pos + 7];
                ep->bSynchAddress = buf[pos + 8];
            }
        }
        else
        { /* class/vendor descriptor -> "extra" */
            if (cur)
                tmp_extra_push(&cur->extra, &cur->extra_len, &cur->extra_cap, buf + pos, blen);
            else
                tmp_extra_push(&cfg_extra, &cfg_extra_len, &cfg_extra_cap, buf + pos, blen);
        }
        pos += blen;
    }

    /* group altsettings by bInterfaceNumber (first-appearance order) */
    int nbuckets = 0;
    uint8_t bucket_num[64];
    int *bucket_of = calloc((size_t)(nalt ? nalt : 1), sizeof(*bucket_of)); /* alt index -> bucket */
    for (int i = 0; i < nalt; i++)
    {
        int bucket = -1;
        for (int k = 0; k < nbuckets; k++)
            if (bucket_num[k] == alts[i].d.bInterfaceNumber)
            {
                bucket = k;
                break;
            }
        if (bucket < 0 && nbuckets < 64)
        {
            bucket = nbuckets;
            bucket_num[nbuckets++] = alts[i].d.bInterfaceNumber;
        }
        bucket_of[i] = bucket;
    }

    struct libusb_interface *ifaces = calloc((size_t)(nbuckets ? nbuckets : 1), sizeof(*ifaces));
    for (int k = 0; k < nbuckets; k++)
    {
        int cnt = 0;
        for (int i = 0; i < nalt; i++)
            if (bucket_of[i] == k)
                cnt++;
        struct libusb_interface_descriptor *as = calloc((size_t)cnt, sizeof(*as));
        int ai = 0;
        for (int i = 0; i < nalt; i++)
        {
            if (bucket_of[i] != k)
                continue;
            struct tmp_alt *alt = &alts[i];
            struct libusb_interface_descriptor *dst = &as[ai++];
            *dst = alt->d;
            if (alt->neps)
            {
                struct libusb_endpoint_descriptor *eps = calloc((size_t)alt->neps, sizeof(*eps));
                memcpy(eps, alt->eps, (size_t)alt->neps * sizeof(*eps));
                dst->endpoint = eps;
            }
            dst->bNumEndpoints = (uint8_t)alt->neps;
            if (alt->extra_len)
            {
                dst->extra = alt->extra;
                dst->extra_length = alt->extra_len;
                alt->extra = NULL;
            }
        }
        ((struct libusb_interface *)&ifaces[k])->altsetting = as;
        ((struct libusb_interface *)&ifaces[k])->num_altsetting = cnt;
    }

    cfg->interface = ifaces;
    cfg->bNumInterfaces = (uint8_t)nbuckets; /* keep array length and field in sync */
    if (cfg_extra_len)
    {
        cfg->extra = cfg_extra;
        cfg->extra_length = cfg_extra_len;
    }

    for (int i = 0; i < nalt; i++)
    {
        free(alts[i].eps);
        free(alts[i].extra);
    }
    free(alts);
    free(bucket_of);
    return cfg;
}

static int build_config(libusb_device *dev, struct libusb_config_descriptor **out)
{
    int rc = fetch_config_blob(dev);
    if (rc < 0)
        return rc;
    struct libusb_config_descriptor *cfg = parse_config(dev->cfg_raw, dev->cfg_len);
    if (!cfg)
        return LIBUSB_ERROR_IO;
    *out = cfg;
    return LIBUSB_SUCCESS;
}

int LIBUSB_CALL libusb_get_active_config_descriptor(libusb_device *dev, struct libusb_config_descriptor **config)
{
    if (!dev || !config)
        return LIBUSB_ERROR_INVALID_PARAM;
    return build_config(dev, config);
}

int LIBUSB_CALL libusb_get_config_descriptor(libusb_device *dev, uint8_t index, struct libusb_config_descriptor **config)
{
    if (!dev || !config)
        return LIBUSB_ERROR_INVALID_PARAM;
    if (index != 0)
        return LIBUSB_ERROR_NOT_FOUND; /* single-config devices */
    return build_config(dev, config);
}

int LIBUSB_CALL libusb_get_config_descriptor_by_value(libusb_device *dev, uint8_t value, struct libusb_config_descriptor **config)
{
    if (!dev || !config)
        return LIBUSB_ERROR_INVALID_PARAM;
    struct libusb_config_descriptor *cfg;
    int rc = build_config(dev, &cfg);
    if (rc < 0)
        return rc;
    if (cfg->bConfigurationValue != value)
    {
        libusb_free_config_descriptor(cfg);
        return LIBUSB_ERROR_NOT_FOUND;
    }
    *config = cfg;
    return LIBUSB_SUCCESS;
}

void LIBUSB_CALL libusb_free_config_descriptor(struct libusb_config_descriptor *cfg)
{
    if (!cfg)
        return;
    for (int k = 0; k < cfg->bNumInterfaces; k++)
    {
        const struct libusb_interface *itf = &cfg->interface[k];
        for (int alt = 0; alt < itf->num_altsetting; alt++)
        {
            free((void *)itf->altsetting[alt].endpoint);
            free((void *)itf->altsetting[alt].extra);
        }
        free((void *)itf->altsetting);
    }
    free((void *)cfg->interface);
    free((void *)cfg->extra);
    free(cfg);
}

int LIBUSB_CALL libusb_get_max_packet_size(libusb_device *dev, unsigned char endpoint)
{
    struct libusb_config_descriptor *cfg;
    if (build_config(dev, &cfg) < 0)
        return LIBUSB_ERROR_OTHER;
    int result = LIBUSB_ERROR_NOT_FOUND;
    for (int k = 0; k < cfg->bNumInterfaces && result < 0; k++)
        for (int alt = 0; alt < cfg->interface[k].num_altsetting && result < 0; alt++)
        {
            const struct libusb_interface_descriptor *id = &cfg->interface[k].altsetting[alt];
            for (int ep_idx = 0; ep_idx < id->bNumEndpoints; ep_idx++)
                if (id->endpoint[ep_idx].bEndpointAddress == endpoint)
                {
                    result = id->endpoint[ep_idx].wMaxPacketSize;
                    break;
                }
        }
    libusb_free_config_descriptor(cfg);
    return result;
}

int LIBUSB_CALL libusb_get_max_iso_packet_size(libusb_device *dev, unsigned char endpoint)
{
    int mps = libusb_get_max_packet_size(dev, endpoint);
    if (mps < 0)
        return mps;
    int mult = ((mps >> 11) & 3) + 1; /* high-bandwidth multiplier */
    return (mps & 0x7ff) * mult;
}

int LIBUSB_CALL libusb_get_string_descriptor_ascii(libusb_device_handle *handle, uint8_t index, unsigned char *data, int length)
{
    if (!handle || !data || length < 1)
        return LIBUSB_ERROR_INVALID_PARAM;
    uint8_t buf[256];
    int actual = 0;
    int rc = ctrl_io(handle, 0x80, LIBUSB_REQUEST_GET_DESCRIPTOR,
                     (LIBUSB_DT_STRING << 8) | index, 0x0409, buf, sizeof(buf), &actual);
    if (rc < 0)
        return rc;
    if (actual < 2 || buf[1] != LIBUSB_DT_STRING)
        return LIBUSB_ERROR_IO;
    int count = buf[0] < actual ? buf[0] : actual;
    int di = 0;
    for (int i = 2; i + 1 < count && di < length - 1; i += 2)
        data[di++] = buf[i + 1] ? '?' : buf[i]; /* UTF-16LE -> ASCII (non-ASCII -> '?') */
    data[di] = 0;
    return di;
}

/* ===================================================================== */
/* synchronous transfers                                                 */
/* ===================================================================== */
int LIBUSB_CALL libusb_control_transfer(libusb_device_handle *handle, uint8_t request_type, uint8_t bRequest,
                            uint16_t wValue, uint16_t wIndex, unsigned char *data,
                            uint16_t wLength, unsigned int timeout)
{
    (void)timeout;
    if (!handle)
        return LIBUSB_ERROR_NO_DEVICE;
    int actual = 0;
    int rc = ctrl_io(handle, request_type, bRequest, wValue, wIndex, data, wLength, &actual);
    if (rc < 0)
        return rc;
    return (request_type & LIBUSB_ENDPOINT_IN) ? actual : (int)wLength;
}

static int data_xfer(libusb_device_handle *handle, unsigned char endpoint, unsigned char *data,
                     int length, int *transferred, int interval)
{
    if (!handle)
        return LIBUSB_ERROR_NO_DEVICE;
    int dir = (endpoint & 0x80) ? USB_IN : USB_OUT;
    int actual = 0;
    pthread_mutex_lock(&handle->iolock);
    int rc = usbip_client_submit(handle->fd, handle->devid, &handle->seq, dir, endpoint & 0x0f,
                                 NULL, interval, data, length, &actual);
    pthread_mutex_unlock(&handle->iolock);
    if (transferred)
        *transferred = actual;
    return rc < 0 ? rc : LIBUSB_SUCCESS;
}

int LIBUSB_CALL libusb_bulk_transfer(libusb_device_handle *handle, unsigned char endpoint, unsigned char *data,
                         int length, int *transferred, unsigned int timeout)
{
    (void)timeout;
    return data_xfer(handle, endpoint, data, length, transferred, 0);
}

int LIBUSB_CALL libusb_interrupt_transfer(libusb_device_handle *handle, unsigned char endpoint, unsigned char *data,
                              int length, int *transferred, unsigned int timeout)
{
    (void)timeout;
    return data_xfer(handle, endpoint, data, length, transferred, 1);
}

/* ===================================================================== */
/* asynchronous I/O (worker thread over the synchronous primitives)      */
/* ===================================================================== */
struct libusb_transfer *LIBUSB_CALL libusb_alloc_transfer(int iso_packets)
{
    size_t sz = sizeof(struct itransfer) + sizeof(struct libusb_transfer) + (size_t)iso_packets * sizeof(struct libusb_iso_packet_descriptor);
    struct itransfer *it = calloc(1, sz);
    if (!it)
        return NULL;
    struct libusb_transfer *pub = PUB_OF(it);
    pub->num_iso_packets = iso_packets;
    return pub;
}

static void free_transfer_internal(struct libusb_transfer *pub)
{
    if (!pub)
        return;
    if ((pub->flags & LIBUSB_TRANSFER_FREE_BUFFER) && pub->buffer)
        free(pub->buffer);
    free(IT_OF(pub));
}

void LIBUSB_CALL libusb_free_transfer(struct libusb_transfer *pub) { free_transfer_internal(pub); }

static void run_transfer(struct libusb_transfer *pub)
{
    struct libusb_device_handle *handle = pub->dev_handle;
    if (!handle)
    {
        pub->status = LIBUSB_TRANSFER_ERROR;
        pub->actual_length = 0;
        return;
    }

    if (pub->type == LIBUSB_TRANSFER_TYPE_CONTROL)
    {
        uint8_t *setup_bytes = pub->buffer;
        uint16_t wValue = usb_get_le16(setup_bytes + 2);
        uint16_t wIndex = usb_get_le16(setup_bytes + 4);
        uint16_t wLen = usb_get_le16(setup_bytes + 6);
        int actual = 0;
        int rc = ctrl_io(handle, setup_bytes[0], setup_bytes[1], wValue, wIndex, setup_bytes + 8, wLen, &actual);
        pub->actual_length = rc < 0 ? 0 : actual;
        pub->status = map_status(rc);
    }
    else if (pub->type == LIBUSB_TRANSFER_TYPE_BULK ||
             pub->type == LIBUSB_TRANSFER_TYPE_INTERRUPT)
    {
        int dir = (pub->endpoint & 0x80) ? USB_IN : USB_OUT;
        int interval = (pub->type == LIBUSB_TRANSFER_TYPE_INTERRUPT) ? 1 : 0;
        int actual = 0;
        pthread_mutex_lock(&handle->iolock);
        int rc = usbip_client_submit(handle->fd, handle->devid, &handle->seq, dir, pub->endpoint & 0x0f,
                                     NULL, interval, pub->buffer, pub->length, &actual);
        pthread_mutex_unlock(&handle->iolock);
        pub->actual_length = actual;
        pub->status = map_status(rc);
    }
    else if (pub->type == LIBUSB_TRANSFER_TYPE_ISOCHRONOUS)
    {
        int np = pub->num_iso_packets;
        struct iso_pkt *pkts = calloc((size_t)np, sizeof(*pkts));
        if (!pkts)
        {
            pub->status = LIBUSB_TRANSFER_ERROR;
            return;
        }
        uint32_t off = 0;
        for (int i = 0; i < np; i++)
        {
            pkts[i].offset = off;
            pkts[i].length = pub->iso_packet_desc[i].length;
            off += pkts[i].length;
        }
        int dir = (pub->endpoint & 0x80) ? USB_IN : USB_OUT;
        int total = 0;
        pthread_mutex_lock(&handle->iolock);
        int rc = usbip_client_submit_iso(handle->fd, handle->devid, &handle->seq, dir, pub->endpoint & 0x0f,
                                         0, pub->buffer, pkts, np, &total);
        pthread_mutex_unlock(&handle->iolock);
        for (int i = 0; i < np; i++)
        {
            pub->iso_packet_desc[i].actual_length = pkts[i].actual_length;
            pub->iso_packet_desc[i].status = pkts[i].status ? LIBUSB_TRANSFER_ERROR : LIBUSB_TRANSFER_COMPLETED;
        }
        free(pkts);
        pub->actual_length = total;
        pub->status = rc < 0 ? map_status(rc) : LIBUSB_TRANSFER_COMPLETED;
    }
    else
    {
        pub->status = LIBUSB_TRANSFER_ERROR;
    }
}

static void *worker_main(void *arg)
{
    struct libusb_context *ctx = arg;
    for (;;)
    {
        pthread_mutex_lock(&ctx->lock);
        while (!ctx->subq_head && !ctx->shutting)
            pthread_cond_wait(&ctx->qcond, &ctx->lock);
        if (ctx->shutting && !ctx->subq_head)
        {
            pthread_mutex_unlock(&ctx->lock);
            break;
        }
        struct itransfer *it = ctx->subq_head;
        ctx->subq_head = it->next;
        if (!ctx->subq_head)
            ctx->subq_tail = NULL;
        it->state = ST_INFLIGHT;
        int cancelled = it->cancelled;
        pthread_mutex_unlock(&ctx->lock);

        struct libusb_transfer *pub = PUB_OF(it);
        if (cancelled)
        {
            pub->status = LIBUSB_TRANSFER_CANCELLED;
            pub->actual_length = 0;
        }
        else
            run_transfer(pub);

        pthread_mutex_lock(&ctx->lock);
        it->next = NULL;
        if (ctx->doneq_tail)
            ctx->doneq_tail->next = it;
        else
            ctx->doneq_head = it;
        ctx->doneq_tail = it;
        pthread_mutex_unlock(&ctx->lock);
        uint8_t one = 1;
#ifdef _WIN32
        int written = send((SOCKET)ctx->pipe_w, (const char *)&one, 1, 0);
        (void)written;
#else
        ssize_t written = write(ctx->pipe_w, &one, 1);
        (void)written;
#endif
    }
    return NULL;
}

int LIBUSB_CALL libusb_submit_transfer(struct libusb_transfer *pub)
{
    if (!pub || !pub->dev_handle)
        return LIBUSB_ERROR_INVALID_PARAM;
    struct itransfer *it = IT_OF(pub);
    struct libusb_context *ctx = pub->dev_handle->dev->ctx;
    it->ctx = ctx;
    it->cancelled = 0;
    pub->status = LIBUSB_TRANSFER_COMPLETED;

    pthread_mutex_lock(&ctx->lock);
    if (!ctx->worker_started)
    {
        if (pthread_create(&ctx->worker, NULL, worker_main, ctx) != 0)
        {
            pthread_mutex_unlock(&ctx->lock);
            return LIBUSB_ERROR_OTHER;
        }
        ctx->worker_started = 1;
    }
    it->state = ST_QUEUED;
    it->next = NULL;
    if (ctx->subq_tail)
        ctx->subq_tail->next = it;
    else
        ctx->subq_head = it;
    ctx->subq_tail = it;
    pthread_cond_signal(&ctx->qcond);
    pthread_mutex_unlock(&ctx->lock);
    return LIBUSB_SUCCESS;
}

int LIBUSB_CALL libusb_cancel_transfer(struct libusb_transfer *pub)
{
    if (!pub)
        return LIBUSB_ERROR_INVALID_PARAM;
    struct itransfer *it = IT_OF(pub);
    struct libusb_context *ctx = it->ctx;
    if (!ctx)
        return LIBUSB_ERROR_NOT_FOUND;
    pthread_mutex_lock(&ctx->lock);
    int rc = (it->state == ST_DONE) ? LIBUSB_ERROR_NOT_FOUND : LIBUSB_SUCCESS;
    if (rc == LIBUSB_SUCCESS)
    {
        it->cancelled = 1;
        pthread_cond_signal(&ctx->qcond);
    }
    pthread_mutex_unlock(&ctx->lock);
    return rc; /* queued -> completes CANCELLED; in-flight -> best-effort (documented) */
}

unsigned char *LIBUSB_CALL libusb_get_iso_packet_buffer_simple(struct libusb_transfer *pub, unsigned int packet)
{
    if ((int)packet >= pub->num_iso_packets)
        return NULL;
    if (pub->num_iso_packets == 0)
        return NULL;
    return pub->buffer + packet * (unsigned int)(pub->length / pub->num_iso_packets);
}

unsigned char *LIBUSB_CALL libusb_get_iso_packet_buffer(struct libusb_transfer *pub, unsigned int packet)
{
    if ((int)packet >= pub->num_iso_packets)
        return NULL;
    unsigned int off = 0;
    for (unsigned int i = 0; i < packet; i++)
        off += pub->iso_packet_desc[i].length;
    return pub->buffer + off;
}

/* ===================================================================== */
/* event handling                                                        */
/* ===================================================================== */
static int handle_events(struct libusb_context *ctx, struct timeval *tv, int *completed)
{
    ctx = resolve_ctx(ctx);
    if (!ctx)
        return LIBUSB_ERROR_INVALID_PARAM;

    int timeout_ms = tv ? (int)(tv->tv_sec * 1000 + tv->tv_usec / 1000) : 60000;
    if (completed && *completed)
        timeout_ms = 0;

    if (ctx->pipe_r >= 0)
    {
#ifdef _WIN32
        WSAPOLLFD pfd = {(SOCKET)ctx->pipe_r, POLLRDNORM, 0};
        WSAPoll(&pfd, 1, timeout_ms);
        char tmp[64];
        while (recv((SOCKET)ctx->pipe_r, tmp, sizeof(tmp), 0) > 0)
        {
        } /* drain wakeups */
#else
        struct pollfd pfd = {ctx->pipe_r, POLLIN, 0};
        poll(&pfd, 1, timeout_ms);
        uint8_t tmp[64];
        while (read(ctx->pipe_r, tmp, sizeof(tmp)) > 0)
        {
        } /* drain wakeups */
#endif
    }

    pthread_mutex_lock(&ctx->lock);
    struct itransfer *list = ctx->doneq_head;
    ctx->doneq_head = ctx->doneq_tail = NULL;
    pthread_mutex_unlock(&ctx->lock);

    while (list)
    {
        struct itransfer *it = list;
        list = it->next;
        struct libusb_transfer *pub = PUB_OF(it);
        it->state = ST_DONE;
        if (pub->callback)
            pub->callback(pub);
        if (pub->flags & LIBUSB_TRANSFER_FREE_TRANSFER)
            free_transfer_internal(pub);
    }
    return LIBUSB_SUCCESS;
}

int LIBUSB_CALL libusb_handle_events(libusb_context *ctx)
{
    return handle_events(ctx, NULL, NULL);
}

int LIBUSB_CALL libusb_handle_events_completed(libusb_context *ctx, int *completed)
{
    return handle_events(ctx, NULL, completed);
}

int LIBUSB_CALL libusb_handle_events_timeout(libusb_context *ctx, struct timeval *tv)
{
    return handle_events(ctx, tv, NULL);
}

int LIBUSB_CALL libusb_handle_events_timeout_completed(libusb_context *ctx, struct timeval *tv, int *completed)
{
    return handle_events(ctx, tv, completed);
}

int LIBUSB_CALL libusb_handle_events_locked(libusb_context *ctx, struct timeval *tv)
{
    return handle_events(ctx, tv, NULL);
}

int LIBUSB_CALL libusb_get_next_timeout(libusb_context *ctx, struct timeval *tv)
{
    (void)ctx;
    (void)tv;
    return 0;
}

void LIBUSB_CALL libusb_lock_events(libusb_context *ctx)
{
    ctx = resolve_ctx(ctx);
    if (ctx)
        pthread_mutex_lock(&ctx->evlock);
}

void LIBUSB_CALL libusb_unlock_events(libusb_context *ctx)
{
    ctx = resolve_ctx(ctx);
    if (ctx)
        pthread_mutex_unlock(&ctx->evlock);
}

int LIBUSB_CALL libusb_event_handling_ok(libusb_context *ctx)
{
    (void)ctx;
    return 1;
}

int LIBUSB_CALL libusb_event_handler_active(libusb_context *ctx)
{
    (void)ctx;
    return 0;
}

int LIBUSB_CALL libusb_try_lock_events(libusb_context *ctx)
{
    ctx = resolve_ctx(ctx);
    return (ctx && pthread_mutex_trylock(&ctx->evlock) == 0) ? 0 : 1;
}

void LIBUSB_CALL libusb_lock_event_waiters(libusb_context *ctx)
{
    (void)ctx;
}

void LIBUSB_CALL libusb_unlock_event_waiters(libusb_context *ctx)
{
    (void)ctx;
}

int LIBUSB_CALL libusb_wait_for_event(libusb_context *ctx, struct timeval *tv)
{
    return handle_events(ctx, tv, NULL);
}

int LIBUSB_CALL libusb_pollfds_handle_timeouts(libusb_context *ctx)
{
    (void)ctx;
    return 1;
}

const struct libusb_pollfd **LIBUSB_CALL libusb_get_pollfds(libusb_context *ctx)
{
    ctx = resolve_ctx(ctx);
    if (!ctx)
        return NULL;
    const struct libusb_pollfd **arr = calloc(2, sizeof(*arr));
    if (!arr)
        return NULL;
    struct libusb_pollfd *pollfd = calloc(1, sizeof(*pollfd));
    if (!pollfd)
    {
        free(arr);
        return NULL;
    }
    pollfd->fd = ctx->pipe_r;
    pollfd->events = POLLIN;
    arr[0] = pollfd;
    arr[1] = NULL;
    return arr;
}

void LIBUSB_CALL libusb_free_pollfds(const struct libusb_pollfd **pollfds)
{
    if (!pollfds)
        return;
    for (int i = 0; pollfds[i]; i++)
        free((void *)pollfds[i]);
    free((void *)pollfds);
}

void LIBUSB_CALL libusb_set_pollfd_notifiers(libusb_context *ctx, libusb_pollfd_added_cb added_cb, libusb_pollfd_removed_cb removed_cb, void *user_data)
{
    ctx = resolve_ctx(ctx);
    if (!ctx)
        return;
    ctx->pfd_added = added_cb;
    ctx->pfd_removed = removed_cb;
    ctx->pfd_user = user_data;
}

/* ===================================================================== */
/* hotplug (stubbed; libusb_has_capability(HAS_HOTPLUG) reports 0)       */
/* ===================================================================== */
int LIBUSB_CALL libusb_hotplug_register_callback(libusb_context *ctx, int events, int flags,
                                     int vendor_id, int product_id, int dev_class,
                                     libusb_hotplug_callback_fn cb_fn, void *user_data,
                                     libusb_hotplug_callback_handle *handle)
{
    (void)ctx;
    (void)events;
    (void)flags;
    (void)vendor_id;
    (void)product_id;
    (void)dev_class;
    (void)cb_fn;
    (void)user_data;
    if (handle)
        *handle = 0;
    return LIBUSB_ERROR_NOT_SUPPORTED;
}

void LIBUSB_CALL libusb_hotplug_deregister_callback(libusb_context *ctx, libusb_hotplug_callback_handle handle)
{
    (void)ctx;
    (void)handle;
}
