#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include "websocket.h"
#include "netplay.h"
#include "../core/settings.h"
#include "../core/logger.h"
#include <windows.h>
#include <winhttp.h>

namespace ps4::net {
WebSocketRelay::~WebSocketRelay() { Stop(); }
void WebSocketRelay::Stop() {
    stop_ = true;
    { std::lock_guard lock(mutex_);
      if (socket_) { WinHttpCloseHandle(socket_); socket_ = nullptr; } }
    cv_.notify_all();
    if (reader_.joinable()) reader_.join();
    connected_ = false;
    std::lock_guard lock(mutex_); tx_.clear(); rx_.clear();
}
void WebSocketRelay::Start(const std::string& url, const std::string& room, const std::string& token) {
    Stop(); stop_ = false;
    { std::lock_guard lock(mutex_); error_.clear(); }
    reader_ = std::thread(&WebSocketRelay::Run, this, url, room, token);
}
bool WebSocketRelay::Send(const std::vector<uint8_t>& packet) {
    std::lock_guard lock(mutex_);
    if (!connected_) return false;
    if (tx_.size() >= 256) { error_ = "Relay send queue full"; return false; }
    tx_.push_back(packet); cv_.notify_one(); return true;
}
bool WebSocketRelay::Receive(std::vector<uint8_t>& packet) {
    std::lock_guard lock(mutex_);
    if (rx_.empty()) return false;
    packet = std::move(rx_.front()); rx_.pop_front(); return true;
}
std::string WebSocketRelay::Error() { std::lock_guard lock(mutex_); return error_; }
void WebSocketRelay::Run(std::string url, std::string room, std::string token) {
    HINTERNET session = nullptr, connection = nullptr, request = nullptr, ws = nullptr;
    DWORD error = ERROR_SUCCESS;
    do {
        const auto wide = core::Utf8ToWide(url);
        URL_COMPONENTS parts{}; parts.dwStructSize = sizeof(parts);
        parts.dwHostNameLength = static_cast<DWORD>(-1); parts.dwUrlPathLength = static_cast<DWORD>(-1);
        if (!WinHttpCrackUrl(wide.c_str(), 0, 0, &parts)) { error = GetLastError(); break; }
        std::wstring host(parts.lpszHostName, parts.dwHostNameLength);
        session = WinHttpOpen(L"NidhoggPort/2.0", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
            WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
        if (!session) { error = GetLastError(); break; }
        WinHttpSetTimeouts(session, 2000, 2000, 2000, 3000);
        connection = WinHttpConnect(session, host.c_str(), parts.nPort, 0);
        if (!connection) { error = GetLastError(); break; }
        const auto path = core::Utf8ToWide("/relay?roomId=" + room);
        request = WinHttpOpenRequest(connection, L"GET", path.c_str(), nullptr,
            WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
            parts.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0);
        if (!request) { error = GetLastError(); break; }
        WinHttpSetOption(request, WINHTTP_OPTION_UPGRADE_TO_WEB_SOCKET, nullptr, 0);
        // Timeout belongs to the request and is inherited by the upgraded socket.
        // A socket handle does not accept WINHTTP_OPTION_RECEIVE_TIMEOUT.
        const auto headers = core::Utf8ToWide(std::string("X-Nidhogg-Version: ") + kProtocolVersion +
            "\r\nX-Nidhogg-Token: " + token + "\r\n");
        if (!WinHttpSendRequest(request, headers.c_str(), static_cast<DWORD>(headers.size()),
                nullptr, 0, 0, 0) || !WinHttpReceiveResponse(request, nullptr)) { error = GetLastError(); break; }
        DWORD status = 0, size = sizeof(status);
        WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
            WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX);
        if (status != 101) { error = ERROR_WINHTTP_INVALID_SERVER_RESPONSE; break; }
        WinHttpSetTimeouts(request, 2000, 2000, 2000, 0);
        ws = WinHttpWebSocketCompleteUpgrade(request, 0);
        if (!ws) { error = GetLastError(); break; }
        WinHttpCloseHandle(request); request = nullptr;
        // Receive remains blocked until data/close; only the sender handles outbound writes.
        { std::lock_guard lock(mutex_);
          if (stop_) { WinHttpCloseHandle(ws); ws = nullptr; break; }
          socket_ = ws; connected_ = true; }
        std::thread sender([&, ws] {
            while (!stop_ && connected_) {
                std::vector<uint8_t> data;
                { std::unique_lock lock(mutex_);
                  cv_.wait(lock, [&] { return stop_ || !connected_ || !tx_.empty(); });
                  if (stop_ || !connected_) break;
                  data = std::move(tx_.front()); tx_.pop_front(); }
                if (WinHttpWebSocketSend(ws, WINHTTP_WEB_SOCKET_BINARY_MESSAGE_BUFFER_TYPE,
                    data.data(), static_cast<DWORD>(data.size())) != NO_ERROR) {
                    connected_ = false;
                    std::lock_guard lock(mutex_);
                    if (socket_) { WinHttpCloseHandle(socket_); socket_ = nullptr; }
                    break;
                }
            }
        });
        std::vector<uint8_t> packet;
        while (!stop_ && connected_) {
            uint8_t buffer[2048]; DWORD read = 0;
            WINHTTP_WEB_SOCKET_BUFFER_TYPE type{};
            error = WinHttpWebSocketReceive(ws, buffer, sizeof(buffer), &read, &type);
            if (error || type == WINHTTP_WEB_SOCKET_CLOSE_BUFFER_TYPE) break;
            if ((type != WINHTTP_WEB_SOCKET_BINARY_FRAGMENT_BUFFER_TYPE &&
                 type != WINHTTP_WEB_SOCKET_BINARY_MESSAGE_BUFFER_TYPE) || packet.size() + read > 2048) {
                error = ERROR_INVALID_DATA; break;
            }
            packet.insert(packet.end(), buffer, buffer + read);
            if (type == WINHTTP_WEB_SOCKET_BINARY_MESSAGE_BUFFER_TYPE) {
                std::lock_guard lock(mutex_);
                if (rx_.size() >= 256) { error = ERROR_BUFFER_OVERFLOW; break; }
                rx_.push_back(std::move(packet)); packet.clear();
            }
        }
        connected_ = false; cv_.notify_all();
        { std::lock_guard lock(mutex_);
          if (socket_) { WinHttpCloseHandle(socket_); socket_ = nullptr; } }
        sender.join(); ws = nullptr;
    } while (false);
    connected_ = false;
    if (request) WinHttpCloseHandle(request);
    if (connection) WinHttpCloseHandle(connection);
    if (session) WinHttpCloseHandle(session);
    if (!stop_) { std::lock_guard lock(mutex_);
      error_ = "WebSocket relay disconnected (WinHTTP " + std::to_string(error) + ")";
      LOG_WARN("[netplay] %s", error_.c_str()); }
}
}
