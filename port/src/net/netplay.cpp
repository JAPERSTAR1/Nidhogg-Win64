#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include "netplay.h"
#include "crypto.h"

#include "../core/logger.h"
#include "../core/settings.h"
#include "../hle.h"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <winhttp.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
#include <sstream>
#include <thread>
#include <unordered_map>

namespace ps4::net {
namespace {

constexpr uint32_t kWireMagic = 0x504E484Eu;  // "NHNP"
constexpr uint8_t kPktRegisterEndpoint = 1;
constexpr uint8_t kPktPunch = 2;
constexpr uint8_t kPktPunchAck = 3;
constexpr uint8_t kPktSyncBarrier = 4;
constexpr uint8_t kPktInput = 5;
constexpr uint8_t kPktDisconnect = 6;
constexpr uint8_t kPktRelayEnvelope = 0x52;

#pragma pack(push, 1)
struct WireAad {
    uint32_t magic = kWireMagic;
    uint8_t type = 0;
    uint8_t senderRole = 0;  // 1 = Host, 2 = Guest
    uint16_t reserved = 0;
    uint64_t sessionId = 0;
};

struct SyncBarrierPayload {
    uint8_t atLevelSelect = 0;
    uint8_t ackPeerBarrier = 0;
    uint8_t stageIndex = 0;
    uint8_t inputDelay = 2;
    uint32_t matchSeed = 0;
    uint64_t sendTimeUsec = 0;
    uint64_t echoTimeUsec = 0;
};

struct InputPayload {
    uint64_t latestFrame = 0;
    uint8_t historyCount = 0;
    uint8_t flags = 0;         // bit 0: hasStateHash
    uint16_t pad = 0;
    uint32_t buttonsHistory[16]{};
    uint64_t stateHashFrame = 0;
    uint64_t stateHash = 0;
    uint64_t sendTimeUsec = 0;
    uint64_t echoTimeUsec = 0;
};
#pragma pack(pop)

struct DelayedPacket {
    uint64_t deliverAtUsec = 0;
    bool viaWebSocket = false;
    sockaddr_in dst{};
    std::vector<uint8_t> bytes;
};

uint64_t MonoUsec() {
    static LARGE_INTEGER freq{};
    static LARGE_INTEGER start{};
    if (!freq.QuadPart) {
        QueryPerformanceFrequency(&freq);
        QueryPerformanceCounter(&start);
    }
    LARGE_INTEGER now{};
    QueryPerformanceCounter(&now);
    return static_cast<uint64_t>((now.QuadPart - start.QuadPart) * 1000000ULL / freq.QuadPart);
}

std::string JsonEsc(const std::string& s) {
    std::string o;
    for (unsigned char c : s) {
        if (c == '"') o += "\\\"";
        else if (c == '\\') o += "\\\\";
        else if (c >= 0x20) o.push_back(static_cast<char>(c));
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
    if (j[p] == '"') ++p;
    char* end = nullptr;
    long long v = std::strtoll(j.c_str() + p, &end, 10);
    if (end == j.c_str() + p) return false;
    out = v;
    return true;
}

bool JsonGetU64(const std::string& j, const char* key, uint64_t& out) {
    std::string needle = std::string("\"") + key + "\"";
    size_t p = j.find(needle);
    if (p == std::string::npos) return false;
    p = j.find(':', p + needle.size());
    if (p == std::string::npos) return false;
    ++p;
    while (p < j.size() && std::isspace(static_cast<unsigned char>(j[p]))) ++p;
    if (p >= j.size()) return false;
    if (j[p] == '"') ++p;
    char* end = nullptr;
    unsigned long long v = std::strtoull(j.c_str() + p, &end, 10);
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

struct ParsedUrl {
    bool https = false;
    std::wstring host = L"127.0.0.1";
    std::string hostUtf8 = "127.0.0.1";
    uint16_t port = 27880;
};

ParsedUrl ParseServerUrl(const std::string& url) {
    ParsedUrl u;
    std::string s = url;
    if (s.rfind("https://", 0) == 0) { u.https = true; u.port = 443; s.erase(0, 8); }
    else if (s.rfind("wss://", 0) == 0) { u.https = true; u.port = 443; s.erase(0, 6); }
    else if (s.rfind("http://", 0) == 0) { u.https = false; u.port = 80; s.erase(0, 7); }
    else if (s.rfind("ws://", 0) == 0) { u.https = false; u.port = 80; s.erase(0, 5); }
    size_t slash = s.find('/');
    if (slash != std::string::npos) s.resize(slash);
    size_t colon = s.find(':');
    if (colon != std::string::npos) {
        u.hostUtf8 = s.substr(0, colon);
        int p = std::atoi(s.substr(colon + 1).c_str());
        if (p > 0 && p <= 65535) u.port = static_cast<uint16_t>(p);
    } else if (!s.empty()) {
        u.hostUtf8 = s;
    }
    if (u.hostUtf8.empty()) u.hostUtf8 = "127.0.0.1";
    u.host = core::Utf8ToWide(u.hostUtf8);
    return u;
}

std::string EffectiveProtocolVersion() {
    char* ov = nullptr;
    size_t len = 0;
    if (_dupenv_s(&ov, &len, "NIDHOGG_PROTOCOL_OVERRIDE") == 0 && ov && *ov) {
        std::string s(ov);
        std::free(ov);
        return s;
    }
    if (ov) std::free(ov);
    return kProtocolVersion;
}

bool HttpRequestJsonAt(const std::string& baseUrl, const std::string& method,
                       const std::string& path, const std::string& body, int timeoutMs,
                       int& outStatus, std::string& outResponse, std::string& outErr,
                       uint32_t* outRttMs = nullptr) {
    const ParsedUrl u = ParseServerUrl(baseUrl);
    const uint64_t t0 = MonoUsec();

    HINTERNET hSession = WinHttpOpen(L"NidhoggPort/1.0", WINHTTP_ACCESS_TYPE_NO_PROXY,
                                     WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSession) { outErr = "WinHttpOpen failed"; return false; }
    WinHttpSetTimeouts(hSession, timeoutMs, timeoutMs, timeoutMs, timeoutMs);

    HINTERNET hConnect = WinHttpConnect(hSession, u.host.c_str(), u.port, 0);
    if (!hConnect) {
        WinHttpCloseHandle(hSession);
        outErr = "Cannot connect to lobby server (" + baseUrl + ")";
        return false;
    }

    std::wstring wMethod = core::Utf8ToWide(method);
    std::wstring wPath = core::Utf8ToWide(path);
    const DWORD flags = u.https ? WINHTTP_FLAG_SECURE : 0;
    HINTERNET hReq = WinHttpOpenRequest(hConnect, wMethod.c_str(), wPath.c_str(), nullptr,
                                        WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
    if (!hReq) {
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        outErr = "WinHttpOpenRequest failed";
        return false;
    }

    std::wstring headers = L"Content-Type: application/json\r\nX-Nidhogg-Version: " +
                           core::Utf8ToWide(EffectiveProtocolVersion()) + L"\r\n";
    BOOL sent = WinHttpSendRequest(
        hReq, headers.c_str(), static_cast<DWORD>(headers.size()),
        body.empty() ? WINHTTP_NO_REQUEST_DATA : const_cast<char*>(body.data()),
        static_cast<DWORD>(body.size()), static_cast<DWORD>(body.size()), 0);
    if (!sent || !WinHttpReceiveResponse(hReq, nullptr)) {
        WinHttpCloseHandle(hReq);
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        outErr = "Lobby server unreachable (" + baseUrl + ")";
        return false;
    }

    DWORD statusCode = 0;
    DWORD sz = sizeof(statusCode);
    WinHttpQueryHeaders(hReq, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                        WINHTTP_HEADER_NAME_BY_INDEX, &statusCode, &sz, WINHTTP_NO_HEADER_INDEX);
    outStatus = static_cast<int>(statusCode);

    outResponse.clear();
    for (;;) {
        DWORD avail = 0;
        if (!WinHttpQueryDataAvailable(hReq, &avail) || avail == 0) break;
        std::string chunk(avail, '\0');
        DWORD got = 0;
        if (!WinHttpReadData(hReq, chunk.data(), avail, &got) || got == 0) break;
        chunk.resize(got);
        outResponse += chunk;
    }

    if (outRttMs) *outRttMs = static_cast<uint32_t>((MonoUsec() - t0) / 1000ULL);

    WinHttpCloseHandle(hReq);
    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);
    return true;
}

std::mutex g_endpointMtx;
std::string g_resolvedEndpoint;
uint64_t g_resolvedAtUsec = 0;

// Resolves configured server URL; if default `https://lobby.japer.me` is not yet
// online in DNS while a local `NidhoggServer.exe` is running on 127.0.0.1:27880,
// seamlessly falls back to localhost so both production and local play work out of the box.
bool HttpRequestJson(const std::string& method, const std::string& path, const std::string& body,
                     int& outStatus, std::string& outResponse, std::string& outErr,
                     uint32_t* outRttMs = nullptr) {
    const std::string configured = core::CurrentSettings().lobbyServerUrl;
    std::string cached;
    {
        std::scoped_lock lk(g_endpointMtx);
        if (!g_resolvedEndpoint.empty() && (MonoUsec() - g_resolvedAtUsec < 15000000ULL)) {
            cached = g_resolvedEndpoint;
        }
    }

    if (!cached.empty()) {
        if (HttpRequestJsonAt(cached, method, path, body, 2000, outStatus, outResponse, outErr,
                              outRttMs)) {
            return true;
        }
        std::scoped_lock lk(g_endpointMtx);
        g_resolvedEndpoint.clear();
    }

    if (HttpRequestJsonAt(configured, method, path, body, 1500, outStatus, outResponse, outErr,
                          outRttMs)) {
        std::scoped_lock lk(g_endpointMtx);
        g_resolvedEndpoint = configured;
        g_resolvedAtUsec = MonoUsec();
        return true;
    }

    // Automatic localhost fallback when default public hostname is offline
    if (configured.find("127.0.0.1") == std::string::npos &&
        configured.find("localhost") == std::string::npos) {
        for (const char* localCand : {"http://127.0.0.1:27880", "http://127.0.0.1:27885"}) {
            int hst = 0;
            std::string hresp, herr;
            if (HttpRequestJsonAt(localCand, "GET", "/api/health", "", 300, hst, hresp, herr) &&
                hst == 200 && hresp.find("nidhogg-lobby") != std::string::npos) {
                if (HttpRequestJsonAt(localCand, method, path, body, 1000, outStatus, outResponse,
                                      herr, outRttMs)) {
                    std::scoped_lock lk(g_endpointMtx);
                    g_resolvedEndpoint = localCand;
                    g_resolvedAtUsec = MonoUsec();
                    LOG_INFO("[netplay] public endpoint %s offline; auto-connected to local %s",
                             configured.c_str(), localCand);
                    return true;
                }
            }
        }
    }
    return false;
}

struct NetState {
    std::mutex mtx;
    bool initialized = false;
    std::atomic<bool> shuttingDown{false};
    std::thread bgHeartbeatThread;

    SOCKET udpSock = INVALID_SOCKET;
    uint16_t localUdpPort = 0;
    std::string localLanIp = "127.0.0.1";
    std::string stunPublicIp;
    uint16_t stunPublicPort = 0;

    // WinHTTP WebSocket gameplay relay handles
    std::mutex wsTxMtx;
    HINTERNET wsSession = nullptr;
    HINTERNET wsConnect = nullptr;
    HINTERNET wsReq = nullptr;
    HINTERNET wsHandle = nullptr;
    std::atomic<bool> wsConnected{false};
    std::atomic<bool> wsStop{false};
    std::thread wsRxThread;
    std::deque<std::vector<uint8_t>> wsRxQueue;

    // Network condition simulator (for Phase 11 testing)
    int simOneWayLatencyMs = 0;
    int simJitterMs = 0;
    int simLossPct = 0;
    bool forceRelay = false;
    uint32_t simRng = 0x9E3779B9u;
    std::deque<DelayedPacket> txQueue;

    // Lobby & room state
    NetStatsSnapshot snap;
    std::string authToken;
    uint64_t sessionId = 0;
    uint8_t sessionKey[32]{};
    bool hasSessionKey = false;
    uint32_t matchSeed = 0xCAFE1234u;
    uint16_t relayUdpPort = 27881;
    sockaddr_in relayAddr{};
    bool hasRelayAddr = false;

    sockaddr_in peerPublicAddr{};
    bool hasPeerPublic = false;
    sockaddr_in peerLanAddr{};
    bool hasPeerLan = false;
    sockaddr_in activePeerAddr{};
    bool directEstablished = false;

    uint64_t connectStartUsec = 0;
    uint64_t lastRxUsec = 0;
    uint64_t lastPeerTimestampUsec = 0;
    uint64_t nextNonce = 1;

    // Match barrier & frame-indexed lockstep buffers
    bool localAtBarrier = false;
    bool peerAtBarrier = false;
    bool peerAckedBarrier = false;
    uint64_t menuPulseCounter = 0;

    std::string lastSyncedRoom;
    uint32_t roomTransitionCount = 0;
    uint64_t matchFrame = 0;
    std::unordered_map<uint64_t, uint32_t> localInputs;
    std::unordered_map<uint64_t, uint32_t> remoteInputs;
    std::unordered_map<uint64_t, uint64_t> localStateHashes;
    std::unordered_map<uint64_t, uint64_t> remoteStateHashes;

    bool cliHandled = false;
    bool cliAutoStart = false;
};

NetState g_net;

std::string DetectLocalLanIp() {
    SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == INVALID_SOCKET) return "127.0.0.1";
    sockaddr_in dst{};
    dst.sin_family = AF_INET;
    dst.sin_port = htons(53);
    inet_pton(AF_INET, "1.1.1.1", &dst.sin_addr);
    connect(s, reinterpret_cast<sockaddr*>(&dst), sizeof(dst));
    sockaddr_in name{};
    int namelen = sizeof(name);
    std::string ip = "127.0.0.1";
    if (getsockname(s, reinterpret_cast<sockaddr*>(&name), &namelen) == 0) {
        char buf[64]{};
        if (inet_ntop(AF_INET, &name.sin_addr, buf, sizeof(buf)) &&
            std::strcmp(buf, "0.0.0.0") != 0)
            ip = buf;
    }
    closesocket(s);
    return ip;
}

// RFC 5389 STUN Binding Request on our gameplay UDP socket to discover external
// NAT IP:port without needing raw UDP on the Cloudflare Tunnel server.
void DiscoverStunEndpoint(SOCKET sock, std::string& outIp, uint16_t& outPort) {
    char* skipEnv = nullptr;
    size_t skipLen = 0;
    if (_dupenv_s(&skipEnv, &skipLen, "NIDHOGG_SKIP_STUN") == 0 && skipEnv && *skipEnv == '1') {
        std::free(skipEnv);
        return;
    }
    if (skipEnv) std::free(skipEnv);

    struct StunServer {
        const char* host;
        const char* port;
    };
    const StunServer servers[] = {
        {"stun.cloudflare.com", "3478"},
        {"stun.l.google.com", "19302"},
    };

    for (const auto& sv : servers) {
        addrinfo hints{};
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_DGRAM;
        hints.ai_protocol = IPPROTO_UDP;
        addrinfo* res = nullptr;
        if (getaddrinfo(sv.host, sv.port, &hints, &res) != 0 || !res) continue;

        uint8_t req[20]{};
        req[0] = 0x00; req[1] = 0x01;  // Binding Request
        req[2] = 0x00; req[3] = 0x00;  // Length = 0
        req[4] = 0x21; req[5] = 0x12; req[6] = 0xA4; req[7] = 0x42;  // Magic Cookie
        RandomBytes(req + 8, 12);      // 96-bit Transaction ID

        sendto(sock, reinterpret_cast<const char*>(req), 20, 0, res->ai_addr,
               static_cast<int>(res->ai_addrlen));
        freeaddrinfo(res);

        const uint64_t deadline = MonoUsec() + 250000ULL;  // 250ms max STUN wait
        while (MonoUsec() < deadline) {
            uint8_t resp[512];
            sockaddr_in from{};
            int flen = sizeof(from);
            int n = recvfrom(sock, reinterpret_cast<char*>(resp), sizeof(resp), 0,
                             reinterpret_cast<sockaddr*>(&from), &flen);
            if (n >= 20 && resp[0] == 0x01 && resp[1] == 0x01 &&
                std::memcmp(resp + 8, req + 8, 12) == 0) {
                const size_t msgLen = (static_cast<size_t>(resp[2]) << 8) | resp[3];
                size_t off = 20;
                while (off + 4 <= static_cast<size_t>(n) && off < 20 + msgLen) {
                    const uint16_t attrType = (static_cast<uint16_t>(resp[off]) << 8) | resp[off + 1];
                    const uint16_t attrLen = (static_cast<uint16_t>(resp[off + 2]) << 8) | resp[off + 3];
                    off += 4;
                    if (off + attrLen > static_cast<size_t>(n)) break;
                    if (attrType == 0x0020 && attrLen >= 8 && resp[off + 1] == 0x01) {
                        // XOR-MAPPED-ADDRESS IPv4
                        const uint16_t xport = ((static_cast<uint16_t>(resp[off + 2]) << 8) |
                                                resp[off + 3]) ^ 0x2112u;
                        uint32_t xip = 0;
                        std::memcpy(&xip, resp + off + 4, 4);
                        const uint32_t cookieNet = htonl(0x2112A442u);
                        xip ^= cookieNet;
                        in_addr ia{};
                        ia.s_addr = xip;
                        char ipStr[64]{};
                        if (inet_ntop(AF_INET, &ia, ipStr, sizeof(ipStr))) {
                            outIp = ipStr;
                            outPort = xport;
                            LOG_INFO("[stun] discovered public NAT endpoint %s:%u via %s",
                                     outIp.c_str(), outPort, sv.host);
                            return;
                        }
                    }
                    off += (attrLen + 3u) & ~3u;
                }
            }
            Sleep(5);
        }
    }
}

bool EnsureUdpSocketLocked() {
    if (g_net.udpSock != INVALID_SOCKET) return true;
    SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == INVALID_SOCKET) return false;
    u_long nonBlock = 1;
    ioctlsocket(s, FIONBIO, &nonBlock);
    int bufSz = 256 * 1024;
    setsockopt(s, SOL_SOCKET, SO_RCVBUF, reinterpret_cast<const char*>(&bufSz), sizeof(bufSz));
    setsockopt(s, SOL_SOCKET, SO_SNDBUF, reinterpret_cast<const char*>(&bufSz), sizeof(bufSz));

    sockaddr_in bindAddr{};
    bindAddr.sin_family = AF_INET;
    bindAddr.sin_addr.s_addr = htonl(INADDR_ANY);
    bindAddr.sin_port = 0;
    if (bind(s, reinterpret_cast<sockaddr*>(&bindAddr), sizeof(bindAddr)) != 0) {
        closesocket(s);
        return false;
    }
    sockaddr_in bound{};
    int blen = sizeof(bound);
    getsockname(s, reinterpret_cast<sockaddr*>(&bound), &blen);
    g_net.localUdpPort = ntohs(bound.sin_port);
    g_net.localLanIp = DetectLocalLanIp();
    g_net.udpSock = s;

    DiscoverStunEndpoint(s, g_net.stunPublicIp, g_net.stunPublicPort);
    LOG_INFO("[netplay] UDP bound LAN=%s:%u STUN=%s:%u", g_net.localLanIp.c_str(),
             g_net.localUdpPort,
             g_net.stunPublicIp.empty() ? "(none)" : g_net.stunPublicIp.c_str(),
             g_net.stunPublicPort);
    return true;
}

void StopWebSocketRelay() {
    g_net.wsStop.store(true);
    g_net.wsConnected.store(false);
    {
        std::scoped_lock lk(g_net.wsTxMtx);
        if (g_net.wsHandle) {
            WinHttpWebSocketClose(g_net.wsHandle, WINHTTP_WEB_SOCKET_SUCCESS_CLOSE_STATUS, nullptr, 0);
            WinHttpCloseHandle(g_net.wsHandle);
            g_net.wsHandle = nullptr;
        }
        if (g_net.wsReq) { WinHttpCloseHandle(g_net.wsReq); g_net.wsReq = nullptr; }
        if (g_net.wsConnect) { WinHttpCloseHandle(g_net.wsConnect); g_net.wsConnect = nullptr; }
        if (g_net.wsSession) { WinHttpCloseHandle(g_net.wsSession); g_net.wsSession = nullptr; }
    }
    if (g_net.wsRxThread.joinable()) g_net.wsRxThread.join();
    std::scoped_lock lk(g_net.mtx);
    g_net.wsRxQueue.clear();
}

void StartWebSocketRelay(uint64_t sessionId, int role, const std::string& authToken) {
    StopWebSocketRelay();
    g_net.wsStop.store(false);

    const std::string base = ActiveLobbyEndpoint();
    const ParsedUrl u = ParseServerUrl(base);

    HINTERNET hSess = WinHttpOpen(L"NidhoggWS/1.0", WINHTTP_ACCESS_TYPE_NO_PROXY,
                                  WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSess) return;
    WinHttpSetTimeouts(hSess, 2000, 2000, 5000, 45000);

    HINTERNET hConn = WinHttpConnect(hSess, u.host.c_str(), u.port, 0);
    if (!hConn) { WinHttpCloseHandle(hSess); return; }

    std::ostringstream p;
    p << "/ws?session=" << sessionId << "&role=" << role << "&token=" << authToken;
    std::wstring wPath = core::Utf8ToWide(p.str());
    HINTERNET hReq = WinHttpOpenRequest(hConn, L"GET", wPath.c_str(), nullptr,
                                        WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                        u.https ? WINHTTP_FLAG_SECURE : 0);
    if (!hReq) {
        WinHttpCloseHandle(hConn);
        WinHttpCloseHandle(hSess);
        return;
    }

    if (!WinHttpSetOption(hReq, WINHTTP_OPTION_UPGRADE_TO_WEB_SOCKET, nullptr, 0) ||
        !WinHttpSendRequest(hReq, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
        !WinHttpReceiveResponse(hReq, nullptr)) {
        WinHttpCloseHandle(hReq);
        WinHttpCloseHandle(hConn);
        WinHttpCloseHandle(hSess);
        LOG_WARN("[ws-relay] upgrade handshake failed against %s", base.c_str());
        return;
    }

    HINTERNET hWs = WinHttpWebSocketCompleteUpgrade(hReq, 0);
    WinHttpCloseHandle(hReq);
    if (!hWs) {
        WinHttpCloseHandle(hConn);
        WinHttpCloseHandle(hSess);
        return;
    }

    {
        std::scoped_lock lk(g_net.wsTxMtx);
        g_net.wsSession = hSess;
        g_net.wsConnect = hConn;
        g_net.wsHandle = hWs;
        g_net.wsConnected.store(true);
    }
    LOG_INFO("[ws-relay] Binary WebSocket relay connected (%s/ws)", base.c_str());

    g_net.wsRxThread = std::thread([]() {
        std::vector<uint8_t> accum;
        uint8_t chunk[2048];
        while (!g_net.wsStop.load()) {
            HINTERNET h = nullptr;
            {
                std::scoped_lock lk(g_net.wsTxMtx);
                h = g_net.wsHandle;
            }
            if (!h) break;
            DWORD got = 0;
            WINHTTP_WEB_SOCKET_BUFFER_TYPE btype{};
            DWORD err = WinHttpWebSocketReceive(h, chunk, sizeof(chunk), &got, &btype);
            if (err != NO_ERROR) break;
            if (btype == WINHTTP_WEB_SOCKET_CLOSE_BUFFER_TYPE) break;
            accum.insert(accum.end(), chunk, chunk + got);
            if (btype == WINHTTP_WEB_SOCKET_BINARY_MESSAGE_BUFFER_TYPE ||
                btype == WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE) {
                if (!accum.empty()) {
                    std::scoped_lock lk(g_net.mtx);
                    if (g_net.wsRxQueue.size() < 512) {
                        g_net.wsRxQueue.push_back(std::move(accum));
                    }
                    accum.clear();
                }
            }
        }
        g_net.wsConnected.store(false);
    });
}

void SendWsBinaryNow(const uint8_t* data, size_t len) {
    if (!g_net.wsConnected.load() || !data || !len) return;
    std::scoped_lock lk(g_net.wsTxMtx);
    if (g_net.wsHandle) {
        WinHttpWebSocketSend(g_net.wsHandle, WINHTTP_WEB_SOCKET_BINARY_MESSAGE_BUFFER_TYPE,
                             const_cast<uint8_t*>(data), static_cast<DWORD>(len));
    }
}

void FlushTxQueueLocked() {
    const uint64_t now = MonoUsec();
    for (auto it = g_net.txQueue.begin(); it != g_net.txQueue.end();) {
        if (now >= it->deliverAtUsec) {
            if (it->viaWebSocket) {
                SendWsBinaryNow(it->bytes.data(), it->bytes.size());
                ++g_net.snap.packetsSent;
            } else if (g_net.udpSock != INVALID_SOCKET) {
                sendto(g_net.udpSock, reinterpret_cast<const char*>(it->bytes.data()),
                       static_cast<int>(it->bytes.size()), 0,
                       reinterpret_cast<const sockaddr*>(&it->dst), sizeof(it->dst));
                ++g_net.snap.packetsSent;
            }
            it = g_net.txQueue.erase(it);
        } else {
            ++it;
        }
    }
}

bool ShouldSimDropLocked() {
    if (g_net.simLossPct <= 0) return false;
    g_net.simRng = g_net.simRng * 1664525u + 1013904223u;
    return static_cast<int>((g_net.simRng >> 16) % 100u) < g_net.simLossPct;
}

uint64_t ComputeSimDelayUsecLocked() {
    uint64_t d = static_cast<uint64_t>(std::max(0, g_net.simOneWayLatencyMs)) * 1000ULL;
    if (g_net.simJitterMs > 0) {
        g_net.simRng = g_net.simRng * 1664525u + 1013904223u;
        d += (g_net.simRng >> 16) % static_cast<uint32_t>(g_net.simJitterMs * 1000 + 1);
    }
    return d;
}

void QueueUdpSendLocked(const sockaddr_in& dst, const uint8_t* data, size_t len) {
    if (g_net.udpSock == INVALID_SOCKET || !data || !len || ShouldSimDropLocked()) return;
    const uint64_t delayUsec = ComputeSimDelayUsecLocked();
    if (delayUsec == 0) {
        sendto(g_net.udpSock, reinterpret_cast<const char*>(data), static_cast<int>(len), 0,
               reinterpret_cast<const sockaddr*>(&dst), sizeof(dst));
        ++g_net.snap.packetsSent;
    } else {
        DelayedPacket dp;
        dp.deliverAtUsec = MonoUsec() + delayUsec;
        dp.viaWebSocket = false;
        dp.dst = dst;
        dp.bytes.assign(data, data + len);
        g_net.txQueue.push_back(std::move(dp));
    }
}

void QueueWsSendLocked(const uint8_t* data, size_t len) {
    if (!g_net.wsConnected.load() || !data || !len || ShouldSimDropLocked()) return;
    const uint64_t delayUsec = ComputeSimDelayUsecLocked();
    if (delayUsec == 0) {
        SendWsBinaryNow(data, len);
        ++g_net.snap.packetsSent;
    } else {
        DelayedPacket dp;
        dp.deliverAtUsec = MonoUsec() + delayUsec;
        dp.viaWebSocket = true;
        dp.bytes.assign(data, data + len);
        g_net.txQueue.push_back(std::move(dp));
    }
}

void SendSealedPeerPacketLocked(uint8_t pktType, const void* payload, size_t payloadLen) {
    if (!g_net.hasSessionKey || g_net.sessionId == 0) return;
    WireAad aad{};
    aad.magic = kWireMagic;
    aad.type = pktType;
    aad.senderRole = g_net.snap.isHost ? 1 : 2;
    aad.sessionId = g_net.sessionId;

    std::vector<uint8_t> sealed;
    if (!SealPacket(g_net.sessionKey, g_net.nextNonce++, reinterpret_cast<const uint8_t*>(&aad),
                    sizeof(aad), static_cast<const uint8_t*>(payload), payloadLen, sealed))
        return;

    auto sendRelays = [&]() {
        if (g_net.wsConnected.load()) {
            QueueWsSendLocked(sealed.data(), sealed.size());
        } else if (g_net.hasRelayAddr) {
            std::vector<uint8_t> env(10 + sealed.size());
            env[0] = kPktRelayEnvelope;
            std::memcpy(&env[1], &g_net.sessionId, 8);
            env[9] = aad.senderRole;
            std::memcpy(&env[10], sealed.data(), sealed.size());
            QueueUdpSendLocked(g_net.relayAddr, env.data(), env.size());
        }
    };

    if (g_net.forceRelay) {
        sendRelays();
        return;
    }

    if (g_net.directEstablished) {
        QueueUdpSendLocked(g_net.activePeerAddr, sealed.data(), sealed.size());
    } else {
        if (g_net.hasPeerPublic)
            QueueUdpSendLocked(g_net.peerPublicAddr, sealed.data(), sealed.size());
        if (g_net.hasPeerLan)
            QueueUdpSendLocked(g_net.peerLanAddr, sealed.data(), sealed.size());
        sendRelays();
    }
}

void SendRegisterEndpointToRelayLocked() {
    if (!g_net.hasRelayAddr || !g_net.hasSessionKey || g_net.sessionId == 0) return;
    WireAad aad{};
    aad.magic = kWireMagic;
    aad.type = kPktRegisterEndpoint;
    aad.senderRole = g_net.snap.isHost ? 1 : 2;
    aad.sessionId = g_net.sessionId;
    const uint64_t ts = MonoUsec();
    std::vector<uint8_t> sealed;
    if (SealPacket(g_net.sessionKey, g_net.nextNonce++, reinterpret_cast<const uint8_t*>(&aad),
                   sizeof(aad), reinterpret_cast<const uint8_t*>(&ts), sizeof(ts), sealed)) {
        QueueUdpSendLocked(g_net.relayAddr, sealed.data(), sealed.size());
    }
}

void UpdateRttLocked(uint64_t echoedSendTimeUsec) {
    if (!echoedSendTimeUsec) return;
    const uint64_t now = MonoUsec();
    if (now < echoedSendTimeUsec) return;
    const double sampleMs = static_cast<double>(now - echoedSendTimeUsec) / 1000.0;
    if (sampleMs > 5000.0) return;
    if (g_net.snap.rttMs <= 0.01) {
        g_net.snap.rttMs = sampleMs;
        g_net.snap.jitterMs = 0.0;
    } else {
        const double delta = std::fabs(sampleMs - g_net.snap.rttMs);
        g_net.snap.jitterMs = g_net.snap.jitterMs * 0.85 + delta * 0.15;
        g_net.snap.rttMs = g_net.snap.rttMs * 0.85 + sampleMs * 0.15;
    }
}

const char* TransportLabel(TransportMode m) {
    switch (m) {
        case TransportMode::DirectP2P: return "DirectP2P";
        case TransportMode::WebSocketRelay: return "WebSocketRelay";
        case TransportMode::RelayFallback: return "UdpRelay";
        default: return "Punching";
    }
}

void VerifyHashesForFrameLocked(uint64_t f) {
    auto lit = g_net.localStateHashes.find(f);
    auto rit = g_net.remoteStateHashes.find(f);
    if (lit == g_net.localStateHashes.end() || rit == g_net.remoteStateHashes.end()) return;

    ++g_net.snap.stateChecks;
    g_net.snap.lastLocalHash = lit->second;
    g_net.snap.lastRemoteHash = rit->second;
    if (lit->second != rit->second) {
        if (!g_net.snap.desyncDetected) {
            g_net.snap.desyncDetected = true;
            g_net.snap.desyncFrame = f;
        }
        LOG_ERROR("[netplay] DESYNC DETECTED at matchFrame=%llu local=%016llx remote=%016llx",
                  static_cast<unsigned long long>(f),
                  static_cast<unsigned long long>(lit->second),
                  static_cast<unsigned long long>(rit->second));
    } else if ((f % 120) == 0) {
        LOG_INFO("[netplay] frame=%llu mode=%s rtt=%.1fms jitter=%.1fms hash=%016llx sync=OK",
                 static_cast<unsigned long long>(f), TransportLabel(g_net.snap.transport),
                 g_net.snap.rttMs, g_net.snap.jitterMs,
                 static_cast<unsigned long long>(lit->second));
    }
    g_net.localStateHashes.erase(lit);
    g_net.remoteStateHashes.erase(rit);
}

void ProcessAuthenticatedPacketLocked(const uint8_t* pkt, size_t pktLen,
                                      const sockaddr_in* directFrom, bool fromWs, bool fromUdpRelay) {
    if (pktLen < sizeof(WireAad) + 8 + 16) return;
    WireAad aad{};
    std::memcpy(&aad, pkt, sizeof(aad));
    if (aad.magic != kWireMagic || aad.sessionId != g_net.sessionId) return;
    const uint8_t expectedPeerRole = g_net.snap.isHost ? 2 : 1;
    if (aad.senderRole != expectedPeerRole) return;

    uint64_t nonce = 0;
    std::vector<uint8_t> plain;
    if (!OpenPacket(g_net.sessionKey, pkt, pktLen, sizeof(WireAad), nonce, plain)) return;

    ++g_net.snap.packetsReceived;
    g_net.lastRxUsec = MonoUsec();

    if (directFrom && !fromWs && !fromUdpRelay && !g_net.forceRelay) {
        g_net.directEstablished = true;
        g_net.activePeerAddr = *directFrom;
        g_net.snap.transport = TransportMode::DirectP2P;
    } else if (!g_net.directEstablished) {
        g_net.snap.transport = fromWs ? TransportMode::WebSocketRelay
                                      : TransportMode::RelayFallback;
    }

    switch (aad.type) {
        case kPktPunch: {
            uint64_t peerTs = 0;
            if (plain.size() >= 8) std::memcpy(&peerTs, plain.data(), 8);
            g_net.lastPeerTimestampUsec = peerTs;
            uint64_t ackTs[2] = {MonoUsec(), peerTs};
            SendSealedPeerPacketLocked(kPktPunchAck, ackTs, sizeof(ackTs));
            break;
        }
        case kPktPunchAck: {
            if (plain.size() >= 16) {
                uint64_t peerTs = 0, echoed = 0;
                std::memcpy(&peerTs, plain.data(), 8);
                std::memcpy(&echoed, plain.data() + 8, 8);
                g_net.lastPeerTimestampUsec = peerTs;
                UpdateRttLocked(echoed);
            }
            if (g_net.snap.phase == SessionPhase::ConnectingTransport) {
                g_net.snap.phase = SessionPhase::SyncingMenu;
                g_net.snap.statusText = "Connected — synchronizing stage entry...";
                LOG_INFO("[netplay] transport established (%s, rtt=%.1fms)",
                         TransportLabel(g_net.snap.transport), g_net.snap.rttMs);
            }
            break;
        }
        case kPktSyncBarrier: {
            if (plain.size() < sizeof(SyncBarrierPayload)) break;
            SyncBarrierPayload sb{};
            std::memcpy(&sb, plain.data(), sizeof(sb));
            g_net.lastPeerTimestampUsec = sb.sendTimeUsec;
            UpdateRttLocked(sb.echoTimeUsec);
            if (g_net.snap.phase == SessionPhase::ConnectingTransport) {
                g_net.snap.phase = SessionPhase::SyncingMenu;
            }
            if (!g_net.snap.isHost) {
                g_net.snap.stageIndex = sb.stageIndex;
                g_net.snap.inputDelayFrames = std::clamp<int>(sb.inputDelay, 1, 8);
                g_net.matchSeed = sb.matchSeed;
            }
            if (sb.atLevelSelect) g_net.peerAtBarrier = true;
            if (sb.ackPeerBarrier) g_net.peerAckedBarrier = true;
            break;
        }
        case kPktInput: {
            if (plain.size() < sizeof(InputPayload)) break;
            InputPayload ip{};
            std::memcpy(&ip, plain.data(), sizeof(ip));
            g_net.lastPeerTimestampUsec = ip.sendTimeUsec;
            UpdateRttLocked(ip.echoTimeUsec);
            g_net.peerAtBarrier = true;
            g_net.peerAckedBarrier = true;

            const uint8_t count = std::min<uint8_t>(ip.historyCount, 16);
            for (uint8_t k = 0; k < count; ++k) {
                if (ip.latestFrame >= k) {
                    g_net.remoteInputs[ip.latestFrame - k] = ip.buttonsHistory[k];
                }
            }
            if (ip.flags & 1u) {
                g_net.remoteStateHashes[ip.stateHashFrame] = ip.stateHash;
                VerifyHashesForFrameLocked(ip.stateHashFrame);
            }
            break;
        }
        case kPktDisconnect: {
            g_net.snap.phase = SessionPhase::Disconnected;
            g_net.snap.remoteConnected = false;
            g_net.snap.statusText = "Opponent disconnected from match.";
            LOG_WARN("[netplay] received peer disconnect notification");
            break;
        }
        default:
            break;
    }
}

void PumpTransportsRxLocked() {
    if (!g_net.hasSessionKey) return;
    FlushTxQueueLocked();

    // 1. Drain Binary WebSocket Relay RX frames
    while (!g_net.wsRxQueue.empty()) {
        std::vector<uint8_t> frm = std::move(g_net.wsRxQueue.front());
        g_net.wsRxQueue.pop_front();
        ProcessAuthenticatedPacketLocked(frm.data(), frm.size(), nullptr, true, false);
    }

    // 2. Drain Direct / Relay UDP socket frames
    if (g_net.udpSock != INVALID_SOCKET) {
        uint8_t buf[2048];
        for (int iter = 0; iter < 64; ++iter) {
            sockaddr_in from{};
            int fromLen = sizeof(from);
            int n = recvfrom(g_net.udpSock, reinterpret_cast<char*>(buf), sizeof(buf), 0,
                             reinterpret_cast<sockaddr*>(&from), &fromLen);
            if (n <= 0) break;
            if (n > 10 && buf[0] == kPktRelayEnvelope) {
                ProcessAuthenticatedPacketLocked(buf + 10, static_cast<size_t>(n - 10),
                                                 nullptr, false, true);
            } else {
                ProcessAuthenticatedPacketLocked(buf, static_cast<size_t>(n), &from, false, false);
            }
        }
    }
}

void SendInputPacketForFrameLocked(uint64_t targetFrame, bool includeHash, uint64_t hashFrame,
                                   uint64_t hashVal) {
    InputPayload ip{};
    ip.latestFrame = targetFrame;
    ip.historyCount = static_cast<uint8_t>(std::min<uint64_t>(targetFrame + 1, 16));
    for (uint8_t k = 0; k < ip.historyCount; ++k) {
        auto it = g_net.localInputs.find(targetFrame - k);
        ip.buttonsHistory[k] = (it != g_net.localInputs.end()) ? it->second : 0u;
    }
    if (includeHash) {
        ip.flags |= 1u;
        ip.stateHashFrame = hashFrame;
        ip.stateHash = hashVal;
    }
    ip.sendTimeUsec = MonoUsec();
    ip.echoTimeUsec = g_net.lastPeerTimestampUsec;
    SendSealedPeerPacketLocked(kPktInput, &ip, sizeof(ip));
}

bool NetplayStepCallbackImpl(uint64_t /*simFrame*/, uint32_t localButtons,
                             uint32_t& outP1Buttons, uint32_t& outP2Buttons,
                             uint32_t& ioRandState, uint64_t& outMatchFrame,
                             bool& outLockClock) {
    outMatchFrame = 0;
    outLockClock = false;
    std::unique_lock<std::mutex> lk(g_net.mtx);
    PumpTransportsRxLocked();

    if (g_net.snap.phase == SessionPhase::ConnectingTransport) {
        const uint64_t now = MonoUsec();
        if (now - g_net.connectStartUsec > 1200000ULL &&
            g_net.snap.transport == TransportMode::PunchingP2P) {
            g_net.snap.transport = g_net.wsConnected.load() ? TransportMode::WebSocketRelay
                                                            : TransportMode::RelayFallback;
        }
        uint64_t ts = now;
        SendSealedPeerPacketLocked(kPktPunch, &ts, sizeof(ts));
        FlushTxQueueLocked();
        outP1Buttons = 0;
        outP2Buttons = 0;
        return true;
    }

    if (g_net.snap.phase == SessionPhase::SyncingMenu) {
        const std::string room = ps4::CurrentRoom();
        uint32_t navBtn = 0;
        if (room == "rLevelSelect") {
            g_net.localAtBarrier = true;
        } else {
            g_net.localAtBarrier = false;
            ++g_net.menuPulseCounter;
            if ((g_net.menuPulseCounter % 50) < 8) {
                navBtn = (room == "rShowLobbies" || room == "rControlsRoom" || room == "rCredits")
                             ? 0x2000u
                             : 0x4000u;
            }
        }

        SyncBarrierPayload sb{};
        sb.atLevelSelect = g_net.localAtBarrier ? 1 : 0;
        sb.ackPeerBarrier = g_net.peerAtBarrier ? 1 : 0;
        sb.stageIndex = static_cast<uint8_t>(g_net.snap.stageIndex);
        sb.inputDelay = static_cast<uint8_t>(g_net.snap.inputDelayFrames);
        sb.matchSeed = g_net.matchSeed;
        sb.sendTimeUsec = MonoUsec();
        sb.echoTimeUsec = g_net.lastPeerTimestampUsec;
        SendSealedPeerPacketLocked(kPktSyncBarrier, &sb, sizeof(sb));
        FlushTxQueueLocked();

        if (g_net.localAtBarrier && g_net.peerAtBarrier && g_net.peerAckedBarrier) {
            g_net.snap.phase = SessionPhase::InMatch;
            g_net.snap.statusText = "In Match — Synchronized";
            g_net.matchFrame = 0;
            g_net.lastSyncedRoom = room;
            g_net.roomTransitionCount = 0;
            g_net.localInputs.clear();
            g_net.remoteInputs.clear();
            g_net.localStateHashes.clear();
            g_net.remoteStateHashes.clear();
            const int delay = std::clamp(g_net.snap.inputDelayFrames, 1, 8);
            for (int d = 0; d < delay; ++d) {
                g_net.localInputs[static_cast<uint64_t>(d)] = 0u;
                g_net.remoteInputs[static_cast<uint64_t>(d)] = 0u;
            }
            ioRandState = g_net.matchSeed;
            LOG_INFO("[netplay] BARRIER SYNCED -> entering InMatch (seed=0x%08x, stage=%d, delay=%df, mode=%s)",
                     g_net.matchSeed, g_net.snap.stageIndex, delay,
                     TransportLabel(g_net.snap.transport));
        } else {
            outP1Buttons = navBtn;
            outP2Buttons = 0;
            return true;
        }
    }

    if (g_net.snap.phase != SessionPhase::InMatch) return false;

    const uint64_t f = g_net.matchFrame;
    outMatchFrame = f;
    outLockClock = true;

    // Deterministically re-anchor PRNG on every synchronized room entry so any
    // pre-match menu/ambient RNG calls in rLevelSelect cannot drift combat RNG.
    const std::string curRoom = ps4::CurrentRoom();
    if (curRoom != g_net.lastSyncedRoom) {
        g_net.lastSyncedRoom = curRoom;
        ++g_net.roomTransitionCount;
        ioRandState = g_net.matchSeed ^ (g_net.roomTransitionCount * 0x9E3779B9u);
    }
    const uint32_t delay = static_cast<uint32_t>(std::clamp(g_net.snap.inputDelayFrames, 1, 8));

    uint32_t effectiveLocal = localButtons;
    if (f < 160) {
        effectiveLocal = 0;
        if (g_net.snap.isHost) {
            const int stg = g_net.snap.stageIndex;
            if (stg == 3 && f >= 15 && f < 23) effectiveLocal = 0x0040u;
            else if (stg == 1 && ((f >= 15 && f < 23) || (f >= 60 && f < 68)))
                effectiveLocal = 0x0040u;
            else if (stg == 2 && f >= 15 && f < 23) effectiveLocal = 0x0010u;
            if (f >= 115 && f < 125) effectiveLocal = 0x4000u;
        }
    }

    const uint64_t futureFrame = f + delay;
    g_net.localInputs[futureFrame] = effectiveLocal;

    bool sendHash = false;
    uint64_t curHash = 0;
    if (f >= 180 && (f % 30) == 0) {
        curHash = ps4::ComputeGuestStateHash();
        g_net.localStateHashes[f] = curHash;
        VerifyHashesForFrameLocked(f);
        sendHash = true;
    }

    SendInputPacketForFrameLocked(futureFrame, sendHash, f, curHash);
    FlushTxQueueLocked();

    const uint64_t waitStart = MonoUsec();
    uint64_t lastRetx = waitStart;
    while (g_net.remoteInputs.find(f) == g_net.remoteInputs.end()) {
        PumpTransportsRxLocked();
        if (g_net.remoteInputs.find(f) != g_net.remoteInputs.end()) break;
        if (g_net.snap.phase != SessionPhase::InMatch || ps4::IsQuitRequested()) return false;
        const uint64_t now = MonoUsec();
        if (now - waitStart > 5000000ULL) {
            g_net.snap.phase = SessionPhase::Disconnected;
            g_net.snap.remoteConnected = false;
            g_net.snap.statusText = "Connection lost (peer timed out).";
            LOG_WARN("[netplay] peer timed out waiting for frame %llu",
                     static_cast<unsigned long long>(f));
            return false;
        }
        if (now - lastRetx >= 6000ULL) {
            SendInputPacketForFrameLocked(futureFrame, sendHash, f, curHash);
            FlushTxQueueLocked();
            lastRetx = now;
        }
        lk.unlock();
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        Sleep(1);
        lk.lock();
    }

    const uint32_t myInput = g_net.localInputs[f];
    const uint32_t peerInput = g_net.remoteInputs[f];
    if (g_net.snap.isHost) {
        outP1Buttons = myInput;
        outP2Buttons = peerInput;
    } else {
        outP1Buttons = peerInput;
        outP2Buttons = myInput;
    }

    if (f > 64) {
        g_net.localInputs.erase(f - 64);
        g_net.remoteInputs.erase(f - 64);
    }

    g_net.matchFrame = f + 1;
    g_net.snap.matchFrame = g_net.matchFrame;
    return true;
}

void ApplyRoomStateFromJsonLocked(const std::string& resp, bool& outNeedWsStart) {
    outNeedWsStart = false;
    JsonGetStr(resp, "roomId", g_net.snap.roomId);
    JsonGetStr(resp, "roomName", g_net.snap.roomName);
    JsonGetStr(resp, "roomCode", g_net.snap.roomCode);
    JsonGetStr(resp, "region", g_net.snap.region);
    JsonGetStr(resp, "hostName", g_net.snap.hostName);
    JsonGetStr(resp, "guestName", g_net.snap.guestName);
    JsonGetBool(resp, "isPublic", g_net.snap.isPublic);
    JsonGetBool(resp, "guestConnected", g_net.snap.remoteConnected);

    bool hostReady = false, guestReady = false;
    JsonGetBool(resp, "hostReady", hostReady);
    JsonGetBool(resp, "guestReady", guestReady);
    if (g_net.snap.isHost) {
        g_net.snap.localReady = hostReady;
        g_net.snap.remoteReady = guestReady;
    } else {
        g_net.snap.localReady = guestReady;
        g_net.snap.remoteReady = hostReady;
    }

    int64_t pCount = 1, stageIdx = 0, relayPort = 27881;
    if (JsonGetInt(resp, "playerCount", pCount)) g_net.snap.playerCount = static_cast<int>(pCount);
    if (JsonGetInt(resp, "stageIndex", stageIdx)) g_net.snap.stageIndex = static_cast<int>(stageIdx);
    if (JsonGetInt(resp, "relayUdpPort", relayPort) && relayPort > 0 && relayPort <= 65535)
        g_net.relayUdpPort = static_cast<uint16_t>(relayPort);

    uint64_t sid = 0, seed = 0;
    if (JsonGetU64(resp, "sessionId", sid) && sid != 0) g_net.sessionId = sid;
    if (JsonGetU64(resp, "matchSeed", seed) && seed != 0)
        g_net.matchSeed = static_cast<uint32_t>(seed);

    std::string keyHex;
    if (JsonGetStr(resp, "sessionKey", keyHex) && keyHex.size() == 64) {
        if (HexToBytes(keyHex, g_net.sessionKey, 32)) g_net.hasSessionKey = true;
    }

    const ParsedUrl u = ParseServerUrl(ActiveLobbyEndpoint());
    sockaddr_in raddr{};
    raddr.sin_family = AF_INET;
    raddr.sin_port = htons(g_net.relayUdpPort);
    if (inet_pton(AF_INET, u.hostUtf8.c_str(), &raddr.sin_addr) == 1) {
        g_net.relayAddr = raddr;
        g_net.hasRelayAddr = true;
    }

    // Peer endpoints for UDP hole punching
    std::string peerPubIp, peerLanIp;
    int64_t peerPubPort = 0, peerLanPort = 0;
    if (JsonGetStr(resp, "peerPublicIp", peerPubIp) && JsonGetInt(resp, "peerPublicPort", peerPubPort) &&
        !peerPubIp.empty() && peerPubPort > 0) {
        sockaddr_in pa{};
        pa.sin_family = AF_INET;
        pa.sin_port = htons(static_cast<uint16_t>(peerPubPort));
        if (inet_pton(AF_INET, peerPubIp.c_str(), &pa.sin_addr) == 1) {
            g_net.peerPublicAddr = pa;
            g_net.hasPeerPublic = true;
        }
    }
    if (JsonGetStr(resp, "peerLanIp", peerLanIp) && JsonGetInt(resp, "peerLanPort", peerLanPort) &&
        !peerLanIp.empty() && peerLanPort > 0) {
        sockaddr_in la{};
        la.sin_family = AF_INET;
        la.sin_port = htons(static_cast<uint16_t>(peerLanPort));
        if (inet_pton(AF_INET, peerLanIp.c_str(), &la.sin_addr) == 1) {
            g_net.peerLanAddr = la;
            g_net.hasPeerLan = true;
        }
    }

    bool kicked = false;
    if (JsonGetBool(resp, "kicked", kicked) && kicked) {
        g_net.snap.phase = SessionPhase::Disconnected;
        g_net.snap.statusText = "Removed from room by host.";
        g_net.authToken.clear();
        ps4::SetNetplayStepCallback(nullptr);
        return;
    }

    std::string roomStatus;
    JsonGetStr(resp, "status", roomStatus);
    if (roomStatus == "closed") {
        g_net.snap.phase = SessionPhase::Disconnected;
        g_net.snap.statusText = "Host closed the room.";
        g_net.authToken.clear();
        ps4::SetNetplayStepCallback(nullptr);
        return;
    }

    if (roomStatus == "in_match" && g_net.snap.phase == SessionPhase::LobbyWaiting &&
        g_net.hasSessionKey) {
        g_net.snap.phase = SessionPhase::ConnectingTransport;
        g_net.snap.transport = g_net.forceRelay ? TransportMode::RelayFallback
                                                : TransportMode::PunchingP2P;
        g_net.snap.inputDelayFrames = std::clamp(core::CurrentSettings().inputDelayFrames, 1, 8);
        g_net.snap.desyncDetected = false;
        g_net.snap.stateChecks = 0;
        g_net.directEstablished = false;
        g_net.localAtBarrier = false;
        g_net.peerAtBarrier = false;
        g_net.peerAckedBarrier = false;
        g_net.menuPulseCounter = 0;
        g_net.connectStartUsec = MonoUsec();
        g_net.snap.statusText = "Starting match — negotiating encrypted P2P...";
        SendRegisterEndpointToRelayLocked();
        ps4::SetNetplayStepCallback(&NetplayStepCallbackImpl);
        LOG_INFO("[netplay] transitioning to ConnectingTransport (sessionId=%llu, role=%s)",
                 static_cast<unsigned long long>(g_net.sessionId),
                 g_net.snap.isHost ? "Host/P1" : "Guest/P2");
    }
}

void CheckEnvSimConfigLocked() {
    auto envInt = [](const char* k, int defVal) {
        char* v = nullptr;
        size_t len = 0;
        if (_dupenv_s(&v, &len, k) == 0 && v && *v) {
            int x = std::atoi(v);
            std::free(v);
            return x;
        }
        if (v) std::free(v);
        return defVal;
    };
    const int rttMs = envInt("NIDHOGG_NET_SIM_LATENCY_MS", 0);
    g_net.simOneWayLatencyMs = std::max(0, rttMs / 2);
    g_net.simJitterMs = std::max(0, envInt("NIDHOGG_NET_SIM_JITTER_MS", 0));
    g_net.simLossPct = std::clamp(envInt("NIDHOGG_NET_SIM_LOSS_PCT", 0), 0, 90);
    g_net.forceRelay = envInt("NIDHOGG_FORCE_RELAY", 0) != 0;
}

}  // namespace

void InitNetplay() {
    std::scoped_lock lk(g_net.mtx);
    if (g_net.initialized) return;
    WSADATA wsa{};
    WSAStartup(MAKEWORD(2, 2), &wsa);
    CheckEnvSimConfigLocked();
    g_net.initialized = true;
}

void ShutdownNetplay() {
    LeaveRoom();
    std::scoped_lock lk(g_net.mtx);
    if (g_net.udpSock != INVALID_SOCKET) {
        closesocket(g_net.udpSock);
        g_net.udpSock = INVALID_SOCKET;
    }
    if (g_net.initialized) {
        WSACleanup();
        g_net.initialized = false;
    }
}

bool RefreshPublicRooms(const std::string& regionFilter, bool availableOnly,
                        std::vector<PublicRoomEntry>& outRooms, std::string& outError) {
    InitNetplay();
    outRooms.clear();
    std::string path = "/api/rooms?available=" + std::string(availableOnly ? "1" : "0");
    if (!regionFilter.empty() && regionFilter != "all") path += "&region=" + regionFilter;

    int status = 0;
    std::string resp;
    uint32_t rttMs = 0;
    if (!HttpRequestJson("GET", path, "", status, resp, outError, &rttMs)) return false;
    if (status == 426) {
        JsonGetStr(resp, "error", outError);
        if (outError.empty()) outError = "Client/server protocol version mismatch.";
        return false;
    }
    if (status != 200) {
        JsonGetStr(resp, "error", outError);
        if (outError.empty()) outError = "Failed to fetch rooms (HTTP " + std::to_string(status) + ")";
        return false;
    }

    // Parse JSON array of room objects inside `"rooms":[ ... ]`
    size_t arrPos = resp.find("\"rooms\"");
    size_t pos = (arrPos != std::string::npos) ? resp.find('[', arrPos) : 0;
    while (pos != std::string::npos && pos < resp.size()) {
        size_t objStart = resp.find('{', pos);
        if (objStart == std::string::npos) break;
        size_t objEnd = resp.find('}', objStart);
        if (objEnd == std::string::npos) break;
        std::string item = resp.substr(objStart, objEnd - objStart + 1);
        PublicRoomEntry e;
        if (JsonGetStr(item, "roomId", e.roomId)) {
            JsonGetStr(item, "roomName", e.roomName);
            JsonGetStr(item, "region", e.region);
            JsonGetStr(item, "status", e.status);
            int64_t pc = 1, mp = 2, stg = 0, ping = 0;
            if (JsonGetInt(item, "playerCount", pc)) e.playerCount = static_cast<int>(pc);
            if (JsonGetInt(item, "maxPlayers", mp)) e.maxPlayers = static_cast<int>(mp);
            if (JsonGetInt(item, "stageIndex", stg)) e.stageIndex = static_cast<int>(stg);
            if (JsonGetInt(item, "pingMs", ping) && ping > 0)
                e.estimatedPingMs = static_cast<int>(ping) + static_cast<int>(rttMs);
            else
                e.estimatedPingMs = static_cast<int>(rttMs);
            outRooms.push_back(std::move(e));
        }
        pos = objEnd + 1;
    }
    return true;
}

bool CreateRoom(const std::string& roomName, bool isPublic, int stageIndex, std::string& outError) {
    InitNetplay();
    LeaveRoom();

    uint16_t udpPort = 0;
    std::string lanIp;
    {
        std::scoped_lock lk(g_net.mtx);
        if (!EnsureUdpSocketLocked()) {
            outError = "Failed to bind local UDP socket.";
            return false;
        }
        udpPort = g_net.localUdpPort;
        lanIp = g_net.localLanIp;
    }

    const auto& cfg = core::CurrentSettings();
    const std::string rName = roomName.empty() ? (cfg.playerName + "'s Arena") : roomName;
    std::ostringstream body;
    body << "{\"roomName\":\"" << JsonEsc(rName) << "\","
         << "\"hostName\":\"" << JsonEsc(cfg.playerName) << "\","
         << "\"region\":\"" << JsonEsc(cfg.region) << "\","
         << "\"isPublic\":" << (isPublic ? "true" : "false") << ","
         << "\"stageIndex\":" << stageIndex << ","
         << "\"lanIp\":\"" << JsonEsc(lanIp) << "\","
         << "\"lanPort\":" << udpPort << "}";

    int status = 0;
    std::string resp;
    if (!HttpRequestJson("POST", "/api/rooms/create", body.str(), status, resp, outError))
        return false;
    if (status != 200) {
        if (!JsonGetStr(resp, "error", outError) || outError.empty())
            outError = "CreateRoom failed (HTTP " + std::to_string(status) + ")";
        return false;
    }

    std::scoped_lock lk(g_net.mtx);
    g_net.snap = NetStatsSnapshot{};
    g_net.snap.phase = SessionPhase::LobbyWaiting;
    g_net.snap.isHost = true;
    g_net.snap.localPlayerNum = 1;
    g_net.snap.playerCount = 1;
    g_net.snap.statusText = isPublic ? "Hosting Public Room — waiting for challenger..."
                                     : "Hosting Private Room — share Room Code with friend.";
    JsonGetStr(resp, "authToken", g_net.authToken);
    ApplyRoomStateFromJsonLocked(resp);
    SendRegisterEndpointToRelayLocked();
    g_net.lastHeartbeatUsec = MonoUsec();
    LOG_INFO("[netplay] created %s room id=%s code=%s",
             isPublic ? "PUBLIC" : "PRIVATE", g_net.snap.roomId.c_str(),
             g_net.snap.roomCode.c_str());
    return true;
}

bool JoinRoomById(const std::string& roomId, std::string& outError) {
    InitNetplay();
    LeaveRoom();

    uint16_t udpPort = 0;
    std::string lanIp;
    {
        std::scoped_lock lk(g_net.mtx);
        if (!EnsureUdpSocketLocked()) { outError = "Failed to bind UDP socket."; return false; }
        udpPort = g_net.localUdpPort;
        lanIp = g_net.localLanIp;
    }

    const auto& cfg = core::CurrentSettings();
    std::ostringstream body;
    body << "{\"roomId\":\"" << JsonEsc(roomId) << "\","
         << "\"playerName\":\"" << JsonEsc(cfg.playerName) << "\","
         << "\"lanIp\":\"" << JsonEsc(lanIp) << "\","
         << "\"lanPort\":" << udpPort << "}";

    int status = 0;
    std::string resp;
    if (!HttpRequestJson("POST", "/api/rooms/join", body.str(), status, resp, outError))
        return false;
    if (status != 200) {
        if (!JsonGetStr(resp, "error", outError) || outError.empty())
            outError = "Failed to join room (HTTP " + std::to_string(status) + ")";
        return false;
    }

    std::scoped_lock lk(g_net.mtx);
    g_net.snap = NetStatsSnapshot{};
    g_net.snap.phase = SessionPhase::LobbyWaiting;
    g_net.snap.isHost = false;
    g_net.snap.localPlayerNum = 2;
    g_net.snap.remoteConnected = true;
    g_net.snap.statusText = "Joined room — toggle Ready when prepared.";
    JsonGetStr(resp, "authToken", g_net.authToken);
    ApplyRoomStateFromJsonLocked(resp);
    SendRegisterEndpointToRelayLocked();
    g_net.lastHeartbeatUsec = MonoUsec();
    LOG_INFO("[netplay] joined room id=%s host=%s", g_net.snap.roomId.c_str(),
             g_net.snap.hostName.c_str());
    return true;
}

bool JoinRoomByCode(const std::string& roomCode, std::string& outError) {
    InitNetplay();
    LeaveRoom();

    std::string cleanCode;
    for (char c : roomCode) {
        if (std::isalnum(static_cast<unsigned char>(c)))
            cleanCode.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(c))));
    }
    if (cleanCode.size() != 6) {
        outError = "Room code must be 6 characters.";
        return false;
    }

    uint16_t udpPort = 0;
    std::string lanIp;
    {
        std::scoped_lock lk(g_net.mtx);
        if (!EnsureUdpSocketLocked()) { outError = "Failed to bind UDP socket."; return false; }
        udpPort = g_net.localUdpPort;
        lanIp = g_net.localLanIp;
    }

    const auto& cfg = core::CurrentSettings();
    std::ostringstream body;
    body << "{\"roomCode\":\"" << JsonEsc(cleanCode) << "\","
         << "\"playerName\":\"" << JsonEsc(cfg.playerName) << "\","
         << "\"lanIp\":\"" << JsonEsc(lanIp) << "\","
         << "\"lanPort\":" << udpPort << "}";

    int status = 0;
    std::string resp;
    if (!HttpRequestJson("POST", "/api/rooms/join_code", body.str(), status, resp, outError))
        return false;
    if (status != 200) {
        if (!JsonGetStr(resp, "error", outError) || outError.empty())
            outError = "Invalid or expired room code.";
        return false;
    }

    std::scoped_lock lk(g_net.mtx);
    g_net.snap = NetStatsSnapshot{};
    g_net.snap.phase = SessionPhase::LobbyWaiting;
    g_net.snap.isHost = false;
    g_net.snap.localPlayerNum = 2;
    g_net.snap.remoteConnected = true;
    g_net.snap.statusText = "Joined private room via code — toggle Ready!";
    JsonGetStr(resp, "authToken", g_net.authToken);
    ApplyRoomStateFromJsonLocked(resp);
    SendRegisterEndpointToRelayLocked();
    g_net.lastHeartbeatUsec = MonoUsec();
    LOG_INFO("[netplay] joined room by code=%s (id=%s)", cleanCode.c_str(),
             g_net.snap.roomId.c_str());
    return true;
}

bool QuickMatch(const std::string& preferredRegion, bool& outNoRoomsAvailable,
                std::string& outError) {
    InitNetplay();
    outNoRoomsAvailable = false;
    LeaveRoom();

    uint16_t udpPort = 0;
    std::string lanIp;
    {
        std::scoped_lock lk(g_net.mtx);
        if (!EnsureUdpSocketLocked()) { outError = "Failed to bind UDP socket."; return false; }
        udpPort = g_net.localUdpPort;
        lanIp = g_net.localLanIp;
    }

    const auto& cfg = core::CurrentSettings();
    std::ostringstream body;
    body << "{\"region\":\"" << JsonEsc(preferredRegion.empty() ? cfg.region : preferredRegion) << "\","
         << "\"playerName\":\"" << JsonEsc(cfg.playerName) << "\","
         << "\"lanIp\":\"" << JsonEsc(lanIp) << "\","
         << "\"lanPort\":" << udpPort << "}";

    int status = 0;
    std::string resp;
    if (!HttpRequestJson("POST", "/api/quickmatch", body.str(), status, resp, outError))
        return false;
    if (status == 404) {
        outNoRoomsAvailable = true;
        outError = "No open public rooms found. Create a public room?";
        return false;
    }
    if (status != 200) {
        if (!JsonGetStr(resp, "error", outError) || outError.empty())
            outError = "Quick Match failed (HTTP " + std::to_string(status) + ")";
        return false;
    }

    std::scoped_lock lk(g_net.mtx);
    g_net.snap = NetStatsSnapshot{};
    g_net.snap.phase = SessionPhase::LobbyWaiting;
    g_net.snap.isHost = false;
    g_net.snap.localPlayerNum = 2;
    g_net.snap.remoteConnected = true;
    g_net.snap.statusText = "Quick Match found opponent! Toggle Ready.";
    JsonGetStr(resp, "authToken", g_net.authToken);
    ApplyRoomStateFromJsonLocked(resp);
    SendRegisterEndpointToRelayLocked();
    g_net.lastHeartbeatUsec = MonoUsec();
    LOG_INFO("[netplay] quickmatch joined room id=%s", g_net.snap.roomId.c_str());
    return true;
}

bool SetRoomVisibility(bool isPublic, std::string& outError) {
    std::string roomId, token;
    {
        std::scoped_lock lk(g_net.mtx);
        if (!g_net.snap.isHost || g_net.snap.roomId.empty()) {
            outError = "Only the room host can change visibility.";
            return false;
        }
        roomId = g_net.snap.roomId;
        token = g_net.authToken;
    }

    std::ostringstream body;
    body << "{\"roomId\":\"" << JsonEsc(roomId) << "\","
         << "\"authToken\":\"" << JsonEsc(token) << "\","
         << "\"isPublic\":" << (isPublic ? "true" : "false") << "}";

    int status = 0;
    std::string resp;
    if (!HttpRequestJson("POST", "/api/rooms/visibility", body.str(), status, resp, outError))
        return false;
    if (status != 200) {
        if (!JsonGetStr(resp, "error", outError) || outError.empty())
            outError = "Visibility update failed.";
        return false;
    }

    std::scoped_lock lk(g_net.mtx);
    ApplyRoomStateFromJsonLocked(resp);
    g_net.snap.statusText = isPublic ? "Room switched to PUBLIC (listed in browser)."
                                     : "Room switched to PRIVATE (hidden, join by code only).";
    LOG_INFO("[netplay] switched room %s visibility -> %s", roomId.c_str(),
             isPublic ? "PUBLIC" : "PRIVATE");
    return true;
}

bool SetReady(bool ready, std::string& outError) {
    std::string roomId, token;
    {
        std::scoped_lock lk(g_net.mtx);
        roomId = g_net.snap.roomId;
        token = g_net.authToken;
    }
    if (roomId.empty()) { outError = "Not in a room."; return false; }

    std::ostringstream body;
    body << "{\"roomId\":\"" << JsonEsc(roomId) << "\","
         << "\"authToken\":\"" << JsonEsc(token) << "\","
         << "\"ready\":" << (ready ? "true" : "false") << "}";
    int status = 0;
    std::string resp;
    if (!HttpRequestJson("POST", "/api/rooms/ready", body.str(), status, resp, outError))
        return false;
    if (status != 200) {
        JsonGetStr(resp, "error", outError);
        return false;
    }
    std::scoped_lock lk(g_net.mtx);
    ApplyRoomStateFromJsonLocked(resp);
    return true;
}

bool KickGuest(std::string& outError) {
    std::string roomId, token;
    {
        std::scoped_lock lk(g_net.mtx);
        if (!g_net.snap.isHost || g_net.snap.roomId.empty()) {
            outError = "Only the host can kick a player.";
            return false;
        }
        roomId = g_net.snap.roomId;
        token = g_net.authToken;
    }
    std::ostringstream body;
    body << "{\"roomId\":\"" << JsonEsc(roomId) << "\",\"authToken\":\"" << JsonEsc(token) << "\"}";
    int status = 0;
    std::string resp;
    if (!HttpRequestJson("POST", "/api/rooms/kick", body.str(), status, resp, outError))
        return false;
    if (status != 200) {
        JsonGetStr(resp, "error", outError);
        return false;
    }
    std::scoped_lock lk(g_net.mtx);
    ApplyRoomStateFromJsonLocked(resp);
    g_net.snap.statusText = "Removed guest from room.";
    return true;
}

bool StartMatch(std::string& outError) {
    std::string roomId, token;
    {
        std::scoped_lock lk(g_net.mtx);
        if (!g_net.snap.isHost || g_net.snap.roomId.empty()) {
            outError = "Only the host can start the match.";
            return false;
        }
        roomId = g_net.snap.roomId;
        token = g_net.authToken;
    }
    std::ostringstream body;
    body << "{\"roomId\":\"" << JsonEsc(roomId) << "\",\"authToken\":\"" << JsonEsc(token) << "\"}";
    int status = 0;
    std::string resp;
    if (!HttpRequestJson("POST", "/api/rooms/start", body.str(), status, resp, outError))
        return false;
    if (status != 200) {
        if (!JsonGetStr(resp, "error", outError) || outError.empty())
            outError = "Cannot start match yet (need 2 players).";
        return false;
    }
    std::scoped_lock lk(g_net.mtx);
    ApplyRoomStateFromJsonLocked(resp);
    return true;
}

void LeaveRoom() {
    ps4::SetNetplayStepCallback(nullptr);
    std::string roomId, token;
    {
        std::scoped_lock lk(g_net.mtx);
        if (g_net.hasSessionKey && g_net.sessionId != 0 &&
            (g_net.snap.phase == SessionPhase::InMatch ||
             g_net.snap.phase == SessionPhase::SyncingMenu ||
             g_net.snap.phase == SessionPhase::ConnectingTransport)) {
            uint64_t bye = MonoUsec();
            SendSealedPeerPacketLocked(kPktDisconnect, &bye, sizeof(bye));
            FlushTxQueueLocked();
        }
        roomId = g_net.snap.roomId;
        token = g_net.authToken;
        g_net.snap = NetStatsSnapshot{};
        g_net.authToken.clear();
        g_net.sessionId = 0;
        g_net.hasSessionKey = false;
        g_net.directEstablished = false;
        g_net.hasPeerPublic = false;
        g_net.hasPeerLan = false;
        g_net.txQueue.clear();
    }
    if (!roomId.empty() && !token.empty()) {
        std::ostringstream body;
        body << "{\"roomId\":\"" << JsonEsc(roomId) << "\",\"authToken\":\"" << JsonEsc(token) << "\"}";
        int st = 0;
        std::string resp, err;
        HttpRequestJson("POST", "/api/rooms/leave", body.str(), st, resp, err);
    }
}

bool CopyRoomCodeToClipboard() {
    std::string code;
    {
        std::scoped_lock lk(g_net.mtx);
        code = g_net.snap.roomCode;
    }
    if (code.empty()) return false;
    if (!OpenClipboard(static_cast<HWND>(ps4::GameWindow()))) return false;
    EmptyClipboard();
    HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE, code.size() + 1);
    if (!hMem) { CloseClipboard(); return false; }
    void* dst = GlobalLock(hMem);
    if (dst) {
        std::memcpy(dst, code.c_str(), code.size() + 1);
        GlobalUnlock(hMem);
        SetClipboardData(CF_TEXT, hMem);
    }
    CloseClipboard();
    return true;
}

void TickNetplayLobby() {
    InitNetplay();

    // Optional automated multi-instance CLI/env workflow triggers (used by Phase 11 QA)
    bool doEnvSetup = false;
    {
        std::scoped_lock lk(g_net.mtx);
        if (!g_net.cliHandled && ps4::SubmittedFrames() >= 15) {
            g_net.cliHandled = true;
            doEnvSetup = true;
        }
    }
    if (doEnvSetup) {
        auto getEnv = [](const char* k) -> std::string {
            char* v = nullptr;
            size_t len = 0;
            std::string s;
            if (_dupenv_s(&v, &len, k) == 0 && v) { s = v; std::free(v); }
            return s;
        };
        const std::string mode = getEnv("NIDHOGG_NET_AUTO");
        const std::string arg = getEnv("NIDHOGG_NET_ARG");
        std::string err;
        if (mode == "host_public") {
            g_net.cliAutoStart = true;
            CreateRoom(arg.empty() ? "QA Public Room" : arg, true, 0, err);
        } else if (mode == "host_private") {
            g_net.cliAutoStart = true;
            CreateRoom("QA Private Room", false, 0, err);
            const std::string codeOut = getEnv("NIDHOGG_NET_CODE_OUT");
            if (!codeOut.empty()) {
                if (std::FILE* f = core::FopenUtf8(codeOut, "wb")) {
                    auto sc = GetSnapshot().roomCode;
                    std::fwrite(sc.data(), 1, sc.size(), f);
                    std::fclose(f);
                }
            }
        } else if (mode == "join_code" && !arg.empty()) {
            if (JoinRoomByCode(arg, err)) SetReady(true, err);
        } else if (mode == "quickmatch") {
            bool none = false;
            if (QuickMatch(arg, none, err)) SetReady(true, err);
        }
        if (!err.empty()) LOG_WARN("[netplay-auto] mode=%s err=%s", mode.c_str(), err.c_str());
    }

    std::string roomId, token;
    bool shouldPoll = false;
    bool autoStartReady = false;
    {
        std::scoped_lock lk(g_net.mtx);
        FlushTxQueueLocked();
        if (g_net.snap.phase == SessionPhase::LobbyWaiting) {
            PumpUdpRxLocked();
        }
        const uint64_t now = MonoUsec();
        if (!g_net.snap.roomId.empty() && !g_net.authToken.empty() &&
            now - g_net.lastHeartbeatUsec >= 400000ULL) {  // 400ms lobby poll / heartbeat
            g_net.lastHeartbeatUsec = now;
            roomId = g_net.snap.roomId;
            token = g_net.authToken;
            shouldPoll = true;
        }
    }

    if (shouldPoll) {
        std::ostringstream body;
        body << "{\"roomId\":\"" << JsonEsc(roomId) << "\",\"authToken\":\"" << JsonEsc(token) << "\"}";
        int status = 0;
        std::string resp, err;
        if (HttpRequestJson("POST", "/api/rooms/heartbeat", body.str(), status, resp, err) &&
            status == 200) {
            std::scoped_lock lk(g_net.mtx);
            if (g_net.snap.roomId == roomId) {
                ApplyRoomStateFromJsonLocked(resp);
                if (g_net.snap.phase == SessionPhase::LobbyWaiting) {
                    SendRegisterEndpointToRelayLocked();
                    if (g_net.cliAutoStart && g_net.snap.isHost && g_net.snap.remoteConnected &&
                        g_net.snap.remoteReady) {
                        autoStartReady = true;
                    }
                }
            }
        } else if (status == 404 || status == 403) {
            std::scoped_lock lk(g_net.mtx);
            if (g_net.snap.phase == SessionPhase::LobbyWaiting) {
                g_net.snap.phase = SessionPhase::Disconnected;
                g_net.snap.statusText = "Room expired or closed.";
                g_net.authToken.clear();
            }
        }
    }

    if (autoStartReady) {
        std::string err;
        SetReady(true, err);
        StartMatch(err);
    }
}

NetStatsSnapshot GetSnapshot() {
    std::scoped_lock lk(g_net.mtx);
    return g_net.snap;
}

}  // namespace ps4::net
