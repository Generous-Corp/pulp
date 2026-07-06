// Building a signable swap-pack manifest from a UX bundle: files are hashed into
// deterministic entries and the capability set is inferred from the bundle's JS, so
// a developer never hand-writes declared_capabilities.
#include <catch2/catch_test_macros.hpp>

#include <pulp/format/reload/pack_build.hpp>

#include "reload_test_support.hpp"

#include <filesystem>
#include <fstream>
#include <string>

using namespace pulp::format::reload;
namespace fs = std::filesystem;

namespace {
fs::path make_bundle() { return pulp::test::unique_tmp_dir("pulp-bundle-"); }
void write(const fs::path& p, std::string_view s) {
    fs::create_directories(p.parent_path());
    std::ofstream o(p, std::ios::binary);
    o.write(s.data(), static_cast<std::streamsize>(s.size()));
}
}  // namespace

TEST_CASE("build_signable_manifest hashes files + infers capabilities from the JS",
          "[reload][pack-build]") {
    auto b = make_bundle();
    write(b / "ui.js", "img.setImageSource('a.png'); pulp.exec('x');");
    write(b / "theme.json", "{\"bg\":\"#000\"}");

    auto r = build_signable_manifest(b, "com.pulp.demo", 3, "stable");
    REQUIRE(r.ok);
    REQUIRE(r.manifest.plugin_id == "com.pulp.demo");
    REQUIRE(r.manifest.pack_version == 3u);
    REQUIRE(r.manifest.update_channel == "stable");
    REQUIRE(r.manifest.files.size() == 2);
    // Capabilities inferred from the JS: setImageSource → filesystem, exec → exec.
    REQUIRE(r.manifest.declared_capabilities ==
            std::vector<std::string>{"exec", "filesystem"});
    for (const auto& f : r.manifest.files) {
        REQUIRE_FALSE(f.sha256_hex.empty());
        REQUIRE(f.path.find('\\') == std::string::npos);  // POSIX separators
    }
    fs::remove_all(b);
}

TEST_CASE("build_signable_manifest is deterministic (sorted files, stable content)",
          "[reload][pack-build]") {
    auto b = make_bundle();
    write(b / "b.js", "createKnob('g',0,0,1,1);");
    write(b / "a.js", "ctx.fillRect(0,0,1,1);");
    auto r1 = build_signable_manifest(b, "p", 1, "");
    auto r2 = build_signable_manifest(b, "p", 1, "");
    REQUIRE(r1.ok);
    REQUIRE(r1.manifest.files.size() == 2);
    REQUIRE(r1.manifest.files[0].path == "a.js");  // sorted
    REQUIRE(r1.manifest.files[1].path == "b.js");
    REQUIRE(r1.manifest.declared_capabilities.empty());  // pure UI → none
    REQUIRE(swap_pack_signed_message(r1.manifest) == swap_pack_signed_message(r2.manifest));
    fs::remove_all(b);
}

TEST_CASE("build_signable_manifest rejects a symlinked bundle file",
          "[reload][pack-build]") {
    auto b = make_bundle();
    write(b / "real.js", "ctx;");
    std::error_code ec;
    fs::create_symlink(b / "real.js", b / "link.js", ec);
    if (ec) { SUCCEED("platform has no symlinks; skipping"); return; }
    auto r = build_signable_manifest(b, "p", 1, "");
    REQUIRE_FALSE(r.ok);
    REQUIRE(r.error.find("symlink") != std::string::npos);
    fs::remove_all(b);
}
