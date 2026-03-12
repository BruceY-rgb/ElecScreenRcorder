#include "utils.h"
#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <io.h>

int64_t getHighPrecisionTimestamp() {
    FILETIME ft;
    GetSystemTimePreciseAsFileTime(&ft);
    ULARGE_INTEGER uli;
    uli.LowPart = ft.dwLowDateTime;
    uli.HighPart = ft.dwHighDateTime;
    // Convert from 100-nanosecond intervals to milliseconds
    return static_cast<int64_t>(uli.QuadPart / 10000);
}

void initHighResTimer() {
    timeBeginPeriod(1);
}

void cleanupHighResTimer() {
    timeEndPeriod(1);
}

void setupBinaryMode() {
    _setmode(_fileno(stdin), _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);
}

void setupDpiAwareness() {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
}
