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

std::string GetControlUtf8(HWND ctrl) {
    int len = GetWindowTextLengthW(ctrl);
    if (len <= 0) return {};
    std::wstring w(static_cast<size_t>(len) + 1, L'\0');
    GetWindowTextW(ctrl, w.data(), len + 1);
    w.resize(static_cast<size_t>(len));
    return ps4::core::WideToUtf8(w);
}

void PullUiIntoSettings() {
    auto& s = ps4::core::CurrentSettings();
    int dm = static_cast<int>(SendMessageW(g_cmbMode, CB_GETCURSEL, 0, 0));
    if (dm >= 0 && dm <= 2) s.displayMode = static_cast<ps4::core::DisplayMode>(dm);

    int ri = static_cast<int>(SendMessageW(g_cmbRes, CB_GETCURSEL, 0, 0));
    if (ri >= 0 && ri < static_cast<int>(std::size(kResList))) {
        s.displayWidth = kResList[ri].w;
        s.displayHeight = kResList[ri].h;
    }

    int qi = static_cast<int>(SendMessageW(g_cmbQual, CB_GETCURSEL, 0, 0));
    if (qi >= 0 && qi <= 3) {
        ps4::core::ApplyQualityPreset(s, static_cast<ps4::core::QualityPreset>(qi));
    }

    std::string nm = GetControlUtf8(g_edtName);
    if (!nm.empty()) s.playerName = nm;
    std::string url = GetControlUtf8(g_edtUrl);
    if (!url.empty()) s.lobbyServerUrl = url;
    ps4::core::SaveSettings(s);
}

void RefreshAssetStatus(bool showPopup) {
    auto& s = ps4::core::CurrentSettings();
    auto v = ps4::core::AutoDiscoverAssets(s, nullptr);
    if (v.valid && s.assetPath != v.rootDir) {
        s.assetPath = v.rootDir;
        ps4::core::SaveSettings(s);
    }
    std::wstring statusText = ps4::core::Utf8ToWide(
        v.valid ? ("READY: " + v.rootDir) : ("MISSING GAME FILES: " + v.summary));
    SetWindowTextW(g_lblAssets, statusText.c_str());

    if (showPopup) {
        MessageBoxW(g_hwnd, statusText.c_str(),
                    v.valid ? L"Game Files Verified" : L"Game Files Incomplete",
                    v.valid ? (MB_ICONINFORMATION | MB_OK) : (MB_ICONWARNING | MB_OK));
    }
}

void LaunchGameExe(const wchar_t* extraFlag) {
    PullUiIntoSettings();
    auto& s = ps4::core::CurrentSettings();
    auto v = ps4::core::AutoDiscoverAssets(s, nullptr);
    if (!v.valid) {
        int r = MessageBoxW(
            g_hwnd,
            L"Nidhogg game files (eboot.bin + Data\\) have not been selected yet.\n\n"
            L"Select your CUSA00920 game folder now?",
            L"Nidhogg — Game Files Required", MB_ICONQUESTION | MB_YESNO);
        if (r == IDYES) {
            std::string chosen;
            if (ps4::core::PromptForAssetDirectoryGui(g_hwnd, chosen)) {
                auto cv = ps4::core::ValidateAssetsAt(chosen);
                if (cv.valid) {
                    s.assetPath = cv.rootDir;
                    ps4::core::SaveSettings(s);
                    v = cv;
                    RefreshAssetStatus(false);
                } else {
                    std::wstring werr = ps4::core::Utf8ToWide(cv.summary);
                    MessageBoxW(g_hwnd, werr.c_str(), L"Invalid Game Folder", MB_ICONERROR | MB_OK);
                    return;
                }
            } else {
                return;
            }
        } else {
            return;
        }
    }

    const std::string exeDir = ps4::core::ExeDir();
    std::string gameExe = exeDir + "\\Nidhogg.exe";
    if (!ps4::core::FileExistsUtf8(gameExe)) gameExe = exeDir + "\\nidhogg_host.exe";
    if (!ps4::core::FileExistsUtf8(gameExe)) {
        MessageBoxW(g_hwnd, L"Could not find Nidhogg.exe next to NidhoggLauncher.exe.",
                    L"Launch Error", MB_ICONERROR | MB_OK);
        return;
    }

    std::wstring cmd = L"\"" + ps4::core::Utf8ToWide(gameExe) + L"\"";
    if (extraFlag && *extraFlag) {
        cmd += L" ";
        cmd += extraFlag;
    }
    std::wstring workDir = ps4::core::Utf8ToWide(exeDir);
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    if (CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, workDir.c_str(),
                       &si, &pi)) {
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        DestroyWindow(g_hwnd);
    } else {
        MessageBoxW(g_hwnd, L"Failed to launch Nidhogg.exe.", L"Launch Error", MB_ICONERROR | MB_OK);
    }
}

void ToggleLocalServer() {
    DWORD pid = FindServerProcessId();
    if (pid != 0) {
        HANDLE h = OpenProcess(PROCESS_TERMINATE, FALSE, pid);
        if (h) {
            TerminateProcess(h, 0);
            CloseHandle(h);
        }
        Sleep(150);
    } else {
        std::string srvExe = ps4::core::ExeDir() + "\\NidhoggServer.exe";
        if (!ps4::core::FileExistsUtf8(srvExe)) {
            MessageBoxW(g_hwnd, L"NidhoggServer.exe was not found in the application directory.",
                        L"Server Missing", MB_ICONWARNING | MB_OK);
            return;
        }
        std::wstring wSrv = L"\"" + ps4::core::Utf8ToWide(srvExe) + L"\"";
        STARTUPINFOW si{};
        si.cb = sizeof(si);
        PROCESS_INFORMATION pi{};
        if (CreateProcessW(nullptr, wSrv.data(), nullptr, nullptr, FALSE,
                           CREATE_NO_WINDOW | DETACHED_PROCESS, nullptr, nullptr, &si, &pi)) {
            CloseHandle(pi.hThread);
            CloseHandle(pi.hProcess);
            Sleep(250);
        }
    }
    SetWindowTextW(g_btnServer,
                   FindServerProcessId() ? L"Stop Local Server" : L"Start Local Server");
    AsyncCheckServerStatus(g_hwnd, GetControlUtf8(g_edtUrl));
}

LRESULT CALLBACK LauncherProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_CTLCOLORSTATIC: {
            HDC hdc = reinterpret_cast<HDC>(wp);
            SetTextColor(hdc, RGB(236, 239, 246));
            SetBkColor(hdc, RGB(20, 23, 34));
            return reinterpret_cast<LRESULT>(g_bgBrush);
        }
        case WM_APP_SERVER_STATUS: {
            const int st = static_cast<int>(wp);
            if (st == 1) {
                std::wstring txt = L"Multiplayer Endpoint: ONLINE (" +
                                   ps4::core::Utf8ToWide(GetControlUtf8(g_edtUrl)) + L")";
                SetWindowTextW(g_lblServer, txt.c_str());
            } else if (st == 2) {
                SetWindowTextW(g_lblServer,
                               L"Multiplayer Endpoint: Local Lobby Active (http://127.0.0.1:27880)");
            } else {
                SetWindowTextW(g_lblServer,
                               L"Multiplayer Endpoint: Offline (Click 'Start Local Server' to host locally)");
            }
            SetWindowTextW(g_btnServer,
                           FindServerProcessId() ? L"Stop Local Server" : L"Start Local Server");
            return 0;
        }
        case WM_COMMAND: {
            const int id = LOWORD(wp);
            switch (id) {
                case IDC_BTN_PLAY:     LaunchGameExe(nullptr); break;
                case IDC_BTN_ONLINE:   LaunchGameExe(L"--online"); break;
                case IDC_BTN_SETTINGS: LaunchGameExe(L"--settings"); break;
                case IDC_BTN_BROWSE: {
                    std::string chosen;
                    if (ps4::core::PromptForAssetDirectoryGui(hwnd, chosen)) {
                        auto v = ps4::core::ValidateAssetsAt(chosen);
                        if (v.valid) {
                            ps4::core::CurrentSettings().assetPath = v.rootDir;
                            ps4::core::SaveSettings(ps4::core::CurrentSettings());
                        }
                        RefreshAssetStatus(true);
                    }
                    break;
                }
                case IDC_BTN_VERIFY: RefreshAssetStatus(true); break;
                case IDC_BTN_SAVES: {
                    std::wstring dir = ps4::core::Utf8ToWide(ps4::core::SavesDirPath());
                    ShellExecuteW(hwnd, L"open", dir.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
                    break;
                }
                case IDC_BTN_LOGS: {
