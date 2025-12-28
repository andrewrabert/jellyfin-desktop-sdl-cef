#include "settings.h"
#include <fstream>
#include <sstream>
#include <cstdlib>
#include <sys/stat.h>

Settings& Settings::instance() {
    static Settings instance;
    return instance;
}

std::string Settings::getConfigPath() {
    std::string config_dir;

    const char* xdg_config = std::getenv("XDG_CONFIG_HOME");
    if (xdg_config && xdg_config[0]) {
        config_dir = xdg_config;
    } else {
        const char* home = std::getenv("HOME");
        if (home) {
            config_dir = std::string(home) + "/.config";
        } else {
            config_dir = "/tmp";
        }
    }

    config_dir += "/jellyfin-desktop-cef";
    mkdir(config_dir.c_str(), 0755);

    return config_dir + "/settings.json";
}

bool Settings::load() {
    std::ifstream file(getConfigPath());
    if (!file.is_open()) {
        return false;
    }

    std::stringstream buffer;
    buffer << file.rdbuf();
    std::string content = buffer.str();

    // Simple JSON parsing for serverUrl
    size_t pos = content.find("\"serverUrl\"");
    if (pos != std::string::npos) {
        pos = content.find(':', pos);
        if (pos != std::string::npos) {
            pos = content.find('"', pos);
            if (pos != std::string::npos) {
                size_t end = content.find('"', pos + 1);
                if (end != std::string::npos) {
                    server_url_ = content.substr(pos + 1, end - pos - 1);
                }
            }
        }
    }

    return true;
}

bool Settings::save() {
    std::ofstream file(getConfigPath());
    if (!file.is_open()) {
        return false;
    }

    file << "{\n";
    file << "  \"serverUrl\": \"" << server_url_ << "\"\n";
    file << "}\n";

    return true;
}
