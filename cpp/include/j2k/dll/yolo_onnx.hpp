#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace j2k::dll {

/// Class ids for Roboflow export `object.v4i.yolov11` (nc=3).
inline constexpr int kYoloClsBasketball = 0;
inline constexpr int kYoloClsPlayer = 1;
inline constexpr int kYoloClsStamBar = 2;

/// Single ONNX detection candidate (post-NMS, mapped to source pixel space).
struct DetectionCandidate {
    std::array<float, 4> box{};  ///< xyxy in source (1920x1080) pixel space
    float score = 0.f;
};

/// All per-class candidates from one run_multi() call, sorted score-descending.
/// No EMA smoothing, no carry, no cross-class winner selection applied — those
/// are the engine's responsibility when using this API.
struct MultiDetectionResult {
    std::vector<DetectionCandidate> players;   ///< all player candidates
    std::vector<DetectionCandidate> balls;     ///< all ball candidates (basic plausibility filtered)
    std::vector<DetectionCandidate> staminas;  ///< all stamina candidates
    double pre_ms = 0.0;
    double infer_ms = 0.0;
    double post_ms = 0.0;
    double total_ms = 0.0;
    bool ok = false;
};

struct YoloOnnxConfig {
    std::string model_path;
    int input_size = 1280;
    float conf_threshold = 0.25f;
    float player_conf_threshold = 0.f;
    float stamina_conf_threshold = 0.f;
    float iou_threshold = 0.45f;
    /// Minimum score for basketball (class 0). If <= 0, derived from ``conf_threshold``.
    float ball_conf_threshold = 0.f;
    float player_ema_alpha = 0.40f;
    float ball_ema_alpha = 0.45f;
    /// If <= 0, runtime derives a host-friendly default based on CPU cores.
    int onnx_threads = 0;
    /// Absolute path to TRT engine cache directory. If empty, falls back to "./trt_cache".
    /// j2k_engine.cpp injects script_dir/trt_cache so the path is always correct regardless
    /// of the Python process working directory.
    std::string trt_cache_path = "./trt_cache";
};

class YoloOnnxRunner {
public:
    YoloOnnxRunner();
    ~YoloOnnxRunner();
    YoloOnnxRunner(const YoloOnnxRunner&) = delete;
    YoloOnnxRunner& operator=(const YoloOnnxRunner&) = delete;

    bool load(const YoloOnnxConfig& cfg);
    bool loaded() const { return loaded_; }
    bool cuda_active() const;
    const std::string& active_provider() const;
    const std::string& available_providers_csv() const;

    /// Returns ALL per-class candidates after NMS and basic plausibility filtering.
    /// No EMA, no carry, no cross-class winner selection. Sorted score-descending.
    /// Engine uses this for multi-box ownership selection and debug display.
    MultiDetectionResult run_multi(const std::uint8_t* bgr, int w, int h, int stride);

    /// Letterbox -> ONNX -> decode + NMS. Boxes are in **source** pixel space (xyxy).
    bool run(const std::uint8_t* bgr,
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
             double* out_pre_ms = nullptr,
             double* out_infer_ms = nullptr,
             double* out_post_ms = nullptr,
             const std::array<float, 4>* prev_ball_hint = nullptr,
             const std::array<float, 4>* prev_player_hint = nullptr,
             bool shot_active_context = false,
             float* out_ball_conf = nullptr,
             float* out_player_conf = nullptr,
             float* out_stamina_conf = nullptr);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    bool loaded_ = false;
};

}  // namespace j2k::dll
