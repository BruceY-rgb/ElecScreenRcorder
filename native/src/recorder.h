#pragma once

#include <string>
#include <memory>
#include <cstdint>

/**
 * OBS Recording Manager
 *
 * Handles OBS initialization, screen capture, hardware encoding,
 * audio recording, and recording lifecycle (start/stop/pause/resume).
 */

// Forward declarations
struct obs_video_info;
struct obs_audio_info;
struct obs_output;
struct obs_encoder;
struct obs_source;
struct obs_scene;

/**
 * Recording configuration
 */
struct RecordingConfig {
    int width = 1920;
    int height = 1080;
    int fps = 60;
    int videoBitrate = 15000;  // kbps (configurable)
    int audioBitrate = 192;    // kbps per track
    std::string savePath;
    bool separateAudio = false;
    bool remuxToMp4 = false;
};

/**
 * Encoder type for hardware encoding
 */
enum class EncoderType {
    NVENC,   // NVIDIA NVENC
    AMF,     // AMD AMF
    QSV,     // Intel Quick Sync Video
    X264,    // Software fallback
    NONE     // Not available
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
     * Initialize OBS (must be called once at startup)
     * @param modulePath Path to OBS plugins directory
     * @return true if initialized successfully
     */
    bool initialize(const std::wstring& modulePath);

    /**
     * Shutdown OBS (must be called before exit)
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
     * Set callback for recording stop
     * @param callback Function to call when recording stops
     */
    using StopCallback = void(*)(void*);
    void setStopCallback(StopCallback callback, void* userData);

private:
    // OBS objects
    void* obs_;  // obs_context (opaque)
    obs_output* output_ = nullptr;
    obs_encoder* videoEncoder_ = nullptr;
    obs_encoder* audioEncoder_ = nullptr;
    obs_encoder* audioEncoder2_ = nullptr;  // For separate audio
    obs_source* screenSource_ = nullptr;
    obs_source* systemAudioSource_ = nullptr;
    obs_source* microphoneSource_ = nullptr;
    obs_scene* scene_ = nullptr;

    // State
    RecordingState state_ = RecordingState::IDLE;
    RecordingConfig config_;
    EncoderType encoderType_ = EncoderType::NONE;
    std::string outputPath_;

    // Callbacks
    StopCallback stopCallback_ = nullptr;
    void* stopCallbackData_ = nullptr;

    // Track pause duration
    int64_t totalPausedDuration_ = 0;
    int64_t pauseBeginTime_ = 0;

    // Private methods
    bool initVideo();
    bool initAudio();
    bool createSources();
    bool createEncoders();
    bool createOutput();
    void cleanup();
    void signal_stop(bool success);
};

/**
 * Global recorder instance
 */
extern Recorder* g_recorder;

/**
 * Initialize recorder
 */
bool initRecorder(const std::wstring& modulePath);

/**
 * Get recorder instance
 */
Recorder* getRecorder();

/**
 * Shutdown recorder
 */
void shutdownRecorder();

