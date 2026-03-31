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
