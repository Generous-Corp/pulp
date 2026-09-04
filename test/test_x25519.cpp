// test_x25519.cpp — RFC 7748 test-vector coverage for the X25519 runtime
// crypto surface.
//
// Three layers, because each catches something the others cannot:
//   (1) §5.2 raw scalar-multiplication vectors, which pin the arithmetic
//       against the spec independently of any key-exchange framing
//   (2) §6.1 Diffie-Hellman, which additionally proves the two directions
//       agree — the property the whole primitive exists for
//   (3) small-order peer points, which must be refused rather than
//       returning a "shared" secret the peer already knew

#include <catch2/catch_test_macros.hpp>
#include <pulp/runtime/crypto.hpp>

#include <array>
#include <cstdint>
#include <string>
#include <string_view>
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

std::string bytes_to_hex(const std::vector<uint8_t>& bytes) {
    static constexpr char kDigits[] = "0123456789abcdef";
    std::string out;
    out.reserve(bytes.size() * 2);
    for (uint8_t b : bytes) {
        out.push_back(kDigits[b >> 4]);
        out.push_back(kDigits[b & 0x0f]);
    }
    return out;
}

// RFC 7748 §6.1.
constexpr std::string_view kAlicePrivate =
    "77076d0a7318a57d3c16c17251b26645df4c2f87ebc0992ab177fba51db92c2a";
constexpr std::string_view kAlicePublic =
    "8520f0098930a754748b7ddcb43ef75a0dbf3a0d26381af4eba4a98eaa9b4e6a";
constexpr std::string_view kBobPrivate =
    "5dab087e624a8a4b79e17f8b83800ee66f3bb1292618b6fd1c2f8b27ff88e0eb";
constexpr std::string_view kBobPublic =
    "de9edb7d7b7dc1b4d35b61c2ece435373f8343c85b78674dadfc7e146f882b4f";
constexpr std::string_view kSharedSecret =
    "4a5d9d5ba4ce2de1728e3bf480350f25e07e21c947d19e3376f09b3c1e161742";

std::optional<std::vector<uint8_t>> scalarmult(std::string_view scalar_hex,
                                               std::string_view u_hex) {
    auto scalar = hex_to_bytes(scalar_hex);
    auto u = hex_to_bytes(u_hex);
    return x25519_shared_secret(scalar.data(), scalar.size(),
                                u.data(), u.size());
}

} // namespace

TEST_CASE("X25519 matches RFC 7748 §5.2 scalar-multiplication vector 1",
          "[crypto][x25519]") {
    auto out = scalarmult(
        "a546e36bf0527c9d3b16154b82465edd62144c0ac1fc5a18506a2244ba449ac4",
        "e6db6867583030db3594c1a424b15f7c726624ec26b3353b10a903a6d0ab1c4c");
    REQUIRE(out.has_value());
    REQUIRE(bytes_to_hex(*out) ==
            "c3da55379de9c6908e94ea4df28d084f32eccf03491c71f754b4075577a28552");
}

TEST_CASE("X25519 matches RFC 7748 §5.2 scalar-multiplication vector 2",
          "[crypto][x25519]") {
    // This vector's u-coordinate has its high bit set. RFC 7748 requires
    // that bit to be masked off before use, so a run that reproduces the
    // published output also proves the masking happens.
    auto out = scalarmult(
        "4b66e9d4d1b4673c5ad22691957d6af5c11b6421e0ea01d42ca4169e7918ba0d",
        "e5210f12786811d3f4b7959d0538ae2c31dbe7106fc03c3efc4cd549c715a493");
    REQUIRE(out.has_value());
    REQUIRE(bytes_to_hex(*out) ==
            "95cbde9476e8907d7aade45cb4b873f88b595a68799fa152e6f8f7647aac7957");
}

TEST_CASE("X25519 derives the RFC 7748 §6.1 public keys from their scalars",
          "[crypto][x25519]") {
    auto alice_private = hex_to_bytes(kAlicePrivate);
    auto alice = x25519_keypair_from_private_key(alice_private.data(),
                                                 alice_private.size());
    REQUIRE(alice.has_value());
    REQUIRE(bytes_to_hex(alice->public_key) == kAlicePublic);
    // The private key is carried through unchanged — callers store it and
    // reconstitute the pair later, so a silently clamped copy would make a
    // round-trip through disk produce a different key than the one held.
    REQUIRE(bytes_to_hex(alice->private_key) == kAlicePrivate);

    auto bob_private = hex_to_bytes(kBobPrivate);
    auto bob = x25519_keypair_from_private_key(bob_private.data(),
                                               bob_private.size());
    REQUIRE(bob.has_value());
    REQUIRE(bytes_to_hex(bob->public_key) == kBobPublic);
}

TEST_CASE("X25519 agrees on the RFC 7748 §6.1 shared secret in both directions",
          "[crypto][x25519]") {
    auto alice_private = hex_to_bytes(kAlicePrivate);
    auto alice_public = hex_to_bytes(kAlicePublic);
    auto bob_private = hex_to_bytes(kBobPrivate);
    auto bob_public = hex_to_bytes(kBobPublic);

    auto from_alice = x25519_shared_secret(
        alice_private.data(), alice_private.size(),
        bob_public.data(), bob_public.size());
    auto from_bob = x25519_shared_secret(
        bob_private.data(), bob_private.size(),
        alice_public.data(), alice_public.size());

    REQUIRE(from_alice.has_value());
    REQUIRE(from_bob.has_value());
    REQUIRE(bytes_to_hex(*from_alice) == kSharedSecret);
    // Asserted separately rather than only against each other: two sides
    // that agree on the wrong value would pass an equality-only check.
    REQUIRE(bytes_to_hex(*from_bob) == kSharedSecret);
}

TEST_CASE("X25519 refuses small-order peer public keys", "[crypto][x25519]") {
    auto alice_private = hex_to_bytes(kAlicePrivate);

    // The control. Without it, a build in which every agreement returns
    // nullopt would pass this test while providing no key exchange at all,
    // and the refusals below would look like a working defence.
    auto bob_public = hex_to_bytes(kBobPublic);
    REQUIRE(x25519_shared_secret(alice_private.data(), alice_private.size(),
                                 bob_public.data(), bob_public.size())
                .has_value());

    // Each of these drives the result to all-zero regardless of our private
    // key, so the peer would know the "shared" secret in advance.
    const std::string_view small_order[] = {
        // Order 1 — the identity.
        "0000000000000000000000000000000000000000000000000000000000000000",
        // Order 4.
        "0100000000000000000000000000000000000000000000000000000000000000",
        // Order 8 (RFC 7748 §6.1 contributory-behaviour note).
        "e0eb7a7c3b41b8ae1656e3faf19fc46ada098deb9c32b1fd866205165f49b800",
        "5f9c95bca3508c24b1d0b1559c83ef5b04445cc4581c8e86d8224eddd09f11d7",
    };
    for (auto hex : small_order) {
        auto peer = hex_to_bytes(hex);
        INFO("peer public key " << hex);
        REQUIRE_FALSE(x25519_shared_secret(alice_private.data(),
                                           alice_private.size(),
                                           peer.data(), peer.size())
                          .has_value());
    }
}

TEST_CASE("X25519 rejects wrong-sized inputs", "[crypto][x25519]") {
    auto key = hex_to_bytes(kAlicePrivate);
    auto peer = hex_to_bytes(kBobPublic);

    REQUIRE_FALSE(x25519_keypair_from_private_key(nullptr, 32).has_value());
    REQUIRE_FALSE(x25519_keypair_from_private_key(key.data(), 31).has_value());
    REQUIRE_FALSE(x25519_keypair_from_private_key(key.data(), 33).has_value());

    REQUIRE_FALSE(x25519_shared_secret(nullptr, 32,
                                       peer.data(), peer.size()).has_value());
    REQUIRE_FALSE(x25519_shared_secret(key.data(), 31,
                                       peer.data(), peer.size()).has_value());
    REQUIRE_FALSE(x25519_shared_secret(key.data(), key.size(),
                                       nullptr, 32).has_value());
    REQUIRE_FALSE(x25519_shared_secret(key.data(), key.size(),
                                       peer.data(), 31).has_value());
}

TEST_CASE("X25519 generated keypairs complete an exchange", "[crypto][x25519]") {
    auto alice = x25519_keypair_generate();
    auto bob = x25519_keypair_generate();
    REQUIRE(alice.has_value());
    REQUIRE(bob.has_value());
    REQUIRE(alice->public_key.size() == x25519_public_key_size);
    REQUIRE(alice->private_key.size() == x25519_private_key_size);
    REQUIRE(alice->private_key != bob->private_key);

    auto from_alice = x25519_shared_secret(
        alice->private_key.data(), alice->private_key.size(),
        bob->public_key.data(), bob->public_key.size());
    auto from_bob = x25519_shared_secret(
        bob->private_key.data(), bob->private_key.size(),
        alice->public_key.data(), alice->public_key.size());

    REQUIRE(from_alice.has_value());
    REQUIRE(from_bob.has_value());
    REQUIRE(*from_alice == *from_bob);

    // A fresh pair must not reproduce the exchange above, or the generator
    // is returning a fixed key and every "unique" secret is the same one.
    auto carol = x25519_keypair_generate();
    REQUIRE(carol.has_value());
    auto from_carol = x25519_shared_secret(
        carol->private_key.data(), carol->private_key.size(),
        bob->public_key.data(), bob->public_key.size());
    REQUIRE(from_carol.has_value());
    REQUIRE(*from_carol != *from_alice);
}

TEST_CASE("X25519 recovers the public key of a generated private key",
          "[crypto][x25519]") {
    auto generated = x25519_keypair_generate();
    REQUIRE(generated.has_value());
    auto recovered = x25519_keypair_from_private_key(
        generated->private_key.data(), generated->private_key.size());
    REQUIRE(recovered.has_value());
    REQUIRE(recovered->public_key == generated->public_key);
}
