#include <pulp/runtime/crypto.hpp>
#include <pulp/runtime/ip_address.hpp>
#include <pulp/runtime/system.hpp>

#include <mbedtls/sha256.h>
#include <mbedtls/sha1.h>
#include <mbedtls/md5.h>
#include <mbedtls/aes.h>
#include <mbedtls/gcm.h>
#include <mbedtls/md.h>

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

// ── HMAC ────────────────────────────────────────────────────────────────

namespace {
std::vector<uint8_t> hmac_with_md(mbedtls_md_type_t md_type, size_t out_size,
                                    const uint8_t* key, size_t key_size,
                                    const uint8_t* data, size_t data_size) {
    std::vector<uint8_t> tag(out_size, 0);
    const mbedtls_md_info_t* md = mbedtls_md_info_from_type(md_type);
    if (md == nullptr) return tag; // never happens for SHA-1/256 in mbedTLS
    mbedtls_md_context_t ctx;
    mbedtls_md_init(&ctx);
    if (mbedtls_md_setup(&ctx, md, /*hmac=*/1) != 0) {
        mbedtls_md_free(&ctx);
        return tag;
    }
    mbedtls_md_hmac_starts(&ctx, key, key_size);
    mbedtls_md_hmac_update(&ctx, data, data_size);
    mbedtls_md_hmac_finish(&ctx, tag.data());
    mbedtls_md_free(&ctx);
    return tag;
}
} // namespace

std::vector<uint8_t> hmac_sha256(const uint8_t* key, size_t key_size,
                                   const uint8_t* data, size_t data_size) {
    return hmac_with_md(MBEDTLS_MD_SHA256, 32, key, key_size, data, data_size);
}
std::vector<uint8_t> hmac_sha256(std::string_view key, std::string_view data) {
    return hmac_sha256(reinterpret_cast<const uint8_t*>(key.data()), key.size(),
                       reinterpret_cast<const uint8_t*>(data.data()), data.size());
}

std::vector<uint8_t> hmac_sha1(const uint8_t* key, size_t key_size,
                                 const uint8_t* data, size_t data_size) {
    return hmac_with_md(MBEDTLS_MD_SHA1, 20, key, key_size, data, data_size);
}

// ── AES-256-GCM ─────────────────────────────────────────────────────────

std::optional<GcmOutput> aes_gcm_encrypt(
    const uint8_t* plaintext, size_t plaintext_size,
    const uint8_t* key_32,
    const uint8_t* iv, size_t iv_size,
    const uint8_t* aad, size_t aad_size) {
    GcmOutput out;
    out.ciphertext.resize(plaintext_size);
    out.tag.resize(16);

    mbedtls_gcm_context ctx;
    mbedtls_gcm_init(&ctx);
    if (mbedtls_gcm_setkey(&ctx, MBEDTLS_CIPHER_ID_AES, key_32, 256) != 0) {
        mbedtls_gcm_free(&ctx);
        return std::nullopt;
    }
    int rc = mbedtls_gcm_crypt_and_tag(
        &ctx, MBEDTLS_GCM_ENCRYPT, plaintext_size,
        iv, iv_size, aad, aad_size,
        plaintext,
        out.ciphertext.empty() ? nullptr : out.ciphertext.data(),
        out.tag.size(), out.tag.data());
    mbedtls_gcm_free(&ctx);
    if (rc != 0) return std::nullopt;
    return out;
}

std::optional<std::vector<uint8_t>> aes_gcm_decrypt(
    const uint8_t* ciphertext, size_t ciphertext_size,
    const uint8_t* key_32,
    const uint8_t* iv, size_t iv_size,
    const uint8_t* aad, size_t aad_size,
    const uint8_t* tag_16) {
    std::vector<uint8_t> plaintext(ciphertext_size);

    mbedtls_gcm_context ctx;
    mbedtls_gcm_init(&ctx);
    if (mbedtls_gcm_setkey(&ctx, MBEDTLS_CIPHER_ID_AES, key_32, 256) != 0) {
        mbedtls_gcm_free(&ctx);
        return std::nullopt;
    }
    int rc = mbedtls_gcm_auth_decrypt(
        &ctx, ciphertext_size,
        iv, iv_size, aad, aad_size,
        tag_16, 16,
        ciphertext,
        plaintext.empty() ? nullptr : plaintext.data());
    mbedtls_gcm_free(&ctx);
    if (rc != 0) {
        // Tag mismatch (MBEDTLS_ERR_GCM_AUTH_FAILED) or other failure;
        // overwrite the plaintext buffer so partial decryption doesn't
        // escape (mbedTLS already zeroes it on auth failure, but be
        // explicit here in case of build-config differences).
        std::fill(plaintext.begin(), plaintext.end(), uint8_t{0});
        return std::nullopt;
    }
    return plaintext;
}

// ── Constant-time compare ───────────────────────────────────────────────

bool constant_time_equal(const uint8_t* a, const uint8_t* b, size_t size) {
    // Branchless XOR + accumulate.
    uint8_t diff = 0;
    for (size_t i = 0; i < size; ++i)
        diff = static_cast<uint8_t>(diff | (a[i] ^ b[i]));
    return diff == 0;
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
