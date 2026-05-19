#pragma once

#include "j2k/dll/tracking_authority.hpp"

#include <string>

namespace j2k::dll {

bool parse_j2k_cue_tempo(const std::string& json_text, float& release_cue, float& tempo_speed);

bool read_j2k_config_file(const std::string& path, float& release_cue, float& tempo_speed);

/// Merges ``release_cue_value`` and ``tempo_speed`` into ``j2k_config.json`` (atomic replace).
bool write_j2k_cue_tempo_merge(const std::string& path, float release_cue, float tempo_speed);

struct J2kYoloJsonSettings {
    std::string onnx_relative;
    int imgsz = 1280;  ///< matches Ultralytics default export and current trained model
    float conf = 0.25f;
    float player_conf = 0.f;
    float stamina_conf = 0.f;
    float iou = 0.45f;
    /// If > 0, used for class 0 (basketball) only; other classes still use ``conf``.
    float ball_conf = 0.f;
    float player_ema_alpha = 0.40f;
    float ball_ema_alpha = 0.45f;
    int onnx_threads = 0;
};

/// Best-effort scan of ``j2k_config.json`` for ``yolo_onnx``, ``yolo_imgsz``, ``yolo_conf``, ``yolo_iou``.
bool read_j2k_yolo_json_file(const std::string& path, J2kYoloJsonSettings& out);

/// If ``cv_fadeaway_r2_high`` is present (0 or 1), sets ``fadeaway_when_r2_high`` (1 = R2 active means fadeaway).
bool read_j2k_cv_fadeaway_r2_high(const std::string& path, bool& fadeaway_when_r2_high);

/// Optional motion thresholds for ``FrameBallTracker``.
bool read_j2k_motion_thresholds(const std::string& path,
                                int& tracking,
                                int& searching,
                                int& boosted);

/// Best-effort parse of tracking authority thresholds from j2k_config.json.
/// Supports nested keys under tracking.{player|basketball|stamina} and
/// defaults from default_tracking_authority_config() when omitted.
bool read_j2k_tracking_json_file(const std::string& path, TrackingAuthorityConfig& out);

}  // namespace j2k::dll
