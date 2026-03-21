/**
 * Audio Device Enumeration
 *
 * Uses FFmpeg dshow to enumerate available audio input devices.
 */

#include <windows.h>
#include <iostream>
#include <string>
#include <vector>
#include <algorithm>

// Helper to convert wide string to narrow string
static std::string w2a(const std::wstring& wstr) {
    if (wstr.empty()) return "";
    int size_needed = WideCharToMultiByte(CP_UTF8, 0, &wstr[0], (int)wstr.size(), NULL, 0, NULL, NULL);
    if (size_needed <= 0) return "";
    std::string result(size_needed, 0);
    WideCharToMultiByte(CP_UTF8, 0, &wstr[0], (int)wstr.size(), &result[0], size_needed, NULL, NULL);
    return result;
}

/**
 * Enumerate audio input devices using FFmpeg dshow.
 * Runs: ffmpeg -list_devices true -f dshow -i dummy
 * Parses output for audio input device names.
 */
std::vector<std::string> getAudioInputDevices() {
    std::vector<std::string> devices;

    // First, find FFmpeg path from the module directory
    wchar_t exePath[MAX_PATH];
    GetModuleFileNameW(NULL, exePath, MAX_PATH);
    std::wstring modulePath = exePath;
    size_t lastSlash = modulePath.rfind(L'\\');
    if (lastSlash != std::wstring::npos) {
        modulePath = modulePath.substr(0, lastSlash);
    }

    std::wstring ffmpegW = modulePath;
    std::wstring ffmpegExe = ffmpegW + L"\\ffmpeg.exe";
    if (GetFileAttributesW(ffmpegExe.c_str()) == INVALID_FILE_ATTRIBUTES) {
        // Try parent directory
        size_t pos = ffmpegW.find(L"\\dist");
        if (pos != std::wstring::npos) {
            ffmpegW = ffmpegW.substr(0, pos);
        }
        ffmpegExe = ffmpegW + L"\\ffmpeg.exe";
    }

    if (GetFileAttributesW(ffmpegExe.c_str()) == INVALID_FILE_ATTRIBUTES) {
        // Try common default path
        ffmpegExe = L"D:\\ffmpeg\\ffmpeg-master-latest-win64-gpl\\bin\\ffmpeg.exe";
    }

    std::string ffmpegPath = w2a(ffmpegExe);

    // Build command: ffmpeg -list_devices true -f dshow -i dummy
    std::string command = "\"" + ffmpegPath + "\" -list_devices true -f dshow -i dummy";

    // Create pipes for capturing output
    HANDLE hStdoutRead = NULL, hStdoutWrite = NULL;
    HANDLE hStderrRead = NULL, hStderrWrite = NULL;
    SECURITY_ATTRIBUTES sa = { sizeof(SECURITY_ATTRIBUTES), NULL, TRUE };

    if (!CreatePipe(&hStdoutRead, &hStdoutWrite, &sa, 0) ||
        !CreatePipe(&hStderrRead, &hStderrWrite, &sa, 0)) {
        return devices;
    }
    SetHandleInformation(hStdoutRead, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(hStderrRead, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOA si = {};
    PROCESS_INFORMATION pi = {};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    si.hStdOutput = hStdoutWrite;
    si.hStdError = hStderrWrite;

    char* cmdLine = _strdup(command.c_str());
    if (!CreateProcessA(nullptr, cmdLine, nullptr, nullptr, TRUE,
                       CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        free(cmdLine);
        CloseHandle(hStdoutRead);
        CloseHandle(hStdoutWrite);
        CloseHandle(hStderrRead);
        CloseHandle(hStderrWrite);
        return devices;
    }
    free(cmdLine);

    // Close write ends in parent
    CloseHandle(hStdoutWrite);
    CloseHandle(hStderrWrite);

    // Read stderr output (FFmpeg logs to stderr)
    std::string output;
    char buffer[4096];
    DWORD bytesRead;

    while (true) {
        if (PeekNamedPipe(hStderrRead, NULL, 0, NULL, NULL, NULL)) {
            if (ReadFile(hStderrRead, buffer, sizeof(buffer) - 1, &bytesRead, NULL) && bytesRead > 0) {
                buffer[bytesRead] = '\0';
                output += buffer;
            } else {
                break;
            }
        } else {
            break;
        }

        // Check if process exited
        DWORD exitCode;
        if (GetExitCodeProcess(pi.hProcess, &exitCode) && exitCode != STILL_ACTIVE) {
            // Read any remaining output
            while (ReadFile(hStderrRead, buffer, sizeof(buffer) - 1, &bytesRead, NULL) && bytesRead > 0) {
                buffer[bytesRead] = '\0';
                output += buffer;
            }
            break;
        }
    }

    WaitForSingleObject(pi.hProcess, 2000);
    TerminateProcess(pi.hProcess, 0);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    CloseHandle(hStdoutRead);
    CloseHandle(hStderrRead);

    // Parse the output for audio devices
    // FFmpeg output format (newer versions, no section header):
    //   [in#0 @ hex] "device name" (audio)
    //   [in#0 @ hex]   Alternative name "@device_..."
    //   [in#0 @ hex] "device name 2" (audio)
    //
    // We match lines containing "(audio)" and extract the device name from quotes.
    // Skip "Alternative name" lines (they contain \t or multiple spaces before "Alternative").

    std::vector<std::string> foundDevices;

    // Split into lines
    std::string::size_type start = 0;
    while (start < output.size()) {
        std::string::size_type end = output.find('\n', start);
        if (end == std::string::npos) end = output.size();

        std::string line = output.substr(start, end - start);

        // Remove \r
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }

        // Only process lines that contain "(audio)" marker
        if (line.find("(audio)") != std::string::npos) {
            // Skip "Alternative name" lines
            if (line.find("Alternative name") != std::string::npos) {
                start = end + 1;
                continue;
            }

            // Look for device line: [in#0 @ hex] "device name" (audio)
            size_t quoteStart = line.find("\"");
            if (quoteStart != std::string::npos) {
                size_t quoteEnd = line.find("\"", quoteStart + 1);
                if (quoteEnd != std::string::npos) {
                    std::string deviceName = line.substr(quoteStart + 1, quoteEnd - quoteStart - 1);

                    if (!deviceName.empty()) {
                        if (std::find(foundDevices.begin(), foundDevices.end(), deviceName) == foundDevices.end()) {
                            foundDevices.push_back(deviceName);
                        }
                    }
                }
            }
        }

        start = end + 1;
    }

    return foundDevices;
}
