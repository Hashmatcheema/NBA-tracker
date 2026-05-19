#include "j2k/dll_placeholder.hpp"

#include <sstream>

namespace j2k {

std::string describe_dll_placeholder_contract(const ModuleConfig& config) {
    std::ostringstream oss;
    oss << "DLL placeholder contract (safe mode)\n";
    oss << "  module_name=" << (config.module_name.empty() ? "<unset>" : config.module_name) << "\n";
    oss << "  module_path=" << (config.module_path.empty() ? "<unset>" : config.module_path) << "\n";
    oss << "  exported_function_name=" << (config.exported_function_name.empty() ? "<unset>" : config.exported_function_name) << "\n";
    oss << "  config_path=" << (config.config_path.empty() ? "<unset>" : config.config_path) << "\n";
    oss << "  enable_module_loading=" << (config.enable_module_loading ? "true" : "false") << "\n";
    oss << "\n";
    oss << "What this module does:\n";
    oss << "  - Reads and echoes module contract values.\n";
    oss << "  - Supports local config validation only.\n";
    oss << "\n";
    oss << "What this module does NOT do:\n";
    oss << "  - Process injection\n";
    oss << "  - Remote thread creation\n";
    oss << "  - WriteProcessMemory/manual mapping\n";
    oss << "  - Stealth/anti-cheat bypass behavior\n";
    return oss.str();
}

}  // namespace j2k
