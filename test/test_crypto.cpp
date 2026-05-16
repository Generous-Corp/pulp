#include <catch2/catch_test_macros.hpp>
#include <pulp/runtime/crypto.hpp>
#include <algorithm>
#include <cctype>
#include <cstring>
#include <vector>

using namespace pulp::runtime;

// ── SHA-256 ─────────────────────────────────────────────────────────────

TEST_CASE("SHA-256 empty string", "[crypto][sha256]") {
    auto hex = sha256_hex("");
    // Known: SHA-256 of empty string
    REQUIRE(hex == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
}

TEST_CASE("SHA-256 known value", "[crypto][sha256]") {
    auto hex = sha256_hex("hello");
    REQUIRE(hex == "2cf24dba5fb0a30e26e83b2ac5b9e29e1b161e5c1fa7425e73043362938b9824");
}

TEST_CASE("SHA-256 binary data", "[crypto][sha256]") {
    auto digest = sha256("test data");
    REQUIRE(digest.size() == 32);
}

TEST_CASE("SHA-256 pointer overload preserves embedded NUL bytes",
          "[crypto][sha256][coverage][issue-641]") {
    const std::vector<uint8_t> data = {'a', 'b', 'c', 0x00, 'd', 'e', 'f'};

    auto digest = sha256(data.data(), data.size());
    auto hex = sha256_hex(data.data(), data.size());

    REQUIRE(digest.size() == 32);
    REQUIRE(hex == "516a5e926ce20c5f4d80f00e1a01abdf14986def6588d6abeed9fce090bc660c");
    REQUIRE(std::all_of(hex.begin(), hex.end(), [](unsigned char c) {
        return std::isdigit(c) || (c >= 'a' && c <= 'f');
    }));
}

// ── SHA-1 ───────────────────────────────────────────────────────────────

TEST_CASE("SHA-1 known value", "[crypto][sha1]") {
    auto digest = sha1("hello");
    const std::vector<uint8_t> expected = {
        0xaa, 0xf4, 0xc6, 0x1d, 0xdc, 0xc5, 0xe8, 0xa2, 0xda, 0xbe,
        0xde, 0x0f, 0x3b, 0x48, 0x2c, 0xd9, 0xae, 0xa9, 0x43, 0x4d,
    };

    REQUIRE(digest == expected);
}

TEST_CASE("SHA-1 pointer overload preserves embedded NUL bytes",
          "[crypto][sha1][coverage][issue-641]") {
    const std::vector<uint8_t> data = {'w', 's', 0x00, 'k', 'e', 'y'};
    const std::vector<uint8_t> expected = {
        0xb1, 0x24, 0x2e, 0x4b, 0x0c, 0x53, 0x4d, 0x49, 0x0f, 0x5e,
        0xf7, 0xee, 0x67, 0xb8, 0x80, 0xf0, 0xa2, 0xb8, 0x8a, 0x6b,
    };

    REQUIRE(sha1(data.data(), data.size()) == expected);
}

// ── MD5 ─────────────────────────────────────────────────────────────────

TEST_CASE("MD5 empty string", "[crypto][md5]") {
    auto hex = md5_hex("");
    REQUIRE(hex == "d41d8cd98f00b204e9800998ecf8427e");
}

TEST_CASE("MD5 known value", "[crypto][md5]") {
    auto hex = md5_hex("hello");
    REQUIRE(hex == "5d41402abc4b2a76b9719d911017c592");
}

TEST_CASE("MD5 binary data returns raw digest bytes",
          "[crypto][md5][coverage][issue-641]") {
    const std::vector<uint8_t> data = {0x00, 0xff, 0x10, 0x20, 0x00};
    const std::vector<uint8_t> expected = {
        0x96, 0x26, 0x6b, 0xae, 0xb1, 0xeb, 0x57, 0x35,
        0xd5, 0x51, 0xca, 0xc2, 0xd9, 0xa8, 0x27, 0x75,
    };

    REQUIRE(md5(data.data(), data.size()) == expected);
}

// ── AES-256-CBC ─────────────────────────────────────────────────────────

TEST_CASE("AES encrypt/decrypt round-trip", "[crypto][aes]") {
    uint8_t key[32] = {};
    uint8_t iv[16] = {};
    std::memset(key, 0x42, 32);
    std::memset(iv, 0x13, 16);

    std::string plaintext = "Hello, Pulp crypto!";
    auto encrypted = aes_encrypt(
        reinterpret_cast<const uint8_t*>(plaintext.data()),
        plaintext.size(), key, iv);

    REQUIRE(encrypted.has_value());
    REQUIRE(encrypted->size() > plaintext.size());  // Padded
    REQUIRE(encrypted->size() % 16 == 0);

    auto decrypted = aes_decrypt(encrypted->data(), encrypted->size(), key, iv);
    REQUIRE(decrypted.has_value());

    std::string result(decrypted->begin(), decrypted->end());
    REQUIRE(result == plaintext);
}

TEST_CASE("AES empty plaintext", "[crypto][aes]") {
    uint8_t key[32] = {};
    uint8_t iv[16] = {};
    std::memset(key, 0xAA, 32);
    std::memset(iv, 0xBB, 16);

    auto encrypted = aes_encrypt(nullptr, 0, key, iv);
    REQUIRE(encrypted.has_value());
    REQUIRE(encrypted->size() == 16);  // One block of padding

    auto decrypted = aes_decrypt(encrypted->data(), encrypted->size(), key, iv);
    REQUIRE(decrypted.has_value());
    REQUIRE(decrypted->empty());
}

TEST_CASE("AES exact block plaintext adds and removes full padding block",
          "[crypto][aes][coverage][issue-641]") {
    uint8_t key[32] = {};
    uint8_t iv[16] = {};
    std::memset(key, 0x21, 32);
    std::memset(iv, 0x43, 16);

    std::string plaintext = "sixteen byte txt";
    REQUIRE(plaintext.size() == 16);

    auto encrypted = aes_encrypt(
        reinterpret_cast<const uint8_t*>(plaintext.data()),
        plaintext.size(), key, iv);
    REQUIRE(encrypted.has_value());
    REQUIRE(encrypted->size() == 32);

    auto decrypted = aes_decrypt(encrypted->data(), encrypted->size(), key, iv);
    REQUIRE(decrypted.has_value());
    REQUIRE(std::string(decrypted->begin(), decrypted->end()) == plaintext);
}

TEST_CASE("AES decrypt invalid data", "[crypto][aes]") {
    uint8_t key[32] = {};
    uint8_t iv[16] = {};
    uint8_t garbage[16] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};

    // Decrypting garbage may succeed (AES doesn't validate content)
    // but padding check should catch invalid data
    auto result = aes_decrypt(garbage, 16, key, iv);
    // May or may not succeed — depends on whether last byte happens to be valid padding
}

TEST_CASE("AES decrypt rejects non-block-aligned ciphertext", "[crypto][aes]") {
    uint8_t key[32] = {};
    uint8_t iv[16] = {};
    uint8_t ciphertext[15] = {};

    auto result = aes_decrypt(ciphertext, sizeof(ciphertext), key, iv);
    REQUIRE_FALSE(result.has_value());
}

TEST_CASE("AES decrypt rejects invalid PKCS7 padding bytes",
          "[crypto][aes][coverage][issue-641]") {
    uint8_t key[32] = {};
    uint8_t iv[16] = {};
    std::memset(key, 0x5c, 32);
    std::memset(iv, 0xa7, 16);

    std::string plaintext = "padding check";
    auto encrypted = aes_encrypt(
        reinterpret_cast<const uint8_t*>(plaintext.data()),
        plaintext.size(), key, iv);
    REQUIRE(encrypted.has_value());

    encrypted->back() ^= 0x01;
    auto result = aes_decrypt(encrypted->data(), encrypted->size(), key, iv);
    REQUIRE_FALSE(result.has_value());
}

// ── Machine ID ──────────────────────────────────────────────────────────

TEST_CASE("Machine ID is deterministic", "[crypto][machine_id]") {
    auto id1 = machine_id();
    auto id2 = machine_id();

    REQUIRE(id1 == id2);
    REQUIRE(id1.size() == 64);  // SHA-256 hex = 64 chars
}

TEST_CASE("Machine ID is non-empty", "[crypto][machine_id]") {
    auto id = machine_id();
    REQUIRE_FALSE(id.empty());
    // Should not be all zeros
    REQUIRE(id != std::string(64, '0'));
}
