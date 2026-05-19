#include "j2k/python_module_ports.hpp"

namespace j2k {

const std::vector<std::string>& python_module_ports_tools() {
    static const std::vector<std::string> kToolsPorts = {
        "j2k_annotations_names",
        "j2k_console",
        "j2k_console_listener",
        "j2k_console_listener_watchdog",
        "j2k_fadeaway_rhythm_smoke",
        "j2k_train_yolo",
        "j2k_vendor_bootstrap",
    };
    return kToolsPorts;
}

}  // namespace j2k
