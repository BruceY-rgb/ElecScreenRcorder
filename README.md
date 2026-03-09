# Screen Recording Tool

A high-performance Windows screen recording application built with Electron and C++ Native Core using libobs.

## Features

- **High-Quality Video Recording**: Supports 2K/1440p at 60fps with hardware encoding (NVENC/AMF/QSV)
- **Audio Recording**: Separate tracks for system audio and microphone
- **Input Event Capture**: Records keyboard and mouse click events via global Win32 hooks
- **Mouse Tracking**: High-frequency mouse position polling at 200Hz (5ms interval)
- **High-Precision Timestamps**: Uses `GetSystemTimePreciseAsFileTime` for sub-microsecond accuracy
- **Pause/Resume**: Full pause/resume support with timestamp alignment
- **CSV Export**: Outputs actions.csv (keyboard/clicks) and movements.csv (mouse trajectory)

## Tech Stack

### Electron Side
- **TypeScript** - Type-safe JavaScript
- **React 18** - UI framework
- **Vite 5** - Build tool
- **Electron 28+** - Desktop framework

### C++ Native Core
- **C++17** - Native core language
- **libobs** - OBS Studio core library for video capture
- **Hardware Encoding** - NVENC/AMF/QSV with x264 fallback
- **Win32 API** - Input capture (SetWindowsHookEx, GetCursorPos)

## Architecture

This project uses a **Sidecar Architecture**:

```
┌─────────────────────────────────────────────────────────┐
│                    Electron App                         │
│  ┌─────────────┐  ┌─────────────┐  ┌─────────────┐    │
│  │  React UI   │──│  Main Proc  │──│  Preload    │    │
│  └─────────────┘  └──────┬──────┘  └─────────────┘    │
└───────────────────────────┼─────────────────────────────┘
                            │ stdio JSON
                            ▼
┌─────────────────────────────────────────────────────────┐
│              recorder_core.exe (C++ Native)             │
│  ┌──────────┐  ┌──────────┐  ┌──────────┐            │
│  │ OBS Rec  │  │  Input   │  │   CSV    │            │
│  │  Engine  │  │ Capture  │  │  Writer  │            │
│  └──────────┘  └──────────┘  └──────────┘            │
└─────────────────────────────────────────────────────────┘
```

## Directory Structure

```
/screen-recording-tool
├── /native                          # C++ Native Core
│   ├── /src
│   │   ├── main.cpp                # Entry: stdin/stdout JSON loop
│   │   ├── recorder.h/cpp          # OBS recording orchestrator
│   │   ├── input_capture.h/cpp     # Keyboard/mouse hooks + polling
│   │   ├── csv_writer.h/cpp        # Thread-safe CSV writer
│   │   ├── protocol.h/cpp          # JSON protocol parser/serializer
│   │   ├── system_info.h/cpp       # System info query API
│   │   └── utils.h/cpp             # Timestamp, threading utilities
│   ├── CMakeLists.txt
│   └── /dist                       # Build output (exe + DLLs)
├── /src
│   ├── /main                       # Electron main process
│   ├── /preload                    # contextBridge API
│   └── /renderer                   # React UI
├── /docs
│   └── plan.md                     # Design specification
├── package.json
├── tsconfig.json
├── electron-builder.yml
└── README.md
```

## Quick Start

### Prerequisites

- Windows 10/11 (x64)
- Node.js 18+
- Visual Studio 2022 (for C++ build)
- CMake 3.20+

### Install Dependencies

```bash
npm install
```

### Development Mode

```bash
npm run dev
```

### Build

#### Build Electron App

```bash
npm run build
```

#### Build C++ Native Core

```bash
cd native
cmake -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
```

#### Package Electron App

```bash
npm run build:electron
```

## Communication Protocol

The Electron app communicates with the C++ Native Core via stdio JSON:

### Commands (stdin)

```json
{"action": "start", "config": {"resolution": "2k", "fps": 60, "savePath": "D:/video.mp4"}}
{"action": "stop"}
{"action": "pause"}
{"action": "resume"}
{"action": "getSystemInfo"}
```

### Responses (stdout)

```json
{"type": "status", "state": "recording"}
{"type": "error", "msg": "Microphone init failed"}
{"type": "finish", "videoPath": "D:/output/video.mkv", "actionsPath": "D:/output/actions.csv", "movementsPath": "D:/output/movements.csv"}
```

## Output Files

After recording, three files are generated:

| File | Description |
|------|-------------|
| `video.mkv` | Recorded video with audio |
| `actions.csv` | Keyboard and mouse click events |
| `movements.csv` | Mouse position data at 200Hz |

## License

MIT
