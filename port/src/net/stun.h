#pragma once
#include <winsock2.h>
#include <string>
namespace ps4::net {
// Best-effort IPv4 server-reflexive candidate on the gameplay socket.
// This is STUN-assisted hole punching, not a complete ICE implementation.
bool DiscoverStunEndpoint(SOCKET socket, std::string& ip, uint16_t& port);
}
