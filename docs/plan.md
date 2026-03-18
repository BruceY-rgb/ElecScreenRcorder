# Electron 录屏工具

## 1. 目标

- 支持录制高指令视频
- 支持录音(麦克风、分文件)
- 支持记录鼠标、键盘触发事件
- 支持轮询记录鼠标位置变化信息
- 支持高精度时间戳

## 2. 系统架构与Electron规范

> **核心原则**：采用Sidecar模式，Electron仅作为UI控制器，通过子进程管理Native Core

### 2.1 项目目录结构

``` bash
/project-root
  ├── /native          # [交付物] 那个 C++ 的录制核心 exe
  ├── /src
  │   ├── /main        # [主进程] Node.js 逻辑，负责调用 native exe、文件操作
  │   │   ├── services # 核心服务（如 RecordingService.js）
  │   │   └── index.js # Electron 入口
  │   ├── /preload     # [桥接层] 定义 UI 能调用的 API
  │   └── /renderer    # [GUI] 纯前端代码 (Vue/React)
  │       ├── components
  │       └── views
```

### 2.2 Electron IPC通讯规范

> 技术要求：C++开发，无GUI，专注于高性能采集

### 3.1 音视频录制能力

- **画质标准**：支持2K(1440p)60fps录制
  - 在1080p 60fps下，编码码率不低于10,000kbps
- **底层技术**：配合OBS核心库(libobs)实现，强制请求硬件解码(NVENC/AMF/QSV)以降低CPU占用
- **音频处理**：支持同步录音，且支持将音频录制到独立归到或独立文件(实现游戏音与麦克风分离)


### 3.2 高精输入采集能力

- **时间戳基准**:**必须**使用Win32`GetSystemTimePreciseAsFileTime`获取高精度时间
  - 禁止使用Hook回调自带的时间戳(GetTickCount精度不足)
  - 要求视频第一帧时间与输入日志首行时间严格对齐
- **键盘与鼠标点击**
  - 使用Win32 API`SetWindowsHook`注册全局Hook，记录按键及点击绝对坐标
- **鼠标移动轨迹**
  - 使用DirectX `DirectInput` API 轮询鼠标状态
  - **轮询周期**：5ms(即200Hz采样率)
  - 记录内容：绝对位置(x,y)及相对位移(dx,dy)


## 4. Native Core交付与通讯规范

> **交互逻辑**：定义Electron如何控制EXE，以及高频数据如何存储

### 4.1 交付物形态

- **文件**：独立的Windows可执行文件(如`recorder_core.exe`)
- **环境**：**绿色免安装**。所有依赖库(OBS dll, ffmpeg dll, VC++  Runtime等)必须打包在exe同级目录

### 4.2 进程通讯协议(Stdio)

程序启动后进入 **守护模式**，不利己退出，通过标准输入输出交互

- **输入(stdin)**:接收json指令(换行符`\n`分隔)

```json
{"action": "start", "config": {"resolution": "2k", "fps": 60, "savePath": "D:/video.mp4"}}
{"action": "stop"}
```

- **输出(stout)**：仅打印JSON状态/结果(换行符`\n`分隔)，禁止打印调试日志

```json
{"type": "status", "state": "recording"}
{"type": "error", "msg": "Microphone init failed"}
```

## 4.3 数据落盘与生命周期策略

核心：由于鼠标采样率极高（>120Hz），严禁通过 Stdio 实时将数据打印给 Electron，防止主进程阻塞

- 录制中 (Recording)
  - 收到 start 指令后，开启 OBS 录制与输入采集线程
  - C++ 核心应将采集到的输入数据，实时流式写入到本地临时文件（CSV 或 Binary）
  - 文件 A (actions.csv)：离散事件（按键、点击）
  - 文件 B (movements.csv)：高频轨迹（5ms 轮询的 timestamp, x, y, dx, dy）
- 结束时 (Finishing)
  - 收到 stop 指令后，停止录制，flush 并关闭所有文件句柄
  - 最终响应：在 Stdout 返回所有生成文件的绝对路径供 Electron 读取
  - 示例响应

```json
{
  "type": "finish",
  "videoPath": "D:/output/rec_001.mp4",
  "actionsPath": "D:/output/rec_001_actions.csv",
  "movementsPath": "D:/output/rec_001_movements.csv"
}
```

## 5. 格式说明

### 5.1 视频
- 默认MKV，支持设置自动转封装mp4
### 5.2 录音
- 独立音频文件
### 5.3 触发事件（actions.csv）
- 鼠标点击
  - altKey、ctrlKey、metaKey、shiftKey 组合键信息
  - x、y 鼠标的绝对位置坐标
  - button 左右键、中键标志
  - type 按下/抬起
  - time 触发时间戳（单位ms，录制开始相对时间）
  - rawTime 系统时间戳
- 键盘操作
  - altKey、ctrlKey、metaKey、shiftKey 组合键信息
  - keycode、keyChar 键盘按键信息
  - type 按下/抬起
  - time 触发时间戳（单位ms，录制开始相对时间）
  - rawTime 系统时间戳
5.4 轮询事件（movements.csv）
- 鼠标位置轮询
  - x、y 鼠标的绝对位置坐标
  - dx、dy 轮询周期内的相对位移累计值
  - time 触发时间戳（单位ms，录制开始相对时间）
  - rawTime 系统时间戳
5.5 数据样例（CSV）
```csv
# actions.csv
type,time,rawTime,x,y,button,keycode,keyChar,altKey,ctrlKey,shiftKey,metaKey
MOUSE_DOWN,469,1765289833003,333,981,1,,,false,false,false,false
MOUSE_UP,551,1765289833085,333,981,1,,,false,false,false,false
KEY_DOWN,18617,1765289851151,,,,25,P,false,false,false,false
KEY_UP,18704,1765289851238,,,,25,P,false,false,false,false
```
```csv
# movements.csv
time,rawTime,x,y,dx,dy
0,1765289833003,333,981,0,0
5,1765289833008,343,991,10,10
```
## 6. 附录
### 6.1 会议纪要（2026-02-28）
#### 6.1.1 产品需求
- 需要一个可拖动悬浮窗
  - 不会录制进视频
  - 展示分辨率、帧率、键鼠状态等监视结果
- 提供一个返回系统信息的API
  - 分辨率、缩放比例、CPU、显卡、键盘、鼠标（hz）等软硬件信息
  - 可以使GUI在开始录制前调用，确定是否符合条件并开始录制
- 允许暂停录制
  - 相对时间计时暂停
  - 暂停、继续录制后，相对时间戳保持匹配（视频相对时间戳与actions相对时间戳）
### 6.2 会议纪要（2026-03-03）
- 待开始，启动前乙方产品设计对齐