// Swap-pack manifest schema + per-file integrity.
// A tampered/truncated/missing file must fail closed at the hash layer before
// anything is installed. Signing (3.1b) + install (3.1c) are separate slices.
#include <catch2/catch_test_macros.hpp>

#include <pulp/format/reload/swap_pack.hpp>
#include <pulp/runtime/crypto.hpp>

#include <filesystem>
#include <fstream>
#include <string>

using namespace pulp::format::reload;
namespace fs = std::filesystem;

namespace {
void write_file(const fs::path& p, const std::string& content) {
    fs::create_directories(p.parent_path());
    std::ofstream(p, std::ios::binary) << content;
}
std::string hash_of(const std::string& content) {
    return pulp::runtime::sha256_hex(content);
}
// A per-test unique root under the build tree (no Date/rand available; use the
// Catch section-free unique name via the caller).
fs::path make_root(const std::string& name) {
    auto root = fs::temp_directory_path() / ("pulp-swap-pack-" + name);
    std::error_code ec; fs::remove_all(root, ec);
    fs::create_directories(root);
    return root;
}
}  // namespace

TEST_CASE("swap-pack manifest parses required fields + kinds", "[reload][swap-pack][3.1]") {
    std::string err;
    auto m = parse_swap_pack_manifest(R"({
        "id": "pack.reverb.hall",
        "plugin_id": "com.pulp.demo",
        "format_version": 1,
        "files": [
            {"path": "ui.js", "sha256": "aa", "kind": "ui-script"},
            {"path": "graph.pulpgraph", "sha256": "bb", "kind": "dsp-graph"}
        ]
    })", err);
    REQUIRE(m.has_value());
    REQUIRE(m->id == "pack.reverb.hall");
    REQUIRE(m->plugin_id == "com.pulp.demo");
    REQUIRE(m->files.size() == 2);
    REQUIRE(m->files[0].kind == SwapPackKind::UiScript);
    REQUIRE(m->files[1].kind == SwapPackKind::DspGraph);
}

TEST_CASE("swap-pack manifest rejects malformed JSON / missing fields", "[reload][swap-pack][3.1]") {
    std::string err;
    REQUIRE_FALSE(parse_swap_pack_manifest("not json", err).has_value());
    REQUIRE_FALSE(err.empty());
    REQUIRE_FALSE(parse_swap_pack_manifest(R"({"id":"x"})", err).has_value());        // no plugin_id/files
    REQUIRE_FALSE(parse_swap_pack_manifest(R"({"id":"x","plugin_id":"y"})", err).has_value());  // no files
    REQUIRE_FALSE(parse_swap_pack_manifest(
        R"({"id":"x","plugin_id":"y","files":[{"path":"a"}]})", err).has_value());    // file has no sha256
}

TEST_CASE("swap-pack integrity passes when every file matches its hash", "[reload][swap-pack][3.1]") {
    auto root = make_root("ok");
    write_file(root / "ui.js", "export default {}");
    write_file(root / "sub" / "graph.pulpgraph", "GRAPHBYTES");

    SwapPackManifest m;
    m.id = "p"; m.plugin_id = "q";
    m.files = {
        {"ui.js", hash_of("export default {}"), SwapPackKind::UiScript},
        {"sub/graph.pulpgraph", hash_of("GRAPHBYTES"), SwapPackKind::DspGraph},
    };
    auto r = verify_swap_pack_integrity(root, m);
    INFO("detail: " << r.detail);
    REQUIRE(r.ok());
}

TEST_CASE("swap-pack integrity fails closed on a tampered file", "[reload][swap-pack][3.1]") {
    auto root = make_root("tamper");
    write_file(root / "ui.js", "original");
    SwapPackManifest m;
    m.id = "p"; m.plugin_id = "q";
    m.files = {{"ui.js", hash_of("original"), SwapPackKind::UiScript}};
    REQUIRE(verify_swap_pack_integrity(root, m).ok());

    write_file(root / "ui.js", "TAMPERED");                 // same path, different bytes
    auto r = verify_swap_pack_integrity(root, m);
    REQUIRE_FALSE(r.ok());
    REQUIRE(r.status == SwapPackVerify::HashMismatch);
    REQUIRE(r.detail == "ui.js");
}

TEST_CASE("swap-pack integrity fails closed on a missing file", "[reload][swap-pack][3.1]") {
    auto root = make_root("missing");
    SwapPackManifest m;
    m.id = "p"; m.plugin_id = "q";
    m.files = {{"gone.js", hash_of("whatever"), SwapPackKind::UiScript}};
    auto r = verify_swap_pack_integrity(root, m);
    REQUIRE(r.status == SwapPackVerify::MissingFile);
    REQUIRE(r.detail == "gone.js");
}

// ── Ed25519 pack signature (item 3.1b) ────────────────────────────────────────
namespace {
// Sign a manifest with a keypair: set signer key + detached signature over the
// canonical signed message.
void sign_manifest(SwapPackManifest& m, const pulp::runtime::Ed25519KeyPair& kp) {
    m.signer_public_key = kp.public_key;
    const auto msg = swap_pack_signed_message(m);
    auto sig = pulp::runtime::ed25519_sign(kp.private_key.data(), kp.private_key.size(),
                                           msg.data(), msg.size());
    REQUIRE(sig.has_value());
    m.signature = *sig;
}
}  // namespace

TEST_CASE("swap-pack signature: a pack signed by the trusted key verifies end to end",
          "[reload][swap-pack][3.1]") {
    auto kp = pulp::runtime::ed25519_keypair_generate();
    REQUIRE(kp.has_value());
    auto root = make_root("sig-ok");
    write_file(root / "ui.js", "UI");
    SwapPackManifest m;
    m.id = "p"; m.plugin_id = "q";
    m.files = {{"ui.js", hash_of("UI"), SwapPackKind::UiScript}};
    sign_manifest(m, *kp);

    REQUIRE(verify_swap_pack_signature(m, kp->public_key).ok());
    REQUIRE(verify_swap_pack(root, m, kp->public_key).ok());   // sig + integrity
}

TEST_CASE("swap-pack manifest parses the policy fields", "[reload][swap-pack][policy]") {
    std::string err;
    auto m = parse_swap_pack_manifest(
        R"({"id":"p","plugin_id":"q","pack_version":5,"pack_type":"ui-script",)"
        R"("update_channel":"stable","min_host_version":8,)"
        R"("capabilities":["filesystem","network"],)"
        R"("files":[{"path":"ui.js","sha256":"ab","kind":"ui-script"}]})",
        err);
    REQUIRE(m.has_value());
    REQUIRE(m->pack_version == 5u);
    REQUIRE(m->pack_type == SwapPackKind::UiScript);
    REQUIRE(m->update_channel == "stable");
    REQUIRE(m->min_host_version == 8);
    REQUIRE(m->declared_capabilities == std::vector<std::string>{"filesystem", "network"});
    // Absent policy fields keep their safe defaults (older manifests still parse).
    auto legacy = parse_swap_pack_manifest(
        R"({"id":"p","plugin_id":"q","files":[{"path":"ui.js","sha256":"ab"}]})", err);
    REQUIRE(legacy.has_value());
    REQUIRE(legacy->pack_version == 0u);
    REQUIRE(legacy->declared_capabilities.empty());
}

TEST_CASE("swap-pack signature: policy fields are bound (tampering any of them fails)",
          "[reload][swap-pack][policy]") {
    auto kp = pulp::runtime::ed25519_keypair_generate();
    REQUIRE(kp.has_value());
    SwapPackManifest m;
    m.id = "p"; m.plugin_id = "q";
    m.files = {{"ui.js", hash_of("UI"), SwapPackKind::UiScript}};
    m.pack_version = 3;
    m.pack_type = SwapPackKind::UiScript;
    m.update_channel = "stable";
    m.min_host_version = 8;
    m.declared_capabilities = {"filesystem", "network"};
    sign_manifest(m, *kp);
    REQUIRE(verify_swap_pack_signature(m, kp->public_key).ok());

    // Every policy field is covered by the signature: mutating it without re-signing
    // breaks verification. A CDN or installer cannot swap the version, kind, channel,
    // host floor, or (critically) the granted capabilities under a still-valid sig.
    auto rejects = [&](auto mutate) {
        auto t = m; mutate(t);
        return !verify_swap_pack_signature(t, kp->public_key).ok();
    };
    REQUIRE(rejects([](SwapPackManifest& t) { t.pack_version = 2; }));            // downgrade
    REQUIRE(rejects([](SwapPackManifest& t) { t.pack_type = SwapPackKind::DspGraph; }));
    REQUIRE(rejects([](SwapPackManifest& t) { t.update_channel = "beta"; }));
    REQUIRE(rejects([](SwapPackManifest& t) { t.min_host_version = 1; }));
    REQUIRE(rejects([](SwapPackManifest& t) { t.declared_capabilities = {"filesystem"}; }));
    REQUIRE(rejects([](SwapPackManifest& t) { t.declared_capabilities.push_back("exec"); }));
}

TEST_CASE("swap-pack signature: an untrusted signer fails closed", "[reload][swap-pack][3.1]") {
    auto kp = pulp::runtime::ed25519_keypair_generate();
    auto other = pulp::runtime::ed25519_keypair_generate();
    REQUIRE(kp.has_value()); REQUIRE(other.has_value());
    SwapPackManifest m; m.id = "p"; m.plugin_id = "q";
    m.files = {{"ui.js", hash_of("UI"), SwapPackKind::UiScript}};
    sign_manifest(m, *kp);
    // Verify against a DIFFERENT trusted key → UntrustedSigner.
    auto r = verify_swap_pack_signature(m, other->public_key);
    REQUIRE(r.status == SwapPackVerify::UntrustedSigner);
}

TEST_CASE("swap-pack signature: a flipped signature byte fails closed", "[reload][swap-pack][3.1]") {
    auto kp = pulp::runtime::ed25519_keypair_generate();
    REQUIRE(kp.has_value());
    SwapPackManifest m; m.id = "p"; m.plugin_id = "q";
    m.files = {{"ui.js", hash_of("UI"), SwapPackKind::UiScript}};
    sign_manifest(m, *kp);
    m.signature[0] ^= 0x01;                                   // tamper the signature
    auto r = verify_swap_pack_signature(m, kp->public_key);
    REQUIRE(r.status == SwapPackVerify::BadSignature);
}

TEST_CASE("swap-pack signature: tampering a signed file hash breaks the signature",
          "[reload][swap-pack][3.1]") {
    auto kp = pulp::runtime::ed25519_keypair_generate();
    REQUIRE(kp.has_value());
    SwapPackManifest m; m.id = "p"; m.plugin_id = "q";
    m.files = {{"ui.js", hash_of("UI"), SwapPackKind::UiScript}};
    sign_manifest(m, *kp);
    m.files[0].sha256_hex = hash_of("EVIL");                  // re-point to attacker content
    auto r = verify_swap_pack_signature(m, kp->public_key);
    REQUIRE(r.status == SwapPackVerify::BadSignature);        // message changed → sig invalid
}

TEST_CASE("swap-pack verify: signature passes but a tampered FILE still fails integrity",
          "[reload][swap-pack][3.1]") {
    auto kp = pulp::runtime::ed25519_keypair_generate();
    REQUIRE(kp.has_value());
    auto root = make_root("sig-file-tamper");
    write_file(root / "ui.js", "UI");
    SwapPackManifest m; m.id = "p"; m.plugin_id = "q";
    m.files = {{"ui.js", hash_of("UI"), SwapPackKind::UiScript}};
    sign_manifest(m, *kp);
    REQUIRE(verify_swap_pack(root, m, kp->public_key).ok());
    write_file(root / "ui.js", "EVIL");                       // bytes differ from the signed hash
    auto r = verify_swap_pack(root, m, kp->public_key);
    REQUIRE(r.status == SwapPackVerify::HashMismatch);        // sig ok, integrity fails closed
}

TEST_CASE("swap-pack signed message is unambiguous: newline-in-path can't collide (3.1b hardening)",
          "[reload][swap-pack][3.1]") {
    // The length-prefixed encoding must NOT let a single file whose path embeds
    // the delimiter structure collide with a two-file manifest (the classic
    // newline-join forgery). Distinct manifests → distinct signed messages.
    SwapPackManifest two; two.id = "p"; two.plugin_id = "q";
    two.files = {{"A", "B", SwapPackKind::UiScript}, {"C", "D", SwapPackKind::DspGraph}};
    SwapPackManifest one; one.id = "p"; one.plugin_id = "q";
    one.files = {{"A\nB\nui-script\nC", "D", SwapPackKind::DspGraph}};
    REQUIRE(swap_pack_signed_message(two) != swap_pack_signed_message(one));

    // And the file COUNT is bound: dropping a file changes the message.
    SwapPackManifest dropped = two; dropped.files.pop_back();
    REQUIRE(swap_pack_signed_message(two) != swap_pack_signed_message(dropped));
}
