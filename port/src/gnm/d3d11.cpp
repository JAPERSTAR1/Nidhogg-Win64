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

BufferDesc DecodeBuffer(const uint32_t* w) {
    BufferDesc b;
    b.base = (static_cast<uint64_t>(w[1] & 0xff) << 32) | w[0];
    b.stride = (w[1] >> 16) & 0x3fffu;
    b.records = w[2];
    b.format = w[3];
    return b;
}

// One 256-bit image descriptor (T#).  GNM stores the address as address >> 8,
// and dword 4 bits 26:13 carry (pitch - 1) in texels for linear-aligned images.
struct ImageDesc {
    uint64_t address = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t pitch = 0;
    uint32_t dataFormat = 0;
    uint32_t numFormat = 0;
    uint32_t tiling = 0;
    uint32_t type = 0;
    uint32_t src[8] = {};
};

ImageDesc DecodeImage(const uint32_t* w) {
    ImageDesc i;
    for (int k = 0; k < 8; ++k) i.src[k] = w[k];
    i.address = ((static_cast<uint64_t>(w[1] & 0x3f) << 32) | w[0]) << 8;
    i.dataFormat = (w[1] >> 20) & 0x3f;
    i.numFormat = (w[1] >> 26) & 0xf;
    i.width = (w[2] & 0x3fff) + 1;
    i.height = ((w[2] >> 14) & 0x3fff) + 1;
    i.tiling = (w[3] >> 20) & 0x1f;
    i.type = w[3] >> 28;
    const uint32_t rawPitch = (w[4] >> 13) & 0x3fff;
    i.pitch = rawPitch ? (rawPitch + 1) : i.width;
    if (i.pitch < i.width) i.pitch = i.width;
    return i;
}

// --------------------------------------------------------------- shader pair --

const char* kShaderSource = R"(
cbuffer Transform : register(b0) {
    float4 Row0;
    float4 Row1;
    float4 Row2;
    float4 Row3;
};

struct VsIn {
    float3 pos   : POSITION;
    float4 color : COLOR0;
    float2 uv    : TEXCOORD0;
};

struct VsOut {
    float4 pos   : SV_POSITION;
    float4 color : COLOR0;
    float2 uv    : TEXCOORD0;
};

VsOut VSMain(VsIn i) {
    VsOut o;
    o.pos = i.pos.x * Row0 + i.pos.y * Row1 + i.pos.z * Row2 + Row3;
    o.color = i.color;
    o.uv = i.uv;
    return o;
}

Texture2D    Source : register(t0);
SamplerState Linear : register(s0);

float4 PSMain(VsOut i) : SV_Target {
    return Source.Sample(Linear, i.uv) * i.color;
}
)";

struct Vertex {
    float pos[3];
    float color[4];
    float uv[2];
};

// --------------------------------------------------------------------- state --

struct Renderer {
    bool tried = false;
    bool ok = false;

    HWND hwnd = nullptr;
    uint32_t width = 0;
    uint32_t height = 0;

    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
