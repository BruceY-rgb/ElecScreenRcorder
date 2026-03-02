#pragma once

#include <string>
#include <atomic>

/**
 * Initialize CSV writer system
 */
void initCsvWriter();

/**
 * Shutdown CSV writer system
 */
void shutdownCsvWriter();

/**
 * Start CSV writer (open files, start writer thread)
 *
 * @param actionsPath Path to actions CSV file
 * @param movementsPath Path to movements CSV file
 * @param recordingStartTime Raw timestamp when recording started
 * @return true if started successfully
 */
bool startCsvWriter(const std::string& actionsPath,
                    const std::string& movementsPath,
                    int64_t recordingStartTime);

/**
 * Stop CSV writer (drain queues, close files)
 */
void stopCsvWriter();

/**
 * Check if CSV writer is currently running
 */
bool isCsvWriterRunning();

/**
 * Pause CSV writing (accumulate pause time)
 */
void pauseCsvWriter();

/**
 * Resume CSV writing
 */
void resumeCsvWriter();
