#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace j2k::dll {

/// Center-crop motion ball tracker (fast differencing on the same ROI as before). Runs every frame;
/// Square-held frames get a short motion threshold boost so the box is less likely to drop during
/// shots. YOLO can replace this path later without changing the GCV contract.
class FrameBallTracker {
public:
    void reset(int width, int height);

    /// Updates EMA ball box from frame; always tracks when frame valid.
    void track(const std::uint8_t* bgr, int w, int h, int stride);

    /// While Square is held, temporarily lowers motion threshold (see track()).
    void on_square_held_frame();
    void set_motion_thresholds(int tracking, int searching, int boosted);

    bool have_ball() const { return have_ball_; }
    const std::array<float, 4>& ball_box() const { return ball_box_xyxy_; }
    int last_seen_frame() const { return last_seen_frame_; }

    /// Re-anchor the EMA to a YOLO-confirmed ball centre.  Call whenever YOLO detects a ball
    /// so that the optical-flow tracker follows the correct region after a camera cut.
    void seed_from_yolo(float cx, float cy, float r);

private:
    int w_ = 0;
    int h_ = 0;
    int frame_index_ = 0;
    bool have_ball_ = false;
    std::array<float, 4> ball_box_xyxy_{};
    int last_seen_frame_ = 0;
    int square_motion_boost_frames_ = 0;
    int motion_thr_tracking_ = 3;
    int motion_thr_searching_ = 4;
    int motion_thr_boosted_ = 2;

    /// EMA of motion anchor (image px); stabilizes box center frame-to-frame.
    float ema_cx_ = 0.f;
    float ema_cy_ = 0.f;

    std::vector<std::uint8_t> gray_prev_;
    std::vector<std::uint8_t> gray_work_;
};

}  // namespace j2k::dll
