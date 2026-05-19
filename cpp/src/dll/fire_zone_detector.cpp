#include "j2k/dll/fire_zone_detector.hpp"

#include <algorithm>
#include <cstdlib>
namespace j2k::dll {

void FireZoneReleaseDetector::reset() {
    prev_inside_ = false;
    has_fired_ = false;
    visual_owner_ = false;
    inside_streak_ = 0;
}

FireZoneResult FireZoneReleaseDetector::update(const float* ball_box_xyxy,
                                               bool have_ball,
                                               const float* zone_xyxy,
                                               bool have_zone,
                                               bool square_held,
                                               bool shot_active,
                                               bool shot_type_locked,
                                               bool release_fired_this_hold) {
    FireZoneResult r;
    r.visual_owner = visual_owner_;

    if (!square_held) {
        prev_inside_ = false;
        r.reason = "square_not_held";
        return r;
    }
    if (!shot_active) {
        prev_inside_ = false;
        inside_streak_ = 0;
        r.reason = "shot_not_active";
        return r;
    }
    if (release_fired_this_hold) {
        r.reason = "already_fired_this_hold";
        return r;
    }
    if (!shot_type_locked) {
        r.reason = "shot_type_not_locked";
        return r;
    }
    if (!have_ball || ball_box_xyxy == nullptr) {
        prev_inside_ = false;
        r.reason = "no_ball";
        return r;
    }
    if (!have_zone || zone_xyxy == nullptr) {
        prev_inside_ = false;
        r.reason = "no_fire_zone";
        return r;
    }

    const float bx1 = ball_box_xyxy[0];
    const float by1 = ball_box_xyxy[1];
    const float bx2 = ball_box_xyxy[2];
    const float by2 = ball_box_xyxy[3];
    const float fx1 = zone_xyxy[0];
    const float fy1 = zone_xyxy[1];
    const float fx2 = zone_xyxy[2];
    const float fy2 = zone_xyxy[3];

    const float ball_cx = 0.5f * (bx1 + bx2);
    const float ball_cy = 0.5f * (by1 + by2);
    r.ball_cx = ball_cx;
    r.ball_cy = ball_cy;

    const bool inside_now =
        (ball_cx >= fx1 && ball_cx <= fx2 && ball_cy >= fy1 && ball_cy <= fy2);
    const bool entered = inside_now && !prev_inside_;

    if (inside_now) {
        inside_streak_ = std::min(inside_streak_ + 1, 10000);
    } else {
        inside_streak_ = 0;
    }

    r.inside_now = inside_now;
    r.entered = entered;

    if (entered) {
        visual_owner_ = true;
        r.visual_owner = true;
    }

    // Edge trigger, or short dwell if the ball centroid jitters in/out and never produces a clean edge.
    constexpr int kInsideDwellFrames = 3;
    const bool dwell_release = !has_fired_ && inside_streak_ >= kInsideDwellFrames;

    if ((entered || dwell_release) && !has_fired_) {
        r.release_now = true;
        r.reason = entered ? "ball_entered_fire_zone" : "ball_dwell_in_fire_zone";
        has_fired_ = true;
    } else if (inside_now) {
        r.reason = "ball_inside_fire_zone_already_inside_or_fired";
    } else {
        r.reason = "ball_not_in_fire_zone";
    }

    prev_inside_ = inside_now;
    return r;
}

}  // namespace j2k::dll
