/**
 * FFmpeg-based Screen Recording Implementation
 *
 * Uses Windows GDI screen capture + FFmpeg for encoding.
 * Spawns ffmpeg as a child process.
 */

#include "recorder.h"
#include "utils.h"

#include <windows.h>
#include <iostream>
#include <sstream>
#include <thread>
#include <chrono>
#include <fstream>

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

Recorder::Recorder()
    : state_(RecordingState::IDLE)
    , encoderType_(EncoderType::NONE)
{
    memset(&ffmpegProcess_, 0, sizeof(ffmpegProcess_));
}

Recorder::~Recorder() {
    shutdown();
    if (g_timelineLogFile.is_open()) {
        g_timelineLogFile.close();
    }
}

bool Recorder::initialize(const std::wstring& modulePath) {
    // Open timeline log file
    g_timelineLogFile.open("recording_timeline.log", std::ios::app);
    writeNativeLog("===== NATIVE CORE INITIALIZED =====");

    // Set FFmpeg path from module path
    std::wstring ffmpegW = modulePath;
    if (ffmpegW.empty()) {
        // Try default path
        ffmpegW = L".\\dist";
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
        std::cerr << "[RECORDER] FFmpeg not found at: " << std::string(ffmpegExe.begin(), ffmpegExe.end()) << std::endl;
        return false;
    }

    ffmpegPath_ = std::string(ffmpegExe.begin(), ffmpegExe.end());
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

    return true;
}

EncoderType Recorder::checkHardwareEncoders() {
    // Skip hardware encoder detection for now - just use software x264
    // This prevents potential hangs when spawning ffmpeg
    return EncoderType::X264;
}

std::string Recorder::buildFFmpegCommand(const RecordingConfig& config) {
    std::ostringstream cmd;

    cmd << "\"" << ffmpegPath_ << "\"";

    // Debug: log config values received
    std::cerr << "[RECORDER] buildFFmpegCommand: width=" << config.width
              << " height=" << config.height
              << " fps=" << config.fps
              << " videoBitrate=" << config.videoBitrate
              << " captureAudio=" << config.captureAudio
              << " captureMicrophone=" << config.captureMicrophone
              << " separateAudio=" << config.separateAudio
              << " audioBitrate=" << config.audioBitrate << std::endl;

    // Global options
    cmd << " -y";  // Overwrite output

    // ===== INPUTS FIRST =====
    // Video input - Windows screen capture using GDI
    cmd << " -f gdigrab";
    cmd << " -framerate " << config.fps;
    cmd << " -offset_x 0 -offset_y 0";
    cmd << " -draw_mouse 1";
    cmd << " -video_size " << config.width << "x" << config.height;
    cmd << " -i desktop";
    cmd << " -use_wallclock_as_timestamps 0";

    // Count audio inputs
    int audioInputCount = 0;

    // System audio input (if enabled) - use WASAPI
    if (config.captureAudio) {
        cmd << " -f wasapi -i \"audio=Speakers\"";
        audioInputCount++;
    }

    // Microphone input (if enabled) - use dshow
    if (config.captureMicrophone) {
        cmd << " -f dshow -i audio=\"";
        if (!config.microphoneDevice.empty()) {
            cmd << config.microphoneDevice;
        } else {
            cmd << "virtual-audio-capturer";
        }
        cmd << "\"";
        audioInputCount++;
    }

    // ===== OUTPUT OPTIONS AFTER ALL INPUTS =====
    // Video encoder
    cmd << " -c:v ";
    switch (encoderType_) {
        case EncoderType::NVENC:
            cmd << "h264_nvenc";
            cmd << " -preset p7";
            cmd << " -tune hq";
            break;
        case EncoderType::AMF:
            cmd << "h264_amf";
            cmd << " -quality speed";
            break;
        case EncoderType::QSV:
            cmd << "h264_qsv";
            break;
        default:
            cmd << "libx264";
            cmd << " -preset ultrafast";
            cmd << " -tune zerolatency";
    }

    // Video bitrate options
    cmd << " -b:v " << config.videoBitrate << "k";
    cmd << " -maxrate " << (config.videoBitrate * 1.5) << "k";
    cmd << " -bufsize " << (config.videoBitrate * 2) << "k";

    // Audio encoder (if any audio input enabled)
    if (audioInputCount > 0) {
        cmd << " -c:a aac";
        cmd << " -b:a " << config.audioBitrate << "k";
    }

    // Map inputs based on configuration
    if (audioInputCount == 2 && config.separateAudio) {
        // Separate audio tracks: system audio + microphone as separate streams
        // Map: video from input 0, system audio from input 1, microphone from input 2
        cmd << " -map 0:v -map 1:a -map 2:a";
    } else if (audioInputCount == 2) {
        // Mix both audio sources into one track
        cmd << " -filter_complex \"[1:a][2:a]amix=inputs=2:duration=longest[aout]\"";
        cmd << " -map 0:v -map \"[aout]\"";
    } else if (audioInputCount == 1) {
        // Single audio source
        cmd << " -map 0:v -map 1:a";
    } else {
        // Video only
        cmd << " -map 0:v";
    }

    // Timestamp control - ensure video starts at 0
    cmd << " -avoid_negative_ts make_zero";
    cmd << " -vsync cfr";

    // Output
    cmd << " -f matroska \"" << config.savePath << "\"";

    return cmd.str();
}

bool Recorder::startFFmpeg(const std::string& command) {
    writeNativeLog("F1: startFFmpeg() called, creating process...");

    std::cerr << "[RECORDER] startFFmpeg: BEGIN" << std::endl;

    STARTUPINFOA si = {};
    PROCESS_INFORMATION pi = {};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);

    // Don't redirect stderr - let it go to console for debugging
    si.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
    si.hStdError = GetStdHandle(STD_ERROR_HANDLE);

    char* cmdLine = _strdup(command.c_str());
    std::cerr << "[RECORDER] startFFmpeg: CreateProcess with command: " << command << std::endl;

    if (!CreateProcessA(nullptr, cmdLine, nullptr, nullptr, TRUE,
                       CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        DWORD err = GetLastError();
        std::cerr << "[RECORDER] startFFmpeg: CreateProcess failed: " << err << std::endl;
        free(cmdLine);
        return false;
    }

    free(cmdLine);
    ffmpegProcess_ = pi;

    writeNativeLog((std::string("F2: FFmpeg process created, PID: ") + std::to_string(pi.dwProcessId)).c_str());
    std::cerr << "[RECORDER] startFFmpeg: FFmpeg process created (PID: " << pi.dwProcessId << ")" << std::endl;

    writeNativeLog("F3: Sleeping 500ms for FFmpeg initialization...");
    // Give FFmpeg time to start and output any errors
    Sleep(500);
    writeNativeLog("F4: Sleep done, verifying process...");

    // Check if FFmpeg is still running or exited with error
    DWORD exitCode;
    if (GetExitCodeProcess(pi.hProcess, &exitCode)) {
        std::cerr << "[RECORDER] startFFmpeg: FFmpeg exitCode=" << exitCode << " (STILL_ACTIVE=" << STILL_ACTIVE << ")" << std::endl;
        // If process has already exited (not STILL_ACTIVE), it means FFmpeg failed immediately
        if (exitCode != STILL_ACTIVE) {
            std::cerr << "[RECORDER] startFFmpeg: FFmpeg exited early with error code " << exitCode << std::endl;
            return false;
        }
    }

    writeNativeLog("F5: FFmpeg is running, recording active");
    return true;
}

void Recorder::sendToFFmpeg(const char* cmd, size_t len) {
    // This would be used for interactive FFmpeg control via pipe
    // For now we just use simple process management
    (void)cmd;
    (void)len;
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

    std::cerr << "[RECORDER] startRecording: outputPath=" << outputPath_ << std::endl;
    std::cerr << "[RECORDER] startRecording: width=" << config.width << " height=" << config.height << " fps=" << config.fps << std::endl;

    // Build and start FFmpeg command
    std::string command = buildFFmpegCommand(config);
    std::cerr << "[RECORDER] startRecording: FFmpeg command: " << command << std::endl;

    writeNativeLog("N2: Calling startFFmpeg()...");
    bool started = startFFmpeg(command);
    writeNativeLog((std::string("N3: startFFmpeg() returned: ") + (started ? "true" : "false")).c_str());
    std::cerr << "[RECORDER] startRecording: startFFmpeg returned " << started << std::endl;

    // If failed and audio was enabled, try without audio
    if (!started && (config.captureAudio || config.captureMicrophone)) {
        std::cerr << "[RECORDER] startRecording: FFmpeg failed with audio, retrying without audio" << std::endl;
        RecordingConfig noAudioConfig = config;
        noAudioConfig.captureAudio = false;
        noAudioConfig.captureMicrophone = false;
        command = buildFFmpegCommand(noAudioConfig);
        std::cerr << "[RECORDER] startRecording: Retry FFmpeg command: " << command << std::endl;
        started = startFFmpeg(command);
        if (started) {
            std::cerr << "[RECORDER] startRecording: Recording started successfully (without audio)" << std::endl;
        }
    }

    if (!started) {
        std::cerr << "[RECORDER] startRecording: startFFmpeg failed" << std::endl;
        return false;
    }

    // Give FFmpeg time to start capturing
    Sleep(500);

    state_ = RecordingState::RECORDING;
    totalPausedDuration_ = 0;

    writeNativeLog("N4: Recording started successfully");
    std::cerr << "[RECORDER] startRecording: Recording started successfully" << std::endl;
    return true;
}

void Recorder::stopRecording() {
    if (state_ == RecordingState::IDLE) return;

    // Send quit command to FFmpeg
    if (ffmpegProcess_.hProcess) {
        // Give FFmpeg a moment to finish writing
        Sleep(500);

        // Terminate the process if still running
        TerminateProcess(ffmpegProcess_.hProcess, 0);

        CloseHandle(ffmpegProcess_.hProcess);
        CloseHandle(ffmpegProcess_.hThread);
        memset(&ffmpegProcess_, 0, sizeof(ffmpegProcess_));
    }

    state_ = RecordingState::IDLE;
    std::cout << "[RECORDER] Recording stopped" << std::endl;

    if (stopCallback_) {
        stopCallback_(stopCallbackData_);
    }
}

void Recorder::pauseRecording() {
    if (state_ != RecordingState::RECORDING) return;
    pauseBeginTime_ = getHighPrecisionTimestamp();
    state_ = RecordingState::PAUSED;
    std::cout << "[RECORDER] Recording paused" << std::endl;
}

void Recorder::resumeRecording() {
    if (state_ != RecordingState::PAUSED) return;
    totalPausedDuration_ += (getHighPrecisionTimestamp() - pauseBeginTime_);
    state_ = RecordingState::RECORDING;
    std::cout << "[RECORDER] Recording resumed" << std::endl;
}

void Recorder::shutdown() {
    if (state_ == RecordingState::RECORDING) {
        stopRecording();
    }

    if (ffmpegProcess_.hProcess) {
        CloseHandle(ffmpegProcess_.hProcess);
        CloseHandle(ffmpegProcess_.hThread);
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
