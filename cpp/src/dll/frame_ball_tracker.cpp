#include "j2k/dll/frame_ball_tracker.hpp"

#include <algorithm>
#include <cmath>

namespace j2k::dll {

namespace {

/// Motion search uses the same center crop as before; gray buffers only store this ROI.
inline void motion_roi_bounds(int w, int h, int& x0, int& x1, int& y0, int& y1) {
    x0 = w / 6;
    x1 = w - w / 6;
    y0 = h / 8;
    y1 = h - h / 10;
}

}  // namespace

void FrameBallTracker::reset(int width, int height) {
    w_ = width;
    h_ = height;
    frame_index_ = 0;
    have_ball_ = false;
    ball_box_xyxy_.fill(0.f);
    last_seen_frame_ = 0;
    ema_cx_ = 0.f;
    ema_cy_ = 0.f;
    gray_prev_.clear();
    gray_work_.clear();
    int x0 = 0;
    int x1 = 0;
    int y0 = 0;
    int y1 = 0;
    motion_roi_bounds(std::max(1, width), std::max(1, height), x0, x1, y0, y1);
    const int rw = std::max(1, x1 - x0);
    const int rh = std::max(1, y1 - y0);
    gray_prev_.assign(static_cast<size_t>(rw) * static_cast<size_t>(rh), 0);
}

void FrameBallTracker::track(const std::uint8_t* bgr, int w, int h, int stride) {
    if (bgr == nullptr || w <= 8 || h <= 8 || w != w_ || h != h_) {
        if (w > 0 && h > 0 && (w != w_ || h != h_)) {
            reset(w, h);
        }
        return;
    }

    ++frame_index_;

    int x0 = 0;
    int x1 = 0;
    int y0 = 0;
    int y1 = 0;
    motion_roi_bounds(w, h, x0, x1, y0, y1);
    const int roi_w = x1 - x0;
    const int roi_h = y1 - y0;
    const size_t roi_n = static_cast<size_t>(roi_w) * static_cast<size_t>(roi_h);

    if (gray_work_.size() != roi_n) {
        gray_work_.assign(roi_n, 0);
    }

    for (int y = y0; y < y1; ++y) {
        const std::uint8_t* row = bgr + y * stride;
        std::uint8_t* gw = gray_work_.data() + static_cast<size_t>(y - y0) * static_cast<size_t>(roi_w);
        for (int x = x0; x < x1; ++x) {
            const std::uint8_t* px = row + x * 3;
            const int b = px[0];
            const int g = px[1];
            const int r = px[2];
            gw[x - x0] = static_cast<std::uint8_t>((r * 54 + g * 183 + b * 19) >> 8);
        }
    }

    if (gray_prev_.size() != gray_work_.size()) {
        gray_prev_ = gray_work_;
        have_ball_ = false;
        return;
    }

    /* Base 4; while tracking use 3 so brief low motion (dribble / slow pan) does not drop the box. */
    int motion_thr = have_ball_ ? motion_thr_tracking_ : motion_thr_searching_;
    if (square_motion_boost_frames_ > 0) {
        motion_thr = motion_thr_boosted_;
    }

    // Global strongest motion (fallback seed).
    int gxi = 0;
    int gyi = 0;
    int global_max_d = 0;
    for (int yi = 0; yi < roi_h; ++yi) {
        const std::uint8_t* c = gray_work_.data() + static_cast<size_t>(yi) * static_cast<size_t>(roi_w);
        const std::uint8_t* p = gray_prev_.data() + static_cast<size_t>(yi) * static_cast<size_t>(roi_w);
        for (int xi = 0; xi < roi_w; ++xi) {
            const int d = std::abs(static_cast<int>(c[xi]) - static_cast<int>(p[xi]));
            if (d > global_max_d) {
                global_max_d = d;
                gxi = xi;
                gyi = yi;
            }
        }
    }

    int seed_xi = gxi;
    int seed_yi = gyi;
    int max_d = global_max_d;
    bool used_local_seed = false;

    // When already tracking, prefer motion near the smoothed ball anchor so limbs do not steal the seed.
    if (have_ball_) {
        const int cx_i = std::clamp(static_cast<int>(std::lround(ema_cx_)) - x0, 0, roi_w - 1);
        const int cy_i = std::clamp(static_cast<int>(std::lround(ema_cy_)) - y0, 0, roi_h - 1);
        constexpr int kGateHalf = 120;
        int lxi = gxi;
        int lyi = gyi;
        int local_max_d = 0;
        const int rx0 = std::max(0, cx_i - kGateHalf);
        const int rx1 = std::min(roi_w - 1, cx_i + kGateHalf);
        const int ry0 = std::max(0, cy_i - kGateHalf);
        const int ry1 = std::min(roi_h - 1, cy_i + kGateHalf);
        for (int yi = ry0; yi <= ry1; ++yi) {
            const std::uint8_t* c = gray_work_.data() + static_cast<size_t>(yi) * static_cast<size_t>(roi_w);
            const std::uint8_t* p = gray_prev_.data() + static_cast<size_t>(yi) * static_cast<size_t>(roi_w);
            for (int xi = rx0; xi <= rx1; ++xi) {
                const int d = std::abs(static_cast<int>(c[xi]) - static_cast<int>(p[xi]));
                if (d > local_max_d) {
                    local_max_d = d;
                    lxi = xi;
                    lyi = yi;
                }
            }
        }
        /* Favor local motion near the EMA (65% vs global) so scoreboard/UI spikes do not steal seed. */
        if (local_max_d >= motion_thr && local_max_d * 100 >= global_max_d * 65) {
            seed_xi = lxi;
            seed_yi = lyi;
            max_d = local_max_d;
            used_local_seed = true;
        }
    }

    constexpr int kLocalRad = 42;
    const int x_lo = std::max(0, seed_xi - kLocalRad);
    const int x_hi = std::min(roi_w - 1, seed_xi + kLocalRad);
    const int y_lo = std::max(0, seed_yi - kLocalRad);
    const int y_hi = std::min(roi_h - 1, seed_yi + kLocalRad);

    double sum_w = 0.0;
    double acc_x = 0.0;
    double acc_y = 0.0;
    for (int yi = y_lo; yi <= y_hi; ++yi) {
        const std::uint8_t* c = gray_work_.data() + static_cast<size_t>(yi) * static_cast<size_t>(roi_w);
        const std::uint8_t* p = gray_prev_.data() + static_cast<size_t>(yi) * static_cast<size_t>(roi_w);
        for (int xi = x_lo; xi <= x_hi; ++xi) {
            const int d = std::abs(static_cast<int>(c[xi]) - static_cast<int>(p[xi]));
            if (d < 2) {
                continue;
            }
            const double dw = static_cast<double>(d) * static_cast<double>(d);
            sum_w += dw;
            acc_x += dw * (static_cast<double>(xi) + 0.5);
            acc_y += dw * (static_cast<double>(yi) + 0.5);
        }
    }

    float raw_cx = static_cast<float>(x0) + static_cast<float>(seed_xi) + 0.5f;
    float raw_cy = static_cast<float>(y0) + static_cast<float>(seed_yi) + 0.5f;
    if (sum_w >= 1.0) {
        raw_cx = static_cast<float>(x0) + static_cast<float>(acc_x / sum_w);
        raw_cy = static_cast<float>(y0) + static_cast<float>(acc_y / sum_w);
    }

    gray_prev_.swap(gray_work_);

    const int best_score = max_d;

    if (square_motion_boost_frames_ > 0) {
        --square_motion_boost_frames_;
    }

    if (best_score < motion_thr) {
        /* Hold last box for a stretch so release logic keeps ball context during shots. */
        constexpr int kHoldFrames = 96;
        if (have_ball_ && (frame_index_ - last_seen_frame_) < kHoldFrames) {
            return;
        }
        have_ball_ = false;
        return;
    }

    /* Slightly slower EMA = less jumpy box when motion centroid flickers frame to frame. */
    constexpr float kCenterEma = 0.20f;
    if (!have_ball_) {
        ema_cx_ = raw_cx;
        ema_cy_ = raw_cy;
    } else {
        ema_cx_ = ema_cx_ * (1.f - kCenterEma) + raw_cx * kCenterEma;
        ema_cy_ = ema_cy_ * (1.f - kCenterEma) + raw_cy * kCenterEma;
    }

    constexpr float kHalf = 18.f;
    const float cx = ema_cx_;
    const float cy = ema_cy_;
    const float bx1 = std::max(0.f, cx - kHalf);
    const float by1 = std::max(0.f, cy - kHalf);
    const float bx2 = std::min(static_cast<float>(w - 1), cx + kHalf);
    const float by2 = std::min(static_cast<float>(h - 1), cy + kHalf);

    ball_box_xyxy_[0] = bx1;
    ball_box_xyxy_[1] = by1;
    ball_box_xyxy_[2] = bx2;
    ball_box_xyxy_[3] = by2;
    have_ball_ = true;
    last_seen_frame_ = frame_index_;
}

void FrameBallTracker::seed_from_yolo(float cx, float cy, float r) {
    // Re-anchor EMA to YOLO-confirmed position so the tracker searches the right region
    // after a camera cut or scene change.  Only update if YOLO position diverges from the
    // current EMA by more than 2× the ball radius (avoid jitter on normal detections).
    const float dx = cx - ema_cx_;
    const float dy = cy - ema_cy_;
    const float dist2 = dx * dx + dy * dy;
    const float thr = 2.f * r;
    if (!have_ball_ || dist2 > thr * thr) {
        ema_cx_ = cx;
        ema_cy_ = cy;
        const float bx1 = cx - r;
        const float by1 = cy - r;
        ball_box_xyxy_[0] = bx1;
        ball_box_xyxy_[1] = by1;
        ball_box_xyxy_[2] = cx + r;
        ball_box_xyxy_[3] = cy + r;
        have_ball_ = true;
        last_seen_frame_ = frame_index_;
    }
}

void FrameBallTracker::set_motion_thresholds(int tracking, int searching, int boosted) {
    motion_thr_tracking_ = std::clamp(tracking, 1, 16);
    motion_thr_searching_ = std::clamp(searching, 1, 16);
    motion_thr_boosted_ = std::clamp(boosted, 1, 16);
}

void FrameBallTracker::on_square_held_frame() {
    /* Lower motion threshold briefly while shooting so the box does not drop out on slow motion. */
    square_motion_boost_frames_ = 6;
}

}  // namespace j2k::dll
