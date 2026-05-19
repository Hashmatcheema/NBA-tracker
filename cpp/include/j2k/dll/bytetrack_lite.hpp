#pragma once

#include <array>
#include <cstdint>
#include <vector>

namespace j2k::dll {

struct ByteTrackDetection {
    std::array<float, 4> box{};
    float score = 0.f;
};

class ByteTrackLite {
public:
    struct Config {
        float high_thresh = 0.45f;
        float low_thresh = 0.10f;
        float match_iou_high = 0.18f;
        float match_iou_low = 0.08f;
        int max_missed = 12;
    };

    struct UpdateResult {
        bool have_box = false;
        std::array<float, 4> box{};
        float score = 0.f;
        bool matched_this_frame = false;
    };

    explicit ByteTrackLite(const Config& cfg = Config{}) : cfg_(cfg) {}

    void reset();
    UpdateResult update(const std::vector<ByteTrackDetection>& detections);

private:
    static float iou_xyxy(const std::array<float, 4>& a, const std::array<float, 4>& b);
    static std::array<float, 4> lerp_box(const std::array<float, 4>& a,
                                         const std::array<float, 4>& b,
                                         float alpha);

    Config cfg_{};
    bool have_track_ = false;
    int missed_ = 0;
    std::array<float, 4> track_box_{};
    std::array<float, 4> prev_track_box_{};
    float track_score_ = 0.f;
};

}  // namespace j2k::dll
