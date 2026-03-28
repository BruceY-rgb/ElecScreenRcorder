/**
 * WASAPI Audio Capture Implementation
 *
 * Implements system audio capture using WASAPI Loopback
 * and microphone capture using standard capture endpoint.
 */

#include "audio_capture.h"

#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <combaseapi.h>
#include <Functiondiscoverykeys_devpkey.h>
#include <ks.h>
#include <ksmedia.h>
#include <cstring>
#include <iostream>
#include <vector>

// Use __uuidof() to get IIDs at compile time from the actual interface definitions
// This is more reliable than manually defined GUIDs
// The interfaces are defined in audioclient.h and mmdeviceapi.h

// Reference time constants
#define REFTIMES_PER_SEC  10000000LL
#define REFTIMES_PER_MILLISEC 10000LL

// Helper macros
#define SAFE_RELEASE(p) if ((p) != nullptr) { (p)->Release(); (p) = nullptr; }

bool initCOM() {
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(hr) && hr != RPC_E_CHANGED_MODE && hr != CO_E_NOTINITIALIZED) {
        std::cerr << "[AudioCapture] COM initialization failed: 0x" << std::hex << hr << std::endl;
        return false;
    }
    return true;
}

bool isFloatFormat(const WAVEFORMATEX* fmt) {
    if (!fmt) return false;
    if (fmt->wFormatTag == WAVE_FORMAT_IEEE_FLOAT) return true;
    if (fmt->wFormatTag == WAVE_FORMAT_EXTENSIBLE) {
        auto* ext = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(fmt);
        return IsEqualGUID(ext->SubFormat, KSDATAFORMAT_SUBTYPE_IEEE_FLOAT);
    }
    return false;
}

AudioCapture::AudioCapture(bool isLoopback)
    : isLoopback_(isLoopback) {
}

AudioCapture::~AudioCapture() {
    stop();
    closeNamedPipe();

    if (mixFormat_) {
        CoTaskMemFree(mixFormat_);
        mixFormat_ = nullptr;
    }

    SAFE_RELEASE(captureClient_);
    SAFE_RELEASE(audioClient_);
    SAFE_RELEASE(device_);
    SAFE_RELEASE(enumerator_);

    if (stopEvent_) {
        CloseHandle(stopEvent_);
        stopEvent_ = nullptr;
    }
}

bool AudioCapture::initialize() {
    if (initialized_) {
        return true;
    }

    HRESULT hr;

    // Ensure COM is initialized on the current thread
    hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(hr) && hr != RPC_E_CHANGED_MODE && hr != S_FALSE) {
        // STA fallback for threads already in apartment-threaded mode
        hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        if (FAILED(hr) && hr != RPC_E_CHANGED_MODE && hr != S_FALSE) {
            std::cerr << "[AudioCapture] COM init failed on this thread: 0x"
                      << std::hex << hr << std::dec << std::endl;
            return false;
        }
    }

    // Create device enumerator (CLSCTX_ALL for maximum compatibility)
    std::cerr << "[AudioCapture] Creating MMDeviceEnumerator..." << std::endl;
    hr = CoCreateInstance(
        __uuidof(MMDeviceEnumerator),
        nullptr,
        CLSCTX_ALL,
        __uuidof(IMMDeviceEnumerator),
        (void**)&enumerator_);

    if (FAILED(hr)) {
        std::cerr << "[AudioCapture] Failed to create device enumerator: 0x"
                  << std::hex << hr << std::dec << std::endl;
        return false;
    }

    std::cerr << "[AudioCapture] Device enumerator created successfully" << std::endl;

    // Get default audio endpoint
    // eRender for loopback (system audio), eCapture for microphone
    ERole role = eConsole;
    EDataFlow deviceType = isLoopback_ ? eRender : eCapture;
    std::cerr << "[AudioCapture] Getting default audio endpoint, type=" << (isLoopback_ ? "eRender" : "eCapture") << std::endl;

    hr = enumerator_->GetDefaultAudioEndpoint(deviceType, role, &device_);
    if (FAILED(hr)) {
        std::cerr << "[AudioCapture] Failed to get default audio endpoint: 0x" << std::hex << hr << std::endl;
        return false;
    }

    std::cerr << "[AudioCapture] Got default audio endpoint" << std::endl;

    // Activate the audio client
    std::cerr << "[AudioCapture] Activating audio client..." << std::endl;
    hr = device_->Activate(
        __uuidof(IAudioClient),
        CLSCTX_ALL,
        nullptr,
        (void**)&audioClient_);

    if (FAILED(hr)) {
        std::cerr << "[AudioCapture] Failed to activate audio client: 0x" << std::hex << hr << std::endl;
        // Try with CLSCTX_ALL as fallback
        hr = device_->Activate(
            __uuidof(IAudioClient),
            CLSCTX_ALL,
            nullptr,
            (void**)&audioClient_);
        if (FAILED(hr)) {
            std::cerr << "[AudioCapture] Retry also failed: 0x" << std::hex << hr << std::endl;
            return false;
        }
    }

    // Get the mix format
    hr = audioClient_->GetMixFormat(&mixFormat_);
    if (FAILED(hr)) {
        std::cerr << "[AudioCapture] Failed to get mix format: 0x" << std::hex << hr << std::endl;
        return false;
    }

    std::cout << "[AudioCapture] Format: "
              << mixFormat_->nChannels << "ch, "
              << mixFormat_->nSamplesPerSec << "Hz, "
              << mixFormat_->wBitsPerSample << "bit"
              << std::endl;

    // Calculate buffer duration (1 second for simplicity)
    REFERENCE_TIME hnsRequestedDuration = REFTIMES_PER_SEC;

    // Initialize the audio client
    // For loopback: AUDCLNT_STREAMFLAGS_LOOPBACK
    // For capture: 0
    DWORD flags = isLoopback_ ? AUDCLNT_STREAMFLAGS_LOOPBACK : 0;

    hr = audioClient_->Initialize(
        AUDCLNT_SHAREMODE_SHARED,
        flags,
        hnsRequestedDuration,
        0,
        mixFormat_,
        nullptr);

    if (FAILED(hr)) {
        std::cerr << "[AudioCapture] Failed to initialize audio client: 0x" << std::hex << hr << std::endl;
        return false;
    }

    // Get the capture client service
    hr = audioClient_->GetService(
        __uuidof(IAudioCaptureClient),
        (void**)&captureClient_);

    if (FAILED(hr)) {
        std::cerr << "[AudioCapture] Failed to get capture client: 0x" << std::hex << hr << std::endl;
        return false;
    }

    // Create stop event
    stopEvent_ = CreateEvent(nullptr, FALSE, FALSE, nullptr);
    if (!stopEvent_) {
        std::cerr << "[AudioCapture] Failed to create stop event" << std::endl;
        return false;
    }

    initialized_ = true;
    std::cout << "[AudioCapture] Initialized successfully ("
              << (isLoopback_ ? "loopback/system audio" : "microphone")
              << ")" << std::endl;

    return true;
}

bool AudioCapture::start() {
    if (!initialized_) {
        std::cerr << "[AudioCapture] Not initialized" << std::endl;
        return false;
    }

    if (running_) {
        std::cerr << "[AudioCapture] Already running" << std::endl;
        return true;
    }

    std::cerr << "[AudioCapture] DEBUG: Starting audio client..." << std::endl;

    HRESULT hr = audioClient_->Start();
    if (FAILED(hr)) {
        std::cerr << "[AudioCapture] Failed to start: 0x" << std::hex << hr << std::endl;
        return false;
    }

    std::cerr << "[AudioCapture] DEBUG: Audio client started, creating capture thread..." << std::endl;

    running_ = true;
    captureThread_ = std::thread(&AudioCapture::captureThread, this);

    std::cout << "[AudioCapture] Started capturing" << std::endl;
    std::cerr << "[AudioCapture] DEBUG: Capture thread created" << std::endl;
    return true;
}

void AudioCapture::stop() {
    if (!running_) {
        return;
    }

    running_ = false;

    if (stopEvent_) {
        SetEvent(stopEvent_);
    }

    if (captureThread_.joinable()) {
        captureThread_.join();
    }

    if (audioClient_) {
        audioClient_->Stop();
    }

    std::cout << "[AudioCapture] Stopped capturing" << std::endl;
}

void AudioCapture::captureThread() {
    std::cout << "[AudioCapture] DEBUG: Capture thread started" << std::endl;
    std::cerr << "[AudioCapture] DEBUG: Capture thread started" << std::endl;

    if (!captureClient_) {
        std::cerr << "[AudioCapture] ERROR: captureClient_ is NULL!" << std::endl;
        return;
    }

    const uint32_t sampleRate = mixFormat_->nSamplesPerSec;
    const uint16_t channels = mixFormat_->nChannels;
    const uint16_t bytesPerSample = mixFormat_->wBitsPerSample / 8;
    const uint32_t frameSize = channels * bytesPerSample;

    std::cerr << "[AudioCapture] DEBUG: format: sampleRate=" << sampleRate
              << ", channels=" << channels
              << ", bytesPerSample=" << bytesPerSample
              << ", frameSize=" << frameSize << std::endl;

    while (running_) {
        // Wait for either data or stop event
        HANDLE handles[] = { stopEvent_ };
        DWORD waitResult = WaitForMultipleObjects(1, handles, FALSE, 100);

        if (waitResult == WAIT_OBJECT_0) {
            // Stop event signaled
            std::cerr << "[AudioCapture] DEBUG: Stop event received, exiting capture thread" << std::endl;
            break;
        }

        if (waitResult != WAIT_TIMEOUT) {
            std::cerr << "[AudioCapture] DEBUG: Wait returned unexpected result: " << waitResult << std::endl;
            continue;
        }

        // Get the next packet size
        UINT32 packetLength = 0;
        HRESULT hr = captureClient_->GetNextPacketSize(&packetLength);
        if (FAILED(hr)) {
            std::cerr << "[AudioCapture] GetNextPacketSize failed: 0x" << std::hex << hr << std::endl;
            continue;
        }

        // DEBUG: Log packet status periodically
        static int debugCounter = 0;
        debugCounter++;
        if (debugCounter % 50 == 0) { // Every 50 iterations (~5 seconds)
            bool pipeReady = pipeReadyEvent_ && WaitForSingleObject(pipeReadyEvent_, 0) == WAIT_OBJECT_0;
            bool pipeValid = namedPipe_ != INVALID_HANDLE_VALUE;
            std::cerr << "[AudioCapture] DEBUG: loop check: packet=" << packetLength
                      << ", pipeReady=" << pipeReady
                      << ", pipeValid=" << pipeValid
                      << ", running=" << running_ << std::endl;
        }

        // If no packet available, write silence to keep the pipe flowing
        if (packetLength == 0) {
            // Check pipe status
            bool pipeReady = pipeReadyEvent_ && WaitForSingleObject(pipeReadyEvent_, 0) == WAIT_OBJECT_0;
            bool pipeValid = namedPipe_ != INVALID_HANDLE_VALUE;

            // ALWAYS write silence to pipe, even if FFmpeg not connected yet
            // This buffers data for FFmpeg to read when it connects
            size_t silenceSize = frameSize * 100; // 100 frames worth of silence
            std::vector<uint8_t> silence(silenceSize, 0);
            if (namedPipe_ != INVALID_HANDLE_VALUE) {
                DWORD bytesWritten;
                if (!WriteFile(namedPipe_, silence.data(), (DWORD)silence.size(), &bytesWritten, nullptr)) {
                    DWORD err = GetLastError();
                    // Ignore ERROR_NO_DATA - means pipe buffer is full or no reader
                    if (err != ERROR_NO_DATA && err != ERROR_BROKEN_PIPE) {
                        std::cerr << "[AudioCapture] DEBUG: Failed to write silence, error: " << err << std::endl;
                    } else if (err == ERROR_NO_DATA) {
                        // Pipe buffer full - this is normal when FFmpeg hasn't connected yet
                    }
                } else {
                    // DEBUG: Log silence written
                    static int silenceCounter = 0;
                    silenceCounter++;
                    if (silenceCounter % 100 == 0) { // Don't spam too much
                        std::cerr << "[AudioCapture] DEBUG: Wrote " << bytesWritten << " bytes silence, pipeReady=" << pipeReady << std::endl;
                    }
                }
            }
            continue;
        }

        while (packetLength != 0 && running_) {
            // Get the buffer
            BYTE* pData = nullptr;
            UINT32 numFramesAvailable = 0;
            DWORD flags = 0;
            UINT64 devicePosition = 0;
            UINT64 qpcPosition = 0;

            hr = captureClient_->GetBuffer(
                &pData,
                &numFramesAvailable,
                &flags,
                &devicePosition,
                &qpcPosition);

            if (FAILED(hr)) {
                std::cerr << "[AudioCapture] GetBuffer failed: 0x" << std::hex << hr << std::endl;
                break;
            }

            if (numFramesAvailable > 0) {
                size_t dataBytes = numFramesAvailable * frameSize;

                // DEBUG: Log every time we get audio data (not just periodically)
                bool isSilent = (flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0;
                bool pipeReady = pipeReadyEvent_ && WaitForSingleObject(pipeReadyEvent_, 0) == WAIT_OBJECT_0;
                std::cerr << "[AudioCapture] DEBUG: Got data: frames=" << numFramesAvailable
                          << ", bytes=" << dataBytes
                          << ", silent=" << isSilent
                          << ", pipeReady=" << pipeReady << std::endl;

                if (flags & AUDCLNT_BUFFERFLAGS_SILENT) {
                    std::cerr << "[AudioCapture] DEBUG: Silent flag set - no actual audio data" << std::endl;
                    // Still write silence to pipe
                    if (pipeReady && namedPipe_ != INVALID_HANDLE_VALUE) {
                        std::vector<uint8_t> silence(dataBytes, 0);
                        DWORD bytesWritten;
                        WriteFile(namedPipe_, silence.data(), (DWORD)silence.size(), &bytesWritten, nullptr);
                    }
                } else if (pData) {
                    // Only write to pipe if FFmpeg has connected
                    if (pipeReadyEvent_ && WaitForSingleObject(pipeReadyEvent_, 0) == WAIT_OBJECT_0) {
                        // FFmpeg is connected, write audio
                        // Convert float32 to int16
                        const float* floatData = reinterpret_cast<const float*>(pData);
                        std::vector<uint8_t> int16Data(numFramesAvailable * frameSize / 2);
                        int16_t* outPtr = reinterpret_cast<int16_t*>(int16Data.data());

                        for (uint32_t i = 0; i < numFramesAvailable * channels; i++) {
                            float sample = floatData[i];
                            if (sample > 1.0f) sample = 1.0f;
                            if (sample < -1.0f) sample = -1.0f;
                            outPtr[i] = static_cast<int16_t>(sample * 32767.0f);
                        }

                        if (namedPipe_ != INVALID_HANDLE_VALUE) {
                            DWORD bytesWritten;
                            // Non-blocking write with timeout
                            if (WriteFile(namedPipe_, int16Data.data(), (DWORD)int16Data.size(), &bytesWritten, nullptr)) {
                                std::cerr << "[AudioCapture] DEBUG: Wrote " << bytesWritten << " bytes to pipe" << std::endl;
                            } else {
                                DWORD err = GetLastError();
                                if (err == ERROR_NO_DATA) {
                                    // Pipe buffer full - skip this frame, FFmpeg will read what it can
                                    std::cerr << "[AudioCapture] DEBUG: Pipe buffer full, skipping" << std::endl;
                                } else if (err != ERROR_BROKEN_PIPE) {
                                    std::cerr << "[AudioCapture] ERROR: Write failed: " << err << std::endl;
                                }
                            }
                        }
                    } else {
                        // FFmpeg not connected yet - skip write
                        // std::cerr << "[AudioCapture] DEBUG: Skipping - FFmpeg not connected" << std::endl;
                    }
                }
            } else {
                // std::cerr << "[AudioCapture] DEBUG: numFramesAvailable=0" << std::endl;
            }

            // Release the buffer
            hr = captureClient_->ReleaseBuffer(numFramesAvailable);
            if (FAILED(hr)) {
                std::cerr << "[AudioCapture] ReleaseBuffer failed: 0x" << std::hex << hr << std::endl;
                break;
            }

            // Get next packet size
            hr = captureClient_->GetNextPacketSize(&packetLength);
            if (FAILED(hr)) {
                break;
            }
        }
    }

    std::cout << "[AudioCapture] Capture thread ended" << std::endl;
}

// ===== Named Pipe Support =====

bool AudioCapture::createNamedPipe(const std::string& pipeName) {
    pipePath_ = "\\\\.\\pipe\\" + pipeName;

    // Create event to signal when FFmpeg connects
    pipeReadyEvent_ = CreateEvent(nullptr, FALSE, FALSE, nullptr);
    if (!pipeReadyEvent_) {
        std::cerr << "[AudioCapture] Failed to create pipe ready event: " << GetLastError() << std::endl;
        return false;
    }

    // Use PIPE_ACCESS_DUPLEX for bidirectional access
    // This allows both the server (C++) to write and FFmpeg (client) to read
    namedPipe_ = CreateNamedPipeA(
        pipePath_.c_str(),
        PIPE_ACCESS_DUPLEX,
        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
        1,              // max instances
        256 * 1024,     // out buffer 256KB
        256 * 1024,     // in buffer 256KB (needed for duplex)
        0,              // default timeout
        nullptr);

    if (namedPipe_ == INVALID_HANDLE_VALUE) {
        std::cerr << "[AudioCapture] Failed to create named pipe: " << pipePath_
                  << " error=" << GetLastError() << std::endl;
        CloseHandle(pipeReadyEvent_);
        pipeReadyEvent_ = nullptr;
        return false;
    }

    pipeConnected_ = false;

    // Start async connection thread — blocks until FFmpeg connects
    pipeThread_ = std::thread(&AudioCapture::pipeConnectionThread, this);

    std::cerr << "[AudioCapture] Named pipe created (DUPLEX mode): " << pipePath_ << std::endl;
    return true;
}

void AudioCapture::pipeConnectionThread() {
    std::cerr << "[AudioCapture] DEBUG: pipeConnectionThread started, waiting for FFmpeg..." << std::endl;
    BOOL result = ConnectNamedPipe(namedPipe_, nullptr);
    if (result || GetLastError() == ERROR_PIPE_CONNECTED) {
        pipeConnected_ = true;
        std::cerr << "[AudioCapture] DEBUG: FFmpeg connected to audio pipe" << std::endl;
        // Signal the event so waiting code knows FFmpeg is connected
        if (pipeReadyEvent_) {
            SetEvent(pipeReadyEvent_);
        }
    } else {
        DWORD err = GetLastError();
        std::cerr << "[AudioCapture] DEBUG: ConnectNamedPipe failed with error: " << err << std::endl;
        if (err != ERROR_NO_DATA && err != ERROR_BROKEN_PIPE) {
            std::cerr << "[AudioCapture] ConnectNamedPipe failed: " << err << std::endl;
        }
    }
}

void AudioCapture::closeNamedPipe() {
    pipeConnected_ = false;

    if (namedPipe_ != INVALID_HANDLE_VALUE) {
        FlushFileBuffers(namedPipe_);
        DisconnectNamedPipe(namedPipe_);
        CloseHandle(namedPipe_);
        namedPipe_ = INVALID_HANDLE_VALUE;
    }

    if (pipeThread_.joinable()) {
        pipeThread_.join();
    }

    if (pipeReadyEvent_) {
        CloseHandle(pipeReadyEvent_);
        pipeReadyEvent_ = nullptr;
    }

    pipePath_.clear();
}

bool AudioCapture::waitForPipeReady(int timeoutMs) {
    if (!pipeReadyEvent_) {
        std::cerr << "[AudioCapture] waitForPipeReady: no event handle" << std::endl;
        return false;
    }

    DWORD waitResult = WaitForSingleObject(pipeReadyEvent_, timeoutMs);
    if (waitResult == WAIT_OBJECT_0) {
        std::cerr << "[AudioCapture] waitForPipeReady: FFmpeg connected (wait succeeded)" << std::endl;
        return true;
    } else if (waitResult == WAIT_TIMEOUT) {
        std::cerr << "[AudioCapture] waitForPipeReady: timeout waiting for FFmpeg" << std::endl;
        return false;
    } else {
        std::cerr << "[AudioCapture] waitForPipeReady: wait failed: " << GetLastError() << std::endl;
        return false;
    }
}

std::string AudioCapture::getFFmpegFormatString() const {
    if (!mixFormat_) return "f32le";

    if (isFloatFormat(mixFormat_)) {
        return (mixFormat_->wBitsPerSample == 64) ? "f64le" : "f32le";
    }

    switch (mixFormat_->wBitsPerSample) {
        case 16: return "s16le";
        case 24: return "s24le";
        case 32: return "s32le";
        default: return "s16le";
    }
}
