#define COBJMACROS
#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <stdio.h>
#include <stdlib.h>

#include "../win/mcam_shm.h"
#include "ptp.h"
#include "sink.h"

/* --camera picks the slot: its own virtual camera, CLSID and frame section. */
static int slot(void) { return mcam_slot_for(eos_only_vid); }

/* IMFVirtualCamera (mfvirtualcamera.h, Windows 11 SDK) is missing from MinGW. It extends IMFAttributes;
   only the prefix of the vtable we call is declared, in SDK order. */
typedef struct IMFVirtualCamera IMFVirtualCamera;
typedef struct {
    IMFAttributesVtbl base;
    HRESULT(STDMETHODCALLTYPE *AddDeviceSourceInfo)(IMFVirtualCamera *, LPCWSTR);
    void *AddProperty;
    void *AddRegistryEntry;
    HRESULT(STDMETHODCALLTYPE *Start)(IMFVirtualCamera *, IMFAsyncCallback *);
    HRESULT(STDMETHODCALLTYPE *Stop)(IMFVirtualCamera *);
    HRESULT(STDMETHODCALLTYPE *Remove)(IMFVirtualCamera *);
} IMFVirtualCameraVtbl;
struct IMFVirtualCamera { const IMFVirtualCameraVtbl *lpVtbl; };

typedef HRESULT(WINAPI *create_vcam_fn)(int type, int lifetime, int access, LPCWSTR name, LPCWSTR source_id,
                                        const GUID *categories, ULONG ncategories, IMFVirtualCamera **out);
enum { SOFTWARE_CAMERA_SOURCE = 0 };
enum { LIFETIME_SESSION = 0, LIFETIME_SYSTEM = 1 };
enum { ACCESS_CURRENT_USER = 0, ACCESS_ALL_USERS = 1 };

#define WATCH_GRACE_MS 3000 /* keep the camera streaming this long after the last frame request */

struct sink {
    IMFVirtualCamera *vcam;
    HANDLE map;
    mcam_shm *shm;
    int w, h;
    LONG last_requests;
    ULONGLONG last_request_ms;
};

static HRESULT create_vcam(const char *name, int s, int lifetime, int access, IMFVirtualCamera **out)
{
    wchar_t wname[128];
    create_vcam_fn create;
    HMODULE m;

    MultiByteToWideChar(CP_UTF8, 0, name ? name : mcam_name[s], -1, wname, 128);
    CoInitializeEx(NULL, COINIT_MULTITHREADED);
    MFStartup(MF_VERSION, MFSTARTUP_LITE);
    if (!(m = LoadLibraryW(L"mfsensorgroup.dll")) ||
        !(create = (create_vcam_fn)(void (*)(void))GetProcAddress(m, "MFCreateVirtualCamera"))) {
        fprintf(stderr, "marksman-cam: MFCreateVirtualCamera not available (needs Windows 11)\n");
        return E_NOTIMPL;
    }
    return create(SOFTWARE_CAMERA_SOURCE, lifetime, access, wname, mcam_clsid_str[s], NULL, 0, out);
}

long vcam_install(int s)
{
    IMFVirtualCamera *vcam;
    HRESULT hr = create_vcam(NULL, s, LIFETIME_SYSTEM, ACCESS_ALL_USERS, &vcam);
    if (SUCCEEDED(hr)) {
        hr = vcam->lpVtbl->Start(vcam, NULL);
        vcam->lpVtbl->base.Release((IMFAttributes *)vcam); /* system lifetime: it stays registered */
    }
    return hr;
}

long vcam_uninstall(int s)
{
    IMFVirtualCamera *vcam;
    HRESULT hr = create_vcam(NULL, s, LIFETIME_SYSTEM, ACCESS_ALL_USERS, &vcam); /* same identity = same camera */
    if (SUCCEEDED(hr)) {
        hr = vcam->lpVtbl->Remove(vcam);
        vcam->lpVtbl->base.Release((IMFAttributes *)vcam);
    }
    return hr;
}

sink_t *sink_open_shared(int w, int h)
{
    sink_t *s = calloc(1, sizeof *s);
    if (!s)
        return NULL;
    s->w = w;
    s->h = h;
    s->shm = mcam_shm_open(&s->map, slot()); /* may only succeed once the Frame Server has created it */
    return s;
}

sink_t *sink_open(const char *device, int w, int h, int fps)
{
    sink_t *s = sink_open_shared(w, h);
    HRESULT hr;

    (void)fps; /* the media source advertises 30 fps */
    if (!s)
        return NULL;
    /* Session lifetime: Windows removes the camera when this process exits, even on a crash. */
    hr = create_vcam(device, slot(), LIFETIME_SESSION, ACCESS_CURRENT_USER, &s->vcam);
    if (SUCCEEDED(hr) && FAILED(hr = s->vcam->lpVtbl->Start(s->vcam, NULL)))
        fprintf(stderr, "marksman-cam: virtual camera Start failed 0x%08lx\n", (unsigned long)hr);
    if (FAILED(hr)) {
        fprintf(stderr, "marksman-cam: MFCreateVirtualCamera failed 0x%08lx; is marksman-cam-vcam.dll registered? (README)\n",
                (unsigned long)hr);
        sink_close(s);
        return NULL;
    }
    return s;
}

void sink_size(sink_t *s, int *w, int *h)
{
    if (!s->shm)
        s->shm = mcam_shm_open(&s->map, slot());
    *w = s->w;
    *h = s->h;
    if (s->shm && s->shm->req_width > 0 && s->shm->req_width <= MCAM_MAX_W && s->shm->req_height > 0 &&
        s->shm->req_height <= MCAM_MAX_H) {
        *w = s->shm->req_width & ~1;
        *h = s->shm->req_height & ~1;
    }
}

int sink_watched(sink_t *s)
{
    const ULONGLONG now = GetTickCount64();
    if (!s->shm)
        return 0;
    if (s->shm->requests != s->last_requests) {
        s->last_requests = s->shm->requests;
        s->last_request_ms = now;
    }
    return s->last_request_ms && now - s->last_request_ms < WATCH_GRACE_MS;
}

int sink_write(sink_t *s, const uint8_t *bgrx, const view_t *v)
{
    mcam_shm *shm = s->shm;
    if (!shm)
        return 0; /* nobody has opened the camera yet */
    if (v->out_w > MCAM_MAX_W || v->out_h > MCAM_MAX_H)
        return -1;
    const LONG i = !shm->latest; /* the slot the source is not being pointed at */
    mcam_slot *slot = &shm->slot[i];
    InterlockedIncrement(&slot->seq); /* odd: slot in flux */
    slot->width = v->out_w;
    slot->height = v->out_h;
    to_nv12(bgrx, v->out_w, v->out_h, v, shm->data[i]);
    InterlockedIncrement(&slot->seq);
    InterlockedExchange(&shm->latest, i);
    InterlockedIncrement(&shm->frames);
    return 0;
}

void sink_close(sink_t *s)
{
    if (!s)
        return;
    if (s->vcam) {
        s->vcam->lpVtbl->Remove(s->vcam);
        s->vcam->lpVtbl->base.Release((IMFAttributes *)s->vcam);
        MFShutdown();
        CoUninitialize();
    }
    if (s->shm)
        UnmapViewOfFile(s->shm);
    if (s->map)
        CloseHandle(s->map);
    free(s);
}
