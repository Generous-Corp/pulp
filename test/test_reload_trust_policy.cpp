// Opt-in require-signed policy for the dev reload watcher: off loads unsigned, on
// resolves a signed sidecar into a trust that verifies, and on with no/invalid
// sidecar refuses.
#include <catch2/catch_test_macros.hpp>

#include <pulp/format/reload/reload_trust_policy.hpp>

#include "reload_test_support.hpp"

#include <filesystem>
#include <fstream>
#include <string>

using namespace pulp::format::reload;
namespace fs = std::filesystem;

namespace {
fs::path make_dir() { return pulp::test::unique_tmp_dir("pulp-trustpol-"); }
void write(const fs::path& p, std::string_view s) {
    std::ofstream(p, std::ios::binary).write(s.data(), static_cast<std::streamsize>(s.size()));
}
// Write a signed sidecar for `logic` and return the trusted public key.
std::vector<std::uint8_t> write_signed_sidecar(const fs::path& logic, std::string_view bytes) {
    auto kp = pulp::runtime::ed25519_keypair_generate();
    REQUIRE(kp.has_value());
    SwapPackManifest m;
    m.id = "p"; m.plugin_id = "q";
    m.files = {{logic.filename().string(), pulp::runtime::sha256_hex(bytes), SwapPackKind::DspGraph}};
    m.signer_public_key = kp->public_key;
    const auto msg = swap_pack_signed_message(m);
    auto sig = pulp::runtime::ed25519_sign(kp->private_key.data(), kp->private_key.size(),
                                           msg.data(), msg.size());
    REQUIRE(sig.has_value());
    m.signature = *sig;
    write(reload_sidecar_manifest_path(logic), serialize_swap_pack_manifest(m));
    return kp->public_key;
}
}  // namespace

TEST_CASE("require_signed OFF → load without trust", "[reload][trust-policy]") {
    auto d = make_dir();
    const fs::path logic = d / "logic.mod";
    write(logic, "bytes");
    auto decision = resolve_reload_trust(logic, ReloadTrustPolicy{/*require_signed=*/false, {}});
    REQUIRE(std::holds_alternative<std::monostate>(decision));
    fs::remove_all(d);
}

TEST_CASE("require_signed ON + valid signed sidecar → a trust that verifies",
          "[reload][trust-policy]") {
    auto d = make_dir();
    const fs::path logic = d / "logic.mod";
    const std::string bytes = "the-logic-bytes";
    write(logic, bytes);
    auto key = write_signed_sidecar(logic, bytes);

    auto decision = resolve_reload_trust(logic, ReloadTrustPolicy{true, key});
    REQUIRE(std::holds_alternative<SwapPackTrust>(decision));
    const auto& trust = std::get<SwapPackTrust>(decision);
    // The resolved trust authenticates the on-disk logic (sig + integrity).
    REQUIRE(verify_swap_pack(trust.pack_root, trust.manifest, key).ok());
    fs::remove_all(d);
}

TEST_CASE("require_signed ON + no sidecar → refuse", "[reload][trust-policy]") {
    auto d = make_dir();
    const fs::path logic = d / "logic.mod";
    write(logic, "bytes");
    auto decision = resolve_reload_trust(logic, ReloadTrustPolicy{true, {}});
    REQUIRE(std::holds_alternative<ReloadOutcome>(decision));
    REQUIRE(std::get<ReloadOutcome>(decision).status == ReloadOutcome::Status::RejectedSignature);
    fs::remove_all(d);
}

TEST_CASE("require_signed ON + malformed sidecar → refuse (fail closed)",
          "[reload][trust-policy]") {
    auto d = make_dir();
    const fs::path logic = d / "logic.mod";
    write(logic, "bytes");
    write(reload_sidecar_manifest_path(logic), "{ not valid manifest json");
    auto decision = resolve_reload_trust(logic, ReloadTrustPolicy{true, {}});
    REQUIRE(std::holds_alternative<ReloadOutcome>(decision));
    fs::remove_all(d);
}
