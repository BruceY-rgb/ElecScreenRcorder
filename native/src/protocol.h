#pragma once

#include <string>
#include <optional>
#include <cstdint>
#include <vector>

/**
 * Command types supported by the recorder
 */
enum class CommandType {
    START,
    STOP,
    PAUSE,
    RESUME,
    SYSINFO,
    CHECK_INPUT,
    QUIT,
    UNKNOWN
};

/**
 * Current input state structure
 */
struct InputState {
    bool anyKeyPressed = false;
    bool mouseButtonPressed = false;
    int pressedKeyCount = 0;
    std::vector<int> pressedVKs;       // debug: which VK codes are down
    std::vector<int> pressedMouseBtns;  // debug: which mouse buttons are down
};

/**
 * Recording configuration from START command
 */
struct StartConfig {
    int width = 1920;                // Screen width
    int height = 1080;               // Screen height
    int fps = 60;                    // Frame rate
    int bitrate = 15000;             // Video bitrate in kbps
    std::string savePath;             // Output file path
    bool separateAudio = false;      // Separate audio tracks
    bool remuxToMp4 = false;         // Convert to MP4 after recording
    bool captureAudio = true;        // Capture system audio
    bool captureMicrophone = false;  // Capture microphone
    std::string microphoneDevice;   // Microphone device name
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
 * @param scalingFactor DPI scaling factor
 * @param cpuName CPU name
 * @param gpuName GPU name
 * @param ramGB RAM in GB
 * @param mousePollingRate Mouse polling rate in Hz
 */
std::string createSysInfoResponse(int screenWidth, int screenHeight, int refreshRate,
                                  double scalingFactor = 1.0,
                                  const std::string& cpuName = "",
                                  const std::string& gpuName = "",
                                  int ramGB = 0,
                                  int mousePollingRate = 200);

/**
 * Create a finish response JSON string
 * @param videoPath Path to the recorded video file
 * @param actionsPath Path to the actions CSV file
 * @param movementsPath Path to the movements CSV file
 * @param duration Recording duration in milliseconds
 */
std::string createFinishResponse(const std::string& videoPath,
                                 const std::string& actionsPath,
                                 const std::string& movementsPath,
                                 const std::string& micAudioPath = "",
                                 int64_t duration = 0);

/**
 * Create an error response JSON string
 * @param message Error message
 */
std::string createErrorResponse(const std::string& message);

/**
 * Create an input state response JSON string
 * @param state InputState struct with current input status
 */
std::string createInputStateResponse(const InputState& state);

/**
 * Create a mouse activity response JSON string
 * @param eventsPerSecond Mouse move events per second
 */
std::string createMouseActivityResponse(int eventsPerSecond);

/**
 * Escape a string for JSON (handle special characters)
 */
std::string jsonEscape(const std::string& s);
