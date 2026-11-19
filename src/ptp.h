/* PTP over USB bulk (PIMA 15740) plus the Canon EOS and Sony (PC Remote) live-view vendor operations.
   The eos_* entry points serve both vendors and dispatch on ptp_t.sony. */
#pragma once
#include <stdint.h>

#define PTP_OK  0x2001
#define PTP_BUF (8u << 20) /* one container; live-view JPEGs are well under this */

/* One bulk transfer. Returns 0 on success and stores the byte count in *got. */
typedef int (*ptp_bulk_fn)(void *ctx, uint8_t ep, uint8_t *buf, int len, int *got, unsigned timeout_ms);

typedef struct ptp ptp_t;
typedef int (*ptp_op_fn)(ptp_t *p, uint16_t code, const uint32_t *params, int nparams,
                         const uint8_t *out, uint32_t outlen, const uint8_t **in, uint32_t *inlen);

struct ptp {
    ptp_bulk_fn bulk;
    void *ctx;
    uint8_t ep_in, ep_out;
    uint32_t tid;
    int dead;     /* set after a transport error; eos_close then skips the goodbye commands */
    uint8_t *buf; /* PTP_BUF bytes; data-in payloads point into it until the next transaction */
    ptp_op_fn op; /* operation-level transport (Windows WPD); bulk is unused when set */
    void (*release)(ptp_t *p);
    int sony;     /* Sony PC Remote camera; otherwise Canon EOS/PowerShot */
};

/* Runs one operation. Returns the PTP response code, or -1 on a transport/protocol error.
   Pass in/inlen whenever the operation has a data-in phase: the WPD transport must know up front. */
int ptp_transact(ptp_t *p, uint16_t code, const uint32_t *params, int nparams,
                 const uint8_t *out, uint32_t outlen, const uint8_t **in, uint32_t *inlen);

extern uint16_t eos_only_vid;           /* 0 = any supported camera; else only this USB vendor ID */
ptp_t *eos_open(void);                  /* first Canon or Sony still-image USB device, claimed; NULL if none */
#ifdef _WIN32
ptp_t *wpd_open(void);                  /* same, through the stock Windows MTP driver */
#endif
void eos_close(ptp_t *p);               /* leaves live view, closes the session, releases USB */
int eos_start_liveview(ptp_t *p);       /* PTP_OK on success */
int eos_set_prop(ptp_t *p, uint32_t prop, uint32_t value);
int eos_poll(ptp_t *p);                 /* drains events and keeps the camera awake; call ~1/s */
int eos_frame(ptp_t *p, const uint8_t **jpeg, uint32_t *len); /* 1 frame, 0 not ready, -1 error */
const uint8_t *eos_find_jpeg(const uint8_t *data, uint32_t len, uint32_t *jpeg_len);
const uint8_t *sony_find_jpeg(const uint8_t *data, uint32_t len, uint32_t *jpeg_len);
const char *eos_hint(ptp_t *p);         /* why a camera might be failing, if it can tell; else NULL */
long eos_setting(const char *name);     /* "iso" -> 0xD103, ...; -1 if unknown */
