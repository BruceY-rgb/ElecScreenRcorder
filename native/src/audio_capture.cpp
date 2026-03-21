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
        return true;
    }

    HRESULT hr = audioClient_->Start();
    if (FAILED(hr)) {
        std::cerr << "[AudioCapture] Failed to start: 0x" << std::hex << hr << std::endl;
        return false;
    }

    running_ = true;
    captureThread_ = std::thread(&AudioCapture::captureThread, this);

    std::cout << "[AudioCapture] Started capturing" << std::endl;
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
    std::cout << "[AudioCapture] Capture thread started" << std::endl;

    const uint32_t sampleRate = mixFormat_->nSamplesPerSec;
    const uint16_t channels = mixFormat_->nChannels;
    const uint16_t bytesPerSample = mixFormat_->wBitsPerSample / 8;
    const uint32_t frameSize = channels * bytesPerSample;

    while (running_) {
        // Wait for either data or stop event
        HANDLE handles[] = { stopEvent_ };
        DWORD waitResult = WaitForMultipleObjects(1, handles, FALSE, 100);

        if (waitResult == WAIT_OBJECT_0) {
            // Stop event signaled
            break;
        }

        if (waitResult != WAIT_TIMEOUT) {
            continue;
        }

        // Get the next packet size
        UINT32 packetLength = 0;
        HRESULT hr = captureClient_->GetNextPacketSize(&packetLength);
        if (FAILED(hr)) {
            std::cerr << "[AudioCapture] GetNextPacketSize failed: 0x" << std::hex << hr << std::endl;
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

                if (flags & AUDCLNT_BUFFERFLAGS_SILENT) {
                    // Silent data - write zeros to pipe and callback
                    std::vector<uint8_t> silence(dataBytes, 0);
                    if (pipeConnected_ && namedPipe_ != INVALID_HANDLE_VALUE) {
                        DWORD bytesWritten;
                        WriteFile(namedPipe_, silence.data(), (DWORD)silence.size(), &bytesWritten, nullptr);
                    }
                    if (callback_) {
                        callback_(silence.data(), silence.size(), qpcPosition);
                    }
                } else if (pData) {
                    // Write audio data to named pipe
                    if (pipeConnected_ && namedPipe_ != INVALID_HANDLE_VALUE) {
                        DWORD bytesWritten;
                        WriteFile(namedPipe_, pData, (DWORD)dataBytes, &bytesWritten, nullptr);
                    }
                    if (callback_) {
                        callback_(pData, dataBytes, qpcPosition);
                    }
                }
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

    namedPipe_ = CreateNamedPipeA(
        pipePath_.c_str(),
        PIPE_ACCESS_OUTBOUND,
        PIPE_TYPE_BYTE | PIPE_WAIT,
        1,              // max instances
        256 * 1024,     // out buffer 256KB
        0,              // in buffer (unused, outbound pipe)
        0,              // default timeout
        nullptr);

    if (namedPipe_ == INVALID_HANDLE_VALUE) {
        std::cerr << "[AudioCapture] Failed to create named pipe: " << pipePath_
                  << " error=" << GetLastError() << std::endl;
        return false;
    }

    pipeConnected_ = false;

    // Start async connection thread — blocks until FFmpeg connects
    pipeThread_ = std::thread(&AudioCapture::pipeConnectionThread, this);

    std::cerr << "[AudioCapture] Named pipe created: " << pipePath_ << std::endl;
    return true;
}

void AudioCapture::pipeConnectionThread() {
    BOOL result = ConnectNamedPipe(namedPipe_, nullptr);
    if (result || GetLastError() == ERROR_PIPE_CONNECTED) {
        pipeConnected_ = true;
        std::cerr << "[AudioCapture] FFmpeg connected to audio pipe" << std::endl;
    } else {
        DWORD err = GetLastError();
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

    pipePath_.clear();
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
