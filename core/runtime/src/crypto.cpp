#include <pulp/runtime/crypto.hpp>
#include <pulp/runtime/ip_address.hpp>
#include <pulp/runtime/system.hpp>

#include <mbedtls/sha256.h>
#include <mbedtls/sha1.h>
#include <mbedtls/md5.h>
#include <mbedtls/aes.h>
#include <mbedtls/gcm.h>
#include <mbedtls/md.h>
#include <mbedtls/entropy.h>
#include <mbedtls/ctr_drbg.h>

extern "C" {
#include "tweetnacl/tweetnacl.h"
}

#include <sstream>
#include <iomanip>
#include <cstring>
#include <algorithm>
#include <array>
#include <mutex>

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
std::optional<std::vector<uint8_t>> hmac_with_md(
    mbedtls_md_type_t md_type, size_t out_size,
    const uint8_t* key, size_t key_size,
    const uint8_t* data, size_t data_size) {
    const mbedtls_md_info_t* md = mbedtls_md_info_from_type(md_type);
    if (md == nullptr) return std::nullopt;
    mbedtls_md_context_t ctx;
    mbedtls_md_init(&ctx);
    if (mbedtls_md_setup(&ctx, md, /*hmac=*/1) != 0) {
        mbedtls_md_free(&ctx);
        return std::nullopt;
    }
    std::vector<uint8_t> tag(out_size, 0);
    // Propagate the return code of every HMAC step so an alloc / bad-
    // input / build-config failure surfaces as nullopt instead of a
    // silently-zero tag (Codex P1 on #2841).
    int rc = mbedtls_md_hmac_starts(&ctx, key, key_size);
    if (rc == 0) rc = mbedtls_md_hmac_update(&ctx, data, data_size);
    if (rc == 0) rc = mbedtls_md_hmac_finish(&ctx, tag.data());
    mbedtls_md_free(&ctx);
    if (rc != 0) return std::nullopt;
    return tag;
}
} // namespace

std::optional<std::vector<uint8_t>> hmac_sha256(
    const uint8_t* key, size_t key_size,
    const uint8_t* data, size_t data_size) {
    return hmac_with_md(MBEDTLS_MD_SHA256, 32, key, key_size, data, data_size);
}
std::optional<std::vector<uint8_t>> hmac_sha256(std::string_view key,
                                                  std::string_view data) {
    return hmac_sha256(reinterpret_cast<const uint8_t*>(key.data()), key.size(),
                       reinterpret_cast<const uint8_t*>(data.data()), data.size());
}

std::optional<std::vector<uint8_t>> hmac_sha1(
    const uint8_t* key, size_t key_size,
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

// ── Ed25519 (RFC 8032) via vendored TweetNaCl ──────────────────────────
//
// TweetNaCl is public-domain (Bernstein/Janssen/Lange/Schwabe). We vendor
// it under external/tweetnacl/ and expose detached EdDSA signing through
// Pulp's runtime-crypto namespace.
//
// TweetNaCl requires an externally-supplied `randombytes()`; we wire it
// to mbedTLS CTR-DRBG so we share entropy infrastructure with the rest
// of the runtime. The DRBG is constructed lazily and protected by a
// std::mutex (initialisation + reseed are not thread-safe by default).

namespace ed25519_impl {

struct TweetNaclRng {
    mbedtls_entropy_context entropy{};
    mbedtls_ctr_drbg_context drbg{};
    bool ok = false;

    TweetNaclRng() {
        mbedtls_entropy_init(&entropy);
        mbedtls_ctr_drbg_init(&drbg);
        // Personalisation string keeps this DRBG distinct from any other
        // CTR-DRBG instance the runtime might create later.
        static const char pers[] = "pulp::runtime::ed25519";
        int rc = mbedtls_ctr_drbg_seed(&drbg, mbedtls_entropy_func, &entropy,
                                       reinterpret_cast<const uint8_t*>(pers),
                                       sizeof(pers) - 1);
        ok = (rc == 0);
    }
    ~TweetNaclRng() {
        mbedtls_ctr_drbg_free(&drbg);
        mbedtls_entropy_free(&entropy);
    }
};

inline std::mutex& tweetnacl_rng_mutex() {
    static std::mutex m;
    return m;
}

inline TweetNaclRng& tweetnacl_rng() {
    static TweetNaclRng r;
    return r;
}

// Thread-local override: when `g_randombytes_inject_active` is true,
// the next call to ::randombytes() returns the 32 injected bytes verbatim
// instead of drawing from CTR-DRBG. Used to make crypto_sign_keypair
// derive a deterministic keypair from a seed (for RFC 8032 vectors and
// reproducible-from-backup workflows).
inline thread_local bool g_randombytes_inject_active = false;
inline thread_local std::array<uint8_t, 32> g_randombytes_inject_seed{};

} // namespace ed25519_impl
} // namespace pulp::runtime

// TweetNaCl's published API calls back into ::randombytes(). Define it at
// file scope (outside any namespace) so the vendored translation unit
// links cleanly. Returns silently on failure — TweetNaCl has no error
// channel for the callback, so on a hard RNG failure the caller will hit
// verify-mismatch downstream.
extern "C" void randombytes(unsigned char* buf, unsigned long long n) {
    using namespace pulp::runtime::ed25519_impl;
    // Honour the deterministic-seed override used by ed25519_keypair_from_seed.
    // TweetNaCl asks for exactly 32 bytes here (the seed half of crypto_sign sk).
    if (g_randombytes_inject_active && n == 32) {
        std::memcpy(buf, g_randombytes_inject_seed.data(), 32);
        g_randombytes_inject_active = false;  // single-shot
        return;
    }

    std::lock_guard<std::mutex> lock(tweetnacl_rng_mutex());
    auto& r = tweetnacl_rng();
    if (!r.ok) {
        std::fill(buf, buf + n, uint8_t{0});
        return;
    }
    constexpr size_t kChunk = 1024;  // CTR-DRBG max request size in mbedTLS
    while (n > 0) {
        size_t take = (n > kChunk) ? kChunk : static_cast<size_t>(n);
        if (mbedtls_ctr_drbg_random(&r.drbg, buf, take) != 0) {
            std::fill(buf, buf + n, uint8_t{0});
            return;
        }
        buf += take;
        n   -= take;
    }
}

namespace pulp::runtime {

std::optional<Ed25519KeyPair> ed25519_keypair_generate() {
    {
        std::lock_guard<std::mutex> lock(ed25519_impl::tweetnacl_rng_mutex());
        if (!ed25519_impl::tweetnacl_rng().ok) return std::nullopt;
    }
    Ed25519KeyPair kp;
    kp.public_key.resize(ed25519_public_key_size);
    kp.private_key.resize(ed25519_private_key_size);
    int rc = crypto_sign_keypair(kp.public_key.data(), kp.private_key.data());
    if (rc != 0) return std::nullopt;
    return kp;
}

std::optional<Ed25519KeyPair> ed25519_keypair_from_seed(
    const uint8_t* seed, size_t seed_size) {
    if (seed == nullptr || seed_size != ed25519_seed_size) return std::nullopt;

    // TweetNaCl's crypto_sign_keypair derives the secret key as
    //   sk[0..31] = randombytes(32)         <- the "seed"
    //   d         = SHA-512(sk[0..31])
    //   clamp(d); scalarbase(p, d); pack(pk, p)
    //   sk[32..63] = pk
    // To reproduce a specific seed without reaching into TweetNaCl
    // internals (scalarbase / pack are static), we inject the seed via
    // the thread-local randombytes override and then call the public
    // crypto_sign_keypair API.
    ed25519_impl::g_randombytes_inject_active = true;
    std::memcpy(ed25519_impl::g_randombytes_inject_seed.data(), seed, 32);

    Ed25519KeyPair kp;
    kp.public_key.resize(ed25519_public_key_size);
    kp.private_key.resize(ed25519_private_key_size);
    int rc = crypto_sign_keypair(kp.public_key.data(), kp.private_key.data());

    // Defensive — override is single-shot but make sure it is cleared
    // even if crypto_sign_keypair ever takes a different path.
    ed25519_impl::g_randombytes_inject_active = false;
    std::fill(ed25519_impl::g_randombytes_inject_seed.begin(),
              ed25519_impl::g_randombytes_inject_seed.end(), uint8_t{0});

    if (rc != 0) return std::nullopt;
    return kp;
}

std::optional<std::vector<uint8_t>> ed25519_sign(
    const uint8_t* private_key, size_t private_key_size,
    const uint8_t* message, size_t message_size) {
    if (private_key == nullptr || private_key_size != ed25519_private_key_size)
        return std::nullopt;
    if (message == nullptr && message_size != 0) return std::nullopt;

    // crypto_sign writes signature || message into sm; we strip the message
    // suffix to produce a detached 64-byte signature.
    std::vector<uint8_t> sm(message_size + crypto_sign_BYTES);
    unsigned long long sm_len = 0;
    int rc = crypto_sign(sm.data(), &sm_len, message, message_size, private_key);
    if (rc != 0 || sm_len < crypto_sign_BYTES) return std::nullopt;

    std::vector<uint8_t> sig(sm.begin(), sm.begin() + crypto_sign_BYTES);
    return sig;
}

bool ed25519_verify(
    const uint8_t* public_key, size_t public_key_size,
    const uint8_t* signature, size_t signature_size,
    const uint8_t* message, size_t message_size) {
    if (public_key == nullptr || public_key_size != ed25519_public_key_size)
        return false;
    if (signature == nullptr || signature_size != ed25519_signature_size)
        return false;
    if (message == nullptr && message_size != 0) return false;

    // Reconstruct sm = signature || message, then call crypto_sign_open.
    std::vector<uint8_t> sm(signature_size + message_size);
    std::memcpy(sm.data(), signature, signature_size);
    if (message_size > 0)
        std::memcpy(sm.data() + signature_size, message, message_size);

    std::vector<uint8_t> recovered(sm.size());
    unsigned long long m_len = 0;
    int rc = crypto_sign_open(recovered.data(), &m_len,
                              sm.data(), sm.size(), public_key);
    return rc == 0;
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
