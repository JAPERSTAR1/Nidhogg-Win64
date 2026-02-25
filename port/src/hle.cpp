#include "hle.h"

#include "gnm/d3d11.h"
#include "gnm/pm4.h"

#include <windows.h>
#include <direct.h>
#include <wincodec.h>
#include <wrl/client.h>
#include <xinput.h>

#include <bit>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace ps4 {
namespace {

// ------------------------------------------------------------ trace state --

thread_local uint32_t g_lastSlot = 0xffffffff;
thread_local uint64_t g_lastRet = 0;
uint64_t g_base = 0;
uint64_t g_logLimit = 0;   // per-call trace budget; 0 turns it off
uint64_t g_traceFrom = 0;  // frame to begin tracing from
uint64_t g_calls = 0;
bool g_configured = false;
bool g_seen[512] = {};
uint64_t g_slotCalls[512] = {};   // since the last histogram reset

void Configure() {
    if (g_configured) return;
    g_configured = true;
    char* lim = nullptr;
    size_t n = 0;
    if (_dupenv_s(&lim, &n, "NIDHOGG_IMPORT_LIMIT") == 0 && lim) {
        g_logLimit = std::strtoull(lim, nullptr, 0);
        std::free(lim);
    }
    if (_dupenv_s(&lim, &n, "NIDHOGG_TRACE_FROM") == 0 && lim) {
        g_traceFrom = std::strtoull(lim, nullptr, 0);
        std::free(lim);
    }
}

// Reads a C string out of the image without dying on a null the game is about
// to hand the real libc anyway.
const char* C(uint64_t p) {
    return p ? reinterpret_cast<const char*>(p) : "";
}

// ---------------------------------------------------------------- varargs --

// System V x86-64 va_list.  The image is SysV throughout, so its printf-family
// calls hand us this structure and nothing about it is Win64.
struct VaList {
    uint32_t gp;          // next general-purpose slot, bytes into reg_save
    uint32_t fp;          // next vector slot, bytes into reg_save
    void* overflow;       // next stack argument
    void* reg_save;       // [6 x u64][8 x 16-byte xmm]
};

uint64_t VaGp(VaList* v) {
    if (v->gp < 48) {
        uint64_t r;
        std::memcpy(&r, static_cast<uint8_t*>(v->reg_save) + v->gp, 8);
        v->gp += 8;
        return r;
    }
    uint64_t r;
    std::memcpy(&r, v->overflow, 8);
    v->overflow = static_cast<uint8_t*>(v->overflow) + 8;
    return r;
}

double VaFp(VaList* v) {
    if (v->fp < 48 + 8 * 16) {
        double d;
        std::memcpy(&d, static_cast<uint8_t*>(v->reg_save) + v->fp, 8);
        v->fp += 16;
        return d;
    }
    double d;
    std::memcpy(&d, v->overflow, 8);
    v->overflow = static_cast<uint8_t*>(v->overflow) + 8;
    return d;
}

// Formats one SCE format string into `out`.  Conversions go through the host's
// snprintf one at a time after the argument has been pulled off the SysV list,
// which keeps the width/precision/flags handling in the CRT where it belongs.
size_t FormatVa(char* out, size_t cap, const char* fmt, VaList* va) {
    size_t n = 0;
    auto put = [&](const char* s, size_t len) {
        if (n + len >= cap) len = (n < cap) ? cap - n - 1 : 0;
        std::memcpy(out + n, s, len);
        n += len;
    };
    if (cap == 0) return 0;

    for (const char* p = fmt; *p;) {
        if (*p != '%') {
            put(p, 1);
            ++p;
            continue;
        }
        const char* start = p++;
        if (*p == '%') { put("%", 1); ++p; continue; }

        std::string spec = "%";
        while (*p && std::strchr("-+ #0'", *p)) { spec += *p++; }
        while (*p >= '0' && *p <= '9') spec += *p++;

        std::string prec;
        if (*p == '.') {
            prec += *p++;
            while (*p >= '0' && *p <= '9') prec += *p++;
        }
        spec += prec;

        int width = 0;
        int precision = -1;
        std::sscanf(spec.c_str() + 1, "%d.%d", &width, &precision);

        // length modifiers
        int lng = 0;  // 0 int, 1 long long, 2 char, 3 short
        bool isSize = false, isPtrdiff = false;
        if (*p == 'l') { lng = 1; ++p; if (*p == 'l') ++p; }
        else if (*p == 'h') { lng = 3; ++p; if (*p == 'h') { lng = 2; ++p; } }
        else if (*p == 'z' || *p == 'j' || *p == 't') { lng = 1; isSize = true; ++p; }
        else if (*p == 'L') { ++p; }

        char conv = *p;
        if (!conv) { put(start, 1); break; }
        ++p;

        char tmp[1024];
        tmp[0] = '\0';
        std::string hostSpec = spec;
        if (lng == 1) hostSpec += "ll";
        else if (lng == 3) hostSpec += "h";
        else if (lng == 2) hostSpec += "hh";

        switch (conv) {
            case 'd':
            case 'i': {
                long long v = static_cast<long long>(VaGp(va));
                std::snprintf(tmp, sizeof(tmp), hostSpec.empty() ? "%lld" : (hostSpec + "d").c_str(), v);
                break;
            }
            case 'u':
            case 'o':
            case 'x':
            case 'X': {
                unsigned long long v = VaGp(va);
                std::snprintf(tmp, sizeof(tmp), (hostSpec + conv).c_str(), v);
                break;
            }
            case 'c': {
                int v = static_cast<int>(VaGp(va));
                std::snprintf(tmp, sizeof(tmp), (hostSpec + "c").c_str(), v);
                break;
            }
            case 's': {
                const char* s = reinterpret_cast<const char*>(VaGp(va));
                if (!s) s = "(null)";
                if (precision >= 0 && static_cast<int>(std::strlen(s)) > precision) {
                    std::string cut(s, static_cast<size_t>(precision));
                    std::snprintf(tmp, sizeof(tmp), (hostSpec + "s").c_str(), cut.c_str());
                } else {
                    std::snprintf(tmp, sizeof(tmp), (hostSpec + "s").c_str(), s);
                }
                break;
            }
            case 'f':
            case 'F':
            case 'e':
            case 'E':
            case 'g':
            case 'G':
            case 'a':
            case 'A': {
                double v = VaFp(va);
                std::snprintf(tmp, sizeof(tmp), (hostSpec + conv).c_str(), v);
                break;
            }
            case 'p': {
                void* v = reinterpret_cast<void*>(VaGp(va));
                std::snprintf(tmp, sizeof(tmp), "%p", v);
                break;
            }
            case 'n':
                VaGp(va);
                break;
            default:
                put(start, static_cast<size_t>(p - start));
                continue;
        }
        put(tmp, std::strlen(tmp));
    }
    out[n < cap ? n : cap - 1] = '\0';
    return n;
}

// Builds a va_list that describes the arguments of a variadic import call.
VaList MakeVa(const ImportRegs& r, const uint64_t* stack, uint32_t gpOffset) {
    VaList v;
    v.gp = gpOffset;
    v.fp = 48;
    v.overflow = const_cast<uint64_t*>(stack);
    v.reg_save = const_cast<uint64_t*>(&r.gpr[0]);
    return v;
}

float XmmF32(const ImportRegs& r, int i) {
    float v;
    std::memcpy(&v, r.xmm[i], 4);
    return v;
}

double XmmF64(const ImportRegs& r, int i) {
    double v;
    std::memcpy(&v, r.xmm[i], 8);
    return v;
}

uint64_t RetF32(float v) {
    return static_cast<uint64_t>(std::bit_cast<uint32_t>(v));
}

uint64_t RetF64(double v) {
    return std::bit_cast<uint64_t>(v);
}

int NullSafeStrcmp(const char* a, const char* b) {
    if (a == b) return 0;
    if (!a) return -1;
    if (!b) return 1;
    return std::strcmp(a, b);
}

int NullSafeStricmp(const char* a, const char* b) {
    if (a == b) return 0;
    if (!a) return -1;
    if (!b) return 1;
    return _stricmp(a, b);
}

// High-resolution monotonic clock in microseconds and simulated 1.6 GHz TSC.
uint64_t NowUsec() {
    static LARGE_INTEGER freq{};
    static LARGE_INTEGER start{};
    if (!freq.QuadPart) {
        QueryPerformanceFrequency(&freq);
        QueryPerformanceCounter(&start);
    }
    LARGE_INTEGER now{};
    QueryPerformanceCounter(&now);
    return static_cast<uint64_t>((now.QuadPart - start.QuadPart) * 1000000ULL / freq.QuadPart);
}

// Runtime Win64 -> SysV 2-argument shim for guest qsort comparators.
using SysvComparFn = int (*)(uint64_t fn, const void* a, const void* b);

SysvComparFn GetSysvCompar() {
    static SysvComparFn s_fn = nullptr;
    if (s_fn) return s_fn;
    auto* code = static_cast<uint8_t*>(
        VirtualAlloc(nullptr, 256, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    uint8_t* c = code;
    auto emit = [&](std::initializer_list<uint8_t> bytes) {
        for (uint8_t b : bytes) *c++ = b;
    };
    emit({0x57});                                            // push rdi
    emit({0x56});                                            // push rsi
    emit({0x48, 0x81, 0xEC, 0xA8, 0x00, 0x00, 0x00});        // sub rsp, 0xA8
    for (uint8_t reg = 6; reg < 16; ++reg) {
        const uint8_t rex = reg >= 8 ? 0x44 : 0x00;
        const uint8_t modrm = static_cast<uint8_t>(0x84 | ((reg & 7) << 3));
        const uint32_t disp = (reg - 6) * 16;
        if (rex) emit({rex});
        emit({0x0F, 0x11, modrm, 0x24});
        std::memcpy(c, &disp, 4);
