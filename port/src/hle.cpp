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
