#include "j2k/config.hpp"

#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>

namespace j2k {

namespace {

std::string read_text_file(const std::string& file_path) {
    std::ifstream in(file_path, std::ios::in | std::ios::binary);
    if (!in.is_open()) {
        throw std::runtime_error("Failed to open config file: " + file_path);
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

std::string trim_quotes(std::string value) {
    while (!value.empty() &&
           (value.front() == ' ' || value.front() == '\n' || value.front() == '\r' || value.front() == '\t')) {
        value.erase(value.begin());
    }
    while (!value.empty() &&
           (value.back() == ' ' || value.back() == '\n' || value.back() == '\r' || value.back() == '\t')) {
        value.pop_back();
    }
    if (value.size() >= 2 && value.front() == '"' && value.back() == '"') {
        value = value.substr(1, value.size() - 2);
    }
    return value;
}

std::string json_value(const std::string& text, const std::string& key, const std::string& fallback) {
    const std::string token = "\"" + key + "\"";
    const std::size_t pos = text.find(token);
    if (pos == std::string::npos) {
        return fallback;
    }
    const std::size_t colon = text.find(':', pos + token.size());
    if (colon == std::string::npos) {
        return fallback;
    }
    std::size_t end = text.find('\n', colon + 1);
    if (end == std::string::npos) {
        end = text.size();
    }
    std::string raw = text.substr(colon + 1, end - (colon + 1));
    const std::size_t comma = raw.find(',');
    if (comma != std::string::npos) {
        raw = raw.substr(0, comma);
    }
    return trim_quotes(raw);
}

bool json_bool(const std::string& text, const std::string& key, bool fallback) {
    const std::string token = "\"" + key + "\"";
    const std::size_t pos = text.find(token);
    if (pos == std::string::npos) {
        return fallback;
    }
    const std::size_t colon = text.find(':', pos + token.size());
    if (colon == std::string::npos) {
        return fallback;
    }
    std::size_t end = text.find('\n', colon + 1);
    if (end == std::string::npos) {
        end = text.size();
    }
    std::string raw = text.substr(colon + 1, end - (colon + 1));
    const std::size_t comma = raw.find(',');
    if (comma != std::string::npos) {
        raw = raw.substr(0, comma);
    }
    raw = trim_quotes(raw);
    if (raw == "true" || raw == "1") {
        return true;
    }
    if (raw == "false" || raw == "0") {
        return false;
    }
    return fallback;
}

}  // namespace

AppConfig load_config_file(const std::string& file_path) {
    const std::string text = read_text_file(file_path);
    AppConfig cfg{};
    cfg.module.module_path = json_value(text, "module_path", "");
    cfg.module.module_name = json_value(text, "module_name", "");
    cfg.module.exported_function_name = json_value(text, "exported_function_name", "");
    cfg.module.config_path = json_value(text, "config_path", "");
    cfg.module.enable_module_loading = json_bool(text, "enable_module_loading", false);
    return cfg;
}

}  // namespace j2k
