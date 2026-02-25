#pragma once

#include <cstdint>
#include <vector>

namespace ps4::gnm {

// One PM4 register space.  Everything the command stream writes lands in one
// of these four blocks, and every draw records a copy of all of them: the
// backend needs to look at whatever the translated shader reads, and the
// interpreter has no business knowing which registers those are.
enum class Space : uint32_t { Config, Context, Sh, UConfig };
constexpr uint32_t kSpaceCount = 4;
constexpr uint32_t kSpaceSize = 0x1000;

struct RegFile {
    uint32_t block[kSpaceCount][kSpaceSize] = {};

    uint32_t* operator[](Space s) { return block[static_cast<uint32_t>(s)]; }
    const uint32_t* operator[](Space s) const {
        return block[static_cast<uint32_t>(s)];
    }
};

struct Draw {
    enum class Kind { Auto, Indexed, Dispatch };

    Kind kind = Kind::Auto;
    uint64_t streamPos = 0;    // payload offset inside the submitted buffer
    uint32_t indexCount = 0;
    uint32_t maxIndex = 0;
    uint64_t indexBase = 0;    // GPU address of the index buffer (Indexed only)
    uint32_t dispatch[3] = {}; // thread counts (Dispatch only)
    uint32_t indexType = 0;    // 0 = 16 bit, 1 = 32 bit
    RegFile regs;
};

class Machine {
public:
    // Runs the submitted dynamic command buffers in order.  This title does not
    // use constant command buffers; if a future buffer does, it lands here too.
    void Submit(const uint64_t* buffers, const uint32_t* sizes, uint32_t count);

    const std::vector<Draw>& draws() const { return draws_; }
    uint64_t memoryWrites() const { return writes_; }
    uint32_t errors() const { return errors_; }
    uint32_t indexType() const { return indexType_; }

    void Reset();

private:
    void RunBuffer(uint64_t addr, uint32_t bytes, uint32_t depth);
    void Packet(uint32_t op, const uint32_t* p, uint32_t n, uint64_t pos);
    void SetRegs(Space space, const uint32_t* p, uint32_t n);
    void WriteMemory(uint64_t addr, const uint32_t* data, uint32_t words);
    bool WaitMemory(uint64_t addr, uint32_t ref, uint32_t mask, uint32_t func);

    RegFile regs_;
    std::vector<Draw> draws_;
    uint64_t writes_ = 0;
    uint32_t errors_ = 0;
    uint32_t indexType_ = 0;
};

// Provided by the platform layer: true when [addr, addr + n) is committed
// memory the host can touch.  GPU addresses in this port are host addresses --
// direct memory is reserved at a fixed base precisely so that stays true.
bool HostReadable(uint64_t addr, size_t n);

}  // namespace ps4::gnm
