#include "j2k/python_module_ports.hpp"

namespace j2k {

const std::vector<std::string>& python_module_ports_analysis() {
    static const std::vector<std::string> kAnalysisPorts = {
        "j2k_dataset_audit",
        "j2k_detection_constants",
        "j2k_feedback",
        "j2k_feedback_video_test",
        "j2k_reference_video_calibrate",
        "j2k_selftest",
        "j2k_shooting_stress",
        "j2k_stamina_tracker",
        "j2k_video_apex_test",
        "j2k_video_replay_deterministic",
    };
    return kAnalysisPorts;
}

}  // namespace j2k
