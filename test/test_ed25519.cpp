// test_ed25519.cpp — RFC 8032 §7.1 test-vector coverage for the Ed25519
// surface added in macOS plan item 7.1b.
//
// Each test vector is verified two ways:
//   (1) ed25519_keypair_from_seed(seed) reproduces the published public key
//   (2) ed25519_sign(seed||pk, msg) matches the published signature byte-for-byte
//   (3) ed25519_verify(pk, sig, msg) returns true; flipping any byte returns false
//
// Test 1 (empty message) and Test 2 (1-byte message) are taken directly
// from RFC 8032 §7.1 to lock the implementation against the spec.

#include <catch2/catch_test_macros.hpp>
#include <pulp/runtime/crypto.hpp>

#include <array>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

using namespace pulp::runtime;

namespace {

// Hex helpers — keep the test self-contained so the suite reads
// alongside its RFC source.
std::vector<uint8_t> hex_to_bytes(std::string_view hex) {
    std::vector<uint8_t> out;
    out.reserve(hex.size() / 2);
    for (size_t i = 0; i + 1 < hex.size(); i += 2) {
        auto nibble = [](char c) -> int {
            if (c >= '0' && c <= '9') return c - '0';
            if (c >= 'a' && c <= 'f') return c - 'a' + 10;
            if (c >= 'A' && c <= 'F') return c - 'A' + 10;
            return -1;
        };
        int hi = nibble(hex[i]);
        int lo = nibble(hex[i + 1]);
        if (hi < 0 || lo < 0) return {};
        out.push_back(static_cast<uint8_t>((hi << 4) | lo));
    }
    return out;
}

} // namespace

TEST_CASE("Ed25519 RFC 8032 §7.1 TEST 1 (empty message)",
          "[crypto][ed25519][rfc8032]") {
    // TEST 1 (RFC 8032 §7.1):
    //   SEED   = 9d61b19deffd5a60ba844af492ec2cc4...44949014c1700b5b9ed4d24115b6e7
    //   PUBLIC = d75a980182b10ab7d54bfed3c964073a...09ea59c4f44d5d692e7ac651b3a51a
    //   MESSAGE = (empty)
    //   SIG    = e5564300c360ac729086e2cc806e828a...b58b2a99284ddff86c0cc1edbabd2e0f
    auto seed = hex_to_bytes(
        "9d61b19deffd5a60ba844af492ec2cc4"
        "4449c5697b326919703bac031cae7f60");
    auto expected_pk = hex_to_bytes(
        "d75a980182b10ab7d54bfed3c964073a"
        "0ee172f3daa62325af021a68f707511a");
    auto expected_sig = hex_to_bytes(
        "e5564300c360ac729086e2cc806e828a"
        "84877f1eb8e5d974d873e06522490155"
        "5fb8821590a33bacc61e39701cf9b46b"
        "d25bf5f0595bbe24655141438e7a100b");

    REQUIRE(seed.size() == 32);
    REQUIRE(expected_pk.size() == 32);
    REQUIRE(expected_sig.size() == 64);

    auto kp = ed25519_keypair_from_seed(seed.data(), seed.size());
    REQUIRE(kp.has_value());
    REQUIRE(kp->public_key == expected_pk);

    auto sig = ed25519_sign(kp->private_key.data(), kp->private_key.size(),
                            nullptr, 0);
    REQUIRE(sig.has_value());
    REQUIRE(*sig == expected_sig);

    REQUIRE(ed25519_verify(kp->public_key.data(), kp->public_key.size(),
                           sig->data(), sig->size(),
                           nullptr, 0));
}

TEST_CASE("Ed25519 RFC 8032 §7.1 TEST 2 (1-byte message)",
          "[crypto][ed25519][rfc8032]") {
    // TEST 2 (RFC 8032 §7.1):
    //   SEED   = 4ccd089b28ff96da9db6c346ec114e0f...e6b51e9eb8f8b3fbcdef8ad6e1faf17b
    //   PUBLIC = 3d4017c3e843895a92b70aa74d1b7ebc...22acc8de493ed9527d12e7e4a4f24cb
    //   MESSAGE = 72
    //   SIG    = 92a009a9f0d4cab8720e820b5f642540...c5bf5f63e1f50068d0c1d2812eee6c34
    auto seed = hex_to_bytes(
        "4ccd089b28ff96da9db6c346ec114e0f"
        "5b8a319f35aba624da8cf6ed4fb8a6fb");
    auto expected_pk = hex_to_bytes(
        "3d4017c3e843895a92b70aa74d1b7ebc"
        "9c982ccf2ec4968cc0cd55f12af4660c");
    auto msg = hex_to_bytes("72");
    auto expected_sig = hex_to_bytes(
        "92a009a9f0d4cab8720e820b5f642540"
        "a2b27b5416503f8fb3762223ebdb69da"
        "085ac1e43e15996e458f3613d0f11d8c"
        "387b2eaeb4302aeeb00d291612bb0c00");

    auto kp = ed25519_keypair_from_seed(seed.data(), seed.size());
    REQUIRE(kp.has_value());
    REQUIRE(kp->public_key == expected_pk);

    auto sig = ed25519_sign(kp->private_key.data(), kp->private_key.size(),
                            msg.data(), msg.size());
    REQUIRE(sig.has_value());
    REQUIRE(*sig == expected_sig);

    REQUIRE(ed25519_verify(kp->public_key.data(), kp->public_key.size(),
                           sig->data(), sig->size(),
                           msg.data(), msg.size()));
}

TEST_CASE("Ed25519 generated keypair signs and round-trips",
          "[crypto][ed25519]") {
    auto kp = ed25519_keypair_generate();
    REQUIRE(kp.has_value());
    REQUIRE(kp->public_key.size() == ed25519_public_key_size);
    REQUIRE(kp->private_key.size() == ed25519_private_key_size);

    // NaCl convention: last 32 bytes of the secret key ARE the public key.
    REQUIRE(std::memcmp(kp->private_key.data() + 32,
                        kp->public_key.data(), 32) == 0);

    std::string msg = "Pulp Ed25519 round-trip";
    auto sig = ed25519_sign(kp->private_key.data(), kp->private_key.size(),
                            reinterpret_cast<const uint8_t*>(msg.data()),
                            msg.size());
    REQUIRE(sig.has_value());
    REQUIRE(sig->size() == ed25519_signature_size);

    REQUIRE(ed25519_verify(kp->public_key.data(), kp->public_key.size(),
                           sig->data(), sig->size(),
                           reinterpret_cast<const uint8_t*>(msg.data()),
                           msg.size()));
}

TEST_CASE("Ed25519 verify rejects tampered signature",
          "[crypto][ed25519]") {
    auto kp = ed25519_keypair_generate();
    REQUIRE(kp.has_value());

    std::string msg = "tamper-detection";
    auto sig = ed25519_sign(kp->private_key.data(), kp->private_key.size(),
                            reinterpret_cast<const uint8_t*>(msg.data()),
                            msg.size());
    REQUIRE(sig.has_value());

    // Flip the first byte.
    auto bad = *sig;
    bad[0] ^= 0x01;
    REQUIRE_FALSE(ed25519_verify(kp->public_key.data(), kp->public_key.size(),
                                 bad.data(), bad.size(),
                                 reinterpret_cast<const uint8_t*>(msg.data()),
                                 msg.size()));

    // Flip a middle byte.
    bad = *sig;
    bad[32] ^= 0x80;
    REQUIRE_FALSE(ed25519_verify(kp->public_key.data(), kp->public_key.size(),
                                 bad.data(), bad.size(),
                                 reinterpret_cast<const uint8_t*>(msg.data()),
                                 msg.size()));

    // Flip the last byte.
    bad = *sig;
    bad[63] ^= 0x40;
    REQUIRE_FALSE(ed25519_verify(kp->public_key.data(), kp->public_key.size(),
                                 bad.data(), bad.size(),
                                 reinterpret_cast<const uint8_t*>(msg.data()),
                                 msg.size()));
}

TEST_CASE("Ed25519 verify rejects message-mismatch",
          "[crypto][ed25519]") {
    auto kp = ed25519_keypair_generate();
    REQUIRE(kp.has_value());

    std::string msg = "the original message";
    auto sig = ed25519_sign(kp->private_key.data(), kp->private_key.size(),
                            reinterpret_cast<const uint8_t*>(msg.data()),
                            msg.size());
    REQUIRE(sig.has_value());

    std::string tampered = "the original messagE";  // single-char flip
    REQUIRE_FALSE(ed25519_verify(kp->public_key.data(), kp->public_key.size(),
                                 sig->data(), sig->size(),
                                 reinterpret_cast<const uint8_t*>(tampered.data()),
                                 tampered.size()));
}

TEST_CASE("Ed25519 verify rejects wrong public key",
          "[crypto][ed25519]") {
    auto kp_a = ed25519_keypair_generate();
    auto kp_b = ed25519_keypair_generate();
    REQUIRE(kp_a.has_value());
    REQUIRE(kp_b.has_value());
    REQUIRE(kp_a->public_key != kp_b->public_key);

    std::string msg = "signed by A";
    auto sig = ed25519_sign(kp_a->private_key.data(), kp_a->private_key.size(),
                            reinterpret_cast<const uint8_t*>(msg.data()),
                            msg.size());
    REQUIRE(sig.has_value());

    // Authentic with A's pk, rejected against B's.
    REQUIRE(ed25519_verify(kp_a->public_key.data(), kp_a->public_key.size(),
                           sig->data(), sig->size(),
                           reinterpret_cast<const uint8_t*>(msg.data()),
                           msg.size()));
    REQUIRE_FALSE(ed25519_verify(kp_b->public_key.data(), kp_b->public_key.size(),
                                 sig->data(), sig->size(),
                                 reinterpret_cast<const uint8_t*>(msg.data()),
                                 msg.size()));
}

TEST_CASE("Ed25519 input-size validation",
          "[crypto][ed25519]") {
    auto kp = ed25519_keypair_generate();
    REQUIRE(kp.has_value());

    std::vector<uint8_t> short_seed(16, 0xAA);
    REQUIRE_FALSE(ed25519_keypair_from_seed(short_seed.data(), short_seed.size())
                  .has_value());

    std::vector<uint8_t> short_sk(32, 0xBB);
    REQUIRE_FALSE(ed25519_sign(short_sk.data(), short_sk.size(),
                               nullptr, 0).has_value());

    std::vector<uint8_t> short_pk(16, 0xCC);
    std::vector<uint8_t> dummy_sig(64, 0xDD);
    REQUIRE_FALSE(ed25519_verify(short_pk.data(), short_pk.size(),
                                 dummy_sig.data(), dummy_sig.size(),
                                 nullptr, 0));

    std::vector<uint8_t> short_sig(32, 0xEE);
    REQUIRE_FALSE(ed25519_verify(kp->public_key.data(), kp->public_key.size(),
                                 short_sig.data(), short_sig.size(),
                                 nullptr, 0));
}

TEST_CASE("Ed25519 keypair_from_seed is deterministic across calls",
          "[crypto][ed25519]") {
    std::array<uint8_t, 32> seed{};
    for (size_t i = 0; i < seed.size(); ++i)
        seed[i] = static_cast<uint8_t>(i * 7);

    auto kp1 = ed25519_keypair_from_seed(seed.data(), seed.size());
    auto kp2 = ed25519_keypair_from_seed(seed.data(), seed.size());
    REQUIRE(kp1.has_value());
    REQUIRE(kp2.has_value());
    REQUIRE(kp1->public_key == kp2->public_key);
    REQUIRE(kp1->private_key == kp2->private_key);
}
