#pragma once

#include <cstddef>
#include <cstdint>

namespace ps4::audio {

// libSceNgs2 HLE backed by LibAtrac9, PS4 VAG ADPCM, and PCM16/F32 mixing.
int32_t Ngs2SystemCreate(const void* option, const void* allocator, uint64_t* outHandle);
int32_t Ngs2RackCreate(uint64_t systemHandle, uint32_t rackId, const void* option,
                       const void* allocator, uint64_t* outHandle);
int32_t Ngs2RackGetVoiceHandle(uint64_t rackHandle, uint32_t voiceIndex, uint64_t* outHandle);
int32_t Ngs2GetWaveformFrameInfo(const void* format, uint32_t* outFrameSize,
                                 uint32_t* outNumFrameSamples, uint32_t* outUnitsPerFrame,
                                 uint32_t* outNumDelaySamples);
int32_t Ngs2VoiceControl(uint64_t voiceHandle, const void* paramList);
int32_t Ngs2VoiceGetStateFlags(uint64_t voiceHandle, uint32_t* outFlags);
int32_t Ngs2VoiceGetState(uint64_t voiceHandle, void* outState, size_t stateSize);
int32_t Ngs2SystemRender(uint64_t systemHandle, const void* bufferInfos, uint32_t numBuffers);
int32_t Ngs2Destroy(uint64_t handle);

// libSceAudioOut HLE backed by Windows waveOut (48 kHz stereo PCM16).
int32_t AudioOutOpen(int32_t userId, int32_t type, int32_t index, uint32_t len,
                     uint32_t freq, uint32_t param);
int32_t AudioOutSetVolume(int32_t handle, uint32_t flag, const int32_t* vols);
int32_t AudioOutOutput(int32_t handle, const void* ptr);
int32_t AudioOutOutputs(const void* params, uint32_t count);
int32_t AudioOutClose(int32_t handle);

// Diagnostics: total frames mixed and non-silent peak amplitude.
uint64_t MixedFrames();
float PeakAmplitude();
uint32_t ActiveVoices();

}  // namespace ps4::audio
