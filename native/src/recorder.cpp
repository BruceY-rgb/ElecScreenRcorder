/**
 * OBS Recording Implementation
 *
 * Handles OBS initialization, screen capture, hardware encoding,
 * audio recording, and recording lifecycle.
 */

#include "recorder.h"
#include "utils.h"

#include <obs.hpp>
#include <util/platform.h>
#include <windows.h>

#include <iostream>
#include <memory>
#include <vector>

// Global recorder instance
Recorder* g_recorder = nullptr;

// OBS signal handler callback
void on_output_stop(void* param, calldata_t* data) {
    (void)param;
    (void)data;

    if (g_recorder) {
        g_recorder->signal_stop(true);
    }
}

Recorder::Recorder()
    : obs_(nullptr)
    , output_(nullptr)
    , videoEncoder_(nullptr)
    , audioEncoder_(nullptr)
    , audioEncoder2_(nullptr)
    , screenSource_(nullptr)
    , systemAudioSource_(nullptr)
    , microphoneSource_(nullptr)
    , scene_(nullptr)
{
}

Recorder::~Recorder() {
    shutdown();
}

bool Recorder::initialize(const std::wstring& modulePath) {
    // Initialize OBS with English locale
    if (!obs_startup("en-US", nullptr, nullptr)) {
        std::cerr << "[RECORDER] Failed to initialize OBS\n";
        return false;
    }

    // Configure video
    if (!initVideo()) {
        obs_shutdown();
        return false;
    }

    // Configure audio
    if (!initAudio()) {
        obs_shutdown();
        return false;
    }

    // Add data and module paths
    std::wstring dataPath = modulePath + L"\\data\\libobs\\";
    std::wstring pluginPath = modulePath + L"\\obs-plugins\\64bit\\";

    obs_add_data_path(os_wcs_to_utf8(dataPath.c_str(), dataPath.length()));
    obs_add_module_path(os_wcs_to_utf8(pluginPath.c_str(), pluginPath.length()),
                       (dataPath + L"\\obs-plugins\\%module%\\").c_str());

    // Load all modules
    obs_load_all_modules();
    obs_post_load_modules();

    std::cout << "[RECORDER] OBS initialized successfully\n";
    return true;
}

bool Recorder::initVideo() {
    obs_video_info ovi;
    ovi.graphics_module = "libobs-d3d11.dll";
    ovi.base_width = 1920;
    ovi.base_height = 1080;
    ovi.output_width = 1920;
    ovi.output_height = 1080;
    ovi.output_format = VIDEO_FORMAT_NV12;
    ovi.colorspace = VIDEO_CS_709;
    ovi.range = VIDEO_RANGE_PARTIAL;
    ovi.scale_type = OBS_SCALE_BICUBIC;
    ovi.fps_num = 60;
    ovi.fps_den = 1;

    if (!obs_reset_video(&ovi)) {
        std::cerr << "[RECORDER] Failed to reset video\n";
        return false;
    }

    return true;
}

bool Recorder::initAudio() {
    obs_audio_info oai;
    oai.samples_per_sec = 48000;
    oai.channels = 2;
    oai.format = AUDIO_FORMAT_FLOAT_PLANAR;

    if (!obs_reset_audio(&oai)) {
        std::cerr << "[RECORDER] Failed to reset audio\n";
        return false;
    }

    return true;
}

bool Recorder::createSources() {
    // Create screen capture source (monitor_capture)
    obs_source_info* monitorCaptureInfo = obs_get_source_by_name("monitor_capture");
    if (!monitorCaptureInfo) {
        std::cerr << "[RECORDER] monitor_capture source type not found\n";
        return false;
    }

    // Create screen source
    screenSource_ = obs_source_create("monitor_capture", "screen_capture", nullptr, nullptr);
    if (!screenSource_) {
        std::cerr << "[RECORDER] Failed to create screen source\n";
        return false;
    }

    // Configure screen capture
    obs_data_t* screenSettings = obs_data_create();
    obs_data_set_string(screenSettings, "capture_mode", "fullscreen");
    obs_data_set_int(screenSettings, "monitor", 0);  // Primary monitor
    obs_source_update(screenSource_, screenSettings);
    obs_data_release(screenSettings);

    // Create scene and add screen source
    scene_ = obs_scene_create("recording_scene");
    if (!scene_) {
        std::cerr << "[RECORDER] Failed to create scene\n";
        return false;
    }

    obs_sceneitem_add(scene_, screenSource_);

    // Create system audio source (WASAPI output capture)
    systemAudioSource_ = obs_source_create("wasapi_output_capture", "system_audio", nullptr, nullptr);
    if (systemAudioSource_) {
        obs_data_t* audioSettings = obs_data_create();
        obs_data_set_string(audioSettings, "device_id", "default");
        obs_source_update(systemAudioSource_, audioSettings);
        obs_data_release(audioSettings);
    }

    // Create microphone source (WASAPI input capture)
    microphoneSource_ = obs_source_create("wasapi_input_capture", "microphone", nullptr, nullptr);
    if (microphoneSource_) {
        obs_data_t* micSettings = obs_data_create();
        obs_data_set_string(micSettings, "device_id", "default");
        obs_source_update(microphoneSource_, micSettings);
        obs_data_release(micSettings);
    }

    return true;
}

EncoderType Recorder::getAvailableEncoder() const {
    // Check for NVENC
    if (obs_get_encoder_by_name("jim_nvenc")) {
        return EncoderType::NVENC;
    }

    // Check for AMF
    if (obs_get_encoder_by_name("h264_texture_amf")) {
        return EncoderType::AMF;
    }

    // Check for QSV
    if (obs_get_encoder_by_name("obs_qsv11_v2")) {
        return EncoderType::QSV;
    }

    // Check for x264 (always available)
    if (obs_get_encoder_by_name("obs_x264")) {
        return EncoderType::X264;
    }

    return EncoderType::NONE;
}

bool Recorder::createEncoders() {
    // Determine available encoder
    encoderType_ = getAvailableEncoder();

    const char* encoderId = nullptr;
    switch (encoderType_) {
        case EncoderType::NVENC:
            encoderId = "jim_nvenc";
            std::cout << "[RECORDER] Using NVENC encoder\n";
            break;
        case EncoderType::AMF:
            encoderId = "h264_texture_amf";
            std::cout << "[RECORDER] Using AMF encoder\n";
            break;
        case EncoderType::QSV:
            encoderId = "obs_qsv11_v2";
            std::cout << "[RECORDER] Using QSV encoder\n";
            break;
        case EncoderType::X264:
            encoderId = "obs_x264";
            std::cout << "[RECORDER] Using x264 encoder\n";
            break;
        default:
            std::cerr << "[RECORDER] No encoder available\n";
            return false;
    }

    // Create video encoder settings
    obs_data_t* videoSettings = obs_data_create();
    obs_data_set_int(videoSettings, "bitrate", config_.videoBitrate);
    obs_data_set_string(videoSettings, "rate_control", "CBR");
    obs_data_set_string(videoSettings, "preset", "quality");
    obs_data_set_string(videoSettings, "profile", "high");
    obs_data_set_string(videoSettings, "tune", "zerolatency");

    // Create video encoder
    videoEncoder_ = obs_video_encoder_create(encoderId, "video_encoder", videoSettings, nullptr);
    if (!videoEncoder_) {
        std::cerr << "[RECORDER] Failed to create video encoder\n";
        obs_data_release(videoSettings);
        return false;
    }
    obs_data_release(videoSettings);

    // Set video encoder output
    obs_encoder_set_video(videoEncoder_, obs_get_video());

    // Create audio encoder (AAC)
    obs_data_t* audioSettings = obs_data_create();
    obs_data_set_int(audioSettings, "bitrate", config_.audioBitrate);

    audioEncoder_ = obs_audio_encoder_create("ffmpeg_aac", "audio_encoder", audioSettings, nullptr);
    if (!audioEncoder_) {
        std::cerr << "[RECORDER] Failed to create audio encoder\n";
        obs_data_release(audioSettings);
        return false;
    }
    obs_data_release(audioSettings);

    obs_encoder_set_audio(audioEncoder_, obs_get_audio());

    // Create second audio encoder for separate track
    if (config_.separateAudio) {
        obs_data_t* audioSettings2 = obs_data_create();
        obs_data_set_int(audioSettings2, "bitrate", config_.audioBitrate);

        audioEncoder2_ = obs_audio_encoder_create("ffmpeg_aac", "audio_encoder_2", audioSettings2, nullptr);
        if (audioEncoder2_) {
            obs_encoder_set_audio(audioEncoder2_, obs_get_audio());
        }
        obs_data_release(audioSettings2);
    }

    return true;
}

bool Recorder::createOutput() {
    // Create ffmpeg_muxer output
    output_ = obs_output_create("ffmpeg_muxer", "recording_output", nullptr, nullptr);
    if (!output_) {
        std::cerr << "[RECORDER] Failed to create output\n";
        return false;
    }

    // Configure output settings
    obs_data_t* outputSettings = obs_data_create();
    obs_data_set_string(outputSettings, "path", config_.savePath.c_str());
    obs_data_set_string(outputSettings, "format", "mkv");
    obs_data_set_bool(outputSettings, "write_file", true);

    obs_output_update(output_, outputSettings);
    obs_data_release(outputSettings);

    // Connect output to encoders
    obs_output_set_video_encoder(output_, videoEncoder_);
    obs_output_set_audio_encoder(output_, audioEncoder_, 0);

    if (audioEncoder2_) {
        obs_output_set_audio_encoder(output_, audioEncoder2_, 1);
    }

    // Set up scene source
    obs_source_t* sceneSource = obs_scene_get_source(scene_);
    obs_set_output_source(0, sceneSource);

    if (systemAudioSource_) {
        obs_set_output_source(1, systemAudioSource_);
    }

    if (microphoneSource_) {
        obs_set_output_source(3, microphoneSource_);
    }

    // Register stop signal handler
    signal_handler_t* signalHandler = obs_output_get_signal_handler(output_);
    signal_handler_connect(signalHandler, "stop", on_output_stop, this);

    return true;
}

void Recorder::shutdown() {
    if (state_ == RecordingState::RECORDING) {
        stopRecording();
    }

    cleanup();

    if (obs_startup()) {
        obs_shutdown();
    }
}

void Recorder::cleanup() {
    // Stop and release output
    if (output_) {
        obs_output_stop(output_);
        obs_output_release(output_);
        output_ = nullptr;
    }

    // Release encoders
    if (videoEncoder_) {
        obs_encoder_release(videoEncoder_);
        videoEncoder_ = nullptr;
    }

    if (audioEncoder_) {
        obs_encoder_release(audioEncoder_);
        audioEncoder_ = nullptr;
    }

    if (audioEncoder2_) {
        obs_encoder_release(audioEncoder2_);
        audioEncoder2_ = nullptr;
    }

    // Release sources
    if (screenSource_) {
        obs_source_release(screenSource_);
        screenSource_ = nullptr;
    }

    if (systemAudioSource_) {
        obs_source_release(systemAudioSource_);
        systemAudioSource_ = nullptr;
    }

    if (microphoneSource_) {
        obs_source_release(microphoneSource_);
        microphoneSource_ = nullptr;
    }

    // Release scene
    if (scene_) {
        obs_scene_release(scene_);
        scene_ = nullptr;
    }

    // Clear output sources
    obs_set_output_source(0, nullptr);
    obs_set_output_source(1, nullptr);
    obs_set_output_source(2, nullptr);
    obs_set_output_source(3, nullptr);
}

bool Recorder::startRecording(const RecordingConfig& config) {
    if (state_ != RecordingState::IDLE) {
        std::cerr << "[RECORDER] Already recording or paused\n";
        return false;
    }

    config_ = config;
    outputPath_ = config.savePath;

    // Create sources
    if (!createSources()) {
        cleanup();
        return false;
    }

    // Create encoders
    if (!createEncoders()) {
        cleanup();
        return false;
    }

    // Create output
    if (!createOutput()) {
        cleanup();
        return false;
    }

    // Start recording
    if (!obs_output_start(output_)) {
        std::cerr << "[RECORDER] Failed to start output: " << obs_output_get_last_error(output_) << "\n";
        cleanup();
        return false;
    }

    state_ = RecordingState::RECORDING;
    totalPausedDuration_ = 0;

    std::cout << "[RECORDER] Recording started: " << outputPath_ << "\n";
    return true;
}

void Recorder::stopRecording() {
    if (state_ == RecordingState::IDLE) {
        return;
    }

    // Stop output
    if (output_) {
        obs_output_stop(output_);
    }

    // Wait for output to stop
    int timeout = 5000;  // 5 seconds
    while (obs_output_active(output_) && timeout > 0) {
        Sleep(100);
        timeout -= 100;
    }

    // Cleanup OBS objects
    cleanup();

    state_ = RecordingState::IDLE;

    std::cout << "[RECORDER] Recording stopped\n";

    // Call stop callback
    if (stopCallback_) {
        stopCallback_(stopCallbackData_);
    }
}

void Recorder::pauseRecording() {
    if (state_ != RecordingState::RECORDING) {
        return;
    }

    if (output_ && obs_output_can_pause(output_)) {
        obs_output_pause(output_, true);
        pauseBeginTime_ = getHighPrecisionTimestamp();
        state_ = RecordingState::PAUSED;
        std::cout << "[RECORDER] Recording paused\n";
    }
}

void Recorder::resumeRecording() {
    if (state_ != RecordingState::PAUSED) {
        return;
    }

    if (output_) {
        obs_output_pause(output_, false);
        totalPausedDuration_ += (getHighPrecisionTimestamp() - pauseBeginTime_);
        state_ = RecordingState::RECORDING;
        std::cout << "[RECORDER] Recording resumed\n";
    }
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

std::string Recorder::getOutputPath() const {
    return outputPath_;
}

void Recorder::setStopCallback(StopCallback callback, void* userData) {
    stopCallback_ = callback;
    stopCallbackData_ = userData;
}

void Recorder::signal_stop(bool success) {
    (void)success;
    // Output has stopped, cleanup will be done in stopRecording
}

// Global recorder functions
bool initRecorder(const std::wstring& modulePath) {
    if (g_recorder) {
        return true;
    }

    g_recorder = new Recorder();
    return g_recorder->initialize(modulePath);
}

Recorder* getRecorder() {
    return g_recorder;
}

void shutdownRecorder() {
    if (g_recorder) {
        g_recorder->shutdown();
        delete g_recorder;
        g_recorder = nullptr;
    }
}
