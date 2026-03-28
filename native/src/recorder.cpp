/**
 * FFmpeg-based Screen Recording Implementation
 *
 * Uses Windows GDI screen capture + FFmpeg for encoding.
 * Spawns ffmpeg as a child process.
 */

#include "recorder.h"
#include "utils.h"
#include "audio_capture.h"

#include <windows.h>
#include <shlobj.h>
#include <iostream>
#include <sstream>
#include <thread>
#include <chrono>
#include <fstream>
#include <atomic>
#include <cstdlib>
#include <cstring>

// Timeline log file
static std::ofstream g_timelineLogFile;

static int64_t getTimestampMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()
    ).count();
}

static void writeNativeLog(const char* message) {
    int64_t ts = getTimestampMs();
    std::string logLine = "[NATIVE " + std::to_string(ts) + "] " + message + "\n";

    // Write to file
    if (g_timelineLogFile.is_open()) {
        g_timelineLogFile << logLine;
        g_timelineLogFile.flush();
    }

    // Also output to cerr for debugging
    std::cerr << logLine;
}

// Global recorder instance
Recorder* g_recorder = nullptr;

// Get AppData/Roaming/ScreenCraft/logs path for logging
static std::wstring getNativeLogPath() {
    wchar_t appData[MAX_PATH];
    if (SHGetFolderPathW(NULL, CSIDL_APPDATA, NULL, 0, appData) != S_OK) {
        return L"";
    }
    std::wstring logDir = std::wstring(appData) + L"\\ScreenCraft\\logs";
    CreateDirectoryW(logDir.c_str(), NULL);
    return logDir + L"\\native_timeline.log";
}

Recorder::Recorder()
    : state_(RecordingState::IDLE)
    , encoderType_(EncoderType::NONE)
    , ffmpegStdin_(nullptr)
{
    std::cerr << "[DIAG] Recorder::Recorder() called" << std::endl; std::cerr.flush();
    memset(&ffmpegProcess_, 0, sizeof(ffmpegProcess_));
}

Recorder::~Recorder() {
    shutdown();
    if (g_timelineLogFile.is_open()) {
        g_timelineLogFile.close();
    }
}

bool Recorder::initialize(const std::wstring& modulePath) {
    // === Early diagnostics: output FFmpeg path check BEFORE opening log file ===
    std::cerr << "[DIAG] initialize() called" << std::endl;
    std::cerr.flush();

    // Set FFmpeg path from module path
    std::wstring ffmpegW = modulePath;
    if (ffmpegW.empty()) {
        // Try default path (user's custom FFmpeg installation)
        ffmpegW = L"D:\\ffmpeg\\ffmpeg-master-latest-win64-gpl\\bin";
    }

    // Find ffmpeg.exe
    std::wstring ffmpegExe = ffmpegW + L"\\ffmpeg.exe";
    if (GetFileAttributesW(ffmpegExe.c_str()) == INVALID_FILE_ATTRIBUTES) {
        // Try parent directory
        size_t pos = ffmpegW.find(L"\\dist");
        if (pos != std::wstring::npos) {
            ffmpegW = ffmpegW.substr(0, pos);
        }
        ffmpegExe = ffmpegW + L"\\ffmpeg.exe";
    }

    if (GetFileAttributesW(ffmpegExe.c_str()) == INVALID_FILE_ATTRIBUTES) {
        // Convert wstring to narrow for stderr output
        int sz = WideCharToMultiByte(CP_UTF8, 0, ffmpegExe.c_str(), (int)ffmpegExe.size(), nullptr, 0, nullptr, nullptr);
        std::string narrow(sz, 0);
        WideCharToMultiByte(CP_UTF8, 0, ffmpegExe.c_str(), (int)ffmpegExe.size(), &narrow[0], sz, nullptr, nullptr);
        std::cerr << "[DIAG] FFmpeg NOT FOUND at: " << narrow << std::endl;
        std::cerr.flush();
        return false;
    }
    {
        int sz2 = WideCharToMultiByte(CP_UTF8, 0, ffmpegExe.c_str(), (int)ffmpegExe.size(), nullptr, 0, nullptr, nullptr);
        std::string narrow2(sz2, 0);
        WideCharToMultiByte(CP_UTF8, 0, ffmpegExe.c_str(), (int)ffmpegExe.size(), &narrow2[0], sz2, nullptr, nullptr);
        std::cerr << "[DIAG] FFmpeg found: " << narrow2 << std::endl;
    }
    std::cerr.flush();
    std::cerr.flush();

    // === Now safe to initialize COM and open log file ===
    // Initialize COM for WASAPI (may already be initialized by host)
    initCOM();

    // Open timeline log file to AppData/ScreenCraft/logs (fixed location)
    std::wstring logPath = getNativeLogPath();
    if (!logPath.empty()) {
        g_timelineLogFile.open(logPath, std::ios::app);
    }
    if (!g_timelineLogFile.is_open()) {
        // Fallback to working directory
        g_timelineLogFile.open("recording_timeline.log", std::ios::app);
    }
    writeNativeLog("===== NATIVE CORE INITIALIZED =====");

    // FFmpeg path is already confirmed to exist above
    {
        int sz = WideCharToMultiByte(CP_UTF8, 0, ffmpegExe.c_str(), (int)ffmpegExe.size(), nullptr, 0, nullptr, nullptr);
        ffmpegPath_.resize(sz);
        WideCharToMultiByte(CP_UTF8, 0, ffmpegExe.c_str(), (int)ffmpegExe.size(), &ffmpegPath_[0], sz, nullptr, nullptr);
    }
    std::cout << "[RECORDER] FFmpeg path: " << ffmpegPath_ << std::endl;

    // Check for hardware encoders
    encoderType_ = checkHardwareEncoders();

    std::cout << "[RECORDER] Initialized with encoder: ";
    switch (encoderType_) {
        case EncoderType::NVENC: std::cout << "NVIDIA NVENC"; break;
        case EncoderType::AMF: std::cout << "AMD AMF"; break;
        case EncoderType::QSV: std::cout << "Intel QSV"; break;
        case EncoderType::X264: std::cout << "x264 (software)"; break;
        default: std::cout << "none";
    }
    std::cout << std::endl;

    // Check ddagrab availability
    ddagrabAvailable_ = isDdagrabAvailable();
    std::cout << "[RECORDER] ddagrab (DXGI Desktop Duplication): "
              << (ddagrabAvailable_ ? "available" : "not available (will use gdigrab)")
              << std::endl;

    return true;
}

// Helper: run an FFmpeg command and capture stdout+stderr, with timeout
static std::string runFFmpegProbe(const std::string& ffmpegPath, const std::string& args, int timeoutMs = 3000) {
    std::string command = "\"" + ffmpegPath + "\" " + args;
    std::string output;

    SECURITY_ATTRIBUTES sa = {};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;

    HANDLE hReadPipe, hWritePipe;
    if (!CreatePipe(&hReadPipe, &hWritePipe, &sa, 0)) return output;

    STARTUPINFOA si = {};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.hStdOutput = hWritePipe;
    si.hStdError = hWritePipe;
    si.hStdInput = nullptr;
    si.wShowWindow = SW_HIDE;

    PROCESS_INFORMATION pi = {};
    if (!CreateProcessA(nullptr, const_cast<char*>(command.c_str()), nullptr, nullptr,
                        TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        CloseHandle(hReadPipe);
        CloseHandle(hWritePipe);
        return output;
    }

    CloseHandle(hWritePipe);

    // Read output with timeout
    auto startTime = std::chrono::steady_clock::now();
    char buffer[4096];
    DWORD bytesRead;
    while (true) {
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - startTime).count();
        if (elapsed > timeoutMs) break;

        DWORD available = 0;
        if (!PeekNamedPipe(hReadPipe, nullptr, 0, nullptr, &available, nullptr) || available == 0) {
            if (WaitForSingleObject(pi.hProcess, 50) == WAIT_OBJECT_0) {
                // Process ended, read remaining
                PeekNamedPipe(hReadPipe, nullptr, 0, nullptr, &available, nullptr);
                if (available == 0) break;
            }
            continue;
        }
        if (ReadFile(hReadPipe, buffer, sizeof(buffer) - 1, &bytesRead, nullptr) && bytesRead > 0) {
            buffer[bytesRead] = '\0';
            output += buffer;
        }
    }

    TerminateProcess(pi.hProcess, 0);
    WaitForSingleObject(pi.hProcess, 1000);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    CloseHandle(hReadPipe);
    return output;
}

// Test if a specific hardware encoder actually works on this system
static bool testEncoderWorks(const std::string& ffmpegPath, const std::string& encoderName) {
    // Encode a single black frame — if the encoder opens successfully, it works
    std::string args = "-hide_banner -f lavfi -i color=black:s=64x64:d=0.1 -frames:v 1"
                       " -c:v " + encoderName + " -f null NUL";
    std::string output = runFFmpegProbe(ffmpegPath, args, 5000);
    // Check for success indicators (encoded at least 1 frame) and no fatal errors
    bool hasError = output.find("Error while opening encoder") != std::string::npos ||
                    output.find("Driver does not support") != std::string::npos ||
                    output.find("Cannot load") != std::string::npos ||
                    output.find("No capable devices found") != std::string::npos;
    bool hasSuccess = output.find("frame=") != std::string::npos ||
                      output.find("video:") != std::string::npos;
    return hasSuccess && !hasError;
}

EncoderType Recorder::checkHardwareEncoders() {
    // First check which encoders are compiled in
    std::string output = runFFmpegProbe(ffmpegPath_, "-hide_banner -encoders");
    if (output.empty()) {
        std::cerr << "[RECORDER] Failed to probe encoders, defaulting to x264" << std::endl;
        return EncoderType::X264;
    }

    // Then actually test each available encoder with the current GPU driver
    // Priority: NVENC > AMF > QSV > X264
    if (output.find("h264_nvenc") != std::string::npos) {
        std::cerr << "[RECORDER] Testing h264_nvenc..." << std::endl;
        if (testEncoderWorks(ffmpegPath_, "h264_nvenc")) {
            std::cerr << "[RECORDER] h264_nvenc works!" << std::endl;
            return EncoderType::NVENC;
        }
        std::cerr << "[RECORDER] h264_nvenc not supported by current GPU driver" << std::endl;
    }
    if (output.find("h264_amf") != std::string::npos) {
        std::cerr << "[RECORDER] Testing h264_amf..." << std::endl;
        if (testEncoderWorks(ffmpegPath_, "h264_amf")) {
            std::cerr << "[RECORDER] h264_amf works!" << std::endl;
            return EncoderType::AMF;
        }
        std::cerr << "[RECORDER] h264_amf not supported by current GPU driver" << std::endl;
    }
    if (output.find("h264_qsv") != std::string::npos) {
        std::cerr << "[RECORDER] Testing h264_qsv..." << std::endl;
        if (testEncoderWorks(ffmpegPath_, "h264_qsv")) {
            std::cerr << "[RECORDER] h264_qsv works!" << std::endl;
            return EncoderType::QSV;
        }
        std::cerr << "[RECORDER] h264_qsv not supported by current GPU driver" << std::endl;
    }

    std::cerr << "[RECORDER] No hardware encoder available, using x264" << std::endl;
    return EncoderType::X264;
}

bool Recorder::isDdagrabAvailable() {
    std::string output = runFFmpegProbe(ffmpegPath_, "-hide_banner -filters");
    if (output.find("ddagrab") != std::string::npos) {
        return true;
    }
    return false;
}

std::string Recorder::buildFFmpegCommand(const RecordingConfig& config) {
    std::ostringstream cmd;

    cmd << "\"" << ffmpegPath_ << "\"";

    std::cerr << "[RECORDER] buildFFmpegCommand: width=" << config.width
              << " height=" << config.height
              << " fps=" << config.fps
              << " videoBitrate=" << config.videoBitrate
              << " captureAudio=" << config.captureAudio
              << " captureMicrophone=" << config.captureMicrophone
              << " separateAudio=" << config.separateAudio
              << " audioBitrate=" << config.audioBitrate
              << " ddagrab=" << ddagrabAvailable_
              << " captureHwnd=" << config.captureHwnd << std::endl;

    // Global options
    cmd << " -y";
    cmd << " -rtbufsize 100M";

    // ===== VIDEO INPUT =====
    // Use ddagrab (DXGI Desktop Duplication) when available and not doing window capture.
    // ddagrab respects SetWindowDisplayAffinity(WDA_EXCLUDEFROMCAPTURE) so overlay is hidden.
    // Fallback to gdigrab for window capture or when ddagrab is unavailable.
    bool useDdagrab = ddagrabAvailable_ && (config.captureHwnd == 0);

    if (useDdagrab) {
        cmd << " -f lavfi -i \"ddagrab=output_idx=0"
            << ":framerate=" << config.fps
            << ":draw_mouse=1"
            << ":video_size=" << config.width << "x" << config.height
            << "\"";
        std::cerr << "[RECORDER] Using ddagrab (DXGI) capture" << std::endl;
    } else {
        cmd << " -f gdigrab";
        cmd << " -framerate " << config.fps;
        cmd << " -draw_mouse 1";
        if (config.captureHwnd != 0) {
            cmd << " -i hwnd=" << config.captureHwnd;
            std::cerr << "[RECORDER] Using gdigrab hwnd capture: " << config.captureHwnd << std::endl;
        } else {
            cmd << " -offset_x 0 -offset_y 0";
            cmd << " -video_size " << config.width << "x" << config.height;
            cmd << " -i desktop";
            std::cerr << "[RECORDER] Using gdigrab desktop capture" << std::endl;
        }
    }

    cmd << " -use_wallclock_as_timestamps 0";
    cmd << " -fflags +genpts";

    // ===== AUDIO INPUT =====
    int audioInputCount = 0;

    // System audio via WASAPI named pipe (replaces VB-Audio Virtual Cable)
    if (config.captureAudio && systemAudioCapture_) {
        std::string pipePath = systemAudioCapture_->getPipePath();
        std::string fmt = systemAudioCapture_->getFFmpegFormatString();
        int sampleRate = systemAudioCapture_->getSampleRate();
        int channels = systemAudioCapture_->getChannels();

        cmd << " -f " << fmt;
        cmd << " -ar " << sampleRate;
        cmd << " -ac " << channels;
        cmd << " -i \"" << pipePath << "\"";
        audioInputCount++;
        std::cerr << "[RECORDER] System audio via WASAPI pipe: " << pipePath
                  << " fmt=" << fmt << " ar=" << sampleRate << " ac=" << channels << std::endl;
    }

    // Microphone is recorded to a separate file via a dedicated FFmpeg process.

    // ===== VIDEO ENCODER =====
    if (useDdagrab && encoderType_ != EncoderType::X264) {
        // Hardware encoder with ddagrab: need format conversion for some encoders
        // QSV requires nv12 format specifically
        switch (encoderType_) {
            case EncoderType::NVENC:
                // NVENC can encode d3d11 directly on Windows
                cmd << " -c:v h264_nvenc -preset p7 -tune hq";
                break;
            case EncoderType::AMF:
                // AMF can handle d3d11 on Windows
                cmd << " -c:v h264_amf -quality speed";
                break;
            case EncoderType::QSV:
                // QSV requires nv12 format - need to convert from d3d11
                cmd << " -vf format=nv12";
                cmd << " -c:v h264_qsv";
                break;
            default:
                break;
        }
    } else if (useDdagrab) {
        // Software encoder with ddagrab: need hwdownload to get frames from GPU
        cmd << " -vf hwdownload,format=bgra,format=yuv420p";
        cmd << " -c:v libx264 -preset ultrafast -tune zerolatency";
    } else {
        // gdigrab path: standard software/hardware encoder
        cmd << " -c:v ";
        switch (encoderType_) {
            case EncoderType::NVENC:
                cmd << "h264_nvenc -preset p7 -tune hq";
                break;
            case EncoderType::AMF:
                cmd << "h264_amf -quality speed";
                break;
            case EncoderType::QSV:
                cmd << "h264_qsv";
                break;
            default:
                cmd << "libx264 -preset ultrafast -tune zerolatency";
        }
    }

    // Video bitrate
    cmd << " -b:v " << config.videoBitrate << "k";
    cmd << " -maxrate " << (config.videoBitrate * 1.5) << "k";
    cmd << " -bufsize " << (config.videoBitrate * 2) << "k";

    // Audio encoder
    if (audioInputCount > 0) {
        cmd << " -c:a aac";
        cmd << " -b:a " << config.audioBitrate << "k";
        cmd << " -threads 2";
    }

    // Stream mapping
    if (audioInputCount == 1) {
        cmd << " -map 0:v -map 1:a";
    } else {
        cmd << " -map 0:v";
    }

    // Timestamp control
    cmd << " -avoid_negative_ts make_zero";
    cmd << " -vsync cfr";

    // Output
    cmd << " -f matroska \"" << config.savePath << "\"";

    return cmd.str();
}

bool Recorder::waitForFFmpegReady(HANDLE hStderrRead, int timeoutMs) {
    const auto startTime = std::chrono::steady_clock::now();
    std::string buffer;
    bool sawReadySignal = false;
    auto readyTime = std::chrono::steady_clock::now();
    // After seeing "Press [q] to stop", wait up to 500ms more to check for encoder errors
    const int postReadyCheckMs = 500;

    while (true) {
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - startTime
        ).count();

        if (elapsed > timeoutMs) {
            writeNativeLog(("F_TIMEOUT: No ready signal after " + std::to_string(timeoutMs) + "ms").c_str());
            return false;
        }

        // If we saw the ready signal, check if enough time passed without errors
        if (sawReadySignal) {
            auto sinceReady = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - readyTime
            ).count();
            if (sinceReady > postReadyCheckMs) {
                // Passed the post-ready check window — FFmpeg is truly recording
                return true;
            }
        }

        DWORD bytesAvailable = 0;
        if (PeekNamedPipe(hStderrRead, NULL, 0, NULL, &bytesAvailable, NULL) && bytesAvailable > 0) {
            char tempBuf[4096];
            DWORD bytesRead = 0;
            DWORD toRead = bytesAvailable < sizeof(tempBuf) ? bytesAvailable : sizeof(tempBuf);

            if (ReadFile(hStderrRead, tempBuf, toRead, &bytesRead, NULL) && bytesRead > 0) {
                buffer.append(tempBuf, bytesRead);

                std::string chunk(tempBuf, bytesRead);
                writeNativeLog(("F_STDERR: " + chunk).c_str());

                // Failure: FFmpeg encountered an error (check BEFORE success)
                if (buffer.find("Error opening input") != std::string::npos ||
                    buffer.find("Unknown input format") != std::string::npos ||
                    buffer.find("Cannot open") != std::string::npos ||
                    buffer.find("No such file or directory") != std::string::npos ||
                    buffer.find("Immediate exit requested") != std::string::npos ||
                    buffer.find("Failed to create output duplicator") != std::string::npos ||
                    buffer.find("Error while opening encoder") != std::string::npos ||
                    buffer.find("Driver does not support") != std::string::npos ||
                    buffer.find("Terminating thread with return code") != std::string::npos ||
                    (buffer.find("ddagrab") != std::string::npos && buffer.find("Error") != std::string::npos)) {
                    writeNativeLog("F_ERROR: FFmpeg reported an error in stderr");
                    return false;
                }

                // Success signal: mark ready but keep checking for errors
                if (!sawReadySignal &&
                    (buffer.find("Press [q] to stop") != std::string::npos ||
                     buffer.find("frame=") != std::string::npos)) {
                    sawReadySignal = true;
                    readyTime = std::chrono::steady_clock::now();
                    writeNativeLog("F_READY: Saw ready signal, waiting to confirm no errors...");
                }
            }
        } else {
            // No data available - check if process exited
            DWORD exitCode;
            if (GetExitCodeProcess(ffmpegProcess_.hProcess, &exitCode) && exitCode != STILL_ACTIVE) {
                // Read any remaining output
                if (PeekNamedPipe(hStderrRead, NULL, 0, NULL, &bytesAvailable, NULL) && bytesAvailable > 0) {
                    char tempBuf[4096];
                    DWORD bytesRead = 0;
                    DWORD toRead = bytesAvailable < sizeof(tempBuf) ? bytesAvailable : sizeof(tempBuf);
                    if (ReadFile(hStderrRead, tempBuf, toRead, &bytesRead, NULL) && bytesRead > 0) {
                        writeNativeLog(("F_STDERR(final): " + std::string(tempBuf, bytesRead)).c_str());
                    }
                }
                writeNativeLog(("F_ERROR: FFmpeg exited with code " + std::to_string(exitCode)).c_str());
                return false;
            }
        }

        Sleep(50);
    }
}

bool Recorder::startFFmpeg(const std::string& command) {
    writeNativeLog("F1: startFFmpeg() called, creating process...");

    // Create pipe to capture FFmpeg stderr
    HANDLE hStderrRead = NULL;
    HANDLE hStderrWrite = NULL;
    SECURITY_ATTRIBUTES sa = { sizeof(SECURITY_ATTRIBUTES), NULL, TRUE };

    if (!CreatePipe(&hStderrRead, &hStderrWrite, &sa, 0)) {
        writeNativeLog("F_ERROR: Failed to create stderr pipe");
        return false;
    }

    // Ensure the read end is not inherited by the child process
    SetHandleInformation(hStderrRead, HANDLE_FLAG_INHERIT, 0);

    // Create stdin pipe for sending commands to FFmpeg
    HANDLE hStdinRead = NULL;
    HANDLE hStdinWrite = NULL;

    if (!CreatePipe(&hStdinRead, &hStdinWrite, &sa, 0)) {
        writeNativeLog("F_ERROR: Failed to create stdin pipe");
        CloseHandle(hStderrRead);
        CloseHandle(hStderrWrite);
        return false;
    }

    // Ensure the read end is inherited by child, write end is not
    SetHandleInformation(hStdinWrite, HANDLE_FLAG_INHERIT, 0);

    // Store stdin write handle for later use in stopRecording()
    ffmpegStdin_ = hStdinWrite;

    STARTUPINFOA si = {};
    PROCESS_INFORMATION pi = {};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    si.hStdInput = hStdinRead;     // Redirect stdin from pipe
    si.hStdOutput = hStderrWrite;  // Redirect stdout to pipe
    si.hStdError = hStderrWrite;   // Redirect stderr to pipe

    char* cmdLine = _strdup(command.c_str());
    writeNativeLog(("F2: Creating FFmpeg process: " + command).c_str());

    if (!CreateProcessA(nullptr, cmdLine, nullptr, nullptr, TRUE,
                       CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        DWORD err = GetLastError();
        writeNativeLog(("F_ERROR: CreateProcess failed, error=" + std::to_string(err)).c_str());
        free(cmdLine);
        CloseHandle(hStderrRead);
        CloseHandle(hStderrWrite);
        CloseHandle(hStdinRead);
        CloseHandle(hStdinWrite);
        ffmpegStdin_ = nullptr;
        return false;
    }

    free(cmdLine);
    CloseHandle(hStderrWrite);  // Close write end - only child uses it
    CloseHandle(hStdinRead);    // Close read end - only child uses it
    ffmpegProcess_ = pi;

    writeNativeLog(("F3: FFmpeg process created, PID: " + std::to_string(pi.dwProcessId)).c_str());
    writeNativeLog("F4: Waiting for FFmpeg to start recording...");

    bool ready = waitForFFmpegReady(hStderrRead, 3000);

    CloseHandle(hStderrRead);

    if (!ready) {
        DWORD exitCode;
        if (GetExitCodeProcess(pi.hProcess, &exitCode) && exitCode != STILL_ACTIVE) {
            writeNativeLog("F_ERROR: FFmpeg exited early");
        } else {
            writeNativeLog("F_WARN: FFmpeg timeout, process still running - terminating");
            TerminateProcess(pi.hProcess, 1);
        }
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
        memset(&ffmpegProcess_, 0, sizeof(ffmpegProcess_));
        // Close stdin handle on error
        if (ffmpegStdin_) {
            CloseHandle(ffmpegStdin_);
            ffmpegStdin_ = nullptr;
        }
        return false;
    }

    writeNativeLog("F5: FFmpeg is recording!");
    return true;
}

bool Recorder::startRecording(const RecordingConfig& config) {
    writeNativeLog("N1: startRecording() called");

    std::cout << "[RECORDER] startRecording: BEGIN" << std::endl;
    std::cout << "[RECORDER] startRecording: current state=" << (int)state_ << std::endl;

    if (state_ != RecordingState::IDLE) {
        std::cerr << "[RECORDER] startRecording: not IDLE, returning false" << std::endl;
        return false;
    }

    std::cerr << "[RECORDER] startRecording: ffmpegPath_='" << ffmpegPath_ << "'" << std::endl;

    if (ffmpegPath_.empty()) {
        std::cerr << "[RECORDER] startRecording: FFmpeg not initialized" << std::endl;
        return false;
    }

    config_ = config;
    outputPath_ = config.savePath;

    // Initialize segment tracking
    finalOutputPath_ = config.savePath;
    segmentCounter_ = 0;
    segmentPaths_.clear();
    currentSegmentPath_ = generateSegmentPath();

    // Initialize mic segment tracking
    micSegmentCounter_ = 0;
    micSegmentPaths_.clear();
    currentMicSegmentPath_.clear();
    finalMicAudioPath_.clear();

    // Start WASAPI system audio capture via named pipe
    if (config.captureAudio) {
        if (!startSystemAudioCapture()) {
            std::cerr << "[RECORDER] Warning: WASAPI system audio capture failed, continuing without" << std::endl;
        }
    }

    if (config.captureMicrophone) {
        std::string basePath = config.savePath;
        size_t dotPos = basePath.rfind('.');
        if (dotPos != std::string::npos) {
            finalMicAudioPath_ = basePath.substr(0, dotPos) + "_mic.m4a";
        } else {
            finalMicAudioPath_ = basePath + "_mic.m4a";
        }
        currentMicSegmentPath_ = generateMicSegmentPath();
    }

    std::cerr << "[RECORDER] startRecording: outputPath=" << outputPath_ << std::endl;
    std::cerr << "[RECORDER] startRecording: segment=" << currentSegmentPath_ << std::endl;
    std::cerr << "[RECORDER] startRecording: width=" << config.width << " height=" << config.height << " fps=" << config.fps << std::endl;

    // Build FFmpeg command using segment path
    RecordingConfig segConfig = config;
    segConfig.savePath = currentSegmentPath_;
    std::string command = buildFFmpegCommand(segConfig);
    std::cerr << "[RECORDER] startRecording: FFmpeg command: " << command << std::endl;

    writeNativeLog("N2: Calling startFFmpeg()...");
    bool started = startFFmpeg(command);
    writeNativeLog((std::string("N3: startFFmpeg() returned: ") + (started ? "true" : "false")).c_str());
    std::cerr << "[RECORDER] startRecording: startFFmpeg returned " << started << std::endl;

    // If ddagrab failed, try falling back to gdigrab
    if (!started && ddagrabAvailable_ && config.captureHwnd == 0) {
        std::cerr << "[RECORDER] startRecording: ddagrab failed, falling back to gdigrab" << std::endl;
        writeNativeLog("N3_FALLBACK: ddagrab failed, retrying with gdigrab");

        // Restart audio capture to recreate the named pipe (previous pipe was disconnected)
        if (config.captureAudio) {
            stopSystemAudioCapture();
            if (!startSystemAudioCapture()) {
                std::cerr << "[RECORDER] Warning: Could not restart audio capture for fallback" << std::endl;
            }
        }

        ddagrabAvailable_ = false;
        segConfig.savePath = currentSegmentPath_;
        command = buildFFmpegCommand(segConfig);
        started = startFFmpeg(command);
    }

    // If failed and audio was enabled, try without system audio
    if (!started && config.captureAudio) {
        std::cerr << "[RECORDER] startRecording: FFmpeg failed with audio, retrying without system audio" << std::endl;
        stopSystemAudioCapture();
        RecordingConfig noSysAudioConfig = config;
        noSysAudioConfig.captureAudio = false;
        noSysAudioConfig.savePath = currentSegmentPath_;
        command = buildFFmpegCommand(noSysAudioConfig);
        std::cerr << "[RECORDER] startRecording: Retry FFmpeg command: " << command << std::endl;
        started = startFFmpeg(command);
        if (started) {
            config_ = noSysAudioConfig;
            config_.savePath = finalOutputPath_;
            std::cerr << "[RECORDER] startRecording: Recording started successfully (without system audio)" << std::endl;
        }
    }

    if (!started) {
        std::cerr << "[RECORDER] startRecording: startFFmpeg failed" << std::endl;
        stopSystemAudioCapture();
        return false;
    }

    // Start microphone FFmpeg process if enabled
    if (config_.captureMicrophone && !finalMicAudioPath_.empty()) {
        RecordingConfig micConfig = config_;
        micConfig.savePath = currentMicSegmentPath_;
        std::string micCmd = buildMicFFmpegCommand(micConfig);
        writeNativeLog(("N_MIC: Starting mic FFmpeg: " + micCmd).c_str());
        bool micStarted = startMicFFmpeg(micCmd);
        if (!micStarted) {
            writeNativeLog("N_MIC_WARN: Mic FFmpeg failed to start, continuing without mic");
            finalMicAudioPath_.clear();
        }
    }

    state_ = RecordingState::RECORDING;
    totalPausedDuration_ = 0;

    writeNativeLog("N4: Recording started successfully");
    std::cerr << "[RECORDER] startRecording: Recording started successfully" << std::endl;
    return true;
}

void Recorder::stopRecording() {
    if (state_ == RecordingState::IDLE) return;

    writeNativeLog("S1: stopRecording() called");

    // If FFmpeg is currently running (RECORDING state), stop it and save the last segment
    if (state_ == RecordingState::RECORDING && ffmpegProcess_.hProcess) {
        stopFFmpegGracefully();
        segmentPaths_.push_back(currentSegmentPath_);
        writeNativeLog(("S2: Saved final segment: " + currentSegmentPath_).c_str());
    }
    // If PAUSED, FFmpeg is already stopped and segment already saved in pauseRecording()

    // Stop WASAPI system audio capture (after FFmpeg stopped to avoid broken pipe)
    stopSystemAudioCapture();

    // Stop mic FFmpeg if running
    if (state_ == RecordingState::RECORDING && micFfmpegProcess_.hProcess) {
        writeNativeLog("S2_MIC: Stopping mic FFmpeg");
        stopMicFFmpegGracefully();
        micSegmentPaths_.push_back(currentMicSegmentPath_);
        writeNativeLog(("S2_MIC: Saved final mic segment: " + currentMicSegmentPath_).c_str());
    }

    // Handle segment concatenation
    if (segmentPaths_.size() > 1) {
        writeNativeLog(("S3: Concatenating " + std::to_string(segmentPaths_.size()) + " segments").c_str());
        if (concatenateSegments()) {
            writeNativeLog("S4: Concatenation successful, cleaning up segments");
            cleanupSegmentFiles();
        } else {
            writeNativeLog("S4: Concatenation FAILED, keeping segment files");
            // Fallback: rename first segment as the output
            if (!segmentPaths_.empty()) {
                std::rename(segmentPaths_[0].c_str(), finalOutputPath_.c_str());
                writeNativeLog("S4: Fallback - renamed first segment to final output");
            }
        }
    } else if (segmentPaths_.size() == 1) {
        // Single segment: just rename to final output path
        writeNativeLog("S3: Single segment, renaming to final output");
        std::rename(segmentPaths_[0].c_str(), finalOutputPath_.c_str());
    }

    // Handle mic segment concatenation
    if (!finalMicAudioPath_.empty()) {
        if (micSegmentPaths_.size() > 1) {
            writeNativeLog(("S3_MIC: Concatenating " + std::to_string(micSegmentPaths_.size()) + " mic segments").c_str());
            if (concatenateMicSegments()) {
                writeNativeLog("S4_MIC: Mic concatenation successful");
                cleanupMicSegmentFiles();
            } else {
                writeNativeLog("S4_MIC: Mic concatenation FAILED");
                if (!micSegmentPaths_.empty()) {
                    std::rename(micSegmentPaths_[0].c_str(), finalMicAudioPath_.c_str());
                }
            }
        } else if (micSegmentPaths_.size() == 1) {
            writeNativeLog("S3_MIC: Single mic segment, renaming to final output");
            std::rename(micSegmentPaths_[0].c_str(), finalMicAudioPath_.c_str());
        }
    }

    // Reset segment state
    segmentPaths_.clear();
    currentSegmentPath_.clear();
    segmentCounter_ = 0;
    finalOutputPath_.clear();

    // Reset mic segment state
    micSegmentPaths_.clear();
    currentMicSegmentPath_.clear();
    micSegmentCounter_ = 0;
    // Keep finalMicAudioPath_ so getMicAudioPath() can return it after stop

    state_ = RecordingState::IDLE;
    std::cout << "[RECORDER] Recording stopped" << std::endl;

    if (stopCallback_) {
        stopCallback_(stopCallbackData_);
    }
}

void Recorder::pauseRecording() {
    std::cerr << "[RECORDER] pauseRecording() called, state=" << (int)state_ << std::endl;

    if (state_ != RecordingState::RECORDING) {
        std::cerr << "[RECORDER] pauseRecording() - NOT RECORDING, returning" << std::endl;
        return;
    }

    std::cerr << "[RECORDER] pauseRecording() - calling stopFFmpegGracefully()" << std::endl;
    writeNativeLog("P1: pauseRecording() - stopping FFmpeg for current segment");

    // Gracefully stop the current FFmpeg process
    stopFFmpegGracefully();

    std::cerr << "[RECORDER] pauseRecording() - stopFFmpegGracefully() returned" << std::endl;

    // Save the completed segment path
    std::cerr << "[RECORDER] pauseRecording() - pushing segment: " << currentSegmentPath_ << std::endl;
    segmentPaths_.push_back(currentSegmentPath_);
    writeNativeLog(("P2: Segment saved: " + currentSegmentPath_).c_str());

    // Stop WASAPI system audio capture
    stopSystemAudioCapture();

    // Stop mic FFmpeg and save mic segment
    if (!finalMicAudioPath_.empty() && micFfmpegProcess_.hProcess) {
        writeNativeLog("P2_MIC: Stopping mic FFmpeg for pause");
        stopMicFFmpegGracefully();
        micSegmentPaths_.push_back(currentMicSegmentPath_);
        writeNativeLog(("P2_MIC: Mic segment saved: " + currentMicSegmentPath_).c_str());
    }

    pauseBeginTime_ = getHighPrecisionTimestamp();
    state_ = RecordingState::PAUSED;
    std::cerr << "[RECORDER] pauseRecording() - DONE, state now PAUSED" << std::endl;
    std::cout << "[RECORDER] Recording paused (segment " << segmentPaths_.size() << " saved)" << std::endl;
}

void Recorder::resumeRecording() {
    std::cerr << "[RECORDER] resumeRecording() called, state=" << (int)state_ << std::endl;

    if (state_ != RecordingState::PAUSED) {
        std::cerr << "[RECORDER] resumeRecording() - NOT PAUSED, returning" << std::endl;
        return;
    }

    std::cerr << "[RECORDER] resumeRecording() - generating new segment path" << std::endl;
    writeNativeLog("R1: resumeRecording() - starting new segment");

    // Generate new segment path
    currentSegmentPath_ = generateSegmentPath();
    std::cerr << "[RECORDER] resumeRecording() - new segment: " << currentSegmentPath_ << std::endl;
    writeNativeLog(("R2: New segment path: " + currentSegmentPath_).c_str());

    // Restart WASAPI system audio capture for the new segment
    if (config_.captureAudio) {
        if (!startSystemAudioCapture()) {
            std::cerr << "[RECORDER] resumeRecording: WASAPI restart failed" << std::endl;
        }
    }

    // Build FFmpeg command for the new segment
    RecordingConfig segConfig = config_;
    segConfig.savePath = currentSegmentPath_;
    std::string command = buildFFmpegCommand(segConfig);

    std::cerr << "[RECORDER] resumeRecording() - starting FFmpeg" << std::endl;
    bool started = startFFmpeg(command);

    // If failed and audio was enabled, retry without audio
    if (!started && config_.captureAudio) {
        std::cerr << "[RECORDER] resumeRecording() - FFmpeg failed with audio, retrying without" << std::endl;
        writeNativeLog("R3: FFmpeg failed with audio, retrying without audio");
        stopSystemAudioCapture();
        RecordingConfig noAudioConfig = config_;
        noAudioConfig.captureAudio = false;
        noAudioConfig.savePath = currentSegmentPath_;
        command = buildFFmpegCommand(noAudioConfig);
        started = startFFmpeg(command);
    }

    if (!started) {
        std::cerr << "[RECORDER] resumeRecording() - FAILED to start FFmpeg" << std::endl;
        writeNativeLog("R_ERROR: Failed to start new FFmpeg segment, staying PAUSED");
        stopSystemAudioCapture();
        return;
    }

    std::cerr << "[RECORDER] resumeRecording() - FFmpeg started successfully" << std::endl;

    // Start new mic segment if mic is enabled
    if (!finalMicAudioPath_.empty()) {
        currentMicSegmentPath_ = generateMicSegmentPath();
        RecordingConfig micConfig = config_;
        micConfig.savePath = currentMicSegmentPath_;
        std::string micCmd = buildMicFFmpegCommand(micConfig);
        writeNativeLog(("R_MIC: Starting new mic segment: " + currentMicSegmentPath_).c_str());
        bool micStarted = startMicFFmpeg(micCmd);
        if (!micStarted) {
            writeNativeLog("R_MIC_WARN: Mic FFmpeg failed to start on resume");
        }
    }

    totalPausedDuration_ += (getHighPrecisionTimestamp() - pauseBeginTime_);
    state_ = RecordingState::RECORDING;
    writeNativeLog("R4: New segment recording started");
    std::cout << "[RECORDER] Recording resumed (segment " << (segmentPaths_.size() + 1) << ")" << std::endl;
}

void Recorder::shutdown() {
    if (state_ == RecordingState::RECORDING || state_ == RecordingState::PAUSED) {
        stopRecording();
    }

    // Clean up stdin handle if still open
    if (ffmpegStdin_) {
        CloseHandle(ffmpegStdin_);
        ffmpegStdin_ = nullptr;
    }

    if (ffmpegProcess_.hProcess) {
        CloseHandle(ffmpegProcess_.hProcess);
        CloseHandle(ffmpegProcess_.hThread);
    }

    // Clean up mic FFmpeg handles
    if (micFfmpegStdin_) {
        CloseHandle(micFfmpegStdin_);
        micFfmpegStdin_ = nullptr;
    }

    if (micFfmpegProcess_.hProcess) {
        CloseHandle(micFfmpegProcess_.hProcess);
        CloseHandle(micFfmpegProcess_.hThread);
    }

    if (g_timelineLogFile.is_open()) {
        writeNativeLog("===== NATIVE CORE SHUTDOWN =====");
        g_timelineLogFile.close();
    }

    std::cout << "[RECORDER] Shutdown complete" << std::endl;
}

bool Recorder::isRecording() const { return state_ == RecordingState::RECORDING; }
bool Recorder::isPaused() const { return state_ == RecordingState::PAUSED; }
RecordingState Recorder::getState() const { return state_; }
EncoderType Recorder::getAvailableEncoder() const { return encoderType_; }
std::string Recorder::getOutputPath() const { return outputPath_; }
std::string Recorder::getMicAudioPath() const { return finalMicAudioPath_; }

void Recorder::setStopCallback(StopCallback callback, void* userData) {
    stopCallback_ = callback;
    stopCallbackData_ = userData;
}

void Recorder::signal_stop(bool success) {
    (void)success;
    // This would be called when FFmpeg terminates
}

bool Recorder::isFFmpegAvailable() const {
    return !ffmpegPath_.empty();
}

bool Recorder::stopFFmpegGracefully(int timeoutMs) {
    std::cerr << "[RECORDER] stopFFmpegGracefully() called" << std::endl;
    writeNativeLog("FFG1: stopFFmpegGracefully() called");

    if (ffmpegProcess_.hProcess && ffmpegStdin_) {
        std::cerr << "[RECORDER] stopFFmpegGracefully() - sending 'q' to FFmpeg stdin" << std::endl;
        // Send 'q' to FFmpeg stdin to request graceful shutdown
        const char quitCmd = 'q';
        DWORD bytesWritten = 0;
        if (!WriteFile(ffmpegStdin_, &quitCmd, 1, &bytesWritten, NULL)) {
            std::cerr << "[RECORDER] ERROR: Failed to write to FFmpeg stdin. Error: " << GetLastError() << std::endl;
            writeNativeLog("FFG2_ERROR: Failed to write 'q' to FFmpeg stdin, forcing termination.");
            // Proceed to forceful termination if write fails
            TerminateProcess(ffmpegProcess_.hProcess, 0);
            WaitForSingleObject(ffmpegProcess_.hProcess, 3000);
            CloseHandle(ffmpegStdin_);
            ffmpegStdin_ = nullptr;
            // Clean up process handles
            if (ffmpegProcess_.hProcess) {
                CloseHandle(ffmpegProcess_.hProcess);
                CloseHandle(ffmpegProcess_.hThread);
                memset(&ffmpegProcess_, 0, sizeof(ffmpegProcess_));
            }
            writeNativeLog("FFG4: FFmpeg process cleaned up (write failed)");
            return false; // Indicate failure to gracefully stop
        }
        if (!FlushFileBuffers(ffmpegStdin_)) {
            std::cerr << "[RECORDER] ERROR: Failed to flush FFmpeg stdin. Error: " << GetLastError() << std::endl;
            writeNativeLog("FFG2_ERROR: Failed to flush FFmpeg stdin, forcing termination.");
            // Proceed to forceful termination if flush fails
            TerminateProcess(ffmpegProcess_.hProcess, 0);
            WaitForSingleObject(ffmpegProcess_.hProcess, 3000);
            CloseHandle(ffmpegStdin_);
            ffmpegStdin_ = nullptr;
            // Clean up process handles
            if (ffmpegProcess_.hProcess) {
                CloseHandle(ffmpegProcess_.hProcess);
                CloseHandle(ffmpegProcess_.hThread);
                memset(&ffmpegProcess_, 0, sizeof(ffmpegProcess_));
            }
            writeNativeLog("FFG4: FFmpeg process cleaned up (flush failed)");
            return false; // Indicate failure to gracefully stop
        }
        CloseHandle(ffmpegStdin_);
        ffmpegStdin_ = nullptr;
        std::cerr << "[RECORDER] stopFFmpegGracefully() - waiting for FFmpeg to exit..." << std::endl;
        writeNativeLog("FFG2: Sent 'q' to FFmpeg, waiting for graceful exit...");

        DWORD waitResult = WaitForSingleObject(ffmpegProcess_.hProcess, (DWORD)timeoutMs);

        if (waitResult == WAIT_TIMEOUT) {
            std::cerr << "[RECORDER] stopFFmpegGracefully() - TIMEOUT, forcing termination" << std::endl;
            writeNativeLog("FFG3: FFmpeg did not exit gracefully, forcing termination");
            TerminateProcess(ffmpegProcess_.hProcess, 0);
            WaitForSingleObject(ffmpegProcess_.hProcess, 3000);
        } else {
            std::cerr << "[RECORDER] stopFFmpegGracefully() - FFmpeg exited gracefully" << std::endl;
            writeNativeLog("FFG3: FFmpeg exited gracefully");
        }
    } else if (ffmpegProcess_.hProcess) {
        std::cerr << "[RECORDER] stopFFmpegGracefully() - no stdin handle, forcing termination" << std::endl;
        writeNativeLog("FFG2: No stdin handle, forcing termination");
        TerminateProcess(ffmpegProcess_.hProcess, 0);
        WaitForSingleObject(ffmpegProcess_.hProcess, 3000);
    } else {
        std::cerr << "[RECORDER] stopFFmpegGracefully() - no FFmpeg process to stop" << std::endl;
        writeNativeLog("FFG2: No FFmpeg process to stop");
        return true;
    }

    // Clean up process handles
    if (ffmpegProcess_.hProcess) {
        std::cerr << "[RECORDER] stopFFmpegGracefully() - cleaning up process handles" << std::endl;
        CloseHandle(ffmpegProcess_.hProcess);
        CloseHandle(ffmpegProcess_.hThread);
        memset(&ffmpegProcess_, 0, sizeof(ffmpegProcess_));
    }

    writeNativeLog("FFG4: FFmpeg process cleaned up");
    return true;
}

std::string Recorder::generateSegmentPath() {
    segmentCounter_++;

    std::cerr << "[RECORDER] generateSegmentPath() - finalOutputPath_='" << finalOutputPath_ << "'" << std::endl;

    // Extract directory and base name from finalOutputPath_
    std::string dir, baseName, ext;
    size_t lastSlash = finalOutputPath_.rfind('\\');
    if (lastSlash == std::string::npos) {
        lastSlash = finalOutputPath_.rfind('/');
    }

    if (lastSlash != std::string::npos) {
        dir = finalOutputPath_.substr(0, lastSlash + 1);
        baseName = finalOutputPath_.substr(lastSlash + 1);
    } else {
        dir = "";
        baseName = finalOutputPath_;
    }

    // Remove extension
    size_t dotPos = baseName.rfind('.');
    if (dotPos != std::string::npos) {
        ext = baseName.substr(dotPos);
        baseName = baseName.substr(0, dotPos);
    } else {
        ext = ".mkv";
    }

    // Generate segment path: recording_seg001.mkv, recording_seg002.mkv, etc.
    char segNum[16];
    snprintf(segNum, sizeof(segNum), "_seg%03d", segmentCounter_);

    std::string segPath = dir + baseName + segNum + ext;
    std::cerr << "[RECORDER] generateSegmentPath() - dir='" << dir << "' baseName='" << baseName << "' ext='" << ext << "' => segment=" << segPath << std::endl;
    writeNativeLog(("Generated segment path: " + segPath).c_str());
    return segPath;
}

bool Recorder::concatenateSegments() {
    if (segmentPaths_.empty()) return false;

    writeNativeLog(("CONCAT: Concatenating " + std::to_string(segmentPaths_.size()) + " segments").c_str());

    // Write concat list file
    std::string listPath = finalOutputPath_ + ".concat_list.txt";
    {
        std::ofstream listFile(listPath);
        if (!listFile.is_open()) {
            writeNativeLog("CONCAT_ERROR: Failed to create concat list file");
            return false;
        }
        for (const auto& seg : segmentPaths_) {
            // Use forward slashes and escape single quotes for ffmpeg concat
            std::string escapedPath = seg;
            // Replace backslashes with forward slashes
            for (auto& c : escapedPath) {
                if (c == '\\') c = '/';
            }
            listFile << "file '" << escapedPath << "'\n";
        }
        listFile.close();
    }

    // Build concat command
    std::string concatCmd = "\"" + ffmpegPath_ + "\""
        + " -y -f concat -safe 0"
        + " -i \"" + listPath + "\""
        + " -c copy"
        + " \"" + finalOutputPath_ + "\"";

    writeNativeLog(("CONCAT: Command: " + concatCmd).c_str());

    // Execute concat synchronously
    STARTUPINFOA si = {};
    PROCESS_INFORMATION pi = {};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW | STARTF_USESTDHANDLES;
    si.wShowWindow = SW_HIDE;

    // Create pipes for stderr capture
    SECURITY_ATTRIBUTES sa = { sizeof(SECURITY_ATTRIBUTES), NULL, TRUE };
    HANDLE stderrRead, stderrWrite;
    CreatePipe(&stderrRead, &stderrWrite, &sa, 0);
    si.hStdError = stderrWrite;

    char* cmdLine = _strdup(concatCmd.c_str());
    bool success = false;
    std::string errorOutput;

    if (CreateProcessA(nullptr, cmdLine, nullptr, nullptr, TRUE,
                       CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        // Close stderr write handle in parent
        CloseHandle(stderrWrite);
        stderrWrite = nullptr;

        // Wait for concat to finish (should be fast since -c copy)
        DWORD waitResult = WaitForSingleObject(pi.hProcess, 60000);  // 60s timeout

        // Read stderr output
        char buffer[4096];
        DWORD bytesRead;
        while (ReadFile(stderrRead, buffer, sizeof(buffer) - 1, &bytesRead, NULL) && bytesRead > 0) {
            buffer[bytesRead] = 0;
            errorOutput += buffer;
        }
        CloseHandle(stderrRead);
        stderrRead = nullptr;

        if (waitResult == WAIT_TIMEOUT) {
            writeNativeLog("CONCAT_ERROR: Concat process timed out");
            TerminateProcess(pi.hProcess, 1);
        } else {
            DWORD exitCode = 0;
            GetExitCodeProcess(pi.hProcess, &exitCode);
            if (exitCode == 0) {
                writeNativeLog("CONCAT: Success");
                success = true;
            } else {
                writeNativeLog(("CONCAT_ERROR: FFmpeg exited with code " + std::to_string(exitCode)).c_str());
                // Log detailed error output
                if (!errorOutput.empty()) {
                    writeNativeLog(("CONCAT_ERROR: FFmpeg stderr: " + errorOutput).c_str());
                }
            }
        }

        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
    } else {
        writeNativeLog(("CONCAT_ERROR: Failed to create process, error=" + std::to_string(GetLastError())).c_str());
    }

    free(cmdLine);

    // Clean up list file
    std::remove(listPath.c_str());

    if (!success) {
        // 合并失败时，将分段文件移动到临时目录保留
        writeNativeLog("CONCAT_ERROR: Concatenation failed, preserving segment files for recovery");
        std::string failedDir = finalOutputPath_ + "_failed_segments";
        CreateDirectoryA(failedDir.c_str(), NULL);
        for (const auto& seg : segmentPaths_) {
            std::string destPath = failedDir + "\\" + seg.substr(seg.find_last_of("\\/") + 1);
            MoveFileA(seg.c_str(), destPath.c_str());
            writeNativeLog(("CONCAT: Moved segment to " + destPath).c_str());
        }
        writeNativeLog(("CONCAT: All segments preserved in " + failedDir).c_str());
    }

    return success;
}

void Recorder::cleanupSegmentFiles() {
    writeNativeLog(("CLEANUP: Removing " + std::to_string(segmentPaths_.size()) + " segment files").c_str());
    for (const auto& seg : segmentPaths_) {
        if (std::remove(seg.c_str()) == 0) {
            writeNativeLog(("CLEANUP: Removed " + seg).c_str());
        } else {
            writeNativeLog(("CLEANUP_WARN: Failed to remove " + seg).c_str());
        }
    }
}

// ===== Microphone separate FFmpeg process =====

std::string Recorder::buildMicFFmpegCommand(const RecordingConfig& config) {
    std::ostringstream cmd;
    cmd << "\"" << ffmpegPath_ << "\"";
    cmd << " -y";
    cmd << " -thread_queue_size 512";

    // For microphone, use dshow to capture audio input device
    // Use "audio=device_name" format for dshow

    // Escape microphone device name if it contains special characters
    std::string escapedMicDevice = config.microphoneDevice;
    if (!escapedMicDevice.empty()) {
        // Check if escaping is needed
        if (escapedMicDevice.find(' ') != std::string::npos ||
            escapedMicDevice.find('(') != std::string::npos ||
            escapedMicDevice.find(')') != std::string::npos ||
            escapedMicDevice.find('&') != std::string::npos ||
            escapedMicDevice.find('"') != std::string::npos) {
            // Replace internal double quotes with two double quotes
            size_t pos = escapedMicDevice.find('"');
            while (pos != std::string::npos) {
                escapedMicDevice.replace(pos, 1, "\"\"");
                pos = escapedMicDevice.find('"', pos + 2);
            }
            // Wrap in double quotes
            escapedMicDevice = "\"" + escapedMicDevice + "\"";
        }
    }

    cmd << " -f dshow -i audio";
    if (!config.microphoneDevice.empty()) {
        // Use double quotes around device name - FFmpeg dshow expects exact name
        cmd << "=" << escapedMicDevice;
    } else {
        // Use default audio input device (empty string after audio=)
        cmd << "=\"\"";
    }

    // AAC encoding for output audio
    cmd << " -c:a aac";
    cmd << " -b:a " << config.audioBitrate << "k";
    cmd << " \"" << config.savePath << "\"";

    std::string result = cmd.str();
    writeNativeLog(("MIC_BUILD: FFmpeg mic command: " + result).c_str());
    return result;
}

bool Recorder::startMicFFmpeg(const std::string& command) {
    writeNativeLog("MIC_F1: startMicFFmpeg() called");
    writeNativeLog(("MIC_F1b: Mic device: '" + config_.microphoneDevice + "'").c_str());
    writeNativeLog(("MIC_F1c: Using FFmpeg: " + ffmpegPath_).c_str());

    HANDLE hStderrRead = NULL, hStderrWrite = NULL;
    SECURITY_ATTRIBUTES sa = { sizeof(SECURITY_ATTRIBUTES), NULL, TRUE };

    if (!CreatePipe(&hStderrRead, &hStderrWrite, &sa, 0)) {
        writeNativeLog("MIC_F_ERROR: Failed to create stderr pipe");
        return false;
    }
    SetHandleInformation(hStderrRead, HANDLE_FLAG_INHERIT, 0);

    HANDLE hStdinRead = NULL, hStdinWrite = NULL;
    if (!CreatePipe(&hStdinRead, &hStdinWrite, &sa, 0)) {
        writeNativeLog("MIC_F_ERROR: Failed to create stdin pipe");
        CloseHandle(hStderrRead);
        CloseHandle(hStderrWrite);
        return false;
    }
    SetHandleInformation(hStdinWrite, HANDLE_FLAG_INHERIT, 0);

    micFfmpegStdin_ = hStdinWrite;

    STARTUPINFOA si = {};
    PROCESS_INFORMATION pi = {};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    si.hStdInput = hStdinRead;
    si.hStdOutput = hStderrWrite;
    si.hStdError = hStderrWrite;

    char* cmdLine = _strdup(command.c_str());
    writeNativeLog(("MIC_F2: Creating mic FFmpeg process: " + command).c_str());

    if (!CreateProcessA(nullptr, cmdLine, nullptr, nullptr, TRUE,
                       CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        DWORD err = GetLastError();
        writeNativeLog(("MIC_F_ERROR: CreateProcess failed, error=" + std::to_string(err)).c_str());
        free(cmdLine);
        CloseHandle(hStderrRead);
        CloseHandle(hStderrWrite);
        CloseHandle(hStdinRead);
        CloseHandle(hStdinWrite);
        micFfmpegStdin_ = nullptr;
        return false;
    }

    free(cmdLine);
    CloseHandle(hStderrWrite);
    CloseHandle(hStdinRead);
    micFfmpegProcess_ = pi;

    writeNativeLog(("MIC_F3: Mic FFmpeg PID: " + std::to_string(pi.dwProcessId)).c_str());

    // Collect stderr output for detailed diagnosis
    std::string stderrOutput;
    bool seenDeviceList = false;

    // Wait for mic FFmpeg to be ready with extended timeout and better error detection
    const auto startTime = std::chrono::steady_clock::now();
    const int timeoutMs = 5000;
    std::string buffer;
    DWORD exitCode = 0;

    while (true) {
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - startTime
        ).count();

        if (elapsed > timeoutMs) {
            writeNativeLog("MIC_F_WARN: Mic FFmpeg timeout waiting for ready signal");
            break;
        }

        DWORD bytesAvailable = 0;
        if (PeekNamedPipe(hStderrRead, NULL, 0, NULL, &bytesAvailable, NULL) && bytesAvailable > 0) {
            char tempBuf[4096];
            DWORD bytesRead = 0;
            DWORD toRead = bytesAvailable < sizeof(tempBuf) ? bytesAvailable : sizeof(tempBuf);

            if (ReadFile(hStderrRead, tempBuf, toRead, &bytesRead, NULL) && bytesRead > 0) {
                std::string chunk(tempBuf, bytesRead);
                stderrOutput += chunk;

                // Log stderr chunks for debugging
                writeNativeLog(("MIC_F_STDERR: " + chunk).c_str());

                // Success: FFmpeg is ready
                if (chunk.find("Press [q] to stop") != std::string::npos ||
                    chunk.find("frame=") != std::string::npos) {
                    writeNativeLog("MIC_F4: Mic FFmpeg is recording (ready signal detected)");
                    CloseHandle(hStderrRead);
                    return true;
                }

                // Check for specific dshow device errors
                if (chunk.find("Invalid data found") != std::string::npos ||
                    chunk.find("Unknown input format") != std::string::npos ||
                    chunk.find("No such device") != std::string::npos ||
                    chunk.find("cannot open") != std::string::npos ||
                    chunk.find("The system cannot find") != std::string::npos) {
                    writeNativeLog("MIC_F_ERROR: Mic FFmpeg device open failed");
                    break;
                }
            }
        }

        // Check if process exited
        if (GetExitCodeProcess(pi.hProcess, &exitCode) && exitCode != STILL_ACTIVE) {
            writeNativeLog(("MIC_F_ERROR: Mic FFmpeg exited early with code " + std::to_string(exitCode)).c_str());
            // Read remaining stderr
            if (PeekNamedPipe(hStderrRead, NULL, 0, NULL, &bytesAvailable, NULL) && bytesAvailable > 0) {
                char tempBuf[4096];
                DWORD bytesRead = 0;
                if (ReadFile(hStderrRead, tempBuf, sizeof(tempBuf) - 1, &bytesRead, NULL) && bytesRead > 0) {
                    tempBuf[bytesRead] = '\0';
                    stderrOutput += tempBuf;
                    writeNativeLog(("MIC_F_STDERR(final): " + std::string(tempBuf, bytesRead)).c_str());
                }
            }
            break;
        }

        Sleep(20);
    }

    CloseHandle(hStderrRead);

    // Process failed - clean up
    if (GetExitCodeProcess(pi.hProcess, &exitCode) && exitCode != STILL_ACTIVE) {
        writeNativeLog(("MIC_F_ERROR: Mic FFmpeg exited with code " + std::to_string(exitCode)).c_str());
    } else {
        writeNativeLog("MIC_F_WARN: Mic FFmpeg timeout - terminating");
        TerminateProcess(pi.hProcess, 1);
        WaitForSingleObject(pi.hProcess, 3000);
    }
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    memset(&micFfmpegProcess_, 0, sizeof(micFfmpegProcess_));
    if (micFfmpegStdin_) {
        CloseHandle(micFfmpegStdin_);
        micFfmpegStdin_ = nullptr;
    }

    // Log diagnostic info
    writeNativeLog(("MIC_F_DONE: Mic FFmpeg failed to start. Stderr length: " + std::to_string(stderrOutput.size())).c_str());
    if (!stderrOutput.empty()) {
        writeNativeLog(("MIC_F_DONE: Last stderr: " + stderrOutput.substr(0, 500)).c_str());
    }

    return false;
}

bool Recorder::stopMicFFmpegGracefully(int timeoutMs) {
    writeNativeLog("MIC_FFG1: stopMicFFmpegGracefully() called");

    if (micFfmpegProcess_.hProcess && micFfmpegStdin_) {
        const char quitCmd = 'q';
        DWORD bytesWritten = 0;
        WriteFile(micFfmpegStdin_, &quitCmd, 1, &bytesWritten, NULL);
        FlushFileBuffers(micFfmpegStdin_);
        CloseHandle(micFfmpegStdin_);
        micFfmpegStdin_ = nullptr;
        writeNativeLog("MIC_FFG2: Sent 'q', waiting for exit...");

        DWORD waitResult = WaitForSingleObject(micFfmpegProcess_.hProcess, (DWORD)timeoutMs);
        if (waitResult == WAIT_TIMEOUT) {
            writeNativeLog("MIC_FFG3: Timeout, forcing termination");
            TerminateProcess(micFfmpegProcess_.hProcess, 0);
            WaitForSingleObject(micFfmpegProcess_.hProcess, 3000);
        } else {
            writeNativeLog("MIC_FFG3: Exited gracefully");
        }
    } else if (micFfmpegProcess_.hProcess) {
        writeNativeLog("MIC_FFG2: No stdin, forcing termination");
        TerminateProcess(micFfmpegProcess_.hProcess, 0);
        WaitForSingleObject(micFfmpegProcess_.hProcess, 3000);
    } else {
        writeNativeLog("MIC_FFG2: No mic FFmpeg process to stop");
        return true;
    }

    if (micFfmpegProcess_.hProcess) {
        CloseHandle(micFfmpegProcess_.hProcess);
        CloseHandle(micFfmpegProcess_.hThread);
        memset(&micFfmpegProcess_, 0, sizeof(micFfmpegProcess_));
    }

    writeNativeLog("MIC_FFG4: Mic FFmpeg cleaned up");
    return true;
}

std::string Recorder::generateMicSegmentPath() {
    micSegmentCounter_++;

    std::string dir, baseName, ext;
    size_t lastSlash = finalMicAudioPath_.rfind('\\');
    if (lastSlash == std::string::npos) {
        lastSlash = finalMicAudioPath_.rfind('/');
    }

    if (lastSlash != std::string::npos) {
        dir = finalMicAudioPath_.substr(0, lastSlash + 1);
        baseName = finalMicAudioPath_.substr(lastSlash + 1);
    } else {
        dir = "";
        baseName = finalMicAudioPath_;
    }

    size_t dotPos = baseName.rfind('.');
    if (dotPos != std::string::npos) {
        ext = baseName.substr(dotPos);
        baseName = baseName.substr(0, dotPos);
    } else {
        ext = ".m4a";
    }

    char segNum[16];
    snprintf(segNum, sizeof(segNum), "_seg%03d", micSegmentCounter_);

    std::string segPath = dir + baseName + segNum + ext;
    writeNativeLog(("MIC: Generated segment path: " + segPath).c_str());
    return segPath;
}

bool Recorder::concatenateMicSegments() {
    if (micSegmentPaths_.empty()) return false;

    writeNativeLog(("MIC_CONCAT: Concatenating " + std::to_string(micSegmentPaths_.size()) + " mic segments").c_str());

    std::string listPath = finalMicAudioPath_ + ".concat_list.txt";
    {
        std::ofstream listFile(listPath);
        if (!listFile.is_open()) {
            writeNativeLog("MIC_CONCAT_ERROR: Failed to create concat list file");
            return false;
        }
        for (const auto& seg : micSegmentPaths_) {
            std::string escapedPath = seg;
            for (auto& c : escapedPath) {
                if (c == '\\') c = '/';
            }
            listFile << "file '" << escapedPath << "'\n";
        }
        listFile.close();
    }

    std::string concatCmd = "\"" + ffmpegPath_ + "\""
        + " -y -f concat -safe 0"
        + " -i \"" + listPath + "\""
        + " -c copy"
        + " \"" + finalMicAudioPath_ + "\"";

    writeNativeLog(("MIC_CONCAT: Command: " + concatCmd).c_str());

    STARTUPINFOA si = {};
    PROCESS_INFORMATION pi = {};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW | STARTF_USESTDHANDLES;
    si.wShowWindow = SW_HIDE;

    SECURITY_ATTRIBUTES sa = { sizeof(SECURITY_ATTRIBUTES), NULL, TRUE };
    HANDLE stderrRead, stderrWrite;
    CreatePipe(&stderrRead, &stderrWrite, &sa, 0);
    si.hStdError = stderrWrite;

    char* cmdLine = _strdup(concatCmd.c_str());
    bool success = false;
    std::string errorOutput;

    if (CreateProcessA(nullptr, cmdLine, nullptr, nullptr, TRUE,
                       CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        CloseHandle(stderrWrite);
        stderrWrite = nullptr;

        DWORD waitResult = WaitForSingleObject(pi.hProcess, 60000);

        char buffer[4096];
        DWORD bytesRead;
        while (ReadFile(stderrRead, buffer, sizeof(buffer) - 1, &bytesRead, NULL) && bytesRead > 0) {
            buffer[bytesRead] = 0;
            errorOutput += buffer;
        }
        CloseHandle(stderrRead);
        stderrRead = nullptr;

        if (waitResult == WAIT_TIMEOUT) {
            writeNativeLog("MIC_CONCAT_ERROR: Timed out");
            TerminateProcess(pi.hProcess, 1);
        } else {
            DWORD exitCode = 0;
            GetExitCodeProcess(pi.hProcess, &exitCode);
            if (exitCode == 0) {
                writeNativeLog("MIC_CONCAT: Success");
                success = true;
            } else {
                writeNativeLog(("MIC_CONCAT_ERROR: exit code " + std::to_string(exitCode)).c_str());
                if (!errorOutput.empty()) {
                    writeNativeLog(("MIC_CONCAT_ERROR: FFmpeg stderr: " + errorOutput).c_str());
                }
            }
        }
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
    } else {
        writeNativeLog(("MIC_CONCAT_ERROR: CreateProcess failed, error=" + std::to_string(GetLastError())).c_str());
    }

    free(cmdLine);
    std::remove(listPath.c_str());

    if (!success) {
        writeNativeLog("MIC_CONCAT_ERROR: Concatenation failed, preserving segment files for recovery");
        std::string failedDir = finalMicAudioPath_ + "_failed_segments";
        CreateDirectoryA(failedDir.c_str(), NULL);
        for (const auto& seg : micSegmentPaths_) {
            std::string destPath = failedDir + "\\" + seg.substr(seg.find_last_of("\\/") + 1);
            MoveFileA(seg.c_str(), destPath.c_str());
        }
        writeNativeLog(("MIC_CONCAT: All segments preserved in " + failedDir).c_str());
    }

    return success;
}

void Recorder::cleanupMicSegmentFiles() {
    writeNativeLog(("MIC_CLEANUP: Removing " + std::to_string(micSegmentPaths_.size()) + " mic segment files").c_str());
    for (const auto& seg : micSegmentPaths_) {
        if (std::remove(seg.c_str()) == 0) {
            writeNativeLog(("MIC_CLEANUP: Removed " + seg).c_str());
        } else {
            writeNativeLog(("MIC_CLEANUP_WARN: Failed to remove " + seg).c_str());
        }
    }
}

// ===== WASAPI System Audio Capture =====

bool Recorder::startSystemAudioCapture() {
    // Generate a unique pipe name for this recording segment
    systemAudioPipeName_ = "screencraft_audio_" + std::to_string(GetCurrentProcessId())
                           + "_" + std::to_string(segmentCounter_);

    systemAudioCapture_ = std::make_unique<AudioCapture>(true);  // true = loopback

    if (!systemAudioCapture_->initialize()) {
        writeNativeLog("WASAPI: Failed to initialize system audio capture");
        systemAudioCapture_.reset();
        return false;
    }

    if (!systemAudioCapture_->createNamedPipe(systemAudioPipeName_)) {
        writeNativeLog("WASAPI: Failed to create named pipe");
        systemAudioCapture_.reset();
        return false;
    }

    // Audio data is written directly to the named pipe inside AudioCapture::captureThread()

    // Start WASAPI capture BEFORE FFmpeg (so pipe is ready when FFmpeg connects)
    if (!systemAudioCapture_->start()) {
        writeNativeLog("WASAPI: Failed to start capture");
        systemAudioCapture_->closeNamedPipe();
        systemAudioCapture_.reset();
        return false;
    }

    // The pipe thread is now running ConnectNamedPipe, waiting for FFmpeg to connect.
    // When FFmpeg starts and opens the pipe, the connection will be established.

    writeNativeLog(("WASAPI: System audio capture started, pipe: " + systemAudioCapture_->getPipePath()).c_str());
    return true;
}

void Recorder::stopSystemAudioCapture() {
    if (systemAudioCapture_) {
        systemAudioCapture_->stop();
        systemAudioCapture_->closeNamedPipe();
        systemAudioCapture_.reset();
        writeNativeLog("WASAPI: System audio capture stopped");
    }
    systemAudioPipeName_.clear();
}

// Global functions
bool initRecorder(const std::wstring& modulePath) {
    if (g_recorder) return true;
    g_recorder = new Recorder();
    return g_recorder->initialize(modulePath);
}

Recorder* getRecorder() { return g_recorder; }

void shutdownRecorder() {
    if (g_recorder) {
        g_recorder->shutdown();
        delete g_recorder;
        g_recorder = nullptr;
    }
}
