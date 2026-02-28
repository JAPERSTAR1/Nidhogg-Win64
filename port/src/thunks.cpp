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

        emit({0x48, 0x81, 0xEC, 0xD8, 0x00, 0x00, 0x00});  // sub rsp, 0xD8
        emit({0x48, 0x89, 0x7C, 0x24, 0x20});              // mov [rsp+20], rdi
        emit({0x48, 0x89, 0x74, 0x24, 0x28});              // mov [rsp+28], rsi
        emit({0x48, 0x89, 0x54, 0x24, 0x30});              // mov [rsp+30], rdx
        emit({0x48, 0x89, 0x4C, 0x24, 0x38});              // mov [rsp+38], rcx
        emit({0x4C, 0x89, 0x44, 0x24, 0x40});              // mov [rsp+40], r8
        emit({0x4C, 0x89, 0x4C, 0x24, 0x48});              // mov [rsp+48], r9
        emit({0x0F, 0x11, 0x44, 0x24, 0x50});              // movups [rsp+50], xmm0
        emit({0x0F, 0x11, 0x4C, 0x24, 0x60});              // movups [rsp+60], xmm1
        emit({0x0F, 0x11, 0x54, 0x24, 0x70});              // movups [rsp+70], xmm2
        emit({0x0F, 0x11, 0x9C, 0x24, 0x80, 0x00, 0x00, 0x00});  // [rsp+80], xmm3
        emit({0x0F, 0x11, 0xA4, 0x24, 0x90, 0x00, 0x00, 0x00});  // [rsp+90], xmm4
        emit({0x0F, 0x11, 0xAC, 0x24, 0xA0, 0x00, 0x00, 0x00});  // [rsp+A0], xmm5
        emit({0x0F, 0x11, 0xB4, 0x24, 0xB0, 0x00, 0x00, 0x00});  // [rsp+B0], xmm6
        emit({0x0F, 0x11, 0xBC, 0x24, 0xC0, 0x00, 0x00, 0x00});  // [rsp+C0], xmm7
        emit({0x4C, 0x8B, 0x84, 0x24, 0xD8, 0x00, 0x00, 0x00});  // mov r8, [rsp+D8]
        emit({0x4C, 0x8D, 0x8C, 0x24, 0xE0, 0x00, 0x00, 0x00});  // lea r9, [rsp+E0]
        emit({0xB9}); emit32(static_cast<uint32_t>(i));          // mov ecx, slot
        emit({0x48, 0x8D, 0x54, 0x24, 0x20});                    // lea rdx, [rsp+20]
        emit({0x48, 0xB8}); emit64(hleFn);                       // mov rax, HleCall
        emit({0xFF, 0xD0});                                      // call rax
        emit({0x48, 0x81, 0xC4, 0xD8, 0x00, 0x00, 0x00});        // add rsp, 0xD8
        emit({0x66, 0x48, 0x0F, 0x6E, 0xC0});                    // movq xmm0, rax
        emit({0xC3});                                            // ret

        const size_t used = static_cast<size_t>(c - start);
        if (used > perStub) {
            std::printf("[thunks] stub %zu overflowed (%zu > %zu)\n", i, used, perStub);
            std::abort();
        }

        *reinterpret_cast<uint64_t*>(image.at(slot)) =
            reinterpret_cast<uint64_t>(start);
        ++i;
    }
    FlushInstructionCache(GetCurrentProcess(), page, total);
}

}  // namespace ps4
