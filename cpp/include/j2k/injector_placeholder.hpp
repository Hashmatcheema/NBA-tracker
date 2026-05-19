#pragma once

#include <string>

#include "j2k/config.hpp"

namespace j2k {

struct ModuleLoadResult {
    bool ok = false;
    std::string message;
};

// Safe placeholder only.
// This does not inject anything and does not load modules into remote processes.
ModuleLoadResult validate_module_loading_inputs(const ModuleConfig& config);

}  // namespace j2k
