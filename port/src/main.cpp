// Nidhogg (CUSA00920) native Windows x64 host.
//
// Strategy: the PS4 build is plain x86-64 with a System V ABI, so it is mapped
// and executed as-is inside a Windows process.  The SCE platform layer is
// replaced one library at a time; this file is the harness that makes that
// possible and reports where the image currently gives up.
#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "image.h"
#include "hle.h"
#include "thunks.h"

namespace {

uint64_t g_imageBase = 0;
DWORD g_mainThread = 0;

// Walks the frame-pointer chain of the image's main thread and prints the
// return addresses it finds.  Used by the watchdog when the game parks itself
// in a wait loop and stops calling into the HLE layer, which is exactly the
// case where "last import" tells you nothing.
void DumpMainStack(const char* why) {
    HANDLE th = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT, FALSE, g_mainThread);
    if (!th) return;
    SuspendThread(th);
    CONTEXT c{};
    c.ContextFlags = CONTEXT_FULL;
    if (GetThreadContext(th, &c)) {
        std::printf("\n[watchdog] %s\n", why);
        std::printf("[watchdog] rip=+0x%llx (last import slot %u from +0x%llx)\n",
                    static_cast<unsigned long long>(static_cast<uint64_t>(c.Rip) - g_imageBase),
                    ps4::LastSlot(),
                    static_cast<unsigned long long>(ps4::LastCallSite() - g_imageBase));
        std::printf("[watchdog] stack:");
        uint64_t bp = c.Rbp;
        for (int i = 0; i < 24 && bp; ++i) {
            MEMORY_BASIC_INFORMATION mbi{};
            if (!VirtualQuery(reinterpret_cast<void*>(bp), &mbi, sizeof(mbi)) ||
                mbi.State != MEM_COMMIT || (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) ||
                bp + 16 > reinterpret_cast<uint64_t>(mbi.BaseAddress) + mbi.RegionSize)
                break;
            const uint64_t savedBp = *reinterpret_cast<uint64_t*>(bp);
            const uint64_t ret = *reinterpret_cast<uint64_t*>(bp + 8);
            std::printf(" +0x%llx", static_cast<unsigned long long>(ret - g_imageBase));
            if (savedBp <= bp) break;
            bp = savedBp;
        }
        std::printf("\n");

        // The host frames between the image and the park point break the rbp
        // chain, so scan the raw stack for anything that points into the image.
        MEMORY_BASIC_INFORMATION stack{};
        if (VirtualQuery(reinterpret_cast<void*>(c.Rsp), &stack, sizeof(stack)) &&
            stack.State == MEM_COMMIT) {
            const uint64_t lo = reinterpret_cast<uint64_t>(stack.BaseAddress);
            const uint64_t hi = lo + stack.RegionSize;
            std::printf("[watchdog] image frames on stack:");
            int shown = 0;
            for (uint64_t p = (c.Rsp + 7) & ~7ull; p + 8 <= hi && shown < 20; p += 8) {
                const uint64_t v = *reinterpret_cast<uint64_t*>(p);
                if (v > g_imageBase && v < g_imageBase + 0x41ec48) {
                    std::printf(" +0x%llx", static_cast<unsigned long long>(v - g_imageBase));
                    ++shown;
                }
            }
            std::printf("\n");
        }
        std::fflush(stdout);
    }
    ResumeThread(th);
    CloseHandle(th);
}

// Win64 passes args in rcx/rdx/r8; the PS4 image expects SysV (rdi/rsi/rdx).
// This tail-jump shim moves the first two and hands control to the image, so
// the callee returns straight to the C++ caller with the same stack shape.
using SysvCall = uint64_t (*)(uint64_t fn, uint64_t a0, uint64_t a1);
SysvCall MakeSysvCall() {
    auto* p = static_cast<uint8_t*>(
        VirtualAlloc(nullptr, 0x1000, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    const uint8_t code[] = {0x48, 0x89, 0xD7,   // mov rdi, rdx
                            0x4C, 0x89, 0xC6,   // mov rsi, r8
                            0xFF, 0xE1};        // jmp rcx
    std::memcpy(p, code, sizeof(code));
    FlushInstructionCache(GetCurrentProcess(), p, sizeof(code));
    return reinterpret_cast<SysvCall>(p);
}

LONG WINAPI OnFault(EXCEPTION_POINTERS* ep) {    const auto* rec = ep->ExceptionRecord;

    // Windows libraries raise two exceptions that are pure diagnostics: the
