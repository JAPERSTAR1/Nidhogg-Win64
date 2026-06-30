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
constexpr uint8_t kPktRelayEnvelope = 0x52;  // 'R' envelope to/from UDP relay server

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
    uint8_t historyCount = 0;  // 1..16 frames: [latestFrame, latestFrame-1, ...]
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
    else if (s.rfind("http://", 0) == 0) { u.https = false; u.port = 80; s.erase(0, 7); }
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

bool HttpRequestJson(const std::string& method, const std::string& path, const std::string& body,
                     int& outStatus, std::string& outResponse, std::string& outErr,
                     uint32_t* outRttMs = nullptr) {
    const ParsedUrl u = ParseServerUrl(core::CurrentSettings().lobbyServerUrl);
    const uint64_t t0 = MonoUsec();

    HINTERNET hSession = WinHttpOpen(L"NidhoggPort/1.0", WINHTTP_ACCESS_TYPE_NO_PROXY,
                                     WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSession) { outErr = "WinHttpOpen failed"; return false; }
    WinHttpSetTimeouts(hSession, 1500, 1500, 2000, 2000);

    HINTERNET hConnect = WinHttpConnect(hSession, u.host.c_str(), u.port, 0);
    if (!hConnect) {
        WinHttpCloseHandle(hSession);
        outErr = "Cannot connect to lobby server (" + u.hostUtf8 + ")";
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
        outErr = "Lobby server unreachable (" + core::CurrentSettings().lobbyServerUrl + ")";
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

    if (outRttMs) {
        *outRttMs = static_cast<uint32_t>((MonoUsec() - t0) / 1000ULL);
    }

    WinHttpCloseHandle(hReq);
    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);
    return true;
}

struct NetState {
    std::mutex mtx;
    bool initialized = false;
    SOCKET udpSock = INVALID_SOCKET;
    uint16_t localUdpPort = 0;
    std::string localLanIp = "127.0.0.1";

    // Network condition simulator (for Phase 11 testing)
    int simOneWayLatencyMs = 0;  // half of simulated RTT
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
    uint64_t lastHeartbeatUsec = 0;
    uint64_t lastRxUsec = 0;
    uint64_t lastPeerTimestampUsec = 0;
    uint64_t nextNonce = 1;

    // Match barrier & frame-indexed lockstep buffers
    bool localAtBarrier = false;
    bool peerAtBarrier = false;
    bool peerAckedBarrier = false;
    uint64_t menuPulseCounter = 0;

    uint64_t matchFrame = 0;
    std::unordered_map<uint64_t, uint32_t> localInputs;
    std::unordered_map<uint64_t, uint32_t> remoteInputs;
    std::unordered_map<uint64_t, uint64_t> localStateHashes;
    std::unordered_map<uint64_t, uint64_t> remoteStateHashes;

    // CLI auto-test state
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
    inet_pton(AF_INET, "8.8.8.8", &dst.sin_addr);
    connect(s, reinterpret_cast<sockaddr*>(&dst), sizeof(dst));
    sockaddr_in name{};
    int namelen = sizeof(name);
    std::string ip = "127.0.0.1";
    if (getsockname(s, reinterpret_cast<sockaddr*>(&name), &namelen) == 0) {
        char buf[64]{};
        if (inet_ntop(AF_INET, &name.sin_addr, buf, sizeof(buf)) && std::strcmp(buf, "0.0.0.0") != 0)
            ip = buf;
    }
    closesocket(s);
    return ip;
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
    LOG_INFO("[netplay] opened UDP socket on %s:%u", g_net.localLanIp.c_str(), g_net.localUdpPort);
    return true;
}

void FlushTxQueueLocked() {
    if (g_net.udpSock == INVALID_SOCKET) return;
    const uint64_t now = MonoUsec();
    for (auto it = g_net.txQueue.begin(); it != g_net.txQueue.end();) {
        if (now >= it->deliverAtUsec) {
            sendto(g_net.udpSock, reinterpret_cast<const char*>(it->bytes.data()),
                   static_cast<int>(it->bytes.size()), 0,
                   reinterpret_cast<const sockaddr*>(&it->dst), sizeof(it->dst));
            ++g_net.snap.packetsSent;
            it = g_net.txQueue.erase(it);
        } else {
            ++it;
        }
    }
}

void QueueUdpSendLocked(const sockaddr_in& dst, const uint8_t* data, size_t len) {
    if (g_net.udpSock == INVALID_SOCKET || !data || !len) return;
    if (g_net.simLossPct > 0) {
        g_net.simRng = g_net.simRng * 1664525u + 1013904223u;
        const int roll = static_cast<int>((g_net.simRng >> 16) % 100u);
        if (roll < g_net.simLossPct) return;  // Simulated packet drop
    }
    uint64_t delayUsec = static_cast<uint64_t>(std::max(0, g_net.simOneWayLatencyMs)) * 1000ULL;
    if (g_net.simJitterMs > 0) {
        g_net.simRng = g_net.simRng * 1664525u + 1013904223u;
        const uint32_t j = (g_net.simRng >> 16) % static_cast<uint32_t>(g_net.simJitterMs * 1000 + 1);
        delayUsec += j;
    }
    if (delayUsec == 0) {
        sendto(g_net.udpSock, reinterpret_cast<const char*>(data), static_cast<int>(len), 0,
               reinterpret_cast<const sockaddr*>(&dst), sizeof(dst));
        ++g_net.snap.packetsSent;
    } else {
        DelayedPacket dp;
        dp.deliverAtUsec = MonoUsec() + delayUsec;
        dp.dst = dst;
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

