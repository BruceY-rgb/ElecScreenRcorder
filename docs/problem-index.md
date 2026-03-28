# 项目问题索引文档

## 核心问题：录制架构极不稳定

> **这是最关键的问题**：整个项目架构录制过程极为不稳定，包装成安装包之后在不同环境下会出现各种不同的问题。需要让 manus 对录制设计进行全面评估，确定需要采集哪些诊断数据/日志来排查问题。

---

## 问题 1: 录制过程不稳定

### 问题描述
- 录制经常不成功
- 录制结束后无法正常结束

### 关键文件

| 文件 | 行号 | 说明 |
|------|------|------|
| `src/main/services/RecorderService.ts` | 282-385 | `start()` - 录制启动 |
| `src/main/services/RecorderService.ts` | 387-435 | `stop()` - 录制停止，35秒超时 |
| `src/main/services/RecorderService.ts` | 886-925 | `checkHeartbeat()` - 心跳检测 |
| `src/main/services/RecorderService.ts` | 677-719 | `handleProcessExit()` - 进程退出处理 |
| `native/src/recorder.cpp` | 611-738 | `startRecording()` - FFmpeg 启动 |
| `native/src/recorder.cpp` | 740-821 | `stopRecording()` - 录制停止 |
| `native/src/recorder.cpp` | 988-1036 | `stopFFmpegGracefully()` - FFmpeg 优雅停止 |
| `native/src/recorder.cpp` | 431-516 | `waitForFFmpegReady()` - FFmpeg 就绪检测 |

### 可能原因
1. `stopFFmpegGracefully()` 管道写入未检查返回值
2. 心跳超时在录制期间被忽略，无法检测 FFmpeg 崩溃
3. 分段合并失败时无错误处理

---

## 问题 2: 硬件检测不稳定（麦克风/分辨率）

### 问题描述
- 麦克风阵列使用不稳定，经常失败
- 分辨率检测经常出现异常
- 硬件设备枚举不可靠

### 关键文件

| 文件 | 行号 | 说明 |
|------|------|------|
| `native/src/system_info.cpp` | - | 系统信息获取（分辨率、GPU等） |
| `native/src/audio_device.cpp` | - | 麦克风设备枚举 |
| `native/src/recorder.h` | 22-35 | `RecordingConfig` - 麦克风配置 |
| `native/src/recorder.cpp` | 1167-1192 | `buildMicFFmpegCommand()` - 麦克风命令构建 |
| `native/src/recorder.cpp` | 1194-1345 | `startMicFFmpeg()` - 麦克风 FFmpeg 启动 |
| `native/src/recorder.cpp` | 1350-1385 | `stopMicFFmpegGracefully()` - 麦克风停止 |

### 可能原因
1. 麦克风设备名称包含特殊字符导致 FFmpeg 启动失败
2. 分段合并依赖 FFmpeg concat，单个分段损坏会导致合并失败
3. 分辨率枚举使用 EnumDisplayMonitors 不稳定
4. 音频设备枚举依赖 FFmpeg -list_devices，不够可靠

---

## 问题 3: 组合键共同按下时只能记录一个

### 问题描述
- 多个按键同时按下时，actions.csv 只显示一个

### 关键文件

| 文件 | 行号 | 说明 |
|------|------|------|
| `native/src/input_capture.cpp` | 302-363 | `LowLevelKeyboardProc()` - 键盘 Hook |
| `native/src/csv_writer.cpp` | 91 | CSV 键盘类型硬编码为 "keyboard" |
| `native/src/csv_writer.cpp` | 113-115 | 鼠标事件类型生成 |

### 根本原因
- 键盘事件存储了 `isDown` 字段但未在 CSV 中区分
- 每次只处理单个按键事件，无"组合键"概念

---

## 问题 4: actions.csv 的 type 字段定义不标准

### 问题描述
- 当前 type 值为 `keyboard`、`mouse_left_down` 等
- 文档期望值为 `KEY_DOWN`、`KEY_UP`、`MOUSE_DOWN`、`MOUSE_UP`

### 关键文件

| 文件 | 行号 | 说明 |
|------|------|------|
| `native/src/csv_writer.cpp` | 91 | 键盘类型硬编码 "keyboard" |
| `native/src/csv_writer.cpp` | 113-115 | 鼠标类型为 `mouse_left_down` 格式 |
| `/docs/plan.md` | 132-139 | 期望的 CSV 格式定义 |

### 期望格式 (来自 plan.md)
```csv
type,time,rawTime,x,y,button,keycode,keyChar,altKey,ctrlKey,shiftKey,metaKey
MOUSE_DOWN,469,1765289833003,333,981,1,,,false,false,false,false
KEY_DOWN,18617,1765289851151,,,,25,P,false,false,false,false
```

---

## 问题 5: 悬浮窗游戏全屏后不会置顶

### 问题描述
- 游戏全屏启动后，悬浮窗不会自动置顶在游戏上层

### 关键文件

| 文件 | 行号 | 说明 |
|------|------|------|
| `src/main/windows/mainWindow.ts` | 46-66 | `overlayWindow` 创建，静态 `alwaysOnTop: true` |

### 根本原因
- `alwaysOnTop: true` 是静态设置，某些全屏应用下会被系统强制降级
- 没有动态检测和恢复置顶状态的逻辑

### 修复建议
1. 定期检测窗口是否失去置顶状态，必要时重新调用 `setAlwaysOnTop(true)`
2. 检测到全屏应用时自动最小化/隐藏悬浮窗

---

## 问题 6: 窗口 UI 问题

### 问题描述
- 样式过于不成熟
- 悬浮窗收起会出现闪屏
- 收起后无法打开

### 关键文件

| 文件 | 行号 | 说明 |
|------|------|------|
| `src/main/ipc/handlers.ts` | 108-132 | `overlay:setMode` 和 `setHoverSize` IPC |
| `src/renderer/src/components/OverlayDisplay.tsx` | - | 悬浮窗 React 组件（未监听 mode 事件） |
| `src/renderer/src/styles.css` | 97-549 | 主样式文件 |
| `src/renderer/src/styles.css` | 524-549 | 悬浮窗样式 |

### 根本原因
1. `preload/index.ts` 暴露了 `setMode` 和 `setHoverSize` API
2. `OverlayDisplay.tsx` 未监听 `overlay:mode` 事件
3. 缺少 collapsed 状态的样式和交互逻辑

### 参考建议
参考成熟的 Windows 屏幕录制软件（如 OBS Studio、Bandicam、ShareX）的 UI 设计：
- 悬浮窗采用半透明磨砂玻璃效果
- 收起时使用动画过渡，避免闪屏
- 状态指示器清晰显示录制状态/时长
- 参考 `/docs/plan.md` 中的 UI 设计要求

---

## 文件索引汇总

| 问题 | 核心文件 |
|------|----------|
| 录制稳定性 | `src/main/services/RecorderService.ts`, `native/src/recorder.cpp` |
| 硬件检测不稳定 | `native/src/system_info.cpp`, `native/src/audio_device.cpp` |
| 组合键问题 | `native/src/input_capture.cpp`, `native/src/csv_writer.cpp` |
| CSV type 定义 | `native/src/csv_writer.cpp`, `/docs/plan.md` |
| 悬浮窗置顶 | `src/main/windows/mainWindow.ts` |
| UI 问题 | `OverlayDisplay.tsx`, `styles.css`, `handlers.ts` |