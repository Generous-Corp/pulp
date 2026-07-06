// Remote UX update decision: the pure, fail-closed gate for the opt-in signed
// auto-update path. Accepts only a signed, ui-script, non-downgrade, non-revoked
// pack with a fresh revocation list and no capability escalation.
#include <catch2/catch_test_macros.hpp>

#include <pulp/format/reload/remote_update.hpp>

#include "reload_test_support.hpp"

#include <filesystem>
#include <fstream>
#include <string>

using namespace pulp::format::reload;
using pulp::view::CapabilitySet;
using pulp::view::ReloadCapability;
namespace fs = std::filesystem;

namespace {
std::string hash_of(std::string_view s) { return pulp::runtime::sha256_hex(s); }

struct Staged {
    fs::path root;
    SwapPackManifest manifest;
    std::vector<std::uint8_t> trusted_key;
};

// A signed ui-script pack at version `ver` declaring `caps`.
Staged stage(std::uint64_t ver, std::vector<std::string> caps,
             SwapPackKind kind = SwapPackKind::UiScript) {
    auto kp = pulp::runtime::ed25519_keypair_generate();
    REQUIRE(kp.has_value());
    fs::path root = pulp::test::unique_tmp_dir("pulp-remote-");
    const std::string bytes = "export const ui = 1;";
    std::ofstream(root / "ui.js", std::ios::binary).write(bytes.data(),
                                                          static_cast<std::streamsize>(bytes.size()));
    SwapPackManifest m;
    m.id = "p"; m.plugin_id = "com.pulp.demo";
    m.pack_type = kind;
    m.pack_version = ver;
    m.declared_capabilities = std::move(caps);
    m.files = {{"ui.js", hash_of(bytes), SwapPackKind::UiScript}};
    m.signer_public_key = kp->public_key;
    const auto msg = swap_pack_signed_message(m);
    auto sig = pulp::runtime::ed25519_sign(kp->private_key.data(), kp->private_key.size(),
                                           msg.data(), msg.size());
    REQUIRE(sig.has_value());
    m.signature = *sig;
    return {root, m, kp->public_key};
}

CapabilitySet caps_of(std::vector<std::string> names) {
    CapabilitySet s;
    REQUIRE(CapabilitySet::parse(names, s));
    return s;
}
}  // namespace

TEST_CASE("remote update accepts a signed, fresh, non-downgrade UX pack", "[reload][remote-update]") {
    auto s = stage(/*ver=*/5, {"filesystem"});
    SignedRevocationList srl;  // fresh, revokes nothing
    srl.epoch = 1;
    auto r = evaluate_remote_ux_update(s.root, s.manifest, s.trusted_key,
                                       /*installed=*/4, caps_of({"filesystem", "network"}),
                                       /*srl_fresh=*/true, &srl);
    REQUIRE(r.ok());
    fs::remove_all(s.root);
}

TEST_CASE("remote update rejects a wrong-signer pack", "[reload][remote-update]") {
    auto s = stage(5, {});
    auto attacker = pulp::runtime::ed25519_keypair_generate();
    SignedRevocationList srl; srl.epoch = 1;
    auto r = evaluate_remote_ux_update(s.root, s.manifest, attacker->public_key, 4,
                                       CapabilitySet::all(), true, &srl);
    REQUIRE(r.verdict == RemoteUpdateVerdict::RejectSignature);
    fs::remove_all(s.root);
}

TEST_CASE("remote update rejects a non-ui-script pack (wrong lane)", "[reload][remote-update]") {
    auto s = stage(5, {}, SwapPackKind::DspGraph);
    SignedRevocationList srl; srl.epoch = 1;
    auto r = evaluate_remote_ux_update(s.root, s.manifest, s.trusted_key, 4,
                                       CapabilitySet::all(), true, &srl);
    REQUIRE(r.verdict == RemoteUpdateVerdict::RejectKind);
    fs::remove_all(s.root);
}

TEST_CASE("remote update fails closed without a fresh revocation list", "[reload][remote-update]") {
    auto s = stage(5, {});
    auto r = evaluate_remote_ux_update(s.root, s.manifest, s.trusted_key, 4,
                                       CapabilitySet::all(), /*srl_fresh=*/false, nullptr);
    REQUIRE(r.verdict == RemoteUpdateVerdict::RejectStaleRevocation);
    fs::remove_all(s.root);
}

TEST_CASE("remote update rejects a revoked signer", "[reload][remote-update]") {
    auto s = stage(5, {});
    SignedRevocationList srl;
    srl.epoch = 1;
    srl.revoked_signer_key_fprs = {srl_hex_encode(s.manifest.signer_public_key)};
    auto r = evaluate_remote_ux_update(s.root, s.manifest, s.trusted_key, 4,
                                       CapabilitySet::all(), true, &srl);
    REQUIRE(r.verdict == RemoteUpdateVerdict::RejectRevoked);
    fs::remove_all(s.root);
}

TEST_CASE("remote update rejects a downgrade", "[reload][remote-update]") {
    auto s = stage(/*ver=*/3, {});
    SignedRevocationList srl; srl.epoch = 1;
    auto r = evaluate_remote_ux_update(s.root, s.manifest, s.trusted_key,
                                       /*installed=*/3, CapabilitySet::all(), true, &srl);
    REQUIRE(r.verdict == RemoteUpdateVerdict::RejectDowngrade);
    fs::remove_all(s.root);
}

TEST_CASE("remote update rejects capability escalation", "[reload][remote-update]") {
    auto s = stage(5, {"network"});  // pushed UX wants network
    SignedRevocationList srl; srl.epoch = 1;
    // Host granted only filesystem → network is an escalation.
    auto r = evaluate_remote_ux_update(s.root, s.manifest, s.trusted_key, 4,
                                       caps_of({"filesystem"}), true, &srl);
    REQUIRE(r.verdict == RemoteUpdateVerdict::RejectCapabilityEscalation);
    fs::remove_all(s.root);
}
