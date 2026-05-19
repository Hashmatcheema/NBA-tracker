#include "j2k/dll/config_json_io.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <limits>
#include <sstream>
#include <string>

namespace j2k::dll {

namespace {

size_t skip_ws(const std::string& s, size_t i) {
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' || s[i] == '\r')) {
        ++i;
    }
    return i;
}

/// Walk the JSON text tracking brace/bracket depth and string-literal state, and
/// return the offset of `"<key>":` at the requested depth. Depth 1 = direct child
/// of the root object. Returns std::string::npos if not found.
///
/// This avoids the previous bug where `text.find("\"key\"")` would match a key with
/// the same name nested inside `profiles`, breaking root-level reads on configs that
/// include nested profile maps.
size_t find_key_at_depth(const std::string& text, const char* key, int target_depth) {
    const std::string pat = std::string("\"") + key + "\"";
    int depth = 0;
    bool in_string = false;
    bool escape = false;
    for (size_t i = 0; i < text.size(); ++i) {
        const char c = text[i];
        if (in_string) {
            if (escape) { escape = false; continue; }
            if (c == '\\') { escape = true; continue; }
            if (c == '"') {
                in_string = false;
                // Test if this string token equals our key and is at the right depth.
                if (depth == target_depth && (i + 1 - pat.size()) < text.size() &&
                    text.compare(i + 1 - pat.size(), pat.size(), pat) == 0) {
                    // Confirm a colon follows (key:value), not just a string in an array.
                    size_t j = skip_ws(text, i + 1);
                    if (j < text.size() && text[j] == ':') {
                        return i + 1 - pat.size();
                    }
                }
            }
            continue;
        }
        if (c == '"') { in_string = true; continue; }
        if (c == '{' || c == '[') ++depth;
        else if (c == '}' || c == ']') --depth;
    }
    return std::string::npos;
}

bool extract_json_string_value(const std::string& text, const char* key, std::string& out) {
    const size_t p = find_key_at_depth(text, key, 1);
    if (p == std::string::npos) return false;
    const std::string pat = std::string("\"") + key + "\"";
    size_t c = text.find(':', p + pat.size());
    if (c == std::string::npos) return false;
    size_t q = text.find('"', c + 1);
    if (q == std::string::npos) return false;
    size_t r = text.find('"', q + 1);
    if (r == std::string::npos) return false;
    out.assign(text, q + 1, r - q - 1);
    return !out.empty();
}

bool extract_json_float_value(const std::string& text, const char* key, float& out) {
    const size_t p = find_key_at_depth(text, key, 1);
    if (p == std::string::npos) return false;
    const std::string pat = std::string("\"") + key + "\"";
    size_t c = text.find(':', p + pat.size());
    if (c == std::string::npos) return false;
    size_t i = skip_ws(text, c + 1);
    char* end = nullptr;
    const float v = std::strtof(text.c_str() + i, &end);
    if (end == text.c_str() + i) return false;
    // Schema sanity: refuse non-finite values rather than letting them propagate.
    if (!(v == v) /* NaN */ || v > 1e30f || v < -1e30f) return false;
    out = v;
    return true;
}

bool extract_json_int_value(const std::string& text, const char* key, int& out) {
    const size_t p = find_key_at_depth(text, key, 1);
    if (p == std::string::npos) return false;
    const std::string pat = std::string("\"") + key + "\"";
    size_t c = text.find(':', p + pat.size());
    if (c == std::string::npos) return false;
    size_t i = skip_ws(text, c + 1);
    char* end = nullptr;
    const long v = std::strtol(text.c_str() + i, &end, 10);
    if (end == text.c_str() + i) return false;
    if (v < std::numeric_limits<int>::min() || v > std::numeric_limits<int>::max()) return false;
    out = static_cast<int>(v);
    return true;
}

bool extract_json_bool_value(const std::string& text, const char* key, bool& out) {
    const size_t p = find_key_at_depth(text, key, 1);
    if (p == std::string::npos) return false;
    const std::string pat = std::string("\"") + key + "\"";
    size_t c = text.find(':', p + pat.size());
    if (c == std::string::npos) return false;
    size_t i = skip_ws(text, c + 1);
    if (i >= text.size()) return false;
    if (text.compare(i, 4, "true") == 0) {
        out = true;
        return true;
    }
    if (text.compare(i, 5, "false") == 0) {
        out = false;
        return true;
    }
    int iv = 0;
    if (extract_json_int_value(text, key, iv)) {
        out = (iv != 0);
        return true;
    }
    return false;
}

bool extract_json_object(const std::string& text, const char* key, std::string& out_obj_text) {
    const size_t p = find_key_at_depth(text, key, 1);
    if (p == std::string::npos) return false;
    const std::string pat = std::string("\"") + key + "\"";
    size_t c = text.find(':', p + pat.size());
    if (c == std::string::npos) return false;
    size_t i = skip_ws(text, c + 1);
    if (i >= text.size() || text[i] != '{') return false;
    int depth = 0;
    bool in_string = false;
    bool escape = false;
    size_t end = std::string::npos;
    for (size_t k = i; k < text.size(); ++k) {
        const char ch = text[k];
        if (in_string) {
            if (escape) {
                escape = false;
                continue;
            }
            if (ch == '\\') {
                escape = true;
                continue;
            }
            if (ch == '"') {
                in_string = false;
            }
            continue;
        }
        if (ch == '"') {
            in_string = true;
            continue;
        }
        if (ch == '{') {
            ++depth;
        } else if (ch == '}') {
            --depth;
            if (depth == 0) {
                end = k;
                break;
            }
        }
    }
    if (end == std::string::npos || end <= i) return false;
    out_obj_text.assign(text, i, end - i + 1);
    return true;
}

void apply_track_float(const std::string& obj,
                       const char* key,
                       float min_v,
                       float max_v,
                       float& dst) {
    float v = dst;
    if (extract_json_float_value(obj, key, v) && v >= min_v && v <= max_v) {
        dst = v;
    }
}

void apply_track_int(const std::string& obj,
                     const char* key,
                     int min_v,
                     int max_v,
                     int& dst) {
    int v = dst;
    if (extract_json_int_value(obj, key, v) && v >= min_v && v <= max_v) {
        dst = v;
    }
}

void apply_track_bool(const std::string& obj, const char* key, bool& dst) {
    bool v = dst;
    if (extract_json_bool_value(obj, key, v)) {
        dst = v;
    }
}

}  // namespace

bool parse_j2k_cue_tempo(const std::string& text, float& release_cue, float& tempo_speed) {
    bool ok_rc = extract_json_float_value(text, "release_cue_value", release_cue);
    bool ok_ts = extract_json_float_value(text, "tempo_speed", tempo_speed);
    if (!ok_rc) {
        ok_rc = extract_json_float_value(text, "manual_shot_normal_hold", release_cue);
    }
    if (!ok_ts) {
        ok_ts = extract_json_float_value(text, "manual_shot_normal_tempo", tempo_speed);
    }
    if (ok_rc) {
        release_cue = std::max(0.f, std::min(100.f, release_cue));
    }
    if (ok_ts) {
        tempo_speed = std::max(0.f, std::min(100.f, tempo_speed));
    }
    return ok_rc || ok_ts;
}

bool read_j2k_config_file(const std::string& path, float& release_cue, float& tempo_speed) {
    std::ifstream f(path.c_str(), std::ios::binary);
    if (!f) {
        return false;
    }
    std::ostringstream ss;
    ss << f.rdbuf();
    return parse_j2k_cue_tempo(ss.str(), release_cue, tempo_speed);
}

static bool replace_number_after_key(std::string& s, const char* key, float val) {
    const std::string quoted = std::string("\"") + key + "\"";
    size_t p = s.find(quoted);
    if (p == std::string::npos) {
        return false;
    }
    size_t c = s.find(':', p);
    if (c == std::string::npos) {
        return false;
    }
    size_t i = c + 1;
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' || s[i] == '\r')) {
        ++i;
    }
    size_t j = i;
    if (j < s.size() && (s[j] == '-' || s[j] == '+')) {
        ++j;
    }
    while (j < s.size() && ((s[j] >= '0' && s[j] <= '9') || s[j] == '.')) {
        ++j;
    }
    if (j <= i) {
        return false;
    }
    char buf[48];
    std::snprintf(buf, sizeof(buf), "%.2f", static_cast<double>(val));
    s.replace(i, j - i, buf);
    return true;
}

static void insert_key_before_close(std::string& s, const char* key, float val) {
    char buf[96];
    std::snprintf(buf,
                  sizeof(buf),
                  "  \"%s\": %.2f",
                  key,
                  static_cast<double>(val));
    size_t b = s.rfind('}');
    if (b == std::string::npos) {
        s = "{\n";
        s += buf;
        s += "\n}\n";
        return;
    }
    bool need_comma = false;
    for (size_t k = b; k-- > 0;) {
        if (s[k] == ' ' || s[k] == '\t' || s[k] == '\n' || s[k] == '\r') {
            continue;
        }
        need_comma = (s[k] != '{' && s[k] != ',');
        break;
    }
    std::string ins = need_comma ? std::string(",\n") + buf : std::string("\n") + buf;
    s.insert(b, ins);
}

bool write_j2k_cue_tempo_merge(const std::string& path, float release_cue, float tempo_speed) {
    release_cue = std::max(0.f, std::min(100.f, release_cue));
    tempo_speed = std::max(0.f, std::min(100.f, tempo_speed));

    std::string body;
    {
        std::ifstream inf(path.c_str(), std::ios::binary);
        if (inf) {
            std::ostringstream ss;
            ss << inf.rdbuf();
            body = ss.str();
        }
    }
    if (body.empty()) {
        body = "{\n}\n";
    }

    if (!replace_number_after_key(body, "release_cue_value", release_cue)) {
        insert_key_before_close(body, "release_cue_value", release_cue);
    }
    if (!replace_number_after_key(body, "tempo_speed", tempo_speed)) {
        insert_key_before_close(body, "tempo_speed", tempo_speed);
    }

    namespace fs = std::filesystem;
    const fs::path dest(path);
    const fs::path tmp_path = dest.string() + ".tmp";
    {
        std::ofstream out(tmp_path, std::ios::binary | std::ios::trunc);
        if (!out) {
            return false;
        }
        out << body;
        out.flush();
    }
    std::error_code ec;
    fs::rename(tmp_path, dest, ec);
    if (ec) {
        return false;
    }
    return true;
}

bool read_j2k_yolo_json_file(const std::string& path, J2kYoloJsonSettings& out) {
    std::ifstream f(path.c_str(), std::ios::binary);
    if (!f) {
        return false;
    }
    std::ostringstream ss;
    ss << f.rdbuf();
    const std::string text = ss.str();
    (void)extract_json_string_value(text, "yolo_onnx", out.onnx_relative);
    int iz = out.imgsz;
    if (extract_json_int_value(text, "yolo_imgsz", iz) && iz >= 32 && iz <= 1280) {
        out.imgsz = iz;
    }
    float cf = out.conf;
    if (extract_json_float_value(text, "yolo_conf", cf) && cf > 0.01f && cf < 1.f) {
        out.conf = cf;
    }
    float pc = out.player_conf;
    if (extract_json_float_value(text, "yolo_player_conf", pc) && pc > 0.01f && pc < 1.f) {
        out.player_conf = pc;
    }
    float sc = out.stamina_conf;
    if (extract_json_float_value(text, "yolo_stamina_conf", sc) && sc > 0.01f && sc < 1.f) {
        out.stamina_conf = sc;
    }
    float io = out.iou;
    if (extract_json_float_value(text, "yolo_iou", io) && io > 0.05f && io < 0.99f) {
        out.iou = io;
    }
    float bc = out.ball_conf;
    if (extract_json_float_value(text, "yolo_ball_conf", bc) && bc > 0.01f && bc < 1.f) {
        out.ball_conf = bc;
    }
    float pea = out.player_ema_alpha;
    if (extract_json_float_value(text, "player_ema_alpha", pea) && pea >= 0.01f && pea <= 1.f) {
        out.player_ema_alpha = pea;
    }
    float bea = out.ball_ema_alpha;
    if (extract_json_float_value(text, "ball_ema_alpha", bea) && bea >= 0.01f && bea <= 1.f) {
        out.ball_ema_alpha = bea;
    }
    int threads = out.onnx_threads;
    if (extract_json_int_value(text, "yolo_onnx_threads", threads) && threads >= 1 && threads <= 32) {
        out.onnx_threads = threads;
    }
    return true;
}

bool read_j2k_cv_fadeaway_r2_high(const std::string& path, bool& fadeaway_when_r2_high) {
    std::ifstream f(path.c_str(), std::ios::binary);
    if (!f) {
        return false;
    }
    std::ostringstream ss;
    ss << f.rdbuf();
    int v = 0;
    if (!extract_json_int_value(ss.str(), "cv_fadeaway_r2_high", v)) {
        return false;
    }
    fadeaway_when_r2_high = (v != 0);
    return true;
}

bool read_j2k_motion_thresholds(const std::string& path,
                                int& tracking,
                                int& searching,
                                int& boosted) {
    std::ifstream f(path.c_str(), std::ios::binary);
    if (!f) {
        return false;
    }
    std::ostringstream ss;
    ss << f.rdbuf();
    const std::string text = ss.str();
    bool any = false;
    int v = tracking;
    if (extract_json_int_value(text, "motion_threshold_tracking", v) && v >= 1 && v <= 16) {
        tracking = v;
        any = true;
    }
    v = searching;
    if (extract_json_int_value(text, "motion_threshold_searching", v) && v >= 1 && v <= 16) {
        searching = v;
        any = true;
    }
    v = boosted;
    if (extract_json_int_value(text, "motion_threshold_boosted", v) && v >= 1 && v <= 16) {
        boosted = v;
        any = true;
    }
    return any;
}

bool read_j2k_tracking_json_file(const std::string& path, TrackingAuthorityConfig& out) {
    std::ifstream f(path.c_str(), std::ios::binary);
    if (!f) {
        return false;
    }
    std::ostringstream ss;
    ss << f.rdbuf();
    const std::string text = ss.str();

    std::string tracking_obj;
    if (!extract_json_object(text, "tracking", tracking_obj)) {
        return false;
    }

    std::string player_obj;
    if (extract_json_object(tracking_obj, "player", player_obj)) {
        apply_track_float(player_obj, "min_conf", 0.01f, 0.99f, out.player.min_conf);
        apply_track_float(player_obj, "create_conf", 0.01f, 0.99f, out.player.create_conf);
        apply_track_float(player_obj, "keep_conf", 0.01f, 0.99f, out.player.keep_conf);
        apply_track_int(player_obj, "max_missed_frames", 1, 120, out.player.max_missed_frames);
        apply_track_float(player_obj, "smoothing_alpha", 0.01f, 1.0f, out.player.smoothing_alpha);
        apply_track_float(player_obj, "max_jump_px", 8.f, 1200.f, out.player.max_jump_px);
        apply_track_float(player_obj, "max_velocity_px", 8.f, 2000.f, out.player.max_velocity_px);
        apply_track_float(player_obj, "min_box_area", 0.0f, 1.0f, out.player.min_box_area_norm);
        apply_track_float(player_obj, "max_box_area", 0.0f, 1.0f, out.player.max_box_area_norm);
        apply_track_float(player_obj, "min_aspect", 0.05f, 20.f, out.player.min_aspect);
        apply_track_float(player_obj, "max_aspect", 0.05f, 20.f, out.player.max_aspect);
        apply_track_int(player_obj, "confirm_frames", 1, 12, out.player.confirm_frames);
        apply_track_bool(player_obj, "freeze_identity_during_shot", out.player.freeze_identity_during_shot);
    }

    std::string ball_obj;
    if (extract_json_object(tracking_obj, "basketball", ball_obj)) {
        apply_track_float(ball_obj, "min_conf", 0.01f, 0.99f, out.ball.min_conf);
        apply_track_float(ball_obj, "create_conf", 0.01f, 0.99f, out.ball.create_conf);
        apply_track_float(ball_obj, "keep_conf", 0.01f, 0.99f, out.ball.keep_conf);
        apply_track_int(ball_obj, "max_missed_frames", 1, 60, out.ball.max_missed_frames);
        apply_track_float(ball_obj, "smoothing_alpha", 0.01f, 1.0f, out.ball.smoothing_alpha);
        apply_track_float(ball_obj, "max_jump_px", 8.f, 1600.f, out.ball.max_jump_px);
        apply_track_float(ball_obj, "max_velocity_px", 8.f, 2000.f, out.ball.max_velocity_px);
        apply_track_float(ball_obj, "min_box_area", 0.0f, 1.0f, out.ball.min_box_area_norm);
        apply_track_float(ball_obj, "max_box_area", 0.0f, 1.0f, out.ball.max_box_area_norm);
        apply_track_float(ball_obj, "min_aspect", 0.05f, 20.f, out.ball.min_aspect);
        apply_track_float(ball_obj, "max_aspect", 0.05f, 20.f, out.ball.max_aspect);
        apply_track_int(ball_obj, "confirm_frames", 1, 12, out.ball.confirm_frames);
        apply_track_bool(ball_obj,
                         "require_near_player_when_unstable",
                         out.ball.require_near_player_when_unstable);
        apply_track_bool(ball_obj, "freeze_identity_during_shot", out.ball.freeze_identity_during_shot);
    }

    std::string stamina_obj;
    if (extract_json_object(tracking_obj, "stamina", stamina_obj)) {
        apply_track_float(stamina_obj, "min_conf", 0.01f, 0.99f, out.stamina.min_conf);
        apply_track_float(stamina_obj, "create_conf", 0.01f, 0.99f, out.stamina.create_conf);
        apply_track_float(stamina_obj, "keep_conf", 0.01f, 0.99f, out.stamina.keep_conf);
        apply_track_int(stamina_obj, "max_missed_frames", 1, 120, out.stamina.max_missed_frames);
        apply_track_float(stamina_obj, "smoothing_alpha", 0.01f, 1.0f, out.stamina.smoothing_alpha);
        apply_track_float(stamina_obj, "max_jump_px", 8.f, 1000.f, out.stamina.max_jump_px);
        apply_track_float(stamina_obj, "max_velocity_px", 8.f, 2000.f, out.stamina.max_velocity_px);
        apply_track_float(stamina_obj, "min_box_area", 0.0f, 1.0f, out.stamina.min_box_area_norm);
        apply_track_float(stamina_obj, "max_box_area", 0.0f, 1.0f, out.stamina.max_box_area_norm);
        apply_track_float(stamina_obj, "min_aspect", 0.05f, 40.f, out.stamina.min_aspect);
        apply_track_float(stamina_obj, "max_aspect", 0.05f, 40.f, out.stamina.max_aspect);
        apply_track_int(stamina_obj, "confirm_frames", 1, 12, out.stamina.confirm_frames);
        apply_track_float(stamina_obj,
                          "max_distance_from_player",
                          8.f,
                          1600.f,
                          out.stamina.max_distance_from_player_px);
        apply_track_bool(stamina_obj, "require_player_association", out.stamina.require_player_association);
        apply_track_bool(stamina_obj,
                         "freeze_identity_during_shot",
                         out.stamina.freeze_identity_during_shot);
    }

    // Optional region limits object by class: {"region_limits":{"x_min":...}}
    auto apply_region = [&](const std::string& class_obj, TrackClassConfig& cfg) {
        std::string region_obj;
        if (!extract_json_object(class_obj, "region_limits", region_obj)) {
            return;
        }
        apply_track_float(region_obj, "x_min", 0.f, 1.f, cfg.region_x_min);
        apply_track_float(region_obj, "x_max", 0.f, 1.f, cfg.region_x_max);
        apply_track_float(region_obj, "y_min", 0.f, 1.f, cfg.region_y_min);
        apply_track_float(region_obj, "y_max", 0.f, 1.f, cfg.region_y_max);
    };

    if (!player_obj.empty()) apply_region(player_obj, out.player);
    if (!ball_obj.empty()) apply_region(ball_obj, out.ball);
    if (!stamina_obj.empty()) apply_region(stamina_obj, out.stamina);
    return true;
}

}  // namespace j2k::dll
