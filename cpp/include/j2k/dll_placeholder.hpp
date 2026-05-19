#pragma once

#include <string>

#include "j2k/config.hpp"

namespace j2k {

// Safe placeholder only.
// No injection or process memory operations are implemented.
std::string describe_dll_placeholder_contract(const ModuleConfig& config);

}  // namespace j2k
