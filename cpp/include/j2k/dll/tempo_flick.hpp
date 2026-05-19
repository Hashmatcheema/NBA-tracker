#pragma once

#include <chrono>
#include <cstdint>

namespace j2k::dll {

class UniversalTempoFlick {
public:
    void start(int flick_sign, float tempo_speed);
    /// Returns (stick_value -100/0/+100, still_active)
    std::pair<float, bool> update();
    void cancel();

private:
    bool active_ = false;
    std::chrono::steady_clock::time_point start_t_{};
    double flick_ms_ = 40.0;
    float flick_value_ = 0.f;
};

}  // namespace j2k::dll
