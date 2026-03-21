// Test: Check default audio output device
#include <windows.h>
#include <mmdeviceapi.h>
#include <endpointvolume.h>
#include <iostream>
#include <string>

// Helper
static std::string w2a(const std::wstring& wstr) {
    if (wstr.empty()) return "";
    int size_needed = WideCharToMultiByte(CP_UTF8, 0, &wstr[0], (int)wstr.size(), NULL, 0, NULL, NULL);
    if (size_needed <= 0) return "";
    std::string result(size_needed, 0);
    WideCharToMultiByte(CP_UTF8, 0, &wstr[0], (int)wstr.size(), &result[0], size_needed, NULL, NULL);
    return result;
}

int main() {
    HRESULT hr;
    IMMDeviceEnumerator* pEnumerator = nullptr;
    IMMDevice* pDevice = nullptr;

    hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(hr)) {
        std::cerr << "COM init failed: 0x" << std::hex << hr << std::endl;
        return 1;
    }

    hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_INPROC_SERVER,
        __uuidof(IMMDeviceEnumerator), (void**)&pEnumerator);
    if (FAILED(hr)) {
        std::cerr << "Failed to create enumerator" << std::endl;
        return 1;
    }

    // Get default output device (eRender=0, eConsole=1)
    hr = pEnumerator->GetDefaultAudioEndpoint(eRender, eConsole, &pDevice);
    if (FAILED(hr)) {
        std::cerr << "Failed to get default endpoint: 0x" << std::hex << hr << std::endl;
        pEnumerator->Release();
        return 1;
    }

    // Get device ID
    LPWSTR pwszID = nullptr;
    hr = pDevice->GetId(&pwszID);
    if (SUCCEEDED(hr) && pwszID) {
        std::cout << "Default Output Device ID: " << w2a(pwszID) << std::endl;
        CoTaskMemFree(pwszID);
    }

    // Get device friendly name
    IPropertyStore* pPropStore = nullptr;
    hr = pDevice->OpenPropertyStore(STGM_READ, &pPropStore);
    if (SUCCEEDED(hr) && pPropStore) {
        PROPVARIANT varName;
        PropVariantInit(&varName);
        hr = pPropStore->GetValue(PKEY_Device_FriendlyName, &varName);
        if (SUCCEEDED(hr) && varName.vt == VT_LPWSTR) {
            std::wstring deviceNameW(varName.pwszVal);
            std::cout << "Default Output Device Name: " << w2a(deviceNameW) << std::endl;

            // Check if VB-Audio
            std::string name = w2a(deviceNameW);
            if (name.find("CABLE") != std::string::npos) {
                std::cout << "*** VB-Audio IS the default output device! ***" << std::endl;
            } else {
                std::cout << "*** VB-Audio is NOT the default output device ***" << std::endl;
                std::cout << "Please set CABLE Output as default in Windows Sound settings" << std::endl;
            }

            PropVariantClear(&varName);
        }
        pPropStore->Release();
    }

    pDevice->Release();
    pEnumerator->Release();

    return 0;
}
