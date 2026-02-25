// ELF image loader for the PS4 (ET_SCE_RELEXEC) build of Nidhogg.
//
// The dumped eboot carries plain x86-64 code, so the port does not need to
// translate instructions: it maps the image at a Windows base address, applies
// the relative relocations, points every import slot at a shim, and calls the
// original entry point.  Everything above that line is SCE API replacement.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace ps4 {

struct Segment {
    uint64_t vaddr = 0;
    uint64_t memsz = 0;
    uint32_t flags = 0;   // PF_X=1 PF_W=2 PF_R=4
    const uint8_t* file = nullptr;
    uint64_t filesz = 0;
};

struct Reloc {
    uint64_t offset;
    uint32_t type;
    uint64_t addend;
};

class Image {
public:
    // Parses the ELF, reserves a Windows region, copies the segments in.
    bool load(const std::string& elfPath);
    // Applies R_X86_64_RELATIVE fixups recovered from the relocation table.
    size_t relocate();
    // Points the import slots at generated stubs.
    size_t installImports();

    uint8_t* base() const { return base_; }
    uint64_t entry() const { return entry_; }
    uint64_t vaddr(uint8_t* p) const { return static_cast<uint64_t>(p - base_); }
    uint8_t* at(uint64_t vaddr) const { return base_ + vaddr; }
    uint64_t size() const { return span_; }

    const std::vector<Segment>& segments() const { return segs_; }
    const std::vector<Reloc>& relocs() const { return relocs_; }
    const std::vector<uint64_t>& imports() const { return imports_; }

private:
    bool parse(const std::vector<uint8_t>& file);
    void findRelocs(const std::vector<uint8_t>& file);
    void findImports(const std::vector<uint8_t>& file);

    std::vector<uint8_t> blob_;
    std::vector<Segment> segs_;
    std::vector<Reloc> relocs_;
    std::vector<uint64_t> imports_;
    uint8_t* base_ = nullptr;
    uint64_t span_ = 0;
    uint64_t entry_ = 0;
};

}  // namespace ps4
