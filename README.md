# UxPlay Multi-Screen Router

Windows 平台 iOS 设备多窗口 AirPlay 投屏方案，基于 [UxPlay](https://github.com/FDH2-UxPlay/UxPlay) 深度定制。

一台 PC 同时接收多台 iPhone/iPad 的 AirPlay 镜像投屏，每个设备画面独立窗口显示，支持 VNC 反向控制、网格布局、HTTP API 控制接口（供易语言等自动化工具调用）。

---

## 项目架构

```
┌──────────────────────────────────────────────────────────────┐
│                    uxplay-router.exe                         │
│  ┌─────────────────────────────────────────────────────────┐ │
│  │  mDNS/Bonjour (dnssd)  ───  广播单一 AirPlay 服务       │ │
│  │  HTTP Server (RTSP)    ───  接收设备连接请求             │ │
│  │  TCP/UDP Port Mapper   ───  按源 IP 分发到后端           │ │
│  │  HTTP API (38980)      ───  外部自动化控制接口           │ │
│  │  VNC Client (TrollVNC) ───  反向触控/滑动事件            │ │
│  │  Grid Layout Engine    ───  3x2 网格窗口布局             │ │
│  │  System Tray Icon      ───  托盘图标管理                 │ │
│  └──────────┬──────────────────────┬───────────────────────┘ │
│             │ fork/spawn           │ shared memory            │
│    ┌────────▼───────┐    ┌────────▼───────┐                  │
│    │ uxplay.exe ×5  │    │ uxplay-panel.exe│                  │
│    │ (后端投屏进程)  │    │ (状态栏窗口)    │                  │
│    │ GStreamer 管线   │    │ 挂载条/标题显示  │                  │
│    │ D3D9 渲染       │    │ 大漠插件兼容     │                  │
│    └────────────────┘    └────────────────┘                  │
└──────────────────────────────────────────────────────────────┘
```

---

## 核心组件

### 1. uxplay-router.exe（路由器主进程）

**源码**: `router/uxplay-router.c` (2193 行)

单一入口程序，集成以下所有功能：

- **mDNS 服务**: 通过 dnssd 广播一个 AirPlay 服务，所有设备连接到同一个入口
- **连接路由**: 按设备源 IP 地址分配到对应的后端 slot，每个 slot 对应一个 `uxplay.exe` 子进程
- **端口映射**: 公共端口 (7000/7001/7100 TCP + 6000/6001/7011 UDP) 映射到后端端口 (slot_base + offset * 10)
- **网格布局**: 3×2 网格，每个 cell ≈853×480，竖屏流居中 pillarbox，横屏流填满 cell
- **VNC 反向控制**: 从 TrollVNC 接收触控/滑动事件，反向注入到对应后端窗口
- **HTTP API**: 监听 `38980 + group` 端口，供易语言客户端控制触摸/滑动/窗口信息
- **系统托盘**: 阴影/置顶/全屏管理、退出菜单
- **进程管理**: spawn/kill 子进程、idle 超时释放 slot

### 2. uxplay.exe（后端投屏进程）

**源码**: `UxPlay/uxplay.cpp` + `UxPlay/renderers/video_renderer.c` + `UxPlay/lib/*.c`

从 [UxPlay](https://github.com/FDH2-UxPlay/UxPlay) 定制，每个进程处理一台设备的 AirPlay 镜像：

- **渲染器**: 默认 D3D9 (`d3dvideosink`)，兼容大漠 dx2/dx3 自动化 hook
- **Pipeline**: `appsrc ! queue ! h264parse ! decoder ! videoflip ! videoconvert ! videoscale ! video/x-raw,width=4096,height=4096 ! d3dvideosink`
- **窗口重命名**: 连接后自动 SetWindowTextA+W 改名为配置的窗口标题
- **旋转检测**: `video_report_size()` 回调检测分辨率变化，设置 `rotation_pending` 标志
- **中文 MessageBox**: 连接/拒绝/驱逐弹窗使用中文显示
- **日志系统**: `LOCALAPPDATA\rotation.log` 记录旋转事件和 GStreamer 错误

### 3. uxplay-panel.exe（面板进程）

**源码**: `router/panel.c` (133 行)

独立进程，通过共享内存 (`UxPlayPanelShm`) 与 router 通信：

- 创建 `WS_POPUP | WS_VISIBLE` 无边框窗口
- `WM_NCHITTEST → HTNOWHERE` 使窗口不响应鼠标事件
- 大漠插件安全：damo 注入只冻结 panel 进程，不影响 router
- 窗口图标通过 `WM_SETICON` 设置（链接 icon.res）

---

## 配置文件 (groups.ini)

```ini
[配置]
组数量=3
组1名称=A
组2名称=B
组3名称=C
窗口标题1=iPhone Se2 D1
窗口标题2=iPhone Se2 D2
...
IP地址1=172.16.103.16
IP地址2=172.16.103.17
...
```

- `组数量`: 1-8 组，决定同时活跃的后端数量
- `组N名称`: 组名，用于 HTTP API 端口偏移 (`38980 + group`)
- `窗口标题1..32`: 每个 slot 的窗口标题（连接后自动设置）
- `IP地址1..32`: 每个 slot 的设备 IP（供路由匹配）

---

## HTTP API

**端口**: `38980 + group` (group 从 0 开始)

### POST /control

触摸/滑动控制，JSON body：

```json
{
  "action": "click|tap|swipe|down|up",
  "x": 100, "y": 200,
  "x2": 300, "y2": 400,
  "windowIndex": "1"
}
```

- `windowIndex`: 映射到网格 cell 位置（1-6），支持 "13"=窗口 1+3，也兼容数字 `1`
- 坐标经过 `api_point()` 从 screen 坐标转换到窗口局部坐标

### GET /panels

返回所有 slot 的窗口信息：

```json
[{"index":1, "cell":1, "title":"iPhone Se2 D1", "hwnd":12345, "x":0, "y":0, "w":853, "h":480}]
```

### GET /config

返回当前组和 slot 配置。

---

## 设计思路

### 为什么需要路由器？

UxPlay 原版只支持单设备投屏。本项目的目标是**一台 PC 同时接收多台设备**，但 AirPlay 协议通过 mDNS 广播发现服务，同一网络只能有一个服务。解决方案：

1. 路由器广播**单一** AirPlay 服务
2. 所有设备连接到路由器的公共端口
3. 路由器按源 IP 包头改写，转发到对应的后端 loopback 端口
4. 每个后端独立处理一台设备的完整 AirPlay 会话

### 为什么分离 panel 进程？

大漠插件 (damo) 的 dx2/dx3 hook 会冻结被注入进程。如果 panel 窗口在 router 进程中：
- damo 注入 → router 被冻结 → 所有投屏断开

分离到独立进程后：
- damo 注入 panel 进程 → 只冻结 panel → router 和所有后端不受影响

### 为什么用 D3D9 而不是 D3D11？

大漠自动化插件通过 D3D9 EndScene/Present hook 实现后台截图/控制。使用 `d3dvideosink`（D3D9）可以直接受大漠控制，无需额外适配。

---

## 编译环境

- **交叉编译**: Linux (Ubuntu) + mingw-w64
- **后端依赖**: GStreamer 1.0 (MSYS2), OpenSSL, libplist, libmicrodns
- **Router/Panel**: 纯 Win32 API，无外部依赖
- **构建命令**:
  ```bash
  # 后端
  cd UxPlay/build && ninja
  
  # 路由器 (需要 icon.res)
  x86_64-w64-mingw32-gcc -O2 -Wall -o uxplay-router.exe uxplay-router.c \
    -lws2_32 -liphlpapi -lole32 -lshell32 -lcomctl32 -lcomdlg32 icon.res \
    -L/path/to/dnssd -ldnssd

  # 面板
  x86_64-w64-mingw32-gcc -O2 -Wall -o uxplay-panel.exe panel.c -luser32 -lgdi32 icon.res
  ```

---

## 当前进度

### 已完成

- [x] 路由器核心: mDNS 广播、端口映射、连接路由、进程管理
- [x] 多设备支持: 最多 5 个 slot 同时投屏（可配置到 8）
- [x] 网格布局: 3×2 网格，竖屏 pillarbox 居中，横屏填满
- [x] VNC 反向控制: TrollVNC 触控/滑动事件转发到对应窗口
- [x] HTTP API: `/control` 触摸控制 + `/panels` 窗口信息查询
- [x] 配置文件: groups.ini 支持组/标题/设备 IP 配置
- [x] 窗口标题自动设置: 连接后 SetWindowText 改名
- [x] 系统托盘: 阴影/置顶/全屏管理
- [x] 中文 MessageBox: 连接/拒绝/驱逐提示
- [x] 大漠兼容: D3D9 渲染 + panel 进程分离
- [x] 设备名匹配: 基于 AirPlay name 而非 VNC desktop-name
- [x] JSON API: windowIndex 支持字符串和数字（易语言兼容）
- [x] 旋转检测: 视频流分辨率变化检测和日志
- [x] 旋转窗口消失修复: deferred reset 延迟重置（conn_reset/feedback 超时不再立即销毁窗口）+ 窗口重建后 router 重新定位去边框 + VNC 重新 hook

### 进行中

- [ ] 全屏冻结问题排查
- [ ] VNC 反向控制稳定性（旋转后已修复窗口重建 hook，仍待长稳测试）

---

## 已知问题

### 1. iPhone 旋转时视频窗口消失（严重）

**现象**: 设备在投屏过程中旋转（竖屏↔横屏），视频窗口从任务栏完全消失。

**根因分析** (最终确认, 2026-08):

旋转时 iPhone 的行为：
1. 先断开旧 TCP 连接 → mirror 线程检测到 ECONNRESET → 调用 `conn_reset(cls, 1)` 回调
2. 同时/随后发送 TEARDOWN + 新 SETUP，新连接数据（新 SPS/PPS）到达 → `video_report_size()` 才设置 `rotation_pending=true`

**为什么之前的 conn_reset 抑制无效**：`rotation_pending` 是在**新 SPS 到达时**（`video_report_size`）才置位的，而 `conn_reset` 在**旧 TCP 断开时**就已触发——两者时序上 conn_reset 几乎总是更早。因此 conn_reset 抑制检查的是尚未置位的 flag，旋转期间照样穿空：
- `conn_reset` 未被抑制 → `reset_loop=true` + `relaunch_video=true` → 主循环退出 → `video_renderer_destroy()` → 窗口被销毁
- 重建的 pipeline 需要新数据 preroll 才显示，若 HTTP 服务器重启期间新 SETUP 丢失，窗口就**永久消失**
- 第二个漏网点：`feedback_callback`（每 1 秒的心跳超时检测）**完全没检查** `rotation_pending`，旋转期间心跳中断超过 `-reset` 阈值（默认 15 秒）会强制 `full_video_reset` + 主循环退出，同样销毁窗口

**修复方案（deferred reset 延迟重置）**:
- `conn_reset()` 不再立即销毁 pipeline，改为**武装延迟重置**（`g_reset_pending` + 2.5 秒确认窗口）
- 主循环 `reset_callback`（每 100ms）负责真正执行：只有确认窗口内**没有新视频数据**到达（真断连）才触发重建
- `video_report_size()` 收到新数据即**取消**延迟重置——旋转确认信号
- `feedback_callback` 超时同样走延迟重置，不再直接 `full_video_reset`
- 原 `rotation_pending` 抑制保留作第一道防线；TEARDOWN 抑制逻辑不变

**配套修复**:
- router `apply_grid_layout`：记录每 slot 已样式化的窗口 HWND，窗口重建（旋转/reset）后强制重新去边框+定位，避免新窗口以默认样式/位置出现
- VNC 反向控制：`vncm_watcher` 检测视频窗口被销毁重建后重新 hook，旋转后反向控制不失效

**旋转日志位置**: `%LOCALAPPDATA%\rotation.log`（记录 `conn_reset deferred` / `rotation confirmed ... CANCELLED` / `deferred reset FIRED` 判定过程）

### 2. Win+Arrow 窗口吸附

子进程窗口作为 router 窗口的 child window，Win+Arrow snap 不适用于 child window。由于 damo 注入限制（router 进程注入会冻结投屏），无法将 strip 窗口放入 router 进程。用户已接受当前状态。

### 3. 全屏冻结

部分场景下所有屏幕同时冻结，根因尚未确认。可能与 D3D9 Device Lost 事件或 VNC 连接超时有关。

---

## 源码文件说明

| 文件 | 行数 | 说明 |
|------|------|------|
| `router/uxplay-router.c` | 2193 | 路由器主程序: mDNS、路由、API、VNC、布局、进程管理 |
| `router/panel.c` | 133 | 面板窗口进程: 大漠兼容的 strip/status 窗口 |
| `router/icon.res` | - | PE 图标资源 (logo.ico → ResEdit) |
| `router/logo.ico` | - | 图标源文件 |
| `UxPlay/uxplay.cpp` | 4058 | 后端主程序: AirPlay 会话、窗口管理、旋转检测、VNC |
| `UxPlay/renderers/video_renderer.c` | 1213 | GStreamer 管线: D3D9 渲染、bus 事件处理 |
| `UxPlay/lib/raop.c` | 896 | RAOP 协议核心: 连接管理、回调注册 |
| `UxPlay/lib/raop_handlers.h` | 1354 | RTSP 处理器: SETUP/TEARDOWN/SET_PARAMETER |
| `UxPlay/lib/raop_rtp_mirror.c` | 965 | 镜像 RTP 线程: TCP 数据接收、解密、推送 |
| `dist/groups.ini` | 34 | 示例配置文件 |

---

## GitHub Actions 在线编译

本仓库已配置 `.github/workflows/build.yml`，每次 push 到 `master`/`main` 自动编译并产出 Windows 产物：

- 环境：`windows-latest` + MSYS2 UCRT64 + MinGW-w64 gcc
- 产出 3 个 exe：`uxplay.exe`（后端）、`uxplay-router.exe`（路由器）、`uxplay-panel.exe`（面板）
- 下载：GitHub 仓库 → Actions → 最新一次成功 run → 底部 Artifacts → `uxplay-windows` zip

编译要点（踩坑记录）：

- `UxPlay` 用 `cmake -G "Unix Makefiles" -DNO_X11_DEPS=ON`，icon.o 需先用 `windres icon.rc -O coff` 预生成（CMake 引用 `${CMAKE_BINARY_DIR}/icon.o` 但无生成规则）
- router 的显式 W 后缀 API（`LoadIconW`/`LoadCursorW`）配系统资源宏（`IDI_APPLICATION`/`IDC_ARROW`/`IDC_HAND`）时必须用 `MAKEINTRESOURCEW()` 显式转宽字符，否则非 UNICODE 环境下类型不匹配
- mdnsd 版 `libdnssd.a` 缺失公共层函数（`dnssd_init`/`dnssd_set_airplay_features`/`utils_hwaddr_*`），由 `router/dnssd_compat.c` 补齐
- 链接顺序：`-lws2_32` 必须放在 `-ldnssd` 之后（libdnssd.a 引用 `gethostname`/`ntohs`）
- router 需要 `-lgdi32`（`BitBlt`/`CreateCompatibleDC` 等 GDI 函数）和 `UxPlay/lib/compat.c`（`wsa_strerror`）

---

## 发布产物

`UxPlay-Multi-win64.zip` 包含：

- `uxplay-router.exe` — 路由器
- `uxplay.exe` — 后端 (最多5个实例)
- `uxplay-panel.exe` — 面板窗口
- `groups.ini` — 配置文件
- `*.dll` — GStreamer + FFmpeg + 系统依赖

---

## License

基于 [UxPlay](https://github.com/FDH2-UxPlay/UxPlay) (GPLv3) 定制。
