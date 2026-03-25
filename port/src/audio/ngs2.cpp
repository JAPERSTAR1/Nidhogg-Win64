#define NOMINMAX
#include "ngs2.h"

#include "../atrac9/libatrac9.h"
#include "../gnm/pm4.h"

#include <windows.h>
#include <mmsystem.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <deque>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace ps4::audio {
namespace {

struct WaveformFormat {
    uint32_t waveformType;
    uint32_t numChannels;
    uint32_t sampleRate;
    uint32_t configData;
    uint32_t frameOffset;
    uint32_t frameMargin;
};

struct WaveformBlock {
    uint32_t dataOffset;
    uint32_t dataSize;
    uint32_t numRepeats;
    uint32_t numSkipSamples;
    uint32_t numSamples;
    uint32_t reserved;
    uint64_t userData;
};

struct VoiceParamHeader {
    uint16_t size;
    int16_t next;
    uint32_t id;
};

struct RenderBufferInfo {
    void* buffer;
    size_t bufferSize;
    uint32_t waveformType;
    uint32_t numChannels;
};

struct QueuedBlock {
    const uint8_t* base = nullptr;
    uint32_t dataOffset = 0;
    uint32_t dataSize = 0;
    uint32_t numRepeats = 0;
    uint32_t numSkipSamples = 0;
    uint32_t numSamples = 0;
    uint64_t userData = 0;

    uint32_t curByteOff = 0;
    uint32_t skipRemaining = 0;
    uint32_t samplesEmitted = 0;
    uint32_t repeatsDone = 0;
};

struct Voice {
    uint64_t handle = 0;
    uint32_t rackId = 0;

    bool playing = false;
    bool paused = false;
    bool keyedOff = false;

    WaveformFormat fmt{};
    void* at9 = nullptr;
    Atrac9CodecInfo at9Info{};
    bool at9Valid = false;

    // VAG IIR history per channel
    double vagHist1[2] = {};
    double vagHist2[2] = {};

    std::deque<QueuedBlock> blocks;
    std::vector<float> pcmL;
    std::vector<float> pcmR;
    double readPos = 0.0;

    float pitchRatio = 1.0f;
    float portVol = 1.0f;
    float matLL = 0.7071f;
    float matLR = 0.7071f;
    float matRL = 0.0f;
    float matRR = 0.7071f;
    bool isStereoMat = false;

    uint64_t totalDecodedSamples = 0;
    uint64_t totalDecodedBytes = 0;
    uint64_t currentUserData = 0;
    const void* currentWavePtr = nullptr;

    ~Voice() {
        if (at9) Atrac9ReleaseHandle(at9);
    }
};

struct Rack {
    uint64_t handle = 0;
    uint32_t rackId = 0;
    std::vector<uint64_t> voiceHandles;
};

struct System {
    uint64_t handle = 0;
    uint32_t sampleRate = 48000;
    uint32_t grainSamples = 512;
    std::vector<uint64_t> rackHandles;
};

std::recursive_mutex g_mtx;
uint64_t g_nextHandle = 0x100;
std::unordered_map<uint64_t, System> g_systems;
std::unordered_map<uint64_t, Rack> g_racks;
std::unordered_map<uint64_t, std::unique_ptr<Voice>> g_voices;

std::atomic<uint64_t> g_mixedFrames{0};
std::atomic<uint32_t> g_activeVoices{0};
std::atomic<float> g_peakAmp{0.0f};

bool HostOk(const void* p, size_t n) {
    return p && ps4::gnm::HostReadable(reinterpret_cast<uint64_t>(p), n);
}

// Sony PS1/PS2/PS3/PS4 16-byte VAG ADPCM block -> 28 samples.
void DecodeVagBlock(const uint8_t* block, double& s1, double& s2, float out[28]) {
    static constexpr double kFilter[5][2] = {
        {0.0, 0.0},
        {60.0 / 64.0, 0.0},
        {115.0 / 64.0, -52.0 / 64.0},
        {98.0 / 64.0, -55.0 / 64.0},
        {122.0 / 64.0, -60.0 / 64.0},
    };
    const uint8_t predictNr = (block[0] >> 4) & 0x0f;
    const uint8_t shiftFactor = block[0] & 0x0f;
    const uint8_t fIdx = predictNr < 5 ? predictNr : 0;
    const double f0 = kFilter[fIdx][0];
    const double f1 = kFilter[fIdx][1];

    for (int i = 0; i < 14; ++i) {
        const uint8_t byte = block[2 + i];
        int nibbles[2] = {
            static_cast<int16_t>((byte & 0x0f) << 12) >> shiftFactor,
            static_cast<int16_t>((byte & 0xf0) << 8) >> shiftFactor,
        };
        for (int k = 0; k < 2; ++k) {
            double s = static_cast<double>(nibbles[k]) + s1 * f0 + s2 * f1;
            s = std::clamp(s, -32768.0, 32767.0);
            s2 = s1;
            s1 = s;
            out[i * 2 + k] = static_cast<float>(s / 32768.0);
        }
    }
}

// Decodes one frame/superframe from the front queued block into v.pcmL / v.pcmR.
// Returns false when no more audio is available and the voice should finish.
bool DecodeNextChunk(Voice& v) {
    while (!v.blocks.empty()) {
        QueuedBlock& b = v.blocks.front();
        if (!b.base || !b.dataSize) {
            v.blocks.pop_front();
            continue;
        }
        v.currentUserData = b.userData;
        v.currentWavePtr = b.base + b.dataOffset + b.curByteOff;

        const bool sampleCapHit = b.numSamples > 0 && b.samplesEmitted >= b.numSamples;
        if (b.curByteOff >= b.dataSize || sampleCapHit) {
            const bool infLoop = (b.numRepeats == 0xFFFFFFFFu) && !v.keyedOff;
            if (infLoop || b.repeatsDone < b.numRepeats) {
                ++b.repeatsDone;
                b.curByteOff = 0;
                b.samplesEmitted = 0;
                b.skipRemaining = b.numSkipSamples;
                v.currentWavePtr = b.base + b.dataOffset;
            } else {
                v.blocks.pop_front();
                continue;
            }
