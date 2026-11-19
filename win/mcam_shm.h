/* Frame hand-off between marksman-cam.exe and the media source DLL running inside the Frame Server
   service (LocalService, session 0). Whichever side starts first creates the section: normal users may
   lack "create global objects", LocalService always has it. */
#pragma once
#include <windows.h>
#include <sddl.h>

#define MCAM_SOURCE_CLSID_STR L"{815866E8-7D99-481E-A357-6B004C6761CC}"
#define MCAM_SOURCE_CLSID {0x815866E8, 0x7D99, 0x481E, {0xA3, 0x57, 0x6B, 0x00, 0x4C, 0x67, 0x61, 0xCC}}
#define MCAM_SHM_NAME L"Global\\MarksmanCamFrame3" /* bump the suffix whenever the layout changes */
#define MCAM_SHM_SDDL L"D:P(A;;GA;;;SY)(A;;GA;;;LS)(A;;GA;;;BA)(A;;GA;;;IU)"
#define MCAM_MAX_W 1920
#define MCAM_MAX_H 1080

typedef struct {
    volatile LONG seq;           /* odd while the app is writing this slot */
    volatile LONG width, height; /* NV12 frame in the slot, pitch = width */
    LONG reserved;
} mcam_slot;

/* Double buffered: the app writes the slot that `latest` does not point at, then flips `latest`, so the
   source never reads a frame that is being written (a single buffer made it fall back to grey: flicker). */
typedef struct {
    volatile LONG latest;                /* slot holding the newest complete frame */
    volatile LONG frames;                /* bumped per published frame; the source's "app alive" signal */
    volatile LONG req_width, req_height; /* size the consuming app selected; written by the source */
    volatile LONG requests;              /* bumped by the source per frame request: someone is watching */
    LONG reserved;
    mcam_slot slot[2];
    BYTE data[2][MCAM_MAX_W * MCAM_MAX_H * 3 / 2];
} mcam_shm;

static inline mcam_shm *mcam_shm_open(HANDLE *map)
{
    SECURITY_ATTRIBUTES sa = {sizeof sa, NULL, FALSE};
    void *view;
    *map = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, MCAM_SHM_NAME);
    if (!*map && ConvertStringSecurityDescriptorToSecurityDescriptorW(MCAM_SHM_SDDL, SDDL_REVISION_1,
                                                                       &sa.lpSecurityDescriptor, NULL)) {
        *map = CreateFileMappingW(INVALID_HANDLE_VALUE, &sa, PAGE_READWRITE, 0, sizeof(mcam_shm), MCAM_SHM_NAME);
        LocalFree(sa.lpSecurityDescriptor);
    }
    if (!*map)
        return NULL;
    if (!(view = MapViewOfFile(*map, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(mcam_shm)))) {
        CloseHandle(*map);
        *map = NULL;
    }
    return (mcam_shm *)view;
}
