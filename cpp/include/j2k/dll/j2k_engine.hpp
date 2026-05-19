#pragma once

#include "j2k/dll/fire_zone_detector.hpp"
#include "j2k/dll/frame_ball_tracker.hpp"
#include "j2k/dll/bytetrack_lite.hpp"
#include "j2k/dll/gcv_layout.hpp"
#include "j2k/dll/tracking_authority.hpp"
#include "j2k/dll/tempo_flick.hpp"
#include "j2k/dll/yolo_onnx.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>

namespace j2k::dll {

/// Tracking state machine — labels the current detection/lock quality for logging and
/// fail-safe gating. Transitions are updated each process_frame(); they do not affect
/// shot logic directly but enable precise failure attribution.
enum class TrackingState : std::uint8_t {
    kIdle = 0,          ///< not yet started
    kSeekingPlayer,     ///< no player lock; searching
    kPlayerLocked,      ///< player confirmed, ball not yet tracked
    kBallTracked,       ///< player + ball both tracked (normal dribble state)
    kShotActive,        ///< Square held — tracking in shot-critical mode
    kTempLostBall,      ///< player OK; ball briefly missing (carry active)
    kTempLostPlayer,    ///< player carry active (YOLO miss but stable cache present)
};

struct FrameTelemetry {
    double capture_ms = 0.0;
    double preprocess_ms = 0.0;
    double inference_ms = 0.0;
    double postprocess_ms = 0.0;
    double tracking_ms = 0.0;
    double visual_ms = 0.0;
    double total_frame_ms = 0.0;
    double fps = 0.0;
    bool player_locked = false;
    bool ball_locked = false;
    bool stamina_locked = false;
    int player_lost_frames = 0;
    int ball_lost_frames = 0;
    int stamina_lost_frames = 0;
    bool cuda_active = false;
    std::string provider;
    bool heavy_processed = false;
    int cadence_every_n = 1;
};

class J2kEngine {
public:
    void init(int width, int height, const char* script_dir_utf8);
    void shutdown();
    int process_frame(const std::uint8_t* bgr, int w, int h, int stride, std::array<std::uint8_t, kGcvSize>& gcv_io);

    /// Render the four overlay shapes (player rect, fire zone rect, stamina rect,
    /// ball circle) directly onto a writable BGR frame buffer. The shapes are
    /// decoded from the GCV bytes that `process_frame` just wrote. No labels,
    /// no Python-side drawing required.
    void draw_overlay(std::uint8_t* bgr, int w, int h, int stride,
                      const std::array<std::uint8_t, kGcvSize>& gcv) const;

    TrackingState tracking_state() const { return tracking_state_; }
    FrameTelemetry frame_telemetry() const { return telemetry_; }

private:
    void reload_config_throttled();

    void compute_player_stub(std::array<float, 4>& out_xyxy) const;
    void compute_fire_zone(const std::array<float, 4>& player_xyxy, std::array<float, 4>& zone_out) const;

    std::array<float, 4> last_stable_player_box_{};
    bool have_last_stable_player_ = false;
    int last_stable_player_seen_frame_ = 0;
    std::array<float, 4> pending_player_box_{};
    int pending_player_confirm_frames_ = 0;

    int width_ = 0;
    int height_ = 0;
    std::string script_dir_;
    int frame_n_ = 0;

    float release_cue_value_ = 50.f;
    float tempo_speed_ = 35.f;
    std::filesystem::file_time_type last_cfg_mtime_{};

    bool prev_square_held_ = false;
    bool shot_active_ = false;
    bool release_fired_this_hold_ = false;
    bool visual_owner_ = false;
    bool shot_type_locked_ = false;
    int square_held_frames_ = 0;
    int shot_id_ = 0;

    enum class ShotType : std::uint8_t { Normal, Fadeaway, Nodip };
    ShotType shot_type_ = ShotType::Normal;
    ShotType latched_windup_ = ShotType::Normal;
    /// From ``j2k_config.json`` ``cv_fadeaway_r2_high`` (default true): R2 "hot" selects fadeaway.
    bool fadeaway_when_r2_high_ = true;

    ShotType classify_shot_from_r2(int r2a, int r2b) const;

    FireZoneReleaseDetector fire_zone_;
    UniversalTempoFlick tempo_flick_;
    FrameBallTracker tracker_;
    ByteTrackLite player_bt_{ByteTrackLite::Config{0.40f, 0.12f, 0.16f, 0.08f, 14}};
    ByteTrackLite ball_bt_{ByteTrackLite::Config{0.22f, 0.05f, 0.12f, 0.06f, 10}};
    bool use_bytetrack_ = true;
    TrackingAuthority authority_{};
    AuthorityOutput authority_state_{};
    std::unique_ptr<YoloOnnxRunner> yolo_;

    // NoFire / diagnostics (falling edge)
    const char* last_visual_reason_ = "";
    bool last_inside_fire_zone_ = false;
    bool last_entered_fire_zone_ = false;
    std::array<float, 4> last_fire_zone_box_{};
    bool have_last_fire_zone_ = false;
    float last_ball_cx_ = 0.f;
    float last_ball_cy_ = 0.f;
    int last_ball_seen_frame_ = 0;
    std::array<float, 4> shot_locked_ball_box_{};
    bool have_shot_locked_ball_ = false;
    int shot_locked_ball_gap_frames_ = 0;
    std::array<float, 4> last_live_ball_box_{};
    std::array<float, 4> prev_live_ball_box_{};
    bool have_last_live_ball_ = false;
    bool have_prev_live_ball_ = false;
    int last_live_ball_frame_ = -1;
    // Separate YOLO-only carry state: optical-flow hold-ups must not pollute carry.
    std::array<float, 4> last_yolo_ball_box_{};
    bool have_last_yolo_ball_ = false;
    int last_yolo_ball_frame_ = -1;
    const char* last_nofire_hint_ = "";

    // Perf (simple ring for p95 approx — last 64 samples)
    static constexpr int kPerfRing = 64;
    double perf_ring_total_[kPerfRing]{};
    double perf_ring_yolo_[kPerfRing]{};
    double perf_ring_pre_[kPerfRing]{};
    double perf_ring_infer_[kPerfRing]{};
    double perf_ring_post_[kPerfRing]{};
    int perf_i_ = 0;
    void push_perf_sample(double total_ms, double yolo_ms, double pre_ms, double infer_ms, double post_ms);
    double approx_p95_total() const;
    double approx_p95(const double* ring) const;

    void log_perf_warn_if_needed(double total_ms,
                                 double yolo_ms,
                                 double track_ms,
                                 double visual_ms,
                                 bool square_held);
    bool should_process_heavy_frame();
    void record_frame_interval_ms();

    std::chrono::steady_clock::time_point last_frame_time_{};
    std::deque<double> frame_intervals_ms_{};
    int skipped_heavy_frames_ = 0;
    int yolo_every_n_ = 1;
    int yolo_every_n_runtime_ = 1;
    int heavy_backoff_frames_ = 0;
    std::atomic<bool> yolo_warmup_done_{true};
    std::thread yolo_warmup_thread_{};
    std::thread yolo_warmup_timeout_thread_{};
    double last_total_ms_ = 0.0;
    double last_yolo_ms_ = 0.0;
    int player_yolo_miss_frames_ = 0;
    int ball_yolo_miss_frames_ = 0;
    std::array<float, 4> last_stamina_box_{};
    bool have_last_stamina_ = false;
    int stamina_miss_frames_ = 0;
    int stamina_static_heavy_frames_ = 0;

    // Visual-only adaptive player overlay fit state (does not affect tracking output).
    mutable std::array<float, 4> last_overlay_player_box_{};
    mutable bool have_last_overlay_player_box_ = false;
    mutable int overlay_last_w_ = 0;
    mutable int overlay_last_h_ = 0;

    TrackingState tracking_state_ = TrackingState::kIdle;
    int tracking_state_frame_ = 0;  ///< frame_n_ when tracking_state_ last changed
    FrameTelemetry telemetry_{};
};

}  // namespace j2k::dll
