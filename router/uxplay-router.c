/* uxplay-router: advertise ONE AirPlay service, route clients by source IP
 * to hidden loopback-bound uxplay.exe backend instances.
 *
 * Usage: uxplay-router.exe [N] [name]   N=instances (default 3, max 8)
 */
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <wctype.h>
#include <shobjidl.h>
#include <objbase.h>

/* GUIDs not exported by mingw uuid lib — define locally */
static const CLSID k_CLSID_VirtualDesktopManager =
    {0xaa509086, 0x5ca9, 0x4c25, {0x8f, 0x95, 0x58, 0x9d, 0x3c, 0x07, 0xb4, 0x8a}};
static const IID k_IID_IVirtualDesktopManager =
    {0x140577ee, 0x2b59, 0x494d, {0xb5, 0xa6, 0xa4, 0xe5, 0xa3, 0xc3, 0x4a, 0x24}};
#include <iphlpapi.h>
#include <stdio.h>
#include <io.h>
#include <fcntl.h>
#include <shellapi.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "dnssd.h"

#define MAXSLOTS   8
#define PUB_TCP_N  3
#define PUB_UDP_N  3
#define MAXFLOWS   64
#define IDLE_SLOT_FREE 300
#define IDLE_FLOW_FREE 120

static int g_base = 0;   /* -base N: port shift for multi-instance (N*100 backends, N*20 public) */
#define BE_BASE()   (20000 + g_base * 100)
#define PUBS()      (g_base * 20)
static const unsigned short pub_tcp[PUB_TCP_N] = {7000, 7001, 7100};
static const int be_tcp_off[PUB_TCP_N] = {0, 1, 2}; /* rtsp misc mirror */
static const unsigned short pub_udp[PUB_UDP_N] = {6000, 6001, 7011};
static const int be_udp_off[PUB_UDP_N] = {3, 4, 5}; /* timing audio mirroraudio */

typedef struct {
    int used;
    char ip[64];
    volatile LONG conns;
    time_t last_active;
    int wrap_placed;
    int styled;
} slot_t;

static slot_t g_slot[MAXSLOTS];
static int g_nslots = 5;
static int g_with_audio = 0;
static char g_geom[64] = "";
static int g_fullscreen = 0;
static CRITICAL_SECTION g_lock;

typedef struct {
    int used;
    char ip[64];
    SOCKADDR_STORAGE cli;
    int clilen;
    SOCKET s;
    time_t last_active;
} uflow_t;

static uflow_t g_flow[PUB_UDP_N][MAXFLOWS];

static HANDLE g_child[MAXSLOTS];
static DWORD g_child_pid[MAXSLOTS];

/* ---- VNC reverse control (TrollVNC on devices) ---- */
typedef struct { int kind; int mask; int x, y, dir; } pev_t;
#define PEV_QN 32

typedef struct {
    SOCKET s;
    int fw, fh;          /* device framebuffer size */
    int connected;
    int tried;
    CRITICAL_SECTION cs;
    DWORD last_send;
    int btnmask;
    WNDPROC orig_proc;
    pev_t q[PEV_QN];
    volatile LONG qh, qt;
    HANDLE wake;
} vnc_state_t;
static vnc_state_t g_vnc[MAXSLOTS];
static int g_vnc_port = 5901;
static const char *g_vnc_rot = NULL;
static int g_map_cw = 1, g_map_ch = 1;

static int rx(SOCKET s, char *b, int n) {
    int got = 0;
    while (got < n) {
        int r = recv(s, b + got, n - got, 0);
        if (r <= 0) return -1;
        got += r;
    }
    return got;
}

static SOCKET vnc_connect(const char *ip, int *fw_out, int *fh_out)
{
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    SOCKADDR_IN a; memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = inet_addr(ip);
    a.sin_port = htons((unsigned short)g_vnc_port);
    if (s == INVALID_SOCKET) return INVALID_SOCKET;
    if (connect(s, (SOCKADDR *)&a, sizeof(a)) != 0) {
        fprintf(stderr, "router: vnc %s:%d connect failed (%d)\n", ip, g_vnc_port, WSAGetLastError());
        closesocket(s); return INVALID_SOCKET;
    }
    int one = 1;
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (char *)&one, sizeof(one));
    DWORD to = 3000;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (char *)&to, sizeof(to));
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, (char *)&to, sizeof(to));
    char buf[64];
    int vmaj = 3, vmin = 8;
    /* version */
    if (rx(s, buf, 12) != 12) { fprintf(stderr, "router: vnc %s: no version\n", ip); goto fail; }
    sscanf(buf, "RFB %d.%d", &vmaj, &vmin);
    memcpy(buf, "RFB 003.008\n", 12);
    if (send(s, buf, 12, 0) != 12) goto fail;
    /* security */
    unsigned char types[16];
    int ntypes = 0;
    if (vmaj == 3 && vmin > 6) {
        if (rx(s, buf, 1) != 1) { fprintf(stderr, "router: vnc %s: no sec count\n", ip); goto fail; }
        ntypes = (unsigned char)buf[0];
        if (ntypes <= 0 || ntypes > 15) { fprintf(stderr, "router: vnc %s: bad sec count %d\n", ip, ntypes); goto fail; }
        if (rx(s, (char *)types, ntypes) != ntypes) goto fail;
    } else {
        unsigned int st;
        if (rx(s, (char *)&st, 4) != 4) { fprintf(stderr, "router: vnc %s: no 3.3 sectype\n", ip); goto fail; }
        st = ((st & 0xff) << 24) | ((st & 0xff00) << 8) | ((st >> 8) & 0xff00) | (st >> 24);
        if (st == 0 || st > 2) { fprintf(stderr, "router: vnc %s: server refused (%u)\n", ip, st); goto fail; }
        types[0] = (unsigned char)st;
        ntypes = 1;
    }
    {
        int sel = -1;
        for (int k = 0; k < ntypes; k++) if (types[k] == 1) sel = 1;
        if (sel < 0) {
            fprintf(stderr, "router: vnc %s: no None auth offered (%d types:", ip, ntypes);
            for (int k = 0; k < ntypes; k++) fprintf(stderr, " %u", types[k]);
            fprintf(stderr, ")\n");
            goto fail;
        }
        unsigned char s1 = (unsigned char)sel;
        if (send(s, (char *)&s1, 1, 0) != 1) goto fail;
    }
    if (vmaj == 3 && vmin > 7) {
        unsigned int res;
        if (rx(s, (char *)&res, 4) != 4) { fprintf(stderr, "router: vnc %s: no sec result\n", ip); goto fail; }
        if (res != 0) { fprintf(stderr, "router: vnc %s: sec failed %u\n", ip, res); goto fail; }
    }
    printf("router: vnc %s: handshake ok (server rfb %d.%d), sending init\n", ip, vmaj, vmin);
    /* ClientInit: shared */
    unsigned char ci = 1;
    if (send(s, (char *)&ci, 1, 0) != 1) goto fail;
    /* ServerInit */
    unsigned char si[24];
    if (rx(s, (char *)si, 24) != 24) { fprintf(stderr, "router: vnc %s: no serverinit\n", ip); goto fail; }
    int fw = (si[0] << 8) | si[1];
    int fh = (si[2] << 8) | si[3];
    unsigned int nl = ((unsigned)si[20] << 24) | ((unsigned)si[21] << 16) | ((unsigned)si[22] << 8) | si[23];
    if (nl > 0 && nl < 256 && rx(s, buf, (int)nl) != (int)nl) goto fail;
    if (fw_out) *fw_out = fw;
    if (fh_out) *fh_out = fh;
    printf("router: vnc connected %s framebuffer %dx%d\n", ip, fw, fh);
    return s;
fail:
    closesocket(s);
    return INVALID_SOCKET;
}

static void vnc_ensure(int idx)
{
    vnc_state_t *v = &g_vnc[idx];
    if (v->connected) return;
    if (v->tried && GetTickCount() - v->tried < 3000) return;
    v->tried = GetTickCount();
    if (!g_slot[idx].used || !g_slot[idx].ip[0]) return;
    fprintf(stderr, "router: vnc attempting %s:%d\n", g_slot[idx].ip, g_vnc_port);
    EnterCriticalSection(&v->cs);
    int fw = 0, fh = 0;
    SOCKET s = vnc_connect(g_slot[idx].ip, &fw, &fh);
    if (s != INVALID_SOCKET) {
        v->s = s;
        v->fw = fw;
        v->fh = fh;
        v->connected = 1;
    }
    LeaveCriticalSection(&v->cs);
}

static void vnc_pointer(int idx, int mask, int wx, int wy)
{
    vnc_state_t *v = &g_vnc[idx];
    vnc_ensure(idx);
    if (!v->connected) return;
    int rot_ccw = g_vnc_rot && !_stricmp(g_vnc_rot, "ccw");
    float xl = (float)wx / (g_map_cw > 0 ? g_map_cw : 1);
    float yl = (float)wy / (g_map_ch > 0 ? g_map_ch : 1);
    if (xl < 0) xl = 0;
    if (xl > 1) xl = 1;
    if (yl < 0) yl = 0;
    if (yl > 1) yl = 1;
    int fx, fy;
    if (v->fw && v->fh && v->fw < v->fh) { /* portrait fb, landscape window */
        if (rot_ccw) { fx = (int)(yl * v->fw); fy = (int)((1.0f - xl) * v->fh); }
        else         { fx = (int)((1.0f - yl) * v->fw); fy = (int)(xl * v->fh); }
    } else {
        fx = (int)(xl * v->fw);
        fy = (int)(yl * v->fh);
    }
    unsigned char msg[6] = { 4, (unsigned char)(mask & 0x7f),
                             (unsigned char)(fx >> 8), (unsigned char)(fx & 0xff),
                             (unsigned char)(fy >> 8), (unsigned char)(fy & 0xff) };
    EnterCriticalSection(&v->cs);
    if (!v->connected || send(v->s, (char *)msg, 6, 0) != 6) {
        if (v->connected) { closesocket(v->s); v->s = INVALID_SOCKET; v->connected = 0; }
    }
    LeaveCriticalSection(&v->cs);
}

static void vnc_post(int idx, int kind, int mask, int x, int y, int dir)
{
    vnc_state_t *v = &g_vnc[idx];
    EnterCriticalSection(&v->cs);
    LONG h = v->qh;
    if (h - v->qt < PEV_QN) {
        pev_t *e = &v->q[h % PEV_QN];
        e->kind = kind; e->mask = mask; e->x = x; e->y = y; e->dir = dir;
        v->qh = h + 1;
    }
    LeaveCriticalSection(&v->cs);
    SetEvent(v->wake);
}

static char g_be_name[MAXSLOTS][64];

static void die(const char *m) { fprintf(stderr, "router: %s failed (%d)\n", m, WSAGetLastError()); exit(1); }

#define GRID_W 856
#define GRID_H 480
#define GRID_S "1334x750"

#define ROW_GAP 20   /* extra space below row 1 for external status windows */

/* per-slot resolved device identity & grid placement */
static volatile LONG g_cellw = 853, g_cellh = 480;
static int g_dimw[MAXSLOTS], g_dimh[MAXSLOTS];   /* mirror stream size */
static wchar_t g_wname[MAXSLOTS][128];   /* RFB device name per slot */
static int g_cell_of[MAXSLOTS];          /* grid cell index per slot */
static int g_gi_of[MAXSLOTS];            /* global cfg index per slot (-1 unknown) */


static int g_vrect[MAXSLOTS][4];         /* last applied video window rect */
static int g_cellxy[MAXSLOTS][2];        /* cell origin per slot */
static int g_vrect_set[MAXSLOTS];
static void notify_backend_ip(int idx);
static void spawn_backend(int i, const char *exedir, const char *name);

/* ---------------- group config (optional -config file.ini) ---------------- */
static int g_cfg_active = 0;          /* config mode on, group chosen */
static int g_cfg_ngroups = 0;
static wchar_t g_cfg_gname[8][64];    /* 组N名称 */
static wchar_t g_cfg_title[32][128];  /* 窗口标题N */
static wchar_t g_cfg_ipw[32][64];     /* IP地址N (wide) */
static char g_cfg_ip[32][64];         /* same, narrow for socket compare */
static int g_cfg_count = 0;           /* number of window-title entries */
static int g_cfg_group = -1;          /* chosen group index, 0-based */

static wchar_t *cfg_trimw(wchar_t *s) {
    wchar_t *e;
    while (*s == L' ' || *s == L'\t' || *s == L'\r') s++;
    e = s + wcslen(s);
    while (e > s && (e[-1] == L' ' || e[-1] == L'\t' || e[-1] == L'\r')) *--e = 0;
    return s;
}

static int g_cfg_in_main_sec = 1;   /* accept keys only in [配置] or before any section */

static void cfg_parse_linew(wchar_t *line) {
    wchar_t *eq, *h;
    /* section header: switch acceptance window */
    h = cfg_trimw(line);
    if (h[0] == L'[') {
        g_cfg_in_main_sec = (wcsncmp(h, L"[配置]", 4) == 0);
        return;
    }
    eq = wcschr(h, L'=');
    if (!eq || !g_cfg_in_main_sec) return;
    *eq = 0;
    wchar_t *key = cfg_trimw(h), *val = cfg_trimw(eq + 1);
    if (!wcscmp(key, L"组数量")) {
        g_cfg_ngroups = _wtoi(val);
        return;
    }
    int idx = 0;
    if (swscanf(key, L"组%d名称", &idx) == 1 && idx >= 1 && idx <= 8) {
        _snwprintf(g_cfg_gname[idx - 1], 64, L"%s", val);
        g_cfg_gname[idx - 1][63] = 0;
        return;
    }
    if (swscanf(key, L"窗口标题%d", &idx) == 1 && idx >= 1 && idx <= 32) {
        _snwprintf(g_cfg_title[idx - 1], 128, L"%s", val);
        g_cfg_title[idx - 1][127] = 0;
        if (idx > g_cfg_count) g_cfg_count = idx;
        return;
    }
    if (swscanf(key, L"控制IP%d", &idx) == 1 && idx >= 1 && idx <= 32) {
        _snwprintf(g_cfg_ipw[idx - 1], 64, L"%s", val);
        g_cfg_ipw[idx - 1][63] = 0;
        WideCharToMultiByte(CP_ACP, 0, g_cfg_ipw[idx - 1], -1,
                            g_cfg_ip[idx - 1], 64, NULL, NULL);
        return;
    }
    if (swscanf(key, L"IP地址%d", &idx) == 1 && idx >= 1 && idx <= 32) {
        _snwprintf(g_cfg_ipw[idx - 1], 64, L"%s", val);
        g_cfg_ipw[idx - 1][63] = 0;
        WideCharToMultiByte(CP_ACP, 0, g_cfg_ipw[idx - 1], -1,
                            g_cfg_ip[idx - 1], 64, NULL, NULL);
        return;
    }
}

static char g_svc_name[128];   /* advertised AirPlay name in config mode */

static int load_config(const char *path) {
    g_cfg_in_main_sec = 1;
    HANDLE f = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL,
                           OPEN_EXISTING, 0, NULL);
    if (f == INVALID_HANDLE_VALUE) {
        printf("router: cannot open config %s\n", path);
        return 0;
    }
    DWORD sz = GetFileSize(f, NULL), rd = 0;
    char *buf = (char *) malloc(sz + 2);
    ReadFile(f, buf, sz, &rd, NULL);
    CloseHandle(f);
    buf[rd] = 0;
    buf[rd + 1] = 0;
    /* try UTF-8 first, fall back to ANSI (GBK) */
    int wlen = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, buf, (int) rd, NULL, 0);
    UINT cp = (wlen > 0) ? CP_UTF8 : CP_ACP;
    if (wlen <= 0) wlen = MultiByteToWideChar(CP_ACP, 0, buf, (int) rd, NULL, 0);
    wchar_t *wbuf = (wchar_t *) malloc((wlen + 1) * sizeof(wchar_t));
    MultiByteToWideChar(cp, 0, buf, (int) rd, wbuf, wlen);
    wbuf[wlen] = 0;
    free(buf);
    wchar_t *ctxl = NULL;
    for (wchar_t *line = wcstok_s(wbuf, L"\r\n", &ctxl); line;
         line = wcstok_s(NULL, L"\r\n", &ctxl))
        cfg_parse_linew(line);
    free(wbuf);
    printf("router: config loaded: groups=%d titles=%d\n", g_cfg_ngroups, g_cfg_count);
    for (int i = 0; i < g_cfg_count && i < 32; i++)
        printf("router:   cfg[%02d] title='%ls' ip='%s'\n", i + 1,
               g_cfg_title[i], g_cfg_ip[i][0] ? g_cfg_ip[i] : "(none)");
    return (g_cfg_ngroups > 0 && g_cfg_count >= 5);
}

/* ---------------- group picker dialog ---------------- */
static int g_pick_result = -1;
static int g_pick_n = 0;

static void pick_btn_rect(int i, RECT *rc, int cw) {
    int bw = cw - 88;
    rc->left = 44;
    rc->right = 44 + bw;
    rc->top = 102 + i * 60;
    rc->bottom = rc->top + 46;
}

static int pick_hit(int x, int y, int cw) {
    for (int i = 0; i < g_pick_n; i++) {
        RECT rc;
        pick_btn_rect(i, &rc, cw);
        if (x >= rc.left && x < rc.right && y >= rc.top && y < rc.bottom) return i;
    }
    return -1;
}

static HFONT pick_mkfont(int hgt, LONG weight) {
    return CreateFontW(hgt, 0, 0, 0, weight, 0, 0, 0, DEFAULT_CHARSET,
                       OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                       CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Microsoft YaHei");
}

static LRESULT CALLBACK pick_proc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
    static int hover = -1, tracked = 0;
    switch (m) {
    case WM_SETCURSOR: {
        POINT pt;
        RECT cl;
        GetCursorPos(&pt);
        ScreenToClient(h, &pt);
        GetClientRect(h, &cl);
        if (pick_hit(pt.x, pt.y, cl.right) >= 0) {
            SetCursor(LoadCursorW(NULL, MAKEINTRESOURCEW(IDC_HAND)));
            return TRUE;
        }
        return DefWindowProcW(h, m, wp, lp);
    }
    case WM_MOUSEMOVE: {
        RECT cl;
        GetClientRect(h, &cl);
        int idx = pick_hit((short) LOWORD(lp), (short) HIWORD(lp), cl.right);
        if (idx != hover) { hover = idx; InvalidateRect(h, NULL, FALSE); }
        if (!tracked) {
            TRACKMOUSEEVENT t = { sizeof(t), TME_LEAVE, h, 0 };
            TrackMouseEvent(&t);
            tracked = 1;
        }
        return 0;
    }
    case WM_MOUSELEAVE:
        tracked = 0;
        if (hover != -1) { hover = -1; InvalidateRect(h, NULL, FALSE); }
        return 0;
    case WM_LBUTTONDOWN: {
        RECT cl;
        GetClientRect(h, &cl);
        int idx = pick_hit((short) LOWORD(lp), (short) HIWORD(lp), cl.right);
        if (idx >= 0) { g_pick_result = idx; PostQuitMessage(0); }
        return 0;
    }
    case WM_KEYDOWN:
        if (wp == VK_ESCAPE) { PostQuitMessage(0); return 0; }
        if (wp >= '1' && wp <= '6' && (int)(wp - '1') < g_pick_n) {
            g_pick_result = (int)(wp - '1');
            PostQuitMessage(0);
        }
        return 0;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        RECT cl;
        GetClientRect(h, &cl);
        HDC mem = CreateCompatibleDC(dc);
        HBITMAP bm = CreateCompatibleBitmap(dc, cl.right, cl.bottom);
        HBITMAP obm = (HBITMAP) SelectObject(mem, bm);
        SetBkMode(mem, TRANSPARENT);
        /* page background */
        HBRUSH bg = CreateSolidBrush(RGB(246, 247, 250));
        FillRect(mem, &cl, bg);
        DeleteObject(bg);
        /* header */
        HFONT fh = pick_mkfont(-21, FW_BOLD);
        HFONT fb = pick_mkfont(-19, FW_SEMIBOLD);
        SelectObject(mem, fh);
        SetTextColor(mem, RGB(32, 37, 48));
        RECT hr = {34, 22, cl.right - 34, 58};
        DrawTextW(mem, L"请选择要控制的分组", -1, &hr, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        /* subtitle hint */
        HFONT fs = pick_mkfont(-13, FW_NORMAL);
        SelectObject(mem, fs);
        SetTextColor(mem, RGB(110, 118, 132));
        RECT sr = {34, 60, cl.right - 34, 82};
        DrawTextW(mem, L"仅接受所选组内的设备连接，其他设备将被拒绝", -1, &sr,
                  DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        /* buttons */
        HPEN npen = (HPEN) GetStockObject(NULL_PEN);
        for (int i = 0; i < g_pick_n; i++) {
            RECT rc;
            pick_btn_rect(i, &rc, cl.right);
            COLORREF col = (i == hover) ? RGB(21, 96, 189) : RGB(0, 120, 215);
            HBRUSH br = CreateSolidBrush(col);
            SelectObject(mem, br);
            SelectObject(mem, npen);
            RoundRect(mem, rc.left, rc.top, rc.right, rc.bottom, 12, 12);
            DeleteObject(br);
            wchar_t label[96];
            _snwprintf(label, 96, L"控制%s组", g_cfg_gname[i]);
            label[95] = 0;
            SelectObject(mem, fb);
            SetTextColor(mem, RGB(255, 255, 255));
            DrawTextW(mem, label, -1, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        }
        BitBlt(dc, 0, 0, cl.right, cl.bottom, mem, 0, 0, SRCCOPY);
        SelectObject(mem, obm);
        DeleteObject(bm);
        DeleteDC(mem);
        DeleteObject(fh);
        DeleteObject(fb);
        DeleteObject(fs);
        EndPaint(h, &ps);
        return 0;
    }
    case WM_CLOSE:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(h, m, wp, lp);
}

static int pick_group_dialog(void) {
    WNDCLASSW wc; memset(&wc, 0, sizeof(wc));
    wc.lpfnWndProc = pick_proc;
    wc.hInstance = GetModuleHandleW(NULL);
    wc.hCursor = LoadCursorW(NULL, MAKEINTRESOURCEW(IDC_ARROW));
    wc.hIcon = LoadIconW(GetModuleHandleW(NULL), MAKEINTRESOURCEW(1));
    if (!wc.hIcon) wc.hIcon = LoadIconW(NULL, MAKEINTRESOURCEW(IDI_APPLICATION));
    wc.lpszClassName = L"UxPickGroup";
    RegisterClassW(&wc);
    g_pick_n = g_cfg_ngroups > 6 ? 6 : g_cfg_ngroups;
    int cw = 400, ch = 98 + g_pick_n * 60 + 30;
    RECT fr = {0, 0, cw, ch};
    AdjustWindowRect(&fr, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU, FALSE);
    int fw = fr.right - fr.left, fhh = fr.bottom - fr.top;
    int swx = GetSystemMetrics(SM_CXSCREEN), shy = GetSystemMetrics(SM_CYSCREEN);
    HWND dlg = CreateWindowExW(WS_EX_TOPMOST, L"UxPickGroup",
                               L"AirPlay 群控 - 选择分组",
                               WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_VISIBLE,
                               (swx - fw) / 2, (shy - fhh) / 2, fw, fhh,
                               NULL, NULL, GetModuleHandleW(NULL), NULL);
    {
        HANDLE ic = LoadImageW(GetModuleHandleW(NULL), MAKEINTRESOURCEW(1),
                               IMAGE_ICON, GetSystemMetrics(SM_CXSMICON),
                               GetSystemMetrics(SM_CYSMICON), LR_DEFAULTCOLOR);
        if (ic) SendMessageW(dlg, WM_SETICON, ICON_SMALL, (LPARAM) ic);
    }
    MSG m;
    while (GetMessageW(&m, NULL, 0, 0) > 0) {
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
    DestroyWindow(dlg);
    return g_pick_result;
}
/* ---------------------------------------------------------------------------- */
/*  Strip / status-panel subsystem — lives in a SEPARATE PROCESS (uxplay-panel)
    so external tool injection (damo CreateFoobarRect etc.) cannot freeze the
    router.  Communication is via shared memory.                                 */
static void rlog(const char *fmt, ...);
static int g_strip_h = 20;
static HWND g_strip[MAXSLOTS];    /* HWNDs read from panel process via shm  */

#define PANEL_SHM_NAME   L"UxPlayPanelShm"
#define PANEL_READY_NAME L"UxPlayPanelReady"
#define ROUTER_ALIVE_NAME L"UxPlayRouterAlive"
#define MAXSLOTS_P 5

typedef struct {
    int nslots;
    int alive;
    struct {
        wchar_t cls[128];
        wchar_t title[96];
        int x, y, w, h;
        HWND hwnd;
        int alive;
    } p[MAXSLOTS_P];
} panel_shm_t;

static panel_shm_t   *g_pshm     = NULL;
static HANDLE          g_pshm_h   = NULL;
static HANDLE          g_alive_h  = NULL;
static HANDLE          g_panel_h  = NULL;

static void panel_write_slot(int i, const wchar_t *cls, const wchar_t *title,
                              int x, int y, int w, int h) {
    if (!g_pshm || i < 0 || i >= MAXSLOTS_P) return;
    _snwprintf(g_pshm->p[i].cls,   128, L"%s", cls);
    _snwprintf(g_pshm->p[i].title,  96, L"%s", title);
    g_pshm->p[i].x = x; g_pshm->p[i].y = y;
    g_pshm->p[i].w = w; g_pshm->p[i].h = h;
    g_pshm->p[i].alive = 0;
}
static void panel_clear_slot(int i) {
    if (!g_pshm || i < 0 || i >= MAXSLOTS_P) return;
    g_pshm->p[i].cls[0] = 0;
    g_pshm->p[i].alive = 1;
}
static void panel_sync_handles(void) {
    if (!g_pshm) return;
    for (int i = 0; i < MAXSLOTS_P; i++)
        g_strip[i] = g_pshm->p[i].hwnd;
}
static void panel_start(void) {
    g_pshm_h = CreateFileMappingW(INVALID_HANDLE_VALUE, NULL,
                                   PAGE_READWRITE, 0,
                                   sizeof(panel_shm_t), PANEL_SHM_NAME);
    if (!g_pshm_h) return;
    g_pshm = (panel_shm_t *) MapViewOfFile(g_pshm_h,
                  FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, sizeof(panel_shm_t));
    if (!g_pshm) { CloseHandle(g_pshm_h); g_pshm_h = NULL; return; }
    memset(g_pshm, 0, sizeof(*g_pshm));
    g_pshm->nslots = MAXSLOTS_P;

    g_alive_h = CreateEventW(NULL, TRUE, TRUE, ROUTER_ALIVE_NAME);
    HANDLE hReady = CreateEventW(NULL, TRUE, FALSE, PANEL_READY_NAME);

    wchar_t exe[MAX_PATH + 4];
    GetModuleFileNameW(NULL, exe, MAX_PATH);
    wchar_t *bs = wcsrchr(exe, L'\\');
    if (bs) { wcscpy(bs + 1, L"uxplay-panel.exe"); }
    else    { wcscpy(exe, L"uxplay-panel.exe"); }
    wchar_t cmd[MAX_PATH + 8];
    _snwprintf(cmd, sizeof(cmd)/sizeof(wchar_t), L"\"%s\"", exe);
    cmd[sizeof(cmd)/sizeof(wchar_t)-1] = 0;

    STARTUPINFOW si; memset(&si, 0, sizeof(si)); si.cb = sizeof(si);
    PROCESS_INFORMATION pi; memset(&pi, 0, sizeof(pi));
    if (!CreateProcessW(NULL, cmd, NULL, NULL, FALSE, 0,
                        NULL, NULL, &si, &pi)) {
        if (hReady) CloseHandle(hReady);
        return;
    }
    g_panel_h = pi.hProcess;
    CloseHandle(pi.hThread);
    if (hReady) {
        WaitForSingleObject(hReady, 3000);
        CloseHandle(hReady);
    }
}

typedef struct { DWORD pid; HWND hwnd; int best_area; } findwin_t;

static BOOL CALLBACK enum_win_proc(HWND h, LPARAM lp) {
    findwin_t *ctx = (findwin_t *) lp;
    DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    if (pid == ctx->pid && IsWindowVisible(h) && !GetWindow(h, GW_OWNER)) {
        RECT r;
        if (GetWindowRect(h, &r)) {
            int area = (r.right - r.left) * (r.bottom - r.top);
            /* pick the LARGEST visible top-level window of the backend —
               the d3dvideosink video window dwarfs any helper/status
               window, so "first visible" can't grab a wrong small one */
            if (area > ctx->best_area) {
                ctx->hwnd = h;
                ctx->best_area = area;
            }
        }
    }
    return TRUE;
}

static void apply_grid_layout(void)
{
    int i, cols = 3, rows = 2;
    /* HWND we last styled/positioned per slot.  The backend's video window is
       destroyed & recreated on rotation / pipeline reset; a fresh HWND must
       force re-styling (borderless + position) even when the video rect did
       not change — otherwise the recreated window keeps its default frame and
       shows up unpositioned (looks like it "vanished"). */
    static HWND styled_hwnd[MAXSLOTS];
    static int nf_logged[MAXSLOTS];
    int sw = GetSystemMetrics(SM_CXSCREEN);
    int sh = GetSystemMetrics(SM_CYSCREEN);
    int w = sw / cols;
    int h = w * GRID_H / GRID_W;
    if (rows * h + ROW_GAP > sh) {
        h = (sh - ROW_GAP) / rows;
        w = h * GRID_W / GRID_H;
    }
    g_cellw = w;
    g_cellh = h;
    g_map_cw = w;
    g_map_ch = h;
    /* keep backends' ip targets fresh (UDP may drop; heals missed notifies) */
    for (i = 0; i < g_nslots && i < 5; i++)
        if (g_slot[i].used) notify_backend_ip(i);
    for (i = 0; i < g_nslots && i < 5; i++) {
        int cell = (g_gi_of[i] >= 0) ? g_cell_of[i] : i;
        int gx = (cell % cols) * w;
        int gy = (cell / cols) * h + ((cell / cols) > 0 ? ROW_GAP : 0);
        g_cellxy[i][0] = gx;
        g_cellxy[i][1] = gy;
        /* video rect: portrait streams keep aspect (pillarbox centered),
           landscape fills the whole cell */
        int vw = g_dimw[i], vh = g_dimh[i];
        int vwd = w, vht = h, vx = gx, vy = gy;
        if (vw > 0 && vh > 0 && vh > vw) {
            vwd = (int)((double) h * vw / vh);
            if (vwd < 1) vwd = 1;
            if (vwd > w) vwd = w;
            vx = gx + (w - vwd) / 2;
        }
        if (!g_vrect_set[i] || g_vrect[i][0] != vx || g_vrect[i][1] != vy ||
            g_vrect[i][2] != vwd || g_vrect[i][3] != vht)
            g_slot[i].styled = 0;   /* rect changed: re-place window */
        g_vrect[i][0] = vx; g_vrect[i][1] = vy; g_vrect[i][2] = vwd; g_vrect[i][3] = vht;
        g_vrect_set[i] = 1;
        if (!g_child[i]) continue;
        if (WaitForSingleObject(g_child[i], 0) != WAIT_TIMEOUT) continue;
        findwin_t ctx = { g_child_pid[i], NULL, 0 };
        EnumWindows(enum_win_proc, (LPARAM)&ctx);
        if (!ctx.hwnd) {
            if (!nf_logged[i]) {
                fprintf(stderr, "router: slot %d: no visible window for backend pid %lu yet\n",
                        i + 1, (unsigned long) g_child_pid[i]);
                nf_logged[i] = 1;
            }
            continue;
        }
        nf_logged[i] = 0;
        /* recreated window (rotation/pipeline reset) → force re-style+position */
        if (styled_hwnd[i] != ctx.hwnd) {
            g_slot[i].styled = 0;
            styled_hwnd[i] = ctx.hwnd;
            {
                wchar_t cls[128];
                if (GetClassNameW(ctx.hwnd, cls, 128) > 0) {
                    RECT r; GetWindowRect(ctx.hwnd, &r);
                    fprintf(stderr, "router: slot %d: video window class='%ls' size=%dx%d\n",
                            i + 1, cls, r.right - r.left, r.bottom - r.top);
                }
            }
        }
        if (!g_slot[i].styled) {
            LONG_PTR st = GetWindowLongPtrW(ctx.hwnd, GWL_STYLE);
            st &= ~(WS_CAPTION | WS_THICKFRAME | WS_SYSMENU |
                    WS_MINIMIZEBOX | WS_MAXIMIZEBOX | WS_DLGFRAME);
            st |= WS_POPUP;
            SetWindowLongPtrW(ctx.hwnd, GWL_STYLE, st);
            SetWindowPos(ctx.hwnd, HWND_TOPMOST, vx, vy, vwd, vht,
                         SWP_FRAMECHANGED | SWP_SHOWWINDOW);
            g_slot[i].styled = 1;
            printf("router: slot %d video window at (%d,%d) %dx%d\n", i + 1, vx, vy, vwd, vht);
        }
        /* video window title: use resolved device name when known */
        {
            wchar_t want[128];
            int have = 0;
            if (g_wname[i][0]) { _snwprintf(want, 128, L"%s", g_wname[i]); have = 1; }
            else if (g_cfg_active && g_cfg_group >= 0 && g_gi_of[i] >= 0) {
                _snwprintf(want, 128, L"%s", g_cfg_title[g_gi_of[i]]); have = 1;
            }
            if (have) {
                wchar_t cur[128];
                int cl = GetWindowTextW(ctx.hwnd, cur, 128);
                cur[127] = 0;
                if (cl == 0 || wcscmp(cur, want) != 0 ||
                    !wcsncmp(cur, L"Gst", 3) || !wcsncmp(cur, L"Direct3D", 8)) {
                    SetWindowTextW(ctx.hwnd, want);
                    /* also set ANSI title so tools using GetWindowTextA
                       (按键抓抓 tree-view etc.) see correct text */
                    char narrow[128];
                    WideCharToMultiByte(CP_ACP, 0, want, -1, narrow, sizeof(narrow), NULL, NULL);
                    SetWindowTextA(ctx.hwnd, narrow);
                    rlog("retitle slot %d: '%ls' -> '%ls'", i + 1, cur, want);
                }
            }
        }
        /* status strip — lives in panel.exe process (shared memory) */
        {
            static int sx[MAXSLOTS], sy[MAXSLOTS], swd[MAXSLOTS];
            static int sgi[MAXSLOTS];
            int gi = (g_cfg_active && g_cfg_group >= 0 && g_gi_of[i] >= 0) ? g_gi_of[i] : -1;
            panel_sync_handles();
            HWND sw = g_strip[i];
            if (sw && gi != sgi[i]) {
                panel_clear_slot(i);
                sw = NULL; g_strip[i] = NULL;
                sgi[i] = -1;
            }
            if (!sw && g_pshm) {
                wchar_t cls[128], title[96];
                if (g_wname[i][0]) {
                    wchar_t ipw[64];
                    MultiByteToWideChar(CP_ACP, 0, g_slot[i].ip, -1, ipw, 64);
                    ipw[63] = 0;
                    _snwprintf(cls, 128, L"%s", g_wname[i]);
                    cls[127] = 0;
                    _snwprintf(title, 96, L"%d|%s", gi + 1, ipw);
                } else if (gi >= 0) {
                    _snwprintf(cls, 128, L"%s", g_cfg_title[gi]);
                    cls[127] = 0;
                    _snwprintf(title, 96, L"%d|%s", gi + 1, g_cfg_ipw[gi]);
                } else {
                    _snwprintf(cls, 128, L"UxPlayStrip");
                    _snwprintf(title, 96, L"UxPlay-Strip-%d", i + 1);
                }
                title[95] = 0;
                panel_write_slot(i, cls, title, gx, gy + h, w, g_strip_h);
                sgi[i] = gi; sx[i] = -1;
            } else if (sw && g_pshm && (gx != sx[i] || gy != sy[i] || w != swd[i])) {
                g_pshm->p[i].x = gx; g_pshm->p[i].y = gy + h;
                g_pshm->p[i].w = w;  g_pshm->p[i].h = g_strip_h;
                sx[i] = gx; sy[i] = gy; swd[i] = w;
            }
        }
    }
}

/* ---------------- tray icon (exit without console) ---------------- */
#define WM_TRAY (WM_APP + 100)
#define IDM_TRAY_EXIT 2001
static NOTIFYICONDATAW g_nid;

static void router_kill_children(void) {
    for (int i = 0; i < 5; i++) {
        if (g_child[i] && WaitForSingleObject(g_child[i], 0) == WAIT_TIMEOUT)
            TerminateProcess(g_child[i], 0);
    }
    if (g_panel_h) {
        if (g_alive_h) ResetEvent(g_alive_h);
        WaitForSingleObject(g_panel_h, 1000);
        TerminateProcess(g_panel_h, 0);
        CloseHandle(g_panel_h); g_panel_h = NULL;
    }
    if (g_pshm) { UnmapViewOfFile(g_pshm); g_pshm = NULL; }
    if (g_pshm_h) { CloseHandle(g_pshm_h); g_pshm_h = NULL; }
}

static LRESULT CALLBACK tray_proc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
    if (m == WM_TRAY && (lp == WM_RBUTTONUP || lp == WM_LBUTTONUP)) {
        POINT pt;
        GetCursorPos(&pt);
        HMENU menu = CreatePopupMenu();
        AppendMenuW(menu, MF_STRING, IDM_TRAY_EXIT, L"退出");
        SetForegroundWindow(h);
        int cmd = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_NONOTIFY | TPM_RIGHTBUTTON,
                                 pt.x, pt.y, 0, h, NULL);
        DestroyMenu(menu);
        if (cmd == IDM_TRAY_EXIT) {
            router_kill_children();
            Shell_NotifyIconW(NIM_DELETE, &g_nid);
            ExitProcess(0);
        }
        return 0;
    }
    return DefWindowProcW(h, m, wp, lp);
}

static wchar_t g_tray_tip[128] = L"AirPlay 群控";

static void tray_set_tip(const wchar_t *tip) {
    EnterCriticalSection(&g_lock);
    _snwprintf(g_tray_tip, 128, L"%s", tip);
    g_tray_tip[127] = 0;
    LeaveCriticalSection(&g_lock);
    if (g_nid.hWnd) {
        wcscpy(g_nid.szTip, g_tray_tip);
        Shell_NotifyIconW(NIM_MODIFY, &g_nid);
    }
}

/* tray balloon — non-blocking way to tell the operator a device was
   rejected (config mode: name not in the selected group).  No-op when
   running with a console (debug mode) where rlog covers it. */
static void tray_notify(const wchar_t *title, const wchar_t *msg) {
    if (!g_nid.hWnd) return;                       /* no tray icon */
    wcsncpy(g_nid.szInfoTitle, title, 63);  g_nid.szInfoTitle[63] = 0;
    wcsncpy(g_nid.szInfo,     msg,   255);  g_nid.szInfo[255]     = 0;
    g_nid.uFlags = NIF_INFO;
    g_nid.dwInfoFlags = NIIF_INFO;
    Shell_NotifyIconW(NIM_MODIFY, &g_nid);
    g_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;  /* restore for tip updates */
}

static HWND tray_init(void) {
    WNDCLASSW tc; memset(&tc, 0, sizeof(tc));
    tc.lpfnWndProc = tray_proc;
    tc.hInstance = GetModuleHandleW(NULL);
    tc.lpszClassName = L"UxTray";
    RegisterClassW(&tc);
    HWND w = CreateWindowExW(0, L"UxTray", L"UxTrayMsg", WS_POPUP,
                             0, 0, 0, 0, NULL, NULL, GetModuleHandleW(NULL), NULL);
    memset(&g_nid, 0, sizeof(g_nid));
    g_nid.cbSize = sizeof(g_nid);
    g_nid.hWnd = w;
    g_nid.uID = 1;
    g_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    g_nid.uCallbackMessage = WM_TRAY;
    g_nid.hIcon = LoadIconW(GetModuleHandleW(NULL), MAKEINTRESOURCEW(1));
    if (!g_nid.hIcon) {
        wchar_t exe[MAX_PATH];
        GetModuleFileNameW(NULL, exe, MAX_PATH);
        g_nid.hIcon = ExtractAssociatedIconW(GetModuleHandleW(NULL), exe, &(WORD){0});
    }
    if (!g_nid.hIcon) g_nid.hIcon = LoadIconW(NULL, MAKEINTRESOURCEW(IDI_APPLICATION));
    EnterCriticalSection(&g_lock);
    wcscpy(g_nid.szTip, g_tray_tip);
    LeaveCriticalSection(&g_lock);
    Shell_NotifyIconW(NIM_ADD, &g_nid);
    return w;
}

static DWORD WINAPI window_mgr_thread(LPVOID p) {
    (void) p;
    if (!GetConsoleWindow()) tray_init();
    for (;;) {
        apply_grid_layout();
        DWORD t0 = GetTickCount();
        while (GetTickCount() - t0 < 900) {
            MSG m;
            while (PeekMessageW(&m, NULL, 0, 0, PM_REMOVE)) {
                TranslateMessage(&m);
                DispatchMessageW(&m);
            }
            Sleep(15);
        }
    }
    return 0;
}

static char *build_child_env(int base) {    LPCH os = GetEnvironmentStringsA();
    const char *r = os;
    size_t olen = 1;
    while (*r) {
        size_t l = strlen(r) + 1;
        if (_strnicmp(r, "UXPLAY_PORT_REMAP=", 18) != 0) olen += l;
        r += l;
    }
    char pairs[512];
    int plen = _snprintf(pairs, sizeof(pairs) - 2,
              "UXPLAY_PORT_REMAP=%d=%d,%d=%d,%d=%d,%d=%d,%d=%d,%d=%d",
              base, pub_tcp[0] + PUBS(), base + 1, pub_tcp[1] + PUBS(), base + 2, pub_tcp[2] + PUBS(), base + 3, pub_udp[0] + PUBS(), base + 4, pub_udp[1] + PUBS(), base + 5, pub_udp[2] + PUBS());
    plen += 1;
    int plen2 = _snprintf(pairs + plen, sizeof(pairs) - plen - 1,
              "UXPLAY_CTRL_PORT=%d", base + 6);
    plen2 += 1;
    pairs[plen + plen2] = 0;
    char *blk = (char *)malloc(olen + plen + plen2 + 2);
    char *w = blk; r = os;
    while (*r) {
        size_t l = strlen(r) + 1;
        if (_strnicmp(r, "UXPLAY_PORT_REMAP=", 18) != 0 &&
            _strnicmp(r, "UXPLAY_CTRL_PORT=", 17) != 0) { memcpy(w, r, l); w += l; }
        r += l;
    }
    memcpy(w, pairs, plen + plen2); w += plen + plen2;
    *w = 0;
    FreeEnvironmentStringsA(os);
    return blk;
}

static void notify_backend_ip(int idx) {
    SOCKET u = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (u == INVALID_SOCKET) return;
    SOCKADDR_IN d; memset(&d, 0, sizeof(d));
    d.sin_family = AF_INET;
    d.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    d.sin_port = htons((unsigned short)(BE_BASE() + (idx + 1) * 10 + 6));
    char msg[80];
    int n = _snprintf(msg, sizeof(msg), "ip %s", g_slot[idx].ip);
    int sent = sendto(u, msg, n, 0, (SOCKADDR *)&d, sizeof(d));
    printf("router: notify backend %d ctrl=%d '%s' sent=%d err=%d\n",
           idx + 1, 20000 + (idx + 1) * 10 + 6, msg, sent,
           sent < 0 ? WSAGetLastError() : 0);
    closesocket(u);
}

static int slot_assign(const char *ip) {
    int i, free_idx = -1;
    /* device NAME is authoritative in every mode: any device may take a
       free slot regardless of IP (wifi / usb / dhcp all fine); wrong
       devices are evicted once their VNC name report arrives */
    EnterCriticalSection(&g_lock);
    for (i = 0; i < g_nslots; i++) {
        if (g_slot[i].used && !strcmp(g_slot[i].ip, ip)) {
            g_slot[i].last_active = time(NULL);
            LeaveCriticalSection(&g_lock);
            return i;
        }
        if (!g_slot[i].used && free_idx < 0) free_idx = i;
    }
    if (free_idx >= 0) {
        memset(&g_slot[free_idx], 0, sizeof(slot_t));
        strncpy(g_slot[free_idx].ip, ip, sizeof(g_slot[free_idx].ip) - 1);
        g_slot[free_idx].used = 1;
        g_slot[free_idx].last_active = time(NULL);
    }
    LeaveCriticalSection(&g_lock);
    if (free_idx >= 0 && free_idx < 5) notify_backend_ip(free_idx);
    return free_idx; /* -1: all slots busy -> connection dropped */
}

static void slot_touch(const char *ip) {
    int i;
    EnterCriticalSection(&g_lock);
    for (i = 0; i < g_nslots; i++) {
        if (g_slot[i].used && !strcmp(g_slot[i].ip, ip)) {
            g_slot[i].last_active = time(NULL);
            break;
        }
    }
    LeaveCriticalSection(&g_lock);
}

static void strip_v4map(char *s) {
    if (!_strnicmp(s, "::ffff:", 7)) {
        memmove(s, s + 7, strlen(s) - 6);
    }
}

static void ip_of(SOCKET s, char *out, size_t n) {
    SOCKADDR_STORAGE a; int l = sizeof(a);
    out[0] = 0;
    if (getpeername(s, (SOCKADDR *)&a, &l) == 0) {
        char h[64];
        if (getnameinfo((SOCKADDR *)&a, l, h, sizeof(h), NULL, 0, NI_NUMERICHOST) == 0) {
            strncpy(out, h, n - 1); out[n - 1] = 0;
            strip_v4map(out);
            return;
        }
    }
    strncpy(out, "unknown", n - 1); out[n - 1] = 0;
}

static DWORD WINAPI relay_thread(LPVOID p) {
    intptr_t *argp = (intptr_t *)p;
    SOCKET a = (SOCKET)argp[0];
    SOCKET b = (SOCKET)argp[1];
    int idx = (int)argp[2];
    char buf[65536];
    for (;;) {
        fd_set rs; FD_ZERO(&rs); FD_SET(a, &rs); FD_SET(b, &rs);
        struct timeval tv = {30, 0};
        int r = select(0, &rs, NULL, NULL, &tv);
        if (r <= 0) break;
        if (FD_ISSET(a, &rs)) {
            int n = recv(a, buf, sizeof(buf), 0);
            if (n <= 0 || send(b, buf, n, 0) != n) break;
        }
        if (FD_ISSET(b, &rs)) {
            int n = recv(b, buf, sizeof(buf), 0);
            if (n <= 0 || send(a, buf, n, 0) != n) break;
        }
    }
    closesocket(a); closesocket(b);
    InterlockedDecrement(&g_slot[idx].conns);
    g_slot[idx].last_active = time(NULL);
    free(p);
    return 0;
}

typedef struct { SOCKET c; int idx; int port; } acc_t;

static DWORD WINAPI accept_thread(LPVOID p) {
    acc_t *pa = (acc_t *)p;
    SOCKET l = pa->c;
    for (;;) {
        SOCKET s = accept(l, NULL, NULL);
        if (s == INVALID_SOCKET) break;
        char ip[64]; ip_of(s, ip, sizeof(ip));
        int idx = slot_assign(ip);
        if (idx < 0) { fprintf(stderr, "router: all %d slots busy, dropped %s\n", g_nslots, ip); closesocket(s); continue; }
        SOCKET b = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        SOCKADDR_IN dst; dst.sin_family = AF_INET; dst.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        dst.sin_port = htons((unsigned short)(BE_BASE() + (idx + 1) * 10 + pa->port));
        /* the backend's mirror-data port is only created during RTSP SETUP, so
           the very first device connection races it; retry briefly instead of
           failing immediately.  WSAECONNREFUSED (10061) = port not up yet. */
        int cok = 0;
        int max_attempt = (pa->port == 2) ? 25 : 1;   /* only mirror waits for SETUP */
        for (int attempt = 0; attempt < max_attempt; attempt++) {
            if (connect(b, (SOCKADDR *)&dst, sizeof(dst)) == 0) { cok = 1; break; }
            int err = WSAGetLastError();
            if (attempt == 0)
                fprintf(stderr, "router: backend %d port %u connect fail (err=%d)%s\n",
                        idx + 1, (unsigned)ntohs(dst.sin_port), err,
                        pa->port == 2 ? ", retrying for RTSP SETUP..." : "");
            if (err != WSAECONNREFUSED && err != WSAETIMEDOUT) break;  /* fatal */
            closesocket(b);
            b = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
            if (b == INVALID_SOCKET) break;
            Sleep(120);
        }
        if (!cok) {
            fprintf(stderr, "router: backend %d not reachable (port %u)\n",
                    idx + 1, (unsigned)ntohs(dst.sin_port));
            closesocket(s); closesocket(b); continue;
        }
        { BOOL nd = TRUE; setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (char *)&nd, sizeof(nd));
          setsockopt(b, IPPROTO_TCP, TCP_NODELAY, (char *)&nd, sizeof(nd));
          int bufsz = 512 * 1024;
          setsockopt(s, SOL_SOCKET, SO_RCVBUF, (char *)&bufsz, sizeof(bufsz));
          setsockopt(s, SOL_SOCKET, SO_SNDBUF, (char *)&bufsz, sizeof(bufsz));
          setsockopt(b, SOL_SOCKET, SO_RCVBUF, (char *)&bufsz, sizeof(bufsz));
          setsockopt(b, SOL_SOCKET, SO_SNDBUF, (char *)&bufsz, sizeof(bufsz)); }
        InterlockedIncrement(&g_slot[idx].conns);
        intptr_t *arg = malloc(sizeof(intptr_t) * 3);
        arg[0] = s; arg[1] = b; arg[2] = (intptr_t)idx;
        HANDLE t = CreateThread(NULL, 0, relay_thread, arg, 0, NULL);
        if (t) CloseHandle(t);
    }
    free(pa);
    return 0;
}

static int flow_find(uflow_t *tbl, const char *ip) {
    int i;
    for (i = 0; i < MAXFLOWS; i++)
        if (tbl[i].used && !strcmp(tbl[i].ip, ip)) return i;
    return -1;
}

static int flow_create(uflow_t *tbl, const char *ip, SOCKADDR *cli, int clilen, int beport) {
    int i;
    for (i = 0; i < MAXFLOWS; i++) {
        if (!tbl[i].used) {
            SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
            SOCKADDR_IN dst; dst.sin_family = AF_INET; dst.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            dst.sin_port = htons((unsigned short)beport);
            if (s == INVALID_SOCKET || connect(s, (SOCKADDR *)&dst, sizeof(dst)) != 0) return -1;
            tbl[i].used = 1; strncpy(tbl[i].ip, ip, sizeof(tbl[i].ip) - 1);
            memcpy(&tbl[i].cli, cli, clilen); tbl[i].clilen = clilen;
            tbl[i].s = s; tbl[i].last_active = time(NULL);
            return i;
        }
    }
    return -1;
}

/* ---------------- HTTP API (novnc-cef-client compatible) ---------------- */
#define API_W 853
#define API_H 480

/* tiny JSON field extractors (flat objects only) */
static const char *jval(const char *j, const char *k) {
    char pat[64];
    _snprintf(pat, sizeof(pat), "\"%s\"", k);
    const char *p = j;
    while ((p = strstr(p, pat)) != NULL) {
        p += strlen(pat);
        while (*p == ' ' || *p == ':' || *p == '\t' || *p == '\r' || *p == '\n') p++;
        return p;
    }
    return NULL;
}

static int jget_int(const char *j, const char *k, int def) {
    const char *p = jval(j, k);
    if (!p) return def;
    /* skip JSON string quotes so "769" and 769 both work */
    if (*p == '"') p++;
    int neg = 0;
    if (*p == '-') { neg = 1; p++; }
    if (*p < '0' || *p > '9') return def;
    long v = 0;
    while (*p >= '0' && *p <= '9') v = v * 10 + (*p++ - '0');
    return (int)(neg ? -v : v);
}

static void junescape(const char *s, const char *end, char *out, size_t n) {
    size_t o = 0;
    while (s < end && o + 8 < n) {
        if (*s == '\\') {
            s++;
            if (s >= end) break;
            switch (*s) {
            case 'n': out[o++] = '\n'; s++; break;
            case 'r': out[o++] = '\r'; s++; break;
            case 't': out[o++] = '\t'; s++; break;
            case 'b': out[o++] = '\b'; s++; break;
            case 'f': out[o++] = '\f'; s++; break;
            case 'u': {
                unsigned cp = 0;
                if (s + 4 < end) {
                    for (int i = 1; i <= 4 && s + i < end; i++) {
                        char c = s[i];
                        cp = cp * 16 + (c >= '0' && c <= '9' ? c - '0' :
                                        c >= 'a' && c <= 'f' ? c - 'a' + 10 :
                                        c >= 'A' && c <= 'F' ? c - 'A' + 10 : 0);
                    }
                    /* encode UTF-8 */
                    if (cp < 0x80) out[o++] = (char) cp;
                    else if (cp < 0x800) {
                        out[o++] = (char)(0xC0 | (cp >> 6));
                        out[o++] = (char)(0x80 | (cp & 0x3F));
                    } else {
                        out[o++] = (char)(0xE0 | (cp >> 12));
                        out[o++] = (char)(0x80 | ((cp >> 6) & 0x3F));
                        out[o++] = (char)(0x80 | (cp & 0x3F));
                    }
                }
                s += 5;
                break;
            }
            default: out[o++] = *s++; break;   /* \" \\ \/ */
            }
        } else out[o++] = *s++;
    }
    out[o] = 0;
}

static int jget_str(const char *j, const char *k, char *out, size_t n) {
    const char *p = jval(j, k);
    if (!p || *p != '"') return 0;
    p++;
    const char *e = p;
    while (e < j + strlen(j)) {
        if (*e == '\\' ) { e += 2; continue; }
        if (*e == '"') break;
        e++;
    }
    junescape(p, e, out, (int) n);
    return 1;
}

/* keysym lookup for keypress action */
static unsigned int api_keysym(const char *code) {
    static const struct { const char *n; unsigned int s; } t[] = {
        {"Enter", 0xFF0D}, {"Backspace", 0xFF08}, {"Tab", 0xFF09},
        {"Escape", 0xFF1B}, {"Delete", 0xFFFF}, {"Home", 0xFF50},
        {"End", 0xFF57}, {"PageUp", 0xFF55}, {"PageDown", 0xFF56},
        {"ArrowLeft", 0xFF51}, {"ArrowUp", 0xFF52},
        {"ArrowRight", 0xFF53}, {"ArrowDown", 0xFF54},
        {"Space", 0x0020}, {"ControlLeft", 0xFFE3}, {"ControlRight", 0xFFE4},
        {"ShiftLeft", 0xFFE1}, {"ShiftRight", 0xFFE2},
    };
    for (size_t i = 0; i < sizeof(t) / sizeof(t[0]); i++)
        if (!_stricmp(t[i].n, code)) return t[i].s;
    if (!_strnicmp(code, "Key", 3) && code[3] && !code[4])
        return (unsigned int) tolower((unsigned char) code[3]);
    if (!_strnicmp(code, "Digit", 5) && code[5] && !code[6])
        return (unsigned int) code[5];
    if (!code[1]) return (unsigned int) code[0];
    return 0;
}

/* forward a raw command datagram to backend slot */
static void be_cmd(int slot, const char *cmd, int len) {
    SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == INVALID_SOCKET) return;
    BOOL nd = TRUE;
    setsockopt(s, SOL_SOCKET, SO_BROADCAST, (char *) &nd, sizeof(nd));
    SOCKADDR_IN dst; memset(&dst, 0, sizeof(dst));
    dst.sin_family = AF_INET;
    dst.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    dst.sin_port = htons((unsigned short)(BE_BASE() + (slot + 1) * 10 + 6));
    sendto(s, cmd, len, 0, (SOCKADDR *) &dst, sizeof(dst));
    closesocket(s);
}

static void be_pointer(int slot, int mask, int wx, int wy) {
    char b[96];
    int l = _snprintf(b, sizeof(b), "P %d %d %d", mask, wx, wy);
    be_cmd(slot, b, l);
}

/* convert an API-space (853x480) point to local video-window pixels.
   returns 0 when the point misses the pillarboxed portrait picture.
   clamp=1 keeps edge points instead of dropping them. */
static int api_point(int slot, int ax, int ay, int clampv, int *ox, int *oy) {
    int cw = g_cellw < 1 ? 1 : g_cellw, ch = g_cellh < 1 ? 1 : g_cellh;
    int cx = (int)((long long) ax * cw / API_W), cy = (int)((long long) ay * ch / API_H);
    if (!g_vrect_set[slot]) { *ox = cx; *oy = cy; return 1; }
    int lx = cx - (g_vrect[slot][0] - g_cellxy[slot][0]);
    int ly = cy - (g_vrect[slot][1] - g_cellxy[slot][1]);
    int vw2 = g_vrect[slot][2], vh2 = g_vrect[slot][3];
    if (clampv) {
        if (lx < 0) lx = 0;
        if (ly < 0) ly = 0;
        if (lx >= vw2) lx = vw2 - 1;
        if (ly >= vh2) ly = vh2 - 1;
    } else {
        if (lx < 0 || ly < 0 || lx >= vw2 || ly >= vh2) return 0;
    }
    *ox = lx; *oy = ly;
    return 1;
}

static int api_scale_x(int x) {
    int cw = g_cellw;
    if (cw < 1) cw = 1;
    int v = (int)((long long) x * cw / API_W);
    if (v < 0) v = 0;
    if (v >= cw) v = cw - 1;
    return v;
}

static int api_scale_y(int y) {
    int ch = g_cellh;
    if (ch < 1) ch = 1;
    int v = (int)((long long) y * ch / API_H);
    if (v < 0) v = 0;
    if (v >= ch) v = ch - 1;
    return v;
}

/* drag synthesis with cancel support */
typedef struct { int slot; volatile LONG abort; HANDLE t; int last_x, last_y; } adrag_t;
static adrag_t g_ad[MAXSLOTS];

static void api_cancel_drag(int slot) {
    adrag_t *d = &g_ad[slot];
    if (d->t && WaitForSingleObject(d->t, 0) == WAIT_OBJECT_0) {
        CloseHandle(d->t);
        d->t = NULL;
        return;
    }
    if (d->t) {
        InterlockedExchange(&d->abort, 1);
        WaitForSingleObject(d->t, 2000);
        CloseHandle(d->t);
        d->t = NULL;
        be_pointer(slot, 0, d->last_x, d->last_y);   /* safety release */
    }
}

/* per-drag params storage indexed by slot */
static int g_d_fromx[MAXSLOTS], g_d_fromy[MAXSLOTS], g_d_tox[MAXSLOTS], g_d_toy[MAXSLOTS];
static int g_d_dur[MAXSLOTS], g_d_hold[MAXSLOTS], g_d_ease[MAXSLOTS];

static DWORD WINAPI api_drag_run(LPVOID pv) {
    adrag_t *d = (adrag_t *) pv;
    int slot = d->slot;
    int fx, fy, tx, ty;
    api_point(slot, g_d_fromx[slot], g_d_fromy[slot], 1, &fx, &fy);
    api_point(slot, g_d_tox[slot],   g_d_toy[slot],   1, &tx, &ty);
    int dur = g_d_dur[slot] > 0 ? g_d_dur[slot] : 300;
    int hold = g_d_hold[slot] > 0 ? g_d_hold[slot] : 0;
    int ease = g_d_ease[slot];
    be_pointer(slot, 1, fx, fy);
    d->last_x = fx; d->last_y = fy;
    Sleep(20);
    int steps = dur / 16;
    if (steps < 2) steps = 2;
    if (steps > 100) steps = 100;
    float stepms = (float) dur / steps;
    for (int i = 1; i <= steps; i++) {
        if (d->abort) break;
        float t = (float) i / steps;
        if (ease)
            t = t < 0.5f ? 2 * t * t : 1 - (-2 * t + 2) * (-2 * t + 2) / 2;
        int cx = fx + (int)((tx - fx) * t);
        int cy = fy + (int)((ty - fy) * t);
        be_pointer(slot, 1, cx, cy);
        d->last_x = cx; d->last_y = cy;
        Sleep((DWORD) stepms);
    }
    if (!d->abort) {
        if (hold) Sleep(hold);
        be_pointer(slot, 0, tx, ty);
        d->last_x = tx; d->last_y = ty;
    } else {
        be_pointer(slot, 0, d->last_x, d->last_y);
    }
    return 0;
}

static void api_start_drag(int slot, int fx, int fy, int tx, int ty,
                           int dur, int hold, int ease) {
    api_cancel_drag(slot);
    adrag_t *d = &g_ad[slot];
    g_d_fromx[slot] = fx; g_d_fromy[slot] = fy;
    g_d_tox[slot] = tx;   g_d_toy[slot] = ty;
    g_d_dur[slot] = dur;  g_d_hold[slot] = hold; g_d_ease[slot] = ease;
    d->slot = slot;
    InterlockedExchange(&d->abort, 0);
    d->t = CreateThread(NULL, 0, api_drag_run, d, 0, NULL);
}

/* windowIndex parsing: "13" → windows 1+3; number compat; default 1 */
/* find the slot whose grid cell == cell_idx; returns -1 if none */
static int slot_for_cell(int cell_idx) {
    for (int i = 0; i < g_nslots && i < MAXSLOTS; i++) {
        int cell = (g_gi_of[i] >= 0) ? g_cell_of[i] : i;
        if (cell == cell_idx) return i;
    }
    return -1;
}

static int api_targets(const char *body, int *out) {
    char wi[64];
    int n = 0, have = jget_str(body, "windowIndex", wi, sizeof(wi));
    if (!have) {
        const char *p = jval(body, "windowIndex");
        if (p && *p >= '0' && *p <= '9') {
            int num = jget_int(body, "windowIndex", 1);
            if (num <= 0) num = 1;
            wi[0] = (char)('0' + num);
            wi[1] = 0;
            have = 1;
        }
    }
    if (!have) { wi[0] = '1'; wi[1] = 0; }
    for (const char *c = wi; *c; c++) {
        if (*c >= '1' && *c <= '5') {
            int cell = *c - '1';          /* 0-based grid cell index */
            int slot = slot_for_cell(cell);
            if (slot < 0) continue;
            int dup = 0;
            for (int i = 0; i < n; i++) if (out[i] == slot) dup = 1;
            if (!dup && n < MAXSLOTS) out[n++] = slot;
        }
    }
    return n;
}

static int api_handle_control(const char *body, char *msg, size_t msz) {
    char action[32] = {0};
    jget_str(body, "action", action, sizeof(action));
    int targets[MAXSLOTS];
    int nt = api_targets(body, targets);
    if (nt <= 0) {
        _snprintf(msg, msz, "No valid windowIndex");
        return 0;
    }
    if (!action[0]) {
        _snprintf(msg, msz, "missing action");
        return 0;
    }
    int x = jget_int(body, "x", 0), y = jget_int(body, "y", 0);

    if (!_stricmp(action, "click") || !_stricmp(action, "rightclick")) {
        int mask = !_stricmp(action, "click") ? 1 : 4;
        for (int i = 0; i < nt; i++) {
            int px, py;
            if (!api_point(targets[i], x, y, 0, &px, &py)) continue;
            api_cancel_drag(targets[i]);
            be_pointer(targets[i], mask, px, py);
            Sleep(15);
            be_pointer(targets[i], 0, px, py);
        }
    } else if (!_stricmp(action, "release")) {
        for (int i = 0; i < nt; i++) {
            int px, py;
            api_point(targets[i], x, y, 1, &px, &py);
            api_cancel_drag(targets[i]);
            be_pointer(targets[i], 0, px, py);
        }
    } else if (!_stricmp(action, "drag")) {
        int fx = jget_int(body, "fromX", 0), fy = jget_int(body, "fromY", 0);
        int txx = jget_int(body, "toX", fx),  tyy = jget_int(body, "toY", fy);
        int dur = jget_int(body, "duration", 300);
        int hold = jget_int(body, "hold", 0);
        char mode[16]; mode[0] = 0;
        jget_str(body, "mode", mode, sizeof(mode));
        int ease = !_stricmp(mode, "ease");
        for (int i = 0; i < nt; i++)
            api_start_drag(targets[i], fx, fy, txx, tyy, dur, hold, ease);
    } else if (!_stricmp(action, "scroll")) {
        int dy = jget_int(body, "deltaY", 0), dx = jget_int(body, "deltaX", 0);
        int cnt = abs(dy) ? abs(dy) : abs(dx);
        if (cnt < 1) cnt = 1;
        if (cnt > 10) cnt = 10;
        int dir = (dy > 0 || dx > 0) ? -1 : 1;
        for (int i = 0; i < nt; i++) {
            int px, py;
            if (!api_point(targets[i], x, y, 1, &px, &py)) continue;
            char b[96];
            int l = _snprintf(b, sizeof(b), "W %d %d %d %d", dir, cnt, px, py);
            be_cmd(targets[i], b, l);
        }
    } else if (!_stricmp(action, "keypress")) {
        char code[64]; code[0] = 0;
        jget_str(body, "code", code, sizeof(code));
        unsigned int sym = api_keysym(code);
        if (!sym) { _snprintf(msg, msz, "unknown key '%s'", code); return 0; }
        int down = -1;
        const char *dv = jval(body, "down");
        if (dv && !_strnicmp(dv, "true", 4)) down = 1;
        else if (dv && !_strnicmp(dv, "false", 5)) down = 0;
        for (int i = 0; i < nt; i++) {
            char b[48];
            int l;
            if (down == -1) {
                l = _snprintf(b, sizeof(b), "K %u 1", sym); be_cmd(targets[i], b, l);
                Sleep(15);
                l = _snprintf(b, sizeof(b), "K %u 0", sym); be_cmd(targets[i], b, l);
            } else {
                l = _snprintf(b, sizeof(b), "K %u %d", sym, down); be_cmd(targets[i], b, l);
            }
        }
    } else if (!_stricmp(action, "clipboard")) {
        char text[2048]; text[0] = 0;
        if (!jget_str(body, "text", text, sizeof(text))) text[0] = 0;
        for (int i = 0; i < nt; i++) {
            char b[2304];
            int l = _snprintf(b, sizeof(b), "X %s", text);
            be_cmd(targets[i], b, l);
        }
    } else {
        _snprintf(msg, msz, "unknown action '%s'", action);
        return 0;
    }
    {
        char list[32];
        int o = 0;
        for (int i = 0; i < nt; i++)
            o += _snprintf(list + o, sizeof(list) - o, "%s%d", i ? "," : "", targets[i] + 1);
        _snprintf(msg, msz, "Sent to window %s", list);
    }
    return 1;
}

static const char *strcasestr_portable(const char *h, const char *n) {
    size_t nl = strlen(n);
    for (; *h; h++)
        if (!_strnicmp(h, n, nl)) return h;
    return NULL;
}

static DWORD WINAPI http_client_thread(LPVOID p) {
    SOCKET c = (SOCKET)(intptr_t) p;
    char req[16384];
    int rl = 0, body_start = -1;
    /* read until header end or full */
    while (rl < (int) sizeof(req) - 1) {
        int r = recv(c, req + rl, (int) sizeof(req) - 1 - rl, 0);
        if (r <= 0) break;
        rl += r;
        req[rl] = 0;
        char *hd = strstr(req, "\r\n\r\n");
        if (hd) {
            body_start = (int)(hd - req) + 4;
            /* check content-length satisfied */
            int clen = 0;
            const char *clp = strcasestr_portable(req, "content-length:");
            if (clp) clen = atoi(clp + 15);
            if (rl - body_start >= clen) break;
        }
    }
    req[rl] = 0;
    if (body_start < 0) { closesocket(c); return 0; }
    char method[8] = {0}, path[256] = {0};
    sscanf(req, "%7s %255s", method, path);
    const char *body = req + body_start;

    #define RESP(CT, JSON) { \
        char h[512]; int hl = _snprintf(h, sizeof(h), \
            "HTTP/1.1 200 OK\r\nAccess-Control-Allow-Origin: *\r\n" \
            "Access-Control-Allow-Methods: GET, POST, OPTIONS\r\n" \
            "Access-Control-Allow-Headers: Content-Type\r\n" \
            "Content-Type: " CT "\r\nContent-Length: %d\r\n\r\n", (int) strlen(JSON)); \
        send(c, h, hl, 0); send(c, JSON, (int) strlen(JSON), 0); }

    if (!strcmp(method, "OPTIONS")) {
        RESP("application/json", "{}");
    } else if (!strcmp(method, "GET") && !strncmp(path, "/status", 7)) {
        int port = 38980 + ((g_cfg_active && g_cfg_group >= 0) ? g_cfg_group + 1 : 0);
        char out[512];
        _snprintf(out, sizeof(out),
                  "{\"success\":true,\"windowCount\":%d,\"control\":true,"
                  "\"master\":-1,\"sync\":false,\"portrait\":false,"
                  "\"clientWidth\":%d,\"clientHeight\":%d,\"port\":%d}",
                  g_nslots, API_W, API_H, port);
        RESP("application/json; charset=utf-8", out);
    } else if (!strcmp(method, "GET") && !strncmp(path, "/windows", 8)) {
        char w[2048];
        int o = _snprintf(w, sizeof(w), "{\"success\":true,");
        if (g_cfg_active && g_cfg_group >= 0)
            o += _snprintf(w + o, sizeof(w) - o,
                           "\"groupIndex\":%d,\"groupName\":\"%ls\",", g_cfg_group + 1, g_cfg_gname[g_cfg_group]);
        else
            o += _snprintf(w + o, sizeof(w) - o, "\"groupIndex\":null,\"groupName\":null,");
        o += _snprintf(w + o, sizeof(w) - o, "\"windowCount\":%d,\"windows\":[", g_nslots);
        for (int i = 0; i < g_nslots && i < MAXSLOTS; i++) {
            int gi = (g_cfg_active && g_cfg_group >= 0) ? g_cfg_group * 5 + i : -1;
            char esc[260];
            if (gi >= 0)
                WideCharToMultiByte(CP_UTF8, 0, g_cfg_title[gi], -1, esc, sizeof(esc) - 1, NULL, NULL);
            else
                _snprintf(esc, sizeof(esc), "UxPlay-%d", i + 1);
            o += _snprintf(w + o, sizeof(w) - o,
                           "%s{\"index\":%d,\"title\":\"%s\",\"controlIP\":\"%s\",\"alive\":%s}",
                           i ? "," : "",
                           gi >= 0 ? gi + 1 : i + 1,
                           esc,
                           g_slot[i].used ? g_slot[i].ip : "",
                           g_slot[i].used ? "true" : "false");
        }
        _snprintf(w + o, sizeof(w) - o, "]}");
        RESP("application/json; charset=utf-8", w);
    } else if (!strcmp(method, "POST") && !strncmp(path, "/exit", 5)) {
        RESP("application/json", "{\"ok\":true}");
        shutdown(c, SD_BOTH);
        closesocket(c);
        Sleep(150);
        router_kill_children();
        Shell_NotifyIconW(NIM_DELETE, &g_nid);
        ExitProcess(0);
        return 0;
    } else if (!strcmp(method, "POST") && !strncmp(path, "/refresh", 8)) {
        RESP("application/json", "{\"ok\":true}");
    } else if (!strcmp(method, "GET") && !strncmp(path, "/panels", 7)) {
        char w[4096];
        int o = _snprintf(w, sizeof(w), "{\"success\":true,\"panels\":[");
        for (int i = 0; i < g_nslots && i < MAXSLOTS; i++) {
            RECT rc2 = {0, 0, 0, 0};
            HWND ph = g_strip[i];
            if (ph && IsWindow(ph)) GetWindowRect(ph, &rc2);
            int gi = (g_cfg_active && g_cfg_group >= 0) ? g_cfg_group * 5 + i : -1;
            char esc[260];
            if (gi >= 0)
                WideCharToMultiByte(CP_UTF8, 0, g_cfg_title[gi], -1, esc, sizeof(esc) - 1, NULL, NULL);
            else
                _snprintf(esc, sizeof(esc), "UxPlay-%d", i + 1);
            int cell = (g_gi_of[i] >= 0) ? g_cell_of[i] : i;
            o += _snprintf(w + o, sizeof(w) - o,
                           "%s{\"index\":%d,\"cell\":%d,\"title\":\"%s\",\"hwnd\":%lu,"
                           "\"x\":%ld,\"y\":%ld,\"w\":%ld,\"h\":%ld}",
                           i ? "," : "", gi >= 0 ? gi + 1 : i + 1, cell + 1, esc,
                           (unsigned long) (uintptr_t) ph,
                           rc2.left, rc2.top, rc2.right - rc2.left, rc2.bottom - rc2.top);
        }
        _snprintf(w + o, sizeof(w) - o, "]}");
        RESP("application/json; charset=utf-8", w);
    } else if (!strcmp(method, "GET") && !strncmp(path, "/diag", 5)) {
        char out[256];
        _snprintf(out, sizeof(out),
                  "{\"canvasW\":%d,\"canvasH\":%d,\"rectW\":%ld,\"rectH\":%ld}",
                  API_W, API_H, g_cellw, g_cellh);
        RESP("application/json", out);
    } else if (!strcmp(method, "POST")) {
        char msg[128];
        int ok = api_handle_control(body, msg, sizeof(msg));
        char out[256];
        _snprintf(out, sizeof(out), ok ? "{\"success\":true,\"message\":\"%s\"}"
                                       : "{\"success\":false,\"error\":\"%s\"}", msg);
        RESP("application/json; charset=utf-8", out);
    } else {
        const char *nf = "Not Found";
        char h[128];
        int hl = _snprintf(h, sizeof(h), "HTTP/1.1 404 Not Found\r\nContent-Length: %d\r\n\r\n", (int) strlen(nf));
        send(c, h, hl, 0);
        send(c, nf, (int) strlen(nf), 0);
    }
    shutdown(c, SD_BOTH);
    closesocket(c);
    return 0;
}

static DWORD WINAPI http_server_thread(LPVOID p) {
    (void) p;
    int port = 38980 + ((g_cfg_active && g_cfg_group >= 0) ? g_cfg_group + 1 : 0);
    SOCKET l = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    SOCKADDR_IN a; memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    a.sin_port = htons((unsigned short) port);
    BOOL ru = TRUE;
    setsockopt(l, SOL_SOCKET, SO_REUSEADDR, (char *) &ru, sizeof(ru));
    if (bind(l, (SOCKADDR *) &a, sizeof(a)) != 0 || listen(l, 8) != 0) {
        fprintf(stderr, "router: http api bind %d failed (%d)\n", port, WSAGetLastError());
        return 0;
    }
    fprintf(stderr, "router: http api on http://0.0.0.0:%d\n", port);
    for (;;) {
        SOCKET c = accept(l, NULL, NULL);
        if (c == INVALID_SOCKET) continue;
        CreateThread(NULL, 0, http_client_thread, (LPVOID)(intptr_t) c, 0, NULL);
    }
    return 0;
}
/* ------------------------------------------------------------------ */

/* ---------------- device-name matching (config mode) ---------------- */

static char g_exedir[MAX_PATH] = {0};
static char g_svcbase[64] = "UxPlay";
static int g_debug = 0;   /* UXPLAY_DEBUG=1: console + per-backend be*.log */
static FILE *g_dbgf = NULL;

static void rlog(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    if (g_dbgf) {
        va_start(ap, fmt);
        vfprintf(g_dbgf, fmt, ap);
        va_end(ap);
        fflush(g_dbgf);
    }
}

/* reject: device not in selected group -> kill & respawn its backend */
static void router_reject_slot(int s) {
    rlog("router: REJECTED slot %d device '%ls' (not in group)\n", s + 1, g_wname[s]);
    { char nb[300]; WideCharToMultiByte(CP_UTF8, 0, g_wname[s], -1, nb, sizeof(nb), NULL, NULL);
      rlog("router:   raw reported name utf8: %s (len %d)\n", nb, (int) wcslen(g_wname[s]));
      rlog("router:   expected titles %d..%d e.g. '%ls'\n", g_cfg_group * 5 + 1,
           g_cfg_group * 5 + 5, g_cfg_title[g_cfg_group * 5]); }
    /* 托盘气泡提示: 设备不在当前组, 连接被拒绝 (不再是无声秒断) */
    {
        wchar_t ttl[64], msg[256];
        _snwprintf(ttl, 64, L"UxPlay Router - 拒绝连接");
        _snwprintf(msg, 256,
                   L"设备 '%ls' 不属于当前组，已断开连接。\n请检查 groups.ini 中该设备的窗口标题是否配置正确。",
                   g_wname[s]);
        tray_notify(ttl, msg);
    }
    if (g_child[s] && WaitForSingleObject(g_child[s], 0) == WAIT_TIMEOUT) {
        TerminateProcess(g_child[s], 0);
        WaitForSingleObject(g_child[s], 2000);
    }
    if (g_strip[s]) { panel_clear_slot(s); g_strip[s] = NULL; }
    EnterCriticalSection(&g_lock);
    memset(&g_slot[s], 0, sizeof(slot_t));
    LeaveCriticalSection(&g_lock);
    g_wname[s][0] = 0;
    g_gi_of[s] = -1;
    g_dimw[s] = 0;
    g_dimh[s] = 0;
    g_vrect_set[s] = 0;
    g_cell_of[s] = s;
    spawn_backend(s, g_exedir, g_svcbase);   /* fresh backend for the next candidate */
}

/* persistent device->window binding for dynamic (no-config) mode:
   HKCU\Software\UxPlayGroup\<utf8 name> = cell index */
static int reg_get_cell(const wchar_t *wname) {
    char nm[256];
    WideCharToMultiByte(CP_UTF8, 0, wname, -1, nm, sizeof(nm), NULL, NULL);
    HKEY k;
    if (RegOpenKeyExA(HKEY_CURRENT_USER, "Software\\UxPlayGroup", 0, KEY_READ, &k) != ERROR_SUCCESS)
        return -1;
    DWORD v = 0xFFFFFFFF, sz = sizeof(v), t;
    LSTATUS r = RegQueryValueExA(k, nm, NULL, &t, (BYTE *) &v, &sz);
    RegCloseKey(k);
    if (r != ERROR_SUCCESS || t != REG_DWORD || v > 9) return -1;
    return (int) v;
}

static void reg_set_cell(const wchar_t *wname, int cell) {
    char nm[256];
    WideCharToMultiByte(CP_UTF8, 0, wname, -1, nm, sizeof(nm), NULL, NULL);
    HKEY k;
    if (RegCreateKeyExA(HKEY_CURRENT_USER, "Software\\UxPlayGroup", 0, NULL,
                        0, KEY_WRITE, NULL, &k, NULL) != ERROR_SUCCESS)
        return;
    DWORD v = (DWORD) cell;
    RegSetValueExA(k, nm, 0, REG_DWORD, (const BYTE *) &v, sizeof(v));
    RegCloseKey(k);
}

static int g_claimed[MAXSLOTS];   /* dynamic mode: cells taken this session */

static void cfg_squash(const wchar_t *in, wchar_t *out) {
    int o = 0;
    for (int i = 0; in[i] && o < 128; i++) {
        wchar_t c = in[i];
        if (c == L' ' || c == L'\t' || c == L'\u00a0' || c == L'\u3000') continue;
        out[o++] = towlower(c);
    }
    out[o] = 0;
}

static void router_apply_name(int s, const wchar_t *name) {
    _snwprintf(g_wname[s], 128, L"%s", name);
    g_wname[s][127] = 0;
    if (!name[0]) {
        fprintf(stderr, "router: slot %d reported EMPTY device name\n", s + 1);
        return;
    }
    if (!(g_cfg_active && g_cfg_group >= 0)) {
        /* dynamic mode: keep every device on the window it used before
           (persisted in registry); new devices claim a free window */
        int cell = reg_get_cell(name);
        if (cell < 0 || cell >= MAXSLOTS) {
            cell = -1;
            for (int c = 0; c < g_nslots && c < MAXSLOTS; c++)
                if (!g_claimed[c]) { cell = c; break; }
            if (cell < 0) cell = s % MAXSLOTS;
        }
        g_claimed[cell] = 1;
        g_gi_of[s] = -1;
        g_cell_of[s] = cell;
        reg_set_cell(name, cell);
        rlog("router: NAME '%ls' ip %s slot %d -> window %d (dynamic)\n", name,
             g_slot[s].ip, s + 1, cell + 1);
        notify_backend_ip(s);
        return;
    }
    /* find this device among ALL configured titles.
       exact match first, then whitespace-insensitive fallback — devices
       often carry stray/invisible spaces in their advertised name */
    int hit = -1;
    for (int gi = 0; gi < g_cfg_count && gi < 32; gi++)
        if (!_wcsicmp(g_cfg_title[gi], name)) { hit = gi; break; }
    if (hit < 0) {
        wchar_t sq1[130], sq2[130];
        cfg_squash(name, sq1);
        for (int gi = 0; gi < g_cfg_count && gi < 32; gi++) {
            cfg_squash(g_cfg_title[gi], sq2);
            if (!wcscmp(sq1, sq2)) { hit = gi; break; }
        }
    }
    if (hit >= 0 && !(hit >= g_cfg_group * 5 && hit < g_cfg_group * 5 + 5))
        fprintf(stderr, "router: '%ls' belongs to another group (#%d)\n", name, hit / 5 + 1);
    int g0 = g_cfg_group * 5;
    if (hit >= g0 && hit < g0 + 5) {
        /* evict STALE sessions of the SAME device lingering on other slots
           (e.g. it moved from usb to wifi). Never touch live sessions:
           two devices reporting the same name is a config problem — kicking
           active ones creates an endless evict ping-pong. */
        for (int t = 0; t < MAXSLOTS; t++) {
            if (t == s || !g_child[t]) continue;
            if (g_slot[s].ip[0] && !_stricmp(g_slot[t].ip, g_slot[s].ip)) continue;
            if (g_wname[t][0] && !_wcsicmp(g_wname[t], name)) {
                EnterCriticalSection(&g_lock);
                int busy = g_slot[t].conns > 0 ||
                           (time(NULL) - g_slot[t].last_active) < 20;
                LeaveCriticalSection(&g_lock);
                if (busy) {
                    fprintf(stderr, "router: WARNING duplicate name '%ls': slot %d (%s) vs slot %d (%s) -- check these devices!\n",
                            name, t + 1, g_slot[t].ip, s + 1, g_slot[s].ip);
                    continue;
                }
                rlog("router: EVICT stale slot %d (%s) of '%ls' claimed by slot %d (%s)\n",
                          t + 1, g_slot[t].ip, name, s + 1, g_slot[s].ip);
                TerminateProcess(g_child[t], 0);
                WaitForSingleObject(g_child[t], 2000);
                if (g_strip[t]) { panel_clear_slot(t); g_strip[t] = NULL; }
                memset(&g_slot[t], 0, sizeof(slot_t));
                g_wname[t][0] = 0; g_gi_of[t] = -1; g_dimw[t] = 0; g_dimh[t] = 0;
                g_vrect_set[t] = 0; g_cell_of[t] = t;
                spawn_backend(t, g_exedir, g_svcbase);
            }
        }
        g_gi_of[s] = hit;
        g_cell_of[s] = hit - g0;
        rlog("router: NAME '%ls' ip %s slot %d -> window %d (config)\n", name,
             g_slot[s].ip, s + 1, hit + 1);
        notify_backend_ip(s);
    } else {
        router_reject_slot(s);
    }
}

static DWORD WINAPI name_server_thread(LPVOID p) {
    (void) p;
    SOCKET rs[MAXSLOTS];
    int n = 0;
    for (int i = 0; i < MAXSLOTS; i++) {
        SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        SOCKADDR_IN a; memset(&a, 0, sizeof(a));
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        a.sin_port = htons((unsigned short)(BE_BASE() + (i + 1) * 10 + 70));
        if (s != INVALID_SOCKET && bind(s, (SOCKADDR *) &a, sizeof(a)) == 0)
            rs[n++] = s;
        else if (s != INVALID_SOCKET) closesocket(s);
    }
    if (!n) return 0;
    for (;;) {
        fd_set rd;
        FD_ZERO(&rd);
        SOCKET mx = 0;
        for (int i = 0; i < n; i++) { FD_SET(rs[i], &rd); if (rs[i] > mx) mx = rs[i]; }
        struct timeval tv = {1, 0};
        int r = select((int) mx + 1, &rd, NULL, NULL, &tv);
        if (r <= 0) continue;
        for (int i = 0; i < n; i++) {
            if (!FD_ISSET(rs[i], &rd)) continue;
            char b[512];
            int rl = recv(rs[i], b, sizeof(b) - 1, 0);
            if (rl <= 0) continue;
            b[rl] = 0;
            if (!strncmp(b, "dim ", 4)) {
                char *ip2 = b + 4, *sp2 = strchr(ip2, ' ');
                if (sp2) {
                    *sp2 = 0;
                    int dw = atoi(sp2 + 1);
                    char *sp3 = strchr(sp2 + 1, ' ');
                    int dh = sp3 ? atoi(sp3 + 1) : 0;
                    for (int s = 0; s < MAXSLOTS; s++)
                        if (g_slot[s].used && !_stricmp(g_slot[s].ip, ip2)) {
                            g_dimw[s] = dw; g_dimh[s] = dh;
                            break;
                        }
                }
                continue;
            }
            /* "aname <ip> <utf8 name>" — identity from the AirPlay layer
               (the real iPhone name); overrides the TrollVNC desktop name */
            const char *key = NULL;
            if (!strncmp(b, "aname ", 6)) key = b + 6;
            else if (!strncmp(b, "name ", 5)) key = b + 5;
            if (!key) continue;
            char *sp = strchr(key, ' ');
            if (!sp) continue;
            *sp = 0;
            char *ip = (char *) key, *nm = sp + 1;
            wchar_t wnm[128];
            MultiByteToWideChar(CP_UTF8, 0, nm, -1, wnm, 128);
            wnm[127] = 0;
            for (int s = 0; s < MAXSLOTS; s++) {
                if (g_slot[s].used && !_stricmp(g_slot[s].ip, ip)) {
                    router_apply_name(s, wnm);
                    break;
                }
            }
        }
    }
    return 0;
}
/* ------------------------------------------------------------------ */

static void spawn_backend(int i, const char *exedir, const char *name) {
    unsigned char mac[6]; mac[0] = 0x02;
    for (int j = 1; j < 6; j++) mac[j] = rand() & 0xFF;
    int base = BE_BASE() + (i + 1) * 10;
    snprintf(g_be_name[i], 64, "%s-%d", name, i + 1);
    char cmd[512], macstr[32];
    snprintf(macstr, 32, "%02x:%02x:%02x:%02x:%02x:%02x", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    const char *sink = getenv("UXPLAY_SINK");
    char sinkarg[160];
#ifndef DEF_SINK
#define DEF_SINK "d3dvideosink"
#endif
    _snprintf(sinkarg, sizeof(sinkarg), " -vs \"%s force-aspect-ratio=false\"",
              (sink && *sink) ? sink : DEF_SINK);
    snprintf(cmd, 512, "\"%suxplay.exe\" -n \"%s\" -nh -m %s -p tcp %d,%d,%d -p udp %d,%d,%d%s%s%s%s%s",
             exedir, g_be_name[i], macstr,
             base + be_tcp_off[2], base + be_tcp_off[0], base + be_tcp_off[1],
             base + be_udp_off[2], base + be_udp_off[1], base + be_udp_off[0],
             " -s " GRID_S, g_geom, g_fullscreen ? " -fs" : "",
             g_with_audio ? "" : " -a -vsync no",
             sinkarg);
    STARTUPINFOA si; PROCESS_INFORMATION pi;
    ZeroMemory(&si, sizeof(si)); si.cb = sizeof(si);
    ZeroMemory(&pi, sizeof(pi));
    HANDLE logh = INVALID_HANDLE_VALUE;
    char logfile[MAX_PATH];
    DWORD flags = HIGH_PRIORITY_CLASS | CREATE_NO_WINDOW;
    if (g_debug) {
        _snprintf(logfile, sizeof(logfile), "%sbe%d.log", exedir, i + 1);
        logfile[sizeof(logfile) - 1] = 0;
        SECURITY_ATTRIBUTES sa = { sizeof(sa), NULL, TRUE };   /* inheritable! */
        logh = CreateFileA(logfile, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           &sa, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        if (logh != INVALID_HANDLE_VALUE) {
            si.dwFlags |= STARTF_USESTDHANDLES;
            si.hStdOutput = logh;
            si.hStdError = logh;
            si.hStdInput = NULL;
            /* no -d: keep logs readable; add UXPLAY_VVERBOSE later if needed */
        }
    }
    char *env = build_child_env(base);
    BOOL ok = CreateProcessA(NULL, cmd, NULL, NULL,
                             logh != INVALID_HANDLE_VALUE ? TRUE : FALSE,
                             flags, env, NULL, &si, &pi);
    free(env);
    if (logh != INVALID_HANDLE_VALUE) CloseHandle(logh);
    if (!ok) {
        fprintf(stderr, "router: cannot spawn uxplay.exe (error %ld)\n", GetLastError()); exit(1);
    }
    CloseHandle(pi.hThread);
    g_child[i] = pi.hProcess;
    g_child_pid[i] = pi.dwProcessId;
    printf("router: started backend \"%s\" on ports tcp %d/%d/%d udp %d/%d/%d\n",
           g_be_name[i], base + 2, base, base + 1, base + 5, base + 4, base + 3);
}

int main(int argc, char **argv) {
    {
        const char *dp = getenv("UXPLAY_DEBUG");
        g_debug = dp && *dp && *dp != '0';
    }
    if (g_debug) {
        char lp[MAX_PATH], exe[MAX_PATH];
        GetModuleFileNameA(NULL, exe, MAX_PATH);
        strcpy(lp, exe); strrchr(lp, '\\')[1] = 0;
        strcat(lp, "router_debug.log");
        g_dbgf = fopen(lp, "a");
        if (g_dbgf) setvbuf(g_dbgf, NULL, _IONBF, 0);
    }
    if (g_debug && !GetConsoleWindow()) AllocConsole();
    /* GUI subsystem: no console; logs go to router.log ONLY when UXPLAY_LOG=1 */
    if (!GetConsoleWindow() && getenv("UXPLAY_LOG")) {
        char lp[MAX_PATH], exe[MAX_PATH];
        GetModuleFileNameA(NULL, exe, MAX_PATH);
        strcpy(lp, exe); strrchr(lp, '\\')[1] = 0;
        strcat(lp, "router.log");
        int fd = _open(lp, _O_WRONLY | _O_CREAT | _O_TRUNC, 0644);
        if (fd >= 0) {
            fflush(stdout); fflush(stderr);
            _dup2(fd, _fileno(stdout));
            _dup2(fd, _fileno(stderr));
            _close(fd);
            setvbuf(stdout, NULL, _IONBF, 0);
            setvbuf(stderr, NULL, _IONBF, 0);
        }
    }
    WSADATA wsa; WSAStartup(MAKEWORD(2, 2), &wsa);
    SetProcessDPIAware();
    {
        const char *vp = getenv("VNC_PORT");
        if (vp && *vp) g_vnc_port = atoi(vp);
        g_vnc_rot = getenv("VNC_ROT");
    }
    {
        const char *sh2 = getenv("UXPLAY_STATUS_H");
        if (sh2 && *sh2) {
            int v2 = atoi(sh2);
            if (v2 < 20) v2 = 20;
            if (v2 > 400) v2 = 400;
            g_strip_h = v2;
        }
    }
    for (int k = 0; k < MAXSLOTS; k++) {
        InitializeCriticalSection(&g_vnc[k].cs);
    }
    InitializeCriticalSection(&g_lock);
    panel_start();

    const char *pos[2] = {NULL, NULL};
    const char *name_override = NULL;
    const char *cfg_path = NULL;
    int posn = 0, sflag = 0;
    for (int a = 1; a < argc; a++) {
        if (sflag) { g_nslots = atoi(argv[a]); sflag = 0; continue; }
        if (!strcmp(argv[a], "-s")) { sflag = 1; continue; }
        if (!strcmp(argv[a], "-audio")) { g_with_audio = 1; continue; }
        if (!strcmp(argv[a], "-config") && a + 1 < argc) { cfg_path = argv[++a]; continue; }
        if (!strcmp(argv[a], "-base") && a + 1 < argc) { g_base = atoi(argv[++a]); continue; }
        if (!strcmp(argv[a], "-fs")) { g_fullscreen = 1; continue; }
        if (!strcmp(argv[a], "-size") && a + 1 < argc) {
            snprintf(g_geom, sizeof(g_geom), " -s %s", argv[++a]);
            continue;
        }
        if (!strncmp(argv[a], "-s", 2)) { g_nslots = atoi(argv[a] + 2); continue; }
        {
            size_t L = strlen(argv[a]);
            if (L > 4 && !_stricmp(argv[a] + L - 4, ".ini")) { cfg_path = argv[a]; continue; }
        }
        if (posn < 2) pos[posn++] = argv[a];
    }
    if (posn > 0 && atoi(pos[0]) > 0) g_nslots = atoi(pos[0]);
    else if (posn == 1) { name_override = pos[0]; }
    if (posn > 1 && !name_override) name_override = pos[1];
    else if (posn == 1 && atoi(pos[0]) <= 0) name_override = pos[0];
    if (g_nslots < 1) g_nslots = 1;
    if (g_nslots < 1) g_nslots = 1;
    if (g_nslots > MAXSLOTS) g_nslots = MAXSLOTS;

    /* group config mode: load ini, ask user for a group, lock slots
       (only when -config / *.ini argument is given; bare launch = default mode) */
    if (cfg_path) {
        if (load_config(cfg_path)) {
            int pick = pick_group_dialog();
            if (pick >= 0 && pick < g_cfg_ngroups) {
                char gn[96];
                g_cfg_group = pick;
                g_cfg_active = 1;
                g_nslots = 5;
                WideCharToMultiByte(CP_UTF8, 0, g_cfg_gname[pick], -1, gn, sizeof(gn) - 8, NULL, NULL);
                _snprintf(g_svc_name, sizeof(g_svc_name), "AirPlay %s", gn);
                {
                    wchar_t tip[128];
                    _snwprintf(tip, 128, L"AirPlay 群控 - 控制%ls组", g_cfg_gname[pick]);
                    tray_set_tip(tip);
                }
                printf("router: group selected: %ls (devices %d-%d)\n",
                       g_cfg_gname[pick], pick * 5 + 1, pick * 5 + 5);
            } else {
                printf("router: no group selected, running in default mode\n");
            }
        } else {
            MessageBoxW(NULL,
                L"groups.ini 解析失败！\n需要：组数量>=1 且 窗口标题>=5 条",
                L"UxPlay Router", MB_OK | MB_ICONERROR);
        }
    }

    const char *name = name_override ? name_override
                     : (g_cfg_active && g_svc_name[0]) ? g_svc_name : "UxPlay";

    SetEnvironmentVariable("UXPLAY_HIDDEN", "1");
    SetEnvironmentVariable("UXPLAY_LOOPBACK", "1");

    /* spawn hidden backends */
    char exedir[MAX_PATH], exe[MAX_PATH];
    GetModuleFileNameA(NULL, exe, MAX_PATH);
    strcpy(exedir, exe); strrchr(exedir, '\\')[1] = 0;
    {
        /* 定位随包分发的 GStreamer 插件目录: 兼容扁平布局(gstreamer-1.0)
           与 MSYS2 布局(lib\gstreamer-1.0), 找到哪个用哪个, 避免后端
           uxplay.exe 因找不到插件而退出(表现为 backend not reachable) */
        char gstpath[MAX_PATH]; gstpath[0] = 0;
        const char *cands[] = { "gstreamer-1.0", "lib\\gstreamer-1.0" };
        int k;
        for (k = 0; k < (int)(sizeof(cands) / sizeof(cands[0])); k++) {
            char tmp[MAX_PATH];
            snprintf(tmp, sizeof(tmp), "%s%s", exedir, cands[k]);
            DWORD attr = GetFileAttributesA(tmp);
            if (attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY)) {
                snprintf(gstpath, sizeof(gstpath), "%s", tmp);
                break;
            }
        }
        if (gstpath[0]) {
            /* 追加到已有 GST_PLUGIN_PATH(start.bat 或用户环境已设则不覆盖) */
            char cur[8192]; cur[0] = 0;
            GetEnvironmentVariableA("GST_PLUGIN_PATH", cur, sizeof(cur));
            char nb[16384];
            if (cur[0]) snprintf(nb, sizeof(nb), "%s;%s", gstpath, cur);
            else snprintf(nb, sizeof(nb), "%s", gstpath);
            SetEnvironmentVariable("GST_PLUGIN_PATH", nb);
            /* 插件扫描器同样指向随包目录 */
            char sc[MAX_PATH];
            snprintf(sc, sizeof(sc), "%s\\gst-plugin-scanner.exe", gstpath);
            if (GetFileAttributesA(sc) != INVALID_FILE_ATTRIBUTES)
                SetEnvironmentVariable("GST_PLUGIN_SCANNER", sc);
        }
        char pbuf[4096]; pbuf[0] = 0;
        GetEnvironmentVariableA("PATH", pbuf, sizeof(pbuf));
        char nbuf[8192];
        snprintf(nbuf, sizeof(nbuf), "%s;%s", exedir, pbuf);
        SetEnvironmentVariable("PATH", nbuf);
    }
    srand(GetTickCount());
    int i;
    for (i = 0; i < MAXSLOTS; i++) { g_cell_of[i] = i; g_gi_of[i] = -1; }
    _snprintf(g_svcbase, sizeof(g_svcbase), "%s", name);
    _snprintf(g_exedir, sizeof(g_exedir), "%s", exedir);
    for (i = 0; i < g_nslots; i++) spawn_backend(i, exedir, name);

    /* advertise the single AirPlay service via UxPlay's mdnsd */
    unsigned char hw[6] = {0x02};
    for (i = 1; i < 6; i++) hw[i] = rand() & 0xFF;
    int err = 0;
    dnssd_t *dnssd = dnssd_init(name, (int)strlen(name), (char *)hw, 6, 0, &err);
    if (!dnssd) {
        wchar_t em[128];
        _snwprintf(em, 128, L"dnssd_init 失败 (err=%d)", err);
        MessageBoxW(NULL, em, L"UxPlay Router", MB_OK | MB_ICONERROR);
        exit(1);
    }
    dnssd_set_airplay_features(dnssd,  0, 0); dnssd_set_airplay_features(dnssd,  1, 1);
    dnssd_set_airplay_features(dnssd,  2, 1); dnssd_set_airplay_features(dnssd,  3, 0);
    dnssd_set_airplay_features(dnssd,  4, 0); dnssd_set_airplay_features(dnssd,  5, 1);
    dnssd_set_airplay_features(dnssd,  6, 1); dnssd_set_airplay_features(dnssd,  7, 1);
    dnssd_set_airplay_features(dnssd,  8, 0); dnssd_set_airplay_features(dnssd,  9, 1);
    dnssd_set_airplay_features(dnssd, 10, 1); dnssd_set_airplay_features(dnssd, 11, 1);
    dnssd_set_airplay_features(dnssd, 12, 1); dnssd_set_airplay_features(dnssd, 13, 1);
    dnssd_set_airplay_features(dnssd, 14, 1); dnssd_set_airplay_features(dnssd, 15, 1);
    dnssd_set_airplay_features(dnssd, 16, 1); dnssd_set_airplay_features(dnssd, 17, 1);
    dnssd_set_airplay_features(dnssd, 18, 1); dnssd_set_airplay_features(dnssd, 19, 1);
    dnssd_set_airplay_features(dnssd, 20, 1); dnssd_set_airplay_features(dnssd, 21, 1);
    dnssd_set_airplay_features(dnssd, 22, 1); dnssd_set_airplay_features(dnssd, 23, 0);
    dnssd_set_airplay_features(dnssd, 24, 0); dnssd_set_airplay_features(dnssd, 25, 1);
    dnssd_set_airplay_features(dnssd, 26, 0); dnssd_set_airplay_features(dnssd, 27, 1);
    dnssd_set_airplay_features(dnssd, 28, 1); dnssd_set_airplay_features(dnssd, 29, 0);
    dnssd_set_airplay_features(dnssd, 30, 1); dnssd_set_airplay_features(dnssd, 31, 0);
    if (dnssd_register_raop(dnssd, 7000 + PUBS()) || dnssd_register_airplay(dnssd, 7000 + PUBS())) {
        MessageBoxW(NULL,
            L"mDNS 注册失败（UDP 5353 绑定失败）\n检查是否有其他程序占用 5353 端口",
            L"UxPlay Router", MB_OK | MB_ICONERROR);
        exit(1);
    }
    printf("router: advertised AirPlay service \"%s\" (ports tcp 7000/7001/7100 udp 6000/6001/7011)\n", name);

    /* show which IPv4 is used for advertising and list alternatives */
    {
        SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        SOCKADDR_IN r; memset(&r, 0, sizeof(r));
        r.sin_family = AF_INET; r.sin_port = htons(5353);
        inet_pton(AF_INET, "224.0.0.251", &r.sin_addr);
        char cur[32] = "?";
        const char *ov = getenv("MDNS_IFACE_IP");
        if (ov && *ov) {
            strncpy(cur, ov, sizeof(cur) - 1); cur[sizeof(cur) - 1] = 0;
        } else if (s != INVALID_SOCKET) {
            if (connect(s, (SOCKADDR *)&r, sizeof(r)) == 0) {
                SOCKADDR_IN l; int ll = sizeof(l);
                if (getsockname(s, (SOCKADDR *)&l, &ll) == 0) {
                    strncpy(cur, inet_ntoa(l.sin_addr), sizeof(cur) - 1);
                }
            }
            closesocket(s);
        }
        printf("router: mDNS advertising via %s\n", cur);
        ULONG sz = 16 * 1024;
        PIP_ADAPTER_ADDRESSES aa = malloc(sz);
        if (GetAdaptersAddresses(AF_INET, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST,
                                 NULL, aa, &sz) == NO_ERROR) {
            for (PIP_ADAPTER_ADDRESSES p = aa; p; p = p->Next) {
                if (p->OperStatus != IfOperStatusUp) continue;
                for (PIP_ADAPTER_UNICAST_ADDRESS u = p->FirstUnicastAddress; u; u = u->Next) {
                    SOCKADDR_IN *sa = (SOCKADDR_IN *)u->Address.lpSockaddr;
                    if (sa->sin_family != AF_INET) continue;
                    char ip[32];
                    strncpy(ip, inet_ntoa(sa->sin_addr), sizeof(ip) - 1);
                    if (strcmp(ip, cur)) {
                        printf("router:   other interface: %-15s (%ls)\n", ip, p->FriendlyName);
                    } else {
                        printf("router:   (interface: %ls)\n", p->FriendlyName);
                    }
                }
            }
            printf("router: NOTE if the advertising address is a virtual adapter (VMware/Hyper-V),\n"
                   "        run:  set MDNS_IFACE_IP=<your real LAN IP>  then start router again\n");
        }
        free(aa);
    }

    CreateThread(NULL, 0, window_mgr_thread, NULL, 0, NULL);
    CreateThread(NULL, 0, http_server_thread, NULL, 0, NULL);
    CreateThread(NULL, 0, name_server_thread, NULL, 0, NULL);

    /* public listeners */
    SOCKET ltcp[PUB_TCP_N], lup[PUB_UDP_N];
    for (i = 0; i < PUB_TCP_N; i++) {
        SOCKET s = socket(AF_INET6, SOCK_STREAM, IPPROTO_TCP);
#ifdef IPV6_V6ONLY
        { int vo = 0; setsockopt(s, IPPROTO_IPV6, IPV6_V6ONLY, (char *)&vo, sizeof(vo)); }
#endif
        SOCKADDR_IN6 a; memset(&a, 0, sizeof(a)); a.sin6_family = AF_INET6; a.sin6_addr = in6addr_any; a.sin6_port = htons(pub_tcp[i] + PUBS());
        BOOL ru = TRUE; setsockopt(s, SOL_SOCKET, SO_REUSEADDR, (char *)&ru, sizeof(ru));
        if (bind(s, (SOCKADDR *)&a, sizeof(a)) || listen(s, 8)) die("tcp bind/listen");
        ltcp[i] = s;
        acc_t *pa = malloc(sizeof(acc_t)); pa->c = s; pa->idx = i; pa->port = be_tcp_off[i];
        HANDLE t = CreateThread(NULL, 0, accept_thread, pa, 0, NULL); CloseHandle(t);
    }
    for (i = 0; i < PUB_UDP_N; i++) {
        SOCKET s = socket(AF_INET6, SOCK_DGRAM, IPPROTO_UDP);
#ifdef IPV6_V6ONLY
        { int vo = 0; setsockopt(s, IPPROTO_IPV6, IPV6_V6ONLY, (char *)&vo, sizeof(vo)); }
#endif
        SOCKADDR_IN6 a; memset(&a, 0, sizeof(a)); a.sin6_family = AF_INET6; a.sin6_addr = in6addr_any; a.sin6_port = htons(pub_udp[i] + PUBS());
        BOOL ru = TRUE; setsockopt(s, SOL_SOCKET, SO_REUSEADDR, (char *)&ru, sizeof(ru));
        if (bind(s, (SOCKADDR *)&a, sizeof(a))) die("udp bind");
        lup[i] = s;
    }

    char buf[65536];
    for (;;) {
        fd_set rs; FD_ZERO(&rs);
        int maxf = 0;
        for (i = 0; i < PUB_UDP_N; i++) { FD_SET(lup[i], &rs); if (lup[i] > maxf) maxf = lup[i]; }
        int p, f;
        for (p = 0; p < PUB_UDP_N; p++)
            for (f = 0; f < MAXFLOWS; f++)
                if (g_flow[p][f].used) { FD_SET(g_flow[p][f].s, &rs); if (g_flow[p][f].s > maxf) maxf = g_flow[p][f].s; }
        struct timeval tv = {2, 0};
        if (select(maxf + 1, &rs, NULL, NULL, &tv) <= 0) {
            /* idle cleanup */
            for (p = 0; p < PUB_UDP_N; p++)
                for (f = 0; f < MAXFLOWS; f++)
                    if (g_flow[p][f].used && time(NULL) - g_flow[p][f].last_active > IDLE_FLOW_FREE) {
                        closesocket(g_flow[p][f].s); g_flow[p][f].used = 0;
                    }
            EnterCriticalSection(&g_lock);
            for (i = 0; i < g_nslots; i++)
                if (g_slot[i].used && g_slot[i].conns == 0 && time(NULL) - g_slot[i].last_active > IDLE_SLOT_FREE) {
                    rlog("router: slot %d released (%s idle)\n", i + 1, g_slot[i].ip);
                    g_slot[i].used = 0;
                    /* clear the identity too — a stale name here would later
                       trigger phantom same-name evictions of live devices */
                    if (g_wname[i][0]) {
                        rlog("router:   cleared stale identity '%ls'\n", g_wname[i]);
                        g_wname[i][0] = 0;
                        g_gi_of[i] = -1;
                        g_dimw[i] = 0; g_dimh[i] = 0;
                        g_vrect_set[i] = 0;
                        g_cell_of[i] = i;
                        if (g_strip[i]) { panel_clear_slot(i); g_strip[i] = NULL; }
                    }
                }
            LeaveCriticalSection(&g_lock);
            continue;
        }
        for (p = 0; p < PUB_UDP_N; p++) {
            if (FD_ISSET(lup[p], &rs)) {
                SOCKADDR_STORAGE cli; int cl = sizeof(cli);
                int n = recvfrom(lup[p], buf, sizeof(buf), 0, (SOCKADDR *)&cli, &cl);
                if (n <= 0) continue;
                char ip[64]; ip[0] = 0;
                { char hb[64];
                  if (getnameinfo((SOCKADDR *)&cli, cl, hb, sizeof(hb), NULL, 0, NI_NUMERICHOST) == 0) {
                      strncpy(ip, hb, sizeof(ip) - 1); ip[sizeof(ip) - 1] = 0;
                      strip_v4map(ip);
                  } }
                int fi = flow_find(g_flow[p], ip);
                if (fi < 0) {
                    slot_touch(ip);
                    int idx = slot_assign(ip);
                    if (idx < 0) { fprintf(stderr, "router: no free slot for UDP from %s\n", ip); continue; }
                    fi = flow_create(g_flow[p], ip, (SOCKADDR *)&cli, cl, BE_BASE() + (idx + 1) * 10 + be_udp_off[p]);
                    if (fi < 0) continue;
                    printf("router: device %s assigned to slot %d (backend %s)\n", ip, idx + 1, g_be_name[idx]);
                } else {
                    slot_touch(ip);
                }
                send(g_flow[p][fi].s, buf, n, 0);
            }
        }
        for (p = 0; p < PUB_UDP_N; p++)
            for (f = 0; f < MAXFLOWS; f++)
                if (g_flow[p][f].used && FD_ISSET(g_flow[p][f].s, &rs)) {
                    int n = recv(g_flow[p][f].s, buf, sizeof(buf), 0);
                    if (n > 0) sendto(lup[p], buf, n, 0, (SOCKADDR *)&g_flow[p][f].cli, g_flow[p][f].clilen);
                    g_flow[p][f].last_active = time(NULL);
                }
    }

    for (i = 0; i < g_nslots; i++) TerminateProcess(g_child[i], 0);
    return 0;
}
