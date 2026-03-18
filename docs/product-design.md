# ScreenCraft 屏幕录制工具 - 技术设计文档

## 一、系统架构

### 1.1 整体架构

采用 Sidecar（边车）架构模式：

```
┌─────────────────────────────────────────────────────────────┐
│                      Electron 应用层                        │
│  ┌──────────────┐    ┌──────────────┐    ┌──────────────┐ │
│  │   渲染进程    │◄──►│    主进程    │◄──►│   预加载脚本 │ │
│  │  (React UI)  │    │  (Node.js)   │    │   (Bridge)   │ │
│  └──────────────┘    └──────────────┘    └──────────────┘ │
│                             │                              │
│                    ┌────────▼────────┐                    │
│                    │  RecorderService │                   │
│                    │   (子进程管理)    │                   │
│                    └────────┬────────┘                    │
└─────────────────────────────┼─────────────────────────────┘
                              │ stdio JSON 协议
                              │ (二进制模式 stdin/stdout)
┌─────────────────────────────┼─────────────────────────────┐
│                    ┌────────▼────────┐                    │
│                    │  recorder_core  │  C++ 原生核心层    │
│                    │      .exe        │                    │
│                    └────────┬────────┘                    │
│                             │                              │
│   ┌─────────────────────────┼─────────────────────────┐   │
│   │                         │                         │   │
│ ┌─▼───────┐           ┌─────▼──────┐           ┌──────▼─┐│
│ │ OBS     │           │  输入捕获   │           │  CSV   ││
│ │ 录制引擎 │           │ (Hook+轮询) │           │ 文件写入││
│ └─────────┘           └─────────────┘           └─────────┘│
└─────────────────────────────────────────────────────────────┘
```

### 1.2 技术栈

| 层级 | 技术选型 | 版本 |
|------|----------|------|
| UI 框架 | React | 18.x |
| 构建工具 | Vite | 5.x |
| 桌面框架 | Electron | 28+ |
| 视频引擎 | libobs | OBS Studio 核心库 |
| 编码器 | NVENC/AMF/QSV/x264 | 硬件优先，软件兜底 |
| 编程语言 | C++17 | - |
| JSON 库 | nlohmann/json | Header-only |
| 并发队列 | moodycamel::ReaderWriterQueue | Lock-free |

---

## 二、核心功能规格

### 2.1 视频录制

| 参数 | 规格 |
|------|------|
| 分辨率支持 | 1920×1080、2560×1440、3840×2160 |
| 帧率 | 30fps、60fps、120fps |
| 编码器优先级 | NVENC → AMF → QSV → x264 |
| 视频格式 | MKV（默认），支持转封装 MP4 |
| 码率 | 2K@60fps ≥15000kbps，1080p@60fps ≥10000kbps |

### 2.2 音频录制

| 参数 | 规格 |
|------|------|
| 系统音频 | WASAPI 回环捕获 |
| 麦克风 | WASAPI 输入捕获 |
| 音轨 | 分轨录制，独立 MKV 音轨 |
| 编码格式 | AAC |

### 2.3 输入事件采集

| 事件类型 | 采集方式 | 采样率 | 时间戳精度 |
|----------|----------|--------|------------|
| 键盘按键 | Win32 钩子 (WH_KEYBOARD_LL) | 事件触发 | GetSystemTimePreciseAsFileTime |
| 鼠标点击 | Win32 钩子 (WH_MOUSE_LL) | 事件触发 | GetSystemTimePreciseAsFileTime |
| 鼠标移动 | 定时轮询 GetCursorPos | 200Hz (5ms) | GetSystemTimePreciseAsFileTime |

### 2.4 录制控制

- 支持暂停/恢复功能
- 暂停期间不记录输入事件
- 暂停时长不计入相对时间戳
- 视频帧与 CSV 时间戳保持对齐

---

## 三、数据格式

### 3.1 actions.csv（离散事件）

```csv
type,time,rawTime,x,y,button,keycode,keyChar,altKey,ctrlKey,shiftKey,metaKey
MOUSE_DOWN,469,1765289833003,333,981,1,,,false,false,false,false
MOUSE_UP,551,1765289833085,333,981,1,,,false,false,false,false
KEY_DOWN,18617,1765289851151,,,,25,P,false,false,false,false
KEY_UP,18704,1765289851238,,,,25,P,false,false,false,false
```

### 3.2 movements.csv（鼠标轨迹）

```csv
time,rawTime,x,y,dx,dy
0,1765289833003,333,981,0,0
5,1765289833008,343,991,10,10
10,1765289833013,358,1005,15,14
```

### 3.3 时间戳对齐机制

```
录制开始时记录绝对时间戳 T0
视频帧时间 = 帧序号 × (1000 / fps) - 累计暂停时长
CSV 事件 time = (rawTime - T0) - 累计暂停时长
```

---

## 四、通讯协议

### 4.1 进程通讯方式

- 方式：stdio（stdin/stdout）
- 格式：Newline-delimited JSON
- Windows 二进制模式：`_setmode(_fileno(stdout), _O_BINARY)`

### 4.2 命令格式

```json
{"action": "start", "config": {"resolution": "2560x1440", "fps": 60, "savePath": "D:/output"}}
{"action": "pause"}
{"action": "resume"}
{"action": "stop"}
{"action": "getSystemInfo"}
```

### 4.3 响应格式

```json
{"type": "status", "state": "recording"}
{"type": "error", "msg": "Microphone init failed"}
{"type": "finish", "videoPath": "...", "actionsPath": "...", "movementsPath": "..."}
{"type": "systemInfo", "displays": [...], "cpu": "...", "gpu": "...", "memory": ...}
```

---

## 五、关键技术实现

### 5.1 高精度时间戳

必须使用 `GetSystemTimePreciseAsFileTime`，禁止使用 `GetTickCount`：

```cpp
FILETIME ft;
GetSystemTimePreciseAsFileTime(&ft);
ULARGE_INTEGER uli;
uli.LowPart = ft.dwLowDateTime;
uli.HighPart = ft.dwHighDateTime;
int64_t timestamp_ms = uli.QuadPart / 10000;
```

### 5.2 无锁队列设计

Hook 回调必须在 300ms 内返回，否则 Windows 会移除钩子：

```cpp
LRESULT CALLBACK KeyboardHookProc(int nCode, WPARAM wParam, LPARAM lParam) {
    if (nCode >= 0) {
        KeyboardEvent evt;
        evt.rawTime = GetHighPrecisionTimestamp();
        evt.keycode = /* ... */;
        g_keyboardQueue.enqueue(evt);  // Lock-free, fast
    }
    return CallNextHookEx(NULL, nCode, wParam, lParam);
}
```

### 5.3 200Hz 鼠标轮询

需要调用 `timeBeginPeriod(1)` 设置系统定时器分辨率：

```cpp
timeBeginPeriod(1);  // 设置前
while (recording) {
    GetCursorPos(&pos);
    g_mouseQueue.enqueue(pos);
    Sleep(5);  // 实际约 5ms，而非 15.6ms
}
timeEndPeriod(1);  // 恢复后
```

### 5.4 libobs 初始化

```cpp
obs_startup("en-US", nullptr, nullptr);
obs_reset_video(&ovi);
obs_reset_audio(&oai);

// 关键：加载模块
obs_add_data_path("./data/libobs/");
obs_add_module_path("./obs-plugins/64bit/", "./data/obs-plugins/%module%/");
obs_load_all_modules();
obs_post_load_modules();
```

### 5.5 DPI 感知

在进程启动时设置 DPI 感知，确保鼠标坐标与物理像素匹配：

```cpp
SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
```

---

## 六、系统要求

### 6.1 运行环境

| 项目 | 最低要求 |
|------|----------|
| 操作系统 | Windows 10 (64-bit) |
| 内存 | 8 GB |
| 显卡 | 支持 DirectX 11 |

### 6.2 硬件编码器支持

| 编码器 | 厂商 | 最低显卡 |
|--------|------|----------|
| NVENC | NVIDIA | GTX 600 |
| AMF | AMD | RX 200 |
| QSV | Intel | 第 6 代 Core |

---

## 七、输出文件

| 文件 | 格式 | 描述 |
|------|------|------|
| video.mkv | MKV | 视频（含独立音轨） |
| actions.csv | CSV | 键盘/鼠标点击事件 |
| movements.csv | CSV | 鼠标移动轨迹（200Hz） |

---

## 八、目录结构

```
/screen-recording-tool
├── /native                          # C++ Native Core
│   ├── /src
│   │   ├── main.cpp                # 入口：stdin/stdout JSON 循环
│   │   ├── recorder.h/cpp          # OBS 录制编排
│   │   ├── input_capture.h/cpp     # 键盘/鼠标 Hook + 轮询
│   │   ├── csv_writer.h/cpp        # CSV 写入
│   │   ├── protocol.h/cpp          # JSON 协议解析
│   │   ├── system_info.h/cpp       # 系统信息查询
│   │   └── utils.h/cpp             # 时间戳工具
│   ├── /third_party
│   │   ├── /obs-studio             # libobs
│   │   ├── /nlohmann-json          # JSON 库
│   │   └── /readerwriterqueue      # 无锁队列
│   └── CMakeLists.txt
├── /src
│   ├── /main                       # Electron 主进程
│   │   ├── /services               # 服务层
│   │   │   └── RecorderService.ts  # 子进程管理
│   │   ├── /ipc                    # IPC 处理器
│   │   └── /windows                # 窗口管理
│   ├── /preload                    # contextBridge
│   └── /renderer                   # React UI
├── /docs
│   ├── plan.md                     # 设计规范
│   ├── working-plan.md             # 开发计划
│   └── product-design.md           # 本文档
└── package.json
```
