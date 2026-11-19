/* Windows install mode: a permanent all-users "Marksman Cam" camera plus a background service that wakes the
   camera only while some app is reading the camera. */
#include <windows.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>

#include "sink.h"

#define SVC_NAME L"marksman-cam"
#define CAM_NAME "Marksman Cam"

static int fail(const char *what, long err)
{
    fprintf(stderr, "marksman-cam: %s failed (0x%08lx)\n", what, (unsigned long)err);
    return 1;
}

static void stop_service(SC_HANDLE scm, const wchar_t *name)
{
    SC_HANDLE s = OpenServiceW(scm, name, SERVICE_STOP | SERVICE_QUERY_STATUS);
    SERVICE_STATUS st;
    if (!s)
        return;
    if (ControlService(s, SERVICE_CONTROL_STOP, &st))
        for (int i = 0; i < 100 && QueryServiceStatus(s, &st) && st.dwCurrentState != SERVICE_STOPPED; i++)
            Sleep(100);
    CloseServiceHandle(s);
}

/* DllRegisterServer / DllUnregisterServer, without depending on regsvr32. */
static long call_dll(const wchar_t *dll, const char *fn)
{
    HMODULE m = LoadLibraryW(dll);
    HRESULT(STDAPICALLTYPE * f)(void);
    long hr = HRESULT_FROM_WIN32(GetLastError());
    if (m && (f = (HRESULT(STDAPICALLTYPE *)(void))(void (*)(void))GetProcAddress(m, fn)))
        hr = f();
    if (m)
        FreeLibrary(m);
    return hr;
}

static void install_paths(wchar_t *dir, wchar_t *exe, wchar_t *dll)
{
    const wchar_t *pf = _wgetenv(L"ProgramFiles");
    swprintf(dir, MAX_PATH, L"%ls\\marksman-cam", pf ? pf : L"C:\\Program Files");
    swprintf(exe, MAX_PATH, L"%ls\\marksman-cam.exe", dir);
    swprintf(dll, MAX_PATH, L"%ls\\marksman-cam-vcam.dll", dir);
}

int winsvc_install(int argc, char **argv)
{
    wchar_t self[MAX_PATH], src_dll[MAX_PATH], dir[MAX_PATH], exe[MAX_PATH], dll[MAX_PATH], cmd[2048];
    SC_HANDLE scm, svc;
    long hr;
    int n;

    install_paths(dir, exe, dll);
    GetModuleFileNameW(NULL, self, MAX_PATH);
    wcscpy(src_dll, self);
    wcscpy(wcsrchr(src_dll, L'\\') + 1, L"marksman-cam-vcam.dll");
    if (!(scm = OpenSCManagerW(NULL, NULL, SC_MANAGER_ALL_ACCESS)))
        return fail("opening the service manager (run as administrator)", GetLastError());

    /* Upgrades in place: stop our service, and the Frame Server so it lets go of the old DLL. */
    stop_service(scm, SVC_NAME);
    stop_service(scm, L"FrameServerMonitor");
    stop_service(scm, L"FrameServer");
    CreateDirectoryW(dir, NULL);
    if (_wcsicmp(self, exe) && (!CopyFileW(self, exe, FALSE) || !CopyFileW(src_dll, dll, FALSE)))
        return fail("copying marksman-cam.exe and marksman-cam-vcam.dll to Program Files", GetLastError());
    if ((hr = call_dll(dll, "DllRegisterServer")) < 0)
        return fail("registering marksman-cam-vcam.dll", hr);
    vcam_uninstall(CAM_NAME); /* reinstall cleanly; harmless when absent */
    if ((hr = vcam_install(CAM_NAME)) < 0)
        return fail("creating the Marksman Cam camera", hr);

    /* The service runs "marksman-cam.exe --service" plus whatever options were given with --install. */
    n = swprintf(cmd, 2048, L"\"%ls\" --service", exe);
    for (int i = 1; i < argc && n > 0 && n < 2000; i++)
        if (strcmp(argv[i], "--install"))
            n += swprintf(cmd + n, 2048 - n, L" \"%hs\"", argv[i]);

    /* ponytail: LocalSystem, because WPD device access as LocalService is untested; try LocalService once verified */
    svc = CreateServiceW(scm, SVC_NAME, L"Marksman Cam", SERVICE_ALL_ACCESS, SERVICE_WIN32_OWN_PROCESS,
                         SERVICE_AUTO_START, SERVICE_ERROR_NORMAL, cmd, NULL, NULL, NULL, NULL, NULL);
    if (!svc && GetLastError() == ERROR_SERVICE_EXISTS && (svc = OpenServiceW(scm, SVC_NAME, SERVICE_ALL_ACCESS)))
        ChangeServiceConfigW(svc, SERVICE_NO_CHANGE, SERVICE_AUTO_START, SERVICE_NO_CHANGE, cmd, NULL, NULL, NULL,
                             NULL, NULL, NULL);
    if (!svc)
        return fail("creating the service", GetLastError());
    SERVICE_DESCRIPTIONW desc = {L"Camera live view as the \"Marksman Cam\" camera. Wakes the camera only while an app uses it."};
    ChangeServiceConfig2W(svc, SERVICE_CONFIG_DESCRIPTION, &desc);
    SC_ACTION restart[3] = {{SC_ACTION_RESTART, 5000}, {SC_ACTION_RESTART, 5000}, {SC_ACTION_RESTART, 30000}};
    SERVICE_FAILURE_ACTIONSW actions = {86400, NULL, NULL, 3, restart};
    ChangeServiceConfig2W(svc, SERVICE_CONFIG_FAILURE_ACTIONS, &actions);
    if (!StartServiceW(svc, 0, NULL) && GetLastError() != ERROR_SERVICE_ALREADY_RUNNING)
        return fail("starting the service", GetLastError());
    CloseServiceHandle(svc);
    CloseServiceHandle(scm);
    printf("installed to %ls\n\"Marksman Cam\" is now always available; the camera wakes when an app opens it.\n", dir);
    return 0;
}

int winsvc_uninstall(void)
{
    wchar_t dir[MAX_PATH], exe[MAX_PATH], dll[MAX_PATH];
    SC_HANDLE scm, svc;

    install_paths(dir, exe, dll);
    if (!(scm = OpenSCManagerW(NULL, NULL, SC_MANAGER_ALL_ACCESS)))
        return fail("opening the service manager (run as administrator)", GetLastError());
    stop_service(scm, SVC_NAME);
    if ((svc = OpenServiceW(scm, SVC_NAME, DELETE))) {
        DeleteService(svc);
        CloseServiceHandle(svc);
    }
    vcam_uninstall(CAM_NAME);
    stop_service(scm, L"FrameServerMonitor");
    stop_service(scm, L"FrameServer");
    call_dll(dll, "DllUnregisterServer");
    CloseServiceHandle(scm);
    printf("uninstalled; %ls can now be deleted\n", dir);
    return 0;
}

static SERVICE_STATUS_HANDLE status_handle;
static volatile sig_atomic_t *stop_flag;
static int (*service_body)(void);

static void report(DWORD state)
{
    SERVICE_STATUS st = {SERVICE_WIN32_OWN_PROCESS, state,
                         state == SERVICE_RUNNING ? SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN : 0, NO_ERROR, 0, 0,
                         state == SERVICE_STOP_PENDING ? 10000 : 0};
    SetServiceStatus(status_handle, &st);
}

static DWORD WINAPI control(DWORD code, DWORD type, LPVOID data, LPVOID ctx)
{
    (void)type, (void)data, (void)ctx;
    if (code == SERVICE_CONTROL_STOP || code == SERVICE_CONTROL_SHUTDOWN) {
        *stop_flag = 1;
        report(SERVICE_STOP_PENDING);
    }
    return NO_ERROR;
}

static void WINAPI service_main(DWORD argc, LPWSTR *argv)
{
    (void)argc, (void)argv;
    status_handle = RegisterServiceCtrlHandlerExW(SVC_NAME, control, NULL);
    report(SERVICE_RUNNING);
    service_body();
    report(SERVICE_STOPPED);
}

int winsvc_run(int (*body)(void), volatile sig_atomic_t *stop)
{
    SERVICE_TABLE_ENTRYW table[] = {{(LPWSTR)SVC_NAME, service_main}, {NULL, NULL}};
    const wchar_t *pd = _wgetenv(L"ProgramData");
    wchar_t log[MAX_PATH];
    if (pd) { /* ponytail: append-only log of connect/idle lines; rotate if it ever matters */
        swprintf(log, MAX_PATH, L"%ls\\marksman-cam.log", pd);
        if (_wfreopen(log, L"a", stderr))
            setvbuf(stderr, NULL, _IONBF, 0);
    }
    service_body = body;
    stop_flag = stop;
    return StartServiceCtrlDispatcherW(table) ? 0 : fail("--service is started by the service manager; use --install", GetLastError());
}
