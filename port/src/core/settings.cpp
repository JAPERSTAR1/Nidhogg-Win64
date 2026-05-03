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
