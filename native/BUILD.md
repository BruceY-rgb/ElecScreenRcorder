# 构建指南

本文档说明如何在 Windows 上构建 recorder_core.exe。

## 前置条件

### 软件要求
- Windows 10/11 (64-bit)
- Visual Studio 2022 (包含 C++ Desktop 开发)
- CMake 3.20+
- Git

### 确保 OBS Studio submodule 已初始化
```powershell
cd native/third_party/obs-studio
git submodule update --init --recursive
```

---

## 步骤 1: 下载 OBS Studio

### 方式 A: 使用脚本自动下载（推荐）

```powershell
cd native/scripts
.\download-obs.ps1 -Version "30.2.3" -OutputDir "..\..\obs-download"
```

### 方式 B: 手动下载

1. 访问 https://github.com/obsproject/obs-studio/releases
2. 下载 `OBS-Studio-30.2.3-portable.zip`
3. 解压到项目根目录的 `obs-download` 文件夹

---

## 步骤 2: 提取 OBS DLL

```powershell
cd native/scripts
.\setup-obs.ps1 -ObsDir "..\..\obs-download\extracted\OBS-Studio-30.2.3" -DistDir "..\dist"
```

这会将以下文件复制到 `native/dist/`:
- `obs.dll`, `libobs-d3d11.dll` (核心库)
- `w32-pthreads.dll` (线程库)
- `avcodec-60.dll`, `avformat-60.dll` 等 (FFmpeg 库)
- `obs-plugins/64bit/` (插件)
- `data/` (数据文件)

---

## 步骤 3: 构建 libobs（可选）

如果 CMake 找不到 obs 库，需要构建 libobs：

```powershell
cd native/third_party/obs-studio

# 配置构建
cmake -B build -G "Visual Studio 17 2022" -A x64 ^
  -DENABLE_UI=OFF ^
  -DENABLE_BROWSER=OFF ^
  -DENABLE_SCRIPTING=OFF ^
  -DENABLE_TESTING=OFF ^
  -DENABLE_VST=OFF ^
  -DENABLE_AUTOMATION=OFF ^
  -DENABLE_WEBRTC=OFF ^
  -DENABLE_JACK=OFF ^
  -DENABLE_PULSEAUDIO=OFF ^
  -DENABLE_SRT=OFF ^
  -DENABLE_RTMPS=OFF ^
  -DCMAKE_BUILD_TYPE=Release

# 构建 libobs
cmake --build build --config Release --target libobs
```

---

## 步骤 4: 构建 recorder_core

```powershell
cd native

# 配置 CMake
cmake -B build -G "Visual Studio 17 2022" -A x64 -DCMAKE_BUILD_TYPE=Release

# 构建
cmake --build build --config Release
```

输出: `native/dist/recorder_core.exe`

---

## 步骤 5: 验证构建

### 检查输出文件
```powershell
ls native/dist/
```

应该包含:
- `recorder_core.exe` (主程序)
- `obs.dll`, `libobs-d3d11.dll` 等 (OBS DLL)
- `obs-plugins/64bit/*.dll` (插件)
- `data/` (数据文件夹)

### 运行测试
```powershell
# 启动程序，应该输出 {"type":"status","state":"ready"}
.\native\dist\recorder_core.exe
```

---

## 常见问题

### Q: CMake 找不到 obs 库
A: 确保已执行步骤 2 提取 OBS DLL，或步骤 3 构建 libobs

### Q: 链接错误 LNK2019
A: 确保所有 OBS DLL 都在 `native/dist/` 目录中

### Q: 运行时提示缺少 DLL
A: 将 `native/dist` 添加到系统 PATH，或将 DLL 复制到 exe 同目录

### Q: 屏幕捕获不工作
A: 确保 `win-capture.dll` 插件已正确复制到 `obs-plugins/64bit/`

---

## 完整构建脚本

```powershell
# 完整构建流程
cd "screen-recording-tool"

# 1. 初始化子模块
git submodule update --init --recursive

# 2. 下载 OBS
cd native/scripts
.\download-obs.ps1

# 3. 提取 DLL
$obsDir = "..\..\obs-download\extracted\OBS-Studio-30.2.3"
$distDir = "..\dist"
.\setup-obs.ps1 -ObsDir $obsDir -DistDir $distDir

# 4. 构建
cd ..
cmake -B build -G "Visual Studio 17 2022" -A x64 -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release

# 5. 测试
.\dist\recorder_core.exe
```
