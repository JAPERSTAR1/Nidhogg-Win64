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
    bool hasBlocks = false;

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
    uint32_t sampleRate = 48000;
    uint32_t channels = 8;
    bool isFloat = true;
    float volL = 1.0f;
    float volR = 1.0f;
    int openedDeviceId = -999;

    HWAVEOUT hwo = nullptr;
    HANDLE ev = nullptr;
    WAVEHDR hdrs[kNumBufs]{};
    std::vector<int16_t> bufs[kNumBufs];
    int nextBuf = 0;
};

std::mutex g_audioOutMtx;
std::atomic<bool> g_shuttingDown{false};
WavePort g_ports[16];

void CloseWaveDeviceLocked(WavePort& p) {
    if (p.hwo) {
        waveOutReset(p.hwo);
        for (int i = 0; i < WavePort::kNumBufs; ++i)
            waveOutUnprepareHeader(p.hwo, &p.hdrs[i], sizeof(WAVEHDR));
        waveOutClose(p.hwo);
        p.hwo = nullptr;
    }
}

void OpenWaveDeviceLocked(WavePort& p, int deviceId) {
    CloseWaveDeviceLocked(p);
    p.openedDeviceId = deviceId;
    p.nextBuf = 0;
    if (!p.ev) p.ev = CreateEventA(nullptr, FALSE, FALSE, nullptr);

    WAVEFORMATEX wfx{};
    wfx.wFormatTag = WAVE_FORMAT_PCM;
    wfx.nChannels = 2;
    wfx.nSamplesPerSec = p.sampleRate ? p.sampleRate : 48000;
    wfx.wBitsPerSample = 16;
    wfx.nBlockAlign = 4;
    wfx.nAvgBytesPerSec = wfx.nSamplesPerSec * wfx.nBlockAlign;

    const UINT dev = (deviceId < 0) ? WAVE_MAPPER : static_cast<UINT>(deviceId);
    MMRESULT mr = waveOutOpen(&p.hwo, dev, &wfx, reinterpret_cast<DWORD_PTR>(p.ev), 0,
                              CALLBACK_EVENT);
    if (mr != MMSYSERR_NOERROR && dev != WAVE_MAPPER) {
        mr = waveOutOpen(&p.hwo, WAVE_MAPPER, &wfx, reinterpret_cast<DWORD_PTR>(p.ev), 0,
                         CALLBACK_EVENT);
    }
    if (mr == MMSYSERR_NOERROR) {
        for (int i = 0; i < WavePort::kNumBufs; ++i) {
            p.bufs[i].assign(static_cast<size_t>(p.grainSamples * 2), 0);
            std::memset(&p.hdrs[i], 0, sizeof(WAVEHDR));
            p.hdrs[i].lpData = reinterpret_cast<LPSTR>(p.bufs[i].data());
            p.hdrs[i].dwBufferLength = p.grainSamples * 4;
            waveOutPrepareHeader(p.hwo, &p.hdrs[i], sizeof(WAVEHDR));
        }
    } else {
        p.hwo = nullptr;
    }
}

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
        uint8_t cfg[4];
        std::memcpy(cfg, &fmt->configData, 4);
        if (cfg[0] != 0xFE && cfg[3] == 0xFE) std::reverse(cfg, cfg + 4);
        if (h && Atrac9InitDecoder(h, cfg) == 0) {
            Atrac9CodecInfo ci{};
            Atrac9GetCodecInfo(h, &ci);
            if (ci.superframeSize > 0) frameSize = static_cast<uint32_t>(ci.superframeSize);
            if (ci.frameSamples > 0 && ci.framesInSuperframe > 0) {
                frameSamples = static_cast<uint32_t>(ci.frameSamples * ci.framesInSuperframe);
                unitsPerFrame = static_cast<uint32_t>(ci.framesInSuperframe);
                delaySamples = static_cast<uint32_t>(ci.frameSamples);
            }
        }
        if (h) Atrac9ReleaseHandle(h);
    } else if (fmt->waveformType == 0x12) {
        frameSize = 2u * std::max(1u, fmt->numChannels);
        frameSamples = 1;
    } else {
        frameSize = 16u * std::max(1u, fmt->numChannels);
        frameSamples = 28;
    }

    if (HostOk(outFrameSize, 4)) *outFrameSize = frameSize;
    if (HostOk(outNumFrameSamples, 4)) *outNumFrameSamples = frameSamples;
    if (HostOk(outUnitsPerFrame, 4)) *outUnitsPerFrame = unitsPerFrame;
    if (HostOk(outNumDelaySamples, 4)) *outNumDelaySamples = delaySamples;
    return 0;
}

int32_t Ngs2VoiceControl(uint64_t voiceHandle, const void* paramList) {
    std::scoped_lock lk(g_mtx);
    auto it = g_voices.find(voiceHandle);
    if (it == g_voices.end() || !paramList) return 0;
    Voice& v = *it->second;

    const auto* cur = static_cast<const uint8_t*>(paramList);
    for (int guard = 0; guard < 64 && HostOk(cur, sizeof(VoiceParamHeader)); ++guard) {
        const auto* hdr = reinterpret_cast<const VoiceParamHeader*>(cur);
        switch (hdr->id) {
            case 1: {  // MatrixLevelsParam ([outCh * nIn + inCh], 8 output channels)
                if (hdr->size >= 0x18 && HostOk(cur, 0x18)) {
                    const uint32_t nLevels = *reinterpret_cast<const uint32_t*>(cur + 0x0C);
                    const float* lv = *reinterpret_cast<const float* const*>(cur + 0x10);
                    if (lv && nLevels >= 8 && (nLevels % 8) == 0 &&
                        HostOk(lv, nLevels * sizeof(float))) {
                        // Notice 0x100fd0da passes nLevels = 64 with only lv[0..7] populated
                        // for direct 1-to-8 broadcast when channel-mask routing is used.
                        const uint32_t rawIn = nLevels / 8;
                        const uint32_t nIn = (rawIn == 2) ? 2u : 1u;
                        const uint32_t stride = (rawIn == 2) ? 2u : 1u;
                        auto downmixL = [&](uint32_t c) {
                            return lv[0 * stride + c] + 0.7071f * lv[2 * stride + c] +
                                   0.7071f * lv[4 * stride + c] + 0.7071f * lv[6 * stride + c];
                        };
                        auto downmixR = [&](uint32_t c) {
                            return lv[1 * stride + c] + 0.7071f * lv[2 * stride + c] +
                                   0.7071f * lv[5 * stride + c] + 0.7071f * lv[7 * stride + c];
                        };
                        if (nIn == 2) {
                            v.isStereoMat = true;
                            v.matLL = downmixL(0);
                            v.matLR = downmixR(0);
                            v.matRL = downmixL(1);
                            v.matRR = downmixR(1);
                        } else {
                            v.isStereoMat = false;
                            v.matLL = downmixL(0);
                            v.matLR = downmixR(0);
                        }
                    }
                }
                break;
            }
            case 2: {  // PortVolumeParam
                if (hdr->size >= 0x10 && HostOk(cur, 0x10)) {
                    const uint32_t port = *reinterpret_cast<const uint32_t*>(cur + 0x08);
                    const float level = *reinterpret_cast<const float*>(cur + 0x0C);
                    if (port == 0) v.portVol = std::clamp(level, 0.0f, 4.0f);
                }
                break;
            }
            case 6: {  // EventParam
                if (hdr->size >= 0x0C && HostOk(cur, 0x0C)) {
                    const uint32_t ev = *reinterpret_cast<const uint32_t*>(cur + 0x08);
                    if (ev == 0) {         // Play
                        v.playing = true;
                        v.paused = false;
                        v.keyedOff = false;
                    } else if (ev == 1) {  // KeyOff
                        v.keyedOff = true;
                    } else if (ev == 3) {  // Kill
                        v.playing = false;
                        v.paused = false;
                        v.keyedOff = false;
                        v.hasBlocks = false;
                        v.blocks.clear();
                        v.pcmL.clear();
                        v.pcmR.clear();
                        v.readPos = 0.0;
                    } else if (ev == 4) {  // Pause
                        v.paused = true;
                    } else if (ev == 5) {  // Resume
                        v.paused = false;
                    }
                }
                break;
            }
            case 0x10000000u: {  // SamplerVoiceSetupParam
                if (hdr->size >= 0x28 && HostOk(cur, 0x28)) {
                    std::memcpy(&v.fmt, cur + 0x08, sizeof(WaveformFormat));
                    v.hasBlocks = false;
                    v.blocks.clear();
                    v.pcmL.clear();
                    v.pcmR.clear();
                    v.readPos = 0.0;
                    v.vagHist1[0] = v.vagHist1[1] = 0.0;
                    v.vagHist2[0] = v.vagHist2[1] = 0.0;
                    v.totalDecodedSamples = 0;
                    v.totalDecodedBytes = 0;
                    v.currentWavePtr = nullptr;
                    v.at9Valid = false;
                    if (v.fmt.waveformType == 0x40) {
                        if (!v.at9) v.at9 = Atrac9GetHandle();
                        uint8_t cfg[4];
                        std::memcpy(cfg, &v.fmt.configData, 4);
                        if (cfg[0] != 0xFE && cfg[3] == 0xFE) std::reverse(cfg, cfg + 4);
                        if (v.at9 && Atrac9InitDecoder(v.at9, cfg) == 0) {
                            Atrac9GetCodecInfo(v.at9, &v.at9Info);
                            v.at9Valid = true;
                        }
                    }
                }
                break;
            }
            case 0x10000001u: {  // SamplerVoiceWaveformBlocksParam
                if (hdr->size >= 0x20 && HostOk(cur, 0x20)) {
                    const auto* data = *reinterpret_cast<const uint8_t* const*>(cur + 0x08);
                    const uint32_t nBlocks = *reinterpret_cast<const uint32_t*>(cur + 0x14);
                    const auto* aBlock = *reinterpret_cast<const WaveformBlock* const*>(cur + 0x18);
                    if (data && aBlock && nBlocks > 0 && nBlocks <= 16 &&
                        HostOk(aBlock, nBlocks * sizeof(WaveformBlock))) {
                        for (uint32_t i = 0; i < nBlocks; ++i) {
                            if (!aBlock[i].dataSize) continue;
                            QueuedBlock qb;
                            qb.base = data;
                            qb.dataOffset = aBlock[i].dataOffset;
                            qb.dataSize = aBlock[i].dataSize;
                            qb.numRepeats = aBlock[i].numRepeats;
                            qb.numSkipSamples = aBlock[i].numSkipSamples;
                            qb.numSamples = aBlock[i].numSamples;
                            qb.userData = aBlock[i].userData;
                            qb.skipRemaining = aBlock[i].numSkipSamples;
                            if (v.blocks.empty())
                                v.currentWavePtr = data + qb.dataOffset;
                            v.blocks.push_back(qb);
                            v.hasBlocks = true;
                        }
                    }
                }
                break;
            }
            case 0x10000005u: {  // SamplerVoicePitchParam
                if (hdr->size >= 0x10 && HostOk(cur, 0x10)) {
                    const float r = *reinterpret_cast<const float*>(cur + 0x08);
                    if (std::isfinite(r) && r > 0.01f) v.pitchRatio = std::clamp(r, 0.05f, 8.0f);
                }
                break;
            }
            default:
                break;
        }
        if (hdr->next == 0) break;
        cur += hdr->next;
    }
    return 0;
}

int32_t Ngs2VoiceGetStateFlags(uint64_t voiceHandle, uint32_t* outFlags) {
    std::scoped_lock lk(g_mtx);
    uint32_t flags = 0;
    auto it = g_voices.find(voiceHandle);
    if (it != g_voices.end()) {
        const Voice& v = *it->second;
        if (v.playing) {
            flags = 0x05u;
            if (v.paused) flags |= 0x20u;
        }
    }
    if (HostOk(outFlags, 4)) *outFlags = flags;
    return 0;
}

int32_t Ngs2VoiceGetState(uint64_t voiceHandle, void* outState, size_t stateSize) {
    std::scoped_lock lk(g_mtx);
    if (!HostOk(outState, stateSize) || stateSize < 4) return -1;
    std::memset(outState, 0, stateSize);
    auto it = g_voices.find(voiceHandle);
    if (it == g_voices.end()) return 0;

    const Voice& v = *it->second;
    uint32_t flags = 0;
    if (v.playing) {
        flags = 0x05u;
        if (v.paused) flags |= 0x20u;
    }
    auto* b = static_cast<uint8_t*>(outState);
    *reinterpret_cast<uint32_t*>(b + 0x00) = flags;
    if (stateSize >= 0x30 && v.rackId == 0x1000) {
        *reinterpret_cast<float*>(b + 0x04) = v.playing ? 1.0f : 0.0f;
        *reinterpret_cast<float*>(b + 0x08) = v.playing ? 0.5f : 0.0f;
        *reinterpret_cast<uint64_t*>(b + 0x10) = v.totalDecodedSamples;
        *reinterpret_cast<uint64_t*>(b + 0x18) = v.totalDecodedBytes;
        *reinterpret_cast<uint64_t*>(b + 0x20) = v.currentUserData;
        *reinterpret_cast<const void**>(b + 0x28) = v.currentWavePtr;
    }
    return 0;
}

int32_t Ngs2SystemRender(uint64_t systemHandle, const void* bufferInfos, uint32_t numBuffers) {
    std::scoped_lock lk(g_mtx);
    auto sysIt = g_systems.find(systemHandle);
    const uint32_t slotCount = (numBuffers > 0 && numBuffers <= 16) ? numBuffers : 14;
    if (sysIt == g_systems.end() ||
        !HostOk(bufferInfos, slotCount * sizeof(RenderBufferInfo)))
        return -1;

    const System& sys = sysIt->second;
    const uint32_t grain = sys.grainSamples;
    const double outRate = static_cast<double>(sys.sampleRate ? sys.sampleRate : 48000u);

    const auto& cfg = core::CurrentSettings();
    const float masterGain = cfg.mute ? 0.0f : std::clamp(cfg.masterVolume, 0.0f, 1.0f);
    const float musicGain = masterGain * std::clamp(cfg.musicVolume, 0.0f, 1.0f);
    const float sfxGain = masterGain * std::clamp(cfg.sfxVolume, 0.0f, 1.0f);

    std::vector<float> mixL(grain, 0.0f);
    std::vector<float> mixR(grain, 0.0f);
    uint32_t activeCount = 0;

    for (uint64_t rh : sys.rackHandles) {
        auto rIt = g_racks.find(rh);
        if (rIt == g_racks.end() || rIt->second.rackId != 0x1000) continue;

        for (uint64_t vh : rIt->second.voiceHandles) {
            auto vIt = g_voices.find(vh);
            if (vIt == g_voices.end()) continue;
            Voice& v = *vIt->second;
            if (!v.playing || v.paused) continue;
            if (!v.hasBlocks) {
                ++activeCount;
                continue;
            }

            const bool isMusic = (v.fmt.numChannels >= 2) ||
                                 (v.at9Valid && v.at9Info.channels >= 2);
            const float catGain = isMusic ? musicGain : sfxGain;

            const uint32_t srcRate = v.at9Valid && v.at9Info.samplingRate > 0
                                         ? static_cast<uint32_t>(v.at9Info.samplingRate)
                                         : (v.fmt.sampleRate ? v.fmt.sampleRate : 48000u);
            const double step = (static_cast<double>(srcRate) / outRate) * v.pitchRatio;

            bool alive = true;
            for (uint32_t s = 0; s < grain; ++s) {
                while (static_cast<size_t>(v.readPos) + 1 >= v.pcmL.size()) {
                    const size_t drop = static_cast<size_t>(v.readPos);
                    if (drop > 0 && drop <= v.pcmL.size()) {
                        v.pcmL.erase(v.pcmL.begin(), v.pcmL.begin() + static_cast<ptrdiff_t>(drop));
                        v.pcmR.erase(v.pcmR.begin(), v.pcmR.begin() + static_cast<ptrdiff_t>(drop));
                        v.readPos -= static_cast<double>(drop);
                    }
                    if (!DecodeNextChunk(v)) {
                        if (static_cast<size_t>(v.readPos) >= v.pcmL.size()) {
                            alive = false;
                        }
                        break;
                    }
                }
                if (!alive || static_cast<size_t>(v.readPos) >= v.pcmL.size()) {
                    v.playing = false;
                    v.pcmL.clear();
                    v.pcmR.clear();
                    v.readPos = 0.0;
                    break;
                }

                const size_t idx0 = static_cast<size_t>(v.readPos);
                const size_t idx1 = std::min(idx0 + 1, v.pcmL.size() - 1);
                const float frac = static_cast<float>(v.readPos - static_cast<double>(idx0));
                const float inL = v.pcmL[idx0] + (v.pcmL[idx1] - v.pcmL[idx0]) * frac;
                const float inR = v.pcmR[idx0] + (v.pcmR[idx1] - v.pcmR[idx0]) * frac;

                float outL, outR;
                const float vGain = v.portVol * catGain;
                if (v.isStereoMat) {
                    outL = (inL * v.matLL + inR * v.matRL) * vGain;
                    outR = (inL * v.matLR + inR * v.matRR) * vGain;
                } else {
                    outL = inL * v.matLL * vGain;
                    outR = inL * v.matLR * vGain;
                }
                mixL[s] += outL;
                mixR[s] += outR;
                v.readPos += step;
            }
            if (v.playing) ++activeCount;
        }
    }

    g_activeVoices.store(activeCount, std::memory_order_relaxed);
    g_mixedFrames.fetch_add(grain, std::memory_order_relaxed);

    float peak = 0.0f;
    for (uint32_t s = 0; s < grain; ++s) {
        mixL[s] = std::clamp(mixL[s], -1.0f, 1.0f);
        mixR[s] = std::clamp(mixR[s], -1.0f, 1.0f);
        peak = std::max(peak, std::max(std::fabs(mixL[s]), std::fabs(mixR[s])));
    }
    if (peak > 0.0f) g_peakAmp.store(peak, std::memory_order_relaxed);

    const auto* infos = static_cast<const RenderBufferInfo*>(bufferInfos);
    bool wroteMaster = false;
    for (uint32_t b = 0; b < slotCount; ++b) {
        const RenderBufferInfo& bi = infos[b];
        if (!bi.buffer || !bi.numChannels) continue;
        const bool isI16 = (bi.waveformType == 0x12 || bi.waveformType == 0x13);
        const size_t elemBytes = isI16 ? 2 : 4;
        const size_t need = elemBytes * bi.numChannels * grain;
        if (bi.bufferSize < need || !HostOk(bi.buffer, need)) continue;

        std::memset(bi.buffer, 0, need);
        if (wroteMaster) continue;
        wroteMaster = true;

        if (isI16) {
            auto* dst = static_cast<int16_t*>(bi.buffer);
            for (uint32_t s = 0; s < grain; ++s) {
                dst[s * bi.numChannels + 0] = static_cast<int16_t>(mixL[s] * 32767.0f);
                if (bi.numChannels > 1)
                    dst[s * bi.numChannels + 1] = static_cast<int16_t>(mixR[s] * 32767.0f);
            }
        } else {
            auto* dst = static_cast<float*>(bi.buffer);
            for (uint32_t s = 0; s < grain; ++s) {
                dst[s * bi.numChannels + 0] = mixL[s];
                if (bi.numChannels > 1)
                    dst[s * bi.numChannels + 1] = mixR[s];
            }
        }
    }
    return 0;
}

int32_t Ngs2Destroy(uint64_t /*handle*/) { return 0; }

// ------------------------------------------------------------ AudioOut --

int32_t AudioOutOpen(int32_t /*userId*/, int32_t /*type*/, int32_t /*index*/, uint32_t len,
                     uint32_t freq, uint32_t param) {
    std::scoped_lock lk(g_audioOutMtx);
    int slot = -1;
    for (int i = 1; i < 16; ++i) {
        if (!g_ports[i].inUse) { slot = i; break; }
    }
    if (slot < 0) return -1;

    WavePort& p = g_ports[slot];
    p.inUse = true;
    p.grainSamples = (len >= 64 && len <= 4096) ? len : 512;
    p.sampleRate = freq ? freq : 48000;
    const uint32_t fmt = param & 0xffu;
    if (fmt == 0) { p.channels = 1; p.isFloat = false; }
    else if (fmt == 1) { p.channels = 2; p.isFloat = false; }
    else if (fmt == 2) { p.channels = 8; p.isFloat = false; }
    else if (fmt == 3) { p.channels = 1; p.isFloat = true; }
    else if (fmt == 4) { p.channels = 2; p.isFloat = true; }
    else { p.channels = 8; p.isFloat = true; }

    OpenWaveDeviceLocked(p, core::CurrentSettings().audioDeviceId);
    return slot;
}

int32_t AudioOutSetVolume(int32_t handle, uint32_t /*flag*/, const int32_t* vols) {
    std::scoped_lock lk(g_audioOutMtx);
    if (handle <= 0 || handle >= 16 || !g_ports[handle].inUse) return -1;
    if (HostOk(vols, 8)) {
        g_ports[handle].volL = std::clamp(vols[0] / 32768.0f, 0.0f, 2.0f);
        g_ports[handle].volR = std::clamp(vols[1] / 32768.0f, 0.0f, 2.0f);
    }
    return 0;
}

int32_t AudioOutOutput(int32_t handle, const void* ptr) {
    struct SingleParam {
        int32_t h;
        int32_t pad;
        const void* p;
    } sp{handle, 0, ptr};
    return AudioOutOutputs(&sp, 1);
}

int32_t AudioOutOutputs(const void* params, uint32_t count) {
    struct OutParam {
        int32_t handle;
        int32_t pad;
        const void* ptr;
    };
    if (!count || count > 16 || !HostOk(params, count * sizeof(OutParam))) return 0;
    const auto* list = static_cast<const OutParam*>(params);

    if (g_shuttingDown.load(std::memory_order_relaxed)) {
        Sleep(5);
        return 0;
    }

    const int wantDev = core::CurrentSettings().audioDeviceId;
    WavePort* primary = nullptr;
    uint32_t grain = 512;
    {
        std::scoped_lock lk(g_audioOutMtx);
        for (uint32_t i = 0; i < count; ++i) {
            const int32_t h = list[i].handle;
            if (h > 0 && h < 16 && g_ports[h].inUse) {
                primary = &g_ports[h];
                grain = primary->grainSamples;
                if (primary->openedDeviceId != wantDev) {
                    OpenWaveDeviceLocked(*primary, wantDev);
                }
                break;
            }
        }
    }
    if (!primary) {
        Sleep(10);
        return 0;
    }

    std::vector<float> sumL(grain, 0.0f);
    std::vector<float> sumR(grain, 0.0f);
    for (uint32_t i = 0; i < count; ++i) {
        const int32_t h = list[i].handle;
        const void* src = list[i].ptr;
        if (h <= 0 || h >= 16 || !g_ports[h].inUse || !src) continue;
        const WavePort& wp = g_ports[h];
        const uint32_t ch = wp.channels;
        if (wp.isFloat) {
            if (!HostOk(src, grain * ch * sizeof(float))) continue;
            const auto* f = static_cast<const float*>(src);
            for (uint32_t s = 0; s < grain; ++s) {
                const float l = f[s * ch + 0];
                const float r = ch > 1 ? f[s * ch + 1] : l;
                sumL[s] += l * wp.volL;
                sumR[s] += r * wp.volR;
            }
        } else {
            if (!HostOk(src, grain * ch * sizeof(int16_t))) continue;
            const auto* pcm = static_cast<const int16_t*>(src);
            for (uint32_t s = 0; s < grain; ++s) {
                const float l = (pcm[s * ch + 0] / 32768.0f);
                const float r = ch > 1 ? (pcm[s * ch + 1] / 32768.0f) : l;
                sumL[s] += l * wp.volL;
                sumR[s] += r * wp.volR;
            }
        }
    }

    if (!primary->hwo) {
        Sleep((grain * 1000) / 48000);
        return static_cast<int32_t>(grain);
    }

    const int idx = primary->nextBuf;
    primary->nextBuf = (idx + 1) % WavePort::kNumBufs;
    WAVEHDR& hdr = primary->hdrs[idx];

    while (hdr.dwFlags & WHDR_INQUEUE) {
        WaitForSingleObject(primary->ev, 15);
    }

    int16_t* dst = primary->bufs[idx].data();
    for (uint32_t s = 0; s < grain; ++s) {
        dst[s * 2 + 0] = static_cast<int16_t>(std::clamp(sumL[s], -1.0f, 1.0f) * 32767.0f);
        dst[s * 2 + 1] = static_cast<int16_t>(std::clamp(sumR[s], -1.0f, 1.0f) * 32767.0f);
    }
    MMRESULT wr = waveOutWrite(primary->hwo, &hdr, sizeof(WAVEHDR));
    if (wr == MMSYSERR_NODRIVER || wr == WAVERR_UNPREPARED) {
        std::scoped_lock lk(g_audioOutMtx);
        OpenWaveDeviceLocked(*primary, -1);
    }
    return static_cast<int32_t>(grain);
}

int32_t AudioOutClose(int32_t handle) {
    std::scoped_lock lk(g_audioOutMtx);
    if (handle <= 0 || handle >= 16 || !g_ports[handle].inUse) return 0;
    WavePort& p = g_ports[handle];
    CloseWaveDeviceLocked(p);
    if (p.ev) {
        CloseHandle(p.ev);
        p.ev = nullptr;
    }
    p.inUse = false;
    return 0;
}

void ShutdownAudio() {
    g_shuttingDown.store(true, std::memory_order_relaxed);
    std::scoped_lock lk(g_audioOutMtx);
    for (int i = 1; i < 16; ++i) {
        if (!g_ports[i].inUse) continue;
        CloseWaveDeviceLocked(g_ports[i]);
        if (g_ports[i].ev) {
            CloseHandle(g_ports[i].ev);
            g_ports[i].ev = nullptr;
        }
        g_ports[i].inUse = false;
    }
}

uint64_t MixedFrames() { return g_mixedFrames.load(std::memory_order_relaxed); }
float PeakAmplitude() { return g_peakAmp.exchange(0.0f, std::memory_order_relaxed); }
uint32_t ActiveVoices() { return g_activeVoices.load(std::memory_order_relaxed); }

}  // namespace ps4::audio
