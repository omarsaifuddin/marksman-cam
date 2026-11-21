/* Virtual camera output: v4l2loopback on Linux, a Windows 11 Media Foundation virtual camera on Windows. */
#pragma once
#include "image.h"

typedef struct sink sink_t;

sink_t *sink_open(const char *device, int w, int h, int fps); /* device NULL = platform default */
void sink_size(sink_t *s, int *w, int *h);  /* size to render next; on Windows the consuming app picks it */
int sink_watched(sink_t *s);                /* 1 while an app is reading frames; the camera idles otherwise */
int sink_write(sink_t *s, const yuv_t *f, const view_t *v); /* renders f (NULL: "no camera") and sends it; 0 = ok */
int sink_repeat(sink_t *s);                 /* no new picture: keep the stream alive with the last one; 0 = ok */
int sink_controls(sink_t *s, view_t *v);    /* an app changed the picture controls (Windows camera settings): 1 */
void sink_set_controls(sink_t *s, const view_t *v); /* tell apps the current picture controls */
void sink_close(sink_t *s);

#ifdef _WIN32
/* On Windows, --camera (eos_only_vid) picks the slot: "Marksman Cam", "Marksman Cam (Canon)" or "(Sony)". */
sink_t *sink_open_shared(int w, int h);     /* frames only; for the service, with the camera installed system-wide */
long vcam_install(int slot);                /* persistent, all-users virtual camera for a slot; needs admin. HRESULT */
long vcam_uninstall(int slot);
#endif
