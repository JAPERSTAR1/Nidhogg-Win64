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
