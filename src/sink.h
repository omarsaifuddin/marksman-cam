/* Virtual camera output: v4l2loopback on Linux, a Windows 11 Media Foundation virtual camera on Windows. */
#pragma once
#include "image.h"

typedef struct sink sink_t;

sink_t *sink_open(const char *device, int w, int h, int fps); /* device NULL = platform default */
void sink_size(sink_t *s, int *w, int *h);  /* size to render next; on Windows the consuming app picks it */
int sink_watched(sink_t *s);                /* 1 while an app is reading frames; the camera idles otherwise */
int sink_write(sink_t *s, const uint8_t *bgrx, const view_t *v); /* frame is sink_size() big; 0 = ok */
void sink_close(sink_t *s);

#ifdef _WIN32
sink_t *sink_open_shared(int w, int h);     /* frames only; for the service, with the camera installed system-wide */
long vcam_install(const char *name);        /* persistent, all-users "Marksman Cam"; needs admin. HRESULT */
long vcam_uninstall(const char *name);
#endif
