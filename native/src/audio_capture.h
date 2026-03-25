/**
 * WASAPI Audio Capture Module
 *
 * Uses Windows Audio Session API (WASAPI) to capture system audio (loopback)
 * or microphone input without requiring virtual audio devices.
 */

#pragma once

#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <endpointvolume.h>
#include <functional>
#include <cstdint>
#include <string>
#include <thread>
#include <atomic>

/**
 * Audio capture class for system audio (loopback) and microphone capture.
 * Supports writing captured PCM data to a Windows named pipe for FFmpeg integration.
 */
class AudioCapture {
public:
    explicit AudioCapture(bool isLoopback);
    ~AudioCapture();

    bool initialize();
    bool start();
    void stop();
    bool isCapturing() const { return running_; }

    using AudioCallback = std::function<void(const uint8_t* data, size_t bytes, uint64_t timestamp)>;
    void setCallback(AudioCallback callback) { callback_ = std::move(callback); }

    WAVEFORMATEX* getFormat() const { return mixFormat_; }
    uint32_t getSampleRate() const { return mixFormat_ ? mixFormat_->nSamplesPerSec : 0; }
    uint16_t getChannels() const { return mixFormat_ ? mixFormat_->nChannels : 0; }

    // Named pipe support for FFmpeg integration
    bool createNamedPipe(const std::string& pipeName);
    void closeNamedPipe();
    std::string getPipePath() const { return pipePath_; }
    bool waitForPipeReady(int timeoutMs = 5000);  // Wait for FFmpeg to connect

    // FFmpeg format helpers (based on detected WASAPI mix format)
    std::string getFFmpegFormatString() const;
    int getBitsPerSample() const { return mixFormat_ ? mixFormat_->wBitsPerSample : 0; }

private:
    void captureThread();
    void pipeConnectionThread();

    bool isLoopback_;
    bool running_ = false;
    bool initialized_ = false;

    // COM interfaces
    IMMDeviceEnumerator* enumerator_ = nullptr;
    IMMDevice* device_ = nullptr;
    IAudioClient* audioClient_ = nullptr;
    IAudioCaptureClient* captureClient_ = nullptr;

    // Audio format
    WAVEFORMATEX* mixFormat_ = nullptr;

    // Thread and synchronization
    std::thread captureThread_;
    HANDLE stopEvent_ = nullptr;

    // Callback
    AudioCallback callback_;

    // Named pipe for FFmpeg
    HANDLE namedPipe_ = INVALID_HANDLE_VALUE;
    std::string pipePath_;
    std::atomic<bool> pipeConnected_{false};
    std::thread pipeThread_;
    HANDLE pipeReadyEvent_ = nullptr;  // Event signaled when FFmpeg connects
};

// Helper function to initialize COM
bool initCOM();

// Check if WASAPI audio format is IEEE float
bool isFloatFormat(const WAVEFORMATEX* fmt);
