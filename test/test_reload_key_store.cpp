// Signing-key material + file-backed store. The key blob round-trips, load reuses an
// existing key (never mints a second identity), and generation produces a usable
// signing key that verifies a signature.
#include <catch2/catch_test_macros.hpp>

#include <pulp/format/reload/key_store.hpp>
#include <pulp/format/reload/swap_pack.hpp>

#include "reload_test_support.hpp"

#include <filesystem>
#include <string>

using namespace pulp::format::reload;
namespace fs = std::filesystem;

namespace {
fs::path tmp_key() { return pulp::test::unique_tmp_file("pulp-key-", ".txt"); }
}  // namespace

TEST_CASE("key blob round-trips; a bad magic line fails closed", "[reload][key-store]") {
    auto kp = pulp::runtime::ed25519_keypair_generate();
    REQUIRE(kp.has_value());
    KeyMaterial km{kp->public_key, kp->private_key};
    auto back = parse_key_blob(serialize_key_blob(km));
    REQUIRE(back.has_value());
    REQUIRE(back->public_key == km.public_key);
    REQUIRE(back->private_key == km.private_key);

    REQUIRE_FALSE(parse_key_blob("not-a-pulp-key\nAAA\nBBB\n").has_value());
    REQUIRE_FALSE(parse_key_blob("PULP-RELOAD-KEY-v1\n!!!not-base64!!!\n@@@\n").has_value());
}

TEST_CASE("load_or_generate_file generates once, then reuses (never a second identity)",
          "[reload][key-store]") {
    const fs::path p = tmp_key();
    fs::remove(p);

    bool gen1 = false;
    auto k1 = load_or_generate_key_file(p, gen1);
    REQUIRE(k1.has_value());
    REQUIRE(gen1);                    // first call generated it
    REQUIRE(fs::exists(p));

    bool gen2 = true;
    auto k2 = load_or_generate_key_file(p, gen2);
    REQUIRE(k2.has_value());
    REQUIRE_FALSE(gen2);              // second call REUSED — no regeneration
    REQUIRE(k2->public_key == k1->public_key);
    REQUIRE(k2->private_key == k1->private_key);
    fs::remove(p);
}

TEST_CASE("a generated key actually signs + verifies a manifest", "[reload][key-store]") {
    const fs::path p = tmp_key();
    fs::remove(p);
    bool gen = false;
    auto k = load_or_generate_key_file(p, gen);
    REQUIRE(k.has_value());

    SwapPackManifest m;
    m.id = "p"; m.plugin_id = "q";
    m.files = {{"ui.js", pulp::runtime::sha256_hex(std::string_view("UI")), SwapPackKind::UiScript}};
    m.signer_public_key = k->public_key;
    const auto msg = swap_pack_signed_message(m);
    auto sig = pulp::runtime::ed25519_sign(k->private_key.data(), k->private_key.size(),
                                           msg.data(), msg.size());
    REQUIRE(sig.has_value());
    m.signature = *sig;
    REQUIRE(verify_swap_pack_signature(m, k->public_key).ok());
    fs::remove(p);
}
