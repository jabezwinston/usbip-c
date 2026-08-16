/**
 * Copyright (C) 2026 Jabez Winston
 *
 * SPDX-License-Identifier: MIT
 *
 * Date : 04-June-2026
 *
 * usbip.c - the USB/IP transport. Wire framing (big-endian), the import/devlist
 * handshake, and a threaded accept/serve loop. CLASS-FREE and device-agnostic:
 * it shuttles URBs between the socket and usbip_device_dispatch().
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "os_compat.h"
#include "usbip_device_internal.h"

#ifdef _WIN32
/* One-time Winsock init. Idempotent across the accept/serve threads and the
 * client (usbip_host.c) path; matched by an atexit-registered WSACleanup. */
static pthread_once_t g_wsa_once = PTHREAD_ONCE_INIT;
static void wsa_cleanup(void) { WSACleanup(); }
static void wsa_init(void)
{
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) == 0)
        atexit(wsa_cleanup);
}
void usbip_net_startup(void) { pthread_once(&g_wsa_once, wsa_init); }
#endif

struct usb_transport
{
    char host[64];
    int port;
};

usb_transport *usbip_transport(const char *host, int port)
{
    usb_transport *transport = calloc(1, sizeof(*transport));
    if (host)
        snprintf(transport->host, sizeof(transport->host), "%s", host);
    transport->port = port ? port : USBIP_DEFAULT_PORT;
    return transport;
}

usb_transport *usbip_loopback(void)
{
    return usbip_transport("127.0.0.1", USBIP_DEFAULT_PORT);
}

void usbip_transport_free(usb_transport *transport) 
{ 
    free(transport); 
}

void usbip_transport_params(usb_transport *transport, const char **host, int *port)
{
    *host = transport->host[0] ? transport->host : NULL;
    *port = transport->port;
}

/* The wire is big-endian throughout; usb_get_be32()/usb_put_be32() come from
 * usb_byteorder.h. */

/* USB bmAttributes (ctrl0,iso1,bulk2,intr3) -> usbmon numbering
 * (iso0,intr1,ctrl2,bulk3); ep0 is always control. */
static int bmattr_to_usbmon(int bmattr, int ep)
{
    static const int map[4] = {
        USBMON_XFER_CTRL, USBMON_XFER_ISO,
        USBMON_XFER_BULK, USBMON_XFER_INTR
    };

    if (ep == 0)
        return USBMON_XFER_CTRL;

    return map[bmattr & 3];
}

/* usbmon transfer_type for a device endpoint. */
static int pcap_xfer_dev(usbip_device *dev, int ep, int dir)
{
    if (ep == 0)
        return USBMON_XFER_CTRL;

    usbip_ep *endpoint = (ep < 16) ? dev->ep_map[ep][dir] : NULL;
    return endpoint ? bmattr_to_usbmon(endpoint->type, ep) : USBMON_XFER_BULK;
}
#define EP_ADDR(ep, dir) ((int)(ep) | ((dir) == USB_IN ? USB_EP_ADDR_DIR_IN : 0))

/* Client-side endpoint-type map, for capture only. The USB/IP header carries no
 * transfer type, so learn each endpoint's from the CONFIGURATION descriptors read
 * on EP0. Byte-compatible twin: the Python library's usbip/protocol.py. */
#define PCAP_EPMAP_DEVS 8
static struct pcap_epmap
{
    uint32_t devid;
    uint8_t  type[16][2]; /* [ep][dir]: 0 = unknown, else bmAttributes type + 1 */
} pcap_epmap[PCAP_EPMAP_DEVS];
static int pcap_epmap_n;

static struct pcap_epmap *pcap_epmap_find(uint32_t devid, int create)
{
    for (int i = 0; i < pcap_epmap_n; i++)
    {
        if (pcap_epmap[i].devid == devid)
            return &pcap_epmap[i];
    }
    if (!create)
        return NULL;

    int i = (pcap_epmap_n < PCAP_EPMAP_DEVS) ? pcap_epmap_n++ : 0; /* full: recycle slot 0 */
    memset(&pcap_epmap[i], 0, sizeof(pcap_epmap[i]));
    pcap_epmap[i].devid = devid;
    return &pcap_epmap[i];
}

/* Walk a CONFIGURATION descriptor and record what every endpoint in it declared. */
static void pcap_epmap_learn(uint32_t devid, const uint8_t *cfg, int len)
{
    struct pcap_epmap *map = NULL;
    int i = 0;

    while (i + 2 <= len)
    {
        int blen = cfg[i];
        int type = cfg[i + 1];

        if (blen == 0) /* malformed: stepping by 0 would spin forever */
            break;

        if (type == USB_DT_ENDPOINT && blen >= 7 && i + 7 <= len)
        {
            if (!map && !(map = pcap_epmap_find(devid, 1)))
                return;

            int ep = cfg[i + 2] & 0x0f;
            int dir = (cfg[i + 2] & USB_EP_ADDR_DIR_IN) ? USB_IN : USB_OUT;
            map->type[ep][dir] = (uint8_t)((cfg[i + 3] & 3) + 1);
        }
        i += blen; /* advance by this descriptor's own length */
    }
}

/* usbmon transfer_type as the client can tell it: from the learned map, else from the
 * polling interval (only interrupt endpoints are polled), else bulk. */
static int pcap_xfer_client(uint32_t devid, int ep, int dir, int interval)
{
    if (ep == 0)
        return USBMON_XFER_CTRL;

    struct pcap_epmap *map = pcap_epmap_find(devid, 0);
    uint8_t type = (map && ep < 16) ? map->type[ep][dir] : 0;

    if (type)
        return bmattr_to_usbmon(type - 1, ep);

    return interval > 0 ? USBMON_XFER_INTR : USBMON_XFER_BULK;
}

static int readn(int fd, void *buf, size_t len)
{
    size_t got = 0;
    while (got < len)
    {
        ssize_t chunk = recv(fd, (char *)buf + got, len - got, 0);
        if (chunk <= 0)
            return -1;
        got += (size_t)chunk;
    }
    return 0;
}
static int writen(int fd, const void *buf, size_t len)
{
    size_t put = 0;
    while (put < len)
    {
        ssize_t chunk = send(fd, (const char *)buf + put, len - put, MSG_NOSIGNAL);
        if (chunk <= 0)
            return -1;
        put += (size_t)chunk;
    }
    return 0;
}

/* ---- URB read / RET write --------------------------------------------- */

/* Zero a 48-byte USB/IP header and fill the five basic fields it always starts with. */
static void put_basic_hdr(uint8_t hdr[USBIP_HDR_LEN], uint32_t command, uint32_t seqnum,
                          uint32_t devid, uint32_t direction, uint32_t ep)
{
    memset(hdr, 0, USBIP_HDR_LEN);
    usb_put_be32(hdr + USBIP_HDR_COMMAND_OFF, command);
    usb_put_be32(hdr + USBIP_HDR_SEQNUM_OFF, seqnum);
    usb_put_be32(hdr + USBIP_HDR_DEVID_OFF, devid);
    usb_put_be32(hdr + USBIP_HDR_DIRECTION_OFF, direction);
    usb_put_be32(hdr + USBIP_HDR_EP_OFF, ep);
}

/* One usbip_iso_packet_descriptor, wire <-> struct. */
static void iso_desc_pack(uint8_t *raw, const struct iso_pkt *pkt)
{
    usb_put_be32(raw + USBIP_ISO_OFFSET_OFF, pkt->offset);
    usb_put_be32(raw + USBIP_ISO_LENGTH_OFF, pkt->length);
    usb_put_be32(raw + USBIP_ISO_ACTUAL_OFF, pkt->actual_length);
    usb_put_be32(raw + USBIP_ISO_STATUS_OFF, pkt->status);
}

static void iso_desc_parse(const uint8_t *raw, struct iso_pkt *pkt)
{
    pkt->offset = usb_get_be32(raw + USBIP_ISO_OFFSET_OFF);
    pkt->length = usb_get_be32(raw + USBIP_ISO_LENGTH_OFF);
    pkt->actual_length = usb_get_be32(raw + USBIP_ISO_ACTUAL_OFF);
    pkt->status = usb_get_be32(raw + USBIP_ISO_STATUS_OFF);
}

int usbip_read_cmd(int fd, struct urb *urb, usbip_device *dev)
{
    uint8_t hdr[USBIP_HDR_LEN];
    if (readn(fd, hdr, USBIP_HDR_LEN))
        return -1;

    urb->command   = usb_get_be32(hdr + USBIP_HDR_COMMAND_OFF);
    urb->seqnum    = usb_get_be32(hdr + USBIP_HDR_SEQNUM_OFF);
    urb->devid     = usb_get_be32(hdr + USBIP_HDR_DEVID_OFF);
    urb->direction = usb_get_be32(hdr + USBIP_HDR_DIRECTION_OFF);
    urb->ep        = usb_get_be32(hdr + USBIP_HDR_EP_OFF);
    urb->data = NULL;
    urb->data_len = 0;
    urb->number_of_packets = 0;
    urb->iso = NULL;
    if (urb->command == USBIP_CMD_SUBMIT)
    {
        urb->flags  = usb_get_be32(hdr + USBIP_CMD_FLAGS_OFF);
        urb->length = (int32_t)usb_get_be32(hdr + USBIP_CMD_LENGTH_OFF);
        urb->number_of_packets = (int32_t)usb_get_be32(hdr + USBIP_CMD_NUMPKTS_OFF);
        urb->interval = (int32_t)usb_get_be32(hdr + USBIP_CMD_INTERVAL_OFF);
        memcpy(urb->setup, hdr + USBIP_CMD_SETUP_OFF, sizeof(urb->setup));
        if (urb->direction == USB_OUT && urb->length > 0)
        { /* transfer_buffer */
            urb->data = malloc((size_t)urb->length);
            if (!urb->data || readn(fd, urb->data, (size_t)urb->length))
            {
                free(urb->data);
                return -1;
            }
            urb->data_len = urb->length;
        }
        /* isochronous: descriptors follow, but ONLY when the EP is iso (the wire's
         * number_of_packets is unreliable for non-iso - may be 0 or 0xffffffff). */
        usbip_ep *ep = (urb->ep < 16) ? dev->ep_map[urb->ep][urb->direction] : NULL;
        if (ep && ep->type == USB_ISO && urb->number_of_packets > 0)
        {
            int np = urb->number_of_packets;
            urb->iso = calloc((size_t)np, sizeof(*urb->iso));
            uint8_t *raw = malloc((size_t)np * USBIP_ISO_DESC_LEN);
            if (!urb->iso || !raw || readn(fd, raw, (size_t)np * USBIP_ISO_DESC_LEN))
            {
                free(raw);
                free(urb->iso);
                urb->iso = NULL;
                free(urb->data);
                return -1;
            }
            for (int i = 0; i < np; i++)
                iso_desc_parse(raw + i * USBIP_ISO_DESC_LEN, &urb->iso[i]);

            free(raw);
        }
    }
    else if (urb->command == USBIP_CMD_UNLINK)
    {
        urb->unlink_seqnum = usb_get_be32(hdr + USBIP_UNLINK_SEQNUM_OFF);
    }
    return 0;
}

int usbip_send_ret(struct conn *conn, uint32_t seqnum, uint32_t devid,
                   uint32_t direction, uint32_t ep, int32_t status,
                   const void *data, int actual)
{
    uint8_t hdr[USBIP_HDR_LEN];
    put_basic_hdr(hdr, USBIP_RET_SUBMIT, seqnum, devid, direction, ep);
    usb_put_be32(hdr + USBIP_RET_STATUS_OFF, (uint32_t)status);
    usb_put_be32(hdr + USBIP_RET_ACTUAL_OFF, (uint32_t)actual);

    pthread_mutex_lock(&conn->wlock);

    int rc = writen(conn->fd, hdr, USBIP_HDR_LEN);
    if (rc == 0 && direction == USB_IN && actual > 0)
        rc = writen(conn->fd, data, (size_t)actual);

    pthread_mutex_unlock(&conn->wlock);

    if (usbip_pcap_enabled())
    { /* completion: every RET path lands here */
        const uint8_t *in = (direction == USB_IN && actual > 0) ? data : NULL;
        int in_len = (direction == USB_IN) ? actual : 0;
        int xt = pcap_xfer_dev(conn->dev, (int)ep, (int)direction);

        usbip_pcap_packet(devid, seqnum, xt, USBMON_EVENT_COMPLETE, EP_ADDR(ep, direction), status,
                          NULL, in, in_len, actual);
    }
    return rc;
}

int usbip_send_ret_iso(struct conn *conn, struct urb *urb, const void *data)
{
    int np = urb->number_of_packets;
    uint32_t total = 0;
    for (int i = 0; i < np; i++)
        total += urb->iso[i].actual_length;

    uint8_t hdr[USBIP_HDR_LEN];
    put_basic_hdr(hdr, USBIP_RET_SUBMIT, urb->seqnum, urb->devid, urb->direction, urb->ep);
    usb_put_be32(hdr + USBIP_RET_STATUS_OFF, 0);
    usb_put_be32(hdr + USBIP_RET_ACTUAL_OFF, total); /* Σ per-packet actual */
    usb_put_be32(hdr + USBIP_RET_NUMPKTS_OFF, (uint32_t)np);
    /* USBIP_RET_ERRCOUNT_OFF stays 0 */

    uint8_t *desc = malloc((size_t)np * USBIP_ISO_DESC_LEN); /* echo offset/length, send actual/status */
    if (!desc)
        return USB_ERROR_NO_MEM;

    for (int i = 0; i < np; i++)
        iso_desc_pack(desc + i * USBIP_ISO_DESC_LEN, &urb->iso[i]);

    pthread_mutex_lock(&conn->wlock);
    int rc = writen(conn->fd, hdr, USBIP_HDR_LEN);
    if (rc == 0 && urb->direction == USB_IN && total > 0)
        rc = writen(conn->fd, data, total);

    if (rc == 0)
        rc = writen(conn->fd, desc, (size_t)np * USBIP_ISO_DESC_LEN);

    pthread_mutex_unlock(&conn->wlock);
    free(desc);

    if (usbip_pcap_enabled())
    { /* iso completion (best-effort: data, no per-pkt desc) */
        const uint8_t *in = (urb->direction == USB_IN && total > 0) ? data : NULL;
        usbip_pcap_packet(urb->devid, urb->seqnum, USBMON_XFER_ISO, USBMON_EVENT_COMPLETE,
                          EP_ADDR(urb->ep, urb->direction), 0, NULL,
                          in, urb->direction == USB_IN ? (int)total : 0, (int)total);
    }
    return rc;
}

int usbip_send_ret_unlink(struct conn *conn, uint32_t seqnum, int32_t status)
{
    uint8_t hdr[USBIP_HDR_LEN];
    put_basic_hdr(hdr, USBIP_RET_UNLINK, seqnum, 0, 0, 0);
    usb_put_be32(hdr + USBIP_RET_STATUS_OFF, (uint32_t)status);
    pthread_mutex_lock(&conn->wlock);
    int rc = writen(conn->fd, hdr, USBIP_HDR_LEN);
    pthread_mutex_unlock(&conn->wlock);
    return rc;
}

/* ---- the listener -------------------------------------------------------
 * One bound socket serving several devices, keyed by address. The importer picks by
 * busid, as usbipd does. Each import gets its own connection and serve thread. */
struct usbip_server
{
    char host[64];
    int port;
    int srv_fd;
    int running;
    pthread_t accept_thread;
    usbip_device *devs[USBIP_MAX_DEVICES];
    int n_devs;
    uint32_t next_devnum; /* wire address handed to the next device */
    pthread_mutex_t lock; /* guards devs/n_devs against the accept thread */
    struct usbip_server *next;
};

static struct usbip_server *g_servers;
static pthread_mutex_t g_servers_lock = PTHREAD_MUTEX_INITIALIZER;

/* op_common reply: version, code, status. The replies differ only in those. */
static int write_op_hdr(int fd, uint16_t code, uint32_t status)
{
    uint8_t hdr[USBIP_OP_HDR_LEN] = {0};
    usb_put_be16(hdr, USBIP_PROTO_VERSION);
    usb_put_be16(hdr + USBIP_OP_CODE_OFF, code);
    usb_put_be32(hdr + USBIP_OP_STATUS_OFF, status);
    return writen(fd, hdr, sizeof(hdr));
}

static void emit_iface_list(int fd, usbip_device *dev)
{
    int off = 0;
    while (off + 2 <= dev->descr_len)
    {
        const uint8_t *desc = dev->descr + off;
        if (desc[1] == USB_DT_INTERFACE && desc[3] == 0) /* one tuple per interface (alt 0 only) */
            writen(fd, (uint8_t[]){desc[5], desc[6], desc[7], 0}, USBIP_IFACE_TUPLE_LEN);
        off += desc[0] ? desc[0] : 1;
    }
}

/* Snapshot the listener's devices: the accept thread reads the list while the app
 * may still be plugging or unplugging on another thread. */
static int server_snapshot(struct usbip_server *server, usbip_device *out[USBIP_MAX_DEVICES])
{
    pthread_mutex_lock(&server->lock);
    int count = server->n_devs;
    memcpy(out, server->devs, (size_t)count * sizeof(*out));
    pthread_mutex_unlock(&server->lock);
    return count;
}

/* returns the imported device to enter the URB phase, NULL otherwise */
static usbip_device *handshake(int fd, struct usbip_server *server)
{
    uint8_t op[USBIP_OP_HDR_LEN];
    if (readn(fd, op, sizeof(op)))
        return NULL;

    uint16_t code = usb_get_be16(op + USBIP_OP_CODE_OFF);

    usbip_device *devs[USBIP_MAX_DEVICES];
    int count = server_snapshot(server, devs);
    uint8_t packed[USBIP_DEV_LEN];

    if (code == USBIP_OP_REQ_IMPORT)
    {
        uint8_t busid[USBIP_DEV_BUSID_LEN];
        if (readn(fd, busid, sizeof(busid)))
            return NULL;

        busid[sizeof(busid) - 1] = 0;
        for (int i = 0; i < count; i++)
        {
            if (strcmp((char *)busid, devs[i]->busid))
                continue;

            pack_usbip_device(devs[i], packed);
            write_op_hdr(fd, USBIP_OP_REP_IMPORT, USBIP_OP_STATUS_OK);
            writen(fd, packed, USBIP_DEV_LEN);
            return devs[i];
        }
        /* No such bus. Silently importing the wrong device is worse than a failed
         * attach, which `usbip attach` reports either way. */
        write_op_hdr(fd, USBIP_OP_REP_IMPORT, USBIP_OP_STATUS_ERROR);
        return NULL;
    }
    if (code == USBIP_OP_REQ_DEVLIST)
    {
        write_op_hdr(fd, USBIP_OP_REP_DEVLIST, USBIP_OP_STATUS_OK);
        uint8_t cnt[4];
        usb_put_be32(cnt, (uint32_t)count); /* exported device count */
        writen(fd, cnt, sizeof(cnt));
        for (int i = 0; i < count; i++)
        {
            pack_usbip_device(devs[i], packed);
            writen(fd, packed, USBIP_DEV_LEN);
            emit_iface_list(fd, devs[i]);
        }
        return NULL;
    }
    return NULL;
}

/* ---- serve / accept ---------------------------------------------------- */
struct accepted
{
    int fd;
    struct usbip_server *srv;
};

static void *serve_conn(void *arg)
{
    struct accepted *accepted = arg;
    int fd = accepted->fd;
    struct usbip_server *srv = accepted->srv;
    free(accepted);

    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, SOCKOPT_VAL(&one), sizeof(one));
    usbip_sock_nosigpipe(fd); /* accept() did not inherit it from the listener */

    usbip_device *dev = handshake(fd, srv); /* which device this connection imported */
    if (!dev)
    {
        sock_close(fd);
        return NULL;
    }

    struct conn conn;
    conn.fd = fd;
    conn.dev = dev;
    conn.pace_started = 0;
    conn.pace_head = NULL; /* iso pacer starts lazily on first paced transfer */
    pthread_mutex_init(&conn.wlock, NULL);

    for (;;)
    {
        struct urb urb;
        if (usbip_read_cmd(fd, &urb, dev) < 0)
            break;
        if (urb.command == USBIP_CMD_UNLINK)
        {
            usbip_device_handle_unlink(&conn, &urb);
            free(urb.iso);
            continue;
        }
        if (usbip_pcap_enabled())
        { /* request: setup + any OUT payload */
            const uint8_t *out = (urb.direction == USB_OUT) ? urb.data : NULL;
            int out_len = (urb.direction == USB_OUT) ? urb.data_len : 0;
            const uint8_t *setup = (urb.ep == 0) ? urb.setup : NULL;
            int xt = pcap_xfer_dev(dev, (int)urb.ep, (int)urb.direction);

            usbip_pcap_packet(urb.devid, urb.seqnum, xt, USBMON_EVENT_SUBMIT,
                              EP_ADDR(urb.ep, urb.direction), USBMON_STATUS_PENDING,
                              setup, out, out_len, urb.length);
        }
        usbip_device_dispatch(&conn, &urb);
        free(urb.data);
        free(urb.iso);
    }
    usbip_device_pacer_stop(&conn); /* drain + stop the iso pacer before tearing down conn */
    pthread_mutex_destroy(&conn.wlock);
    sock_close(fd);
    return NULL;
}

static void *accept_loop(void *arg)
{
    struct usbip_server *server = arg;
    while (server->running)
    {
        int fd = accept(server->srv_fd, NULL, NULL);
        if (fd < 0)
            break;
        struct accepted *accepted = malloc(sizeof(*accepted));
        accepted->fd = fd;
        accepted->srv = server;
        pthread_t th;
        if (pthread_create(&th, NULL, serve_conn, accepted) == 0)
            pthread_detach(th);
        else
        {
            sock_close(fd);
            free(accepted);
        }
    }
    return NULL;
}

/* Find (or create) the listener for an address; g_servers_lock held.
 * Never freed on purpose -- a detached serve thread still holds one. An idle one has
 * no socket and no thread, and is rebound by the next plug. */
static struct usbip_server *server_for(const char *host, int port)
{
    for (struct usbip_server *server = g_servers; server; server = server->next)
        if (server->port == port && !strcmp(server->host, host))
            return server;

    struct usbip_server *server = calloc(1, sizeof(*server));
    if (!server)
        return NULL;

    snprintf(server->host, sizeof(server->host), "%s", host);
    server->port = port;
    server->srv_fd = -1;
    pthread_mutex_init(&server->lock, NULL);
    server->next = g_servers;
    g_servers = server;
    return server;
}

/* Bind, listen and start accepting on an idle listener. g_servers_lock held. */
static int server_start(struct usbip_server *server)
{
#ifdef _WIN32
    usbip_net_startup();
#endif
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
        return USB_ERROR_IO;
    int yes = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, SOCKOPT_VAL(&yes), sizeof(yes));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)server->port);
    addr.sin_addr.s_addr = (server->host[0] && strcmp(server->host, "0.0.0.0")) ? inet_addr(server->host) : INADDR_ANY;

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0 || listen(fd, USBIP_LISTEN_BACKLOG) < 0)
    {
        sock_close(fd);
        return USB_ERROR_IO;
    }

    server->srv_fd = fd;
    server->running = 1;
    server->next_devnum = USBIP_DEV_DEVNUM;
    if (pthread_create(&server->accept_thread, NULL, accept_loop, server) != 0)
    {
        sock_close(fd);
        server->srv_fd = -1;
        server->running = 0;
        return USB_ERROR_OTHER;
    }
    return USB_SUCCESS;
}

/* Stop accepting; established connections are unaffected. g_servers_lock held. */
static void server_stop(struct usbip_server *server)
{
    server->running = 0;
    if (server->srv_fd >= 0)
    {
        shutdown(server->srv_fd, SHUT_RDWR); /* wakes accept() so the thread can exit */
        sock_close(server->srv_fd);
        server->srv_fd = -1;
    }
    pthread_join(server->accept_thread, NULL);
}

/* Serve `dev`, joining the listener already bound to its address.
 * Named on the wire by its busid: whatever set_busid() chose, else "1-<slot>" in plug
 * order. devnum follows suit, which is what lets `lsusb -s` tell devices apart. */
int usbip_serve_start(usbip_device *dev)
{
    pthread_mutex_lock(&g_servers_lock);
    struct usbip_server *server = server_for(dev->host, dev->port);
    int rc = USB_ERROR_NO_MEM;
    if (!server)
        goto out;

    if (server->n_devs >= USBIP_MAX_DEVICES)
    {
        fprintf(stderr, "usbip: cannot export more than %d devices on %s:%d\n",  USBIP_MAX_DEVICES, dev->host, dev->port);
        goto out;
    }
    if (!server->running)
    {
        rc = server_start(server);
        if (rc != USB_SUCCESS)
            goto out;
    }

    pthread_mutex_lock(&server->lock);

    if (!dev->busid[0])
        snprintf(dev->busid, sizeof(dev->busid), "1-%d", server->n_devs + 1);

    dev->busnum = USBIP_DEV_BUSNUM;
    dev->devnum = server->next_devnum++;
    dev->server = server;
    server->devs[server->n_devs++] = dev;
    pthread_mutex_unlock(&server->lock);
    rc = USB_SUCCESS;

out:
    pthread_mutex_unlock(&g_servers_lock);
    return rc;
}

/* Unplug one device. The listener keeps serving while other devices use it; the
 * last one out stops it accepting. */
void usbip_serve_stop(usbip_device *dev)
{
    pthread_mutex_lock(&g_servers_lock);
    struct usbip_server *server = dev->server;
    if (server)
    {
        pthread_mutex_lock(&server->lock);
        for (int i = 0; i < server->n_devs; i++)
        {
            if (server->devs[i] == dev)
            {
                memmove(&server->devs[i], &server->devs[i + 1], (size_t)(server->n_devs - i - 1) * sizeof(*server->devs));
                server->n_devs--;
                break;
            }
        }
        int empty = (server->n_devs == 0);
        pthread_mutex_unlock(&server->lock);

        dev->server = NULL;
        if (empty)
            server_stop(server);
    }
    pthread_mutex_unlock(&g_servers_lock);
}

/* ======================================================================
 * Client side (used by usbip_host.c) - the importer half of the protocol.
 * ====================================================================== */
int usbip_connect(usb_transport *transport)
{
#ifdef _WIN32
    usbip_net_startup();
#endif
    const char *host = "127.0.0.1";
    int port = USBIP_DEFAULT_PORT;
    if (transport)
    {
        usbip_transport_params(transport, &host, &port);
        if (!host)
            host = "127.0.0.1";
    }

    char portstr[16];
    snprintf(portstr, sizeof(portstr), "%d", port);
    struct addrinfo hints, *res, *rp;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;

    if (getaddrinfo(host, portstr, &hints, &res) != 0)
        return -1;

    int fd = -1;
    for (rp = res; rp; rp = rp->ai_next)
    {
        fd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (fd < 0)
            continue;
        if (connect(fd, rp->ai_addr, rp->ai_addrlen) == 0)
            break;
        sock_close(fd);
        fd = -1;
    }

    freeaddrinfo(res);

    if (fd >= 0)
    {
        int one = 1;
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, SOCKOPT_VAL(&one), sizeof(one));
        usbip_sock_nosigpipe(fd);
    }
    return fd;
}

static void parse_devinfo(const uint8_t *dev, struct usbip_devinfo *out)
{
    memset(out, 0, sizeof(*out));
    memcpy(out->busid, dev + USBIP_DEV_BUSID_OFF, sizeof(out->busid) - 1);
    out->busnum = usb_get_be32(dev + USBIP_DEV_BUSNUM_OFF);
    out->devnum = usb_get_be32(dev + USBIP_DEV_DEVNUM_OFF);
    out->speed  = usb_get_be32(dev + USBIP_DEV_SPEED_OFF);
    out->vid = usb_get_be16(dev + USBIP_DEV_VID_OFF);
    out->pid = usb_get_be16(dev + USBIP_DEV_PID_OFF);
    out->bcdDevice = usb_get_be16(dev + USBIP_DEV_BCDDEVICE_OFF);
    out->dclass = dev[USBIP_DEV_CLASS_OFF];
    out->dsub   = dev[USBIP_DEV_SUBCLASS_OFF];
    out->dproto = dev[USBIP_DEV_PROTOCOL_OFF];
    out->n_cfg  = dev[USBIP_DEV_NUM_CFG_OFF];
    out->n_ifaces = dev[USBIP_DEV_NUM_IFACE_OFF];
}

int usbip_client_import(int fd, const char *busid, struct usbip_devinfo *out)
{
    uint8_t req[USBIP_OP_HDR_LEN + USBIP_DEV_BUSID_LEN];
    memset(req, 0, sizeof(req));
    usb_put_be16(req, USBIP_PROTO_VERSION);
    usb_put_be16(req + USBIP_OP_CODE_OFF, USBIP_OP_REQ_IMPORT);
    snprintf((char *)req + USBIP_OP_HDR_LEN, USBIP_DEV_BUSID_LEN, "%s", busid);

    if (writen(fd, req, sizeof(req)))
        return USB_ERROR_IO;

    uint8_t rep[USBIP_OP_HDR_LEN];
    if (readn(fd, rep, sizeof(rep)))
        return USB_ERROR_IO;

    if (usb_get_be32(rep + USBIP_OP_STATUS_OFF) != USBIP_OP_STATUS_OK) /* status != 0 */
        return USB_ERROR_NOT_FOUND;

    uint8_t dev[USBIP_DEV_LEN];
    if (readn(fd, dev, USBIP_DEV_LEN))
        return USB_ERROR_IO;

    parse_devinfo(dev, out);
    return USB_SUCCESS;
}

int usbip_client_devlist(int fd, struct usbip_devinfo *list, int max)
{
    uint8_t req[USBIP_OP_HDR_LEN] = {0};
    usb_put_be16(req, USBIP_PROTO_VERSION);
    usb_put_be16(req + USBIP_OP_CODE_OFF, USBIP_OP_REQ_DEVLIST);

    if (writen(fd, req, sizeof(req)))
        return USB_ERROR_IO;

    uint8_t rep[USBIP_OP_HDR_LEN];

    if (readn(fd, rep, sizeof(rep)))
        return USB_ERROR_IO;

    if (usb_get_be32(rep + USBIP_OP_STATUS_OFF) != USBIP_OP_STATUS_OK)
        return USB_ERROR_IO;

    uint8_t cnt[4];
    if (readn(fd, cnt, 4))
        return USB_ERROR_IO;

    int count = (int)usb_get_be32(cnt), got = 0;
    for (int i = 0; i < count; i++)
    {
        uint8_t dev[USBIP_DEV_LEN];
        if (readn(fd, dev, USBIP_DEV_LEN))
            return USB_ERROR_IO;

        struct usbip_devinfo info;
        parse_devinfo(dev, &info);
        for (int j = 0; j < info.n_ifaces; j++)
        {
            uint8_t ie[USBIP_IFACE_TUPLE_LEN];
            if (readn(fd, ie, sizeof(ie)))
                return USB_ERROR_IO;
            if (j < (int)(sizeof(info.iclass) / sizeof(info.iclass[0])))
            {
                info.iclass[j] = ie[0];
                info.isub[j] = ie[1];
                info.iproto[j] = ie[2];
            }
        }
        if (got < max)
            list[got++] = info;
    }
    return got;
}

/* RET_SUBMIT status -> library error. The wire carries Linux errnos: -EPIPE
 * (USBIP_STATUS_STALL) is a STALLed pipe, as is a -9 some servers answer with
 * (the library's own USB_ERROR_PIPE); anything else is an I/O failure. */
static int client_status_error(int32_t status)
{
    if (status == USBIP_STATUS_STALL || status == USB_ERROR_PIPE)
        return USB_ERROR_PIPE;

    return USB_ERROR_IO;
}

/* Did this transfer just complete a GET_DESCRIPTOR(CONFIGURATION)? That is the one
 * reply whose payload names every endpoint's type, so it is the only one worth
 * handing to pcap_epmap_learn(). Mirrors _is_config_descriptor_reply() in Python. */
static int is_config_descriptor_reply(int ep, int dir, int32_t status, int act, const uint8_t *setup)
{
    if (ep != 0 || dir != USB_IN || status != 0 || act <= 0 || !setup)
        return 0;

    return setup[0] == USB_REQ_DIR_IN && setup[1] == USB_REQ_GET_DESCRIPTOR &&
           setup[3] == USB_DT_CONFIG;
}

int usbip_client_submit(int fd, uint32_t devid, uint32_t *seqctr,
                        int dir, int ep, const uint8_t setup[8], int interval,
                        uint8_t *buf, int len, int *actual)
{
    uint32_t seq = ++(*seqctr);
    int cap = usbip_pcap_enabled();
    int xt = cap ? pcap_xfer_client(devid, ep, dir, interval) : USBMON_XFER_BULK;
    if (cap)
    {
        /* only an OUT carries payload on the way out; only EP0 carries a SETUP */
        const uint8_t *cap_setup = (ep == 0) ? setup : NULL;
        const uint8_t *cap_data = (dir == USB_OUT) ? buf : NULL;
        int cap_len = (dir == USB_OUT) ? len : 0;

        usbip_pcap_packet(devid, seq, xt, USBMON_EVENT_SUBMIT, EP_ADDR(ep, dir), USBMON_STATUS_PENDING,
                          cap_setup, cap_data, cap_len, len);
    }

    uint8_t hdr[USBIP_HDR_LEN];
    put_basic_hdr(hdr, USBIP_CMD_SUBMIT, seq, devid, (uint32_t)dir, (uint32_t)ep);
    usb_put_be32(hdr + USBIP_CMD_LENGTH_OFF, (uint32_t)len);
    usb_put_be32(hdr + USBIP_CMD_INTERVAL_OFF, (uint32_t)interval);

    if (setup)
        memcpy(hdr + USBIP_CMD_SETUP_OFF, setup, 8);

    if (writen(fd, hdr, USBIP_HDR_LEN))
        return USB_ERROR_IO;

    if (dir == USB_OUT && len > 0 && writen(fd, buf, (size_t)len))
        return USB_ERROR_IO;

    uint8_t ret[USBIP_HDR_LEN];
    if (readn(fd, ret, USBIP_HDR_LEN))
        return USB_ERROR_IO;

    int32_t status = (int32_t)usb_get_be32(ret + USBIP_RET_STATUS_OFF);
    int32_t act = (int32_t)usb_get_be32(ret + USBIP_RET_ACTUAL_OFF);

    if (dir == USB_IN && act > 0)
    { /* read exactly 'act' bytes; cap into buf */
        int into = act < len ? act : len;
        if (into > 0 && readn(fd, buf, (size_t)into))
            return USB_ERROR_IO;
        int rest = act - into;
        while (rest > 0)
        {
            uint8_t tmp[256];
            int chunk = rest < (int)sizeof(tmp) ? rest : (int)sizeof(tmp);
            if (readn(fd, tmp, (size_t)chunk))
                return USB_ERROR_IO;
            rest -= chunk;
        }
        act = into;
    }
    if (actual)
        *actual = act;
    /* Learn the endpoint types from this device's configuration, so the endpoints it
     * describes are captured as what they are instead of as bulk. */
    if (cap && is_config_descriptor_reply(ep, dir, status, act, setup))
        pcap_epmap_learn(devid, buf, act);
    if (cap)
        usbip_pcap_packet(devid, seq, xt, USBMON_EVENT_COMPLETE, EP_ADDR(ep, dir), status, NULL,
                          (dir == USB_IN && act > 0) ? buf : NULL,
                          dir == USB_IN ? act : 0, act);
    if (status != 0)
        return client_status_error(status);

    return USB_SUCCESS;
}

int usbip_client_submit_iso(int fd, uint32_t devid, uint32_t *seqctr, int dir, int ep,
                            int interval, uint8_t *buf, struct iso_pkt *pkts, int npkts,
                            int *total_actual)
{
    uint32_t total_len = 0;
    for (int i = 0; i < npkts; i++)
        total_len += pkts[i].length;

    uint32_t seq = ++(*seqctr);
    int cap = usbip_pcap_enabled();
    if (cap)
    {
        /* only an OUT carries payload on the way out */
        const uint8_t *cap_data = (dir == USB_OUT) ? buf : NULL;
        int cap_len = (dir == USB_OUT) ? (int)total_len : 0;

        usbip_pcap_packet(devid, seq, USBMON_XFER_ISO, USBMON_EVENT_SUBMIT, EP_ADDR(ep, dir),
                          USBMON_STATUS_PENDING, NULL, cap_data, cap_len, (int)total_len);
    }

    uint8_t hdr[USBIP_HDR_LEN];
    put_basic_hdr(hdr, USBIP_CMD_SUBMIT, seq, devid, (uint32_t)dir, (uint32_t)ep);
    usb_put_be32(hdr + USBIP_CMD_LENGTH_OFF, total_len);
    usb_put_be32(hdr + USBIP_CMD_NUMPKTS_OFF, (uint32_t)npkts);
    usb_put_be32(hdr + USBIP_CMD_INTERVAL_OFF, (uint32_t)interval);
    if (writen(fd, hdr, USBIP_HDR_LEN))
        return USB_ERROR_IO;

    if (dir == USB_OUT) /* gather OUT data from the slots */
        for (int i = 0; i < npkts; i++)
            if (pkts[i].length && writen(fd, buf + pkts[i].offset, pkts[i].length))
                return USB_ERROR_IO;

    uint8_t *desc = malloc((size_t)npkts * USBIP_ISO_DESC_LEN);
    if (!desc)
        return USB_ERROR_NO_MEM;

    for (int i = 0; i < npkts; i++)
    { /* request: offset/length only, actual/status not yet known */
        struct iso_pkt req = {pkts[i].offset, pkts[i].length, 0, 0};
        iso_desc_pack(desc + i * USBIP_ISO_DESC_LEN, &req);
    }
    int wrc = writen(fd, desc, (size_t)npkts * USBIP_ISO_DESC_LEN);
    free(desc);
    if (wrc)
        return USB_ERROR_IO;

    uint8_t ret[USBIP_HDR_LEN];
    if (readn(fd, ret, USBIP_HDR_LEN))
        return USB_ERROR_IO;

    int32_t status = (int32_t)usb_get_be32(ret + USBIP_RET_STATUS_OFF);
    uint32_t actual = usb_get_be32(ret + USBIP_RET_ACTUAL_OFF);
    int rnp = (int)usb_get_be32(ret + USBIP_RET_NUMPKTS_OFF);

    uint8_t *tmp = NULL; /* de-padded IN data */
    if (dir == USB_IN && actual > 0)
    {
        tmp = malloc(actual);
        if (!tmp || readn(fd, tmp, actual))
        {
            free(tmp);
            return USB_ERROR_IO;
        }
    }
    if (rnp > 0)
    {
        uint8_t *rd = malloc((size_t)rnp * USBIP_ISO_DESC_LEN);
        if (!rd || readn(fd, rd, (size_t)rnp * USBIP_ISO_DESC_LEN))
        {
            free(rd);
            free(tmp);
            return USB_ERROR_IO;
        }
        uint32_t cum = 0;
        for (int i = 0; i < rnp && i < npkts; i++)
        {
            iso_desc_parse(rd + i * USBIP_ISO_DESC_LEN, &pkts[i]);
            if (dir == USB_IN && tmp && pkts[i].actual_length)
            { /* scatter into slots */
                memcpy(buf + pkts[i].offset, tmp + cum, pkts[i].actual_length);
                cum += pkts[i].actual_length;
            }
        }
        free(rd);
    }
    free(tmp);
    if (total_actual)
        *total_actual = (int)actual;
    if (cap)
        usbip_pcap_packet(devid, seq, USBMON_XFER_ISO, USBMON_EVENT_COMPLETE, EP_ADDR(ep, dir), status, NULL,
                          (dir == USB_IN && actual > 0) ? buf : NULL,
                          dir == USB_IN ? (int)actual : 0, (int)actual);
    if (status != 0)
        return client_status_error(status);
    return USB_SUCCESS;
}
