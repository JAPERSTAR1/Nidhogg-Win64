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
