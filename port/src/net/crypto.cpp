#define NOMINMAX
#include "crypto.h"

#include <windows.h>
#include <bcrypt.h>

#include <cstdio>
#include <cstring>

namespace ps4::net {
namespace {

bool Aes128BlockEncrypt(const uint8_t key16[16], const uint8_t in16[16], uint8_t out16[16]) {
    BCRYPT_ALG_HANDLE hAlg = nullptr;
    if (BCryptOpenAlgorithmProvider(&hAlg, BCRYPT_AES_ALGORITHM, nullptr, 0) != 0) return false;
    BCryptSetProperty(hAlg, BCRYPT_CHAINING_MODE,
                      reinterpret_cast<PUCHAR>(const_cast<wchar_t*>(BCRYPT_CHAIN_MODE_ECB)),
                      sizeof(BCRYPT_CHAIN_MODE_ECB), 0);
    BCRYPT_KEY_HANDLE hKey = nullptr;
    NTSTATUS st = BCryptGenerateSymmetricKey(hAlg, &hKey, nullptr, 0,
                                             const_cast<PUCHAR>(key16), 16, 0);
    bool ok = false;
    if (st == 0) {
        ULONG cbOut = 0;
        uint8_t block[16];
        std::memcpy(block, in16, 16);
        if (BCryptEncrypt(hKey, block, 16, nullptr, nullptr, 0, out16, 16, &cbOut, 0) == 0 &&
            cbOut == 16) {
            ok = true;
        }
        BCryptDestroyKey(hKey);
    }
    BCryptCloseAlgorithmProvider(hAlg, 0);
    return ok;
}

bool Aes128CtrXor(const uint8_t key16[16], uint64_t nonce, const uint8_t* src, size_t len,
                  uint8_t* dst) {
    BCRYPT_ALG_HANDLE hAlg = nullptr;
    if (BCryptOpenAlgorithmProvider(&hAlg, BCRYPT_AES_ALGORITHM, nullptr, 0) != 0) return false;
    BCryptSetProperty(hAlg, BCRYPT_CHAINING_MODE,
                      reinterpret_cast<PUCHAR>(const_cast<wchar_t*>(BCRYPT_CHAIN_MODE_ECB)),
                      sizeof(BCRYPT_CHAIN_MODE_ECB), 0);
    BCRYPT_KEY_HANDLE hKey = nullptr;
    if (BCryptGenerateSymmetricKey(hAlg, &hKey, nullptr, 0, const_cast<PUCHAR>(key16), 16, 0) != 0) {
        BCryptCloseAlgorithmProvider(hAlg, 0);
        return false;
    }

    bool ok = true;
    uint32_t counter = 0;
    for (size_t off = 0; off < len; off += 16, ++counter) {
        uint8_t ctrBlock[16]{};
        std::memcpy(ctrBlock + 0, &nonce, 8);
        std::memcpy(ctrBlock + 12, &counter, 4);
        uint8_t ks[16]{};
        ULONG cbOut = 0;
        if (BCryptEncrypt(hKey, ctrBlock, 16, nullptr, nullptr, 0, ks, 16, &cbOut, 0) != 0) {
            ok = false;
            break;
        }
        const size_t chunk = (len - off < 16) ? (len - off) : 16;
        for (size_t i = 0; i < chunk; ++i) dst[off + i] = src[off + i] ^ ks[i];
    }

    BCryptDestroyKey(hKey);
    BCryptCloseAlgorithmProvider(hAlg, 0);
    return ok;
}

bool ConstantTimeEq(const uint8_t* a, const uint8_t* b, size_t n) {
    uint8_t diff = 0;
    for (size_t i = 0; i < n; ++i) diff |= (a[i] ^ b[i]);
    return diff == 0;
}

}  // namespace

bool RandomBytes(void* out, size_t len) {
    if (!out || !len) return true;
    return BCryptGenRandom(nullptr, static_cast<PUCHAR>(out), static_cast<ULONG>(len),
                           BCRYPT_USE_SYSTEM_PREFERRED_RNG) == 0;
}

std::string RandomHex(size_t numBytes) {
    std::vector<uint8_t> buf(numBytes);
    RandomBytes(buf.data(), buf.size());
    static const char kHex[] = "0123456789abcdef";
    std::string out;
    out.reserve(numBytes * 2);
    for (uint8_t b : buf) {
        out.push_back(kHex[b >> 4]);
        out.push_back(kHex[b & 0xf]);
    }
    return out;
}

bool HmacSha256(const uint8_t* key, size_t keyLen, const uint8_t* data, size_t dataLen,
                uint8_t outMac[32]) {
    BCRYPT_ALG_HANDLE hAlg = nullptr;
    if (BCryptOpenAlgorithmProvider(&hAlg, BCRYPT_SHA256_ALGORITHM, nullptr,
                                    BCRYPT_ALG_HANDLE_HMAC_FLAG) != 0)
        return false;
    BCRYPT_HASH_HANDLE hHash = nullptr;
    bool ok = false;
    if (BCryptCreateHash(hAlg, &hHash, nullptr, 0, const_cast<PUCHAR>(key),
                         static_cast<ULONG>(keyLen), 0) == 0) {
        if (BCryptHashData(hHash, const_cast<PUCHAR>(data), static_cast<ULONG>(dataLen), 0) == 0 &&
            BCryptFinishHash(hHash, outMac, 32, 0) == 0) {
            ok = true;
        }
        BCryptDestroyHash(hHash);
    }
    BCryptCloseAlgorithmProvider(hAlg, 0);
    return ok;
}

// Wire layout: [aad (aadLen bytes)] [nonce (8 bytes)] [ciphertext (plainLen)] [hmac16 (16 bytes)]
bool SealPacket(const uint8_t sessionKey[32], uint64_t nonce, const uint8_t* headerAad,
                size_t aadLen, const uint8_t* plaintext, size_t plainLen,
                std::vector<uint8_t>& outPacket) {
    (void)Aes128BlockEncrypt;
    outPacket.resize(aadLen + 8 + plainLen + 16);
    if (aadLen && headerAad) std::memcpy(outPacket.data(), headerAad, aadLen);
    std::memcpy(outPacket.data() + aadLen, &nonce, 8);
    if (plainLen > 0) {
        if (!Aes128CtrXor(sessionKey, nonce, plaintext, plainLen,
                          outPacket.data() + aadLen + 8))
            return false;
    }
    uint8_t fullMac[32]{};
    if (!HmacSha256(sessionKey + 16, 16, outPacket.data(), aadLen + 8 + plainLen, fullMac))
        return false;
    std::memcpy(outPacket.data() + aadLen + 8 + plainLen, fullMac, 16);
    return true;
}

bool OpenPacket(const uint8_t sessionKey[32], const uint8_t* packet, size_t packetLen,
                size_t aadLen, uint64_t& outNonce, std::vector<uint8_t>& outPlaintext) {
    if (!packet || packetLen < aadLen + 8 + 16) return false;
    const size_t cipherLen = packetLen - aadLen - 8 - 16;
    uint8_t fullMac[32]{};
    if (!HmacSha256(sessionKey + 16, 16, packet, aadLen + 8 + cipherLen, fullMac)) return false;
    if (!ConstantTimeEq(fullMac, packet + aadLen + 8 + cipherLen, 16)) return false;

    std::memcpy(&outNonce, packet + aadLen, 8);
    outPlaintext.resize(cipherLen);
    if (cipherLen > 0) {
        if (!Aes128CtrXor(sessionKey, outNonce, packet + aadLen + 8, cipherLen,
                          outPlaintext.data()))
            return false;
    }
    return true;
}

bool HexToBytes(const std::string& hex, uint8_t* out, size_t expectedBytes) {
    if (hex.size() != expectedBytes * 2) return false;
    auto nib = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    for (size_t i = 0; i < expectedBytes; ++i) {
        int hi = nib(hex[i * 2]);
        int lo = nib(hex[i * 2 + 1]);
        if (hi < 0 || lo < 0) return false;
        out[i] = static_cast<uint8_t>((hi << 4) | lo);
    }
    return true;
}

}  // namespace ps4::net
