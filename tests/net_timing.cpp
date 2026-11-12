#include "../port/src/net/timing.h"
#include <cassert>
#include <cstdint>
#include <iostream>

int main() {
    using namespace ps4::net::timing;
    // Independent monotonic origins, symmetric routes and multiple latency scales.
    for (int64_t offset : {-900000000LL, -10000LL, 0LL, 10000LL, 900000000LL}) {
        for (uint64_t oneWay : {1000ULL, 29000ULL, 120000ULL}) {
            const uint64_t localSent = 2000000000ULL;
            const auto peerSent = static_cast<uint64_t>(static_cast<int64_t>(localSent + oneWay) + offset);
            const auto sample = EstimatePeerClock(localSent, peerSent, localSent + 2 * oneWay);
            assert(sample.valid && sample.rttUsec == 2 * oneWay);
            assert(sample.peerOffsetUsec == offset);
            const uint64_t localStart = localSent + 2000000;
            const auto hostStart = static_cast<uint64_t>(static_cast<int64_t>(localStart) + offset);
            assert(LocalDeadline(hostStart, sample.peerOffsetUsec) == localStart);
        }
    }
    // 8 ms forward/reverse asymmetry yields a bounded 4 ms clock estimate error.
    const auto asymmetric = EstimatePeerClock(2000000, 3029000, 2066000);
    assert(asymmetric.valid && asymmetric.peerOffsetUsec == 996000);
    assert(LocalDeadline(5000000, asymmetric.peerOffsetUsec) == 4004000);
    assert(!EstimatePeerClock(100, 200, 99).valid);
    assert(!EstimatePeerClock(100, UINT64_MAX, 200).valid);
    assert(LocalDeadline(0, 1) == 0);
    assert(LocalDeadline(UINT64_MAX, 0) == 0);
    assert(LocalDeadline(0, INT64_MIN) == 0);
    assert(FramePacingCorrection(1000) == 0);
    assert(FramePacingCorrection(100000) == -500);
    assert(FramePacingCorrection(-100000) == 500);
    assert(FramePacingCorrection(std::numeric_limits<double>::quiet_NaN()) == 0);
    for (double initial : {-40000.0, 40000.0}) {
        double error = initial;
        for (int frame = 0; frame < 600; ++frame) error += FramePacingCorrection(error);
        assert(std::abs(error) <= 2000.0);
    }
    std::cout << "Clock-origin, latency, asymmetry and invalid-packet checks passed\n";
}
