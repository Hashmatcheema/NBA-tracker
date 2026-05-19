#include "j2k/python_module_ports.hpp"

namespace j2k {

const std::vector<std::string>& python_module_ports_runtime() {
    static const std::vector<std::string> kRuntimePorts = {
        "j2k_adaptive_timing",
        "j2k_frame_policy",
        "j2k_frame_sequence",
        "j2k_live_model",
        "j2k_memory_constants",
        "j2k_nometer_detector",
        "j2k_nometer_overlay",
        "j2k_paths",
        "j2k_release_intelligence",
        "j2k_runtime_tuning",
        "j2k_settings",
        "j2k_shot_memory",
    };
    return kRuntimePorts;
}

}  // namespace j2k
