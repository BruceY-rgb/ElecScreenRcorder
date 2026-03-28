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

    std::cerr << "[RECORDER] No hardware encoders found or working, defaulting to x264" << std::endl;
    return EncoderType::X264;
}

bool Recorder::isDdagrabAvailable() {
    // Check if ddagrab is available by running a probe command
    // ffmpeg -hide_banner -f ddagrab -i desktop -frames:v 1 -f null NUL
    std::string output = runFFmpegProbe(ffmpegPath_, "-hide_banner -f ddagrab -i desktop -frames:v 1 -f null NUL", 5000);
    bool available = output.find("ddagrab") != std::string::npos &&
                     output.find("No such file or directory") == std::string::npos &&
                     output.find("Cannot open video device") == std::string::npos &&
                     output.find("Error") == std::string::npos;
    return available;
}

void Recorder::shutdown() {
    writeNativeLog("SHUTDOWN: Recorder shutdown called");
    stopRecording(); // Ensure recording is stopped and processes are cleaned up
    // Additional cleanup if necessary
}

bool Recorder::startRecording(const RecordingConfig& config) {
    if (state_ != RecordingState::IDLE) {
        std::cerr << "[RECORDER] Already recording or paused." << std::endl;
        writeNativeLog("START_FAIL: Already recording or paused.");
        return false;
    }

    config_ = config;
    outputPath_ = config.savePath;
    finalOutputPath_ = config.savePath;
    segmentCounter_ = 0;
    segmentPaths_.clear();
    totalPausedDuration_ = 0;

    finalMicAudioPath_ = config.separateAudio ? config.savePath.substr(0, config.savePath.find_last_of('.')) + "_mic.mkv" : "";
    micSegmentCounter_ = 0;
    micSegmentPaths_.clear();

    // Start system audio capture if enabled
    if (config_.captureAudio) {
        if (!startSystemAudioCapture()) {
            std::cerr << "[RECORDER] Failed to start system audio capture." << std::endl;
            writeNativeLog("START_FAIL: Failed to start system audio capture.");
            return false;
        }
    }

    // Build FFmpeg command
    std::string ffmpegCommand = buildFFmpegCommand(config_);
    if (ffmpegCommand.empty()) {
        std::cerr << "[RECORDER] Failed to build FFmpeg command." << std::endl;
        writeNativeLog("START_FAIL: Failed to build FFmpeg command.");
        stopSystemAudioCapture();
        return false;
    }

    // Start FFmpeg process
    if (!startFFmpeg(ffmpegCommand)) {
        std::cerr << "[RECORDER] Failed to start FFmpeg process." << std::endl;
        writeNativeLog("START_FAIL: Failed to start FFmpeg process.");
        stopSystemAudioCapture();
        return false;
    }

    // Start mic FFmpeg process if enabled
    if (config_.captureMicrophone) {
        std::string micFFmpegCommand = buildMicFFmpegCommand(config_);
        if (micFFmpegCommand.empty()) {
            std::cerr << "[RECORDER] Failed to build mic FFmpeg command." << std::endl;
            writeNativeLog("START_FAIL: Failed to build mic FFmpeg command.");
            stopFFmpegGracefully(1000); // Stop main FFmpeg if mic fails
            stopSystemAudioCapture();
            return false;
        }
        if (!startMicFFmpeg(micFFmpegCommand)) {
            std::cerr << "[RECORDER] Failed to start mic FFmpeg process." << std::endl;
            writeNativeLog("START_FAIL: Failed to start mic FFmpeg process.");
            stopFFmpegGracefully(1000); // Stop main FFmpeg if mic fails
            stopSystemAudioCapture();
            return false;
        }
    }

    state_ = RecordingState::RECORDING;
    writeNativeLog("START_SUCCESS: Recording started.");
    return true;
}

std::string Recorder::buildFFmpegCommand(const RecordingConfig& config) {
    std::ostringstream cmd;
    bool useDdagrab = ddagrabAvailable_ && config.captureHwnd == 0; // Only use ddagrab for desktop

    cmd << "\"" << ffmpegPath_ << "\"";
    cmd << " -y"; // Overwrite output files without asking

    // ===== VIDEO INPUT =====
    if (useDdagrab) {
        cmd << " -f ddagrab";
        cmd << " -i desktop";
        cmd << " -framerate " << config.fps;
        cmd << " -s " << config.width << "x" << config.height;
        writeNativeLog("FFMPEG_CMD: Using ddagrab for video input.");
    } else {
        // gdigrab for window capture or if ddagrab is not available
        cmd << " -f gdigrab";
        cmd << " -i desktop"; // gdigrab always captures desktop, then we crop
        cmd << " -framerate " << config.fps;
        cmd << " -offset_x 0 -offset_y 0"; // Placeholder, actual cropping handled by -vf
        cmd << " -video_size " << config.width << "x" << config.height;
        writeNativeLog("FFMPEG_CMD: Using gdigrab for video input.");
    }

    // ===== AUDIO INPUT =====
    int audioInputCount = 0;
    if (config_.captureAudio && !systemAudioPipeName_.empty()) {
        cmd << " -f s16le -ar " << systemAudioCapture_->getSampleRate() << " -ac " << systemAudioCapture_->getChannels();
        cmd << " -i \"" << systemAudioPipeName_ << "\"";
        audioInputCount++;
        std::cerr << "[RECORDER] System audio via WASAPI pipe: " << systemAudioPipeName_
                  << " fmt=" << systemAudioCapture_->getFormat() << " ar=" << systemAudioCapture_->getSampleRate() << " ac=" << systemAudioCapture_->getChannels() << std::endl;
    }

    // Microphone is recorded to a separate file via a dedicated FFmpeg process.

    // ===== VIDEO ENCODER =====
    if (useDdagrab && encoderType_ != EncoderType::X264) {
        // Hardware encoder with ddagrab: need format conversion for some encoders
        // QSV requires nv12 format specifically
        switch (encoderType_) {
            case EncoderType::NVENC:
                // NVENC with ddagrab: format conversion to nv12 for robustness
                cmd << " -vf format=nv12 -c:v h264_nvenc -preset p7 -tune hq";
                break;
            case EncoderType::AMF:
                // AMF with ddagrab: format conversion to nv12 for robustness
                cmd << " -vf format=nv12 -c:v h264_amf -quality speed";
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
                // Ensure correct pixel format for libx264
                cmd << " -vf hwdownload,format=bgra,format=yuv420p -pix_fmt yuv420p -c:v libx264 -preset ultrafast -tune zerolatency";
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
    cmd << " -f matroska \"" << currentSegmentPath_ << "\"";

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
                    buffer.find("Invalid argument") != std::string::npos ||
                    buffer.find("Option not found") != std::string::npos ||
                    buffer.find("Unable to open") != std::string::npos ||
                    buffer.find("Error initializing filter") != std::string::npos ||
                    buffer.find("Error while opening encoder") != std::string::npos ||
                    buffer.find("Failed to create D3D11 device") != std::string::npos ||
                    buffer.find("Failed to initialize DXGI Desktop Duplication API") != std::string::npos ||
                    buffer.find("No capable devices found") != std::string::npos ||
                    buffer.find("Error during codec control") != std::string::npos) {
                    writeNativeLog("F_ERROR: FFmpeg reported an error.");
                    return false;
                }

                // Success: FFmpeg is ready and waiting for input
                if (buffer.find("Press [q] to stop, [?] for status") != std::string::npos ||
                    buffer.find("Output #0, matroska") != std::string::npos) {
                    if (!sawReadySignal) {
                        sawReadySignal = true;
                        readyTime = std::chrono::steady_clock::now();
                        writeNativeLog("F_READY: FFmpeg ready signal detected.");
                    }
                }
            }
        } else {
            // No bytes available, wait a bit
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }

        // If FFmpeg process has exited prematurely, it's a failure
        DWORD exitCode;
        if (GetExitCodeProcess(ffmpegProcess_.hProcess, &exitCode) && exitCode != STILL_ACTIVE) {
            writeNativeLog(("F_EXITED: FFmpeg process exited prematurely with code " + std::to_string(exitCode)).c_str());
            return false;
        }
    }
}

bool Recorder::startFFmpeg(const std::string& command) {
    writeNativeLog(("S1: Starting FFmpeg with command: " + command).c_str());
    std::cerr << "[RECORDER] Starting FFmpeg: " << command << std::endl;

    // Create pipes for stdin, stdout, stderr
    SECURITY_ATTRIBUTES saAttr = {};
    saAttr.nLength = sizeof(SECURITY_ATTRIBUTES);
    saAttr.bInheritHandle = TRUE;
    saAttr.lpSecurityDescriptor = NULL;

    HANDLE hChildStdinRd, hChildStdinWr;
    HANDLE hChildStdoutRd, hChildStdoutWr;
    HANDLE hChildStderrRd, hChildStderrWr;

    if (!CreatePipe(&hChildStdinRd, &hChildStdinWr, &saAttr, 0)) {
        writeNativeLog("START_FAIL: Stdin pipe creation failed.");
        return false;
    }
    if (!SetHandleInformation(hChildStdinWr, HANDLE_FLAG_INHERIT, 0)) { // Child doesn't inherit write handle
        writeNativeLog("START_FAIL: SetHandleInformation for StdinWr failed.");
        CloseHandle(hChildStdinRd); CloseHandle(hChildStdinWr);
        return false;
    }

    if (!CreatePipe(&hChildStdoutRd, &hChildStdoutWr, &saAttr, 0)) {
        writeNativeLog("START_FAIL: Stdout pipe creation failed.");
        CloseHandle(hChildStdinRd); CloseHandle(hChildStdinWr);
        return false;
    }
    if (!SetHandleInformation(hChildStdoutRd, HANDLE_FLAG_INHERIT, 0)) { // Child doesn't inherit read handle
        writeNativeLog("START_FAIL: SetHandleInformation for StdoutRd failed.");
        CloseHandle(hChildStdinRd); CloseHandle(hChildStdinWr);
        CloseHandle(hChildStdoutRd); CloseHandle(hChildStdoutWr);
        return false;
    }

    if (!CreatePipe(&hChildStderrRd, &hChildStderrWr, &saAttr, 0)) {
        writeNativeLog("START_FAIL: Stderr pipe creation failed.");
        CloseHandle(hChildStdinRd); CloseHandle(hChildStdinWr);
        CloseHandle(hChildStdoutRd); CloseHandle(hChildStdoutWr);
        return false;
    }
    if (!SetHandleInformation(hChildStderrRd, HANDLE_FLAG_INHERIT, 0)) { // Child doesn't inherit read handle
        writeNativeLog("START_FAIL: SetHandleInformation for StderrRd failed.");
        CloseHandle(hChildStdinRd); CloseHandle(hChildStdinWr);
        CloseHandle(hChildStdoutRd); CloseHandle(hChildStdoutWr);
        CloseHandle(hChildStderrRd); CloseHandle(hChildStderrWr);
        return false;
    }

    STARTUPINFOA si = {};
    si.cb = sizeof(si);
    si.hStdError = hChildStderrWr;
    si.hStdOutput = hChildStdoutWr;
    si.hStdInput = hChildStdinRd;
    si.dwFlags |= STARTF_USESTDHANDLES;

    PROCESS_INFORMATION pi = {};

    // Convert command string to mutable char array
    std::vector<char> cmdBuf(command.begin(), command.end());
    cmdBuf.push_back('\0');

    if (!CreateProcessA(NULL,   // No module name (use command line)
                        cmdBuf.data(),           // Command line
                        NULL,           // Process handle not inheritable
                        NULL,           // Thread handle not inheritable
                        TRUE,           // Set handle inheritance to TRUE
                        CREATE_NO_WINDOW, // No console window
                        NULL,           // Use parent's environment block
                        NULL,           // Use parent's starting directory
                        &si,            // Pointer to STARTUPINFO structure
                        &pi))           // Pointer to PROCESS_INFORMATION structure
    {
        std::cerr << "[RECORDER] CreateProcess failed (" << GetLastError() << ")." << std::endl;
        writeNativeLog(("START_FAIL: CreateProcess failed with error " + std::to_string(GetLastError())).c_str());
        CloseHandle(hChildStdinRd); CloseHandle(hChildStdinWr);
        CloseHandle(hChildStdoutRd); CloseHandle(hChildStdoutWr);
        CloseHandle(hChildStderrRd); CloseHandle(hChildStderrWr);
        return false;
    }

    // Close handles to the child process's stdin and stdout that are used by the parent.
    // If these handles are not closed, the child process will not terminate.
    CloseHandle(hChildStdinRd);
    CloseHandle(hChildStdoutWr);
    CloseHandle(hChildStderrWr);

    ffmpegProcess_ = pi;
    ffmpegStdin_ = hChildStdinWr;

    // Wait for FFmpeg to be ready (parse stderr output)
    if (!waitForFFmpegReady(hChildStderrRd, 10000)) { // 10 second timeout for FFmpeg to start
        std::cerr << "[RECORDER] FFmpeg did not become ready in time or reported an error." << std::endl;
        writeNativeLog("START_FAIL: FFmpeg not ready or reported error.");
        // Attempt to terminate the process if it didn't become ready
        TerminateProcess(pi.hProcess, 0);
        WaitForSingleObject(pi.hProcess, 3000);
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
        CloseHandle(ffmpegStdin_);
        ffmpegStdin_ = nullptr;
        memset(&ffmpegProcess_, 0, sizeof(ffmpegProcess_));
        CloseHandle(hChildStderrRd);
        CloseHandle(hChildStdoutRd);
        return false;
    }

    CloseHandle(hChildStderrRd); // Close after waitForFFmpegReady
    CloseHandle(hChildStdoutRd); // Close after waitForFFmpegReady

    writeNativeLog("S1_SUCCESS: FFmpeg process started and ready.");
    return true;
}

void Recorder::stopRecording() {
    writeNativeLog("S2: stopRecording() called");
    std::cerr << "[RECORDER] stopRecording() called" << std::endl;

    if (state_ == RecordingState::IDLE) {
        writeNativeLog("S2: Not recording, nothing to stop.");
        return;
    }

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

    state_ = RecordingState::IDLE;
    writeNativeLog("S5: Recording stopped and cleaned up.");

    if (stopCallback_) {
        stopCallback_(stopCallbackData_);
    }
}

void Recorder::pauseRecording() {
    if (state_ != RecordingState::RECORDING) {
        std::cerr << "[RECORDER] Not recording, cannot pause." << std::endl;
        writeNativeLog("PAUSE_FAIL: Not recording, cannot pause.");
        return;
    }

    // Stop FFmpeg gracefully to save the current segment
    if (stopFFmpegGracefully(5000)) { // Give FFmpeg 5 seconds to finish current segment
        segmentPaths_.push_back(currentSegmentPath_);
        writeNativeLog(("PAUSE_SUCCESS: Saved segment: " + currentSegmentPath_).c_str());
        state_ = RecordingState::PAUSED;
        pauseBeginTime_ = getTimestampMs();
        writeNativeLog("PAUSE_SUCCESS: Recording paused.");
    } else {
        std::cerr << "[RECORDER] Failed to gracefully stop FFmpeg for pausing. Forcing stop." << std::endl;
        writeNativeLog("PAUSE_FAIL: Failed to gracefully stop FFmpeg for pausing. Forcing stop.");
        // If graceful stop fails, force terminate and reset
        TerminateProcess(ffmpegProcess_.hProcess, 0);
        WaitForSingleObject(ffmpegProcess_.hProcess, 3000);
        CloseHandle(ffmpegProcess_.hProcess);
        CloseHandle(ffmpegProcess_.hThread);
        memset(&ffmpegProcess_, 0, sizeof(ffmpegProcess_));
        if (ffmpegStdin_) {
            CloseHandle(ffmpegStdin_);
            ffmpegStdin_ = nullptr;
        }
        state_ = RecordingState::IDLE;
        writeNativeLog("PAUSE_FAIL: Recording forcefully stopped due to FFmpeg error during pause.");
    }

    // Stop mic FFmpeg if running
    if (micFfmpegProcess_.hProcess) {
        if (stopMicFFmpegGracefully(5000)) {
            micSegmentPaths_.push_back(currentMicSegmentPath_);
            writeNativeLog(("PAUSE_SUCCESS: Saved mic segment: " + currentMicSegmentPath_).c_str());
        } else {
            writeNativeLog("PAUSE_FAIL: Failed to gracefully stop mic FFmpeg for pausing. Forcing stop.");
            TerminateProcess(micFfmpegProcess_.hProcess, 0);
            WaitForSingleObject(micFfmpegProcess_.hProcess, 3000);
            CloseHandle(micFfmpegProcess_.hProcess);
            CloseHandle(micFfmpegProcess_.hThread);
            memset(&micFfmpegProcess_, 0, sizeof(micFfmpegProcess_));
            if (micFfmpegStdin_) {
                CloseHandle(micFfmpegStdin_);
                micFfmpegStdin_ = nullptr;
            }
        }
    }

    // Stop WASAPI system audio capture
    stopSystemAudioCapture();
}

void Recorder::resumeRecording() {
    if (state_ != RecordingState::PAUSED) {
        std::cerr << "[RECORDER] Not paused, cannot resume." << std::endl;
        writeNativeLog("RESUME_FAIL: Not paused, cannot resume.");
        return;
    }

    totalPausedDuration_ += (getTimestampMs() - pauseBeginTime_);

    // Generate a new segment path for the resumed recording
    currentSegmentPath_ = generateSegmentPath();
    currentMicSegmentPath_ = generateMicSegmentPath();

    // Start system audio capture if enabled
    if (config_.captureAudio) {
        if (!startSystemAudioCapture()) {
            std::cerr << "[RECORDER] Failed to start system audio capture for resume." << std::endl;
            writeNativeLog("RESUME_FAIL: Failed to start system audio capture.");
            return;
        }
    }

    // Restart FFmpeg process
    std::string ffmpegCommand = buildFFmpegCommand(config_);
    if (ffmpegCommand.empty()) {
        std::cerr << "[RECORDER] Failed to build FFmpeg command for resume." << std::endl;
        writeNativeLog("RESUME_FAIL: Failed to build FFmpeg command.");
        stopSystemAudioCapture();
        return;
    }

    if (!startFFmpeg(ffmpegCommand)) {
        std::cerr << "[RECORDER] Failed to restart FFmpeg process for resume." << std::endl;
        writeNativeLog("RESUME_FAIL: Failed to restart FFmpeg process.");
        stopSystemAudioCapture();
        return;
    }

    // Restart mic FFmpeg process if enabled
    if (config_.captureMicrophone) {
        std::string micFFmpegCommand = buildMicFFmpegCommand(config_);
        if (micFFmpegCommand.empty()) {
            std::cerr << "[RECORDER] Failed to build mic FFmpeg command for resume." << std::endl;
            writeNativeLog("RESUME_FAIL: Failed to build mic FFmpeg command.");
            stopFFmpegGracefully(1000); // Stop main FFmpeg if mic fails
            stopSystemAudioCapture();
            return;
        }
        if (!startMicFFmpeg(micFFmpegCommand)) {
            std::cerr << "[RECORDER] Failed to restart mic FFmpeg process for resume." << std::endl;
            writeNativeLog("RESUME_FAIL: Failed to restart mic FFmpeg process.");
            stopFFmpegGracefully(1000); // Stop main FFmpeg if mic fails
            stopSystemAudioCapture();
            return;
        }
    }

    state_ = RecordingState::RECORDING;
    writeNativeLog("RESUME_SUCCESS: Recording resumed.");
}

bool Recorder::isRecording() const {
    return state_ == RecordingState::RECORDING;
}

bool Recorder::isPaused() const {
    return state_ == RecordingState::PAUSED;
}

RecordingState Recorder::getState() const {
    return state_;
}

EncoderType Recorder::getAvailableEncoder() const {
    return encoderType_;
}

std::string Recorder::getOutputPath() const {
    return finalOutputPath_;
}

std::string Recorder::getMicAudioPath() const {
    return finalMicAudioPath_;
}

void Recorder::setStopCallback(StopCallback callback, void* userData) {
    stopCallback_ = callback;
    stopCallbackData_ = userData;
}

void Recorder::signal_stop(bool success) {
    // This function is called from the main thread, so it can safely call the callback
    if (stopCallback_) {
        stopCallback_(stopCallbackData_);
    }
}

bool Recorder::isFFmpegAvailable() const {
    return !ffmpegPath_.empty();
}

bool Recorder::stopFFmpegGracefully(int timeoutMs) {
    writeNativeLog("FFG1: stopFFmpegGracefully() called");
    std::cerr << "[RECORDER] stopFFmpegGracefully() called" << std::endl;

    if (ffmpegProcess_.hProcess == NULL) {
        writeNativeLog("FFG1: No FFmpeg process to stop");
        return true;
    }

    PROCESS_INFORMATION currentFfmpegProcess = ffmpegProcess_;
    HANDLE currentFfmpegStdin = ffmpegStdin_;

    // Clear current process info immediately to allow new recordings
    memset(&ffmpegProcess_, 0, sizeof(ffmpegProcess_));
    ffmpegStdin_ = nullptr;

    // Send 'q' to FFmpeg's stdin to gracefully quit
    if (currentFfmpegStdin) {
        DWORD bytesWritten;
        if (!WriteFile(currentFfmpegStdin, "q\n", 2, &bytesWritten, NULL)) {
            std::cerr << "[RECORDER] ERROR: Failed to write 'q' to FFmpeg stdin. Error: " << GetLastError() << std::endl;
            writeNativeLog("FFG2_ERROR: Failed to write 'q' to FFmpeg stdin, forcing termination.");
            // If we can't send 'q', terminate and let async cleanup handle the rest
            TerminateProcess(currentFfmpegProcess.hProcess, 0);
        }
        if (!FlushFileBuffers(currentFfmpegStdin)) {
            std::cerr << "[RECORDER] ERROR: Failed to flush FFmpeg stdin. Error: " << GetLastError() << std::endl;
            writeNativeLog("FFG2_ERROR: Failed to flush FFmpeg stdin, forcing termination.");
            // If flush fails, terminate and let async cleanup handle the rest
            TerminateProcess(currentFfmpegProcess.hProcess, 0);
        }
        CloseHandle(currentFfmpegStdin);
        writeNativeLog("FFG2: Sent 'q' to FFmpeg, initiating async cleanup...");
    } else if (currentFfmpegProcess.hProcess) {
        std::cerr << "[RECORDER] stopFFmpegGracefully() - no stdin handle, forcing termination" << std::endl;
        writeNativeLog("FFG2: No stdin handle, forcing termination");
        TerminateProcess(currentFfmpegProcess.hProcess, 0);
    }

    // Detach the actual waiting and cleanup to an asynchronous thread
    std::thread(&Recorder::waitForFFmpegExitAndCleanup, this, currentFfmpegProcess, currentFfmpegStdin, timeoutMs, currentSegmentPath_, finalOutputPath_, false).detach();

    return true;
}

void Recorder::waitForFFmpegExitAndCleanup(PROCESS_INFORMATION ffmpegProcess, HANDLE ffmpegStdin, int timeoutMs, const std::string& segmentPath, const std::string& finalPath, bool isMicProcess) {
    writeNativeLog("FFG_ASYNC: waitForFFmpegExitAndCleanup started.");
    std::cerr << "[RECORDER] waitForFFmpegExitAndCleanup() - waiting for FFmpeg to exit..." << std::endl;

    DWORD waitResult = WaitForSingleObject(ffmpegProcess.hProcess, (DWORD)timeoutMs);

    if (waitResult == WAIT_TIMEOUT) {
        std::cerr << "[RECORDER] waitForFFmpegExitAndCleanup() - TIMEOUT, forcing termination" << std::endl;
        writeNativeLog("FFG_ASYNC: FFmpeg did not exit gracefully, forcing termination");
        TerminateProcess(ffmpegProcess.hProcess, 0);
        WaitForSingleObject(ffmpegProcess.hProcess, 3000); // Give it a moment to terminate
    } else {
        std::cerr << "[RECORDER] waitForFFmpegExitAndCleanup() - FFmpeg exited gracefully" << std::endl;
        writeNativeLog("FFG_ASYNC: FFmpeg exited gracefully");
    }

    // Clean up process handles
    if (ffmpegProcess.hProcess) {
        CloseHandle(ffmpegProcess.hProcess);
        CloseHandle(ffmpegProcess.hThread);
    }
    if (ffmpegStdin) {
        CloseHandle(ffmpegStdin);
    }

    writeNativeLog("FFG_ASYNC: FFmpeg process cleaned up.");

    // If this was the main FFmpeg process, signal stop to the main thread
    if (!isMicProcess && g_recorder) {
        g_recorder->signal_stop(true);
    }
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
    sprintf_s(segNum, sizeof(segNum), "_seg%03d", segmentCounter_);
    currentSegmentPath_ = dir + baseName + segNum + ext;
    return currentSegmentPath_;
}

bool Recorder::concatenateSegments() {
    if (segmentPaths_.empty()) {
        writeNativeLog("CONCAT_FAIL: No segments to concatenate.");
        return false;
    }

    // Create a temporary concat file list
    std::string concatListPath = finalOutputPath_ + ".concat.txt";
    std::ofstream concatFile(concatListPath);
    if (!concatFile.is_open()) {
        writeNativeLog("CONCAT_FAIL: Failed to create concat list file.");
        return false;
    }
    for (const auto& path : segmentPaths_) {
        concatFile << "file '" << path << "'\n";
    }
    concatFile.close();

    // Build FFmpeg concat command
    std::ostringstream cmd;
    cmd << "\"" << ffmpegPath_ << "\"";
    cmd << " -y -f concat -safe 0 -i \"" << concatListPath << "\"";
    cmd << " -c copy \"" << finalOutputPath_ << "\"";

    writeNativeLog(("CONCAT_CMD: " + cmd.str()).c_str());

    // Execute FFmpeg concat command
    PROCESS_INFORMATION pi = {};
    STARTUPINFOA si = {};
    si.cb = sizeof(si);
    std::vector<char> cmdBuf(cmd.str().begin(), cmd.str().end());
    cmdBuf.push_back('\0');

    if (!CreateProcessA(NULL, cmdBuf.data(), NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
        std::cerr << "[RECORDER] Concatenation CreateProcess failed (" << GetLastError() << ")." << std::endl;
        writeNativeLog(("CONCAT_FAIL: CreateProcess failed with error " + std::to_string(GetLastError())).c_str());
        return false;
    }

    WaitForSingleObject(pi.hProcess, INFINITE);

    DWORD exitCode;
    GetExitCodeProcess(pi.hProcess, &exitCode);

    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);

    // Delete the concat list file
    DeleteFileA(concatListPath.c_str());

    if (exitCode != 0) {
        std::cerr << "[RECORDER] FFmpeg concatenation failed with exit code " << exitCode << std::endl;
        writeNativeLog(("CONCAT_FAIL: FFmpeg concatenation failed with exit code " + std::to_string(exitCode)).c_str());
        return false;
    }

    writeNativeLog("CONCAT_SUCCESS: Segments concatenated successfully.");
    return true;
}

void Recorder::cleanupSegmentFiles() {
    for (const auto& path : segmentPaths_) {
        DeleteFileA(path.c_str());
    }
    segmentPaths_.clear();
    writeNativeLog("CLEANUP: Segment files cleaned up.");
}

// --- Microphone specific implementations ---

std::string Recorder::buildMicFFmpegCommand(const RecordingConfig& config) {
    std::ostringstream cmd;
    cmd << "\"" << ffmpegPath_ << "\"";
    cmd << " -y"; // Overwrite output files without asking

    // Input: WASAPI loopback for microphone
    cmd << " -f dshow -i audio=\"" << config.microphoneDevice << "\"" ;

    // Audio encoder
    cmd << " -c:a aac";
    cmd << " -b:a " << config.audioBitrate << "k";
    cmd << " -threads 2";

    // Output
    cmd << " -f matroska \"" << currentMicSegmentPath_ << "\"";

    return cmd.str();
}

bool Recorder::startMicFFmpeg(const std::string& command) {
    writeNativeLog(("S1_MIC: Starting mic FFmpeg with command: " + command).c_str());
    std::cerr << "[RECORDER] Starting mic FFmpeg: " << command << std::endl;

    // Create pipes for stdin, stdout, stderr
    SECURITY_ATTRIBUTES saAttr = {};
    saAttr.nLength = sizeof(SECURITY_ATTRIBUTES);
    saAttr.bInheritHandle = TRUE;
    saAttr.lpSecurityDescriptor = NULL;

    HANDLE hChildStdinRd, hChildStdinWr;
    HANDLE hChildStdoutRd, hChildStdoutWr;
    HANDLE hChildStderrRd, hChildStderrWr;

    if (!CreatePipe(&hChildStdinRd, &hChildStdinWr, &saAttr, 0)) {
        writeNativeLog("START_FAIL_MIC: Stdin pipe creation failed.");
        return false;
    }
    if (!SetHandleInformation(hChildStdinWr, HANDLE_FLAG_INHERIT, 0)) { // Child doesn't inherit write handle
        writeNativeLog("START_FAIL_MIC: SetHandleInformation for StdinWr failed.");
        CloseHandle(hChildStdinRd); CloseHandle(hChildStdinWr);
        return false;
    }

    if (!CreatePipe(&hChildStdoutRd, &hChildStdoutWr, &saAttr, 0)) {
        writeNativeLog("START_FAIL_MIC: Stdout pipe creation failed.");
        CloseHandle(hChildStdinRd); CloseHandle(hChildStdinWr);
        CloseHandle(hChildStdoutRd); CloseHandle(hChildStdoutWr);
        return false;
    }
    if (!SetHandleInformation(hChildStdoutRd, HANDLE_FLAG_INHERIT, 0)) { // Child doesn't inherit read handle
        writeNativeLog("START_FAIL_MIC: SetHandleInformation for StdoutRd failed.");
        CloseHandle(hChildStdinRd); CloseHandle(hChildStdinWr);
        CloseHandle(hChildStdoutRd); CloseHandle(hChildStdoutWr);
        return false;
    }

    if (!CreatePipe(&hChildStderrRd, &hChildStderrWr, &saAttr, 0)) {
        writeNativeLog("START_FAIL_MIC: Stderr pipe creation failed.");
        CloseHandle(hChildStdinRd); CloseHandle(hChildStdinWr);
        CloseHandle(hChildStdoutRd); CloseHandle(hChildStdoutWr);
        return false;
    }
    if (!SetHandleInformation(hChildStderrRd, HANDLE_FLAG_INHERIT, 0)) { // Child doesn't inherit read handle
        writeNativeLog("START_FAIL_MIC: SetHandleInformation for StderrRd failed.");
        CloseHandle(hChildStdinRd); CloseHandle(hChildStdinWr);
        CloseHandle(hChildStdoutRd); CloseHandle(hChildStdoutWr);
        CloseHandle(hChildStderrRd); CloseHandle(hChildStderrWr);
        return false;
    }

    STARTUPINFOA si = {};
    si.cb = sizeof(si);
    si.hStdError = hChildStderrWr;
    si.hStdOutput = hChildStdoutWr;
    si.hStdInput = hChildStdinRd;
    si.dwFlags |= STARTF_USESTDHANDLES;

    PROCESS_INFORMATION pi = {};

    // Convert command string to mutable char array
    std::vector<char> cmdBuf(command.begin(), command.end());
    cmdBuf.push_back('\0');

    if (!CreateProcessA(NULL,   // No module name (use command line)
                        cmdBuf.data(),           // Command line
                        NULL,           // Process handle not inheritable
                        NULL,           // Thread handle not inheritable
                        TRUE,           // Set handle inheritance to TRUE
                        CREATE_NO_WINDOW, // No console window
                        NULL,           // Use parent's environment block
                        NULL,           // Use parent's starting directory
                        &si,            // Pointer to STARTUPINFO structure
                        &pi))           // Pointer to PROCESS_INFORMATION structure
    {
        std::cerr << "[RECORDER] Mic CreateProcess failed (" << GetLastError() << ")." << std::endl;
        writeNativeLog(("START_FAIL_MIC: CreateProcess failed with error " + std::to_string(GetLastError())).c_str());
        CloseHandle(hChildStdinRd); CloseHandle(hChildStdinWr);
        CloseHandle(hChildStdoutRd); CloseHandle(hChildStdoutWr);
        CloseHandle(hChildStderrRd); CloseHandle(hChildStderrWr);
        return false;
    }

    // Close handles to the child process's stdin and stdout that are used by the parent.
    CloseHandle(hChildStdinRd);
    CloseHandle(hChildStdoutWr);
    CloseHandle(hChildStderrWr);

    micFfmpegProcess_ = pi;
    micFfmpegStdin_ = hChildStdinWr;

    // Mic FFmpeg doesn't have a "ready" signal like main FFmpeg, so we just assume it starts.
    // In a real application, you might want to parse its stderr for errors.
    CloseHandle(hChildStderrRd);
    CloseHandle(hChildStdoutRd);

    writeNativeLog("S1_SUCCESS_MIC: Mic FFmpeg process started.");
    return true;
}

bool Recorder::stopMicFFmpegGracefully(int timeoutMs) {
    writeNativeLog("FFG1_MIC: stopMicFFmpegGracefully() called");
    std::cerr << "[RECORDER] stopMicFFmpegGracefully() called" << std::endl;

    if (micFfmpegProcess_.hProcess == NULL) {
        writeNativeLog("FFG1_MIC: No mic FFmpeg process to stop");
        return true;
    }

    PROCESS_INFORMATION currentMicFfmpegProcess = micFfmpegProcess_;
    HANDLE currentMicFfmpegStdin = micFfmpegStdin_;

    // Clear current process info immediately
    memset(&micFfmpegProcess_, 0, sizeof(micFfmpegProcess_));
    micFfmpegStdin_ = nullptr;

    // Send 'q' to FFmpeg's stdin to gracefully quit
    if (currentMicFfmpegStdin) {
        DWORD bytesWritten;
        if (!WriteFile(currentMicFfmpegStdin, "q\n", 2, &bytesWritten, NULL)) {
            std::cerr << "[RECORDER] ERROR: Failed to write 'q' to mic FFmpeg stdin. Error: " << GetLastError() << std::endl;
            writeNativeLog("FFG2_ERROR_MIC: Failed to write 'q' to mic FFmpeg stdin, forcing termination.");
            TerminateProcess(currentMicFfmpegProcess.hProcess, 0);
        }
        if (!FlushFileBuffers(currentMicFfmpegStdin)) {
            std::cerr << "[RECORDER] ERROR: Failed to flush mic FFmpeg stdin. Error: " << GetLastError() << std::endl;
            writeNativeLog("FFG2_ERROR_MIC: Failed to flush mic FFmpeg stdin, forcing termination.");
            TerminateProcess(currentMicFfmpegProcess.hProcess, 0);
        }
        CloseHandle(currentMicFfmpegStdin);
        writeNativeLog("FFG2_MIC: Sent 'q' to mic FFmpeg, initiating async cleanup...");
    } else if (currentMicFfmpegProcess.hProcess) {
        std::cerr << "[RECORDER] stopMicFFmpegGracefully() - no stdin handle, forcing termination" << std::endl;
        writeNativeLog("FFG2_MIC: No stdin handle, forcing termination");
        TerminateProcess(currentMicFfmpegProcess.hProcess, 0);
    }

    // Detach the actual waiting and cleanup to an asynchronous thread
    std::thread(&Recorder::waitForFFmpegExitAndCleanup, this, currentMicFfmpegProcess, currentMicFfmpegStdin, timeoutMs, currentMicSegmentPath_, finalMicAudioPath_, true).detach();

    return true;
}

std::string Recorder::generateMicSegmentPath() {
    micSegmentCounter_++;

    std::cerr << "[RECORDER] generateMicSegmentPath() - finalMicAudioPath_='" << finalMicAudioPath_ << "'" << std::endl;

    // Extract directory and base name from finalMicAudioPath_
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

    // Remove extension
    size_t dotPos = baseName.rfind('.');
    if (dotPos != std::string::npos) {
        ext = baseName.substr(dotPos);
        baseName = baseName.substr(0, dotPos);
    } else {
        ext = ".mkv";
    }

    // Generate segment path: recording_mic_seg001.mkv, recording_mic_seg002.mkv, etc.
    char segNum[16];
    sprintf_s(segNum, sizeof(segNum), "_mic_seg%03d", micSegmentCounter_);
    currentMicSegmentPath_ = dir + baseName + segNum + ext;
    return currentMicSegmentPath_;
}

bool Recorder::concatenateMicSegments() {
    if (micSegmentPaths_.empty()) {
        writeNativeLog("CONCAT_FAIL_MIC: No mic segments to concatenate.");
        return false;
    }

    // Create a temporary concat file list
    std::string concatListPath = finalMicAudioPath_ + ".concat.txt";
    std::ofstream concatFile(concatListPath);
    if (!concatFile.is_open()) {
        writeNativeLog("CONCAT_FAIL_MIC: Failed to create mic concat list file.");
        return false;
    }
    for (const auto& path : micSegmentPaths_) {
        concatFile << "file '" << path << "'\n";
    }
    concatFile.close();

    // Build FFmpeg concat command
    std::ostringstream cmd;
    cmd << "\"" << ffmpegPath_ << "\"";
    cmd << " -y -f concat -safe 0 -i \"" << concatListPath << "\"";
    cmd << " -c copy \"" << finalMicAudioPath_ << "\"";

    writeNativeLog(("CONCAT_CMD_MIC: " + cmd.str()).c_str());

    // Execute FFmpeg concat command
    PROCESS_INFORMATION pi = {};
    STARTUPINFOA si = {};
    si.cb = sizeof(si);
    std::vector<char> cmdBuf(cmd.str().begin(), cmd.str().end());
    cmdBuf.push_back('\0');

    if (!CreateProcessA(NULL, cmdBuf.data(), NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
        std::cerr << "[RECORDER] Mic Concatenation CreateProcess failed (" << GetLastError() << ")." << std::endl;
        writeNativeLog(("CONCAT_FAIL_MIC: CreateProcess failed with error " + std::to_string(GetLastError())).c_str());
        return false;
    }

    WaitForSingleObject(pi.hProcess, INFINITE);

    DWORD exitCode;
    GetExitCodeProcess(pi.hProcess, &exitCode);

    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);

    // Delete the concat list file
    DeleteFileA(concatListPath.c_str());

    if (exitCode != 0) {
        std::cerr << "[RECORDER] Mic FFmpeg concatenation failed with exit code " << exitCode << std::endl;
        writeNativeLog(("CONCAT_FAIL_MIC: Mic FFmpeg concatenation failed with exit code " + std::to_string(exitCode)).c_str());
        return false;
    }

    writeNativeLog("CONCAT_SUCCESS_MIC: Mic segments concatenated successfully.");
    return true;
}

void Recorder::cleanupMicSegmentFiles() {
    for (const auto& path : micSegmentPaths_) {
        DeleteFileA(path.c_str());
    }
    micSegmentPaths_.clear();
    writeNativeLog("CLEANUP_MIC: Mic segment files cleaned up.");
}

// Global functions
bool initRecorder(const std::wstring& modulePath) {
    if (g_recorder) {
        return true; // Already initialized
    }
    g_recorder = new Recorder();
    if (!g_recorder->initialize(modulePath)) {
        delete g_recorder;
        g_recorder = nullptr;
        return false;
    }
    return true;
}

Recorder* getRecorder() {
    return g_recorder;
}

void shutdownRecorder() {
    if (g_recorder) {
        delete g_recorder;
        g_recorder = nullptr;
    }
}

// WASAPI system audio capture (replaces VB-Audio Virtual Cable)
bool Recorder::startSystemAudioCapture() {
    writeNativeLog("Recorder::startSystemAudioCapture() called");
    if (systemAudioCapture_) {
        writeNativeLog("System audio capture already running.");
        return true;
    }

    systemAudioCapture_ = std::make_unique<AudioCapture>(true); // true for loopback
    if (!systemAudioCapture_->initialize()) {
        writeNativeLog("Failed to initialize system audio capture.");
        systemAudioCapture_.reset();
        return false;
    }

    // Generate a unique pipe name
    systemAudioPipeName_ = "\\\\.\\pipe\\ScreenCraft_SystemAudio_" + std::to_string(GetCurrentProcessId());
    if (!systemAudioCapture_->createNamedPipe(systemAudioPipeName_)) {
        writeNativeLog("Failed to create named pipe for system audio.");
        systemAudioCapture_.reset();
        return false;
    }

    if (!systemAudioCapture_->start()) {
        writeNativeLog("Failed to start system audio capture.");
        systemAudioCapture_.reset();
        return false;
    }

    writeNativeLog("System audio capture started successfully.");
    return true;
}

void Recorder::stopSystemAudioCapture() {
    writeNativeLog("Recorder::stopSystemAudioCapture() called");
    if (systemAudioCapture_) {
        systemAudioCapture_->stop();
        systemAudioCapture_->closeNamedPipe();
        systemAudioCapture_.reset();
        writeNativeLog("System audio capture stopped and cleaned up.");
    }
}
