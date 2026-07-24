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
