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
