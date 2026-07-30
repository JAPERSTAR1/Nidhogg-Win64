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
        size_t amp = query.find('&', pos);
        std::string part = query.substr(pos, amp == std::string::npos ? std::string::npos : amp - pos);
        if (part.rfind(needle, 0) == 0) return part.substr(needle.size());
        if (amp == std::string::npos) break;
        pos = amp + 1;
    }
    return {};
}

std::string HeaderValueCaseInsensitive(const std::string& headers, const char* name) {
    std::string lowerH = headers;
    std::string lowerN = name;
    for (char& c : lowerH) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    for (char& c : lowerN) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    lowerN += ":";
    size_t pos = lowerH.find(lowerN);
    if (pos == std::string::npos) return {};
    pos += lowerN.size();
    while (pos < headers.size() && (headers[pos] == ' ' || headers[pos] == '\t')) ++pos;
    size_t end = headers.find("\r\n", pos);
    std::string val = headers.substr(pos, end == std::string::npos ? std::string::npos : end - pos);
    while (!val.empty() && (val.back() == ' ' || val.back() == '\r' || val.back() == '\n'))
        val.pop_back();
    return val;
}

void HandleWebSocketRelay(SOCKET clientSock, const std::string& query, const std::string& wsKey) {
    const uint64_t sid = std::strtoull(QueryParam(query, "session").c_str(), nullptr, 10);
    const int role = std::atoi(QueryParam(query, "role").c_str());
    const std::string token = QueryParam(query, "token");

    std::string roomId;
    {
        std::scoped_lock lk(g_mtx);
        auto sit = g_sessionToRoom.find(sid);
        if (sit == g_sessionToRoom.end() || (role != 1 && role != 2)) {
            SendHttpResponse(clientSock, 403, "Forbidden", "{\"error\":\"Invalid session\"}");
            closesocket(clientSock);
            return;
        }
        auto rit = g_rooms.find(sit->second);
        if (rit == g_rooms.end()) {
            SendHttpResponse(clientSock, 404, "Not Found", "{\"error\":\"Room not found\"}");
            closesocket(clientSock);
            return;
        }
        Room& rm = rit->second;
        const std::string& expectedToken = (role == 1) ? rm.hostToken : rm.guestToken;
        if (token.empty() || token != expectedToken) {
            SendHttpResponse(clientSock, 403, "Forbidden", "{\"error\":\"Bad auth token\"}");
            closesocket(clientSock);
            return;
        }
        roomId = rm.roomId;
    }

    // Complete RFC 6455 handshake
    const std::string concat = wsKey + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    uint8_t sha1[20]{};
    if (!Sha1Hash(reinterpret_cast<const uint8_t*>(concat.data()), concat.size(), sha1)) {
        closesocket(clientSock);
        return;
    }
    const std::string acceptKey = Base64Encode(sha1, 20);
    std::ostringstream resp;
    resp << "HTTP/1.1 101 Switching Protocols\r\n"
         << "Upgrade: websocket\r\n"
         << "Connection: Upgrade\r\n"
         << "Sec-WebSocket-Accept: " << acceptKey << "\r\n\r\n";
    const std::string rstr = resp.str();
    int noDelay = 1;
    setsockopt(clientSock, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&noDelay),
               sizeof(noDelay));
    DWORD rxTimeout = 45000;  // 45s idle timeout
    setsockopt(clientSock, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&rxTimeout),
               sizeof(rxTimeout));
    if (!SendAll(clientSock, rstr.data(), rstr.size())) {
        closesocket(clientSock);
        return;
    }

    {
        std::scoped_lock lk(g_mtx);
        auto rit = g_rooms.find(roomId);
        if (rit != g_rooms.end()) {
            SOCKET& slot = (role == 1) ? rit->second.wsHostSock : rit->second.wsGuestSock;
            if (slot != INVALID_SOCKET) closesocket(slot);
            slot = clientSock;
        }
    }
    std::printf("[ws-relay] connected session=%llu role=%d room=%s\n",
                static_cast<unsigned long long>(sid), role, roomId.c_str());

    // Frame forwarding loop
    std::vector<uint8_t> payload;
    while (g_running.load()) {
        uint8_t h2[2]{};
        if (!RecvExact(clientSock, h2, 2)) break;
        const uint8_t opcode = h2[0] & 0x0fu;
        const bool masked = (h2[1] & 0x80u) != 0;
        uint64_t plen = h2[1] & 0x7fu;
        if (plen == 126) {
            uint8_t ext[2]{};
            if (!RecvExact(clientSock, ext, 2)) break;
            plen = (static_cast<uint64_t>(ext[0]) << 8) | ext[1];
        } else if (plen == 127) {
            uint8_t ext[8]{};
            if (!RecvExact(clientSock, ext, 8)) break;
            plen = 0;
            for (int i = 0; i < 8; ++i) plen = (plen << 8) | ext[i];
        }
        if (plen > 4096) break;  // Gameplay frames are < 256 bytes

        uint8_t maskKey[4]{};
        if (masked && !RecvExact(clientSock, maskKey, 4)) break;
        payload.resize(static_cast<size_t>(plen));
        if (plen > 0 && !RecvExact(clientSock, payload.data(), static_cast<size_t>(plen))) break;
        if (masked) {
            for (size_t i = 0; i < payload.size(); ++i) payload[i] ^= maskKey[i & 3];
        }

        if (opcode == 0x8) {  // Close
            break;
        } else if (opcode == 0x9) {  // Ping -> Pong
            SendWsFrame(clientSock, 0xA, payload.data(), payload.size());
            continue;
        } else if (opcode == 0x2 || opcode == 0x1) {  // Binary / Text gameplay relay frame
            SOCKET peerSock = INVALID_SOCKET;
            {
                std::scoped_lock lk(g_mtx);
                auto rit = g_rooms.find(roomId);
                if (rit != g_rooms.end()) {
                    if (role == 1) rit->second.hostLastSeenMs = NowMs();
                    else rit->second.guestLastSeenMs = NowMs();
                    peerSock = (role == 1) ? rit->second.wsGuestSock : rit->second.wsHostSock;
                }
            }
            if (peerSock != INVALID_SOCKET && !payload.empty()) {
                SendWsFrame(peerSock, 0x2, payload.data(), payload.size());
            }
        }
    }

    {
        std::scoped_lock lk(g_mtx);
        auto rit = g_rooms.find(roomId);
        if (rit != g_rooms.end()) {
            SOCKET& slot = (role == 1) ? rit->second.wsHostSock : rit->second.wsGuestSock;
            if (slot == clientSock) slot = INVALID_SOCKET;
        }
    }
    closesocket(clientSock);
    std::printf("[ws-relay] disconnected session=%llu role=%d\n",
                static_cast<unsigned long long>(sid), role);
}

void UpdateCallerEndpointsFromJson(EndpointInfo& ep, const std::string& body,
                                   const std::string& fallbackIp) {
    std::string pubIp, lanIp;
    int64_t pubPort = 0, lanPort = 0;
    if (JsonGetStr(body, "publicIp", pubIp) && !pubIp.empty()) ep.publicIp = pubIp;
    else if (ep.publicIp.empty()) ep.publicIp = fallbackIp;
    if (JsonGetInt(body, "publicPort", pubPort) && pubPort > 0 && pubPort <= 65535)
        ep.publicPort = static_cast<uint16_t>(pubPort);
    if (JsonGetStr(body, "lanIp", lanIp) && !lanIp.empty()) ep.lanIp = lanIp;
    if (JsonGetInt(body, "lanPort", lanPort) && lanPort > 0 && lanPort <= 65535) {
        ep.lanPort = static_cast<uint16_t>(lanPort);
        if (ep.publicPort == 0) ep.publicPort = ep.lanPort;
    }
}

void HandleHttpClient(SOCKET s, std::string remoteIp) {
    int noDelay = 1;
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&noDelay), sizeof(noDelay));
    DWORD timeoutMs = 5000;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeoutMs), sizeof(timeoutMs));
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&timeoutMs), sizeof(timeoutMs));

    std::string req;
    char buf[4096];
    size_t headerEnd = std::string::npos;
    while (req.size() < 16384) {
        int n = recv(s, buf, sizeof(buf), 0);
        if (n <= 0) { closesocket(s); return; }
        req.append(buf, static_cast<size_t>(n));
        headerEnd = req.find("\r\n\r\n");
        if (headerEnd != std::string::npos) break;
    }
    if (headerEnd == std::string::npos) { closesocket(s); return; }

    const std::string headers = req.substr(0, headerEnd + 2);
    size_t lineEnd = headers.find("\r\n");
    const std::string reqLine = headers.substr(0, lineEnd);
    std::istringstream rl(reqLine);
    std::string method, target;
    rl >> method >> target;

    std::string path = target;
    std::string query;
    size_t qpos = target.find('?');
    if (qpos != std::string::npos) {
        path = target.substr(0, qpos);
        query = target.substr(qpos + 1);
    }

    // Prefer Cloudflare Tunnel's CF-Connecting-IP header when behind cloudflared
    const std::string cfIp = HeaderValueCaseInsensitive(headers, "CF-Connecting-IP");
    if (!cfIp.empty()) remoteIp = cfIp;

    // WebSocket Upgrade for `/ws`
    const std::string upgradeHdr = HeaderValueCaseInsensitive(headers, "Upgrade");
    if (path == "/ws" && _stricmp(upgradeHdr.c_str(), "websocket") == 0) {
        const std::string wsKey = HeaderValueCaseInsensitive(headers, "Sec-WebSocket-Key");
        if (wsKey.empty()) {
            SendHttpResponse(s, 400, "Bad Request", "{\"error\":\"Missing Sec-WebSocket-Key\"}");
            closesocket(s);
            return;
        }
        HandleWebSocketRelay(s, query, wsKey);
        return;
    }

    size_t contentLen = 0;
    const std::string clStr = HeaderValueCaseInsensitive(headers, "Content-Length");
    if (!clStr.empty()) contentLen = static_cast<size_t>(std::strtoull(clStr.c_str(), nullptr, 10));
    if (contentLen > 8192) {
        SendHttpResponse(s, 413, "Payload Too Large", "{\"error\":\"Request body too large\"}");
        closesocket(s);
        return;
    }

    std::string body = req.substr(headerEnd + 4);
    while (body.size() < contentLen) {
        int n = recv(s, buf, static_cast<int>(std::min<size_t>(sizeof(buf), contentLen - body.size())), 0);
        if (n <= 0) break;
        body.append(buf, static_cast<size_t>(n));
    }

    // Public health check (no version header required so browsers & Cloudflare checks succeed)
    if (method == "GET" && (path == "/" || path == "/api/health")) {
        size_t activeRooms = 0;
        {
            std::scoped_lock lk(g_mtx);
            PurgeExpiredRoomsLocked();
            activeRooms = g_rooms.size();
        }
        const uint64_t upSec = (NowMs() / 1000ULL) - g_startSec;
        std::ostringstream out;
        out << "{\"ok\":true,\"service\":\"nidhogg-lobby\",\"version\":\"" << kExpectedProtocol
            << "\",\"activeRooms\":" << activeRooms << ",\"uptimeSec\":" << upSec << "}";
        SendHttpResponse(s, 200, "OK", out.str());
        closesocket(s);
        return;
    }

    // Version check on `/api/*`
    const std::string clientVer = HeaderValueCaseInsensitive(headers, "X-Nidhogg-Version");
    if (!clientVer.empty() && clientVer != kExpectedProtocol) {
        SendHttpResponse(s, 426, "Upgrade Required",
                         std::string("{\"error\":\"Protocol version mismatch (server ") +
                             kExpectedProtocol + ", client " + JsonEsc(clientVer) + ")\"}");
        closesocket(s);
        return;
    }

    std::scoped_lock lk(g_mtx);
    PurgeExpiredRoomsLocked();

    const bool isCreate = (path == "/api/rooms/create");
    if (!CheckRateLimitLocked(remoteIp, isCreate)) {
        SendHttpResponse(s, 429, "Too Many Requests",
                         "{\"error\":\"Rate limit exceeded. Please wait a moment.\"}");
        closesocket(s);
        return;
    }

    if (method == "GET" && path == "/api/rooms") {
        const std::string regFilter = QueryParam(query, "region");
        const bool availOnly = (QueryParam(query, "available") == "1");
        std::ostringstream o;
        o << "{\"rooms\":[";
        bool first = true;
        for (const auto& [id, rm] : g_rooms) {
            if (!rm.isPublic || rm.status == "closed") continue;
            const int pCount = rm.guestConnected ? 2 : 1;
            if (availOnly && (pCount >= 2 || rm.status != "waiting")) continue;
            if (!regFilter.empty() && regFilter != "all" && rm.region != regFilter) continue;
            if (!first) o << ",";
            first = false;
            o << "{"
              << "\"roomId\":\"" << JsonEsc(rm.roomId) << "\","
              << "\"roomName\":\"" << JsonEsc(rm.roomName) << "\","
              << "\"region\":\"" << JsonEsc(rm.region) << "\","
              << "\"playerCount\":" << pCount << ","
              << "\"maxPlayers\":2,"
              << "\"stageIndex\":" << rm.stageIndex << ","
              << "\"status\":\"" << JsonEsc(rm.status) << "\""
              << "}";
        }
        o << "]}";
        SendHttpResponse(s, 200, "OK", o.str());
    } else if (method == "POST" && path == "/api/rooms/create") {
        if (static_cast<int>(g_rooms.size()) >= g_maxRooms) {
            SendHttpResponse(s, 503, "Service Unavailable", "{\"error\":\"Server room limit reached\"}");
            closesocket(s);
            return;
        }
        Room rm;
        rm.roomId = RandomHex(8);
        do {
            rm.roomCode = GenerateRoomCode();
        } while (std::any_of(g_rooms.begin(), g_rooms.end(),
                             [&](const auto& kv) { return kv.second.roomCode == rm.roomCode; }));

        std::string rName, hName, reg;
        JsonGetStr(body, "roomName", rName);
        JsonGetStr(body, "hostName", hName);
        JsonGetStr(body, "region", reg);
        JsonGetBool(body, "isPublic", rm.isPublic);
        int64_t stg = 0;
        if (JsonGetInt(body, "stageIndex", stg)) rm.stageIndex = static_cast<int>(stg & 3);

        rm.roomName = SanitizeText(rName, 32, "Nidhogg Arena");
        rm.hostName = SanitizeText(hName, 20, "Host");
        rm.region = SanitizeText(reg, 16, "us-east");
        rm.hostToken = RandomHex(16);
        rm.hostLastSeenMs = NowMs();

        CsPrng(&rm.sessionId, sizeof(rm.sessionId));
        if (rm.sessionId == 0) rm.sessionId = 1;
        rm.sessionKeyHex = RandomHex(32);
        CsPrng(&rm.matchSeed, sizeof(rm.matchSeed));
        if (rm.matchSeed == 0) rm.matchSeed = 0x13572468u;

        UpdateCallerEndpointsFromJson(rm.hostEp, body, remoteIp);

        const std::string reply = SerializeRoomForCallerLocked(rm, true, rm.hostToken);
        g_sessionToRoom[rm.sessionId] = rm.roomId;
        std::printf("[lobby] created %s room id=%s code=%s name=\"%s\" host=%s (%s:%u)\n",
                    rm.isPublic ? "PUBLIC" : "PRIVATE", rm.roomId.c_str(), rm.roomCode.c_str(),
                    rm.roomName.c_str(), rm.hostName.c_str(),
                    rm.hostEp.publicIp.c_str(), rm.hostEp.publicPort);
        g_rooms.emplace(rm.roomId, std::move(rm));
        SendHttpResponse(s, 200, "OK", reply);
    } else if (method == "POST" && (path == "/api/rooms/join" || path == "/api/rooms/join_code")) {
        Room* targetRoom = nullptr;
        if (path == "/api/rooms/join") {
            std::string rid;
            JsonGetStr(body, "roomId", rid);
            auto it = g_rooms.find(rid);
            if (it != g_rooms.end() && it->second.isPublic) targetRoom = &it->second;
        } else {
            std::string code;
            JsonGetStr(body, "roomCode", code);
            for (char& c : code) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
            for (auto& [id, rm] : g_rooms) {
                if (rm.roomCode == code && rm.status != "closed") {
                    targetRoom = &rm;
                    break;
                }
            }
        }

        if (!targetRoom) {
            SendHttpResponse(s, 404, "Not Found", "{\"error\":\"Room not found or invalid code.\"}");
        } else if (targetRoom->guestConnected || targetRoom->status != "waiting") {
            SendHttpResponse(s, 409, "Conflict", "{\"error\":\"Room is full (2/2) or already in match.\"}");
        } else {
            std::string pName;
            JsonGetStr(body, "playerName", pName);
            targetRoom->guestConnected = true;
            targetRoom->guestName = SanitizeText(pName, 20, "Challenger");
            targetRoom->guestToken = RandomHex(16);
            targetRoom->guestReady = false;
            targetRoom->guestLastSeenMs = NowMs();
            UpdateCallerEndpointsFromJson(targetRoom->guestEp, body, remoteIp);
            std::printf("[lobby] %s joined room %s (%s)\n", targetRoom->guestName.c_str(),
                        targetRoom->roomId.c_str(), targetRoom->roomCode.c_str());
            SendHttpResponse(s, 200, "OK",
                             SerializeRoomForCallerLocked(*targetRoom, false, targetRoom->guestToken));
        }
    } else if (method == "POST" && path == "/api/quickmatch") {
        std::string wantReg, pName;
        JsonGetStr(body, "region", wantReg);
        JsonGetStr(body, "playerName", pName);

        Room* best = nullptr;
        for (auto& [id, rm] : g_rooms) {
            if (!rm.isPublic || rm.guestConnected || rm.status != "waiting") continue;
            if (!best) best = &rm;
            else if (rm.region == wantReg && best->region != wantReg) best = &rm;
        }
        if (!best) {
            SendHttpResponse(s, 404, "Not Found", "{\"error\":\"No open public rooms found.\"}");
        } else {
            best->guestConnected = true;
            best->guestName = SanitizeText(pName, 20, "Challenger");
            best->guestToken = RandomHex(16);
            best->guestReady = false;
            best->guestLastSeenMs = NowMs();
            UpdateCallerEndpointsFromJson(best->guestEp, body, remoteIp);
            std::printf("[lobby] quickmatch paired %s -> room %s\n",
                        best->guestName.c_str(), best->roomId.c_str());
            SendHttpResponse(s, 200, "OK",
                             SerializeRoomForCallerLocked(*best, false, best->guestToken));
        }
    } else if (method == "POST" && path == "/api/rooms/visibility") {
        std::string rid, tok;
        bool wantPublic = true;
        JsonGetStr(body, "roomId", rid);
        JsonGetStr(body, "authToken", tok);
        JsonGetBool(body, "isPublic", wantPublic);
        auto it = g_rooms.find(rid);
        if (it == g_rooms.end() || tok != it->second.hostToken) {
            SendHttpResponse(s, 403, "Forbidden", "{\"error\":\"Unauthorized\"}");
        } else {
            it->second.isPublic = wantPublic;
            it->second.hostLastSeenMs = NowMs();
            std::printf("[lobby] room %s visibility -> %s\n", rid.c_str(),
                        wantPublic ? "PUBLIC" : "PRIVATE");
            SendHttpResponse(s, 200, "OK", SerializeRoomForCallerLocked(it->second, true));
        }
    } else if (method == "POST" && path == "/api/rooms/ready") {
        std::string rid, tok;
        bool rdy = false;
        JsonGetStr(body, "roomId", rid);
        JsonGetStr(body, "authToken", tok);
        JsonGetBool(body, "ready", rdy);
        auto it = g_rooms.find(rid);
        if (it == g_rooms.end()) {
            SendHttpResponse(s, 404, "Not Found", "{\"error\":\"Room not found\"}");
        } else if (tok == it->second.hostToken) {
