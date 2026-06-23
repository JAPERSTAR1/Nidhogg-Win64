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

