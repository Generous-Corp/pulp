#include <catch2/catch_test_macros.hpp>
#include <pulp/runtime/crypto.hpp>
#include <cstring>

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

// ── MD5 ─────────────────────────────────────────────────────────────────

TEST_CASE("MD5 empty string", "[crypto][md5]") {
    auto hex = md5_hex("");
    REQUIRE(hex == "d41d8cd98f00b204e9800998ecf8427e");
}

TEST_CASE("MD5 known value", "[crypto][md5]") {
    auto hex = md5_hex("hello");
    REQUIRE(hex == "5d41402abc4b2a76b9719d911017c592");
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

TEST_CASE("AES decrypt invalid data", "[crypto][aes]") {
    uint8_t key[32] = {};
    uint8_t iv[16] = {};
    uint8_t garbage[16] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};

    // Decrypting garbage may succeed (AES doesn't validate content)
    // but padding check should catch invalid data
    auto result = aes_decrypt(garbage, 16, key, iv);
    // May or may not succeed — depends on whether last byte happens to be valid padding
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
