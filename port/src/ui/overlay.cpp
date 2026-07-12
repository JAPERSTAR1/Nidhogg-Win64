#define NOMINMAX
#include "overlay.h"

#include "../core/logger.h"
#include "../core/settings.h"
#include "../gnm/d3d11.h"
#include "../hle.h"
#include "../net/netplay.h"

#include <windows.h>
#include <windowsx.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <xinput.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace ps4::ui {
namespace {

// Compact, complete 8x8 bitmap font for ASCII 32..126 (95 glyphs).
// Each glyph is 8 rows of 8 bits (MSB = leftmost pixel).
const uint8_t kFont8x8[95][8] = {
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, // ' '
    {0x18,0x3C,0x3C,0x18,0x18,0x00,0x18,0x00}, // '!'
    {0x66,0x66,0x24,0x00,0x00,0x00,0x00,0x00}, // '"'
    {0x6C,0x6C,0xFE,0x6C,0xFE,0x6C,0x6C,0x00}, // '#'
    {0x18,0x3E,0x60,0x3C,0x06,0x7C,0x18,0x00}, // '$'
    {0x00,0xC6,0xCC,0x18,0x30,0x66,0xC6,0x00}, // '%'
    {0x38,0x6C,0x38,0x76,0xDC,0xCC,0x76,0x00}, // '&'
    {0x18,0x18,0x30,0x00,0x00,0x00,0x00,0x00}, // '\''
    {0x0C,0x18,0x30,0x30,0x30,0x18,0x0C,0x00}, // '('
    {0x30,0x18,0x0C,0x0C,0x0C,0x18,0x30,0x00}, // ')'
    {0x00,0x66,0x3C,0xFF,0x3C,0x66,0x00,0x00}, // '*'
    {0x00,0x18,0x18,0x7E,0x18,0x18,0x00,0x00}, // '+'
    {0x00,0x00,0x00,0x00,0x00,0x18,0x18,0x30}, // ','
    {0x00,0x00,0x00,0x7E,0x00,0x00,0x00,0x00}, // '-'
    {0x00,0x00,0x00,0x00,0x00,0x18,0x18,0x00}, // '.'
    {0x06,0x0C,0x18,0x30,0x60,0xC0,0x80,0x00}, // '/'
    {0x3C,0x66,0x6E,0x76,0x66,0x66,0x3C,0x00}, // '0'
    {0x18,0x38,0x18,0x18,0x18,0x18,0x7E,0x00}, // '1'
    {0x3C,0x66,0x06,0x1C,0x30,0x66,0x7E,0x00}, // '2'
    {0x3C,0x66,0x06,0x1C,0x06,0x66,0x3C,0x00}, // '3'
    {0x0E,0x1E,0x36,0x66,0x7F,0x06,0x06,0x00}, // '4'
    {0x7E,0x60,0x7C,0x06,0x06,0x66,0x3C,0x00}, // '5'
    {0x1C,0x30,0x60,0x7C,0x66,0x66,0x3C,0x00}, // '6'
    {0x7E,0x66,0x06,0x0C,0x18,0x18,0x18,0x00}, // '7'
    {0x3C,0x66,0x66,0x3C,0x66,0x66,0x3C,0x00}, // '8'
    {0x3C,0x66,0x66,0x3E,0x06,0x0C,0x38,0x00}, // '9'
    {0x00,0x18,0x18,0x00,0x00,0x18,0x18,0x00}, // ':'
    {0x00,0x18,0x18,0x00,0x00,0x18,0x18,0x30}, // ';'
    {0x0C,0x18,0x30,0x60,0x30,0x18,0x0C,0x00}, // '<'
    {0x00,0x00,0x7E,0x00,0x00,0x7E,0x00,0x00}, // '='
    {0x30,0x18,0x0C,0x06,0x0C,0x18,0x30,0x00}, // '>'
    {0x3C,0x66,0x06,0x0C,0x18,0x00,0x18,0x00}, // '?'
    {0x3C,0x66,0x6E,0x6E,0x60,0x62,0x3C,0x00}, // '@'
    {0x18,0x3C,0x66,0x66,0x7E,0x66,0x66,0x00}, // 'A'
    {0x7C,0x66,0x66,0x7C,0x66,0x66,0x7C,0x00}, // 'B'
    {0x3C,0x66,0x60,0x60,0x60,0x66,0x3C,0x00}, // 'C'
    {0x78,0x6C,0x66,0x66,0x66,0x6C,0x78,0x00}, // 'D'
    {0x7E,0x60,0x60,0x7C,0x60,0x60,0x7E,0x00}, // 'E'
    {0x7E,0x60,0x60,0x7C,0x60,0x60,0x60,0x00}, // 'F'
    {0x3C,0x66,0x60,0x6E,0x66,0x66,0x3E,0x00}, // 'G'
    {0x66,0x66,0x66,0x7E,0x66,0x66,0x66,0x00}, // 'H'
    {0x3C,0x18,0x18,0x18,0x18,0x18,0x3C,0x00}, // 'I'
    {0x1E,0x0C,0x0C,0x0C,0xCC,0xCC,0x78,0x00}, // 'J'
    {0x66,0x6C,0x78,0x70,0x78,0x6C,0x66,0x00}, // 'K'
    {0x60,0x60,0x60,0x60,0x60,0x60,0x7E,0x00}, // 'L'
    {0xC6,0xEE,0xFE,0xD6,0xC6,0xC6,0xC6,0x00}, // 'M'
    {0x66,0x76,0x7E,0x7E,0x6E,0x66,0x66,0x00}, // 'N'
    {0x3C,0x66,0x66,0x66,0x66,0x66,0x3C,0x00}, // 'O'
    {0x7C,0x66,0x66,0x7C,0x60,0x60,0x60,0x00}, // 'P'
    {0x3C,0x66,0x66,0x66,0x6A,0x6C,0x36,0x00}, // 'Q'
    {0x7C,0x66,0x66,0x7C,0x6C,0x66,0x66,0x00}, // 'R'
    {0x3C,0x66,0x60,0x3C,0x06,0x66,0x3C,0x00}, // 'S'
    {0x7E,0x18,0x18,0x18,0x18,0x18,0x18,0x00}, // 'T'
    {0x66,0x66,0x66,0x66,0x66,0x66,0x3C,0x00}, // 'U'
    {0x66,0x66,0x66,0x66,0x66,0x3C,0x18,0x00}, // 'V'
    {0xC6,0xC6,0xC6,0xD6,0xFE,0xEE,0xC6,0x00}, // 'W'
    {0x66,0x66,0x3C,0x18,0x3C,0x66,0x66,0x00}, // 'X'
    {0x66,0x66,0x66,0x3C,0x18,0x18,0x18,0x00}, // 'Y'
    {0x7E,0x06,0x0C,0x18,0x30,0x60,0x7E,0x00}, // 'Z'
    {0x3C,0x30,0x30,0x30,0x30,0x30,0x3C,0x00}, // '['
    {0xC0,0x60,0x30,0x18,0x0C,0x06,0x02,0x00}, // '\\'
    {0x3C,0x0C,0x0C,0x0C,0x0C,0x0C,0x3C,0x00}, // ']'
    {0x10,0x38,0x6C,0xC6,0x00,0x00,0x00,0x00}, // '^'
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0xFF}, // '_'
    {0x30,0x18,0x0C,0x00,0x00,0x00,0x00,0x00}, // '`'
    {0x00,0x00,0x3C,0x06,0x3E,0x66,0x3E,0x00}, // 'a'
    {0x60,0x60,0x7C,0x66,0x66,0x66,0x7C,0x00}, // 'b'
    {0x00,0x00,0x3C,0x66,0x60,0x66,0x3C,0x00}, // 'c'
    {0x06,0x06,0x3E,0x66,0x66,0x66,0x3E,0x00}, // 'd'
    {0x00,0x00,0x3C,0x66,0x7E,0x60,0x3C,0x00}, // 'e'
    {0x1C,0x36,0x30,0x7C,0x30,0x30,0x30,0x00}, // 'f'
    {0x00,0x00,0x3E,0x66,0x66,0x3E,0x06,0x3C}, // 'g'
    {0x60,0x60,0x7C,0x66,0x66,0x66,0x66,0x00}, // 'h'
    {0x18,0x00,0x38,0x18,0x18,0x18,0x3C,0x00}, // 'i'
    {0x06,0x00,0x0E,0x06,0x06,0x66,0x66,0x3C}, // 'j'
    {0x60,0x60,0x66,0x6C,0x78,0x6C,0x66,0x00}, // 'k'
    {0x38,0x18,0x18,0x18,0x18,0x18,0x3C,0x00}, // 'l'
    {0x00,0x00,0xEC,0xFE,0xD6,0xC6,0xC6,0x00}, // 'm'
    {0x00,0x00,0x7C,0x66,0x66,0x66,0x66,0x00}, // 'n'
    {0x00,0x00,0x3C,0x66,0x66,0x66,0x3C,0x00}, // 'o'
    {0x00,0x00,0x7C,0x66,0x66,0x7C,0x60,0x60}, // 'p'
    {0x00,0x00,0x3E,0x66,0x66,0x3E,0x06,0x06}, // 'q'
    {0x00,0x00,0x7C,0x66,0x60,0x60,0x60,0x00}, // 'r'
    {0x00,0x00,0x3E,0x60,0x3C,0x06,0x7C,0x00}, // 's'
    {0x30,0x30,0x7C,0x30,0x30,0x36,0x1C,0x00}, // 't'
    {0x00,0x00,0x66,0x66,0x66,0x66,0x3E,0x00}, // 'u'
    {0x00,0x00,0x66,0x66,0x66,0x3C,0x18,0x00}, // 'v'
    {0x00,0x00,0xC6,0xC6,0xD6,0xFE,0x6C,0x00}, // 'w'
    {0x00,0x00,0x66,0x3C,0x18,0x3C,0x66,0x00}, // 'x'
    {0x00,0x00,0x66,0x66,0x66,0x3E,0x06,0x3C}, // 'y'
    {0x00,0x00,0x7E,0x0C,0x18,0x30,0x7E,0x00}, // 'z'
    {0x0E,0x18,0x18,0x70,0x18,0x18,0x0E,0x00}, // '{'
    {0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x00}, // '|'
    {0x70,0x18,0x18,0x0E,0x18,0x18,0x70,0x00}, // '}'
    {0x76,0xDC,0x00,0x00,0x00,0x00,0x00,0x00}, // '~'
};

const char* kUiShader = R"(
cbuffer UiViewport : register(b0) {
    float2 InvScreen;
    float2 Pad;
};
struct VsIn {
    float2 pos   : POSITION;
    float2 uv    : TEXCOORD0;
    float4 color : COLOR0;
};
struct VsOut {
    float4 pos   : SV_POSITION;
    float2 uv    : TEXCOORD0;
    float4 color : COLOR0;
};
VsOut VSMain(VsIn i) {
    VsOut o;
    o.pos = float4(i.pos.x * InvScreen.x * 2.0 - 1.0,
                   1.0 - i.pos.y * InvScreen.y * 2.0, 0.0, 1.0);
    o.uv = i.uv;
    o.color = i.color;
    return o;
}
Texture2D Atlas : register(t0);
SamplerState PointSmp : register(s0);
float4 PSMain(VsOut i) : SV_Target {
    return Atlas.Sample(PointSmp, i.uv) * i.color;
}
)";

struct UiVertex {
    float x, y;
    float u, v;
    float r, g, b, a;
};

struct Color {
    float r, g, b, a;
};

constexpr Color Rgba(uint8_t r, uint8_t g, uint8_t b, uint8_t a = 255) {
    return {r / 255.0f, g / 255.0f, b / 255.0f, a / 255.0f};
}

constexpr Color kColPanelBg    = Rgba(16, 18, 26, 238);
constexpr Color kColHeaderBg   = Rgba(25, 28, 40, 255);
constexpr Color kColRowIdle    = Rgba(28, 32, 46, 220);
constexpr Color kColRowHover   = Rgba(46, 52, 74, 245);
constexpr Color kColRowFocus   = Rgba(66, 58, 26, 245);
constexpr Color kColGold       = Rgba(248, 206, 52, 255);
constexpr Color kColGoldDim    = Rgba(185, 150, 38, 255);
constexpr Color kColText       = Rgba(240, 242, 248, 255);
constexpr Color kColMuted      = Rgba(158, 166, 186, 255);
constexpr Color kColGreen      = Rgba(72, 214, 122, 255);
constexpr Color kColRed        = Rgba(238, 82, 83, 255);
constexpr Color kColBorder     = Rgba(74, 82, 110, 255);

enum class OnlineSubPage : int {
    Home = 0,
    Browser = 1,
    JoinCode = 2,
    OnlineSettings = 3,
};

enum class EditTarget : int {
    None = 0,
    RoomCode = 1,
    PlayerName = 2,
    ServerUrl = 3,
};

struct OverlayState {
    bool gpuReady = false;
    ComPtr<ID3D11VertexShader> vs;
    ComPtr<ID3D11PixelShader> ps;
    ComPtr<ID3D11InputLayout> layout;
    ComPtr<ID3D11Buffer> vb;
    ComPtr<ID3D11Buffer> cb;
    ComPtr<ID3D11ShaderResourceView> atlasSrv;
    ComPtr<ID3D11SamplerState> pointSmp;
    ComPtr<ID3D11BlendState> alphaBlend;
    ComPtr<ID3D11RasterizerState> raster;
    ComPtr<ID3D11DepthStencilState> depthOff;
    uint32_t vbCap = 0;
    std::vector<UiVertex> verts;

    // UI state
    bool open = false;
    bool checkedStartupFlags = false;
    int tab = 0;           // 0..5
    int selectedRow = 0;
    int rowCount = 1;
    int mouseX = -1;
    int mouseY = -1;
    bool mouseClicked = false;
    bool mouseDown = false;

    // Hardware lists cached when opening overlay
    std::vector<core::MonitorInfo> monitors;
    std::vector<core::AudioDeviceInfo> audioDevices;
    int selectedResolutionIdx = 2;

    // Display mode confirmation revert timer (Phase 3.6)
    bool confirmDisplayPending = false;
    double confirmDisplayDeadlineSec = 0.0;
    core::Settings backupDisplaySettings;

    // Keybinding capture (Phase 4)
    int capturingBindId = -1;  // -1 = none, 0..15 = P1/P2 key slot

    // Online UI state (Phases 7 & 8)
    OnlineSubPage onlinePage = OnlineSubPage::Home;
    EditTarget editingText = EditTarget::None;
    std::string joinCodeInput;
    std::string browserRegion = "all";
    bool browserAvailableOnly = true;
    int createStageIndex = 0;
    bool offerCreateAfterQuickMatch = false;
    std::vector<net::PublicRoomEntry> publicRooms;
    std::string onlineBanner;
    bool onlineBannerIsError = false;

    // Frame timing diagnostics
    uint64_t lastPresentUsec = 0;
    uint64_t lastSimUsec = 0;
    double smoothPresFps = 60.0;
    double smoothSimHz = 60.0;
    std::array<float, 120> frameTimesMs{};
    size_t frameTimeCursor = 0;

    // Controller edge detection for overlay navigation
    WORD prevPadButtons = 0;
};

OverlayState g_ui;

double NowSec() {
    static LARGE_INTEGER freq{};
    static LARGE_INTEGER start{};
    if (!freq.QuadPart) {
        QueryPerformanceFrequency(&freq);
        QueryPerformanceCounter(&start);
    }
    LARGE_INTEGER now{};
    QueryPerformanceCounter(&now);
    return static_cast<double>(now.QuadPart - start.QuadPart) / static_cast<double>(freq.QuadPart);
}

// Atlas layout: 128x64 texels.
// Cell (0,0) [0..7, 0..7] is solid white (used for solid rectangles).
// Glyphs 0..94 occupy 16 columns x 6 rows starting at cell index 1.
void BuildFontAtlas(ID3D11Device* dev) {
    constexpr uint32_t W = 128, H = 64;
    std::vector<uint32_t> px(W * H, 0u);

    // Solid white block at cell 0 (top-left 8x8)
    for (uint32_t y = 0; y < 8; ++y)
        for (uint32_t x = 0; x < 8; ++x)
            px[y * W + x] = 0xFFFFFFFFu;

    for (int gi = 0; gi < 95; ++gi) {
        const int cell = gi + 1;
        const int cx = (cell % 16) * 8;
        const int cy = (cell / 16) * 8;
        for (int r = 0; r < 8; ++r) {
            const uint8_t bits = kFont8x8[gi][r];
            for (int c = 0; c < 8; ++c) {
                if (bits & (0x80u >> c)) {
                    px[(cy + r) * W + (cx + c)] = 0xFFFFFFFFu;
                }
            }
        }
    }

    D3D11_TEXTURE2D_DESC td{};
    td.Width = W;
    td.Height = H;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_IMMUTABLE;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA sd{px.data(), W * 4, 0};
    ComPtr<ID3D11Texture2D> tex;
    if (SUCCEEDED(dev->CreateTexture2D(&td, &sd, &tex))) {
        dev->CreateShaderResourceView(tex.Get(), nullptr, &g_ui.atlasSrv);
    }
}

void PushQuad(float x, float y, float w, float h, float u0, float v0, float u1, float v1,
              Color c) {
    const float x1 = x + w, y1 = y + h;
    g_ui.verts.push_back({x,  y,  u0, v0, c.r, c.g, c.b, c.a});
    g_ui.verts.push_back({x1, y,  u1, v0, c.r, c.g, c.b, c.a});
    g_ui.verts.push_back({x1, y1, u1, v1, c.r, c.g, c.b, c.a});
    g_ui.verts.push_back({x,  y,  u0, v0, c.r, c.g, c.b, c.a});
    g_ui.verts.push_back({x1, y1, u1, v1, c.r, c.g, c.b, c.a});
    g_ui.verts.push_back({x,  y1, u0, v1, c.r, c.g, c.b, c.a});
}

void DrawRect(float x, float y, float w, float h, Color c) {
    const float u = 2.0f / 128.0f;
    const float v = 2.0f / 64.0f;
    PushQuad(x, y, w, h, u, v, u, v, c);
}

void DrawOutline(float x, float y, float w, float h, float thick, Color c) {
    DrawRect(x, y, w, thick, c);
    DrawRect(x, y + h - thick, w, thick, c);
    DrawRect(x, y, thick, h, c);
    DrawRect(x + w - thick, y, thick, h, c);
}

void DrawTextUi(float x, float y, float scale, Color c, const std::string& text) {
    const float glyphW = 8.0f * scale;
    const float glyphH = 8.0f * scale;
    float cx = x;
    for (unsigned char ch : text) {
        if (ch == '\n') {
            cx = x;
            y += glyphH + 4.0f * scale;
            continue;
        }
        if (ch < 32 || ch > 126) ch = '?';
        const int cell = (ch - 32) + 1;
        const float u0 = ((cell % 16) * 8) / 128.0f;
        const float v0 = ((cell / 16) * 8) / 64.0f;
        const float u1 = u0 + 8.0f / 128.0f;
        const float v1 = v0 + 8.0f / 64.0f;
        PushQuad(cx, y, glyphW, glyphH, u0, v0, u1, v1, c);
        cx += glyphW;
    }
}

float MeasureTextWidth(float scale, const std::string& text) {
    return static_cast<float>(text.size()) * 8.0f * scale;
}

void RefreshHardwareLists() {
    g_ui.monitors = core::EnumerateMonitors();
    g_ui.audioDevices = core::EnumerateAudioDevices();
    const auto& cfg = core::CurrentSettings();
    const int mi = std::clamp(cfg.monitorIndex, 0,
                              std::max(0, static_cast<int>(g_ui.monitors.size()) - 1));
    if (!g_ui.monitors.empty()) {
        const auto& modes = g_ui.monitors[mi].modes;
        for (size_t i = 0; i < modes.size(); ++i) {
            if (modes[i].width == cfg.displayWidth && modes[i].height == cfg.displayHeight) {
                g_ui.selectedResolutionIdx = static_cast<int>(i);
                break;
            }
        }
    }
}

int* KeySlotPtr(core::Settings& s, int bindId) {
    switch (bindId) {
        case 0: return &s.p1Keys.up;
        case 1: return &s.p1Keys.down;
        case 2: return &s.p1Keys.left;
        case 3: return &s.p1Keys.right;
        case 4: return &s.p1Keys.jump;
        case 5: return &s.p1Keys.attack;
        case 6: return &s.p1Keys.back;
        case 7: return &s.p1Keys.start;
        case 8: return &s.p2Keys.up;
        case 9: return &s.p2Keys.down;
        case 10: return &s.p2Keys.left;
        case 11: return &s.p2Keys.right;
        case 12: return &s.p2Keys.jump;
        case 13: return &s.p2Keys.attack;
        case 14: return &s.p2Keys.back;
        case 15: return &s.p2Keys.start;
        default: return nullptr;
    }
}

void TriggerDisplayApply(void* hwnd) {
    auto& cfg = core::CurrentSettings();
    gnm::ApplyDisplaySettings(hwnd);
    core::SaveSettings(cfg);
    g_ui.confirmDisplayPending = true;
    g_ui.confirmDisplayDeadlineSec = NowSec() + 15.0;
}

void RevertDisplayChanges(void* hwnd) {
    auto& cfg = core::CurrentSettings();
    cfg.displayMode = g_ui.backupDisplaySettings.displayMode;
    cfg.displayWidth = g_ui.backupDisplaySettings.displayWidth;
    cfg.displayHeight = g_ui.backupDisplaySettings.displayHeight;
    cfg.monitorIndex = g_ui.backupDisplaySettings.monitorIndex;
    g_ui.confirmDisplayPending = false;
    gnm::ApplyDisplaySettings(hwnd);
    core::SaveSettings(cfg);
    RefreshHardwareLists();
}

std::string ReadClipboardText() {
    std::string out;
    if (!OpenClipboard(nullptr)) return out;
    HANDLE h = GetClipboardData(CF_TEXT);
    if (h) {
        const char* p = static_cast<const char*>(GlobalLock(h));
        if (p) {
            out = p;
            GlobalUnlock(h);
        }
    }
    CloseClipboard();
    return out;
}

const char* StageName(int idx) {
    switch (idx & 3) {
        case 0: return "Castle";
        case 1: return "Mines";
        case 2: return "Clouds";
        case 3: return "Wilds";
        default: return "Castle";
    }
}

void AdjustSettingRow(void* hwnd, int tab, int row, int dir, bool activated) {
    auto& cfg = core::CurrentSettings();
    bool save = false;

    if (tab == 0) {  // Graphics
        switch (row) {
            case 0: {  // Quality Preset
                int q = (static_cast<int>(cfg.qualityPreset) + (dir ? dir : 1) + 5) % 5;
                core::ApplyQualityPreset(cfg, static_cast<core::QualityPreset>(q));
                save = true;
                break;
            }
            case 1: {  // Internal Render Scale
                const float scales[] = {0.50f, 0.75f, 1.00f, 1.25f, 1.50f, 2.00f};
                int cur = 2;
                for (int i = 0; i < 6; ++i)
                    if (std::fabs(cfg.renderScale - scales[i]) < 0.05f) cur = i;
                cur = std::clamp(cur + (dir ? dir : 1), 0, 5);
                cfg.renderScale = scales[cur];
                cfg.qualityPreset = core::QualityPreset::Custom;
                save = true;
                break;
            }
            case 2: {  // Texture / Presentation Filter
                int f = (static_cast<int>(cfg.textureFilter) + (dir ? dir : 1) + 4) % 4;
                cfg.textureFilter = static_cast<core::TextureFilterMode>(f);
                cfg.qualityPreset = core::QualityPreset::Custom;
                save = true;
                break;
            }
            case 3: {  // Anisotropy
                const int levels[] = {1, 2, 4, 8, 16};
                int idx = 0;
                for (int i = 0; i < 5; ++i) if (cfg.anisotropy == levels[i]) idx = i;
                idx = (idx + (dir ? dir : 1) + 5) % 5;
                cfg.anisotropy = levels[idx];
                cfg.qualityPreset = core::QualityPreset::Custom;
                save = true;
                break;
            }
            case 4:  // Scanlines Post-FX
                cfg.postScanlines = !cfg.postScanlines;
                cfg.qualityPreset = core::QualityPreset::Custom;
                save = true;
                break;
            case 5: {  // VSync
                int v = (static_cast<int>(cfg.vsync) + (dir ? dir : 1) + 3) % 3;
                cfg.vsync = static_cast<core::VSyncMode>(v);
                save = true;
                break;
            }
            case 6:  // Swap Buffers
                cfg.swapBuffers = (cfg.swapBuffers == 2) ? 3 : 2;
                save = true;
                break;
            case 7: {  // Presentation FPS Limit
                const int caps[] = {30, 60, 120, 144, 240, 0};
                int idx = 1;
                for (int i = 0; i < 6; ++i) if (cfg.presentationFpsLimit == caps[i]) idx = i;
                idx = (idx + (dir ? dir : 1) + 6) % 6;
                cfg.presentationFpsLimit = caps[idx];
                save = true;
                break;
            }
        }
    } else if (tab == 1) {  // Display
        if (g_ui.monitors.empty()) RefreshHardwareLists();
        const int monCount = std::max(1, static_cast<int>(g_ui.monitors.size()));
        const int mi = std::clamp(cfg.monitorIndex, 0, monCount - 1);
        const auto& modes = g_ui.monitors[mi].modes;
        switch (row) {
            case 0: {  // Display Mode
                if (!g_ui.confirmDisplayPending) g_ui.backupDisplaySettings = cfg;
                int dm = (static_cast<int>(cfg.displayMode) + (dir ? dir : 1) + 3) % 3;
                cfg.displayMode = static_cast<core::DisplayMode>(dm);
                break;
            }
            case 1: {  // Monitor
                if (!g_ui.confirmDisplayPending) g_ui.backupDisplaySettings = cfg;
                cfg.monitorIndex = (cfg.monitorIndex + (dir ? dir : 1) + monCount) % monCount;
                RefreshHardwareLists();
                break;
            }
            case 2: {  // Resolution
                if (!modes.empty()) {
                    if (!g_ui.confirmDisplayPending) g_ui.backupDisplaySettings = cfg;
                    const int n = static_cast<int>(modes.size());
                    g_ui.selectedResolutionIdx =
                        (g_ui.selectedResolutionIdx + (dir ? dir : 1) + n) % n;
                    cfg.displayWidth = modes[g_ui.selectedResolutionIdx].width;
                    cfg.displayHeight = modes[g_ui.selectedResolutionIdx].height;
                }
                break;
            }
            case 3: {  // Aspect Ratio Scaling
                cfg.aspectMode = (cfg.aspectMode == core::AspectMode::Preserve16x9)
                                     ? core::AspectMode::IntegerScale
                                     : core::AspectMode::Preserve16x9;
                save = true;
                break;
            }
            case 4: {  // Apply Display Settings button
                if (activated) {
                    TriggerDisplayApply(hwnd);
                }
                break;
            }
        }
    } else if (tab == 2) {  // Audio
        if (g_ui.audioDevices.empty()) RefreshHardwareLists();
        const float step = 0.05f * (dir ? static_cast<float>(dir) : 1.0f);
        switch (row) {
            case 0: cfg.masterVolume = std::clamp(cfg.masterVolume + step, 0.0f, 1.0f); save = true; break;
            case 1: cfg.musicVolume  = std::clamp(cfg.musicVolume  + step, 0.0f, 1.0f); save = true; break;
            case 2: cfg.sfxVolume    = std::clamp(cfg.sfxVolume    + step, 0.0f, 1.0f); save = true; break;
            case 3: cfg.mute = !cfg.mute; save = true; break;
            case 4: {
                const int n = static_cast<int>(g_ui.audioDevices.size());
                if (n > 0) {
                    int cur = 0;
                    for (int i = 0; i < n; ++i)
                        if (g_ui.audioDevices[i].id == cfg.audioDeviceId) cur = i;
                    cur = (cur + (dir ? dir : 1) + n) % n;
                    cfg.audioDeviceId = g_ui.audioDevices[cur].id;
                    save = true;
                }
                break;
            }
        }
    } else if (tab == 3) {  // Controls
        if (row == 0) {
            cfg.p1Controller = ((cfg.p1Controller + 1) + (dir ? dir : 1) + 5) % 5 - 1;
            save = true;
        } else if (row == 1) {
            cfg.p2Controller = ((cfg.p2Controller + 1) + (dir ? dir : 1) + 5) % 5 - 1;
            save = true;
        } else if (row == 2) {
            cfg.vibration = !cfg.vibration;
            save = true;
        } else if (row >= 3 && row <= 18 && activated) {
            g_ui.capturingBindId = row - 3;
        } else if (row == 19 && activated) {
            const auto def = core::MakeDefaultSettings();
            cfg.p1Keys = def.p1Keys;
            cfg.p2Keys = def.p2Keys;
            cfg.p1Controller = def.p1Controller;
            cfg.p2Controller = def.p2Controller;
            save = true;
        }
    } else if (tab == 4) {  // Online
        const auto snap = net::GetSnapshot();
        std::string err;
        if (snap.phase == net::SessionPhase::LobbyWaiting ||
            snap.phase == net::SessionPhase::ConnectingTransport ||
            snap.phase == net::SessionPhase::SyncingMenu ||
            snap.phase == net::SessionPhase::InMatch) {
            // Active Room Lobby controls
            switch (row) {
                case 0:
                    if (activated && net::CopyRoomCodeToClipboard()) {
                        g_ui.onlineBanner = "Copied Room Code (" + snap.roomCode + ") to clipboard.";
                        g_ui.onlineBannerIsError = false;
                    }
                    break;
                case 1:
                    if (activated) {
                        if (!net::SetReady(!snap.localReady, err)) {
                            g_ui.onlineBanner = err;
                            g_ui.onlineBannerIsError = true;
                        }
                    }
                    break;
                case 2:
                    if (activated && snap.isHost) {
                        if (net::SetRoomVisibility(!snap.isPublic, err)) {
                            g_ui.onlineBanner = !snap.isPublic
                                ? "Switched room to PUBLIC."
                                : "Switched room to PRIVATE (hidden from browser).";
                            g_ui.onlineBannerIsError = false;
                        } else {
                            g_ui.onlineBanner = err;
                            g_ui.onlineBannerIsError = true;
                        }
                    }
                    break;
                case 3:
                    if (activated && snap.isHost) {
                        if (net::StartMatch(err)) {
                            g_ui.open = false;  // Close overlay as synchronized match begins!
                        } else {
                            g_ui.onlineBanner = err;
                            g_ui.onlineBannerIsError = true;
                        }
                    }
                    break;
                case 4:
                    if (activated && snap.isHost && snap.remoteConnected) {
                        if (!net::KickGuest(err)) {
                            g_ui.onlineBanner = err;
                            g_ui.onlineBannerIsError = true;
                        }
                    }
                    break;
                case 5:
                    if (activated) {
                        net::LeaveRoom();
                        g_ui.onlineBanner = "Left online room.";
                        g_ui.onlineBannerIsError = false;
                    }
                    break;
            }
        } else if (g_ui.onlinePage == OnlineSubPage::Home) {
            switch (row) {
                case 0:  // Quick Match (or Create Public Room after empty Quick Match)
                    if (activated) {
                        if (g_ui.offerCreateAfterQuickMatch) {
                            g_ui.offerCreateAfterQuickMatch = false;
                            if (!net::CreateRoom("", true, g_ui.createStageIndex, err)) {
                                g_ui.onlineBanner = err;
                                g_ui.onlineBannerIsError = true;
                            }
                        } else {
                            bool noRooms = false;
                            if (!net::QuickMatch(cfg.region, noRooms, err)) {
                                g_ui.offerCreateAfterQuickMatch = noRooms;
                                g_ui.onlineBanner = err;
                                g_ui.onlineBannerIsError = !noRooms;
                            }
                        }
                    }
                    break;
                case 1:  // Browse Public Rooms
                    if (activated) {
                        g_ui.onlinePage = OnlineSubPage::Browser;
                        g_ui.selectedRow = 0;
                        if (!net::RefreshPublicRooms(g_ui.browserRegion, g_ui.browserAvailableOnly,
                                                     g_ui.publicRooms, err)) {
                            g_ui.onlineBanner = err;
                            g_ui.onlineBannerIsError = true;
                        } else {
                            g_ui.onlineBanner = "Loaded " + std::to_string(g_ui.publicRooms.size()) +
                                                " public room(s).";
                            g_ui.onlineBannerIsError = false;
                        }
                    }
                    break;
                case 2:  // Create Public Room
                    if (activated) {
                        if (!net::CreateRoom("", true, g_ui.createStageIndex, err)) {
                            g_ui.onlineBanner = err;
                            g_ui.onlineBannerIsError = true;
                        }
                    }
                    break;
                case 3:  // Create Private Room
                    if (activated) {
                        if (!net::CreateRoom("", false, g_ui.createStageIndex, err)) {
                            g_ui.onlineBanner = err;
                            g_ui.onlineBannerIsError = true;
                        }
                    }
                    break;
                case 4:  // Join Private Room by Code
                    if (activated) {
                        g_ui.onlinePage = OnlineSubPage::JoinCode;
                        g_ui.editingText = EditTarget::RoomCode;
                        g_ui.selectedRow = 0;
                    }
                    break;
                case 5:  // Preferred Stage
                    g_ui.createStageIndex = (g_ui.createStageIndex + (dir ? dir : 1) + 4) % 4;
                    break;
                case 6:  // Online Settings
                    if (activated) {
                        g_ui.onlinePage = OnlineSubPage::OnlineSettings;
                        g_ui.selectedRow = 0;
                    }
                    break;
            }
        } else if (g_ui.onlinePage == OnlineSubPage::Browser) {
            if (row == 0 && activated) {
                if (!net::RefreshPublicRooms(g_ui.browserRegion, g_ui.browserAvailableOnly,
                                             g_ui.publicRooms, err)) {
                    g_ui.onlineBanner = err;
                    g_ui.onlineBannerIsError = true;
                }
            } else if (row == 1) {
                const char* regs[] = {"all", "us-east", "us-west", "eu-west", "ap-east"};
                int idx = 0;
                for (int i = 0; i < 5; ++i) if (g_ui.browserRegion == regs[i]) idx = i;
                idx = (idx + (dir ? dir : 1) + 5) % 5;
                g_ui.browserRegion = regs[idx];
                net::RefreshPublicRooms(g_ui.browserRegion, g_ui.browserAvailableOnly,
                                        g_ui.publicRooms, err);
            } else if (row == 2) {
                g_ui.browserAvailableOnly = !g_ui.browserAvailableOnly;
                net::RefreshPublicRooms(g_ui.browserRegion, g_ui.browserAvailableOnly,
                                        g_ui.publicRooms, err);
            } else if (row == 3 && activated) {
                g_ui.onlinePage = OnlineSubPage::Home;
                g_ui.selectedRow = 1;
            } else if (row >= 4 && activated) {
                const size_t rIdx = static_cast<size_t>(row - 4);
                if (rIdx < g_ui.publicRooms.size()) {
                    if (!net::JoinRoomById(g_ui.publicRooms[rIdx].roomId, err)) {
                        g_ui.onlineBanner = err;
                        g_ui.onlineBannerIsError = true;
                    } else {
                        g_ui.onlinePage = OnlineSubPage::Home;
                    }
                }
            }
        } else if (g_ui.onlinePage == OnlineSubPage::JoinCode) {
            if (row == 0 && activated) {
                g_ui.editingText = EditTarget::RoomCode;
            } else if (row == 1 && activated) {
                std::string clip = ReadClipboardText();
                g_ui.joinCodeInput.clear();
                for (char c : clip) {
                    if (std::isalnum(static_cast<unsigned char>(c)) && g_ui.joinCodeInput.size() < 6)
                        g_ui.joinCodeInput.push_back(
                            static_cast<char>(std::toupper(static_cast<unsigned char>(c))));
                }
            } else if (row == 2 && activated) {
                g_ui.editingText = EditTarget::None;
                if (!net::JoinRoomByCode(g_ui.joinCodeInput, err)) {
                    g_ui.onlineBanner = err;
                    g_ui.onlineBannerIsError = true;
                } else {
                    g_ui.onlinePage = OnlineSubPage::Home;
                }
            } else if (row == 3 && activated) {
                g_ui.editingText = EditTarget::None;
                g_ui.onlinePage = OnlineSubPage::Home;
                g_ui.selectedRow = 4;
            }
        } else if (g_ui.onlinePage == OnlineSubPage::OnlineSettings) {
            switch (row) {
                case 0:
                    if (activated) g_ui.editingText = (g_ui.editingText == EditTarget::PlayerName)
                                                          ? EditTarget::None
                                                          : EditTarget::PlayerName;
                    break;
                case 1: {
                    const char* regs[] = {"us-east", "us-west", "eu-west", "ap-east"};
                    int idx = 0;
                    for (int i = 0; i < 4; ++i) if (cfg.region == regs[i]) idx = i;
                    idx = (idx + (dir ? dir : 1) + 4) % 4;
                    cfg.region = regs[idx];
                    save = true;
                    break;
                }
                case 2:
                    cfg.inputDelayFrames = std::clamp(cfg.inputDelayFrames + (dir ? dir : 1), 1, 8);
                    save = true;
                    break;
                case 3:
                    if (activated) g_ui.editingText = (g_ui.editingText == EditTarget::ServerUrl)
                                                          ? EditTarget::None
                                                          : EditTarget::ServerUrl;
                    break;
                case 4:
