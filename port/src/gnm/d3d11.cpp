#include "d3d11.h"

#include <windows.h>

#include <d3d11.h>
#include <d3dcompiler.h>
#include <dxgi.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unordered_map>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace ps4::gnm {
namespace {

// ---------------------------------------------------------------- registers --

// SH register space: GNM numbers the stage user-data blocks from these bases
// and the command stream carries them already biased by 0x2C00, which is the
// same bias pm4.cpp applies.  PS dwords 4..11 are the 256-bit T#, 12..15 the
// 128-bit S#; VS dwords 0..1 the fetch shader, 2..3 the V# table, 8..11 the matrix V#.
constexpr uint32_t kPsTexture = 0x0C + 4;
constexpr uint32_t kPsSampler = 0x0C + 12;
constexpr uint32_t kVsFetch = 0x4C;
constexpr uint32_t kVsTable = 0x4C + 2;
constexpr uint32_t kVsMatrix = 0x4C + 8;

constexpr uint32_t kCtxTargetMask = 0x08E;
constexpr uint32_t kCtxBlendConstants = 0x105;
constexpr uint32_t kCtxViewport = 0x10F;
constexpr uint32_t kCtxBlend0Control = 0x1E0;
constexpr uint32_t kCtxColor0Base = 0x318;

// One buffer descriptor (V#).  The address is 40 bits split across the first
// two dwords; the rest is stride and the component format.
struct BufferDesc {
    uint64_t base = 0;
    uint32_t stride = 0;
    uint32_t records = 0;
    uint32_t format = 0;
};
