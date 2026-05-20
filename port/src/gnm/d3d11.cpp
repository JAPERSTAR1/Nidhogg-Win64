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

const char* kPresentShaderSource = R"(
cbuffer PresentParams : register(b0) {
    float2 TexSize;
    float2 InvTexSize;
    int    FilterMode;   // 0=Point, 1=SharpBilinear, 2=Bilinear, 3=Anisotropic
    int    Scanlines;
    float2 Pad;
};
struct VsOut {
    float4 pos : SV_POSITION;
    float2 uv  : TEXCOORD0;
};
VsOut VSMain(uint vid : SV_VertexID) {
    VsOut o;
    o.uv = float2((vid << 1) & 2, vid & 2);
    o.pos = float4(o.uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
    return o;
}
Texture2D    MasterTex : register(t0);
SamplerState Smp       : register(s0);
float4 PSMain(VsOut i) : SV_Target {
    float2 uv = i.uv;
    if (FilterMode == 1) {
        // Sharp-bilinear pixel-art anti-aliasing: crisp texel interiors with
        // 1-device-pixel anti-aliased transitions on non-integer resolutions.
        float2 tx = uv * TexSize;
        float2 fw = max(fwidth(tx), float2(1e-4, 1e-4));
        float2 fl = floor(tx - 0.5) + 0.5;
        float2 fr = saturate((tx - fl) / fw);
        uv = (fl + fr) * InvTexSize;
    }
    float4 col = MasterTex.Sample(Smp, uv);
    if (Scanlines != 0) {
        float s = 0.5 - 0.5 * cos(i.uv.y * 1080.0 * 6.2831853);
        col.rgb *= lerp(1.0, 0.83, s);
    }
    col.a = 1.0;
    return col;
}
)";

struct Vertex {
    float pos[3];
    float color[4];
    float uv[2];
};

struct PresentCb {
    float texSize[2];
    float invTexSize[2];
    int filterMode;
    int scanlines;
    float pad[2];
};

struct Renderer {
    bool tried = false;
    bool ok = false;
    bool allowTearing = false;
    bool displayInitialized = false;

    HWND hwnd = nullptr;
    uint32_t swapWidth = 0;
    uint32_t swapHeight = 0;
    int swapBuffers = 2;

    uint32_t masterWidth = 0;
    uint32_t masterHeight = 0;

    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<IDXGISwapChain> swap;
    ComPtr<ID3D11RenderTargetView> backBuffer;

    ComPtr<ID3D11Texture2D> masterTex;
    ComPtr<ID3D11RenderTargetView> masterRtv;
    ComPtr<ID3D11ShaderResourceView> masterSrv;

    ComPtr<ID3D11VertexShader> vs;
    ComPtr<ID3D11PixelShader> ps;
    ComPtr<ID3D11InputLayout> layout;
    ComPtr<ID3D11Buffer> vb;
    ComPtr<ID3D11Buffer> ib;
    ComPtr<ID3D11Buffer> cb;

    ComPtr<ID3D11VertexShader> presentVs;
    ComPtr<ID3D11PixelShader> presentPs;
    ComPtr<ID3D11Buffer> presentCb;
    ComPtr<ID3D11SamplerState> presentPointSmp;
    ComPtr<ID3D11SamplerState> presentLinearSmp;
    ComPtr<ID3D11SamplerState> presentAnisoSmp;
    int builtAnisoLevel = 0;

    ComPtr<ID3D11BlendState> blendDisabled;
    ComPtr<ID3D11RasterizerState> rasterizer;
    ComPtr<ID3D11DepthStencilState> depthDisabled;
    ComPtr<ID3D11ShaderResourceView> missingTexture;
    ComPtr<ID3D11SamplerState> defaultSampler;

    struct OffscreenRt {
        uint32_t width = 0;
        uint32_t height = 0;
        ComPtr<ID3D11Texture2D> tex;
        ComPtr<ID3D11RenderTargetView> rtv;
        ComPtr<ID3D11ShaderResourceView> srv;
    };
    struct ClearRgba {
        float c[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    };

    std::unordered_map<uint64_t, ID3D11ShaderResourceView*> textures;
    std::unordered_map<uint64_t, ID3D11SamplerState*> samplers;
    std::unordered_map<uint64_t, ID3D11BlendState*> blends;
    std::unordered_map<uint64_t, OffscreenRt> offscreenRts;
    std::unordered_map<uint64_t, ClearRgba> pendingClears;

    uint32_t vbCapacity = 0;
    uint32_t ibCapacity = 0;
    uint64_t lastPresentQpcUsec = 0;

    std::vector<Vertex> vertices;
    std::vector<uint32_t> indices;
    float matrix[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
};

Renderer g;

uint64_t QpcUsec() {
    static LARGE_INTEGER freq{};
    if (!freq.QuadPart) QueryPerformanceFrequency(&freq);
    LARGE_INTEGER now{};
    QueryPerformanceCounter(&now);
    return static_cast<uint64_t>(now.QuadPart * 1000000ULL / freq.QuadPart);
}

bool Readable(uint64_t address, size_t bytes) {
    return address != 0 && ps4::gnm::HostReadable(address, bytes);
}

uint32_t TexelBytes(uint32_t dataFormat) {
    switch (dataFormat) {
        case 1: return 1;
        case 2: return 2;
        case 3: return 2;
        case 4: return 4;
        case 5: return 4;
        case 6: return 4;
        case 7: return 4;
        case 8: return 4;
        case 9: return 4;
        case 10: return 4;
        case 11: return 8;
        case 12: return 8;
        case 13: return 12;
        case 14: return 16;
        case 16: return 2;
        case 17: return 2;
        case 19: return 2;
        default: return 0;
    }
}

DXGI_FORMAT TexelFormat(uint32_t dataFormat, uint32_t numFormat) {
    switch (dataFormat) {
        case 1: return DXGI_FORMAT_R8_UNORM;
        case 2: return numFormat == 7 ? DXGI_FORMAT_R16_FLOAT : DXGI_FORMAT_R16_UNORM;
        case 3: return DXGI_FORMAT_R8G8_UNORM;
        case 4: return DXGI_FORMAT_R32_FLOAT;
        case 5: return numFormat == 7 ? DXGI_FORMAT_R16G16_FLOAT : DXGI_FORMAT_R16G16_UNORM;
        case 8: return DXGI_FORMAT_R10G10B10A2_UNORM;
        case 10:
            return numFormat == 9 ? DXGI_FORMAT_R8G8B8A8_UNORM_SRGB
                                  : DXGI_FORMAT_R8G8B8A8_UNORM;
        case 11: return DXGI_FORMAT_R32G32_FLOAT;
        case 12:
            return numFormat == 7 ? DXGI_FORMAT_R16G16B16A16_FLOAT
                                  : DXGI_FORMAT_R16G16B16A16_UNORM;
        case 13: return DXGI_FORMAT_R32G32B32_FLOAT;
        case 14: return DXGI_FORMAT_R32G32B32A32_FLOAT;
        case 16: return DXGI_FORMAT_B5G6R5_UNORM;
        case 17: return DXGI_FORMAT_B5G5R5A1_UNORM;
        default: return DXGI_FORMAT_UNKNOWN;
    }
}

uint64_t ImageKey(const ImageDesc& i) {
    uint64_t k = i.address * 1099511628211ull;
    k ^= static_cast<uint64_t>(i.width) << 48;
    k ^= static_cast<uint64_t>(i.height) << 32;
    k ^= static_cast<uint64_t>(i.pitch) << 16;
    k ^= static_cast<uint64_t>(i.dataFormat) << 8;
    k ^= i.numFormat << 4;
    k ^= i.tiling;
    const uint32_t texel = TexelBytes(i.dataFormat);
    const uint64_t span = static_cast<uint64_t>(i.pitch) * texel * i.height;
    if (texel && span >= 8 && Readable(i.address, static_cast<size_t>(span))) {
        const auto* b = reinterpret_cast<const uint8_t*>(i.address);
        const uint64_t step = span > 512 ? (span / 64) & ~7ull : 8;
        for (uint64_t off = 0; off + 8 <= span; off += (step ? step : 8)) {
            uint64_t w;
            std::memcpy(&w, b + off, 8);
            k = (k ^ w) * 1099511628211ull;
        }
    }
    return k;
}

ID3D11ShaderResourceView* CreateTexture(const ImageDesc& image) {
    const uint32_t texel = TexelBytes(image.dataFormat);
    const DXGI_FORMAT format = TexelFormat(image.dataFormat, image.numFormat);
    if (!texel || format == DXGI_FORMAT_UNKNOWN) return nullptr;

    const uint64_t pitchBytes = static_cast<uint64_t>(image.pitch) * texel;
    const uint64_t total = pitchBytes * (image.height - 1) + static_cast<uint64_t>(image.width) * texel;
    if (!image.width || !image.height || total > (32u << 20)) return nullptr;
    if (!Readable(image.address, static_cast<size_t>(total))) return nullptr;

    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = image.width;
    desc.Height = image.height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = format;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_IMMUTABLE;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

    D3D11_SUBRESOURCE_DATA data{};
    data.pSysMem = reinterpret_cast<const void*>(image.address);
    data.SysMemPitch = static_cast<UINT>(pitchBytes);

    ComPtr<ID3D11Texture2D> texture;
    if (FAILED(g.device->CreateTexture2D(&desc, &data, &texture))) return nullptr;

    D3D11_SHADER_RESOURCE_VIEW_DESC view{};
    view.Format = format;
    view.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    view.Texture2D.MipLevels = 1;

    ID3D11ShaderResourceView* srv = nullptr;
    if (FAILED(g.device->CreateShaderResourceView(texture.Get(), &view, &srv))) return nullptr;
    return srv;
}

ID3D11ShaderResourceView* LookupTexture(const ImageDesc& image) {
    if (!image.address) return nullptr;
    auto rtIt = g.offscreenRts.find(image.address);
    if (rtIt != g.offscreenRts.end() && rtIt->second.srv) return rtIt->second.srv.Get();
    const uint64_t key = ImageKey(image);
    auto it = g.textures.find(key);
    if (it != g.textures.end()) return it->second;
    ID3D11ShaderResourceView* srv = CreateTexture(image);
    if (srv) g.textures.emplace(key, srv);
    return srv;
}

ID3D11RenderTargetView* EnsureOffscreenRt(uint64_t addr, uint32_t w, uint32_t h) {
    auto it = g.offscreenRts.find(addr);
    if (it != g.offscreenRts.end() && it->second.width == w && it->second.height == h)
        return it->second.rtv.Get();

    Renderer::OffscreenRt rt;
    rt.width = w;
    rt.height = h;
    D3D11_TEXTURE2D_DESC td{};
    td.Width = w;
    td.Height = h;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    if (FAILED(g.device->CreateTexture2D(&td, nullptr, &rt.tex))) return nullptr;
    if (FAILED(g.device->CreateRenderTargetView(rt.tex.Get(), nullptr, &rt.rtv))) return nullptr;
    if (FAILED(g.device->CreateShaderResourceView(rt.tex.Get(), nullptr, &rt.srv))) return nullptr;

    float clr[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    auto pc = g.pendingClears.find(addr);
    if (pc != g.pendingClears.end()) {
        std::memcpy(clr, pc->second.c, sizeof(clr));
        g.pendingClears.erase(pc);
    }
    g.context->ClearRenderTargetView(rt.rtv.Get(), clr);
    auto& slot = g.offscreenRts[addr];
    slot = std::move(rt);
    return slot.rtv.Get();
}

bool EnsureMasterRt(uint32_t w, uint32_t h) {
    w = std::clamp(w, 320u, 7680u);
    h = std::clamp(h, 180u, 4320u);
    if (g.masterRtv && g.masterSrv && g.masterWidth == w && g.masterHeight == h) return true;

    g.masterRtv.Reset();
    g.masterSrv.Reset();
    g.masterTex.Reset();

    D3D11_TEXTURE2D_DESC td{};
    td.Width = w;
    td.Height = h;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    if (FAILED(g.device->CreateTexture2D(&td, nullptr, &g.masterTex))) return false;
    if (FAILED(g.device->CreateRenderTargetView(g.masterTex.Get(), nullptr, &g.masterRtv)))
        return false;
    if (FAILED(g.device->CreateShaderResourceView(g.masterTex.Get(), nullptr, &g.masterSrv)))
        return false;

    g.masterWidth = w;
    g.masterHeight = h;
    LOG_INFO("[d3d11] internal master RT resized to %ux%u", w, h);
    return true;
}

D3D11_TEXTURE_ADDRESS_MODE MapClamp(uint32_t mode) {
    switch (mode & 0x7) {
        case 0: return D3D11_TEXTURE_ADDRESS_WRAP;
        case 1: return D3D11_TEXTURE_ADDRESS_MIRROR;
        case 3: return D3D11_TEXTURE_ADDRESS_MIRROR_ONCE;
        case 6: return D3D11_TEXTURE_ADDRESS_BORDER;
        default: return D3D11_TEXTURE_ADDRESS_CLAMP;
    }
}

ID3D11SamplerState* LookupSampler(const uint32_t* words) {
    const auto& cfg = core::CurrentSettings();
    const uint32_t anisoTag = (cfg.textureFilter == core::TextureFilterMode::Anisotropic)
                                  ? static_cast<uint32_t>(std::clamp(cfg.anisotropy, 1, 16))
                                  : 0u;
    const uint64_t key = (static_cast<uint64_t>(words[0]) * 1099511628211ull) ^
                         (static_cast<uint64_t>(words[2]) << 32) ^ words[1] ^
                         (static_cast<uint64_t>(anisoTag) << 56);
    auto it = g.samplers.find(key);
    if (it != g.samplers.end()) return it->second;

    const uint32_t w0 = words[0];
    const uint32_t w2 = words[2];
    const bool magLinear = ((w2 >> 20) & 0x3) == 1 || ((w2 >> 20) & 0x3) == 3;
    const bool minLinear = ((w2 >> 22) & 0x3) == 1 || ((w2 >> 22) & 0x3) == 3;
    D3D11_FILTER filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
    if (anisoTag > 1 && (minLinear || magLinear)) filter = D3D11_FILTER_ANISOTROPIC;
    else if (minLinear && magLinear) filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    else if (minLinear) filter = D3D11_FILTER_MIN_LINEAR_MAG_MIP_POINT;
    else if (magLinear) filter = D3D11_FILTER_MIN_POINT_MAG_LINEAR_MIP_POINT;

    D3D11_SAMPLER_DESC desc{};
    desc.Filter = filter;
    desc.AddressU = MapClamp(w0);
    desc.AddressV = MapClamp(w0 >> 3);
    desc.AddressW = MapClamp(w0 >> 6);
    desc.MaxAnisotropy = std::max(1u, anisoTag);
    desc.MaxLOD = D3D11_FLOAT32_MAX;

    ID3D11SamplerState* state = nullptr;
    if (FAILED(g.device->CreateSamplerState(&desc, &state))) return nullptr;
    g.samplers.emplace(key, state);
    return state;
}

D3D11_BLEND MapBlendFactor(uint32_t f, bool isAlpha) {
    switch (f & 0x1f) {
        case 0: return D3D11_BLEND_ZERO;
        case 1: return D3D11_BLEND_ONE;
        case 2: return isAlpha ? D3D11_BLEND_SRC_ALPHA : D3D11_BLEND_SRC_COLOR;
        case 3: return isAlpha ? D3D11_BLEND_INV_SRC_ALPHA : D3D11_BLEND_INV_SRC_COLOR;
        case 4: return D3D11_BLEND_SRC_ALPHA;
        case 5: return D3D11_BLEND_INV_SRC_ALPHA;
        case 6: return D3D11_BLEND_DEST_ALPHA;
        case 7: return D3D11_BLEND_INV_DEST_ALPHA;
        case 8: return isAlpha ? D3D11_BLEND_DEST_ALPHA : D3D11_BLEND_DEST_COLOR;
        case 9: return isAlpha ? D3D11_BLEND_INV_DEST_ALPHA : D3D11_BLEND_INV_DEST_COLOR;
        case 10: return D3D11_BLEND_SRC_ALPHA_SAT;
        case 13:
        case 19: return D3D11_BLEND_BLEND_FACTOR;
        case 14:
        case 20: return D3D11_BLEND_INV_BLEND_FACTOR;
        default: return D3D11_BLEND_ONE;
    }
}

D3D11_BLEND_OP MapBlendOp(uint32_t op) {
    switch (op & 0x7) {
        case 1: return D3D11_BLEND_OP_SUBTRACT;
        case 2: return D3D11_BLEND_OP_MIN;
        case 3: return D3D11_BLEND_OP_MAX;
        case 4: return D3D11_BLEND_OP_REV_SUBTRACT;
        default: return D3D11_BLEND_OP_ADD;
    }
}

ID3D11BlendState* LookupBlend(uint32_t ctl, uint32_t targetMask) {
    const uint8_t writeMask = (targetMask & 0xf) ? static_cast<uint8_t>(targetMask & 0xf)
                                                 : static_cast<uint8_t>(D3D11_COLOR_WRITE_ENABLE_ALL);
    const uint64_t key = (static_cast<uint64_t>(writeMask) << 32) | ctl;
    auto it = g.blends.find(key);
    if (it != g.blends.end()) return it->second;

    const bool enable = ((ctl >> 30) & 1u) != 0;
    const bool sepAlpha = ((ctl >> 29) & 1u) != 0;
    const uint32_t cSrc = ctl & 0x1f;
    const uint32_t cOp = (ctl >> 5) & 0x7;
    const uint32_t cDst = (ctl >> 8) & 0x1f;
    const uint32_t aSrc = sepAlpha ? ((ctl >> 16) & 0x1f) : cSrc;
    const uint32_t aOp = sepAlpha ? ((ctl >> 21) & 0x7) : cOp;
    const uint32_t aDst = sepAlpha ? ((ctl >> 24) & 0x1f) : cDst;

    D3D11_BLEND_DESC desc{};
    auto& rt = desc.RenderTarget[0];
    rt.BlendEnable = enable ? TRUE : FALSE;
    rt.SrcBlend = MapBlendFactor(cSrc, false);
    rt.DestBlend = MapBlendFactor(cDst, false);
    rt.BlendOp = MapBlendOp(cOp);
    rt.SrcBlendAlpha = MapBlendFactor(aSrc, true);
    rt.DestBlendAlpha = MapBlendFactor(aDst, true);
    rt.BlendOpAlpha = MapBlendOp(aOp);
    rt.RenderTargetWriteMask = writeMask;

    ID3D11BlendState* state = nullptr;
    if (FAILED(g.device->CreateBlendState(&desc, &state))) return g.blendDisabled.Get();
    g.blends.emplace(key, state);
    return state;
}

bool CompileShaders() {
    ComPtr<ID3DBlob> vsBlob, psBlob, pvsBlob, ppsBlob, errors;
    const UINT flags = D3DCOMPILE_ENABLE_STRICTNESS;

    if (FAILED(D3DCompile(kGuestShaderSource, std::strlen(kGuestShaderSource), "nidhogg.hlsl",
                          nullptr, nullptr, "VSMain", "vs_5_0", flags, 0, &vsBlob, &errors)))
        return false;
    if (FAILED(D3DCompile(kGuestShaderSource, std::strlen(kGuestShaderSource), "nidhogg.hlsl",
                          nullptr, nullptr, "PSMain", "ps_5_0", flags, 0, &psBlob, &errors)))
        return false;
    if (FAILED(D3DCompile(kPresentShaderSource, std::strlen(kPresentShaderSource), "present.hlsl",
                          nullptr, nullptr, "VSMain", "vs_5_0", flags, 0, &pvsBlob, &errors)))
        return false;
    if (FAILED(D3DCompile(kPresentShaderSource, std::strlen(kPresentShaderSource), "present.hlsl",
                          nullptr, nullptr, "PSMain", "ps_5_0", flags, 0, &ppsBlob, &errors)))
        return false;

    if (FAILED(g.device->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(),
                                            nullptr, &g.vs)))
        return false;
    if (FAILED(g.device->CreatePixelShader(psBlob->GetBufferPointer(), psBlob->GetBufferSize(),
                                           nullptr, &g.ps)))
        return false;
    if (FAILED(g.device->CreateVertexShader(pvsBlob->GetBufferPointer(), pvsBlob->GetBufferSize(),
                                            nullptr, &g.presentVs)))
        return false;
    if (FAILED(g.device->CreatePixelShader(ppsBlob->GetBufferPointer(), ppsBlob->GetBufferSize(),
                                           nullptr, &g.presentPs)))
        return false;

    const D3D11_INPUT_ELEMENT_DESC elements[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT,    0, 0,  D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"COLOR",    0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT,       0, 28, D3D11_INPUT_PER_VERTEX_DATA, 0},
    };
    return SUCCEEDED(g.device->CreateInputLayout(elements, 3, vsBlob->GetBufferPointer(),
                                                 vsBlob->GetBufferSize(), &g.layout));
}

bool EnsureSwapchainSize(uint32_t width, uint32_t height, int wantBuffers) {
    width = std::max(16u, width);
    height = std::max(16u, height);
    wantBuffers = std::clamp(wantBuffers, 2, 3);
    if (g.swap && g.backBuffer && width == g.swapWidth && height == g.swapHeight &&
        wantBuffers == g.swapBuffers)
        return true;

    g.backBuffer.Reset();
    if (!g.swap) return false;
    const UINT flags = g.allowTearing ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0u;
    if (FAILED(g.swap->ResizeBuffers(static_cast<UINT>(wantBuffers), width, height,
                                     DXGI_FORMAT_R8G8B8A8_UNORM, flags)))
        return false;

    ComPtr<ID3D11Texture2D> back;
    if (FAILED(g.swap->GetBuffer(0, IID_PPV_ARGS(&back)))) return false;
    if (FAILED(g.device->CreateRenderTargetView(back.Get(), nullptr, &g.backBuffer))) return false;
    g.swapWidth = width;
    g.swapHeight = height;
    g.swapBuffers = wantBuffers;
    return true;
}

bool CreateDevice(HWND hwnd, uint32_t width, uint32_t height) {
    BOOL tearing = FALSE;
    {
        ComPtr<IDXGIFactory5> fac5;
        if (SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&fac5)))) {
            if (FAILED(fac5->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &tearing,
                                                 sizeof(tearing))))
                tearing = FALSE;
        }
    }
    g.allowTearing = (tearing == TRUE);
    const int initBufs = std::clamp(core::CurrentSettings().swapBuffers, 2, 3);

    DXGI_SWAP_CHAIN_DESC swapDesc{};
    swapDesc.BufferCount = static_cast<UINT>(initBufs);
    swapDesc.BufferDesc.Width = width;
    swapDesc.BufferDesc.Height = height;
    swapDesc.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    swapDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    swapDesc.OutputWindow = hwnd;
    swapDesc.SampleDesc.Count = 1;
    swapDesc.Windowed = TRUE;
    swapDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    swapDesc.Flags = g.allowTearing ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0u;

    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
    D3D_FEATURE_LEVEL got{};
    HRESULT hr = D3D11CreateDeviceAndSwapChain(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, levels, 2, D3D11_SDK_VERSION, &swapDesc,
        &g.swap, &g.device, &got, &g.context);
    if (FAILED(hr)) {
        g.allowTearing = false;
        swapDesc.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
        swapDesc.Flags = 0;
        hr = D3D11CreateDeviceAndSwapChain(
            nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, levels, 2, D3D11_SDK_VERSION, &swapDesc,
            &g.swap, &g.device, &got, &g.context);
    }
    if (FAILED(hr)) {
        LOG_ERROR("[d3d11] device creation failed 0x%08lx", static_cast<unsigned long>(hr));
        return false;
    }

    // Disable DXGI's built-in Alt+Enter handler so our settings/revert manager owns it cleanly.
    {
        ComPtr<IDXGIDevice> dxgiDev;
        ComPtr<IDXGIAdapter> dxgiAdp;
        ComPtr<IDXGIFactory1> dxgiFac;
        if (SUCCEEDED(g.device.As(&dxgiDev)) && SUCCEEDED(dxgiDev->GetAdapter(&dxgiAdp)) &&
            SUCCEEDED(dxgiAdp->GetParent(IID_PPV_ARGS(&dxgiFac)))) {
            dxgiFac->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER);
        }
    }

    LOG_INFO("[d3d11] feature level 0x%x (tearing=%d)", static_cast<unsigned>(got),
             g.allowTearing ? 1 : 0);

    ComPtr<ID3D11Texture2D> back;
    if (FAILED(g.swap->GetBuffer(0, IID_PPV_ARGS(&back)))) return false;
    if (FAILED(g.device->CreateRenderTargetView(back.Get(), nullptr, &g.backBuffer))) return false;

    D3D11_BLEND_DESC blend{};
    blend.RenderTarget[0].BlendEnable = FALSE;
    blend.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    if (FAILED(g.device->CreateBlendState(&blend, &g.blendDisabled))) return false;

    D3D11_RASTERIZER_DESC raster{};
    raster.FillMode = D3D11_FILL_SOLID;
    raster.CullMode = D3D11_CULL_NONE;
    raster.FrontCounterClockwise = TRUE;
    raster.DepthClipEnable = TRUE;
    if (FAILED(g.device->CreateRasterizerState(&raster, &g.rasterizer))) return false;

    D3D11_DEPTH_STENCIL_DESC depth{};
    depth.DepthEnable = FALSE;
    depth.StencilEnable = FALSE;
    if (FAILED(g.device->CreateDepthStencilState(&depth, &g.depthDisabled))) return false;

    const uint32_t pixels[4] = {0xffffffffu, 0xffffffffu, 0xffffffffu, 0xffffffffu};
    D3D11_TEXTURE2D_DESC whiteDesc{};
    whiteDesc.Width = 2;
    whiteDesc.Height = 2;
    whiteDesc.MipLevels = 1;
    whiteDesc.ArraySize = 1;
    whiteDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    whiteDesc.SampleDesc.Count = 1;
    whiteDesc.Usage = D3D11_USAGE_IMMUTABLE;
    whiteDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA whiteData{pixels, 8, 0};
    ComPtr<ID3D11Texture2D> whiteTexture;
    if (SUCCEEDED(g.device->CreateTexture2D(&whiteDesc, &whiteData, &whiteTexture)))
        g.device->CreateShaderResourceView(whiteTexture.Get(), nullptr, &g.missingTexture);

    D3D11_SAMPLER_DESC smp{};
    smp.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
    smp.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    smp.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    smp.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    smp.MaxLOD = D3D11_FLOAT32_MAX;
    if (FAILED(g.device->CreateSamplerState(&smp, &g.defaultSampler))) return false;
    if (FAILED(g.device->CreateSamplerState(&smp, &g.presentPointSmp))) return false;

    smp.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    if (FAILED(g.device->CreateSamplerState(&smp, &g.presentLinearSmp))) return false;

    D3D11_BUFFER_DESC bufferDesc{};
    bufferDesc.Usage = D3D11_USAGE_DYNAMIC;
    bufferDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    bufferDesc.ByteWidth = 64;
    bufferDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if (FAILED(g.device->CreateBuffer(&bufferDesc, nullptr, &g.cb))) return false;

    bufferDesc.ByteWidth = sizeof(PresentCb);
    if (FAILED(g.device->CreateBuffer(&bufferDesc, nullptr, &g.presentCb))) return false;

    g.swapWidth = width;
    g.swapHeight = height;
    g.swapBuffers = initBufs;
    return CompileShaders();
}

bool EnsureRenderer(HWND hwnd) {
    RECT rc{};
    GetClientRect(hwnd, &rc);
    const uint32_t cw = static_cast<uint32_t>(std::max<LONG>(16, rc.right - rc.left));
    const uint32_t ch = static_cast<uint32_t>(std::max<LONG>(16, rc.bottom - rc.top));

    if (g.ok && g.hwnd == hwnd) {
        return EnsureSwapchainSize(cw, ch, core::CurrentSettings().swapBuffers);
    }
    if (g.tried) return false;
    g.tried = true;
    g.hwnd = hwnd;
    g.ok = CreateDevice(hwnd, cw, ch);
    if (g.ok && !g.displayInitialized) {
        g.displayInitialized = true;
        ApplyDisplaySettings(hwnd);
        GetClientRect(hwnd, &rc);
        EnsureSwapchainSize(static_cast<uint32_t>(std::max<LONG>(16, rc.right - rc.left)),
                            static_cast<uint32_t>(std::max<LONG>(16, rc.bottom - rc.top)),
                            core::CurrentSettings().swapBuffers);
    }
    return g.ok;
}

bool BuildGeometry(const Draw& draw, const uint32_t* sh) {
    const uint64_t table = (static_cast<uint64_t>(sh[kVsTable + 1] & 0xffff) << 32) |
                           sh[kVsTable];
    if (!Readable(table, 0x30)) return false;

    const auto* tableWords = reinterpret_cast<const uint32_t*>(table);
    const BufferDesc pos = DecodeBuffer(tableWords + 0);
    const BufferDesc color = DecodeBuffer(tableWords + 4);
    const BufferDesc uv = DecodeBuffer(tableWords + 8);
    if (!pos.stride || !pos.base) return false;

    std::vector<uint32_t> localIndices;
    const uint32_t* indexData = nullptr;
    uint32_t indexCount = 0;

    if (draw.kind == Draw::Kind::Indexed) {
        const uint32_t bytes = draw.indexCount * (draw.indexType ? 4u : 2u);
        if (!draw.indexCount || !Readable(draw.indexBase, bytes)) return false;
        indexCount = draw.indexCount;
        if (draw.indexType) {
            indexData = reinterpret_cast<const uint32_t*>(draw.indexBase);
        } else {
            localIndices.resize(indexCount);
            const auto* src = reinterpret_cast<const uint16_t*>(draw.indexBase);
            for (uint32_t i = 0; i < indexCount; ++i) localIndices[i] = src[i];
            indexData = localIndices.data();
        }
    } else if (draw.kind == Draw::Kind::Auto) {
        if (!draw.indexCount) return false;
        indexCount = draw.indexCount;
        localIndices.resize(indexCount);
        for (uint32_t i = 0; i < indexCount; ++i) localIndices[i] = i;
        indexData = localIndices.data();
    } else {
        return false;
    }

    uint32_t highest = 0;
    for (uint32_t i = 0; i < indexCount; ++i)
        if (indexData[i] > highest) highest = indexData[i];

    const uint32_t need = highest + 1;
    if (need > 65536) return false;

    const uint64_t posBytes = static_cast<uint64_t>(pos.stride) * need;
    const uint64_t colorBytes = static_cast<uint64_t>(color.stride) * need;
    const uint64_t uvBytes = static_cast<uint64_t>(uv.stride) * need;
    if (!Readable(pos.base, static_cast<size_t>(posBytes))) return false;
    const bool haveColor = color.stride && color.base && Readable(color.base, static_cast<size_t>(colorBytes));
    const bool haveUv = uv.stride && uv.base && Readable(uv.base, static_cast<size_t>(uvBytes));

    const uint32_t baseVertex = static_cast<uint32_t>(g.vertices.size());
    for (uint32_t v = 0; v < need; ++v) {
        Vertex out{};
        std::memcpy(out.pos, reinterpret_cast<const void*>(pos.base + v * pos.stride), 12);
        if (haveColor)
            std::memcpy(out.color, reinterpret_cast<const void*>(color.base + v * color.stride), 16);
        else
            out.color[0] = out.color[1] = out.color[2] = out.color[3] = 1.0f;
        if (haveUv) std::memcpy(out.uv, reinterpret_cast<const void*>(uv.base + v * uv.stride), 8);
        g.vertices.push_back(out);
    }
    for (uint32_t i = 0; i < indexCount; ++i) g.indices.push_back(baseVertex + indexData[i]);
    return true;
}

bool LoadMatrix(const uint32_t* sh) {
    const uint64_t address = (static_cast<uint64_t>(sh[kVsMatrix + 1] & 0xffff) << 32) |
                             sh[kVsMatrix];
    const uint32_t stride = (sh[kVsMatrix + 1] >> 16) & 0x3fff;
    if (!Readable(address, 0x40)) return false;
    const uint32_t step = stride ? stride : 16;
    for (int row = 0; row < 4; ++row)
        std::memcpy(&g.matrix[row * 4],
                    reinterpret_cast<const void*>(address + static_cast<uint64_t>(row) * step),
                    16);
    return true;
}

void UploadAndDraw() {
    if (g.indices.empty() || g.vertices.empty()) return;

    if (!g.vb || g.vbCapacity < g.vertices.size()) {
        g.vb.Reset();
        D3D11_BUFFER_DESC desc{};
        desc.Usage = D3D11_USAGE_DYNAMIC;
        desc.BindFlags = D3D11_BIND_VERTEX_BUFFER;
        desc.ByteWidth = static_cast<UINT>(g.vertices.size() * sizeof(Vertex));
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        if (FAILED(g.device->CreateBuffer(&desc, nullptr, &g.vb))) return;
        g.vbCapacity = static_cast<uint32_t>(g.vertices.size());
    }
    if (!g.ib || g.ibCapacity < g.indices.size()) {
        g.ib.Reset();
        D3D11_BUFFER_DESC desc{};
        desc.Usage = D3D11_USAGE_DYNAMIC;
        desc.BindFlags = D3D11_BIND_INDEX_BUFFER;
        desc.ByteWidth = static_cast<UINT>(g.indices.size() * sizeof(uint32_t));
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        if (FAILED(g.device->CreateBuffer(&desc, nullptr, &g.ib))) return;
        g.ibCapacity = static_cast<uint32_t>(g.indices.size());
    }

    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(g.context->Map(g.vb.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) return;
    std::memcpy(mapped.pData, g.vertices.data(), g.vertices.size() * sizeof(Vertex));
    g.context->Unmap(g.vb.Get(), 0);

    if (FAILED(g.context->Map(g.ib.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) return;
    std::memcpy(mapped.pData, g.indices.data(), g.indices.size() * sizeof(uint32_t));
    g.context->Unmap(g.ib.Get(), 0);

    if (FAILED(g.context->Map(g.cb.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) return;
    std::memcpy(mapped.pData, g.matrix, sizeof(g.matrix));
    g.context->Unmap(g.cb.Get(), 0);

    const UINT stride = sizeof(Vertex);
    const UINT offset = 0;
    ID3D11Buffer* vb = g.vb.Get();
    ID3D11Buffer* cb = g.cb.Get();
    ID3D11Buffer* ib = g.ib.Get();
    g.context->IASetVertexBuffers(0, 1, &vb, &stride, &offset);
    g.context->IASetIndexBuffer(ib, DXGI_FORMAT_R32_UINT, 0);
    g.context->IASetInputLayout(g.layout.Get());
    g.context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    g.context->VSSetShader(g.vs.Get(), nullptr, 0);
    g.context->VSSetConstantBuffers(0, 1, &cb);
    g.context->PSSetShader(g.ps.Get(), nullptr, 0);
    g.context->DrawIndexed(static_cast<UINT>(g.indices.size()), 0, 0);
}

void MaybeCaptureScreenshot(uint64_t simFrame) {
    static std::vector<uint64_t> s_shotFrames;
    static std::string s_shotDir = "work";
    static bool s_parsedShots = false;
    if (!s_parsedShots) {
        s_parsedShots = true;
        char* dirEnv = nullptr;
        size_t dirLen = 0;
        if (_dupenv_s(&dirEnv, &dirLen, "NIDHOGG_SCREENSHOT_DIR") == 0 && dirEnv && *dirEnv)
            s_shotDir = dirEnv;
        if (dirEnv) std::free(dirEnv);

        char* env = nullptr;
        size_t len = 0;
        if (_dupenv_s(&env, &len, "NIDHOGG_SCREENSHOT") == 0 && env) {
            const char* p = env;
            while (*p) {
                char* end = nullptr;
                unsigned long long f = std::strtoull(p, &end, 10);
                if (end == p) break;
                if (f > 0) s_shotFrames.push_back(f);
                p = (*end == ',') ? (end + 1) : end;
            }
            std::free(env);
        }
    }
    bool takeShot = false;
    for (uint64_t f : s_shotFrames) {
        if (f == simFrame) { takeShot = true; break; }
    }
    if (!takeShot) return;

    core::MakeDirsUtf8(s_shotDir);
    ComPtr<ID3D11Texture2D> back;
    if (FAILED(g.swap->GetBuffer(0, IID_PPV_ARGS(&back)))) return;
    D3D11_TEXTURE2D_DESC td{};
    back->GetDesc(&td);
    td.Usage = D3D11_USAGE_STAGING;
    td.BindFlags = 0;
    td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> stage;
    if (FAILED(g.device->CreateTexture2D(&td, nullptr, &stage))) return;
    g.context->CopyResource(stage.Get(), back.Get());
    D3D11_MAPPED_SUBRESOURCE map{};
    if (FAILED(g.context->Map(stage.Get(), 0, D3D11_MAP_READ, 0, &map))) return;

    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    ComPtr<IWICImagingFactory> wic;
    if (SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr,
                                   CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic)))) {
        std::string shotPath = s_shotDir + "\\shot_" + std::to_string(simFrame) + ".png";
        std::wstring wpath = core::Utf8ToWide(shotPath);
        ComPtr<IWICStream> stream;
        ComPtr<IWICBitmapEncoder> enc;
        ComPtr<IWICBitmapFrameEncode> frm;
        if (!wpath.empty() &&
            SUCCEEDED(wic->CreateStream(&stream)) &&
            SUCCEEDED(stream->InitializeFromFilename(wpath.c_str(), GENERIC_WRITE)) &&
            SUCCEEDED(wic->CreateEncoder(GUID_ContainerFormatPng, nullptr, &enc)) &&
            SUCCEEDED(enc->Initialize(stream.Get(), WICBitmapEncoderNoCache)) &&
            SUCCEEDED(enc->CreateNewFrame(&frm, nullptr)) &&
            SUCCEEDED(frm->Initialize(nullptr)) &&
            SUCCEEDED(frm->SetSize(td.Width, td.Height))) {
            WICPixelFormatGUID pf = GUID_WICPixelFormat32bppRGBA;
            frm->SetPixelFormat(&pf);
            ComPtr<IWICBitmap> bmp;
            if (SUCCEEDED(wic->CreateBitmapFromMemory(
                    td.Width, td.Height, GUID_WICPixelFormat32bppRGBA,
                    map.RowPitch, map.RowPitch * td.Height,
                    static_cast<BYTE*>(map.pData), &bmp))) {
                frm->WriteSource(bmp.Get(), nullptr);
                frm->Commit();
                enc->Commit();
                LOG_INFO("[d3d11] saved %s", shotPath.c_str());
            }
        }
    }
    g.context->Unmap(stage.Get(), 0);
}

void CompositeAndPresent(HWND hwnd, uint64_t simFrame, bool isNewSimFrame) {
    if (!g.backBuffer || !g.masterSrv) return;
    const auto& cfg = core::CurrentSettings();

    // Unbind masterRtv before sampling masterSrv
    ID3D11RenderTargetView* backRtv = g.backBuffer.Get();
    g.context->OMSetRenderTargets(1, &backRtv, nullptr);
    const float black[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    g.context->ClearRenderTargetView(backRtv, black);

    // Compute aspect-correct presentation viewport (16:9 letterbox/pillarbox or integer scale)
    const float sw = static_cast<float>(g.swapWidth);
    const float sh = static_cast<float>(g.swapHeight);
    float vpW = sw, vpH = sh;
    if (cfg.aspectMode == core::AspectMode::IntegerScale && sw >= 480.0f && sh >= 270.0f) {
        const int scale = std::max(1, std::min(static_cast<int>(sw) / 480, static_cast<int>(sh) / 270));
        vpW = static_cast<float>(480 * scale);
        vpH = static_cast<float>(270 * scale);
    } else {
        if (sw * 9.0f > sh * 16.0f) {
            vpH = sh;
            vpW = std::floor(sh * (16.0f / 9.0f) + 0.5f);
        } else {
            vpW = sw;
            vpH = std::floor(sw * (9.0f / 16.0f) + 0.5f);
        }
    }
    const float vpX = std::floor((sw - vpW) * 0.5f);
    const float vpY = std::floor((sh - vpH) * 0.5f);

    const D3D11_VIEWPORT presVp{vpX, vpY, vpW, vpH, 0.0f, 1.0f};
    g.context->RSSetViewports(1, &presVp);
    g.context->RSSetState(g.rasterizer.Get());
    g.context->OMSetBlendState(g.blendDisabled.Get(), nullptr, 0xffffffffu);
    g.context->OMSetDepthStencilState(g.depthDisabled.Get(), 0);

    PresentCb pcb{};
    pcb.texSize[0] = static_cast<float>(g.masterWidth);
    pcb.texSize[1] = static_cast<float>(g.masterHeight);
    pcb.invTexSize[0] = 1.0f / pcb.texSize[0];
    pcb.invTexSize[1] = 1.0f / pcb.texSize[1];
    pcb.filterMode = static_cast<int>(cfg.textureFilter);
    pcb.scanlines = cfg.postScanlines ? 1 : 0;

    D3D11_MAPPED_SUBRESOURCE map{};
    if (SUCCEEDED(g.context->Map(g.presentCb.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &map))) {
        std::memcpy(map.pData, &pcb, sizeof(pcb));
        g.context->Unmap(g.presentCb.Get(), 0);
    }

    ID3D11SamplerState* smp = g.presentPointSmp.Get();
    if (cfg.textureFilter == core::TextureFilterMode::SharpBilinear ||
        cfg.textureFilter == core::TextureFilterMode::Bilinear) {
        smp = g.presentLinearSmp.Get();
    } else if (cfg.textureFilter == core::TextureFilterMode::Anisotropic) {
        const int wantAniso = std::clamp(cfg.anisotropy, 1, 16);
        if (!g.presentAnisoSmp || g.builtAnisoLevel != wantAniso) {
            g.presentAnisoSmp.Reset();
            D3D11_SAMPLER_DESC ad{};
            ad.Filter = wantAniso > 1 ? D3D11_FILTER_ANISOTROPIC : D3D11_FILTER_MIN_MAG_MIP_LINEAR;
            ad.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
            ad.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
            ad.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
            ad.MaxAnisotropy = static_cast<UINT>(wantAniso);
            ad.MaxLOD = D3D11_FLOAT32_MAX;
            g.device->CreateSamplerState(&ad, &g.presentAnisoSmp);
            g.builtAnisoLevel = wantAniso;
        }
        if (g.presentAnisoSmp) smp = g.presentAnisoSmp.Get();
    }

    ID3D11Buffer* pcbPtr = g.presentCb.Get();
    ID3D11ShaderResourceView* msrv = g.masterSrv.Get();
    g.context->IASetInputLayout(nullptr);
    g.context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    g.context->VSSetShader(g.presentVs.Get(), nullptr, 0);
    g.context->PSSetShader(g.presentPs.Get(), nullptr, 0);
    g.context->PSSetConstantBuffers(0, 1, &pcbPtr);
    g.context->PSSetShaderResources(0, 1, &msrv);
    g.context->PSSetSamplers(0, 1, &smp);
    g.context->Draw(3, 0);

    ID3D11ShaderResourceView* nullSrv = nullptr;
    g.context->PSSetShaderResources(0, 1, &nullSrv);

    // Composite native Settings / Online / Diagnostics UI at full display resolution
    ui::RenderOverlay(hwnd, g.device.Get(), g.context.Get(), g.swapWidth, g.swapHeight,
                      isNewSimFrame);

    if (isNewSimFrame) {
        MaybeCaptureScreenshot(simFrame);
    }

    const uint64_t nowUsec = QpcUsec();
    const double dtMs = g.lastPresentQpcUsec
                            ? static_cast<double>(nowUsec - g.lastPresentQpcUsec) / 1000.0
                            : 16.67;
    g.lastPresentQpcUsec = nowUsec;

    BOOL inFs = FALSE;
    g.swap->GetFullscreenState(&inFs, nullptr);
    UINT sync = 1u;
    if (cfg.vsync == core::VSyncMode::Off) {
        sync = 0u;
    } else if (cfg.vsync == core::VSyncMode::Adaptive) {
        sync = (dtMs > 17.2) ? 0u : 1u;
    }
    const UINT pflags = (sync == 0u && g.allowTearing && !inFs) ? DXGI_PRESENT_ALLOW_TEARING : 0u;
    g.swap->Present(sync, pflags);
}

}  // namespace

void RenderFrame(void* hwndVoid, const std::vector<Draw>& draws) {
    HWND hwnd = static_cast<HWND>(hwndVoid);
    if (!hwnd || draws.empty()) return;
    if (!EnsureRenderer(hwnd)) return;

    MSG msg;
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    const auto& cfg = core::CurrentSettings();
    const float scale = std::clamp(cfg.renderScale, 0.5f, 2.0f);
    const uint32_t targetMasterW = static_cast<uint32_t>(1920.0f * scale + 0.5f);
    const uint32_t targetMasterH = static_cast<uint32_t>(1080.0f * scale + 0.5f);
    if (!EnsureMasterRt(targetMasterW, targetMasterH)) return;

    uint64_t mainRtAddr = 0;
    for (auto it = draws.rbegin(); it != draws.rend(); ++it) {
        if (it->kind == Draw::Kind::Dispatch) continue;
        const uint32_t* sh = it->regs[Space::Sh];
        if (sh[0x048] && sh[0x048] != 0x40100010u) continue;
        mainRtAddr = static_cast<uint64_t>(it->regs[Space::Context][kCtxColor0Base]) << 8;
        if (mainRtAddr) break;
    }

    const float clear[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    ID3D11RenderTargetView* currentRtv = g.masterRtv.Get();
    g.context->OMSetRenderTargets(1, &currentRtv, nullptr);
    g.context->ClearRenderTargetView(g.masterRtv.Get(), clear);
    g.context->OMSetBlendState(g.blendDisabled.Get(), nullptr, 0xffffffff);
    g.context->OMSetDepthStencilState(g.depthDisabled.Get(), 0);
    g.context->RSSetState(g.rasterizer.Get());
    const D3D11_VIEWPORT masterVp{0.0f, 0.0f, static_cast<float>(g.masterWidth),
                                  static_cast<float>(g.masterHeight), 0.0f, 1.0f};
    g.context->RSSetViewports(1, &masterVp);

    const float sx = static_cast<float>(g.masterWidth) / 1920.0f;
    const float sy = static_cast<float>(g.masterHeight) / 1080.0f;

    g.vertices.clear();
    g.indices.clear();

    for (const auto& draw : draws) {
        const uint32_t* sh = draw.regs[Space::Sh];
        const uint32_t* cx = draw.regs[Space::Context];

        if (draw.kind == Draw::Kind::Dispatch) {
            if (sh[0x20C] == 0x40100009u) {
                const BufferDesc src = DecodeBuffer(&sh[0x244]);
                const BufferDesc dst = DecodeBuffer(&sh[0x248]);
                if (src.base && dst.base && dst.records >= 256 && Readable(src.base, 4)) {
                    const uint32_t rgba = *reinterpret_cast<const uint32_t*>(src.base);
                    Renderer::ClearRgba cc{{
                        (rgba & 0xffu) / 255.0f,
                        ((rgba >> 8) & 0xffu) / 255.0f,
                        ((rgba >> 16) & 0xffu) / 255.0f,
                        ((rgba >> 24) & 0xffu) / 255.0f,
                    }};
                    if (dst.base == mainRtAddr) {
                        g.context->ClearRenderTargetView(g.masterRtv.Get(), cc.c);
                    } else {
                        auto rtIt = g.offscreenRts.find(dst.base);
                        if (rtIt != g.offscreenRts.end() && rtIt->second.rtv)
                            g.context->ClearRenderTargetView(rtIt->second.rtv.Get(), cc.c);
                        else
                            g.pendingClears[dst.base] = cc;
                    }
                }
            }
            continue;
        }

        if (sh[0x048] && sh[0x048] != 0x40100010u) continue;
        if (!LoadMatrix(sh)) continue;

        const size_t firstIndex = g.indices.size();
        if (!BuildGeometry(draw, sh)) continue;

        const float xScale = *reinterpret_cast<const float*>(&cx[kCtxViewport + 0]);
        const float xOffset = *reinterpret_cast<const float*>(&cx[kCtxViewport + 1]);
        const float yScale = *reinterpret_cast<const float*>(&cx[kCtxViewport + 2]);
        const float yOffset = *reinterpret_cast<const float*>(&cx[kCtxViewport + 3]);

        const uint64_t rtAddr = static_cast<uint64_t>(cx[kCtxColor0Base]) << 8;
        bool isOffscreen = false;
        ID3D11RenderTargetView* targetRtv = g.masterRtv.Get();
        if (rtAddr && mainRtAddr && rtAddr != mainRtAddr) {
            const uint32_t rtPitch = ((cx[0x319] & 0x7ffu) + 1u) << 3;
            const uint32_t rtSliceH = rtPitch ? (((cx[0x31A] & 0x3fffffu) + 1u) * 64u / rtPitch) : 0;
            uint32_t rw = (xScale > 0.0f) ? static_cast<uint32_t>(xScale * 2.0f + 0.5f) : rtPitch;
            uint32_t rh = (yScale < 0.0f) ? static_cast<uint32_t>(-yScale * 2.0f + 0.5f) : rtSliceH;
            if (rw >= 16 && rh >= 16 && rw <= 4096 && rh <= 4096) {
                if (ID3D11RenderTargetView* off = EnsureOffscreenRt(rtAddr, rw, rh)) {
                    targetRtv = off;
                    isOffscreen = true;
                }
            }
        }
        if (targetRtv != currentRtv) {
            ID3D11ShaderResourceView* nullSrv = nullptr;
            g.context->PSSetShaderResources(0, 1, &nullSrv);
            currentRtv = targetRtv;
            g.context->OMSetRenderTargets(1, &currentRtv, nullptr);
        }

        if (xScale > 0.0f && yScale < 0.0f) {
            const float mulX = isOffscreen ? 1.0f : sx;
            const float mulY = isOffscreen ? 1.0f : sy;
            const D3D11_VIEWPORT dvp{(xOffset - xScale) * mulX, (yOffset + yScale) * mulY,
                                     (xScale * 2.0f) * mulX, (-yScale * 2.0f) * mulY, 0.0f, 1.0f};
            g.context->RSSetViewports(1, &dvp);
        }

        const float blendFactor[4] = {
            *reinterpret_cast<const float*>(&cx[kCtxBlendConstants + 0]),
            *reinterpret_cast<const float*>(&cx[kCtxBlendConstants + 1]),
            *reinterpret_cast<const float*>(&cx[kCtxBlendConstants + 2]),
            *reinterpret_cast<const float*>(&cx[kCtxBlendConstants + 3]),
        };
        g.context->OMSetBlendState(LookupBlend(cx[kCtxBlend0Control], cx[kCtxTargetMask]),
                                   blendFactor, 0xffffffff);

        const ImageDesc texture = DecodeImage(&sh[kPsTexture]);
        ID3D11ShaderResourceView* srv = LookupTexture(texture);
        if (!srv) srv = g.missingTexture.Get();
        ID3D11SamplerState* sampler = LookupSampler(&sh[kPsSampler]);
        if (!sampler) sampler = g.defaultSampler.Get();
        g.context->PSSetShaderResources(0, 1, &srv);
        g.context->PSSetSamplers(0, 1, &sampler);
        UploadAndDraw();
        g.indices.resize(firstIndex);
        g.vertices.clear();
    }

    static uint64_t s_simFrame = 0;
    ++s_simFrame;

    // Decoupled 30 FPS presentation cap: simulation continues at 60.00 Hz while
    // every 2nd frame skips swapchain presentation unless a screenshot is scheduled.
    if (cfg.presentationFpsLimit > 0 && cfg.presentationFpsLimit <= 30 && (s_simFrame & 1u) != 0) {
        MaybeCaptureScreenshot(s_simFrame);
        return;
    }

    CompositeAndPresent(hwnd, s_simFrame, true);
}

void PresentCachedFrame(void* hwndVoid) {
    HWND hwnd = static_cast<HWND>(hwndVoid);
    if (!hwnd || !g.ok || !g.swap || !g.masterSrv) return;
    MSG msg;
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    if (!EnsureRenderer(hwnd)) return;
    CompositeAndPresent(hwnd, 0, false);
}

bool OnWindowMessage(void* hwnd, uint32_t msg, uint64_t wp, int64_t lp) {
    return ui::OnWindowMessage(hwnd, msg, wp, lp);
}

bool IsOverlayCapturingInput() {
    return ui::IsOverlayOpen();
}

void ApplyDisplaySettings(void* hwndVoid) {
    HWND hwnd = static_cast<HWND>(hwndVoid);
    if (!hwnd) return;

    auto& cfg = core::CurrentSettings();
    const auto monitors = core::EnumerateMonitors();
    const int mi = std::clamp(cfg.monitorIndex, 0,
                              std::max(0, static_cast<int>(monitors.size()) - 1));
    const core::MonitorInfo mon = monitors.empty()
                                      ? core::MonitorInfo{0, "Primary", 0, 0, 1920, 1080, true, {}}
                                      : monitors[mi];

    // Always exit exclusive fullscreen before changing window styles or monitor
    if (g.swap) {
        g.backBuffer.Reset();
        g.swap->SetFullscreenState(FALSE, nullptr);
    }

    if (cfg.displayMode == core::DisplayMode::Windowed) {
        const uint32_t w = std::clamp(cfg.displayWidth, 640u, std::max(640u, mon.width));
        const uint32_t h = std::clamp(cfg.displayHeight, 360u, std::max(360u, mon.height));
        const DWORD style = WS_OVERLAPPEDWINDOW | WS_VISIBLE;
        SetWindowLongPtrW(hwnd, GWL_STYLE, style);
        RECT rc{0, 0, static_cast<LONG>(w), static_cast<LONG>(h)};
        AdjustWindowRect(&rc, style, FALSE);
        const int winW = rc.right - rc.left;
        const int winH = rc.bottom - rc.top;
        const int winX = mon.left + std::max(0, (static_cast<int>(mon.width) - winW) / 2);
        const int winY = mon.top + std::max(0, (static_cast<int>(mon.height) - winH) / 2);
        SetWindowPos(hwnd, HWND_NOTOPMOST, winX, winY, winW, winH,
                     SWP_FRAMECHANGED | SWP_SHOWWINDOW);
        LOG_INFO("[display] applied Windowed %ux%u on monitor #%d", w, h, mi);
    } else if (cfg.displayMode == core::DisplayMode::Borderless) {
        SetWindowLongPtrW(hwnd, GWL_STYLE, WS_POPUP | WS_VISIBLE);
        SetWindowPos(hwnd, HWND_TOP, mon.left, mon.top,
                     static_cast<int>(mon.width), static_cast<int>(mon.height),
                     SWP_FRAMECHANGED | SWP_SHOWWINDOW);
        LOG_INFO("[display] applied Borderless Windowed %ux%u on monitor #%d",
                 mon.width, mon.height, mi);
    } else if (cfg.displayMode == core::DisplayMode::ExclusiveFullscreen) {
        SetWindowLongPtrW(hwnd, GWL_STYLE, WS_POPUP | WS_VISIBLE);
        SetWindowPos(hwnd, HWND_TOP, mon.left, mon.top,
                     static_cast<int>(mon.width), static_cast<int>(mon.height),
                     SWP_FRAMECHANGED | SWP_SHOWWINDOW);

        bool enteredExclusive = false;
        if (g.swap) {
            ComPtr<IDXGIOutput> targetOut;
            ComPtr<IDXGIFactory1> fac;
            if (SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&fac)))) {
                int curIdx = 0;
                for (UINT a = 0; !targetOut; ++a) {
                    ComPtr<IDXGIAdapter1> adp;
                    if (fac->EnumAdapters1(a, &adp) == DXGI_ERROR_NOT_FOUND) break;
                    for (UINT o = 0;; ++o) {
                        ComPtr<IDXGIOutput> out;
                        if (adp->EnumOutputs(o, &out) == DXGI_ERROR_NOT_FOUND) break;
                        DXGI_OUTPUT_DESC od{};
                        if (FAILED(out->GetDesc(&od)) || !od.AttachedToDesktop) continue;
                        if (curIdx == mi) { targetOut = out; break; }
                        ++curIdx;
                    }
                }
            }

            DXGI_MODE_DESC md{};
            md.Width = cfg.displayWidth ? cfg.displayWidth : mon.width;
            md.Height = cfg.displayHeight ? cfg.displayHeight : mon.height;
            md.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            g.swap->ResizeTarget(&md);
            if (SUCCEEDED(g.swap->SetFullscreenState(TRUE, targetOut.Get()))) {
                enteredExclusive = true;
                LOG_INFO("[display] entered Exclusive Fullscreen %ux%u on monitor #%d",
                         md.Width, md.Height, mi);
            }
        }
        if (!enteredExclusive) {
            LOG_WARN("[display] DXGI Exclusive Fullscreen unavailable on current session; using Borderless Fullscreen fallback");
            cfg.displayMode = core::DisplayMode::Borderless;
        }
    }

    if (g.swap && g.device) {
        RECT client{};
        GetClientRect(hwnd, &client);
        g.swapWidth = 0;  // force backbuffer recreation
        EnsureSwapchainSize(static_cast<uint32_t>(std::max<LONG>(16, client.right - client.left)),
                            static_cast<uint32_t>(std::max<LONG>(16, client.bottom - client.top)),
                            cfg.swapBuffers);
    }
}

void ShutdownRenderer() {
    ui::ShutdownOverlay();
    if (g.swap) {
        g.swap->SetFullscreenState(FALSE, nullptr);
    }
    for (auto& [k, srv] : g.textures) {
        if (srv) srv->Release();
    }
    g.textures.clear();
    for (auto& [k, smp] : g.samplers) {
        if (smp) smp->Release();
    }
    g.samplers.clear();
    for (auto& [k, bl] : g.blends) {
        if (bl) bl->Release();
    }
    g.blends.clear();
    g.offscreenRts.clear();
    g.pendingClears.clear();
    g.masterRtv.Reset();
    g.masterSrv.Reset();
    g.masterTex.Reset();
    g.backBuffer.Reset();
    g.vb.Reset();
    g.ib.Reset();
    g.cb.Reset();
    g.presentCb.Reset();
    g.presentVs.Reset();
    g.presentPs.Reset();
    g.presentPointSmp.Reset();
    g.presentLinearSmp.Reset();
    g.presentAnisoSmp.Reset();
    g.vs.Reset();
    g.ps.Reset();
    g.layout.Reset();
    g.blendDisabled.Reset();
    g.rasterizer.Reset();
    g.depthDisabled.Reset();
    g.missingTexture.Reset();
    g.defaultSampler.Reset();
    g.swap.Reset();
    g.context.Reset();
    g.device.Reset();
    g.ok = false;
    g.tried = false;
    g.displayInitialized = false;
}

}  // namespace ps4::gnm
