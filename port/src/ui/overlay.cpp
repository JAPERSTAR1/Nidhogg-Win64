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
                    if (activated) {
                        g_ui.editingText = EditTarget::None;
                        g_ui.onlinePage = OnlineSubPage::Home;
                        g_ui.selectedRow = 6;
                    }
                    break;
            }
        }
    } else if (tab == 5) {  // General
        switch (row) {
            case 0: break;  // Language (English only)
            case 1: cfg.showFps = !cfg.showFps; save = true; break;
            case 2: cfg.showFrameGraph = !cfg.showFrameGraph; save = true; break;
            case 3: cfg.showNetStats = !cfg.showNetStats; save = true; break;
            case 4: cfg.launchDirectly = !cfg.launchDirectly; save = true; break;
            case 5:
                if (activated) {
                    core::ResetSettingsToDefaults();
                    RefreshHardwareLists();
                    gnm::ApplyDisplaySettings(hwnd);
                }
                break;
            case 6:
                if (activated) g_ui.open = false;
                break;
            case 7:
                if (activated) ps4::RequestQuit();
                break;
        }
    }

    if (save) core::SaveSettings(cfg);
}

void PollControllerForOverlay(void* hwnd) {
    XINPUT_STATE xi{};
    if (XInputGetState(0, &xi) != ERROR_SUCCESS) return;
    const WORD b = xi.Gamepad.wButtons;
    const WORD pressed = b & ~g_ui.prevPadButtons;
    g_ui.prevPadButtons = b;

    // Select/Back + Start toggles native Settings/Online overlay on controller
    if ((b & XINPUT_GAMEPAD_BACK) && (pressed & XINPUT_GAMEPAD_START)) {
        g_ui.open = !g_ui.open;
        if (g_ui.open) RefreshHardwareLists();
        return;
    }
    if (!g_ui.open || g_ui.capturingBindId >= 0) return;

    if (g_ui.confirmDisplayPending) {
        if (pressed & XINPUT_GAMEPAD_A) {
            g_ui.confirmDisplayPending = false;
        } else if (pressed & XINPUT_GAMEPAD_B) {
            RevertDisplayChanges(hwnd);
        }
        return;
    }

    if (pressed & XINPUT_GAMEPAD_LEFT_SHOULDER) {
        g_ui.tab = (g_ui.tab + 5) % 6;
        g_ui.selectedRow = 0;
    } else if (pressed & XINPUT_GAMEPAD_RIGHT_SHOULDER) {
        g_ui.tab = (g_ui.tab + 1) % 6;
        g_ui.selectedRow = 0;
    } else if (pressed & XINPUT_GAMEPAD_DPAD_UP) {
        g_ui.selectedRow = (g_ui.selectedRow + g_ui.rowCount - 1) % std::max(1, g_ui.rowCount);
    } else if (pressed & XINPUT_GAMEPAD_DPAD_DOWN) {
        g_ui.selectedRow = (g_ui.selectedRow + 1) % std::max(1, g_ui.rowCount);
    } else if (pressed & XINPUT_GAMEPAD_DPAD_LEFT) {
        AdjustSettingRow(hwnd, g_ui.tab, g_ui.selectedRow, -1, false);
    } else if (pressed & XINPUT_GAMEPAD_DPAD_RIGHT) {
        AdjustSettingRow(hwnd, g_ui.tab, g_ui.selectedRow, +1, false);
    } else if (pressed & XINPUT_GAMEPAD_A) {
        AdjustSettingRow(hwnd, g_ui.tab, g_ui.selectedRow, +1, true);
    } else if (pressed & XINPUT_GAMEPAD_B) {
        g_ui.open = false;
    }
}

}  // namespace

void InitOverlay(ID3D11Device* device) {
    if (g_ui.gpuReady || !device) return;

    ComPtr<ID3DBlob> vsb, psb, err;
    if (FAILED(D3DCompile(kUiShader, std::strlen(kUiShader), "ui.hlsl", nullptr, nullptr,
                          "VSMain", "vs_5_0", D3DCOMPILE_ENABLE_STRICTNESS, 0, &vsb, &err)))
        return;
    if (FAILED(D3DCompile(kUiShader, std::strlen(kUiShader), "ui.hlsl", nullptr, nullptr,
                          "PSMain", "ps_5_0", D3DCOMPILE_ENABLE_STRICTNESS, 0, &psb, &err)))
        return;

    device->CreateVertexShader(vsb->GetBufferPointer(), vsb->GetBufferSize(), nullptr, &g_ui.vs);
    device->CreatePixelShader(psb->GetBufferPointer(), psb->GetBufferSize(), nullptr, &g_ui.ps);

    const D3D11_INPUT_ELEMENT_DESC il[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32_FLOAT,       0, 0,  D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT,       0, 8,  D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"COLOR",    0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 16, D3D11_INPUT_PER_VERTEX_DATA, 0},
    };
    device->CreateInputLayout(il, 3, vsb->GetBufferPointer(), vsb->GetBufferSize(), &g_ui.layout);

    D3D11_BUFFER_DESC cbd{};
    cbd.Usage = D3D11_USAGE_DYNAMIC;
    cbd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    cbd.ByteWidth = 16;
    cbd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    device->CreateBuffer(&cbd, nullptr, &g_ui.cb);

    D3D11_SAMPLER_DESC sd{};
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
    sd.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    device->CreateSamplerState(&sd, &g_ui.pointSmp);

    D3D11_BLEND_DESC bd{};
    bd.RenderTarget[0].BlendEnable = TRUE;
    bd.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
    bd.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
    bd.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    bd.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
    bd.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
    bd.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    device->CreateBlendState(&bd, &g_ui.alphaBlend);

    D3D11_RASTERIZER_DESC rd{};
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_NONE;
    rd.DepthClipEnable = TRUE;
    device->CreateRasterizerState(&rd, &g_ui.raster);

    D3D11_DEPTH_STENCIL_DESC dd{};
    dd.DepthEnable = FALSE;
    dd.StencilEnable = FALSE;
    device->CreateDepthStencilState(&dd, &g_ui.depthOff);

    BuildFontAtlas(device);
    g_ui.gpuReady = true;
}

void ShutdownOverlay() {
    g_ui.vb.Reset();
    g_ui.cb.Reset();
    g_ui.vs.Reset();
    g_ui.ps.Reset();
    g_ui.layout.Reset();
    g_ui.atlasSrv.Reset();
    g_ui.pointSmp.Reset();
    g_ui.alphaBlend.Reset();
    g_ui.raster.Reset();
    g_ui.depthOff.Reset();
    g_ui.vbCap = 0;
    g_ui.gpuReady = false;
}

void OpenOverlayTab(int tabIndex) {
    g_ui.open = true;
    g_ui.tab = std::clamp(tabIndex, 0, 5);
    g_ui.selectedRow = 0;
    RefreshHardwareLists();
}

bool IsOverlayOpen() {
    return g_ui.open;
}

bool OnWindowMessage(void* hwnd, uint32_t msg, uint64_t wp, int64_t lp) {
    if (msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN) {
        const int vk = static_cast<int>(wp);

        // Keybinding capture mode
        if (g_ui.open && g_ui.capturingBindId >= 0) {
            if (vk != VK_ESCAPE && vk != VK_F1 && vk != VK_F10) {
                auto& cfg = core::CurrentSettings();
                if (int* slot = KeySlotPtr(cfg, g_ui.capturingBindId)) {
                    *slot = vk;
                    core::SaveSettings(cfg);
                }
            }
            g_ui.capturingBindId = -1;
            return true;
        }

        // Alt+Enter fullscreen toggle
        if (vk == VK_RETURN && (GetKeyState(VK_MENU) & 0x8000)) {
            auto& cfg = core::CurrentSettings();
            if (!g_ui.confirmDisplayPending) g_ui.backupDisplaySettings = cfg;
            cfg.displayMode = (cfg.displayMode == core::DisplayMode::Windowed)
                                  ? core::DisplayMode::Borderless
                                  : core::DisplayMode::Windowed;
            gnm::ApplyDisplaySettings(hwnd);
            core::SaveSettings(cfg);
            return true;
        }

        if (vk == VK_F1 || vk == VK_F10) {
            g_ui.open = !g_ui.open;
            g_ui.editingText = EditTarget::None;
            g_ui.capturingBindId = -1;
            if (g_ui.open) RefreshHardwareLists();
            return true;
        }

        if (!g_ui.open) return false;

        if (g_ui.confirmDisplayPending) {
            if (vk == VK_RETURN) g_ui.confirmDisplayPending = false;
            else if (vk == VK_ESCAPE) RevertDisplayChanges(hwnd);
            return true;
        }

        if (g_ui.editingText != EditTarget::None) {
            auto& cfg = core::CurrentSettings();
            std::string* target = nullptr;
            size_t maxLen = 64;
            if (g_ui.editingText == EditTarget::RoomCode) { target = &g_ui.joinCodeInput; maxLen = 6; }
            else if (g_ui.editingText == EditTarget::PlayerName) { target = &cfg.playerName; maxLen = 24; }
            else if (g_ui.editingText == EditTarget::ServerUrl) { target = &cfg.lobbyServerUrl; maxLen = 96; }

            if (vk == VK_RETURN || vk == VK_ESCAPE) {
                g_ui.editingText = EditTarget::None;
                core::SaveSettings(cfg);
            } else if (vk == VK_BACK && target && !target->empty()) {
                target->pop_back();
            } else if (vk == 'V' && (GetKeyState(VK_CONTROL) & 0x8000) && target) {
                std::string clip = ReadClipboardText();
                for (char c : clip) {
                    if (g_ui.editingText == EditTarget::RoomCode) {
                        if (std::isalnum(static_cast<unsigned char>(c)) && target->size() < maxLen)
                            target->push_back(
                                static_cast<char>(std::toupper(static_cast<unsigned char>(c))));
                    } else if (c >= 32 && c <= 126 && target->size() < maxLen) {
                        target->push_back(c);
                    }
                }
            }
            return true;
        }

        if (vk == VK_ESCAPE) {
            if (g_ui.tab == 4 && g_ui.onlinePage != OnlineSubPage::Home) {
                g_ui.onlinePage = OnlineSubPage::Home;
                g_ui.selectedRow = 0;
            } else {
                g_ui.open = false;
            }
            return true;
        }
        if (vk == VK_TAB) {
            const int delta = (GetKeyState(VK_SHIFT) & 0x8000) ? 5 : 1;
            g_ui.tab = (g_ui.tab + delta) % 6;
            g_ui.selectedRow = 0;
            return true;
        }
        if (vk == VK_UP || vk == 'W') {
            g_ui.selectedRow = (g_ui.selectedRow + g_ui.rowCount - 1) % std::max(1, g_ui.rowCount);
            return true;
        }
        if (vk == VK_DOWN || vk == 'S') {
            g_ui.selectedRow = (g_ui.selectedRow + 1) % std::max(1, g_ui.rowCount);
            return true;
        }
        if (vk == VK_LEFT || vk == 'A') {
            AdjustSettingRow(hwnd, g_ui.tab, g_ui.selectedRow, -1, false);
            return true;
        }
        if (vk == VK_RIGHT || vk == 'D') {
            AdjustSettingRow(hwnd, g_ui.tab, g_ui.selectedRow, +1, false);
            return true;
        }
        if (vk == VK_RETURN || vk == VK_SPACE) {
            AdjustSettingRow(hwnd, g_ui.tab, g_ui.selectedRow, +1, true);
            return true;
        }
        return true;
    }

    if (msg == WM_CHAR && g_ui.open && g_ui.editingText != EditTarget::None) {
        const wchar_t wc = static_cast<wchar_t>(wp);
        if (wc >= 32 && wc <= 126) {
            const char c = static_cast<char>(wc);
            auto& cfg = core::CurrentSettings();
            if (g_ui.editingText == EditTarget::RoomCode) {
                if (std::isalnum(static_cast<unsigned char>(c)) && g_ui.joinCodeInput.size() < 6)
                    g_ui.joinCodeInput.push_back(
                        static_cast<char>(std::toupper(static_cast<unsigned char>(c))));
            } else if (g_ui.editingText == EditTarget::PlayerName) {
                if (cfg.playerName.size() < 24) cfg.playerName.push_back(c);
            } else if (g_ui.editingText == EditTarget::ServerUrl) {
                if (cfg.lobbyServerUrl.size() < 96) cfg.lobbyServerUrl.push_back(c);
            }
        }
        return true;
    }

    if (msg == WM_MOUSEMOVE) {
        g_ui.mouseX = GET_X_LPARAM(lp);
        g_ui.mouseY = GET_Y_LPARAM(lp);
        return g_ui.open;
    }
    if (msg == WM_LBUTTONDOWN) {
        g_ui.mouseX = GET_X_LPARAM(lp);
        g_ui.mouseY = GET_Y_LPARAM(lp);
        if (g_ui.open) {
            g_ui.mouseClicked = true;
            g_ui.mouseDown = true;
            return true;
        }
    }
    if (msg == WM_LBUTTONUP) {
        g_ui.mouseDown = false;
        return g_ui.open;
    }
    return false;
}

void RenderOverlay(void* hwnd, ID3D11Device* device, ID3D11DeviceContext* context,
                   uint32_t targetWidth, uint32_t targetHeight, bool isNewSimFrame) {
    if (!device || !context || !targetWidth || !targetHeight) return;
    InitOverlay(device);
    if (!g_ui.gpuReady) return;

    if (!g_ui.checkedStartupFlags && ps4::SubmittedFrames() >= 5) {
        g_ui.checkedStartupFlags = true;
        char* v = nullptr;
        size_t len = 0;
        if (_dupenv_s(&v, &len, "NIDHOGG_OPEN_ONLINE") == 0 && v && *v == '1') {
            OpenOverlayTab(4);
        }
        if (v) { std::free(v); v = nullptr; }
        if (_dupenv_s(&v, &len, "NIDHOGG_OPEN_SETTINGS") == 0 && v && *v == '1') {
            OpenOverlayTab(0);
        }
        if (v) std::free(v);
    }

    net::TickNetplayLobby();
    PollControllerForOverlay(hwnd);

    // Check 15s display revert timeout
    if (g_ui.confirmDisplayPending && NowSec() >= g_ui.confirmDisplayDeadlineSec) {
        RevertDisplayChanges(hwnd);
    }

    // Update FPS & frame-time diagnostics
    static LARGE_INTEGER s_qpcFreq{};
    if (!s_qpcFreq.QuadPart) QueryPerformanceFrequency(&s_qpcFreq);
    LARGE_INTEGER qpcNow{};
    QueryPerformanceCounter(&qpcNow);
    const uint64_t nowUsec = static_cast<uint64_t>(qpcNow.QuadPart * 1000000ULL / s_qpcFreq.QuadPart);
    if (g_ui.lastPresentUsec != 0 && nowUsec > g_ui.lastPresentUsec) {
        const float dtMs = static_cast<float>(nowUsec - g_ui.lastPresentUsec) / 1000.0f;
        const double instFps = 1000.0 / std::max(0.1f, dtMs);
        g_ui.smoothPresFps = g_ui.smoothPresFps * 0.90 + instFps * 0.10;
        g_ui.frameTimesMs[g_ui.frameTimeCursor] = dtMs;
        g_ui.frameTimeCursor = (g_ui.frameTimeCursor + 1) % g_ui.frameTimesMs.size();
    }
    g_ui.lastPresentUsec = nowUsec;

    if (isNewSimFrame) {
        if (g_ui.lastSimUsec != 0 && nowUsec > g_ui.lastSimUsec) {
            const double dtSimMs = static_cast<double>(nowUsec - g_ui.lastSimUsec) / 1000.0;
            const double instSimHz = 1000.0 / std::max(0.5, dtSimMs);
            g_ui.smoothSimHz = g_ui.smoothSimHz * 0.90 + instSimHz * 0.10;
        }
        g_ui.lastSimUsec = nowUsec;
    }

    g_ui.verts.clear();
    const float W = static_cast<float>(targetWidth);
    const float H = static_cast<float>(targetHeight);
    const float S = std::clamp(std::floor(H / 500.0f + 0.4f), 1.0f, 5.0f);
    const auto& cfg = core::CurrentSettings();
    const auto netSnap = net::GetSnapshot();

    // ---- 1. Diagnostics HUD (FPS / Frame-Time Graph / Netplay Status) ----
    float hudY = 12.0f * S;
    if (cfg.showFps) {
        char fpsBuf[128];
        const uint32_t intW = static_cast<uint32_t>(1920.0f * cfg.renderScale + 0.5f);
        const uint32_t intH = static_cast<uint32_t>(1080.0f * cfg.renderScale + 0.5f);
        std::snprintf(fpsBuf, sizeof(fpsBuf), "PRES %5.1f FPS | SIM %4.1f Hz | INT %ux%u",
                      g_ui.smoothPresFps, g_ui.smoothSimHz, intW, intH);
        const float bw = MeasureTextWidth(S, fpsBuf) + 16.0f * S;
        const float bh = 16.0f * S;
        DrawRect(12.0f * S, hudY, bw, bh, Rgba(12, 14, 20, 200));
        DrawOutline(12.0f * S, hudY, bw, bh, 1.0f * S, kColBorder);
        DrawTextUi(20.0f * S, hudY + 4.0f * S, S, kColGold, fpsBuf);
        hudY += bh + 6.0f * S;
    }

    if (cfg.showFrameGraph) {
        const float gw = 124.0f * 2.0f * S;
        const float gh = 44.0f * S;
        DrawRect(12.0f * S, hudY, gw, gh, Rgba(12, 14, 20, 200));
        DrawOutline(12.0f * S, hudY, gw, gh, 1.0f * S, kColBorder);
        // 16.67 ms target line
        const float y16 = hudY + gh - (16.67f / 33.33f) * (gh - 6.0f * S) - 2.0f * S;
        DrawRect(14.0f * S, y16, gw - 4.0f * S, 1.0f * S, Rgba(72, 214, 122, 120));
        for (size_t i = 0; i < g_ui.frameTimesMs.size(); ++i) {
            const size_t idx = (g_ui.frameTimeCursor + i) % g_ui.frameTimesMs.size();
            const float ms = std::clamp(g_ui.frameTimesMs[idx], 0.0f, 33.33f);
            const float barH = (ms / 33.33f) * (gh - 6.0f * S);
            const Color bc = (ms <= 18.0f) ? kColGreen : (ms <= 26.0f ? kColGold : kColRed);
            DrawRect(14.0f * S + static_cast<float>(i) * 2.0f * S,
                     hudY + gh - 2.0f * S - barH, 1.6f * S, barH, bc);
        }
        hudY += gh + 6.0f * S;
    }

    if (cfg.showNetStats && netSnap.phase != net::SessionPhase::Offline) {
        char netBuf[192];
        const char* tmode =
            (netSnap.transport == net::TransportMode::DirectP2P) ? "P2P-AES" :
            (netSnap.transport == net::TransportMode::RelayFallback) ? "RELAY-AES" : "NEGOTIATING";
        const char* syncTxt = netSnap.desyncDetected ? "DESYNC!" : "SYNC OK";
        std::snprintf(netBuf, sizeof(netBuf), "ONLINE [%s] P%d | RTT %.1fms | D:%df | %s #%llu",
                      tmode, netSnap.localPlayerNum, netSnap.rttMs, netSnap.inputDelayFrames,
                      syncTxt, static_cast<unsigned long long>(netSnap.matchFrame));
        const float bw = MeasureTextWidth(S, netBuf) + 16.0f * S;
        const float bh = 16.0f * S;
        const Color col = netSnap.desyncDetected ? kColRed : kColGreen;
        DrawRect(W - bw - 12.0f * S, 12.0f * S, bw, bh, Rgba(12, 14, 20, 215));
        DrawOutline(W - bw - 12.0f * S, 12.0f * S, bw, bh, 1.0f * S, col);
        DrawTextUi(W - bw - 4.0f * S, 16.0f * S, S, col, netBuf);
    }

    // ---- 2. Full Settings & Online Overlay (F1) ----
    if (g_ui.open) {
        DrawRect(0, 0, W, H, Rgba(6, 8, 14, 175));

        const float pw = std::min(W - 40.0f * S, 460.0f * S);
        const float ph = std::min(H - 36.0f * S, 360.0f * S);
        const float px = std::floor((W - pw) * 0.5f);
        const float py = std::floor((H - ph) * 0.5f);

        DrawRect(px, py, pw, ph, kColPanelBg);
        DrawOutline(px, py, pw, ph, 2.0f * S, kColGold);

        // Title header
        DrawRect(px + 2.0f * S, py + 2.0f * S, pw - 4.0f * S, 24.0f * S, kColHeaderBg);
        DrawTextUi(px + 12.0f * S, py + 9.0f * S, S, kColGold,
                   "NIDHOGG  //  PC SETTINGS & ONLINE MULTIPLAYER");
        const std::string closeHint = "[F1 / ESC]";
        DrawTextUi(px + pw - MeasureTextWidth(S, closeHint) - 12.0f * S, py + 9.0f * S,
                   S, kColMuted, closeHint);

        // Navigation Tabs
        const char* tabNames[6] = {"GRAPHICS", "DISPLAY", "AUDIO", "CONTROLS", "ONLINE", "GENERAL"};
        const float tabY = py + 30.0f * S;
        const float tabW = (pw - 24.0f * S) / 6.0f;
        const float tabH = 20.0f * S;
        for (int t = 0; t < 6; ++t) {
            const float tx = px + 12.0f * S + t * tabW;
            const bool hovered = (g_ui.mouseX >= tx && g_ui.mouseX < tx + tabW - 3.0f * S &&
                                  g_ui.mouseY >= tabY && g_ui.mouseY < tabY + tabH);
            if (hovered && g_ui.mouseClicked && !g_ui.confirmDisplayPending &&
                g_ui.capturingBindId < 0) {
                g_ui.tab = t;
                g_ui.selectedRow = 0;
                g_ui.editingText = EditTarget::None;
            }
            const bool active = (g_ui.tab == t);
            DrawRect(tx, tabY, tabW - 3.0f * S, tabH,
                     active ? kColGold : (hovered ? kColRowHover : kColRowIdle));
            const Color tc = active ? Rgba(14, 16, 22, 255) : kColText;
            const float tw = MeasureTextWidth(S, tabNames[t]);
            DrawTextUi(tx + (tabW - 3.0f * S - tw) * 0.5f, tabY + 6.0f * S, S, tc, tabNames[t]);
        }

        // Build rows for active tab
        struct RowItem {
            std::string label;
            std::string value;
            bool isButton = false;
            Color valColor = kColGold;
        };
        std::vector<RowItem> rows;
        std::string footerNote;

        if (g_ui.tab == 0) {
            const char* qNames[] = {"Low", "Medium", "High", "Ultra", "Custom"};
            const char* fNames[] = {"Pixel Point (Original)", "Sharp Bilinear (Pixel AA)",
                                    "Bilinear Smooth", "Anisotropic"};
            const char* vNames[] = {"Off (Lowest Latency)", "On", "Adaptive"};
            char scaleBuf[64], fpsCapBuf[64], anisoBuf[16];
            std::snprintf(scaleBuf, sizeof(scaleBuf), "%.2fx (%ux%u)", cfg.renderScale,
                          static_cast<uint32_t>(1920 * cfg.renderScale + 0.5f),
                          static_cast<uint32_t>(1080 * cfg.renderScale + 0.5f));
            if (cfg.presentationFpsLimit == 0) std::strcpy(fpsCapBuf, "Unlimited (Sim 60 Hz)");
            else std::snprintf(fpsCapBuf, sizeof(fpsCapBuf), "%d FPS (Sim 60 Hz)", cfg.presentationFpsLimit);
            std::snprintf(anisoBuf, sizeof(anisoBuf), "%dx", cfg.anisotropy);

            rows.push_back({"Quality Preset", qNames[static_cast<int>(cfg.qualityPreset) % 5]});
            rows.push_back({"Internal Render Scale", scaleBuf});
            rows.push_back({"Texture / Shader Filter", fNames[static_cast<int>(cfg.textureFilter) % 4]});
            rows.push_back({"Anisotropic Filtering", anisoBuf});
            rows.push_back({"Scanline Post-FX", cfg.postScanlines ? "Enabled" : "Off"});
            rows.push_back({"Vertical Sync (VSync)", vNames[static_cast<int>(cfg.vsync) % 3]});
            rows.push_back({"Presentation Buffering", cfg.swapBuffers == 3 ? "Triple (3)" : "Double (2)"});
            rows.push_back({"Presentation FPS Limit", fpsCapBuf});
            footerNote = "Original 60.00 Hz physics & combat speed is always preserved.";
        } else if (g_ui.tab == 1) {
            if (g_ui.monitors.empty()) RefreshHardwareLists();
            const char* dmNames[] = {"Windowed", "Borderless Windowed", "Exclusive Fullscreen"};
            const int mi = std::clamp(cfg.monitorIndex, 0,
                                      std::max(0, static_cast<int>(g_ui.monitors.size()) - 1));
            std::string monLabel = g_ui.monitors.empty()
                ? "Primary Display"
                : ("#" + std::to_string(mi) + " " + std::to_string(g_ui.monitors[mi].width) + "x" +
                   std::to_string(g_ui.monitors[mi].height));
            std::string resLabel = std::to_string(cfg.displayWidth) + " x " +
                                   std::to_string(cfg.displayHeight);
            rows.push_back({"Display Mode", dmNames[static_cast<int>(cfg.displayMode) % 3]});
            rows.push_back({"Target Monitor", monLabel});
            rows.push_back({"Display Resolution", resLabel});
            rows.push_back({"Aspect Scaling", cfg.aspectMode == core::AspectMode::IntegerScale
                                                  ? "Integer Pixel Scale"
                                                  : "Preserve 16:9 (Letterbox)"});
            rows.push_back({"[ APPLY DISPLAY SETTINGS ]", "Enter / Click", true, kColGreen});
            footerNote = "Display mode changes auto-revert in 15s unless confirmed. Alt+Enter toggles Fullscreen.";
        } else if (g_ui.tab == 2) {
            if (g_ui.audioDevices.empty()) RefreshHardwareLists();
            auto pct = [](float v) {
                char b[32];
                std::snprintf(b, sizeof(b), "%d%%", static_cast<int>(std::round(v * 100.0f)));
                return std::string(b);
            };
            std::string devName = "Windows Default";
            for (const auto& ad : g_ui.audioDevices)
                if (ad.id == cfg.audioDeviceId) devName = ad.name;
            if (devName.size() > 28) devName.resize(28);
            rows.push_back({"Master Volume", pct(cfg.masterVolume)});
            rows.push_back({"Music Volume (ATRAC9)", pct(cfg.musicVolume)});
            rows.push_back({"Sound Effects Volume", pct(cfg.sfxVolume)});
            rows.push_back({"Mute All Audio", cfg.mute ? "Muted" : "Unmuted",
                            false, cfg.mute ? kColRed : kColGreen});
            rows.push_back({"Audio Output Device", devName});
            footerNote = "Audio levels and output device switch live without restarting.";
        } else if (g_ui.tab == 3) {
            auto padStr = [](int idx) {
                return idx < 0 ? std::string("Disabled") : ("XInput Pad " + std::to_string(idx + 1));
            };
            rows.push_back({"Player 1 Controller", padStr(cfg.p1Controller)});
            rows.push_back({"Player 2 Controller", padStr(cfg.p2Controller)});
            rows.push_back({"Controller Vibration", cfg.vibration ? "Enabled" : "Off"});
            const char* actNames[16] = {
                "P1 Up", "P1 Down", "P1 Left", "P1 Right",
                "P1 Jump / Confirm", "P1 Attack / Throw", "P1 Back", "P1 Start",
                "P2 Up", "P2 Down", "P2 Left", "P2 Right",
                "P2 Jump / Confirm", "P2 Attack / Throw", "P2 Back", "P2 Start",
            };
            auto& mutCfg = core::CurrentSettings();
            for (int i = 0; i < 16; ++i) {
                int* kp = KeySlotPtr(mutCfg, i);
                std::string kn = (g_ui.capturingBindId == i)
                                     ? ">> PRESS KEY <<"
                                     : core::VirtualKeyName(kp ? *kp : 0);
                rows.push_back({actNames[i], kn, true,
                                g_ui.capturingBindId == i ? kColGreen : kColGold});
            }
            rows.push_back({"[ Reset Controls to Defaults ]", "Reset", true, kColMuted});

            const auto conflicts = core::DetectKeyConflicts(cfg);
            if (!conflicts.empty()) {
                footerNote = "CONFLICT: " + conflicts[0].actionA + " & " + conflicts[0].actionB +
                             " both bound to [" + core::VirtualKeyName(conflicts[0].vk) + "]!";
            } else {
                footerNote = "Select any binding and press Enter/Click to remap. Zero key conflicts.";
            }
        } else if (g_ui.tab == 4) {
            if (netSnap.phase == net::SessionPhase::LobbyWaiting ||
                netSnap.phase == net::SessionPhase::ConnectingTransport ||
                netSnap.phase == net::SessionPhase::SyncingMenu ||
                netSnap.phase == net::SessionPhase::InMatch) {
                const std::string visStr = netSnap.isPublic ? "PUBLIC ROOM" : "PRIVATE ROOM";
                const std::string p1Line = netSnap.hostName + (netSnap.isHost
                    ? (netSnap.localReady ? " [READY]" : " [NOT READY]")
                    : (netSnap.remoteReady ? " [READY]" : " [NOT READY]"));
                const std::string p2Line = netSnap.remoteConnected
                    ? (netSnap.guestName + (!netSnap.isHost
                        ? (netSnap.localReady ? " [READY]" : " [NOT READY]")
                        : (netSnap.remoteReady ? " [READY]" : " [NOT READY]")))
                    : "(Waiting for Player 2...)";

                rows.push_back({"Room Code (" + visStr + ")",
                                netSnap.roomCode + "  [Copy]", true, kColGold});
                rows.push_back({"Ready Status (" + std::to_string(netSnap.playerCount) + "/2)  " +
                                    p1Line + " vs " + p2Line,
                                netSnap.localReady ? "READY" : "NOT READY", true,
                                netSnap.localReady ? kColGreen : kColGold});
                rows.push_back({"Switch Room Visibility",
                                netSnap.isHost ? (netSnap.isPublic ? "Make PRIVATE" : "Make PUBLIC")
                                               : "Host Only",
                                true, netSnap.isHost ? kColGold : kColMuted});
                rows.push_back({"[ START ONLINE MATCH ]",
                                netSnap.isHost ? StageName(netSnap.stageIndex) : "Waiting on Host",
                                true, netSnap.isHost ? kColGreen : kColMuted});
                rows.push_back({"Host Kick Challenger",
                                (netSnap.isHost && netSnap.remoteConnected) ? "Kick P2" : "---",
                                true, kColRed});
                rows.push_back({"[ Leave / Close Room ]", "Disconnect", true, kColRed});
                footerNote = netSnap.statusText;
            } else if (g_ui.onlinePage == OnlineSubPage::Home) {
                rows.push_back({g_ui.offerCreateAfterQuickMatch
                                    ? "1. [ Create Public Room Now ]"
                                    : "1. Quick Match",
                                g_ui.offerCreateAfterQuickMatch ? "No open room found" : "Auto-Match",
                                true, kColGreen});
                rows.push_back({"2. Browse Public Rooms", "Server List", true, kColGold});
                rows.push_back({"3. Create Public Room", "Visible in Browser", true, kColGold});
                rows.push_back({"4. Create Private Room", "Hidden (6-Char Code)", true, kColGold});
                rows.push_back({"5. Join Private Room", "Enter Room Code", true, kColGold});
                rows.push_back({"   Host Stage Selection", StageName(g_ui.createStageIndex)});
                rows.push_back({"6. Online Settings", cfg.playerName + " @ " + cfg.region, true, kColMuted});
                footerNote = g_ui.onlineBanner.empty()
                                 ? "Native P2P encrypted UDP lockstep + automatic relay fallback."
                                 : g_ui.onlineBanner;
            } else if (g_ui.onlinePage == OnlineSubPage::Browser) {
                rows.push_back({"[ Refresh Public Rooms ]",
                                std::to_string(g_ui.publicRooms.size()) + " found", true, kColGreen});
                rows.push_back({"Region Filter", g_ui.browserRegion});
                rows.push_back({"Available Rooms Only", g_ui.browserAvailableOnly ? "Yes (1/2)" : "All"});
                rows.push_back({"[ < Back to Online Menu ]", "Esc", true, kColMuted});
                for (const auto& rm : g_ui.publicRooms) {
                    char info[96];
                    std::snprintf(info, sizeof(info), "%s | %d/%d | ~%dms | %s [JOIN]",
                                  rm.region.c_str(), rm.playerCount, rm.maxPlayers,
                                  rm.estimatedPingMs, rm.status.c_str());
                    rows.push_back({rm.roomName, info, true,
                                    rm.playerCount < rm.maxPlayers ? kColGreen : kColMuted});
                }
                footerNote = g_ui.onlineBanner.empty()
                                 ? "Private rooms are never shown in public listings."
                                 : g_ui.onlineBanner;
            } else if (g_ui.onlinePage == OnlineSubPage::JoinCode) {
                const std::string shown = g_ui.joinCodeInput +
                                          (g_ui.editingText == EditTarget::RoomCode ? "_" : "");
                rows.push_back({"6-Char Room Code", shown.empty() ? "(Type Code)" : shown, true, kColGold});
                rows.push_back({"[ Paste Code from Clipboard ]", "Ctrl+V", true, kColMuted});
                rows.push_back({"[ JOIN PRIVATE ROOM ]", "Connect", true, kColGreen});
                rows.push_back({"[ < Back ]", "Esc", true, kColMuted});
                footerNote = g_ui.onlineBanner.empty()
                                 ? "Type the 6-character code shared by the room host."
                                 : g_ui.onlineBanner;
            } else if (g_ui.onlinePage == OnlineSubPage::OnlineSettings) {
                rows.push_back({"Player Name",
                                cfg.playerName + (g_ui.editingText == EditTarget::PlayerName ? "_" : ""),
                                true});
                rows.push_back({"Preferred Region", cfg.region});
                rows.push_back({"Lockstep Input Delay", std::to_string(cfg.inputDelayFrames) + " frames"});
                rows.push_back({"Lobby Server URL",
                                cfg.lobbyServerUrl + (g_ui.editingText == EditTarget::ServerUrl ? "_" : ""),
                                true});
                rows.push_back({"[ < Back to Online Menu ]", "Save & Return", true, kColGreen});
                footerNote = "Click Player Name or Server URL to edit text (Ctrl+V supported).";
            }
        } else if (g_ui.tab == 5) {
            rows.push_back({"Game Language", "English (Original CUSA00920)"});
            rows.push_back({"Show FPS Counter", cfg.showFps ? "Shown" : "Hidden"});
            rows.push_back({"Show Frame-Time Graph", cfg.showFrameGraph ? "Shown" : "Hidden"});
            rows.push_back({"Show Network Statistics", cfg.showNetStats ? "Shown" : "Hidden"});
            rows.push_back({"Startup Mode", cfg.launchDirectly ? "Direct to Game" : "Show Launcher"});
            rows.push_back({"[ Reset All Settings to Defaults ]", "Reset", true, kColRed});
            rows.push_back({"[ Resume Game ]", "F1", true, kColGreen});
            rows.push_back({"[ Quit to Desktop ]", "Clean Exit", true, kColRed});
            footerNote = "Settings saved automatically to " + core::ConfigFilePath();
        }

        g_ui.rowCount = static_cast<int>(rows.size());
        if (g_ui.selectedRow >= g_ui.rowCount) g_ui.selectedRow = std::max(0, g_ui.rowCount - 1);

        // Scrollable row viewport (displays up to 11 rows cleanly)
        const int maxVis = 11;
        int startRow = 0;
        if (g_ui.rowCount > maxVis) {
            startRow = std::clamp(g_ui.selectedRow - maxVis / 2, 0, g_ui.rowCount - maxVis);
        }
        const int endRow = std::min(g_ui.rowCount, startRow + maxVis);

        float ry = tabY + tabH + 10.0f * S;
        const float rowH = 22.0f * S;
        const float rowX = px + 12.0f * S;
        const float rowW = pw - 24.0f * S;

        for (int r = startRow; r < endRow; ++r) {
            const bool hovered = (g_ui.mouseX >= rowX && g_ui.mouseX < rowX + rowW &&
                                  g_ui.mouseY >= ry && g_ui.mouseY < ry + rowH - 2.0f * S);
            if (hovered && !g_ui.confirmDisplayPending && g_ui.capturingBindId < 0) {
                g_ui.selectedRow = r;
                if (g_ui.mouseClicked) {
                    const int clickDir = (!rows[r].isButton && g_ui.mouseX > rowX + rowW * 0.72f)
                                             ? ((g_ui.mouseX > rowX + rowW * 0.86f) ? +1 : -1)
                                             : +1;
                    AdjustSettingRow(hwnd, g_ui.tab, r, clickDir, true);
                }
            }
            const bool sel = (g_ui.selectedRow == r);
            DrawRect(rowX, ry, rowW, rowH - 2.0f * S,
                     sel ? kColRowFocus : (hovered ? kColRowHover : kColRowIdle));
            if (sel) DrawOutline(rowX, ry, rowW, rowH - 2.0f * S, 1.0f * S, kColGoldDim);

            std::string lbl = rows[r].label;
            if (lbl.size() > 34) lbl.resize(34);
            DrawTextUi(rowX + 8.0f * S, ry + 6.0f * S, S, kColText, lbl);

            std::string val = rows[r].isButton ? rows[r].value : ("< " + rows[r].value + " >");
            if (val.size() > 30) val = val.substr(0, 30);
            const float vw = MeasureTextWidth(S, val);
            DrawTextUi(rowX + rowW - vw - 8.0f * S, ry + 6.0f * S, S, rows[r].valColor, val);
            ry += rowH;
        }

        // Footer bar
        DrawRect(px + 2.0f * S, py + ph - 24.0f * S, pw - 4.0f * S, 22.0f * S, kColHeaderBg);
        if (footerNote.size() > 54) footerNote.resize(54);
        const Color footCol = (g_ui.tab == 4 && g_ui.onlineBannerIsError) ? kColRed : kColMuted;
        DrawTextUi(px + 10.0f * S, py + ph - 16.0f * S, S, footCol, footerNote);

        // 15-Second Display Mode Confirmation Modal
        if (g_ui.confirmDisplayPending) {
            DrawRect(px, py, pw, ph, Rgba(6, 8, 14, 220));
            const float mw = 320.0f * S, mh = 110.0f * S;
            const float mx = px + (pw - mw) * 0.5f, my = py + (ph - mh) * 0.5f;
            DrawRect(mx, my, mw, mh, kColHeaderBg);
            DrawOutline(mx, my, mw, mh, 2.0f * S, kColGold);
            DrawTextUi(mx + 14.0f * S, my + 14.0f * S, S, kColGold, "KEEP DISPLAY SETTINGS?");
            char timerMsg[96];
            const int rem = std::max(0, static_cast<int>(std::ceil(g_ui.confirmDisplayDeadlineSec - NowSec())));
            std::snprintf(timerMsg, sizeof(timerMsg), "Reverting automatically in %d seconds...", rem);
            DrawTextUi(mx + 14.0f * S, my + 38.0f * S, S, kColText, timerMsg);
            DrawTextUi(mx + 14.0f * S, my + 62.0f * S, S, kColGreen, "[ENTER] Keep Changes");
            DrawTextUi(mx + 14.0f * S, my + 82.0f * S, S, kColRed,   "[ESC]   Revert Now");
        }
    }

    g_ui.mouseClicked = false;
    if (g_ui.verts.empty()) return;

    // Upload and draw UI batch
    if (!g_ui.vb || g_ui.vbCap < g_ui.verts.size()) {
        g_ui.vb.Reset();
        g_ui.vbCap = static_cast<uint32_t>(g_ui.verts.size() + 1024);
        D3D11_BUFFER_DESC vbd{};
        vbd.Usage = D3D11_USAGE_DYNAMIC;
        vbd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
        vbd.ByteWidth = g_ui.vbCap * sizeof(UiVertex);
        vbd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        if (FAILED(device->CreateBuffer(&vbd, nullptr, &g_ui.vb))) return;
    }

    D3D11_MAPPED_SUBRESOURCE map{};
    if (SUCCEEDED(context->Map(g_ui.vb.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &map))) {
        std::memcpy(map.pData, g_ui.verts.data(), g_ui.verts.size() * sizeof(UiVertex));
        context->Unmap(g_ui.vb.Get(), 0);
    }
    if (SUCCEEDED(context->Map(g_ui.cb.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &map))) {
        const float cbData[4] = {1.0f / W, 1.0f / H, 0.0f, 0.0f};
        std::memcpy(map.pData, cbData, sizeof(cbData));
        context->Unmap(g_ui.cb.Get(), 0);
    }

    const D3D11_VIEWPORT vp{0.0f, 0.0f, W, H, 0.0f, 1.0f};
    context->RSSetViewports(1, &vp);
    context->RSSetState(g_ui.raster.Get());
    context->OMSetDepthStencilState(g_ui.depthOff.Get(), 0);
