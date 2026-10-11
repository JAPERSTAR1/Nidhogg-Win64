#define WIN32_LEAN_AND_MEAN
#include "stun.h"
#include "crypto.h"
#include <ws2tcpip.h>
#include <windows.h>
#include <cstring>
namespace ps4::net {
bool DiscoverStunEndpoint(SOCKET socket, std::string& ip, uint16_t& port) {
    addrinfo hint{}; hint.ai_family = AF_INET; hint.ai_socktype = SOCK_DGRAM;
    addrinfo* result = nullptr;
    if (getaddrinfo("stun.l.google.com", "19302", &hint, &result) != 0 || !result) return false;
    sockaddr_in server = *reinterpret_cast<sockaddr_in*>(result->ai_addr); freeaddrinfo(result);
    uint8_t request[20] = {0, 1, 0, 0, 0x21, 0x12, 0xa4, 0x42};
    if (!RandomBytes(request + 8, 12)) return false;
    const auto started = GetTickCount64();
    for (int attempt = 0; attempt < 2; ++attempt) {
        sendto(socket, reinterpret_cast<char*>(request), sizeof(request), 0,
            reinterpret_cast<sockaddr*>(&server), sizeof(server));
        while (GetTickCount64() - started < static_cast<ULONGLONG>((attempt + 1) * 400)) {
            uint8_t data[1024]; sockaddr_in from{}; int fromSize = sizeof(from);
            const int size = recvfrom(socket, reinterpret_cast<char*>(data), sizeof(data), 0,
                reinterpret_cast<sockaddr*>(&from), &fromSize);
            if (size < 0) { Sleep(2); continue; }
            if (size < 20 || from.sin_addr.s_addr != server.sin_addr.s_addr || from.sin_port != server.sin_port ||
                data[0] != 1 || data[1] != 1 || std::memcmp(data + 4, request + 4, 16) != 0) continue;
            const size_t end = 20 + (static_cast<size_t>(data[2]) << 8) + data[3];
            if (end > static_cast<size_t>(size)) continue;
            for (size_t pos = 20; pos + 4 <= end;) {
                const unsigned type = (data[pos] << 8) | data[pos + 1];
                const size_t len = (static_cast<size_t>(data[pos + 2]) << 8) | data[pos + 3];
                if (pos + 4 + len > end) break;
                const uint8_t* value = data + pos + 4;
                if (type == 0x20 && len == 8 && value[1] == 1) {
                    port = static_cast<uint16_t>(((value[2] << 8) | value[3]) ^ 0x2112);
                    uint8_t address[4];
                    for (int i = 0; i < 4; ++i) address[i] = value[4 + i] ^ request[4 + i];
                    char text[INET_ADDRSTRLEN]{}; inet_ntop(AF_INET, address, text, sizeof(text));
                    ip = text; return port != 0;
                }
                pos += 4 + ((len + 3) & ~size_t(3));
            }
        }
    }
    return false; // WSS relay remains usable on networks blocking STUN/UDP.
}
}
