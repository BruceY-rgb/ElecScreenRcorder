/**
 * recorder_core.exe - C++ Native Core Entry Point
 *
 * This is the main entry point for the screen recording tool's native core.
 * Communication with Electron happens via stdio JSON protocol.
 *
 * Key setup:
 * - Binary mode for stdin/stdout (prevents \n -> \r\n conversion)
 * - DPI awareness for accurate mouse coordinates
 * - High-precision timer initialization
 * - Input capture (keyboard/mouse hooks + polling)
 * - CSV writer for input events
 */

#include "protocol.h"
#include "utils.h"
#include "input_capture.h"
#include "csv_writer.h"
#include "recorder.h"
#include "system_info.h"

#include <iostream>
#include <string>
#include <memory>
#include <windows.h>

// Application state
enum class AppState {
    READY,
    RECORDING,
    PAUSED,
    STOPPED
};

class RecorderApp {
private:
    AppState state_ = AppState::READY;
    std::string currentVideoPath_;
    std::string currentActionsPath_;
    std::string currentMovementsPath_;
    int64_t recordingStartTime_ = 0;
    int64_t pauseBeginTime_ = 0;
    int64_t totalPausedDuration_ = 0;

public:
    RecorderApp() = default;

    ~RecorderApp() {
        // Ensure cleanup on destruction
        if (state_ == AppState::RECORDING || state_ == AppState::PAUSED) {
            stopRecording();
        }
    }

    AppState getState() const { return state_; }

    void sendResponse(const std::string& json) {
        std::cout << json << "\n" << std::flush;
    }

    void sendDebug(const std::string& msg) {
        std::cerr << "[DEBUG] " << msg << "\n" << std::flush;
    }

    // Start recording
    bool startRecording(const Command& cmd) {
        const auto& config = cmd.config;

        // Validate config
        if (config.savePath.empty()) {
            return false;
        }

        // Generate paths for CSV files
        currentVideoPath_ = config.savePath;
        size_t extPos = currentVideoPath_.rfind('.');
        if (extPos != std::string::npos) {
            currentActionsPath_ = currentVideoPath_.substr(0, extPos) + "_actions.csv";
            currentMovementsPath_ = currentVideoPath_.substr(0, extPos) + "_movements.csv";
        } else {
            currentActionsPath_ = currentVideoPath_ + "_actions.csv";
            currentMovementsPath_ = currentVideoPath_ + "_movements.csv";
        }

        // Get recording start time
        recordingStartTime_ = getHighPrecisionTimestamp();
        totalPausedDuration_ = 0;

        // Initialize input capture
        initInputCapture();

        // Start CSV writer
        if (!startCsvWriter(currentActionsPath_, currentMovementsPath_, recordingStartTime_)) {
            shutdownInputCapture();
            return false;
        }

        // Start input capture (hooks + polling)
        if (!startInputCapture()) {
            stopCsvWriter();
            shutdownInputCapture();
            return false;
        }

        // Start OBS recording
        RecordingConfig obsConfig;
        obsConfig.width = config.width;
        obsConfig.height = config.height;
        obsConfig.fps = config.fps;
        obsConfig.videoBitrate = (config.height >= 1440) ? 15000 : 10000;
        obsConfig.savePath = config.savePath;
        obsConfig.separateAudio = config.separateAudio;
        obsConfig.remuxToMp4 = config.remuxToMp4;

        Recorder* recorder = getRecorder();
        if (!recorder || !recorder->startRecording(obsConfig)) {
            stopInputCapture();
            stopCsvWriter();
            shutdownInputCapture();
            return false;
        }

        return true;
    }

    // Stop recording
    void stopRecording() {
        // Stop OBS recording
        Recorder* recorder = getRecorder();
        if (recorder) {
            recorder->stopRecording();
        }

        // Stop input capture
        stopInputCapture();

        // Stop CSV writer
        stopCsvWriter();

        // Shutdown input capture
        shutdownInputCapture();
    }

    // Handle START command
    void handleStart(const Command& cmd) {
        if (state_ == AppState::RECORDING) {
            sendResponse(createErrorResponse("Already recording"));
            return;
        }

        if (state_ == AppState::PAUSED) {
            sendResponse(createErrorResponse("Recording is paused, resume first"));
            return;
        }

        if (!startRecording(cmd)) {
            sendResponse(createErrorResponse("Failed to start recording"));
            return;
        }

        state_ = AppState::RECORDING;
        sendResponse(createStatusResponse("recording"));
    }

    // Handle STOP command
    void handleStop() {
        if (state_ != AppState::RECORDING && state_ != AppState::PAUSED) {
            sendResponse(createErrorResponse("Not recording"));
            return;
        }

        std::string videoPath = currentVideoPath_;
        std::string actionsPath = currentActionsPath_;
        std::string movementsPath = currentMovementsPath_;

        stopRecording();

        state_ = AppState::READY;
        currentVideoPath_.clear();
        currentActionsPath_.clear();
        currentMovementsPath_.clear();
        recordingStartTime_ = 0;
        totalPausedDuration_ = 0;

        sendResponse(createFinishResponse(videoPath, actionsPath, movementsPath));
    }

    // Handle PAUSE command
    void handlePause() {
        if (state_ != AppState::RECORDING) {
            sendResponse(createErrorResponse("Not recording"));
            return;
        }

        pauseBeginTime_ = getHighPrecisionTimestamp();

        // Pause CSV writer
        pauseCsvWriter();

        // Pause OBS recording
        Recorder* recorder = getRecorder();
        if (recorder) {
            recorder->pauseRecording();
        }

        state_ = AppState::PAUSED;
        sendResponse(createStatusResponse("paused"));
    }

    // Handle RESUME command
    void handleResume() {
        if (state_ != AppState::PAUSED) {
            sendResponse(createErrorResponse("Not paused"));
            return;
        }

        int64_t pauseEnd = getHighPrecisionTimestamp();
        totalPausedDuration_ += (pauseEnd - pauseBeginTime_);

        // Resume CSV writer
        resumeCsvWriter();

        // Resume OBS recording
        Recorder* recorder = getRecorder();
        if (recorder) {
            recorder->resumeRecording();
        }

        state_ = AppState::RECORDING;
        sendResponse(createStatusResponse("recording"));
    }

    // Handle SYSINFO command
    void handleSysInfo() {
        // Get comprehensive system information
        SystemInfo info = getSystemInfo();

        sendResponse(createSysInfoResponseEx(info));
    }

    // Process a single command
    void processCommand(const Command& cmd) {
        switch (cmd.type) {
            case CommandType::START:
                handleStart(cmd);
                break;
            case CommandType::STOP:
                handleStop();
                break;
            case CommandType::PAUSE:
                handlePause();
                break;
            case CommandType::RESUME:
                handleResume();
                break;
            case CommandType::SYSINFO:
                handleSysInfo();
                break;
            case CommandType::QUIT:
                // Ensure recording is stopped before quit
                if (state_ == AppState::RECORDING || state_ == AppState::PAUSED) {
                    handleStop();
                }
                state_ = AppState::STOPPED;
                sendResponse(createStatusResponse("stopped"));
                break;
            default:
                sendResponse(createErrorResponse("Unknown command: " + cmd.rawAction));
                break;
        }
    }

    // Process input line
    void processLine(const std::string& line) {
        auto cmdOpt = parseCommand(line);

        if (!cmdOpt) {
            sendResponse(createErrorResponse("JSON parse error"));
            return;
        }

        processCommand(*cmdOpt);
    }

    bool shouldContinue() const {
        return state_ != AppState::STOPPED;
    }
};

int main() {
    // CRITICAL: Setup binary mode first (before any I/O)
    setupBinaryMode();

    // Setup DPI awareness for accurate mouse coordinates
    setupDpiAwareness();

    // Initialize high-resolution timer
    initHighResTimer();

    // Initialize CSV writer system
    initCsvWriter();

    // Initialize OBS recorder
    // Get the module path from the executable directory
    wchar_t exePath[MAX_PATH];
    GetModuleFileNameW(NULL, exePath, MAX_PATH);
    std::wstring modulePath = exePath;
    size_t lastSlash = modulePath.rfind(L'\\');
    if (lastSlash != std::wstring::npos) {
        modulePath = modulePath.substr(0, lastSlash);
    }

    if (!initRecorder(modulePath)) {
        std::cerr << "[MAIN] Failed to initialize recorder\n";
    }

    // Create application instance
    RecorderApp app;

    // Send ready status
    app.sendResponse(createStatusResponse("ready"));

    // Main command loop
    std::string line;
    while (app.shouldContinue()) {
        if (!std::getline(std::cin, line)) {
            // EOF or error
            break;
        }

        if (line.empty()) {
            continue;
        }

        app.processLine(line);
    }

    // Cleanup
    shutdownRecorder();
    shutdownCsvWriter();
    cleanupHighResTimer();

    return 0;
}
