#include "pm4.h"

#include <windows.h>

#include <cstddef>
#include <cstdio>

namespace ps4::gnm {
namespace {

constexpr uint32_t kMaxIndirectDepth = 4;
constexpr int kWaitSpins = 4'000'000;

const char* OpName(uint32_t op) {
    switch (op) {
        case 0x10: return "NOP";
        case 0x15: return "DISPATCH_DIRECT";
        case 0x27: return "DRAW_INDEX_2";
        case 0x2a: return "INDEX_TYPE";
        case 0x2d: return "DRAW_INDEX_AUTO";
        case 0x37: return "WRITE_DATA";
        case 0x3c: return "WAIT_REG_MEM";
        case 0x3f: return "INDIRECT_BUFFER";
        case 0x46: return "EVENT_WRITE";
        case 0x47: return "EVENT_WRITE_EOP";
        case 0x48: return "EVENT_WRITE_EOS";
        case 0x58: return "ACQUIRE_MEM";
        case 0x68: return "SET_CONFIG_REG";
        case 0x69: return "SET_CONTEXT_REG";
        case 0x76: return "SET_SH_REG";
        case 0x79: return "SET_UCONFIG_REG";
        default: return nullptr;
    }
}

// Type-3 header: bits 31:30 = 3, 29:16 = count - 1, 15:8 = opcode.  The low
// byte is a GNM routing tag (0 = graphics ring, 2 = compute ring); the packets
// this title emits are otherwise plain PM4.
inline uint32_t Op(uint32_t h) { return (h >> 8) & 0xff; }
inline uint32_t Count(uint32_t h) { return ((h >> 16) & 0x3fff) + 1; }

// A GPU address arrives as (hi << 32) | lo, with control bits parked in the
// upper half of the hi dword.  Only the low 16 bits of hi carry address.
inline uint64_t GpuAddr(uint32_t lo, uint32_t hi) {
    return (static_cast<uint64_t>(hi & 0xffff) << 32) | lo;
}

}  // namespace

void Machine::Reset() {
    regs_ = RegFile{};
    draws_.clear();
    writes_ = 0;
    errors_ = 0;
    indexType_ = 0;
}

void Machine::Submit(const uint64_t* buffers, const uint32_t* sizes,
                     uint32_t count) {
    // A Draw carries a full register snapshot, so they belong to one submit and
    // one frame.  Keeping them across submits leaks about half a megabyte per
    // frame and would make the backend redraw the whole run every time.
    draws_.clear();
    for (uint32_t i = 0; i < count; ++i) {
        if (!buffers[i] || !sizes[i]) continue;
        RunBuffer(buffers[i], sizes[i], 0);
    }
}

void Machine::RunBuffer(uint64_t addr, uint32_t bytes, uint32_t depth) {
    if (depth > kMaxIndirectDepth) {
        ++errors_;
        std::printf("[pm4] indirect buffers nested too deep at %p\n",
                    reinterpret_cast<void*>(addr));
        return;
    }
    if (!HostReadable(addr, bytes)) {
        ++errors_;
        std::printf("[pm4] buffer %p size=0x%x is not mapped\n",
                    reinterpret_cast<void*>(addr), bytes);
        return;
    }

    const auto* dw = reinterpret_cast<const uint32_t*>(addr);
    const uint32_t words = bytes / 4;
    for (uint32_t i = 0; i < words;) {
        const uint32_t header = dw[i];
        const uint32_t type = header >> 30;

        if (type == 3) {
            const uint32_t n = Count(header);
            if (i + 1 + n > words) {
                ++errors_;
                std::printf("[pm4] packet at +0x%x runs past the buffer\n", i * 4);
                return;
            }
            Packet(Op(header), dw + i + 1, n, i);
            i += 1 + n;
        } else if (type == 0) {
            // Legacy type-0 packet: bits 15:0 hold the payload dword count.
            i += 1 + (header & 0xffff);
        } else if (type == 1) {
            i += 2;
        } else {
            i += 1;
        }
    }
}

void Machine::Packet(uint32_t op, const uint32_t* p, uint32_t n, uint64_t pos) {
    switch (op) {
        case 0x10:  // NOP: padding, payload is whatever the buffer already held
            break;

        case 0x68: SetRegs(Space::Config, p, n); break;
        case 0x69: SetRegs(Space::Context, p, n); break;
        case 0x76: SetRegs(Space::Sh, p, n); break;
        case 0x79: SetRegs(Space::UConfig, p, n); break;

        case 0x2a:  // INDEX_TYPE
            if (n >= 1) indexType_ = p[0] & 1;
            break;

        case 0x2d: {  // DRAW_INDEX_AUTO(count, initiator)
            if (n < 1) break;
            Draw d;
            d.kind = Draw::Kind::Auto;
            d.indexCount = p[0];
            d.streamPos = pos;
            d.indexType = indexType_;
            d.regs = regs_;
            draws_.push_back(std::move(d));
            break;
        }

        case 0x27: {  // DRAW_INDEX_2(max, base_lo, base_hi, count, initiator)
            if (n < 5) break;
            Draw d;
            d.kind = Draw::Kind::Indexed;
            d.maxIndex = p[0];
            d.indexBase = GpuAddr(p[1], p[2]);
            d.indexCount = p[3];
            d.streamPos = pos;
            d.indexType = indexType_;
            d.regs = regs_;
            draws_.push_back(std::move(d));
            break;
        }

        case 0x15: {  // DISPATCH_DIRECT(gx, gy, gz, initiator)
            if (n < 3) break;
            Draw d;
            d.kind = Draw::Kind::Dispatch;
            d.dispatch[0] = p[0];
            d.dispatch[1] = p[1];
            d.dispatch[2] = p[2];
            d.streamPos = pos;
            d.regs = regs_;
            draws_.push_back(std::move(d));
            break;
        }

        case 0x37: {  // WRITE_DATA(control, addr_lo, addr_hi, data...)
            if (n < 4) break;
            WriteMemory(GpuAddr(p[1], p[2]), p + 3, n - 3);
            break;
        }

        case 0x47: {  // EVENT_WRITE_EOP(control, addr_lo, addr_hi, data_lo, data_hi)
            if (n < 5) break;
            const uint64_t addr = GpuAddr(p[1], p[2]);
            if (addr) {
                const uint32_t data[2] = {p[3], p[4]};
                WriteMemory(addr, data, 2);
            }
            break;
        }

        case 0x48: {  // EVENT_WRITE_EOS(control, addr_lo, addr_hi, data)
            if (n < 4) break;
            const uint64_t addr = GpuAddr(p[1], p[2]);
            if (addr) WriteMemory(addr, p + 3, 1);
            break;
        }

        case 0x46:  // EVENT_WRITE: report only, no address payload
            break;

        case 0x58:  // ACQUIRE_MEM: cache maintenance; all memory is coherent here
            break;

        case 0x3c: {  // WAIT_REG_MEM(control, addr_lo, addr_hi, ref, mask, poll)
            if (n < 5) break;
            const uint32_t func = p[0] & 0xf;
            const uint32_t space = (p[0] >> 4) & 3;
            const uint64_t addr = GpuAddr(p[1], p[2]);
            if (space != 1) {
                // Register-space waits are satisfied by the shadow register
                // file, which the packet stream has already applied.
                break;
            }
            if (!WaitMemory(addr, p[3], p[4], func)) {
                ++errors_;
                std::printf(
                    "[pm4] WAIT_REG_MEM at +0x%llx timed out: [%p] = 0x%08x, "
                    "want 0x%08x mask 0x%08x func %u\n",
                    static_cast<unsigned long long>(pos * 4),
                    reinterpret_cast<void*>(addr),
                    HostReadable(addr, 4) ? *reinterpret_cast<uint32_t*>(addr)
                                          : 0xffffffffu,
                    p[3], p[4], func);
            }
            break;
        }

        case 0x3f: {  // INDIRECT_BUFFER(base_lo, base_hi, size, flags)
            if (n < 3) break;
            const uint64_t ib = GpuAddr(p[0], p[1]);
            const uint32_t size = n >= 3 ? p[2] : 0;
            if (ib && size) RunBuffer(ib, size, 1);
            break;
        }

        default:
            ++errors_;
            {
                static bool seen[256] = {};
                if (!seen[op & 0xff]) {
                    seen[op & 0xff] = true;
                    const char* nm = OpName(op);
                    std::printf("[pm4] unhandled op 0x%02x%s%s at +0x%llx, n=%u:",
                                op, nm ? " " : "", nm ? nm : "",
                                static_cast<unsigned long long>(pos * 4), n);
                    for (uint32_t k = 0; k < n && k < 8; ++k)
                        std::printf(" %08x", p[k]);
                    std::printf("\n");
                    std::fflush(stdout);
                }
            }
            break;
    }
}

void Machine::SetRegs(Space space, const uint32_t* p, uint32_t n) {
    if (!n) return;
    const uint32_t base = p[0];
    uint32_t* block = regs_[space];
    for (uint32_t i = 1; i < n; ++i) {
        const uint32_t reg = base + i - 1;
        if (reg >= kSpaceSize) {
            ++errors_;
            std::printf("[pm4] register 0x%x outside space %u\n", reg,
                        static_cast<uint32_t>(space));
            continue;
        }
        block[reg] = p[i];
    }
}

void Machine::WriteMemory(uint64_t addr, const uint32_t* data, uint32_t words) {
    if (!HostReadable(addr, static_cast<size_t>(words) * 4)) {
        ++errors_;
        std::printf("[pm4] write of %u dwords to unmapped %p\n", words,
                    reinterpret_cast<void*>(addr));
        return;
    }
    auto* dst = reinterpret_cast<uint32_t*>(addr);
    for (uint32_t i = 0; i < words; ++i) dst[i] = data[i];
    ++writes_;
}

bool Machine::WaitMemory(uint64_t addr, uint32_t ref, uint32_t mask,
                         uint32_t func) {
    if (!HostReadable(addr, 4)) return false;
    auto* p = reinterpret_cast<volatile uint32_t*>(addr);
    for (int i = 0; i < kWaitSpins; ++i) {
        const uint32_t v = *p & mask;
        const uint32_t r = ref & mask;
        bool ok;
        switch (func) {
            case 0: ok = true; break;
            case 1: ok = v < r; break;
            case 2: ok = v <= r; break;
            case 3: ok = v == r; break;
            case 4: ok = v != r; break;
            case 5: ok = v >= r; break;
            case 6: ok = v > r; break;
            default: ok = true; break;
        }
        if (ok) return true;
        Sleep(0);
    }
    return false;
}

}  // namespace ps4::gnm
