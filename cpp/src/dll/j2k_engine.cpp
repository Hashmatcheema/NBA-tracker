#ifdef _MSC_VER
#define _CRT_SECURE_NO_WARNINGS
#endif
#include "j2k/dll/config_json_io.hpp"
#include "j2k/dll/j2k_engine.hpp"
#include "j2k/dll/gcv_layout.hpp"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

#include <algorithm>
#include <cmath>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <numeric>
#include <string>
#include <thread>
#include <vector>

namespace {

constexpr int kStaminaCarryHeavyMissMax = 12;
// Raised 8 → 24: at n=4 cadence, 8 heavy frames = ~32 actual frames ≈ 1.5s at 22fps.
// A stationary player (e.g. catch-and-shoot) has a genuinely static stamina bar — the old
// threshold rejected real detections and forced the stamina-proxy fallback prematurely.
constexpr int kStaminaStaticHeavyMax = 24;

void push_unique_yolo_path(std::vector<std::filesystem::path>& out, const std::filesystem::path& p) {
    if (p.empty()) {
        return;
    }
    for (const auto& q : out) {
        if (q == p) {
            return;
        }
    }
    out.push_back(p);
}

bool env_truthy_engine(const char* name, bool fallback) {
#ifdef _WIN32
    char buf[32]{};
    if (GetEnvironmentVariableA(name, buf, static_cast<DWORD>(sizeof(buf))) == 0) {
        return fallback;
    }
    const std::string v(buf);
#else
    const char* raw = std::getenv(name);
    if (raw == nullptr) {
        return fallback;
    }
    const std::string v(raw);
#endif
    if (v == "1" || v == "true" || v == "TRUE" || v == "on" || v == "ON") {
        return true;
    }
    if (v == "0" || v == "false" || v == "FALSE" || v == "off" || v == "OFF") {
        return false;
    }
    return fallback;
}

std::int64_t debug_now_ms_engine() {
    return static_cast<std::int64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count());
}

bool release_agent_log_enabled_engine() {
    return false;
}

void agent_log_c1a192_engine(const char* hypothesis_id,
                             const char* location,
                             const char* message,
                             const std::string& data_json) {
    (void)hypothesis_id;
    (void)location;
    (void)message;
    (void)data_json;
}

void debug_log_line_engine(const char* run_id,
                           const char* hypothesis_id,
                           const char* location,
                           const char* message,
                           const std::string& data_json) {
    const char* en = std::getenv("J2K_TRACK_DEBUG");
    if (en == nullptr || !(std::strcmp(en, "1") == 0 || std::strcmp(en, "true") == 0 || std::strcmp(en, "TRUE") == 0)) {
        return;
    }
    const char* path = std::getenv("J2K_TRACK_DEBUG_LOG");
    const char* out = (path != nullptr && path[0] != '\0') ? path : "j2k_track_debug.log";
    std::FILE* fp = std::fopen(out, "ab");
    if (fp == nullptr) {
        return;
    }
    const std::int64_t ts = debug_now_ms_engine();
    std::fprintf(
        fp,
        "{\"runId\":\"%s\",\"hypothesisId\":\"%s\",\"location\":\"%s\",\"message\":\"%s\",\"data\":%s,\"timestamp\":%lld}\n",
        run_id ? run_id : "",
        hypothesis_id ? hypothesis_id : "",
        location ? location : "",
        message ? message : "",
        data_json.c_str(),
        static_cast<long long>(ts));
    std::fclose(fp);
}

void pack_u16_be(std::uint8_t* d, std::uint16_t v) {
    d[0] = static_cast<std::uint8_t>((static_cast<unsigned>(v) >> 8) & 0xffU);
    d[1] = static_cast<std::uint8_t>(static_cast<unsigned>(v) & 0xffU);
}

/// Reject full-frame / centered glitch detections and single-frame teleports vs last box.
bool player_det_plausible(const std::array<float, 4>& b,
                          int w,
                          int h,
                          const std::array<float, 4>* prev,
                          bool have_prev) {
    const float x1 = b[0];
    const float y1 = b[1];
    const float x2 = b[2];
    const float y2 = b[3];
    if (!(x2 > x1 + 6.f && y2 > y1 + 6.f)) {
        return false;
    }
    const float bw = x2 - x1;
    const float bh = y2 - y1;
    const float fw = static_cast<float>(std::max(1, w));
    const float fh = static_cast<float>(std::max(1, h));
    if (bw > 0.75f * fw || bh > 0.92f * fh) {
        return false;
    }
    if (bw < 0.020f * fw || bh < 0.040f * fh) {
        return false;
    }
    if (bw * bh > 0.70f * fw * fh) {
        return false;
    }
    const float ar = bh / std::max(4.f, bw);
    if (ar < 0.35f || ar > 7.0f) {
        return false;
    }
    if (x1 < -0.03f * fw || y1 < -0.03f * fh || x2 > fw * 1.03f || y2 > fh * 1.03f) {
        return false;
    }
    if (have_prev && prev != nullptr) {
        const float pcx = 0.5f * (x1 + x2);
        const float pcy = 0.5f * (y1 + y2);
        const float qcx = 0.5f * ((*prev)[0] + (*prev)[2]);
        const float qcy = 0.5f * ((*prev)[1] + (*prev)[3]);
        const float ndx = (pcx - qcx) / fw;
        const float ndy = (pcy - qcy) / fh;
        if (std::hypot(ndx, ndy) > 0.75f) {
            return false;
        }
    }
    return true;
}

bool runtime_player_box_sane(const std::array<float, 4>& b, int w, int h) {
    const float fw = static_cast<float>(std::max(1, w));
    const float fh = static_cast<float>(std::max(1, h));
    const float x1 = b[0];
    const float y1 = b[1];
    const float x2 = b[2];
    const float y2 = b[3];
    if (!(x2 > x1 + 6.f && y2 > y1 + 6.f)) {
        return false;
    }
    const float cx = 0.5f * (x1 + x2);
    const float cy = 0.5f * (y1 + y2);
    const float bw = x2 - x1;
    const float bh = y2 - y1;
    // Keep true far-player detections, but reject tiny ankle-level locks.
    if (bw < fw * 0.020f || bh < fh * 0.055f) {
        return false;
    }
    // Reject boxes that are too large — crowd sections, court markings, HUD elements.
    // Close-up players occupy ~6-17% of frame area; 18% is the hard ceiling.
    if ((bw * bh) > fw * fh * 0.18f) {
        return false;
    }
    // Reject landscape-orientation boxes — NBA 2K players are always portrait (taller than wide).
    // width > height × 1.25 is a court line, referee group, or crowd row, not a player.
    // 1.25 rather than 1.10: EMA smoothing can blend two portrait frames into a near-square
    // result transiently; we only want to kill clearly-landscape misdetections.
    if (bw > bh * 1.25f) {
        return false;
    }
    // Reject tiny boxes pinned in extreme corners (common bad lock failure mode).
    if ((cx < fw * 0.12f || cx > fw * 0.88f) &&
        (cy < fh * 0.18f || cy > fh * 0.90f) &&
        (bw * bh) < fw * fh * 0.02f) {
        return false;
    }
    return true;
}

float box_center_dist(const std::array<float, 4>& a, const std::array<float, 4>& b) {
    const float acx = 0.5f * (a[0] + a[2]);
    const float acy = 0.5f * (a[1] + a[3]);
    const float bcx = 0.5f * (b[0] + b[2]);
    const float bcy = 0.5f * (b[1] + b[3]);
    return std::hypot(acx - bcx, acy - bcy);
}

float xyxy_iou(const std::array<float, 4>& a, const std::array<float, 4>& b) {
    const float ix1 = std::max(a[0], b[0]);
    const float iy1 = std::max(a[1], b[1]);
    const float ix2 = std::min(a[2], b[2]);
    const float iy2 = std::min(a[3], b[3]);
    const float iw = std::max(0.f, ix2 - ix1);
    const float ih = std::max(0.f, iy2 - iy1);
    const float inter = iw * ih;
    const float aa = std::max(0.f, a[2] - a[0]) * std::max(0.f, a[3] - a[1]);
    const float bb = std::max(0.f, b[2] - b[0]) * std::max(0.f, b[3] - b[1]);
    const float uni = aa + bb - inter;
    if (uni <= 1e-6f) {
        return 0.f;
    }
    return inter / uni;
}

bool ball_box_plausible_for_apex(const std::array<float, 4>& pb, const std::array<float, 4>& bb) {
    const float px1 = pb[0];
    const float py1 = pb[1];
    const float px2 = pb[2];
    const float py2 = pb[3];
    const float bx1 = bb[0];
    const float by1 = bb[1];
    const float bx2 = bb[2];
    const float by2 = bb[3];
    const float ph = std::max(1.f, py2 - py1);
    const float pw = std::max(1.f, px2 - px1);
    const float cx = 0.5f * (bx1 + bx2);
    const float cy = 0.5f * (by1 + by2);
    if (cy > py2 + ph * 0.12f) {
        return false;
    }
    if (cx < px1 - pw * 0.42f || cx > px2 + pw * 0.42f) {
        return false;
    }
    if (cy < py1 - ph * 0.90f) {  // was 0.62 — too strict for fadeaway/high-arc shots near top of frame
        return false;
    }
    return true;
}

bool ball_box_in_player_search_zone(const std::array<float, 4>& pb,
                                    const std::array<float, 4>& bb,
                                    bool shot_active) {
    const float px1 = pb[0];
    const float py1 = pb[1];
    const float px2 = pb[2];
    const float py2 = pb[3];
    const float bx1 = bb[0];
    const float by1 = bb[1];
    const float bx2 = bb[2];
    const float by2 = bb[3];
    const float pw = std::max(1.f, px2 - px1);
    const float ph = std::max(1.f, py2 - py1);
    const float cx = 0.5f * (bx1 + bx2);
    const float cy = 0.5f * (by1 + by2);
    const float xs = shot_active ? 0.60f : 0.45f;
    const float ts = shot_active ? 0.95f : 0.72f;
    constexpr float bs = 0.18f;
    const float xl = px1 - pw * xs;
    const float xr = px2 + pw * xs;
    const float yt = py1 - ph * ts;
    const float yb = py2 + ph * bs;
    return xl <= cx && cx <= xr && yt <= cy && cy <= yb;
}

bool predict_ball_box_from_live_history(const std::array<float, 4>& last_live_box,
                                        const std::array<float, 4>& prev_live_box,
                                        int gap_frames,
                                        const std::array<float, 4>& person_box,
                                        bool shot_active,
                                        std::array<float, 4>& out_pred) {
    constexpr int kMaxGap = 6;
    if (gap_frames <= 0 || gap_frames > kMaxGap) {
        return false;
    }
    const float lcx = 0.5f * (last_live_box[0] + last_live_box[2]);
    const float lcy = 0.5f * (last_live_box[1] + last_live_box[3]);
    const float pcx = 0.5f * (prev_live_box[0] + prev_live_box[2]);
    const float pcy = 0.5f * (prev_live_box[1] + prev_live_box[3]);
    float vx = lcx - pcx;
    float vy = lcy - pcy;
    const float ph = std::max(1.f, person_box[3] - person_box[1]);
    const float max_x_pf = ph * 0.22f;
    const float max_y_pf = ph * 0.24f;
    vx = std::clamp(vx, -max_x_pf, max_x_pf);
    vy = std::clamp(vy, -max_y_pf, max_y_pf);
    const float dx = vx * static_cast<float>(gap_frames);
    const float dy = vy * static_cast<float>(gap_frames);
    out_pred = {last_live_box[0] + dx,
                last_live_box[1] + dy,
                last_live_box[2] + dx,
                last_live_box[3] + dy};
    if (!(ball_box_in_player_search_zone(person_box, out_pred, shot_active) ||
          ball_box_plausible_for_apex(person_box, out_pred))) {
        return false;
    }
    return true;
}

/// Greedy NMS for a single class's detection candidates.
/// Sorts by descending score then suppresses lower-confidence boxes whose IoU with any
/// already-kept box exceeds iou_thresh.  Reduces duplicate ghost detections before the
/// tracking authority sees them.
void apply_candidate_nms(std::vector<j2k::dll::DetectionCandidate>& cands, float iou_thresh) {
    if (cands.size() <= 1) {
        return;
    }
    std::stable_sort(cands.begin(), cands.end(),
                     [](const j2k::dll::DetectionCandidate& a,
                        const j2k::dll::DetectionCandidate& b) {
                         return a.score > b.score;
                     });
    std::vector<bool> keep(cands.size(), true);
    for (std::size_t i = 0; i < cands.size(); ++i) {
        if (!keep[i]) {
            continue;
        }
        for (std::size_t j = i + 1; j < cands.size(); ++j) {
            if (!keep[j]) {
                continue;
            }
            if (xyxy_iou(cands[i].box, cands[j].box) > iou_thresh) {
                keep[j] = false;
            }
        }
    }
    std::vector<j2k::dll::DetectionCandidate> result;
    result.reserve(cands.size());
    for (std::size_t i = 0; i < cands.size(); ++i) {
        if (keep[i]) {
            result.push_back(cands[i]);
        }
    }
    cands = std::move(result);
}

bool runtime_ball_box_sane(const std::array<float, 4>& b, int w, int h) {
    const float x1 = b[0];
    const float y1 = b[1];
    const float x2 = b[2];
    const float y2 = b[3];
    const float fw = static_cast<float>(std::max(1, w));
    const float fh = static_cast<float>(std::max(1, h));
    if (!(x2 > x1 + 2.f && y2 > y1 + 2.f)) {
        return false;
    }
    const float bw = x2 - x1;
    const float bh = y2 - y1;
    if (bw < 2.f || bh < 2.f || bw > fw * 0.55f || bh > fh * 0.55f) {
        return false;
    }
    return true;
}

bool fit_player_overlay_box_from_pixels(const std::uint8_t* bgr,
                                        int w,
                                        int h,
                                        int stride,
                                        const std::array<int, 4>& coarse,
                                        std::array<float, 4>& out_box) {
    if (bgr == nullptr || w <= 0 || h <= 0 || stride < (w * 3)) {
        return false;
    }
    const int cx1 = std::clamp(coarse[0], 0, w - 1);
    const int cy1 = std::clamp(coarse[1], 0, h - 1);
    const int cx2 = std::clamp(coarse[2], 0, w - 1);
    const int cy2 = std::clamp(coarse[3], 0, h - 1);
    if (cx2 <= cx1 + 6 || cy2 <= cy1 + 8) {
        return false;
    }

    const int pad_x = std::max(2, (cx2 - cx1) / 10);
    const int pad_y = std::max(2, (cy2 - cy1) / 12);
    const int rx1 = std::max(0, cx1 - pad_x);
    const int ry1 = std::max(0, cy1 - pad_y);
    const int rx2 = std::min(w - 1, cx2 + pad_x);
    const int ry2 = std::min(h - 1, cy2 + pad_y);
    const int rw = rx2 - rx1 + 1;
    const int rh = ry2 - ry1 + 1;
    if (rw < 12 || rh < 16) {
        return false;
    }

    auto is_player_like = [](std::uint8_t b, std::uint8_t g, std::uint8_t r) -> bool {
        const int maxc = std::max({static_cast<int>(r), static_cast<int>(g), static_cast<int>(b)});
        const int minc = std::min({static_cast<int>(r), static_cast<int>(g), static_cast<int>(b)});
        const int sat = maxc - minc;
        const bool dark = maxc < 100;
        // Hardwood approximation: warm and relatively low saturation.
        const bool wood_like =
            (r > g && g > b && (r - g) < 55 && (g - b) < 55 && maxc > 70 && sat < 70);
        const bool saturated = sat > 42;
        return dark || (saturated && !wood_like);
    };

    std::vector<std::uint8_t> mask(static_cast<size_t>(rw) * static_cast<size_t>(rh), 0);
    int fg_count = 0;
    for (int y = ry1; y <= ry2; ++y) {
        const auto* row = bgr + static_cast<std::size_t>(y) * static_cast<std::size_t>(stride);
        for (int x = rx1; x <= rx2; ++x) {
            const auto* p = row + static_cast<std::size_t>(x) * 3U;
            if (!is_player_like(p[0], p[1], p[2])) {
                continue;
            }
            const int lx = x - rx1;
            const int ly = y - ry1;
            mask[static_cast<size_t>(ly) * static_cast<size_t>(rw) + static_cast<size_t>(lx)] = 1;
            ++fg_count;
        }
    }

    const int min_fg = std::max(24, (rw * rh) / 90);
    if (fg_count < min_fg) {
        return false;
    }

    std::vector<std::uint8_t> visited(static_cast<size_t>(rw) * static_cast<size_t>(rh), 0);
    std::vector<int> queue;
    queue.reserve(static_cast<size_t>(rw) * static_cast<size_t>(rh) / 4U);

    const float anchor_x = static_cast<float>(cx1 + cx2) * 0.5f;
    const float anchor_y = static_cast<float>(cy1) + static_cast<float>(cy2 - cy1) * 0.72f;
    int best_x1 = -1;
    int best_y1 = -1;
    int best_x2 = -1;
    int best_y2 = -1;
    float best_score = -1e9f;

    for (int sy = 0; sy < rh; ++sy) {
        for (int sx = 0; sx < rw; ++sx) {
            const size_t seed_idx =
                static_cast<size_t>(sy) * static_cast<size_t>(rw) + static_cast<size_t>(sx);
            if (!mask[seed_idx] || visited[seed_idx]) {
                continue;
            }
            queue.clear();
            queue.push_back(static_cast<int>(seed_idx));
            visited[seed_idx] = 1;

            int qhead = 0;
            int comp_area = 0;
            int bx1 = sx;
            int by1 = sy;
            int bx2 = sx;
            int by2 = sy;

            while (qhead < static_cast<int>(queue.size())) {
                const int idx = queue[static_cast<size_t>(qhead++)];
                const int y = idx / rw;
                const int x = idx - y * rw;
                ++comp_area;
                bx1 = std::min(bx1, x);
                by1 = std::min(by1, y);
                bx2 = std::max(bx2, x);
                by2 = std::max(by2, y);

                const int n4[4][2] = {{x - 1, y}, {x + 1, y}, {x, y - 1}, {x, y + 1}};
                for (const auto& n : n4) {
                    const int nx = n[0];
                    const int ny = n[1];
                    if (nx < 0 || ny < 0 || nx >= rw || ny >= rh) {
                        continue;
                    }
                    const size_t nidx = static_cast<size_t>(ny) * static_cast<size_t>(rw) +
                                        static_cast<size_t>(nx);
                    if (!mask[nidx] || visited[nidx]) {
                        continue;
                    }
                    visited[nidx] = 1;
                    queue.push_back(static_cast<int>(nidx));
                }
            }

            const int bw = bx2 - bx1 + 1;
            const int bh = by2 - by1 + 1;
            if (comp_area < std::max(18, min_fg / 3) || bw < 6 || bh < 12) {
                continue;
            }
            if (bw > static_cast<int>(rw * 0.75f) || bh > static_cast<int>(rh * 0.95f)) {
                continue;
            }
            const float gcx = static_cast<float>(rx1 + bx1 + bx2) * 0.5f;
            const float gcy = static_cast<float>(ry1 + by1 + by2) * 0.5f;
            const float dist = std::hypot(gcx - anchor_x, gcy - anchor_y);
            const float score = static_cast<float>(comp_area) - dist * 2.2f;
            if (score > best_score) {
                best_score = score;
                best_x1 = bx1;
                best_y1 = by1;
                best_x2 = bx2;
                best_y2 = by2;
            }
        }
    }

    if (best_x1 < 0 || best_y1 < 0 || best_x2 <= best_x1 || best_y2 <= best_y1) {
        return false;
    }

    const int mx = std::max(2, (best_x2 - best_x1) / 6);
    const int my = std::max(2, (best_y2 - best_y1) / 8);
    const float ox1 = static_cast<float>(std::clamp(rx1 + best_x1 - mx, 0, w - 1));
    const float oy1 = static_cast<float>(std::clamp(ry1 + best_y1 - my, 0, h - 1));
    const float ox2 = static_cast<float>(std::clamp(rx1 + best_x2 + mx, 0, w - 1));
    const float oy2 = static_cast<float>(std::clamp(ry1 + best_y2 + my, 0, h - 1));
    if (!(ox2 > ox1 + 6.f && oy2 > oy1 + 10.f)) {
        return false;
    }
    out_box = {ox1, oy1, ox2, oy2};
    return true;
}

// ── BGR drawing primitives (no OpenCV — DLL doesn't link cv2) ───────────────
inline void put_pixel_bgr(std::uint8_t* bgr, int w, int h, int stride,
                          int x, int y,
                          std::uint8_t b, std::uint8_t g, std::uint8_t r) {
    if (x < 0 || y < 0 || x >= w || y >= h) return;
    std::uint8_t* p = bgr + static_cast<std::size_t>(y) * static_cast<std::size_t>(stride) +
                      static_cast<std::size_t>(x) * 3U;
    p[0] = b;
    p[1] = g;
    p[2] = r;
}

void draw_rect_bgr(std::uint8_t* bgr, int w, int h, int stride,
                   int x1, int y1, int x2, int y2,
                   std::uint8_t b, std::uint8_t g, std::uint8_t r,
                   int thickness) {
    if (x2 <= x1 || y2 <= y1 || thickness <= 0) return;
    const int xa = std::max(0, std::min(w - 1, x1));
    const int xb = std::max(0, std::min(w - 1, x2));
    const int ya = std::max(0, std::min(h - 1, y1));
    const int yb = std::max(0, std::min(h - 1, y2));
    for (int t = 0; t < thickness; ++t) {
        const int yt = ya + t;
        const int yc = yb - t;
        if (yt <= yb) {
            for (int x = xa; x <= xb; ++x) put_pixel_bgr(bgr, w, h, stride, x, yt, b, g, r);
        }
        if (yc >= ya && yc != yt) {
            for (int x = xa; x <= xb; ++x) put_pixel_bgr(bgr, w, h, stride, x, yc, b, g, r);
        }
    }
    for (int t = 0; t < thickness; ++t) {
        const int xt = xa + t;
        const int xc = xb - t;
        if (xt <= xb) {
            for (int y = ya; y <= yb; ++y) put_pixel_bgr(bgr, w, h, stride, xt, y, b, g, r);
        }
        if (xc >= xa && xc != xt) {
            for (int y = ya; y <= yb; ++y) put_pixel_bgr(bgr, w, h, stride, xc, y, b, g, r);
        }
    }
}

void draw_circle_bgr(std::uint8_t* bgr, int w, int h, int stride,
                     int cx, int cy, int radius,
                     std::uint8_t b, std::uint8_t g, std::uint8_t r,
                     int thickness) {
    if (radius <= 0 || thickness <= 0) return;
    // Midpoint-circle algorithm; repeat for each thickness offset.
    for (int t = 0; t < thickness; ++t) {
        const int rr = radius - t;
        if (rr <= 0) break;
        int x = rr;
        int y = 0;
        int err = 0;
        while (x >= y) {
            put_pixel_bgr(bgr, w, h, stride, cx + x, cy + y, b, g, r);
            put_pixel_bgr(bgr, w, h, stride, cx + y, cy + x, b, g, r);
            put_pixel_bgr(bgr, w, h, stride, cx - y, cy + x, b, g, r);
            put_pixel_bgr(bgr, w, h, stride, cx - x, cy + y, b, g, r);
            put_pixel_bgr(bgr, w, h, stride, cx - x, cy - y, b, g, r);
            put_pixel_bgr(bgr, w, h, stride, cx - y, cy - x, b, g, r);
            put_pixel_bgr(bgr, w, h, stride, cx + y, cy - x, b, g, r);
            put_pixel_bgr(bgr, w, h, stride, cx + x, cy - y, b, g, r);
            ++y;
            if (err <= 0) {
                err += 2 * y + 1;
            } else {
                --x;
                err += 2 * (y - x) + 1;
            }
        }
    }
}

std::array<int, 4> decode_box_u16(const std::uint8_t* bytes, int w, int h) {
    auto u16 = [](const std::uint8_t* p) -> int {
        return (static_cast<int>(p[0]) << 8) | static_cast<int>(p[1]);
    };
    const int x1n = u16(bytes);
    const int y1n = u16(bytes + 2);
    const int x2n = u16(bytes + 4);
    const int y2n = u16(bytes + 6);
    if (x2n <= x1n || y2n <= y1n) {
        return {0, 0, 0, 0};
    }
    const int x1 = static_cast<int>(static_cast<long long>(x1n) * w / 65535);
    const int y1 = static_cast<int>(static_cast<long long>(y1n) * h / 65535);
    const int x2 = static_cast<int>(static_cast<long long>(x2n) * w / 65535);
    const int y2 = static_cast<int>(static_cast<long long>(y2n) * h / 65535);
    return {x1, y1, x2, y2};
}

}  // namespace

namespace j2k::dll {

J2kEngine::ShotType J2kEngine::classify_shot_from_r2(int r2a, int r2b) const {
    const bool hot = std::max(r2a, r2b) >= 50;
    if (fadeaway_when_r2_high_) {
        return hot ? ShotType::Fadeaway : ShotType::Normal;
    }
    return hot ? ShotType::Normal : ShotType::Fadeaway;
}

void J2kEngine::init(int width, int height, const char* script_dir_utf8) {
    if (yolo_warmup_thread_.joinable()) {
        yolo_warmup_thread_.join();
    }
    if (yolo_warmup_timeout_thread_.joinable()) {
        yolo_warmup_timeout_thread_.join();
    }
    width_ = width;
    height_ = height;
    script_dir_ = script_dir_utf8 ? script_dir_utf8 : "";
    frame_n_ = 0;
    prev_square_held_ = false;
    shot_active_ = false;
    release_fired_this_hold_ = false;
    visual_owner_ = false;
    shot_type_locked_ = false;
    square_held_frames_ = 0;
    shot_id_ = 0;
    shot_type_ = ShotType::Normal;
    latched_windup_ = ShotType::Normal;
    fire_zone_.reset();
    tempo_flick_.cancel();
    tracker_.reset(width_, height_);
    player_bt_.reset();
    ball_bt_.reset();
    last_cfg_mtime_ = {};
    release_cue_value_ = 50.f;
    tempo_speed_ = 35.f;
    last_visual_reason_ = "";
    last_inside_fire_zone_ = false;
    last_entered_fire_zone_ = false;
    have_last_fire_zone_ = false;
    last_fire_zone_box_.fill(0.f);
    last_ball_cx_ = 0.f;
    last_ball_cy_ = 0.f;
    last_ball_seen_frame_ = 0;
    shot_locked_ball_box_.fill(0.f);
    have_shot_locked_ball_ = false;
    shot_locked_ball_gap_frames_ = 0;
    last_live_ball_box_.fill(0.f);
    prev_live_ball_box_.fill(0.f);
    have_last_live_ball_ = false;
    have_prev_live_ball_ = false;
    last_live_ball_frame_ = -1;
    last_yolo_ball_box_.fill(0.f);
    have_last_yolo_ball_ = false;
    last_yolo_ball_frame_ = -1;
    last_nofire_hint_ = "";
    perf_i_ = 0;
    std::memset(perf_ring_total_, 0, sizeof(perf_ring_total_));
    std::memset(perf_ring_yolo_, 0, sizeof(perf_ring_yolo_));
    std::memset(perf_ring_pre_, 0, sizeof(perf_ring_pre_));
    std::memset(perf_ring_infer_, 0, sizeof(perf_ring_infer_));
    std::memset(perf_ring_post_, 0, sizeof(perf_ring_post_));
    have_last_stable_player_ = false;
    last_stable_player_box_.fill(0.f);
    last_stable_player_seen_frame_ = 0;
    pending_player_box_.fill(0.f);
    pending_player_confirm_frames_ = 0;
    frame_intervals_ms_.clear();
    skipped_heavy_frames_ = 0;
    last_frame_time_ = {};
    yolo_every_n_ = 1;
    yolo_every_n_runtime_ = 1;
    heavy_backoff_frames_ = 0;
    yolo_warmup_done_.store(true, std::memory_order_release);
    last_total_ms_ = 0.0;
    last_yolo_ms_ = 0.0;
    player_yolo_miss_frames_ = 0;
    ball_yolo_miss_frames_ = 0;
    have_last_stamina_ = false;
    stamina_miss_frames_ = 0;
    stamina_static_heavy_frames_ = 0;
    tracking_state_ = TrackingState::kIdle;
    tracking_state_frame_ = 0;
    telemetry_ = FrameTelemetry{};
#ifdef _WIN32
    char yolo_every_buf[16]{};
    if (GetEnvironmentVariableA("J2K_YOLO_EVERY_N",
                                yolo_every_buf,
                                static_cast<DWORD>(sizeof(yolo_every_buf))) > 0) {
        const int parsed = std::atoi(yolo_every_buf);
        if (parsed >= 1 && parsed <= 240) {
            yolo_every_n_ = parsed;
        }
    }
#else
    if (const char* ev = std::getenv("J2K_YOLO_EVERY_N")) {
        const int parsed = std::atoi(ev);
        if (parsed >= 1 && parsed <= 240) {
            yolo_every_n_ = parsed;
        }
    }
#endif
    yolo_every_n_runtime_ = yolo_every_n_;
    use_bytetrack_ = env_truthy_engine("J2K_USE_BYTETRACK", true);
    reload_config_throttled();
    authority_.reset(width_, height_);

    yolo_ = std::make_unique<YoloOnnxRunner>();
    namespace fs = std::filesystem;
    J2kYoloJsonSettings yjson;
    std::string cfg_path = script_dir_;
    if (!cfg_path.empty() && cfg_path.back() != '\\' && cfg_path.back() != '/') {
        cfg_path += '/';
    }
    cfg_path += "j2k_config.json";
    (void)read_j2k_yolo_json_file(cfg_path, yjson);
    TrackingAuthorityConfig tracking_cfg = default_tracking_authority_config();
    (void)read_j2k_tracking_json_file(cfg_path, tracking_cfg);
    authority_.set_config(tracking_cfg);

    YoloOnnxConfig ycfg;
    ycfg.input_size = yjson.imgsz;
    ycfg.conf_threshold = yjson.conf;
    ycfg.player_conf_threshold = yjson.player_conf;
    ycfg.stamina_conf_threshold = yjson.stamina_conf;
    ycfg.iou_threshold = yjson.iou;
    ycfg.ball_conf_threshold = yjson.ball_conf;
    ycfg.player_ema_alpha = yjson.player_ema_alpha;
    ycfg.ball_ema_alpha = yjson.ball_ema_alpha;
    ycfg.onnx_threads = yjson.onnx_threads;

    // J2K_YOLO_IMGSZ env var allows runtime override of the config-specified input size.
    std::string env_imgsz;
#ifdef _WIN32
    char env_imgsz_buf[64]{};
    if (GetEnvironmentVariableA("J2K_YOLO_IMGSZ",
                                 env_imgsz_buf,
                                 static_cast<DWORD>(sizeof(env_imgsz_buf))) > 0) {
        env_imgsz = env_imgsz_buf;
    }
#else
    if (const char* ev = std::getenv("J2K_YOLO_IMGSZ")) {
        env_imgsz = ev;
    }
#endif
    if (!env_imgsz.empty()) {
        const int forced = std::atoi(env_imgsz.c_str());
        if (forced >= 320) {
            ycfg.input_size = forced;
        }
    }

    std::string env_onnx;
#ifdef _WIN32
    char env_onnx_buf[2048]{};
    if (GetEnvironmentVariableA("J2K_YOLO_ONNX",
                                 env_onnx_buf,
                                 static_cast<DWORD>(sizeof(env_onnx_buf))) > 0) {
        env_onnx = env_onnx_buf;
    }
#else
    if (const char* ev = std::getenv("J2K_YOLO_ONNX")) {
        env_onnx = ev;
    }
#endif

    std::vector<fs::path> yolo_candidates;
    push_unique_yolo_path(yolo_candidates, fs::path(env_onnx));
    if (!yjson.onnx_relative.empty()) {
        const fs::path rel(yjson.onnx_relative);
        const fs::path base(script_dir_);
        push_unique_yolo_path(yolo_candidates, rel.is_absolute() ? rel : (base / rel));
    }
    push_unique_yolo_path(yolo_candidates, fs::path(script_dir_) / "models" / "j2k_yolo.onnx");
    push_unique_yolo_path(yolo_candidates, fs::path(script_dir_) / "bin" / "models" / "j2k_yolo.onnx");
    if (!script_dir_.empty()) {
        push_unique_yolo_path(yolo_candidates,
                              fs::path(script_dir_).parent_path() / "models" / "j2k_yolo.onnx");
    }

    fs::path onnx_found;
    std::error_code ec_exist;
    for (const auto& cand : yolo_candidates) {
        if (fs::exists(cand, ec_exist) && !ec_exist) {
            onnx_found = cand;
            break;
        }
    }

    if (!onnx_found.empty()) {
        ycfg.model_path = onnx_found.string();

        // Inject absolute TRT cache path so yolo_onnx.cpp never relies on process CWD.
        // The Python process CWD is scripts/ (set by j2k.bat), but trt_cache lives in python/.
        // We also pre-create the directory here so ORT never fails due to a missing cache dir.
        {
            const fs::path trt_dir = fs::path(script_dir_) / "trt_cache";
            std::error_code ec_mk;
            fs::create_directories(trt_dir, ec_mk);
            ycfg.trt_cache_path = trt_dir.string();
        }

#ifdef _WIN32
        // Prepend python/bin to PATH so ORT 1.26 CUDA/TRT provider DLLs find
        // CUDA 12 / cuDNN 9 / TRT 10 DLLs that are staged there.
        // ORT dynamically loads cuDNN/cuBLAS by filename (no full path), so they
        // must be on PATH — file existence in python/bin alone is not enough.
        {
            const fs::path bin_dir = fs::path(script_dir_) / "bin";
            std::error_code ec_chk;
            if (fs::exists(bin_dir, ec_chk) && !ec_chk) {
                const std::string bin_str = bin_dir.string();
                char existing_path[32768] = {};
                GetEnvironmentVariableA("PATH", existing_path, sizeof(existing_path));
                // Only prepend if not already present.
                if (std::string(existing_path).find(bin_str) == std::string::npos) {
                    const std::string new_path = bin_str + ";" + std::string(existing_path);
                    SetEnvironmentVariableA("PATH", new_path.c_str());
                    std::fprintf(stdout,
                                 "[J2K ONNX] Prepended %s to PATH for CUDA provider DLLs.\n",
                                 bin_str.c_str());
                    std::fflush(stdout);
                }
                // Also register with the DLL directory API (used by modern LoadLibraryEx).
                const std::wstring bin_dir_w = bin_dir.wstring();
                AddDllDirectory(bin_dir_w.c_str());

                // Diagnostic: try loading onnxruntime_providers_cuda.dll ourselves to get the
                // exact Windows error code — ORT's error only says "Failed to load shared library".
                const fs::path cuda_prov = bin_dir / "onnxruntime_providers_cuda.dll";
                if (fs::exists(cuda_prov)) {
                    const std::wstring cuda_prov_w = cuda_prov.wstring();
                    HMODULE h = LoadLibraryExW(cuda_prov_w.c_str(), nullptr,
                                               LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR |
                                               LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
                    if (h) {
                        std::fprintf(stdout, "[J2K ONNX] onnxruntime_providers_cuda.dll probe: OK (handle=%p)\n",
                                     static_cast<void*>(h));
                        FreeLibrary(h);
                    } else {
                        const DWORD err = GetLastError();
                        char msg[512] = {};
                        FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                                       nullptr, err, 0, msg, sizeof(msg), nullptr);
                        // Strip trailing newline from FormatMessage
                        for (char& c : msg) { if (c == '\r' || c == '\n') c = ' '; }
                        std::fprintf(stdout,
                                     "[J2K ONNX] onnxruntime_providers_cuda.dll probe FAILED: "
                                     "Win32 error %lu — %s\n",
                                     static_cast<unsigned long>(err), msg);
                        // Retry with standard search to compare results
                        HMODULE h2 = LoadLibraryW(cuda_prov_w.c_str());
                        if (h2) {
                            std::fprintf(stdout, "[J2K ONNX]   Standard LoadLibraryW: OK\n");
                            FreeLibrary(h2);
                        } else {
                            const DWORD err2 = GetLastError();
                            std::fprintf(stdout,
                                         "[J2K ONNX]   Standard LoadLibraryW also failed: Win32 error %lu\n",
                                         static_cast<unsigned long>(err2));
                        }
                    }
                    std::fflush(stdout);
                }
            }
        }
#endif

        // #region agent log
        agent_log_c1a192_engine(
            "H56",
            "j2k_engine.cpp:init",
            "before_yolo_load",
            std::string("{\"modelPath\":\"") + ycfg.model_path + "\",\"imgsz\":" +
                std::to_string(ycfg.input_size) + ",\"conf\":" + std::to_string(ycfg.conf_threshold) +
                ",\"playerConf\":" + std::to_string(ycfg.player_conf_threshold) +
                ",\"ballConf\":" + std::to_string(ycfg.ball_conf_threshold) +
                ",\"staminaConf\":" + std::to_string(ycfg.stamina_conf_threshold) +
                ",\"iou\":" + std::to_string(ycfg.iou_threshold) + ",\"onnxThreads\":" +
                std::to_string(ycfg.onnx_threads) + ",\"ortCudaEnv\":" +
                (std::getenv("J2K_ORT_CUDA") != nullptr ? "\"set\"" : "\"unset\"") + "}");
        // #endregion
        const bool yolo_loaded = yolo_->load(ycfg);
        // #region agent log
        agent_log_c1a192_engine(
            "H56",
            "j2k_engine.cpp:init",
            "after_yolo_load",
            std::string("{\"loaded\":") + (yolo_loaded ? "true" : "false") + "}");
        // #endregion
        if (yolo_loaded) {
            telemetry_.cuda_active = yolo_->cuda_active();
            telemetry_.provider = yolo_->active_provider();

            // Config validation — print every key threshold at startup so users and
            // support can verify the loaded config without needing to read the JSON.
            std::fprintf(stdout,
                "[J2K CONFIG] imgsz=%d conf=%.2f player_conf=%.2f ball_conf=%.2f stam_conf=%.2f "
                "release_cue=%.0f tempo_speed=%.0f fadeaway_r2_high=%d "
                "ball_create_conf=%.2f player_create_conf=%.2f stam_create_conf=%.2f\n",
                ycfg.input_size,
                ycfg.conf_threshold, ycfg.player_conf_threshold,
                ycfg.ball_conf_threshold, ycfg.stamina_conf_threshold,
                release_cue_value_, tempo_speed_,
                fadeaway_when_r2_high_ ? 1 : 0,
                default_tracking_authority_config().ball.create_conf,
                default_tracking_authority_config().player.create_conf,
                default_tracking_authority_config().stamina.create_conf);
            std::fflush(stdout);

            // Detect whether TRT cache already exists so we can print the right message.
            // The warmup call always runs exactly once (single-threaded, no concurrent access).
            // On first launch TRT compiles the engine — this blocks the warmup thread for 2-10 min
            // while yolo_warmup_done_ stays false (inference gate closed, heavy=0).
            // On subsequent launches the engine loads from cache in <1 s → gate opens instantly.
            // NO timeout thread: a timeout that releases the gate while warmup is still inside
            // Session::Run() causes concurrent ONNX session access — the main loop blocks on
            // ORT's internal mutex until TRT finishes, indistinguishable from a hang.
            bool trt_cache_exists = false;
            {
                namespace fs = std::filesystem;
                std::error_code ec_chk;
                // Check the canonical trt_cache path relative to the script dir, not cwd,
                // since the Python process cwd may differ from python/.
                const fs::path trt_dir = fs::path(script_dir_) / "trt_cache";
                if (fs::exists(trt_dir, ec_chk) && !ec_chk) {
                    for (const auto& entry : fs::directory_iterator(trt_dir, ec_chk)) {
                        if (!ec_chk && entry.path().extension() == ".engine") {
                            trt_cache_exists = true;
                            break;
                        }
                    }
                }
            }
            if (!trt_cache_exists) {
                std::fprintf(stdout,
                             "[J2K YOLO] ================================================\n"
                             "[J2K YOLO]  FIRST RUN — TRT ENGINE BUILDING (~2-10 min)\n"
                             "[J2K YOLO]  Do NOT close the script. Boxes appear when done.\n"
                             "[J2K YOLO]  Next launch loads in <1 second from cache.\n"
                             "[J2K YOLO] ================================================\n");
                std::fflush(stdout);
            }

            yolo_warmup_done_.store(false, std::memory_order_release);

            // Warmup thread: runs one inference call to trigger TRT compilation (or load from cache).
            // Single-threaded ONNX access — no concurrent Run() calls, no ORT mutex contention.
            yolo_warmup_thread_ = std::thread([this, width, height, trt_cache_exists]() {
                const int ww = std::max(64, width);
                const int hh = std::max(64, height);
                std::vector<std::uint8_t> warm_bgr(static_cast<std::size_t>(ww) * static_cast<std::size_t>(hh) * 3U, 0U);
                std::array<float, 4> b{};
                std::array<float, 4> p{};
                std::array<float, 4> s{};
                bool hb = false;
                bool hp = false;
                bool hs = false;
                double ym = 0.0;
                double yp = 0.0;
                double yi = 0.0;
                double yo = 0.0;
                float bc = 0.f;
                float pc = 0.f;
                float sc = 0.f;
                if (!trt_cache_exists) {
                    std::fprintf(stdout, "[J2K YOLO] TRT compiling — please wait (progress every 30 s)...\n");
                    std::fflush(stdout);
                }
                try {
                    if (yolo_ && yolo_->loaded()) {
                        (void)yolo_->run(warm_bgr.data(),
                                         ww,
                                         hh,
                                         ww * 3,
                                         b,
                                         hb,
                                         p,
                                         hp,
                                         s,
                                         hs,
                                         &ym,
                                         &yp,
                                         &yi,
                                         &yo,
                                         nullptr,
                                         nullptr,
                                         false,
                                         &bc,
                                         &pc,
                                         &sc);
                        if (!trt_cache_exists) {
                            std::fprintf(stdout,
                                         "[J2K YOLO] TRT engine built (%.0f ms). Boxes active.\n",
                                         yi);
                            std::fflush(stdout);
                        }
                    }
                } catch (const std::exception& e) {
                    std::fprintf(stderr, "[J2K YOLO] Warmup error: %s — inference may be degraded.\n", e.what());
                    std::fflush(stderr);
                } catch (...) {
                    std::fprintf(stderr, "[J2K YOLO] Warmup unknown error — inference may be degraded.\n");
                    std::fflush(stderr);
                }
                // #region agent log
                debug_log_line_engine("pre-fix",
                                      "H11",
                                      "j2k_engine.cpp:init_warmup_thread",
                                      "yolo_async_warmup_done",
                                      std::string("{\"inferMs\":") + std::to_string(yi) +
                                          ",\"totalMs\":" + std::to_string(ym) +
                                          ",\"frameW\":" + std::to_string(ww) +
                                          ",\"frameH\":" + std::to_string(hh) + "}");
                // #endregion
                yolo_warmup_done_.store(true, std::memory_order_release);
            });

            // Progress watcher: spawned from init() (never from inside warmup lambda) so
            // shutdown() can join it safely without a data race.
            // Prints every 30 s during TRT compilation so the user knows to keep waiting.
            // This thread ONLY reads yolo_warmup_done_ — it never calls ONNX.
            yolo_warmup_timeout_thread_ = std::thread([this, trt_cache_exists]() {
                if (trt_cache_exists) {
                    return;  // Fast path — cache load is <1 s, no progress ticker needed.
                }
                constexpr int kTickMs = 100;
                constexpr int kReportEvery = 30'000;  // print every 30 s
                int elapsed = 0;
                while (!yolo_warmup_done_.load(std::memory_order_acquire)) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(kTickMs));
                    elapsed += kTickMs;
                    if (elapsed % kReportEvery == 0) {
                        std::fprintf(stdout,
                                     "[J2K YOLO] TRT still compiling... %d s elapsed. Please wait.\n",
                                     elapsed / 1000);
                        std::fflush(stdout);
                    }
                }
            });
        } else {
            std::fprintf(stderr,
                         "[J2K YOLO] FATAL: model load failed. Tracking is disabled until CUDA-capable ONNX session loads.\n");
            std::fprintf(stdout,
                         "[J2K YOLO] FATAL: model load failed — check ONNX model path and GPU/CUDA installation.\n");
            std::fflush(stdout);
            telemetry_.cuda_active = false;
            telemetry_.provider = "LOAD_FAILED";
        }
    } else if (!yolo_candidates.empty()) {
        std::fprintf(stderr, "[J2K YOLO] No ONNX model file on disk. Checked:\n");
        for (const auto& cand : yolo_candidates) {
            std::fprintf(stderr, "  - %s\n", cand.string().c_str());
        }
        const fs::path suggest = fs::path(script_dir_) / "models" / "j2k_yolo.onnx";
        std::fprintf(stderr,
                     "[J2K YOLO] Place an exported YOLO .onnx there, or set J2K_YOLO_ONNX.\n"
                     "  Example: scripts\\j2k.bat export_onnx path\\to\\best.pt \"%s\"\n",
                     suggest.string().c_str());
    }
}

void J2kEngine::shutdown() {
    // Signal the timeout thread to exit its sleep loop immediately.
    yolo_warmup_done_.store(true, std::memory_order_release);
    if (yolo_warmup_timeout_thread_.joinable()) {
        yolo_warmup_timeout_thread_.join();
    }
    if (yolo_warmup_thread_.joinable()) {
        yolo_warmup_thread_.join();
    }
    yolo_.reset();
    fire_zone_.reset();
    tempo_flick_.cancel();
}

void J2kEngine::reload_config_throttled() {
    if (script_dir_.empty()) {
        return;
    }
    // Avoid filesystem stat jitter every frame; config hot-reload still checks frequently enough.
    if (frame_n_ > 0 && (frame_n_ % 15) != 0) {
        return;
    }
    std::string path = script_dir_;
    if (!path.empty() && path.back() != '\\' && path.back() != '/') {
        path += '/';
    }
    path += "j2k_config.json";

    namespace fs = std::filesystem;
    const fs::path fp(path);
    std::error_code ec;
    if (!fs::exists(fp, ec) || ec) {
        return;
    }
    const auto mtime = fs::last_write_time(fp, ec);
    if (ec) {
        return;
    }
    const bool periodic = (frame_n_ == 0 || (frame_n_ % 120) == 0);
    const bool touched = (mtime != last_cfg_mtime_);
    if (!periodic && !touched) {
        return;
    }
    last_cfg_mtime_ = mtime;

    bool fa_r2 = fadeaway_when_r2_high_;
    if (read_j2k_cv_fadeaway_r2_high(path, fa_r2)) {
        fadeaway_when_r2_high_ = fa_r2;
    }

    float rc = release_cue_value_;
    float ts = tempo_speed_;
    if (!read_j2k_config_file(path, rc, ts)) {
        return;
    }
    release_cue_value_ = rc;
    tempo_speed_ = ts;
    int motion_tracking = 3;
    int motion_searching = 4;
    int motion_boosted = 2;
    (void)read_j2k_motion_thresholds(path, motion_tracking, motion_searching, motion_boosted);
    tracker_.set_motion_thresholds(motion_tracking, motion_searching, motion_boosted);
}

void J2kEngine::compute_player_stub(std::array<float, 4>& out_xyxy) const {
    const float w = static_cast<float>(std::max(1, width_));
    const float h = static_cast<float>(std::max(1, height_));
    const float cx = 0.5f * w;
    const float pw = w * 0.22f;
    out_xyxy[0] = cx - pw * 0.5f;
    out_xyxy[1] = h * 0.34f;
    out_xyxy[2] = cx + pw * 0.5f;
    out_xyxy[3] = h * 0.93f;
}

void J2kEngine::compute_fire_zone(const std::array<float, 4>& player_xyxy,
                                  std::array<float, 4>& zone_out) const {
    const float px1 = player_xyxy[0];
    const float py1 = player_xyxy[1];
    const float px2 = player_xyxy[2];
    const float py2 = player_xyxy[3];
    const float ph = std::max(8.f, py2 - py1);
    const float pcx = 0.5f * (px1 + px2);

    // release_cue 0: fire zone top (y1) flush with player top (py1); band extends downward into box.
    // 1..100: whole band shifts up (smaller y); max travel ~55% of player height.
    const float t = std::max(0.f, std::min(100.f, release_cue_value_)) / 100.f;
    const float zone_h = ph * 0.14f;
    const float shift_up = t * (0.55f * ph);
    const float y1 = py1 - shift_up;
    const float y2 = py1 + zone_h - shift_up;

    const float half_w = std::max(40.f, (px2 - px1) * 0.55f);
    zone_out[0] = pcx - half_w;
    zone_out[1] = std::max(0.f, y1);
    zone_out[2] = pcx + half_w;
    zone_out[3] = std::max(zone_out[1] + 4.f, y2);
}

void J2kEngine::push_perf_sample(double total_ms,
                                 double yolo_ms,
                                 double pre_ms,
                                 double infer_ms,
                                 double post_ms) {
    const int idx = perf_i_ % kPerfRing;
    perf_ring_total_[idx] = total_ms;
    perf_ring_yolo_[idx] = yolo_ms;
    perf_ring_pre_[idx] = pre_ms;
    perf_ring_infer_[idx] = infer_ms;
    perf_ring_post_[idx] = post_ms;
    ++perf_i_;
}

double J2kEngine::approx_p95(const double* ring) const {
    double buf[kPerfRing];
    int n = 0;
    for (int i = 0; i < kPerfRing; ++i) {
        const double v = ring[i];
        if (v > 0.001) {
            buf[n++] = v;
        }
    }
    if (n == 0) {
        return 0.0;
    }
    std::sort(buf, buf + n);
    const int idx = static_cast<int>(std::ceil(0.95 * static_cast<double>(n))) - 1;
    return buf[std::max(0, std::min(n - 1, idx))];
}

double J2kEngine::approx_p95_total() const {
    return approx_p95(perf_ring_total_);
}

void J2kEngine::log_perf_warn_if_needed(double total_ms,
                                        double yolo_ms,
                                        double track_ms,
                                        double visual_ms,
                                        bool square_held) {
    (void)total_ms;
    (void)yolo_ms;
    (void)track_ms;
    (void)visual_ms;
    (void)square_held;
    // Stderr PERF_WARN disabled: ONNX YOLO routinely pushes total_ms past the old 18ms
    // heuristic, which spammed the host as ERROR. `push_perf_sample` still records timing.
}

bool J2kEngine::should_process_heavy_frame() {
    const int base_cadence = std::max(1, yolo_every_n_);
    // Keep gameplay thread responsive until ONNX warmup finishes off-thread.
    if (!yolo_warmup_done_.load(std::memory_order_acquire)) {
        ++skipped_heavy_frames_;
        // #region agent log
        if ((frame_n_ % 30) == 0) {
            debug_log_line_engine("pre-fix",
                                  "H11",
                                  "j2k_engine.cpp:heavy_gate_cadence",
                                  "heavy_gate_waiting_async_warmup",
                                  std::string("{\"frame\":") + std::to_string(frame_n_) +
                                      ",\"baseCadenceEveryN\":" + std::to_string(base_cadence) + "}");
        }
        // #endregion
        return false;
    }
    // During an active shot hold run YOLO every frame (capped at cadence 2 to avoid GPU stalls).
    // Without this, fire zone uses ball positions up to base_cadence frames stale — at n=4 and
    // 22fps capture that is ~180ms of dead tracking during the most timing-critical moment.
    if (shot_active_ && prev_square_held_) {
        skipped_heavy_frames_ = 0;
        // Keep shot responsiveness but avoid forcing GPU overload when heavy inference is expensive.
        const int shot_cadence = std::clamp(std::max(base_cadence, yolo_every_n_runtime_), 2, 4);
        return (frame_n_ % std::max(1, shot_cadence)) == 0;
    }
    if (heavy_backoff_frames_ > 0) {
        --heavy_backoff_frames_;
        ++skipped_heavy_frames_;
        return false;
    }
    const double last_capture_ms = frame_intervals_ms_.empty() ? 0.0 : frame_intervals_ms_.back();
    if (last_capture_ms > 35.0) {
        heavy_backoff_frames_ = std::max(heavy_backoff_frames_, 1);
        yolo_every_n_runtime_ = std::max(yolo_every_n_runtime_, base_cadence + 1);
        ++skipped_heavy_frames_;
        return false;
    }
    if (yolo_every_n_runtime_ < base_cadence) {
        yolo_every_n_runtime_ = base_cadence;
    }
    if ((frame_n_ % 10) == 0) {
        double avg_ms = 16.67;
        if (!frame_intervals_ms_.empty()) {
            const double sum = std::accumulate(frame_intervals_ms_.begin(), frame_intervals_ms_.end(), 0.0);
            avg_ms = sum / static_cast<double>(frame_intervals_ms_.size());
        }
        const double budget_ms = std::max(4.0, avg_ms * 1.0);
        const double heavy_ms = std::max(1.0, last_yolo_ms_);
        const int required_cadence = std::max(
            base_cadence,
            static_cast<int>(std::ceil(heavy_ms / budget_ms)));
        const int cadence_max = base_cadence + 3;
        yolo_every_n_runtime_ = std::clamp(required_cadence, base_cadence, cadence_max);
    }
    const int cadence = std::max(base_cadence, yolo_every_n_runtime_);
    const bool process_heavy = (frame_n_ % cadence) == 0;
    if (!process_heavy) {
        ++skipped_heavy_frames_;
    } else {
        skipped_heavy_frames_ = 0;
    }
    if ((frame_n_ % 30) == 0) {
        double avg_ms = 0.0;
        if (!frame_intervals_ms_.empty()) {
            const double sum = std::accumulate(frame_intervals_ms_.begin(), frame_intervals_ms_.end(), 0.0);
            avg_ms = sum / static_cast<double>(frame_intervals_ms_.size());
        }
        // #region agent log
        debug_log_line_engine("pre-fix",
                              "H9",
                              "j2k_engine.cpp:heavy_gate_cadence",
                              "heavy_frame_gate",
                              std::string("{\"frame\":") + std::to_string(frame_n_) +
                                  ",\"intervalSamples\":" + std::to_string(frame_intervals_ms_.size()) +
                                  ",\"avgMs\":" + std::to_string(avg_ms) +
                                  ",\"baseCadenceEveryN\":" + std::to_string(base_cadence) +
                                  ",\"runtimeCadenceEveryN\":" + std::to_string(cadence) +
                                  ",\"processHeavy\":" + (process_heavy ? "true" : "false") +
                                  ",\"skipCounter\":" + std::to_string(skipped_heavy_frames_) + "}");
        // #endregion
    }
    return process_heavy;
}

void J2kEngine::record_frame_interval_ms() {
    const auto now = std::chrono::steady_clock::now();
    if (last_frame_time_.time_since_epoch().count() > 0) {
        const double elapsed_ms =
            std::chrono::duration<double, std::milli>(now - last_frame_time_).count();
        frame_intervals_ms_.push_back(elapsed_ms);
        if (frame_intervals_ms_.size() > 30) {
            frame_intervals_ms_.pop_front();
        }
    }
    last_frame_time_ = now;
}

void J2kEngine::draw_overlay(std::uint8_t* bgr, int w, int h, int stride,
                             const std::array<std::uint8_t, kGcvSize>& gcv) const {
    if (bgr == nullptr || w <= 0 || h <= 0 || stride < (w * 3)) return;

    // Colors are BGR (matching the frame layout).
    constexpr std::uint8_t kCyanB = 255, kCyanG = 255, kCyanR = 0;       // player
    constexpr std::uint8_t kOrangeB = 0,  kOrangeG = 140, kOrangeR = 255; // fire zone
    constexpr std::uint8_t kGoldB = 0,    kGoldG = 215, kGoldR = 255;     // stamina
    constexpr std::uint8_t kGreenB = 0,   kGreenG = 255, kGreenR = 80;    // ball
    constexpr int kThickness = 2;
    constexpr int kFireZoneThickness = 3;

    // Player rect (offset 12)
    {
        const auto box = decode_box_u16(gcv.data() + kGcvOutPlayerBox, w, h);
        if (box[2] > box[0] && box[3] > box[1]) {
            // Draw the tracked player box directly from packed GCV values.
            // This avoids dual-box visuals (pixel-fit box + tracked box) in debug videos.
            draw_rect_bgr(bgr, w, h, stride, box[0], box[1], box[2], box[3],
                          kCyanB, kCyanG, kCyanR, kThickness);
        }
    }
    // Fire zone rect (offset 32)
    {
        const auto box = decode_box_u16(gcv.data() + kGcvOutFireZone, w, h);
        if (box[2] > box[0] && box[3] > box[1]) {
            // Draw full computed fire zone to avoid undersized visual boxes.
            draw_rect_bgr(bgr, w, h, stride, box[0], box[1], box[2], box[3],
                          kOrangeB, kOrangeG, kOrangeR, kFireZoneThickness);
        }
    }
    // Stamina rect (offset 40)
    {
        const auto box = decode_box_u16(gcv.data() + kGcvOutStamBox, w, h);
        if (box[2] > box[0] && box[3] > box[1]) {
            draw_rect_bgr(bgr, w, h, stride, box[0], box[1], box[2], box[3],
                          kGoldB, kGoldG, kGoldR, kThickness);
        }
    }
    // Ball circle (offset 24)
    {
        const auto box = decode_box_u16(gcv.data() + kGcvOutBallBox, w, h);
        if (box[2] > box[0] && box[3] > box[1]) {
            const int cx = (box[0] + box[2]) / 2;
            const int cy = (box[1] + box[3]) / 2;
            const int rad = std::max(4, std::max(box[2] - box[0], box[3] - box[1]) / 2);
            draw_circle_bgr(bgr, w, h, stride, cx, cy, rad,
                            kGreenB, kGreenG, kGreenR, kThickness);
        }
    }
}

int J2kEngine::process_frame(const std::uint8_t* bgr,
                             int w,
                             int h,
                             int stride,
                             std::array<std::uint8_t, kGcvSize>& gcv_io) {
    if (bgr == nullptr || w <= 0 || h <= 0 || stride < (w * 3)) {
        return -1;
    }
    const auto t_frame0 = std::chrono::steady_clock::now();
    record_frame_interval_ms();

    std::uint8_t input_copy[24];
    std::memcpy(input_copy, gcv_io.data(), 24);

    const bool square_held = input_copy[kGcvByteSquare] >= 50;
    const bool rising = square_held && !prev_square_held_;
    const bool falling = (!square_held) && prev_square_held_;

    if (rising) {
        ++shot_id_;
        shot_active_ = true;
        release_fired_this_hold_ = false;
        visual_owner_ = false;
        shot_type_locked_ = false;
        square_held_frames_ = 0;
        fire_zone_.reset();
        tempo_flick_.cancel();
        const int r2a = input_copy[kGcvByteR2A];
        const int r2b = input_copy[kGcvByteR2B];
        shot_type_ = classify_shot_from_r2(r2a, r2b);
        latched_windup_ = shot_type_;
        have_shot_locked_ball_ = false;
        shot_locked_ball_gap_frames_ = 0;
        last_nofire_hint_ = "";
    }

    if (square_held && shot_active_) {
        ++square_held_frames_;
    }

    reload_config_throttled();

    double yolo_ms = 0.0;
    double yolo_pre_ms = 0.0;
    double yolo_infer_ms = 0.0;
    double yolo_post_ms = 0.0;
    std::array<float, 4> yolo_ball{};
    std::array<float, 4> yolo_player{};
    std::array<float, 4> yolo_stamina{};
    bool have_yolo_ball = false;
    bool have_yolo_player = false;
    bool have_yolo_stamina = false;
    float yolo_ball_conf = 0.f;
    float yolo_player_conf = 0.f;
    float yolo_stamina_conf = 0.f;
    std::vector<DetectionCandidate> player_candidates;
    std::vector<DetectionCandidate> ball_candidates;
    std::vector<DetectionCandidate> stamina_candidates;
    bool detections_fresh = false;
    const bool process_heavy = should_process_heavy_frame();
    bool heavy_attempted = false;
    if (process_heavy && yolo_ && yolo_->loaded()) {
        heavy_attempted = true;
        const auto multi = yolo_->run_multi(bgr, w, h, stride);
        yolo_pre_ms = multi.pre_ms;
        yolo_infer_ms = multi.infer_ms;
        yolo_post_ms = multi.post_ms;
        yolo_ms = (multi.total_ms > 0.0) ? multi.total_ms : (yolo_pre_ms + yolo_infer_ms + yolo_post_ms);
        if (multi.ok) {
            detections_fresh = true;
            player_candidates = multi.players;
            ball_candidates = multi.balls;
            stamina_candidates = multi.staminas;
            // NMS: suppress duplicate detections before the tracking authority scores them.
            // Ball uses a tighter threshold (0.35) to aggressively merge ghost duplicates.
            apply_candidate_nms(player_candidates, 0.45f);
            apply_candidate_nms(ball_candidates, 0.35f);
            apply_candidate_nms(stamina_candidates, 0.50f);
        }
    }
    authority_.update(player_candidates,
                      ball_candidates,
                      stamina_candidates,
                      detections_fresh,
                      shot_active_ && square_held,
                      frame_n_,
                      authority_state_);
    have_yolo_player = authority_state_.player.exists && !authority_state_.player.lost;
    have_yolo_ball = authority_state_.ball.exists && !authority_state_.ball.lost;
    have_yolo_stamina = authority_state_.stamina.exists && !authority_state_.stamina.lost;
    const bool authority_player_track_live =
        authority_state_.player.exists && !authority_state_.player.lost;
    const bool authority_stamina_track_live =
        authority_state_.stamina.exists && !authority_state_.stamina.lost;
    yolo_player = authority_state_.player.smooth_box;
    yolo_ball = authority_state_.ball.smooth_box;
    yolo_stamina = authority_state_.stamina.smooth_box;
    yolo_player_conf = authority_state_.player.confidence;
    yolo_ball_conf = authority_state_.ball.confidence;
    yolo_stamina_conf = authority_state_.stamina.confidence;
    // Raw (un-smoothed) ball box from the authority — only valid when visible this frame
    // (missed_frames == 0).  Used for shot timing to avoid EMA lag that causes the
    // displayed/tracked ball to appear 1-2 frames below its real position during the
    // rapid vertical ascent of a jump shot, which can make the ball miss the fire zone.
    const bool ball_raw_is_fresh = authority_state_.ball.visible;  // missed == 0
    const std::array<float, 4> yolo_ball_raw = authority_state_.ball.raw_box;
    // Require a minimum confidence on the raw detection before using it for shot timing.
    // A 0.10-conf raw box (just above the new create_conf floor) could fire the release
    // before the ball reaches apex if the detection is a floor reflection at the right Y.
    const bool have_yolo_ball_raw = have_yolo_ball && ball_raw_is_fresh &&
        yolo_ball_conf >= 0.20f &&
        (yolo_ball_raw[2] > yolo_ball_raw[0] + 2.f) &&
        (yolo_ball_raw[3] > yolo_ball_raw[1] + 2.f);
    if (heavy_attempted) {
        // Hard guardrail: insert minimal cooldown only on extreme spikes.
        if (yolo_ms > 80.0) {
            heavy_backoff_frames_ = std::max(heavy_backoff_frames_, 1);
            yolo_every_n_runtime_ = std::max(yolo_every_n_runtime_, std::max(yolo_every_n_ + 1, 4));
        }
    }
    if (yolo_ && yolo_->loaded()) {
        if (have_yolo_player) {
            player_yolo_miss_frames_ = 0;
        } else {
            ++player_yolo_miss_frames_;
            // Reacquire player sooner when stale: temporarily raise YOLO frequency.
            if (player_yolo_miss_frames_ > 3) {
                yolo_every_n_runtime_ = std::min(yolo_every_n_runtime_, 2);
            }
        }
        if (have_yolo_ball) {
            ball_yolo_miss_frames_ = 0;
        } else {
            ++ball_yolo_miss_frames_;
            // Ball loss: boost YOLO cadence to reacquire sooner (same logic as player).
            // Ball loss during a dribble/shot is more timing-critical than player loss.
            if (ball_yolo_miss_frames_ > 3) {
                yolo_every_n_runtime_ = std::min(yolo_every_n_runtime_, 2);
            }
        }
        if (have_yolo_stamina && !authority_stamina_track_live) {
            const float sw_live = yolo_stamina[2] - yolo_stamina[0];
            const float sh_live = yolo_stamina[3] - yolo_stamina[1];
            const float sar_live = sw_live / std::max(1.f, sh_live);
            const bool stamina_live_sane =
                (sw_live >= 8.f && sh_live >= 3.f && sar_live >= 1.2f && sar_live <= 24.f);
            if (!stamina_live_sane) {
                have_yolo_stamina = false;
            }
        }
        if (have_yolo_stamina) {
            const bool same_stamina =
                have_last_stamina_ &&
                box_center_dist(yolo_stamina, last_stamina_box_) <= 1.2f &&
                xyxy_iou(yolo_stamina, last_stamina_box_) >= 0.995f;
            stamina_static_heavy_frames_ = same_stamina ? (stamina_static_heavy_frames_ + 1) : 0;
            const bool reject_stale_live_stamina =
                (!authority_stamina_track_live) &&
                same_stamina && stamina_static_heavy_frames_ > kStaminaStaticHeavyMax &&
                player_yolo_miss_frames_ > 3;
            if (!reject_stale_live_stamina) {
                last_stamina_box_ = yolo_stamina;
                have_last_stamina_ = true;
                stamina_miss_frames_ = 0;
            } else {
                have_yolo_stamina = false;
                ++stamina_miss_frames_;
                if (stamina_miss_frames_ > kStaminaCarryHeavyMissMax) {
                    have_last_stamina_ = false;
                }
            }
        } else if (heavy_attempted) {
            ++stamina_miss_frames_;
            stamina_static_heavy_frames_ = 0;
            if (stamina_miss_frames_ > kStaminaCarryHeavyMissMax) {
                have_last_stamina_ = false;
            }
        }
    }

    const auto t_track0 = std::chrono::steady_clock::now();
    tracker_.track(bgr, w, h, stride);
    if (square_held) {
        tracker_.on_square_held_frame();
    }
    const auto t_track1 = std::chrono::steady_clock::now();
    const double track_ms =
        std::chrono::duration<double, std::milli>(t_track1 - t_track0).count();

    // Shot type is latched at the rising edge (classify_shot_from_r2 called in `rising` block above).
    // Lock it on the first held frame so mid-hold R2 changes cannot flip fadeaway ↔ normal.
    if (shot_active_ && square_held && !shot_type_locked_) {
        shot_type_locked_ = true;
        latched_windup_ = shot_type_;
    }

    const auto t_vis0 = std::chrono::steady_clock::now();
    std::array<float, 4> pbox{};
    const bool have_stamina_anchor =
        have_yolo_stamina || (have_last_stamina_ && stamina_miss_frames_ <= kStaminaCarryHeavyMissMax);
    const std::array<float, 4> stamina_anchor_box = have_yolo_stamina ? yolo_stamina : last_stamina_box_;
    const auto box_matches_stamina_anchor = [&](const std::array<float, 4>& box) -> bool {
        if (!have_stamina_anchor) {
            return true;
        }
        const float bx1 = box[0];
        const float by1 = box[1];
        const float bx2 = box[2];
        const float by2 = box[3];
        const float bw = std::max(1.f, bx2 - bx1);
        const float bh = std::max(1.f, by2 - by1);
        const float bcx = 0.5f * (bx1 + bx2);
        // Stamina bar lives near the player feet/lower body, not head/torso.
        const float b_anchor_y = by2 + bh * 0.02f;
        const float scx = 0.5f * (stamina_anchor_box[0] + stamina_anchor_box[2]);
        const float scy = 0.5f * (stamina_anchor_box[1] + stamina_anchor_box[3]);
        const float dx = std::abs(bcx - scx);
        const float dy = std::abs(b_anchor_y - scy);
        const float ref_x = std::max(28.f, bw * 0.90f);
        const float ref_y = std::max(24.f, bh * 0.30f);
        return (dx <= (ref_x * 2.60f) && dy <= (ref_y * 2.40f));
    };
    bool yolo_sane = have_yolo_player &&
                     (authority_player_track_live || runtime_player_box_sane(yolo_player, w, h));
    if (yolo_sane && !authority_player_track_live && !box_matches_stamina_anchor(yolo_player)) {
        yolo_sane = false;
    }
    bool refresh_stable_from_selected = yolo_sane;
    if (yolo_sane && !authority_player_track_live && have_last_stable_player_) {
        // Guard against sudden wrong locks (hoop/background) despite passing coarse sane checks.
        const float ph_last = std::max(1.f, last_stable_player_box_[3] - last_stable_player_box_[1]);
        const float jump = box_center_dist(yolo_player, last_stable_player_box_);
        const float ov = xyxy_iou(yolo_player, last_stable_player_box_);
        if (jump > (ph_last * 1.8f) && ov < 0.06f) {
            yolo_sane = false;
            refresh_stable_from_selected = false;
        }
    }
    const char* player_source = "stub";
    if (yolo_sane && !authority_player_track_live && have_last_stable_player_) {
        // Switch hysteresis: require multiple consistent frames before accepting a large target jump.
        const float ph_last = std::max(1.f, last_stable_player_box_[3] - last_stable_player_box_[1]);
        const float jump = box_center_dist(yolo_player, last_stable_player_box_);
        const float ov = xyxy_iou(yolo_player, last_stable_player_box_);
        if (jump > (ph_last * 0.95f) && ov < 0.15f) {
            const bool pending_same = (pending_player_confirm_frames_ > 0) &&
                                      (box_center_dist(pending_player_box_, yolo_player) <= (ph_last * 0.60f));
            if (pending_same) {
                ++pending_player_confirm_frames_;
            } else {
                pending_player_box_ = yolo_player;
                pending_player_confirm_frames_ = 1;
            }
            if (pending_player_confirm_frames_ < 2) {
                yolo_sane = false;
                refresh_stable_from_selected = false;
            } else {
                pending_player_confirm_frames_ = 0;
            }
        } else {
            pending_player_confirm_frames_ = 0;
        }
    } else if (!yolo_sane) {
        pending_player_confirm_frames_ = 0;
    }
    // Accept sane live YOLO player boxes even if strict plausibility rejects them.
    if (yolo_sane) {
        pbox = yolo_player;
        player_source = "yolo";
    } else if (have_last_stable_player_ &&
               (frame_n_ - last_stable_player_seen_frame_) <= (have_stamina_anchor ? 180 : 60) &&
               runtime_player_box_sane(last_stable_player_box_, w, h) &&
               box_matches_stamina_anchor(last_stable_player_box_)) {
        pbox = last_stable_player_box_;
        player_source = "stable_carry";
    } else if (have_yolo_player && have_stamina_anchor) {
        // Ported from the older Python tracker behavior: when live player is implausible
        // but stamina is present, prefer stamina-anchored proxy over raw tiny player boxes.
        const float sx1 = stamina_anchor_box[0];
        const float sy1 = stamina_anchor_box[1];
        const float sx2 = stamina_anchor_box[2];
        const float sy2 = stamina_anchor_box[3];
        const float sw = std::max(8.f, sx2 - sx1);
        const float sh = std::max(4.f, sy2 - sy1);
        const float cx = 0.5f * (sx1 + sx2);
        const float cy = 0.5f * (sy1 + sy2);
        const float sw_ratio = sw / std::max(1.f, static_cast<float>(w));
        const float sh_ratio = sh / std::max(1.f, static_cast<float>(h));
        const float cy_ratio = cy / std::max(1.f, static_cast<float>(h));
        const bool stamina_plausible_proxy = (sw_ratio >= 0.030f && sw_ratio <= 0.28f &&
                                              sh_ratio >= 0.008f && sh_ratio <= 0.12f &&
                                              cy_ratio >= 0.56f);
        bool stamina_near_last = true;
        if (have_last_stable_player_) {
            const float lcx = 0.5f * (last_stable_player_box_[0] + last_stable_player_box_[2]);
            stamina_near_last = std::abs(cx - lcx) <= (static_cast<float>(w) * 0.24f);
        }
        if (stamina_plausible_proxy && stamina_near_last) {
            const float sw_proxy = std::clamp(sw, static_cast<float>(w) * 0.035f, static_cast<float>(w) * 0.14f);
            const float sh_proxy = std::clamp(sh, static_cast<float>(h) * 0.008f, static_cast<float>(h) * 0.055f);
            const float ph = std::clamp(sh_proxy * 4.9f, 86.f, static_cast<float>(h) * 0.38f);
            const float pw = std::clamp(sw_proxy * 1.18f, 24.f, static_cast<float>(w) * 0.16f);
            const float bottom = std::clamp(sy1 - std::max(5.f, sh_proxy * 0.30f), ph + 1.f, static_cast<float>(h - 1));
            pbox = {
                std::max(0.f, cx - pw * 0.5f),
                std::max(0.f, bottom - ph),
                std::min(static_cast<float>(w - 1), cx + pw * 0.5f),
                bottom};
            player_source = "stamina_proxy_from_bad_player";
            refresh_stable_from_selected = true;
        } else if (have_last_stable_player_ && runtime_player_box_sane(last_stable_player_box_, w, h)) {
            pbox = last_stable_player_box_;
            player_source = "stable_carry_reject_bad_stamina_proxy";
        } else {
            pbox = yolo_player;
            player_source = "yolo_raw_fallback_reject_bad_stamina_proxy";
        }
    } else if (have_yolo_player) {
        // Prefer raw YOLO fallback over center stub when a player box exists but fails strict sane gates.
        pbox = yolo_player;
        player_source = "yolo_raw_fallback";
    } else if (have_stamina_anchor) {
        const float sx1 = stamina_anchor_box[0];
        const float sy1 = stamina_anchor_box[1];
        const float sx2 = stamina_anchor_box[2];
        const float sy2 = stamina_anchor_box[3];
        const float sw = std::max(8.f, sx2 - sx1);
        const float sh = std::max(4.f, sy2 - sy1);
        const float cx = 0.5f * (sx1 + sx2);
        const float sw_ratio = sw / std::max(1.f, static_cast<float>(w));
        const float sh_ratio = sh / std::max(1.f, static_cast<float>(h));
        const float cy_ratio = (0.5f * (sy1 + sy2)) / std::max(1.f, static_cast<float>(h));
        const bool stamina_plausible_proxy = (sw_ratio >= 0.030f && sw_ratio <= 0.22f &&
                                              sh_ratio >= 0.006f && sh_ratio <= 0.08f &&
                                              cy_ratio >= 0.52f && cy_ratio <= 0.98f);
        bool stamina_near_last = true;
        if (have_last_stable_player_) {
            const float lcx = 0.5f * (last_stable_player_box_[0] + last_stable_player_box_[2]);
            stamina_near_last = std::abs(cx - lcx) <= (static_cast<float>(w) * 0.20f);
        }
        if (stamina_plausible_proxy && stamina_near_last) {
            const float ph = std::clamp(sh * 8.0f, 120.f, static_cast<float>(h) * 0.60f);
            const float pw = std::clamp(sw * 1.6f, 32.f, static_cast<float>(w) * 0.30f);
            const float bottom = std::clamp(sy1 - 4.f, ph + 1.f, static_cast<float>(h - 1));
            pbox = {
                std::max(0.f, cx - pw * 0.5f),
                std::max(0.f, bottom - ph),
                std::min(static_cast<float>(w - 1), cx + pw * 0.5f),
                bottom};
            player_source = "stamina_proxy";
            refresh_stable_from_selected = true;
        } else if (stamina_plausible_proxy) {
            // Relaxed fallback: still anchor to visible stamina to avoid player-box dropouts.
            const float ph = std::clamp(sh * 7.2f, 92.f, static_cast<float>(h) * 0.44f);
            const float pw = std::clamp(sw * 1.35f, 26.f, static_cast<float>(w) * 0.18f);
            const float bottom = std::clamp(sy1 - 3.f, ph + 1.f, static_cast<float>(h - 1));
            pbox = {
                std::max(0.f, cx - pw * 0.5f),
                std::max(0.f, bottom - ph),
                std::min(static_cast<float>(w - 1), cx + pw * 0.5f),
                bottom};
            player_source = "stamina_proxy_relaxed";
            refresh_stable_from_selected = true;
        } else if (have_last_stable_player_ && runtime_player_box_sane(last_stable_player_box_, w, h)) {
            pbox = last_stable_player_box_;
            player_source = "stable_carry_reject_bad_stamina_anchor";
        } else if (have_yolo_player && runtime_player_box_sane(yolo_player, w, h)) {
            pbox = yolo_player;
            player_source = "yolo_raw_fallback_bad_stamina_anchor";
            refresh_stable_from_selected = true;
        } else {
            // Last-resort fallback: keep a small player proxy above stamina rather than dropping out.
            const float ph = std::clamp(sh * 6.4f, 84.f, static_cast<float>(h) * 0.40f);
            const float pw = std::clamp(sw * 1.20f, 22.f, static_cast<float>(w) * 0.16f);
            const float bottom = std::clamp(sy1 - 2.f, ph + 1.f, static_cast<float>(h - 1));
            pbox = {
                std::max(0.f, cx - pw * 0.5f),
                std::max(0.f, bottom - ph),
                std::min(static_cast<float>(w - 1), cx + pw * 0.5f),
                bottom};
            player_source = "stamina_proxy_last_resort";
            refresh_stable_from_selected = true;
        }
    } else {
        if (have_last_stable_player_ &&
            (frame_n_ - last_stable_player_seen_frame_) <= 90 &&
            runtime_player_box_sane(last_stable_player_box_, w, h)) {
            pbox = last_stable_player_box_;
            player_source = "stable_carry_no_stamina";
        } else {
            have_last_stable_player_ = false;
            pbox = {0.f, 0.f, 0.f, 0.f};
            player_source = "none";
        }
    }
    if (refresh_stable_from_selected && runtime_player_box_sane(pbox, w, h)) {
        last_stable_player_box_ = pbox;
        have_last_stable_player_ = true;
        last_stable_player_seen_frame_ = frame_n_;
    }
    if ((frame_n_ % 30) == 0) {
        const float pw = pbox[2] - pbox[0];
        const float ph = pbox[3] - pbox[1];
        const float yw = have_yolo_player ? (yolo_player[2] - yolo_player[0]) : 0.f;
        const float yh = have_yolo_player ? (yolo_player[3] - yolo_player[1]) : 0.f;
        // #region agent log
        debug_log_line_engine("pre-fix",
                              "H8",
                              "j2k_engine.cpp:player_source",
                              "engine_player_source",
                              std::string("{\"frame\":") + std::to_string(frame_n_) +
                                  ",\"haveYoloPlayer\":" + (have_yolo_player ? "true" : "false") +
                                  ",\"yoloSane\":" + (yolo_sane ? "true" : "false") +
                                  ",\"haveLastStable\":" + (have_last_stable_player_ ? "true" : "false") +
                                  ",\"source\":\"" + player_source + "\"" +
                                  ",\"yoloW\":" + std::to_string(yw) +
                                  ",\"yoloH\":" + std::to_string(yh) +
                                  ",\"playerW\":" + std::to_string(pw) +
                                  ",\"playerH\":" + std::to_string(ph) + "}");
        // #endregion
    }
    // Stamina bar is the ground truth for player X position.
    // Always snap the player box horizontally to the stamina bar centre when available —
    // regardless of whether YOLO is also active.  This makes the box move left/centre/right
    // as the player moves and drift back to centre when standing still, matching the stamina
    // bar which is always rendered directly below the player sprite.
    if ((pbox[2] > pbox[0]) && (pbox[3] > pbox[1]) && have_stamina_anchor) {
        const float sx1 = stamina_anchor_box[0];
        const float sy1 = stamina_anchor_box[1];
        const float sx2 = stamina_anchor_box[2];
        const float sy2 = stamina_anchor_box[3];
        const float scx = 0.5f * (sx1 + sx2);  // stamina centre — authoritative X
        const float sh = std::max(4.f, sy2 - sy1);
        const float cur_w = std::max(1.f, pbox[2] - pbox[0]);
        const float cur_h = std::max(1.f, pbox[3] - pbox[1]);
        float target_h = cur_h;
        if (have_last_stable_player_) {
            const float lh = std::max(1.f, last_stable_player_box_[3] - last_stable_player_box_[1]);
            target_h = 0.70f * target_h + 0.30f * lh;
        }
        target_h = std::clamp(target_h, 86.f, static_cast<float>(h) * 0.42f);
        float target_w = cur_w;
        target_w = std::clamp(target_w, 24.f, target_h * 0.64f);

        // Use stamina centre directly as the player box X — no blending with YOLO X.
        const float anchor_cx = scx;
        const float bottom = std::clamp(sy1 - std::max(3.f, sh * 0.22f),
                                        target_h + 1.f,
                                        static_cast<float>(h - 1));
        pbox = {
            std::max(0.f, anchor_cx - target_w * 0.5f),
            std::max(0.f, bottom - target_h),
            std::min(static_cast<float>(w - 1), anchor_cx + target_w * 0.5f),
            bottom};
    }
    // Clamp player box to valid frame bounds before AR check.
    // In close-up scenes the YOLO model may regress y3 > frame_height
    // (player body extrapolated below the visible area).  The EMA in
    // yolo_onnx then drifts toward that value.  Without clamping here,
    // packing truncates y3 to fh while y1 stays near fh, collapsing the
    // box height and falsely reporting AR < 1.0.
    {
        const float fw_ = static_cast<float>(width_) - 1.f;
        const float fh_ = static_cast<float>(height_) - 1.f;
        pbox[0] = std::max(0.f, std::min(fw_, pbox[0]));
        pbox[1] = std::max(0.f, std::min(fh_, pbox[1]));
        pbox[2] = std::max(0.f, std::min(fw_, pbox[2]));
        pbox[3] = std::max(0.f, std::min(fh_, pbox[3]));
    }
    // Enforce portrait aspect ratio on the player box.
    // Primary: extend top upward (anchored at feet).  If the player is near the top of
    // the frame and the top clamps at y=0, apply the remaining deficit to the bottom.
    // Scoring excuses frames where even full-frame height is too short for 1.30 AR
    // (close-up shots where the player is wider than frame_h/1.30).
    if (!authority_player_track_live) {
        const float pbw = pbox[2] - pbox[0];
        const float pbh = pbox[3] - pbox[1];
        const float kMinPortraitRatio = have_stamina_anchor ? 1.18f : 1.40f;
        if (pbw > 4.f && pbh < pbw * kMinPortraitRatio) {
            const float target_h  = pbw * kMinPortraitRatio;
            const float new_top   = std::max(0.f, pbox[3] - target_h);
            const float achieved_h = pbox[3] - new_top;
            pbox[1] = new_top;
            if (achieved_h < target_h) {
                // Top was clamped — push bottom down for the remaining deficit.
                const float deficit = target_h - achieved_h;
                pbox[3] = std::min(static_cast<float>(height_) - 1.f, pbox[3] + deficit);
            }
        }
    }

    // Sanity-gate the player box before using it to position the fire zone.
    // A box that is too short (<5% of frame height) is an ankle-lock or noise;
    // one that is too tall (>70% of frame height) is a misdetection of a large element.
    // Either case produces a wildly mis-positioned fire zone → ball never enters → no fire.
    const float pbox_ph = pbox[3] - pbox[1];
    const bool player_box_height_valid =
        (pbox_ph >= static_cast<float>(height_) * 0.05f) &&
        (pbox_ph <= static_cast<float>(height_) * 0.70f);

    std::array<float, 4> zbox{};
    if ((pbox[2] > pbox[0]) && (pbox[3] > pbox[1]) && player_box_height_valid) {
        compute_fire_zone(pbox, zbox);
        // Widen the zone vertically during active shot — the 14%-of-height default band
        // is too thin to reliably catch the ball when EMA lag + YOLO cadence both push
        // the tracked ball position below the real apex. 1.8× expansion gives ~25% of
        // player height without compromising timing precision in normal gameplay.
        if (shot_active_ && square_held) {
            const float cy = 0.5f * (zbox[1] + zbox[3]);
            const float hh = (zbox[3] - zbox[1]) * 0.5f * 1.8f;
            zbox[1] = std::max(0.f, cy - hh);
            zbox[3] = std::min(static_cast<float>(h - 1), cy + hh);
        }
        have_last_fire_zone_ = true;
        last_fire_zone_box_ = zbox;
    } else if (shot_active_ && square_held && have_last_fire_zone_) {
        // Player temporarily lost mid-shot — hold the last known fire zone so release
        // detection can still trigger.  Without this, fz.release_now is permanently false
        // once the player box drops, causing a systematic "TIMING LATE" on every shot.
        zbox = last_fire_zone_box_;
    } else {
        zbox = {0.f, 0.f, 0.f, 0.f};
        have_last_fire_zone_ = false;
    }

    const bool shot_context = shot_active_ && square_held;
    std::array<float, 4> live_ball_box{};
    bool have_live_ball = false;
    bool live_ball_from_yolo = false;
    if (have_yolo_ball) {
        // During shot, use the raw (un-smoothed) YOLO detection for the timing path
        // so fire-zone checks see the ball's actual position, not the EMA-lagged one.
        // On non-shot frames keep the smooth box for display stability.
        if (shot_context && have_yolo_ball_raw) {
            live_ball_box = yolo_ball_raw;
        } else {
            live_ball_box = yolo_ball;
        }
        have_live_ball = true;
        live_ball_from_yolo = true;
        // Re-anchor optical-flow tracker to this YOLO position.  After a camera
        // cut the tracker's EMA stays at the pre-cut location; seeding it here
        // lets optflow follow the ball correctly on subsequent frames so it does
        // not block the short-carry with a stale out-of-zone position.
        {
            const auto& seed_box = (shot_context && have_yolo_ball_raw) ? yolo_ball_raw : yolo_ball;
            const float bcx = 0.5f * (seed_box[0] + seed_box[2]);
            const float bcy = 0.5f * (seed_box[1] + seed_box[3]);
            const float br  = 0.5f * std::max(seed_box[2] - seed_box[0],
                                              seed_box[3] - seed_box[1]);
            tracker_.seed_from_yolo(bcx, bcy, br);
        }
    }
    const bool had_live_ball_pre_sane = have_live_ball;
    const float pre_sane_bw = had_live_ball_pre_sane ? (live_ball_box[2] - live_ball_box[0]) : 0.f;
    const float pre_sane_bh = had_live_ball_pre_sane ? (live_ball_box[3] - live_ball_box[1]) : 0.f;
    const bool pre_sane_ok =
        had_live_ball_pre_sane ? runtime_ball_box_sane(live_ball_box, w, h) : false;
    if (had_live_ball_pre_sane && !pre_sane_ok) {
        have_live_ball = false;
    }
    if ((frame_n_ % 30) == 0) {
        const float bw = have_live_ball ? (live_ball_box[2] - live_ball_box[0]) : 0.f;
        const float bh = have_live_ball ? (live_ball_box[3] - live_ball_box[1]) : 0.f;
        // #region agent log
        debug_log_line_engine("pre-fix",
                              "H6",
                              "j2k_engine.cpp:live_ball_gate",
                              "engine_live_ball_gate",
                              std::string("{\"frame\":") + std::to_string(frame_n_) +
                                  ",\"shotContext\":" + (shot_context ? "true" : "false") +
                                  ",\"hadLivePreSane\":" + (had_live_ball_pre_sane ? "true" : "false") +
                                  ",\"preSaneOk\":" + (pre_sane_ok ? "true" : "false") +
                                  ",\"haveLiveBall\":" + (have_live_ball ? "true" : "false") +
                                  ",\"liveFromYolo\":" + (live_ball_from_yolo ? "true" : "false") +
                                  ",\"preBallW\":" + std::to_string(pre_sane_bw) +
                                  ",\"preBallH\":" + std::to_string(pre_sane_bh) +
                                  ",\"liveBallW\":" + std::to_string(bw) +
                                  ",\"liveBallH\":" + std::to_string(bh) + "}");
        // #endregion
    }
    if (have_live_ball) {
        if (have_last_live_ball_) {
            prev_live_ball_box_ = last_live_ball_box_;
            have_prev_live_ball_ = true;
        }
        last_live_ball_box_ = live_ball_box;
        have_last_live_ball_ = true;
        last_live_ball_frame_ = frame_n_;
    }
    // Track YOLO-confirmed ball separately so the carry never uses a stale
    // optical-flow position (the tracker holds for 96 frames after last motion).
    if (live_ball_from_yolo) {
        last_yolo_ball_box_ = yolo_ball;
        have_last_yolo_ball_ = true;
        last_yolo_ball_frame_ = frame_n_;
    }

    std::array<float, 4> ball_box_store{};
    const float* ball_ptr = nullptr;
    bool have_ball = false;
    if (shot_context) {
        bool accept_live = have_live_ball;
        const bool live_plausible = have_live_ball && ball_box_plausible_for_apex(pbox, live_ball_box);
        const bool live_in_zone =
            have_live_ball && ball_box_in_player_search_zone(pbox, live_ball_box, true);
        const float ph = std::max(1.f, pbox[3] - pbox[1]);
        const float live_cy = have_live_ball ? (0.5f * (live_ball_box[1] + live_ball_box[3])) : 0.f;
        const bool live_not_feet = have_live_ball && (live_cy <= (pbox[3] - ph * 0.08f));
        const bool live_zone_strict = live_in_zone && live_not_feet;
        if (have_live_ball && !have_shot_locked_ball_ && !(live_plausible || live_zone_strict)) {
            // Do not start a shot lock from clearly bad detections.
            accept_live = false;
        }
        if (have_live_ball && have_shot_locked_ball_) {
            if (!(live_plausible || live_zone_strict)) {
                constexpr float kJumpMaxFracPh = 1.20f;
                if (box_center_dist(shot_locked_ball_box_, live_ball_box) > ph * kJumpMaxFracPh &&
                    xyxy_iou(shot_locked_ball_box_, live_ball_box) < 0.05f) {
                    accept_live = false;
                }
            }
        }
        if (accept_live && have_live_ball) {
            shot_locked_ball_box_ = live_ball_box;
            have_shot_locked_ball_ = true;
            shot_locked_ball_gap_frames_ = 0;
        } else if (have_shot_locked_ball_) {
            ++shot_locked_ball_gap_frames_;
            constexpr int kShotLockMaxGap = 14;  // was 10; aligned with extended carry window
            if (have_last_live_ball_ && have_prev_live_ball_) {
                std::array<float, 4> pred{};
                if (predict_ball_box_from_live_history(last_live_ball_box_,
                                                       prev_live_ball_box_,
                                                       shot_locked_ball_gap_frames_,
                                                       pbox,
                                                       true,
                                                       pred)) {
                    shot_locked_ball_box_ = pred;
                }
            }
            if (shot_locked_ball_gap_frames_ > kShotLockMaxGap) {
                have_shot_locked_ball_ = false;
                shot_locked_ball_gap_frames_ = 0;
            }
        }

        if (have_shot_locked_ball_ && runtime_ball_box_sane(shot_locked_ball_box_, w, h)) {
            ball_box_store = shot_locked_ball_box_;
            ball_ptr = ball_box_store.data();
            have_ball = true;
        } else if (have_live_ball && (live_ball_from_yolo || (live_plausible || live_zone_strict))) {
            ball_box_store = live_ball_box;
            ball_ptr = ball_box_store.data();
            have_ball = true;
        }
    } else {
        // Non-shot path.
        const auto nonshot_zone_check = [&](const std::array<float, 4>& candidate) -> bool {
            // Require ball within expanded player zone (xs=0.80, ts=0.72, bs=0.45).
            // xs widened from 0.60 to 0.80 to cover dribble-right/left borderline cases
            // where the stub player box under-estimates the reachable zone width.
            const float ph  = std::max(1.f, pbox[3] - pbox[1]);
            const float pw  = std::max(1.f, pbox[2] - pbox[0]);
            const float bcx = 0.5f * (candidate[0] + candidate[2]);
            const float bcy = 0.5f * (candidate[1] + candidate[3]);
            return (bcx >= pbox[0] - pw * 0.80f && bcx <= pbox[2] + pw * 0.80f &&
                    bcy >= pbox[1] - ph * 0.72f && bcy <= pbox[3] + ph * 0.45f);
        };

        if (have_live_ball && nonshot_zone_check(live_ball_box)) {
            ball_box_store = live_ball_box;
            ball_ptr = ball_box_store.data();
            have_ball = true;
        } else if (!have_live_ball && have_last_live_ball_) {
            // Carry path: YOLO missed. Try velocity prediction first (ball moves
            // smoothly between YOLO frames instead of freezing at last known spot).
            // Fall back to static carry if prediction is unavailable or out-of-zone.
            const int gap = frame_n_ - last_live_ball_frame_;
            // Keep non-shot carry long enough to bridge short/medium YOLO dropouts
            // seen in live gameplay captures (especially after movement bursts).
            constexpr int kNonshotCarryMax = 36;
            if (gap > 0 && gap <= kNonshotCarryMax) {
                bool carry_ok = false;
                if (have_prev_live_ball_) {
                    std::array<float, 4> pred{};
                    if (predict_ball_box_from_live_history(last_live_ball_box_,
                                                           prev_live_ball_box_,
                                                           gap,
                                                           pbox,
                                                           false,
                                                           pred) &&
                        nonshot_zone_check(pred)) {
                        ball_box_store = pred;
                        ball_ptr = ball_box_store.data();
                        have_ball = true;
                        carry_ok = true;
                    }
                }
                if (!carry_ok && gap <= 20 && nonshot_zone_check(last_live_ball_box_)) {
                    // Static carry fallback when velocity prediction is unavailable or
                    // predicts out-of-zone (e.g. ball stationary, single detection history).
                    ball_box_store = last_live_ball_box_;
                    ball_ptr = ball_box_store.data();
                    have_ball = true;
                }
            }
        }
    }
    if (!have_ball && have_last_live_ball_) {
        const int gap = frame_n_ - last_live_ball_frame_;
        if (gap > 0 && gap <= 24 && runtime_ball_box_sane(last_live_ball_box_, w, h)) {
            ball_box_store = last_live_ball_box_;
            ball_ptr = ball_box_store.data();
            have_ball = true;
        }
    }
    if (!have_ball && tracker_.have_ball()) {
        const auto& motion_ball = tracker_.ball_box();
        if (runtime_ball_box_sane(motion_ball, w, h)) {
            const float bcx = 0.5f * (motion_ball[0] + motion_ball[2]);
            const float bcy = 0.5f * (motion_ball[1] + motion_ball[3]);
            const float pcx = 0.5f * (pbox[0] + pbox[2]);
            const float pcy = 0.5f * (pbox[1] + pbox[3]);
            const float nx = bcx / std::max(1.f, static_cast<float>(w));
            const float ny = bcy / std::max(1.f, static_cast<float>(h));
            const bool in_court_core = (nx >= 0.04f && nx <= 0.96f && ny >= 0.06f && ny <= 0.96f);
            (void)pcx;
            (void)pcy;
            const bool near_player_motion = in_court_core;
            if (near_player_motion) {
                ball_box_store = motion_ball;
                ball_ptr = ball_box_store.data();
                have_ball = true;
            }
        }
    }
    // Do not synthesize fake player-anchored balls when detection is missing.
    // Authority carries/predicts short misses; beyond that, leave ball absent.
    if (have_ball && last_ball_seen_frame_ > 0 && ball_ptr != nullptr) {
        const float ph = std::max(1.f, pbox[3] - pbox[1]);
        const float max_step = std::max(20.f, ph * 1.10f);
        const float cx = 0.5f * (ball_ptr[0] + ball_ptr[2]);
        const float cy = 0.5f * (ball_ptr[1] + ball_ptr[3]);
        const float dx = cx - last_ball_cx_;
        const float dy = cy - last_ball_cy_;
        const float dist = std::sqrt(dx * dx + dy * dy);
        if (dist > max_step) {
            const float scale = max_step / dist;
            const float clamped_cx = last_ball_cx_ + dx * scale;
            const float clamped_cy = last_ball_cy_ + dy * scale;
            const float bw = std::max(6.f, ball_ptr[2] - ball_ptr[0]);
            const float bh = std::max(6.f, ball_ptr[3] - ball_ptr[1]);
            ball_box_store = {
                std::clamp(clamped_cx - bw * 0.5f, 0.f, static_cast<float>(w) - 1.f),
                std::clamp(clamped_cy - bh * 0.5f, 0.f, static_cast<float>(h) - 1.f),
                std::clamp(clamped_cx + bw * 0.5f, 0.f, static_cast<float>(w) - 1.f),
                std::clamp(clamped_cy + bh * 0.5f, 0.f, static_cast<float>(h) - 1.f)};
            ball_ptr = ball_box_store.data();
        }
    }
    if (have_ball) {
        last_ball_cx_ = 0.5f * (ball_ptr[0] + ball_ptr[2]);
        last_ball_cy_ = 0.5f * (ball_ptr[1] + ball_ptr[3]);
        last_ball_seen_frame_ = frame_n_;
    }
    if ((frame_n_ % 30) == 0) {
        const float bw = (have_ball && ball_ptr != nullptr) ? (ball_ptr[2] - ball_ptr[0]) : 0.f;
        const float bh = (have_ball && ball_ptr != nullptr) ? (ball_ptr[3] - ball_ptr[1]) : 0.f;
        // #region agent log
        debug_log_line_engine("pre-fix",
                              "H7",
                              "j2k_engine.cpp:final_ball_choice",
                              "engine_final_ball_choice",
                              std::string("{\"frame\":") + std::to_string(frame_n_) +
                                  ",\"haveBallFinal\":" + (have_ball ? "true" : "false") +
                                  ",\"haveShotLock\":" + (have_shot_locked_ball_ ? "true" : "false") +
                                  ",\"ballW\":" + std::to_string(bw) +
                                  ",\"ballH\":" + std::to_string(bh) + "}");
        // #endregion
    }

    // YOLO often drops the ball for 1–6 frames; keep release geometry from last centroid briefly.
    // With n=2 shot-active cadence at 22fps, 3 frames ≈ 136ms — too short for a single dropped
    // inference cycle. 6 frames ≈ 273ms keeps fire-zone geometry valid across one missed YOLO run.
    std::array<float, 4> ball_release_box{};
    const float* ball_for_fz = ball_ptr;
    bool have_ball_fz = have_ball;
    if (!have_ball && shot_context && last_ball_seen_frame_ > 0 &&
        (frame_n_ - last_ball_seen_frame_) <= 6) {
        const float rr =
            std::max(10.f, std::min(32.f, 0.02f * static_cast<float>(std::min(w, h))));
        ball_release_box[0] = last_ball_cx_ - rr;
        ball_release_box[1] = last_ball_cy_ - rr;
        ball_release_box[2] = last_ball_cx_ + rr;
        ball_release_box[3] = last_ball_cy_ + rr;
        ball_for_fz = ball_release_box.data();
        have_ball_fz = true;
    }

    const bool have_zone = (zbox[2] > zbox[0]) && (zbox[3] > zbox[1]);
    FireZoneResult fz = fire_zone_.update(
        ball_for_fz,
        have_ball_fz,
        zbox.data(),
        have_zone,
        square_held,
        shot_active_,
        shot_type_locked_,
        release_fired_this_hold_);

    last_visual_reason_ = fz.reason;
    last_inside_fire_zone_ = fz.inside_now;
    last_entered_fire_zone_ = fz.entered;
    visual_owner_ = fz.visual_owner;

    if (square_held && shot_active_ && !release_fired_this_hold_) {
        last_nofire_hint_ = fz.reason;
    }

    // Per-frame shot diagnostics — set J2K_SHOT_DEBUG=1 to enable.
    // Printed every frame while square is held so post-hoc analysis can pinpoint exactly
    // which frame the ball entered the zone, why it was rejected, etc.
    {
        static int s_shot_debug = -1;
        if (s_shot_debug < 0) {
            char ev[8]{};
            s_shot_debug = (GetEnvironmentVariableA("J2K_SHOT_DEBUG", ev, sizeof(ev)) > 0 &&
                            (ev[0] == '1')) ? 1 : 0;
        }
        if (s_shot_debug == 1 && shot_active_) {
            const char* ball_src = have_ball
                ? (live_ball_from_yolo ? (have_yolo_ball_raw ? "raw" : "smooth")
                                       : "carry")
                : "none";
            std::fprintf(stdout,
                "[J2K SHOT] f=%d id=%d sq=%d sqF=%d type=%s locked=%d "
                "ball=%d bsrc=%s bcy=%.0f "
                "fz=[%.0f,%.0f] hzone=%d inside=%d entered=%d fired=%d "
                "reason=%s player=%s stam=%d\n",
                frame_n_, shot_id_,
                square_held ? 1 : 0, square_held_frames_,
                (latched_windup_ == ShotType::Fadeaway) ? "fade" :
                    (latched_windup_ == ShotType::Nodip) ? "nodip" : "norm",
                shot_type_locked_ ? 1 : 0,
                have_ball ? 1 : 0, ball_src,
                have_ball && ball_ptr ? 0.5f * (ball_ptr[1] + ball_ptr[3]) : -1.f,
                zbox[1], zbox[3], have_zone ? 1 : 0,
                fz.inside_now ? 1 : 0, fz.entered ? 1 : 0,
                release_fired_this_hold_ ? 1 : 0,
                fz.reason ? fz.reason : "",
                player_source ? player_source : "",
                have_stamina_anchor ? 1 : 0);
        }
    }

    const auto t_vis1 = std::chrono::steady_clock::now();
    const double visual_ms =
        std::chrono::duration<double, std::milli>(t_vis1 - t_vis0).count();

    if (fz.release_now) {
        // Normal: s0_flick_y = -1 → flick -100. Fadeaway: s1_flick_y = +1 → flick +100.
        const int flick_sign = (latched_windup_ == ShotType::Fadeaway) ? 1 : -1;
        tempo_flick_.start(flick_sign, tempo_speed_);
        release_fired_this_hold_ = true;
    }

    const auto flick_pair = tempo_flick_.update();
    const float flick_val = flick_pair.first;
    const bool flick_active = flick_pair.second;

    float windup = 0.f;
    if (shot_active_ && square_held && shot_type_locked_ && !release_fired_this_hold_ &&
        !flick_active) {
        windup = (latched_windup_ == ShotType::Fadeaway) ? -100.f : 100.f;
    }

    float stick = 0.f;
    if (flick_active) {
        stick = flick_val;
    } else if (shot_active_ && square_held && shot_type_locked_ && !release_fired_this_hold_) {
        stick = windup;
    } else {
        stick = 0.f;
    }
    stick = stick_quantized_for_output(stick);

    if (falling) {
        if (shot_active_ && !release_fired_this_hold_) {
            const char* reason = last_nofire_hint_;
            if (!reason || !reason[0]) {
                reason = "unknown";
            }
            if (!shot_type_locked_) {
                reason = "shot_type_not_locked";
            } else if (!have_ball) {
                reason = "no_ball";
            } else if (reason && std::strcmp(reason, "ball_not_in_fire_zone") == 0) {
                reason = "ball_never_entered_fire_zone";
            }
            // last_visual_reason_ is set every frame — on the falling edge (square just
            // released) fire_zone_.update() sees square_held=false and always returns
            // "square_not_held", which would overwrite the meaningful reason.  Use
            // last_nofire_hint_ (preserved from the most recent held frame) instead.
            const char* last_visual_for_log =
                (last_nofire_hint_ && last_nofire_hint_[0])
                    ? last_nofire_hint_
                    : (last_visual_reason_ ? last_visual_reason_ : "");
            std::fprintf(stdout,
                         "[J2K NoFireSummary] shot_id=%d shot_type_locked=%d sq_frames=%d "
                         "ball_seen_f=%d visual_owner=%d released=%d block_reason=%s "
                         "last_visual=%s ball_cy=%.0f fz_y1=%.0f fz_y2=%.0f "
                         "p95_total_ms=%.2f p95_yolo_ms=%.2f "
                         "p95_yolo_pre_ms=%.2f p95_yolo_infer_ms=%.2f p95_yolo_post_ms=%.2f "
                         "track_ms=%.2f visual_ms=%.2f\n",
                         shot_id_,
                         shot_type_locked_ ? 1 : 0,
                         square_held_frames_,
                         last_ball_seen_frame_,
                         visual_owner_ ? 1 : 0,
                         release_fired_this_hold_ ? 1 : 0,
                         reason,
                         last_visual_for_log,
                         last_ball_cy_,
                         last_fire_zone_box_[1],
                         last_fire_zone_box_[3],
                         approx_p95_total(),
                         approx_p95(perf_ring_yolo_),
                         approx_p95(perf_ring_pre_),
                         approx_p95(perf_ring_infer_),
                         approx_p95(perf_ring_post_),
                         track_ms,
                         visual_ms);
        }
        shot_active_ = false;
        release_fired_this_hold_ = false;
        visual_owner_ = false;
        shot_type_locked_ = false;
        square_held_frames_ = 0;
        fire_zone_.reset();
        tempo_flick_.cancel();
        have_shot_locked_ball_ = false;
        shot_locked_ball_gap_frames_ = 0;
    }

    std::memset(gcv_io.data(), 0, kGcvSize);
    pack_fixed16(1.0f, gcv_io.data() + kGcvCvFlag);
    pack_fixed16(stick, gcv_io.data() + kGcvStick1Y);
    pack_fixed16(1.0f, gcv_io.data() + kGcvSqBlock);

    const auto norm_u16 = [w, h](float v, int dim) -> std::uint16_t {
        const float d = static_cast<float>(std::max(1, dim));
        const float t = std::clamp(v / d, 0.f, 1.f);
        return static_cast<std::uint16_t>(std::lround(t * 65535.f));
    };
    const auto pack_box = [&](int offset, const std::array<float, 4>& b) {
        pack_u16_be(gcv_io.data() + offset, norm_u16(b[0], w));
        pack_u16_be(gcv_io.data() + offset + 2, norm_u16(b[1], h));
        pack_u16_be(gcv_io.data() + offset + 4, norm_u16(b[2], w));
        pack_u16_be(gcv_io.data() + offset + 6, norm_u16(b[3], h));
    };
    // Emit player/fire-zone only when geometry is valid; avoid synthetic centered boxes.
    if ((pbox[2] > pbox[0]) && (pbox[3] > pbox[1])) {
        pack_box(kGcvOutPlayerBox, pbox);
    }
    if ((zbox[2] > zbox[0]) && (zbox[3] > zbox[1])) {
        pack_box(kGcvOutFireZone, zbox);
    }
    if (have_ball && ball_ptr != nullptr) {
        const std::array<float, 4> bb = {ball_ptr[0], ball_ptr[1], ball_ptr[2], ball_ptr[3]};
        pack_box(kGcvOutBallBox, bb);
    }
    // Stamina: prefer real YOLO/live carry, but fall back to a player-anchored
    // proxy to avoid hard telemetry dropouts when the class is momentarily missed.
    const bool stamina_live_pack_sane = authority_stamina_track_live ||
                                        (have_yolo_stamina &&
                                        (yolo_stamina[2] > yolo_stamina[0] + 3.f) &&
                                        (yolo_stamina[3] > yolo_stamina[1] + 2.f));
    if (stamina_live_pack_sane) {
        pack_box(kGcvOutStamBox, yolo_stamina);
    } else if (have_last_stamina_ && stamina_miss_frames_ <= kStaminaCarryHeavyMissMax) {
        pack_box(kGcvOutStamBox, last_stamina_box_);
    } else {
        const float px1 = pbox[0];
        const float py1 = pbox[1];
        const float px2 = pbox[2];
        const float py2 = pbox[3];
        const float pw = std::max(1.f, px2 - px1);
        const float ph = std::max(1.f, py2 - py1);
        const float cx = 0.5f * (px1 + px2);
        const float bar_w = std::clamp(pw * 0.90f, 40.f, static_cast<float>(w) * 0.42f);
        const float bar_h = std::clamp(ph * 0.07f, 6.f, static_cast<float>(h) * 0.06f);
        const float y_mid = std::clamp(py2 + ph * 0.02f, bar_h + 1.f, static_cast<float>(h) - 1.f);
        const std::array<float, 4> stamina_proxy = {
            std::clamp(cx - bar_w * 0.5f, 0.f, static_cast<float>(w) - 1.f),
            std::clamp(y_mid - bar_h * 0.5f, 0.f, static_cast<float>(h) - 1.f),
            std::clamp(cx + bar_w * 0.5f, 0.f, static_cast<float>(w) - 1.f),
            std::clamp(y_mid + bar_h * 0.5f, 0.f, static_cast<float>(h) - 1.f)};
        pack_box(kGcvOutStamBox, stamina_proxy);
    }

    gcv_io[23] = 0;
    // Preserve host controller snapshot bytes (not part of CV outputs) for the next p() entry.
    gcv_io[20] = input_copy[20];
    gcv_io[21] = input_copy[21];
    gcv_io[22] = input_copy[22];

    // Update tracking state machine — labeling only, no effect on shot logic.
    {
        TrackingState new_state = TrackingState::kSeekingPlayer;
        if (shot_context) {
            new_state = TrackingState::kShotActive;
        } else if (!have_yolo_player && !have_last_stable_player_) {
            new_state = TrackingState::kSeekingPlayer;
        } else if (!have_ball) {
            const bool player_from_carry =
                !have_yolo_player && have_last_stable_player_;
            new_state = player_from_carry
                            ? TrackingState::kTempLostPlayer
                            : TrackingState::kTempLostBall;
        } else if (have_yolo_player && have_ball) {
            new_state = TrackingState::kBallTracked;
        } else {
            new_state = TrackingState::kPlayerLocked;
        }
        if (new_state != tracking_state_) {
            tracking_state_ = new_state;
            tracking_state_frame_ = frame_n_;
        }
    }

    prev_square_held_ = square_held;
    ++frame_n_;

    const auto t_frame1 = std::chrono::steady_clock::now();
    const double total_ms =
        std::chrono::duration<double, std::milli>(t_frame1 - t_frame0).count();
    const double capture_ms = frame_intervals_ms_.empty() ? 0.0 : frame_intervals_ms_.back();
    const double fps = (capture_ms > 0.001) ? (1000.0 / capture_ms) : 0.0;
    telemetry_.capture_ms = capture_ms;
    telemetry_.preprocess_ms = yolo_pre_ms;
    telemetry_.inference_ms = yolo_infer_ms;
    telemetry_.postprocess_ms = yolo_post_ms;
    telemetry_.tracking_ms = track_ms;
    telemetry_.visual_ms = visual_ms;
    telemetry_.total_frame_ms = total_ms;
    telemetry_.fps = fps;
    telemetry_.player_locked = authority_state_.player.visible || authority_state_.player.predicted;
    telemetry_.ball_locked = authority_state_.ball.visible || authority_state_.ball.predicted;
    telemetry_.stamina_locked = authority_state_.stamina.visible || authority_state_.stamina.predicted;
    telemetry_.player_lost_frames = authority_state_.player.missed_frames;
    telemetry_.ball_lost_frames = authority_state_.ball.missed_frames;
    telemetry_.stamina_lost_frames = authority_state_.stamina.missed_frames;
    if (yolo_) {
        telemetry_.cuda_active = yolo_->cuda_active();
        telemetry_.provider = yolo_->active_provider();
    }
    telemetry_.heavy_processed = process_heavy && heavy_attempted;
    telemetry_.cadence_every_n = std::max(1, std::max(yolo_every_n_, yolo_every_n_runtime_));
    // #region agent log
    if (telemetry_.heavy_processed || ((frame_n_ % 30) == 0)) {
        const int cadence_log = std::max(std::max(1, yolo_every_n_), yolo_every_n_runtime_);
        agent_log_c1a192_engine(
            "H43",
            "j2k_engine.cpp:process_frame",
            "frame_timing_breakdown",
            std::string("{\"frame\":") + std::to_string(frame_n_) +
                ",\"cadenceEveryN\":" + std::to_string(cadence_log) +
                ",\"processHeavyThisFrame\":" + std::string(process_heavy ? "true" : "false") +
                ",\"totalMs\":" + std::to_string(total_ms) +
                ",\"yoloMs\":" + std::to_string(yolo_ms) +
                ",\"yoloInferMs\":" + std::to_string(yolo_infer_ms) +
                ",\"yoloPreMs\":" + std::to_string(yolo_pre_ms) +
                ",\"yoloPostMs\":" + std::to_string(yolo_post_ms) +
                ",\"trackMs\":" + std::to_string(track_ms) +
                ",\"visualMs\":" + std::to_string(visual_ms) + "}");
    }
    // #endregion
    push_perf_sample(total_ms, yolo_ms, yolo_pre_ms, yolo_infer_ms, yolo_post_ms);
    last_total_ms_ = total_ms;
    last_yolo_ms_ = yolo_ms;
    log_perf_warn_if_needed(total_ms, yolo_ms, track_ms, visual_ms, square_held);

    return kGcvSize;
}

}  // namespace j2k::dll
