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
        c += 4;
    }
    emit({0x48, 0x89, 0xD7});                                // mov rdi, rdx
    emit({0x4C, 0x89, 0xC6});                                // mov rsi, r8
    emit({0xFF, 0xD1});                                      // call rcx
    for (uint8_t reg = 6; reg < 16; ++reg) {
        const uint8_t rex = reg >= 8 ? 0x44 : 0x00;
        const uint8_t modrm = static_cast<uint8_t>(0x84 | ((reg & 7) << 3));
        const uint32_t disp = (reg - 6) * 16;
        if (rex) emit({rex});
        emit({0x0F, 0x10, modrm, 0x24});
        std::memcpy(c, &disp, 4);
        c += 4;
    }
    emit({0x48, 0x81, 0xC4, 0xA8, 0x00, 0x00, 0x00});        // add rsp, 0xA8
    emit({0x5E});                                            // pop rsi
    emit({0x5F});                                            // pop rdi
    emit({0xC3});                                            // ret
    FlushInstructionCache(GetCurrentProcess(), code, 256);
    s_fn = reinterpret_cast<SysvComparFn>(code);
    return s_fn;
}

void GuestQsort(uint8_t* base, size_t nmemb, size_t size, uint64_t compar) {
    if (!base || nmemb < 2 || !size || !compar) return;
    SysvComparFn cmp = GetSysvCompar();
    std::vector<uint8_t> tmp(size);
    auto swapElem = [&](size_t i, size_t j) {
        if (i == j) return;
        std::memcpy(tmp.data(), base + i * size, size);
        std::memcpy(base + i * size, base + j * size, size);
        std::memcpy(base + j * size, tmp.data(), size);
    };
    auto sortRange = [&](auto& self, ptrdiff_t lo, ptrdiff_t hi) -> void {
        while (lo < hi) {
            if (hi - lo < 16) {
                for (ptrdiff_t i = lo + 1; i <= hi; ++i) {
                    std::memcpy(tmp.data(), base + i * size, size);
                    ptrdiff_t j = i - 1;
                    while (j >= lo && cmp(compar, base + j * size, tmp.data()) > 0) {
                        std::memcpy(base + (j + 1) * size, base + j * size, size);
                        --j;
                    }
                    std::memcpy(base + (j + 1) * size, tmp.data(), size);
                }
                return;
            }
            ptrdiff_t mid = lo + (hi - lo) / 2;
            swapElem(static_cast<size_t>(mid), static_cast<size_t>(hi));
            std::memcpy(tmp.data(), base + hi * size, size);
            ptrdiff_t i = lo;
            for (ptrdiff_t j = lo; j < hi; ++j) {
                if (cmp(compar, base + j * size, tmp.data()) < 0) {
                    swapElem(static_cast<size_t>(i), static_cast<size_t>(j));
                    ++i;
                }
            }
            swapElem(static_cast<size_t>(i), static_cast<size_t>(hi));
            if (i - lo < hi - i) {
                self(self, lo, i - 1);
                lo = i + 1;
            } else {
                self(self, i + 1, hi);
                hi = i - 1;
            }
        }
    };
    sortRange(sortRange, 0, static_cast<ptrdiff_t>(nmemb) - 1);
}

// WIC-backed libScePngDec implementation.
IWICImagingFactory* GetWicFactory() {
    static IWICImagingFactory* s_factory = nullptr;
    if (s_factory) return s_factory;
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                     IID_PPV_ARGS(&s_factory));
    return s_factory;
}

int32_t PngParseHeader(const void* pngMem, uint32_t pngSize, void* infoOut) {
    IWICImagingFactory* f = GetWicFactory();
    if (!f || !pngMem || !pngSize) return -1;
    using Microsoft::WRL::ComPtr;
    ComPtr<IWICStream> stream;
    if (FAILED(f->CreateStream(&stream))) return -1;
    if (FAILED(stream->InitializeFromMemory(
            const_cast<BYTE*>(static_cast<const BYTE*>(pngMem)), pngSize)))
        return -1;
    ComPtr<IWICBitmapDecoder> dec;
    if (FAILED(f->CreateDecoderFromStream(stream.Get(), nullptr,
                                          WICDecodeMetadataCacheOnDemand, &dec)))
        return -1;
    ComPtr<IWICBitmapFrameDecode> frame;
    if (FAILED(dec->GetFrame(0, &frame))) return -1;
    UINT w = 0, h = 0;
    if (FAILED(frame->GetSize(&w, &h))) return -1;
    if (infoOut) {
        auto* u32 = static_cast<uint32_t*>(infoOut);
        auto* u16 = static_cast<uint16_t*>(infoOut);
        u32[0] = w;
        u32[1] = h;
        u16[4] = 3;  // RGBA
        u16[5] = 8;  // 8 bpc
        u32[3] = 0;
    }
    return 0;
}

int32_t PngDecodeRgba(const void* pngMem, uint32_t pngSize, void* dstMem,
                      uint32_t dstSize, uint32_t pitchInPixels) {
    IWICImagingFactory* f = GetWicFactory();
    if (!f || !pngMem || !pngSize || !dstMem) return -1;
    using Microsoft::WRL::ComPtr;
    ComPtr<IWICStream> stream;
    if (FAILED(f->CreateStream(&stream))) return -1;
    if (FAILED(stream->InitializeFromMemory(
            const_cast<BYTE*>(static_cast<const BYTE*>(pngMem)), pngSize)))
        return -1;
    ComPtr<IWICBitmapDecoder> dec;
    if (FAILED(f->CreateDecoderFromStream(stream.Get(), nullptr,
                                          WICDecodeMetadataCacheOnDemand, &dec)))
        return -1;
    ComPtr<IWICBitmapFrameDecode> frame;
    if (FAILED(dec->GetFrame(0, &frame))) return -1;
    UINT w = 0, h = 0;
    if (FAILED(frame->GetSize(&w, &h))) return -1;
    ComPtr<IWICFormatConverter> conv;
    if (FAILED(f->CreateFormatConverter(&conv))) return -1;
    if (FAILED(conv->Initialize(frame.Get(), GUID_WICPixelFormat32bppRGBA,
                                WICBitmapDitherTypeNone, nullptr, 0.0,
                                WICBitmapPaletteTypeCustom)))
        return -1;
    const UINT stride = (pitchInPixels ? pitchInPixels : w) * 4u;
    const UINT need = stride * h;
    if (dstSize < need) return -1;
    if (FAILED(conv->CopyPixels(nullptr, stride, dstSize, static_cast<BYTE*>(dstMem))))
        return -1;
    return 0;
}

// PM4 packet emission helpers for libSceGnmDriver.
uint32_t* Pm4SetReg(uint32_t* cmd, uint32_t op, uint32_t reg,
                    std::initializer_list<uint32_t> vals) {
    const uint32_t count = static_cast<uint32_t>(vals.size());
    *cmd++ = 0xC0000000u | (count << 16) | (op << 8);
    *cmd++ = reg;
    for (uint32_t v : vals) *cmd++ = v;
    return cmd;
}

uint32_t* Pm4Nop(uint32_t* cmd, uint32_t totalDwords) {
    if (!totalDwords) return cmd;
    if (totalDwords == 1) {
        *cmd++ = 0x80000000u;  // Type-2 1-dword filler
        return cmd;
    }
    *cmd++ = 0xC0001000u | ((totalDwords - 2) << 16);
    for (uint32_t i = 1; i < totalDwords; ++i) *cmd++ = 0;
    return cmd;
}

void Emit(const char* text, size_t len) {
    if (len) {
        std::fwrite(text, 1, len, stdout);
        std::fflush(stdout);
    }
}

// ----------------------------------------------------------- direct memory --

// PS4 direct memory is a physical-address space both the CPU and the GPU can
// see.  The port backs it with one reserved range of host address space and
// hands out offsets into it as physical addresses, so `map` is just the commit
// half of the reservation and every GPU pointer the game stores stays a real
// pointer.  The arena is sparse: 2 GiB reserved costs nothing until it is
// committed.
constexpr uint64_t kDirectArenaSize = 0x80000000ull;  // 2 GiB
// The game stores GPU addresses as 32-bit `cpuAddress >> 8` (that is why the
// texture path shifts left by 8 before a memcpy), so every byte the GPU can be
// pointed at has to live below 2^40.  Windows likes to hand out heap and
// VirtualAlloc ranges above that, which silently truncates the address and
// produces a write into an unmapped duplicate of the low bits.
constexpr uint64_t kDirectArenaBase = 0x0000004000000000ull;  // 256 GiB
uint8_t* g_directArena = nullptr;
uint64_t g_directUsed = 0;

bool DirectReserve() {
    if (g_directArena) return true;
    void* p = nullptr;
    for (uint64_t a = kDirectArenaBase; !p && a < 0x0000008000000000ull; a += kDirectArenaSize)
        p = VirtualAlloc(reinterpret_cast<void*>(a), kDirectArenaSize, MEM_RESERVE, PAGE_NOACCESS);
    if (!p) {
        std::printf("[direct] reserve of 0x%llx failed: %lu\n",
                    static_cast<unsigned long long>(kDirectArenaSize), GetLastError());
        std::fflush(stdout);
        return false;
    }
    g_directArena = static_cast<uint8_t*>(p);
    std::printf("[direct] arena %p size 0x%llx\n", g_directArena,
                static_cast<unsigned long long>(kDirectArenaSize));
    std::fflush(stdout);
    return true;
}

uint64_t DirectAllocate(uint64_t len, uint64_t align) {
    if (!DirectReserve()) return 0;
    if (align < 0x1000) align = 0x1000;
    const uint64_t at = (g_directUsed + align - 1) & ~(align - 1);
    if (at + len > kDirectArenaSize) return 0;
    g_directUsed = at + len;
    return at;
}

void* DirectMap(uint64_t phys, uint64_t len) {
    if (!g_directArena) return nullptr;
    if (phys + len > kDirectArenaSize) return nullptr;
    void* p = VirtualAlloc(g_directArena + phys, len, MEM_COMMIT, PAGE_READWRITE);
    return p;
}

// ------------------------------------------------------------------ files --

// The image addresses its data as /app0/..., the mount point SCE gives a
// game's own directory.  On this side that is the CUSA00920 tree; the env var
// overrides the default so the host can be run from anywhere.
const std::string& App0() {
    static std::string root;
    if (!root.empty()) return root;

    char* v = nullptr;
    size_t n = 0;
    if (_dupenv_s(&v, &n, "NIDHOGG_APP0") == 0 && v && *v) {
        root = v;
        std::free(v);
    } else {
        std::free(v);
        for (const char* cand : {"..\\CUSA00920", "CUSA00920", "..\\..\\CUSA00920"}) {
            DWORD a = GetFileAttributesA(cand);
            if (a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY)) {
                root = cand;
                break;
            }
        }
        if (root.empty()) root = ".";
    }
    char full[MAX_PATH]{};
    if (GetFullPathNameA(root.c_str(), MAX_PATH, full, nullptr)) root = full;
    std::printf("[fios] app0 = %s\n", root.c_str());
    std::fflush(stdout);
    return root;
}

std::string g_saveMountDir;
std::FILE* g_posixFds[64] = {};

std::string SaveRoot(const char* dirName) {
    char full[MAX_PATH]{};
    GetFullPathNameA("savedata", MAX_PATH, full, nullptr);
    std::string out = full[0] ? full : "savedata";
    if (dirName && *dirName) {
        out += '\\';
        out += dirName;
    }
    return out;
}

std::string HostPath(const char* p) {
    std::string s = p ? p : "";
    if (s.rfind("/savedata0/", 0) == 0 || s == "/savedata0") {
        std::string rest = s.size() > 11 ? s.substr(11) : "";
        for (char& c : rest) {
            if (c == '/') c = '\\';
        }
        std::string out = g_saveMountDir.empty() ? SaveRoot("00") : g_saveMountDir;
        if (!rest.empty()) {
            out += '\\';
            out += rest;
        }
        return out;
    }
    std::string out = App0();
    if (s.rfind("/app0/", 0) == 0) s.erase(0, 6);
    else if (s.rfind("/app0", 0) == 0) s.erase(0, 5);
    for (char& c : s) {
        if (c == '/') c = '\\';
    }
    if (!s.empty() && s.front() != '\\') out += '\\';
    out += s;
    return out;
}

void MakeParentDirs(const std::string& path) {
    for (size_t i = 1; i < path.size(); ++i) {
        if (path[i] != '\\') continue;
        std::string dir = path.substr(0, i);
        DWORD a = GetFileAttributesA(dir.c_str());
        if (a == INVALID_FILE_ATTRIBUTES) CreateDirectoryA(dir.c_str(), nullptr);
    }
}

// ------------------------------------------------------------- gnm submit --

bool Readable(const void* p, size_t n);

// PS4 GPU addresses come in two encodings.  Resource descriptors (textures,
// buffers) store `cpuAddress >> 8` in a 32-bit field; command-buffer lists in
// the submit ABI carry full 64-bit pointers.  This recognises both by asking
// which form is mapped, so a misread shows up as "not readable" instead of a
// wild write.
uint64_t GpuAddrToHost(uint64_t v) {
    if (Readable(reinterpret_cast<void*>(v), 4)) return v;
    if (Readable(reinterpret_cast<void*>(v << 8), 4)) return v << 8;
    return 0;
}

uint64_t g_submits = 0;
bool g_dumpedSubmit = false;

void DumpPackets(uint64_t base, uint32_t bytes, const char* tag) {
    auto* dw = reinterpret_cast<const uint32_t*>(base);
    const uint32_t count = bytes / 4;
    std::printf("[gnm] %s buffer %p size=0x%x dwords=%u\n", tag,
                reinterpret_cast<void*>(base), bytes, count);
    std::printf("[gnm]   first dwords:");
    for (uint32_t i = 0; i < count && i < 24; ++i) std::printf(" %08x", dw[i]);
    std::printf("\n");
    // The full buffer goes to disk; the packet walk below is a quick look and
    // an offline parse is how the stream gets understood.
    char name[64];
    std::snprintf(name, sizeof(name), "work/%s_%llx.bin", tag,
                  static_cast<unsigned long long>(base));
    if (std::FILE* f = std::fopen(name, "wb")) {
        std::fwrite(dw, 4, count, f);
        std::fclose(f);
    }
    for (uint32_t i = 0; i < count;) {
        const uint32_t h = dw[i];
        const uint32_t type = h >> 30;
        if (type == 3) {
            const uint32_t op = (h >> 8) & 0xff;
            const uint32_t n = ((h >> 16) & 0x3fff) + 1;
            if (op == 0x37) {  // WRITE_DATA
                std::printf("[gnm]   %04u WRITE_DATA n=%u:", i, n);
                for (uint32_t k = 0; k < n && i + 1 + k < count; ++k)
                    std::printf(" %08x", dw[i + 1 + k]);
                std::printf("\n");
            }
            i += 1 + n;
        } else if (type == 2) {  // type-2 filler: 1 dword
            i += 1;
        } else {
            // type-0 packets carry a dword count in the low half; the padding
            // at the head of every buffer is a run of zero-count type-0 packets
            i += 1 + (type == 0 ? (h & 0xffff) : 1);
        }
    }
}

gnm::Machine g_machine;

// First frame only.  The register snapshot of each draw is what the D3D11 side
// will consume, so it goes to disk in full; the console gets the handful of
// registers whose values are inside the direct-memory arena, which is how the
// shader program addresses and resource descriptors show themselves.
void DumpFrameState() {
    const auto& draws = g_machine.draws();
    std::printf("[gnm] first frame: %zu draws, %llu memory writes, %u errors\n",
                draws.size(),
                static_cast<unsigned long long>(g_machine.memoryWrites()),
                g_machine.errors());
    std::fflush(stdout);
    if (draws.empty()) return;

    const char* spaces[4] = {"config", "context", "sh", "uconfig"};
    for (size_t i = 0; i < draws.size(); ++i) {
        char name[64];
        std::snprintf(name, sizeof(name), "work/draw%zu_regs.txt", i);
        std::FILE* f = std::fopen(name, "w");
        if (!f) continue;
        for (uint32_t s = 0; s < 4; ++s) {
            std::fprintf(f, "[%s]\n", spaces[s]);
            for (uint32_t r = 0; r < gnm::kSpaceSize; ++r) {
                const uint32_t v = draws[i].regs.block[s][r];
                if (v) std::fprintf(f, "  %03x = %08x\n", r, v);
            }
        }
        std::fclose(f);
    }

    for (size_t i = 0; i < draws.size(); ++i) {
        const auto& d = draws[i];
        std::printf("[gnm] draw%zu kind=%d pos=+0x%llx count=%u max=%u base=%08x prim=%08x\n",
                    i, static_cast<int>(d.kind),
                    static_cast<unsigned long long>(d.streamPos * 4), d.indexCount,
                    d.maxIndex, d.indexBase,
                    d.regs[gnm::Space::UConfig][0x242]);
        for (uint32_t r = 0; r < 0x400; ++r) {
            const uint32_t v = d.regs[gnm::Space::Sh][r];
            // A shader program address lands in PGM_LO as address >> 8, so the
            // high half of the 40-bit arena address is what to look for.
            if (v >= 0x40000000u && v < 0x40200000u)
                std::printf("[gnm]   sh[0x%03x] = %08x  -> 0x%010llx\n", r, v,
                            static_cast<unsigned long long>(
                                static_cast<uint64_t>(v) << 8));
        }
    }
    std::fflush(stdout);
}

// ---------------------------------------------------------------- fake IDs --

// Handles handed back by the HLE layer: small integers, never real pointers, so
// a stray dereference is obvious rather than quietly working.
constexpr int kFakeFdBase = 0x100;

// --------------------------------------------------------------- video out --

struct VideoOutBuffer {
    void* addr = nullptr;
    uint32_t pitch = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    int index = 0;
};

struct VideoOut {
    bool open = false;
    uint32_t width = 1920;
    uint32_t height = 1080;
    uint32_t pitch = 1920 * 4;
    uint32_t flipRate = 60;
    uint64_t frame = 0;
    uint64_t flipCount = 0;
    uint64_t lastFlipArg = 0;
    VideoOutBuffer buffers[8];
    int bufferCount = 0;
    HWND window = nullptr;
};

VideoOut g_videoOut;

const char* kClassName = "NidhoggPort";

// True when [p, p+n) is committed memory the host can read.  The image is SysV
// code, so registers the caller did not set up still hold whatever was there --
// an import that takes fewer arguments than the slot's C prototype must never
// trust them.
//
// Every HLE call validates its pointers, and so does the PM4 backend, which put
// a few thousand VirtualQuery syscalls in every frame -- the profiler had the
// main thread inside NtQueryVirtualMemory for 99% of its samples.  The guest
// only ever touches a handful of large regions (the image, the direct-memory
// arena, host allocations), so remembering the region each address landed in
// turns the common case back into two comparisons.
struct SpanCache {
    static constexpr int kSlots = 64;
    uintptr_t lo[kSlots] = {};
    uintptr_t hi[kSlots] = {};
};

thread_local SpanCache g_spans;

bool Readable(const void* p, size_t n) {
    if (!p) return false;
    auto a = reinterpret_cast<uintptr_t>(p);
    const uintptr_t end = a + n;

    const uint32_t slot = static_cast<uint32_t>((a >> 12) * 2654435761u) &
                          (SpanCache::kSlots - 1);
    if (a >= g_spans.lo[slot] && end <= g_spans.hi[slot]) return true;

    while (a < end) {
        MEMORY_BASIC_INFORMATION mbi{};
        if (!VirtualQuery(reinterpret_cast<void*>(a), &mbi, sizeof(mbi))) return false;
        if (mbi.State != MEM_COMMIT) return false;
        if (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) return false;
        const uintptr_t base = reinterpret_cast<uintptr_t>(mbi.BaseAddress);
        if (a == reinterpret_cast<uintptr_t>(p) && base + mbi.RegionSize >= end) {
            g_spans.lo[slot] = base;
            g_spans.hi[slot] = base + mbi.RegionSize;
        }
        a = base + mbi.RegionSize;
    }
    return true;
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_CLOSE:
            DestroyWindow(hwnd);
            return 0;
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
        default:
            return DefWindowProcA(hwnd, msg, wp, lp);
    }
}

HWND CreatePortWindow(uint32_t w, uint32_t h) {
    if (g_videoOut.window) return g_videoOut.window;
    HINSTANCE inst = GetModuleHandleA(nullptr);
    WNDCLASSA wc{};
    wc.style = CS_OWNDC;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorA(nullptr, IDC_ARROW);
    wc.lpszClassName = kClassName;
    if (!RegisterClassA(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return nullptr;
    RECT rc{0, 0, static_cast<LONG>(w), static_cast<LONG>(h)};
    AdjustWindowRect(&rc, WS_OVERLAPPEDWINDOW, FALSE);
    HWND hwnd = CreateWindowExA(0, kClassName, "Nidhogg", WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                                CW_USEDEFAULT, CW_USEDEFAULT, rc.right - rc.left,
                                rc.bottom - rc.top, nullptr, nullptr, inst, nullptr);
    g_videoOut.window = hwnd;
    return hwnd;
}

}  // namespace

uint32_t LastSlot() { return g_lastSlot; }
uint64_t LastCallSite() { return g_lastRet; }
void HleSetImageBase(uint64_t base) { g_base = base; }

// ------------------------------------------------------------------ names --

const char* SlotName(uint32_t s) {
    switch (s) {
        case 0: return "free";
        case 1: return "strchr";
        case 2: return "strcasecmp";
        case 3: return "malloc";
        case 4: return "strlen";
        case 5: return "strcpy";
        case 6: return "strncpy";
        case 7: return "sscanf";
        case 8: return "strtol";
        case 9: return "__stack_chk_fail";
        case 10: return "memcpy";
        case 11: return "memmove";
        case 12: return "strcat";
        case 13: return "operator new";
        case 14: return "operator delete";
        case 15: return "memset";
        case 16: return "operator new[]";
        case 17: return "sprintf";
        case 18: return "sceSysmoduleLoadModule";
        case 20: return "scePngDecParseHeader";
        case 21: return "scePngDecQueryMemorySize";
        case 22: return "scePngDecCreate";
        case 23: return "scePngDecDecode";
        case 24: return "printf";
        case 25: return "scePngDecDelete";
        case 26: return "snprintf";
        case 27: return "scePadOpen";
        case 28: return "sceUserServiceGetUserName";
        case 29: return "scePadSetVibration";
        case 30: return "scePadClose";
        case 31: return "scePadRead";
        case 32: return "scePadInit";
        case 33: return "sceUserServiceInitialize";
        case 34: return "sceUserServiceGetInitialUser";
        case 35: return "sceUserServiceGetLoginUserIdList";
        case 36: return "sceUserServiceGetEvent";
        case 38: return "scePadSetLightBar";
        case 39: return "scePadResetLightBar";
        case 40: return "std::ios_base::Init::Init";
        case 41: return "__cxa_atexit";
        case 42: return "atexit_init";
        case 44: return "std::__throw_length_error";
        case 47: return "fseek";
        case 48: return "ftell";
        case 49: return "fread";
        case 50: return "fclose";
        case 51: return "fopen";
        case 52: return "mkdir";
        case 53: return "fwrite";
        case 54: return "remove";
        case 55: return "vsprintf";
        case 59: return "htons";
        case 68: return "strcmp";
        case 69: return "np: system event count";
        case 82: return "sceSaveDataDialogOpen";
        case 84: return "sceNpCommerceDialogOpen";
        case 88: return "sceCommonDialogUpdateBar";
        case 89: return "sceKernelGettimeofday";
        case 90: return "sceNpGetNpId";
        case 102: return "memcmp";
        case 110: return "_FSin";
        case 111: return "sceVideoOut? (ctor head)";
        case 112: return "sceVideoOutOpen";
        case 113: return "sceVideoOutSetFlipRate";
        case 114: return "sceKernelCreateEqueue";
        case 115: return "puts";
        case 116: return "sceVideoOutSetBufferAttribute";
        case 117: return "sceVideoOutRegisterBuffers";
        case 118: return "sceKernelWaitEqueue";
        case 119: return "sceKernelUsleep";
        case 120: return "sceVideoOutGetFlipStatus";
        case 121: return "sceKernelDeleteEqueue";
        case 122: return "sceVideoOutClose";
        case 124: return "sceSaveDataInitialize3";
        case 125: return "sceSaveDataMount2";
        case 126: return "sceSaveDataUmount";
        case 127: return "sceSaveDataSaveIcon";
        case 128: return "sceKernelOpen";
        case 129: return "sceKernelWrite";
        case 130: return "sceKernelClose";
        case 131: return "sceSaveDataGetParam";
        case 132: return "localtime_r";
        case 133: return "strftime";
        case 134: return "sceSaveDataLoadIcon";
        case 135: return "sceKernelStat";
        case 136: return "sceKernelRead";
        case 138: return "strrchr";
        case 140: return "pthread_attr_init";
        case 141: return "pthread_attr_setdetachstate";
        case 142: return "pthread_create";
        case 143: return "pthread_attr_destroy";
        case 144: return "usleep";
        case 145: return "pthread_mutex_init";
        case 146: return "pthread_mutex_destroy";
        case 147: return "pthread_mutex_lock";
