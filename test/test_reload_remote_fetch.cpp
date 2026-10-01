// Opt-in remote-update fetch orchestration: the network layer over the pure gate.
// Uses an INJECTED fake fetcher (no real network) to prove: disabled = no touch;
// non-HTTPS / no fetcher / unreachable = Unavailable (offline-graceful, with an
// optional fallback); a served-but-malformed manifest = Rejected; a valid signed
// newer pack = Applied (installed); an equal/older server version = UpToDate; and
// that the update-check URL leaks nothing beyond plugin/channel/version.
#include <catch2/catch_test_macros.hpp>

#include <pulp/format/reload/remote_update_fetch.hpp>

#include "reload_test_support.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <string>

using namespace pulp::format::reload;
using pulp::view::CapabilitySet;
namespace fs = std::filesystem;

namespace {
std::string hash_of(std::string_view s) { return pulp::runtime::sha256_hex(s); }

// A signed ui-script pack at version `ver`, materialized on disk; returns the pack
// root, its serialized manifest JSON, and the trusted signer key.
struct Served {
    fs::path root;
    std::string manifest_json;
    SwapPackManifest manifest;
    std::vector<std::uint8_t> key;
};
Served serve(std::uint64_t ver) {
    auto kp = pulp::runtime::ed25519_keypair_generate();
    REQUIRE(kp.has_value());
    fs::path root = pulp::test::unique_tmp_dir("pulp-fetch-src-");
    const std::string bytes = "export const ui = " + std::to_string(ver) + ";";
    std::ofstream(root / "ui.js", std::ios::binary)
        .write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    SwapPackManifest m;
    m.id = "p"; m.plugin_id = "com.pulp.demo";
    m.pack_type = SwapPackKind::UiScript;
    m.pack_version = ver;
    m.files = {{"ui.js", hash_of(bytes), SwapPackKind::UiScript}};
    m.signer_public_key = kp->public_key;
    const auto msg = swap_pack_signed_message(m);
    auto sig = pulp::runtime::ed25519_sign(kp->private_key.data(), kp->private_key.size(),
                                           msg.data(), msg.size());
    REQUIRE(sig.has_value());
    m.signature = *sig;
    return {root, serialize_swap_pack_manifest(m), m, kp->public_key};
}

RemoteUpdateConfig base_config(const Served& s, std::uint64_t installed) {
    RemoteUpdateConfig cfg;
    cfg.enabled = true;
    cfg.base_url = "https://updates.example/check";
    cfg.plugin_id = "com.pulp.demo";
    cfg.trusted_public_key = s.key;
    cfg.installed_pack_version = installed;
    cfg.host_granted = CapabilitySet::all();
    cfg.install_base = pulp::test::unique_tmp_dir("pulp-fetch-install-");
    return cfg;
}
SignedRevocationList fresh_srl() { SignedRevocationList srl; srl.epoch = 1; return srl; }
}  // namespace

TEST_CASE("remote fetch: disabled touches no network", "[reload][remote-fetch]") {
    auto s = serve(5);
    auto cfg = base_config(s, 4);
    cfg.enabled = false;
    bool fetched = false;
    cfg.fetcher = [&](const std::string&) { fetched = true; return FetchedPack{}; };
    auto r = check_and_fetch_remote_ux_update(cfg, /*srl_fresh=*/true, nullptr);
    REQUIRE(r.status == RemoteFetchStatus::Disabled);
    REQUIRE_FALSE(fetched);            // the fetcher was never invoked
    fs::remove_all(s.root);
}

TEST_CASE("remote fetch: a non-HTTPS endpoint is refused (Unavailable)", "[reload][remote-fetch]") {
    auto s = serve(5);
    auto cfg = base_config(s, 4);
    cfg.base_url = "http://updates.example/check";  // plaintext
    cfg.fetcher = [&](const std::string&) { return FetchedPack{true, s.manifest_json, s.root, ""}; };
    auto srl = fresh_srl();
    auto r = check_and_fetch_remote_ux_update(cfg, true, &srl);
    REQUIRE(r.status == RemoteFetchStatus::Unavailable);
    REQUIRE(r.detail.find("https") != std::string::npos);
    fs::remove_all(s.root);
}

TEST_CASE("remote fetch: offline is graceful and can use a fallback", "[reload][remote-fetch]") {
    auto s = serve(5);
    auto srl = fresh_srl();

    // Unreachable, no fallback → Unavailable, no installed_root.
    auto cfg = base_config(s, 4);
    cfg.fetcher = [](const std::string&) { return FetchedPack{/*reachable=*/false, "", {}, "no route"}; };
    auto r = check_and_fetch_remote_ux_update(cfg, true, &srl);
    REQUIRE(r.status == RemoteFetchStatus::Unavailable);
    REQUIRE(r.installed_root.empty());

    // Same, but the developer supplied an offline fallback pack (keep THEIR look).
    const fs::path fallback = pulp::test::unique_tmp_dir("pulp-fetch-fallback-");
    cfg.offline_fallback_root = fallback;
    auto r2 = check_and_fetch_remote_ux_update(cfg, true, &srl);
    REQUIRE(r2.status == RemoteFetchStatus::Unavailable);
    REQUIRE(r2.installed_root == fallback);   // caller can load the fallback to stay on-brand
    fs::remove_all(s.root);
    fs::remove_all(fallback);
}

TEST_CASE("remote fetch: a served-but-malformed manifest is Rejected (not offline)",
          "[reload][remote-fetch]") {
    auto s = serve(5);
    auto cfg = base_config(s, 4);
    cfg.fetcher = [](const std::string&) { return FetchedPack{true, "not json at all", {}, ""}; };
    auto srl = fresh_srl();
    auto r = check_and_fetch_remote_ux_update(cfg, true, &srl);
    REQUIRE(r.status == RemoteFetchStatus::Rejected);
    fs::remove_all(s.root);
}

TEST_CASE("remote fetch: a valid signed newer pack is Applied + installed",
          "[reload][remote-fetch]") {
    auto s = serve(/*ver=*/5);
    auto cfg = base_config(s, /*installed=*/4);
    cfg.fetcher = [&](const std::string&) { return FetchedPack{true, s.manifest_json, s.root, ""}; };
    auto srl = fresh_srl();
    auto r = check_and_fetch_remote_ux_update(cfg, /*srl_fresh=*/true, &srl);
    INFO("detail: " << r.detail);
    REQUIRE(r.status == RemoteFetchStatus::Applied);
    REQUIRE_FALSE(r.installed_root.empty());
    REQUIRE(fs::exists(r.installed_root / "ui.js"));   // installed content-addressed
    // install_verified_pack publishes the pack READ-ONLY (immutable): the
    // content-addressed dir is dr-x and its files r--, so restore owner
    // permissions through the whole tree before removing it.
    fs::remove_all(s.root);
    REQUIRE(pulp::test::remove_tmp_tree(cfg.install_base));
    REQUIRE_FALSE(fs::exists(cfg.install_base));
}

#ifndef _WIN32  // POSIX permission bits; Windows has no read-only directories.
TEST_CASE("remote fetch: test temp trees are removable after a read-only install",
          "[reload][remote-fetch][test-hygiene]") {
    // The exact shape install_verified_pack publishes, built by hand so the
    // assertion does not depend on the fetch path succeeding.
    const fs::path base = pulp::test::unique_tmp_dir("pulp-fetch-install-");
    const fs::path pack = base / "2ae42d8a";
    fs::create_directories(pack / "sub");
    std::ofstream(pack / "ui.js") << "export const ui = 1;";
    fs::permissions(pack / "ui.js", fs::perms::owner_read, fs::perm_options::replace);
    fs::permissions(pack / "sub", fs::perms::none, fs::perm_options::replace);
    fs::permissions(pack, fs::perms::owner_read | fs::perms::owner_exec,
                    fs::perm_options::replace);
    std::error_code ec;
    fs::remove_all(base, ec);
    REQUIRE(fs::exists(pack / "ui.js"));      // a bare remove_all leaves it behind
    REQUIRE(pulp::test::remove_tmp_tree(base));
    REQUIRE_FALSE(fs::exists(base));
}
#endif

TEST_CASE("remote fetch: an equal/older server version is UpToDate (benign)",
          "[reload][remote-fetch]") {
    auto s = serve(/*ver=*/3);
    auto cfg = base_config(s, /*installed=*/3);   // server offers nothing newer
    cfg.fetcher = [&](const std::string&) { return FetchedPack{true, s.manifest_json, s.root, ""}; };
    auto srl = fresh_srl();
    auto r = check_and_fetch_remote_ux_update(cfg, true, &srl);
    REQUIRE(r.status == RemoteFetchStatus::UpToDate);
    REQUIRE(r.installed_root.empty());
    fs::remove_all(s.root);
}

TEST_CASE("remote fetch: the update-check URL leaks nothing beyond plugin/channel/version",
          "[reload][remote-fetch][privacy]") {
    const std::string url = build_update_check_url("https://u.example/check", "com.pulp.demo",
                                                   "stable", 7);
    REQUIRE(url.rfind("https://", 0) == 0);
    REQUIRE(url.find("plugin=com.pulp.demo") != std::string::npos);
    REQUIRE(url.find("channel=stable") != std::string::npos);
    REQUIRE(url.find("installed=7") != std::string::npos);
    // Exactly three query parameters — no device/user/telemetry fields sneak in.
    REQUIRE(std::count(url.begin(), url.end(), '=') == 3);
    REQUIRE(std::count(url.begin(), url.end(), '&') == 2);
    // A value with reserved characters is percent-encoded, not injected raw.
    const std::string escaped = build_update_check_url("https://u.example/c", "a b&x=1", "s", 0);
    REQUIRE(escaped.find("a b&x=1") == std::string::npos);
    REQUIRE(escaped.find("plugin=a%20b%26x%3D1") != std::string::npos);
}

TEST_CASE("remote fetch: an untrusted manifest path cannot escape the download dir",
          "[reload][remote-fetch][security]") {
    // The default fetcher writes files BEFORE the signature is verified, so a hostile
    // manifest path must be screened lexically first.
    REQUIRE(is_safe_pack_member_path("ui.js"));
    REQUIRE(is_safe_pack_member_path("sub/theme.json"));
    REQUIRE_FALSE(is_safe_pack_member_path(""));
    REQUIRE_FALSE(is_safe_pack_member_path("/etc/passwd"));       // absolute
    REQUIRE_FALSE(is_safe_pack_member_path("../../etc/passwd"));  // traversal
    REQUIRE_FALSE(is_safe_pack_member_path("a/../../b"));         // traversal mid-path
}
