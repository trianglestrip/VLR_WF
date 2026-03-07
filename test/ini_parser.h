// ============================================================================
// Simple INI Parser for VLR_WF
// ============================================================================

#pragma once

#include <string>
#include <map>
#include <fstream>
#include <sstream>
#include <algorithm>

class INIParser {
public:
    bool load(const std::string& filename) {
        std::ifstream file(filename);
        if (!file.is_open()) {
            return false;
        }

        std::string line;
        std::string currentSection;

        while (std::getline(file, line)) {
            line = trim(line);
            
            if (line.empty() || line[0] == '#' || line[0] == ';') {
                continue;
            }

            if (line[0] == '[' && line[line.length() - 1] == ']') {
                currentSection = line.substr(1, line.length() - 2);
                continue;
            }

            size_t pos = line.find('=');
            if (pos != std::string::npos) {
                std::string key = trim(line.substr(0, pos));
                std::string value = trim(line.substr(pos + 1));
                
                if (value[0] == '#' || value[0] == ';') {
                    size_t commentPos = value.find_first_of("#;");
                    value = trim(value.substr(0, commentPos));
                }
                
                std::string fullKey = currentSection.empty() ? key : currentSection + "." + key;
                data[fullKey] = value;
            }
        }

        return true;
    }

    std::string getString(const std::string& section, const std::string& key, const std::string& defaultValue = "") const {
        std::string fullKey = section + "." + key;
        auto it = data.find(fullKey);
        return (it != data.end()) ? it->second : defaultValue;
    }

    int getInt(const std::string& section, const std::string& key, int defaultValue = 0) const {
        std::string value = getString(section, key);
        if (value.empty()) return defaultValue;
        try {
            return std::stoi(value);
        } catch (...) {
            return defaultValue;
        }
    }

    float getFloat(const std::string& section, const std::string& key, float defaultValue = 0.0f) const {
        std::string value = getString(section, key);
        if (value.empty()) return defaultValue;
        try {
            return std::stof(value);
        } catch (...) {
            return defaultValue;
        }
    }

    bool getBool(const std::string& section, const std::string& key, bool defaultValue = false) const {
        std::string value = getString(section, key);
        if (value.empty()) return defaultValue;
        std::transform(value.begin(), value.end(), value.begin(), ::tolower);
        return (value == "true" || value == "1" || value == "yes" || value == "on");
    }

private:
    std::map<std::string, std::string> data;

    static std::string trim(const std::string& str) {
        size_t start = 0;
        size_t end = str.length();
        
        while (start < end && std::isspace(str[start])) {
            ++start;
        }
        
        while (end > start && std::isspace(str[end - 1])) {
            --end;
        }
        
        return str.substr(start, end - start);
    }
};
