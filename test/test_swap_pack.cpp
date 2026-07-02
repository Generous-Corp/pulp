// Swap-pack manifest schema + per-file integrity (live-swap plan item 3.1a).
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
