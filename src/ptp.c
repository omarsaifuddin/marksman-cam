#include "ptp.h"

#include <libusb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CANON_VID  0x04A9
#define TIMEOUT_MS 5000
#define ALIGN      1024u /* keeps every bulk read a multiple of the max packet size (HS 512, SS 1024) */

enum { CMD = 1, DATA = 2, RESP = 3 };

#define OP_OPEN_SESSION     0x1002
#define OP_CLOSE_SESSION    0x1003
#define OP_EOS_SET_PROP     0x9110
#define OP_EOS_REMOTE_MODE  0x9114
#define OP_EOS_EVENT_MODE   0x9115
#define OP_EOS_GET_EVENT    0x9116
#define OP_EOS_KEEP_ON      0x911D
#define OP_EOS_VIEWFINDER   0x9153

#define RC_BUSY             0x2019
#define RC_SESSION_OPEN     0x201E
#define RC_NOT_READY        0xA102

#define PROP_EVF_OUTPUT     0xD1B0
#define PROP_EVF_MODE       0xD1B3
#define EVF_OFF 0
#define EVF_PC  2

static uint16_t rd16(const uint8_t *b) { return (uint16_t)(b[0] | b[1] << 8); }
static uint32_t rd32(const uint8_t *b) { return b[0] | b[1] << 8 | b[2] << 16 | (uint32_t)b[3] << 24; }
static void wr16(uint8_t *b, uint16_t v) { b[0] = (uint8_t)v; b[1] = (uint8_t)(v >> 8); }
static void wr32(uint8_t *b, uint32_t v) { wr16(b, (uint16_t)v); wr16(b + 2, (uint16_t)(v >> 16)); }

static int send_container(ptp_t *p, uint16_t type, uint16_t code, const uint8_t *payload, uint32_t n)
{
    uint8_t c[64];
    int got;
    if (n > sizeof c - 12)
        return -1;
    wr32(c, 12 + n);
    wr16(c + 4, type);
    wr16(c + 6, code);
    wr32(c + 8, p->tid);
    if (n)
        memcpy(c + 12, payload, n);
    return p->bulk(p->ctx, p->ep_out, c, (int)(12 + n), &got, TIMEOUT_MS) || got != (int)(12 + n) ? -1 : 0;
}

/* Reads one container into dst (cap bytes, a multiple of ALIGN). Returns its length or -1. */
static long recv_container(ptp_t *p, uint8_t *dst, uint32_t cap)
{
    uint32_t have = 0, len;
    int got;
    while (have < 12) { /* also swallows the zero-length packet that ends an exact-multiple transfer */
        if (p->bulk(p->ctx, p->ep_in, dst + have, (int)(cap - have), &got, TIMEOUT_MS))
            return -1;
        have += (uint32_t)got;
    }
    len = rd32(dst);
    if (len < 12 || len > cap)
        return -1;
    while (have < len) {
        if (p->bulk(p->ctx, p->ep_in, dst + have, (int)(cap - have), &got, TIMEOUT_MS))
            return -1;
        have += (uint32_t)got;
    }
    return (long)len;
}

int ptp_transact(ptp_t *p, uint16_t code, const uint32_t *params, int nparams,
                 const uint8_t *out, uint32_t outlen, const uint8_t **in, uint32_t *inlen)
{
    uint8_t args[20] = {0};
    uint32_t off = 0;
    long len;
    int i;

    if (nparams > 5)
        return -1;
    if (p->op) {
        int rc = p->op(p, code, params, nparams, out, outlen, in, inlen);
        if (rc < 0)
            p->dead = -1;
        return rc;
    }
    for (i = 0; i < nparams; i++)
        wr32(args + 4 * i, params[i]);
    if (in) {
        *in = NULL;
        *inlen = 0;
    }
    if (send_container(p, CMD, code, args, 4u * (uint32_t)nparams) ||
        (out && send_container(p, DATA, code, out, outlen)) ||
        (len = recv_container(p, p->buf, PTP_BUF)) < 0)
        return p->dead = -1;
    if (rd16(p->buf + 4) == DATA) {
        if (in) {
            *in = p->buf + 12;
            *inlen = (uint32_t)len - 12;
        }
        off = ((uint32_t)len + ALIGN - 1) / ALIGN * ALIGN; /* response goes after the data, not over it */
        if (off + ALIGN > PTP_BUF || (len = recv_container(p, p->buf + off, PTP_BUF - off)) < 0)
            return p->dead = -1;
    }
    p->tid++; /* OpenSession must use transaction id 0, so count up after each operation */
    return rd16(p->buf + off + 4) == RESP ? rd16(p->buf + off + 6) : -1;
}

int eos_set_prop(ptp_t *p, uint32_t prop, uint32_t value)
{
    uint8_t d[12];
    wr32(d, 12);
    wr32(d + 4, prop);
    wr32(d + 8, value);
    return ptp_transact(p, OP_EOS_SET_PROP, NULL, 0, d, sizeof d, NULL, NULL);
}

static int get_event(ptp_t *p)
{
    const uint8_t *d;
    uint32_t n;
    return ptp_transact(p, OP_EOS_GET_EVENT, NULL, 0, NULL, 0, &d, &n);
}

int eos_start_liveview(ptp_t *p)
{
    const uint32_t one = 1;
    int rc = p->op ? RC_SESSION_OPEN /* the Windows MTP driver owns the session */
                   : ptp_transact(p, OP_OPEN_SESSION, &one, 1, NULL, 0, NULL, NULL);
    if (rc != PTP_OK && rc != RC_SESSION_OPEN)
        return rc;
    if ((rc = ptp_transact(p, OP_EOS_REMOTE_MODE, &one, 1, NULL, 0, NULL, NULL)) != PTP_OK)
        return rc;
    if ((rc = ptp_transact(p, OP_EOS_EVENT_MODE, &one, 1, NULL, 0, NULL, NULL)) != PTP_OK)
        return rc;
    if ((rc = get_event(p)) != PTP_OK)
        return rc;
    eos_set_prop(p, PROP_EVF_MODE, 1); /* needed by older bodies, absent on some newer ones */
    return eos_set_prop(p, PROP_EVF_OUTPUT, EVF_PC);
}

int eos_poll(ptp_t *p)
{
    int rc = get_event(p);
    if (rc < 0)
        return rc;
    ptp_transact(p, OP_EOS_KEEP_ON, NULL, 0, NULL, 0, NULL, NULL); /* unsupported on some bodies; ignore */
    return rc;
}

const uint8_t *eos_find_jpeg(const uint8_t *d, uint32_t n, uint32_t *jlen)
{
    uint32_t i = 0, len, type;
    if (n >= 2 && d[0] == 0xFF && d[1] == 0xD8) {
        *jlen = n;
        return d;
    }
    /* GetViewFinderData payload: [u32 length][u32 type][payload] blocks; types 1/9/11 carry the JPEG */
    while (n - i >= 8) {
        len = rd32(d + i);
        type = rd32(d + i + 4);
        if (len < 8 || len > n - i)
            break;
        if (type == 1 || type == 9 || type == 11) {
            *jlen = len - 8;
            return d + i + 8;
        }
        i += len;
    }
    return NULL;
}

int eos_frame(ptp_t *p, const uint8_t **jpeg, uint32_t *jlen)
{
    static const uint32_t args[3] = {0x00100000, 0, 0};
    const uint8_t *d;
    uint32_t n;
    int rc = ptp_transact(p, OP_EOS_VIEWFINDER, args, 3, NULL, 0, &d, &n);
    if (rc == RC_NOT_READY || rc == RC_BUSY)
        return 0;
    if (rc != PTP_OK)
        return -1;
    return (*jpeg = eos_find_jpeg(d, n, jlen)) != NULL;
}

long eos_setting(const char *name)
{
    static const struct { const char *name; long prop; } t[] = {
        {"av", 0xD101}, {"tv", 0xD102}, {"iso", 0xD103}, {"ec", 0xD104},
        {"wb", 0xD109}, {"kelvin", 0xD10A}, {"style", 0xD110},
    };
    for (size_t i = 0; i < sizeof t / sizeof t[0]; i++)
        if (!strcmp(name, t[i].name))
            return t[i].prop;
    return -1;
}

static int usb_bulk(void *ctx, uint8_t ep, uint8_t *buf, int len, int *got, unsigned timeout_ms)
{
    return libusb_bulk_transfer(ctx, ep, buf, len, got, timeout_ms);
}

static libusb_context *usb;

static void report(const char *what, int err)
{
    static int last;
    if (err == last)
        return; /* the reconnect loop retries every few seconds; say each problem once */
    last = err;
#ifdef _WIN32
    if (err == LIBUSB_ERROR_NOT_SUPPORTED || err == LIBUSB_ERROR_NOT_FOUND)
        return; /* MTP-bound camera: handled by the WPD transport instead */
#endif
    fprintf(stderr, "marksman-cam: %s: %s\n", what, libusb_strerror(err));
    if (err == LIBUSB_ERROR_BUSY || err == LIBUSB_ERROR_ACCESS)
        fprintf(stderr, "marksman-cam: camera held by another program (gvfs, gphoto2, Canon software) or no permission (see README)\n");
}

static ptp_t *try_device(libusb_device *dev)
{
    struct libusb_device_descriptor dd;
    struct libusb_config_descriptor *cfg;
    libusb_device_handle *h;
    ptp_t *p = NULL;
    int err;

    if (libusb_get_device_descriptor(dev, &dd) || dd.idVendor != CANON_VID)
        return NULL;
    if (libusb_get_active_config_descriptor(dev, &cfg))
        return NULL;
    for (int i = 0; i < cfg->bNumInterfaces && !p; i++) {
        const struct libusb_interface_descriptor *id = &cfg->interface[i].altsetting[0];
        uint8_t in = 0, out = 0;
        if (id->bInterfaceClass != LIBUSB_CLASS_IMAGE)
            continue;
        for (int e = 0; e < id->bNumEndpoints; e++) {
            const struct libusb_endpoint_descriptor *ep = &id->endpoint[e];
            if ((ep->bmAttributes & 3) != LIBUSB_TRANSFER_TYPE_BULK)
                continue;
            if (ep->bEndpointAddress & LIBUSB_ENDPOINT_IN)
                in = ep->bEndpointAddress;
            else
                out = ep->bEndpointAddress;
        }
        if (!in || !out)
            continue;
        if ((err = libusb_open(dev, &h))) {
            report("open", err);
            continue;
        }
        libusb_set_auto_detach_kernel_driver(h, 1); /* not supported on Windows; harmless */
        if ((err = libusb_claim_interface(h, id->bInterfaceNumber))) {
            report("claim", err);
            libusb_close(h);
            continue;
        }
        if (!(p = calloc(1, sizeof *p)) || !(p->buf = malloc(PTP_BUF))) {
            free(p);
            p = NULL;
            libusb_close(h);
            break;
        }
        p->bulk = usb_bulk;
        p->ctx = h;
        p->ep_in = in;
        p->ep_out = out;
    }
    libusb_free_config_descriptor(cfg);
    return p;
}

ptp_t *eos_open(void)
{
    libusb_device **list;
    ptp_t *p = NULL;
    ssize_t n;
#ifdef _WIN32
    if ((p = wpd_open()))
        return p;
#endif
    if (!usb && libusb_init(&usb))
        return NULL;
    if ((n = libusb_get_device_list(usb, &list)) < 0)
        return NULL;
    for (ssize_t i = 0; i < n && !p; i++)
        p = try_device(list[i]);
    libusb_free_device_list(list, 1);
    return p;
}

void eos_close(ptp_t *p)
{
    const uint32_t zero = 0;
    if (!p->dead) {
        eos_set_prop(p, PROP_EVF_OUTPUT, EVF_OFF);
        ptp_transact(p, OP_EOS_REMOTE_MODE, &zero, 1, NULL, 0, NULL, NULL);
        if (!p->op)
            ptp_transact(p, OP_CLOSE_SESSION, NULL, 0, NULL, 0, NULL, NULL);
    }
    if (p->release)
        p->release(p);
    else if (p->bulk == usb_bulk)
        libusb_close(p->ctx); /* also releases the claimed interface */
    free(p->buf);
    free(p);
}
