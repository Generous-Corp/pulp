#include <catch2/catch_test_macros.hpp>
#include <pulp/runtime/crypto.hpp>

#include <array>
#include <cstdint>
#include <cstring>
#include <vector>

using namespace pulp::runtime;

namespace {

std::string to_hex(const std::vector<uint8_t>& v) {
    static const char* hex = "0123456789abcdef";
    std::string out;
    out.reserve(v.size() * 2);
    for (uint8_t b : v) { out.push_back(hex[b >> 4]); out.push_back(hex[b & 0x0F]); }
    return out;
}

std::vector<uint8_t> from_hex(std::string_view h) {
    std::vector<uint8_t> out;
    out.reserve(h.size() / 2);
    auto val = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return 10 + c - 'a';
        if (c >= 'A' && c <= 'F') return 10 + c - 'A';
        return 0;
    };
    for (size_t i = 0; i + 1 < h.size(); i += 2) {
        out.push_back(static_cast<uint8_t>((val(h[i]) << 4) | val(h[i + 1])));
    }
    return out;
}

} // namespace

// ────────────────────────────────────────────────────────────────────────
// HMAC-SHA256 — RFC 4231 test vectors (subset)
// ────────────────────────────────────────────────────────────────────────

TEST_CASE("HMAC-SHA256 RFC 4231 test case 1", "[runtime][crypto][hmac][rfc]") {
    auto key = from_hex("0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b");
    auto tag = hmac_sha256(key.data(), key.size(),
                            reinterpret_cast<const uint8_t*>("Hi There"), 8);
    REQUIRE(tag.has_value());
    REQUIRE(to_hex(*tag) ==
            "b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7");
}

TEST_CASE("HMAC-SHA256 RFC 4231 test case 2 ('Jefe')",
          "[runtime][crypto][hmac][rfc]") {
    auto tag = hmac_sha256(
        reinterpret_cast<const uint8_t*>("Jefe"), 4,
        reinterpret_cast<const uint8_t*>("what do ya want for nothing?"), 28);
    REQUIRE(tag.has_value());
    REQUIRE(to_hex(*tag) ==
            "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843");
}

TEST_CASE("HMAC-SHA256 RFC 4231 test case 3 (long data + 0xaa key)",
          "[runtime][crypto][hmac][rfc]") {
    std::array<uint8_t, 20> key; key.fill(0xaa);
    std::array<uint8_t, 50> data; data.fill(0xdd);
    auto tag = hmac_sha256(key.data(), key.size(), data.data(), data.size());
    REQUIRE(tag.has_value());
    REQUIRE(to_hex(*tag) ==
            "773ea91e36800e46854db8ebd09181a72959098b3ef8c122d9635514ced565fe");
}

TEST_CASE("HMAC-SHA256 string_view overload", "[runtime][crypto][hmac]") {
    auto tag = hmac_sha256(std::string_view("Jefe"),
                            std::string_view("what do ya want for nothing?"));
    REQUIRE(tag.has_value());
    REQUIRE(to_hex(*tag) ==
            "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843");
}

// ────────────────────────────────────────────────────────────────────────
// HMAC-SHA1 — RFC 2202 test vectors (subset)
// ────────────────────────────────────────────────────────────────────────

TEST_CASE("HMAC-SHA1 RFC 2202 test case 1", "[runtime][crypto][hmac][rfc]") {
    auto key = from_hex("0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b");
    auto tag = hmac_sha1(key.data(), key.size(),
                          reinterpret_cast<const uint8_t*>("Hi There"), 8);
    REQUIRE(tag.has_value());
    REQUIRE(to_hex(*tag) == "b617318655057264e28bc0b6fb378c8ef146be00");
}

TEST_CASE("HMAC-SHA1 RFC 2202 test case 2 ('Jefe')",
          "[runtime][crypto][hmac][rfc]") {
    auto tag = hmac_sha1(
        reinterpret_cast<const uint8_t*>("Jefe"), 4,
        reinterpret_cast<const uint8_t*>("what do ya want for nothing?"), 28);
    REQUIRE(tag.has_value());
    REQUIRE(to_hex(*tag) == "effcdf6ae5eb2fa2d27416d5f184df9c259a7c79");
}

TEST_CASE("HMAC returns optional so callers can detect mbedTLS failure (Codex #2841 P1)",
          "[runtime][crypto][hmac][regression]") {
    // Smoke: the success path of every supported MAC must yield a value
    // of the expected length. Failure-mode (alloc/build-config) is hard
    // to trigger in a test, but the API change itself is the fix —
    // callers can no longer treat a silently-zero tag as success.
    const uint8_t key[] = {0x00};
    const uint8_t data[] = {0x00};
    auto s256 = hmac_sha256(key, 1, data, 1);
    auto s1   = hmac_sha1(key, 1, data, 1);
    REQUIRE(s256.has_value());
    REQUIRE(s256->size() == 32);
    REQUIRE(s1.has_value());
    REQUIRE(s1->size() == 20);
}

// ────────────────────────────────────────────────────────────────────────
// AES-256-GCM — NIST SP 800-38D test vector
// ────────────────────────────────────────────────────────────────────────

TEST_CASE("AES-256-GCM NIST Test Case 13 (256-bit key, empty plaintext)",
          "[runtime][crypto][gcm][rfc]") {
    auto key = from_hex(
        "0000000000000000000000000000000000000000000000000000000000000000");
    auto iv = from_hex("000000000000000000000000");
    auto out = aes_gcm_encrypt(nullptr, 0, key.data(),
                                iv.data(), iv.size(),
                                nullptr, 0);
    REQUIRE(out.has_value());
    REQUIRE(out->ciphertext.empty());
    REQUIRE(to_hex(out->tag) == "530f8afbc74536b9a963b4f1c4cb738b");
}

TEST_CASE("AES-256-GCM round-trip recovers plaintext + verifies tag",
          "[runtime][crypto][gcm]") {
    std::array<uint8_t, 32> key; for (size_t i = 0; i < 32; ++i) key[i] = static_cast<uint8_t>(i);
    std::array<uint8_t, 12> iv;  for (size_t i = 0; i < 12; ++i) iv[i] = static_cast<uint8_t>(0x10 + i);
    const std::string plain = "Pulp macOS plan Batch D — AEAD via mbedTLS";
    const std::string aad = "header-context";

    auto enc = aes_gcm_encrypt(
        reinterpret_cast<const uint8_t*>(plain.data()), plain.size(),
        key.data(), iv.data(), iv.size(),
        reinterpret_cast<const uint8_t*>(aad.data()), aad.size());
    REQUIRE(enc.has_value());
    REQUIRE(enc->ciphertext.size() == plain.size());
    REQUIRE(enc->tag.size() == 16);

    auto dec = aes_gcm_decrypt(
        enc->ciphertext.data(), enc->ciphertext.size(),
        key.data(), iv.data(), iv.size(),
        reinterpret_cast<const uint8_t*>(aad.data()), aad.size(),
        enc->tag.data());
    REQUIRE(dec.has_value());
    REQUIRE(std::string(dec->begin(), dec->end()) == plain);
}

TEST_CASE("AES-256-GCM rejects tampered ciphertext", "[runtime][crypto][gcm]") {
    std::array<uint8_t, 32> key{};
    std::array<uint8_t, 12> iv{};
    const std::string plain = "secret payload";
    auto enc = aes_gcm_encrypt(
        reinterpret_cast<const uint8_t*>(plain.data()), plain.size(),
        key.data(), iv.data(), iv.size(),
        nullptr, 0);
    REQUIRE(enc.has_value());
    // Flip one byte of ciphertext.
    enc->ciphertext[0] ^= 0x01;
    auto dec = aes_gcm_decrypt(
        enc->ciphertext.data(), enc->ciphertext.size(),
        key.data(), iv.data(), iv.size(),
        nullptr, 0, enc->tag.data());
    REQUIRE_FALSE(dec.has_value()); // tag verify failed
}

TEST_CASE("AES-256-GCM rejects tampered tag", "[runtime][crypto][gcm]") {
    std::array<uint8_t, 32> key{};
    std::array<uint8_t, 12> iv{};
    const std::string plain = "secret payload";
    auto enc = aes_gcm_encrypt(
        reinterpret_cast<const uint8_t*>(plain.data()), plain.size(),
        key.data(), iv.data(), iv.size(),
        nullptr, 0);
    REQUIRE(enc.has_value());
    enc->tag[5] ^= 0xFF;
    auto dec = aes_gcm_decrypt(
        enc->ciphertext.data(), enc->ciphertext.size(),
        key.data(), iv.data(), iv.size(),
        nullptr, 0, enc->tag.data());
    REQUIRE_FALSE(dec.has_value());
}

TEST_CASE("AES-256-GCM rejects mismatched AAD", "[runtime][crypto][gcm]") {
    std::array<uint8_t, 32> key{};
    std::array<uint8_t, 12> iv{};
    const std::string plain = "payload";
    const std::string aad_a = "context-A";
    const std::string aad_b = "context-B";
    auto enc = aes_gcm_encrypt(
        reinterpret_cast<const uint8_t*>(plain.data()), plain.size(),
        key.data(), iv.data(), iv.size(),
        reinterpret_cast<const uint8_t*>(aad_a.data()), aad_a.size());
    REQUIRE(enc.has_value());
    auto dec = aes_gcm_decrypt(
        enc->ciphertext.data(), enc->ciphertext.size(),
        key.data(), iv.data(), iv.size(),
        reinterpret_cast<const uint8_t*>(aad_b.data()), aad_b.size(),
        enc->tag.data());
    REQUIRE_FALSE(dec.has_value());
}

// ────────────────────────────────────────────────────────────────────────
// Constant-time compare
// ────────────────────────────────────────────────────────────────────────

TEST_CASE("constant_time_equal matches std::memcmp behavior on equality",
          "[runtime][crypto][ct-compare]") {
    std::array<uint8_t, 32> a{}; for (auto& b : a) b = 0xA5;
    std::array<uint8_t, 32> b = a;
    REQUIRE(constant_time_equal(a.data(), b.data(), a.size()));

    b[15] ^= 0x01;
    REQUIRE_FALSE(constant_time_equal(a.data(), b.data(), a.size()));
}

TEST_CASE("constant_time_equal handles zero-length", "[runtime][crypto][ct-compare]") {
    REQUIRE(constant_time_equal(nullptr, nullptr, 0));
}
