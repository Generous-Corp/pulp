// test_sparkle_verify.cpp — end-to-end coverage of the Sparkle Ed25519
// wiring introduced in macOS plan item 7.3.
//
// Verifies:
//   1. sign_file_ed25519 produces a signature that round-trips through
//      verify_file_ed25519 against the matching public key.
//   2. sign_file_ed25519 accepts both 32-byte seed AND 64-byte secret-
//      key inputs (both forms Sparkle's `generate_keys` emits over time).
//   3. A tampered file (one-byte flip) is rejected on verify.
//   4. The wrong public key rejects an otherwise-valid signature.
//   5. RFC 8032 test vector lines up: a known seed signing a known
//      message produces a deterministic Ed25519 signature, and the
//      Sparkle helper accepts the base64 of that exact 32-byte seed.
//   6. verify_appcast_signatures walks every signed enclosure in a
//      Sparkle-style XML feed and validates each, returning true only
//      when every signed item passes.
//   7. The CLI's previous silent-empty-signature behaviour (#295) is
//      gone: sign_file_ed25519 returns nullopt (not empty string) on
//      every error path.

#include <catch2/catch_test_macros.hpp>
#include <pulp/ship/appcast.hpp>
#include <pulp/runtime/crypto.hpp>
#include <pulp/runtime/base64.hpp>

#include <array>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

using namespace pulp::runtime;
using namespace pulp::ship;

namespace {

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

struct TempFile {
    std::string path;
    explicit TempFile(const std::vector<uint8_t>& bytes,
                      const std::string& name = "pulp-sparkle-test") {
        path = std::string(std::tmpnam(nullptr)) + "-" + name;
        std::ofstream f(path, std::ios::binary);
        f.write(reinterpret_cast<const char*>(bytes.data()),
                static_cast<std::streamsize>(bytes.size()));
    }
    ~TempFile() { std::remove(path.c_str()); }
    TempFile(const TempFile&) = delete;
    TempFile& operator=(const TempFile&) = delete;
};

} // namespace

TEST_CASE("Sparkle sign_file_ed25519 round-trips with verify_file_ed25519",
          "[ship][sparkle][ed25519]") {
    // Fresh keypair, fresh artifact.
    auto kp = ed25519_keypair_generate();
    REQUIRE(kp.has_value());

    std::vector<uint8_t> artifact{'p','l','u','g','i','n','-','b','i','n'};
    TempFile f(artifact);

    std::string sk_b64 = base64_encode(kp->private_key.data(),
                                       kp->private_key.size());
    std::string pk_b64 = base64_encode(kp->public_key.data(),
                                       kp->public_key.size());

    auto sig_b64 = sign_file_ed25519(f.path, sk_b64);
    REQUIRE(sig_b64.has_value());
    REQUIRE_FALSE(sig_b64->empty());

    REQUIRE(verify_file_ed25519(f.path, *sig_b64, pk_b64));
}

TEST_CASE("Sparkle sign_file_ed25519 accepts a 32-byte seed",
          "[ship][sparkle][ed25519]") {
    // Seed-form key — what Sparkle's modern generate_keys emits.
    std::array<uint8_t, 32> seed{};
    for (size_t i = 0; i < seed.size(); ++i) seed[i] = static_cast<uint8_t>(i + 7);

    auto kp = ed25519_keypair_from_seed(seed.data(), seed.size());
    REQUIRE(kp.has_value());

    std::vector<uint8_t> artifact{'h','e','l','l','o',' ','p','u','l','p'};
    TempFile f(artifact);

    std::string seed_b64 = base64_encode(seed.data(), seed.size());
    std::string pk_b64   = base64_encode(kp->public_key.data(),
                                         kp->public_key.size());

    auto sig_b64 = sign_file_ed25519(f.path, seed_b64);
    REQUIRE(sig_b64.has_value());
    REQUIRE(verify_file_ed25519(f.path, *sig_b64, pk_b64));
}

TEST_CASE("Sparkle verify rejects a tampered file",
          "[ship][sparkle][ed25519]") {
    auto kp = ed25519_keypair_generate();
    REQUIRE(kp.has_value());

    std::vector<uint8_t> artifact(64, 0xAB);
    TempFile f(artifact);
    std::string sk_b64 = base64_encode(kp->private_key.data(),
                                       kp->private_key.size());
    std::string pk_b64 = base64_encode(kp->public_key.data(),
                                       kp->public_key.size());

    auto sig_b64 = sign_file_ed25519(f.path, sk_b64);
    REQUIRE(sig_b64.has_value());

    // Tamper the file: flip one byte.
    std::vector<uint8_t> tampered = artifact;
    tampered[10] ^= 0x01;
    TempFile bad(tampered);
    REQUIRE_FALSE(verify_file_ed25519(bad.path, *sig_b64, pk_b64));
}

TEST_CASE("Sparkle verify rejects the wrong public key",
          "[ship][sparkle][ed25519]") {
    auto kp_a = ed25519_keypair_generate();
    auto kp_b = ed25519_keypair_generate();
    REQUIRE(kp_a.has_value());
    REQUIRE(kp_b.has_value());

    std::vector<uint8_t> artifact(32, 0x33);
    TempFile f(artifact);

    auto sig_b64 = sign_file_ed25519(
        f.path,
        base64_encode(kp_a->private_key.data(), kp_a->private_key.size()));
    REQUIRE(sig_b64.has_value());

    REQUIRE(verify_file_ed25519(
        f.path, *sig_b64,
        base64_encode(kp_a->public_key.data(), kp_a->public_key.size())));
    REQUIRE_FALSE(verify_file_ed25519(
        f.path, *sig_b64,
        base64_encode(kp_b->public_key.data(), kp_b->public_key.size())));
}

TEST_CASE("Sparkle sign refuses garbage key + unreadable file",
          "[ship][sparkle][ed25519]") {
    // No silent empty-string output (#295). Every failure -> nullopt.
    REQUIRE_FALSE(sign_file_ed25519("/path/does/not/exist", "ignored").has_value());

    std::vector<uint8_t> artifact{1, 2, 3};
    TempFile f(artifact);

    REQUIRE_FALSE(sign_file_ed25519(f.path, "not-valid-base64!!!").has_value());

    // Right base64, but neither 32 nor 64 bytes -> reject.
    std::string short_key = base64_encode(std::string_view("short", 5));
    REQUIRE_FALSE(sign_file_ed25519(f.path, short_key).has_value());
}

TEST_CASE("Sparkle verify rejects size-mismatched signature or public key",
          "[ship][sparkle][ed25519]") {
    auto kp = ed25519_keypair_generate();
    REQUIRE(kp.has_value());

    std::vector<uint8_t> artifact(16, 0x77);
    TempFile f(artifact);

    std::string pk_b64 = base64_encode(kp->public_key.data(),
                                       kp->public_key.size());

    // 64-byte-but-junk signature against the right pk.
    std::vector<uint8_t> junk_sig(64, 0xFF);
    std::string junk_sig_b64 = base64_encode(junk_sig.data(), junk_sig.size());
    REQUIRE_FALSE(verify_file_ed25519(f.path, junk_sig_b64, pk_b64));

    // Wrong-size sig (32 bytes instead of 64).
    std::vector<uint8_t> tiny_sig(32, 0xFF);
    REQUIRE_FALSE(verify_file_ed25519(
        f.path,
        base64_encode(tiny_sig.data(), tiny_sig.size()),
        pk_b64));

    // Wrong-size pk (16 bytes).
    std::vector<uint8_t> tiny_pk(16, 0xCC);
    std::string sk_b64 = base64_encode(kp->private_key.data(),
                                       kp->private_key.size());
    auto good_sig = sign_file_ed25519(f.path, sk_b64);
    REQUIRE(good_sig.has_value());
    REQUIRE_FALSE(verify_file_ed25519(
        f.path, *good_sig,
        base64_encode(tiny_pk.data(), tiny_pk.size())));
}

TEST_CASE("Sparkle RFC 8032 TEST 1 cross-check via the Sparkle helper",
          "[ship][sparkle][ed25519][rfc8032]") {
    // RFC 8032 §7.1 TEST 1 — empty message.
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

    // Empty-message artifact: a zero-byte file.
    std::vector<uint8_t> empty;
    TempFile f(empty);

    std::string seed_b64 = base64_encode(seed.data(), seed.size());

    auto sig_b64 = sign_file_ed25519(f.path, seed_b64);
    REQUIRE(sig_b64.has_value());
    auto sig_bytes = base64_decode(*sig_b64);
    REQUIRE(sig_bytes.has_value());
    REQUIRE(*sig_bytes == expected_sig);

    std::string pk_b64 = base64_encode(expected_pk.data(), expected_pk.size());
    REQUIRE(verify_file_ed25519(f.path, *sig_b64, pk_b64));
}

TEST_CASE("verify_appcast_signatures walks all signed enclosures",
          "[ship][sparkle][appcast]") {
    auto kp = ed25519_keypair_generate();
    REQUIRE(kp.has_value());
    std::string sk_b64 = base64_encode(kp->private_key.data(),
                                       kp->private_key.size());
    std::string pk_b64 = base64_encode(kp->public_key.data(),
                                       kp->public_key.size());

    // Two artifacts -> two signed enclosures.
    std::vector<uint8_t> artifact_a{'P','l','u','g','i','n','-','A'};
    std::vector<uint8_t> artifact_b{'P','l','u','g','i','n','-','B'};
    TempFile fa(artifact_a, "a");
    TempFile fb(artifact_b, "b");

    auto sig_a = sign_file_ed25519(fa.path, sk_b64);
    auto sig_b = sign_file_ed25519(fb.path, sk_b64);
    REQUIRE(sig_a.has_value());
    REQUIRE(sig_b.has_value());

    Appcast feed;
    feed.title = "TestPluck Updates";
    feed.link  = "https://example.com/appcast.xml";

    AppcastItem item_a;
    item_a.version = "1.0.0";
    item_a.title = "Version 1.0.0";
    item_a.download_url = fa.path;
    item_a.file_size = artifact_a.size();
    item_a.ed_signature = *sig_a;
    feed.items.push_back(item_a);

    AppcastItem item_b;
    item_b.version = "1.0.1";
    item_b.title = "Version 1.0.1";
    item_b.download_url = fb.path;
    item_b.file_size = artifact_b.size();
    item_b.ed_signature = *sig_b;
    feed.items.push_back(item_b);

    REQUIRE(verify_appcast_signatures(feed, pk_b64));

    // Round-trip via XML (parses + re-serializes the same struct shape).
    auto xml = feed.to_xml();
    auto reparsed = Appcast::from_xml(xml);
    REQUIRE(reparsed.has_value());
    REQUIRE(verify_appcast_signatures(*reparsed, pk_b64));

    // Tamper item B's signature -> whole feed fails.
    Appcast tampered = feed;
    tampered.items[1].ed_signature = *sig_a;  // valid sig for the wrong file
    REQUIRE_FALSE(verify_appcast_signatures(tampered, pk_b64));

    // A feed with zero signed items returns false (callers want "we
    // verified at least one signature, and all of them are good").
    Appcast unsigned_feed;
    unsigned_feed.items.push_back({});
    REQUIRE_FALSE(verify_appcast_signatures(unsigned_feed, pk_b64));
}
