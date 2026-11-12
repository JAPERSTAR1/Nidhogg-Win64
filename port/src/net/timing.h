#pragma once
#include <cstdint>
#include <limits>
#include <algorithm>
#include <cmath>

namespace ps4::net::timing {
struct ClockSample { bool valid = false; uint64_t rttUsec = 0; int64_t peerOffsetUsec = 0; };
inline ClockSample EstimatePeerClock(uint64_t sent, uint64_t peerSent, uint64_t received) {
    constexpr auto max = static_cast<uint64_t>(std::numeric_limits<int64_t>::max());
    if (received < sent || sent > max || peerSent > max || received > max || received - sent > 5000000ULL)
        return {};
    const uint64_t rtt = received - sent;
    return {true, rtt, static_cast<int64_t>(peerSent) - static_cast<int64_t>(sent + rtt / 2)};
}
inline uint64_t LocalDeadline(uint64_t peerDeadline, int64_t peerOffset) {
    constexpr auto max = static_cast<uint64_t>(std::numeric_limits<int64_t>::max());
    if (peerDeadline > max) return 0;
    if (peerOffset >= 0) return static_cast<uint64_t>(peerOffset) > peerDeadline ? 0 : peerDeadline - static_cast<uint64_t>(peerOffset);
    if (peerOffset == std::numeric_limits<int64_t>::min()) return 0;
    const uint64_t magnitude = static_cast<uint64_t>(-peerOffset);
    if (peerDeadline > max - magnitude) return 0;
    return peerDeadline + magnitude;
}
inline int FramePacingCorrection(double phaseErrorUsec) {
    if (!std::isfinite(phaseErrorUsec) || std::fabs(phaseErrorUsec) <= 2000.0) return 0;
    return static_cast<int>(std::clamp(-phaseErrorUsec / 64.0, -500.0, 500.0));
}
}
