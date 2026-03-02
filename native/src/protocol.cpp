#include "protocol.h"
#include <iostream>
#include <sstream>
#include <algorithm>
#include <cctype>
#include <cstdio>

namespace {

// Simple string trimming
std::string trim(const std::string& s) {
    auto start = std::find_if(s.begin(), s.end(), [](unsigned char c) { return !std::isspace(c); });
    auto end = std::find_if(s.rbegin(), s.rend(), [](unsigned char c) { return !std::isspace(c); }).base();
    return (start < end) ? std::string(start, end) : "";
}

// Simple JSON string parsing (extract value for key)
std::string getStringValue(const std::string& json, const std::string& key) {
    std::string searchKey = "\"" + key + "\"";
    size_t keyPos = json.find(searchKey);
    if (keyPos == std::string::npos) return "";

    // Find the colon after the key
    size_t colonPos = json.find(":", keyPos);
    if (colonPos == std::string::npos) return "";

    // Find the opening quote for the value
    size_t quotePos = json.find("\"", colonPos);
    if (quotePos == std::string::npos) return "";

    // Find the closing quote
    size_t endQuotePos = json.find("\"", quotePos + 1);
    if (endQuotePos == std::string::npos) return "";

    return json.substr(quotePos + 1, endQuotePos - quotePos - 1);
}

// Simple JSON boolean parsing
bool getBoolValue(const std::string& json, const std::string& key) {
    std::string searchKey = "\"" + key + "\"";
    size_t keyPos = json.find(searchKey);
    if (keyPos == std::string::npos) return false;

    size_t colonPos = json.find(":", keyPos);
    if (colonPos == std::string::npos) return false;

    // Look for true/false after colon
    std::string afterColon = trim(json.substr(colonPos + 1));
    if (afterColon.substr(0, 4) == "true") return true;
    return false;
}

// Simple JSON integer parsing
int getIntValue(const std::string& json, const std::string& key, int defaultVal = 0) {
    std::string searchKey = "\"" + key + "\"";
    size_t keyPos = json.find(searchKey);
    if (keyPos == std::string::npos) return defaultVal;

    size_t colonPos = json.find(":", keyPos);
    if (colonPos == std::string::npos) return defaultVal;

    std::string afterColon = trim(json.substr(colonPos + 1));
    size_t endPos = 0;
    for (size_t i = 0; i < afterColon.length(); i++) {
        if (!std::isdigit(afterColon[i]) && afterColon[i] != '-') {
            endPos = i;
            break;
        }
    }
    if (endPos == 0) endPos = afterColon.length();

    try {
        return std::stoi(afterColon.substr(0, endPos));
    } catch (...) {
        return defaultVal;
    }
}

// Parse action field
CommandType parseActionType(const std::string& json) {
    std::string action = getStringValue(json, "action");
    std::transform(action.begin(), action.end(), action.begin(), ::tolower);

    if (action == "start") return CommandType::START;
    if (action == "stop") return CommandType::STOP;
    if (action == "pause") return CommandType::PAUSE;
    if (action == "resume") return CommandType::RESUME;
    if (action == "sysinfo") return CommandType::SYSINFO;
    if (action == "quit") return CommandType::QUIT;

    return CommandType::UNKNOWN;
}

// Check if JSON is valid (basic check)
bool isValidJson(const std::string& json) {
    std::string trimmed = trim(json);
    if (trimmed.empty()) return false;
    if (trimmed.front() != '{' || trimmed.back() != '}') return false;
    return trimmed.find("\"action\"") != std::string::npos;
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
    std::string json = trim(input);

    if (!isValidJson(json)) {
        return std::nullopt;
    }

    Command cmd;
    cmd.type = parseActionType(json);
    cmd.rawAction = getStringValue(json, "action");

    if (cmd.type == CommandType::START) {
        // Parse config if present
        // Look for config object
        size_t configStart = json.find("\"config\"");
        if (configStart != std::string::npos) {
            size_t objStart = json.find("{", configStart);
            size_t objEnd = json.find("}", objStart);
            if (objStart != std::string::npos && objEnd != std::string::npos) {
                std::string configJson = json.substr(objStart, objEnd - objStart + 1);
                cmd.config.resolution = getStringValue(configJson, "resolution");
                cmd.config.fps = getIntValue(configJson, "fps", 60);
                cmd.config.savePath = getStringValue(configJson, "savePath");
                cmd.config.separateAudio = getBoolValue(configJson, "separateAudio");
                cmd.config.remuxToMp4 = getBoolValue(configJson, "remuxToMp4");
            }
        }
    }

    return cmd;
}

std::string createStatusResponse(const std::string& state) {
    std::ostringstream oss;
    oss << "{\"type\":\"status\",\"state\":\"" << jsonEscape(state) << "\"}";
    return oss.str();
}

std::string createSysInfoResponse(int screenWidth, int screenHeight, int refreshRate) {
    std::ostringstream oss;
    oss << "{\"type\":\"sysinfo\",\"data\":{";
    oss << "\"screenWidth\":" << screenWidth << ",";
    oss << "\"screenHeight\":" << screenHeight << ",";
    oss << "\"refreshRate\":" << refreshRate << "}}";
    return oss.str();
}

std::string createFinishResponse(const std::string& videoPath,
                                 const std::string& actionsPath,
                                 const std::string& movementsPath) {
    std::ostringstream oss;
    oss << "{\"type\":\"finish\",";
    oss << "\"videoPath\":\"" << jsonEscape(videoPath) << "\",";
    oss << "\"actionsPath\":\"" << jsonEscape(actionsPath) << "\",";
    oss << "\"movementsPath\":\"" << jsonEscape(movementsPath) << "\"}";
    return oss.str();
}

std::string createErrorResponse(const std::string& message) {
    std::ostringstream oss;
    oss << "{\"type\":\"error\",\"msg\":\"" << jsonEscape(message) << "\"}";
    return oss.str();
}
