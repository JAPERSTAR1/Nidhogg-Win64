#define NOMINMAX
#include "logger.h"

#include <windows.h>

#include <cstdio>
#include <cstring>
#include <mutex>

namespace ps4::core {
namespace {

std::mutex g_logMtx;
std::FILE* g_logFile = nullptr;
bool g_mirrorStdout = true;
std::string g_logPath;

std::wstring ToWide(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    if (n <= 0) return {};
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

}  // namespace

void InitLogger(const std::string& logFilePath, bool mirrorToStdout) {
    std::scoped_lock lk(g_logMtx);
    g_mirrorStdout = mirrorToStdout;
    g_logPath = logFilePath;
    if (g_logFile) {
        std::fclose(g_logFile);
        g_logFile = nullptr;
    }
    if (!logFilePath.empty()) {
        std::wstring w = ToWide(logFilePath);
        if (!w.empty()) g_logFile = _wfopen(w.c_str(), L"wb");
    }
}

void ShutdownLogger() {
    std::scoped_lock lk(g_logMtx);
    if (g_logFile) {
        std::fflush(g_logFile);
        std::fclose(g_logFile);
        g_logFile = nullptr;
    }
}

const std::string& LogFilePath() { return g_logPath; }

void Log(const char* level, const char* fmt, ...) {
    char body[4096];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(body, sizeof(body), fmt, ap);
    va_end(ap);

    SYSTEMTIME st{};
    GetLocalTime(&st);
    char line[4352];
    int len = std::snprintf(line, sizeof(line), "[%02u:%02u:%02u.%03u] [%-5s] %s\n",
                            st.wHour, st.wMinute, st.wSecond, st.wMilliseconds,
                            level ? level : "INFO", body);
    if (len < 0) return;
    if (static_cast<size_t>(len) >= sizeof(line)) len = static_cast<int>(sizeof(line) - 1);

    std::scoped_lock lk(g_logMtx);
    if (g_logFile) {
        std::fwrite(line, 1, static_cast<size_t>(len), g_logFile);
        std::fflush(g_logFile);
    }
    if (g_mirrorStdout) {
        std::fwrite(line, 1, static_cast<size_t>(len), stdout);
        std::fflush(stdout);
    }
}

void LogRaw(const char* text, size_t len) {
    if (!text || !len) return;
    std::scoped_lock lk(g_logMtx);
    if (g_logFile) {
        std::fwrite(text, 1, len, g_logFile);
        std::fflush(g_logFile);
    }
    if (g_mirrorStdout) {
        std::fwrite(text, 1, len, stdout);
        std::fflush(stdout);
    }
}

}  // namespace ps4::core
