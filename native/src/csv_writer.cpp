#include "csv_writer.h"
#include "input_capture.h"
#include "../third_party/readerwriterqueue/readerwriterqueue.h"

#include <windows.h>
#include <cstdio>
#include <cstring>
#include <thread>
#include <atomic>
#include <fstream>
#include <mutex>

// Forward declarations
extern moodycamel::ReaderWriterQueue<KeyboardEvent>* g_keyboardQueue;
extern moodycamel::ReaderWriterQueue<MouseClickEvent>* g_mouseClickQueue;
extern moodycamel::ReaderWriterQueue<MouseMoveEvent>* g_mouseMoveQueue;

// File handles
static std::ofstream g_actionsFile;
static std::ofstream g_movementsFile;
static HANDLE g_writerThread = nullptr;
static std::atomic<bool> g_running{false};
static std::atomic<bool> g_writing{false};

// Recording state
static std::atomic<int64_t> g_recordingStartTime{0};
static std::atomic<int64_t> g_totalPausedDuration{0};
static std::atomic<int64_t> g_pauseBeginTime{0};
static std::atomic<bool> g_isPaused{false};

// Buffer
static std::mutex g_bufferMutex;
static std::string g_actionsBuffer;
static std::string g_movementsBuffer;

// Flush interval (100ms)
static const int FLUSH_INTERVAL_MS = 100;

// Escape CSV field
static std::string escapeCsvField(const std::string& field) {
    if (field.find(',') != std::string::npos ||
        field.find('"') != std::string::npos ||
        field.find('\n') != std::string::npos) {
        std::string escaped = "\"";
        for (char c : field) {
            if (c == '"') escaped += "\"\"";
            else escaped += c;
        }
        escaped += "\"";
        return escaped;
    }
    return field;
}

// Calculate relative time
static int64_t calculateRelativeTime(int64_t rawTime) {
    int64_t startTime = g_recordingStartTime.load();
    int64_t pausedDuration = g_totalPausedDuration.load();

    if (startTime == 0) return 0;
    return rawTime - startTime - pausedDuration;
}

// Flush buffers to files
static void flushBuffers() {
    std::lock_guard<std::mutex> lock(g_bufferMutex);

    if (g_actionsFile.is_open() && !g_actionsBuffer.empty()) {
        g_actionsFile << g_actionsBuffer << std::flush;
        g_actionsBuffer.clear();
    }

    if (g_movementsFile.is_open() && !g_movementsBuffer.empty()) {
        g_movementsFile << g_movementsBuffer << std::flush;
        g_movementsBuffer.clear();
    }
}

// Writer thread procedure
static DWORD WINAPI WriterThreadProc(LPVOID lpParameter) {
    (void)lpParameter;

    while (g_running.load()) {
        // Process keyboard events
        if (g_keyboardQueue) {
            KeyboardEvent evt;
            while (g_keyboardQueue->try_dequeue(evt)) {
                int64_t relTime = calculateRelativeTime(evt.rawTime);

                std::lock_guard<std::mutex> lock(g_bufferMutex);
                g_actionsBuffer += "keyboard,";
                g_actionsBuffer += std::to_string(relTime) + ",";
                g_actionsBuffer += std::to_string(evt.rawTime) + ",";
                g_actionsBuffer += ",";  // x (not used for keyboard)
                g_actionsBuffer += ",";  // y (not used for keyboard)
                g_actionsBuffer += ",";  // button (not used for keyboard)
                g_actionsBuffer += std::to_string(evt.keycode) + ",";
                g_actionsBuffer += escapeCsvField(std::string(1, evt.keyChar)) + ",";
                g_actionsBuffer += std::string(evt.altKey ? "1" : "0") + ",";
                g_actionsBuffer += std::string(evt.ctrlKey ? "1" : "0") + ",";
                g_actionsBuffer += std::string(evt.shiftKey ? "1" : "0") + ",";
                g_actionsBuffer += std::string(evt.metaKey ? "1" : "0") + "\n";
            }
        }

        // Process mouse click events
        if (g_mouseClickQueue) {
            MouseClickEvent evt;
            while (g_mouseClickQueue->try_dequeue(evt)) {
                int64_t relTime = calculateRelativeTime(evt.rawTime);

                std::lock_guard<std::mutex> lock(g_bufferMutex);
                std::string type = (evt.button == 1) ? "mouse_left" :
                                   (evt.button == 2) ? "mouse_right" : "mouse_middle";
                type += evt.isDown ? "_down" : "_up";

                g_actionsBuffer += type + ",";
                g_actionsBuffer += std::to_string(relTime) + ",";
                g_actionsBuffer += std::to_string(evt.rawTime) + ",";
                g_actionsBuffer += std::to_string(evt.x) + ",";
                g_actionsBuffer += std::to_string(evt.y) + ",";
                g_actionsBuffer += std::to_string(evt.button) + ",";
                g_actionsBuffer += ",";  // keycode (not used for mouse)
                g_actionsBuffer += ",";  // keyChar (not used for mouse)
                g_actionsBuffer += std::string(evt.altKey ? "1" : "0") + ",";
                g_actionsBuffer += std::string(evt.ctrlKey ? "1" : "0") + ",";
                g_actionsBuffer += std::string(evt.shiftKey ? "1" : "0") + ",";
                g_actionsBuffer += std::string(evt.metaKey ? "1" : "0") + "\n";
            }
        }

        // Process mouse move events
        if (g_mouseMoveQueue) {
            MouseMoveEvent evt;
            while (g_mouseMoveQueue->try_dequeue(evt)) {
                int64_t relTime = calculateRelativeTime(evt.rawTime);

                std::lock_guard<std::mutex> lock(g_bufferMutex);
                g_movementsBuffer += std::to_string(relTime) + ",";
                g_movementsBuffer += std::to_string(evt.rawTime) + ",";
                g_movementsBuffer += std::to_string(evt.x) + ",";
                g_movementsBuffer += std::to_string(evt.y) + ",";
                g_movementsBuffer += std::to_string(evt.dx) + ",";
                g_movementsBuffer += std::to_string(evt.dy) + "\n";
            }
        }

        // Flush buffers periodically
        flushBuffers();

        // Sleep briefly to avoid busy-waiting
        Sleep(10);
    }

    // Final flush before exit
    flushBuffers();

    return 0;
}

// Initialize CSV writer system
void initCsvWriter() {
    // Nothing to initialize
}

// Shutdown CSV writer system
void shutdownCsvWriter() {
    stopCsvWriter();
}

// Start CSV writer
bool startCsvWriter(const std::string& actionsPath,
                    const std::string& movementsPath,
                    int64_t recordingStartTime) {
    if (g_writing.load()) return true;

    // Open files
    g_actionsFile.open(actionsPath, std::ios::out | std::ios::binary);
    if (!g_actionsFile.is_open()) {
        return false;
    }

    g_movementsFile.open(movementsPath, std::ios::out | std::ios::binary);
    if (!g_movementsFile.is_open()) {
        g_actionsFile.close();
        return false;
    }

    // Write CSV headers
    g_actionsFile << "type,time,rawTime,x,y,button,keycode,keyChar,altKey,ctrlKey,shiftKey,metaKey\n";
    g_movementsFile << "time,rawTime,x,y,dx,dy\n";

    // Set recording start time
    g_recordingStartTime.store(recordingStartTime);
    g_totalPausedDuration.store(0);
    g_isPaused.store(false);

    // Start writer thread
    g_running.store(true);
    g_writing.store(true);

    g_writerThread = CreateThread(NULL, 0, WriterThreadProc, NULL, 0, NULL);
    if (!g_writerThread) {
        g_running.store(false);
        g_writing.store(false);
        g_actionsFile.close();
        g_movementsFile.close();
        return false;
    }

    return true;
}

// Stop CSV writer
void stopCsvWriter() {
    if (!g_writing.load()) return;

    // Stop running flag
    g_running.store(false);
    g_writing.store(false);

    // Wait for writer thread to finish
    if (g_writerThread) {
        WaitForSingleObject(g_writerThread, INFINITE);
        CloseHandle(g_writerThread);
        g_writerThread = nullptr;
    }

    // Close files
    if (g_actionsFile.is_open()) {
        g_actionsFile.close();
    }
    if (g_movementsFile.is_open()) {
        g_movementsFile.close();
    }
}

// Check if CSV writer is running
bool isCsvWriterRunning() {
    return g_writing.load();
}

// Pause CSV writing
void pauseCsvWriter() {
    if (g_isPaused.exchange(true)) return;
    g_pauseBeginTime.store(getHighPrecisionTimestamp());
}

// Resume CSV writing
void resumeCsvWriter() {
    if (!g_isPaused.exchange(false)) return;

    int64_t pauseEnd = getHighPrecisionTimestamp();
    int64_t pauseBegin = g_pauseBeginTime.load();
    int64_t pauseDuration = pauseEnd - pauseBegin;

    g_totalPausedDuration.fetch_add(pauseDuration);
}
