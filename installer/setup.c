/* Double-click installer. Embeds marksman-cam.exe + marksman-cam-vcam.dll, asks which cameras to set up, runs
   "marksman-cam.exe --install --camera X" per choice (each its own virtual camera and service) with no console,
   and adds a Settings > Apps entry: Modify re-runs this picker, Uninstall runs "marksman-cam-setup.exe /uninstall". */
#define _WIN32_WINNT 0x0A00
#include <windows.h>
#include <commctrl.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>

#define TITLE L"Marksman Cam Setup"
#define ARP L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\marksman-cam"

static wchar_t dir[MAX_PATH], exe[MAX_PATH], dll[MAX_PATH], setup[MAX_PATH];

static int extract(int id, const wchar_t *path)
{
    HRSRC r = FindResourceW(NULL, MAKEINTRESOURCEW(id), (LPCWSTR)RT_RCDATA);
    DWORD n = r ? SizeofResource(NULL, r) : 0, w = 0;
    HANDLE f;
    if (!n || (f = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL)) == INVALID_HANDLE_VALUE)
        return 0;
    WriteFile(f, LockResource(LoadResource(NULL, r)), n, &w, NULL);
    CloseHandle(f);
    return w == n;
}

/* Runs cmd without a console window. Returns its exit code; its stdout+stderr go to out. */
static DWORD run(wchar_t *cmd, char *out, DWORD cap)
{
    SECURITY_ATTRIBUTES sa = {sizeof sa, NULL, TRUE};
    STARTUPINFOW si = {.cb = sizeof si, .dwFlags = STARTF_USESTDHANDLES};
    PROCESS_INFORMATION pi;
    HANDLE rd, wr;
    DWORD code = 1, n, len = 0;
    char buf[512];

    out[0] = 0;
    if (!CreatePipe(&rd, &wr, &sa, 0))
        return 1;
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
    si.hStdOutput = si.hStdError = wr;
    if (!CreateProcessW(NULL, cmd, NULL, NULL, TRUE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi))
        snprintf(out, cap, "could not start marksman-cam.exe (error %lu)", GetLastError());
    CloseHandle(wr);
    if (out[0]) {
        CloseHandle(rd);
        return 1;
    }
    while (ReadFile(rd, buf, sizeof buf, &n, NULL) && n) /* keep draining so the child never blocks */
        for (DWORD i = 0; i < n && len + 1 < cap; i++)
            out[len++] = buf[i];
    out[len] = 0;
    WaitForSingleObject(pi.hProcess, INFINITE);
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    CloseHandle(rd);
    return code;
}

static void reg_str(HKEY k, const wchar_t *name, const wchar_t *val)
{
    RegSetValueExW(k, name, 0, REG_SZ, (const BYTE *)val, (DWORD)(wcslen(val) + 1) * sizeof(wchar_t));
}

static void reg_dword(HKEY k, const wchar_t *name, DWORD val)
{
    RegSetValueExW(k, name, 0, REG_DWORD, (const BYTE *)&val, sizeof val);
}

static void error(const char *msg)
{
    MessageBoxA(NULL, msg, "Marksman Cam Setup", MB_ICONERROR);
}

enum { BOTH = 100, CANON, SONY };

/* Returns BOTH, CANON or SONY, or 0 if cancelled. */
static int pick_cameras(void)
{
    const TASKDIALOG_BUTTON radios[] = {
        {BOTH, L"Canon and Sony\nTwo cameras in your apps: \"Marksman Cam (Canon)\" and \"Marksman Cam (Sony)\""},
        {CANON, L"Canon only\n\"Marksman Cam (Canon)\""},
        {SONY, L"Sony only\n\"Marksman Cam (Sony)\""},
    };
    TASKDIALOGCONFIG c = {.cbSize = sizeof c,
                          .dwFlags = TDF_ALLOW_DIALOG_CANCELLATION,
                          .dwCommonButtons = TDCBF_OK_BUTTON | TDCBF_CANCEL_BUTTON,
                          .pszWindowTitle = TITLE,
                          .pszMainInstruction = L"Which cameras do you want to use?",
                          .pszContent = L"Each camera gets its own entry in Zoom, Teams, OBS and other apps, and its "
                                        L"own background service that wakes it only while an app uses it.\n\n"
                                        L"Run setup again (or Modify in Settings > Apps) to change this.",
                          .cRadioButtons = 3,
                          .pRadioButtons = radios,
                          .nDefaultRadioButton = BOTH};
    int button = 0, radio = 0;
    if (FAILED(TaskDialogIndirect(&c, &button, &radio, NULL)) || button != IDOK)
        return 0;
    return radio;
}

static int install(void)
{
    wchar_t tmp[MAX_PATH], texe[MAX_PATH], tdll[MAX_PATH], self[MAX_PATH], cmd[2 * MAX_PATH];
    char out[4096];
    HKEY k;
    int choice = pick_cameras();
    if (!choice)
        return 1;
    /* Unwanted slots go first (harmless when absent), including a plain any-make one from a command-line install. */
    const char *steps[4], *what;
    int nsteps = 0;
    steps[nsteps++] = "--uninstall --camera any";
    if (choice == SONY)
        steps[nsteps++] = "--uninstall --camera canon";
    if (choice == CANON)
        steps[nsteps++] = "--uninstall --camera sony";
    if (choice != SONY)
        steps[nsteps++] = "--install --camera canon";
    if (choice != CANON)
        steps[nsteps++] = "--install --camera sony";

    /* --install copies the files from wherever it runs into Program Files, so stage them in %TEMP%. */
    GetTempPathW(MAX_PATH, tmp);
    wcscat(tmp, L"marksman-cam-setup");
    CreateDirectoryW(tmp, NULL);
    swprintf(texe, MAX_PATH, L"%ls\\marksman-cam.exe", tmp);
    swprintf(tdll, MAX_PATH, L"%ls\\marksman-cam-vcam.dll", tmp);
    if (!extract(1, texe) || !extract(2, tdll)) {
        error("Could not unpack the program files into %TEMP%.");
        return 1;
    }
    DWORD code = 0;
    for (int i = 0; i < nsteps && !code; i++) {
        swprintf(cmd, 2 * MAX_PATH, L"\"%ls\" %hs", texe, steps[i]);
        code = run(cmd, out, sizeof out);
    }
    DeleteFileW(texe);
    DeleteFileW(tdll);
    RemoveDirectoryW(tmp);
    if (code) {
        error(out[0] ? out : "marksman-cam.exe --install failed.");
        return 1;
    }

    /* Keep a copy of this setup in the install folder: it is the uninstaller. */
    GetModuleFileNameW(NULL, self, MAX_PATH);
    if (_wcsicmp(self, setup))
        CopyFileW(self, setup, FALSE);
    if (!RegCreateKeyExW(HKEY_LOCAL_MACHINE, ARP, 0, NULL, 0, KEY_WRITE, NULL, &k, NULL)) {
        swprintf(cmd, 2 * MAX_PATH, L"\"%ls\" /uninstall", setup);
        reg_str(k, L"DisplayName", L"Marksman Cam");
        reg_str(k, L"Publisher", L"marksman-cam");
        reg_str(k, L"InstallLocation", dir);
        reg_str(k, L"DisplayIcon", exe);
        reg_str(k, L"UninstallString", cmd);
        swprintf(cmd, 2 * MAX_PATH, L"\"%ls\"", setup);
        reg_str(k, L"ModifyPath", cmd); /* Modify re-runs the camera picker */
        reg_dword(k, L"NoModify", 0);
        reg_dword(k, L"NoRepair", 1);
        RegCloseKey(k);
    }
    what = choice == BOTH ? "\"Marksman Cam (Canon)\" and \"Marksman Cam (Sony)\" are"
           : choice == CANON ? "\"Marksman Cam (Canon)\" is"
                             : "\"Marksman Cam (Sony)\" is";
    snprintf(out, sizeof out,
             "%s installed.\n\nCanon: use a shooting mode (Movie mode where available).\nSony: MENU > Setup > USB "
             "Connection: PC Remote, with Ctrl w/ Smartphone off.\nBoth: turn auto power-off off.\n\nThen pick the "
             "camera in any app.\nTo change cameras or remove: Settings > Apps > Installed apps > Marksman Cam.",
             what);
    MessageBoxA(NULL, out, "Marksman Cam Setup", MB_OK | MB_ICONINFORMATION);
    return 0;
}

static void remove_now_or_at_reboot(const wchar_t *path, int is_dir)
{
    if (!(is_dir ? RemoveDirectoryW(path) : DeleteFileW(path)) && GetLastError() != ERROR_FILE_NOT_FOUND)
        MoveFileExW(path, NULL, MOVEFILE_DELAY_UNTIL_REBOOT);
}

static int uninstall(void)
{
    wchar_t cmd[2 * MAX_PATH];
    char out[4096];

    if (MessageBoxW(NULL, L"Remove Marksman Cam?", TITLE, MB_OKCANCEL | MB_ICONQUESTION) != IDOK)
        return 1;
    swprintf(cmd, 2 * MAX_PATH, L"\"%ls\" --uninstall", exe);
    if (GetFileAttributesW(exe) != INVALID_FILE_ATTRIBUTES && run(cmd, out, sizeof out)) {
        error(out[0] ? out : "marksman-cam.exe --uninstall failed.");
        return 1;
    }
    RegDeleteKeyW(HKEY_LOCAL_MACHINE, ARP);
    /* ponytail: this setup is running from the folder, so it (and the folder) go at next reboot;
       copy-to-%TEMP%-and-relaunch if a leftover file until reboot ever bothers anyone */
    remove_now_or_at_reboot(exe, 0);
    remove_now_or_at_reboot(dll, 0);
    remove_now_or_at_reboot(setup, 0);
    remove_now_or_at_reboot(dir, 1);
    MessageBoxW(NULL, L"Marksman Cam has been removed.", TITLE, MB_OK | MB_ICONINFORMATION);
    return 0;
}

int WINAPI WinMain(HINSTANCE inst, HINSTANCE prev, LPSTR args, int show)
{
    (void)inst, (void)prev, (void)show;
    /* Same folder marksman-cam.exe --install uses (src/winsvc.c install_paths). */
    const wchar_t *pf = _wgetenv(L"ProgramFiles");
    swprintf(dir, MAX_PATH, L"%ls\\marksman-cam", pf ? pf : L"C:\\Program Files");
    swprintf(exe, MAX_PATH, L"%ls\\marksman-cam.exe", dir);
    swprintf(dll, MAX_PATH, L"%ls\\marksman-cam-vcam.dll", dir);
    swprintf(setup, MAX_PATH, L"%ls\\marksman-cam-setup.exe", dir);
    return strstr(args, "/uninstall") ? uninstall() : install();
}
