// NidhoggLauncher.exe — Standalone Native Win32 GUI Launcher for Nidhogg.
//
// Zero PowerShell, zero developer tools, zero console flash.
// Discovers and validates user-supplied PS4 CUSA00920 game files, configures
// display/quality/multiplayer settings in `config/settings.json`, checks live
// lobby server status (`https://lobby.japer.me` / `http://127.0.0.1:27880`),
// optionally starts/stops local `NidhoggServer.exe`, and launches `Nidhogg.exe`.
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>
#include <tlhelp32.h>
#include <winhttp.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "../src/core/logger.h"
#include "../src/core/settings.h"

namespace {

enum CtrlId : int {
    IDC_BTN_PLAY = 1001,
    IDC_BTN_ONLINE = 1002,
    IDC_BTN_SETTINGS = 1003,
    IDC_BTN_BROWSE = 1004,
    IDC_BTN_VERIFY = 1005,
    IDC_BTN_SAVES = 1006,
    IDC_BTN_LOGS = 1007,
    IDC_BTN_SERVER = 1008,
    IDC_BTN_EXIT = 1009,
    IDC_CMB_MODE = 1010,
    IDC_CMB_RES = 1011,
    IDC_CMB_QUAL = 1012,
    IDC_EDT_NAME = 1013,
    IDC_EDT_URL = 1014,
};

constexpr UINT WM_APP_SERVER_STATUS = WM_APP + 10;

HWND g_hwnd = nullptr;
HWND g_lblAssets = nullptr;
HWND g_lblServer = nullptr;
HWND g_cmbMode = nullptr;
HWND g_cmbRes = nullptr;
HWND g_cmbQual = nullptr;
HWND g_edtName = nullptr;
HWND g_edtUrl = nullptr;
HWND g_btnServer = nullptr;
HFONT g_fontTitle = nullptr;
HFONT g_fontUi = nullptr;
HFONT g_fontBold = nullptr;
HBRUSH g_bgBrush = nullptr;
HBRUSH g_panelBrush = nullptr;

struct ResPair {
    uint32_t w;
    uint32_t h;
    const wchar_t* label;
};
const ResPair kResList[] = {
    {1280, 720,  L"1280 x 720 (720p HD)"},
    {1600, 900,  L"1600 x 900 (900p)"},
    {1920, 1080, L"1920 x 1080 (1080p Full HD)"},
    {2560, 1440, L"2560 x 1440 (1440p QHD)"},
    {3840, 2160, L"3840 x 2160 (4K UHD)"},
};

bool ProbeHealthUrl(const std::string& baseUrl) {
    std::string s = baseUrl;
    bool https = false;
    uint16_t port = 80;
    if (s.rfind("https://", 0) == 0) { https = true; port = 443; s.erase(0, 8); }
    else if (s.rfind("http://", 0) == 0) { https = false; port = 80; s.erase(0, 7); }
    size_t sl = s.find('/');
    if (sl != std::string::npos) s.resize(sl);
    size_t col = s.find(':');
    std::string host = s;
    if (col != std::string::npos) {
        host = s.substr(0, col);
        int p = std::atoi(s.substr(col + 1).c_str());
        if (p > 0 && p <= 65535) port = static_cast<uint16_t>(p);
    }
    if (host.empty()) return false;

    HINTERNET hSess = WinHttpOpen(L"NidhoggLauncher/1.0", WINHTTP_ACCESS_TYPE_NO_PROXY,
                                  WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSess) return false;
    WinHttpSetTimeouts(hSess, 800, 800, 800, 800);
    std::wstring wHost = ps4::core::Utf8ToWide(host);
    HINTERNET hConn = WinHttpConnect(hSess, wHost.c_str(), port, 0);
    if (!hConn) { WinHttpCloseHandle(hSess); return false; }
    HINTERNET hReq = WinHttpOpenRequest(hConn, L"GET", L"/api/health", nullptr,
                                        WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                        https ? WINHTTP_FLAG_SECURE : 0);
    bool ok = false;
    if (hReq) {
        if (WinHttpSendRequest(hReq, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                               WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
            WinHttpReceiveResponse(hReq, nullptr)) {
            DWORD code = 0, sz = sizeof(code);
            WinHttpQueryHeaders(hReq, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                                WINHTTP_HEADER_NAME_BY_INDEX, &code, &sz, WINHTTP_NO_HEADER_INDEX);
            ok = (code == 200);
        }
        WinHttpCloseHandle(hReq);
    }
    WinHttpCloseHandle(hConn);
    WinHttpCloseHandle(hSess);
    return ok;
}

DWORD FindServerProcessId() {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;
    PROCESSENTRY32W pe{};
    pe.dwSize = sizeof(pe);
    DWORD pid = 0;
    if (Process32FirstW(snap, &pe)) {
        do {
            if (_wcsicmp(pe.szExeFile, L"NidhoggServer.exe") == 0) {
                pid = pe.th32ProcessID;
                break;
            }
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    return pid;
}

void AsyncCheckServerStatus(HWND hwnd, std::string configuredUrl) {
    std::thread([hwnd, configuredUrl]() {
        int code = 0;  // 0=offline, 1=remote online, 2=local online
        if (ProbeHealthUrl(configuredUrl)) code = 1;
        else if (ProbeHealthUrl("http://127.0.0.1:27880")) code = 2;
        if (IsWindow(hwnd)) PostMessageW(hwnd, WM_APP_SERVER_STATUS, static_cast<WPARAM>(code), 0);
    }).detach();
}

