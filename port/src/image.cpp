#include "image.h"

#include <windows.h>

#include <cstdio>
#include <cstring>

namespace ps4 {
namespace {

constexpr uint32_t PT_LOAD = 1;
constexpr uint32_t PF_X = 1;
constexpr uint32_t PF_W = 2;

uint16_t rd16(const uint8_t* p) { return *reinterpret_cast<const uint16_t*>(p); }
uint32_t rd32(const uint8_t* p) { return *reinterpret_cast<const uint32_t*>(p); }
uint64_t rd64(const uint8_t* p) { return *reinterpret_cast<const uint64_t*>(p); }

}  // namespace

bool Image::parse(const std::vector<uint8_t>& f) {
    if (f.size() < 64 || std::memcmp(f.data(), "\x7f" "ELF", 4) != 0) return false;
    const uint8_t* e = f.data();
    if (e[4] != 2 || e[5] != 1) return false;  // ELFCLASS64, LE

    entry_ = rd64(e + 0x18);
    const uint64_t phoff = rd64(e + 0x20);
    const uint16_t phentsize = rd16(e + 0x36);
    const uint16_t phnum = rd16(e + 0x38);

    for (uint16_t i = 0; i < phnum; ++i) {
        const uint8_t* p = f.data() + phoff + i * phentsize;
        const uint32_t type = rd32(p);
        const uint64_t off = rd64(p + 8);
        const uint64_t vaddr = rd64(p + 16);
        const uint64_t filesz = rd64(p + 32);
        const uint64_t memsz = rd64(p + 40);
        if (type != PT_LOAD) continue;

        if (isSelf) {
            bool matched = false;
            for (const auto& ss : selfSegs) {
                if (ss.filesz == filesz) {
                    off = ss.offset;
                    matched = true;
                    break;
                }
            }
            if (!matched) return false;
        }

        Segment s;
        s.vaddr = vaddr;
        s.memsz = memsz;
        s.flags = rd32(p + 4);
        s.filesz = (off + filesz <= f.size()) ? filesz : (f.size() > off ? f.size() - off : 0);
        s.file = f.data() + off;
        segs_.push_back(s);
        span_ = (vaddr + memsz > span_) ? vaddr + memsz : span_;
    }
    return !segs_.empty();
}

void Image::findRelocs(const std::vector<uint8_t>& f) {
    const uint64_t limit = span_;
    auto plausible = [&](size_t i) {
        if (i + 24 > f.size()) return false;
        const uint64_t off = rd64(&f[i]);
        const uint64_t info = rd64(&f[i + 8]);
        const uint64_t addend = rd64(&f[i + 16]);
        if ((info & 0xffffffffull) != 8) return false;
        if (off < 0x1000 || off + 8 > limit || (off & 7)) return false;
        if (addend >= limit) return false;
        return true;
    };

    size_t i = 0;
    while (i + 24 <= f.size()) {
        if (!plausible(i)) { i += 8; continue; }
        size_t run = 0;
        while (plausible(i + run * 24)) ++run;
        if (run >= 8) {
            for (size_t k = 0; k < run; ++k) {
                const size_t o = i + k * 24;
                relocs_.push_back({rd64(&f[o]), 8, rd64(&f[o + 16])});
            }
            i += run * 24;
        } else {
            i += 8;
        }
    }
}

void Image::findImports(const std::vector<uint8_t>&) {
    const uint64_t plt = 0x171bc0;
    const Segment* text = nullptr;
    for (const auto& s : segs_)
        if ((s.flags & PF_X) && plt >= s.vaddr && plt + 0x10 <= s.vaddr + s.filesz) text = &s;
    if (!text) return;

    auto slotAt = [&](uint64_t va, uint64_t* slot) {
        const uint8_t* p = text->file + (va - text->vaddr);
        if (p[0] != 0xff || p[1] != 0x25) return false;
        const int32_t disp = *reinterpret_cast<const int32_t*>(p + 2);
        *slot = va + 6 + static_cast<int64_t>(disp);
        return true;
    };

    uint64_t slot = 0;
    if (!slotAt(plt + 6, &slot)) return;
    for (uint64_t va = plt + 0x10; va + 6 <= text->vaddr + text->filesz; va += 0x10) {
        uint64_t next = 0;
        if (!slotAt(va, &next) || next != slot + 8) break;
        imports_.push_back(next);
        slot = next;
    }
}

bool Image::load(const std::string& elfPath) {
    FILE* fp = core::FopenUtf8(elfPath, "rb");
    if (!fp) return false;
    if (_fseeki64(fp, 0, SEEK_END) != 0) {
        std::fclose(fp);
        return false;
    }
    const int64_t n = _ftelli64(fp);
    _fseeki64(fp, 0, SEEK_SET);
    if (n <= 0 || n > (256ll << 20)) {
        std::fclose(fp);
        return false;
    }
    blob_.resize(static_cast<size_t>(n));
    if (std::fread(blob_.data(), 1, blob_.size(), fp) != blob_.size()) {
        std::fclose(fp);
        return false;
    }
    std::fclose(fp);

    segs_.clear();
    relocs_.clear();
    imports_.clear();
    span_ = 0;
    if (!parse(blob_)) return false;

    const uint64_t align = 0x10000;
    const uint64_t want = (span_ + align - 1) & ~(align - 1);
    for (uintptr_t hint = 0x10000000; hint < 0x70000000; hint += 0x10000000) {
        void* p = VirtualAlloc(reinterpret_cast<void*>(hint), want,
                               MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
        if (p) { base_ = static_cast<uint8_t*>(p); break; }
    }
    if (!base_) return false;

    std::memset(base_, 0, want);
    for (const auto& s : segs_)
        std::memcpy(base_ + s.vaddr, s.file, static_cast<size_t>(s.filesz));

    findRelocs(blob_);
    findImports(blob_);
    return true;
}

size_t Image::relocate() {
    for (const auto& r : relocs_)
        *reinterpret_cast<uint64_t*>(base_ + r.offset) = reinterpret_cast<uint64_t>(base_ + r.addend);
    return relocs_.size();
}

}  // namespace ps4
