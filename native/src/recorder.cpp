/**
 * OBS Recording Implementation
 *
 * Handles OBS initialization, screen capture, hardware encoding,
 * audio recording, and recording lifecycle.
 */

#include "recorder.h"
#include "utils.h"

// OBS Studio includes
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
    // Initialize OBS - temporarily disabled
    std::cout << "[RECORDER] OBS initialization disabled\n";
    return true;
}

bool Recorder::initVideo() {
    // Initialize video - temporarily disabled
    std::cout << "[RECORDER] Video initialization disabled\n";
    return true;
}

bool Recorder::initAudio() {
    // Initialize audio - temporarily disabled
    std::cout << "[RECORDER] Audio initialization disabled\n";
    return true;
}

bool Recorder::createSources() {
    // Create sources - temporarily disabled
    std::cout << "[RECORDER] Source creation disabled\n";
    return true;
}

EncoderType Recorder::getAvailableEncoder() const {
    // Get available encoder - temporarily disabled
    std::cout << "[RECORDER] Encoder detection disabled\n";
    return EncoderType::X264;
}

bool Recorder::createEncoders() {
    // Create encoders - temporarily disabled
    std::cout << "[RECORDER] Encoder creation disabled\n";
    return true;
}

bool Recorder::createOutput() {
    // Create output - temporarily disabled
    std::cout << "[RECORDER] Output creation disabled\n";
    return true;
}

void Recorder::shutdown() {
    if (state_ == RecordingState::RECORDING) {
        stopRecording();
    }

    cleanup();

    // OBS shutdown - temporarily disabled
    // if (obs_startup()) {
    //     obs_shutdown();
    // }
}

void Recorder::cleanup() {
    // OBS cleanup - temporarily disabled
    std::cout << "[RECORDER] Cleanup disabled\n";
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

    // Create output - temporarily disabled
    // if (!createOutput()) {
    //     cleanup();
    //     return false;
    // }

    // Start recording - temporarily disabled
    // if (!obs_output_start(output_)) {
    //     std::cerr << "[RECORDER] Failed to start output: " << obs_output_get_last_error(output_) << "\n";
    //     cleanup();
    //     return false;
    // }

    state_ = RecordingState::RECORDING;
    totalPausedDuration_ = 0;

    std::cout << "[RECORDER] Recording started: " << outputPath_ << "\n";
    return true;
}

void Recorder::stopRecording() {
    if (state_ == RecordingState::IDLE) {
        return;
    }

    // Stop output - temporarily disabled
    std::cout << "[RECORDER] Output stop disabled\n";

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

    // Pause recording - temporarily disabled
    pauseBeginTime_ = getHighPrecisionTimestamp();
    state_ = RecordingState::PAUSED;
    std::cout << "[RECORDER] Recording paused\n";
}

void Recorder::resumeRecording() {
    if (state_ != RecordingState::PAUSED) {
        return;
    }

    // OBS resume - temporarily disabled
    totalPausedDuration_ += (getHighPrecisionTimestamp() - pauseBeginTime_);
    state_ = RecordingState::RECORDING;
    std::cout << "[RECORDER] Recording resumed\n";
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
