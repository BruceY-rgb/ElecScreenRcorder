#include "input_capture.h"
#include "protocol.h"
#include <windows.h>
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

// Pause tracking
static std::atomic<int64_t> g_recordingStartTime{0};
static std::atomic<int64_t> g_totalPausedDuration{0};
static std::atomic<int64_t> g_pauseBeginTime{0};
static std::atomic<bool> g_isPaused{false};

// Forward declarations
LRESULT CALLBACK LowLevelKeyboardProc(int nCode, WPARAM wParam, LPARAM lParam);
LRESULT CALLBACK LowLevelMouseProc(int nCode, WPARAM wParam, LPARAM lParam);
DWORD WINAPI HookThreadProc(LPVOID lpParameter);
DWORD WINAPI MousePollThreadProc(LPVOID lpParameter);

// Get high-precision timestamp (must match utils.cpp)
int64_t getHighPrecisionTimestamp() {
    FILETIME ft;
    GetSystemTimePreciseAsFileTime(&ft);
    ULARGE_INTEGER uli;
    uli.LowPart = ft.dwLowDateTime;
    uli.HighPart = ft.dwHighDateTime;
    return static_cast<int64_t>(uli.QuadPart / 10000);
}

// Get current input state (any keys or mouse buttons pressed)
InputState getCurrentInputState() {
    InputState state;
    state.anyKeyPressed = false;
    state.mouseButtonPressed = false;
    state.pressedKeyCount = 0;

    // Check standard A-Z, 0-9 keys
    for (int vk = VK_A; vk <= VK_Z; vk++) {
        if (GetAsyncKeyState(vk) & 0x8000) {
            state.anyKeyPressed = true;
            state.pressedKeyCount++;
        }
    }
    for (int vk = VK_0; vk <= VK_9; vk++) {
        if (GetAsyncKeyState(vk) & 0x8000) {
            state.anyKeyPressed = true;
            state.pressedKeyCount++;
        }
    }
    // Check function keys F1-F24
    for (int vk = VK_F1; vk <= VK_F24; vk++) {
        if (GetAsyncKeyState(vk) & 0x8000) {
            state.anyKeyPressed = true;
            state.pressedKeyCount++;
        }
    }
    // Check modifier keys
    if (GetAsyncKeyState(VK_SHIFT) & 0x8000) {
        state.anyKeyPressed = true;
        state.pressedKeyCount++;
    }
    if (GetAsyncKeyState(VK_CONTROL) & 0x8000) {
        state.anyKeyPressed = true;
        state.pressedKeyCount++;
    }
    if (GetAsyncKeyState(VK_MENU) & 0x8000) {  // Alt
        state.anyKeyPressed = true;
        state.pressedKeyCount++;
    }
    if (GetAsyncKeyState(VK_LWIN) & 0x8000 || GetAsyncKeyState(VK_RWIN) & 0x8000) {
        state.anyKeyPressed = true;
        state.pressedKeyCount++;
    }
    // Check space, enter, tab, escape
    if (GetAsyncKeyState(VK_SPACE) & 0x8000) {
        state.anyKeyPressed = true;
        state.pressedKeyCount++;
    }
    if (GetAsyncKeyState(VK_RETURN) & 0x8000) {
        state.anyKeyPressed = true;
        state.pressedKeyCount++;
    }
    if (GetAsyncKeyState(VK_TAB) & 0x8000) {
        state.anyKeyPressed = true;
        state.pressedKeyCount++;
    }
    if (GetAsyncKeyState(VK_ESCAPE) & 0x8000) {
        state.anyKeyPressed = true;
        state.pressedKeyCount++;
    }
    // Check arrow keys
    if (GetAsyncKeyState(VK_LEFT) & 0x8000 || GetAsyncKeyState(VK_RIGHT) & 0x8000 ||
        GetAsyncKeyState(VK_UP) & 0x8000 || GetAsyncKeyState(VK_DOWN) & 0x8000) {
        state.anyKeyPressed = true;
        state.pressedKeyCount++;
    }

    // Check mouse buttons (left, right, middle)
    if (GetAsyncKeyState(VK_LBUTTON) & 0x8000) {
        state.mouseButtonPressed = true;
        state.anyKeyPressed = true;
    }
    if (GetAsyncKeyState(VK_RBUTTON) & 0x8000) {
        state.mouseButtonPressed = true;
        state.anyKeyPressed = true;
    }
    if (GetAsyncKeyState(VK_MBUTTON) & 0x8000) {
        state.mouseButtonPressed = true;
        state.anyKeyPressed = true;
    }

    return state;
}

// Get modifier key states
ModifierKeys getModifierKeys() {
    ModifierKeys keys;
    keys.altKey = (GetAsyncKeyState(VK_MENU) & 0x8000) != 0;
    keys.ctrlKey = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
    keys.shiftKey = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
    keys.metaKey = (GetAsyncKeyState(VK_LWIN) & 0x8000) != 0 ||
                   (GetAsyncKeyState(VK_RWIN) & 0x8000) != 0;
    return keys;
}

// Initialize input capture system
void initInputCapture() {
    if (g_keyboardQueue) return;

    g_keyboardQueue = new moodycamel::ReaderWriterQueue<KeyboardEvent>(4096);
    g_mouseClickQueue = new moodycamel::ReaderWriterQueue<MouseClickEvent>(4096);
    g_mouseMoveQueue = new moodycamel::ReaderWriterQueue<MouseMoveEvent>(4096);

    g_lastMouseInitialized = false;
}

// Shutdown input capture system
void shutdownInputCapture() {
    stopInputCapture();

    delete g_keyboardQueue;
    delete g_mouseClickQueue;
    delete g_mouseMoveQueue;

    g_keyboardQueue = nullptr;
    g_mouseClickQueue = nullptr;
    g_mouseMoveQueue = nullptr;
}

// Low-level keyboard hook callback
LRESULT CALLBACK LowLevelKeyboardProc(int nCode, WPARAM wParam, LPARAM lParam) {
    if (nCode == HC_ACTION && g_capturing.load()) {
        KBDLLHOOKSTRUCT* pKeyBoard = (KBDLLHOOKSTRUCT*)lParam;

        // Only process key down and key up
        if (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN ||
            wParam == WM_KEYUP || wParam == WM_SYSKEYUP) {

            // CRITICAL: Use GetSystemTimePreciseAsFileTime, NOT pKeyBoard->time
            int64_t timestamp = getHighPrecisionTimestamp();

            bool isDown = (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN);

            // Get modifier keys
            ModifierKeys mods = getModifierKeys();

            // Get virtual key code
            int keycode = static_cast<int>(pKeyBoard->vkCode);

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

    return CallNextHookEx(NULL, nCode, wParam, lParam);
}

// Low-level mouse hook callback
LRESULT CALLBACK LowLevelMouseProc(int nCode, WPARAM wParam, LPARAM lParam) {
    if (nCode == HC_ACTION && g_capturing.load()) {
        MSLLHOOKSTRUCT* pMouse = (MSLLHOOKSTRUCT*)lParam;

        // Only process button events, ignore mouse move (handled by polling)
        if (wParam == WM_LBUTTONDOWN || wParam == WM_LBUTTONUP ||
            wParam == WM_RBUTTONDOWN || wParam == WM_RBUTTONUP ||
            wParam == WM_MBUTTONDOWN || wParam == WM_MBUTTONUP) {

            // CRITICAL: Use GetSystemTimePreciseAsFileTime
            int64_t timestamp = getHighPrecisionTimestamp();

            int button = 1;  // default left
            if (wParam == WM_RBUTTONDOWN || wParam == WM_RBUTTONUP) button = 2;
            if (wParam == WM_MBUTTONDOWN || wParam == WM_MBUTTONUP) button = 3;

            bool isDown = (wParam == WM_LBUTTONDOWN ||
                          wParam == WM_RBUTTONDOWN ||
                          wParam == WM_MBUTTONDOWN);

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

    return CallNextHookEx(NULL, nCode, wParam, lParam);
}

// Hook thread procedure
DWORD WINAPI HookThreadProc(LPVOID lpParameter) {
    (void)lpParameter;

    // Install low-level hooks
    g_keyboardHook = SetWindowsHookEx(WH_KEYBOARD_LL, LowLevelKeyboardProc, GetModuleHandle(NULL), 0);
    g_mouseHook = SetWindowsHookEx(WH_MOUSE_LL, LowLevelMouseProc, GetModuleHandle(NULL), 0);

    if (!g_keyboardHook || !g_mouseHook) {
        // Failed to install hooks
        if (g_keyboardHook) { UnhookWindowsHookEx(g_keyboardHook); g_keyboardHook = nullptr; }
        if (g_mouseHook) { UnhookWindowsHookEx(g_mouseHook); g_mouseHook = nullptr; }
        return 1;
    }

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

// Mouse polling thread (200Hz)
DWORD WINAPI MousePollThreadProc(LPVOID lpParameter) {
    (void)lpParameter;

    POINT pos;
    int lastX = 0, lastY = 0;
    bool first = true;

    // Poll at ~200Hz (Sleep 5ms)
    while (g_running.load()) {
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

                // Only record if moved
                if (dx != 0 || dy != 0) {
                    int64_t timestamp = getHighPrecisionTimestamp();

                    MouseMoveEvent evt;
                    evt.rawTime = timestamp;
                    evt.x = pos.x;
                    evt.y = pos.y;
                    evt.dx = dx;
                    evt.dy = dy;

                    if (g_mouseMoveQueue) {
                        g_mouseMoveQueue->enqueue(evt);
                    }

                    lastX = pos.x;
                    lastY = pos.y;
                    g_lastMouseX = pos.x;
                    g_lastMouseY = pos.y;
                }
            }
        }

        Sleep(5);  // ~200Hz with timeBeginPeriod(1)
    }

    return 0;
}

// Start input capture
bool startInputCapture() {
    if (g_capturing.load()) return true;

    g_running.store(true);
    g_capturing.store(true);
    g_lastMouseInitialized = false;

    // Start hook thread
    g_hookThread = CreateThread(NULL, 0, HookThreadProc, NULL, 0, &g_hookThreadId);
    if (!g_hookThread) {
        g_running.store(false);
        g_capturing.store(false);
        return false;
    }

    // Start mouse polling thread
    g_pollThread = CreateThread(NULL, 0, MousePollThreadProc, NULL, 0, NULL);
    if (!g_pollThread) {
        // Cleanup hook thread
        if (g_hookThread) {
            PostThreadMessage(g_hookThreadId, WM_QUIT, 0, 0);
            WaitForSingleObject(g_hookThread, INFINITE);
            CloseHandle(g_hookThread);
            g_hookThread = nullptr;
        }
        g_running.store(false);
        g_capturing.store(false);
        return false;
    }

    return true;
}

// Stop input capture
void stopInputCapture() {
    if (!g_capturing.load()) return;

    g_capturing.store(false);

    // Stop running flag
    g_running.store(false);

    // Stop hook thread
    if (g_hookThread) {
        PostThreadMessage(g_hookThreadId, WM_QUIT, 0, 0);
        WaitForSingleObject(g_hookThread, INFINITE);
        CloseHandle(g_hookThread);
        g_hookThread = nullptr;
    }

    // Stop poll thread
    if (g_pollThread) {
        WaitForSingleObject(g_pollThread, INFINITE);
        CloseHandle(g_pollThread);
        g_pollThread = nullptr;
    }

    g_keyboardHook = nullptr;
    g_mouseHook = nullptr;
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
