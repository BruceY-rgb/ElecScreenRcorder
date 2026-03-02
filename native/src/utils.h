#pragma once

#include <cstdint>
#include <windows.h>

/**
 * Get high-precision timestamp in milliseconds.
 * Uses GetSystemTimePreciseAsFileTime() for sub-microsecond precision.
 *
 * @return Current time in milliseconds since epoch
 */
int64_t getHighPrecisionTimestamp();

/**
 * Initialize high-resolution timer.
 * Calls timeBeginPeriod(1) to set system timer to 1ms precision.
 * Must be called before any time-sensitive operations.
 */
void initHighResTimer();

/**
 * Cleanup high-resolution timer.
 * Calls timeEndPeriod(1) to restore default timer precision.
 * Must be called before exit to restore system state.
 */
void cleanupHighResTimer();

/**
 * Setup binary mode for stdin/stdout.
 * This prevents Windows from converting \n to \r\n on output.
 * MUST be called at program startup.
 */
void setupBinaryMode();

/**
 * Setup DPI awareness for the process.
 * Uses SetProcessDpiAwarenessContext with PER_MONITOR_AWARE_V2.
 * Must be called at program startup for accurate mouse coordinates.
 */
void setupDpiAwareness();
