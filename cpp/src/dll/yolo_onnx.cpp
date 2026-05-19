/**
 * YOLOv8/v11 ONNX via Microsoft ONNX Runtime (CPU). Expects Ultralytics export:
 *   yolo export model=best.pt format=onnx imgsz=1280 simplify=True opset=12
 */
#ifdef _MSC_VER
#define _CRT_SECURE_NO_WARNINGS
#endif
#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#endif

#include "j2k/dll/yolo_onnx.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <thread>
#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <numeric>
#include <string>
#include <utility>
#include <vector>

#include <onnxruntime_cxx_api.h>

namespace j2k::dll {

namespace {

#ifdef _WIN32
std::wstring utf8_to_wide(const std::string& u8) {
    if (u8.empty()) {
        return L"";
    }
    const int n = MultiByteToWideChar(CP_UTF8, 0, u8.c_str(), -1, nullptr, 0);
    if (n <= 0) {
        return L"";
    }
    std::wstring w(static_cast<size_t>(n - 1), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, u8.c_str(), -1, w.data(), n);
    return w;
}
#endif

bool read_env_var(const char* name, std::string& out) {
#ifdef _WIN32
    char* raw = nullptr;
    size_t len = 0;
    const errno_t rc = _dupenv_s(&raw, &len, name);
    if (rc != 0 || raw == nullptr || len == 0) {
        if (raw != nullptr) {
            std::free(raw);
        }
        return false;
    }
    out.assign(raw);
    std::free(raw);
    return true;
#else
    const char* v = std::getenv(name);
    if (v == nullptr) {
        return false;
    }
    out.assign(v);
    return true;
#endif
}

bool env_truthy(const char* name) {
    std::string v;
    if (!read_env_var(name, v)) {
        return false;
    }
    return (v == "1" || v == "true" || v == "TRUE" || v == "on" || v == "ON");
}

bool release_agent_log_enabled_yolo() {
    return false;
}

bool release_verbose_enabled_yolo() {
    static int cached = -1;
    if (cached >= 0) {
        return cached == 1;
    }
    std::string v;
    if (read_env_var("J2K_VERBOSE", v)) {
        cached = (v == "1" || v == "true" || v == "TRUE" || v == "on" || v == "ON") ? 1 : 0;
    } else {
        cached = 0;
    }
    return cached == 1;
}

inline float sigmoid(float x) {
    return 1.f / (1.f + std::exp(-x));
}

// #region agent log
std::string json_quote_ascii(const char* s, std::size_t max_chars) {
    std::string r = "\"";
    if (s == nullptr) {
        r.push_back('"');
        return r;
    }
    for (std::size_t i = 0; i < max_chars && s[i] != '\0'; ++i) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        if (c == '"' || c == '\\') {
            r.push_back('\\');
            r.push_back(static_cast<char>(c));
        } else if (c < 32U) {
            r.push_back(' ');
        } else {
            r.push_back(static_cast<char>(c));
        }
    }
    r.push_back('"');
    return r;
}

void agent_log_c1a192(const char* hypothesis_id,
                      const char* location,
                      const char* message,
                      const std::string& data_json) {
    (void)hypothesis_id;
    (void)location;
    (void)message;
    (void)data_json;
}
// #endregion

struct RawDet {
    float x1 = 0.f;
    float y1 = 0.f;
    float x2 = 0.f;
    float y2 = 0.f;
    float score = 0.f;
    int cls = 0;
};

struct MappedDet {
    std::array<float, 4> box{};
    float score = 0.f;
};

float get_class_threshold(int cls,
                          float base_conf,
                          float player_conf,
                          float ball_conf,
                          float stamina_conf) {
    if (cls == kYoloClsBasketball) {
        return std::max(ball_conf, 0.10f);
    }
    if (cls == kYoloClsPlayer) {
        // conf_player floor is managed in load() where it also controls the post-NMS gate.
        // J2K_YOLO_PLAYER_CONF_FLOOR override is applied there directly to impl_->conf_player.
        return std::max(player_conf > 0.f ? player_conf : base_conf, 0.20f);
    }
    if (cls == kYoloClsStamBar) {
        return std::max(stamina_conf, 0.20f);
    }
    return base_conf;
}

bool stamina_box_plausible(const std::array<float, 4>& b, int w, int h) {
    const float x1 = b[0];
    const float y1 = b[1];
    const float x2 = b[2];
    const float y2 = b[3];
    if (!(x2 > x1 + 3.f && y2 > y1 + 2.f)) {
        return false;
    }
    const float bw = x2 - x1;
    const float bh = y2 - y1;
    const float fw = static_cast<float>(std::max(1, w));
    const float fh = static_cast<float>(std::max(1, h));
    // Stamina bar is a narrow horizontal element — typically 25-55% of frame width
    // and only a few pixels tall.  The old limits (90% width, 18% height) let in
    // wide court markings, floor textures, and scoreboard rows as ghost stamina bars.
    if (bw < fw * 0.04f || bw > fw * 0.60f) {
        return false;
    }
    if (bh < fh * 0.001f || bh > fh * 0.06f) {
        return false;
    }
    const float ar = bw / std::max(1.f, bh);
    if (ar < 2.5f || ar > 30.f) {
        return false;
    }
    return true;
}

bool ball_in_valid_court_zone(const std::array<float, 4>& b, int w, int h) {
    const float cx = 0.5f * (b[0] + b[2]);
    const float cy = 0.5f * (b[1] + b[3]);
    const float fw = static_cast<float>(std::max(1, w));
    const float fh = static_cast<float>(std::max(1, h));
    const float nx = cx / fw;
    const float ny = cy / fh;
    
    // FIXED: Tighter vertical bounds (was 0.18-0.88)
    if (ny < 0.20f || ny > 0.85f) {
        return false;
    }
    
    // FIXED: Tighter horizontal bounds (was 0.12-0.88)
    if (nx < 0.15f || nx > 0.85f) {
        return false;
    }
    
    // FIXED: Expanded rack exclusion (was 0.70 and 0.72)
    if (nx > 0.68f && ny > 0.70f) {
        return false;
    }
    
    // ADDED: Left corner exclusion (sometimes rack on left)
    if (nx < 0.32f && ny > 0.70f) {
        return false;
    }
    
    // ADDED: Top-corner exclusions (scoreboard elements)
    if ((nx < 0.25f || nx > 0.75f) && ny < 0.25f) {
        return false;
    }
    
    return true;
}

std::vector<MappedDet> detect_stamina_template(const std::uint8_t* bgr, int w, int h, int stride) {
    std::vector<MappedDet> out;
    out.reserve(4);  // OPTIMIZED: Pre-allocate
    
    if (bgr == nullptr || w < 16 || h < 16) {
        return out;
    }
    
    const size_t max_candidates = 3;
    struct SearchRegion {
        int x_start;
        int x_end;
        int y_start;
        int y_end;
    };
    // Only scan the bottom portion of the frame where the stamina bar actually lives.
    // The old top region (y: 8-38%) was hitting scoreboard / HUD elements which then
    // anchored the player box to the wrong screen position — removed.
    const SearchRegion regions[] = {
        {std::max(0, static_cast<int>(w * 0.20f)),
         std::min(w, static_cast<int>(w * 0.82f)),
         std::max(0, static_cast<int>(h * 0.60f)),
         std::min(h, static_cast<int>(h * 0.98f))},
    };

    for (const auto& region : regions) {
        for (int y = region.y_start; y < region.y_end; ++y) {
            if (out.size() >= max_candidates) {
                break;
            }
            const std::uint8_t* row = bgr + y * stride;
            int run_start = -1;
            for (int x = region.x_start; x < region.x_end; ++x) {
                const std::uint8_t* p = row + x * 3;
                const int b = p[0];
                const int g = p[1];
                const int r = p[2];
                const int cmax = std::max(r, std::max(g, b));
                const int cmin = std::min(r, std::min(g, b));
                // Accept saturated bright meter colors (green/yellow/blue), not just pure green.
                const bool meter_px = (cmax >= 115) && ((cmax - cmin) >= 40);

                if (meter_px) {
                    if (run_start < 0) {
                        run_start = x;
                    }
                } else if (run_start >= 0) {
                    const int rw = x - run_start;
                    if (rw >= std::max(22, w / 26)) {
                        int y2 = y;
                        while (y2 + 1 < region.y_end) {
                            const std::uint8_t* next_row = bgr + (y2 + 1) * stride;
                            int meter_count = 0;
                            for (int xx = run_start; xx < x; ++xx) {
                                const std::uint8_t* pn = next_row + xx * 3;
                                const int bn = pn[0];
                                const int gn = pn[1];
                                const int rn = pn[2];
                                const int nmax = std::max(rn, std::max(gn, bn));
                                const int nmin = std::min(rn, std::min(gn, bn));
                                if (nmax >= 115 && (nmax - nmin) >= 40) {
                                    ++meter_count;
                                }
                            }
                            if (meter_count * 2 < rw) {
                                break;
                            }
                            ++y2;
                            if (y2 - y > std::max(5, h / 10)) {
                                break;
                            }
                        }
                        std::array<float, 4> box = {
                            static_cast<float>(run_start),
                            static_cast<float>(y),
                            static_cast<float>(x),
                            static_cast<float>(y2 + 1)};
                        if (stamina_box_plausible(box, w, h)) {
                            out.push_back({box, 0.62f});
                        }
                    }
                    run_start = -1;
                }
            }
        }
        if (!out.empty()) {
            break;
        }
    }
    return out;
}

/// Drop jerseys, limbs, and court chips misclassified as the ball.
bool ball_box_plausible(const std::array<float, 4>& b, int w, int h) {
    const float x1 = b[0];
    const float y1 = b[1];
    const float x2 = b[2];
    const float y2 = b[3];
    if (!(x2 > x1 + 2.f && y2 > y1 + 2.f)) {
        return false;
    }
    const float bw = x2 - x1;
    const float bh = y2 - y1;
    const float fw = static_cast<float>(std::max(1, w));
    const float fh = static_cast<float>(std::max(1, h));
    if (bw > 0.50f * fw || bh > 0.50f * fh) {
        return false;
    }
    if (bw < 0.0025f * fw || bh < 0.0025f * fh) {
        return false;
    }
    if (bw * bh > 0.12f * fw * fh) {
        return false;
    }
    const float ar = std::max(bw, bh) / std::max(3.f, std::min(bw, bh));
    if (ar > 3.2f) {
        return false;
    }
    return true;
}

float box_center_dist_xyxy(const std::array<float, 4>& a, const std::array<float, 4>& b) {
    const float acx = 0.5f * (a[0] + a[2]);
    const float acy = 0.5f * (a[1] + a[3]);
    const float bcx = 0.5f * (b[0] + b[2]);
    const float bcy = 0.5f * (b[1] + b[3]);
    return std::hypot(acx - bcx, acy - bcy);
}

float xyxy_iou_box(const std::array<float, 4>& a, const std::array<float, 4>& b) {
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

bool ball_box_plausible_for_apex(const std::array<float, 4>& pb, const std::array<float, 4>& bb) {
    const float px1 = pb[0];
    const float py1 = pb[1];
    const float px2 = pb[2];
    const float py2 = pb[3];
    const float ph = std::max(1.f, py2 - py1);
    const float pw = std::max(1.f, px2 - px1);
    const float cx = 0.5f * (bb[0] + bb[2]);
    const float cy = 0.5f * (bb[1] + bb[3]);
    if (cy > py2 + ph * 0.12f) {
        return false;
    }
    if (cx < px1 - pw * 0.42f || cx > px2 + pw * 0.42f) {
        return false;
    }
    if (cy < py1 - ph * 0.62f) {
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
    const float pw = std::max(1.f, px2 - px1);
    const float ph = std::max(1.f, py2 - py1);
    const float cx = 0.5f * (bb[0] + bb[2]);
    const float cy = 0.5f * (bb[1] + bb[3]);
    const float xs = shot_active ? 0.60f : 0.45f;
    const float ts = shot_active ? 0.95f : 0.72f;
    constexpr float bs = 0.18f;
    const float xl = px1 - pw * xs;
    const float xr = px2 + pw * xs;
    const float yt = py1 - ph * ts;
    const float yb = py2 + ph * bs;
    return xl <= cx && cx <= xr && yt <= cy && cy <= yb;
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

float iou_xyxy(const RawDet& a, const RawDet& b) {
    const float xx1 = std::max(a.x1, b.x1);
    const float yy1 = std::max(a.y1, b.y1);
    const float xx2 = std::min(a.x2, b.x2);
    const float yy2 = std::min(a.y2, b.y2);
    const float iw = std::max(0.f, xx2 - xx1);
    const float ih = std::max(0.f, yy2 - yy1);
    const float inter = iw * ih;
    const float area_a = std::max(0.f, a.x2 - a.x1) * std::max(0.f, a.y2 - a.y1);
    const float area_b = std::max(0.f, b.x2 - b.x1) * std::max(0.f, b.y2 - b.y1);
    const float u = area_a + area_b - inter;
    return (u <= 1e-6f) ? 0.f : (inter / u);
}

std::vector<RawDet> nms_class(const std::vector<RawDet>& in, float iou_thresh) {
    std::vector<RawDet> d = in;
    std::sort(d.begin(), d.end(), [](const RawDet& a, const RawDet& b) { return a.score > b.score; });
    std::vector<RawDet> out;
    std::vector<char> suppressed(d.size(), 0);
    for (size_t i = 0; i < d.size(); ++i) {
        if (suppressed[i]) {
            continue;
        }
        out.push_back(d[i]);
        for (size_t j = i + 1; j < d.size(); ++j) {
            if (suppressed[j] || d[j].cls != d[i].cls) {
                continue;
            }
            if (iou_xyxy(d[i], d[j]) > iou_thresh) {
                suppressed[j] = 1;
            }
        }
    }
    return out;
}

void letterbox_nchw_rgb(const std::uint8_t* bgr,
                        int w,
                        int h,
                        int stride,
                        int imgsz,
                        float* chw,
                        float& scale,
                        int& pad_x,
                        int& pad_y,
                        int& new_w,
                        int& new_h) {
    scale = std::min(static_cast<float>(imgsz) / static_cast<float>(std::max(1, w)),
                     static_cast<float>(imgsz) / static_cast<float>(std::max(1, h)));
    new_w = std::max(1, static_cast<int>(std::lround(static_cast<float>(w) * scale)));
    new_h = std::max(1, static_cast<int>(std::lround(static_cast<float>(h) * scale)));
    pad_x = (imgsz - new_w) / 2;
    pad_y = (imgsz - new_h) / 2;
    const float pad_rgb = 114.f / 255.f;
    const int plane = imgsz * imgsz;
    const size_t chw_elems = static_cast<size_t>(3) * static_cast<size_t>(plane);
    std::fill(chw, chw + chw_elems, pad_rgb);

    for (int y = 0; y < new_h; ++y) {
        const int dy = pad_y + y;
        float sy = static_cast<float>(y) / scale;
        int sy_i = static_cast<int>(sy);
        if (sy_i < 0) {
            sy_i = 0;
        }
        if (sy_i >= h) {
            sy_i = h - 1;
        }
        const std::uint8_t* row = bgr + sy_i * stride;
        const size_t row_base = static_cast<size_t>(dy) * static_cast<size_t>(imgsz);
        for (int x = 0; x < new_w; ++x) {
            const int dx = pad_x + x;
            float sx = static_cast<float>(x) / scale;
            int sx_i = static_cast<int>(sx);
            if (sx_i < 0) {
                sx_i = 0;
            }
            if (sx_i >= w) {
                sx_i = w - 1;
            }
            const std::uint8_t* p = row + sx_i * 3;
            const float bb = static_cast<float>(p[0]) * (1.f / 255.f);
            const float gg = static_cast<float>(p[1]) * (1.f / 255.f);
            const float rr = static_cast<float>(p[2]) * (1.f / 255.f);
            const size_t ix = row_base + static_cast<size_t>(dx);
            chw[ix] = rr;
            chw[plane + ix] = gg;
            chw[2U * static_cast<size_t>(plane) + ix] = bb;
        }
    }
}

void map_box_to_original(float x1,
                         float y1,
                         float x2,
                         float y2,
                         float scale,
                         int pad_x,
                         int pad_y,
                         int src_w,
                         int src_h,
                         std::array<float, 4>& out_xyxy) {
    auto inv = [&](float v, int pad) -> float { return (v - static_cast<float>(pad)) / scale; };
    float nx1 = inv(x1, pad_x);
    float ny1 = inv(y1, pad_y);
    float nx2 = inv(x2, pad_x);
    float ny2 = inv(y2, pad_y);
    nx1 = std::max(0.f, std::min(nx1, static_cast<float>(src_w - 1)));
    nx2 = std::max(0.f, std::min(nx2, static_cast<float>(src_w - 1)));
    ny1 = std::max(0.f, std::min(ny1, static_cast<float>(src_h - 1)));
    ny2 = std::max(0.f, std::min(ny2, static_cast<float>(src_h - 1)));
    if (nx2 < nx1) {
        std::swap(nx1, nx2);
    }
    if (ny2 < ny1) {
        std::swap(ny1, ny2);
    }
    out_xyxy = {nx1, ny1, nx2, ny2};
}

std::int64_t debug_now_ms() {
    return static_cast<std::int64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count());
}

void debug_log_line(const char* run_id,
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
    std::fprintf(
        fp,
        "{\"runId\":\"%s\",\"hypothesisId\":\"%s\",\"location\":\"%s\",\"message\":\"%s\",\"data\":%s,\"timestamp\":%lld}\n",
        run_id ? run_id : "",
        hypothesis_id ? hypothesis_id : "",
        location ? location : "",
        message ? message : "",
        data_json.c_str(),
        static_cast<long long>(debug_now_ms()));
    std::fclose(fp);
}

}  // namespace

struct YoloOnnxRunner::Impl {
    Ort::Env env{ORT_LOGGING_LEVEL_WARNING, "j2k_yolo"};
    Ort::SessionOptions opts{};
    std::unique_ptr<Ort::Session> session;
    Ort::AllocatorWithDefaultOptions alloc{};
    std::vector<Ort::AllocatedStringPtr> in_holders;
    std::vector<Ort::AllocatedStringPtr> out_holders;
    std::vector<const char*> in_names;
    std::vector<const char*> out_names;
    int input_size = 1280;
    float conf = 0.25f;
    float conf_player = 0.20f;
    float conf_ball = 0.18f;
    float conf_stam = 0.20f;
    float player_ema_alpha = 0.40f;
    float ball_ema_alpha = 0.45f;
    float iou = 0.45f;
    int onnx_threads = 0;
    std::vector<float> chw;
    bool debug_det = false;
    int frame_i = 0;
    std::array<float, 4> last_player_box{};
    std::array<float, 4> last_ball_box{};
    std::array<float, 4> last_stam_box{};
    bool have_last_player = false;
    bool have_last_ball = false;
    bool have_last_stam = false;
    int miss_player = 0;
    int miss_ball = 0;
    int miss_stam = 0;
    bool cuda_active = false;
    std::string active_provider = "NONE";
    std::string available_providers_csv;
};

YoloOnnxRunner::YoloOnnxRunner() : impl_(std::make_unique<Impl>()) {}

YoloOnnxRunner::~YoloOnnxRunner() = default;

bool YoloOnnxRunner::load(const YoloOnnxConfig& cfg) {
    loaded_ = false;
    impl_->session.reset();
    impl_->in_holders.clear();
    impl_->out_holders.clear();
    impl_->in_names.clear();
    impl_->out_names.clear();

    if (cfg.model_path.empty()) {
        std::fprintf(stdout, "[J2K YOLO] load() called with empty model_path — aborting.\n");
        std::fflush(stdout);
        return false;
    }
    namespace fs = std::filesystem;
    std::error_code ec;
    if (!fs::exists(cfg.model_path, ec) || ec) {
        std::fprintf(stdout, "[J2K YOLO] ONNX model not found: %s\n", cfg.model_path.c_str());
        std::fprintf(stderr, "[J2K YOLO] ONNX model not found: %s\n", cfg.model_path.c_str());
        std::fflush(stdout);
        return false;
    }
    std::fprintf(stdout, "[J2K YOLO] Model found: %s\n", cfg.model_path.c_str());
    std::fflush(stdout);

    impl_->conf = cfg.conf_threshold;
    impl_->conf_player = cfg.player_conf_threshold;
    impl_->iou = cfg.iou_threshold;
    impl_->frame_i = 0;
    impl_->have_last_player = false;
    impl_->have_last_ball = false;
    impl_->have_last_stam = false;
    impl_->miss_player = 0;
    impl_->miss_ball = 0;
    impl_->miss_stam = 0;
    impl_->debug_det = false;
    impl_->cuda_active = false;
    impl_->active_provider = "NONE";
    impl_->available_providers_csv.clear();
    std::string dbg_env;
    if (read_env_var("J2K_DET_DEBUG", dbg_env)) {
        const std::string v = dbg_env;
        if (v == "1" || v == "true" || v == "TRUE" || v == "on" || v == "ON") {
            impl_->debug_det = true;
        }
    }
    // #region agent log
    debug_log_line("pre-fix",
                   "H10",
                   "yolo_onnx.cpp:det_debug_env",
                   "det_debug_env_read",
                   std::string("{\"envPresent\":") + (dbg_env.empty() ? "false" : "true") +
                       ",\"debugDet\":" + (impl_->debug_det ? "true" : "false") + "}");
    // #endregion
    // was std::max(0.35f, ...) — the old floor silently rejected all 0.20-0.34 confidence
    // players both at pre-NMS gate and at the post-NMS score check (line ~1329), making
    // gate_scale=0.50 in decoded-NMS mode completely pointless for the player class.
    impl_->conf_player = (cfg.player_conf_threshold > 0.001f)
                             ? cfg.player_conf_threshold
                             : std::max(0.20f, cfg.conf_threshold);
    // Allow runtime override: J2K_YOLO_PLAYER_CONF_FLOOR directly sets impl_->conf_player,
    // controlling both the pre-NMS gate and the post-NMS acceptance check.
    {
        std::string pf_ev;
        if (read_env_var("J2K_YOLO_PLAYER_CONF_FLOOR", pf_ev) && !pf_ev.empty()) {
            try {
                const float parsed = std::stof(pf_ev);
                if (parsed > 0.01f && parsed < 1.f) {
                    impl_->conf_player = parsed;
                }
            } catch (...) {}
        }
    }
    impl_->conf_ball = (cfg.ball_conf_threshold > 0.001f)
                           ? std::max(0.10f, cfg.ball_conf_threshold)
                           : std::max(0.10f, cfg.conf_threshold * 0.72f);
    impl_->conf_stam = (cfg.stamina_conf_threshold > 0.001f)
                           ? std::max(0.20f, cfg.stamina_conf_threshold)
                           : std::max(0.20f, cfg.conf_threshold * 0.40f);
    impl_->player_ema_alpha = std::clamp(cfg.player_ema_alpha, 0.01f, 1.0f);
    impl_->ball_ema_alpha = std::clamp(cfg.ball_ema_alpha, 0.01f, 1.0f);
    impl_->onnx_threads = std::clamp(cfg.onnx_threads, 0, 32);

    auto configure_session_options = [&]() {
        impl_->opts = Ort::SessionOptions{};
        impl_->opts.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
        impl_->opts.SetExecutionMode(ExecutionMode::ORT_SEQUENTIAL);
        impl_->opts.EnableCpuMemArena();
        impl_->opts.EnableMemPattern();
        unsigned hc = std::thread::hardware_concurrency();
        int threads = impl_->onnx_threads;
        if (threads <= 0) {
            threads = 4;
            if (hc > 0) {
                threads = std::clamp(static_cast<int>(hc / 2u), 4, 8);
            }
        }
        impl_->opts.SetIntraOpNumThreads(threads);
        if (release_verbose_enabled_yolo()) {
            std::fprintf(stdout, "[J2K YOLO] Using %d threads for ONNX\n", threads);
        }
    };

    configure_session_options();

    // CUDA is on by default. Set J2K_ORT_NO_CUDA=1 to force CPU-only.
    const bool cuda_disabled = env_truthy("J2K_ORT_NO_CUDA");
    const bool cuda_requested = !cuda_disabled;
    bool cuda_available = false;
    {
        const std::vector<std::string> providers = Ort::GetAvailableProviders();
        for (const auto& p : providers) {
            if (!impl_->available_providers_csv.empty()) {
                impl_->available_providers_csv += ",";
            }
            impl_->available_providers_csv += p;
            if (p == "CUDAExecutionProvider") {
                cuda_available = true;
            }
        }
        // Always print available providers — critical for diagnosing load failures.
        std::fprintf(stdout,
                     "[J2K ONNX] Providers: %s\n",
                     impl_->available_providers_csv.empty() ? "(none)" : impl_->available_providers_csv.c_str());
        std::fflush(stdout);
    }
    bool cuda_appended = false;
    bool trt_appended = false;
    if (cuda_requested) {
        if (!cuda_available) {
            // CUDA EP not listed by ORT — ORT-GPU not installed or wrong package.
            // Warn and skip all GPU EP registration; session will open on CPU.
            std::fprintf(stdout,
                         "[J2K ONNX] WARNING: CUDA EP not in ORT provider list — ORT-GPU not installed or GPU unavailable.\n"
                         "[J2K ONNX]   Providers seen: %s\n"
                         "[J2K ONNX]   Falling back to CPU. Install onnxruntime-gpu to enable GPU.\n",
                         impl_->available_providers_csv.empty() ? "(none)" : impl_->available_providers_csv.c_str());
            std::fflush(stdout);
        } else {
        // TRT EP: always register so the session can use TRT for ops CUDA EP cannot handle alone.
        // First launch: TRT compiles the engine (~2-10 min, cached to trt_cache_path).
        // Subsequent launches: loads engine from cache in <1 s — full speed immediately.
        // The warmup thread in j2k_engine.cpp gates inference until compilation completes.
        {
            // Ensure the cache directory exists before ORT tries to write into it.
            // ORT 1.26+ silently fails Ort::Session() creation if the directory is missing.
            namespace fs = std::filesystem;
            const std::string trt_dir = cfg.trt_cache_path.empty() ? "./trt_cache" : cfg.trt_cache_path;
            std::error_code ec_mk;
            fs::create_directories(trt_dir, ec_mk);
            // (ec_mk ignored — if creation fails ORT will report it during session open)
        }
        try {
            OrtTensorRTProviderOptions trt_opts{};
            trt_opts.device_id = 0;
            trt_opts.trt_fp16_enable = 1;
            trt_opts.trt_engine_cache_enable = 1;
            const std::string trt_dir = cfg.trt_cache_path.empty() ? "./trt_cache" : cfg.trt_cache_path;
            trt_opts.trt_engine_cache_path = trt_dir.c_str();
            trt_opts.trt_max_workspace_size = static_cast<size_t>(2) * 1024 * 1024 * 1024;
            impl_->opts.AppendExecutionProvider_TensorRT(trt_opts);
            trt_appended = true;
            // Only print in verbose mode — on normal launches this is just noise.
            if (release_verbose_enabled_yolo()) {
                std::fprintf(stdout, "[J2K ONNX] TensorRT EP registered (FP16, cache=%s)\n", trt_dir.c_str());
            }
        } catch (const Ort::Exception& e) {
            std::fprintf(stdout, "[J2K ONNX] TRT EP unavailable (%s) — CUDA EP only\n", e.what());
        } catch (...) {
            std::fprintf(stdout, "[J2K ONNX] TRT EP load failed — CUDA EP only\n");
        }
        // CUDA EP: append as fallback for any ops TRT doesn't handle.
        // If this fails, DO NOT reset opts — the TRT EP is already registered and
        // TRT runs CUDA internally for all supported YOLO ops. Keep going TRT-only.
        try {
            OrtCUDAProviderOptions cuda_opts{};
            cuda_opts.device_id = 0;
            // Heuristic is 2-5x faster on first call vs exhaustive; accuracy difference is negligible.
            cuda_opts.cudnn_conv_algo_search = OrtCudnnConvAlgoSearchHeuristic;
            impl_->opts.AppendExecutionProvider_CUDA(cuda_opts);
            cuda_appended = true;
        } catch (const Ort::Exception& e) {
            // CUDA EP append failed — keep the TRT EP registration and continue.
            // TRT EP uses CUDA internally so GPU inference still works for all YOLO ops.
            std::fprintf(stdout,
                         "[J2K ONNX] CUDA EP append failed (%s) — continuing TRT-only (expected if cuDNN/cuBLAS not staged).\n",
                         e.what());
            std::fflush(stdout);
        } catch (...) {
            std::fprintf(stdout,
                         "[J2K ONNX] CUDA EP append failed (unknown) — continuing TRT-only.\n");
            std::fflush(stdout);
        }
        }  // end else (cuda_available)
    } else {
        // #region agent log
        agent_log_c1a192("H40", "yolo_onnx.cpp:load", "cuda_ep_not_requested", "{\"requested\":false}");
        // #endregion
    }

    // Checkpoint: show what EPs were actually registered before attempting session creation.
    std::fprintf(stdout,
                 "[J2K ONNX] Creating session — trt=%d cuda_appended=%d trt_cache=%s\n",
                 trt_appended ? 1 : 0,
                 cuda_appended ? 1 : 0,
                 cfg.trt_cache_path.c_str());
    std::fflush(stdout);

#ifdef _WIN32
    const std::wstring wpath = utf8_to_wide(cfg.model_path);
#endif

    auto open_session = [&]() -> bool {
        try {
#ifdef _WIN32
            impl_->session =
                std::make_unique<Ort::Session>(impl_->env, wpath.c_str(), impl_->opts);
#else
            impl_->session =
                std::make_unique<Ort::Session>(impl_->env, cfg.model_path.c_str(), impl_->opts);
#endif
            return true;
        } catch (const Ort::Exception& e) {
            std::fprintf(stdout, "[J2K YOLO] ORT session error (Ort): %s\n", e.what());
            std::fprintf(stderr, "[J2K YOLO] ORT load failed: %s\n", e.what());
            std::fflush(stdout);
            return false;
        } catch (const std::exception& e) {
            std::fprintf(stdout, "[J2K YOLO] ORT session error (std): %s\n", e.what());
            std::fflush(stdout);
            return false;
        } catch (...) {
            std::fprintf(stdout, "[J2K YOLO] ORT session unknown error\n");
            std::fflush(stdout);
            return false;
        }
    };

    bool cuda_session_active = false;
    const bool session_opened = open_session();
    if (!session_opened) {
        std::fprintf(stdout, "[J2K ONNX] FATAL: ONNX session failed — see error above.\n");
        std::fprintf(stderr, "[J2K ONNX] FATAL: failed to open ONNX session.\n");
        std::fflush(stdout);
        return false;
    }
    // Session is GPU-active if TRT EP or CUDA EP was registered.
    // TRT EP uses CUDA internally — no separate CUDA EP required for GPU inference.
    cuda_session_active = trt_appended || cuda_appended;
    if (cuda_requested && !cuda_session_active) {
        // CUDA/TRT EP failed to register (driver not installed, DLL init error, etc.).
        // The ORT session opened successfully and will run on CPU. Warn and continue —
        // returning false here would leave loaded_=false and silence all detections.
        std::fprintf(stdout,
                     "[J2K ONNX] WARNING: CUDA requested but neither TRT nor CUDA EP active "
                     "(trt=%d cuda=%d). Falling back to CPU — inference will be slower.\n",
                     trt_appended ? 1 : 0,
                     cuda_appended ? 1 : 0);
        std::fprintf(stdout,
                     "[J2K ONNX] Set J2K_ORT_NO_CUDA=1 to suppress this warning.\n");
        std::fflush(stdout);
        // cuda_session_active stays false — telemetry will reflect CPU mode correctly.
    }

    const size_t in_n = impl_->session->GetInputCount();
    const size_t out_n = impl_->session->GetOutputCount();
    if (in_n < 1 || out_n < 1) {
        std::fprintf(stderr, "[J2K YOLO] Invalid model IO count\n");
        impl_->session.reset();
        return false;
    }

    int model_hw = -1;
    try {
        const auto ti = impl_->session->GetInputTypeInfo(0).GetTensorTypeAndShapeInfo();
        const std::vector<int64_t> sh = ti.GetShape();
        // Ultralytics ONNX: [1, 3, H, W] NCHW
        if (sh.size() >= 4 && sh[2] > 0 && sh[3] > 0) {
            if (sh[2] == sh[3]) {
                model_hw = static_cast<int>(sh[2]);
            } else {
                model_hw = static_cast<int>(std::max(sh[2], sh[3]));
            }
        }
    } catch (...) {
        model_hw = -1;
    }
    if (model_hw >= 32) {
        if (model_hw != cfg.input_size) {
            if (release_verbose_enabled_yolo()) {
                std::fprintf(stdout,
                             "[J2K YOLO] j2k_config yolo_imgsz=%d does not match ONNX input; using %dx%d "
                             "(re-export ONNX with imgsz=%d to match config).\n",
                             cfg.input_size,
                             model_hw,
                             model_hw,
                             cfg.input_size);
            }
        }
        impl_->input_size = model_hw;
    } else {
        impl_->input_size = std::max(32, cfg.input_size);
        if (release_verbose_enabled_yolo()) {
            std::fprintf(stdout,
                         "[J2K YOLO] Model input H/W not fixed; using j2k_config yolo_imgsz=%d\n",
                         impl_->input_size);
        }
    }

    impl_->chw.assign(static_cast<size_t>(3) * static_cast<size_t>(impl_->input_size) *
                          static_cast<size_t>(impl_->input_size),
                      0.f);

    for (size_t i = 0; i < in_n; ++i) {
        impl_->in_holders.push_back(impl_->session->GetInputNameAllocated(i, impl_->alloc));
        impl_->in_names.push_back(impl_->in_holders.back().get());
    }
    for (size_t i = 0; i < out_n; ++i) {
        impl_->out_holders.push_back(impl_->session->GetOutputNameAllocated(i, impl_->alloc));
        impl_->out_names.push_back(impl_->out_holders.back().get());
    }

    loaded_ = true;
    impl_->cuda_active = cuda_session_active;
    impl_->active_provider = trt_appended ? "TensorrtExecutionProvider" :
                             (cuda_session_active ? "CUDAExecutionProvider" : "CPUExecutionProvider");
    // One-line load confirmation + structured startup banner — always printed.
    {
        const char* provider_str = trt_appended ? "TRT+CUDA" :
                                   (cuda_session_active ? "CUDA" : "CPU");
        const int cpu_fallback = (!trt_appended && !cuda_appended) ? 1 : 0;
        std::fprintf(stdout,
                     "[J2K ONNX] Ready — provider=%s  input=%dx%d  conf=%.2f/%.2f/%.2f\n",
                     provider_str,
                     impl_->input_size, impl_->input_size,
                     impl_->conf_player, impl_->conf_ball, impl_->conf_stam);
        std::fprintf(stdout,
                     "[J2K STARTUP] ACTIVE_PROVIDER=%s TRT_ENABLED=%d CUDA_ENABLED=%d "
                     "CPU_FALLBACK=%d MODEL_INPUT_SIZE=%dx%d "
                     "TRT_CACHE_PATH=%s INFERENCE_BACKEND=%s\n",
                     provider_str,
                     trt_appended ? 1 : 0,
                     cuda_appended ? 1 : 0,
                     cpu_fallback,
                     impl_->input_size, impl_->input_size,
                     cfg.trt_cache_path.empty() ? "./trt_cache" : cfg.trt_cache_path.c_str(),
                     provider_str);
        if (cpu_fallback) {
            std::fprintf(stderr,
                         "[J2K STARTUP] WARNING: Running on CPU — no CUDA/TRT EP active! "
                         "Inference will be extremely slow. Check CUDA/ORT-GPU install.\n");
        }
        std::fflush(stdout);
    }
    // #region agent log
    agent_log_c1a192(
        "H42",
        "yolo_onnx.cpp:load",
        "session_ep_summary",
        std::string("{\"cudaRequested\":") + (cuda_requested ? "true" : "false") +
            ",\"cudaAppendOk\":" + (cuda_appended ? "true" : "false") +
            ",\"cudaSessionActive\":" + (cuda_session_active ? "true" : "false") + "}");
    // #endregion
    if (release_verbose_enabled_yolo()) {
        std::fprintf(stdout, "[J2K YOLO] Loaded %s (letterbox=%d)\n", cfg.model_path.c_str(), impl_->input_size);
    }
    return true;
}

bool YoloOnnxRunner::run(const std::uint8_t* bgr,
                         int w,
                         int h,
                         int stride,
                         std::array<float, 4>& ball_xyxy,
                         bool& have_ball,
                         std::array<float, 4>& player_xyxy,
                         bool& have_player,
                         std::array<float, 4>& stamina_xyxy,
                         bool& have_stamina,
                         double* out_ms,
                         double* out_pre_ms,
                         double* out_infer_ms,
                         double* out_post_ms,
                         const std::array<float, 4>* prev_ball_hint,
                         const std::array<float, 4>* prev_player_hint,
                         bool shot_active_context,
                         float* out_ball_conf,
                         float* out_player_conf,
                         float* out_stamina_conf) {
    have_ball = false;
    have_player = false;
    have_stamina = false;
    if (out_ball_conf != nullptr) {
        *out_ball_conf = 0.f;
    }
    if (out_player_conf != nullptr) {
        *out_player_conf = 0.f;
    }
    if (out_stamina_conf != nullptr) {
        *out_stamina_conf = 0.f;
    }
    if (out_pre_ms != nullptr) {
        *out_pre_ms = 0.0;
    }
    if (out_infer_ms != nullptr) {
        *out_infer_ms = 0.0;
    }
    if (out_post_ms != nullptr) {
        *out_post_ms = 0.0;
    }
    if (!loaded_ || impl_->session == nullptr || bgr == nullptr || w < 16 || h < 16 ||
        stride < (w * 3)) {
        return false;
    }

    const auto t0 = std::chrono::steady_clock::now();
    ++impl_->frame_i;

    float scale = 1.f;
    int pad_x = 0;
    int pad_y = 0;
    int new_w = 0;
    int new_h = 0;
    const int isz = impl_->input_size;
    const auto t_pre0 = std::chrono::steady_clock::now();
    letterbox_nchw_rgb(bgr, w, h, stride, isz, impl_->chw.data(), scale, pad_x, pad_y, new_w, new_h);
    const auto t_pre1 = std::chrono::steady_clock::now();
    if (out_pre_ms != nullptr) {
        *out_pre_ms = std::chrono::duration<double, std::milli>(t_pre1 - t_pre0).count();
    }

    std::array<int64_t, 4> shape = {1, 3, isz, isz};
    Ort::MemoryInfo mem = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeCPUInput);
    Ort::Value in_tensor = Ort::Value::CreateTensor<float>(
        mem,
        impl_->chw.data(),
        impl_->chw.size(),
        shape.data(),
        shape.size());

    try {
        const auto t_inf0 = std::chrono::steady_clock::now();
        auto outs = impl_->session->Run(Ort::RunOptions{nullptr},
                                        impl_->in_names.data(),
                                        &in_tensor,
                                        1,
                                        impl_->out_names.data(),
                                        impl_->out_names.size());
        const auto t_inf1 = std::chrono::steady_clock::now();
        if (out_infer_ms != nullptr) {
            *out_infer_ms = std::chrono::duration<double, std::milli>(t_inf1 - t_inf0).count();
        }
        if (outs.empty()) {
            return false;
        }
        float* raw = outs[0].GetTensorMutableData<float>();
        auto ti = outs[0].GetTensorTypeAndShapeInfo();
        std::vector<int64_t> dims = ti.GetShape();
        if (dims.size() != 3) {
            std::fprintf(stderr, "[J2K YOLO] Expected 3D output, got dim=%zu\n", dims.size());
            return false;
        }
        const int64_t d1 = dims[1];
        const int64_t d2 = dims[2];
        int feat = 0;
        int n_anchors = 0;
        bool feat_major = false;
        /* Ultralytics ONNX: [1, 4+nc, N] or [1, N, 4+nc]. */
        if (d1 < d2) {
            feat = static_cast<int>(d1);
            n_anchors = static_cast<int>(d2);
            feat_major = true;
        } else {
            n_anchors = static_cast<int>(d1);
            feat = static_cast<int>(d2);
            feat_major = false;
        }
        if (feat < 6 || n_anchors < 100) {
            std::fprintf(stderr,
                         "[J2K YOLO] Unexpected output layout [%lld,%lld,%lld] (feat=%d n=%d)\n",
                         static_cast<long long>(dims[0]),
                         static_cast<long long>(d1),
                         static_cast<long long>(d2),
                         feat,
                         n_anchors);
            return false;
        }
        const int nc = feat - 4;
        // Use decoded-NMS parser for [N,6]/[6,N] exports: [x1,y1,x2,y2,score,class].
        // This model reports 300x6, so forcing raw xywh parsing corrupts detections.
        const bool decoded_nms_layout = (feat == 6);
        const bool sample_log = (impl_->frame_i % 30) == 0;
        if (sample_log) {
            // #region agent log
            debug_log_line("pre-fix",
                           "H4",
                           "yolo_onnx.cpp:decoded_layout",
                           "onnx_output_layout",
                           std::string("{\"frame\":") + std::to_string(impl_->frame_i) +
                               ",\"d1\":" + std::to_string(d1) + ",\"d2\":" + std::to_string(d2) +
                               ",\"feat\":" + std::to_string(feat) + ",\"anchors\":" +
                               std::to_string(n_anchors) + ",\"featMajor\":" +
                               (feat_major ? "true" : "false") + ",\"decodedNms\":" +
                               (decoded_nms_layout ? "true" : "false") + "}");
            // #endregion
        }

        auto at = [&](int f, int i) -> float {
            if (feat_major) {
                return raw[static_cast<size_t>(f) * static_cast<size_t>(n_anchors) +
                           static_cast<size_t>(i)];
            }
            return raw[static_cast<size_t>(i) * static_cast<size_t>(feat) + static_cast<size_t>(f)];
        };

        std::vector<RawDet> cand;
        cand.reserve(256);
        float max_ball_score_seen = -1.f;
        float max_player_score_seen = -1.f;
        float max_stam_score_seen = -1.f;
        int raw_ball_cls = 0;
        int raw_player_cls = 0;
        int raw_stam_cls = 0;
        std::array<int, 6> raw_cls_hist{};
        int raw_ball_ge_5e3 = 0;
        int raw_ball_ge_1e3 = 0;
        int raw_ball_ge_2e4 = 0;
        float ball_effective_gate = 0.f;
        for (int i = 0; i < n_anchors; ++i) {
            float x1 = 0.f;
            float y1 = 0.f;
            float x2 = 0.f;
            float y2 = 0.f;
            int best_c = 0;
            float best_s = -1.f;
            if (decoded_nms_layout) {
                x1 = at(0, i);
                y1 = at(1, i);
                x2 = at(2, i);
                y2 = at(3, i);
                best_s = at(4, i);
                best_c = static_cast<int>(std::lround(at(5, i)));
                // The standard Ultralytics/Roboflow export uses 0-based class IDs:
                //   0=basketball, 1=player, 2=stam bar  (data.yaml confirmed)
                // The old "1-based auto-correct" (best_c -= 1) shifted player→ball and
                // stam→player, causing every player and stamina detection to be silently
                // discarded by the wrong-class plausibility gates. Removed.
                // If a future model genuinely uses 1-based IDs, set J2K_YOLO_1BASED_CLASSES=1.
                {
                    static int s_1based = -1;
                    if (s_1based < 0) {
                        std::string ev;
                        s_1based = (read_env_var("J2K_YOLO_1BASED_CLASSES", ev) &&
                                    (ev == "1" || ev == "true")) ? 1 : 0;
                    }
                    if (s_1based == 1 && best_c >= 1 && best_c <= 3) {
                        best_c -= 1;
                    }
                }
            } else {
                const float cx = at(0, i);
                const float cy = at(1, i);
                const float bw = at(2, i);
                const float bh = at(3, i);
                for (int c = 0; c < nc; ++c) {
                    float s = at(4 + c, i);
                    if (s > best_s) {
                        best_s = s;
                        best_c = c;
                    }
                }
                x1 = cx - 0.5f * bw;
                y1 = cy - 0.5f * bh;
                x2 = cx + 0.5f * bw;
                y2 = cy + 0.5f * bh;
            }
            if (best_s > 1.f || best_s < 0.f) {
                best_s = sigmoid(best_s);
            }
            if (best_c < 0 || best_c >= 32) {
                continue;
            }
            if (best_c >= 0 && best_c < static_cast<int>(raw_cls_hist.size())) {
                ++raw_cls_hist[static_cast<size_t>(best_c)];
            }
            if (best_c == kYoloClsBasketball) {
                ++raw_ball_cls;
                max_ball_score_seen = std::max(max_ball_score_seen, best_s);
                if (best_s >= 0.005f) {
                    ++raw_ball_ge_5e3;
                }
                if (best_s >= 0.001f) {
                    ++raw_ball_ge_1e3;
                }
                if (best_s >= 0.0002f) {
                    ++raw_ball_ge_2e4;
                }
            } else if (best_c == kYoloClsPlayer) {
                ++raw_player_cls;
                max_player_score_seen = std::max(max_player_score_seen, best_s);
            } else if (best_c == kYoloClsStamBar) {
                ++raw_stam_cls;
                max_stam_score_seen = std::max(max_stam_score_seen, best_s);
            }
            float cmin = get_class_threshold(
                best_c, impl_->conf, impl_->conf_player, impl_->conf_ball, impl_->conf_stam);
            if (decoded_nms_layout) {
                // Decoded-NMS exports can compress score magnitude; use calibrated per-class gates.
                float gate_scale = 1.0f;
                if (best_c == kYoloClsBasketball) {
                    gate_scale = 0.15f;   // 0.15 * 0.15 = 0.0225 effective — was 0.05 (0.0075, too permissive)
                } else if (best_c == kYoloClsStamBar) {
                    gate_scale = 0.20f;
                } else if (best_c == kYoloClsPlayer) {
                    gate_scale = 0.15f;   // was 0.50 — tracking create_conf handles quality gate
                }
                cmin *= gate_scale;
            }
            if (best_c == kYoloClsBasketball) {
                ball_effective_gate = cmin;
            }
            if (best_s < cmin) {
                continue;
            }
            x1 = std::max(0.f, std::min(x1, static_cast<float>(isz - 1)));
            x2 = std::max(0.f, std::min(x2, static_cast<float>(isz - 1)));
            y1 = std::max(0.f, std::min(y1, static_cast<float>(isz - 1)));
            y2 = std::max(0.f, std::min(y2, static_cast<float>(isz - 1)));
            if (x2 <= x1 || y2 <= y1) {
                continue;
            }
            RawDet d;
            d.x1 = x1;
            d.y1 = y1;
            d.x2 = x2;
            d.y2 = y2;
            d.score = best_s;
            d.cls = best_c;
            cand.push_back(d);
        }

        const auto post_nms = nms_class(cand, impl_->iou);
        std::vector<MappedDet> players;
        std::vector<MappedDet> balls;
        std::vector<MappedDet> stams;
        int stams_post_nms = 0;
        int stams_plausible = 0;
        int rejected_ball_plausibility = 0;
        bool have_stam_fallback_raw = false;
        std::array<float, 4> stam_fallback_raw_box{};
        float stam_fallback_raw_score = 0.f;
        players.reserve(16);
        balls.reserve(32);
        stams.reserve(16);
        const auto t_post0 = std::chrono::steady_clock::now();
        for (const auto& d : post_nms) {
            std::array<float, 4> mb{};
            map_box_to_original(d.x1, d.y1, d.x2, d.y2, scale, pad_x, pad_y, w, h, mb);
            if (d.cls == kYoloClsPlayer) {
                players.push_back({mb, d.score});
            } else if (d.cls == kYoloClsBasketball) {
                // Reject oversized false positives before ranking; these can suppress true ball lock.
                if (mb[2] > mb[0] + 1.f && mb[3] > mb[1] + 1.f && ball_box_plausible(mb, w, h)) {
                    balls.push_back({mb, d.score});
                } else {
                    ++rejected_ball_plausibility;
                }
            } else if (d.cls == kYoloClsStamBar) {
                ++stams_post_nms;
                if (stamina_box_plausible(mb, w, h)) {
                    ++stams_plausible;
                    stams.push_back({mb, d.score});
                } else if (!have_stam_fallback_raw || d.score > stam_fallback_raw_score) {
                    // Keep best raw stamina candidate as a fallback when plausibility rejects all.
                    have_stam_fallback_raw = true;
                    stam_fallback_raw_box = mb;
                    stam_fallback_raw_score = d.score;
                }
            }
        }
        size_t template_stam_count = 0;
        bool used_stam_raw_fallback = false;
        if (stams.empty() && have_stam_fallback_raw) {
            stams.push_back({stam_fallback_raw_box, std::max(stam_fallback_raw_score, 0.22f)});
            used_stam_raw_fallback = true;
        }
        if (stams.empty()) {
            const auto templ_stams = detect_stamina_template(bgr, w, h, stride);
            template_stam_count = templ_stams.size();
            stams.insert(stams.end(), templ_stams.begin(), templ_stams.end());
        }
        if (sample_log) {
            // #region agent log
            debug_log_line("pre-fix",
                           "H5",
                           "yolo_onnx.cpp:raw_score_gate",
                           "raw_class_scores",
                           std::string("{\"frame\":") + std::to_string(impl_->frame_i) +
                               ",\"maxBallRaw\":" + std::to_string(max_ball_score_seen) +
                               ",\"maxPlayerRaw\":" + std::to_string(max_player_score_seen) +
                               ",\"maxStamRaw\":" + std::to_string(max_stam_score_seen) +
                               ",\"rawBallCls\":" + std::to_string(raw_ball_cls) +
                               ",\"rawPlayerCls\":" + std::to_string(raw_player_cls) +
                               ",\"rawStamCls\":" + std::to_string(raw_stam_cls) +
                               ",\"stamPostNms\":" + std::to_string(stams_post_nms) +
                               ",\"stamPlausible\":" + std::to_string(stams_plausible) +
                               ",\"stamRawFallback\":" + (used_stam_raw_fallback ? "true" : "false") +
                               ",\"thrBall\":" + std::to_string(impl_->conf_ball) +
                               ",\"thrPlayer\":" + std::to_string(impl_->conf_player) +
                               ",\"thrStam\":" + std::to_string(impl_->conf_stam) +
                               ",\"decodedNms\":" + (decoded_nms_layout ? "true" : "false") +
                               ",\"templateStamCount\":" + std::to_string(template_stam_count) + "}");
            // #endregion
            // #region agent log
            debug_log_line("pre-fix",
                           "H15",
                           "yolo_onnx.cpp:class_histogram",
                           "raw_class_histogram",
                           std::string("{\"frame\":") + std::to_string(impl_->frame_i) +
                               ",\"cls0\":" + std::to_string(raw_cls_hist[0]) +
                               ",\"cls1\":" + std::to_string(raw_cls_hist[1]) +
                               ",\"cls2\":" + std::to_string(raw_cls_hist[2]) +
                               ",\"cls3\":" + std::to_string(raw_cls_hist[3]) +
                               ",\"cls4\":" + std::to_string(raw_cls_hist[4]) +
                               ",\"cls5\":" + std::to_string(raw_cls_hist[5]) + "}");
            // #endregion
        }
        int rejected_ball_court_zone = 0;
        int rejected_ball_person_logic = 0;
        int accepted_ball_score = 0;

        std::array<float, 4> best_stam{};
        bool have_stam = false;
        bool live_stam = false;
        bool carry_stam = false;
        float best_stam_score = -1e9f;
        float best_stam_conf = 0.f;
        for (const auto& s : stams) {
            float ss = s.score * 100.f;
            if (impl_->have_last_stam) {
                const float dist = box_center_dist_xyxy(impl_->last_stam_box, s.box);
                const float sh = std::max(1.f, s.box[3] - s.box[1]);
                const float ref = std::max(30.f, sh * 0.9f);
                ss -= std::min(20.f, (dist / ref) * 7.f);
                if (dist <= ref) {
                    ss += 10.f;
                }
            }
            if (ss > best_stam_score) {
                best_stam_score = ss;
                best_stam_conf = s.score;
                best_stam = s.box;
                have_stam = true;
            }
        }
        if (have_stam) {
            live_stam = true;
            if (impl_->have_last_stam) {
                const float stam_dist_px = box_center_dist_xyxy(impl_->last_stam_box, best_stam);
                // Raised alphas for faster stamina response at 40 fps.
                // Old values (0.52 / 0.64 / 0.74) made the bar visibly lag behind
                // real-time changes. New values snap the bar within 2-3 frames.
                float stam_alpha = 0.82f;
                if (stam_dist_px > 28.f) {
                    stam_alpha = 0.90f;
                }
                if (stam_dist_px > 64.f) {
                    stam_alpha = 0.96f;
                }
                impl_->last_stam_box = lerp_box(impl_->last_stam_box, best_stam, stam_alpha);
            } else {
                impl_->last_stam_box = best_stam;
            }
            impl_->have_last_stam = true;
            impl_->miss_stam = 0;
            best_stam = impl_->last_stam_box;
        } else if (impl_->have_last_stam && impl_->miss_stam < 24) {
            ++impl_->miss_stam;
            best_stam = impl_->last_stam_box;
            have_stam = true;
            carry_stam = true;
        } else {
            impl_->have_last_stam = false;
            impl_->miss_stam = 0;
        }
        if (have_stam) {
            stamina_xyxy = best_stam;
            have_stamina = true;
            if (out_stamina_conf != nullptr) {
                *out_stamina_conf = best_stam_conf;
            }
        }
        if (sample_log) {
            const float sw = have_stam ? (best_stam[2] - best_stam[0]) : 0.f;
            const float sh = have_stam ? (best_stam[3] - best_stam[1]) : 0.f;
            // #region agent log
            debug_log_line("pre-fix",
                           "H1",
                           "yolo_onnx.cpp:stamina_pick",
                           "stamina_selection",
                           std::string("{\"frame\":") + std::to_string(impl_->frame_i) +
                               ",\"stamCandidates\":" + std::to_string(stams.size()) +
                               ",\"haveStam\":" + (have_stam ? "true" : "false") +
                               ",\"liveStam\":" + (live_stam ? "true" : "false") +
                               ",\"carryStam\":" + (carry_stam ? "true" : "false") +
                               ",\"missStam\":" + std::to_string(impl_->miss_stam) +
                               ",\"bestStamScore\":" + std::to_string(best_stam_score) +
                               ",\"stamW\":" + std::to_string(sw) +
                               ",\"stamH\":" + std::to_string(sh) + "}");
            // #endregion
        }

        bool use_stam_for_player = false;
        float min_stam_dx = std::numeric_limits<float>::max();
        float min_stam_dy = std::numeric_limits<float>::max();
        bool used_stam_player_fallback = false;
        if (have_stam) {
            for (const auto& p : players) {
                const float pcx = 0.5f * (p.box[0] + p.box[2]);
                const float pw = std::max(1.f, p.box[2] - p.box[0]);
                const float ph = std::max(1.f, p.box[3] - p.box[1]);
                const float scx = 0.5f * (best_stam[0] + best_stam[2]);
                const float scy = 0.5f * (best_stam[1] + best_stam[3]);
                const float dx = std::abs(pcx - scx);
                const float player_anchor_y = p.box[1] + ph * 0.10f;
                const float dy = std::abs(player_anchor_y - scy);
                min_stam_dx = std::min(min_stam_dx, dx);
                min_stam_dy = std::min(min_stam_dy, dy);
                const float ref_x = std::max(28.f, pw * 0.85f);
                const float ref_y = std::max(22.f, ph * 0.26f);
                if (dx <= ref_x && dy <= ref_y) {
                    use_stam_for_player = true;
                    break;
                }
            }
            if (min_stam_dx > 140.f || min_stam_dy > 80.f) {
                use_stam_for_player = false;
            }
        }
        float best_player_score = -1e9f;
        float best_player_dx = 0.f;
        float best_player_dy = 0.f;
        bool best_player_from_stam = false;
        int player_rejected_by_stam = 0;
        std::array<float, 4> pb{};
        for (const auto& p : players) {
            if (p.score < impl_->conf_player) {
                continue;
            }
            if (impl_->have_last_player) {
                // STRENGTHENED: Reject impossible player jumps more aggressively
                const float dist = box_center_dist_xyxy(impl_->last_player_box, p.box);
                const float ph = std::max(1.f, impl_->last_player_box[3] - impl_->last_player_box[1]);
                
                // Hard reject if jump > 1.8x height AND low confidence
                if (dist > ph * 1.8f && p.score < 0.65f) {
                    continue;  // Likely false detection (court element, etc.)
                }
                // Also reject very large jumps even with moderate confidence
                if (dist > ph * 2.5f && p.score < 0.80f) {
                    continue;
                }
            }
            float s = p.score * 100.f;
            // Restore original "confidence-first" player pick, but stabilize with stamina class when present.
            if (have_stam && use_stam_for_player) {
                const float pcx = 0.5f * (p.box[0] + p.box[2]);
                const float pw = std::max(1.f, p.box[2] - p.box[0]);
                const float ph = std::max(1.f, p.box[3] - p.box[1]);
                const float scx = 0.5f * (best_stam[0] + best_stam[2]);
                const float scy = 0.5f * (best_stam[1] + best_stam[3]);
                const float dx = std::abs(pcx - scx);
                const float player_anchor_y = p.box[1] + ph * 0.10f;
                const float dy = std::abs(player_anchor_y - scy);
                const float ref_x = std::max(28.f, pw * 0.80f);
                const float ref_y = std::max(20.f, ph * 0.24f);
                if (dx > (ref_x * 1.35f) || dy > (ref_y * 1.35f)) {
                    ++player_rejected_by_stam;
                    continue;
                }
                if (dx <= ref_x) {
                    s += 18.f;
                } else {
                    s -= std::min(20.f, (dx / ref_x) * 10.f);
                }
                if (dy <= ref_y) {
                    s += 14.f;
                } else {
                    s -= std::min(18.f, (dy / ref_y) * 8.f);
                }
            }
            if (impl_->have_last_player) {
                const float ph = std::max(1.f, p.box[3] - p.box[1]);
                const float dist = box_center_dist_xyxy(impl_->last_player_box, p.box);
                const float ref = std::max(70.f, ph * 0.85f);
                s -= std::min(30.f, (dist / ref) * 9.f);
                if (dist <= ref) {
                    s += 12.f;
                }
            }
            if (s > best_player_score) {
                best_player_score = s;
                pb = p.box;
                have_player = true;
                best_player_from_stam = have_stam && use_stam_for_player;
                if (have_stam && use_stam_for_player) {
                    const float pcx = 0.5f * (p.box[0] + p.box[2]);
                    const float pcy = 0.5f * (p.box[1] + p.box[3]);
                    const float scx = 0.5f * (best_stam[0] + best_stam[2]);
                    const float scy = 0.5f * (best_stam[1] + best_stam[3]);
                    best_player_dx = std::abs(pcx - scx);
                    best_player_dy = std::abs(pcy - scy);
                } else {
                    best_player_dx = 0.f;
                    best_player_dy = 0.f;
                }
                if (out_player_conf != nullptr) {
                    *out_player_conf = p.score;
                }
            }
        }
        if (!have_player && have_stam && impl_->have_last_player) {
            // Keep player anchored to stamina when detector briefly misses player class.
            const float lw = std::max(1.f, impl_->last_player_box[2] - impl_->last_player_box[0]);
            const float lh = std::max(1.f, impl_->last_player_box[3] - impl_->last_player_box[1]);
            const float scx = 0.5f * (best_stam[0] + best_stam[2]);
            const float scy = 0.5f * (best_stam[1] + best_stam[3]);
            const float top = scy - lh * 0.10f;
            pb = {scx - lw * 0.5f, top, scx + lw * 0.5f, top + lh};
            have_player = true;
            used_stam_player_fallback = true;
            best_player_from_stam = true;
            best_player_score = std::max(best_player_score, 10.f);
            if (out_player_conf != nullptr) {
                *out_player_conf = 0.f;
            }
        }
        if (have_player) {
            if (impl_->have_last_player) {
                const float ph = std::max(1.f, pb[3] - pb[1]);
                const float dist = box_center_dist_xyxy(impl_->last_player_box, pb);
                const float jump_reject = std::max(90.f, ph * 2.5f);
                if (dist > jump_reject && impl_->miss_player < 20) {
                    pb = impl_->last_player_box;
                } else {
                    // Clamp allows config up to 0.75 (j2k_config.json player_ema_alpha).
                    const float player_alpha = std::clamp(impl_->player_ema_alpha, 0.16f, 0.75f);
                    pb = lerp_box(impl_->last_player_box, pb, player_alpha);
                }
            }
            player_xyxy = pb;
            impl_->last_player_box = pb;
            impl_->have_last_player = true;
            impl_->miss_player = 0;
        } else if (impl_->have_last_player && (impl_->miss_player < 36 || have_stam)) {
            ++impl_->miss_player;
            player_xyxy = impl_->last_player_box;
            have_player = true;
            if (out_player_conf != nullptr) {
                *out_player_conf = 0.f;
            }
        } else {
            impl_->have_last_player = false;
            impl_->miss_player = 0;
        }
        if (sample_log) {
            // #region agent log
            debug_log_line("pre-fix",
                           "H2",
                           "yolo_onnx.cpp:player_pick",
                           "player_selection",
                           std::string("{\"frame\":") + std::to_string(impl_->frame_i) +
                               ",\"playerCandidates\":" + std::to_string(players.size()) +
                               ",\"havePlayer\":" + (have_player ? "true" : "false") +
                               ",\"haveStam\":" + (have_stam ? "true" : "false") +
                               ",\"stamInfluenceEnabled\":" +
                               ((have_stam && use_stam_for_player) ? "true" : "false") +
                               ",\"minStamDx\":" +
                               (have_stam ? std::to_string(min_stam_dx) : std::string("0")) +
                               ",\"minStamDy\":" +
                               (have_stam ? std::to_string(min_stam_dy) : std::string("0")) +
                               ",\"stamRejectedPlayers\":" +
                               std::to_string(player_rejected_by_stam) +
                               ",\"usedStamFallback\":" +
                               (used_stam_player_fallback ? "true" : "false") +
                               ",\"usedStamForBest\":" + (best_player_from_stam ? "true" : "false") +
                               ",\"bestPlayerDx\":" + std::to_string(best_player_dx) +
                               ",\"bestPlayerDy\":" + std::to_string(best_player_dy) +
                               ",\"bestPlayerScore\":" + std::to_string(best_player_score) +
                               ",\"missPlayer\":" + std::to_string(impl_->miss_player) + "}");
            // #endregion
        }

        const std::array<float, 4>* person_for_ball =
            have_player ? &player_xyxy : prev_player_hint;
        float best_ball_score = -1e9f;
        std::array<float, 4> bb{};
        const std::array<float, 4>* continuity_ball =
            impl_->have_last_ball ? &impl_->last_ball_box : prev_ball_hint;
        
        // OPTIMIZED: Cache frequently used values outside loop
        const float person_height = (person_for_ball != nullptr)
            ? std::max(1.f, (*person_for_ball)[3] - (*person_for_ball)[1])
            : 0.f;
        const float ball_ref_dist = std::max(40.f, person_height * 0.28f);
        
        for (const auto& bdet : balls) {
            if (!ball_in_valid_court_zone(bdet.box, w, h)) {
                ++rejected_ball_court_zone;
                continue;
            }
            float s = bdet.score * 100.f;
            
            // ADDED: Zone-based scoring penalties for risky detections
            const float cx_norm = 0.5f * (bdet.box[0] + bdet.box[2]) / static_cast<float>(w);
            const float cy_norm = 0.5f * (bdet.box[1] + bdet.box[3]) / static_cast<float>(h);
            
            // Near rack zone → heavy penalty
            if (cx_norm > 0.65f && cy_norm > 0.68f) {
                s -= 15.f;
            }
            // Near top edge (scoreboard) → penalty
            if (cy_norm < 0.25f) {
                s -= 10.f;
            }
            // Near side edges → mild penalty
            if (cx_norm < 0.20f || cx_norm > 0.80f) {
                s -= 5.f;
            }
            
            if (person_for_ball != nullptr) {
                const bool in_zone = ball_box_in_player_search_zone(*person_for_ball,
                                                                    bdet.box,
                                                                    shot_active_context);
                const bool plausible = ball_box_plausible_for_apex(*person_for_ball, bdet.box);

                // Non-shot context: hard-reject any ball candidate that is not within
                // the player's search zone.  During dribbling the ball is always near
                // the player (hand → floor bounce → back).  The soft ±3 penalty was
                // not enough — a distant false positive (crowd, scoreboard, minimap)
                // at confidence 0.70 (score=70) beats an in-zone ball at 0.60 (score=63).
                // We use a slightly expanded bottom allowance (0.45 vs 0.18) so
                // floor-bounce frames where the ball drops below the player's sprite
                // feet are still accepted.
                if (!shot_active_context) {
                    const float pp1  = (*person_for_ball)[1];
                    const float pp2  = (*person_for_ball)[3];
                    const float ppx1 = (*person_for_ball)[0];
                    const float ppx2 = (*person_for_ball)[2];
                    const float ph   = std::max(1.f, pp2 - pp1);
                    const float pw   = std::max(1.f, ppx2 - ppx1);
                    const float bcx  = 0.5f * (bdet.box[0] + bdet.box[2]);
                    const float bcy  = 0.5f * (bdet.box[1] + bdet.box[3]);
                    // Horizontal: ±60% of player width beyond box edges
                    // Vertical:   72% above top, 45% below bottom (generous for floor bounce)
                    const bool in_expanded_zone =
                        (bcx >= ppx1 - pw * 0.60f && bcx <= ppx2 + pw * 0.60f &&
                         bcy >= pp1  - ph * 0.72f && bcy <= pp2  + ph * 0.45f);
                    if (!in_expanded_zone) {
                        ++rejected_ball_person_logic;
                        continue;  // Hard-reject — court FP, scoreboard, crowd, etc.
                    }
                }

                if (in_zone) {
                    s += shot_active_context ? 18.f : 3.f;
                } else {
                    ++rejected_ball_person_logic;
                    s -= shot_active_context ? 18.f : 3.f;
                }
                if (plausible) {
                    s += shot_active_context ? 18.f : 3.f;
                } else {
                    ++rejected_ball_person_logic;
                    s -= shot_active_context ? 12.f : 3.f;
                }
                // OPTIMIZED: Use cached person_height
                const float cy = 0.5f * (bdet.box[1] + bdet.box[3]);
                if (shot_active_context && cy <= (*person_for_ball)[1] + person_height * 0.62f) {
                    s += 6.f;
                }
                // Jersey false-positive rejection: when not in shot context, heavily penalize
                // any ball detection whose centroid falls inside the player's torso region
                // (upper 60% of the player box, within horizontal player bounds ±25%).
                // Jersey numbers, logos, and chest text live exactly here and regularly fool
                // the detector into ignoring the real ball on the court floor.
                if (!shot_active_context) {
                    const float bcx = 0.5f * (bdet.box[0] + bdet.box[2]);
                    const float bcy = 0.5f * (bdet.box[1] + bdet.box[3]);
                    const float pp1 = (*person_for_ball)[1];
                    const float pp2 = (*person_for_ball)[3];
                    const float ppx1 = (*person_for_ball)[0];
                    const float ppx2 = (*person_for_ball)[2];
                    const float pph  = std::max(1.f, pp2 - pp1);
                    const float ppw  = std::max(1.f, ppx2 - ppx1);
                    // Torso zone: upper 5%–65% of player height, within ±25% of player width.
                    // Jersey numbers / logos live exactly here and YOLO regularly rates them
                    // higher than the real ball on the court floor.  Hard-reject the candidate
                    // so it never pollutes best_ball_score (a soft −30 penalty didn't beat a
                    // high-confidence jersey logo whose raw score was initialised to -1e9).
                    const bool in_torso = (bcy > pp1 + pph * 0.05f &&
                                           bcy < pp1 + pph * 0.65f &&
                                           bcx > ppx1 - ppw * 0.25f &&
                                           bcx < ppx2 + ppw * 0.25f);
                    if (in_torso) {
                        continue;  // Hard-reject jersey/logo FP — skip this candidate entirely
                    }
                }
            }
            if (continuity_ball != nullptr) {
                // OPTIMIZED: Use cached value instead of recalculating
                const float ref = ball_ref_dist;
                const float dist = box_center_dist_xyxy(*continuity_ball, bdet.box);
                s -= std::min(18.f, (dist / ref) * 6.f);
                if (dist <= ref * 0.90f) {
                    s += 6.f;
                }
                if (person_for_ball != nullptr && shot_active_context &&
                    dist > person_height * 1.20f &&  // OPTIMIZED: Use cached person_height
                    xyxy_iou_box(*continuity_ball, bdet.box) < 0.05f) {
                    s -= 25.f;
                }
            }
            if (s > best_ball_score) {
                ++accepted_ball_score;
                best_ball_score = s;
                bb = bdet.box;
                have_ball = true;
                if (out_ball_conf != nullptr) {
                    *out_ball_conf = bdet.score;
                }
            }
        }
        bool used_prev_ball_carry = false;
        float applied_ball_alpha = 0.f;
        float smooth_dist_px = 0.f;
        // Shot context: keep 24-frame carry (ball often disappears briefly at apex).
        // Non-shot: cut to 10 frames so a stale ball near the player doesn't linger
        // and block a fresh detection of the real ball elsewhere on court.
        const int max_carry_frames = shot_active_context ? 24 : 10;
        if (have_ball) {
            if (impl_->have_last_ball) {
                float ball_alpha = impl_->ball_ema_alpha;
                smooth_dist_px = box_center_dist_xyxy(impl_->last_ball_box, bb);
                // Base clamp allows config up to 0.58 (ball_ema_alpha default); distance
                // thresholds below then override upward for fast-moving balls.
                ball_alpha = std::clamp(ball_alpha, 0.18f, 0.75f);
                if (smooth_dist_px > 26.f) {
                    ball_alpha = std::max(ball_alpha, 0.56f);
                }
                if (smooth_dist_px > 60.f) {
                    ball_alpha = std::max(ball_alpha, 0.70f);
                }
                if (shot_active_context) {
                    ball_alpha = std::min(0.76f, ball_alpha + 0.04f);
                }
                applied_ball_alpha = ball_alpha;
                bb = lerp_box(impl_->last_ball_box, bb, ball_alpha);
            }
            ball_xyxy = bb;
            impl_->last_ball_box = bb;
            impl_->have_last_ball = true;
            impl_->miss_ball = 0;
        } else if (continuity_ball != nullptr && impl_->miss_ball < max_carry_frames) {
            // Fallback carry: preserve last valid ball anchor when detector briefly misses.
            ball_xyxy = *continuity_ball;
            have_ball = true;
            used_prev_ball_carry = true;
            ++impl_->miss_ball;
            if (out_ball_conf != nullptr) {
                *out_ball_conf = 0.f;
            }
        } else {
            impl_->have_last_ball = false;
            impl_->miss_ball = 0;
        }
        if (sample_log) {
            // #region agent log
            debug_log_line("pre-fix",
                           "H13",
                           "yolo_onnx.cpp:ball_gate_diag",
                           "ball_gate_diagnostics",
                           std::string("{\"frame\":") + std::to_string(impl_->frame_i) +
                               ",\"rawBallCls\":" + std::to_string(raw_ball_cls) +
                               ",\"rawBallGE5e3\":" + std::to_string(raw_ball_ge_5e3) +
                               ",\"rawBallGE1e3\":" + std::to_string(raw_ball_ge_1e3) +
                               ",\"rawBallGE2e4\":" + std::to_string(raw_ball_ge_2e4) +
                               ",\"ballEffectiveGate\":" + std::to_string(ball_effective_gate) +
                               ",\"postNmsBall\":" + std::to_string(balls.size()) +
                               ",\"rejBallPlausibility\":" +
                               std::to_string(rejected_ball_plausibility) +
                               ",\"rejBallCourt\":" + std::to_string(rejected_ball_court_zone) +
                               ",\"rejBallPersonLogic\":" +
                               std::to_string(rejected_ball_person_logic) +
                               ",\"acceptedBallScore\":" + std::to_string(accepted_ball_score) + "}");
            // #endregion
            // #region agent log
            debug_log_line("pre-fix",
                           "H3",
                           "yolo_onnx.cpp:ball_pick",
                           "ball_selection",
                           std::string("{\"frame\":") + std::to_string(impl_->frame_i) +
                               ",\"ballCandidates\":" + std::to_string(balls.size()) +
                               ",\"haveBall\":" + (have_ball ? "true" : "false") +
                               ",\"usedCarry\":" + (used_prev_ball_carry ? "true" : "false") +
                               ",\"appliedBallAlpha\":" + std::to_string(applied_ball_alpha) +
                               ",\"smoothDistPx\":" + std::to_string(smooth_dist_px) +
                               ",\"shotContext\":" + (shot_active_context ? "true" : "false") +
                               ",\"continuityPresent\":" + (continuity_ball != nullptr ? "true" : "false") +
                               ",\"carryWindowFrames\":" + std::to_string(max_carry_frames) +
                               ",\"bestBallScore\":" + std::to_string(best_ball_score) +
                               ",\"missBall\":" + std::to_string(impl_->miss_ball) + "}");
            // #endregion
        }
        if ((impl_->frame_i % 60) == 0) {
            // #region agent log
            debug_log_line("pre-fix",
                           "H12",
                           "yolo_onnx.cpp:detection_quality",
                           "frame_detection_stats",
                           std::string("{\"frame\":") + std::to_string(impl_->frame_i) +
                               ",\"playerCandidates\":" + std::to_string(players.size()) +
                               ",\"ballCandidates\":" + std::to_string(balls.size()) +
                               ",\"staminaCandidates\":" + std::to_string(stams.size()) +
                               ",\"bestPlayerConf\":" +
                               std::to_string(out_player_conf != nullptr ? *out_player_conf : 0.f) +
                               ",\"bestBallConf\":" +
                               std::to_string(out_ball_conf != nullptr ? *out_ball_conf : 0.f) +
                               ",\"bestStaminaConf\":" + std::to_string(best_stam_conf) +
                               ",\"usedPlayerEma\":" + (impl_->have_last_player ? "true" : "false") +
                               ",\"usedBallCarry\":" + (used_prev_ball_carry ? "true" : "false") +
                               ",\"missPlayer\":" + std::to_string(impl_->miss_player) +
                               ",\"missBall\":" + std::to_string(impl_->miss_ball) + "}");
            // #endregion
        }
        if (impl_->debug_det && (impl_->frame_i % 15) == 0) {
            std::fprintf(stderr,
                         "[J2K DET] f=%d players=%d balls=%d stams=%d have_player=%d have_ball=%d carry=%d\n",
                         impl_->frame_i,
                         static_cast<int>(players.size()),
                         static_cast<int>(balls.size()),
                         static_cast<int>(stams.size()),
                         have_player ? 1 : 0,
                         have_ball ? 1 : 0,
                         used_prev_ball_carry ? 1 : 0);
        }

        const auto t1 = std::chrono::steady_clock::now();
        const double total_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
        if (out_ms != nullptr) {
            *out_ms = total_ms;
        }
        if (out_post_ms != nullptr) {
            const double pre = (out_pre_ms != nullptr) ? *out_pre_ms : 0.0;
            const double inf = (out_infer_ms != nullptr) ? *out_infer_ms : 0.0;
            *out_post_ms = std::max(0.0, total_ms - pre - inf);
        }
        return true;
    } catch (const Ort::Exception& e) {
        std::fprintf(stderr, "[J2K YOLO] inference error: %s\n", e.what());
        return false;
    }
}

MultiDetectionResult YoloOnnxRunner::run_multi(const std::uint8_t* bgr, int w, int h, int stride) {
    MultiDetectionResult result;
    if (!loaded_ || impl_->session == nullptr || bgr == nullptr || w < 16 || h < 16 ||
        stride < (w * 3)) {
        return result;
    }

    const auto t0 = std::chrono::steady_clock::now();
    float scale = 1.f;
    int pad_x = 0, pad_y = 0, new_w = 0, new_h = 0;
    const int isz = impl_->input_size;
    letterbox_nchw_rgb(bgr, w, h, stride, isz, impl_->chw.data(), scale, pad_x, pad_y, new_w, new_h);
    const auto t_pre1 = std::chrono::steady_clock::now();
    result.pre_ms = std::chrono::duration<double, std::milli>(t_pre1 - t0).count();

    std::array<int64_t, 4> shape = {1, 3, isz, isz};
    Ort::MemoryInfo mem = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeCPUInput);
    Ort::Value in_tensor = Ort::Value::CreateTensor<float>(
        mem, impl_->chw.data(), impl_->chw.size(), shape.data(), shape.size());

    try {
        const auto t_inf0 = std::chrono::steady_clock::now();
        auto outs = impl_->session->Run(Ort::RunOptions{nullptr},
                                        impl_->in_names.data(), &in_tensor, 1,
                                        impl_->out_names.data(), impl_->out_names.size());
        const auto t_inf1 = std::chrono::steady_clock::now();
        result.infer_ms = std::chrono::duration<double, std::milli>(t_inf1 - t_inf0).count();

        if (outs.empty()) {
            return result;
        }
        float* raw = outs[0].GetTensorMutableData<float>();
        const auto ti = outs[0].GetTensorTypeAndShapeInfo();
        const std::vector<int64_t> dims = ti.GetShape();
        if (dims.size() != 3) {
            return result;
        }
        const int64_t d1 = dims[1];
        const int64_t d2 = dims[2];
        int feat = 0, n_anchors = 0;
        bool feat_major = false;
        if (d1 < d2) {
            feat = static_cast<int>(d1);
            n_anchors = static_cast<int>(d2);
            feat_major = true;
        } else {
            n_anchors = static_cast<int>(d1);
            feat = static_cast<int>(d2);
            feat_major = false;
        }
        if (feat < 6 || n_anchors < 100) {
            return result;
        }
        const int nc = feat - 4;
        const bool decoded_nms_layout = (feat == 6);

        auto at = [&](int f, int i) -> float {
            if (feat_major)
                return raw[static_cast<size_t>(f) * static_cast<size_t>(n_anchors) +
                           static_cast<size_t>(i)];
            return raw[static_cast<size_t>(i) * static_cast<size_t>(feat) +
                       static_cast<size_t>(f)];
        };

        std::vector<RawDet> cand;
        cand.reserve(256);
        for (int i = 0; i < n_anchors; ++i) {
            float x1 = 0.f, y1 = 0.f, x2 = 0.f, y2 = 0.f;
            int best_c = 0;
            float best_s = -1.f;
            if (decoded_nms_layout) {
                x1 = at(0, i); y1 = at(1, i); x2 = at(2, i); y2 = at(3, i);
                best_s = at(4, i);
                best_c = static_cast<int>(std::lround(at(5, i)));
                static int s_1based = -1;
                if (s_1based < 0) {
                    std::string ev;
                    s_1based = (read_env_var("J2K_YOLO_1BASED_CLASSES", ev) &&
                                (ev == "1" || ev == "true")) ? 1 : 0;
                }
                if (s_1based == 1 && best_c >= 1 && best_c <= 3) {
                    best_c -= 1;
                }
            } else {
                const float cx = at(0, i), cy = at(1, i), bw = at(2, i), bh = at(3, i);
                for (int c = 0; c < nc; ++c) {
                    const float s = at(4 + c, i);
                    if (s > best_s) { best_s = s; best_c = c; }
                }
                x1 = cx - 0.5f * bw; y1 = cy - 0.5f * bh;
                x2 = cx + 0.5f * bw; y2 = cy + 0.5f * bh;
            }
            if (best_s > 1.f || best_s < 0.f) {
                best_s = sigmoid(best_s);
            }
            if (best_c < 0 || best_c >= 32) {
                continue;
            }
            float cmin = get_class_threshold(
                best_c, impl_->conf, impl_->conf_player, impl_->conf_ball, impl_->conf_stam);
            if (decoded_nms_layout) {
                float gate_scale = 1.0f;
                if (best_c == kYoloClsBasketball)      gate_scale = 0.15f;
                else if (best_c == kYoloClsStamBar)    gate_scale = 0.20f;
                else if (best_c == kYoloClsPlayer)     gate_scale = 0.15f;  // was 0.50 — tracking create_conf gates quality
                cmin *= gate_scale;
            }
            if (best_s < cmin) { continue; }
            x1 = std::max(0.f, std::min(x1, static_cast<float>(isz - 1)));
            x2 = std::max(0.f, std::min(x2, static_cast<float>(isz - 1)));
            y1 = std::max(0.f, std::min(y1, static_cast<float>(isz - 1)));
            y2 = std::max(0.f, std::min(y2, static_cast<float>(isz - 1)));
            if (x2 <= x1 || y2 <= y1) { continue; }
            RawDet d;
            d.x1 = x1; d.y1 = y1; d.x2 = x2; d.y2 = y2;
            d.score = best_s; d.cls = best_c;
            cand.push_back(d);
        }

        const auto post_nms = nms_class(cand, impl_->iou);
        const auto t_post0 = std::chrono::steady_clock::now();

        // First-inference diagnostic: print top-5 scores per class so threshold
        // calibration can be verified without running a full video analysis.
        // Set env var J2K_SCORE_DIAG=1 to enable on every frame instead.
        {
            static bool s_ran_diag = false;
            std::string score_diag_env;
            const bool every_frame = read_env_var("J2K_SCORE_DIAG", score_diag_env) &&
                                     (score_diag_env == "1" || score_diag_env == "true");
            if (!s_ran_diag || every_frame) {
                s_ran_diag = true;
                // Collect best scores per class from pre-NMS candidates.
                float top_ball[5]  = {0,0,0,0,0};
                float top_plyr[5]  = {0,0,0,0,0};
                float top_stam[5]  = {0,0,0,0,0};
                for (const auto& rd : cand) {
                    auto insert_top = [](float* arr, float v) {
                        for (int k = 0; k < 5; ++k) {
                            if (v > arr[k]) { std::swap(arr[k], v); }
                        }
                    };
                    if (rd.cls == kYoloClsBasketball) insert_top(top_ball, rd.score);
                    else if (rd.cls == kYoloClsPlayer) insert_top(top_plyr, rd.score);
                    else if (rd.cls == kYoloClsStamBar) insert_top(top_stam, rd.score);
                }
                std::fprintf(stdout,
                    "[J2K SCORE_DIAG] pre-NMS top-5 ball=[%.3f,%.3f,%.3f,%.3f,%.3f] "
                    "player=[%.3f,%.3f,%.3f,%.3f,%.3f] "
                    "stam=[%.3f,%.3f,%.3f,%.3f,%.3f] "
                    "thresholds: ball=%.3f player=%.3f stam=%.3f "
                    "total_cand=%d post_nms=%d\n",
                    top_ball[0],top_ball[1],top_ball[2],top_ball[3],top_ball[4],
                    top_plyr[0],top_plyr[1],top_plyr[2],top_plyr[3],top_plyr[4],
                    top_stam[0],top_stam[1],top_stam[2],top_stam[3],top_stam[4],
                    impl_->conf_ball, impl_->conf_player, impl_->conf_stam,
                    static_cast<int>(cand.size()),
                    static_cast<int>(post_nms.size()));
            }
        }

        for (const auto& d : post_nms) {
            std::array<float, 4> mb{};
            map_box_to_original(d.x1, d.y1, d.x2, d.y2, scale, pad_x, pad_y, w, h, mb);
            if (d.cls == kYoloClsPlayer) {
                if (d.score >= impl_->conf_player) {
                    result.players.push_back({mb, d.score});
                }
            } else if (d.cls == kYoloClsBasketball) {
                // Mirror the score gate that run() uses — without this, every post-NMS ball
                // candidate (even at 0.007 confidence) reached tracking_authority, swamping
                // the create_conf gate with low-quality candidates.
                if (d.score >= impl_->conf_ball &&
                    mb[2] > mb[0] + 1.f && mb[3] > mb[1] + 1.f &&
                    ball_box_plausible(mb, w, h)) {
                    result.balls.push_back({mb, d.score});
                }
            } else if (d.cls == kYoloClsStamBar) {
                // Same fix for stamina: gate on conf_stam so low-confidence HUD/floor detections
                // don't reach tracking_authority and anchor the player box in the wrong place.
                if (d.score >= impl_->conf_stam &&
                    stamina_box_plausible(mb, w, h)) {
                    result.staminas.push_back({mb, d.score});
                }
            }
        }
        // Template stamina fallback when ONNX misses the bar entirely.
        if (result.staminas.empty()) {
            const auto templ = detect_stamina_template(bgr, w, h, stride);
            for (const auto& t : templ) {
                result.staminas.push_back({t.box, t.score});
            }
        }

        // Sort each class by score descending so index 0 = best candidate.
        const auto by_score = [](const DetectionCandidate& a,
                                  const DetectionCandidate& b) { return a.score > b.score; };
        std::sort(result.players.begin(), result.players.end(), by_score);
        std::sort(result.balls.begin(), result.balls.end(), by_score);
        std::sort(result.staminas.begin(), result.staminas.end(), by_score);

        const auto t_post1 = std::chrono::steady_clock::now();
        result.post_ms = std::chrono::duration<double, std::milli>(t_post1 - t_post0).count();
        result.total_ms = std::chrono::duration<double, std::milli>(t_post1 - t0).count();
        result.ok = true;
    } catch (const Ort::Exception& e) {
        std::fprintf(stderr, "[J2K YOLO] run_multi error: %s\n", e.what());
    }
    return result;
}

bool YoloOnnxRunner::cuda_active() const {
    return impl_ != nullptr && impl_->cuda_active;
}

const std::string& YoloOnnxRunner::active_provider() const {
    static const std::string kNone = "NONE";
    return impl_ != nullptr ? impl_->active_provider : kNone;
}

const std::string& YoloOnnxRunner::available_providers_csv() const {
    static const std::string kNone = "";
    return impl_ != nullptr ? impl_->available_providers_csv : kNone;
}

}  // namespace j2k::dll
