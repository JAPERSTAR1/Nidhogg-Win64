#define NOMINMAX
#include "settings.h"
#include "logger.h"

#include <windows.h>
#include <dxgi.h>
#include <mmsystem.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <wrl/client.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <sstream>

namespace ps4::core {
namespace {

std::mutex g_settingsMtx;
Settings g_current = MakeDefaultSettings();
std::string g_userDirOverride;
std::string g_cachedUserDir;

bool IsDirectoryWritable(const std::string& dir) {
    std::string probe = dir + "\\.nidhogg_write_probe_" + std::to_string(GetCurrentProcessId());
    std::wstring w = Utf8ToWide(probe);
    HANDLE h = CreateFileW(w.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    CloseHandle(h);
    return true;
}

std::string JsonEscape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (unsigned char c : s) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                    out += buf;
                } else {
                    out.push_back(static_cast<char>(c));
                }
                break;
        }
    }
    return out;
}

// Minimal, strict key/value extractor over flat/nested JSON objects produced by
// SaveSettings or hand-edited by the user.
bool JsonFindRaw(const std::string& json, const char* key, std::string& outVal) {
    std::string needle = std::string("\"") + key + "\"";
    size_t pos = json.find(needle);
    if (pos == std::string::npos) return false;
    pos = json.find(':', pos + needle.size());
    if (pos == std::string::npos) return false;
    ++pos;
    while (pos < json.size() && std::isspace(static_cast<unsigned char>(json[pos]))) ++pos;
    if (pos >= json.size()) return false;

    if (json[pos] == '"') {
        ++pos;
        std::string res;
        while (pos < json.size()) {
            char c = json[pos++];
            if (c == '"') {
                outVal = res;
                return true;
            }
            if (c == '\\' && pos < json.size()) {
                char esc = json[pos++];
                switch (esc) {
                    case '"': res.push_back('"'); break;
                    case '\\': res.push_back('\\'); break;
                    case '/': res.push_back('/'); break;
                    case 'n': res.push_back('\n'); break;
                    case 'r': res.push_back('\r'); break;
                    case 't': res.push_back('\t'); break;
                    default: res.push_back(esc); break;
                }
            } else {
                res.push_back(c);
            }
        }
        return false;
    }

    size_t start = pos;
    while (pos < json.size() && json[pos] != ',' && json[pos] != '}' && json[pos] != ']' &&
           !std::isspace(static_cast<unsigned char>(json[pos]))) {
        ++pos;
    }
    outVal = json.substr(start, pos - start);
    return !outVal.empty();
}

void JsonReadInt(const std::string& j, const char* k, int& dst, int lo, int hi) {
    std::string v;
    if (!JsonFindRaw(j, k, v)) return;
    char* end = nullptr;
    long parsed = std::strtol(v.c_str(), &end, 10);
    if (end != v.c_str()) dst = std::clamp(static_cast<int>(parsed), lo, hi);
}

void JsonReadU32(const std::string& j, const char* k, uint32_t& dst, uint32_t lo, uint32_t hi) {
    std::string v;
    if (!JsonFindRaw(j, k, v)) return;
    char* end = nullptr;
    unsigned long parsed = std::strtoul(v.c_str(), &end, 10);
    if (end != v.c_str()) dst = std::clamp(static_cast<uint32_t>(parsed), lo, hi);
}

void JsonReadFloat(const std::string& j, const char* k, float& dst, float lo, float hi) {
    std::string v;
    if (!JsonFindRaw(j, k, v)) return;
    char* end = nullptr;
    float parsed = std::strtof(v.c_str(), &end);
    if (end != v.c_str() && std::isfinite(parsed)) dst = std::clamp(parsed, lo, hi);
}

void JsonReadBool(const std::string& j, const char* k, bool& dst) {
    std::string v;
    if (!JsonFindRaw(j, k, v)) return;
    if (v == "true" || v == "1") dst = true;
    else if (v == "false" || v == "0") dst = false;
}

void JsonReadStr(const std::string& j, const char* k, std::string& dst) {
    std::string v;
    if (JsonFindRaw(j, k, v)) dst = v;
}

}  // namespace

std::wstring Utf8ToWide(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    if (n <= 0) return {};
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

std::string WideToUtf8(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0,
                                nullptr, nullptr);
    if (n <= 0) return {};
    std::string s(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), s.data(), n, nullptr,
                        nullptr);
    return s;
}

std::FILE* FopenUtf8(const std::string& path, const char* mode) {
    std::wstring wp = Utf8ToWide(path);
    std::wstring wm = Utf8ToWide(mode ? mode : "rb");
    if (wp.empty() || wm.empty()) return nullptr;
    return _wfopen(wp.c_str(), wm.c_str());
}

bool FileExistsUtf8(const std::string& path) {
    std::wstring w = Utf8ToWide(path);
    if (w.empty()) return false;
    DWORD a = GetFileAttributesW(w.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

bool DirExistsUtf8(const std::string& path) {
    std::wstring w = Utf8ToWide(path);
    if (w.empty()) return false;
    DWORD a = GetFileAttributesW(w.c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

bool MakeDirsUtf8(const std::string& dirPath) {
    if (dirPath.empty()) return false;
    std::string norm = dirPath;
    for (char& c : norm) {
        if (c == '/') c = '\\';
    }
    for (size_t i = 1; i <= norm.size(); ++i) {
        if (i == norm.size() || norm[i] == '\\') {
            std::string sub = norm.substr(0, i);
            if (sub.size() == 2 && sub[1] == ':') continue;
            if (!DirExistsUtf8(sub)) {
                std::wstring w = Utf8ToWide(sub);
                CreateDirectoryW(w.c_str(), nullptr);
            }
        }
    }
    return DirExistsUtf8(norm);
}

std::string ExeDir() {
    wchar_t buf[MAX_PATH * 4]{};
    DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH * 4);
    if (!n) return ".";
    std::wstring w(buf, n);
    size_t slash = w.find_last_of(L"\\/");
    if (slash != std::wstring::npos) w.resize(slash);
    return WideToUtf8(w);
}

void SetUserDirOverride(const std::string& dir) {
    std::scoped_lock lk(g_settingsMtx);
    g_userDirOverride = dir;
    g_cachedUserDir.clear();
}

const std::string& UserDir() {
    std::scoped_lock lk(g_settingsMtx);
    if (!g_cachedUserDir.empty()) return g_cachedUserDir;

    if (!g_userDirOverride.empty()) {
        g_cachedUserDir = g_userDirOverride;
    } else {
        char* env = nullptr;
        size_t len = 0;
        if (_dupenv_s(&env, &len, "NIDHOGG_USER_DIR") == 0 && env && *env) {
            g_cachedUserDir = env;
            std::free(env);
        } else {
            if (env) std::free(env);
            const std::string exe = ExeDir();
            // Portable mode when portable.txt or a local config/ folder sits next
            // to the executable AND that directory is writable.  Protected paths
            // like C:\Program Files fall back cleanly to %LOCALAPPDATA%\Nidhogg.
            const bool wantPortable =
                FileExistsUtf8(exe + "\\portable.txt") || DirExistsUtf8(exe + "\\config");
            if (wantPortable && IsDirectoryWritable(exe)) {
                g_cachedUserDir = exe;
            } else {
                PWSTR appData = nullptr;
                if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &appData)) &&
                    appData) {
                    g_cachedUserDir = WideToUtf8(appData) + "\\Nidhogg";
                    CoTaskMemFree(appData);
                } else {
                    g_cachedUserDir = exe;
                }
            }
        }
    }

    MakeDirsUtf8(g_cachedUserDir + "\\config");
    MakeDirsUtf8(g_cachedUserDir + "\\saves");
    MakeDirsUtf8(g_cachedUserDir + "\\logs");
    return g_cachedUserDir;
}

std::string ConfigFilePath() { return UserDir() + "\\config\\settings.json"; }
std::string SavesDirPath() { return UserDir() + "\\saves"; }
std::string LogsDirPath() { return UserDir() + "\\logs"; }

Settings MakeDefaultSettings() {
    Settings s;
    s.p1Keys.up = 'W';
    s.p1Keys.down = 'S';
    s.p1Keys.left = 'A';
    s.p1Keys.right = 'D';
    s.p1Keys.jump = VK_SPACE;
    s.p1Keys.attack = 'F';
    s.p1Keys.back = VK_ESCAPE;
    s.p1Keys.start = VK_RETURN;
    s.p1Keys.altUp = VK_UP;
    s.p1Keys.altDown = VK_DOWN;
    s.p1Keys.altLeft = VK_LEFT;
    s.p1Keys.altRight = VK_RIGHT;
    s.p1Keys.altJump = 'Z';
    s.p1Keys.altAttack = 'X';

    s.p2Keys.up = 'I';
    s.p2Keys.down = 'K';
    s.p2Keys.left = 'J';
    s.p2Keys.right = 'L';
    s.p2Keys.jump = 'O';
    s.p2Keys.attack = 'U';
    s.p2Keys.back = VK_BACK;
    s.p2Keys.start = VK_RSHIFT;
    s.p2Keys.altJump = VK_RCONTROL;
    return s;
}

void ApplyQualityPreset(Settings& s, QualityPreset preset) {
    s.qualityPreset = preset;
    switch (preset) {
        case QualityPreset::Low:
            s.renderScale = 0.5f;
            s.textureFilter = TextureFilterMode::PixelPoint;
            s.anisotropy = 1;
            s.postScanlines = false;
            s.swapBuffers = 2;
            break;
        case QualityPreset::Medium:
            s.renderScale = 1.0f;
            s.textureFilter = TextureFilterMode::PixelPoint;
            s.anisotropy = 1;
            s.postScanlines = false;
            s.swapBuffers = 2;
            break;
        case QualityPreset::High:
            s.renderScale = 1.0f;
            s.textureFilter = TextureFilterMode::PixelPoint;
            s.anisotropy = 4;
            s.postScanlines = false;
            s.swapBuffers = 2;
            break;
        case QualityPreset::Ultra:
            s.renderScale = 2.0f;
            s.textureFilter = TextureFilterMode::SharpBilinear;
            s.anisotropy = 16;
            s.postScanlines = false;
            s.swapBuffers = 2;
            break;
        case QualityPreset::Custom:
            break;
    }
}

Settings& CurrentSettings() { return g_current; }

bool LoadSettings(Settings& out, const std::string& path) {
    const std::string target = path.empty() ? ConfigFilePath() : path;
    std::FILE* f = FopenUtf8(target, "rb");
    if (!f) {
        out = MakeDefaultSettings();
        return false;
    }
    std::fseek(f, 0, SEEK_END);
    const long sz = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    if (sz <= 0 || sz > (1 << 20)) {
        std::fclose(f);
        out = MakeDefaultSettings();
        return false;
    }
    std::string j(static_cast<size_t>(sz), '\0');
    std::fread(j.data(), 1, j.size(), f);
    std::fclose(f);

    Settings s = MakeDefaultSettings();
    JsonReadStr(j, "assetPath", s.assetPath);

    int dm = static_cast<int>(s.displayMode);
    JsonReadInt(j, "displayMode", dm, 0, 2);
    s.displayMode = static_cast<DisplayMode>(dm);
    JsonReadU32(j, "displayWidth", s.displayWidth, 640, 7680);
    JsonReadU32(j, "displayHeight", s.displayHeight, 360, 4320);
    JsonReadInt(j, "monitorIndex", s.monitorIndex, 0, 15);
    int am = static_cast<int>(s.aspectMode);
    JsonReadInt(j, "aspectMode", am, 0, 1);
    s.aspectMode = static_cast<AspectMode>(am);

    int qp = static_cast<int>(s.qualityPreset);
    JsonReadInt(j, "qualityPreset", qp, 0, 4);
    s.qualityPreset = static_cast<QualityPreset>(qp);
    JsonReadFloat(j, "renderScale", s.renderScale, 0.5f, 2.0f);
    int tf = static_cast<int>(s.textureFilter);
    JsonReadInt(j, "textureFilter", tf, 0, 3);
    s.textureFilter = static_cast<TextureFilterMode>(tf);
    JsonReadInt(j, "anisotropy", s.anisotropy, 1, 16);
    JsonReadBool(j, "postScanlines", s.postScanlines);
    int vs = static_cast<int>(s.vsync);
    JsonReadInt(j, "vsync", vs, 0, 2);
    s.vsync = static_cast<VSyncMode>(vs);
    JsonReadInt(j, "swapBuffers", s.swapBuffers, 2, 3);
    JsonReadInt(j, "presentationFpsLimit", s.presentationFpsLimit, 0, 1000);

    JsonReadFloat(j, "masterVolume", s.masterVolume, 0.0f, 1.0f);
    JsonReadFloat(j, "musicVolume", s.musicVolume, 0.0f, 1.0f);
    JsonReadFloat(j, "sfxVolume", s.sfxVolume, 0.0f, 1.0f);
    JsonReadBool(j, "mute", s.mute);
    JsonReadInt(j, "audioDeviceId", s.audioDeviceId, -1, 64);

    JsonReadInt(j, "p1_up", s.p1Keys.up, 0, 255);
    JsonReadInt(j, "p1_down", s.p1Keys.down, 0, 255);
    JsonReadInt(j, "p1_left", s.p1Keys.left, 0, 255);
    JsonReadInt(j, "p1_right", s.p1Keys.right, 0, 255);
    JsonReadInt(j, "p1_jump", s.p1Keys.jump, 0, 255);
    JsonReadInt(j, "p1_attack", s.p1Keys.attack, 0, 255);
    JsonReadInt(j, "p1_back", s.p1Keys.back, 0, 255);
    JsonReadInt(j, "p1_start", s.p1Keys.start, 0, 255);
    JsonReadInt(j, "p1_altUp", s.p1Keys.altUp, 0, 255);
    JsonReadInt(j, "p1_altDown", s.p1Keys.altDown, 0, 255);
    JsonReadInt(j, "p1_altLeft", s.p1Keys.altLeft, 0, 255);
    JsonReadInt(j, "p1_altRight", s.p1Keys.altRight, 0, 255);
    JsonReadInt(j, "p1_altJump", s.p1Keys.altJump, 0, 255);
    JsonReadInt(j, "p1_altAttack", s.p1Keys.altAttack, 0, 255);

    JsonReadInt(j, "p2_up", s.p2Keys.up, 0, 255);
    JsonReadInt(j, "p2_down", s.p2Keys.down, 0, 255);
    JsonReadInt(j, "p2_left", s.p2Keys.left, 0, 255);
    JsonReadInt(j, "p2_right", s.p2Keys.right, 0, 255);
    JsonReadInt(j, "p2_jump", s.p2Keys.jump, 0, 255);
    JsonReadInt(j, "p2_attack", s.p2Keys.attack, 0, 255);
    JsonReadInt(j, "p2_back", s.p2Keys.back, 0, 255);
    JsonReadInt(j, "p2_start", s.p2Keys.start, 0, 255);
    JsonReadInt(j, "p2_altJump", s.p2Keys.altJump, 0, 255);

    JsonReadInt(j, "p1Controller", s.p1Controller, -1, 3);
    JsonReadInt(j, "p2Controller", s.p2Controller, -1, 3);
    JsonReadBool(j, "vibration", s.vibration);

    JsonReadStr(j, "playerName", s.playerName);
    JsonReadStr(j, "region", s.region);
    JsonReadStr(j, "lobbyServerUrl", s.lobbyServerUrl);
    JsonReadInt(j, "inputDelayFrames", s.inputDelayFrames, 1, 8);

    JsonReadBool(j, "launchDirectly", s.launchDirectly);
    JsonReadBool(j, "showFps", s.showFps);
    JsonReadBool(j, "showFrameGraph", s.showFrameGraph);
    JsonReadBool(j, "showNetStats", s.showNetStats);

    out = s;
    return true;
}

bool SaveSettings(const Settings& s, const std::string& path) {
    const std::string target = path.empty() ? ConfigFilePath() : path;
    size_t slash = target.find_last_of("\\/");
    if (slash != std::string::npos) MakeDirsUtf8(target.substr(0, slash));

    std::ostringstream o;
    o << "{\n"
      << "  \"assetPath\": \"" << JsonEscape(s.assetPath) << "\",\n"
      << "  \"displayMode\": " << static_cast<int>(s.displayMode) << ",\n"
      << "  \"displayWidth\": " << s.displayWidth << ",\n"
      << "  \"displayHeight\": " << s.displayHeight << ",\n"
      << "  \"monitorIndex\": " << s.monitorIndex << ",\n"
      << "  \"aspectMode\": " << static_cast<int>(s.aspectMode) << ",\n"
      << "  \"qualityPreset\": " << static_cast<int>(s.qualityPreset) << ",\n"
      << "  \"renderScale\": " << s.renderScale << ",\n"
      << "  \"textureFilter\": " << static_cast<int>(s.textureFilter) << ",\n"
      << "  \"anisotropy\": " << s.anisotropy << ",\n"
      << "  \"postScanlines\": " << (s.postScanlines ? "true" : "false") << ",\n"
      << "  \"vsync\": " << static_cast<int>(s.vsync) << ",\n"
      << "  \"swapBuffers\": " << s.swapBuffers << ",\n"
      << "  \"presentationFpsLimit\": " << s.presentationFpsLimit << ",\n"
      << "  \"masterVolume\": " << s.masterVolume << ",\n"
      << "  \"musicVolume\": " << s.musicVolume << ",\n"
      << "  \"sfxVolume\": " << s.sfxVolume << ",\n"
      << "  \"mute\": " << (s.mute ? "true" : "false") << ",\n"
      << "  \"audioDeviceId\": " << s.audioDeviceId << ",\n"
      << "  \"p1_up\": " << s.p1Keys.up << ",\n"
      << "  \"p1_down\": " << s.p1Keys.down << ",\n"
      << "  \"p1_left\": " << s.p1Keys.left << ",\n"
      << "  \"p1_right\": " << s.p1Keys.right << ",\n"
      << "  \"p1_jump\": " << s.p1Keys.jump << ",\n"
      << "  \"p1_attack\": " << s.p1Keys.attack << ",\n"
      << "  \"p1_back\": " << s.p1Keys.back << ",\n"
      << "  \"p1_start\": " << s.p1Keys.start << ",\n"
      << "  \"p1_altUp\": " << s.p1Keys.altUp << ",\n"
      << "  \"p1_altDown\": " << s.p1Keys.altDown << ",\n"
      << "  \"p1_altLeft\": " << s.p1Keys.altLeft << ",\n"
      << "  \"p1_altRight\": " << s.p1Keys.altRight << ",\n"
      << "  \"p1_altJump\": " << s.p1Keys.altJump << ",\n"
      << "  \"p1_altAttack\": " << s.p1Keys.altAttack << ",\n"
      << "  \"p2_up\": " << s.p2Keys.up << ",\n"
      << "  \"p2_down\": " << s.p2Keys.down << ",\n"
      << "  \"p2_left\": " << s.p2Keys.left << ",\n"
      << "  \"p2_right\": " << s.p2Keys.right << ",\n"
      << "  \"p2_jump\": " << s.p2Keys.jump << ",\n"
      << "  \"p2_attack\": " << s.p2Keys.attack << ",\n"
      << "  \"p2_back\": " << s.p2Keys.back << ",\n"
      << "  \"p2_start\": " << s.p2Keys.start << ",\n"
      << "  \"p2_altJump\": " << s.p2Keys.altJump << ",\n"
      << "  \"p1Controller\": " << s.p1Controller << ",\n"
      << "  \"p2Controller\": " << s.p2Controller << ",\n"
      << "  \"vibration\": " << (s.vibration ? "true" : "false") << ",\n"
      << "  \"playerName\": \"" << JsonEscape(s.playerName) << "\",\n"
      << "  \"region\": \"" << JsonEscape(s.region) << "\",\n"
      << "  \"lobbyServerUrl\": \"" << JsonEscape(s.lobbyServerUrl) << "\",\n"
      << "  \"inputDelayFrames\": " << s.inputDelayFrames << ",\n"
      << "  \"launchDirectly\": " << (s.launchDirectly ? "true" : "false") << ",\n"
      << "  \"showFps\": " << (s.showFps ? "true" : "false") << ",\n"
      << "  \"showFrameGraph\": " << (s.showFrameGraph ? "true" : "false") << ",\n"
      << "  \"showNetStats\": " << (s.showNetStats ? "true" : "false") << "\n"
      << "}\n";

    const std::string text = o.str();
    std::FILE* f = FopenUtf8(target, "wb");
    if (!f) return false;
    const size_t wrote = std::fwrite(text.data(), 1, text.size(), f);
    std::fclose(f);
    return wrote == text.size();
}

void ResetSettingsToDefaults() {
    std::string keepAssets = g_current.assetPath;
    g_current = MakeDefaultSettings();
    g_current.assetPath = keepAssets;
    SaveSettings(g_current);
}

std::vector<MonitorInfo> EnumerateMonitors() {
    std::vector<MonitorInfo> out;
    using Microsoft::WRL::ComPtr;
    ComPtr<IDXGIFactory1> factory;
    if (SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) {
        for (UINT a = 0;; ++a) {
            ComPtr<IDXGIAdapter1> adapter;
            if (factory->EnumAdapters1(a, &adapter) == DXGI_ERROR_NOT_FOUND) break;
            for (UINT o = 0;; ++o) {
                ComPtr<IDXGIOutput> output;
                if (adapter->EnumOutputs(o, &output) == DXGI_ERROR_NOT_FOUND) break;
                DXGI_OUTPUT_DESC od{};
                if (FAILED(output->GetDesc(&od)) || !od.AttachedToDesktop) continue;

                MonitorInfo mi;
                mi.index = static_cast<int>(out.size());
                mi.name = WideToUtf8(od.DeviceName);
                mi.left = od.DesktopCoordinates.left;
                mi.top = od.DesktopCoordinates.top;
                mi.width = static_cast<uint32_t>(od.DesktopCoordinates.right - od.DesktopCoordinates.left);
                mi.height = static_cast<uint32_t>(od.DesktopCoordinates.bottom - od.DesktopCoordinates.top);
                mi.primary = (mi.left == 0 && mi.top == 0);

                UINT num = 0;
                output->GetDisplayModeList(DXGI_FORMAT_R8G8B8A8_UNORM, 0, &num, nullptr);
                if (num > 0) {
                    std::vector<DXGI_MODE_DESC> modes(num);
                    if (SUCCEEDED(output->GetDisplayModeList(DXGI_FORMAT_R8G8B8A8_UNORM, 0, &num,
                                                             modes.data()))) {
                        for (const auto& m : modes) {
                            if (m.Width < 640 || m.Height < 360) continue;
                            const uint32_t hz = m.RefreshRate.Denominator
                                                    ? (m.RefreshRate.Numerator + m.RefreshRate.Denominator / 2) /
                                                          m.RefreshRate.Denominator
                                                    : 60;
                            bool dup = false;
                            for (auto& existing : mi.modes) {
                                if (existing.width == m.Width && existing.height == m.Height) {
                                    existing.refreshHz = std::max(existing.refreshHz, hz);
                                    dup = true;
                                    break;
                                }
                            }
                            if (!dup) mi.modes.push_back({m.Width, m.Height, hz});
                        }
                    }
                }
                out.push_back(std::move(mi));
            }
        }
    }
    if (out.empty()) {
        MonitorInfo fallback;
        fallback.index = 0;
        fallback.name = "Primary Display";
        fallback.width = static_cast<uint32_t>(GetSystemMetrics(SM_CXSCREEN));
        fallback.height = static_cast<uint32_t>(GetSystemMetrics(SM_CYSCREEN));
        if (!fallback.width) fallback.width = 1920;
        if (!fallback.height) fallback.height = 1080;
        fallback.primary = true;
        fallback.modes = {{1280, 720, 60}, {1600, 900, 60}, {1920, 1080, 60},
                          {2560, 1440, 60}, {3840, 2160, 60}};
        out.push_back(std::move(fallback));
    }
    return out;
}

std::vector<AudioDeviceInfo> EnumerateAudioDevices() {
    std::vector<AudioDeviceInfo> out;
    out.push_back({-1, "Windows Default Audio Device"});
    const UINT n = waveOutGetNumDevs();
    for (UINT i = 0; i < n; ++i) {
        WAVEOUTCAPSW caps{};
        if (waveOutGetDevCapsW(i, &caps, sizeof(caps)) == MMSYSERR_NOERROR) {
            out.push_back({static_cast<int>(i), WideToUtf8(caps.szPname)});
        }
    }
    return out;
}

std::vector<KeyConflict> DetectKeyConflicts(const Settings& s) {
    struct Entry {
        const char* name;
        int vk;
    };
    const Entry entries[] = {
        {"P1 Up", s.p1Keys.up},         {"P1 Down", s.p1Keys.down},
        {"P1 Left", s.p1Keys.left},     {"P1 Right", s.p1Keys.right},
        {"P1 Jump", s.p1Keys.jump},     {"P1 Attack", s.p1Keys.attack},
        {"P1 Back", s.p1Keys.back},     {"P1 Start", s.p1Keys.start},
        {"P2 Up", s.p2Keys.up},         {"P2 Down", s.p2Keys.down},
        {"P2 Left", s.p2Keys.left},     {"P2 Right", s.p2Keys.right},
        {"P2 Jump", s.p2Keys.jump},     {"P2 Attack", s.p2Keys.attack},
        {"P2 Back", s.p2Keys.back},     {"P2 Start", s.p2Keys.start},
    };
    std::vector<KeyConflict> conflicts;
    for (size_t i = 0; i < std::size(entries); ++i) {
        if (!entries[i].vk) continue;
        for (size_t j = i + 1; j < std::size(entries); ++j) {
            if (entries[i].vk == entries[j].vk) {
                conflicts.push_back({entries[i].name, entries[j].name, entries[i].vk});
            }
        }
    }
    return conflicts;
}

const char* VirtualKeyName(int vk) {
    switch (vk) {
        case 0: return "Unbound";
        case VK_SPACE: return "Space";
        case VK_RETURN: return "Enter";
        case VK_ESCAPE: return "Esc";
        case VK_BACK: return "Backspace";
        case VK_TAB: return "Tab";
        case VK_UP: return "Up";
        case VK_DOWN: return "Down";
        case VK_LEFT: return "Left";
        case VK_RIGHT: return "Right";
        case VK_LSHIFT: case VK_SHIFT: return "Shift";
        case VK_RSHIFT: return "RShift";
        case VK_LCONTROL: case VK_CONTROL: return "Ctrl";
        case VK_RCONTROL: return "RCtrl";
        case VK_MENU: return "Alt";
        default: break;
    }
    static char s_names[256][8] = {};
    if (vk >= '0' && vk <= 'Z') {
        s_names[vk][0] = static_cast<char>(vk);
        s_names[vk][1] = '\0';
        return s_names[vk];
    }
    if (vk >= VK_F1 && vk <= VK_F12) {
        std::snprintf(s_names[vk & 255], 8, "F%d", vk - VK_F1 + 1);
        return s_names[vk & 255];
    }
    std::snprintf(s_names[vk & 255], 8, "0x%02X", vk & 255);
    return s_names[vk & 255];
}

AssetValidation ValidateAssetsAt(const std::string& candidateDir) {
    AssetValidation v;
    if (candidateDir.empty()) {
        v.summary = "No game asset directory specified.";
        return v;
    }

    // Accept either a folder directly, or a direct path to eboot.bin / nidhogg.elf.
    std::string root = candidateDir;
    std::string explicitExe;
    if (FileExistsUtf8(candidateDir)) {
        explicitExe = candidateDir;
        size_t sl = root.find_last_of("\\/");
        root = (sl != std::string::npos) ? root.substr(0, sl) : ".";
    }
    while (root.size() > 3 && (root.back() == '\\' || root.back() == '/')) root.pop_back();
    v.rootDir = root;

    // Locate executable: explicit, eboot.bin, nidhogg.elf, or work/nidhogg.elf
    const std::string exeCandidates[] = {
        explicitExe,
        root + "\\eboot.bin",
        root + "\\nidhogg.elf",
        root + "\\work\\nidhogg.elf",
        ExeDir() + "\\work\\nidhogg.elf",
        ExeDir() + "\\..\\..\\work\\nidhogg.elf",
    };
    for (const auto& c : exeCandidates) {
        if (!c.empty() && FileExistsUtf8(c)) {
            v.executablePath = c;
            break;
        }
    }
    if (v.executablePath.empty()) v.missingFiles.push_back("eboot.bin (or nidhogg.elf)");

    // Locate Data directory (either root\Data or parent\CUSA00920\Data when pointed at work\nidhogg.elf)
    std::string dataRoot = root;
    if (!DirExistsUtf8(dataRoot + "\\Data")) {
        const std::string parentCands[] = {
            root + "\\..",
            root + "\\..\\CUSA00920",
            root + "\\..\\..\\CUSA00920",
        };
        for (const auto& pc : parentCands) {
            if (DirExistsUtf8(pc + "\\Data")) {
                dataRoot = pc;
                break;
            }
        }
    }
    v.rootDir = dataRoot;
    v.dataDir = dataRoot + "\\Data";

    const char* requiredAssets[] = {
        "Data\\shader_vv.sb",
        "Data\\shader_p.sb",
        "Data\\Font.csv",
        "Data\\circle.png",
        "Data\\atlas\\0.png",
        "Data\\atlas\\1.png",
        "Data\\atlas\\0_outline.png",
        "Data\\fonts\\fMenu.csv",
        "Data\\fonts\\fMenu.png",
        "Data\\audio\\audio.bnk",
    };
    for (const char* rel : requiredAssets) {
        if (!FileExistsUtf8(dataRoot + "\\" + rel)) v.missingFiles.push_back(rel);
    }

    v.valid = v.missingFiles.empty();
    if (v.valid) {
        v.summary = "Verified Nidhogg (CUSA00920) game files at " + v.rootDir;
    } else {
        std::ostringstream oss;
        oss << "Missing " << v.missingFiles.size() << " required file(s): ";
        for (size_t i = 0; i < v.missingFiles.size() && i < 4; ++i) {
            if (i) oss << ", ";
            oss << v.missingFiles[i];
        }
        if (v.missingFiles.size() > 4) oss << ", ...";
        v.summary = oss.str();
    }
    return v;
}

AssetValidation AutoDiscoverAssets(const Settings& s, const char* cliArg) {
    std::vector<std::string> tries;
    if (cliArg && *cliArg) tries.emplace_back(cliArg);
    if (!s.assetPath.empty()) tries.push_back(s.assetPath);

    char* env = nullptr;
    size_t len = 0;
    if (_dupenv_s(&env, &len, "NIDHOGG_APP0") == 0 && env && *env) {
        tries.emplace_back(env);
    }
    if (env) std::free(env);

    const std::string exe = ExeDir();
    tries.push_back(exe);
    tries.push_back(exe + "\\CUSA00920");
    tries.push_back(exe + "\\..\\CUSA00920");
    tries.push_back(exe + "\\..\\..\\CUSA00920");
    tries.push_back(exe + "\\..\\..\\..\\CUSA00920");
    tries.push_back(".\\CUSA00920");
    tries.push_back("..\\CUSA00920");

    AssetValidation firstFail;
    for (const auto& c : tries) {
        AssetValidation v = ValidateAssetsAt(c);
        if (v.valid) return v;
        if (firstFail.summary.empty()) firstFail = v;
    }
    return firstFail;
}

bool PromptForAssetDirectoryGui(void* parentHwnd, std::string& outSelectedDir) {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    using Microsoft::WRL::ComPtr;
    ComPtr<IFileOpenDialog> dlg;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
