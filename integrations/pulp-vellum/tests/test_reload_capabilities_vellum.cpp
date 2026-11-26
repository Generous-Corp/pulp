// Reload capability names and remote UX update validation, built without Pulp's
// view layer.
//
// A signed content pack declares capabilities by wire name. Pulp's format layer
// validates those names for a remote update, and Vellum's scripted-UI bridge
// enforces the grant, so both sides must agree on every name. This target links
// format-core and Vellum only, so it also proves pack building, signing,
// installing and the remote-update gate build and run with no Pulp view headers
// present.
#include <catch2/catch_test_macros.hpp>
#include <pulp/format/reload/pack_build.hpp>
#include <pulp/format/reload/pack_install.hpp>
#include <pulp/format/reload/remote_update.hpp>
#include <pulp/runtime/crypto.hpp>
#include <pulp/runtime/reload_capabilities.hpp>
#include <vellum/view/reload_capabilities.hpp>

#include <array>
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <utility>
#include <vector>

using namespace pulp::format::reload;
namespace fs = std::filesystem;

namespace {

using PulpCapability = pulp::runtime::ReloadCapability;
using VellumCapability = ::vellum::view::ReloadCapability;

constexpr std::array<std::pair<PulpCapability, VellumCapability>, 7> kCapabilities{{
    {PulpCapability::Exec, VellumCapability::Exec},
    {PulpCapability::Clipboard, VellumCapability::Clipboard},
    {PulpCapability::Filesystem, VellumCapability::Filesystem},
    {PulpCapability::Storage, VellumCapability::Storage},
    {PulpCapability::Ai, VellumCapability::Ai},
    {PulpCapability::RuntimeImport, VellumCapability::RuntimeImport},
    {PulpCapability::Network, VellumCapability::Network},
}};

struct StagedPack {
    fs::path root;
    SwapPackManifest manifest;
    std::vector<std::uint8_t> trusted_key;
    ~StagedPack() {
        std::error_code ec;
        fs::remove_all(root, ec);
    }
};

// A signed ui-script pack at `version` declaring `capabilities`.
StagedPack stage(std::uint64_t version, std::vector<std::string> capabilities) {
    static std::atomic<int> counter{0};
    auto keys = pulp::runtime::ed25519_keypair_generate();
    REQUIRE(keys.has_value());
    StagedPack pack;
    pack.root =
        fs::temp_directory_path() / ("pulp-vellum-remote-" + std::to_string(counter.fetch_add(1)) +
                                     "-" + std::to_string(reinterpret_cast<std::uintptr_t>(&pack)));
    fs::create_directories(pack.root);
    const std::string bytes = "export const ui = 1;";
    std::ofstream(pack.root / "ui.js", std::ios::binary)
        .write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    auto& m = pack.manifest;
    m.id = "p";
    m.plugin_id = "com.pulp.demo";
    m.pack_type = SwapPackKind::UiScript;
    m.pack_version = version;
    m.declared_capabilities = std::move(capabilities);
    m.files = {{"ui.js", pulp::runtime::sha256_hex(bytes), SwapPackKind::UiScript}};
    m.signer_public_key = keys->public_key;
    const auto message = swap_pack_signed_message(m);
    auto signature = pulp::runtime::ed25519_sign(keys->private_key.data(), keys->private_key.size(),
                                                 message.data(), message.size());
    REQUIRE(signature.has_value());
    m.signature = *signature;
    pack.trusted_key = keys->public_key;
    return pack;
}

pulp::runtime::CapabilitySet granted(const std::vector<std::string>& names) {
    pulp::runtime::CapabilitySet set;
    REQUIRE(pulp::runtime::CapabilitySet::parse(names, set));
    return set;
}

} // namespace

TEST_CASE("Pulp and Vellum agree on every reload capability wire name",
          "[pulp-vellum][reload][capabilities]") {
    for (const auto& [pulp_capability, vellum_capability] : kCapabilities) {
        const auto name = pulp::runtime::capability_name(pulp_capability);
        CHECK(name == ::vellum::view::capability_name(vellum_capability));
        PulpCapability pulp_parsed{};
        VellumCapability vellum_parsed{};
        REQUIRE(pulp::runtime::capability_from_name(name, pulp_parsed));
        REQUIRE(::vellum::view::capability_from_name(name, vellum_parsed));
        CHECK(pulp_parsed == pulp_capability);
        CHECK(vellum_parsed == vellum_capability);
    }
    // Both fail closed on a name neither side defines.
    PulpCapability pulp_unknown{};
    VellumCapability vellum_unknown{};
    CHECK_FALSE(pulp::runtime::capability_from_name("camera", pulp_unknown));
    CHECK_FALSE(::vellum::view::capability_from_name("camera", vellum_unknown));
    pulp::runtime::CapabilitySet pulp_set;
    ::vellum::view::CapabilitySet vellum_set;
    CHECK_FALSE(pulp::runtime::CapabilitySet::parse({"network", "camera"}, pulp_set));
    CHECK_FALSE(::vellum::view::CapabilitySet::parse({"network", "camera"}, vellum_set));
}

TEST_CASE("Remote UX update validation runs without the Pulp view layer",
          "[pulp-vellum][reload][remote-update]") {
    SignedRevocationList srl;
    srl.epoch = 1;

    SECTION("a signed pack inside the host's grant is accepted") {
        auto pack = stage(5, {"filesystem"});
        const auto result = evaluate_remote_ux_update(pack.root, pack.manifest, pack.trusted_key, 4,
                                                      granted({"filesystem"}), true, &srl);
        CHECK(result.verdict == RemoteUpdateVerdict::Accept);
    }
    SECTION("a capability the host did not grant is an escalation") {
        auto pack = stage(5, {"network"});
        const auto result = evaluate_remote_ux_update(pack.root, pack.manifest, pack.trusted_key, 4,
                                                      granted({"filesystem"}), true, &srl);
        CHECK(result.verdict == RemoteUpdateVerdict::RejectCapabilityEscalation);
    }
    SECTION("an unknown declared capability fails closed") {
        auto pack = stage(5, {"camera"});
        const auto result =
            evaluate_remote_ux_update(pack.root, pack.manifest, pack.trusted_key, 4,
                                      pulp::runtime::CapabilitySet::all(), true, &srl);
        CHECK(result.verdict == RemoteUpdateVerdict::RejectCapabilityEscalation);
    }
    SECTION("a pack signed by another key is rejected first") {
        auto pack = stage(5, {});
        auto other = pulp::runtime::ed25519_keypair_generate();
        REQUIRE(other.has_value());
        const auto result = evaluate_remote_ux_update(pack.root, pack.manifest, other->public_key,
                                                      4, granted({}), true, &srl);
        CHECK(result.verdict == RemoteUpdateVerdict::RejectSignature);
    }
}

TEST_CASE("A UX pack is built, signed, installed and accepted without the Pulp view layer",
          "[pulp-vellum][reload][signed-pack]") {
    static std::atomic<int> counter{0};
    const auto base = fs::temp_directory_path() /
                      ("pulp-vellum-signed-pack-" + std::to_string(counter.fetch_add(1)) + "-" +
                       std::to_string(reinterpret_cast<std::uintptr_t>(&counter)));
    const auto bundle = base / "bundle";
    const auto installs = base / "installed";
    fs::create_directories(bundle / "assets");
    std::ofstream(bundle / "ui.js", std::ios::binary)
        << "createLabel('v', 'A', '');\nstorageSetItem('last', 'A');\n";
    std::ofstream(bundle / "assets" / "knob.svg", std::ios::binary) << "<svg/>";

    // Capabilities are inferred from the bundle's JS, least privilege first.
    auto built = build_signable_manifest(bundle, "com.pulp.demo", 7, "stable");
    REQUIRE(built.ok);
    auto& manifest = built.manifest;
    CHECK(manifest.declared_capabilities == std::vector<std::string>{"storage"});
    REQUIRE(manifest.files.size() == 2);

    auto keys = pulp::runtime::ed25519_keypair_generate();
    REQUIRE(keys.has_value());
    manifest.signer_public_key = keys->public_key;
    const auto message = swap_pack_signed_message(manifest);
    auto signature = pulp::runtime::ed25519_sign(keys->private_key.data(), keys->private_key.size(),
                                                 message.data(), message.size());
    REQUIRE(signature.has_value());
    manifest.signature = *signature;
    REQUIRE(verify_swap_pack(bundle, manifest, keys->public_key).ok());

    const auto installed = install_verified_pack(bundle, manifest, keys->public_key, installs);
    REQUIRE(installed.ok());
    CHECK(fs::exists(installed.installed_root / "assets" / "knob.svg"));

    SignedRevocationList srl;
    srl.epoch = 1;
    CHECK(evaluate_remote_ux_update(installed.installed_root, manifest, keys->public_key, 6,
                                    granted({"storage"}), true, &srl)
              .verdict == RemoteUpdateVerdict::Accept);
    CHECK(evaluate_remote_ux_update(installed.installed_root, manifest, keys->public_key, 6,
                                    granted({}), true, &srl)
              .verdict == RemoteUpdateVerdict::RejectCapabilityEscalation);

    // A byte changed after signing is refused at install.
    std::ofstream(bundle / "ui.js", std::ios::binary | std::ios::app) << "exec('rm');\n";
    const auto tampered =
        install_verified_pack(bundle, manifest, keys->public_key, base / "installed-tampered");
    CHECK(tampered.status == PackInstall::NotTrusted);

    std::error_code ec;
    fs::permissions(installs, fs::perms::owner_all, fs::perm_options::add, ec);
    for (auto it = fs::recursive_directory_iterator(installs, ec);
         !ec && it != fs::recursive_directory_iterator(); it.increment(ec))
        fs::permissions(it->path(), fs::perms::owner_all, fs::perm_options::add, ec);
    fs::remove_all(base, ec);
}
