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

    // Microphone is now recorded to a separate file via a dedicated FFmpeg process.
    // It is no longer mixed into the video file.

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

    if (config.captureMicrophone) {
        // Derive mic audio path from video path: recording.mkv -> recording_mic.m4a
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

    // Build FFmpeg command using segment path instead of final path
    RecordingConfig segConfig = config;
    segConfig.savePath = currentSegmentPath_;
    std::string command = buildFFmpegCommand(segConfig);
    std::cerr << "[RECORDER] startRecording: FFmpeg command: " << command << std::endl;

    writeNativeLog("N2: Calling startFFmpeg()...");
    bool started = startFFmpeg(command);
    writeNativeLog((std::string("N3: startFFmpeg() returned: ") + (started ? "true" : "false")).c_str());
    std::cerr << "[RECORDER] startRecording: startFFmpeg returned " << started << std::endl;

    // If failed and audio was enabled, try without system audio only (keep microphone)
    if (!started && (config.captureAudio || config.captureMicrophone)) {
        std::cerr << "[RECORDER] startRecording: FFmpeg failed with audio, retrying without system audio" << std::endl;
        RecordingConfig noSysAudioConfig = config;
        noSysAudioConfig.captureAudio = false;  // Disable system audio only
        // Keep captureMicrophone as-is (do not disable)
        noSysAudioConfig.savePath = currentSegmentPath_;
        command = buildFFmpegCommand(noSysAudioConfig);
        std::cerr << "[RECORDER] startRecording: Retry FFmpeg command: " << command << std::endl;
        started = startFFmpeg(command);
        if (started) {
            config_ = noSysAudioConfig;
            config_.savePath = finalOutputPath_;  // Keep original final path in config_
            std::cerr << "[RECORDER] startRecording: Recording started successfully (without system audio)" << std::endl;
        }
    }

    if (!started) {
        std::cerr << "[RECORDER] startRecording: startFFmpeg failed" << std::endl;
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

    // Build FFmpeg command for the new segment
    RecordingConfig segConfig = config_;
    segConfig.savePath = currentSegmentPath_;
    std::string command = buildFFmpegCommand(segConfig);

    std::cerr << "[RECORDER] resumeRecording() - starting FFmpeg" << std::endl;
    bool started = startFFmpeg(command);

    // If failed and audio was enabled, retry without audio
    if (!started && (config_.captureAudio || config_.captureMicrophone)) {
        std::cerr << "[RECORDER] resumeRecording() - FFmpeg failed with audio, retrying without" << std::endl;
        writeNativeLog("R3: FFmpeg failed with audio, retrying without audio");
        RecordingConfig noAudioConfig = config_;
        noAudioConfig.captureAudio = false;
        noAudioConfig.captureMicrophone = false;
        noAudioConfig.savePath = currentSegmentPath_;
        command = buildFFmpegCommand(noAudioConfig);
        started = startFFmpeg(command);
    }

    if (!started) {
        std::cerr << "[RECORDER] resumeRecording() - FAILED to start FFmpeg" << std::endl;
        writeNativeLog("R_ERROR: Failed to start new FFmpeg segment, staying PAUSED");
        std::cerr << "[RECORDER] resumeRecording: Failed to start new segment" << std::endl;
        // Stay in PAUSED state - don't change state
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
        WriteFile(ffmpegStdin_, &quitCmd, 1, &bytesWritten, NULL);
        FlushFileBuffers(ffmpegStdin_);
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
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;

    char* cmdLine = _strdup(concatCmd.c_str());
    bool success = false;

    if (CreateProcessA(nullptr, cmdLine, nullptr, nullptr, FALSE,
                       CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        // Wait for concat to finish (should be fast since -c copy)
        DWORD waitResult = WaitForSingleObject(pi.hProcess, 60000);  // 60s timeout

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
    // For microphone, use dshow to capture default audio input device
    // If custom device is specified, use it; otherwise use default device
    cmd << " -f dshow -i audio";
    if (!config.microphoneDevice.empty()) {
        cmd << "=\"" << config.microphoneDevice << "\"";
    }
    cmd << " -c:a aac";
    cmd << " -b:a " << config.audioBitrate << "k";
    cmd << " \"" << config.savePath << "\"";
    return cmd.str();
}

bool Recorder::startMicFFmpeg(const std::string& command) {
    writeNativeLog("MIC_F1: startMicFFmpeg() called");

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

    // Wait for mic FFmpeg to be ready (audio-only starts faster)
    // Reuse the same wait logic but with the mic process handle temporarily
    PROCESS_INFORMATION savedMain = ffmpegProcess_;
    ffmpegProcess_ = pi;
    bool ready = waitForFFmpegReady(hStderrRead, 5000);
    ffmpegProcess_ = savedMain;

    CloseHandle(hStderrRead);

    if (!ready) {
        DWORD exitCode;
        if (GetExitCodeProcess(pi.hProcess, &exitCode) && exitCode != STILL_ACTIVE) {
            writeNativeLog("MIC_F_ERROR: Mic FFmpeg exited early");
        } else {
            writeNativeLog("MIC_F_WARN: Mic FFmpeg timeout - terminating");
            TerminateProcess(pi.hProcess, 1);
        }
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
        memset(&micFfmpegProcess_, 0, sizeof(micFfmpegProcess_));
        if (micFfmpegStdin_) {
            CloseHandle(micFfmpegStdin_);
            micFfmpegStdin_ = nullptr;
        }
        return false;
    }

    writeNativeLog("MIC_F4: Mic FFmpeg is recording!");
    return true;
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
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;

    char* cmdLine = _strdup(concatCmd.c_str());
    bool success = false;

    if (CreateProcessA(nullptr, cmdLine, nullptr, nullptr, FALSE,
                       CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        DWORD waitResult = WaitForSingleObject(pi.hProcess, 60000);
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
            }
        }
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
    } else {
        writeNativeLog(("MIC_CONCAT_ERROR: CreateProcess failed, error=" + std::to_string(GetLastError())).c_str());
    }

    free(cmdLine);
    std::remove(listPath.c_str());
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
