/* Windows transport: raw PTP operations through the stock MTP driver (Windows Portable Devices MTP
   vendor-extension commands), so the camera keeps its normal driver. No Zadig/WinUSB needed. */
#include <windows.h>
#include <initguid.h> /* MinGW's libportabledeviceguids lacks these; define them here */
#include <portabledeviceapi.h>
#include <portabledevice.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

extern "C" {
#include "ptp.h"
}

/* From the Windows SDK's WpdMtpExtensions.h, which MinGW lacks. */
#define MTP_EXT(pid) {{0x4d545058, 0x1a2e, 0x4106, {0xa3, 0x57, 0x77, 0x1e, 0x08, 0x19, 0xfc, 0x56}}, pid}
static const PROPERTYKEY CMD_NO_DATA = MTP_EXT(12), CMD_READ = MTP_EXT(13), CMD_WRITE = MTP_EXT(14);
static const PROPERTYKEY CMD_READ_DATA = MTP_EXT(15), CMD_WRITE_DATA = MTP_EXT(16), CMD_END = MTP_EXT(17);
static const PROPERTYKEY OP_CODE = MTP_EXT(1001), OP_PARAMS = MTP_EXT(1002), RESP_CODE = MTP_EXT(1003);
static const PROPERTYKEY XFER_CONTEXT = MTP_EXT(1006), XFER_TOTAL = MTP_EXT(1007), XFER_TO_READ = MTP_EXT(1008);
static const PROPERTYKEY XFER_TO_WRITE = MTP_EXT(1010), XFER_DATA = MTP_EXT(1012);

static IPortableDeviceValues *values()
{
    IPortableDeviceValues *v = nullptr;
    CoCreateInstance(CLSID_PortableDeviceValues, nullptr, CLSCTX_INPROC_SERVER, IID_IPortableDeviceValues, (void **)&v);
    return v;
}

/* Sends one WPD command; *res receives the results. Fails if either the call or the driver failed. */
static HRESULT send(IPortableDevice *d, const PROPERTYKEY &cmd, IPortableDeviceValues *in, IPortableDeviceValues **res)
{
    HRESULT hr, driver_hr;
    *res = nullptr;
    in->SetGuidValue(WPD_PROPERTY_COMMON_COMMAND_CATEGORY, cmd.fmtid);
    in->SetUnsignedIntegerValue(WPD_PROPERTY_COMMON_COMMAND_ID, cmd.pid);
    hr = d->SendCommand(0, in, res);
    if (SUCCEEDED(hr) && SUCCEEDED((*res)->GetErrorValue(WPD_PROPERTY_COMMON_HRESULT, &driver_hr)))
        hr = driver_hr;
    if (FAILED(hr) && *res) {
        (*res)->Release();
        *res = nullptr;
    }
    return hr;
}

/* Finishes a data-phase operation and returns its response code. */
static int finish(IPortableDevice *d, LPCWSTR ctx)
{
    IPortableDeviceValues *v = values(), *res;
    ULONG rc = 0;
    if (!v)
        return -1;
    v->SetStringValue(XFER_CONTEXT, ctx);
    HRESULT hr = send(d, CMD_END, v, &res);
    v->Release();
    if (FAILED(hr))
        return -1;
    hr = res->GetUnsignedIntegerValue(RESP_CODE, &rc);
    res->Release();
    return SUCCEEDED(hr) ? (int)rc : -1;
}

static int wpd_op(ptp_t *p, uint16_t code, const uint32_t *params, int nparams, const uint8_t *out,
                  uint32_t outlen, const uint8_t **in, uint32_t *inlen)
{
    IPortableDevice *d = (IPortableDevice *)p->ctx;
    IPortableDevicePropVariantCollection *col = nullptr;
    IPortableDeviceValues *v = values(), *res = nullptr;
    LPWSTR ctx = nullptr;
    int rc = -1;

    if (!v || FAILED(CoCreateInstance(CLSID_PortableDevicePropVariantCollection, nullptr, CLSCTX_INPROC_SERVER,
                                      IID_IPortableDevicePropVariantCollection, (void **)&col)))
        goto done;
    for (int i = 0; i < nparams; i++) {
        PROPVARIANT pv;
        PropVariantInit(&pv);
        pv.vt = VT_UI4;
        pv.ulVal = params[i];
        col->Add(&pv);
    }
    v->SetUnsignedIntegerValue(OP_CODE, code);
    v->SetIUnknownValue(OP_PARAMS, col);

    if (!out && !in) {
        ULONG r;
        if (SUCCEEDED(send(d, CMD_NO_DATA, v, &res)) && SUCCEEDED(res->GetUnsignedIntegerValue(RESP_CODE, &r)))
            rc = (int)r;
        goto done;
    }
    if (out)
        v->SetUnsignedLargeIntegerValue(XFER_TOTAL, outlen);
    if (FAILED(send(d, out ? CMD_WRITE : CMD_READ, v, &res)) || FAILED(res->GetStringValue(XFER_CONTEXT, &ctx)))
        goto done;

    if (out) {
        IPortableDeviceValues *w = values(), *wres;
        if (!w)
            goto done;
        w->SetStringValue(XFER_CONTEXT, ctx);
        w->SetUnsignedIntegerValue(XFER_TO_WRITE, outlen);
        w->SetBufferValue(XFER_DATA, (BYTE *)out, outlen);
        HRESULT hr = send(d, CMD_WRITE_DATA, w, &wres);
        w->Release();
        if (FAILED(hr))
            goto done;
        wres->Release();
    } else {
        ULONGLONG total = 0;
        if (FAILED(res->GetUnsignedLargeIntegerValue(XFER_TOTAL, &total)) || total > PTP_BUF)
            goto done;
        if (total) {
            IPortableDeviceValues *r = values(), *rres;
            BYTE *data = nullptr;
            DWORD got = 0;
            if (!r)
                goto done;
            r->SetStringValue(XFER_CONTEXT, ctx);
            r->SetUnsignedIntegerValue(XFER_TO_READ, (ULONG)total);
            r->SetBufferValue(XFER_DATA, p->buf, (DWORD)total);
            HRESULT hr = send(d, CMD_READ_DATA, r, &rres);
            r->Release();
            if (FAILED(hr))
                goto done;
            hr = rres->GetBufferValue(XFER_DATA, &data, &got);
            rres->Release();
            if (FAILED(hr) || got > PTP_BUF) {
                CoTaskMemFree(data);
                goto done;
            }
            memcpy(p->buf, data, got);
            CoTaskMemFree(data);
            total = got;
        }
        *in = p->buf;
        *inlen = (uint32_t)total;
    }
    rc = finish(d, ctx);

done:
    CoTaskMemFree(ctx);
    if (res)
        res->Release();
    if (col)
        col->Release();
    if (v)
        v->Release();
    return rc;
}

static void wpd_release(ptp_t *p)
{
    IPortableDevice *d = (IPortableDevice *)p->ctx;
    d->Close();
    d->Release();
}

extern "C" ptp_t *wpd_open(void)
{
    IPortableDeviceManager *mgr = nullptr;
    PWSTR *ids = nullptr;
    DWORD n = 0;
    ptp_t *p = nullptr;

    CoInitializeEx(nullptr, COINIT_MULTITHREADED); /* S_FALSE / RPC_E_CHANGED_MODE are fine */
    if (FAILED(CoCreateInstance(CLSID_PortableDeviceManager, nullptr, CLSCTX_INPROC_SERVER, IID_IPortableDeviceManager,
                                (void **)&mgr)))
        return nullptr;
    mgr->RefreshDeviceList();
    if (FAILED(mgr->GetDevices(nullptr, &n)) || !n || !(ids = (PWSTR *)calloc(n, sizeof *ids)) ||
        FAILED(mgr->GetDevices(ids, &n)))
        n = 0;
    for (DWORD i = 0; i < n; i++) {
        int sony = _wcsnicmp(ids[i], L"\\\\?\\usb#vid_054c&", 17) == 0;
        int canon = _wcsnicmp(ids[i], L"\\\\?\\usb#vid_04a9&", 17) == 0;
        if (!p && ((sony && eos_only_vid != 0x04A9) || (canon && eos_only_vid != 0x054C))) {
            IPortableDeviceValues *client = values();
            IPortableDevice *dev = nullptr;
            if (client) {
                client->SetStringValue(WPD_CLIENT_NAME, L"marksman-cam");
                client->SetUnsignedIntegerValue(WPD_CLIENT_MAJOR_VERSION, 1);
                client->SetUnsignedIntegerValue(WPD_CLIENT_MINOR_VERSION, 0);
                client->SetUnsignedIntegerValue(WPD_CLIENT_REVISION, 0);
                client->SetUnsignedIntegerValue(WPD_CLIENT_SECURITY_QUALITY_OF_SERVICE, SECURITY_IMPERSONATION);
            }
            if (client &&
                SUCCEEDED(CoCreateInstance(CLSID_PortableDeviceFTM, nullptr, CLSCTX_INPROC_SERVER, IID_IPortableDevice,
                                           (void **)&dev)) &&
                SUCCEEDED(dev->Open(ids[i], client)) && (p = (ptp_t *)calloc(1, sizeof *p)) &&
                (p->buf = (uint8_t *)malloc(PTP_BUF))) {
                p->ctx = dev;
                p->op = wpd_op;
                p->release = wpd_release;
                p->sony = sony;
            } else {
                if (p)
                    free(p);
                p = nullptr;
                if (dev)
                    dev->Release();
            }
            if (client)
                client->Release();
        }
        CoTaskMemFree(ids[i]);
    }
    free(ids);
    mgr->Release();
    return p;
}
