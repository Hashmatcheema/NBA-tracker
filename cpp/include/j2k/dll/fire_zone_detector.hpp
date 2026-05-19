#pragma once

#include <cstdint>

namespace j2k::dll {

struct FireZoneResult {
    bool inside_now = false;
    bool entered = false;
    bool release_now = false;
    bool visual_owner = false;
    const char* reason = "not_ready";
    float ball_cx = 0.f;
    float ball_cy = 0.f;
};

class FireZoneReleaseDetector {
public:
    void reset();

    FireZoneResult update(const float* ball_box_xyxy,
                          bool have_ball,
                          const float* fire_zone_xyxy,
                          bool have_zone,
                          bool square_held,
                          bool shot_active,
                          bool shot_type_locked,
                          bool release_fired_this_hold);

private:
    bool prev_inside_ = false;
    bool has_fired_ = false;
    bool visual_owner_ = false;
    /// Consecutive frames with ball centroid inside the zone (handles jitter without a clean edge).
    int inside_streak_ = 0;
};

}  // namespace j2k::dll
