/*
 * uxplay-panel.exe — strip/status-panel windows in a separate process.
 * Simple top-level WS_POPUP windows. No snap tricks, no hidden host.
 */

#include <windows.h>
#include <stdio.h>
#include <string.h>

#define MAXSLOTS 5
#define SHM_NAME  L"UxPlayPanelShm"
#define READY_NAME L"UxPlayPanelReady"
#define ROUTER_ALIVE_NAME L"UxPlayRouterAlive"

typedef struct {
    int nslots;
    int alive;
    struct {
        wchar_t cls[128];
        wchar_t title[96];
        int x, y, w, h;
        HWND hwnd;
        int alive;
    } p[MAXSLOTS];
} panel_shm_t;

static panel_shm_t *g_shm = NULL;

static LRESULT CALLBACK panel_wndproc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
    (void) lp;
    switch (m) {
    case WM_NCHITTEST: return HTNOWHERE;
    }
    return DefWindowProcW(h, m, wp, lp);
}

static void sync_windows(void) {
    if (!g_shm) return;
    int n = g_shm->nslots;
    if (n > MAXSLOTS) n = MAXSLOTS;

    for (int i = 0; i < n; i++) {
        HWND sw = g_shm->p[i].hwnd;

        if (g_shm->p[i].alive && !g_shm->p[i].cls[0]) {
            if (sw && IsWindow(sw)) DestroyWindow(sw);
            g_shm->p[i].hwnd = NULL;
            g_shm->p[i].alive = 0;
            continue;
        }

        if (!g_shm->p[i].alive && g_shm->p[i].cls[0]) {
            WNDCLASSW wc; memset(&wc, 0, sizeof(wc));
            wc.lpfnWndProc  = panel_wndproc;
            wc.hInstance     = GetModuleHandleW(NULL);
            wc.hbrBackground = (HBRUSH) GetStockObject(BLACK_BRUSH);
            wc.lpszClassName = g_shm->p[i].cls;
            RegisterClassW(&wc);

            sw = CreateWindowExW(0,
                                 g_shm->p[i].cls, g_shm->p[i].title,
                                 WS_POPUP | WS_VISIBLE,
                                 g_shm->p[i].x, g_shm->p[i].y,
                                 g_shm->p[i].w, g_shm->p[i].h,
                                 NULL, NULL, GetModuleHandleW(NULL), NULL);
            HICON hIcon = LoadIconW(GetModuleHandleW(NULL), MAKEINTRESOURCEW(1));
            if (hIcon) {
                SendMessageW(sw, WM_SETICON, ICON_BIG, (LPARAM) hIcon);
                SendMessageW(sw, WM_SETICON, ICON_SMALL, (LPARAM) hIcon);
            }
            g_shm->p[i].hwnd = sw;
            g_shm->p[i].alive = 1;
            continue;
        }

        if (sw && IsWindow(sw)) {
            wchar_t cur[128]; GetWindowTextW(sw, cur, 128);
            if (wcscmp(cur, g_shm->p[i].title) != 0)
                SetWindowTextW(sw, g_shm->p[i].title);
            RECT rc; GetWindowRect(sw, &rc);
            if (rc.left != g_shm->p[i].x || rc.top != g_shm->p[i].y ||
                rc.right - rc.left != g_shm->p[i].w || rc.bottom - rc.top != g_shm->p[i].h)
                SetWindowPos(sw, NULL, g_shm->p[i].x, g_shm->p[i].y,
                             g_shm->p[i].w, g_shm->p[i].h,
                             SWP_NOZORDER | SWP_NOACTIVATE | SWP_SHOWWINDOW);
        }
    }
}

static int router_alive(void) {
    HANDLE h = OpenEventW(SYNCHRONIZE, FALSE, ROUTER_ALIVE_NAME);
    if (!h) return 0;
    DWORD r = WaitForSingleObject(h, 0);
    CloseHandle(h);
    return (r == WAIT_OBJECT_0);
}

int main(int argc, char *argv[]) {
    (void) argc; (void) argv;

    HANDLE hMap = OpenFileMappingW(FILE_MAP_READ | FILE_MAP_WRITE, FALSE, SHM_NAME);
    if (!hMap) return 1;
    g_shm = (panel_shm_t *) MapViewOfFile(hMap, FILE_MAP_READ | FILE_MAP_WRITE,
                                           0, 0, sizeof(panel_shm_t));
    if (!g_shm) { CloseHandle(hMap); return 1; }

    HANDLE hReady = OpenEventW(EVENT_MODIFY_STATE, FALSE, READY_NAME);
    if (hReady) { SetEvent(hReady); CloseHandle(hReady); }

    for (;;) {
        if (!router_alive()) break;
        sync_windows();
        MSG msg;
        while (PeekMessage(&msg, NULL, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) goto done;
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }
        Sleep(30);
    }
done:
    if (g_shm) {
        for (int i = 0; i < MAXSLOTS; i++) {
            if (g_shm->p[i].alive && g_shm->p[i].hwnd && IsWindow(g_shm->p[i].hwnd))
                DestroyWindow(g_shm->p[i].hwnd);
            g_shm->p[i].hwnd = NULL;
            g_shm->p[i].alive = 0;
        }
    }
    UnmapViewOfFile(g_shm);
    CloseHandle(hMap);
    return 0;
}
