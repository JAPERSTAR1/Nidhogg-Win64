// D3D11 backend for the PM4 interpreter.
//
// The guest emits GNM command buffers; pm4.cpp turns them into Draw records
// with a full register snapshot.  This side turns a Draw into API calls.  The
// two shaders this title uses are hand-translated from their GCN disassembly:
//
//   VS  position = x*A + y*B + z*C + D, where A..D are the four rows of the
//       matrix the guest parks behind VS user-data slot 2, and the vertex is
//       fetched from the three V#s behind slot 1 (pos, colour, uv).
//   PS  sample the T# from PS user-data dword 4 with the sampler at dword 8 and
//       multiply by the interpolated colour.
//
// Anything that does not match that shape is skipped rather than guessed at.
#pragma once

#include <cstdint>
#include <vector>

#include "pm4.h"

namespace ps4::gnm {

// Renders everything the guest submitted this frame and presents it.  Safe to
// call with an empty list or before the window exists: it does nothing then.
void RenderFrame(void* hwnd, const std::vector<Draw>& draws);

}  // namespace ps4::gnm
