#include "thunks.h"

#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "hle.h"
#include "image.h"

namespace ps4 {

// The stub has one job: turn a SysV call into a Win64 call into HleCall without
// moving the arguments.  Everything the HLE layer needs is already in place --
// rdi/rsi/rdx/rcx/r8/r9 for the integer arguments, xmm0..7 for the floats --
// so the stub spills both register files into a frame and hands over its
// address as the register save area.  That is exactly the layout a SysV
// va_list descriptor points at, which is why the varargs imports work.
//
// Frame (0xD8 bytes, rsp 16-byte aligned after the sub):
//   +0x00  0x20  shadow space for the Win64 call
//   +0x20 0x30   rdi rsi rdx rcx r8 r9
//   +0x50 0x80   xmm0..xmm7, 16 bytes each
//   +0xD0  0x08  pad
// The return address the image pushed sits at +0xD8 and is the first stack
// argument of a variadic callee, so it goes to HleCall as `ret`.
void Thunks::install(Image& image) {
    HleSetImageBase(reinterpret_cast<uint64_t>(image.base()));

    const size_t perStub = 160;
    const size_t total = image.imports().size() * perStub;
    auto* page = static_cast<uint8_t*>(
        VirtualAlloc(nullptr, total, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    if (!page) return;

    const uint64_t hleFn = reinterpret_cast<uint64_t>(&HleCall);
    size_t i = 0;
    for (uint64_t slot : image.imports()) {
        uint8_t* const start = page + i * perStub;
        uint8_t* c = start;
        stubs_.push_back(start);

        auto emit = [&](std::initializer_list<uint8_t> bytes) {
            for (uint8_t b : bytes) *c++ = b;
        };
        auto emit32 = [&](uint32_t v) { std::memcpy(c, &v, 4); c += 4; };
        auto emit64 = [&](uint64_t v) { std::memcpy(c, &v, 8); c += 8; };
