#pragma once

#include <cstdarg>
#include <string>

namespace ps4::core {

void InitLogger(const std::string& logFilePath, bool mirrorToStdout);
void ShutdownLogger();
const std::string& LogFilePath();

void Log(const char* level, const char* fmt, ...);
void LogRaw(const char* text, size_t len);

#define LOG_INFO(fmt, ...)  ::ps4::core::Log("INFO",  fmt, ##__VA_ARGS__)
#define LOG_WARN(fmt, ...)  ::ps4::core::Log("WARN",  fmt, ##__VA_ARGS__)
#define LOG_ERROR(fmt, ...) ::ps4::core::Log("ERROR", fmt, ##__VA_ARGS__)

}  // namespace ps4::core
