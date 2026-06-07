// Host-side implementations of the imports the image calls.
//
// The image is the original PS4 code, so every import is a real SCE/libc
// function it expects to exist.  Slots are identified by call-site behaviour:
// the surrounding code shows the argument shape and the error strings name the
// API.  Anything not yet identified returns 0 and is reported.
#pragma once

#include <cstdint>

namespace ps4 {

// Register snapshot taken by the stub before it calls into C++.  The layout
// matches the System V va_list register save area on purpose: shrinking
// wrappers can point a va_list straight at it.
struct ImportRegs {
    uint64_t gpr[6];          // rdi rsi rdx rcx r8 r9
    uint8_t xmm[8][16];       // xmm0..xmm7
};

// Executes import `slot` and returns its value (rax, mirrored into xmm0).
uint64_t HleCall(uint32_t slot, const ImportRegs& regs, uint64_t ret, const uint64_t* stack);

// Human name for a slot, or nullptr.
const char* SlotName(uint32_t slot);

// Lifecycle & telemetry
void RequestQuit();
bool IsQuitRequested();
void HleShutdown();
void* GameWindow();
uint64_t SubmittedFrames();
std::string CurrentRoom();
uint64_t ComputeGuestStateHash();

// Netplay frame-synchronized input & determinism hook.
// When active and returning true, overrides P1/P2 pad state and locks guest
// PRNG + wall clock to the synchronized match frame.
using NetplayStepFn = bool (*)(uint64_t simFrame, uint32_t localButtons,
                               uint32_t& outP1Buttons, uint32_t& outP2Buttons,
                               uint32_t& ioRandState);
void SetNetplayStepCallback(NetplayStepFn fn);

}  // namespace ps4
