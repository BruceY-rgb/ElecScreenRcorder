/**
 * System Information Implementation
 *
 * Queries system hardware and display information using Windows APIs.
 */

#include "system_info.h"
#include "protocol.h"

#include <windows.h>
#include <dxgi1_2.h>
#include <setupapi.h>
#include <regstr.h>
#include <cfgmgr32.h>
#include <iostream>
#include <string>
#include <vector>
#include <algorithm>

#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "setupapi.lib")

// Helper to convert wide string to narrow string
static std::string w2a(const std::wstring& wstr) {
    if (wstr.empty()) return "";
    int size_needed = WideCharToMultiByte(CP_UTF8, 0, &wstr[0], (int)wstr.size(), NULL, 0, NULL, NULL);
    std::string result(size_needed, 0);
    WideCharToMultiByte(CP_UTF8, 0, &wstr[0], (int)wstr.size(), &result[0], size_needed, NULL, NULL);
    return result;
}

bool getDisplayInfo(int& width, int& height, int& refreshRate) {
    // Get primary display device
    DEVMODEW devMode = {};
    devMode.dmSize = sizeof(DEVMODEW);

    if (!EnumDisplaySettingsW(NULL, ENUM_CURRENT_SETTINGS, &devMode)) {
        // Fallback to GetSystemMetrics
        width = GetSystemMetrics(SM_CXSCREEN);
        height = GetSystemMetrics(SM_CYSCREEN);
        refreshRate = 60;
        return false;
    }

    width = devMode.dmPelsWidth;
    height = devMode.dmPelsHeight;
    refreshRate = devMode.dmDisplayFrequency;

    return true;
}

double getDpiScalingFactor() {
    // Method 1: Try to get DPI from monitor
    HMONITOR hMonitor = MonitorFromWindow(GetDesktopWindow(), MONITOR_DEFAULTTOPRIMARY);

    UINT dpiX = 0, dpiY = 0;
    if (GetDpiForMonitor(hMonitor, MDT_EFFECTIVE_DPI, &dpiX, &dpiY) == S_OK) {
        return static_cast<double>(dpiX) / 96.0;
    }

    // Method 2: Try GetDeviceCaps
    HDC hdc = GetDC(NULL);
    if (hdc) {
        int logPixelsX = GetDeviceCaps(hdc, LOGPIXELSX);
        ReleaseDC(NULL, hdc);
        if (logPixelsX > 0) {
            return static_cast<double>(logPixelsX) / 96.0;
        }
    }

    // Method 3: Try registry
    HKEY hKey;
    if (RegOpenKeyExW(HKEY_CURRENT_USER,
        L"Control Panel\\Desktop\\WindowMetrics",
        0, KEY_READ, &hKey) == ERROR_SUCCESS) {

        DWORD value = 96;
        DWORD type = REG_DWORD;
        DWORD size = sizeof(value);
        RegQueryValueExW(hKey, L"AppliedDPI", NULL, &type, (LPBYTE)&value, &size);
        RegCloseKey(hKey);

        return static_cast<double>(value) / 96.0;
    }

    return 1.0;
}

std::string getCpuName() {
    HKEY hKey;
    std::string cpuName;

    // Try to read from registry
    LONG result = RegOpenKeyExA(
        HKEY_LOCAL_MACHINE,
        "HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0",
        0, KEY_READ, &hKey);

    if (result == ERROR_SUCCESS) {
        char buffer[256] = {};
        DWORD bufferSize = sizeof(buffer);
        DWORD type = REG_SZ;

        result = RegQueryValueExA(hKey, "ProcessorNameString", NULL, &type,
            (LPBYTE)buffer, &bufferSize);

        if (result == ERROR_SUCCESS && bufferSize > 0) {
            cpuName = buffer;

            // Trim whitespace
            size_t start = cpuName.find_first_not_of(" \t\r\n");
            size_t end = cpuName.find_last_not_of(" \t\r\n");
            if (start != std::string::npos && end != std::string::npos) {
                cpuName = cpuName.substr(start, end - start + 1);
            } else if (start != std::string::npos) {
                cpuName = cpuName.substr(start);
            }
        }

        RegCloseKey(hKey);
    }

    if (cpuName.empty()) {
        // Try alternative registry location
        result = RegOpenKeyExA(
            HKEY_LOCAL_MACHINE,
            "HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0",
            0, KEY_READ, &hKey);

        if (result == ERROR_SUCCESS) {
            wchar_t wbuffer[256] = {};
            DWORD bufferSize = sizeof(wbuffer);
            DWORD type = REG_SZ;

            if (RegQueryValueExW(hKey, L"ProcessorNameString", NULL, &type,
                (LPBYTE)wbuffer, &bufferSize) == ERROR_SUCCESS) {
                cpuName = w2a(wbuffer);
            }

            RegCloseKey(hKey);
        }
    }

    return cpuName;
}

std::string getGpuName() {
    std::string gpuName;

    // Create DXGI factory
    IDXGIFactory2* pFactory = nullptr;
    HRESULT hr = CreateDXGIFactory1(__uuidof(IDXGIFactory2), (void**)&pFactory);

    if (SUCCEEDED(hr) && pFactory) {
        // Enumerate adapters
        IDXGIAdapter1* pAdapter = nullptr;
        for (UINT i = 0; pFactory->EnumAdapters1(i, &pAdapter) != DXGI_ERROR_NOT_FOUND; ++i) {
            DXGI_ADAPTER_DESC1 desc;
            if (SUCCEEDED(pAdapter->GetDesc1(&desc))) {
                // Skip software renderer (Microsoft Basic Render Driver)
                if (desc.VendorId == 0x1414 && desc.DeviceId == 0x8c) {
                    pAdapter->Release();
                    continue;
                }

                // Convert description to string
                gpuName = w2a(desc.Description);
                pAdapter->Release();
                break;
            }
            pAdapter->Release();
        }
        pFactory->Release();
    }

    if (gpuName.empty()) {
        // Try registry fallback
        HKEY hKey;
        if (RegOpenKeyExA(HKEY_LOCAL_MACHINE,
            "SYSTEM\\CurrentControlSet\\Control\\Class\\{4d36e968-e325-11ce-bfc1-08002be10318}\\0000",
            0, KEY_READ, &hKey) == ERROR_SUCCESS) {

            char buffer[256] = {};
            DWORD bufferSize = sizeof(buffer);
            RegQueryValueExA(hKey, "DriverDesc", NULL, NULL, (LPBYTE)buffer, &bufferSize);
            if (bufferSize > 0) {
                gpuName = buffer;
            }
            RegCloseKey(hKey);
        }
    }

    return gpuName;
}

int64_t getTotalMemory() {
    MEMORYSTATUSEX memStatus;
    memStatus.dwLength = sizeof(memStatus);

    if (GlobalMemoryStatusEx(&memStatus)) {
        return static_cast<int64_t>(memStatus.ullTotalPhys);
    }

    return 0;
}

int getMousePollingRate() {
    // Default polling rate
    int defaultRate = 125;

    // Try to get from registry (Windows doesn't directly expose this)
    // Most mice report 125, 250, 500, or 1000 Hz
    HKEY hKey;
    if (RegOpenKeyExA(HKEY_CURRENT_USER,
        "Control Panel\\Mouse",
        0, KEY_READ, &hKey) == ERROR_SUCCESS) {

        char buffer[32] = {};
        DWORD bufferSize = sizeof(buffer);
        RegQueryValueExA(hKey, "MousePollRate", NULL, NULL, (LPBYTE)buffer, &bufferSize);

        if (bufferSize > 0) {
            int rate = atoi(buffer);
            if (rate > 0) {
                defaultRate = rate;
            }
        }

        RegCloseKey(hKey);
    }

    // Common polling rates: 125, 250, 500, 1000 Hz
    // Return the most common default
    return defaultRate;
}

SystemInfo getSystemInfo() {
    SystemInfo info;

    // Get display info
    getDisplayInfo(info.screenWidth, info.screenHeight, info.refreshRate);

    // Get DPI scaling
    info.scalingFactor = getDpiScalingFactor();

    // Get CPU name
    info.cpuName = getCpuName();

    // Get GPU name
    info.gpuName = getGpuName();

    // Get memory
    info.ramBytes = getTotalMemory();
    info.ramGB = static_cast<int>(info.ramBytes / (1024 * 1024 * 1024));

    // Get input device info
    info.mousePollingRate = getMousePollingRate();

    return info;
}

std::string createSysInfoResponseEx(const SystemInfo& info) {
    // Use the existing protocol function with all parameters
    return createSysInfoResponse(
        info.screenWidth,
        info.screenHeight,
        info.refreshRate,
        info.scalingFactor,
        info.cpuName,
        info.gpuName,
        info.ramGB,
        info.mousePollingRate
    );
}
