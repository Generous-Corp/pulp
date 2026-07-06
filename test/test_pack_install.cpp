// Content-addressed immutable install for verified swap packs. Proves the installed
// directory holds the verified bytes (content-hash named, read-only), that install is
// idempotent, and that symlinks / unknown kinds / untrusted packs are refused before
// anything is published — closing the verify-then-reload TOCTOU on the loaded bytes.
#include <catch2/catch_test_macros.hpp>

#include <pulp/format/reload/pack_install.hpp>
#include <pulp/runtime/crypto.hpp>

#include <filesystem>
#include <fstream>
#include <string>

using namespace pulp::format::reload;
namespace fs = std::filesystem;

namespace {
std::string unique_suffix() {
    static int counter = 0;
    return std::to_string(++counter);
}
fs::path make_root(const std::string& tag) {
    fs::path r = fs::temp_directory_path() / ("pulp-install-" + tag + "-" + unique_suffix());
    fs::remove_all(r);
    fs::create_directories(r);
    return r;
}
void write_file(const fs::path& p, std::string_view bytes) {
    fs::create_directories(p.parent_path());
    std::ofstream o(p, std::ios::binary | std::ios::trunc);
    o.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}
std::string hash_of(std::string_view s) { return pulp::runtime::sha256_hex(s); }

// A trusted single-file UI pack under a fresh root; returns (root, manifest, key).
struct Built {
    fs::path root;
    SwapPackManifest manifest;
    std::vector<std::uint8_t> trusted_key;
};
Built build_pack(const std::string& tag, std::string_view ui_bytes) {
    auto kp = pulp::runtime::ed25519_keypair_generate();
    REQUIRE(kp.has_value());
    fs::path root = make_root(tag);
    write_file(root / "ui.js", ui_bytes);
    SwapPackManifest m;
    m.id = "p"; m.plugin_id = "com.pulp.demo";
    m.pack_type = SwapPackKind::UiScript;
    m.files = {{"ui.js", hash_of(ui_bytes), SwapPackKind::UiScript}};
    m.signer_public_key = kp->public_key;
    const auto msg = swap_pack_signed_message(m);
    auto sig = pulp::runtime::ed25519_sign(kp->private_key.data(), kp->private_key.size(),
                                           msg.data(), msg.size());
    REQUIRE(sig.has_value());
    m.signature = *sig;
    return {root, m, kp->public_key};
}
}  // namespace

TEST_CASE("install: a trusted pack installs to a content-addressed dir with the verified bytes",
          "[reload][install]") {
    auto b = build_pack("ok", "export const ui = 1;");
    const fs::path base = make_root("base-ok");
    auto r = install_verified_pack(b.root, b.manifest, b.trusted_key, base);
    REQUIRE(r.ok());
    // Named by content id, under the install base.
    REQUIRE(r.installed_root.parent_path() == base);
    REQUIRE(r.installed_root.filename() == swap_pack_content_id(b.manifest));
    // The installed file exists and its bytes match what was signed.
    std::ifstream in(r.installed_root / "ui.js", std::ios::binary);
    std::string got((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    REQUIRE(got == "export const ui = 1;");
    fs::permissions(r.installed_root, fs::perms::owner_all, fs::perm_options::add);
    fs::remove_all(base);
    fs::remove_all(b.root);
}

TEST_CASE("install: idempotent — the same content installs to the same dir",
          "[reload][install]") {
    auto b = build_pack("idem", "A");
    const fs::path base = make_root("base-idem");
    auto r1 = install_verified_pack(b.root, b.manifest, b.trusted_key, base);
    auto r2 = install_verified_pack(b.root, b.manifest, b.trusted_key, base);
    REQUIRE(r1.ok());
    REQUIRE(r2.ok());
    REQUIRE(r1.installed_root == r2.installed_root);
    fs::permissions(r1.installed_root, fs::perms::owner_all, fs::perm_options::add);
    fs::remove_all(base);
    fs::remove_all(b.root);
}

TEST_CASE("install: an untrusted pack is refused before anything is published",
          "[reload][install]") {
    auto b = build_pack("untrusted", "A");
    auto attacker = pulp::runtime::ed25519_keypair_generate();
    REQUIRE(attacker.has_value());
    const fs::path base = make_root("base-untrusted");
    // Verify against the WRONG key → NotTrusted, nothing installed.
    auto r = install_verified_pack(b.root, b.manifest, attacker->public_key, base);
    REQUIRE_FALSE(r.ok());
    REQUIRE(r.status == PackInstall::NotTrusted);
    REQUIRE(fs::is_empty(base));
    fs::remove_all(base);
    fs::remove_all(b.root);
}

TEST_CASE("install: a symlinked pack file is refused", "[reload][install]") {
    auto b = build_pack("symlink", "REAL");
    // Replace the real file with a symlink to an outside target of the same bytes.
    // The hash still matches (same bytes), so ONLY the symlink check can catch this.
    const fs::path outside = make_root("symlink-target") / "elsewhere.js";
    write_file(outside, "REAL");
    fs::remove(b.root / "ui.js");
    std::error_code ec;
    fs::create_symlink(outside, b.root / "ui.js", ec);
    if (ec) { SUCCEED("platform does not support symlinks; skipping"); return; }

    const fs::path base = make_root("base-symlink");
    auto r = install_verified_pack(b.root, b.manifest, b.trusted_key, base);
    REQUIRE_FALSE(r.ok());
    REQUIRE(r.status == PackInstall::SymlinkRejected);
    REQUIRE(fs::is_empty(base));
    fs::remove_all(base);
    fs::remove_all(b.root);
}

TEST_CASE("install: an unknown-kind file is refused for consumer packs",
          "[reload][install]") {
    auto b = build_pack("unknown-kind", "A");
    b.manifest.files.front().kind = SwapPackKind::Unknown;
    // Re-sign so the signature still matches the (now unknown-kind) manifest — the
    // kind check, not the signature, must be what refuses it.
    auto kp = pulp::runtime::ed25519_keypair_generate();
    REQUIRE(kp.has_value());
    b.manifest.signer_public_key = kp->public_key;
    const auto msg = swap_pack_signed_message(b.manifest);
    auto sig = pulp::runtime::ed25519_sign(kp->private_key.data(), kp->private_key.size(),
                                           msg.data(), msg.size());
    REQUIRE(sig.has_value());
    b.manifest.signature = *sig;

    const fs::path base = make_root("base-unknown");
    auto r = install_verified_pack(b.root, b.manifest, kp->public_key, base);
    REQUIRE_FALSE(r.ok());
    REQUIRE(r.status == PackInstall::UnknownKind);
    fs::remove_all(base);
    fs::remove_all(b.root);
}
