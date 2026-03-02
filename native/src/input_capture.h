#pragma once

#include <cstdint>
#include <atomic>

// Forward declaration for moodycamel queue
namespace moodycamel {
    template<typename T>
    class ReaderWriterQueue;
}

/**
 * Mouse click event structure
 */
struct MouseClickEvent {
    int64_t rawTime;      // Millisecond timestamp
    int x, y;             // Coordinates
    int button;           // 1=left, 2=right, 3=middle
    bool isDown;          // true=pressed, false=released
    bool altKey, ctrlKey, shiftKey, metaKey;  // Modifier keys
};

/**
 * Keyboard event structure
 */
struct KeyboardEvent {
    int64_t rawTime;      // Millisecond timestamp
    int keycode;          // Virtual key code
    char keyChar;         // Character (e.g., 'A', '1')
    bool isDown;          // true=pressed, false=released
    bool altKey, ctrlKey, shiftKey, metaKey;  // Modifier keys
};

/**
 * Mouse move event structure
 */
struct MouseMoveEvent {
    int64_t rawTime;      // Millisecond timestamp
    int x, y;             // Current coordinates
    int dx, dy;           // Delta from last position
};

// Global lock-free queues for input events
// Initial capacity of 4096 events
extern moodycamel::ReaderWriterQueue<KeyboardEvent>* g_keyboardQueue;
extern moodycamel::ReaderWriterQueue<MouseClickEvent>* g_mouseClickQueue;
extern moodycamel::ReaderWriterQueue<MouseMoveEvent>* g_mouseMoveQueue;

/**
 * Get modifier key states
 */
struct ModifierKeys {
    bool altKey;
    bool ctrlKey;
    bool shiftKey;
    bool metaKey;
};

ModifierKeys getModifierKeys();

/**
 * Initialize input capture system
 * Creates the lock-free queues
 */
void initInputCapture();

/**
 * Shutdown input capture system
 * Stops all threads and deletes queues
 */
void shutdownInputCapture();

/**
 * Start input capture (install hooks, start polling)
 * Must be called after initInputCapture()
 *
 * @return true if started successfully
 */
bool startInputCapture();

/**
 * Stop input capture (uninstall hooks, stop polling)
 */
void stopInputCapture();

/**
 * Check if input capture is currently running
 */
bool isInputCaptureRunning();

/**
 * Set the recording start time for relative timestamp calculation
 * Called when recording starts
 */
void setRecordingStartTime(int64_t startTime);

/**
 * Add paused duration to the total
 * Called when pausing
 */
void addPausedDuration(int64_t duration);

/**
 * Get total paused duration
 */
int64_t getTotalPausedDuration();

/**
 * Reset pause tracking (called when starting new recording)
 */
void resetPauseTracking();

/**
 * Get high-precision timestamp in milliseconds
 * Uses GetSystemTimePreciseAsFileTime()
 */
int64_t getHighPrecisionTimestamp();
