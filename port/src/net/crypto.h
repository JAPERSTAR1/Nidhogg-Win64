#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace ps4::net {

// Fills `out` with OS CSPRNG bytes via BCryptGenRandom.
bool RandomBytes(void* out, size_t len);
std::string RandomHex(size_t numBytes);

// Computes 32-byte HMAC-SHA256(key, data).
bool HmacSha256(const uint8_t* key, size_t keyLen, const uint8_t* data, size_t dataLen,
                uint8_t outMac[32]);

// Encrypts + authenticates a UDP payload (AES-128-CTR + 16-byte truncated HMAC-SHA256).
// Key is 32 bytes: [0..15] = AES-128 key, [16..31] = HMAC-SHA256 key.
bool SealPacket(const uint8_t sessionKey[32], uint64_t nonce, const uint8_t* headeraad,
                size_t aadLen, const uint8_t* plaintext, size_t plainLen,
                std::vector<uint8_t>& outPacket);

// Verifies HMAC-SHA256 before decrypting AES-128-CTR payload into `outPlaintext`.
bool OpenPacket(const uint8_t sessionKey[32], const uint8_t* packet, size_t packetLen,
                size_t aadLen, uint64_t& outNonce, std::vector<uint8_t>& outPlaintext);

bool HexToBytes(const std::string& hex, uint8_t* out, size_t expectedBytes);

}  // namespace ps4::net
