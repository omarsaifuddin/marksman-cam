/* Frame hand-off between marksman-cam.exe and the media source DLL running inside the Frame Server
   service (LocalService, session 0). Whichever side starts first creates the section: normal users may
   lack "create global objects", LocalService always has it. */
#pragma once
#include <windows.h>
#include <sddl.h>
#include <stdio.h>

/* One virtual camera per slot, each with its own CLSID, frame section and service, so several physical
   cameras can be installed side by side. Slot 0 takes any supported camera; the others one make. */
#define MCAM_SLOTS 3
static inline int mcam_slot_for(unsigned vid) { return vid == 0x04A9 ? 1 : vid == 0x054C ? 2 : 0; }
static const char *const mcam_name[MCAM_SLOTS] = {"Marksman Cam", "Marksman Cam (Canon)", "Marksman Cam (Sony)"};
static const wchar_t *const mcam_service[MCAM_SLOTS] = {L"marksman-cam", L"marksman-cam-canon", L"marksman-cam-sony"};
static const wchar_t *const mcam_clsid_str[MCAM_SLOTS] = {L"{815866E8-7D99-481E-A357-6B004C6761CC}",
                                                          L"{070E2FF7-A837-4A7C-971B-D34783A77E35}",
                                                          L"{19D3AC92-2C41-441E-BDC9-AB7A0842AF96}"};
static const GUID mcam_clsid[MCAM_SLOTS] = {
    {0x815866E8, 0x7D99, 0x481E, {0xA3, 0x57, 0x6B, 0x00, 0x4C, 0x67, 0x61, 0xCC}},
    {0x070E2FF7, 0xA837, 0x4A7C, {0x97, 0x1B, 0xD3, 0x47, 0x83, 0xA7, 0x7E, 0x35}},
    {0x19D3AC92, 0x2C41, 0x441E, {0xBD, 0xC9, 0xAB, 0x7A, 0x08, 0x42, 0xAF, 0x96}},
};
#define MCAM_SHM_NAME L"Global\\MarksmanCamFrame4-%d" /* + slot; bump the number whenever the layout changes */
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

static inline mcam_shm *mcam_shm_open(HANDLE *map, int slot)
{
    SECURITY_ATTRIBUTES sa = {sizeof sa, NULL, FALSE};
    wchar_t name[64];
    void *view;
    _snwprintf(name, 64, MCAM_SHM_NAME, slot);
    *map = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, name);
    if (!*map && ConvertStringSecurityDescriptorToSecurityDescriptorW(MCAM_SHM_SDDL, SDDL_REVISION_1,
                                                                       &sa.lpSecurityDescriptor, NULL)) {
        *map = CreateFileMappingW(INVALID_HANDLE_VALUE, &sa, PAGE_READWRITE, 0, sizeof(mcam_shm), name);
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
