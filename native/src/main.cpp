/**
 * recorder_core.exe - C++ Native Core Entry Point
 *
 * This is the main entry point for the screen recording tool's native core.
 * Communication with Electron happens via:
 *   - stdio JSON protocol (default, for local mode)
 *   - TCP Socket (with --socket <port> argument, for remote development)
 *
 * Key setup:
 * - Binary mode for stdin/stdout (prevents \n -> \r\n conversion)
 * - DPI awareness for accurate mouse coordinates
 * - High-precision timer initialization
 * - Input capture (keyboard/mouse hooks + polling)
 * - CSV writer for input events
 */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>

#include "protocol.h"
#include "utils.h"
#include "input_capture.h"
#include "csv_writer.h"
#include "recorder.h"
#include "system_info.h"

#include <iostream>
#include <vector>
#include <cstdlib>

// Forward declaration for audio device enumeration
extern std::vector<std::string> getAudioInputDevices();
#include <string>
#include <memory>
#include <vector>
#include <thread>
#include <atomic>
#include <mutex>
#include <queue>

#pragma comment(lib, "ws2_32.lib")

// Application state
enum class AppState {
    READY,
    RECORDING,
    PAUSED,
    STOPPED
};

// Communication mode
enum class CommMode {
    STDIO,  // Default: local stdio mode
    SOCKET  // Remote: TCP socket mode
};

// Abstract output interface for both stdio and socket
class IOutputHandler {
public:
    virtual ~IOutputHandler() = default;
    virtual void send(const std::string& json) = 0;
};

// Stdio output handler (default)
class StdioOutputHandler : public IOutputHandler {
public:
    void send(const std::string& json) override {
        std::cout << json << "\n" << std::flush;
    }
};

// Socket output handler
class SocketOutputHandler : public IOutputHandler {
private:
    SOCKET clientSocket_;
public:
    SocketOutputHandler(SOCKET sock) : clientSocket_(sock) {}

    void send(const std::string& json) override {
        if (clientSocket_ != INVALID_SOCKET) {
            std::string msg = json + "\n";
            ::send(clientSocket_, msg.c_str(), static_cast<int>(msg.length()), 0);
        }
    }
};

// Socket server manager
class SocketServer {
private:
    SOCKET listenSocket_;
    SOCKET clientSocket_;
    std::atomic<bool> running_;
    std::mutex clientMutex_;
    IOutputHandler* outputHandler_;
    std::queue<std::string> messageQueue_;
    std::mutex queueMutex_;
    std::thread acceptThread_;

public:
    SocketServer() : listenSocket_(INVALID_SOCKET), clientSocket_(INVALID_SOCKET),
                     running_(false), outputHandler_(nullptr) {}

    ~SocketServer() {
        stop();
    }

    bool start(int port) {
        // Initialize Winsock
        WSADATA wsaData;
        int result = WSAStartup(MAKEWORD(2, 2), &wsaData);
        if (result != 0) {
            std::cerr << "[SOCKET] WSAStartup failed: " << result << "\n";
            return false;
        }

        // Create socket
        listenSocket_ = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (listenSocket_ == INVALID_SOCKET) {
            std::cerr << "[SOCKET] socket failed: " << WSAGetLastError() << "\n";
            WSACleanup();
            return false;
        }

        // Bind to port
        sockaddr_in addr;
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = INADDR_ANY;
        addr.sin_port = htons(static_cast<u_short>(port));

        result = bind(listenSocket_, (sockaddr*)&addr, sizeof(addr));
        if (result == SOCKET_ERROR) {
            std::cerr << "[SOCKET] bind failed: " << WSAGetLastError() << "\n";
            closesocket(listenSocket_);
            WSACleanup();
            return false;
        }

        // Listen
        result = listen(listenSocket_, SOMAXCONN);
        if (result == SOCKET_ERROR) {
            std::cerr << "[SOCKET] listen failed: " << WSAGetLastError() << "\n";
            closesocket(listenSocket_);
            WSACleanup();
            return false;
        }

        running_ = true;
        std::cerr << "[SOCKET] Server listening on port " << port << "\n";

        return true;
    }

    bool acceptClient() {
        if (listenSocket_ == INVALID_SOCKET) {
            return false;
        }

        clientSocket_ = accept(listenSocket_, nullptr, nullptr);
        if (clientSocket_ == INVALID_SOCKET) {
            if (running_) {
                std::cerr << "[SOCKET] accept failed: " << WSAGetLastError() << "\n";
            }
            return false;
        }

        // Create socket output handler
        std::lock_guard<std::mutex> lock(clientMutex_);
        outputHandler_ = new SocketOutputHandler(clientSocket_);

        std::cerr << "[SOCKET] Client connected\n";
        return true;
    }

    IOutputHandler* getOutputHandler() {
        std::lock_guard<std::mutex> lock(clientMutex_);
        return outputHandler_;
    }

    bool isClientConnected() {
        std::lock_guard<std::mutex> lock(clientMutex_);
        return clientSocket_ != INVALID_SOCKET;
    }

    bool readLine(std::string& line) {
        std::lock_guard<std::mutex> lock(clientMutex_);
        if (clientSocket_ == INVALID_SOCKET) {
            return false;
        }

        char buffer[4096];
        int result = recv(clientSocket_, buffer, sizeof(buffer) - 1, 0);
        if (result <= 0) {
            if (result == 0) {
                std::cerr << "[SOCKET] Client disconnected\n";
            } else {
                std::cerr << "[SOCKET] recv failed: " << WSAGetLastError() << "\n";
            }
            return false;
        }

        buffer[result] = '\0';

        // Find newline
        char* newline = strchr(buffer, '\n');
        if (newline) {
            *newline = '\0';
        }

        // Remove carriage return if present
        char* cr = strchr(buffer, '\r');
        if (cr) {
            *cr = '\0';
        }

        line = buffer;
        return true;
    }

    void disconnectClient() {
        std::lock_guard<std::mutex> lock(clientMutex_);
        if (clientSocket_ != INVALID_SOCKET) {
            shutdown(clientSocket_, SD_BOTH);
            closesocket(clientSocket_);
            clientSocket_ = INVALID_SOCKET;
        }
        if (outputHandler_) {
            delete outputHandler_;
            outputHandler_ = nullptr;
        }
    }

    void stop() {
        running_ = false;

        disconnectClient();

        if (listenSocket_ != INVALID_SOCKET) {
            closesocket(listenSocket_);
            listenSocket_ = INVALID_SOCKET;
        }

        WSACleanup();
        std::cerr << "[SOCKET] Server stopped\n";
    }

    bool isRunning() const {
        return running_;
    }
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

    // Communication mode
    CommMode commMode_ = CommMode::STDIO;
    IOutputHandler* outputHandler_ = nullptr;
    SocketServer* socketServer_ = nullptr;

    // For socket mode, we need a thread-safe queue for responses
    std::mutex responseQueueMutex_;
    std::queue<std::string> responseQueue_;

    // Mouse activity tracking
    std::atomic<bool> mouseActivityRunning_{false};
    std::thread mouseActivityThread_;
    int lastMouseMoveCount_ = 0;
    std::mutex mouseActivityMutex_;

public:
    RecorderApp() : outputHandler_(new StdioOutputHandler()) {}

    ~RecorderApp() {
        // Ensure cleanup on destruction
        if (state_ == AppState::RECORDING || state_ == AppState::PAUSED) {
            stopRecording();
        }
        if (outputHandler_ && commMode_ == CommMode::STDIO) {
            delete outputHandler_;
        }
    }

    AppState getState() const { return state_; }

    void setSocketMode(SocketServer* server) {
        commMode_ = CommMode::SOCKET;
        socketServer_ = server;
        // For socket mode, responses are handled differently
    }

    void sendResponse(const std::string& json) {
        if (commMode_ == CommMode::STDIO) {
            outputHandler_->send(json);
        } else {
            // Socket mode: use the server's output handler
            IOutputHandler* handler = socketServer_->getOutputHandler();
            if (handler) {
                handler->send(json);
            }
        }
    }

    void sendDebug(const std::string& msg) {
        std::cerr << "[DEBUG] " << msg << "\n" << std::flush;
    }

    // Start recording
    bool startRecording(const Command& cmd) {
        const auto& config = cmd.config;

        // Debug: log config values
        std::cerr << "[DEBUG] startRecording config: width=" << config.width
                  << " height=" << config.height
                  << " fps=" << config.fps
                  << " bitrate=" << config.bitrate
                  << " captureAudio=" << config.captureAudio
                  << " savePath=" << config.savePath << std::endl;

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
        obsConfig.videoBitrate = config.bitrate > 0 ? config.bitrate : 15000;
        obsConfig.savePath = config.savePath;
        obsConfig.separateAudio = config.separateAudio;
        obsConfig.remuxToMp4 = config.remuxToMp4;
        obsConfig.captureAudio = config.captureAudio;
        obsConfig.captureMicrophone = config.captureMicrophone;
        obsConfig.microphoneDevice = config.microphoneDevice;
        obsConfig.captureHwnd = config.captureHwnd;

        // Override captureHwnd from environment variable for testing
        const char* envHwnd = std::getenv("RECORDER_CAPTURE_HWND");
        if (envHwnd && strlen(envHwnd) > 0) {
            int64_t hwnd = std::strtoll(envHwnd, nullptr, 0);
            if (hwnd > 0) {
                obsConfig.captureHwnd = hwnd;
                std::cerr << "[DEBUG] Using captureHwnd from env: " << hwnd << std::endl;
            }
        }

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

        // Start mouse activity reporting
        startMouseActivityReporting();

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

        // Get mic audio path before stopping (recorder clears state on stop but keeps finalMicAudioPath_)
        Recorder* rec = getRecorder();
        std::string micAudioPath = rec ? rec->getMicAudioPath() : "";

        // Stop mouse activity reporting first
        stopMouseActivityReporting();

        stopRecording();

        state_ = AppState::READY;
        currentVideoPath_.clear();
        currentActionsPath_.clear();
        currentMovementsPath_.clear();
        recordingStartTime_ = 0;
        totalPausedDuration_ = 0;

        sendResponse(createFinishResponse(videoPath, actionsPath, movementsPath, micAudioPath));
    }

    // Handle PAUSE command
    void handlePause() {
        std::cerr << "[MAIN] handlePause() called, current state=" << (int)state_ << std::endl;

        if (state_ != AppState::RECORDING) {
            std::cerr << "[MAIN] handlePause() - NOT RECORDING, returning error" << std::endl;
            sendResponse(createErrorResponse("Not recording"));
            return;
        }

        pauseBeginTime_ = getHighPrecisionTimestamp();

        // Pause CSV writer
        std::cerr << "[MAIN] handlePause() - pausing CSV writer" << std::endl;
        pauseCsvWriter();

        // Stop input capture - prevent events from being recorded during pause
        std::cerr << "[MAIN] handlePause() - stopping input capture" << std::endl;
        stopInputCapture();

        // Pause OBS/FFmpeg recording (stops FFmpeg and saves segment)
        std::cerr << "[MAIN] handlePause() - calling recorder->pauseRecording()" << std::endl;
        Recorder* recorder = getRecorder();
        if (recorder) {
            recorder->pauseRecording();
        }
        std::cerr << "[MAIN] handlePause() - recorder->pauseRecording() returned" << std::endl;

        // Stop mouse activity reporting during pause
        std::cerr << "[MAIN] handlePause() - stopping mouse activity reporting" << std::endl;
        stopMouseActivityReporting();

        state_ = AppState::PAUSED;
        std::cerr << "[MAIN] handlePause() - sending paused response" << std::endl;
        sendResponse(createStatusResponse("paused"));
        std::cerr << "[MAIN] handlePause() - DONE" << std::endl;
    }

    // Handle RESUME command
    void handleResume() {
        std::cerr << "[MAIN] handleResume() called, current state=" << (int)state_ << std::endl;

        if (state_ != AppState::PAUSED) {
            std::cerr << "[MAIN] handleResume() - NOT PAUSED, returning error" << std::endl;
            sendResponse(createErrorResponse("Not paused"));
            return;
        }

        // Restart mouse activity reporting
        std::cerr << "[MAIN] handleResume() - starting mouse activity reporting" << std::endl;
        startMouseActivityReporting();

        int64_t pauseEnd = getHighPrecisionTimestamp();
        totalPausedDuration_ += (pauseEnd - pauseBeginTime_);

        // Resume CSV writer
        std::cerr << "[MAIN] handleResume() - resuming CSV writer" << std::endl;
        resumeCsvWriter();

        // Restart input capture to resume recording events
        std::cerr << "[MAIN] handleResume() - starting input capture" << std::endl;
        startInputCapture();

        // Resume OBS/FFmpeg recording (starts a new segment)
        std::cerr << "[MAIN] handleResume() - calling recorder->resumeRecording()" << std::endl;
        Recorder* recorder = getRecorder();
        if (recorder) {
            recorder->resumeRecording();

            // Check if resume actually succeeded (FFmpeg may have failed to start)
            if (recorder->getState() != RecordingState::RECORDING) {
                std::cerr << "[MAIN] handleResume() - RESUME FAILED, rolling back" << std::endl;
                // Resume failed - roll back
                stopMouseActivityReporting();
                stopInputCapture();
                pauseCsvWriter();
                sendResponse(createErrorResponse("Failed to resume recording: FFmpeg failed to start new segment"));
                return;
            }
        }
        std::cerr << "[MAIN] handleResume() - recorder->resumeRecording() succeeded" << std::endl;

        state_ = AppState::RECORDING;
        sendResponse(createStatusResponse("recording"));
        std::cerr << "[MAIN] handleResume() - DONE" << std::endl;
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
            case CommandType::STATUS: {
                // Return current state
                std::string stateStr;
                switch (state_) {
                    case AppState::READY: stateStr = "ready"; break;
                    case AppState::RECORDING: stateStr = "recording"; break;
                    case AppState::PAUSED: stateStr = "paused"; break;
                    case AppState::STOPPED: stateStr = "stopped"; break;
                    default: stateStr = "unknown"; break;
                }
                sendResponse(createStatusResponse(stateStr));
                break;
            }
            case CommandType::CHECK_INPUT: {
                InputState inputState = getCurrentInputState();
                // Only log if there was actual input to reduce spam
                if (inputState.anyKeyPressed || inputState.mouseButtonPressed) {
                    std::cerr << "[DEBUG] CHECK_INPUT: key=" << inputState.anyKeyPressed
                              << " mouse=" << inputState.mouseButtonPressed << std::endl;
                }
                sendResponse(createInputStateResponse(inputState));
                break;
            }
            case CommandType::GET_AUDIO_DEVICES: {
                std::cerr << "[DEBUG] GET_AUDIO_DEVICES: Enumerating audio input devices" << std::endl;
                std::vector<std::string> devices = getAudioInputDevices();
                std::cerr << "[DEBUG] GET_AUDIO_DEVICES: Found " << devices.size() << " devices" << std::endl;
                for (const auto& dev : devices) {
                    std::cerr << "[DEBUG]   - " << dev << std::endl;
                }
                sendResponse(createAudioDevicesResponse(devices));
                break;
            }
            case CommandType::QUIT:
                // Ensure recording is stopped before quit
                if (state_ == AppState::RECORDING || state_ == AppState::PAUSED) {
                    handleStop();
                }
                state_ = AppState::STOPPED;
                sendResponse(createStatusResponse("stopped"));
                break;
            case CommandType::EXCLUDE_FROM_CAPTURE: {
                HWND hwnd = reinterpret_cast<HWND>(cmd.hwnd);
                std::cerr << "[DEBUG] EXCLUDE_FROM_CAPTURE: hwnd=" << (void*)hwnd << std::endl;

                // Check if window is valid
                BOOL isWindow = IsWindow(hwnd);
                std::cerr << "[DEBUG] IsWindow=" << isWindow << std::endl;

                BOOL result = SetWindowDisplayAffinity(hwnd, WDA_EXCLUDEFROMCAPTURE);
                DWORD error = GetLastError();

                std::cerr << "[DEBUG] SetWindowDisplayAffinity result=" << result
                          << " error=" << error << std::endl;

                if (result) {
                    sendResponse(createStatusResponse("ok"));
                } else {
                    sendResponse(createErrorResponse("Failed to set window affinity, error: " + std::to_string(error)));
                }
                break;
            }
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

    bool isSocketMode() const {
        return commMode_ == CommMode::SOCKET;
    }

    // Start mouse activity reporting thread
    void startMouseActivityReporting() {
        if (mouseActivityRunning_.load()) return;

        lastMouseMoveCount_ = 0;
        mouseActivityRunning_.store(true);
        mouseActivityThread_ = std::thread([this]() {
            while (mouseActivityRunning_.load()) {
                // Get current queue size
                int currentCount = getMouseMoveQueueSize();

                // Calculate events per second (difference from last reading)
                int eventsPerSecond = 0;
                {
                    std::lock_guard<std::mutex> lock(mouseActivityMutex_);
                    eventsPerSecond = currentCount - lastMouseMoveCount_;
                    lastMouseMoveCount_ = currentCount;
                }

                // Send mouse activity update
                if (state_ == AppState::RECORDING) {
                    sendResponse(createMouseActivityResponse(eventsPerSecond));
                }

                // Sleep for 1 second
                std::this_thread::sleep_for(std::chrono::seconds(1));
            }
        });
    }

    // Stop mouse activity reporting thread
    void stopMouseActivityReporting() {
        if (!mouseActivityRunning_.load()) return;

        mouseActivityRunning_.store(false);
        if (mouseActivityThread_.joinable()) {
            mouseActivityThread_.join();
        }
    }
};

// Parse command line arguments
struct CommandLineArgs {
    CommMode mode = CommMode::STDIO;
    int socketPort = 8765;
};

CommandLineArgs parseArgs(int argc, char* argv[]) {
    CommandLineArgs args;

    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];

        if (arg == "--socket" && i + 1 < argc) {
            args.mode = CommMode::SOCKET;
            args.socketPort = std::stoi(argv[++i]);
        } else if (arg == "--socket") {
            args.mode = CommMode::SOCKET;
            args.socketPort = 8765;  // Default port
        } else if (arg == "--stdio") {
            args.mode = CommMode::STDIO;
        } else if (arg == "--port" && i + 1 < argc) {
            args.socketPort = std::stoi(argv[++i]);
        }
    }

    return args;
}

int main(int argc, char* argv[]) {
    // 最开头的诊断 - 在任何函数调用之前
    std::cerr << "[DIAG] main() started, argc=" << argc << std::endl; std::cerr.flush();
    OutputDebugStringW(L"[DIAG] main() started\n");

    // Parse command line arguments
    CommandLineArgs args = parseArgs(argc, argv);

    // If socket mode, initialize socket server first
    SocketServer* socketServer = nullptr;
    if (args.mode == CommMode::SOCKET) {
        socketServer = new SocketServer();
        if (!socketServer->start(args.socketPort)) {
            std::cerr << "[MAIN] Failed to start socket server\n";
            delete socketServer;
            return 1;
        }
    }

    // CRITICAL: Setup binary mode first (only for stdio mode)
    if (args.mode == CommMode::STDIO) {
        std::cerr << "[DIAG] calling setupBinaryMode()" << std::endl; std::cerr.flush();
        setupBinaryMode();
        std::cerr << "[DIAG] setupBinaryMode() done" << std::endl; std::cerr.flush();
    }

    // Setup DPI awareness for accurate mouse coordinates
    std::cerr << "[DIAG] calling setupDpiAwareness()" << std::endl; std::cerr.flush();
    setupDpiAwareness();
    std::cerr << "[DIAG] setupDpiAwareness() done" << std::endl; std::cerr.flush();

    // Initialize high-resolution timer
    std::cerr << "[DIAG] calling initHighResTimer()" << std::endl; std::cerr.flush();
    initHighResTimer();
    std::cerr << "[DIAG] initHighResTimer() done" << std::endl; std::cerr.flush();

    // Initialize CSV writer system
    std::cerr << "[MAIN] initCsvWriter()" << std::endl; std::cerr.flush();
    initCsvWriter();

    // Initialize input capture early so hook-based key state tracking
    // is available for CHECK_INPUT before recording starts
    std::cerr << "[MAIN] initInputCapture()" << std::endl; std::cerr.flush();
    initInputCapture();

    // Initialize OBS recorder
    // Get the module path from the executable directory
    wchar_t exePath[MAX_PATH];
    GetModuleFileNameW(NULL, exePath, MAX_PATH);
    std::wstring modulePath = exePath;
    size_t lastSlash = modulePath.rfind(L'\\');
    if (lastSlash != std::wstring::npos) {
        modulePath = modulePath.substr(0, lastSlash);
    }
    std::cerr << "[MAIN] calling initRecorder()" << std::endl; std::cerr.flush();

    if (!initRecorder(modulePath)) {
        std::cerr << "[MAIN] Failed to initialize recorder\n";
        std::cerr.flush();
        shutdownInputCapture();
        shutdownCsvWriter();
        shutdownRecorder();
        cleanupHighResTimer();
        return 1;
    }

    // Create application instance
    RecorderApp app;

    // If socket mode, set it on the app
    if (args.mode == CommMode::SOCKET) {
        app.setSocketMode(socketServer);
    }

    // Main command loop
    if (args.mode == CommMode::STDIO) {
        // Send ready status (stdio mode only, socket mode sends after client connects)
        app.sendResponse(createStatusResponse("ready"));

        // Original stdio mode
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
    } else {
        // Socket mode
        while (app.shouldContinue()) {
            // Wait for client connection
            if (!socketServer->isClientConnected()) {
                std::cerr << "[MAIN] Waiting for client connection...\n";
                if (!socketServer->acceptClient()) {
                    // Error or server stopped
                    break;
                }
                // Send ready status again after connection
                app.sendResponse(createStatusResponse("ready"));
            }

            // Read and process commands
            std::string line;
            if (!socketServer->readLine(line)) {
                // Client disconnected
                socketServer->disconnectClient();
                continue;
            }

            if (line.empty()) {
                continue;
            }

            app.processLine(line);

            // Check if we should continue (app might have received QUIT)
            if (!app.shouldContinue()) {
                break;
            }
        }
    }

    // Cleanup
    shutdownInputCapture();
    shutdownRecorder();
    shutdownCsvWriter();
    cleanupHighResTimer();

    if (socketServer) {
        socketServer->stop();
        delete socketServer;
    }

    return 0;
}
