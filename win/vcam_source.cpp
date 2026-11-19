/* Media Foundation custom media source behind the "Marksman Cam" virtual camera.
   marksman-cam.exe calls MFCreateVirtualCamera with MCAM_SOURCE_CLSID; the Windows Camera Frame Server then
   loads this DLL (as LocalService) and asks it for frames, which it copies out of the shared section that
   marksman-cam.exe fills. Advertises NV12 at 1280x720 (default), 1920x1080 and 640x360, 30 fps. */
#include <windows.h>
#include <sddl.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <ks.h>
#include <new>
#include <string.h>

#include "mcam_shm.h"

/* Not in MinGW headers; values from the Windows SDK (mfidl.h, mfapi.h, ksmedia.h, ks.h). */
static const GUID kStreamCategory    = {0x2939e7b8, 0xa62e, 0x4579, {0xb6, 0x74, 0xd4, 0x07, 0x3d, 0xfa, 0xbb, 0xba}};
static const GUID kStreamId          = {0x11bd5120, 0xd124, 0x446b, {0x88, 0xe6, 0x17, 0x06, 0x02, 0x57, 0xff, 0xf9}};
static const GUID kFrameServerShared = {0x1cb378e9, 0xb279, 0x41d4, {0xaf, 0x97, 0x34, 0xa2, 0x43, 0xe6, 0x83, 0x20}};
static const GUID kFrameSourceTypes  = {0x17145fd1, 0x1b2b, 0x423c, {0x80, 0x01, 0x2b, 0x68, 0x33, 0xed, 0x35, 0x88}};
static const GUID kPinVideoCapture   = {0xfb6c4281, 0x0353, 0x11d1, {0x90, 0x5f, 0x00, 0x00, 0xc0, 0xcc, 0x16, 0xba}};
static const IID kIID_IKsControl      = {0x28f54685, 0x06fd, 0x11d2, {0xb2, 0x7a, 0x00, 0xa0, 0xc9, 0x22, 0x31, 0x96}};
static const IID kIID_IMFMediaStream2 = {0xc5bc37d6, 0x75c7, 0x46a1, {0xa1, 0x32, 0x81, 0xb5, 0xf7, 0x23, 0xc2, 0x0f}};
static const CLSID kClsid = MCAM_SOURCE_CLSID;

/* User-mode IKsControl (ksproxy.h); MinGW's ks.h only has the kernel-mode NTSTATUS flavour. */
struct IKsControl_ : IUnknown {
    virtual HRESULT STDMETHODCALLTYPE KsProperty(PKSPROPERTY p, ULONG plen, LPVOID data, ULONG dlen, ULONG *ret) = 0;
    virtual HRESULT STDMETHODCALLTYPE KsMethod(PKSMETHOD m, ULONG mlen, LPVOID data, ULONG dlen, ULONG *ret) = 0;
    virtual HRESULT STDMETHODCALLTYPE KsEvent(PKSEVENT e, ULONG elen, LPVOID data, ULONG dlen, ULONG *ret) = 0;
};

struct IMFMediaStream2_ : IMFMediaStream {
    virtual HRESULT STDMETHODCALLTYPE SetStreamState(MF_STREAM_STATE value) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetStreamState(MF_STREAM_STATE *value) = 0;
};

static const UINT32 kSizes[][2] = {{1280, 720}, {1920, 1080}, {640, 360}};
static const UINT32 kFps = 30;
static const LONGLONG kFrame = 10000000 / kFps; /* 100 ns units */
static const LONGLONG kStale = 20000000;        /* app gone if no new frame for 2 s */
static LONG g_objects;
static HMODULE g_module;

#define KS_NOT_FOUND HRESULT_FROM_WIN32(ERROR_SET_NOT_FOUND) /* we expose no camera controls */

class Source;

class Stream final : public IMFMediaStream2_, public IKsControl_ {
public:
    Stream(Source *src, IMFStreamDescriptor *sd) : src_(src), sd_(sd)
    {
        sd_->AddRef();
        InitializeSRWLock(&lock_);
        InterlockedIncrement(&g_objects);
    }
    HRESULT Init() { return MFCreateEventQueue(&queue_); }

    STDMETHODIMP QueryInterface(REFIID iid, void **pp) override
    {
        if (!pp)
            return E_POINTER;
        if (iid == IID_IUnknown || iid == IID_IMFMediaEventGenerator || iid == IID_IMFMediaStream || iid == kIID_IMFMediaStream2)
            *pp = static_cast<IMFMediaStream2_ *>(this);
        else if (iid == kIID_IKsControl)
            *pp = static_cast<IKsControl_ *>(this);
        else
            return *pp = nullptr, E_NOINTERFACE;
        AddRef();
        return S_OK;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return InterlockedIncrement(&ref_); }
    STDMETHODIMP_(ULONG) Release() override
    {
        LONG n = InterlockedDecrement(&ref_);
        if (!n)
            delete this;
        return n;
    }

    STDMETHODIMP GetEvent(DWORD flags, IMFMediaEvent **e) override { return queue_->GetEvent(flags, e); }
    STDMETHODIMP BeginGetEvent(IMFAsyncCallback *cb, IUnknown *state) override { return queue_->BeginGetEvent(cb, state); }
    STDMETHODIMP EndGetEvent(IMFAsyncResult *r, IMFMediaEvent **e) override { return queue_->EndGetEvent(r, e); }
    STDMETHODIMP QueueEvent(MediaEventType t, REFGUID ext, HRESULT hr, const PROPVARIANT *v) override
    {
        return queue_->QueueEventParamVar(t, ext, hr, v);
    }

    STDMETHODIMP GetMediaSource(IMFMediaSource **pp) override;
    STDMETHODIMP GetStreamDescriptor(IMFStreamDescriptor **pp) override
    {
        if (!pp)
            return E_POINTER;
        (*pp = sd_)->AddRef();
        return S_OK;
    }
    STDMETHODIMP RequestSample(IUnknown *token) override;

    STDMETHODIMP SetStreamState(MF_STREAM_STATE value) override
    {
        AcquireSRWLockExclusive(&lock_);
        state_ = value;
        ReleaseSRWLockExclusive(&lock_);
        return S_OK;
    }
    STDMETHODIMP GetStreamState(MF_STREAM_STATE *value) override
    {
        if (!value)
            return E_POINTER;
        *value = state_;
        return S_OK;
    }

    STDMETHODIMP KsProperty(PKSPROPERTY, ULONG, LPVOID, ULONG, ULONG *) override { return KS_NOT_FOUND; }
    STDMETHODIMP KsMethod(PKSMETHOD, ULONG, LPVOID, ULONG, ULONG *) override { return KS_NOT_FOUND; }
    STDMETHODIMP KsEvent(PKSEVENT, ULONG, LPVOID, ULONG, ULONG *) override { return KS_NOT_FOUND; }

    HRESULT Start(UINT32 w, UINT32 h)
    {
        AcquireSRWLockExclusive(&lock_);
        w_ = w;
        h_ = h;
        next_ = 0;
        state_ = MF_STREAM_STATE_RUNNING;
        ReleaseSRWLockExclusive(&lock_);
        return queue_->QueueEventParamVar(MEStreamStarted, GUID_NULL, S_OK, nullptr);
    }
    HRESULT Stop()
    {
        SetStreamState(MF_STREAM_STATE_STOPPED);
        return queue_->QueueEventParamVar(MEStreamStopped, GUID_NULL, S_OK, nullptr);
    }
    void Shutdown()
    {
        AcquireSRWLockExclusive(&lock_);
        shutdown_ = true;
        state_ = MF_STREAM_STATE_STOPPED;
        ReleaseSRWLockExclusive(&lock_);
        queue_->Shutdown(); /* keep the pointer: queue calls now fail with MF_E_SHUTDOWN instead of racing */
    }

private:
    ~Stream()
    {
        if (queue_)
            queue_->Release();
        sd_->Release();
        if (shm_)
            UnmapViewOfFile(shm_);
        if (map_)
            CloseHandle(map_);
        free(last_);
        InterlockedDecrement(&g_objects);
    }

    /* Copies the newest frame from marksman-cam.exe. If that read is ever torn, repeats the last good frame;
       "no signal" grey only when the app has stopped publishing. Called under lock_. */
    void Fill(BYTE *dst, UINT32 w, UINT32 h, LONGLONG now)
    {
        const size_t size = (size_t)w * h * 3 / 2;
        if (!shm_)
            shm_ = mcam_shm_open(&map_);
        if (shm_) {
            shm_->req_width = (LONG)w;
            shm_->req_height = (LONG)h;
            InterlockedIncrement(&shm_->requests); /* the app only wakes the camera while this moves */
            const LONG frames = shm_->frames;
            if (frames != last_frames_) {
                last_frames_ = frames;
                last_change_ = now;
            }
            if (now - last_change_ < kStale) {
                const LONG i = shm_->latest & 1;
                const mcam_slot *slot = &shm_->slot[i];
                const LONG seq = slot->seq;
                MemoryBarrier();
                if (!(seq & 1) && (UINT32)slot->width == w && (UINT32)slot->height == h) {
                    memcpy(dst, shm_->data[i], size);
                    MemoryBarrier();
                    if (slot->seq == seq) {
                        if (last_ || (last_ = (BYTE *)malloc(sizeof shm_->data[0]))) {
                            memcpy(last_, dst, size);
                            last_w_ = w;
                            last_h_ = h;
                        }
                        return;
                    }
                }
                if (last_ && last_w_ == w && last_h_ == h) {
                    memcpy(dst, last_, size);
                    return;
                }
            }
        }
        memset(dst, 0x30, (size_t)w * h);
        memset(dst + (size_t)w * h, 0x80, (size_t)w * h / 2);
    }

    LONG ref_ = 1;
    Source *src_;
    IMFStreamDescriptor *sd_;
    IMFMediaEventQueue *queue_ = nullptr;
    SRWLOCK lock_;
    MF_STREAM_STATE state_ = MF_STREAM_STATE_STOPPED;
    bool shutdown_ = false;
    UINT32 w_ = 0, h_ = 0;
    LONGLONG next_ = 0, last_change_ = 0;
    LONG last_frames_ = -1;
    HANDLE map_ = nullptr;
    mcam_shm *shm_ = nullptr;
    BYTE *last_ = nullptr; /* last good frame, repeated if a read is torn */
    UINT32 last_w_ = 0, last_h_ = 0;
};

HRESULT Stream::RequestSample(IUnknown *token)
{
    IMFMediaBuffer *buf = nullptr;
    IMFSample *sample = nullptr;
    BYTE *p;
    HRESULT hr;

    AcquireSRWLockExclusive(&lock_);
    if (shutdown_ || state_ != MF_STREAM_STATE_RUNNING) {
        ReleaseSRWLockExclusive(&lock_);
        return shutdown_ ? MF_E_SHUTDOWN : MF_E_INVALIDREQUEST;
    }
    const UINT32 w = w_, h = h_;
    const DWORD size = w * h * 3 / 2;
    LONGLONG now = MFGetSystemTime();
    const LONGLONG wait = next_ - now;
    next_ = (wait > 0 ? next_ : now) + kFrame;
    ReleaseSRWLockExclusive(&lock_);

    /* ponytail: paces by sleeping on the caller's thread (at most one frame); use MFScheduleWorkItem if that
       ever starves the pipeline. */
    if (wait > 0)
        Sleep((DWORD)(wait / 10000));

    hr = MFCreateMemoryBuffer(size, &buf);
    if (SUCCEEDED(hr))
        hr = buf->Lock(&p, nullptr, nullptr);
    if (SUCCEEDED(hr)) {
        AcquireSRWLockExclusive(&lock_);
        Fill(p, w, h, now = MFGetSystemTime());
        ReleaseSRWLockExclusive(&lock_);
        buf->Unlock();
        hr = buf->SetCurrentLength(size);
    }
    if (SUCCEEDED(hr))
        hr = MFCreateSample(&sample);
    if (SUCCEEDED(hr))
        hr = sample->AddBuffer(buf);
    if (SUCCEEDED(hr))
        hr = sample->SetSampleTime(now);
    if (SUCCEEDED(hr))
        hr = sample->SetSampleDuration(kFrame);
    if (SUCCEEDED(hr) && token)
        hr = sample->SetUnknown(MFSampleExtension_Token, token);
    if (SUCCEEDED(hr))
        hr = queue_->QueueEventParamUnk(MEMediaSample, GUID_NULL, S_OK, sample);
    if (sample)
        sample->Release();
    if (buf)
        buf->Release();
    return hr;
}

static HRESULT MakeType(UINT32 w, UINT32 h, IMFMediaType **out)
{
    IMFMediaType *t;
    HRESULT hr = MFCreateMediaType(&t);
    if (FAILED(hr))
        return hr;
    t->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    t->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
    t->SetUINT64(MF_MT_FRAME_SIZE, (UINT64)w << 32 | h);
    t->SetUINT64(MF_MT_FRAME_RATE, (UINT64)kFps << 32 | 1);
    t->SetUINT64(MF_MT_PIXEL_ASPECT_RATIO, (UINT64)1 << 32 | 1);
    t->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    t->SetUINT32(MF_MT_ALL_SAMPLES_INDEPENDENT, TRUE);
    t->SetUINT32(MF_MT_DEFAULT_STRIDE, w);
    t->SetUINT32(MF_MT_SAMPLE_SIZE, w * h * 3 / 2);
    *out = t;
    return S_OK;
}

class Source final : public IMFMediaSourceEx, public IMFGetService, public IKsControl_ {
public:
    Source()
    {
        InitializeSRWLock(&lock_);
        InterlockedIncrement(&g_objects);
    }

    HRESULT Init(IMFAttributes *from)
    {
        const UINT32 n = sizeof kSizes / sizeof kSizes[0];
        IMFMediaType *types[n] = {};
        IMFStreamDescriptor *sd = nullptr;
        IMFMediaTypeHandler *mth = nullptr;
        HRESULT hr = MFCreateEventQueue(&queue_);
        if (SUCCEEDED(hr))
            hr = MFCreateAttributes(&attrs_, 4);
        if (SUCCEEDED(hr) && from)
            hr = from->CopyAllItems(attrs_);
        for (UINT32 i = 0; i < n && SUCCEEDED(hr); i++)
            hr = MakeType(kSizes[i][0], kSizes[i][1], &types[i]);
        if (SUCCEEDED(hr))
            hr = MFCreateStreamDescriptor(0, n, types, &sd);
        if (SUCCEEDED(hr))
            hr = sd->SetGUID(kStreamCategory, kPinVideoCapture);
        if (SUCCEEDED(hr))
            hr = sd->SetUINT32(kStreamId, 0);
        if (SUCCEEDED(hr))
            hr = sd->SetUINT32(kFrameServerShared, 1);
        if (SUCCEEDED(hr))
            hr = sd->SetUINT32(kFrameSourceTypes, 1 /* MFFrameSourceTypes_Color */);
        if (SUCCEEDED(hr))
            hr = sd->GetMediaTypeHandler(&mth);
        if (SUCCEEDED(hr))
            hr = mth->SetCurrentMediaType(types[0]);
        if (SUCCEEDED(hr))
            hr = MFCreatePresentationDescriptor(1, &sd, &pd_);
        if (SUCCEEDED(hr))
            hr = pd_->SelectStream(0);
        if (SUCCEEDED(hr))
            hr = (stream_ = new (std::nothrow) Stream(this, sd)) ? stream_->Init() : E_OUTOFMEMORY;
        for (UINT32 i = 0; i < n; i++)
            if (types[i])
                types[i]->Release();
        if (mth)
            mth->Release();
        if (sd)
            sd->Release();
        return hr;
    }

    STDMETHODIMP QueryInterface(REFIID iid, void **pp) override
    {
        if (!pp)
            return E_POINTER;
        if (iid == IID_IUnknown || iid == IID_IMFMediaEventGenerator || iid == IID_IMFMediaSource || iid == IID_IMFMediaSourceEx)
            *pp = static_cast<IMFMediaSourceEx *>(this);
        else if (iid == IID_IMFGetService)
            *pp = static_cast<IMFGetService *>(this);
        else if (iid == kIID_IKsControl)
            *pp = static_cast<IKsControl_ *>(this);
        else
            return *pp = nullptr, E_NOINTERFACE;
        AddRef();
        return S_OK;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return InterlockedIncrement(&ref_); }
    STDMETHODIMP_(ULONG) Release() override
    {
        LONG n = InterlockedDecrement(&ref_);
        if (!n)
            delete this;
        return n;
    }

    STDMETHODIMP GetEvent(DWORD flags, IMFMediaEvent **e) override { return queue_->GetEvent(flags, e); }
    STDMETHODIMP BeginGetEvent(IMFAsyncCallback *cb, IUnknown *state) override { return queue_->BeginGetEvent(cb, state); }
    STDMETHODIMP EndGetEvent(IMFAsyncResult *r, IMFMediaEvent **e) override { return queue_->EndGetEvent(r, e); }
    STDMETHODIMP QueueEvent(MediaEventType t, REFGUID ext, HRESULT hr, const PROPVARIANT *v) override
    {
        return queue_->QueueEventParamVar(t, ext, hr, v);
    }

    STDMETHODIMP GetCharacteristics(DWORD *c) override
    {
        if (!c)
            return E_POINTER;
        if (shutdown_)
            return MF_E_SHUTDOWN;
        *c = MFMEDIASOURCE_IS_LIVE;
        return S_OK;
    }
    STDMETHODIMP CreatePresentationDescriptor(IMFPresentationDescriptor **pp) override
    {
        if (!pp)
            return E_POINTER;
        return shutdown_ ? MF_E_SHUTDOWN : pd_->Clone(pp);
    }
    STDMETHODIMP Start(IMFPresentationDescriptor *pd, const GUID *format, const PROPVARIANT *) override
    {
        IMFStreamDescriptor *sd = nullptr;
        IMFMediaTypeHandler *mth = nullptr;
        IMFMediaType *type = nullptr;
        UINT64 size = 0;
        BOOL selected = FALSE;
        HRESULT hr;

        if (!pd)
            return E_INVALIDARG;
        if (format && *format != GUID_NULL)
            return MF_E_UNSUPPORTED_TIME_FORMAT;
        AcquireSRWLockExclusive(&lock_);
        hr = shutdown_ ? MF_E_SHUTDOWN : pd->GetStreamDescriptorByIndex(0, &selected, &sd);
        if (SUCCEEDED(hr))
            hr = sd->GetMediaTypeHandler(&mth);
        if (SUCCEEDED(hr))
            hr = mth->GetCurrentMediaType(&type);
        if (SUCCEEDED(hr))
            hr = type->GetUINT64(MF_MT_FRAME_SIZE, &size);
        if (SUCCEEDED(hr) && selected) {
            hr = queue_->QueueEventParamUnk(started_ ? MEUpdatedStream : MENewStream, GUID_NULL, S_OK,
                                            static_cast<IMFMediaStream *>(stream_));
            started_ = true;
        }
        if (SUCCEEDED(hr)) {
            PROPVARIANT t;
            PropVariantInit(&t);
            t.vt = VT_I8;
            t.hVal.QuadPart = MFGetSystemTime(); /* live source: report when we actually started */
            hr = queue_->QueueEventParamVar(MESourceStarted, GUID_NULL, S_OK, &t);
        }
        if (SUCCEEDED(hr))
            hr = selected ? stream_->Start((UINT32)(size >> 32), (UINT32)size) : stream_->Stop();
        ReleaseSRWLockExclusive(&lock_);
        if (type)
            type->Release();
        if (mth)
            mth->Release();
        if (sd)
            sd->Release();
        return hr;
    }
    STDMETHODIMP Stop() override
    {
        if (shutdown_)
            return MF_E_SHUTDOWN;
        stream_->Stop();
        return queue_->QueueEventParamVar(MESourceStopped, GUID_NULL, S_OK, nullptr);
    }
    STDMETHODIMP Pause() override { return MF_E_INVALID_STATE_TRANSITION; }
    STDMETHODIMP Shutdown() override
    {
        AcquireSRWLockExclusive(&lock_);
        if (!shutdown_) {
            shutdown_ = true;
            queue_->Shutdown();
            stream_->Shutdown();
        }
        ReleaseSRWLockExclusive(&lock_);
        return S_OK;
    }

    STDMETHODIMP GetSourceAttributes(IMFAttributes **pp) override
    {
        if (!pp)
            return E_POINTER;
        (*pp = attrs_)->AddRef();
        return S_OK;
    }
    STDMETHODIMP GetStreamAttributes(DWORD id, IMFAttributes **pp) override
    {
        IMFStreamDescriptor *sd;
        if (!pp)
            return E_POINTER;
        if (id != 0)
            return MF_E_INVALIDSTREAMNUMBER;
        HRESULT hr = stream_->GetStreamDescriptor(&sd);
        if (SUCCEEDED(hr)) {
            hr = sd->QueryInterface(IID_IMFAttributes, (void **)pp);
            sd->Release();
        }
        return hr;
    }
    STDMETHODIMP SetD3DManager(IUnknown *) override { return S_OK; } /* system-memory samples only */

    STDMETHODIMP GetService(REFGUID, REFIID, LPVOID *pp) override
    {
        if (pp)
            *pp = nullptr;
        return MF_E_UNSUPPORTED_SERVICE;
    }

    STDMETHODIMP KsProperty(PKSPROPERTY, ULONG, LPVOID, ULONG, ULONG *) override { return KS_NOT_FOUND; }
    STDMETHODIMP KsMethod(PKSMETHOD, ULONG, LPVOID, ULONG, ULONG *) override { return KS_NOT_FOUND; }
    STDMETHODIMP KsEvent(PKSEVENT, ULONG, LPVOID, ULONG, ULONG *) override { return KS_NOT_FOUND; }

    bool IsShutdown() const { return shutdown_; }

private:
    ~Source()
    {
        if (stream_)
            stream_->Release();
        if (pd_)
            pd_->Release();
        if (attrs_)
            attrs_->Release();
        if (queue_)
            queue_->Release();
        InterlockedDecrement(&g_objects);
    }

    LONG ref_ = 1;
    SRWLOCK lock_;
    bool shutdown_ = false, started_ = false;
    IMFMediaEventQueue *queue_ = nullptr;
    IMFAttributes *attrs_ = nullptr;
    IMFPresentationDescriptor *pd_ = nullptr;
    Stream *stream_ = nullptr;
};

HRESULT Stream::GetMediaSource(IMFMediaSource **pp)
{
    if (!pp)
        return E_POINTER;
    if (shutdown_ || src_->IsShutdown())
        return MF_E_SHUTDOWN;
    return src_->QueryInterface(IID_IMFMediaSource, (void **)pp);
}

/* The Frame Server instantiates the CLSID as an IMFActivate and activates the source from it. */
class Activator final : public IMFActivate {
public:
    Activator() { InterlockedIncrement(&g_objects); }
    HRESULT Init() { return MFCreateAttributes(&attrs_, 1); }

    STDMETHODIMP QueryInterface(REFIID iid, void **pp) override
    {
        if (!pp)
            return E_POINTER;
        if (iid != IID_IUnknown && iid != IID_IMFAttributes && iid != IID_IMFActivate)
            return *pp = nullptr, E_NOINTERFACE;
        *pp = static_cast<IMFActivate *>(this);
        AddRef();
        return S_OK;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return InterlockedIncrement(&ref_); }
    STDMETHODIMP_(ULONG) Release() override
    {
        LONG n = InterlockedDecrement(&ref_);
        if (!n)
            delete this;
        return n;
    }

    STDMETHODIMP ActivateObject(REFIID iid, void **pp) override
    {
        if (!src_) {
            if (!(src_ = new (std::nothrow) Source))
                return E_OUTOFMEMORY;
            HRESULT hr = src_->Init(attrs_);
            if (FAILED(hr)) {
                src_->Release();
                src_ = nullptr;
                return hr;
            }
        }
        return src_->QueryInterface(iid, pp);
    }
    STDMETHODIMP ShutdownObject() override
    {
        if (src_) {
            src_->Shutdown();
            src_->Release();
            src_ = nullptr;
        }
        return S_OK;
    }
    STDMETHODIMP DetachObject() override
    {
        if (src_) {
            src_->Release();
            src_ = nullptr;
        }
        return S_OK;
    }

    /* IMFAttributes: forwarded to attrs_ */
    STDMETHODIMP GetItem(REFGUID k, PROPVARIANT *v) override { return attrs_->GetItem(k, v); }
    STDMETHODIMP GetItemType(REFGUID k, MF_ATTRIBUTE_TYPE *t) override { return attrs_->GetItemType(k, t); }
    STDMETHODIMP CompareItem(REFGUID k, REFPROPVARIANT v, BOOL *r) override { return attrs_->CompareItem(k, v, r); }
    STDMETHODIMP Compare(IMFAttributes *o, MF_ATTRIBUTES_MATCH_TYPE m, BOOL *r) override { return attrs_->Compare(o, m, r); }
    STDMETHODIMP GetUINT32(REFGUID k, UINT32 *v) override { return attrs_->GetUINT32(k, v); }
    STDMETHODIMP GetUINT64(REFGUID k, UINT64 *v) override { return attrs_->GetUINT64(k, v); }
    STDMETHODIMP GetDouble(REFGUID k, double *v) override { return attrs_->GetDouble(k, v); }
    STDMETHODIMP GetGUID(REFGUID k, GUID *v) override { return attrs_->GetGUID(k, v); }
    STDMETHODIMP GetStringLength(REFGUID k, UINT32 *n) override { return attrs_->GetStringLength(k, n); }
    STDMETHODIMP GetString(REFGUID k, LPWSTR s, UINT32 cap, UINT32 *n) override { return attrs_->GetString(k, s, cap, n); }
    STDMETHODIMP GetAllocatedString(REFGUID k, LPWSTR *s, UINT32 *n) override { return attrs_->GetAllocatedString(k, s, n); }
    STDMETHODIMP GetBlobSize(REFGUID k, UINT32 *n) override { return attrs_->GetBlobSize(k, n); }
    STDMETHODIMP GetBlob(REFGUID k, UINT8 *b, UINT32 cap, UINT32 *n) override { return attrs_->GetBlob(k, b, cap, n); }
    STDMETHODIMP GetAllocatedBlob(REFGUID k, UINT8 **b, UINT32 *n) override { return attrs_->GetAllocatedBlob(k, b, n); }
    STDMETHODIMP GetUnknown(REFGUID k, REFIID iid, LPVOID *pp) override { return attrs_->GetUnknown(k, iid, pp); }
    STDMETHODIMP SetItem(REFGUID k, REFPROPVARIANT v) override { return attrs_->SetItem(k, v); }
    STDMETHODIMP DeleteItem(REFGUID k) override { return attrs_->DeleteItem(k); }
    STDMETHODIMP DeleteAllItems() override { return attrs_->DeleteAllItems(); }
    STDMETHODIMP SetUINT32(REFGUID k, UINT32 v) override { return attrs_->SetUINT32(k, v); }
    STDMETHODIMP SetUINT64(REFGUID k, UINT64 v) override { return attrs_->SetUINT64(k, v); }
    STDMETHODIMP SetDouble(REFGUID k, double v) override { return attrs_->SetDouble(k, v); }
    STDMETHODIMP SetGUID(REFGUID k, REFGUID v) override { return attrs_->SetGUID(k, v); }
    STDMETHODIMP SetString(REFGUID k, LPCWSTR v) override { return attrs_->SetString(k, v); }
    STDMETHODIMP SetBlob(REFGUID k, const UINT8 *b, UINT32 n) override { return attrs_->SetBlob(k, b, n); }
    STDMETHODIMP SetUnknown(REFGUID k, IUnknown *v) override { return attrs_->SetUnknown(k, v); }
    STDMETHODIMP LockStore() override { return attrs_->LockStore(); }
    STDMETHODIMP UnlockStore() override { return attrs_->UnlockStore(); }
    STDMETHODIMP GetCount(UINT32 *n) override { return attrs_->GetCount(n); }
    STDMETHODIMP GetItemByIndex(UINT32 i, GUID *k, PROPVARIANT *v) override { return attrs_->GetItemByIndex(i, k, v); }
    STDMETHODIMP CopyAllItems(IMFAttributes *dst) override { return attrs_->CopyAllItems(dst); }

private:
    ~Activator()
    {
        if (src_)
            src_->Release();
        if (attrs_)
            attrs_->Release();
        InterlockedDecrement(&g_objects);
    }

    LONG ref_ = 1;
    IMFAttributes *attrs_ = nullptr;
    Source *src_ = nullptr;
};

class Factory final : public IClassFactory {
public:
    STDMETHODIMP QueryInterface(REFIID iid, void **pp) override
    {
        if (!pp)
            return E_POINTER;
        if (iid != IID_IUnknown && iid != IID_IClassFactory)
            return *pp = nullptr, E_NOINTERFACE;
        *pp = this;
        return S_OK;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return 2; } /* static object */
    STDMETHODIMP_(ULONG) Release() override { return 1; }
    STDMETHODIMP CreateInstance(IUnknown *outer, REFIID iid, void **pp) override
    {
        if (outer)
            return CLASS_E_NOAGGREGATION;
        Activator *a = new (std::nothrow) Activator;
        if (!a)
            return E_OUTOFMEMORY;
        HRESULT hr = a->Init();
        if (SUCCEEDED(hr))
            hr = a->QueryInterface(iid, pp);
        a->Release();
        return hr;
    }
    STDMETHODIMP LockServer(BOOL lock) override
    {
        lock ? InterlockedIncrement(&g_objects) : InterlockedDecrement(&g_objects);
        return S_OK;
    }
};

static Factory g_factory;
#define CLSID_KEY L"SOFTWARE\\Classes\\CLSID\\" MCAM_SOURCE_CLSID_STR

extern "C" BOOL WINAPI DllMain(HINSTANCE h, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH) {
        g_module = h;
        DisableThreadLibraryCalls(h);
    }
    return TRUE;
}

STDAPI DllGetClassObject(REFCLSID clsid, REFIID iid, LPVOID *pp)
{
    return clsid == kClsid ? g_factory.QueryInterface(iid, pp) : CLASS_E_CLASSNOTAVAILABLE;
}

STDAPI DllCanUnloadNow(void)
{
    return g_objects ? S_FALSE : S_OK;
}

/* HKLM, not HKCU: the Frame Server service resolves the CLSID as LocalService. Needs an elevated regsvr32. */
STDAPI DllRegisterServer(void)
{
    wchar_t path[MAX_PATH];
    DWORD n = GetModuleFileNameW(g_module, path, MAX_PATH);
    if (!n || n == MAX_PATH)
        return E_FAIL;
    LSTATUS s = RegSetKeyValueW(HKEY_LOCAL_MACHINE, CLSID_KEY, nullptr, REG_SZ, L"Marksman Cam Source", sizeof L"Marksman Cam Source");
    if (!s)
        s = RegSetKeyValueW(HKEY_LOCAL_MACHINE, CLSID_KEY L"\\InprocServer32", nullptr, REG_SZ, path, (n + 1) * sizeof(wchar_t));
    if (!s)
        s = RegSetKeyValueW(HKEY_LOCAL_MACHINE, CLSID_KEY L"\\InprocServer32", L"ThreadingModel", REG_SZ, L"Both", sizeof L"Both");
    return HRESULT_FROM_WIN32(s);
}

STDAPI DllUnregisterServer(void)
{
    LSTATUS s = RegDeleteTreeW(HKEY_LOCAL_MACHINE, CLSID_KEY);
    return s == ERROR_FILE_NOT_FOUND ? S_OK : HRESULT_FROM_WIN32(s);
}
