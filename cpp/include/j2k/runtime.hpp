#pragma once

namespace j2k {

enum class ShotType {
    Normal = 0,
    Fadeaway = 1,
    Nodip = 2,
};

enum class RhythmState {
    Idle = 0,
    Hold = 1,
    Flick = 2,
};

struct RuntimeOutput {
    float stick_1_y = 0.0f;
    RhythmState state = RhythmState::Idle;
    int hold_f = 0;
};

class ShotRuntime {
public:
    ShotRuntime() = default;

    void begin_shot(ShotType shot_type);
    RuntimeOutput tick(bool square_held, bool release_authorized);
    void reset();

private:
    static float hold_value_for_shot(ShotType shot_type);

    RhythmState state_ = RhythmState::Idle;
    ShotType shot_type_ = ShotType::Normal;
    int hold_f_ = 0;
    float flick_val_ = 100.0f;
    int flick_frames_remaining_ = 0;
};

}  // namespace j2k
