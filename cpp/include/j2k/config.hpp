#pragma once

#include <string>

namespace j2k {

struct ModuleConfig {
    std::string module_path;
    std::string module_name;
    std::string exported_function_name;
    std::string config_path;
    bool enable_module_loading = false;
};

struct AppConfig {
    ModuleConfig module;
};

AppConfig load_config_file(const std::string& file_path);

}  // namespace j2k
