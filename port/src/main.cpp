// Nidhogg (CUSA00920) native Windows x64 runtime entry point.
//
// Maps and relocates the PS4 x86-64 guest executable (raw SELF `eboot.bin` or
// extracted `nidhogg.elf`), installs System V <-> Win64 ABI trampolines,
// initializes settings, logging, crash reporting, DPI awareness, and transfers
// control to the guest entry point.
#define NOMINMAX
#include <windows.h>
#include <shellapi.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sstream>
#include <string>
#include <vector>

#include "core/logger.h"
#include "core/settings.h"
#include "hle.h"
#include "image.h"
#include "thunks.h"

namespace {

uint64_t g_imageBase = 0;
DWORD g_mainThread = 0;
bool g_noGuiPrompts = false;

void EnableDpiAwareness() {
    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    if (user32) {
        using SetDpiCtxFn = BOOL(WINAPI*)(DPI_AWARENESS_CONTEXT);
        auto fn = reinterpret_cast<SetDpiCtxFn>(
            GetProcAddress(user32, "SetProcessDpiAwarenessContext"));
        if (fn && fn(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2)) return;
    }
    SetProcessDPIAware();
}

void DumpMainStack(const char* why) {
    HANDLE th = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT, FALSE, g_mainThread);
    if (!th) return;
    SuspendThread(th);
    CONTEXT c{};
    c.ContextFlags = CONTEXT_FULL;
    if (GetThreadContext(th, &c)) {
        LOG_WARN("[watchdog] %s | rip=+0x%llx (last slot %u from +0x%llx)",
                 why,
                 static_cast<unsigned long long>(static_cast<uint64_t>(c.Rip) - g_imageBase),
                 ps4::LastSlot(),
                 static_cast<unsigned long long>(ps4::LastCallSite() - g_imageBase));
    }
    ResumeThread(th);
    CloseHandle(th);
}

using SysvCall = uint64_t (*)(uint64_t fn, uint64_t a0, uint64_t a1);
SysvCall MakeSysvCall() {
    auto* p = static_cast<uint8_t*>(
        VirtualAlloc(nullptr, 0x1000, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    const uint8_t code[] = {0x48, 0x89, 0xD7,   // mov rdi, rdx
                            0x4C, 0x89, 0xC6,   // mov rsi, r8
                            0xFF, 0xE1};        // jmp rcx
    std::memcpy(p, code, sizeof(code));
    FlushInstructionCache(GetCurrentProcess(), p, sizeof(code));
    return reinterpret_cast<SysvCall>(p);
}

bool IsFatalHardwareException(DWORD code) {
    switch (code) {
        case EXCEPTION_ACCESS_VIOLATION:
        case EXCEPTION_ILLEGAL_INSTRUCTION:
        case EXCEPTION_PRIV_INSTRUCTION:
        case EXCEPTION_STACK_OVERFLOW:
        case EXCEPTION_INT_DIVIDE_BY_ZERO:
        case EXCEPTION_IN_PAGE_ERROR:
            return true;
        default:
            return false;
    }
}

LONG WINAPI OnFault(EXCEPTION_POINTERS* ep) {
    const auto* rec = ep->ExceptionRecord;
    if (!IsFatalHardwareException(rec->ExceptionCode)) {
        return EXCEPTION_CONTINUE_SEARCH;
    }

    const CONTEXT* c = ep->ContextRecord;
    char modName[MAX_PATH] = "?";
    uint64_t modBase = 0;
    {
        MEMORY_BASIC_INFORMATION mbi{};
        if (VirtualQuery(rec->ExceptionAddress, &mbi, sizeof(mbi))) {
            modBase = reinterpret_cast<uint64_t>(mbi.AllocationBase);
            GetModuleFileNameA(static_cast<HMODULE>(mbi.AllocationBase), modName, MAX_PATH);
            const char* slash = std::strrchr(modName, '\\');
            if (slash) std::memmove(modName, slash + 1, std::strlen(slash + 1) + 1);
        }
    }

    std::ostringstream report;
    report << "Nidhogg Fatal Exception Report\n"
           << "Code: 0x" << std::hex << rec->ExceptionCode
           << " at " << rec->ExceptionAddress
           << " (" << modName << "+0x"
           << (reinterpret_cast<uint64_t>(rec->ExceptionAddress) - modBase) << ")\n";
    if (rec->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && rec->NumberParameters >= 2) {
        report << "Access: " << (rec->ExceptionInformation[0] ? "WRITE" : "READ")
               << " at 0x" << rec->ExceptionInformation[1] << "\n";
    }
    report << "Guest RIP offset: +0x" << (static_cast<uint64_t>(c->Rip) - g_imageBase)
           << " | Last HLE slot: " << std::dec << ps4::LastSlot() << " ("
           << (ps4::SlotName(ps4::LastSlot()) ? ps4::SlotName(ps4::LastSlot()) : "?")
           << ") from +0x" << std::hex << (ps4::LastCallSite() - g_imageBase) << "\n"
           << "Frame: " << std::dec << ps4::SubmittedFrames()
           << " | Room: " << ps4::CurrentRoom() << "\n"
           << "Stack:";

    uint64_t bp = c->Rbp;
    for (int i = 0; i < 20 && bp; ++i) {
        MEMORY_BASIC_INFORMATION mbi{};
        if (!VirtualQuery(reinterpret_cast<void*>(bp), &mbi, sizeof(mbi)) ||
            mbi.State != MEM_COMMIT || (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) ||
            bp + 16 > reinterpret_cast<uint64_t>(mbi.BaseAddress) + mbi.RegionSize)
            break;
        const uint64_t savedBp = *reinterpret_cast<uint64_t*>(bp);
        const uint64_t ret = *reinterpret_cast<uint64_t*>(bp + 8);
        report << " +0x" << std::hex << (ret - g_imageBase);
        if (savedBp <= bp) break;
        bp = savedBp;
    }
    report << "\n";

    const std::string crashText = report.str();
    LOG_ERROR("%s", crashText.c_str());

    const std::string crashFile = ps4::core::LogsDirPath() + "\\crash.log";
    if (std::FILE* cf = ps4::core::FopenUtf8(crashFile, "wb")) {
        std::fwrite(crashText.data(), 1, crashText.size(), cf);
        std::fclose(cf);
    }

    if (!g_noGuiPrompts) {
        std::wstring msg = ps4::core::Utf8ToWide(
            "Nidhogg encountered an unexpected error and wrote diagnostic details to:\n\n" +
            crashFile);
        MessageBoxW(nullptr, msg.c_str(), L"Nidhogg Crash Report", MB_ICONERROR | MB_OK);
    }
    TerminateProcess(GetCurrentProcess(), 0xC0000005u);
    return EXCEPTION_CONTINUE_SEARCH;
}

int RunMain(const std::vector<std::string>& args, bool hasConsole) {
    EnableDpiAwareness();
    g_mainThread = GetCurrentThreadId();

    std::string positionalPath;
    std::string cliAssets;
    std::string cliUserDir;
    std::string cliLobbyUrl;
    uint64_t callAt = 0;
    uint64_t maxFrames = 0;
    bool wantConsole = hasConsole;
    bool openOnlineMenu = false;
    bool openSettingsMenu = false;
    bool verifyOnly = false;

    for (size_t i = 1; i < args.size(); ++i) {
        const std::string& a = args[i];
        if (a == "--user-dir" && i + 1 < args.size()) {
            cliUserDir = args[++i];
        } else if (a == "--lobby-url" && i + 1 < args.size()) {
            cliLobbyUrl = args[++i];
        } else if (a == "--assets" && i + 1 < args.size()) {
            cliAssets = args[++i];
        } else if (a == "--call" && i + 1 < args.size()) {
            callAt = std::strtoull(args[++i].c_str(), nullptr, 0);
        } else if (a == "--max-frames" && i + 1 < args.size()) {
            maxFrames = std::strtoull(args[++i].c_str(), nullptr, 0);
        } else if (a == "--console") {
            wantConsole = true;
        } else if (a == "--no-prompt") {
            g_noGuiPrompts = true;
        } else if (a == "--online") {
            openOnlineMenu = true;
        } else if (a == "--settings") {
            openSettingsMenu = true;
        } else if (a == "--verify-assets") {
            verifyOnly = true;
        } else if (!a.empty() && a[0] != '-' && positionalPath.empty()) {
            positionalPath = a;
        }
    }

    if (maxFrames > 0) {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%llu", static_cast<unsigned long long>(maxFrames));
        _putenv_s("NIDHOGG_MAX_FRAMES", buf);
    }
    if (openOnlineMenu) _putenv_s("NIDHOGG_OPEN_ONLINE", "1");
    if (openSettingsMenu) _putenv_s("NIDHOGG_OPEN_SETTINGS", "1");

    if (wantConsole && !hasConsole) {
        if (AttachConsole(ATTACH_PARENT_PROCESS) || AllocConsole()) {
            FILE* dummy = nullptr;
            freopen_s(&dummy, "CONOUT$", "w", stdout);
            freopen_s(&dummy, "CONOUT$", "w", stderr);
            hasConsole = true;
        }
    }
    if (hasConsole) {
        SetConsoleOutputCP(CP_UTF8);
        std::setvbuf(stdout, nullptr, _IONBF, 0);
    }

    if (!cliUserDir.empty()) ps4::core::SetUserDirOverride(cliUserDir);

    const std::string logPath = ps4::core::LogsDirPath() + "\\nidhogg.log";
    ps4::core::InitLogger(logPath, hasConsole);
    LOG_INFO("Nidhogg Windows x64 Runtime starting (userDir=%s)", ps4::core::UserDir().c_str());

    auto& settings = ps4::core::CurrentSettings();
    if (!ps4::core::LoadSettings(settings)) {
        ps4::core::SaveSettings(settings);
    }
    if (!cliLobbyUrl.empty()) settings.lobbyServerUrl = cliLobbyUrl;

    const char* assetHint = !cliAssets.empty()
                                ? cliAssets.c_str()
                                : (!positionalPath.empty() ? positionalPath.c_str() : nullptr);
    ps4::core::AssetValidation assets = ps4::core::AutoDiscoverAssets(settings, assetHint);

    while (!assets.valid && !g_noGuiPrompts && !verifyOnly) {
        std::ostringstream msg;
        msg << "Nidhogg requires your legally acquired PS4 game files (CUSA00920).\n\n"
            << "Status: " << assets.summary << "\n\n"
            << "Required folder contents:\n"
            << "  - eboot.bin (or extracted nidhogg.elf)\n"
            << "  - Data\\ (atlas, audio, fonts, shader_vv.sb, shader_p.sb)\n\n"
            << "Would you like to browse to your Nidhogg game folder now?";
        std::wstring wmsg = ps4::core::Utf8ToWide(msg.str());
        int pick = MessageBoxW(nullptr, wmsg.c_str(), L"Nidhogg — Locate Game Files",
                               MB_ICONINFORMATION | MB_YESNO);
        if (pick != IDYES) break;
        std::string chosen;
        if (!ps4::core::PromptForAssetDirectoryGui(nullptr, chosen)) break;
        assets = ps4::core::ValidateAssetsAt(chosen);
        if (assets.valid) {
            settings.assetPath = assets.rootDir;
            ps4::core::SaveSettings(settings);
        }
    }

    if (verifyOnly) {
        LOG_INFO("[verify] valid=%d summary=%s", assets.valid ? 1 : 0, assets.summary.c_str());
        return assets.valid ? 0 : 2;
    }

    if (!assets.valid) {
        LOG_ERROR("Asset validation failed: %s", assets.summary.c_str());
        ps4::core::ShutdownLogger();
        return 2;
    }

    if (settings.assetPath != assets.rootDir) {
        settings.assetPath = assets.rootDir;
        ps4::core::SaveSettings(settings);
    }
    ps4::HleSetApp0(assets.rootDir);

    ps4::Image image;
    if (!image.load(assets.executablePath)) {
        LOG_ERROR("Failed to load guest executable: %s", assets.executablePath.c_str());
        if (!g_noGuiPrompts) {
            std::wstring werr = ps4::core::Utf8ToWide(
                "Failed to load or parse guest executable:\n" + assets.executablePath +
                "\n\nEnsure eboot.bin is a decrypted PS4 SELF or valid ELF image.");
            MessageBoxW(nullptr, werr.c_str(), L"Nidhogg — Load Error", MB_ICONERROR | MB_OK);
        }
        ps4::core::ShutdownLogger();
        return 1;
    }

    g_imageBase = reinterpret_cast<uint64_t>(image.base());
    LOG_INFO("image   base=%p span=0x%llx entry=0x%llx (%s)", image.base(),
             static_cast<unsigned long long>(image.size()),
             static_cast<unsigned long long>(image.entry()),
             assets.executablePath.c_str());

    const size_t relocs = image.relocate();
    LOG_INFO("relocs  %zu applied", relocs);

    auto* guard = static_cast<uint64_t*>(
        VirtualAlloc(nullptr, 0x1000, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    *guard = 0x8f3a5c17d2e94b60ull;
    *reinterpret_cast<uint64_t*>(image.at(0x33DAC8)) = reinterpret_cast<uint64_t>(guard);

    ps4::Thunks thunks;
    thunks.install(image);
    LOG_INFO("imports %zu slots wired to stubs", thunks.count());

    char* probeEnv = nullptr;
    size_t probeLen = 0;
    if (_dupenv_s(&probeEnv, &probeLen, "NIDHOGG_PROBE_ONLY") == 0 && probeEnv && *probeEnv) {
        std::free(probeEnv);
        LOG_INFO("probe only, exiting cleanly");
        ps4::core::ShutdownLogger();
        return 0;
    }
    if (probeEnv) std::free(probeEnv);

    AddVectoredExceptionHandler(1, OnFault);

    if (callAt) {
        using Fn = uint64_t (*)();
        auto fn = reinterpret_cast<Fn>(image.at(callAt));
        const uint64_t r = fn();
        LOG_INFO("callAt 0x%llx returned 0x%llx", static_cast<unsigned long long>(callAt),
                 static_cast<unsigned long long>(r));
        ps4::core::ShutdownLogger();
        return 0;
    }

    const char* exe = "nidhogg";
    const char* env[] = {"LANG=en_US.UTF-8", "HOME=/app0", nullptr};
    uint8_t* block = static_cast<uint8_t*>(
        VirtualAlloc(nullptr, 0x400, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    *reinterpret_cast<uint32_t*>(block) = 1;
    *reinterpret_cast<uint32_t*>(block + 4) = 0;
    const char** av = reinterpret_cast<const char**>(block + 8);
    av[0] = exe;
    av[1] = nullptr;
    const char** ev = av + 2;
    for (int i = 0; env[i]; ++i) ev[i] = env[i];
    ev[2] = nullptr;

    char* wdEnv = nullptr;
    size_t wdLen = 0;
    DWORD watchdogMs = 0;
    if (_dupenv_s(&wdEnv, &wdLen, "NIDHOGG_WATCHDOG") == 0 && wdEnv) {
        watchdogMs = static_cast<DWORD>(std::strtoul(wdEnv, nullptr, 0));
        std::free(wdEnv);
    }
    if (watchdogMs) {
        auto* ms = new DWORD(watchdogMs);
        CreateThread(nullptr, 0, [](LPVOID p) -> DWORD {
            const DWORD wait = *static_cast<DWORD*>(p);
            delete static_cast<DWORD*>(p);
            Sleep(wait);
            DumpMainStack("no progress, parked at");
            return 0;
        }, ms, 0, nullptr);
    }

    LOG_INFO("entering guest 0x%llx", static_cast<unsigned long long>(image.entry()));
    if (!ps4::net::ResumeOnlineSession()) return 1;
    auto call = MakeSysvCall();
    call(reinterpret_cast<uint64_t>(image.at(image.entry())),
         reinterpret_cast<uint64_t>(block), 0);
    ps4::HleShutdown();
    return 0;
}

std::vector<std::string> ParseWideCommandLine() {
    std::vector<std::string> out;
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv) return out;
    out.reserve(static_cast<size_t>(argc));
    for (int i = 0; i < argc; ++i) out.push_back(ps4::core::WideToUtf8(argv[i]));
    LocalFree(argv);
    return out;
}

}  // namespace

int main(int, char**) {
    return RunMain(ParseWideCommandLine(), true);
}

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
    const bool redirected = (hOut != nullptr && hOut != INVALID_HANDLE_VALUE &&
                             GetFileType(hOut) != FILE_TYPE_UNKNOWN);
    return RunMain(ParseWideCommandLine(), redirected);
}
