#pragma once

#include <string>
#include <vector>

namespace j2k {

// Canonical lists of Python modules intentionally ported/replaced in C++.
const std::vector<std::string>& python_module_ports_runtime();
const std::vector<std::string>& python_module_ports_analysis();
const std::vector<std::string>& python_module_ports_tools();

const std::vector<std::string>& python_module_port_names();
std::string describe_python_module_ports();

}  // namespace j2k
