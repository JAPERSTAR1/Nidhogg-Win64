#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace ps4::net {

inline constexpr const char* kProtocolVersion = "nidhogg-win64-1.0";

enum class SessionPhase : int {
    Offline = 0,
    LobbyWaiting = 1,
    ConnectingTransport = 2,
    SyncingMenu = 3,
    InMatch = 4,
    Disconnected = 5,
    Error = 6,
};

enum class TransportMode : int {
    None = 0,
    PunchingP2P = 1,
    DirectP2P = 2,
    RelayFallback = 3,
};

struct PublicRoomEntry {
    std::string roomId;
    std::string roomName;
    std::string region;
    int playerCount = 1;
    int maxPlayers = 2;
    std::string status;  // "waiting" or "in_match"
    int estimatedPingMs = 0;
    int stageIndex = 0;  // 0=Castle, 1=Mines, 2=Clouds, 3=Wilds
};

struct NetStatsSnapshot {
    SessionPhase phase = SessionPhase::Offline;
    TransportMode transport = TransportMode::None;
    bool isHost = false;
    bool isPublic = true;
    bool localReady = false;
    bool remoteConnected = false;
    bool remoteReady = false;
    int playerCount = 0;
    int localPlayerNum = 1;  // 1 = Host (Yellow), 2 = Guest (Orange)
    int stageIndex = 0;
    int inputDelayFrames = 2;

    std::string roomId;
    std::string roomName;
    std::string roomCode;    // 6-char code (host & joined peer only)
    std::string region;
    std::string hostName;
    std::string guestName;
    std::string statusText;
    std::string errorText;

    double rttMs = 0.0;
    double jitterMs = 0.0;
    uint64_t matchFrame = 0;
    uint64_t packetsSent = 0;
    uint64_t packetsReceived = 0;
    uint64_t stateChecks = 0;
    uint64_t lastLocalHash = 0;
    uint64_t lastRemoteHash = 0;
    bool desyncDetected = false;
    uint64_t desyncFrame = 0;
};

void InitNetplay();
void ShutdownNetplay();
void TickNetplayLobby();  // Called once per frame from UI/renderer

// Lobby & Matchmaking operations
bool RefreshPublicRooms(const std::string& regionFilter, bool availableOnly,
                        std::vector<PublicRoomEntry>& outRooms, std::string& outError);
bool CreateRoom(const std::string& roomName, bool isPublic, int stageIndex, std::string& outError);
bool JoinRoomById(const std::string& roomId, std::string& outError);
bool JoinRoomByCode(const std::string& roomCode, std::string& outError);
bool QuickMatch(const std::string& preferredRegion, bool& outNoRoomsAvailable,
                std::string& outError);
bool SetRoomVisibility(bool isPublic, std::string& outError);
bool SetReady(bool ready, std::string& outError);
bool KickGuest(std::string& outError);
bool StartMatch(std::string& outError);
void LeaveRoom();
bool CopyRoomCodeToClipboard();

NetStatsSnapshot GetSnapshot();

}  // namespace ps4::net
