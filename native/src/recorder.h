/**
 * FFmpeg-based Screen Recording Implementation
 *
 * Uses Windows native screen capture + FFmpeg for encoding.
 * Spawns ffmpeg as a child process and communicates via pipe.
 */

#pragma once

#include <string>
#include <vector>
#include <memory>
#include <cstdint>
#include <windows.h>

/**
 * Recording configuration
 */
struct RecordingConfig {
    int width = 1920;
    int height = 1080;
    int fps = 60;
    int videoBitrate = 15000;  // kbps
    int audioBitrate = 192;    // kbps
    std::string savePath;
    bool separateAudio = false;
    bool remuxToMp4 = false;
    bool captureAudio = true;   // Enable system audio by default
    bool captureMicrophone = false;  // Enable microphone capture
    std::string microphoneDevice;     // Microphone device name (empty = default)
};

/**
 * Encoder type
 */
enum class EncoderType {
    NVENC,   // NVIDIA NVENC
    AMF,     // AMD AMF
    QSV,     // Intel Quick Sync Video
    X264,    // Software fallback
    NONE
};

/**
 * Recording state
 */
enum class RecordingState {
    IDLE,
    RECORDING,
    PAUSED
};

class Recorder {
public:
    Recorder();
    ~Recorder();

    /**
     * Initialize recorder
     * @param modulePath Path to FFmpeg directory
     * @return true if initialized successfully
     */
    bool initialize(const std::wstring& modulePath);

    /**
     * Shutdown recorder
     */
    void shutdown();

    /**
     * Start recording
     * @param config Recording configuration
     * @return true if started successfully
     */
    bool startRecording(const RecordingConfig& config);

    /**
     * Stop recording
     */
    void stopRecording();

    /**
     * Pause recording
     */
    void pauseRecording();

    /**
     * Resume recording
     */
    void resumeRecording();

    /**
     * Check if currently recording
     */
    bool isRecording() const;

    /**
     * Check if paused
     */
    bool isPaused() const;

    /**
     * Get current recording state
     */
    RecordingState getState() const;

    /**
     * Get available encoder type
     */
    EncoderType getAvailableEncoder() const;

    /**
     * Get output file path
     */
    std::string getOutputPath() const;

    /**
     * Get microphone audio file path (empty if mic not enabled)
     */
    std::string getMicAudioPath() const;

    /**
     * Set callback for recording stop
     */
    using StopCallback = void(*)(void*);
    void setStopCallback(StopCallback callback, void* userData);

    /**
     * Signal stop from callback
     */
    void signal_stop(bool success);

    /**
     * Check if FFmpeg is available
     */
    bool isFFmpegAvailable() const;

private:
    // FFmpeg process
    PROCESS_INFORMATION ffmpegProcess_ = {};
    HANDLE ffmpegStdin_ = nullptr;
    std::string ffmpegPath_;

    // State
    RecordingState state_ = RecordingState::IDLE;
    RecordingConfig config_;
    EncoderType encoderType_ = EncoderType::X264;
    std::string outputPath_;

    // Callbacks
    StopCallback stopCallback_ = nullptr;
    void* stopCallbackData_ = nullptr;

    // Track pause duration
    int64_t totalPausedDuration_ = 0;
    int64_t pauseBeginTime_ = 0;

    // Segment tracking for pause/resume
    std::vector<std::string> segmentPaths_;    // Completed segment file paths
    std::string currentSegmentPath_;            // Currently recording segment
    int segmentCounter_ = 0;
    std::string finalOutputPath_;               // User-specified final output path

    // Microphone separate FFmpeg process
    PROCESS_INFORMATION micFfmpegProcess_ = {};
    HANDLE micFfmpegStdin_ = nullptr;
    std::vector<std::string> micSegmentPaths_;
    std::string currentMicSegmentPath_;
    int micSegmentCounter_ = 0;
    std::string finalMicAudioPath_;

    // Check for hardware encoders
    EncoderType checkHardwareEncoders();

    // Build FFmpeg command line
    std::string buildFFmpegCommand(const RecordingConfig& config);

    // Start FFmpeg process
    bool startFFmpeg(const std::string& command);

    // Wait for FFmpeg to actually start recording by parsing stderr
    bool waitForFFmpegReady(HANDLE hStderrRead, int timeoutMs);

    // Gracefully stop the current FFmpeg process
    bool stopFFmpegGracefully(int timeoutMs = 10000);

    // Generate next segment file path (e.g. recording_seg001.mkv)
    std::string generateSegmentPath();

    // Concatenate all segments using ffmpeg concat demuxer
    bool concatenateSegments();

    // Delete temporary segment files
    void cleanupSegmentFiles();

    // Microphone FFmpeg process management
    std::string buildMicFFmpegCommand(const RecordingConfig& config);
    bool startMicFFmpeg(const std::string& command);
    bool stopMicFFmpegGracefully(int timeoutMs = 10000);
    std::string generateMicSegmentPath();
    bool concatenateMicSegments();
    void cleanupMicSegmentFiles();
};

// Global functions
extern Recorder* g_recorder;

bool initRecorder(const std::wstring& modulePath);
Recorder* getRecorder();
void shutdownRecorder();
