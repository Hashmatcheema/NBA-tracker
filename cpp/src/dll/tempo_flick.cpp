#include "j2k/dll/tempo_flick.hpp"

#include <algorithm>

namespace j2k::dll {

void UniversalTempoFlick::start(int flick_sign, float tempo_speed) {
    active_ = true;
    start_t_ = std::chrono::steady_clock::now();

    tempo_speed = std::max(0.0f, std::min(100.0f, tempo_speed));
    constexpr double kMinMs = 20.0;
    constexpr double kMaxMs = 120.0;
    flick_ms_ = kMinMs + (static_cast<double>(tempo_speed) / 100.0) * (kMaxMs - kMinMs);

    flick_value_ = (flick_sign > 0) ? 100.0f : -100.0f;
}

std::pair<float, bool> UniversalTempoFlick::update() {
    if (!active_) {
        return {0.f, false};
    }
    const auto now = std::chrono::steady_clock::now();
    const double elapsed_ms =
        std::chrono::duration<double, std::milli>(now - start_t_).count();
    if (elapsed_ms < flick_ms_) {
        return {flick_value_, true};
    }
    active_ = false;
    flick_value_ = 0.f;
    return {0.f, false};
}

void UniversalTempoFlick::cancel() {
    active_ = false;
    flick_value_ = 0.f;
}

}  // namespace j2k::dll
