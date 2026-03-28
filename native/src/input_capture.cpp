#include <windows.h>
#include <iostream>

// Define virtual key codes if not already defined
#ifndef VK_A
#define VK_A 0x41
#endif

#ifndef VK_Z
#define VK_Z 0x5A
#endif

#ifndef VK_0
#define VK_0 0x30
#endif

#ifndef VK_9
#define VK_9 0x39
#endif

#ifndef VK_F1
#define VK_F1 0x70
#endif

#ifndef VK_F24
#define VK_F24 0x87
#endif

#ifndef VK_SHIFT
#define VK_SHIFT 0x10
#endif

#ifndef VK_CONTROL
#define VK_CONTROL 0x11
#endif

#ifndef VK_MENU
#define VK_MENU 0x12
#endif

#ifndef VK_LWIN
#define VK_LWIN 0x5B
#endif

#ifndef VK_RWIN
#define VK_RWIN 0x5C
#endif

#ifndef VK_SPACE
#define VK_SPACE 0x20
#endif

#ifndef VK_RETURN
#define VK_RETURN 0x0D
#endif

#ifndef VK_TAB
#define VK_TAB 0x09
#endif

#ifndef VK_ESCAPE
#define VK_ESCAPE 0x1B
#endif

#ifndef VK_LEFT
#define VK_LEFT 0x25
#endif

#ifndef VK_RIGHT
#define VK_RIGHT 0x27
#endif

#ifndef VK_UP
#define VK_UP 0x26
#endif

#ifndef VK_DOWN
#define VK_DOWN 0x28
#endif

#ifndef VK_LBUTTON
#define VK_LBUTTON 0x01
#endif

#ifndef VK_RBUTTON
#define VK_RBUTTON 0x02
#endif

#ifndef VK_MBUTTON
#define VK_MBUTTON 0x04
#endif

#include "protocol.h"
#include "input_capture.h"
#include <cstdio>
#include <cstdlib>
#include <atomic>
#include <thread>
#include <functional>

// Lock-free queue header (header-only)
#include "../third_party/readerwriterqueue/readerwriterqueue.h"

// Global queues
moodycamel::ReaderWriterQueue<KeyboardEvent>* g_keyboardQueue = nullptr;
moodycamel::ReaderWriterQueue<MouseClickEvent>* g_mouseClickQueue = nullptr;
moodycamel::ReaderWriterQueue<MouseMoveEvent>* g_mouseMoveQueue = nullptr;

// Thread handles
static HHOOK g_keyboardHook = nullptr;
static HHOOK g_mouseHook = nullptr;
static HANDLE g_hookThread = nullptr;
static HANDLE g_pollThread = nullptr;
static DWORD g_hookThreadId = 0;

// Recording state
static std::atomic<bool> g_capturing{false};
static std::atomic<bool> g_running{false};

// Mouse polling state
static int g_lastMouseX = 0;
static int g_lastMouseY = 0;
static bool g_lastMouseInitialized = false;

// Mouse move event counter for activity tracking
static std::atomic<int64_t> g_mouseMoveCounter{0};

// Pause tracking
static std::atomic<int64_t> g_recordingStartTime{0};
static std::atomic<int64_t> g_totalPausedDuration{0};
static std::atomic<int64_t> g_pauseBeginTime{0};
static std::atomic<bool> g_isPaused{false};

// Hook-based key/mouse state tracking (updated by low-level hooks)
// These are always updated, regardless of g_capturing state, so that
// getCurrentInputState() works before recording starts.
static std::atomic<bool> g_keyStates[256] = {};  // indexed by virtual key code
static std::atomic<bool> g_mouseButtons[3] = {}; // 0=left, 1=right, 2=middle

// Forward declarations
LRESULT CALLBACK LowLevelKeyboardProc(int nCode, WPARAM wParam, LPARAM lParam);
LRESULT CALLBACK LowLevelMouseProc(int nCode, WPARAM wParam, LPARAM lParam);
DWORD WINAPI HookThreadProc(LPVOID lpParameter);
DWORD WINAPI MousePollThreadProc(LPVOID lpParameter);

// getHighPrecisionTimestamp is defined in utils.cpp

// Get current input state using hook-tracked key states.
// GetAsyncKeyState is unreliable from a background process with no foreground
// window — the hook-based tracking is authoritative since the low-level hooks
// receive real key-down/key-up events via the hook thread's message pump.
InputState getCurrentInputState() {
    InputState state;
    state.anyKeyPressed = false;
    state.mouseButtonPressed = false;
    state.pressedKeyCount = 0;

    // Helper: check a VK and record it if pressed
    auto checkKey = [&](int vk) {
        if (g_keyStates[vk].load(std::memory_order_relaxed)) {
            state.anyKeyPressed = true;
            state.pressedKeyCount++;
            state.pressedVKs.push_back(vk);
        }
    };

    // Check keyboard keys tracked by hooks
    // A-Z
    for (int vk = 0x41; vk <= 0x5A; vk++) checkKey(vk);
    // 0-9
    for (int vk = 0x30; vk <= 0x39; vk++) checkKey(vk);
    // Function keys F1-F24
    for (int vk = VK_F1; vk <= VK_F24; vk++) checkKey(vk);

    // Modifier keys (check left/right variants to avoid double-counting with generic VK)
    if (g_keyStates[VK_LSHIFT].load(std::memory_order_relaxed) ||
        g_keyStates[VK_RSHIFT].load(std::memory_order_relaxed)) {
        state.anyKeyPressed = true;
        state.pressedKeyCount++;
        if (g_keyStates[VK_LSHIFT].load(std::memory_order_relaxed)) state.pressedVKs.push_back(VK_LSHIFT);
        if (g_keyStates[VK_RSHIFT].load(std::memory_order_relaxed)) state.pressedVKs.push_back(VK_RSHIFT);
    }
    if (g_keyStates[VK_LCONTROL].load(std::memory_order_relaxed) ||
        g_keyStates[VK_RCONTROL].load(std::memory_order_relaxed)) {
        state.anyKeyPressed = true;
        state.pressedKeyCount++;
        if (g_keyStates[VK_LCONTROL].load(std::memory_order_relaxed)) state.pressedVKs.push_back(VK_LCONTROL);
        if (g_keyStates[VK_RCONTROL].load(std::memory_order_relaxed)) state.pressedVKs.push_back(VK_RCONTROL);
    }
    if (g_keyStates[VK_LMENU].load(std::memory_order_relaxed) ||
        g_keyStates[VK_RMENU].load(std::memory_order_relaxed)) {
        state.anyKeyPressed = true;
        state.pressedKeyCount++;
        if (g_keyStates[VK_LMENU].load(std::memory_order_relaxed)) state.pressedVKs.push_back(VK_LMENU);
        if (g_keyStates[VK_RMENU].load(std::memory_order_relaxed)) state.pressedVKs.push_back(VK_RMENU);
    }
    if (g_keyStates[VK_LWIN].load(std::memory_order_relaxed) ||
        g_keyStates[VK_RWIN].load(std::memory_order_relaxed)) {
        state.anyKeyPressed = true;
        state.pressedKeyCount++;
        if (g_keyStates[VK_LWIN].load(std::memory_order_relaxed)) state.pressedVKs.push_back(VK_LWIN);
        if (g_keyStates[VK_RWIN].load(std::memory_order_relaxed)) state.pressedVKs.push_back(VK_RWIN);
    }

    // Space, Enter, Tab, Escape
    int specialKeys[] = { VK_SPACE, VK_RETURN, VK_TAB, VK_ESCAPE };
    for (int vk : specialKeys) checkKey(vk);
    // Arrow keys
    int arrowKeys[] = { VK_LEFT, VK_RIGHT, VK_UP, VK_DOWN };
    for (int vk : arrowKeys) checkKey(vk);

    // Mouse buttons (tracked by hook)
    for (int i = 0; i < 3; i++) {
        if (g_mouseButtons[i].load(std::memory_order_relaxed)) {
            state.mouseButtonPressed = true;
            state.anyKeyPressed = true;
            state.pressedMouseBtns.push_back(i + 1);  // 1=left, 2=right, 3=middle
        }
    }

    // Debug: also scan ALL 256 VK codes to find any unexpected ones
    // that are set but not in our checked ranges
    for (int vk = 0; vk < 256; vk++) {
        if (g_keyStates[vk].load(std::memory_order_relaxed)) {
            // Check if already reported
            bool found = false;
            for (int reported : state.pressedVKs) {
                if (reported == vk) { found = true; break; }
            }
            if (!found) {
                state.pressedVKs.push_back(vk);  // unreported VK - might be the mystery key
            }
        }
    }

    return state;
}

// Get modifier key states
ModifierKeys getModifierKeys() {
    ModifierKeys keys;
    // Use g_keyStates array (maintained by hook thread) instead of GetAsyncKeyState
    // This provides more accurate modifier key state during key events
    keys.altKey = g_keyStates[VK_MENU].load(std::memory_order_relaxed);
    keys.ctrlKey = g_keyStates[VK_CONTROL].load(std::memory_order_relaxed);
    keys.shiftKey = g_keyStates[VK_SHIFT].load(std::memory_order_relaxed);
    keys.metaKey = g_keyStates[VK_LWIN].load(std::memory_order_relaxed) ||
                   g_keyStates[VK_RWIN].load(std::memory_order_relaxed);
    return keys;
}

// Initialize input capture system
// Starts the hook thread immediately so that getCurrentInputState()
// can track key/mouse state even before recording begins.
void initInputCapture() {
    std::cerr << "[DIAG] initInputCapture() called" << std::endl; std::cerr.flush();

    if (g_keyboardQueue) return;

    std::cerr << "[DIAG] initInputCapture: allocating queues" << std::endl; std::cerr.flush();
    g_keyboardQueue = new moodycamel::ReaderWriterQueue<KeyboardEvent>(4096);
    g_mouseClickQueue = new moodycamel::ReaderWriterQueue<MouseClickEvent>(4096);
    g_mouseMoveQueue = new moodycamel::ReaderWriterQueue<MouseMoveEvent>(4096);

    g_lastMouseInitialized = false;

    // Clear tracked key/mouse state
    for (int i = 0; i < 256; i++) {
        g_keyStates[i].store(false, std::memory_order_relaxed);
    }
    for (int i = 0; i < 3; i++) {
        g_mouseButtons[i].store(false, std::memory_order_relaxed);
    }

    // Start hook thread early so we can track key state before recording
    std::cerr << "[DIAG] initInputCapture: starting hook thread" << std::endl; std::cerr.flush();
    g_running.store(true);
    g_hookThread = CreateThread(NULL, 0, HookThreadProc, NULL, 0, &g_hookThreadId);
    std::cerr << "[DIAG] initInputCapture: CreateThread returned, handle=" << g_hookThread << std::endl; std::cerr.flush();
}

// Shutdown input capture system
void shutdownInputCapture() {
    stopInputCapture();

    // Stop the hook thread (started in initInputCapture)
    g_running.store(false);
    if (g_hookThread) {
        PostThreadMessage(g_hookThreadId, WM_QUIT, 0, 0);
        WaitForSingleObject(g_hookThread, INFINITE);
        CloseHandle(g_hookThread);
        g_hookThread = nullptr;
    }

    delete g_keyboardQueue;
    delete g_mouseClickQueue;
    delete g_mouseMoveQueue;

    g_keyboardQueue = nullptr;
    g_mouseClickQueue = nullptr;
    g_mouseMoveQueue = nullptr;
}

// Low-level keyboard hook callback
LRESULT CALLBACK LowLevelKeyboardProc(int nCode, WPARAM wParam, LPARAM lParam) {
    if (nCode == HC_ACTION) {
        KBDLLHOOKSTRUCT* pKeyBoard = (KBDLLHOOKSTRUCT*)lParam;

        // Only process key down and key up
        if (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN ||
            wParam == WM_KEYUP || wParam == WM_SYSKEYUP) {

            bool isDown = (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN);
            int vk = static_cast<int>(pKeyBoard->vkCode);

            // Always update hook-tracked key state (for getCurrentInputState)
            if (vk >= 0 && vk < 256) {
                g_keyStates[vk].store(isDown, std::memory_order_relaxed);
            }

            // Only enqueue events to the recording queue when capturing
            if (g_capturing.load()) {
                // CRITICAL: Use GetSystemTimePreciseAsFileTime, NOT pKeyBoard->time
                int64_t timestamp = getHighPrecisionTimestamp();

                // Get modifier keys
                ModifierKeys mods = getModifierKeys();

                // Get virtual key code
                int keycode = vk;

                // Get character (if possible)
                char keyChar = 0;
                if (isDown) {
                    BYTE keyboardState[256] = {};
                    GetKeyboardState(keyboardState);
                    UINT scanCode = pKeyBoard->scanCode;
                    WCHAR wch[2] = {};
                    if (ToUnicode(keycode, scanCode, keyboardState, wch, 2, 0) > 0) {
                        if (wch[0] >= 32 && wch[0] < 127) {
                            keyChar = static_cast<char>(wch[0]);
                        }
                    }
                }

                KeyboardEvent evt;
                evt.rawTime = timestamp;
                evt.keycode = keycode;
                evt.keyChar = keyChar;
                evt.isDown = isDown;
                evt.altKey = mods.altKey;
                evt.ctrlKey = mods.ctrlKey;
                evt.shiftKey = mods.shiftKey;
                evt.metaKey = mods.metaKey;

                // Enqueue to lock-free queue (O(1), no blocking)
                if (g_keyboardQueue) {
                    g_keyboardQueue->enqueue(evt);
                }
            }
        }
    }

    return CallNextHookEx(NULL, nCode, wParam, lParam);
}

// Low-level mouse hook callback
LRESULT CALLBACK LowLevelMouseProc(int nCode, WPARAM wParam, LPARAM lParam) {
    if (nCode == HC_ACTION) {
        MSLLHOOKSTRUCT* pMouse = (MSLLHOOKSTRUCT*)lParam;

        // Only process button events, ignore mouse move (handled by polling)
        if (wParam == WM_LBUTTONDOWN || wParam == WM_LBUTTONUP ||
            wParam == WM_RBUTTONDOWN || wParam == WM_RBUTTONUP ||
            wParam == WM_MBUTTONDOWN || wParam == WM_MBUTTONUP) {

            int button = 1;  // default left
            if (wParam == WM_RBUTTONDOWN || wParam == WM_RBUTTONUP) button = 2;
            if (wParam == WM_MBUTTONDOWN || wParam == WM_MBUTTONUP) button = 3;

            bool isDown = (wParam == WM_LBUTTONDOWN ||
                          wParam == WM_RBUTTONDOWN ||
                          wParam == WM_MBUTTONDOWN);

            // Always update hook-tracked mouse state (for getCurrentInputState)
            if (button >= 1 && button <= 3) {
                g_mouseButtons[button - 1].store(isDown, std::memory_order_relaxed);
            }

            // Only enqueue events to the recording queue when capturing
            if (g_capturing.load()) {
                // CRITICAL: Use GetSystemTimePreciseAsFileTime
                int64_t timestamp = getHighPrecisionTimestamp();

                // Get modifier keys
                ModifierKeys mods = getModifierKeys();

                MouseClickEvent evt;
                evt.rawTime = timestamp;
                evt.x = pMouse->pt.x;
                evt.y = pMouse->pt.y;
                evt.button = button;
                evt.isDown = isDown;
                evt.altKey = mods.altKey;
                evt.ctrlKey = mods.ctrlKey;
                evt.shiftKey = mods.shiftKey;
                evt.metaKey = mods.metaKey;

                // Enqueue to lock-free queue
                if (g_mouseClickQueue) {
                    g_mouseClickQueue->enqueue(evt);
                }
            }
        }
    }

    return CallNextHookEx(NULL, nCode, wParam, lParam);
}

// Hook thread procedure
DWORD WINAPI HookThreadProc(LPVOID lpParameter) {
    (void)lpParameter;

    std::cerr << "[DIAG] HookThreadProc: started, installing hooks" << std::endl; std::cerr.flush();

    // Install low-level hooks
    g_keyboardHook = SetWindowsHookEx(WH_KEYBOARD_LL, LowLevelKeyboardProc, GetModuleHandle(NULL), 0);
    std::cerr << "[DIAG] HookThreadProc: keyboard hook result=" << g_keyboardHook << std::endl; std::cerr.flush();

    g_mouseHook = SetWindowsHookEx(WH_MOUSE_LL, LowLevelMouseProc, GetModuleHandle(NULL), 0);
    std::cerr << "[DIAG] HookThreadProc: mouse hook result=" << g_mouseHook << std::endl; std::cerr.flush();

    if (!g_keyboardHook || !g_mouseHook) {
        // Failed to install hooks
        std::cerr << "[DIAG] HookThreadProc: hook install FAILED, keyboard=" << g_keyboardHook << " mouse=" << g_mouseHook << std::endl; std::cerr.flush();
        if (g_keyboardHook) { UnhookWindowsHookEx(g_keyboardHook); g_keyboardHook = nullptr; }
        if (g_mouseHook) { UnhookWindowsHookEx(g_mouseHook); g_mouseHook = nullptr; }
        return 1;
    }

    std::cerr << "[DIAG] HookThreadProc: hooks installed OK, entering message pump" << std::endl; std::cerr.flush();

    // Message pump (REQUIRED for low-level hooks to fire)
    MSG msg;
    while (g_running.load() && GetMessage(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    // Cleanup
    if (g_keyboardHook) { UnhookWindowsHookEx(g_keyboardHook); g_keyboardHook = nullptr; }
    if (g_mouseHook) { UnhookWindowsHookEx(g_mouseHook); g_mouseHook = nullptr; }

    return 0;
}

// Mouse polling thread (200Hz) with high-precision timing
DWORD WINAPI MousePollThreadProc(LPVOID lpParameter) {
    (void)lpParameter;

    POINT pos;
    int lastX = 0, lastY = 0;
    bool first = true;

    // High-precision timing: target time starts at now + 5ms
    int64_t targetTime = getHighPrecisionTimestamp() + 5;

    // Poll at exactly 200Hz (5ms interval) using spin-wait
    // Runs while g_capturing is true (stops when stopInputCapture is called)
    while (g_capturing.load()) {
        // Spin-wait until target time for high precision
        while (getHighPrecisionTimestamp() < targetTime) {
            // Busy wait - yields to other threads but stays precise
            Sleep(0);
        }

        if (GetCursorPos(&pos)) {
            if (first) {
                // Initialize on first poll
                lastX = pos.x;
                lastY = pos.y;
                g_lastMouseX = pos.x;
                g_lastMouseY = pos.y;
                first = false;
            } else {
                int dx = pos.x - lastX;
                int dy = pos.y - lastY;

                // Always record every poll (5ms) for stable timestamp intervals
                int64_t timestamp = getHighPrecisionTimestamp();

                MouseMoveEvent evt;
                evt.rawTime = timestamp;
                evt.x = pos.x;
                evt.y = pos.y;
                evt.dx = dx;
                evt.dy = dy;

                if (g_mouseMoveQueue) {
                    g_mouseMoveQueue->enqueue(evt);
                    g_mouseMoveCounter.fetch_add(1, std::memory_order_relaxed);
                }

                lastX = pos.x;
                lastY = pos.y;
                g_lastMouseX = pos.x;
                g_lastMouseY = pos.y;
            }
        }

        // Set next target time: current target + 5ms
        targetTime += 5;
    }

    return 0;
}

// Start input capture (enable event queueing + start mouse polling)
// The hook thread is already running from initInputCapture().
bool startInputCapture() {
    if (g_capturing.load()) return true;

    g_capturing.store(true);
    g_lastMouseInitialized = false;
    g_mouseMoveCounter.store(0, std::memory_order_relaxed);

    // Start mouse polling thread
    g_pollThread = CreateThread(NULL, 0, MousePollThreadProc, NULL, 0, NULL);
    if (!g_pollThread) {
        g_capturing.store(false);
        return false;
    }

    return true;
}

// Stop input capture (disable event queueing + stop mouse polling)
// The hook thread keeps running for key state tracking until shutdown.
void stopInputCapture() {
    if (!g_capturing.load()) return;

    g_capturing.store(false);

    // Stop poll thread — it checks g_capturing in its loop
    if (g_pollThread) {
        WaitForSingleObject(g_pollThread, 2000);
        CloseHandle(g_pollThread);
        g_pollThread = nullptr;
    }
}

// Check if input capture is running
bool isInputCaptureRunning() {
    return g_capturing.load();
}

// Set recording start time
void setRecordingStartTime(int64_t startTime) {
    g_recordingStartTime.store(startTime);
    g_totalPausedDuration.store(0);
    g_isPaused.store(false);
}

// Add paused duration
void addPausedDuration(int64_t duration) {
    g_totalPausedDuration.fetch_add(duration);
}

// Get total paused duration
int64_t getTotalPausedDuration() {
    return g_totalPausedDuration.load();
}

// Reset pause tracking
void resetPauseTracking() {
    g_recordingStartTime.store(0);
    g_totalPausedDuration.store(0);
    g_pauseBeginTime.store(0);
    g_isPaused.store(false);
}

// Get mouse move counter value (total events since recording start)
int getMouseMoveQueueSize() {
    return static_cast<int>(g_mouseMoveCounter.load(std::memory_order_relaxed));
}
