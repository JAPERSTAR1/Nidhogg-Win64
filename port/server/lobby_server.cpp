// Nidhogg Native Multiplayer Lobby, Matchmaking, Signaling & WebSocket/UDP Relay Server.
//
// Single lightweight Windows x64 executable (`NidhoggServer.exe`) using only
// Winsock2 (`ws2_32.lib`) and CNG (`bcrypt.lib`).
//
// Exposes on a single TCP port (default 27880, Cloudflare Tunnel compatible):
//   - GET  /                   status page / health JSON
//   - GET  /api/health         health & metrics JSON
//   - GET  /api/rooms          public room browser (never exposes private rooms or codes)
//   - POST /api/rooms/create   create Public or Private room (max 2 players)
//   - POST /api/rooms/join     join Public room by roomId
//   - POST /api/rooms/join_code join Private/Public room by 6-char room code
//   - POST /api/quickmatch     join best open Public room or return 404
//   - POST /api/rooms/visibility live Public <-> Private toggle (host only)
//   - POST /api/rooms/ready    set Ready / Not Ready + update STUN endpoint
//   - POST /api/rooms/kick     kick guest (host only)
//   - POST /api/rooms/start    begin match (host only, requires 2 players)
//   - POST /api/rooms/heartbeat keep-alive & state poll
//   - POST /api/rooms/leave    disconnect / close room
//   - GET  /ws                 RFC 6455 Binary WebSocket low-latency gameplay relay
//
// Also binds an optional companion UDP port (default 27881) for direct LAN/WAN
// UDP endpoint reflection and raw UDP relay when reachable directly.
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <bcrypt.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "bcrypt.lib")

namespace {

constexpr const char* kExpectedProtocol = "nidhogg-win64-1.0";
constexpr uint32_t kWireMagic = 0x504E484Eu;  // "NHNP"
constexpr uint8_t kPktRegisterEndpoint = 1;
constexpr uint8_t kPktRelayEnvelope = 0x52;

std::atomic<bool> g_running{true};
uint64_t g_startSec = 0;

uint64_t NowMs() { return GetTickCount64(); }

bool CsPrng(void* out, size_t len) {
    return BCryptGenRandom(nullptr, static_cast<PUCHAR>(out), static_cast<ULONG>(len),
                           BCRYPT_USE_SYSTEM_PREFERRED_RNG) == 0;
}

std::string RandomHex(size_t bytes) {
    std::vector<uint8_t> b(bytes);
    CsPrng(b.data(), b.size());
    static const char kH[] = "0123456789abcdef";
    std::string s;
    s.reserve(bytes * 2);
    for (uint8_t x : b) {
        s.push_back(kH[x >> 4]);
        s.push_back(kH[x & 0xf]);
    }
    return s;
}

std::string GenerateRoomCode() {
    static const char kAlphabet[] = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789";
    uint8_t b[6]{};
    CsPrng(b, sizeof(b));
    std::string code(6, 'A');
    for (int i = 0; i < 6; ++i) code[i] = kAlphabet[b[i] & 31];
    return code;
}

bool Sha1Hash(const uint8_t* data, size_t len, uint8_t out20[20]) {
    BCRYPT_ALG_HANDLE hAlg = nullptr;
    if (BCryptOpenAlgorithmProvider(&hAlg, BCRYPT_SHA1_ALGORITHM, nullptr, 0) != 0) return false;
    BCRYPT_HASH_HANDLE hHash = nullptr;
    bool ok = false;
    if (BCryptCreateHash(hAlg, &hHash, nullptr, 0, nullptr, 0, 0) == 0) {
        if (BCryptHashData(hHash, const_cast<PUCHAR>(data), static_cast<ULONG>(len), 0) == 0 &&
            BCryptFinishHash(hHash, out20, 20, 0) == 0) {
            ok = true;
        }
        BCryptDestroyHash(hHash);
    }
    BCryptCloseAlgorithmProvider(hAlg, 0);
    return ok;
}

std::string Base64Encode(const uint8_t* data, size_t len) {
    static const char kB64[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve(((len + 2) / 3) * 4);
    for (size_t i = 0; i < len; i += 3) {
        uint32_t v = static_cast<uint32_t>(data[i]) << 16;
        if (i + 1 < len) v |= static_cast<uint32_t>(data[i + 1]) << 8;
        if (i + 2 < len) v |= static_cast<uint32_t>(data[i + 2]);
        out.push_back(kB64[(v >> 18) & 63]);
        out.push_back(kB64[(v >> 12) & 63]);
        out.push_back((i + 1 < len) ? kB64[(v >> 6) & 63] : '=');
        out.push_back((i + 2 < len) ? kB64[v & 63] : '=');
    }
    return out;
}

std::string SanitizeText(const std::string& s, size_t maxLen, const char* fallback) {
    std::string out;
    for (unsigned char c : s) {
        if (c >= 32 && c < 127 && c != '"' && c != '\\' && c != '<' && c != '>') {
            out.push_back(static_cast<char>(c));
            if (out.size() >= maxLen) break;
        }
    }
    while (!out.empty() && out.back() == ' ') out.pop_back();
    return out.empty() ? std::string(fallback) : out;
}

std::string JsonEsc(const std::string& s) {
    std::string o;
    for (unsigned char c : s) {
        if (c == '"') o += "\\\"";
        else if (c == '\\') o += "\\\\";
        else if (c >= 32 && c < 127) o.push_back(static_cast<char>(c));
    }
    return o;
}

bool JsonGetStr(const std::string& j, const char* key, std::string& out) {
    std::string needle = std::string("\"") + key + "\"";
    size_t p = j.find(needle);
    if (p == std::string::npos) return false;
    p = j.find(':', p + needle.size());
    if (p == std::string::npos) return false;
    ++p;
    while (p < j.size() && std::isspace(static_cast<unsigned char>(j[p]))) ++p;
    if (p >= j.size() || j[p] != '"') return false;
    ++p;
    std::string r;
    while (p < j.size()) {
        char c = j[p++];
        if (c == '"') { out = r; return true; }
        if (c == '\\' && p < j.size()) r.push_back(j[p++]);
        else r.push_back(c);
    }
    return false;
}

bool JsonGetInt(const std::string& j, const char* key, int64_t& out) {
    std::string needle = std::string("\"") + key + "\"";
    size_t p = j.find(needle);
    if (p == std::string::npos) return false;
    p = j.find(':', p + needle.size());
    if (p == std::string::npos) return false;
    ++p;
    while (p < j.size() && std::isspace(static_cast<unsigned char>(j[p]))) ++p;
    if (p >= j.size()) return false;
    char* end = nullptr;
    long long v = std::strtoll(j.c_str() + p, &end, 10);
    if (end == j.c_str() + p) return false;
    out = v;
    return true;
}

bool JsonGetBool(const std::string& j, const char* key, bool& out) {
    std::string needle = std::string("\"") + key + "\"";
    size_t p = j.find(needle);
    if (p == std::string::npos) return false;
    p = j.find(':', p + needle.size());
    if (p == std::string::npos) return false;
    ++p;
    while (p < j.size() && std::isspace(static_cast<unsigned char>(j[p]))) ++p;
    if (j.compare(p, 4, "true") == 0) { out = true; return true; }
    if (j.compare(p, 5, "false") == 0) { out = false; return true; }
    return false;
}

struct EndpointInfo {
    std::string publicIp;
    uint16_t publicPort = 0;
    std::string lanIp;
    uint16_t lanPort = 0;
    sockaddr_in udpObserved{};
    bool hasUdpObserved = false;
};

struct Room {
    std::string roomId;
    std::string roomCode;
    std::string roomName;
    std::string region;
    bool isPublic = true;
    int stageIndex = 0;
    std::string status = "waiting";  // "waiting", "in_match", "closed"

    uint64_t sessionId = 0;
    std::string sessionKeyHex;       // 64 hex chars = 32 bytes
    uint32_t matchSeed = 0;

    std::string hostName;
    std::string hostToken;
    bool hostReady = false;
    uint64_t hostLastSeenMs = 0;
    EndpointInfo hostEp;

    bool guestConnected = false;
    std::string guestName;
    std::string guestToken;
    std::string kickedToken;
    bool guestReady = false;
    uint64_t guestLastSeenMs = 0;
    EndpointInfo guestEp;

    // Active WebSocket relay client sockets (INVALID_SOCKET if not connected)
    SOCKET wsHostSock = INVALID_SOCKET;
    SOCKET wsGuestSock = INVALID_SOCKET;
};

struct RateBucket {
    uint64_t windowStartMs = 0;
    int apiCount = 0;
    uint64_t createWindowMs = 0;
    int createCount = 0;
};

std::mutex g_mtx;
std::mutex g_wsSendMtx;
std::unordered_map<std::string, Room> g_rooms;
std::unordered_map<uint64_t, std::string> g_sessionToRoom;
std::unordered_map<std::string, RateBucket> g_rates;
uint16_t g_httpPort = 27880;
uint16_t g_udpPort = 27881;
int g_maxRooms = 512;

void PurgeExpiredRoomsLocked() {
    const uint64_t now = NowMs();
    constexpr uint64_t kTimeoutMs = 30000ULL;
    for (auto it = g_rooms.begin(); it != g_rooms.end();) {
        Room& rm = it->second;
        if (rm.status == "closed" || (now - rm.hostLastSeenMs > kTimeoutMs)) {
            g_sessionToRoom.erase(rm.sessionId);
            it = g_rooms.erase(it);
            continue;
        }
        if (rm.guestConnected && (now - rm.guestLastSeenMs > kTimeoutMs)) {
            rm.guestConnected = false;
            rm.guestName.clear();
            rm.guestToken.clear();
            rm.guestReady = false;
            rm.guestEp = EndpointInfo{};
            if (rm.status == "in_match") rm.status = "waiting";
        }
        ++it;
    }
}

bool CheckRateLimitLocked(const std::string& ip, bool isCreate) {
    const uint64_t now = NowMs();
    auto& b = g_rates[ip];
    if (now - b.windowStartMs > 10000ULL) {
        b.windowStartMs = now;
        b.apiCount = 0;
    }
    if (++b.apiCount > 180) return false;
    if (isCreate) {
        if (now - b.createWindowMs > 60000ULL) {
            b.createWindowMs = now;
            b.createCount = 0;
        }
        if (++b.createCount > 20) return false;
    }
    return true;
}

std::string SerializeRoomForCallerLocked(const Room& rm, bool callerIsHost,
                                         const std::string& includeAuthToken = {}) {
    const EndpointInfo& peer = callerIsHost ? rm.guestEp : rm.hostEp;
    const int playerCount = rm.guestConnected ? 2 : 1;
    std::ostringstream o;
    o << "{"
      << "\"roomId\":\"" << JsonEsc(rm.roomId) << "\","
      << "\"roomName\":\"" << JsonEsc(rm.roomName) << "\","
      << "\"roomCode\":\"" << JsonEsc(rm.roomCode) << "\","
      << "\"region\":\"" << JsonEsc(rm.region) << "\","
      << "\"isPublic\":" << (rm.isPublic ? "true" : "false") << ","
      << "\"status\":\"" << JsonEsc(rm.status) << "\","
      << "\"stageIndex\":" << rm.stageIndex << ","
      << "\"playerCount\":" << playerCount << ","
      << "\"maxPlayers\":2,"
      << "\"hostName\":\"" << JsonEsc(rm.hostName) << "\","
      << "\"guestName\":\"" << JsonEsc(rm.guestName) << "\","
      << "\"guestConnected\":" << (rm.guestConnected ? "true" : "false") << ","
      << "\"hostReady\":" << (rm.hostReady ? "true" : "false") << ","
      << "\"guestReady\":" << (rm.guestReady ? "true" : "false") << ","
      << "\"sessionId\":" << rm.sessionId << ","
      << "\"sessionKey\":\"" << rm.sessionKeyHex << "\","
      << "\"matchSeed\":" << rm.matchSeed << ","
      << "\"relayUdpPort\":" << g_udpPort << ","
      << "\"peerPublicIp\":\"" << JsonEsc(peer.publicIp) << "\","
      << "\"peerPublicPort\":" << peer.publicPort << ","
      << "\"peerLanIp\":\"" << JsonEsc(peer.lanIp) << "\","
      << "\"peerLanPort\":" << peer.lanPort;
    if (!includeAuthToken.empty()) {
        o << ",\"authToken\":\"" << JsonEsc(includeAuthToken) << "\"";
    }
    o << "}";
    return o.str();
}

bool SendAll(SOCKET s, const void* data, size_t len) {
    const auto* p = static_cast<const char*>(data);
    size_t sent = 0;
    while (sent < len) {
        int n = send(s, p + sent, static_cast<int>(len - sent), 0);
        if (n <= 0) return false;
        sent += static_cast<size_t>(n);
    }
    return true;
}

void SendHttpResponse(SOCKET s, int status, const char* statusText, const std::string& body,
                      const char* contentType = "application/json") {
    std::ostringstream hdr;
    hdr << "HTTP/1.1 " << status << " " << statusText << "\r\n"
        << "Content-Type: " << contentType << "\r\n"
        << "Content-Length: " << body.size() << "\r\n"
        << "Connection: close\r\n"
        << "Cache-Control: no-store\r\n\r\n";
    const std::string h = hdr.str();
    SendAll(s, h.data(), h.size());
    if (!body.empty()) SendAll(s, body.data(), body.size());
}

// Sends one unmasked RFC 6455 binary WebSocket frame (server -> client).
bool SendWsFrame(SOCKET s, uint8_t opcode, const uint8_t* payload, size_t len) {
    if (s == INVALID_SOCKET) return false;
    uint8_t hdr[10]{};
    size_t hdrLen = 0;
    hdr[0] = static_cast<uint8_t>(0x80u | (opcode & 0x0fu));  // FIN + opcode
    if (len < 126) {
        hdr[1] = static_cast<uint8_t>(len);
        hdrLen = 2;
    } else if (len <= 65535) {
        hdr[1] = 126;
        hdr[2] = static_cast<uint8_t>((len >> 8) & 0xffu);
        hdr[3] = static_cast<uint8_t>(len & 0xffu);
        hdrLen = 4;
    } else {
        return false;
    }
    std::scoped_lock lk(g_wsSendMtx);
    if (!SendAll(s, hdr, hdrLen)) return false;
    return len == 0 || SendAll(s, payload, len);
}

bool RecvExact(SOCKET s, void* buf, size_t len) {
    auto* p = static_cast<char*>(buf);
    size_t got = 0;
    while (got < len) {
        int n = recv(s, p + got, static_cast<int>(len - got), 0);
        if (n <= 0) return false;
        got += static_cast<size_t>(n);
    }
    return true;
}

std::string QueryParam(const std::string& query, const char* key) {
    std::string needle = std::string(key) + "=";
    size_t pos = 0;
    while (pos < query.size()) {
