#pragma once

#include "j2k/dll/yolo_onnx.hpp"

#include <array>
#include <map>
#include <string>
#include <vector>

namespace j2k::dll {

struct TrackClassConfig {
    float min_conf = 0.25f;
    float create_conf = 0.35f;
    float keep_conf = 0.25f;
    int max_missed_frames = 8;
    float smoothing_alpha = 0.45f;
    float max_jump_px = 220.f;
    float max_velocity_px = 240.f;
    float min_box_area_norm = 0.00005f;
    float max_box_area_norm = 0.35f;
    float min_aspect = 0.25f;  // width / height
    float max_aspect = 4.0f;
    int confirm_frames = 2;
    float region_x_min = 0.0f;
    float region_x_max = 1.0f;
    float region_y_min = 0.0f;
    float region_y_max = 1.0f;
    float max_distance_from_player_px = 260.f;
    bool require_near_player_when_unstable = false;
    bool require_player_association = false;
    bool freeze_identity_during_shot = false;
    /// Consecutive YOLO frames with a valid detection required before spawning a new track.
    /// Guards against single-frame false positives creating ghost tracks. Default 1 = immediate.
    int min_pre_active_frames = 1;
};

struct TrackingAuthorityConfig {
    TrackClassConfig player;
    TrackClassConfig ball;
    TrackClassConfig stamina;
};

struct AuthorityTrackState {
    bool exists = false;
    bool visible = false;
    bool predicted = false;
    bool lost = true;
    bool is_reliable = false;
    bool near_player = false;
    bool in_valid_shot_region = false;
    int track_id = 0;
    int associated_player_id = 0;
    int missed_frames = 0;
    int track_age = 0;
    int last_seen_frame = -1;
    float confidence = 0.f;
    float identity_confidence = 0.f;
    float alignment_score = 0.f;
    std::array<float, 4> raw_box{};
    std::array<float, 4> smooth_box{};
    std::array<float, 4> predicted_box{};
    std::array<float, 2> velocity{};
    std::string accepted_reason;
    std::string rejected_reason;
};

struct AuthorityOutput {
    AuthorityTrackState player;
    AuthorityTrackState ball;
    AuthorityTrackState stamina;
    std::map<std::string, std::map<std::string, int>> rejections_by_class;
    std::map<std::string, int> identity_switches_by_class;
};

TrackingAuthorityConfig default_tracking_authority_config();

class TrackingAuthority {
public:
    TrackingAuthority();

    void reset(int frame_w, int frame_h);
    void set_config(const TrackingAuthorityConfig& cfg);
    const TrackingAuthorityConfig& config() const { return cfg_; }

    void update(const std::vector<DetectionCandidate>& player_candidates,
                const std::vector<DetectionCandidate>& ball_candidates,
                const std::vector<DetectionCandidate>& stamina_candidates,
                bool detections_fresh,
                bool shot_active,
                int frame_index,
                AuthorityOutput& out_state);

private:
    struct InternalTrack {
        int cls = -1;
        bool active = false;
        int track_id = 0;
        int missed = 0;
        int age = 0;
        int last_seen = -1;
        float confidence = 0.f;
        float identity_confidence = 0.f;
        std::array<float, 4> raw{};
        std::array<float, 4> smooth{};
        std::array<float, 4> predicted{};
        std::array<float, 2> vel{};
        int pending_switch_frames = 0;
        std::array<float, 4> pending_switch_box{};
        /// Consecutive YOLO detections toward pre-activation confirmation (not-yet-active track).
        int pre_active_frames = 0;
        std::array<float, 4> pre_active_box{};
        std::string accepted_reason;
        std::string rejected_reason;
    };

    TrackingAuthorityConfig cfg_{};
    int frame_w_ = 0;
    int frame_h_ = 0;
    int next_track_id_ = 1;
    InternalTrack* player_ = nullptr;
    InternalTrack* ball_ = nullptr;
    InternalTrack* stamina_ = nullptr;
    std::array<InternalTrack, 3> tracks_{};
    std::map<std::string, std::map<std::string, int>> rejections_by_class_{};
    std::map<std::string, int> identity_switches_by_class_{};
};

}  // namespace j2k::dll
