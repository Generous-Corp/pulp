#include <pulp/runtime/crypto.hpp>
#include <pulp/runtime/ip_address.hpp>
#include <pulp/runtime/system.hpp>

#include <mbedtls/sha256.h>
#include <mbedtls/sha1.h>
#include <mbedtls/md5.h>
#include <mbedtls/aes.h>

#include <sstream>
#include <iomanip>
#include <cstring>
#include <algorithm>

namespace pulp::runtime {

// ── Helpers ─────────────────────────────────────────────────────────────

static std::string bytes_to_hex(const uint8_t* data, size_t size) {
    std::ostringstream ss;
    for (size_t i = 0; i < size; ++i)
        ss << std::hex << std::setfill('0') << std::setw(2) << static_cast<int>(data[i]);
    return ss.str();
}

// ── SHA-256 ─────────────────────────────────────────────────────────────

std::vector<uint8_t> sha256(const uint8_t* data, size_t size) {
    std::vector<uint8_t> digest(32);
    mbedtls_sha256(data, size, digest.data(), 0);
    return digest;
}

std::vector<uint8_t> sha256(std::string_view data) {
    return sha256(reinterpret_cast<const uint8_t*>(data.data()), data.size());
}

std::string sha256_hex(const uint8_t* data, size_t size) {
    auto digest = sha256(data, size);
    return bytes_to_hex(digest.data(), digest.size());
}

std::string sha256_hex(std::string_view data) {
    return sha256_hex(reinterpret_cast<const uint8_t*>(data.data()), data.size());
}

// ── SHA-1 (legacy protocols only) ───────────────────────────────────────

std::vector<uint8_t> sha1(const uint8_t* data, size_t size) {
    std::vector<uint8_t> digest(20);
    mbedtls_sha1(data, size, digest.data());
    return digest;
}

std::vector<uint8_t> sha1(std::string_view data) {
    return sha1(reinterpret_cast<const uint8_t*>(data.data()), data.size());
}

// ── MD5 ─────────────────────────────────────────────────────────────────

std::vector<uint8_t> md5(const uint8_t* data, size_t size) {
    std::vector<uint8_t> digest(16);
    mbedtls_md5(data, size, digest.data());
    return digest;
}

std::vector<uint8_t> md5(std::string_view data) {
    return md5(reinterpret_cast<const uint8_t*>(data.data()), data.size());
}

std::string md5_hex(std::string_view data) {
    auto digest = md5(data);
    return bytes_to_hex(digest.data(), digest.size());
}

// ── AES-256-CBC ─────────────────────────────────────────────────────────

std::optional<std::vector<uint8_t>> aes_encrypt(
    const uint8_t* plaintext, size_t size,
    const uint8_t* key_32, const uint8_t* iv_16)
{
    // PKCS7 padding
    size_t padded_size = ((size / 16) + 1) * 16;
    std::vector<uint8_t> padded(padded_size);
    std::memcpy(padded.data(), plaintext, size);
    uint8_t pad_val = static_cast<uint8_t>(padded_size - size);
    std::fill(padded.begin() + static_cast<ptrdiff_t>(size), padded.end(), pad_val);

    mbedtls_aes_context ctx;
    mbedtls_aes_init(&ctx);
    if (mbedtls_aes_setkey_enc(&ctx, key_32, 256) != 0) {
        mbedtls_aes_free(&ctx);
        return std::nullopt;
    }

    std::vector<uint8_t> result(padded_size);
    uint8_t iv_copy[16];
    std::memcpy(iv_copy, iv_16, 16);

    int ret = mbedtls_aes_crypt_cbc(&ctx, MBEDTLS_AES_ENCRYPT,
                                     padded_size, iv_copy,
                                     padded.data(), result.data());
    mbedtls_aes_free(&ctx);

    if (ret != 0) return std::nullopt;
    return result;
}

std::optional<std::vector<uint8_t>> aes_decrypt(
    const uint8_t* ciphertext, size_t size,
    const uint8_t* key_32, const uint8_t* iv_16)
{
    if (size == 0 || size % 16 != 0) return std::nullopt;

    mbedtls_aes_context ctx;
    mbedtls_aes_init(&ctx);
    if (mbedtls_aes_setkey_dec(&ctx, key_32, 256) != 0) {
        mbedtls_aes_free(&ctx);
        return std::nullopt;
    }

    std::vector<uint8_t> result(size);
    uint8_t iv_copy[16];
    std::memcpy(iv_copy, iv_16, 16);

    int ret = mbedtls_aes_crypt_cbc(&ctx, MBEDTLS_AES_DECRYPT,
                                     size, iv_copy,
                                     ciphertext, result.data());
    mbedtls_aes_free(&ctx);

    if (ret != 0) return std::nullopt;

    // Remove PKCS7 padding
    uint8_t pad_val = result.back();
    if (pad_val == 0 || pad_val > 16) return std::nullopt;
    for (size_t i = 0; i < pad_val; ++i) {
        if (result[result.size() - 1 - i] != pad_val)
            return std::nullopt;
    }
    result.resize(size - pad_val);
    return result;
}

// ── Machine ID ──────────────────────────────────────────────────────────

std::string machine_id() {
    // Combine hostname + CPU model + local IPs for a machine-specific fingerprint
    auto& info = get_system_info();
    std::string seed = hostname() + "|" + info.cpu_model + "|" + info.os_name + "|" + info.arch;

    auto addrs = local_ipv4_addresses();
    for (auto& addr : addrs)
        seed += "|" + addr;

    return sha256_hex(seed);
}

}  // namespace pulp::runtime
