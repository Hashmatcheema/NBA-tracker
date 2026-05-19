#include "j2k/python_module_ports.hpp"

#include <sstream>

namespace j2k {

const std::vector<std::string>& python_module_port_names() {
    static std::vector<std::string> kPorts;
    static bool initialized = false;
    if (!initialized) {
        initialized = true;
        const auto& runtime_ports = python_module_ports_runtime();
        const auto& analysis_ports = python_module_ports_analysis();
        const auto& tools_ports = python_module_ports_tools();
        kPorts.insert(kPorts.end(), runtime_ports.begin(), runtime_ports.end());
        kPorts.insert(kPorts.end(), analysis_ports.begin(), analysis_ports.end());
        kPorts.insert(kPorts.end(), tools_ports.begin(), tools_ports.end());
    }
    return kPorts;
}

std::string describe_python_module_ports() {
    const auto& ports = python_module_port_names();
    std::ostringstream out;
    out << "C++ module port registry active (" << ports.size() << " modules): ";
    for (std::size_t i = 0; i < ports.size(); ++i) {
        out << ports[i];
        if (i + 1 < ports.size()) {
            out << ", ";
        }
    }
    return out.str();
}

}  // namespace j2k
