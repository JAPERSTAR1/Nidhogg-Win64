#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include "netplay.h"
#include "crypto.h"
#include "websocket.h"
#include "stun.h"
#include "timing.h"

#include "../core/logger.h"
#include "../core/settings.h"
#include "../hle.h"
#include "../gnm/d3d11.h"
#include "../ui/overlay.h"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <mmsystem.h>
#include <winhttp.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <future>
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
constexpr uint8_t kPktBootAgreement = 7;
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

struct BootAgreementPayload {
    uint8_t delay = 0;
    uint8_t flags = 0; // 1=proposal, 2=host committed, 4=guest ack, 8=start
    uint16_t reserved = 0;
    uint64_t hostStartUsec = 0;
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
    uint64_t sampleTimeUsec = 0; // Original capture time, unchanged by retransmission.
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
    bool valid = false;
    bool https = false;
    std::wstring host = L"127.0.0.1";
    std::string hostUtf8 = "127.0.0.1";
    uint16_t port = 27880;
};

ParsedUrl ParseServerUrl(const std::string& url) {
    ParsedUrl u;
    const auto wide = core::Utf8ToWide(url);
    URL_COMPONENTS p{}; p.dwStructSize = sizeof(p);
    p.dwHostNameLength = p.dwUrlPathLength = p.dwExtraInfoLength = p.dwUserNameLength = p.dwPasswordLength = static_cast<DWORD>(-1);
    if (!WinHttpCrackUrl(wide.c_str(), 0, 0, &p) || !p.dwHostNameLength || p.dwUserNameLength || p.dwPasswordLength ||
        p.dwExtraInfoLength || (p.dwUrlPathLength && std::wstring(p.lpszUrlPath, p.dwUrlPathLength) != L"/") ||
        (p.nScheme != INTERNET_SCHEME_HTTP && p.nScheme != INTERNET_SCHEME_HTTPS)) return u;
    u.host.assign(p.lpszHostName, p.dwHostNameLength);
    u.hostUtf8 = core::WideToUtf8(u.host); u.https = p.nScheme == INTERNET_SCHEME_HTTPS;
    u.port = p.nPort; u.valid = true;
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
    if (!u.valid) { outErr = "Server URL must be http://host:port or https://host (without a path)."; return false; }
    const uint64_t t0 = MonoUsec();

    HINTERNET hSession = WinHttpOpen(L"NidhoggPort/1.0", WINHTTP_ACCESS_TYPE_NO_PROXY,
                                     WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSession) { outErr = "WinHttpOpen failed"; return false; }
    WinHttpSetTimeouts(hSession, 3000, 3000, 5000, 5000);

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
        const DWORD error = GetLastError();
        WinHttpCloseHandle(hReq);
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        if (error == ERROR_WINHTTP_NAME_NOT_RESOLVED)
            outErr = "DNS could not find " + u.hostUtf8 + ". Check your connection or retry after DNS updates. (WinHTTP 12007)";
        else outErr = "Lobby server unreachable (" + core::CurrentSettings().lobbyServerUrl +
            "; WinHTTP " + std::to_string(error) + ")";
        return false;
    }

    DWORD statusCode = 0;
    DWORD sz = sizeof(statusCode);
    WinHttpQueryHeaders(hReq, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                        WINHTTP_HEADER_NAME_BY_INDEX, &statusCode, &sz, WINHTTP_NO_HEADER_INDEX);
    outStatus = static_cast<int>(statusCode);

    outResponse.clear();
    bool readOk = true;
    for (;;) {
        DWORD avail = 0;
        if (!WinHttpQueryDataAvailable(hReq, &avail)) { readOk = false; break; }
        if (avail == 0) break;
        if (avail > 1024 * 1024 || outResponse.size() + avail > 1024 * 1024) { readOk = false; break; }
        std::string chunk(avail, '\0');
        DWORD got = 0;
        if (!WinHttpReadData(hReq, chunk.data(), avail, &got) || got == 0) { readOk = false; break; }
        chunk.resize(got);
        outResponse += chunk;
    }

    if (outRttMs) {
        *outRttMs = static_cast<uint32_t>((MonoUsec() - t0) / 1000ULL);
    }

    WinHttpCloseHandle(hReq);
    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);
    if (!readOk) outErr = "Incomplete or oversized response from lobby server.";
    return readOk;
}

struct NetState {
    std::mutex mtx;
    bool initialized = false;
    bool timerPeriodActive = false;
    SOCKET udpSock = INVALID_SOCKET;
    uint16_t localUdpPort = 0;
    std::string localLanIp = "127.0.0.1";
    std::string publicIp;
    uint16_t publicPort = 0;

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
    uint64_t peerNonceHigh = 0;
    uint64_t peerNonceMask = 0;
    uint32_t matchSeed = 0xCAFE1234u;
    bool matchCommitted = false;
    uint16_t relayUdpPort = 27881;
    sockaddr_in relayAddr{};
    bool hasRelayAddr = false;
    bool websocketRelay = false;
    WebSocketRelay websocket;
    bool preferUdpRelay = false;
    bool forceWss = false;
    bool hasRelayCookie = false;
    bool relayRegistered = false;
    uint8_t relayCookie[16]{};
    uint64_t lastRelayAckUsec = 0;
    uint64_t lastRelayProbeUsec = 0;
    uint64_t relayProbeNonce = 0;
    uint64_t relayProbeSentUsec = 0;
    std::vector<double> rttSamples, jitterSamples, frameSamples, waitSamples, inputLatencySamples;
    std::unordered_map<uint64_t, uint64_t> capturedInputUsec;
    uint64_t previousGameStepUsec = 0;
    uint64_t measuredFrames = 0;
    uint64_t measuredFrameUsec = 0;
    uint64_t waitedUsec = 0;

    sockaddr_in peerPublicAddr{};
    bool hasPeerPublic = false;
    sockaddr_in peerLanAddr{};
    bool hasPeerLan = false;
    sockaddr_in activePeerAddr{};
    bool directEstablished = false;
    uint64_t lastDirectRxUsec = 0;
    bool stalled = false;

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
    bool autoExitOnDisconnect = false;
    bool qaCombat = false;
    uint64_t gameStartSubmit = UINT64_MAX;
    bool resumed = false;
    bool restartPending = false;
    int hostStageOverride=-1;
    bool handingOff = false;
    bool bootNavigating = false;
    bool bootAgreementDone = false;
    bool bootCommitted = false;
    bool bootAcked = false;
    bool bootStartSeen = false;
    bool autoDelay = true;
    uint8_t peerDelayProposal = 0;
    int configuredDelay = 2;
    uint64_t bestClockRttUsec = UINT64_MAX;
    int64_t peerClockOffsetUsec = 0;
    uint64_t bootHostStartUsec = 0;
    uint64_t peerSampleFrame = 0, peerSampleLocalUsec = 0;
    bool hasPeerSample = false;
    double phaseErrorUsec = 0;
    uint64_t stageSelectionStart = UINT64_MAX;
};

NetState g_net;
std::atomic<int> g_framePacingAdjustment{0};

struct HeartbeatResult {
    std::string roomId, response, error;
    int status = 0;
    bool ok = false;
};
std::future<HeartbeatResult> g_heartbeat;

void RestartAtDeterministicBoot() {
    std::scoped_lock lock(g_net.mtx);
    if(!g_net.restartPending || g_net.resumed) return;
    g_net.restartPending=false;
    g_net.snap.statusText="Preparing match...";
    ps4::RequestGuestRestart();
}

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
    bindAddr.sin_port = htons(g_net.localUdpPort);
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

void DiscoverPublicEndpoint() {
    SOCKET socket;
    { std::scoped_lock lk(g_net.mtx);
      if (g_net.forceRelay || !g_net.publicIp.empty()) return;
      socket = g_net.udpSock; }
    std::string ip; uint16_t port = 0;
    if (!DiscoverStunEndpoint(socket, ip, port)) return;
    std::scoped_lock lk(g_net.mtx);
    if (g_net.udpSock == socket) {
        g_net.publicIp = ip; g_net.publicPort = port;
        LOG_INFO("[netplay] STUN discovered a public UDP candidate");
    }
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

bool UdpRelayHealthyLocked() {
    return g_net.preferUdpRelay && !g_net.forceWss && g_net.relayRegistered &&
        MonoUsec() - g_net.lastRelayAckUsec < 1500000ULL;
}

void SendUdpRelayLocked(const std::vector<uint8_t>& sealed) {
    if (!g_net.hasRelayAddr) return;
    std::vector<uint8_t> envelope(26 + sealed.size());
    envelope[0] = kPktRelayEnvelope;
    std::memcpy(envelope.data() + 1, &g_net.sessionId, 8);
    envelope[9] = g_net.snap.isHost ? 1 : 2;
    std::memcpy(envelope.data() + 10, g_net.relayCookie, 16);
    std::memcpy(envelope.data() + 26, sealed.data(), sealed.size());
    QueueUdpSendLocked(g_net.relayAddr, envelope.data(), envelope.size());
}

void SendSealedPeerPacketLocked(uint8_t pktType, const void* payload, size_t payloadLen) {
    if (!g_net.hasSessionKey || g_net.sessionId == 0) return;
    WireAad aad{};
    aad.magic = kWireMagic;
    aad.type = pktType;
    aad.senderRole = g_net.snap.isHost ? 1 : 2;
    aad.sessionId = g_net.sessionId;

    std::vector<uint8_t> sealed;
    const uint64_t nonce = (g_net.nextNonce++) | (g_net.snap.isHost ? 0ULL : (1ULL << 63));
    if (!SealPacket(g_net.sessionKey, nonce, reinterpret_cast<const uint8_t*>(&aad),
                    sizeof(aad), static_cast<const uint8_t*>(payload), payloadLen, sealed))
        return;

    auto sendViaRelay = [&]() {
        if (g_net.preferUdpRelay && !g_net.forceWss && g_net.hasRelayAddr) {
            SendUdpRelayLocked(sealed);
            if (UdpRelayHealthyLocked()) return;
        }
        if (g_net.websocketRelay) {
            if (g_net.websocket.Send(sealed)) ++g_net.snap.packetsSent;
            return;
        }
        if (!g_net.hasRelayAddr) return;
        std::vector<uint8_t> env(1 + 8 + 1 + sealed.size());
        env[0] = kPktRelayEnvelope;
        std::memcpy(&env[1], &g_net.sessionId, 8);
        env[9] = aad.senderRole;
        std::memcpy(&env[10], sealed.data(), sealed.size());
        QueueUdpSendLocked(g_net.relayAddr, env.data(), env.size());
    };

    if (g_net.forceRelay) {
        sendViaRelay();
        return;
    }

    if (g_net.directEstablished && MonoUsec() - g_net.lastDirectRxUsec > 500000ULL) {
        g_net.directEstablished = false;
        g_net.snap.transport = TransportMode::RelayFallback;
    }

    if (g_net.directEstablished) {
        QueueUdpSendLocked(g_net.activePeerAddr, sealed.data(), sealed.size());
        if (g_net.stalled) sendViaRelay();
    } else {
        if (g_net.hasPeerPublic)
            QueueUdpSendLocked(g_net.peerPublicAddr, sealed.data(), sealed.size());
        if (g_net.hasPeerLan)
            QueueUdpSendLocked(g_net.peerLanAddr, sealed.data(), sealed.size());
        sendViaRelay();
    }
}

void SendRegisterEndpointToRelayLocked() {
    if (!g_net.preferUdpRelay || g_net.forceWss) return;
    if (!g_net.hasRelayAddr || !g_net.hasSessionKey || g_net.sessionId == 0) return;
    const uint64_t now = MonoUsec();
    if (now - g_net.lastRelayProbeUsec < 500000ULL) return;
    g_net.lastRelayProbeUsec = now;
    WireAad aad{};
    aad.magic = kWireMagic;
    aad.type = kPktRegisterEndpoint;
    aad.senderRole = g_net.snap.isHost ? 1 : 2;
    aad.sessionId = g_net.sessionId;
    const uint64_t ts = MonoUsec();
    std::vector<uint8_t> sealed;
    const uint64_t nonce = g_net.nextNonce++ | (g_net.snap.isHost ? 0ULL : (1ULL << 63));
    g_net.relayProbeNonce = nonce; g_net.relayProbeSentUsec = now;
    if (SealPacket(g_net.sessionKey, nonce, reinterpret_cast<const uint8_t*>(&aad),
                   sizeof(aad), reinterpret_cast<const uint8_t*>(&ts), sizeof(ts), sealed)) {
        SendUdpRelayLocked(sealed);
    }
}

void UpdateRttLocked(uint64_t echoedSendTimeUsec) {
    if (!echoedSendTimeUsec) return;
    const uint64_t now = MonoUsec();
    if (now < echoedSendTimeUsec) return;
    const double sampleMs = static_cast<double>(now - echoedSendTimeUsec) / 1000.0;
    if (sampleMs > 5000.0) return;
    if (g_net.rttSamples.size() < 8192) g_net.rttSamples.push_back(sampleMs);
    if (g_net.snap.rttMs <= 0.01) {
        g_net.snap.rttMs = sampleMs;
        g_net.snap.jitterMs = 0.0;
    } else {
        const double delta = std::fabs(sampleMs - g_net.snap.rttMs);
        if (g_net.jitterSamples.size() < 8192) g_net.jitterSamples.push_back(delta);
        g_net.snap.jitterMs = g_net.snap.jitterMs * 0.85 + delta * 0.15;
        g_net.snap.rttMs = g_net.snap.rttMs * 0.85 + sampleMs * 0.15;
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
        if (!g_net.snap.desyncDetected || (f % 120) == 0) {
          g_net.snap.desyncDetected = true;
          g_net.snap.desyncFrame = f;
        LOG_ERROR("[netplay] DESYNC DETECTED at matchFrame=%llu local=%016llx remote=%016llx",
                  static_cast<unsigned long long>(f),
                  static_cast<unsigned long long>(lit->second),
                  static_cast<unsigned long long>(rit->second));
          g_net.snap.phase = SessionPhase::Error;
          g_net.snap.errorText = "Simulation mismatch. Leave the room and start a new match.";
          g_net.snap.statusText = g_net.snap.errorText;
        }
    } else if ((f % 120) == 0) {
        LOG_INFO("[netplay] frame=%llu mode=%s rtt=%.1fms jitter=%.1fms hash=%016llx sync=OK",
                 static_cast<unsigned long long>(f),
                 g_net.snap.transport == TransportMode::DirectP2P ? "DirectP2P" : g_net.snap.transport == TransportMode::UdpRelay ? "UDPRelay" : "WSSRelay",
                 g_net.snap.rttMs, g_net.snap.jitterMs,
                 static_cast<unsigned long long>(lit->second));
    }
    g_net.localStateHashes.erase(lit);
    g_net.remoteStateHashes.erase(rit);
}

void PumpUdpRxLocked() {
    if (g_net.udpSock == INVALID_SOCKET || !g_net.hasSessionKey) return;
    FlushTxQueueLocked();
    SendRegisterEndpointToRelayLocked();

    uint8_t buf[2048];
    for (int iter = 0; iter < 64; ++iter) {
        sockaddr_in from{};
        int fromLen = sizeof(from);
        std::vector<uint8_t> wsPacket;
        int n = recvfrom(g_net.udpSock, reinterpret_cast<char*>(buf), sizeof(buf), 0,
                         reinterpret_cast<sockaddr*>(&from), &fromLen);
        const bool viaWebSocket = n <= 0 && g_net.websocketRelay && g_net.websocket.Receive(wsPacket);
        if (viaWebSocket) {
            n = static_cast<int>(wsPacket.size());
            if (n > sizeof(buf)) continue;
            std::memcpy(buf, wsPacket.data(), n);
        }
        if (n <= 0) break;

        const bool relayDatagram = !viaWebSocket && g_net.hasRelayAddr &&
            from.sin_addr.s_addr == g_net.relayAddr.sin_addr.s_addr && from.sin_port == g_net.relayAddr.sin_port;
        if (relayDatagram && (n == 26 || n == 34) && (buf[0] == 0x53 || buf[0] == 0x55)) {
            uint64_t session = 0; std::memcpy(&session, buf + 1, 8);
            if (session != g_net.sessionId || buf[9] != (g_net.snap.isHost ? 1 : 2)) continue;
            if (buf[0] == 0x53 && n == 26) {
                std::memcpy(g_net.relayCookie, buf + 10, 16); g_net.hasRelayCookie = true;
                g_net.lastRelayProbeUsec = 0; SendRegisterEndpointToRelayLocked();
            } else if (buf[0] == 0x55 && n == 34 && g_net.hasRelayCookie &&
                       std::memcmp(g_net.relayCookie, buf + 10, 16) == 0) {
                uint64_t nonce = 0; std::memcpy(&nonce, buf + 26, 8);
                if (nonce == g_net.relayProbeNonce) {
                    g_net.snap.relayRttMs = (MonoUsec() - g_net.relayProbeSentUsec) / 1000.0;
                    if (!g_net.relayRegistered) LOG_INFO("[netplay] authenticated UDP relay ready (server RTT=%.1fms)", g_net.snap.relayRttMs);
                    g_net.relayRegistered = true; g_net.lastRelayAckUsec = MonoUsec();
                }
            }
            continue;
        }
        bool cameFromRelay = viaWebSocket || relayDatagram;
        const uint8_t* pkt = buf;
        size_t pktLen = static_cast<size_t>(n);
        if (pktLen > 10 && pkt[0] == kPktRelayEnvelope) {
            cameFromRelay = true;
            pkt += 10;
            pktLen -= 10;
        }
        if (pktLen < sizeof(WireAad) + 8 + 16) continue;

        WireAad aad{};
        std::memcpy(&aad, pkt, sizeof(aad));
        if (aad.magic != kWireMagic || aad.sessionId != g_net.sessionId) continue;
        const uint8_t expectedPeerRole = g_net.snap.isHost ? 2 : 1;
        if (aad.senderRole != expectedPeerRole) continue;

        uint64_t nonce = 0;
        std::vector<uint8_t> plain;
        if (!OpenPacket(g_net.sessionKey, pkt, pktLen, sizeof(WireAad), nonce, plain)) {
            continue;  // Failed HMAC authentication
        }
        if (nonce > g_net.peerNonceHigh) {
            const uint64_t shift = nonce - g_net.peerNonceHigh;
            g_net.peerNonceMask = shift >= 64 ? 1 : ((g_net.peerNonceMask << shift) | 1);
            g_net.peerNonceHigh = nonce;
        } else {
            const uint64_t age = g_net.peerNonceHigh - nonce;
            if (age >= 64 || (g_net.peerNonceMask & (1ULL << age))) continue;
            g_net.peerNonceMask |= 1ULL << age;
        }

        ++g_net.snap.packetsReceived;
        g_net.lastRxUsec = MonoUsec();

        if (!cameFromRelay && !g_net.forceRelay) {
            g_net.lastDirectRxUsec = MonoUsec();
            g_net.directEstablished = true;
            g_net.activePeerAddr = from;
            g_net.snap.transport = TransportMode::DirectP2P;
        } else if (g_net.snap.transport != TransportMode::DirectP2P) {
            g_net.snap.transport = relayDatagram ? TransportMode::UdpRelay :
                UdpRelayHealthyLocked() ? TransportMode::UdpRelay : TransportMode::RelayFallback;
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
                    if (!g_net.bootCommitted) {
                        const auto clock = timing::EstimatePeerClock(echoed, peerTs, MonoUsec());
                        if (clock.valid && clock.rttUsec < g_net.bestClockRttUsec) {
                            g_net.bestClockRttUsec = clock.rttUsec;
                            g_net.peerClockOffsetUsec = clock.peerOffsetUsec;
                        }
                    }
                }
                if (g_net.snap.phase == SessionPhase::ConnectingTransport) {
                    g_net.snap.phase = SessionPhase::SyncingMenu;
                    g_net.snap.statusText = "Connected — synchronizing stage entry...";
                    LOG_INFO("[netplay] transport established (%s, rtt=%.1fms)",
                             g_net.snap.transport == TransportMode::DirectP2P ? "DirectP2P" :
                             g_net.snap.transport == TransportMode::UdpRelay ? "UDPRelay" : "WSSRelay",
                             g_net.snap.rttMs);
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
                    g_net.snap.inputDelayFrames = std::clamp<int>(sb.inputDelay, 1, 12);
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
                if (g_net.resumed && g_net.bootCommitted && !g_net.snap.isHost)
                    g_net.bootStartSeen = true; // A first input is an implicit start acknowledgement.
                g_net.lastPeerTimestampUsec = ip.sendTimeUsec;
                UpdateRttLocked(ip.echoTimeUsec);

                // Peer entering InMatch means peer passed the barrier too
                g_net.peerAtBarrier = true;
                g_net.peerAckedBarrier = true;

                const uint8_t count = std::min<uint8_t>(ip.historyCount, 16);
                if (!count || ip.latestFrame > g_net.matchFrame + 64) break;
                const auto delay = static_cast<uint64_t>(g_net.snap.inputDelayFrames);
                if (ip.sampleTimeUsec && ip.latestFrame >= delay &&
                    (!g_net.hasPeerSample || ip.latestFrame - delay > g_net.peerSampleFrame)) {
                    const auto mapped = timing::LocalDeadline(ip.sampleTimeUsec, g_net.peerClockOffsetUsec);
                    if (mapped) {
                        g_net.peerSampleFrame = ip.latestFrame - delay;
                        g_net.peerSampleLocalUsec = mapped;
                        g_net.hasPeerSample = true;
                    }
                }
                for (uint8_t k = 0; k < count; ++k) {
                    if (ip.latestFrame >= k && ip.latestFrame - k + 64 >= g_net.matchFrame) {
                        g_net.remoteInputs[ip.latestFrame - k] = ip.buttonsHistory[k];
                    }
                }
                if (ip.flags & 1u) {
                    g_net.remoteStateHashes[ip.stateHashFrame] = ip.stateHash;
                    VerifyHashesForFrameLocked(ip.stateHashFrame);
                }
                break;
            }
            case kPktBootAgreement: {
                if (plain.size() != sizeof(BootAgreementPayload) || !g_net.resumed || g_net.bootAgreementDone) break;
                BootAgreementPayload agreement{}; std::memcpy(&agreement, plain.data(), sizeof(agreement));
                if (agreement.delay < 1 || agreement.delay > 12) break;
                if (agreement.flags & 1) g_net.peerDelayProposal = agreement.delay;
                if (!g_net.snap.isHost && (agreement.flags & 2)) {
                    if((agreement.reserved&0xfffc)==0x100) g_net.snap.stageIndex=agreement.reserved&3;
                    if (!agreement.hostStartUsec) break;
                    g_net.snap.inputDelayFrames = agreement.delay; g_net.bootCommitted = true;
                    g_net.bootHostStartUsec = agreement.hostStartUsec;
                }
                if (g_net.snap.isHost && g_net.bootCommitted && (agreement.flags & 4) &&
                    agreement.delay == g_net.snap.inputDelayFrames) g_net.bootAcked = true;
                if (!g_net.snap.isHost && g_net.bootCommitted && (agreement.flags & 8) &&
                    agreement.delay == g_net.snap.inputDelayFrames) g_net.bootStartSeen = true;
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
    const auto captured = g_net.capturedInputUsec.find(targetFrame);
    if (captured != g_net.capturedInputUsec.end()) ip.sampleTimeUsec = captured->second;
    SendSealedPeerPacketLocked(kPktInput, &ip, sizeof(ip));
}

#include "boot_negotiation.inc"

bool NetplayStepCallbackImpl(uint64_t simFrame, uint32_t localButtons,
                             uint32_t& outP1Buttons, uint32_t& outP2Buttons,
                             uint32_t& ioRandState) {
    std::unique_lock<std::mutex> lk(g_net.mtx);
    PumpUdpRxLocked();
    if (g_net.snap.phase == SessionPhase::InMatch && g_net.websocketRelay &&
        !g_net.websocket.Connected() && !g_net.websocket.Error().empty() && !g_net.directEstablished && !UdpRelayHealthyLocked()) {
        g_net.snap.phase = SessionPhase::Disconnected;
        g_net.snap.statusText = g_net.websocket.Error();
    }
    bool shownDisconnect = false;
    while ((g_net.snap.phase == SessionPhase::Error || g_net.snap.phase == SessionPhase::Disconnected) &&
           !ps4::IsQuitRequested()) {
        if (g_net.autoExitOnDisconnect) {
            LOG_INFO("[netplay] match stopped: %s", g_net.snap.statusText.c_str());
            ps4::RequestQuit(); break;
        }
        if (!shownDisconnect) { ps4::ui::OpenOverlayTab(4); shownDisconnect = true; }
        lk.unlock();
        gnm::PresentCachedFrame(ps4::GameWindow());
        MSG message;
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&message); DispatchMessageW(&message);
        }
        Sleep(10); lk.lock();
    }

    if (g_net.resumed && !g_net.bootAgreementDone && g_net.snap.phase == SessionPhase::InMatch &&
        !NegotiateBootDelayLocked(lk)) return false;

    if (g_net.snap.phase == SessionPhase::ConnectingTransport) {
        const uint64_t now = MonoUsec();
        if (now - g_net.connectStartUsec > 1200000ULL &&
            g_net.snap.transport == TransportMode::PunchingP2P) {
            g_net.snap.transport = TransportMode::RelayFallback;
        }
        uint64_t ts = now;
        SendSealedPeerPacketLocked(kPktPunch, &ts, sizeof(ts));
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
            // Pulse Cross for 8 frames every 50 frames to advance rLogos -> rMainMenu ->
            // Multiplayer -> Offline Versus -> rLevelSelect cleanly.
            if ((g_net.menuPulseCounter % 50) < 8) navBtn = 0x4000u;
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

        if (g_net.localAtBarrier && g_net.peerAtBarrier && g_net.peerAckedBarrier) {
            // Both instances are parked at rLevelSelect and have ACKed the barrier!
            // Lock RNG + clock and transition synchronously into InMatch frame 0.
            g_net.snap.phase = SessionPhase::InMatch;
            g_net.snap.statusText = "In Match — Synchronized";
            g_net.matchFrame = 0;
            g_net.localInputs.clear();
            g_net.remoteInputs.clear();
            g_net.localStateHashes.clear();
            g_net.remoteStateHashes.clear();
            const int delay = std::clamp(g_net.snap.inputDelayFrames, 1, 12);
            for (int d = 0; d < delay; ++d) {
                g_net.localInputs[static_cast<uint64_t>(d)] = 0u;
                g_net.remoteInputs[static_cast<uint64_t>(d)] = 0u;
            }
            ioRandState = g_net.matchSeed;
            LOG_INFO("[netplay] BARRIER SYNCED -> entering InMatch (seed=0x%08x, stage=%d, delay=%df, mode=%s)",
                     g_net.matchSeed, g_net.snap.stageIndex, delay,
                     g_net.snap.transport == TransportMode::DirectP2P ? "DirectP2P" : g_net.snap.transport == TransportMode::UdpRelay ? "UDPRelay" : "WSSRelay");
        } else {
            outP1Buttons = navBtn;
            outP2Buttons = 0;
            return true;
        }
    }

    if (g_net.snap.phase != SessionPhase::InMatch || ps4::IsQuitRequested()) {
        return false;
    }

    const uint64_t f = g_net.matchFrame;
    const uint64_t stepNow = MonoUsec();
    if (g_net.hasPeerSample) {
        const auto delta = static_cast<int64_t>(f) - static_cast<int64_t>(g_net.peerSampleFrame);
        const double error = static_cast<double>(stepNow) -
            (static_cast<double>(g_net.peerSampleLocalUsec) + delta * 16667.0);
        if (std::abs(delta) <= 64 && std::abs(error) < 1000000.0) {
            g_net.phaseErrorUsec += 0.05 * (error - g_net.phaseErrorUsec);
            g_framePacingAdjustment.store(timing::FramePacingCorrection(g_net.phaseErrorUsec), std::memory_order_relaxed);
        }
    }
    if (g_net.gameStartSubmit != UINT64_MAX) {
        if (g_net.previousGameStepUsec) {
            const uint64_t interval = stepNow - g_net.previousGameStepUsec;
            ++g_net.measuredFrames; g_net.measuredFrameUsec += interval;
            if (g_net.frameSamples.size() < 8192) g_net.frameSamples.push_back(interval / 1000.0);
            g_net.snap.simulationFps = g_net.measuredFrames * 1000000.0 / std::max<uint64_t>(1, g_net.measuredFrameUsec);
        }
        g_net.previousGameStepUsec = stepNow;
    }
    ioRandState = g_net.matchSeed ^ static_cast<uint32_t>(f * 0x9e3779b9ULL);
    if (f == 0 || f == 30 || (f % 300) == 0)
        LOG_INFO("[netplay] input frame=%llu submit=%llu room=%s", f, simFrame, ps4::CurrentRoom().c_str());
    const uint32_t delay = static_cast<uint32_t>(std::clamp(g_net.snap.inputDelayFrames, 1, 12));

    // Deterministic stage-selection macro on frames 0..150 inside rLevelSelect so
    // both peers enter the exact chosen stage on the exact same simulation frame.
    uint32_t effectiveLocal = localButtons;
    if (g_net.bootNavigating) {
        effectiveLocal = 0;
        if (ps4::CurrentRoom() == "rLevelSelect") {
            g_net.bootNavigating = false;
            g_net.stageSelectionStart = f;
            LOG_INFO("[netplay] deterministic boot reached stage selection at frame=%llu", f);
        } else if (g_net.snap.isHost && (f % 50) < 8) effectiveLocal = 0x4000u;
    }
    const uint64_t stageFrame = f - g_net.stageSelectionStart;
    if (!g_net.bootNavigating && stageFrame < 130) {
        effectiveLocal = 0;
        if (g_net.snap.isHost) {
            const int stg = g_net.snap.stageIndex;
            if (stg == 3 && stageFrame >= 15 && stageFrame < 23) effectiveLocal = 0x0040u;
            else if (stg == 1 && ((stageFrame >= 15 && stageFrame < 23) || (stageFrame >= 60 && stageFrame < 68)))
                effectiveLocal = 0x0040u;
            else if (stg == 2 && stageFrame >= 15 && stageFrame < 23) effectiveLocal = 0x0010u;
            if (stageFrame >= 115 && stageFrame < 125) effectiveLocal = 0x4000u;
        }
    }
    // Test-only controls expressed relative to the original match-start event,
    // including negotiated delay. They exercise real input, never mutate actors.
    if (g_net.qaCombat && g_net.gameStartSubmit != UINT64_MAX) {
        const uint64_t c = simFrame + delay - g_net.gameStartSubmit;
        effectiveLocal = 0;
        auto held = [c](uint64_t start, uint64_t length) { return c >= start && c < start + length; };
        if (g_net.snap.isHost) {
            if (held(40, 35)) effectiveLocal |= 0x0040;
            if (held(80, 20)) effectiveLocal |= 0x0010;
            if (held(90, 8)) effectiveLocal |= 0x8000;
            if (held(115, 2200)) effectiveLocal |= 0x0020;
            if (c >= 200 && c < 2200 && (c - 200) % 95 < 12) effectiveLocal |= 0x4000;
        } else {
            if (held(40, 20)) effectiveLocal |= 0x0010;
            if (held(50, 8)) effectiveLocal |= 0x8000;
            if (held(135, 2200)) effectiveLocal |= 0x0080;
        }
    }

    const uint64_t futureFrame = f + delay;
    g_net.localInputs[futureFrame] = effectiveLocal;
    g_net.capturedInputUsec[futureFrame] = MonoUsec();

    bool sendHash = false;
    uint64_t curHash = 0;
    if (f > 0 && (f % 30) == 0 && !g_net.bootNavigating && stageFrame >= 160) {
        curHash = ps4::ComputeGuestStateHash(f);
        g_net.localStateHashes[f] = curHash;
        VerifyHashesForFrameLocked(f);
        sendHash = true;
    }

    SendInputPacketForFrameLocked(futureFrame, sendHash, f, curHash);
    FlushTxQueueLocked();

    // Redundant input history covers isolated loss. Bound retries to 50 Hz so a
    // slow route does not build a queue of obsolete TCP/WebSocket duplicates.
    const uint64_t waitStart = MonoUsec();
    uint64_t lastRetx = waitStart;
    uint64_t lastWaitingPresent = waitStart;
    while (g_net.remoteInputs.find(f) == g_net.remoteInputs.end()) {
        PumpUdpRxLocked();
        if (g_net.remoteInputs.find(f) != g_net.remoteInputs.end()) break;
        if (g_net.snap.phase != SessionPhase::InMatch || ps4::IsQuitRequested()) {
            return false;
        }
        const uint64_t now = MonoUsec();
        if (now - waitStart > 5000000ULL) {  // 5.0s peer timeout
            g_net.snap.phase = SessionPhase::Disconnected;
            g_net.snap.remoteConnected = false;
            g_net.snap.statusText = "Connection lost (peer timed out).";
            LOG_WARN("[netplay] peer timed out waiting for frame %llu",
                     static_cast<unsigned long long>(f));
            return false;
        }
        if (now - lastRetx >= 20000ULL) {
            g_net.stalled = now - waitStart > 80000ULL;
            SendInputPacketForFrameLocked(futureFrame, sendHash, f, curHash);
            FlushTxQueueLocked();
            lastRetx = now;
        }
        lk.unlock();
        if (now - lastWaitingPresent >= 16000ULL) {
            gnm::PresentCachedFrame(ps4::GameWindow()); lastWaitingPresent = now;
        }
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        Sleep(1);
        lk.lock();
    }
    g_net.stalled = false;
    const uint64_t waited = MonoUsec() - waitStart;
    if (g_net.gameStartSubmit != UINT64_MAX) {
        g_net.waitedUsec += waited;
        if (waited > 1000) ++g_net.snap.waitedFrames;
        if (g_net.waitSamples.size() < 8192) g_net.waitSamples.push_back(waited / 1000.0);
        g_net.snap.inputWaitMs = waited / 1000.0;
    }

    const uint32_t myInput = g_net.localInputs[f];
    const uint32_t peerInput = g_net.remoteInputs[f];
    auto captured = g_net.capturedInputUsec.find(f);
    if (captured != g_net.capturedInputUsec.end()) {
        if (g_net.gameStartSubmit != UINT64_MAX && g_net.inputLatencySamples.size() < 8192)
            g_net.inputLatencySamples.push_back((MonoUsec() - captured->second) / 1000.0);
        g_net.capturedInputUsec.erase(captured);
    }
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

void ApplyRoomStateFromJsonLocked(const std::string& resp) {
    std::string roomStatus;
    JsonGetStr(resp, "status", roomStatus);
    // A lobby heartbeat may finish after the host's Start request. Its old
    // seed/key must never replace the match credentials during the local reset.
    if(g_net.matchCommitted && roomStatus=="waiting") return;
    if(roomStatus=="in_match") g_net.matchCommitted=true;
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
    int64_t agreedDelay = 3;
    if (JsonGetInt(resp, "inputDelayFrames", agreedDelay)) {
        g_net.configuredDelay = std::clamp<int>(static_cast<int>(agreedDelay), 1, 12);
        if (!g_net.bootCommitted && !g_net.bootAgreementDone) g_net.snap.inputDelayFrames = g_net.configuredDelay;
    }
    JsonGetBool(resp, "autoInputDelay", g_net.autoDelay);
    if (JsonGetInt(resp, "playerCount", pCount)) g_net.snap.playerCount = static_cast<int>(pCount);
    if ((!g_net.resumed || g_net.snap.phase==SessionPhase::LobbyWaiting) && JsonGetInt(resp, "stageIndex", stageIdx)) g_net.snap.stageIndex =
        g_net.snap.isHost && g_net.hostStageOverride>=0?g_net.hostStageOverride:static_cast<int>(stageIdx);
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
    std::string relayTransport;
    if (JsonGetStr(resp, "relayTransport", relayTransport) &&
        (relayTransport == "websocket" || relayTransport == "udp")) {
        g_net.preferUdpRelay = relayTransport == "udp";
        if (!g_net.websocketRelay) {
            g_net.websocketRelay = true;
            g_net.websocket.Start(core::CurrentSettings().lobbyServerUrl, g_net.snap.roomId, g_net.authToken);
        }
    }

    // Configure relay UDP sockaddr
    const ParsedUrl u = ParseServerUrl(core::CurrentSettings().lobbyServerUrl);
    sockaddr_in raddr{};
    raddr.sin_family = AF_INET;
    raddr.sin_port = htons(g_net.relayUdpPort);
    std::string relayPublicIp;
    JsonGetStr(resp, "relayPublicIp", relayPublicIp);
    const std::string relayHost = relayPublicIp.empty() ? u.hostUtf8 : relayPublicIp;
    if (inet_pton(AF_INET, relayHost.c_str(), &raddr.sin_addr) == 1) {
        g_net.relayAddr = raddr;
        g_net.hasRelayAddr = true;
    }

    // Peer endpoints for UDP hole punching
    std::string peerPubIp, peerLanIp;
    int64_t peerPubPort = 0, peerLanPort = 0;
    if (JsonGetStr(resp, "peerPublicIp", peerPubIp) && JsonGetInt(resp, "peerPublicPort", peerPubPort) &&
        !peerPubIp.empty() && peerPubPort > 0 && peerPubPort <= 65535) {
        sockaddr_in pa{};
        pa.sin_family = AF_INET;
        pa.sin_port = htons(static_cast<uint16_t>(peerPubPort));
        if (inet_pton(AF_INET, peerPubIp.c_str(), &pa.sin_addr) == 1) {
            g_net.peerPublicAddr = pa;
            g_net.hasPeerPublic = true;
        }
    }
    if (JsonGetStr(resp, "peerLanIp", peerLanIp) && JsonGetInt(resp, "peerLanPort", peerLanPort) &&
        !peerLanIp.empty() && peerLanPort > 0 && peerLanPort <= 65535) {
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

    if (roomStatus == "closed") {
        g_net.snap.phase = SessionPhase::Disconnected;
        g_net.snap.statusText = "Host closed the room.";
        g_net.authToken.clear();
        ps4::SetNetplayStepCallback(nullptr);
        return;
    }

    if (roomStatus == "in_match" && g_net.snap.phase == SessionPhase::LobbyWaiting &&
        g_net.hasSessionKey) {
        if (!g_net.resumed) {
            g_net.restartPending = true;
            g_net.snap.statusText = "Starting a clean synchronized game...";
            return;
        }
        g_net.snap.phase = SessionPhase::ConnectingTransport;
        g_net.snap.transport = g_net.forceRelay ? TransportMode::RelayFallback
                                                : TransportMode::PunchingP2P;
        g_net.snap.inputDelayFrames = std::clamp(core::CurrentSettings().inputDelayFrames, 1, 12);
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
    g_net.forceWss = envInt("NIDHOGG_FORCE_WSS", 0) != 0;
    g_net.forceRelay = envInt("NIDHOGG_FORCE_RELAY", 0) != 0;
    g_net.autoExitOnDisconnect = envInt("NIDHOGG_NET_AUTO_EXIT", 0) != 0;
    g_net.qaCombat = envInt("NIDHOGG_NET_QA_COMBAT", 0) != 0;
}

}  // namespace

void InitNetplay() {
    std::scoped_lock lk(g_net.mtx);
    if (g_net.initialized) return;
    WSADATA wsa{};
    WSAStartup(MAKEWORD(2, 2), &wsa);
    g_net.timerPeriodActive = timeBeginPeriod(1) == TIMERR_NOERROR;
    CheckEnvSimConfigLocked();
    g_net.initialized = true;
}

bool ResumeOnlineSession() {
    char* value = nullptr; size_t size = 0;
    if (_dupenv_s(&value, &size, "NIDHOGG_NET_RESUME") != 0 || !value) return true;
    std::string metadata(value); std::free(value); _putenv_s("NIDHOGG_NET_RESUME", "");
    _putenv_s("NIDHOGG_OPEN_ONLINE", ""); _putenv_s("NIDHOGG_OPEN_SETTINGS", "");
    std::string room, token; bool host = false;
    if (!JsonGetStr(metadata, "roomId", room) || !JsonGetStr(metadata, "authToken", token) ||
        !JsonGetBool(metadata, "isHost", host)) return false;
    InitNetplay();
    int status = 0; std::string response, error;
    if (!HttpRequestJson("POST", "/api/rooms/heartbeat", "{\"roomId\":\"" + JsonEsc(room) +
        "\",\"authToken\":\"" + JsonEsc(token) + "\"}", status, response, error) || status != 200) {
        LOG_ERROR("[netplay] cannot resume room (HTTP %d): %s", status, error.c_str()); return false;
    }
    std::unique_lock lk(g_net.mtx);
    int64_t originalPort = 0;
    if (JsonGetInt(metadata, "udpPort", originalPort) && originalPort > 0 && originalPort <= 65535)
        g_net.localUdpPort = static_cast<uint16_t>(originalPort);
    if (!EnsureUdpSocketLocked()) return false;
    lk.unlock(); DiscoverPublicEndpoint(); lk.lock();
    g_net.resumed = true; g_net.cliHandled = true;
    g_net.authToken = token;
    g_net.snap.isHost = host; g_net.snap.localPlayerNum = host ? 1 : 2;
    g_net.snap.phase = SessionPhase::InMatch;
    g_net.snap.transport = TransportMode::RelayFallback;
    g_net.snap.inputDelayFrames = 3; // Identical negotiated value for both peers.
    g_net.snap.statusText = "Starting synchronized game...";
    ApplyRoomStateFromJsonLocked(response);
    if (!g_net.hasSessionKey) return false;
    g_net.bootNavigating = true;
    for (int f = 0; f < g_net.snap.inputDelayFrames; ++f) {
        g_net.localInputs[f] = 0; g_net.remoteInputs[f] = 0;
    }
    ps4::BeginDeterministicSession(g_net.matchSeed);
    ps4::SetNetplayStepCallback(&NetplayStepCallbackImpl);
    LOG_INFO("[netplay] BARRIER SYNCED -> deterministic boot (seed=0x%08x, role=%s)",
        g_net.matchSeed, host ? "host" : "guest");
    return true;
}

void CompleteInProcessBoot() {
    ps4::ui::BeginMatchTransition();
    std::scoped_lock lock(g_net.mtx);
    g_net.resumed=true;g_net.bootNavigating=true;g_net.bootAgreementDone=false;
    g_net.bootCommitted=g_net.bootAcked=g_net.bootStartSeen=false;
    g_net.peerDelayProposal=0;g_net.bestClockRttUsec=UINT64_MAX;
    g_net.matchFrame=0;g_net.stageSelectionStart=UINT64_MAX;
    g_net.gameStartSubmit=UINT64_MAX;
    g_net.localInputs.clear();g_net.remoteInputs.clear();
    g_net.localStateHashes.clear();g_net.remoteStateHashes.clear();
    g_net.snap.phase=SessionPhase::InMatch;
    g_net.snap.statusText="Preparing synchronized match...";
    g_net.snap.inputDelayFrames=3;
    for(int f=0;f<3;++f) {g_net.localInputs[f]=0;g_net.remoteInputs[f]=0;}
    ps4::BeginDeterministicSession(g_net.matchSeed);
    ps4::SetNetplayStepCallback(&NetplayStepCallbackImpl);
    LOG_INFO("[netplay] BARRIER SYNCED -> in-process deterministic boot pid=%lu",GetCurrentProcessId());
}

void NotifyOriginalMatchStart(uint64_t submittedFrame) {
    std::scoped_lock lk(g_net.mtx);
    if (g_net.resumed) g_net.gameStartSubmit = submittedFrame;
}

void ShutdownNetplay() {
    if (g_heartbeat.valid()) g_heartbeat.wait();
    LOG_INFO("[netplay] summary frames=%llu checks=%llu desync=%d",
        g_net.snap.matchFrame, g_net.snap.stateChecks, g_net.snap.desyncDetected ? 1 : 0);
    auto percentile = [](std::vector<double> samples, double p) {
        if (samples.empty()) return 0.0;
        std::sort(samples.begin(), samples.end());
        return samples[static_cast<size_t>((samples.size() - 1) * p)];
    };
    LOG_INFO("[netplay-metrics] fps=%.2f frameP50=%.2fms frameP95=%.2fms waitP95=%.2fms waitedFrames=%llu rttP50=%.2fms rttP95=%.2fms jitterP95=%.2fms relayRtt=%.2fms sent=%llu received=%llu",
        g_net.snap.simulationFps, percentile(g_net.frameSamples, .5), percentile(g_net.frameSamples, .95),
        percentile(g_net.waitSamples, .95), g_net.snap.waitedFrames,
        percentile(g_net.rttSamples, .5), percentile(g_net.rttSamples, .95),
        percentile(g_net.jitterSamples, .95), g_net.snap.relayRttMs, g_net.snap.packetsSent, g_net.snap.packetsReceived);
    LOG_INFO("[netplay-input] sampleToSimulationP50=%.2fms sampleToSimulationP95=%.2fms delayFrames=%d samples=%llu",
        percentile(g_net.inputLatencySamples, .5), percentile(g_net.inputLatencySamples, .95),
        g_net.snap.inputDelayFrames, static_cast<unsigned long long>(g_net.inputLatencySamples.size()));
    if (!g_net.handingOff) LeaveRoom();
    else g_net.websocket.Stop();
    std::scoped_lock lk(g_net.mtx);
    if (g_net.udpSock != INVALID_SOCKET) {
        closesocket(g_net.udpSock);
        g_net.udpSock = INVALID_SOCKET;
    }
    if (g_net.initialized) {
        if (g_net.timerPeriodActive) { timeEndPeriod(1); g_net.timerPeriodActive = false; }
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

    DiscoverPublicEndpoint();
    const auto& cfg = core::CurrentSettings();
    const std::string rName = roomName.empty() ? (cfg.playerName + "'s Arena") : roomName;
    std::ostringstream body;
    body << "{\"roomName\":\"" << JsonEsc(rName) << "\","
         << "\"hostName\":\"" << JsonEsc(cfg.playerName) << "\","
         << "\"region\":\"" << JsonEsc(cfg.region) << "\","
         << "\"isPublic\":" << (isPublic ? "true" : "false") << ","
         << "\"stageIndex\":" << stageIndex << ","
         << "\"inputDelayFrames\":" << cfg.inputDelayFrames << ","
         << "\"autoInputDelay\":" << (cfg.autoInputDelay ? "true" : "false") << ","
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

    DiscoverPublicEndpoint();
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
    g_net.snap.statusText = "Joined room — waiting for the host to start.";
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

    DiscoverPublicEndpoint();
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
    g_net.snap.statusText = "Joined private room via code — waiting for the host to start.";
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

    DiscoverPublicEndpoint();
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
    g_net.snap.statusText = "Quick Match found opponent! Waiting for the host to start.";
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

bool SelectRoomStage(int stageIndex,std::string& outError) {
    std::scoped_lock lock(g_net.mtx);
    if(!g_net.snap.isHost || g_net.snap.phase!=SessionPhase::LobbyWaiting || stageIndex<0 || stageIndex>3) {
        outError="Only the host can choose a map before starting.";return false;
    }
    g_net.hostStageOverride=stageIndex;g_net.snap.stageIndex=stageIndex;return true;
}

bool StartMatch(std::string& outError) {
    // Old deployed lobbies still require the readiness fields. Clients set
    // these automatically; there is no player-facing Ready step.
    if(!SetReady(true,outError)) return false;
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
    std::string startBody=body.str();startBody.pop_back();
    startBody+=",\"stageIndex\":"+std::to_string(GetSnapshot().stageIndex)+"}";
    const uint64_t deadline=MonoUsec()+3000000;
    do {
        if (!HttpRequestJson("POST", "/api/rooms/start", startBody, status, resp, outError)) return false;
        if(status!=409 || resp.find("ready")==std::string::npos || MonoUsec()>=deadline) break;
        Sleep(150);
    } while(true);
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
    g_framePacingAdjustment.store(0, std::memory_order_relaxed);
    ps4::SetNetplayStepCallback(nullptr);
    {std::scoped_lock lock(g_net.mtx);g_net.hostStageOverride=-1;g_net.resumed=false;g_net.restartPending=false;g_net.matchCommitted=false;}
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
        g_net.peerNonceHigh = 0; g_net.peerNonceMask = 0;
        g_net.websocket.Stop();
        g_net.websocketRelay = false;
        g_net.preferUdpRelay = false; g_net.hasRelayCookie = false;
        g_net.relayRegistered = false; g_net.lastRelayAckUsec = 0;
        g_net.lastRelayProbeUsec = 0; std::memset(g_net.relayCookie, 0, sizeof(g_net.relayCookie));
        g_net.bootAgreementDone = false; g_net.bootCommitted = false;
        g_net.bootAcked = false; g_net.bootStartSeen = false; g_net.peerDelayProposal = 0;
        g_net.bestClockRttUsec = UINT64_MAX; g_net.peerClockOffsetUsec = 0;
        g_net.bootHostStartUsec = 0; g_net.hasPeerSample = false; g_net.phaseErrorUsec = 0;
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

int FramePacingAdjustmentUsec() { return g_framePacingAdjustment.load(std::memory_order_relaxed); }

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
            const auto chosenStage=getEnv("NIDHOGG_NET_QA_STAGE");
            if(!chosenStage.empty()) SelectRoomStage(std::clamp(std::atoi(chosenStage.c_str()),0,3),err);
            const std::string codeOut = getEnv("NIDHOGG_NET_CODE_OUT");
            if (!codeOut.empty()) {
                if (std::FILE* f = core::FopenUtf8(codeOut, "wb")) {
                    auto sc = GetSnapshot().roomCode;
                    std::fwrite(sc.data(), 1, sc.size(), f);
                    std::fclose(f);
                }
            }
        } else if (mode == "join_code" && !arg.empty()) {
            JoinRoomByCode(arg, err);
        } else if (mode == "quickmatch") {
            bool none = false;
            QuickMatch(arg, none, err);
        }
        if (!err.empty()) LOG_WARN("[netplay-auto] mode=%s err=%s", mode.c_str(), err.c_str());
    }

    std::string roomId, token, heartbeatBody;
    bool shouldPoll = false,autoReady=false;
    bool autoStartReady = false;
    {
        std::scoped_lock lk(g_net.mtx);
        FlushTxQueueLocked();
        if (g_net.snap.phase == SessionPhase::LobbyWaiting) {
            PumpUdpRxLocked();
        }
        const uint64_t now = MonoUsec();
        if (!g_net.snap.roomId.empty() && !g_net.authToken.empty() &&
            now - g_net.lastHeartbeatUsec >= (g_net.snap.phase == SessionPhase::LobbyWaiting ? 400000ULL : 2000000ULL)) {
            g_net.lastHeartbeatUsec = now;
            roomId = g_net.snap.roomId;
            token = g_net.authToken;
            autoReady=g_net.snap.phase==SessionPhase::LobbyWaiting && !g_net.snap.localReady;
            heartbeatBody = "{\"roomId\":\"" + JsonEsc(roomId) + "\",\"authToken\":\"" + JsonEsc(token) +
                "\",\"lanIp\":\"" + g_net.localLanIp + "\",\"lanPort\":" + std::to_string(g_net.localUdpPort) +
                ",\"publicIp\":\"" + g_net.publicIp + "\",\"publicPort\":" + std::to_string(g_net.publicPort) + "}";
            shouldPoll = true;
        }
    }

    if (shouldPoll && !g_heartbeat.valid()) {
        if(autoReady) {heartbeatBody.pop_back();heartbeatBody+=",\"ready\":true}";}
        g_heartbeat = std::async(std::launch::async, [roomId, heartbeatBody,autoReady] {
            HeartbeatResult result; result.roomId = roomId;
            result.ok = HttpRequestJson("POST", autoReady?"/api/rooms/ready":"/api/rooms/heartbeat", heartbeatBody,
                result.status, result.response, result.error);
            return result;
        });
    }
    if (g_heartbeat.valid() && g_heartbeat.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        const auto result = g_heartbeat.get();
        roomId = result.roomId;
        if (result.ok && result.status == 200) {
            std::scoped_lock lk(g_net.mtx);
            if (g_net.snap.roomId == roomId) {
                ApplyRoomStateFromJsonLocked(result.response);
                if (g_net.snap.phase == SessionPhase::LobbyWaiting) {
                    SendRegisterEndpointToRelayLocked();
                    if (g_net.cliAutoStart && g_net.snap.isHost && g_net.snap.remoteConnected &&
                        g_net.snap.remoteReady) {
                        autoStartReady = true;
                    }
                }
            }
        } else if (result.status == 404 || result.status == 403) {
            std::scoped_lock lk(g_net.mtx);
            if (g_net.snap.roomId == roomId && g_net.snap.phase == SessionPhase::LobbyWaiting) {
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
    RestartAtDeterministicBoot();
}

NetStatsSnapshot GetSnapshot() {
    std::scoped_lock lk(g_net.mtx);
    return g_net.snap;
}

}  // namespace ps4::net
