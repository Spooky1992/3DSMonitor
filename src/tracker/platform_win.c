/* Implémentation Windows : hook d'événements (aucun polling actif du premier plan),
 * notifications de session (verrouillage) et d'alimentation (veille). */
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <tlhelp32.h>
#include <wtsapi32.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../common/cm_util.h"
#include "platform.h"

static HWND g_wnd;
static HWINEVENTHOOK g_hook;
static HANDLE g_mutex;
static volatile int g_changed, g_quit, g_locked;
static void (*g_event_hook)(int ev);

static void to_utf8(const wchar_t *w, char *out, size_t n)
{
    if (!WideCharToMultiByte(CP_UTF8, 0, w, -1, out, (int)n, NULL, NULL)) {
        /* tampon trop petit : on tronque proprement */
        int len = WideCharToMultiByte(CP_UTF8, 0, w, -1, NULL, 0, NULL, NULL);
        char *tmp = (char *)malloc(len > 0 ? (size_t)len : 1);
        if (tmp && len > 0 && WideCharToMultiByte(CP_UTF8, 0, w, -1, tmp, len, NULL, NULL))
            cm_strlcpy(out, tmp, n);
        else if (n)
            out[0] = '\0';
        free(tmp);
    }
}

static void CALLBACK on_win_event(HWINEVENTHOOK h, DWORD ev, HWND hwnd, LONG obj, LONG child,
                                  DWORD thread, DWORD ms)
{
    (void)h; (void)ev; (void)hwnd; (void)obj; (void)child; (void)thread; (void)ms;
    g_changed = 1;
}

static LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_QUERYENDSESSION:
        return TRUE;
    case WM_ENDSESSION:
        if (wp) {
            g_quit = 1;
            if (g_event_hook) g_event_hook(PLAT_EV_QUIT);
        }
        return 0;
    case WM_POWERBROADCAST:
        if (wp == PBT_APMSUSPEND) {
            if (g_event_hook) g_event_hook(PLAT_EV_SUSPEND);
        } else if (wp == PBT_APMRESUMEAUTOMATIC || wp == PBT_APMRESUMESUSPEND) {
            g_changed = 1;
        }
        return TRUE;
    case WM_WTSSESSION_CHANGE:
        if (wp == WTS_SESSION_LOCK) g_locked = 1;
        else if (wp == WTS_SESSION_UNLOCK) g_locked = 0;
        g_changed = 1;
        return 0;
    case WM_CLOSE:
        g_quit = 1;
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

void plat_set_event_hook(void (*fn)(int ev)) { g_event_hook = fn; }

int plat_init(void)
{
    WNDCLASSW wc;
    memset(&wc, 0, sizeof wc);
    wc.lpfnWndProc = wnd_proc;
    wc.hInstance = GetModuleHandleW(NULL);
    wc.lpszClassName = L"ComputerMonitorTracker";
    if (!RegisterClassW(&wc)) return -1;
    /* Fenêtre de premier niveau invisible : nécessaire pour recevoir WM_ENDSESSION. */
    g_wnd = CreateWindowExW(WS_EX_TOOLWINDOW, wc.lpszClassName, L"ComputerMonitor", WS_POPUP,
                            0, 0, 0, 0, NULL, NULL, wc.hInstance, NULL);
    if (!g_wnd) return -1;
    WTSRegisterSessionNotification(g_wnd, NOTIFY_FOR_THIS_SESSION);
    g_hook = SetWinEventHook(EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND, NULL, on_win_event, 0, 0,
                             WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
    return 0;
}

void plat_shutdown(void)
{
    if (g_hook) UnhookWinEvent(g_hook);
    if (g_wnd) {
        WTSUnRegisterSessionNotification(g_wnd);
        DestroyWindow(g_wnd);
    }
    if (g_mutex) CloseHandle(g_mutex);
}

int plat_single_instance(const char *db_path)
{
    (void)db_path;
    g_mutex = CreateMutexW(NULL, TRUE, L"Local\\ComputerMonitorTracker");
    if (!g_mutex || GetLastError() == ERROR_ALREADY_EXISTS) return -1;
    return 0;
}

int plat_wait(int timeout_ms)
{
    MSG m;
    if (!g_changed && !g_quit)
        MsgWaitForMultipleObjectsEx(0, NULL, (DWORD)timeout_ms, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
    while (PeekMessageW(&m, NULL, 0, 0, PM_REMOVE)) {
        if (m.message == WM_QUIT) g_quit = 1;
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
    if (g_quit) return PLAT_EV_QUIT;
    if (g_changed) {
        g_changed = 0;
        return PLAT_EV_CHANGE;
    }
    return PLAT_EV_TIMEOUT;
}

unsigned plat_idle_seconds(void)
{
    LASTINPUTINFO li;
    li.cbSize = sizeof li;
    if (!GetLastInputInfo(&li)) return 0;
    return (unsigned)((GetTickCount() - li.dwTime) / 1000u);
}

int plat_is_locked(void)
{
    BOOL saver = FALSE;
    if (g_locked) return 1;
    if (SystemParametersInfoW(SPI_GETSCREENSAVERRUNNING, 0, &saver, 0) && saver) return 1;
    return 0;
}

/* Nom d'exécutable d'un pid via un instantané (repli si OpenProcess est refusé). */
static int exe_name_from_snapshot(DWORD pid, char *out, size_t n)
{
    PROCESSENTRY32W pe;
    int found = 0;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return -1;
    pe.dwSize = sizeof pe;
    for (BOOL ok = Process32FirstW(snap, &pe); ok; ok = Process32NextW(snap, &pe)) {
        if (pe.th32ProcessID == pid) {
            to_utf8(pe.szExeFile, out, n);
            found = 1;
            break;
        }
    }
    CloseHandle(snap);
    return found ? 0 : -1;
}

static int process_path(DWORD pid, char *out, size_t n)
{
    wchar_t w[MAX_PATH * 2];
    DWORD len = (DWORD)(sizeof w / sizeof w[0]);
    int ok = 0;
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h) return -1;
    if (QueryFullProcessImageNameW(h, 0, w, &len)) {
        to_utf8(w, out, n);
        ok = 1;
    }
    CloseHandle(h);
    return ok ? 0 : -1;
}

typedef struct { DWORD host_pid; DWORD found_pid; } uwp_search;

static BOOL CALLBACK find_uwp_child(HWND child, LPARAM lp)
{
    uwp_search *s = (uwp_search *)lp;
    DWORD pid = 0;
    GetWindowThreadProcessId(child, &pid);
    if (pid && pid != s->host_pid) {
        s->found_pid = pid;
        return FALSE;
    }
    return TRUE;
}

int plat_get_foreground(plat_foreground *out)
{
    static DWORD cached_pid;
    static char cached_path[512], cached_app[128];
    wchar_t wt[512];
    DWORD pid = 0;
    HWND h = GetForegroundWindow();

    memset(out, 0, sizeof *out);
    if (!h) return -1;
    GetWindowThreadProcessId(h, &pid);
    if (!pid) return -1;
    if (GetWindowTextW(h, wt, (int)(sizeof wt / sizeof wt[0])) > 0) to_utf8(wt, out->title, sizeof out->title);

    for (int pass = 0; pass < 2; pass++) {
        if (pid != cached_pid) {
            cached_path[0] = cached_app[0] = '\0';
            if (process_path(pid, cached_path, sizeof cached_path) == 0)
                cm_strlcpy(cached_app, cm_basename(cached_path), sizeof cached_app);
            else if (exe_name_from_snapshot(pid, cached_app, sizeof cached_app) != 0)
                snprintf(cached_app, sizeof cached_app, "pid-%lu", (unsigned long)pid);
            cm_str_tolower(cached_app);
            cached_pid = pid;
        }
        /* Applications UWP : la fenêtre appartient à ApplicationFrameHost, le vrai
         * processus est celui d'une fenêtre enfant. */
        if (pass == 0 && strcmp(cached_app, "applicationframehost.exe") == 0) {
            uwp_search s = {pid, 0};
            EnumChildWindows(h, find_uwp_child, (LPARAM)&s);
            if (s.found_pid) {
                pid = s.found_pid;
                continue;
            }
        }
        break;
    }
    out->pid = pid;
    cm_strlcpy(out->app, cached_app, sizeof out->app);
    cm_strlcpy(out->path, cached_path, sizeof out->path);
    return 0;
}

void plat_app_display_name(const char *path, char *out, size_t n)
{
    wchar_t wpath[MAX_PATH * 2], key[64];
    DWORD dummy = 0, size;
    void *data;
    struct { WORD lang, cp; } *tr;
    UINT len = 0;
    wchar_t *desc = NULL;

    out[0] = '\0';
    if (!path || !*path) return;
    if (!MultiByteToWideChar(CP_UTF8, 0, path, -1, wpath, (int)(sizeof wpath / sizeof wpath[0]))) return;
    size = GetFileVersionInfoSizeW(wpath, &dummy);
    if (!size) return;
    data = malloc(size);
    if (!data) return;
    if (GetFileVersionInfoW(wpath, 0, size, data) &&
        VerQueryValueW(data, L"\\VarFileInfo\\Translation", (void **)&tr, &len) && len >= 4) {
        _snwprintf(key, 64, L"\\StringFileInfo\\%04x%04x\\FileDescription", tr->lang, tr->cp);
        key[63] = 0;
        if (VerQueryValueW(data, key, (void **)&desc, &len) && len > 1 && desc) to_utf8(desc, out, n);
    }
    free(data);
}

int plat_list_processes(plat_process *out, int max)
{
    PROCESSENTRY32W pe;
    DWORD my_session = 0, sess;
    int count = 0;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;
    ProcessIdToSessionId(GetCurrentProcessId(), &my_session);
    pe.dwSize = sizeof pe;
    for (BOOL ok = Process32FirstW(snap, &pe); ok && count < max; ok = Process32NextW(snap, &pe)) {
        /* seulement les processus de la session de l'utilisateur (pas les services) */
        if (!ProcessIdToSessionId(pe.th32ProcessID, &sess) || sess != my_session) continue;
        out[count].pid = pe.th32ProcessID;
        to_utf8(pe.szExeFile, out[count].app, sizeof out[count].app);
        cm_str_tolower(out[count].app);
        count++;
    }
    CloseHandle(snap);
    return count;
}
