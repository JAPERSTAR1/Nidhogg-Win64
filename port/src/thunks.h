// Import thunks.
//
// Every SCE import is a GOT slot that the loader would have filled with the
// address of the real library function.  Until the shim for that library
// exists, the slot points at a generated stub that records the call and
// returns zero, so the image keeps running far enough to see what it wants.
#pragma once

#include <cstdint>
#include <vector>

namespace ps4 {

class Image;

class Thunks {
public:
    void install(Image& image);
    size_t count() const { return stubs_.size(); }

private:
    std::vector<uint8_t*> stubs_;
};

}  // namespace ps4
