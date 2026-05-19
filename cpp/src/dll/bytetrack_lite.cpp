#include "j2k/dll/bytetrack_lite.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace j2k::dll {

void ByteTrackLite::reset() {
    have_track_ = false;
    missed_ = 0;
    track_box_.fill(0.f);
    prev_track_box_.fill(0.f);
    track_score_ = 0.f;
}

float ByteTrackLite::iou_xyxy(const std::array<float, 4>& a, const std::array<float, 4>& b) {
    const float xx1 = std::max(a[0], b[0]);
    const float yy1 = std::max(a[1], b[1]);
    const float xx2 = std::min(a[2], b[2]);
    const float yy2 = std::min(a[3], b[3]);
    const float iw = std::max(0.f, xx2 - xx1);
    const float ih = std::max(0.f, yy2 - yy1);
    const float inter = iw * ih;
    const float aa = std::max(0.f, a[2] - a[0]) * std::max(0.f, a[3] - a[1]);
    const float bb = std::max(0.f, b[2] - b[0]) * std::max(0.f, b[3] - b[1]);
    const float uni = aa + bb - inter;
    if (uni <= 1e-6f) {
        return 0.f;
    }
    return inter / uni;
}

std::array<float, 4> ByteTrackLite::lerp_box(const std::array<float, 4>& a,
                                             const std::array<float, 4>& b,
                                             float alpha) {
    const float t = std::clamp(alpha, 0.f, 1.f);
    return {
        a[0] * (1.f - t) + b[0] * t,
        a[1] * (1.f - t) + b[1] * t,
        a[2] * (1.f - t) + b[2] * t,
        a[3] * (1.f - t) + b[3] * t,
    };
}

ByteTrackLite::UpdateResult ByteTrackLite::update(const std::vector<ByteTrackDetection>& detections) {
    UpdateResult out{};

    std::vector<const ByteTrackDetection*> high;
    std::vector<const ByteTrackDetection*> low;
    high.reserve(detections.size());
    low.reserve(detections.size());
    for (const auto& d : detections) {
        if (d.score >= cfg_.high_thresh) {
            high.push_back(&d);
        } else if (d.score >= cfg_.low_thresh) {
            low.push_back(&d);
        }
    }

    auto predict_box = track_box_;
    if (have_track_) {
        const float vx1 = track_box_[0] - prev_track_box_[0];
        const float vy1 = track_box_[1] - prev_track_box_[1];
        const float vx2 = track_box_[2] - prev_track_box_[2];
        const float vy2 = track_box_[3] - prev_track_box_[3];
        predict_box = {
            track_box_[0] + vx1,
            track_box_[1] + vy1,
            track_box_[2] + vx2,
            track_box_[3] + vy2,
        };
    }

    const ByteTrackDetection* best = nullptr;
    float best_metric = -std::numeric_limits<float>::infinity();
    bool matched_high = false;

    auto score_candidate = [&](const ByteTrackDetection* d) -> float {
        if (!have_track_) {
            return d->score;
        }
        const float iou = iou_xyxy(predict_box, d->box);
        return iou * 0.75f + d->score * 0.25f;
    };

    if (have_track_) {
        for (const auto* d : high) {
            const float iou = iou_xyxy(predict_box, d->box);
            if (iou < cfg_.match_iou_high) {
                continue;
            }
            const float metric = score_candidate(d);
            if (metric > best_metric) {
                best_metric = metric;
                best = d;
                matched_high = true;
            }
        }
        if (best == nullptr) {
            for (const auto* d : low) {
                const float iou = iou_xyxy(predict_box, d->box);
                if (iou < cfg_.match_iou_low) {
                    continue;
                }
                const float metric = score_candidate(d);
                if (metric > best_metric) {
                    best_metric = metric;
                    best = d;
                    matched_high = false;
                }
            }
        }
    }

    if (best == nullptr) {
        for (const auto* d : high) {
            const float metric = score_candidate(d);
            if (metric > best_metric) {
                best_metric = metric;
                best = d;
                matched_high = true;
            }
        }
    }

    if (best != nullptr) {
        prev_track_box_ = track_box_;
        if (!have_track_) {
            track_box_ = best->box;
            prev_track_box_ = best->box;
        } else {
            const float alpha = matched_high ? 0.72f : 0.50f;
            track_box_ = lerp_box(track_box_, best->box, alpha);
        }
        track_score_ = best->score;
        have_track_ = true;
        missed_ = 0;
        out.have_box = true;
        out.box = track_box_;
        out.score = track_score_;
        out.matched_this_frame = true;
        return out;
    }

    if (have_track_) {
        ++missed_;
        if (missed_ <= cfg_.max_missed) {
            out.have_box = true;
            out.box = track_box_;
            out.score = track_score_;
            out.matched_this_frame = false;
            return out;
        }
    }

    reset();
    return out;
}

}  // namespace j2k::dll
