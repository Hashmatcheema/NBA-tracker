#pragma once

#include <cstdint>

namespace j2k::dll {

/// GCV packet size returned by ``p()``. Bytes 0–31 match the legacy 32-byte layout (Titan / GPC).
/// Bytes 32–39 carry fire-zone and bytes 40–47 carry stamina bar for PC overlay only.
inline constexpr int kGcvSize = 48;
inline constexpr int kGcvCvFlag = 0;
inline constexpr int kGcvStick1Y = 4;
inline constexpr int kGcvSqBlock = 8;
/// Host injects raw Square 0–100 before p(). Must NOT overlap kGcvOutPlayerBox (12–19): that
/// region is overwritten with bbox bytes each frame, so reading byte 16 was fed back bbox noise
/// as "square held" and forced stick 100.
inline constexpr int kGcvByteSquare = 20;
/// R2 / modifier bytes for fadeaway detection (0–100). Kept clear of packed fixed32 fields at 0–11.
inline constexpr int kGcvByteR2A = 21;
inline constexpr int kGcvByteR2B = 22;

/// Optional debug overlay: normalized player stub box (big-endian uint16 0..65535), bytes [12..19].
inline constexpr int kGcvOutPlayerBox = 12;
/// Optional debug overlay: ball / motion box, bytes [24..31]. Titan / GPC typically only read 0 and 4.
inline constexpr int kGcvOutBallBox = 24;
/// Fire zone rectangle for PC debug overlay only, bytes [32..39].
inline constexpr int kGcvOutFireZone = 32;
/// Stamina bar rectangle for PC debug overlay only, bytes [40..47].
inline constexpr int kGcvOutStamBox = 40;

inline void pack_fixed16(float val, std::uint8_t* d) {
    if (val > 100.0f) {
        val = 100.0f;
    }
    if (val < -100.0f) {
        val = -100.0f;
    }
    auto q = static_cast<std::int32_t>(static_cast<float>(val) * 65536.0f);
    d[0] = static_cast<std::uint8_t>((static_cast<std::uint32_t>(q) >> 24) & 0xffU);
    d[1] = static_cast<std::uint8_t>((static_cast<std::uint32_t>(q) >> 16) & 0xffU);
    d[2] = static_cast<std::uint8_t>((static_cast<std::uint32_t>(q) >> 8) & 0xffU);
    d[3] = static_cast<std::uint8_t>(static_cast<std::uint32_t>(q) & 0xffU);
}

inline float stick_quantized_for_output(float v) {
    if (v > 50.0f) {
        return 100.0f;
    }
    if (v < -50.0f) {
        return -100.0f;
    }
    return 0.0f;
}

}  // namespace j2k::dll
