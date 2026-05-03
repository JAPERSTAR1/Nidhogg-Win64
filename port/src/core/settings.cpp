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
