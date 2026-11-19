/* Windows check: opens the "Marksman Cam" virtual camera like any app would and reads 90 frames.
   Luma 57 = marksman-cam.exe's "no camera" grey made it through; 48 = the DLL's own fallback (app not
   feeding frames); anything else = live picture. The timeline shows P (picture) / G (fallback grey) per
   frame: any G between Ps is a visible flicker. */
#define COBJMACROS
#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <stdio.h>
#include <wchar.h>

int main(void)
{
    IMFAttributes *attrs;
    IMFActivate **devs = NULL;
    UINT32 n = 0;
    int found = 0;

    CoInitializeEx(NULL, COINIT_MULTITHREADED);
    MFStartup(MF_VERSION, MFSTARTUP_FULL);
    MFCreateAttributes(&attrs, 1);
    IMFAttributes_SetGUID(attrs, &MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE, &MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID);
    if (FAILED(MFEnumDeviceSources(attrs, &devs, &n)))
        return 1;
    for (UINT32 i = 0; i < n; i++) {
        WCHAR *name = NULL;
        UINT32 len;
        IMFActivate_GetAllocatedString(devs[i], &MF_DEVSOURCE_ATTRIBUTE_FRIENDLY_NAME, &name, &len);
        wprintf(L"camera: %ls\n", name);
        if (name && wcsstr(name, L"Marksman Cam") && !found) {
            IMFMediaSource *src;
            IMFSourceReader *reader;
            IMFSample *sample = NULL;
            IMFMediaType *type;
            DWORD idx, flags;
            LONGLONG ts;
            UINT64 size = 0;
            HRESULT hr = IMFActivate_ActivateObject(devs[i], &IID_IMFMediaSource, (void **)&src);
            if (SUCCEEDED(hr))
                hr = MFCreateSourceReaderFromMediaSource(src, NULL, &reader);
            char timeline[91] = {0};
            int grey = 0, frames = 0, unique = 0;
            unsigned last_hash = 0;
            LARGE_INTEGER freq, t0, t1;
            QueryPerformanceFrequency(&freq);
            QueryPerformanceCounter(&t0);
            for (int tries = 0; SUCCEEDED(hr) && frames < 90 && tries < 200; tries++) {
                hr = IMFSourceReader_ReadSample(reader, MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0, &idx, &flags, &ts, &sample);
                if (FAILED(hr) || !sample)
                    continue;
                IMFMediaBuffer *buf;
                BYTE *p;
                DWORD cur;
                IMFSourceReader_GetCurrentMediaType(reader, MF_SOURCE_READER_FIRST_VIDEO_STREAM, &type);
                IMFMediaType_GetUINT64(type, &MF_MT_FRAME_SIZE, &size);
                IMFSample_ConvertToContiguousBuffer(sample, &buf);
                IMFMediaBuffer_Lock(buf, &p, NULL, &cur);
                const DWORD y = (DWORD)(size >> 32) * (DWORD)size; /* luma plane size */
                const int fallback = cur >= y * 3 / 2 && p[0] == 0x30 && p[y / 2] == 0x30 && p[y - 1] == 0x30 &&
                                     p[y] == 0x80 && p[y + y / 4] == 0x80;
                if (!frames)
                    printf("frame: %ux%u, %lu bytes, luma %u\n", (unsigned)(size >> 32), (unsigned)size, cur, p[cur / 3]);
                unsigned hash = 0; /* sparse luma checksum: tells new camera frames from repeats */
                for (DWORD k = 0; k < y; k += 997)
                    hash = hash * 31 + p[k];
                unique += hash != last_hash;
                last_hash = hash;
                timeline[frames++] = fallback ? 'G' : 'P';
                grey += fallback;
                IMFMediaBuffer_Unlock(buf);
                IMFMediaBuffer_Release(buf);
                IMFSample_Release(sample);
                sample = NULL;
            }
            QueryPerformanceCounter(&t1);
            if (frames) {
                const double secs = (double)(t1.QuadPart - t0.QuadPart) / (double)freq.QuadPart;
                printf("%d frames, %d fallback grey: %s\n", frames, grey, timeline);
                printf("delivered %.1f fps, distinct pictures %.1f fps\n", frames / secs, unique / secs);
                found = 1;
            } else {
                printf("read failed: 0x%08lx\n", (unsigned long)hr);
            }
        }
        CoTaskMemFree(name);
    }
    return found ? 0 : 1;
}
