/**
 * Copyright (C) 2026 Jabez Winston
 *
 * SPDX-License-Identifier: MIT
 *
 * Date : 14-June-2026
 *
 * libusbk.c - a drop-in libusbK.dll implemented on the USBIP C library's low-level USB/IP
 * client primitives (usbip_connect / usbip_client_devlist / usbip_client_import /
 * usbip_client_submit[_iso], declared in usbip_device_internal.h). NOT built on usbip_host_*.
 *
 * A Windows program compiled against the real <libusbk.h> can load this DLL and
 * drive a usbip virtual device over USB/IP - no kernel driver, no libusbK.sys.
 * The USB/IP server is chosen by env USBIP_HOST / USBIP_PORT (default
 * 127.0.0.1:3240). Exported symbols are the libusbK API (UsbK_ / LstK_ / OvlK_ /
 * LibK_ / IsoK_ / IsochK_ / StmK_ / HotK_), undecorated via libusbK.def.
 *
 * The USBIP primitives are synchronous; the libusbK overlapped (async) model is
 * layered on top with one worker thread per UsbK handle that runs the blocking
 * submit and signals the OVERLAPPED's event, exactly as the kernel driver would.
 *
 * Windows-only: libusbK is a Windows API (HANDLE/OVERLAPPED/__stdcall). winsock2
 * must precede windows.h, so os_compat.h is included first.
 */
#include "os_compat.h"             /* winsock2 (before windows.h) + winpthreads + sock_close */
#include "usbip_device_internal.h" /* usbip_* client primitives, usbip_devinfo, iso_pkt, USB_* */
#include "libusbk.h"               /* windows.h + the libusbK API */
#undef interface                   /* windows.h defines `interface`; harmless to drop here */

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#ifndef STATUS_PENDING
#define STATUS_PENDING ((ULONG_PTR)0x00000103)
#endif
#define LK_STATUS_FAIL ((ULONG_PTR)0xC0000001) /* generic non-zero NTSTATUS-ish */

/* Fail a BOOL API entry: set the Win32 error and return FALSE. Every parameter
 * guard in the file is `if (bad) LK_FAIL(code);`. */
#define LK_FAIL(err)       \
    do                     \
    {                      \
        SetLastError(err); \
        return FALSE;      \
    } while (0)


/* ===================================================================== */
/* internal handle types                                                 */
/* ===================================================================== */
/* Common prefix for every "object" handle EXCEPT KOVL (which must start with an
 * OVERLAPPED so apps can cast a KOVL_HANDLE straight to LPOVERLAPPED). */
struct lk_common
{
    KLIB_HANDLE_TYPE type;
    KLIB_USER_CONTEXT user_ctx;
    KLIB_HANDLE_CLEANUP_CB *cleanup;
};

struct pipe_pol
{ /* WinUSB-style per-pipe policy store */
    UINT transfer_timeout;
    UCHAR short_packet_terminate;
    UCHAR auto_clear_stall;
    UCHAR ignore_short_packets;
    UCHAR allow_partial_reads;
    UCHAR auto_flush;
    UCHAR raw_io;
    UCHAR reset_pipe_on_resume;
    UINT max_transfer_size;
};

struct aop; /* async op, queued to the worker */

struct kusb
{
    struct lk_common base; /* KLIB_HANDLE_TYPE_USBK */
    char busid[32];
    usb_transport *t; /* owns its transport (from env) */
    int fd;
    uint32_t devid;
    uint32_t seq;
    pthread_mutex_t iolock; /* serialize socket I/O (sync calls + worker) */
    struct usbip_devinfo info;
    uint8_t *cfg_raw; /* cached configuration blob */
    int cfg_len;
    uint8_t cur_config;
    uint8_t cur_iface;       /* selected interface number */
    uint8_t cur_alt[256];    /* alt setting per interface number */
    struct pipe_pol pol[32]; /* index = (id & 0x0f) | (id & 0x80 ? 0x10 : 0) */
    UINT power_auto_suspend;
    UINT power_suspend_delay;
    /* per-handle async worker */
    pthread_t worker;
    int worker_started;
    int shutting;
    pthread_mutex_t qlock;
    pthread_cond_t qcond;
    struct aop *q_head, *q_tail;
};

struct aop
{
    struct aop *next;
    struct kusb *h;
    int is_iso;
    int dir; /* USB_IN / USB_OUT */
    int ep;  /* endpoint number (0..15) */
    int interval;
    int has_setup;
    uint8_t setup[8];
    uint8_t *buf;
    int len;
    struct iso_pkt *pkts; /* iso: descriptor array (freed by worker) */
    int npkts;
    PKISO_CONTEXT isoctx; /* iso: copy per-packet results back here */
    LPOVERLAPPED ovl;     /* results land in ovl->Internal/InternalHigh */
};

/* A KLST_DEVINFO_HANDLE points at the PUBLIC KLST_DEVINFO the app reads, so alone
 * among the handles it cannot carry an lk_common prefix -- that would land on
 * Common.Pid/MI. The user context and cleanup callback sit behind the public struct
 * instead, as upstream's private KLST_DEVINFO_EL does. */
struct klst_el
{
    KLST_DEVINFO pub; /* first: &el->pub IS the handle */
    KLIB_USER_CONTEXT user_ctx;
    KLIB_HANDLE_CLEANUP_CB *cleanup;
    struct klst *owner; /* NULL while detached/cloned (LstK_FreeInfo's to free) */
    struct klst_el *next;
};

/* singly linked, not an array: LstK_AttachInfo/LstK_DetachInfo move elements between
 * lists while the application still holds KLST_DEVINFO_HANDLEs into them, so the
 * elements must keep their addresses */
struct klst
{
    struct lk_common base; /* KLIB_HANDLE_TYPE_LSTK */
    struct klst_el *head;
    struct klst_el *tail;
    struct klst_el *cursor; /* NULL: before first (started==0) or past end */
    int started;            /* MoveNext has been called since the last MoveReset */
    int count;
};

struct kovl_pool
{
    struct lk_common base; /* KLIB_HANDLE_TYPE_OVLPOOLK */
    struct kusb *h;
    int max;
    pthread_mutex_t lock;
    struct kovl *free_list;
    struct kovl *all; /* singly-linked for Free */
};

struct kovl
{
    OVERLAPPED ovl; /* MUST be first: KOVL_HANDLE casts to LPOVERLAPPED */
    KLIB_USER_CONTEXT user_ctx;
    struct kovl_pool *pool;
    struct kovl *next_free;
    struct kovl *next_all;
};

struct kisoch
{
    struct lk_common base; /* KLIB_HANDLE_TYPE_ISOCHK */
    struct kusb *h;
    UCHAR pipe_id;
    uint8_t *buffer;
    UINT buffer_size;
    UINT max_packets;
    UINT num_packets;
    UINT *offset; /* [max_packets] */
    UINT *length; /* [max_packets] */
    UINT *status; /* [max_packets] */
};

struct kstm
{
    struct lk_common base; /* KLIB_HANDLE_TYPE_STMK */
    KSTM_INFO info;
    KSTM_CALLBACK cb;
    int have_cb;
    KSTM_FLAG flags;
    int dir; /* USB_IN / USB_OUT */
    int max_xfer;
    int n_slots;
    pthread_t thread;
    int running;
    int stop;
    pthread_mutex_t lock;
    pthread_cond_t cond;
    KSTM_XFER_CONTEXT *slots; /* [n_slots] */
    uint8_t **slot_buf;       /* [n_slots] */
    /* two rings of slot indices (each capacity n_slots): free slots and
     * completed/queued slots. IN: thread reads free->ready, StmK_Read ready->free.
     * OUT: StmK_Write free->ready, thread writes ready->free. */
    int *free_q;
    int free_head, free_count;
    int *ready_q;
    int ready_head, ready_count;
};

/* ring helpers (caller holds s->lock) */
static void kstm_push(int *ring, int *head, int *count, int capacity, int value)
{
    ring[(*head + *count) % capacity] = value;
    (*count)++;
}
static int kstm_pop(int *ring, int *head, int *count, int capacity)
{
    int value = ring[*head];
    *head = (*head + 1) % capacity;
    (*count)--;
    return value;
}

/* default contexts, indexed by KLIB_HANDLE_TYPE */
static KLIB_USER_CONTEXT g_default_ctx[KLIB_HANDLE_TYPE_COUNT];
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;

/* tracked HotK handles for HotK_FreeAll */
static struct khot *g_hot_list;

struct khot
{
    struct lk_common base; /* KLIB_HANDLE_TYPE_HOTK */
    KHOT_PARAMS params;
    struct khot *next_all;
};

/* ===================================================================== */
/* small helpers                                                         */
/* ===================================================================== */
static usb_transport *lk_env_transport(void)
{
    const char *host = getenv("USBIP_HOST");
    const char *ports = getenv("USBIP_PORT");
    int port = ports ? atoi(ports) : 0; /* 0 -> usbip_transport defaults to 3240 */
    return usbip_transport(host, port); /* host NULL -> 127.0.0.1 */
}

static int lk_pol_index(UCHAR pipe_id)
{
    return (pipe_id & 0x0f) | ((pipe_id & 0x80) ? 0x10 : 0);
}

/* synchronous control/bulk/interrupt submit on an open handle. */
static int lk_submit(struct kusb *handle, int dir, int ep, const uint8_t setup[8],
                     int interval, uint8_t *buf, int len, int *actual)
{
    pthread_mutex_lock(&handle->iolock);
    int rc = usbip_client_submit(handle->fd, handle->devid, &handle->seq, dir, ep, setup, interval, buf, len, actual);
    pthread_mutex_unlock(&handle->iolock);
    return rc;
}

static int lk_control(struct kusb *handle, uint8_t bmReq, uint8_t bReq, uint16_t wValue,
                      uint16_t wIndex, uint8_t *data, uint16_t wLen, int *actual)
{
    uint8_t setup[8] = {USB_SETUP_BYTES(bmReq, bReq, wValue, wIndex, wLen)};
    int dir = (bmReq & 0x80) ? USB_IN : USB_OUT;
    return lk_submit(handle, dir, 0, setup, 0, data, wLen, actual);
}

/* WinUSB's AUTO_CLEAR_STALL policy: a stalled transfer still fails, but the halt is
 * cleared so the NEXT one works. Without it the caller must call UsbK_ResetPipe by
 * hand, and a device that halts a pipe to abandon a transfer -- as MSC does on a
 * failed data phase -- wedges it for good. */
static void lk_auto_clear_stall(struct kusb *handle, UCHAR pipe_id, int rc)
{
    if (rc != USB_ERROR_PIPE || !handle->pol[lk_pol_index(pipe_id)].auto_clear_stall)
        return;
    lk_control(handle, 0x02, USB_REQ_CLEAR_FEATURE, USB_FEATURE_ENDPOINT_HALT, pipe_id, NULL, 0, NULL);
}

/* fetch + cache the configuration descriptor blob (control GET_DESCRIPTOR). */
static int lk_fetch_config(struct kusb *handle)
{
    if (handle->cfg_raw)
        return 0;
    uint8_t hdr[9];
    int act = 0;
    if (lk_control(handle, 0x80, USB_REQ_GET_DESCRIPTOR, (USB_DT_CONFIG << 8), 0, hdr, sizeof(hdr), &act) < 0 || act < 4)
        return -1;
    int total = usb_get_le16(hdr + 2);
    if (total < 9)
        total = 9;
    uint8_t *buf = malloc((size_t)total);
    if (!buf)
        return -1;
    if (lk_control(handle, 0x80, USB_REQ_GET_DESCRIPTOR, (USB_DT_CONFIG << 8), 0, buf, (uint16_t)total, &act) < 0 || act < 9)
    {
        free(buf);
        return -1;
    }
    handle->cfg_raw = buf;
    handle->cfg_len = act;
    if (act >= 6)
        handle->cur_config = buf[5]; /* bConfigurationValue */
    return 0;
}

/* descriptor-blob walkers (operate on the cached config blob) */

/* Is there a well-formed descriptor at offset i of blob b? The loop shape every
 * walker below shares: for (i = start; lk_desc_ok(b, len, i); i += b[i]). */
static int lk_desc_ok(const uint8_t *cfg, int len, int i)
{
    if (i + 2 > len)
        return 0;
    if (cfg[i] < 2 || i + cfg[i] > len)
        return 0;
    return 1;
}

static int lk_iface_off(struct kusb *handle, int ifnum, int alt)
{
    uint8_t *cfg = handle->cfg_raw;
    int len = handle->cfg_len;
    for (int i = 0; lk_desc_ok(cfg, len, i); i += cfg[i])
        if (cfg[i + 1] == USB_DT_INTERFACE && cfg[i + 2] == ifnum && cfg[i + 3] == alt)
            return i;
    return -1;
}

static int lk_ep_off(struct kusb *handle, int iface_off, int ep_index)
{
    uint8_t *cfg = handle->cfg_raw;
    int len = handle->cfg_len, count = 0;
    for (int i = iface_off + cfg[iface_off]; lk_desc_ok(cfg, len, i); i += cfg[i])
    {
        if (cfg[i + 1] == USB_DT_INTERFACE)
            break; /* next interface; stop */
        if (cfg[i + 1] == USB_DT_ENDPOINT)
        {
            if (count == ep_index)
                return i;
            count++;
        }
    }
    return -1;
}

static int lk_iface_number_by_index(struct kusb *handle, int index)
{
    uint8_t *cfg = handle->cfg_raw;
    int len = handle->cfg_len, count = 0, last = -1;
    for (int i = 0; lk_desc_ok(cfg, len, i); i += cfg[i])
    {
        if (cfg[i + 1] == USB_DT_INTERFACE && cfg[i + 3] == 0 && cfg[i + 2] != last)
        {
            if (count == index)
                return cfg[i + 2];
            count++;
            last = cfg[i + 2];
        }
    }
    return index; /* fallback: treat index as the interface number */
}

static int lk_pipe_interval(struct kusb *handle, UCHAR pipe_id)
{
    if (!handle->cfg_raw)
        return 0;
    uint8_t *cfg = handle->cfg_raw;
    int len = handle->cfg_len;
    for (int i = 0; lk_desc_ok(cfg, len, i); i += cfg[i])
        if (cfg[i + 1] == USB_DT_ENDPOINT && cfg[i + 2] == pipe_id)
            return (cfg[i + 3] & 0x03) == 0x03 ? 1 : 0; /* interrupt -> 1, else bulk */
    return 0;
}

/* simple file-style wildcard match (*, ?), case-insensitive - for HotK patterns. */
static int lk_wild(const char *pat, const char *str)
{
    if (!pat || !pat[0])
        return 1; /* empty pattern matches all */
    while (*pat)
    {
        if (*pat == '*')
        {
            pat++;
            if (!*pat)
                return 1;
            for (; *str; str++)
                if (lk_wild(pat, str))
                    return 1;
            return lk_wild(pat, str);
        }
        if (!*str)
            return 0;
        if (*pat != '?')
        {
            char pc = *pat, sc = *str;
            if (pc >= 'a' && pc <= 'z')
                pc = (char)(pc - 32);
            if (sc >= 'a' && sc <= 'z')
                sc = (char)(sc - 32);
            if (pc != sc)
                return 0;
        }
        pat++;
        str++;
    }
    return *str == 0;
}

static ULONG_PTR lk_fail_status(int rc)
{
    (void)rc;
    return LK_STATUS_FAIL;
}

/* ===================================================================== */
/* per-handle async worker                                               */
/* ===================================================================== */
static void *kusb_worker(void *arg)
{
    struct kusb *handle = arg;
    for (;;)
    {
        pthread_mutex_lock(&handle->qlock);
        while (!handle->q_head && !handle->shutting)
            pthread_cond_wait(&handle->qcond, &handle->qlock);
        if (handle->shutting && !handle->q_head)
        {
            pthread_mutex_unlock(&handle->qlock);
            break;
        }
        struct aop *op = handle->q_head;
        handle->q_head = op->next;
        if (!handle->q_head)
            handle->q_tail = NULL;
        pthread_mutex_unlock(&handle->qlock);

        int rc, actual = 0;
        if (op->is_iso)
        {
            int total = 0;
            pthread_mutex_lock(&handle->iolock);
            rc = usbip_client_submit_iso(handle->fd, handle->devid, &handle->seq, op->dir, op->ep, 0,
                                         op->buf, op->pkts, op->npkts, &total);
            pthread_mutex_unlock(&handle->iolock);
            if (op->isoctx)
            {
                for (int i = 0; i < op->npkts && i < op->isoctx->NumberOfPackets; i++)
                {
                    op->isoctx->IsoPackets[i].Length = (USHORT)op->pkts[i].actual_length;
                    op->isoctx->IsoPackets[i].Status = (USHORT)op->pkts[i].status;
                }
            }
            actual = total;
        }
        else
        {
            pthread_mutex_lock(&handle->iolock);
            rc = usbip_client_submit(handle->fd, handle->devid, &handle->seq, op->dir, op->ep,
                                     op->has_setup ? op->setup : NULL, op->interval,
                                     op->buf, op->len, &actual);
            pthread_mutex_unlock(&handle->iolock);
        }

        if (rc < 0 && !op->has_setup)
            lk_auto_clear_stall(handle, (UCHAR)(op->ep | (op->dir == USB_IN ? 0x80 : 0)), rc);

        LPOVERLAPPED ov = op->ovl;
        ov->InternalHigh = (ULONG_PTR)(rc < 0 ? 0 : actual);
        ov->Internal = (ULONG_PTR)(rc < 0 ? lk_fail_status(rc) : 0);
        if (ov->hEvent)
            SetEvent(ov->hEvent);

        free(op->pkts);
        free(op);
    }
    return NULL;
}

static BOOL kusb_worker_ensure(struct kusb *handle)
{
    BOOL ok = TRUE;
    pthread_mutex_lock(&handle->qlock);
    if (!handle->worker_started)
    {
        if (pthread_create(&handle->worker, NULL, kusb_worker, handle) == 0)
            handle->worker_started = 1;
        else
            ok = FALSE;
    }
    pthread_mutex_unlock(&handle->qlock);
    return ok;
}

/* Queue an async op; the overlapped completes from the worker. Returns the
 * FALSE + ERROR_IO_PENDING result an overlapped libusbK call must report. */
static BOOL kusb_queue(struct kusb *handle, struct aop *op, LPOVERLAPPED ovl)
{
    op->h = handle;
    op->ovl = ovl;
    ovl->Internal = STATUS_PENDING;
    ovl->InternalHigh = 0;
    if (ovl->hEvent)
        ResetEvent(ovl->hEvent);
    if (!kusb_worker_ensure(handle))
    {
        free(op->pkts);
        free(op);
        SetLastError(ERROR_OUTOFMEMORY);
        return FALSE;
    }
    pthread_mutex_lock(&handle->qlock);
    op->next = NULL;
    if (handle->q_tail)
        handle->q_tail->next = op;
    else
        handle->q_head = op;
    handle->q_tail = op;
    pthread_cond_signal(&handle->qcond);
    pthread_mutex_unlock(&handle->qlock);
    SetLastError(ERROR_IO_PENDING);
    return FALSE;
}

/* ===================================================================== */
/* LibK - version / context / driver-API table                           */
/* ===================================================================== */
KUSB_EXP VOID KUSB_API LibK_GetVersion(PKLIB_VERSION Version)
{
    if (!Version)
        return;
    Version->Major = 3;
    Version->Minor = 1;
    Version->Micro = 0;
    Version->Nano = 0;
}

static struct lk_common *lk_common_of(KLIB_HANDLE Handle, KLIB_HANDLE_TYPE Type)
{
    if (!Handle)
        return NULL;
    if (Type == KLIB_HANDLE_TYPE_OVLK || Type == KLIB_HANDLE_TYPE_LSTINFOK)
        return NULL; /* neither KOVL nor KLST_DEVINFO has a common prefix */
    return (struct lk_common *)Handle;
}

/* the private element a KLST_DEVINFO_HANDLE points into */
static struct klst_el *lk_el_of(KLIB_HANDLE Handle)
{
    return (struct klst_el *)Handle;
}

KUSB_EXP KLIB_USER_CONTEXT KUSB_API LibK_GetContext(KLIB_HANDLE Handle, KLIB_HANDLE_TYPE HandleType)
{
    if (!Handle)
    {
        if ((unsigned)HandleType >= KLIB_HANDLE_TYPE_COUNT)
            return 0;
        return g_default_ctx[HandleType];
    }
    if (HandleType == KLIB_HANDLE_TYPE_OVLK)
        return ((struct kovl *)Handle)->user_ctx;
    if (HandleType == KLIB_HANDLE_TYPE_LSTINFOK)
        return lk_el_of(Handle)->user_ctx;
    return ((struct lk_common *)Handle)->user_ctx;
}

KUSB_EXP BOOL KUSB_API LibK_SetContext(KLIB_HANDLE Handle, KLIB_HANDLE_TYPE HandleType, KLIB_USER_CONTEXT ContextValue)
{
    if (!Handle)
    {
        if ((unsigned)HandleType >= KLIB_HANDLE_TYPE_COUNT)
            return FALSE;
        g_default_ctx[HandleType] = ContextValue;
        return TRUE;
    }
    if (HandleType == KLIB_HANDLE_TYPE_OVLK)
        ((struct kovl *)Handle)->user_ctx = ContextValue;
    else if (HandleType == KLIB_HANDLE_TYPE_LSTINFOK)
        lk_el_of(Handle)->user_ctx = ContextValue;
    else
        ((struct lk_common *)Handle)->user_ctx = ContextValue;
    return TRUE;
}

KUSB_EXP BOOL KUSB_API LibK_SetCleanupCallback(KLIB_HANDLE Handle, KLIB_HANDLE_TYPE HandleType, KLIB_HANDLE_CLEANUP_CB *CleanupCB)
{
    if (HandleType == KLIB_HANDLE_TYPE_LSTINFOK)
    {
        if (!Handle)
            return FALSE;
        lk_el_of(Handle)->cleanup = CleanupCB;
        return TRUE;
    }
    struct lk_common *common = lk_common_of(Handle, HandleType);
    if (!common)
        return FALSE;
    common->cleanup = CleanupCB;
    return TRUE;
}

KUSB_EXP BOOL KUSB_API LibK_SetDefaultContext(KLIB_HANDLE_TYPE HandleType, KLIB_USER_CONTEXT ContextValue)
{
    if ((unsigned)HandleType >= KLIB_HANDLE_TYPE_COUNT)
        return FALSE;
    g_default_ctx[HandleType] = ContextValue;
    return TRUE;
}

KUSB_EXP KLIB_USER_CONTEXT KUSB_API LibK_GetDefaultContext(KLIB_HANDLE_TYPE HandleType)
{
    if ((unsigned)HandleType >= KLIB_HANDLE_TYPE_COUNT)
        return 0;
    return g_default_ctx[HandleType];
}

KUSB_EXP BOOL KUSB_API LibK_Context_Init(HANDLE Heap, PVOID Reserved)
{
    (void)Heap;
    (void)Reserved;
    return TRUE;
}
KUSB_EXP VOID KUSB_API LibK_Context_Free(VOID) {}

/* The function table is filled from this single source of truth. */
static void lk_fill_driver_api(PKUSB_DRIVER_API api, INT driver_id)
{
    memset(api, 0, sizeof(*api));
    api->Info.DriverID = driver_id;
    api->Info.FunctionCount = KUSB_FNID_COUNT;
    api->Init = UsbK_Init;
    api->Free = UsbK_Free;
    api->ClaimInterface = UsbK_ClaimInterface;
    api->ReleaseInterface = UsbK_ReleaseInterface;
    api->SetAltInterface = UsbK_SetAltInterface;
    api->GetAltInterface = UsbK_GetAltInterface;
    api->GetDescriptor = UsbK_GetDescriptor;
    api->ControlTransfer = UsbK_ControlTransfer;
    api->SetPowerPolicy = UsbK_SetPowerPolicy;
    api->GetPowerPolicy = UsbK_GetPowerPolicy;
    api->SetConfiguration = UsbK_SetConfiguration;
    api->GetConfiguration = UsbK_GetConfiguration;
    api->ResetDevice = UsbK_ResetDevice;
    api->Initialize = UsbK_Initialize;
    api->SelectInterface = UsbK_SelectInterface;
    api->GetAssociatedInterface = UsbK_GetAssociatedInterface;
    api->Clone = UsbK_Clone;
    api->QueryInterfaceSettings = UsbK_QueryInterfaceSettings;
    api->QueryDeviceInformation = UsbK_QueryDeviceInformation;
    api->SetCurrentAlternateSetting = UsbK_SetCurrentAlternateSetting;
    api->GetCurrentAlternateSetting = UsbK_GetCurrentAlternateSetting;
    api->QueryPipe = UsbK_QueryPipe;
    api->SetPipePolicy = UsbK_SetPipePolicy;
    api->GetPipePolicy = UsbK_GetPipePolicy;
    api->ReadPipe = UsbK_ReadPipe;
    api->WritePipe = UsbK_WritePipe;
    api->ResetPipe = UsbK_ResetPipe;
    api->AbortPipe = UsbK_AbortPipe;
    api->FlushPipe = UsbK_FlushPipe;
    api->IsoReadPipe = UsbK_IsoReadPipe;
    api->IsoWritePipe = UsbK_IsoWritePipe;
    api->GetCurrentFrameNumber = UsbK_GetCurrentFrameNumber;
    api->GetOverlappedResult = UsbK_GetOverlappedResult;
    api->GetProperty = UsbK_GetProperty;
    api->IsochReadPipe = UsbK_IsochReadPipe;
    api->IsochWritePipe = UsbK_IsochWritePipe;
    api->QueryPipeEx = UsbK_QueryPipeEx;
    api->GetSuperSpeedPipeCompanionDescriptor = UsbK_GetSuperSpeedPipeCompanionDescriptor;
    memset(api->z_FuncSupported, 1, sizeof(api->z_FuncSupported));
}

KUSB_EXP BOOL KUSB_API LibK_LoadDriverAPI(PKUSB_DRIVER_API DriverAPI, INT DriverID)
{
    if (!DriverAPI)
        return FALSE;
    lk_fill_driver_api(DriverAPI, DriverID);
    return TRUE;
}

KUSB_EXP BOOL KUSB_API LibK_CopyDriverAPI(PKUSB_DRIVER_API DriverAPI, KUSB_HANDLE UsbHandle)
{
    (void)UsbHandle;
    if (!DriverAPI)
        return FALSE;
    lk_fill_driver_api(DriverAPI, KUSB_DRVID_LIBUSBK);
    return TRUE;
}

KUSB_EXP BOOL KUSB_API LibK_IsFunctionSupported(PKUSB_DRIVER_API DriverAPI, UINT FunctionID)
{
    if (!DriverAPI || FunctionID >= KUSB_FNID_COUNT)
        return FALSE;
    return DriverAPI->z_FuncSupported[FunctionID] ? TRUE : FALSE;
}

KUSB_EXP BOOL KUSB_API LibK_GetProcAddress(KPROC *ProcAddress, INT DriverID, INT FunctionID)
{
    (void)DriverID;
    if (!ProcAddress || FunctionID < 0 || FunctionID >= KUSB_FNID_COUNT)
        return FALSE;
    /* Build the table once, then index it by function id. */
    KUSB_DRIVER_API api;
    lk_fill_driver_api(&api, DriverID);
    KPROC *table = (KPROC *)((UCHAR *)&api + sizeof(KUSB_DRIVER_API_INFO));
    *ProcAddress = table[FunctionID];
    return *ProcAddress != NULL;
}

/* ===================================================================== */
/* LstK - device list (snapshot of the USB/IP server's exported devices) */
/* ===================================================================== */
static void lk_fill_devinfo(KLST_DEVINFO *out, const struct usbip_devinfo *in)
{
    memset(out, 0, sizeof(*out));
    out->Common.Vid = in->vid;
    out->Common.Pid = in->pid;
    out->Common.MI = -1;
    out->DriverID = KUSB_DRVID_LIBUSBK;
    out->Connected = TRUE;
    out->SyncFlags = KLST_SYNC_FLAG_UNCHANGED;
    out->BusNumber = (INT)in->busnum;
    out->DeviceAddress = (INT)in->devnum;
    snprintf(out->Common.InstanceID, sizeof(out->Common.InstanceID), "%s", in->busid);
    snprintf(out->DeviceID, sizeof(out->DeviceID), "USB\\VID_%04X&PID_%04X\\%s", in->vid, in->pid, in->busid);
    snprintf(out->DevicePath, sizeof(out->DevicePath), "%s", in->busid); /* re-import key */
    snprintf(out->SymbolicLink, sizeof(out->SymbolicLink), "%s", in->busid);
    snprintf(out->SerialNumber, sizeof(out->SerialNumber), "%s", in->busid);
    snprintf(out->Service, sizeof(out->Service), "libusbK");
    snprintf(out->Mfg, sizeof(out->Mfg), "USB over IP");
    snprintf(out->DeviceDesc, sizeof(out->DeviceDesc), "USBIP USB/IP device");
}

/* append an element the list takes ownership of */
static void lk_list_append(struct klst *list, struct klst_el *el)
{
    el->owner = list;
    el->next = NULL;
    if (list->tail)
        list->tail->next = el;
    else
        list->head = el;
    list->tail = el;
    list->count++;
}

/* unlink an element; it becomes a standalone (detached) devinfo again */
static BOOL lk_list_unlink(struct klst *list, struct klst_el *el)
{
    struct klst_el **pp = &list->head;
    struct klst_el *prev = NULL;
    while (*pp && *pp != el)
    {
        prev = *pp;
        pp = &(*pp)->next;
    }
    if (!*pp)
        return FALSE;
    if (list->cursor == el)
        list->cursor = prev; /* keep MoveNext walking from the element before it */
    *pp = el->next;
    if (list->tail == el)
        list->tail = prev;
    el->next = NULL;
    el->owner = NULL;
    list->count--;
    return TRUE;
}

static void lk_el_free(struct klst_el *el)
{
    if (el->cleanup)
        el->cleanup(&el->pub, KLIB_HANDLE_TYPE_LSTINFOK, el->user_ctx);
    free(el);
}

KUSB_EXP BOOL KUSB_API LstK_InitEx(KLST_HANDLE *DeviceList, KLST_FLAG Flags, PKLST_PATTERN_MATCH PatternMatch)
{
    (void)Flags;
    if (!DeviceList)
        return FALSE;
    usb_transport *transport = lk_env_transport();
    if (!transport)
        return FALSE;
    int fd = usbip_connect(transport);
    if (fd < 0)
    {
        usbip_transport_free(transport);
        SetLastError(ERROR_FILE_NOT_FOUND);
        return FALSE;
    }
    struct usbip_devinfo infos[64];
    int count = usbip_client_devlist(fd, infos, 64);
    sock_close(fd);
    usbip_transport_free(transport);
    if (count < 0)
        count = 0;

    struct klst *list = calloc(1, sizeof(*list));
    if (!list)
        LK_FAIL(ERROR_OUTOFMEMORY);
    list->base.type = KLIB_HANDLE_TYPE_LSTK;
    for (int i = 0; i < count; i++)
    {
        KLST_DEVINFO tmp;
        lk_fill_devinfo(&tmp, &infos[i]);
        if (PatternMatch && PatternMatch->DeviceID[0] && !lk_wild(PatternMatch->DeviceID, tmp.DeviceID))
            continue;
        struct klst_el *el = calloc(1, sizeof(*el));
        if (!el)
        {
            LstK_Free(list);
            SetLastError(ERROR_OUTOFMEMORY);
            return FALSE;
        }
        el->pub = tmp;
        lk_list_append(list, el);
    }
    *DeviceList = list;
    return TRUE;
}

KUSB_EXP BOOL KUSB_API LstK_Init(KLST_HANDLE *DeviceList, KLST_FLAG Flags)
{
    return LstK_InitEx(DeviceList, Flags, NULL);
}

KUSB_EXP BOOL KUSB_API LstK_Free(KLST_HANDLE DeviceList)
{
    struct klst *list = DeviceList;
    if (!list)
        return FALSE;
    if (list->base.cleanup)
        list->base.cleanup(DeviceList, KLIB_HANDLE_TYPE_LSTK, list->base.user_ctx);
    for (struct klst_el *el = list->head, *next; el; el = next)
    {
        next = el->next;
        lk_el_free(el);
    }
    free(list);
    return TRUE;
}

KUSB_EXP BOOL KUSB_API LstK_Count(KLST_HANDLE DeviceList, PUINT Count)
{
    struct klst *list = DeviceList;
    if (!list || !Count)
        return FALSE;
    *Count = (UINT)list->count;
    return TRUE;
}

KUSB_EXP VOID KUSB_API LstK_MoveReset(KLST_HANDLE DeviceList)
{
    struct klst *list = DeviceList;
    if (list)
    {
        list->cursor = NULL;
        list->started = 0;
    }
}

KUSB_EXP BOOL KUSB_API LstK_MoveNext(KLST_HANDLE DeviceList, KLST_DEVINFO_HANDLE *DeviceInfo)
{
    struct klst *list = DeviceList;
    if (!list)
        return FALSE;
    struct klst_el *next = list->started ? (list->cursor ? list->cursor->next : NULL) : list->head;
    list->started = 1;
    list->cursor = next;
    if (!next)
    {
        if (DeviceInfo)
            *DeviceInfo = NULL;
        return FALSE;
    }
    if (DeviceInfo)
        *DeviceInfo = &next->pub;
    return TRUE;
}

KUSB_EXP BOOL KUSB_API LstK_Current(KLST_HANDLE DeviceList, KLST_DEVINFO_HANDLE *DeviceInfo)
{
    struct klst *list = DeviceList;
    if (!list || !DeviceInfo)
        return FALSE;
    if (!list->started || !list->cursor)
    {
        *DeviceInfo = NULL;
        SetLastError(ERROR_NO_MORE_ITEMS);
        return FALSE;
    }
    *DeviceInfo = &list->cursor->pub;
    return TRUE;
}

KUSB_EXP BOOL KUSB_API LstK_FindByVidPid(KLST_HANDLE DeviceList, INT Vid, INT Pid, KLST_DEVINFO_HANDLE *DeviceInfo)
{
    struct klst *list = DeviceList;
    if (!list || !DeviceInfo)
        return FALSE;
    for (struct klst_el *el = list->head; el; el = el->next)
    {
        if (el->pub.Common.Vid == Vid && el->pub.Common.Pid == Pid)
        {
            list->cursor = el;
            list->started = 1;
            *DeviceInfo = &el->pub;
            return TRUE;
        }
    }
    *DeviceInfo = NULL;
    SetLastError(ERROR_NO_MORE_ITEMS);
    return FALSE;
}

KUSB_EXP BOOL KUSB_API LstK_Enumerate(KLST_HANDLE DeviceList, KLST_ENUM_DEVINFO_CB *EnumDevListCB, PVOID Context)
{
    struct klst *list = DeviceList;
    if (!list || !EnumDevListCB)
        return FALSE;
    for (struct klst_el *el = list->head, *next; el; el = next)
    {
        next = el->next; /* the callback may detach the element it is handed */
        if (!EnumDevListCB(DeviceList, &el->pub, Context))
            break;
    }
    return TRUE;
}

/* ---- clone / attach / detach / sync ---------------------------------- */
KUSB_EXP BOOL KUSB_API LstK_CloneInfo(KLST_DEVINFO_HANDLE SrcInfo, KLST_DEVINFO_HANDLE *DstInfo)
{
    if (!SrcInfo || !DstInfo)
        return FALSE;
    struct klst_el *el = calloc(1, sizeof(*el));
    if (!el)
        LK_FAIL(ERROR_OUTOFMEMORY);
    el->pub = *(KLST_DEVINFO *)SrcInfo; /* the copy starts with no context/cleanup */
    *DstInfo = &el->pub;
    return TRUE;
}

KUSB_EXP BOOL KUSB_API LstK_FreeInfo(KLST_DEVINFO_HANDLE DeviceInfo)
{
    struct klst_el *el = lk_el_of(DeviceInfo);
    if (!el)
        return FALSE;
    if (el->owner)
    {   /* still in a list: LstK_Free owns it, detach it first */
        SetLastError(ERROR_INVALID_HANDLE);
        return FALSE;
    }
    lk_el_free(el);
    return TRUE;
}

KUSB_EXP BOOL KUSB_API LstK_AttachInfo(KLST_HANDLE DeviceList, KLST_DEVINFO_HANDLE DeviceInfo)
{
    struct klst *list = DeviceList;
    struct klst_el *el = lk_el_of(DeviceInfo);
    if (!list || !el || el->owner)
        LK_FAIL(ERROR_INVALID_HANDLE);
    lk_list_append(list, el);
    return TRUE;
}

KUSB_EXP BOOL KUSB_API LstK_DetachInfo(KLST_HANDLE DeviceList, KLST_DEVINFO_HANDLE DeviceInfo)
{
    struct klst *list = DeviceList;
    struct klst_el *el = lk_el_of(DeviceInfo);
    if (!list || !el || el->owner != list || !lk_list_unlink(list, el))
        LK_FAIL(ERROR_INVALID_HANDLE);
    return TRUE;
}

KUSB_EXP BOOL KUSB_API LstK_Clone(KLST_HANDLE SrcList, KLST_HANDLE *DstList)
{
    struct klst *src = SrcList;
    if (!src || !DstList)
        return FALSE;
    struct klst *dst = calloc(1, sizeof(*dst));
    if (!dst)
        LK_FAIL(ERROR_OUTOFMEMORY);
    dst->base.type = KLIB_HANDLE_TYPE_LSTK;
    for (struct klst_el *el = src->head; el; el = el->next)
    {
        struct klst_el *copy = calloc(1, sizeof(*copy));
        if (!copy)
        {
            LstK_Free(dst);
            SetLastError(ERROR_OUTOFMEMORY);
            return FALSE;
        }
        copy->pub = el->pub;
        lk_list_append(dst, copy);
    }
    *DstList = dst;
    return TRUE;
}

/* Two device infos are the same device when their DeviceID matches - that string
 * carries vid/pid and the USB/IP busid, which is what the server re-imports by. */
static struct klst_el *lk_list_find_id(struct klst *list, const char *device_id)
{
    for (struct klst_el *el = list->head; el; el = el->next)
        if (strcmp(el->pub.DeviceID, device_id) == 0)
            return el;
    return NULL;
}

KUSB_EXP BOOL KUSB_API LstK_Sync(KLST_HANDLE MasterList, KLST_HANDLE SlaveList, KLST_SYNC_FLAG SyncFlags,
                                 PKLST_PATTERN_MATCH SlaveListPatternMatch, HANDLE Heap)
{
    (void)Heap;
    struct klst *master = MasterList;
    if (!master)
        return FALSE;

    /* no slave given: take a fresh snapshot of the server's devices to sync against */
    struct klst *slave = SlaveList;
    KLST_HANDLE temp = NULL;
    if (!slave)
    {
        if (!LstK_InitEx(&temp, KLST_FLAG_NONE, SlaveListPatternMatch))
            return FALSE;
        slave = temp;
    }

    /* still in the slave list -> unchanged (or a connect-state change); gone -> removed */
    for (struct klst_el *el = master->head; el; el = el->next)
    {
        struct klst_el *match = lk_list_find_id(slave, el->pub.DeviceID);
        if (!match)
        {
            el->pub.SyncFlags = KLST_SYNC_FLAG_REMOVED;
            el->pub.Connected = FALSE;
        }
        else if (match->pub.Connected != el->pub.Connected)
        {
            el->pub.SyncFlags = KLST_SYNC_FLAG_CONNECT_CHANGE;
            el->pub.Connected = match->pub.Connected;
        }
        else
        {
            el->pub.SyncFlags = KLST_SYNC_FLAG_UNCHANGED;
        }
    }

    /* in the slave list but not the master -> added; copy it across */
    for (struct klst_el *el = slave->head; el; el = el->next)
    {
        if (lk_list_find_id(master, el->pub.DeviceID))
            continue;
        struct klst_el *copy = calloc(1, sizeof(*copy));
        if (!copy)
        {
            if (temp)
                LstK_Free(temp);
            SetLastError(ERROR_OUTOFMEMORY);
            return FALSE;
        }
        copy->pub = el->pub;
        copy->pub.SyncFlags = KLST_SYNC_FLAG_ADDED;
        lk_list_append(master, copy);
    }

    /* KLST_SYNC_FLAG_REMOVED in SyncFlags means "drop the gone ones" rather than
     * just marking them; anything else only marks. */
    if (SyncFlags & KLST_SYNC_FLAG_REMOVED)
    {
        for (struct klst_el *el = master->head, *next; el; el = next)
        {
            next = el->next;
            if (el->pub.SyncFlags == KLST_SYNC_FLAG_REMOVED && lk_list_unlink(master, el))
                lk_el_free(el);
        }
    }

    if (temp)
        LstK_Free(temp);
    master->cursor = NULL;
    master->started = 0;
    return TRUE;
}

/* ===================================================================== */
/* UsbK - open / close / config / interface                              */
/* ===================================================================== */
static struct kusb *lk_open_busid(const char *busid)
{
    struct kusb *handle = calloc(1, sizeof(*handle));
    if (!handle)
        return NULL;
    handle->base.type = KLIB_HANDLE_TYPE_USBK;
    handle->fd = -1;
    snprintf(handle->busid, sizeof(handle->busid), "%s", busid);
    handle->t = lk_env_transport();
    if (!handle->t)
    {
        free(handle);
        return NULL;
    }
    handle->fd = usbip_connect(handle->t);
    if (handle->fd < 0)
    {
        usbip_transport_free(handle->t);
        free(handle);
        return NULL;
    }
    if (usbip_client_import(handle->fd, handle->busid, &handle->info) != USB_SUCCESS)
    {
        sock_close(handle->fd);
        usbip_transport_free(handle->t);
        free(handle);
        return NULL;
    }
    handle->devid = (handle->info.busnum << 16) | handle->info.devnum;
    pthread_mutex_init(&handle->iolock, NULL);
    pthread_mutex_init(&handle->qlock, NULL);
    pthread_cond_init(&handle->qcond, NULL);
    return handle;
}

KUSB_EXP BOOL KUSB_API UsbK_Init(KUSB_HANDLE *InterfaceHandle, KLST_DEVINFO_HANDLE DevInfo)
{
    if (!InterfaceHandle || !DevInfo)
        LK_FAIL(ERROR_INVALID_PARAMETER);
    struct kusb *handle = lk_open_busid(DevInfo->DevicePath); /* DevicePath carries the busid */
    if (!handle)
        LK_FAIL(ERROR_FILE_NOT_FOUND);
    *InterfaceHandle = handle;
    return TRUE;
}

KUSB_EXP BOOL KUSB_API UsbK_Free(KUSB_HANDLE InterfaceHandle)
{
    struct kusb *handle = InterfaceHandle;
    if (!handle)
        return TRUE;
    if (handle->base.cleanup)
        handle->base.cleanup(InterfaceHandle, KLIB_HANDLE_TYPE_USBK, handle->base.user_ctx);
    if (handle->worker_started)
    {
        pthread_mutex_lock(&handle->qlock);
        handle->shutting = 1;
        pthread_cond_signal(&handle->qcond);
        pthread_mutex_unlock(&handle->qlock);
        pthread_join(handle->worker, NULL);
    }
    if (handle->fd >= 0)
        sock_close(handle->fd);
    if (handle->t)
        usbip_transport_free(handle->t);
    pthread_mutex_destroy(&handle->iolock);
    pthread_mutex_destroy(&handle->qlock);
    pthread_cond_destroy(&handle->qcond);
    free(handle->cfg_raw);
    free(handle);
    return TRUE;
}

KUSB_EXP BOOL KUSB_API UsbK_Initialize(HANDLE DeviceHandle, KUSB_HANDLE *InterfaceHandle)
{
    /* No Windows device file handle exists over USB/IP. */
    (void)DeviceHandle;
    (void)InterfaceHandle;
    SetLastError(ERROR_NOT_SUPPORTED);
    return FALSE;
}

KUSB_EXP BOOL KUSB_API UsbK_Clone(KUSB_HANDLE InterfaceHandle, KUSB_HANDLE *DstInterfaceHandle)
{
    struct kusb *handle = InterfaceHandle;
    if (!handle || !DstInterfaceHandle)
        LK_FAIL(ERROR_INVALID_PARAMETER);
    struct kusb *dup = lk_open_busid(handle->busid); /* independent import of the same device */
    if (!dup)
        LK_FAIL(ERROR_FILE_NOT_FOUND);
    dup->cur_iface = handle->cur_iface;
    *DstInterfaceHandle = dup;
    return TRUE;
}

KUSB_EXP BOOL KUSB_API UsbK_ClaimInterface(KUSB_HANDLE InterfaceHandle, UCHAR NumberOrIndex, BOOL IsIndex)
{
    struct kusb *handle = InterfaceHandle;
    if (!handle)
        LK_FAIL(ERROR_INVALID_PARAMETER);
    lk_fetch_config(handle);
    handle->cur_iface = IsIndex ? (UCHAR)lk_iface_number_by_index(handle, NumberOrIndex) : NumberOrIndex;
    return TRUE; /* USB/IP imports the whole device; nothing else to bind */
}

KUSB_EXP BOOL KUSB_API UsbK_ReleaseInterface(KUSB_HANDLE InterfaceHandle, UCHAR NumberOrIndex, BOOL IsIndex)
{
    (void)NumberOrIndex;
    (void)IsIndex;
    return InterfaceHandle ? TRUE : FALSE;
}

KUSB_EXP BOOL KUSB_API UsbK_SelectInterface(KUSB_HANDLE InterfaceHandle, UCHAR NumberOrIndex, BOOL IsIndex)
{
    return UsbK_ClaimInterface(InterfaceHandle, NumberOrIndex, IsIndex);
}

KUSB_EXP BOOL KUSB_API UsbK_GetAssociatedInterface(KUSB_HANDLE InterfaceHandle, UCHAR AssociatedInterfaceIndex, KUSB_HANDLE *AssociatedInterfaceHandle)
{
    (void)InterfaceHandle;
    (void)AssociatedInterfaceIndex;
    (void)AssociatedInterfaceHandle;
    SetLastError(ERROR_NO_MORE_ITEMS);
    return FALSE;
}

KUSB_EXP BOOL KUSB_API UsbK_SetConfiguration(KUSB_HANDLE InterfaceHandle, UCHAR ConfigurationNumber)
{
    struct kusb *handle = InterfaceHandle;
    if (!handle)
        LK_FAIL(ERROR_INVALID_PARAMETER);
    int rc = lk_control(handle, 0x00, USB_REQ_SET_CONFIGURATION, ConfigurationNumber, 0, NULL, 0, NULL);
    if (rc < 0)
        LK_FAIL(ERROR_GEN_FAILURE);
    handle->cur_config = ConfigurationNumber;
    return TRUE;
}

KUSB_EXP BOOL KUSB_API UsbK_GetConfiguration(KUSB_HANDLE InterfaceHandle, PUCHAR ConfigurationNumber)
{
    struct kusb *handle = InterfaceHandle;
    if (!handle || !ConfigurationNumber)
        LK_FAIL(ERROR_INVALID_PARAMETER);
    uint8_t cfg_byte = handle->cur_config ? handle->cur_config : 1;
    int act = 0;
    if (lk_control(handle, 0x80, USB_REQ_GET_CONFIGURATION, 0, 0, &cfg_byte, 1, &act) >= 0 && act >= 1)
        handle->cur_config = cfg_byte;
    *ConfigurationNumber = cfg_byte;
    return TRUE;
}

KUSB_EXP BOOL KUSB_API UsbK_SetCurrentAlternateSetting(KUSB_HANDLE InterfaceHandle, UCHAR AltSettingNumber)
{
    struct kusb *handle = InterfaceHandle;
    if (!handle)
        LK_FAIL(ERROR_INVALID_PARAMETER);
    int rc = lk_control(handle, 0x01, USB_REQ_SET_INTERFACE, AltSettingNumber, handle->cur_iface, NULL, 0, NULL);
    if (rc < 0)
        LK_FAIL(ERROR_GEN_FAILURE);
    handle->cur_alt[handle->cur_iface] = AltSettingNumber;
    return TRUE;
}

KUSB_EXP BOOL KUSB_API UsbK_GetCurrentAlternateSetting(KUSB_HANDLE InterfaceHandle, PUCHAR AltSettingNumber)
{
    struct kusb *handle = InterfaceHandle;
    if (!handle || !AltSettingNumber)
        LK_FAIL(ERROR_INVALID_PARAMETER);
    *AltSettingNumber = handle->cur_alt[handle->cur_iface];
    return TRUE;
}

KUSB_EXP BOOL KUSB_API UsbK_SetAltInterface(KUSB_HANDLE InterfaceHandle, UCHAR NumberOrIndex, BOOL IsIndex, UCHAR AltSettingNumber)
{
    struct kusb *handle = InterfaceHandle;
    if (!handle)
        LK_FAIL(ERROR_INVALID_PARAMETER);
    lk_fetch_config(handle);
    UCHAR ifnum = IsIndex ? (UCHAR)lk_iface_number_by_index(handle, NumberOrIndex) : NumberOrIndex;
    int rc = lk_control(handle, 0x01, USB_REQ_SET_INTERFACE, AltSettingNumber, ifnum, NULL, 0, NULL);
    if (rc < 0)
        LK_FAIL(ERROR_GEN_FAILURE);
    handle->cur_alt[ifnum] = AltSettingNumber;
    return TRUE;
}

KUSB_EXP BOOL KUSB_API UsbK_GetAltInterface(KUSB_HANDLE InterfaceHandle, UCHAR NumberOrIndex, BOOL IsIndex, PUCHAR AltSettingNumber)
{
    struct kusb *handle = InterfaceHandle;
    if (!handle || !AltSettingNumber)
        LK_FAIL(ERROR_INVALID_PARAMETER);
    if (handle->cfg_raw == NULL)
        lk_fetch_config(handle);
    UCHAR ifnum = IsIndex ? (UCHAR)lk_iface_number_by_index(handle, NumberOrIndex) : NumberOrIndex;
    *AltSettingNumber = handle->cur_alt[ifnum];
    return TRUE;
}

KUSB_EXP BOOL KUSB_API UsbK_ResetDevice(KUSB_HANDLE InterfaceHandle)
{
    return InterfaceHandle ? TRUE : FALSE; /* no device-reset path over USB/IP import */
}

/* ===================================================================== */
/* UsbK - descriptors / device + interface info / pipes                  */
/* ===================================================================== */
KUSB_EXP BOOL KUSB_API UsbK_GetDescriptor(KUSB_HANDLE InterfaceHandle, UCHAR DescriptorType, UCHAR Index,
                                          USHORT LanguageID, PUCHAR Buffer, UINT BufferLength, PUINT LengthTransferred)
{
    struct kusb *handle = InterfaceHandle;
    if (!handle || !Buffer)
        LK_FAIL(ERROR_INVALID_PARAMETER);
    int act = 0;
    int rc = lk_control(handle, 0x80, USB_REQ_GET_DESCRIPTOR, (uint16_t)((DescriptorType << 8) | Index),
                        LanguageID, Buffer, (uint16_t)BufferLength, &act);
    if (rc < 0)
        LK_FAIL(ERROR_GEN_FAILURE);
    if (LengthTransferred)
        *LengthTransferred = (UINT)act;
    return TRUE;
}

KUSB_EXP BOOL KUSB_API UsbK_QueryInterfaceSettings(KUSB_HANDLE InterfaceHandle, UCHAR AltSettingIndex, PUSB_INTERFACE_DESCRIPTOR UsbAltInterfaceDescriptor)
{
    struct kusb *handle = InterfaceHandle;
    if (!handle || !UsbAltInterfaceDescriptor)
        LK_FAIL(ERROR_INVALID_PARAMETER);
    if (lk_fetch_config(handle) < 0)
        LK_FAIL(ERROR_GEN_FAILURE);
    int off = lk_iface_off(handle, handle->cur_iface, AltSettingIndex);
    if (off < 0)
        LK_FAIL(ERROR_NO_MORE_ITEMS);
    memcpy(UsbAltInterfaceDescriptor, handle->cfg_raw + off, sizeof(USB_INTERFACE_DESCRIPTOR));
    return TRUE;
}

static BOOL lk_query_pipe(struct kusb *handle, UCHAR alt, UCHAR pipe_index, WINUSB_PIPE_INFORMATION *info, ULONG *bytes_per_interval)
{
    if (lk_fetch_config(handle) < 0)
        LK_FAIL(ERROR_GEN_FAILURE);
    int ifoff = lk_iface_off(handle, handle->cur_iface, alt);
    if (ifoff < 0)
        LK_FAIL(ERROR_NO_MORE_ITEMS);
    int epoff = lk_ep_off(handle, ifoff, pipe_index);
    if (epoff < 0)
        LK_FAIL(ERROR_NO_MORE_ITEMS);
    uint8_t *ep = handle->cfg_raw + epoff;
    USHORT mps = usb_get_le16(ep + 4);
    info->PipeType = (USBD_PIPE_TYPE)(ep[3] & 0x03);
    info->PipeId = ep[2];
    info->MaximumPacketSize = mps;
    info->Interval = ep[6];
    if (bytes_per_interval)
        *bytes_per_interval = (ULONG)(mps & 0x7ff) * (((mps >> 11) & 0x3) + 1);
    return TRUE;
}

KUSB_EXP BOOL KUSB_API UsbK_QueryPipe(KUSB_HANDLE InterfaceHandle, UCHAR AltSettingNumber, UCHAR PipeIndex, PWINUSB_PIPE_INFORMATION PipeInformation)
{
    struct kusb *handle = InterfaceHandle;
    if (!handle || !PipeInformation)
        LK_FAIL(ERROR_INVALID_PARAMETER);
    return lk_query_pipe(handle, AltSettingNumber, PipeIndex, PipeInformation, NULL);
}

KUSB_EXP BOOL KUSB_API UsbK_QueryPipeEx(KUSB_HANDLE InterfaceHandle, UCHAR AltSettingNumber, UCHAR PipeIndex, PWINUSB_PIPE_INFORMATION_EX PipeInformationEx)
{
    struct kusb *handle = InterfaceHandle;
    if (!handle || !PipeInformationEx)
        LK_FAIL(ERROR_INVALID_PARAMETER);
    WINUSB_PIPE_INFORMATION base;
    ULONG bpi = 0;
    if (!lk_query_pipe(handle, AltSettingNumber, PipeIndex, &base, &bpi))
        return FALSE;
    PipeInformationEx->PipeType = base.PipeType;
    PipeInformationEx->PipeId = base.PipeId;
    PipeInformationEx->MaximumPacketSize = base.MaximumPacketSize;
    PipeInformationEx->Interval = base.Interval;
    PipeInformationEx->MaximumBytesPerInterval = bpi;
    return TRUE;
}

KUSB_EXP BOOL KUSB_API UsbK_GetSuperSpeedPipeCompanionDescriptor(KUSB_HANDLE InterfaceHandle, UCHAR AltSettingNumber, UCHAR PipeIndex, PUSB_SUPERSPEED_ENDPOINT_COMPANION_DESCRIPTOR PipeCompanionDescriptor)
{
    (void)InterfaceHandle;
    (void)AltSettingNumber;
    (void)PipeIndex;
    (void)PipeCompanionDescriptor;
    SetLastError(ERROR_NOT_FOUND); /* not present on FS/HS devices */
    return FALSE;
}

KUSB_EXP BOOL KUSB_API UsbK_QueryDeviceInformation(KUSB_HANDLE InterfaceHandle, UINT InformationType, PUINT BufferLength, PUCHAR Buffer)
{
    struct kusb *handle = InterfaceHandle;
    if (!handle || !BufferLength || !Buffer)
        LK_FAIL(ERROR_INVALID_PARAMETER);
    if (InformationType == DEVICE_SPEED)
    {
        if (*BufferLength < 1)
        {
            *BufferLength = 1;
            SetLastError(ERROR_INSUFFICIENT_BUFFER);
            return FALSE;
        }
        Buffer[0] = (handle->info.speed >= 3) ? HighSpeed : FullSpeed;
        *BufferLength = 1;
        return TRUE;
    }
    SetLastError(ERROR_NOT_SUPPORTED);
    return FALSE;
}

KUSB_EXP BOOL KUSB_API UsbK_GetCurrentFrameNumber(KUSB_HANDLE InterfaceHandle, PUINT FrameNumber)
{
    if (!InterfaceHandle || !FrameNumber)
        LK_FAIL(ERROR_INVALID_PARAMETER);
    *FrameNumber = 0; /* no SOF counter over USB/IP */
    return TRUE;
}

KUSB_EXP BOOL KUSB_API UsbK_GetProperty(KUSB_HANDLE InterfaceHandle, KUSB_PROPERTY PropertyType, PUINT PropertySize, PVOID Value)
{
    struct kusb *handle = InterfaceHandle;
    if (!handle || !PropertySize || !Value)
        LK_FAIL(ERROR_INVALID_PARAMETER);
    if (PropertyType == KUSB_PROPERTY_DEVICE_FILE_HANDLE)
    {
        if (*PropertySize < sizeof(HANDLE))
        {
            *PropertySize = sizeof(HANDLE);
            SetLastError(ERROR_INSUFFICIENT_BUFFER);
            return FALSE;
        }
        *(HANDLE *)Value = (HANDLE)(INT_PTR)handle->fd; /* the USB/IP socket */
        *PropertySize = sizeof(HANDLE);
        return TRUE;
    }
    SetLastError(ERROR_NOT_SUPPORTED);
    return FALSE;
}

/* ===================================================================== */
/* UsbK - pipe / power policy (stored; honored where meaningful)          */
/* ===================================================================== */
KUSB_EXP BOOL KUSB_API UsbK_SetPipePolicy(KUSB_HANDLE InterfaceHandle, UCHAR PipeID, UINT PolicyType, UINT ValueLength, PVOID Value)
{
    struct kusb *handle = InterfaceHandle;
    if (!handle || !Value || ValueLength < 1)
        LK_FAIL(ERROR_INVALID_PARAMETER);
    struct pipe_pol *pol = &handle->pol[lk_pol_index(PipeID)];
    UINT v4 = (ValueLength >= 4) ? *(UINT *)Value : *(UCHAR *)Value;
    UCHAR v1 = *(UCHAR *)Value;
    switch (PolicyType)
    {
    case SHORT_PACKET_TERMINATE:
        pol->short_packet_terminate = v1;
        break;
    case AUTO_CLEAR_STALL:
        pol->auto_clear_stall = v1;
        break;
    case PIPE_TRANSFER_TIMEOUT:
        pol->transfer_timeout = v4;
        break;
    case IGNORE_SHORT_PACKETS:
        pol->ignore_short_packets = v1;
        break;
    case ALLOW_PARTIAL_READS:
        pol->allow_partial_reads = v1;
        break;
    case AUTO_FLUSH:
        pol->auto_flush = v1;
        break;
    case RAW_IO:
        pol->raw_io = v1;
        break;
    case RESET_PIPE_ON_RESUME:
        pol->reset_pipe_on_resume = v1;
        break;
    case MAXIMUM_TRANSFER_SIZE:
        pol->max_transfer_size = v4;
        break;
    default:
        SetLastError(ERROR_NOT_SUPPORTED);
        return FALSE;
    }
    return TRUE;
}

KUSB_EXP BOOL KUSB_API UsbK_GetPipePolicy(KUSB_HANDLE InterfaceHandle, UCHAR PipeID, UINT PolicyType, PUINT ValueLength, PVOID Value)
{
    struct kusb *handle = InterfaceHandle;
    if (!handle || !ValueLength || !Value)
        LK_FAIL(ERROR_INVALID_PARAMETER);
    struct pipe_pol *pol = &handle->pol[lk_pol_index(PipeID)];
    UINT uint_val = 0;
    UCHAR byte_val = 0;
    int is_u = 0;
    switch (PolicyType)
    {
    case SHORT_PACKET_TERMINATE:
        byte_val = pol->short_packet_terminate;
        break;
    case AUTO_CLEAR_STALL:
        byte_val = pol->auto_clear_stall;
        break;
    case PIPE_TRANSFER_TIMEOUT:
        uint_val = pol->transfer_timeout;
        is_u = 1;
        break;
    case IGNORE_SHORT_PACKETS:
        byte_val = pol->ignore_short_packets;
        break;
    case ALLOW_PARTIAL_READS:
        byte_val = pol->allow_partial_reads;
        break;
    case AUTO_FLUSH:
        byte_val = pol->auto_flush;
        break;
    case RAW_IO:
        byte_val = pol->raw_io;
        break;
    case RESET_PIPE_ON_RESUME:
        byte_val = pol->reset_pipe_on_resume;
        break;
    case MAXIMUM_TRANSFER_SIZE:
        uint_val = pol->max_transfer_size;
        is_u = 1;
        break;
    default:
        SetLastError(ERROR_NOT_SUPPORTED);
        return FALSE;
    }
    if (is_u)
    {
        if (*ValueLength < 4)
        {
            *ValueLength = 4;
            SetLastError(ERROR_INSUFFICIENT_BUFFER);
            return FALSE;
        }
        *(UINT *)Value = uint_val;
        *ValueLength = 4;
    }
    else
    {
        if (*ValueLength < 1)
        {
            *ValueLength = 1;
            SetLastError(ERROR_INSUFFICIENT_BUFFER);
            return FALSE;
        }
        *(UCHAR *)Value = byte_val;
        *ValueLength = 1;
    }
    return TRUE;
}

KUSB_EXP BOOL KUSB_API UsbK_SetPowerPolicy(KUSB_HANDLE InterfaceHandle, UINT PolicyType, UINT ValueLength, PVOID Value)
{
    struct kusb *handle = InterfaceHandle;
    if (!handle || !Value || ValueLength < 1)
        LK_FAIL(ERROR_INVALID_PARAMETER);
    UINT v4 = (ValueLength >= 4) ? *(UINT *)Value : *(UCHAR *)Value;
    if (PolicyType == AUTO_SUSPEND)
        handle->power_auto_suspend = v4;
    else if (PolicyType == SUSPEND_DELAY)
        handle->power_suspend_delay = v4;
    else
    {
        SetLastError(ERROR_NOT_SUPPORTED);
        return FALSE;
    }
    return TRUE; /* stored; power management is a no-op over USB/IP */
}

KUSB_EXP BOOL KUSB_API UsbK_GetPowerPolicy(KUSB_HANDLE InterfaceHandle, UINT PolicyType, PUINT ValueLength, PVOID Value)
{
    struct kusb *handle = InterfaceHandle;
    if (!handle || !ValueLength || !Value)
        LK_FAIL(ERROR_INVALID_PARAMETER);
    UINT uint_val;
    if (PolicyType == AUTO_SUSPEND)
        uint_val = handle->power_auto_suspend;
    else if (PolicyType == SUSPEND_DELAY)
        uint_val = handle->power_suspend_delay;
    else
    {
        SetLastError(ERROR_NOT_SUPPORTED);
        return FALSE;
    }
    if (*ValueLength < 4)
    {
        *ValueLength = 4;
        SetLastError(ERROR_INSUFFICIENT_BUFFER);
        return FALSE;
    }
    *(UINT *)Value = uint_val;
    *ValueLength = 4;
    return TRUE;
}

/* ===================================================================== */
/* UsbK - control / pipe I/O (synchronous when Overlapped==NULL)         */
/* ===================================================================== */
KUSB_EXP BOOL KUSB_API UsbK_ControlTransfer(KUSB_HANDLE InterfaceHandle, WINUSB_SETUP_PACKET SetupPacket,
                                            PUCHAR Buffer, UINT BufferLength, PUINT LengthTransferred, LPOVERLAPPED Overlapped)
{
    struct kusb *handle = InterfaceHandle;
    if (!handle)
        LK_FAIL(ERROR_INVALID_PARAMETER);
    uint8_t setup[8] = {USB_SETUP_BYTES(SetupPacket.RequestType, SetupPacket.Request,
                                        SetupPacket.Value, SetupPacket.Index, SetupPacket.Length)};
    int dir = (SetupPacket.RequestType & 0x80) ? USB_IN : USB_OUT;

    if (Overlapped)
    {
        struct aop *op = calloc(1, sizeof(*op));
        if (!op)
            LK_FAIL(ERROR_OUTOFMEMORY);
        op->dir = dir;
        op->ep = 0;
        op->has_setup = 1;
        memcpy(op->setup, setup, 8);
        op->buf = Buffer;
        op->len = (int)BufferLength;
        return kusb_queue(handle, op, Overlapped);
    }
    int act = 0;
    int rc = lk_submit(handle, dir, 0, setup, 0, Buffer, (int)BufferLength, &act);
    if (rc < 0)
        LK_FAIL(ERROR_GEN_FAILURE);
    if (LengthTransferred)
        *LengthTransferred = (dir == USB_IN) ? (UINT)act : BufferLength;
    return TRUE;
}

static BOOL lk_pipe_io(struct kusb *handle, UCHAR PipeID, PUCHAR Buffer, UINT BufferLength,
                       PUINT LengthTransferred, LPOVERLAPPED Overlapped)
{
    if (!handle)
        LK_FAIL(ERROR_INVALID_PARAMETER);
    int dir = (PipeID & 0x80) ? USB_IN : USB_OUT;
    int ep = PipeID & 0x0f;
    int interval = lk_pipe_interval(handle, PipeID);

    if (Overlapped)
    {
        struct aop *op = calloc(1, sizeof(*op));
        if (!op)
            LK_FAIL(ERROR_OUTOFMEMORY);
        op->dir = dir;
        op->ep = ep;
        op->interval = interval;
        op->buf = Buffer;
        op->len = (int)BufferLength;
        return kusb_queue(handle, op, Overlapped);
    }
    int act = 0;
    int rc = lk_submit(handle, dir, ep, NULL, interval, Buffer, (int)BufferLength, &act);
    if (rc < 0)
    {
        lk_auto_clear_stall(handle, PipeID, rc);
        SetLastError(ERROR_GEN_FAILURE); /* what WinUSB reports for a stalled pipe */
        return FALSE;
    }
    if (LengthTransferred)
        *LengthTransferred = (UINT)act;
    return TRUE;
}

KUSB_EXP BOOL KUSB_API UsbK_ReadPipe(KUSB_HANDLE InterfaceHandle, UCHAR PipeID, PUCHAR Buffer, UINT BufferLength, PUINT LengthTransferred, LPOVERLAPPED Overlapped)
{
    return lk_pipe_io(InterfaceHandle, PipeID, Buffer, BufferLength, LengthTransferred, Overlapped);
}

KUSB_EXP BOOL KUSB_API UsbK_WritePipe(KUSB_HANDLE InterfaceHandle, UCHAR PipeID, PUCHAR Buffer, UINT BufferLength, PUINT LengthTransferred, LPOVERLAPPED Overlapped)
{
    return lk_pipe_io(InterfaceHandle, PipeID, Buffer, BufferLength, LengthTransferred, Overlapped);
}

KUSB_EXP BOOL KUSB_API UsbK_ResetPipe(KUSB_HANDLE InterfaceHandle, UCHAR PipeID)
{
    struct kusb *handle = InterfaceHandle;
    if (!handle)
        LK_FAIL(ERROR_INVALID_PARAMETER);
    int rc = lk_control(handle, 0x02, USB_REQ_CLEAR_FEATURE, USB_FEATURE_ENDPOINT_HALT, PipeID, NULL, 0, NULL);
    if (rc < 0)
        LK_FAIL(ERROR_GEN_FAILURE);
    return TRUE;
}

KUSB_EXP BOOL KUSB_API UsbK_AbortPipe(KUSB_HANDLE InterfaceHandle, UCHAR PipeID)
{
    (void)PipeID;
    return InterfaceHandle ? TRUE : FALSE; /* in-flight submit cannot be interrupted */
}

KUSB_EXP BOOL KUSB_API UsbK_FlushPipe(KUSB_HANDLE InterfaceHandle, UCHAR PipeID)
{
    (void)PipeID;
    return InterfaceHandle ? TRUE : FALSE;
}

KUSB_EXP BOOL KUSB_API UsbK_GetOverlappedResult(KUSB_HANDLE InterfaceHandle, LPOVERLAPPED Overlapped, PUINT lpNumberOfBytesTransferred, BOOL bWait)
{
    (void)InterfaceHandle;
    if (!Overlapped)
        LK_FAIL(ERROR_INVALID_PARAMETER);
    if (bWait)
    {
        if (Overlapped->hEvent)
        {
            WaitForSingleObject(Overlapped->hEvent, INFINITE);
        }
        else
        {
            while (Overlapped->Internal == STATUS_PENDING)
                Sleep(0);
        }
    }
    else if (Overlapped->Internal == STATUS_PENDING)
        LK_FAIL(ERROR_IO_INCOMPLETE);
    if (lpNumberOfBytesTransferred)
        *lpNumberOfBytesTransferred = (UINT)Overlapped->InternalHigh;
    if (Overlapped->Internal != 0)
        LK_FAIL(ERROR_GEN_FAILURE);
    return TRUE;
}

/* ===================================================================== */
/* IsoK - KISO_CONTEXT management + iso pipe I/O                          */
/* ===================================================================== */
KUSB_EXP BOOL KUSB_API IsoK_Init(PKISO_CONTEXT *IsoContext, INT NumberOfPackets, INT StartFrame)
{
    if (!IsoContext || NumberOfPackets < 0)
        LK_FAIL(ERROR_INVALID_PARAMETER);
    size_t sz = sizeof(KISO_CONTEXT) + (size_t)NumberOfPackets * sizeof(KISO_PACKET);
    PKISO_CONTEXT ctx = calloc(1, sz);
    if (!ctx)
        LK_FAIL(ERROR_OUTOFMEMORY);
    ctx->NumberOfPackets = (SHORT)NumberOfPackets;
    ctx->StartFrame = (UINT)StartFrame;
    if (StartFrame)
        ctx->Flags = KISO_FLAG_SET_START_FRAME;
    *IsoContext = ctx;
    return TRUE;
}

KUSB_EXP BOOL KUSB_API IsoK_Free(PKISO_CONTEXT IsoContext)
{
    free(IsoContext);
    return TRUE;
}

KUSB_EXP BOOL KUSB_API IsoK_SetPackets(PKISO_CONTEXT IsoContext, INT PacketSize)
{
    if (!IsoContext || PacketSize <= 0)
        LK_FAIL(ERROR_INVALID_PARAMETER);
    UINT off = 0;
    for (int i = 0; i < IsoContext->NumberOfPackets; i++)
    {
        IsoContext->IsoPackets[i].Offset = off;
        IsoContext->IsoPackets[i].Length = (USHORT)PacketSize;
        IsoContext->IsoPackets[i].Status = 0;
        off += (UINT)PacketSize;
    }
    return TRUE;
}

KUSB_EXP BOOL KUSB_API IsoK_SetPacket(PKISO_CONTEXT IsoContext, INT PacketIndex, PKISO_PACKET IsoPacket)
{
    if (!IsoContext || !IsoPacket || PacketIndex < 0 || PacketIndex >= IsoContext->NumberOfPackets)
        LK_FAIL(ERROR_INVALID_PARAMETER);
    IsoContext->IsoPackets[PacketIndex] = *IsoPacket;
    return TRUE;
}

KUSB_EXP BOOL KUSB_API IsoK_GetPacket(PKISO_CONTEXT IsoContext, INT PacketIndex, PKISO_PACKET IsoPacket)
{
    if (!IsoContext || !IsoPacket || PacketIndex < 0 || PacketIndex >= IsoContext->NumberOfPackets)
        LK_FAIL(ERROR_INVALID_PARAMETER);
    *IsoPacket = IsoContext->IsoPackets[PacketIndex];
    return TRUE;
}

KUSB_EXP BOOL KUSB_API IsoK_EnumPackets(PKISO_CONTEXT IsoContext, KISO_ENUM_PACKETS_CB *EnumPackets, INT StartPacketIndex, PVOID UserState)
{
    if (!IsoContext || !EnumPackets)
        LK_FAIL(ERROR_INVALID_PARAMETER);
    for (int i = StartPacketIndex; i < IsoContext->NumberOfPackets; i++)
        if (!EnumPackets((UINT)i, &IsoContext->IsoPackets[i], UserState))
            break;
    return TRUE;
}

KUSB_EXP BOOL KUSB_API IsoK_ReUse(PKISO_CONTEXT IsoContext)
{
    if (!IsoContext)
        LK_FAIL(ERROR_INVALID_PARAMETER);
    SHORT npkts = IsoContext->NumberOfPackets;
    memset((UCHAR *)IsoContext + offsetof(KISO_CONTEXT, ErrorCount), 0,
           sizeof(KISO_CONTEXT) - offsetof(KISO_CONTEXT, ErrorCount) + (size_t)npkts * sizeof(KISO_PACKET));
    IsoContext->NumberOfPackets = npkts;
    return TRUE;
}

/* The submit half both iso APIs share. Async (Overlapped): queue an aop, which
 * takes ownership of pkts. Sync: run the submit; pkts then carries the
 * per-packet results for the caller to copy out and free. */
static BOOL lk_iso_submit(struct kusb *handle, int dir, int ep, PUCHAR buffer, int buflen,
                          struct iso_pkt *pkts, int np, PKISO_CONTEXT isoctx,
                          LPOVERLAPPED Overlapped, int *sync_rc)
{
    if (Overlapped)
    {
        struct aop *op = calloc(1, sizeof(*op));
        if (!op)
        {
            free(pkts);
            LK_FAIL(ERROR_OUTOFMEMORY);
        }
        op->is_iso = 1;
        op->dir = dir;
        op->ep = ep;
        op->buf = buffer;
        op->len = buflen;
        op->pkts = pkts;
        op->npkts = np;
        op->isoctx = isoctx;
        return kusb_queue(handle, op, Overlapped);
    }
    int total = 0;
    pthread_mutex_lock(&handle->iolock);
    *sync_rc = usbip_client_submit_iso(handle->fd, handle->devid, &handle->seq, dir, ep, 0, buffer, pkts, np, &total);
    pthread_mutex_unlock(&handle->iolock);
    return TRUE;
}

static BOOL lk_iso_pipe(struct kusb *handle, UCHAR PipeID, PUCHAR Buffer, UINT BufferLength,
                        LPOVERLAPPED Overlapped, PKISO_CONTEXT IsoContext)
{
    if (!handle || !IsoContext)
        LK_FAIL(ERROR_INVALID_PARAMETER);
    int np = IsoContext->NumberOfPackets;
    int dir = (PipeID & 0x80) ? USB_IN : USB_OUT;
    int ep = PipeID & 0x0f;

    struct iso_pkt *pkts = calloc((size_t)np, sizeof(*pkts));
    if (!pkts)
        LK_FAIL(ERROR_OUTOFMEMORY);
    for (int i = 0; i < np; i++)
    {
        pkts[i].offset = IsoContext->IsoPackets[i].Offset;
        pkts[i].length = IsoContext->IsoPackets[i].Length;
    }

    if (Overlapped)
        return lk_iso_submit(handle, dir, ep, Buffer, (int)BufferLength, pkts, np, IsoContext,
                             Overlapped, NULL);

    int rc = 0;
    lk_iso_submit(handle, dir, ep, Buffer, (int)BufferLength, pkts, np, NULL, NULL, &rc);
    for (int i = 0; i < np; i++)
    {
        IsoContext->IsoPackets[i].Length = (USHORT)pkts[i].actual_length;
        IsoContext->IsoPackets[i].Status = (USHORT)pkts[i].status;
    }
    free(pkts);
    if (rc < 0)
        LK_FAIL(ERROR_GEN_FAILURE);
    return TRUE;
}

KUSB_EXP BOOL KUSB_API UsbK_IsoReadPipe(KUSB_HANDLE InterfaceHandle, UCHAR PipeID, PUCHAR Buffer, UINT BufferLength, LPOVERLAPPED Overlapped, PKISO_CONTEXT IsoContext)
{
    return lk_iso_pipe(InterfaceHandle, PipeID, Buffer, BufferLength, Overlapped, IsoContext);
}

KUSB_EXP BOOL KUSB_API UsbK_IsoWritePipe(KUSB_HANDLE InterfaceHandle, UCHAR PipeID, PUCHAR Buffer, UINT BufferLength, LPOVERLAPPED Overlapped, PKISO_CONTEXT IsoContext)
{
    return lk_iso_pipe(InterfaceHandle, PipeID, Buffer, BufferLength, Overlapped, IsoContext);
}

/* ===================================================================== */
/* IsochK - SuperSpeed-style isochronous (ride the same iso submit)      */
/* ===================================================================== */
KUSB_EXP BOOL KUSB_API IsochK_Init(KISOCH_HANDLE *IsochHandle, KUSB_HANDLE InterfaceHandle, UCHAR PipeId,
                                   UINT MaxNumberOfPackets, PUCHAR TransferBuffer, UINT TransferBufferSize)
{
    if (!IsochHandle || !InterfaceHandle)
        LK_FAIL(ERROR_INVALID_PARAMETER);
    struct kisoch *k = calloc(1, sizeof(*k));
    if (!k)
        LK_FAIL(ERROR_OUTOFMEMORY);
    k->base.type = KLIB_HANDLE_TYPE_ISOCHK;
    k->h = InterfaceHandle;
    k->pipe_id = PipeId;
    k->buffer = TransferBuffer;
    k->buffer_size = TransferBufferSize;
    k->max_packets = MaxNumberOfPackets;
    k->num_packets = MaxNumberOfPackets;
    k->offset = calloc(MaxNumberOfPackets, sizeof(UINT));
    k->length = calloc(MaxNumberOfPackets, sizeof(UINT));
    k->status = calloc(MaxNumberOfPackets, sizeof(UINT));
    if (!k->offset || !k->length || !k->status)
    {
        free(k->offset);
        free(k->length);
        free(k->status);
        free(k);
        SetLastError(ERROR_OUTOFMEMORY);
        return FALSE;
    }
    *IsochHandle = k;
    return TRUE;
}

KUSB_EXP BOOL KUSB_API IsochK_Free(KISOCH_HANDLE IsochHandle)
{
    struct kisoch *k = IsochHandle;
    if (!k)
        return FALSE;
    free(k->offset);
    free(k->length);
    free(k->status);
    free(k);
    return TRUE;
}

KUSB_EXP BOOL KUSB_API IsochK_SetPacketOffsets(KISOCH_HANDLE IsochHandle, UINT PacketSize)
{
    struct kisoch *k = IsochHandle;
    if (!k || PacketSize == 0)
        LK_FAIL(ERROR_INVALID_PARAMETER);
    UINT off = 0;
    for (UINT i = 0; i < k->num_packets; i++)
    {
        k->offset[i] = off;
        k->length[i] = PacketSize;
        off += PacketSize;
    }
    return TRUE;
}

KUSB_EXP BOOL KUSB_API IsochK_SetPacket(KISOCH_HANDLE IsochHandle, UINT PacketIndex, UINT Offset, UINT Length, UINT Status)
{
    struct kisoch *k = IsochHandle;
    if (!k || PacketIndex >= k->num_packets)
        LK_FAIL(ERROR_INVALID_PARAMETER);
    k->offset[PacketIndex] = Offset;
    k->length[PacketIndex] = Length;
    k->status[PacketIndex] = Status;
    return TRUE;
}

KUSB_EXP BOOL KUSB_API IsochK_GetPacket(KISOCH_HANDLE IsochHandle, UINT PacketIndex, PUINT Offset, PUINT Length, PUINT Status)
{
    struct kisoch *k = IsochHandle;
    if (!k || PacketIndex >= k->num_packets)
        LK_FAIL(ERROR_INVALID_PARAMETER);
    if (Offset)
        *Offset = k->offset[PacketIndex];
    if (Length)
        *Length = k->length[PacketIndex];
    if (Status)
        *Status = k->status[PacketIndex];
    return TRUE;
}

KUSB_EXP BOOL KUSB_API IsochK_EnumPackets(KISOCH_HANDLE IsochHandle, KISOCH_ENUM_PACKETS_CB *EnumPackets, UINT StartPacketIndex, PVOID UserState)
{
    struct kisoch *k = IsochHandle;
    if (!k || !EnumPackets)
        LK_FAIL(ERROR_INVALID_PARAMETER);
    for (UINT i = StartPacketIndex; i < k->num_packets; i++)
        if (!EnumPackets(i, &k->offset[i], &k->length[i], &k->status[i], UserState))
            break;
    return TRUE;
}

KUSB_EXP BOOL KUSB_API IsochK_GetNumberOfPackets(KISOCH_HANDLE IsochHandle, PUINT NumberOfPackets)
{
    struct kisoch *k = IsochHandle;
    if (!k || !NumberOfPackets)
        LK_FAIL(ERROR_INVALID_PARAMETER);
    *NumberOfPackets = k->num_packets;
    return TRUE;
}

KUSB_EXP BOOL KUSB_API IsochK_SetNumberOfPackets(KISOCH_HANDLE IsochHandle, UINT NumberOfPackets)
{
    struct kisoch *k = IsochHandle;
    if (!k || NumberOfPackets > k->max_packets)
        LK_FAIL(ERROR_INVALID_PARAMETER);
    k->num_packets = NumberOfPackets;
    return TRUE;
}

KUSB_EXP BOOL KUSB_API IsochK_CalcPacketInformation(BOOL IsHighSpeed, PWINUSB_PIPE_INFORMATION_EX PipeInformationEx, PKISOCH_PACKET_INFORMATION PacketInformation)
{
    if (!PipeInformationEx || !PacketInformation)
        LK_FAIL(ERROR_INVALID_PARAMETER);
    UINT mps = PipeInformationEx->MaximumBytesPerInterval ? PipeInformationEx->MaximumBytesPerInterval : PipeInformationEx->MaximumPacketSize;
    UINT interval = PipeInformationEx->Interval ? PipeInformationEx->Interval : 1;
    UINT frames_per_ms = IsHighSpeed ? 8 : 1; /* HS microframes vs FS frames */
    UINT period = (1u << (interval - 1));
    PacketInformation->PacketsPerFrame = frames_per_ms;
    PacketInformation->PollingPeriodMicroseconds = (period * 1000u) / frames_per_ms;
    PacketInformation->BytesPerMillisecond = mps * frames_per_ms;
    return TRUE;
}

static BOOL lk_isoch_xfer(KISOCH_HANDLE IsochHandle, UINT DataLength, PUINT FrameNumber, UINT NumberOfPackets, LPOVERLAPPED Overlapped)
{
    struct kisoch *k = IsochHandle;
    if (!k)
        LK_FAIL(ERROR_INVALID_PARAMETER);
    UINT np = NumberOfPackets ? NumberOfPackets : k->num_packets;
    if (np > k->num_packets)
        np = k->num_packets;
    int dir = (k->pipe_id & 0x80) ? USB_IN : USB_OUT;
    int ep = k->pipe_id & 0x0f;
    (void)DataLength;
    (void)FrameNumber;

    struct iso_pkt *pkts = calloc((size_t)np, sizeof(*pkts));
    if (!pkts)
        LK_FAIL(ERROR_OUTOFMEMORY);
    for (UINT i = 0; i < np; i++)
    {
        pkts[i].offset = k->offset[i];
        pkts[i].length = k->length[i];
    }

    if (Overlapped)
        return lk_iso_submit(k->h, dir, ep, k->buffer, (int)k->buffer_size, pkts, (int)np,
                             NULL, Overlapped, NULL);

    int rc = 0;
    lk_iso_submit(k->h, dir, ep, k->buffer, (int)k->buffer_size, pkts, (int)np, NULL, NULL, &rc);
    for (UINT i = 0; i < np; i++)
    {
        k->length[i] = pkts[i].actual_length;
        k->status[i] = pkts[i].status;
    }
    free(pkts);
    if (rc < 0)
        LK_FAIL(ERROR_GEN_FAILURE);
    return TRUE;
}

KUSB_EXP BOOL KUSB_API UsbK_IsochReadPipe(KISOCH_HANDLE IsochHandle, UINT DataLength, PUINT FrameNumber, UINT NumberOfPackets, LPOVERLAPPED Overlapped)
{
    return lk_isoch_xfer(IsochHandle, DataLength, FrameNumber, NumberOfPackets, Overlapped);
}

KUSB_EXP BOOL KUSB_API UsbK_IsochWritePipe(KISOCH_HANDLE IsochHandle, UINT DataLength, PUINT FrameNumber, UINT NumberOfPackets, LPOVERLAPPED Overlapped)
{
    return lk_isoch_xfer(IsochHandle, DataLength, FrameNumber, NumberOfPackets, Overlapped);
}

/* ===================================================================== */
/* OvlK - overlapped pool                                                */
/* ===================================================================== */
KUSB_EXP BOOL KUSB_API OvlK_Init(KOVL_POOL_HANDLE *PoolHandle, KUSB_HANDLE UsbHandle, INT MaxOverlappedCount, KOVL_POOL_FLAG Flags)
{
    (void)Flags;
    if (!PoolHandle || MaxOverlappedCount <= 0)
        LK_FAIL(ERROR_INVALID_PARAMETER);
    struct kovl_pool *pool = calloc(1, sizeof(*pool));
    if (!pool)
        LK_FAIL(ERROR_OUTOFMEMORY);
    pool->base.type = KLIB_HANDLE_TYPE_OVLPOOLK;
    pool->h = UsbHandle;
    pool->max = MaxOverlappedCount;
    pthread_mutex_init(&pool->lock, NULL);
    for (int i = 0; i < MaxOverlappedCount; i++)
    {
        struct kovl *ovl = calloc(1, sizeof(*ovl));
        if (!ovl)
            break;
        ovl->ovl.hEvent = CreateEvent(NULL, TRUE, FALSE, NULL); /* manual-reset */
        ovl->pool = pool;
        ovl->next_all = pool->all;
        pool->all = ovl;
        ovl->next_free = pool->free_list;
        pool->free_list = ovl;
    }
    *PoolHandle = pool;
    return TRUE;
}

KUSB_EXP BOOL KUSB_API OvlK_Free(KOVL_POOL_HANDLE PoolHandle)
{
    struct kovl_pool *pool = PoolHandle;
    if (!pool)
        return FALSE;
    struct kovl *ovl = pool->all;
    while (ovl)
    {
        struct kovl *next = ovl->next_all;
        if (ovl->ovl.hEvent)
            CloseHandle(ovl->ovl.hEvent);
        free(ovl);
        ovl = next;
    }
    pthread_mutex_destroy(&pool->lock);
    free(pool);
    return TRUE;
}

KUSB_EXP BOOL KUSB_API OvlK_Acquire(KOVL_HANDLE *OverlappedK, KOVL_POOL_HANDLE PoolHandle)
{
    struct kovl_pool *pool = PoolHandle;
    if (!OverlappedK || !pool)
        LK_FAIL(ERROR_INVALID_PARAMETER);
    pthread_mutex_lock(&pool->lock);
    struct kovl *ovl = pool->free_list;
    if (ovl)
        pool->free_list = ovl->next_free;
    pthread_mutex_unlock(&pool->lock);
    if (!ovl)
        LK_FAIL(ERROR_NO_MORE_ITEMS);
    ovl->ovl.Internal = 0;
    ovl->ovl.InternalHigh = 0;
    if (ovl->ovl.hEvent)
        ResetEvent(ovl->ovl.hEvent);
    *OverlappedK = ovl;
    return TRUE;
}

KUSB_EXP BOOL KUSB_API OvlK_Release(KOVL_HANDLE OverlappedK)
{
    struct kovl *ovl = OverlappedK;
    if (!ovl || !ovl->pool)
        LK_FAIL(ERROR_INVALID_PARAMETER);
    struct kovl_pool *pool = ovl->pool;
    pthread_mutex_lock(&pool->lock);
    ovl->next_free = pool->free_list;
    pool->free_list = ovl;
    pthread_mutex_unlock(&pool->lock);
    return TRUE;
}

KUSB_EXP HANDLE KUSB_API OvlK_GetEventHandle(KOVL_HANDLE OverlappedK)
{
    struct kovl *ovl = OverlappedK;
    return ovl ? ovl->ovl.hEvent : NULL;
}

KUSB_EXP BOOL KUSB_API OvlK_ReUse(KOVL_HANDLE OverlappedK)
{
    struct kovl *ovl = OverlappedK;
    if (!ovl)
        LK_FAIL(ERROR_INVALID_PARAMETER);
    ovl->ovl.Internal = 0;
    ovl->ovl.InternalHigh = 0;
    if (ovl->ovl.hEvent)
        ResetEvent(ovl->ovl.hEvent);
    return TRUE;
}

KUSB_EXP BOOL KUSB_API OvlK_IsComplete(KOVL_HANDLE OverlappedK)
{
    struct kovl *ovl = OverlappedK;
    if (!ovl)
        return FALSE;
    return ovl->ovl.Internal != STATUS_PENDING ? TRUE : FALSE;
}

static BOOL lk_ovl_wait(struct kovl *ovl, INT TimeoutMS, KOVL_WAIT_FLAG WaitFlags, PUINT TransferredLength)
{
    if (!ovl)
        LK_FAIL(ERROR_INVALID_PARAMETER);
    DWORD ms = (TimeoutMS < 0) ? INFINITE : (DWORD)TimeoutMS;
    DWORD waited = ovl->ovl.hEvent ? WaitForSingleObject(ovl->ovl.hEvent, ms) : WAIT_OBJECT_0;
    if (!ovl->ovl.hEvent)
    {
        /* no event: spin briefly for completion */
        DWORD spun = 0;
        while (ovl->ovl.Internal == STATUS_PENDING && (ms == INFINITE || spun < ms))
        {
            Sleep(1);
            spun++;
        }
        waited = (ovl->ovl.Internal == STATUS_PENDING) ? WAIT_TIMEOUT : WAIT_OBJECT_0;
    }
    BOOL ok;
    if (waited == WAIT_OBJECT_0)
    {
        if (TransferredLength)
            *TransferredLength = (UINT)ovl->ovl.InternalHigh;
        ok = (ovl->ovl.Internal == 0);
        if (!ok)
            SetLastError(ERROR_GEN_FAILURE);
        KOVL_WAIT_FLAG release_flag = ok ? KOVL_WAIT_FLAG_RELEASE_ON_SUCCESS : KOVL_WAIT_FLAG_RELEASE_ON_FAIL;
        if (WaitFlags & release_flag)
            OvlK_Release(ovl);
    }
    else
    {
        SetLastError(ERROR_SEM_TIMEOUT);
        if (WaitFlags & KOVL_WAIT_FLAG_RELEASE_ON_TIMEOUT)
            OvlK_Release(ovl);
        ok = FALSE;
    }
    return ok;
}

KUSB_EXP BOOL KUSB_API OvlK_Wait(KOVL_HANDLE OverlappedK, INT TimeoutMS, KOVL_WAIT_FLAG WaitFlags, PUINT TransferredLength)
{
    return lk_ovl_wait(OverlappedK, TimeoutMS, WaitFlags, TransferredLength);
}

KUSB_EXP BOOL KUSB_API OvlK_WaitOrCancel(KOVL_HANDLE OverlappedK, INT TimeoutMS, PUINT TransferredLength)
{
    return lk_ovl_wait(OverlappedK, TimeoutMS, KOVL_WAIT_FLAG_CANCEL_ON_TIMEOUT, TransferredLength);
}

KUSB_EXP BOOL KUSB_API OvlK_WaitAndRelease(KOVL_HANDLE OverlappedK, INT TimeoutMS, PUINT TransferredLength)
{
    return lk_ovl_wait(OverlappedK, TimeoutMS, KOVL_WAIT_FLAG_RELEASE_ALWAYS, TransferredLength);
}

KUSB_EXP BOOL KUSB_API OvlK_WaitOldest(KOVL_POOL_HANDLE PoolHandle, KOVL_HANDLE *OverlappedK, INT TimeoutMS, KOVL_WAIT_FLAG WaitFlags, PUINT TransferredLength)
{
    /* Single-worker model: there is no global submit order to track, so this waits
     * on the supplied handle (when given) like OvlK_Wait. */
    if (OverlappedK && *OverlappedK)
        return lk_ovl_wait(*OverlappedK, TimeoutMS, WaitFlags, TransferredLength);
    (void)PoolHandle;
    SetLastError(ERROR_NO_MORE_ITEMS);
    return FALSE;
}

/* ===================================================================== */
/* StmK - pipe streams (background thread over synchronous pipe I/O)      */
/* ===================================================================== */
static void *kstm_thread(void *arg)
{
    struct kstm *stream = arg;
    UCHAR pipe = stream->info.PipeID;
    int interval = lk_pipe_interval(stream->info.UsbHandle, pipe);
    int n_slots = stream->n_slots;

    for (;;)
    {
        /* IN consumes a free slot, OUT consumes a ready (queued) slot */
        pthread_mutex_lock(&stream->lock);
        if (stream->dir == USB_IN)
            while (!stream->stop && stream->free_count == 0)
                pthread_cond_wait(&stream->cond, &stream->lock);
        else
            while (!stream->stop && stream->ready_count == 0)
                pthread_cond_wait(&stream->cond, &stream->lock);
        if (stream->stop)
        {
            pthread_mutex_unlock(&stream->lock);
            break;
        }
        int slot = (stream->dir == USB_IN)
                       ? kstm_pop(stream->free_q, &stream->free_head, &stream->free_count, n_slots)
                       : kstm_pop(stream->ready_q, &stream->ready_head, &stream->ready_count, n_slots);
        pthread_mutex_unlock(&stream->lock);

        KSTM_XFER_CONTEXT *xc = &stream->slots[slot];
        int act = 0;
        if (stream->have_cb && stream->cb.Submit)
            stream->cb.Submit(&stream->info, xc, slot, NULL);
        int rc = lk_submit(stream->info.UsbHandle, stream->dir, pipe & 0x0f, NULL, interval,
                           xc->Buffer, (stream->dir == USB_IN) ? stream->max_xfer : xc->TransferLength, &act);
        if (rc < 0)
        {
            if (stream->have_cb && stream->cb.Error)
                stream->cb.Error(&stream->info, xc, slot, rc);
            /* return the slot to its origin ring so the stream keeps cycling */
            pthread_mutex_lock(&stream->lock);
            if (stream->dir == USB_IN)
                kstm_push(stream->free_q, &stream->free_head, &stream->free_count, n_slots, slot);
            else
                kstm_push(stream->free_q, &stream->free_head, &stream->free_count, n_slots, slot);
            pthread_cond_broadcast(&stream->cond);
            pthread_mutex_unlock(&stream->lock);
            continue;
        }
        xc->TransferLength = act;
        if (stream->have_cb && stream->cb.Complete)
            stream->cb.Complete(&stream->info, xc, slot, 0);

        pthread_mutex_lock(&stream->lock);
        if (stream->dir == USB_IN)
            kstm_push(stream->ready_q, &stream->ready_head, &stream->ready_count, n_slots, slot);
        else
            kstm_push(stream->free_q, &stream->free_head, &stream->free_count, n_slots, slot);
        pthread_cond_broadcast(&stream->cond);
        pthread_mutex_unlock(&stream->lock);
    }
    return NULL;
}

KUSB_EXP BOOL KUSB_API StmK_Init(KSTM_HANDLE *StreamHandle, KUSB_HANDLE UsbHandle, UCHAR PipeID, INT MaxTransferSize,
                                 INT MaxPendingTransfers, INT MaxPendingIO, PKSTM_CALLBACK Callbacks, KSTM_FLAG Flags)
{
    if (!StreamHandle || !UsbHandle || MaxTransferSize <= 0 || MaxPendingTransfers <= 0)
        LK_FAIL(ERROR_INVALID_PARAMETER);
    struct kstm *stream = calloc(1, sizeof(*stream));
    if (!stream)
        LK_FAIL(ERROR_OUTOFMEMORY);
    stream->base.type = KLIB_HANDLE_TYPE_STMK;
    stream->flags = Flags;
    stream->dir = (PipeID & 0x80) ? USB_IN : USB_OUT;
    stream->max_xfer = MaxTransferSize;
    stream->n_slots = MaxPendingTransfers;
    stream->free_count = MaxPendingTransfers;
    pthread_mutex_init(&stream->lock, NULL);
    pthread_cond_init(&stream->cond, NULL);
    stream->info.UsbHandle = UsbHandle;
    stream->info.PipeID = PipeID;
    stream->info.MaxPendingTransfers = MaxPendingTransfers;
    stream->info.MaxTransferSize = MaxTransferSize;
    stream->info.MaxPendingIO = MaxPendingIO;
    stream->info.StreamHandle = stream;
    LibK_LoadDriverAPI(&stream->info.DriverAPI, KUSB_DRVID_LIBUSBK);
    if (Callbacks)
    {
        stream->cb = *Callbacks;
        stream->have_cb = 1;
    }

    stream->slots = calloc((size_t)stream->n_slots, sizeof(*stream->slots));
    stream->slot_buf = calloc((size_t)stream->n_slots, sizeof(*stream->slot_buf));
    stream->free_q = calloc((size_t)stream->n_slots, sizeof(*stream->free_q));
    stream->ready_q = calloc((size_t)stream->n_slots, sizeof(*stream->ready_q));
    if (!stream->slots || !stream->slot_buf || !stream->free_q || !stream->ready_q)
        goto oom;
    for (int i = 0; i < stream->n_slots; i++)
    {
        stream->slot_buf[i] = malloc((size_t)MaxTransferSize);
        if (!stream->slot_buf[i])
            goto oom;
        stream->slots[i].Buffer = stream->slot_buf[i];
        stream->slots[i].BufferSize = MaxTransferSize;
        stream->free_q[i] = i; /* every slot starts free */
    }
    stream->free_count = stream->n_slots;
    *StreamHandle = stream;
    return TRUE;
oom:
    if (stream->slot_buf)
        for (int i = 0; i < stream->n_slots; i++)
            free(stream->slot_buf[i]);
    free(stream->slots);
    free(stream->slot_buf);
    free(stream->free_q);
    free(stream->ready_q);
    pthread_mutex_destroy(&stream->lock);
    pthread_cond_destroy(&stream->cond);
    free(stream);
    SetLastError(ERROR_OUTOFMEMORY);
    return FALSE;
}

KUSB_EXP BOOL KUSB_API StmK_Start(KSTM_HANDLE StreamHandle)
{
    struct kstm *stream = StreamHandle;
    if (!stream)
        LK_FAIL(ERROR_INVALID_PARAMETER);
    if (stream->running)
        return TRUE;
    if (stream->have_cb && stream->cb.Started)
        for (int i = 0; i < stream->n_slots; i++)
            stream->cb.Started(&stream->info, &stream->slots[i], i);
    stream->stop = 0;
    if (pthread_create(&stream->thread, NULL, kstm_thread, stream) != 0)
        LK_FAIL(ERROR_OUTOFMEMORY);
    stream->running = 1;
    return TRUE;
}

KUSB_EXP BOOL KUSB_API StmK_Stop(KSTM_HANDLE StreamHandle, INT TimeoutCancelMS)
{
    struct kstm *stream = StreamHandle;
    if (!stream)
        LK_FAIL(ERROR_INVALID_PARAMETER);
    (void)TimeoutCancelMS;
    if (stream->running)
    {
        pthread_mutex_lock(&stream->lock);
        stream->stop = 1;
        pthread_cond_broadcast(&stream->cond);
        pthread_mutex_unlock(&stream->lock);
        pthread_join(stream->thread, NULL);
        stream->running = 0;
    }
    if (stream->have_cb && stream->cb.Stopped)
        for (int i = 0; i < stream->n_slots; i++)
            stream->cb.Stopped(&stream->info, &stream->slots[i], i);
    return TRUE;
}

KUSB_EXP BOOL KUSB_API StmK_Read(KSTM_HANDLE StreamHandle, PUCHAR Buffer, INT Offset, INT Length, PUINT TransferredLength)
{
    struct kstm *stream = StreamHandle;
    if (!stream || stream->dir != USB_IN)
        LK_FAIL(ERROR_INVALID_PARAMETER);
    int n_slots = stream->n_slots;
    pthread_mutex_lock(&stream->lock);
    while (stream->ready_count == 0 && !stream->stop)
        pthread_cond_wait(&stream->cond, &stream->lock);
    if (stream->ready_count == 0)
    {
        pthread_mutex_unlock(&stream->lock);
        if (TransferredLength)
            *TransferredLength = 0;
        return FALSE;
    }
    int slot = kstm_pop(stream->ready_q, &stream->ready_head, &stream->ready_count, n_slots);
    pthread_mutex_unlock(&stream->lock);

    KSTM_XFER_CONTEXT *xc = &stream->slots[slot];
    int got = xc->TransferLength;
    if (got > Length)
        got = Length;
    memcpy(Buffer + Offset, xc->Buffer, (size_t)got);
    if (TransferredLength)
        *TransferredLength = (UINT)got;

    pthread_mutex_lock(&stream->lock);
    kstm_push(stream->free_q, &stream->free_head, &stream->free_count, n_slots, slot); /* slot reusable for the next read */
    pthread_cond_broadcast(&stream->cond);
    pthread_mutex_unlock(&stream->lock);
    return TRUE;
}

KUSB_EXP BOOL KUSB_API StmK_Write(KSTM_HANDLE StreamHandle, PUCHAR Buffer, INT Offset, INT Length, PUINT TransferredLength)
{
    struct kstm *stream = StreamHandle;
    if (!stream || stream->dir != USB_OUT)
        LK_FAIL(ERROR_INVALID_PARAMETER);
    int n_slots = stream->n_slots;
    if (Length > stream->max_xfer)
        Length = stream->max_xfer;
    pthread_mutex_lock(&stream->lock);
    while (stream->free_count == 0 && !stream->stop)
        pthread_cond_wait(&stream->cond, &stream->lock);
    if (stream->free_count == 0)
    {
        pthread_mutex_unlock(&stream->lock);
        return FALSE;
    }
    int slot = kstm_pop(stream->free_q, &stream->free_head, &stream->free_count, n_slots);
    KSTM_XFER_CONTEXT *xc = &stream->slots[slot];
    memcpy(xc->Buffer, Buffer + Offset, (size_t)Length);
    xc->TransferLength = Length;
    kstm_push(stream->ready_q, &stream->ready_head, &stream->ready_count, n_slots, slot); /* hand to the writer thread */
    pthread_cond_broadcast(&stream->cond);
    pthread_mutex_unlock(&stream->lock);
    if (TransferredLength)
        *TransferredLength = (UINT)Length;
    return TRUE;
}

KUSB_EXP BOOL KUSB_API StmK_Free(KSTM_HANDLE StreamHandle)
{
    struct kstm *stream = StreamHandle;
    if (!stream)
        return FALSE;
    StmK_Stop(stream, 0);
    if (stream->slot_buf)
        for (int i = 0; i < stream->n_slots; i++)
            free(stream->slot_buf[i]);
    free(stream->slots);
    free(stream->slot_buf);
    free(stream->free_q);
    free(stream->ready_q);
    pthread_mutex_destroy(&stream->lock);
    pthread_cond_destroy(&stream->cond);
    free(stream);
    return TRUE;
}

/* ===================================================================== */
/* HotK - hot plug (initial enumeration only; no live events over USB/IP)*/
/* ===================================================================== */
KUSB_EXP BOOL KUSB_API HotK_Init(KHOT_HANDLE *Handle, PKHOT_PARAMS InitParams)
{
    if (!Handle || !InitParams)
        LK_FAIL(ERROR_INVALID_PARAMETER);
    struct khot *hot = calloc(1, sizeof(*hot));
    if (!hot)
        LK_FAIL(ERROR_OUTOFMEMORY);
    hot->base.type = KLIB_HANDLE_TYPE_HOTK;
    hot->params = *InitParams;

    pthread_mutex_lock(&g_lock);
    hot->next_all = g_hot_list;
    g_hot_list = hot;
    pthread_mutex_unlock(&g_lock);
    *Handle = hot;

    /* One-shot arrival notification for currently-present matches. USB/IP has no
     * asynchronous arrival/removal, so there are no further callbacks. */
    if (InitParams->OnHotPlug)
    {
        KLST_HANDLE list;
        if (LstK_Init(&list, KLST_FLAG_NONE))
        {
            KLST_DEVINFO_HANDLE di;
            while (LstK_MoveNext(list, &di))
            {
                if (InitParams->PatternMatch.DeviceID[0] && !lk_wild(InitParams->PatternMatch.DeviceID, di->DeviceID))
                    continue;
                InitParams->OnHotPlug(hot, di, KLST_SYNC_FLAG_ADDED);
            }
            LstK_Free(list);
        }
    }
    return TRUE;
}

KUSB_EXP BOOL KUSB_API HotK_Free(KHOT_HANDLE Handle)
{
    struct khot *hot = Handle;
    if (!hot)
        return FALSE;
    pthread_mutex_lock(&g_lock);
    struct khot **pp = &g_hot_list;
    while (*pp)
    {
        if (*pp == hot)
        {
            *pp = hot->next_all;
            break;
        }
        pp = &(*pp)->next_all;
    }
    pthread_mutex_unlock(&g_lock);
    if (hot->base.cleanup)
        hot->base.cleanup(Handle, KLIB_HANDLE_TYPE_HOTK, hot->base.user_ctx);
    free(hot);
    return TRUE;
}

KUSB_EXP VOID KUSB_API HotK_FreeAll(VOID)
{
    for (;;)
    {
        pthread_mutex_lock(&g_lock);
        struct khot *hot = g_hot_list;
        pthread_mutex_unlock(&g_lock);
        if (!hot)
            break;
        HotK_Free(hot);
    }
}

/* ===================================================================== */
/* LUsb0 - libusb0-compat thin shims                                     */
/* ===================================================================== */
KUSB_EXP BOOL KUSB_API LUsb0_ControlTransfer(KUSB_HANDLE InterfaceHandle, WINUSB_SETUP_PACKET SetupPacket,
                                             PUCHAR Buffer, UINT BufferLength, PUINT LengthTransferred, LPOVERLAPPED Overlapped)
{
    return UsbK_ControlTransfer(InterfaceHandle, SetupPacket, Buffer, BufferLength, LengthTransferred, Overlapped);
}

KUSB_EXP BOOL KUSB_API LUsb0_SetConfiguration(KUSB_HANDLE InterfaceHandle, UCHAR ConfigurationNumber)
{
    return UsbK_SetConfiguration(InterfaceHandle, ConfigurationNumber);
}

/* ===================================================================== */
/* WinUsb_* - the WinUSB-named face of the same API                      */
/*                                                                       */
/* libusbK exports these so a program written against winusb.h links     */
/* against libusbK.dll unchanged; each is a plain forward to its UsbK_*  */
/* twin, exactly as upstream's lusbk_wrapper_winusb.c does.              */
/* ===================================================================== */
KUSB_EXP BOOL KUSB_API WinUsb_Initialize(HANDLE DeviceHandle, KUSB_HANDLE *InterfaceHandle)
{
    return UsbK_Initialize(DeviceHandle, InterfaceHandle);
}

KUSB_EXP BOOL KUSB_API WinUsb_Free(KUSB_HANDLE InterfaceHandle)
{
    return UsbK_Free(InterfaceHandle);
}

KUSB_EXP BOOL KUSB_API WinUsb_GetAssociatedInterface(KUSB_HANDLE InterfaceHandle, UCHAR AssociatedInterfaceIndex,
                                                     KUSB_HANDLE *AssociatedInterfaceHandle)
{
    return UsbK_GetAssociatedInterface(InterfaceHandle, AssociatedInterfaceIndex, AssociatedInterfaceHandle);
}

KUSB_EXP BOOL KUSB_API WinUsb_GetDescriptor(KUSB_HANDLE InterfaceHandle, UCHAR DescriptorType, UCHAR Index,
                                            USHORT LanguageID, PUCHAR Buffer, UINT BufferLength, PUINT LengthTransferred)
{
    return UsbK_GetDescriptor(InterfaceHandle, DescriptorType, Index, LanguageID, Buffer, BufferLength, LengthTransferred);
}

KUSB_EXP BOOL KUSB_API WinUsb_QueryInterfaceSettings(KUSB_HANDLE InterfaceHandle, UCHAR AltSettingIndex,
                                                     PUSB_INTERFACE_DESCRIPTOR UsbAltInterfaceDescriptor)
{
    return UsbK_QueryInterfaceSettings(InterfaceHandle, AltSettingIndex, UsbAltInterfaceDescriptor);
}

KUSB_EXP BOOL KUSB_API WinUsb_QueryDeviceInformation(KUSB_HANDLE InterfaceHandle, UINT InformationType,
                                                     PUINT BufferLength, PUCHAR Buffer)
{
    return UsbK_QueryDeviceInformation(InterfaceHandle, InformationType, BufferLength, Buffer);
}

KUSB_EXP BOOL KUSB_API WinUsb_SetCurrentAlternateSetting(KUSB_HANDLE InterfaceHandle, UCHAR AltSettingNumber)
{
    return UsbK_SetCurrentAlternateSetting(InterfaceHandle, AltSettingNumber);
}

KUSB_EXP BOOL KUSB_API WinUsb_GetCurrentAlternateSetting(KUSB_HANDLE InterfaceHandle, PUCHAR AltSettingNumber)
{
    return UsbK_GetCurrentAlternateSetting(InterfaceHandle, AltSettingNumber);
}

KUSB_EXP BOOL KUSB_API WinUsb_QueryPipe(KUSB_HANDLE InterfaceHandle, UCHAR AltSettingNumber, UCHAR PipeIndex,
                                        PWINUSB_PIPE_INFORMATION PipeInformation)
{
    return UsbK_QueryPipe(InterfaceHandle, AltSettingNumber, PipeIndex, PipeInformation);
}

KUSB_EXP BOOL KUSB_API WinUsb_QueryPipeEx(KUSB_HANDLE InterfaceHandle, UCHAR AltSettingNumber, UCHAR PipeIndex,
                                          PWINUSB_PIPE_INFORMATION_EX PipeInformationEx)
{
    return UsbK_QueryPipeEx(InterfaceHandle, AltSettingNumber, PipeIndex, PipeInformationEx);
}

KUSB_EXP BOOL KUSB_API WinUsb_SetPipePolicy(KUSB_HANDLE InterfaceHandle, UCHAR PipeID, UINT PolicyType,
                                            UINT ValueLength, PVOID Value)
{
    return UsbK_SetPipePolicy(InterfaceHandle, PipeID, PolicyType, ValueLength, Value);
}

KUSB_EXP BOOL KUSB_API WinUsb_GetPipePolicy(KUSB_HANDLE InterfaceHandle, UCHAR PipeID, UINT PolicyType,
                                            PUINT ValueLength, PVOID Value)
{
    return UsbK_GetPipePolicy(InterfaceHandle, PipeID, PolicyType, ValueLength, Value);
}

KUSB_EXP BOOL KUSB_API WinUsb_ReadPipe(KUSB_HANDLE InterfaceHandle, UCHAR PipeID, PUCHAR Buffer, UINT BufferLength,
                                       PUINT LengthTransferred, LPOVERLAPPED Overlapped)
{
    return UsbK_ReadPipe(InterfaceHandle, PipeID, Buffer, BufferLength, LengthTransferred, Overlapped);
}

KUSB_EXP BOOL KUSB_API WinUsb_WritePipe(KUSB_HANDLE InterfaceHandle, UCHAR PipeID, PUCHAR Buffer, UINT BufferLength,
                                        PUINT LengthTransferred, LPOVERLAPPED Overlapped)
{
    return UsbK_WritePipe(InterfaceHandle, PipeID, Buffer, BufferLength, LengthTransferred, Overlapped);
}

KUSB_EXP BOOL KUSB_API WinUsb_ControlTransfer(KUSB_HANDLE InterfaceHandle, WINUSB_SETUP_PACKET SetupPacket,
                                              PUCHAR Buffer, UINT BufferLength, PUINT LengthTransferred, LPOVERLAPPED Overlapped)
{
    return UsbK_ControlTransfer(InterfaceHandle, SetupPacket, Buffer, BufferLength, LengthTransferred, Overlapped);
}

KUSB_EXP BOOL KUSB_API WinUsb_ResetPipe(KUSB_HANDLE InterfaceHandle, UCHAR PipeID)
{
    return UsbK_ResetPipe(InterfaceHandle, PipeID);
}

KUSB_EXP BOOL KUSB_API WinUsb_AbortPipe(KUSB_HANDLE InterfaceHandle, UCHAR PipeID)
{
    return UsbK_AbortPipe(InterfaceHandle, PipeID);
}

KUSB_EXP BOOL KUSB_API WinUsb_FlushPipe(KUSB_HANDLE InterfaceHandle, UCHAR PipeID)
{
    return UsbK_FlushPipe(InterfaceHandle, PipeID);
}

KUSB_EXP BOOL KUSB_API WinUsb_SetPowerPolicy(KUSB_HANDLE InterfaceHandle, UINT PolicyType, UINT ValueLength, PVOID Value)
{
    return UsbK_SetPowerPolicy(InterfaceHandle, PolicyType, ValueLength, Value);
}

KUSB_EXP BOOL KUSB_API WinUsb_GetPowerPolicy(KUSB_HANDLE InterfaceHandle, UINT PolicyType, PUINT ValueLength, PVOID Value)
{
    return UsbK_GetPowerPolicy(InterfaceHandle, PolicyType, ValueLength, Value);
}

KUSB_EXP BOOL KUSB_API WinUsb_GetOverlappedResult(KUSB_HANDLE InterfaceHandle, LPOVERLAPPED Overlapped,
                                                  PUINT lpNumberOfBytesTransferred, BOOL bWait)
{
    return UsbK_GetOverlappedResult(InterfaceHandle, Overlapped, lpNumberOfBytesTransferred, bWait);
}
