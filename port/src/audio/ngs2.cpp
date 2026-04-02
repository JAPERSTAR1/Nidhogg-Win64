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
        }

        const uint8_t* src = b.base + b.dataOffset + b.curByteOff;
        const uint32_t avail = b.dataSize - b.curByteOff;

        if (v.fmt.waveformType == 0x40) {  // ATRAC9
            if (!v.at9Valid || v.at9Info.superframeSize <= 0) {
                v.blocks.clear();
                return false;
            }
            const uint32_t sfBytes = static_cast<uint32_t>(v.at9Info.superframeSize);
            if (avail < sfBytes || !HostOk(src, sfBytes)) {
                b.curByteOff = b.dataSize;
                continue;
            }
            const int ch = std::max(1, v.at9Info.channels);
            const int fSamples = v.at9Info.frameSamples;
            const int nFrames = v.at9Info.framesInSuperframe;
            std::vector<int16_t> temp(static_cast<size_t>(fSamples * nFrames * ch));

            int subOff = 0;
            for (int f = 0; f < nFrames; ++f) {
                if (subOff >= static_cast<int>(sfBytes)) break;
                int used = 0;
                if (Atrac9Decode(v.at9, src + subOff,
                                 temp.data() + static_cast<size_t>(f * fSamples * ch), &used) != 0)
                    break;
                subOff += used;
            }

            b.curByteOff += sfBytes;
            v.totalDecodedBytes += sfBytes;
            v.currentWavePtr = b.base + b.dataOffset + b.curByteOff;

            const int totalFrames = fSamples * nFrames;
            for (int i = 0; i < totalFrames; ++i) {
                if (b.skipRemaining > 0) {
                    --b.skipRemaining;
                    continue;
                }
                if (b.numSamples > 0 && b.samplesEmitted >= b.numSamples) break;
                const float l = temp[static_cast<size_t>(i * ch)] / 32768.0f;
                const float r = ch > 1 ? (temp[static_cast<size_t>(i * ch + 1)] / 32768.0f) : l;
                v.pcmL.push_back(l);
                v.pcmR.push_back(r);
                ++b.samplesEmitted;
                ++v.totalDecodedSamples;
            }
            if (!v.pcmL.empty()) return true;
        } else if (v.fmt.waveformType == 0x12) {  // PCM16LE
            const uint32_t ch = std::max(1u, v.fmt.numChannels);
            const uint32_t frameBytes = 2u * ch;
            const uint32_t framesToRead = std::min(avail / frameBytes, 256u);
            if (!framesToRead || !HostOk(src, framesToRead * frameBytes)) {
                b.curByteOff = b.dataSize;
                continue;
            }
            const auto* pcm = reinterpret_cast<const int16_t*>(src);
            for (uint32_t i = 0; i < framesToRead; ++i) {
                if (b.skipRemaining > 0) { --b.skipRemaining; continue; }
                const float l = pcm[i * ch] / 32768.0f;
                const float r = ch > 1 ? (pcm[i * ch + 1] / 32768.0f) : l;
                v.pcmL.push_back(l);
                v.pcmR.push_back(r);
                ++b.samplesEmitted;
                ++v.totalDecodedSamples;
            }
            b.curByteOff += framesToRead * frameBytes;
            v.totalDecodedBytes += framesToRead * frameBytes;
            v.currentWavePtr = b.base + b.dataOffset + b.curByteOff;
            if (!v.pcmL.empty()) return true;
        } else {  // Sony 16-byte VAG ADPCM
            const uint32_t ch = std::max(1u, v.fmt.numChannels);
            const uint32_t blockBytes = 16u * ch;
            if (avail < blockBytes || !HostOk(src, blockBytes)) {
                b.curByteOff = b.dataSize;
                continue;
            }
            float ch0[28]{}, ch1[28]{};
            DecodeVagBlock(src, v.vagHist1[0], v.vagHist2[0], ch0);
            if (ch > 1) DecodeVagBlock(src + 16, v.vagHist1[1], v.vagHist2[1], ch1);
            const bool endFlag = (src[1] & 1u) != 0 && (src[1] & 2u) == 0;
            b.curByteOff += blockBytes;
            v.totalDecodedBytes += blockBytes;
            v.currentWavePtr = b.base + b.dataOffset + b.curByteOff;

            for (int i = 0; i < 28; ++i) {
                if (b.skipRemaining > 0) { --b.skipRemaining; continue; }
                const float l = ch0[i];
                const float r = ch > 1 ? ch1[i] : l;
                v.pcmL.push_back(l);
                v.pcmR.push_back(r);
                ++b.samplesEmitted;
                ++v.totalDecodedSamples;
            }
            if (endFlag) b.curByteOff = b.dataSize;
            if (!v.pcmL.empty()) return true;
        }
    }
    return false;
}

// ------------------------------------------------------------ AudioOut waveOut --

struct WavePort {
    static constexpr int kNumBufs = 6;
    bool inUse = false;
    uint32_t grainSamples = 512;
    uint32_t channels = 8;
    bool isFloat = true;
    float volL = 1.0f;
    float volR = 1.0f;

    HWAVEOUT hwo = nullptr;
    HANDLE ev = nullptr;
    WAVEHDR hdrs[kNumBufs]{};
    std::vector<int16_t> bufs[kNumBufs];
    int nextBuf = 0;
};

std::mutex g_audioOutMtx;
WavePort g_ports[16];

}  // namespace

int32_t Ngs2SystemCreate(const void* option, const void* /*allocator*/, uint64_t* outHandle) {
    std::scoped_lock lk(g_mtx);
    System sys;
    sys.handle = g_nextHandle++;
    if (HostOk(option, 0x28)) {
        const auto* u32 = reinterpret_cast<const uint32_t*>(
            static_cast<const uint8_t*>(option) + 24);
        if (u32[2] >= 64 && u32[2] <= 2048) sys.grainSamples = u32[2];
        if (u32[3] >= 8000 && u32[3] <= 192000) sys.sampleRate = u32[3];
    }
    if (HostOk(outHandle, 8)) *outHandle = sys.handle;
    g_systems.emplace(sys.handle, std::move(sys));
    return 0;
}

int32_t Ngs2RackCreate(uint64_t systemHandle, uint32_t rackId, const void* option,
                       const void* /*allocator*/, uint64_t* outHandle) {
    std::scoped_lock lk(g_mtx);
    auto sysIt = g_systems.find(systemHandle);
    if (sysIt == g_systems.end()) return -1;

    uint32_t maxVoices = 32;
    if (HostOk(option, 0x28)) {
        const uint32_t v = *reinterpret_cast<const uint32_t*>(
            static_cast<const uint8_t*>(option) + 32);
        if (v > 0 && v <= 1024) maxVoices = v;
    }

    Rack rack;
    rack.handle = g_nextHandle++;
    rack.rackId = rackId;
    rack.voiceHandles.reserve(maxVoices);
    for (uint32_t i = 0; i < maxVoices; ++i) {
        auto v = std::make_unique<Voice>();
        v->handle = g_nextHandle++;
        v->rackId = rackId;
        rack.voiceHandles.push_back(v->handle);
        g_voices.emplace(v->handle, std::move(v));
    }

    if (HostOk(outHandle, 8)) *outHandle = rack.handle;
    sysIt->second.rackHandles.push_back(rack.handle);
    g_racks.emplace(rack.handle, std::move(rack));
    return 0;
}

int32_t Ngs2RackGetVoiceHandle(uint64_t rackHandle, uint32_t voiceIndex, uint64_t* outHandle) {
    std::scoped_lock lk(g_mtx);
    auto it = g_racks.find(rackHandle);
    if (it == g_racks.end() || voiceIndex >= it->second.voiceHandles.size()) return -1;
    if (HostOk(outHandle, 8)) *outHandle = it->second.voiceHandles[voiceIndex];
    return 0;
}

int32_t Ngs2GetWaveformFrameInfo(const void* format, uint32_t* outFrameSize,
                                 uint32_t* outNumFrameSamples, uint32_t* outUnitsPerFrame,
                                 uint32_t* outNumDelaySamples) {
    if (!HostOk(format, sizeof(WaveformFormat))) return -1;
    const auto* fmt = static_cast<const WaveformFormat*>(format);

    uint32_t frameSize = 4;
    uint32_t frameSamples = 1;
    uint32_t unitsPerFrame = 1;
    uint32_t delaySamples = 0;

    if (fmt->waveformType == 0x40) {
        void* h = Atrac9GetHandle();
