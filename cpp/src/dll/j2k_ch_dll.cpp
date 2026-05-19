/**
 * j2k_ch.dll — Helios ctypes ABI (r / p / g / x).
 * CV + release logic lives in J2kEngine (C++ only). Tuning UI is Python (j2k_vision_ui).
 */
#include "j2k/dll/gcv_layout.hpp"
#include "j2k/dll/j2k_engine.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <mutex>
#include <memory>

namespace {

std::unique_ptr<j2k::dll::J2kEngine> g_eng;
std::array<std::uint8_t, j2k::dll::kGcvSize> g_gcv{};
std::mutex g_state_mu;
std::string g_script_dir;
std::uint64_t g_p_calls = 0;

std::int64_t now_ms() {
    return static_cast<std::int64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count());
}

void agent_log(const char* hypothesis_id, const char* location, const char* message, const std::string& data_json) {
    (void)hypothesis_id;
    (void)location;
    (void)message;
    (void)data_json;
    // Debug instrumentation disabled after fix verification.
}

bool frame_args_valid(int w, int h, int stride) {
    if (w <= 0 || h <= 0 || stride <= 0) {
        return false;
    }
    // Keep API robust against corrupt host inputs and accidental gigantic dimensions.
    if (w > 8192 || h > 8192) {
        return false;
    }
    const long long min_stride = static_cast<long long>(w) * 3LL;
    if (static_cast<long long>(stride) < min_stride) {
        return false;
    }
    return true;
}

}  // namespace

extern "C" {

#if defined(_WIN32)
#define J2K_CH_EXPORT __declspec(dllexport)
#else
#define J2K_CH_EXPORT __attribute__((visibility("default")))
#endif

J2K_CH_EXPORT int r(int w, int h, const char* script_dir_utf8) {
    if (!frame_args_valid(w, h, w * 3)) {
        return -1;
    }
    std::lock_guard<std::mutex> lock(g_state_mu);
    g_script_dir = script_dir_utf8 ? script_dir_utf8 : "";
    // #region agent log
    agent_log("H55",
              "j2k_ch_dll.cpp:r",
              "dll_r_enter",
              std::string("{\"w\":") + std::to_string(w) + ",\"h\":" + std::to_string(h) +
                  ",\"scriptDir\":\"" + g_script_dir + "\"}");
    // #endregion
    g_p_calls = 0;
    g_eng = std::make_unique<j2k::dll::J2kEngine>();
    // #region agent log
    agent_log("H55",
              "j2k_ch_dll.cpp:r",
              "dll_r_engine_allocated",
              "{\"ok\":true}");
    // #endregion
    g_eng->init(w, h, script_dir_utf8);
    std::fill(g_gcv.begin(), g_gcv.end(), static_cast<std::uint8_t>(0));
    // #region agent log
    agent_log("H7",
              "j2k_ch_dll.cpp:r",
              "dll_r_called",
              std::string("{\"w\":") + std::to_string(w) + ",\"h\":" + std::to_string(h) +
                  ",\"scriptDir\":\"" + g_script_dir + "\"}");
    // #endregion
    return 0;
}

J2K_CH_EXPORT int p(void* frame_ptr, int w, int h, int stride) {
    std::lock_guard<std::mutex> lock(g_state_mu);
    if (!g_eng || frame_ptr == nullptr || !frame_args_valid(w, h, stride)) {
        // #region agent log
        agent_log("H8",
                  "j2k_ch_dll.cpp:p",
                  "dll_p_rejected_args",
                  std::string("{\"haveEng\":") + (g_eng ? "true" : "false") + ",\"frameNull\":" +
                      (frame_ptr == nullptr ? "true" : "false") + ",\"w\":" + std::to_string(w) +
                      ",\"h\":" + std::to_string(h) + ",\"stride\":" + std::to_string(stride) + "}");
        // #endregion
        return -1;
    }
    auto* writable = static_cast<std::uint8_t*>(frame_ptr);
    const auto* bytes = static_cast<const std::uint8_t*>(frame_ptr);
    ++g_p_calls;
    if (g_p_calls == 1ULL) {
        // #region agent log
        agent_log("H25",
                  "j2k_ch_dll.cpp:p",
                  "dll_p_enter_first",
                  std::string("{\"w\":") + std::to_string(w) + ",\"h\":" + std::to_string(h) +
                      ",\"stride\":" + std::to_string(stride) + "}");
        // #endregion
    }
    const auto t0 = now_ms();
    const int rc = g_eng->process_frame(bytes, w, h, stride, g_gcv);
    if (rc >= 0) {
        // Draw the overlay directly onto the host's frame buffer. The same buffer
        // ctypes hands us via `frame_ptr` is what the host displays / writes to disk,
        // so writing into it shows up everywhere the frame is consumed.
        g_eng->draw_overlay(writable, w, h, stride, g_gcv);
    }
    const auto t1 = now_ms();
    if (g_p_calls == 1ULL) {
        // #region agent log
        agent_log("H26",
                  "j2k_ch_dll.cpp:p",
                  "dll_p_exit_first",
                  std::string("{\"rc\":") + std::to_string(rc) + ",\"elapsedMs\":" +
                      std::to_string(static_cast<long long>(t1 - t0)) + "}");
        // #endregion
    }
    if ((g_p_calls % 60ULL) == 0ULL) {
        // #region agent log
        agent_log("H9",
                  "j2k_ch_dll.cpp:p",
                  "dll_p_tick",
                  std::string("{\"calls\":") + std::to_string(g_p_calls) + ",\"w\":" + std::to_string(w) +
                      ",\"h\":" + std::to_string(h) + ",\"stride\":" + std::to_string(stride) +
                      ",\"rc\":" + std::to_string(rc) + "}");
        // #endregion
    }
    return rc;
}

/// Optional: Gtuner IV can set bytes 20-22 (Square, R2a, R2b) before each `p()` so fadeaway
/// detection works when Titan GPC cannot feed R2 into the CV buffer.
J2K_CH_EXPORT void u(int square_0_100, int r2a_0_100, int r2b_0_100) {
    std::lock_guard<std::mutex> lock(g_state_mu);
    auto clamp_u8 = [](int v) -> std::uint8_t {
        if (v < 0) {
            return 0U;
        }
        if (v > 100) {
            return 100U;
        }
        return static_cast<std::uint8_t>(v);
    };
    g_gcv[j2k::dll::kGcvByteSquare] = clamp_u8(square_0_100);
    g_gcv[j2k::dll::kGcvByteR2A] = clamp_u8(r2a_0_100);
    g_gcv[j2k::dll::kGcvByteR2B] = clamp_u8(r2b_0_100);
}

J2K_CH_EXPORT void* g() {
    std::lock_guard<std::mutex> lock(g_state_mu);
    return g_gcv.data();
}

J2K_CH_EXPORT void x() {
    std::lock_guard<std::mutex> lock(g_state_mu);
    if (g_eng) {
        g_eng->shutdown();
        g_eng.reset();
    }
    std::fill(g_gcv.begin(), g_gcv.end(), static_cast<std::uint8_t>(0));
}

}  // extern "C"
