#include "j2k/runtime.hpp"

#include <algorithm>

namespace j2k {

void ShotRuntime::begin_shot(ShotType shot_type) {
    shot_type_ = shot_type;
    state_ = RhythmState::Hold;
    hold_f_ = 0;
    flick_frames_remaining_ = 0;
    flick_val_ = 100.0f;
}

RuntimeOutput ShotRuntime::tick(bool square_held, bool release_authorized) {
    RuntimeOutput out{};

    if (!square_held) {
        reset();
        out.state = state_;
        out.hold_f = hold_f_;
        out.stick_1_y = 0.0f;
        return out;
    }

    if (state_ == RhythmState::Idle) {
        begin_shot(shot_type_);
    }

    if (state_ == RhythmState::Hold) {
        out.stick_1_y = hold_value_for_shot(shot_type_);
        hold_f_ += 1;
        if (release_authorized) {
            state_ = RhythmState::Flick;
            flick_frames_remaining_ = 3;
            out.stick_1_y = 100.0f;
        }
    } else if (state_ == RhythmState::Flick) {
        out.stick_1_y = 100.0f;
        flick_frames_remaining_ = std::max(0, flick_frames_remaining_ - 1);
        if (flick_frames_remaining_ <= 0) {
            state_ = RhythmState::Idle;
            hold_f_ = 0;
            out.stick_1_y = 0.0f;
        }
    }

    out.state = state_;
    out.hold_f = hold_f_;
    return out;
}

void ShotRuntime::reset() {
    state_ = RhythmState::Idle;
    hold_f_ = 0;
    flick_frames_remaining_ = 0;
}

float ShotRuntime::hold_value_for_shot(ShotType shot_type) {
    switch (shot_type) {
        case ShotType::Fadeaway:
            return -100.0f;
        case ShotType::Normal:
        case ShotType::Nodip:
        default:
            return 100.0f;
    }
}

}  // namespace j2k
