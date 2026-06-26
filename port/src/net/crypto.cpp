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
