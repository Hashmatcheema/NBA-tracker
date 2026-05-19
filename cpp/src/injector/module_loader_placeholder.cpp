#include "j2k/injector_placeholder.hpp"

#include <algorithm>
#include <cctype>
#include <sstream>

namespace j2k {

namespace {

std::string trim(std::string value) {
    value.erase(value.begin(), std::find_if(value.begin(), value.end(), [](unsigned char ch) { return !std::isspace(ch); }));
    value.erase(std::find_if(value.rbegin(), value.rend(), [](unsigned char ch) { return !std::isspace(ch); }).base(), value.end());
    return value;
}

bool ends_with_dll(const std::string& value) {
    if (value.size() < 4) {
        return false;
    }
    std::string tail = value.substr(value.size() - 4);
    std::transform(tail.begin(), tail.end(), tail.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return tail == ".dll";
}

bool looks_like_export_name(const std::string& value) {
    if (value.empty()) {
        return false;
    }
    const auto is_alpha_or_underscore = [](unsigned char ch) { return std::isalpha(ch) || ch == '_'; };
    const auto is_alnum_or_underscore = [](unsigned char ch) { return std::isalnum(ch) || ch == '_'; };
    if (!is_alpha_or_underscore(static_cast<unsigned char>(value.front()))) {
        return false;
    }
    for (std::size_t i = 1; i < value.size(); ++i) {
        if (!is_alnum_or_underscore(static_cast<unsigned char>(value[i]))) {
            return false;
        }
    }
    return true;
}

}  // namespace

ModuleLoadResult validate_module_loading_inputs(const ModuleConfig& config) {
    if (!config.enable_module_loading) {
        return {true, "Module loading disabled by config (safe placeholder mode)."};
    }

    const std::string module_path = trim(config.module_path);
    const std::string module_name = trim(config.module_name);
    const std::string exported_function_name = trim(config.exported_function_name);
    const std::string config_path = trim(config.config_path);

    if (module_path.empty()) {
        return {false, "module_path is required when enable_module_loading=true."};
    }
    if (module_name.empty()) {
        return {false, "module_name is required when enable_module_loading=true."};
    }
    if (exported_function_name.empty()) {
        return {false, "exported_function_name is required when enable_module_loading=true."};
    }
    if (!ends_with_dll(module_path)) {
        return {false, "module_path must end with .dll (placeholder contract check)."};
    }
    if (!looks_like_export_name(exported_function_name)) {
        return {
            false,
            "exported_function_name must look like a C/C++ symbol (letters/digits/underscore, cannot start with a digit).",
        };
    }
    if (module_path == "C:/path/to/module.dll" || module_name == "example_module" ||
        exported_function_name == "InitializeModule") {
        return {
            false,
            "Config still contains example placeholder values. Replace module_path/module_name/exported_function_name first.",
        };
    }

    std::ostringstream ok;
    ok << "Inputs validated in safe placeholder mode.\n";
    ok << "  module_name=" << module_name << "\n";
    ok << "  module_path=" << module_path << "\n";
    ok << "  exported_function_name=" << exported_function_name << "\n";
    ok << "  config_path=" << (config_path.empty() ? "<optional, unset>" : config_path) << "\n";
    ok << "No injection or remote process operations are implemented in this build.";
    return {true, ok.str()};
}

}  // namespace j2k
