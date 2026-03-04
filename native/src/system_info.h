#pragma once

#include <string>
#include <cstdint>

/**
 * System Information Query API
 *
 * Queries system hardware and display information including:
 * - Display resolution, refresh rate, DPI scaling
 * - CPU name
 * - GPU name
 * - RAM size
 * - Input device information
 */

/**
 * System information structure
 */
struct SystemInfo {
    // Display info
    int screenWidth = 1920;
    int screenHeight = 1080;
    int refreshRate = 60;
    double scalingFactor = 1.0;

    // CPU info
    std::string cpuName;

    // GPU info
    std::string gpuName;

    // Memory info
    int64_t ramBytes = 0;
    int ramGB = 0;

    // Input device info
    std::string keyboardName;
    std::string mouseName;
    int mousePollingRate = 125;  // Hz, default 125
};

/**
 * Get complete system information
 * @return SystemInfo struct with all queried data
 */
SystemInfo getSystemInfo();

/**
 * Get display resolution using EnumDisplaySettings
 * @param width Output: screen width in pixels
 * @param height Output: screen height in pixels
 * @param refreshRate Output: refresh rate in Hz
 * @return true if successful
 */
bool getDisplayInfo(int& width, int& height, int& refreshRate);

/**
 * Get DPI scaling factor using GetDeviceCaps
 * @return DPI scaling factor (e.g., 1.0, 1.25, 1.5, 2.0)
 */
double getDpiScalingFactor();

/**
 * Get CPU name from registry
 * @return CPU name string
 */
std::string getCpuName();

/**
 * Get GPU name using DXGI
 * @return GPU name string
 */
std::string getGpuName();

/**
 * Get total physical memory
 * @return Total memory in bytes
 */
int64_t getTotalMemory();

/**
 * Get mouse polling rate
 * @return Polling rate in Hz
 */
int getMousePollingRate();

/**
 * Create sysinfo JSON response string
 * @param info SystemInfo struct
 * @return JSON string
 */
std::string createSysInfoResponseEx(const SystemInfo& info);
