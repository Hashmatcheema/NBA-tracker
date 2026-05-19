#include "j2k/dll/tracking_authority.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace j2k::dll {

namespace {

float box_width(const std::array<float, 4>& b) { return std::max(0.f, b[2] - b[0]); }
float box_height(const std::array<float, 4>& b) { return std::max(0.f, b[3] - b[1]); }
float box_area(const std::array<float, 4>& b) { return box_width(b) * box_height(b); }
float box_cx(const std::array<float, 4>& b) { return 0.5f * (b[0] + b[2]); }
float box_cy(const std::array<float, 4>& b) { return 0.5f * (b[1] + b[3]); }
float box_dist(const std::array<float, 4>& a, const std::array<float, 4>& b) {
    return std::hypot(box_cx(a) - box_cx(b), box_cy(a) - box_cy(b));
}

float box_iou(const std::array<float, 4>& a, const std::array<float, 4>& b) {
    const float x1 = std::max(a[0], b[0]);
    const float y1 = std::max(a[1], b[1]);
    const float x2 = std::min(a[2], b[2]);
    const float y2 = std::min(a[3], b[3]);
    const float iw = std::max(0.f, x2 - x1);
    const float ih = std::max(0.f, y2 - y1);
    const float inter = iw * ih;
    const float ua = box_area(a) + box_area(b) - inter;
    if (ua <= 1e-3f) {
        return 0.f;
    }
    return inter / ua;
}

std::array<float, 4> clamp_box(const std::array<float, 4>& b, int w, int h) {
    const float fw = static_cast<float>(std::max(1, w - 1));
    const float fh = static_cast<float>(std::max(1, h - 1));
    return {
        std::clamp(b[0], 0.f, fw),
        std::clamp(b[1], 0.f, fh),
        std::clamp(b[2], 0.f, fw),
        std::clamp(b[3], 0.f, fh),
    };
}

std::array<float, 4> lerp_box(const std::array<float, 4>& a,
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

const char* class_name(int cls) {
    if (cls == kYoloClsPlayer) {
        return "PLAYER";
    }
    if (cls == kYoloClsBasketball) {
        return "BALL";
    }
    return "STAMINA";
}

}  // namespace

TrackingAuthorityConfig default_tracking_authority_config() {
    TrackingAuthorityConfig cfg;

    // Decoded NMS exports compress score magnitudes — the old 0.52 create_conf was rejecting
    // real player detections that score 0.20-0.45.  Ghost-box suppression is now handled by
    // tighter stamina_box_plausible limits, score gates in run_multi, and region checks here.
    cfg.player.min_conf = 0.20f;           // floor — keep permissive; candidate gate in tracking handles quality
    cfg.player.create_conf = 0.32f;        // was 0.28 → slightly tighter to stop wrong-identity locks without blocking real detections
    cfg.player.keep_conf = 0.20f;          // keep permissive — track sustain should not drop on brief occlusion
    cfg.player.max_missed_frames = 18;    // was 12 — extended carry for fast cross-court movement
    cfg.player.smoothing_alpha = 0.65f;   // was 0.44 — snaps to new position faster on running
    cfg.player.max_jump_px = 320.f;       // was 260 — large_jump threshold = 176px; allows sprint
    cfg.player.max_velocity_px = 260.f;   // was 220 — allow fast cross-court movement
    cfg.player.min_box_area_norm = 0.0010f;
    cfg.player.max_box_area_norm = 0.18f;  // was 0.28 — rejects crowd sections/giant misdetections; close-up players ~6-17%
    cfg.player.min_aspect = 0.18f;
    cfg.player.max_aspect = 1.20f;         // was 1.25 → slightly tighter; EMA can blend to near-square so don't go below 1.20
    cfg.player.confirm_frames = 2;
    cfg.player.freeze_identity_during_shot = true;
    cfg.player.region_x_min = 0.06f;
    cfg.player.region_x_max = 0.94f;
    cfg.player.region_y_min = 0.10f;   // was 0.08 — player never in top 10%
    cfg.player.region_y_max = 0.96f;
    cfg.player.min_pre_active_frames = 2;  // 2 consecutive YOLO frames before creating track

    // Ball thresholds rebalanced for decoded NMS score range.
    // 0.52 create_conf was blocking real ball tracks — in decoded-NMS layout ball scores
    // commonly fall in 0.12-0.40 range due to score compression.  Ghost-ball suppression
    // is now handled by: (a) conf_ball score gate in run_multi post-NMS accumulation,
    // (b) ball_box_plausible geometry filter, (c) min_pre_active_frames=2 temporal gate,
    // (d) require_near_player_when_unstable proximity guard.
    cfg.ball.min_conf = 0.10f;
    cfg.ball.create_conf = 0.20f;
    cfg.ball.keep_conf = 0.10f;
    cfg.ball.max_missed_frames = 14;   // extended carry to bridge mid-dribble YOLO dropouts
    cfg.ball.smoothing_alpha = 0.50f;  // was 0.52
    cfg.ball.max_jump_px = 300.f;      // was 420 — basketball doesn't teleport 420px per YOLO frame
    cfg.ball.max_velocity_px = 320.f;  // was 420
    cfg.ball.min_box_area_norm = 0.00005f; // was 0.00001 — exclude sub-pixel noise blobs
    cfg.ball.max_box_area_norm = 0.010f;   // was 0.012
    cfg.ball.min_aspect = 0.30f;       // was 0.20 — more square-ish
    cfg.ball.max_aspect = 2.80f;       // was 3.20
    cfg.ball.confirm_frames = 2;       // was 1 — require 2 large-jump confirmations
    cfg.ball.freeze_identity_during_shot = true;
    cfg.ball.region_x_min = 0.04f;
    cfg.ball.region_x_max = 0.96f;
    cfg.ball.region_y_min = 0.22f;     // was 0.08 — top 22% = hoop zone; relaxed during shot
    cfg.ball.region_y_max = 0.96f;
    cfg.ball.require_near_player_when_unstable = true;  // was false — ghost balls far from player rejected
    cfg.ball.min_pre_active_frames = 2; // require 2 consecutive YOLO frames before spawning ball track

    cfg.stamina.min_conf = 0.20f;
    cfg.stamina.create_conf = 0.38f;
    cfg.stamina.keep_conf = 0.20f;
    cfg.stamina.max_missed_frames = 10;
    cfg.stamina.smoothing_alpha = 0.42f;
    cfg.stamina.max_jump_px = 200.f;    // was 120 — stamina bar moves with player; sprinting player moves ~260px/frame
    cfg.stamina.max_velocity_px = 80.f;
    cfg.stamina.min_box_area_norm = 0.00002f;
    // Tightened to match stamina_box_plausible max (60% × 6% of frame = 3.6% area max).
    // Old 0.030 allowed floor markings and scoreboard rows through as "giant stamina bars".
    cfg.stamina.max_box_area_norm = 0.012f;
    cfg.stamina.min_aspect = 2.5f;    // stamina bar is always wide-horizontal; was 1.20 (let in near-square blobs)
    cfg.stamina.max_aspect = 20.0f;   // was 16 — slightly relaxed upper bound
    cfg.stamina.confirm_frames = 3;
    cfg.stamina.region_x_min = 0.10f;
    cfg.stamina.region_x_max = 0.90f;
    cfg.stamina.region_y_min = 0.45f;
    cfg.stamina.region_y_max = 0.98f;
    cfg.stamina.max_distance_from_player_px = 200.f;  // was 220 — tighter association
    cfg.stamina.require_player_association = true;
    cfg.stamina.min_pre_active_frames = 2;  // require 2 consecutive YOLO frames

    return cfg;
}

TrackingAuthority::TrackingAuthority() : cfg_(default_tracking_authority_config()) {
    tracks_[0].cls = kYoloClsPlayer;
    tracks_[1].cls = kYoloClsBasketball;
    tracks_[2].cls = kYoloClsStamBar;
    player_ = &tracks_[0];
    ball_ = &tracks_[1];
    stamina_ = &tracks_[2];
}

void TrackingAuthority::reset(int frame_w, int frame_h) {
    frame_w_ = std::max(1, frame_w);
    frame_h_ = std::max(1, frame_h);
    next_track_id_ = 1;
    rejections_by_class_.clear();
    identity_switches_by_class_.clear();
    for (auto& t : tracks_) {
        t.active = false;
        t.track_id = 0;
        t.missed = 0;
        t.age = 0;
        t.last_seen = -1;
        t.confidence = 0.f;
        t.identity_confidence = 0.f;
        t.raw = {};
        t.smooth = {};
        t.predicted = {};
        t.vel = {};
        t.pending_switch_frames = 0;
        t.pending_switch_box = {};
        t.pre_active_frames = 0;
        t.pre_active_box = {};
        t.accepted_reason.clear();
        t.rejected_reason.clear();
    }
}

void TrackingAuthority::set_config(const TrackingAuthorityConfig& cfg) {
    cfg_ = cfg;
}

void TrackingAuthority::update(const std::vector<DetectionCandidate>& player_candidates,
                               const std::vector<DetectionCandidate>& ball_candidates,
                               const std::vector<DetectionCandidate>& stamina_candidates,
                               bool detections_fresh,
                               bool shot_active,
                               int frame_index,
                               AuthorityOutput& out_state) {
    auto normalize_reason = [&](int cls, const std::string& reason) -> std::string {
        if (reason == "jump_too_large" || reason == "iou_center_gate_failed") {
            return "too_far_from_last_box";
        }
        if (reason == "scale_change_too_large") {
            return "impossible_size_change";
        }
        if (reason == "aspect_out_of_bounds") {
            return "invalid_aspect_ratio";
        }
        if (reason == "not_associated_with_player") {
            return (cls == kYoloClsStamBar) ? "stamina_not_under_player" : "too_far_from_player";
        }
        if (reason == "too_far_from_player_unstable") {
            return "too_far_from_player";
        }
        if (reason == "region_out_of_bounds" || reason == "area_out_of_bounds" || reason == "invalid_box") {
            return "likely_false_positive";
        }
        return reason;
    };
    auto reject = [&](int cls, const std::string& reason) {
        ++rejections_by_class_[class_name(cls)][normalize_reason(cls, reason)];
    };

    auto valid_candidate = [&](const DetectionCandidate& c,
                               int cls,
                               const TrackClassConfig& ccfg,
                               const InternalTrack& current) -> bool {
        const float create_gate = std::max(0.01f, std::max(ccfg.min_conf, ccfg.create_conf));
        const float keep_gate = std::max(0.01f, std::max(ccfg.min_conf * 0.5f, ccfg.keep_conf));
        const float conf_gate = current.active ? keep_gate : create_gate;
        if (c.score < conf_gate) {
            reject(cls, "low_conf");
            return false;
        }
        const float bw = box_width(c.box);
        const float bh = box_height(c.box);
        if (bw < 2.f || bh < 2.f) {
            reject(cls, "invalid_box");
            return false;
        }
        const float area_n = box_area(c.box) / std::max(1.f, static_cast<float>(frame_w_ * frame_h_));
        if (area_n < ccfg.min_box_area_norm || area_n > ccfg.max_box_area_norm) {
            reject(cls, "area_out_of_bounds");
            return false;
        }
        const float ar = bw / std::max(1.f, bh);
        if (ar < ccfg.min_aspect || ar > ccfg.max_aspect) {
            reject(cls, "aspect_out_of_bounds");
            return false;
        }
        const float nx = box_cx(c.box) / static_cast<float>(std::max(1, frame_w_));
        const float ny = box_cy(c.box) / static_cast<float>(std::max(1, frame_h_));
        // During shot context the ball may arc into the hoop zone (top of frame), so relax
        // the ball's region_y_min to 0.06 to allow near-rim tracking.
        const float eff_region_y_min =
            (cls == kYoloClsBasketball && shot_active)
                ? std::min(ccfg.region_y_min, 0.06f)
                : ccfg.region_y_min;
        if (nx < ccfg.region_x_min || nx > ccfg.region_x_max || ny < eff_region_y_min ||
            ny > ccfg.region_y_max) {
            reject(cls, "region_out_of_bounds");
            return false;
        }
        // Hoop-zone false-positive suppression: the top portion of the frame contains the
        // backboard/hoop.  Ball detections there during non-shot context are almost always
        // spurious reflections or YOLO confusion with net/ball graphic.  High-confidence
        // (>=0.65) detections are still allowed so real close-range hoop shots go through.
        if (cls == kYoloClsBasketball && !shot_active && ny < 0.30f && c.score < 0.65f) {
            reject(cls, "hoop_zone_fp");
            return false;
        }
        // Edge-proximity filter: reject ball detections with center within 3% of frame edges
        // (very thin boxes pressed against the frame edge are almost always background blobs).
        if (cls == kYoloClsBasketball &&
            (nx < 0.03f || nx > 0.97f || ny < 0.03f || ny > 0.97f)) {
            reject(cls, "edge_proximity_fp");
            return false;
        }
        if (current.active) {
            const std::array<float, 4>& ref_box = current.missed > 0 ? current.predicted : current.smooth;
            const float d = box_dist(c.box, ref_box);
            // During an active shot the basketball rises steeply and quickly. At n=5 YOLO
            // cadence the ball can travel 350-500 px between inference runs. Applying the
            // normal max_jump_px gate here (before the velocity-consistent fast-path in
            // update_track) causes every ball detection at apex to be rejected as
            // "jump_too_large", leaving the track stuck at dribble height for the entire
            // shot. Multiply the limit by 2.5 during shot context so the gate only
            // rejects genuine teleport artefacts, not real shot arcs.
            const float eff_max_jump = (cls == kYoloClsBasketball && shot_active)
                ? ccfg.max_jump_px * 2.5f
                : ccfg.max_jump_px;
            if (d > eff_max_jump && current.missed < ccfg.max_missed_frames) {
                reject(cls, "jump_too_large");
                return false;
            }
            const float iou = box_iou(c.box, ref_box);
            if (iou < 0.02f && d > (eff_max_jump * 0.75f)) {
                reject(cls, "iou_center_gate_failed");
                return false;
            }
            const float ca = box_area(c.box);
            const float ra = std::max(1.f, box_area(ref_box));
            const float ratio = std::max(ca, ra) / std::max(1.f, std::min(ca, ra));
            if (ratio > 8.0f && iou < 0.05f) {
                reject(cls, "scale_change_too_large");
                return false;
            }
        }
        return true;
    };

    auto pick_best = [&](const std::vector<DetectionCandidate>& candidates,
                         int cls,
                         const TrackClassConfig& ccfg,
                         const InternalTrack& current,
                         const InternalTrack* player_ref,
                         bool require_near_player_when_unstable,
                         DetectionCandidate& best_out,
                         float& best_score_out,
                         bool& accepted_out) {
        accepted_out = false;
        best_score_out = -std::numeric_limits<float>::infinity();
        const bool unstable = !current.active || current.missed > 1;
        for (const auto& c : candidates) {
            if (!valid_candidate(c, cls, ccfg, current)) {
                continue;
            }
            float s = c.score * 100.f;
            if (current.active) {
                const float d = box_dist(c.box, current.smooth);
                s += std::max(-36.f, 24.f - (d / std::max(20.f, ccfg.max_jump_px)) * 36.f);
                const float ca = box_area(c.box);
                const float ta = std::max(1.f, box_area(current.smooth));
                const float ratio = std::max(ca, ta) / std::max(1.f, std::min(ca, ta));
                s -= std::max(0.f, ratio - 1.f) * 8.f;
            }
            if (cls == kYoloClsPlayer) {
                // Larger player boxes are more likely to be the full character (not just limbs).
                const float area_bonus = std::min(26.f, box_area(c.box) * 0.00008f);
                s += area_bonus;
                // NOTE: frame-center penalty removed — the controlled player can be anywhere
                // on the court.  The stamina anchor bonus (below, lines ~357-359) is the
                // correct identity signal; the center bias was overriding it for off-center play.
            }
            if (player_ref != nullptr && player_ref->active && cls == kYoloClsBasketball) {
                const float dplayer = box_dist(c.box, player_ref->smooth);
                if (require_near_player_when_unstable && unstable &&
                    dplayer > cfg_.ball.max_distance_from_player_px) {
                    reject(cls, "too_far_from_player_unstable");
                    continue;
                }
                s += std::max(-20.f, 16.f - dplayer * 0.08f);
            }
            if (player_ref != nullptr && player_ref->active && cls == kYoloClsStamBar) {
                const float dplayer = box_dist(c.box, player_ref->smooth);
                if (cfg_.stamina.require_player_association &&
                    dplayer > cfg_.stamina.max_distance_from_player_px) {
                    reject(cls, "not_associated_with_player");
                    continue;
                }
                s += std::max(-30.f, 28.f - dplayer * 0.10f);
            }
            if (player_ref != nullptr && player_ref->active && cls == kYoloClsPlayer) {
                const float danchor = box_dist(c.box, player_ref->smooth);
                s += std::max(-18.f, 16.f - danchor * 0.08f);
            }
            if (s > best_score_out) {
                best_score_out = s;
                best_out = c;
                accepted_out = true;
            }
        }
    };

    auto update_track = [&](InternalTrack& t,
                            const TrackClassConfig& ccfg,
                            const DetectionCandidate* best,
                            bool shot_mode,
                            const char* accept_reason) {
        t.accepted_reason.clear();
        t.rejected_reason.clear();
        if (best != nullptr) {
            std::array<float, 4> new_raw = clamp_box(best->box, frame_w_, frame_h_);
            if (!t.active) {
                // Pre-activation confirmation: require min_pre_active_frames consecutive YOLO
                // detections before spawning a new track.  Prevents single-frame FP blobs
                // (reflections, floor textures, hoop) from immediately creating ghost tracks.
                const int min_frames = std::max(1, ccfg.min_pre_active_frames);
                if (t.pre_active_frames > 0 &&
                    box_dist(t.pre_active_box, new_raw) < ccfg.max_jump_px * 0.70f) {
                    ++t.pre_active_frames;
                } else {
                    // First detection or inconsistent position — restart counter.
                    t.pre_active_frames = 1;
                    t.pre_active_box = new_raw;
                }
                if (t.pre_active_frames < min_frames) {
                    t.rejected_reason = "pre_activation_pending";
                    return;  // Not confirmed yet; skip track creation this frame.
                }
                // Confirmed: activate track.
                t.pre_active_frames = 0;
                t.active = true;
                t.track_id = next_track_id_++;
                t.smooth = new_raw;
                t.vel = {0.f, 0.f};
                t.pending_switch_frames = 0;
                t.pending_switch_box = {};
            } else {
                const float d = box_dist(t.smooth, new_raw);
                const bool large_jump = d > (ccfg.max_jump_px * 0.55f);
                // Velocity-consistent fast-path: if the detection is where the current
                // velocity predicts it should be, treat this as continuous movement and
                // skip the pending-switch confirmation window.  Works during carry too
                // (t.missed > 0) — a sprinting player carried for 4 frames at decayed
                // velocity still leaves a directionally-correct predicted position; any
                // detection within 1.5× max_velocity of that prediction snaps immediately.
                // Genuine identity hops are far from the velocity prediction and still
                // hit the confirmation window.
                const bool velocity_consistent = [&]() -> bool {
                    if (!large_jump) {
                        return false;  // Only evaluate for large-jump detections
                    }
                    const float pred_cx = box_cx(t.smooth) + t.vel[0];
                    const float pred_cy = box_cy(t.smooth) + t.vel[1];
                    const float dist_from_pred = std::hypot(box_cx(new_raw) - pred_cx,
                                                            box_cy(new_raw) - pred_cy);
                    // Accept without confirmation if new position is within 1.5× velocity
                    // of the velocity-predicted location (same direction, similar speed).
                    return dist_from_pred < std::max(20.f, ccfg.max_velocity_px * 1.5f);
                }();

                if (large_jump && !velocity_consistent && t.missed < ccfg.max_missed_frames) {
                    if (shot_mode && ccfg.freeze_identity_during_shot &&
                        (t.cls == kYoloClsPlayer || t.cls == kYoloClsBasketball)) {
                        t.rejected_reason = "shot_identity_freeze";
                        ++rejections_by_class_[class_name(t.cls)]["shot_identity_freeze"];
                        t.predicted = t.smooth;
                        t.smooth = clamp_box(t.predicted, frame_w_, frame_h_);
                        t.missed = std::min(t.missed + 1, ccfg.max_missed_frames);
                        return;
                    }
                    const float pd = box_dist(t.pending_switch_box, new_raw);
                    if (t.pending_switch_frames > 0 && pd <= (ccfg.max_jump_px * 0.30f)) {
                        ++t.pending_switch_frames;
                    } else {
                        t.pending_switch_frames = 1;
                        t.pending_switch_box = new_raw;
                    }
                    int required = shot_mode ? (ccfg.confirm_frames + 1) : ccfg.confirm_frames;
                    if (t.cls == kYoloClsPlayer || t.cls == kYoloClsStamBar) {
                        required += 1;
                    }
                    if (t.pending_switch_frames < std::max(2, required)) {
                        t.rejected_reason = "switch_pending_confirmation";
                        ++rejections_by_class_[class_name(t.cls)]["switch_pending_confirmation"];
                        t.predicted = t.smooth;
                        t.smooth = clamp_box(t.predicted, frame_w_, frame_h_);
                        t.missed = std::min(t.missed + 1, ccfg.max_missed_frames);
                        return;
                    }
                    t.pending_switch_frames = 0;
                    ++identity_switches_by_class_[class_name(t.cls)];
                } else {
                    t.pending_switch_frames = 0;
                }
                const float max_step = std::max(8.f, ccfg.max_velocity_px);
                const float old_cx = box_cx(t.smooth);
                const float old_cy = box_cy(t.smooth);
                float new_cx = box_cx(new_raw);
                float new_cy = box_cy(new_raw);
                const float dx = new_cx - old_cx;
                const float dy = new_cy - old_cy;
                const float step = std::hypot(dx, dy);
                if (step > max_step) {
                    const float k = max_step / step;
                    new_cx = old_cx + dx * k;
                    new_cy = old_cy + dy * k;
                    const float bw = box_width(new_raw);
                    const float bh = box_height(new_raw);
                    new_raw = {
                        new_cx - bw * 0.5f,
                        new_cy - bh * 0.5f,
                        new_cx + bw * 0.5f,
                        new_cy + bh * 0.5f,
                    };
                }
                const float nx = box_cx(new_raw);
                const float ny = box_cy(new_raw);
                t.vel = {nx - box_cx(t.smooth), ny - box_cy(t.smooth)};
                t.smooth = lerp_box(t.smooth, new_raw, ccfg.smoothing_alpha);
            }
            t.raw = new_raw;
            t.predicted = t.smooth;
            t.confidence = best->score;
            t.identity_confidence = std::clamp((best->score * 0.70f) + 0.30f, 0.f, 1.f);
            t.missed = 0;
            t.age += 1;
            t.last_seen = frame_index;
            t.accepted_reason = accept_reason;
            return;
        }

        if (!t.active) {
            if (detections_fresh) {
                // YOLO ran but produced no valid candidate — reset pre-activation counter so
                // the confirmation window restarts from scratch on next detection.
                t.pre_active_frames = 0;
                t.pre_active_box = {};
            }
            t.rejected_reason = detections_fresh ? "no_candidate" : "no_new_detections";
            return;
        }

        if (!detections_fresh) {
            t.predicted = t.smooth;
            t.rejected_reason = "no_new_detections_hold";
            return;
        }

        t.missed += 1;
        int allowed_missed = ccfg.max_missed_frames;
        if (t.cls == kYoloClsPlayer && stamina_ != nullptr && stamina_->active) {
            // Keep controlled-player identity stable when stamina remains anchored.
            allowed_missed += 10;
        }
        if (t.missed <= allowed_missed) {
            // Class-specific velocity decay: player preserves momentum best (α=0.90) so
            // carry prediction stays close to a sprinting player's real position across
            // 4-6 carry frames; ball slightly lower (α=0.85) since it decelerates faster;
            // stamina matches player feet so it doesn't drift independently (α=0.80).
            const float vel_decay = (t.cls == kYoloClsPlayer)     ? 0.90f :
                                    (t.cls == kYoloClsBasketball)  ? 0.85f : 0.80f;
            t.vel[0] *= vel_decay;
            t.vel[1] *= vel_decay;
            const float max_vel = std::max(8.f, ccfg.max_velocity_px);
            const float vx = std::clamp(t.vel[0], -max_vel, max_vel);
            const float vy = std::clamp(t.vel[1], -max_vel, max_vel);
            t.predicted = {
                t.smooth[0] + vx, t.smooth[1] + vy, t.smooth[2] + vx, t.smooth[3] + vy,
            };
            t.smooth = clamp_box(t.predicted, frame_w_, frame_h_);
            t.rejected_reason = "carry_prediction";
            return;
        }
        t.active = false;
        t.rejected_reason = "stale_track_killed";
        ++rejections_by_class_[class_name(t.cls)]["stale_track_killed"];
    };

    auto track_to_state = [&](const InternalTrack& t,
                              AuthorityTrackState& out,
                              bool shot_mode,
                              const InternalTrack* player_ref) {
        const int class_max_missed =
            (t.cls == kYoloClsBasketball)
                ? cfg_.ball.max_missed_frames
                : (t.cls == kYoloClsStamBar ? cfg_.stamina.max_missed_frames
                                            : cfg_.player.max_missed_frames);
        out.exists = t.active || (t.missed > 0 && t.missed <= class_max_missed);
        out.visible = t.active && t.missed == 0;
        out.predicted = t.active && t.missed > 0;
        out.lost = !t.active;
        out.is_reliable = t.active && (t.missed <= 1) && (t.identity_confidence >= 0.45f);
        out.track_id = t.track_id;
        out.associated_player_id = (t.cls == kYoloClsStamBar && player_ref != nullptr) ? player_ref->track_id : 0;
        out.missed_frames = t.missed;
        out.track_age = t.age;
        out.last_seen_frame = t.last_seen;
        out.confidence = t.confidence;
        out.identity_confidence = t.identity_confidence;
        out.raw_box = t.raw;
        out.smooth_box = t.smooth;
        out.predicted_box = t.predicted;
        out.velocity = t.vel;
        out.accepted_reason = t.accepted_reason;
        out.rejected_reason = t.rejected_reason;
        if (player_ref != nullptr && player_ref->active && t.active) {
            const float dplayer = box_dist(t.smooth, player_ref->smooth);
            out.alignment_score = std::max(0.f, 1.f - (dplayer / std::max(100.f, cfg_.stamina.max_distance_from_player_px)));
            out.near_player = dplayer <= cfg_.ball.max_distance_from_player_px;
        } else {
            out.alignment_score = 0.f;
            out.near_player = false;
        }
        if (t.cls == kYoloClsBasketball) {
            const float ny = box_cy(t.smooth) / static_cast<float>(std::max(1, frame_h_));
            out.in_valid_shot_region = shot_mode ? (ny >= 0.08f && ny <= 0.90f) : true;
        } else {
            out.in_valid_shot_region = true;
        }
    };

    DetectionCandidate best_player{};
    DetectionCandidate best_stamina{};
    DetectionCandidate best_ball{};
    float score_player = 0.f;
    float score_stamina = 0.f;
    float score_ball = 0.f;
    bool have_player = false;
    bool have_stamina = false;
    bool have_ball = false;

    pick_best(player_candidates,
              kYoloClsPlayer,
              cfg_.player,
              *player_,
              stamina_->active ? stamina_ : nullptr,
              false,
              best_player,
              score_player,
              have_player);
    update_track(*player_, cfg_.player, have_player ? &best_player : nullptr, shot_active, "best_player_candidate");

    pick_best(stamina_candidates,
              kYoloClsStamBar,
              cfg_.stamina,
              *stamina_,
              player_->active ? player_ : nullptr,
              false,
              best_stamina,
              score_stamina,
              have_stamina);
    update_track(*stamina_, cfg_.stamina, have_stamina ? &best_stamina : nullptr, shot_active, "best_stamina_candidate");

    // Disable the near-player proximity guard when the player track itself has been
    // lost for >3 frames — the reference position is too stale to reliably gate ball
    // candidates, and applying both guards simultaneously prevents recovery of either.
    const bool ball_require_near = cfg_.ball.require_near_player_when_unstable &&
                                   (player_->missed <= 3);
    pick_best(ball_candidates,
              kYoloClsBasketball,
              cfg_.ball,
              *ball_,
              player_->active ? player_ : nullptr,
              ball_require_near,
              best_ball,
              score_ball,
              have_ball);
    update_track(*ball_, cfg_.ball, have_ball ? &best_ball : nullptr, shot_active, "best_ball_candidate");

    track_to_state(*player_, out_state.player, shot_active, stamina_->active ? stamina_ : nullptr);
    track_to_state(*ball_, out_state.ball, shot_active, player_->active ? player_ : nullptr);
    track_to_state(*stamina_, out_state.stamina, shot_active, player_->active ? player_ : nullptr);
    out_state.rejections_by_class = rejections_by_class_;
    out_state.identity_switches_by_class = identity_switches_by_class_;
}

}  // namespace j2k::dll
