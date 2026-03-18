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
    , ffmpegStdin_(nullptr)
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
    // Increase real-time buffer for smoother capture
    cmd << " -rtbufsize 100M";

    // ===== INPUTS FIRST =====
    // Video input - Windows screen capture using GDI
    cmd << " -f gdigrab";
    cmd << " -framerate " << config.fps;
    cmd << " -offset_x 0 -offset_y 0";
    cmd << " -draw_mouse 1";
    cmd << " -video_size " << config.width << "x" << config.height;
    cmd << " -i desktop";
    cmd << " -use_wallclock_as_timestamps 0";
    // Generate PTS for video to match audio
    cmd << " -fflags +genpts";

    // Count audio inputs
    int audioInputCount = 0;

    // System audio input - DISABLED (no virtual audio device available)
    // To enable: install VB-Audio Virtual Cable and use "audio=CABLE Input (VB-Audio Virtual Cable)"
    // if (config.captureAudio) {
    //     cmd << " -f dshow -i audio=\"CABLE Input (VB-Audio Virtual Cable)\"";
    //     audioInputCount++;
    // }

    // Microphone input (if enabled) - use dshow
    if (config.captureMicrophone) {
        // Add thread_queue_size to prevent audio input blocking
        cmd << " -thread_queue_size 512";
        cmd << " -f dshow -i audio=\"";
        if (!config.microphoneDevice.empty()) {
            cmd << config.microphoneDevice;
        } else {
            cmd << "外部麦克风 (Realtek(R) Audio)";
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
        // Use multiple threads for audio encoding to reduce CPU load
        cmd << " -threads 2";
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

bool Recorder::waitForFFmpegReady(HANDLE hStderrRead, int timeoutMs) {
    const auto startTime = std::chrono::steady_clock::now();
    std::string buffer;

    while (true) {
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - startTime
        ).count();

        if (elapsed > timeoutMs) {
            writeNativeLog(("F_TIMEOUT: No ready signal after " + std::to_string(timeoutMs) + "ms").c_str());
            return false;
        }

        DWORD bytesAvailable = 0;
        if (PeekNamedPipe(hStderrRead, NULL, 0, NULL, &bytesAvailable, NULL) && bytesAvailable > 0) {
            char tempBuf[4096];
            DWORD bytesRead = 0;
            DWORD toRead = bytesAvailable < sizeof(tempBuf) ? bytesAvailable : sizeof(tempBuf);

            if (ReadFile(hStderrRead, tempBuf, toRead, &bytesRead, NULL) && bytesRead > 0) {
                buffer.append(tempBuf, bytesRead);

                // Log what FFmpeg is saying (for debugging)
                std::string chunk(tempBuf, bytesRead);
                writeNativeLog(("F_STDERR: " + chunk).c_str());

                // Success: FFmpeg is ready to record
                if (buffer.find("Press [q] to stop") != std::string::npos ||
                    buffer.find("frame=") != std::string::npos) {
                    return true;
                }

                // Failure: FFmpeg encountered an error
                if (buffer.find("Error opening input") != std::string::npos ||
                    buffer.find("Unknown input format") != std::string::npos ||
                    buffer.find("Cannot open") != std::string::npos ||
                    buffer.find("No such file or directory") != std::string::npos ||
                    buffer.find("Immediate exit requested") != std::string::npos) {
                    writeNativeLog("F_ERROR: FFmpeg reported an error in stderr");
                    return false;
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

    state_ = RecordingState::RECORDING;
    totalPausedDuration_ = 0;

    writeNativeLog("N4: Recording started successfully");
    std::cerr << "[RECORDER] startRecording: Recording started successfully" << std::endl;
    return true;
}

void Recorder::stopRecording() {
    if (state_ == RecordingState::IDLE) return;

    writeNativeLog("S1: stopRecording() called, sending quit to FFmpeg...");

    // Send quit command to FFmpeg for graceful shutdown
    if (ffmpegProcess_.hProcess && ffmpegStdin_) {
        // Send 'q' to FFmpeg stdin to request graceful shutdown
        const char quitCmd = 'q';
        DWORD bytesWritten = 0;
        WriteFile(ffmpegStdin_, &quitCmd, 1, &bytesWritten, NULL);
        FlushFileBuffers(ffmpegStdin_);
        CloseHandle(ffmpegStdin_);
        ffmpegStdin_ = nullptr;
        writeNativeLog("S2: Sent 'q' to FFmpeg, waiting for graceful exit...");

        // Wait for FFmpeg to exit gracefully (max 10 seconds)
        const DWORD maxWaitMs = 10000;
        DWORD waitResult = WaitForSingleObject(ffmpegProcess_.hProcess, maxWaitMs);

        if (waitResult == WAIT_TIMEOUT) {
            writeNativeLog("S3: FFmpeg did not exit gracefully, forcing termination");
            // Force terminate if still running after timeout
            TerminateProcess(ffmpegProcess_.hProcess, 0);
        } else {
            writeNativeLog("S3: FFmpeg exited gracefully");
        }
    } else if (ffmpegProcess_.hProcess) {
        // Fallback: just terminate if no stdin handle
        writeNativeLog("S2: No stdin handle, forcing termination");
        TerminateProcess(ffmpegProcess_.hProcess, 0);
    }

    // Clean up process handles
    if (ffmpegProcess_.hProcess) {
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
