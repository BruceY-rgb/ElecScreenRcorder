#pragma once

#include <string>
#include <optional>

/**
 * Command types supported by the recorder
 */
enum class CommandType {
    START,
    STOP,
    PAUSE,
    RESUME,
    SYSINFO,
    QUIT,
    UNKNOWN
};

/**
 * Recording configuration from START command
 */
struct StartConfig {
    std::string resolution;      // "1080p", "2k", "4k"
    int fps = 60;                 // 30, 60
    std::string savePath;         // Output file path
    bool separateAudio = false;    // Separate audio tracks
    bool remuxToMp4 = false;      // Convert to MP4 after recording
};

/**
 * Parsed command from JSON input
 */
struct Command {
    CommandType type = CommandType::UNKNOWN;
    StartConfig config;
    std::string rawAction;
};

/**
 * Parse a JSON command from string input.
 *
 * @param input JSON string from stdin
 * @return Parsed Command or empty if invalid
 */
std::optional<Command> parseCommand(const std::string& input);

/**
 * Create a status response JSON string
 * @param state Current state: "ready", "recording", "paused", "stopped"
 */
std::string createStatusResponse(const std::string& state);

/**
 * Create a sysinfo response JSON string
 * @param screenWidth Screen width in pixels
 * @param screenHeight Screen height in pixels
 * @param refreshRate Monitor refresh rate in Hz
 */
std::string createSysInfoResponse(int screenWidth, int screenHeight, int refreshRate);

/**
 * Create a finish response JSON string
 * @param videoPath Path to the recorded video file
 * @param actionsPath Path to the actions CSV file
 * @param movementsPath Path to the movements CSV file
 */
std::string createFinishResponse(const std::string& videoPath,
                                 const std::string& actionsPath,
                                 const std::string& movementsPath);

/**
 * Create an error response JSON string
 * @param message Error message
 */
std::string createErrorResponse(const std::string& message);

/**
 * Escape a string for JSON (handle special characters)
 */
std::string jsonEscape(const std::string& s);
