/**
 * Protocol Implementation
 *
 * JSON protocol parsing and serialization using nlohmann-json library.
 */

#include "protocol.h"
#include "../third_party/nlohmann-json/json.hpp"

#include <iostream>
#include <sstream>
#include <algorithm>
#include <cctype>

using json = nlohmann::json;

namespace {

// Trim whitespace from string
std::string trim(const std::string& s) {
    auto start = std::find_if(s.begin(), s.end(), [](unsigned char c) { return !std::isspace(c); });
    auto end = std::find_if(s.rbegin(), s.rend(), [](unsigned char c) { return !std::isspace(c) }).base();
    return (start < end) ? std::string(start, end) : "";
}

// Parse action type from JSON
CommandType parseActionType(const json& j) {
    if (!j.contains("action") || !j["action"].is_string()) {
        return CommandType::UNKNOWN;
    }

    std::string action = j["action"].get<std::string>();
    std::transform(action.begin(), action.end(), action.begin(), ::tolower);

    if (action == "start") return CommandType::START;
    if (action == "stop") return CommandType::STOP;
    if (action == "pause") return CommandType::PAUSE;
    if (action == "resume") return CommandType::RESUME;
    if (action == "sysinfo") return CommandType::SYSINFO;
    if (action == "quit") return CommandType::QUIT;

    return CommandType::UNKNOWN;
}

} // anonymous namespace

std::string jsonEscape(const std::string& s) {
    std::string result;
    for (char c : s) {
        switch (c) {
            case '"':  result += "\\\""; break;
            case '\\': result += "\\\\"; break;
            case '\b': result += "\\b";  break;
            case '\f': result += "\\f";  break;
            case '\n': result += "\\n";  break;
            case '\r': result += "\\r";  break;
            case '\t': result += "\\t";  break;
            default:   result += c;       break;
        }
    }
    return result;
}

std::optional<Command> parseCommand(const std::string& input) {
    std::string trimmed = trim(input);

    if (trimmed.empty()) {
        return std::nullopt;
    }

    // Parse JSON
    json j;
    try {
        j = json::parse(trimmed);
    } catch (const json::parse_error& e) {
        return std::nullopt;
    }

    if (!j.is_object()) {
        return std::nullopt;
    }

    Command cmd;
    cmd.type = parseActionType(j);

    if (j.contains("action") && j["action"].is_string()) {
        cmd.rawAction = j["action"].get<std::string>();
    }

    // Parse config for START command
    if (cmd.type == CommandType::START && j.contains("config")) {
        const json& config = j["config"];

        if (config.contains("width") && config["width"].is_number()) {
            cmd.config.width = config["width"].get<int>();
        }
        if (config.contains("height") && config["height"].is_number()) {
            cmd.config.height = config["height"].get<int>();
        }
        if (config.contains("fps") && config["fps"].is_number()) {
            cmd.config.fps = config["fps"].get<int>();
        }
        if (config.contains("savePath") && config["savePath"].is_string()) {
            cmd.config.savePath = config["savePath"].get<std::string>();
        }
        if (config.contains("separateAudio") && config["separateAudio"].is_boolean()) {
            cmd.config.separateAudio = config["separateAudio"].get<bool>();
        }
        if (config.contains("remuxToMp4") && config["remuxToMp4"].is_boolean()) {
            cmd.config.remuxToMp4 = config["remuxToMp4"].get<bool>();
        }
    }

    return cmd;
}

std::string createStatusResponse(const std::string& state) {
    json j;
    j["type"] = "status";
    j["state"] = state;
    return j.dump();
}

std::string createSysInfoResponse(int screenWidth, int screenHeight, int refreshRate,
                                  double scalingFactor, const std::string& cpuName,
                                  const std::string& gpuName, int ramGB, int mousePollingRate) {
    json j;
    j["type"] = "sysinfo";
    j["data"] = {
        {"screenWidth", screenWidth},
        {"screenHeight", screenHeight},
        {"refreshRate", refreshRate},
        {"scalingFactor", scalingFactor},
        {"cpuName", cpuName},
        {"gpuName", gpuName},
        {"ramGB", ramGB},
        {"mousePollingRate", mousePollingRate}
    };
    return j.dump();
}

std::string createFinishResponse(const std::string& videoPath,
                                 const std::string& actionsPath,
                                 const std::string& movementsPath,
                                 int64_t duration) {
    json j;
    j["type"] = "finish";
    j["videoPath"] = videoPath;
    j["actionsPath"] = actionsPath;
    j["movementsPath"] = movementsPath;
    j["duration"] = duration;
    return j.dump();
}

std::string createErrorResponse(const std::string& message) {
    json j;
    j["type"] = "error";
    j["msg"] = message;
    return j.dump();
}
