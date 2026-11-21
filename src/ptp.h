/* PTP over USB bulk (PIMA 15740) plus the Canon EOS and Sony (PC Remote) live-view vendor operations.
   The eos_* entry points serve both vendors and dispatch on ptp_t.sony. */
#pragma once
#include <stdint.h>
#include <stddef.h>

#define PTP_OK  0x2001
#define PTP_BUF (8u << 20) /* one container; live-view JPEGs are well under this */

#define PTP_MAX_PROPS 128
#define PTP_MAX_OPTS  64

/* A camera property as the camera last reported it. Canon reports values and the list of allowed values
   in its event stream; Sony in its all-properties block. */
typedef struct {
    uint16_t code;  /* property code, e.g. 0xD103 (Canon ISO) */
    uint16_t type;  /* PTP datatype (Sony); Canon values are handled as 32-bit */
    uint8_t form;   /* 0 none, 1 range (min/max/step), 2 list (opts) */
    uint8_t ro;     /* the camera says it can't be changed right now */
    int64_t value, min, max, step;
    int nopts;
    int64_t opts[PTP_MAX_OPTS];
} ptp_prop_t;

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
    ptp_prop_t *props; /* PTP_MAX_PROPS entries, allocated on first use; nprops in use */
    int nprops;
    unsigned props_seq; /* bumped whenever props change */
    int af_hold;        /* Sony: polls left before the half-press from eos_autofocus is let go */
};

/* Runs one operation. Returns the PTP response code, or -1 on a transport/protocol error.
   Pass in/inlen whenever the operation has a data-in phase: the WPD transport must know up front. */
int ptp_transact(ptp_t *p, uint16_t code, const uint32_t *params, int nparams,
                 const uint8_t *out, uint32_t outlen, const uint8_t **in, uint32_t *inlen);

extern uint16_t eos_only_vid;           /* 0 = any supported camera; else only this USB vendor ID */
extern const char *eos_only_serial;     /* NULL = any; else only the camera with this USB serial number */
int eos_serial_ok(const char *serial);  /* serial passes the --serial filter (case-insensitive) */
ptp_t *eos_open(void);                  /* first Canon or Sony still-image USB device, claimed; NULL if none */
void eos_wait_device(double seconds);   /* returns early when a camera is plugged in (Linux hotplug) */
void eos_list(void);                    /* prints the connected cameras and their serial numbers */
#ifdef _WIN32
ptp_t *wpd_open(void);                  /* same, through the stock Windows MTP driver */
void wpd_list(void);
#endif
void eos_close(ptp_t *p);               /* leaves live view, closes the session, releases USB */
int eos_start_liveview(ptp_t *p);       /* PTP_OK on success */
int eos_set_prop(ptp_t *p, uint32_t prop, int64_t value);
int eos_poll(ptp_t *p);                 /* drains events, refreshes props, keeps the camera awake; call ~1/s */
int eos_frame(ptp_t *p, const uint8_t **jpeg, uint32_t *len); /* 1 frame, 0 not ready, -1 error */
int eos_autofocus(ptp_t *p);            /* one autofocus run */
int eos_focus(ptp_t *p, int step);      /* drive the lens: -3..-1 nearer (fine..coarse), 1..3 farther */
const uint8_t *eos_find_jpeg(const uint8_t *data, uint32_t len, uint32_t *jpeg_len);
const uint8_t *sony_find_jpeg(const uint8_t *data, uint32_t len, uint32_t *jpeg_len);
const char *eos_hint(ptp_t *p);         /* why a camera might be failing, if it can tell; else NULL */

/* Property bookkeeping, shared by both vendors. */
void canon_parse_events(ptp_t *p, const uint8_t *d, uint32_t n);
void sony_parse_props(ptp_t *p, const uint8_t *d, uint32_t n);
const ptp_prop_t *eos_prop(const ptp_t *p, uint16_t code);
long eos_setting(int sony, const char *name); /* "iso" -> 0xD103 (Canon) / 0xD21E (Sony); "p_D1B0" or "0xD1B0" -> code; -1 */
const char *eos_prop_name(int sony, uint16_t code, char buf[8]);   /* "iso", or "p_D1B0" written to buf */
const char *eos_prop_label(int sony, uint16_t code);              /* "ISO"; NULL for properties without one */
void eos_value_label(int sony, uint16_t code, int64_t v, char *buf, size_t n); /* "1/125", "f/5.6", ... */
