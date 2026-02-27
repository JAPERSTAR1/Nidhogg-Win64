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

// Last import slot the HLE layer saw, for the fault report.
uint32_t LastSlot();

// Return address of that import call, i.e. the instruction after the call.
uint64_t LastCallSite();

// Load address of the image, so trace lines can print offsets instead of
// absolute pointers.
void HleSetImageBase(uint64_t base);

}  // namespace ps4
