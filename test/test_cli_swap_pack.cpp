// End-to-end CLI test for `pulp ship swap-pack`: shell out to the built binary,
// confirm it prints the content-aware summary with auto-inferred capabilities and
// writes a manifest that verifies under the generated key — and that without --yes
// (empty stdin) it refuses to sign.
#include <catch2/catch_test_macros.hpp>

#include <pulp/format/reload/swap_pack.hpp>
#include <pulp/format/reload/key_store.hpp>
#include <pulp/platform/child_process.hpp>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>


using namespace pulp::format::reload;

namespace fs = std::filesystem;

namespace {
const char* kCliBin = PULP_CLI_BIN;      // $<TARGET_FILE:pulp-cli>
const char* kRepoRoot = PULP_REPO_ROOT;  // find_project_root() needs a project cwd

// Run the CLI directly -- no shell in between.
//
// This used to compose a `cd '<root>' && <cmd> 2>&1` string for popen(), which is
// POSIX-only twice over: MSVC has no `popen`, and cmd.exe does not treat '...' as
// quoting, so single-quoted paths would have reached the binary as literal
// apostrophes. Handing argv to ChildProcess (posix_spawn / CreateProcess) skips
// the shell and its quoting rules entirely.
//
// The child inherits this process's stdin, which under ctest is not a terminal
// and reads EOF immediately -- which is what the no-confirmation case below
// needs (it used to spell that `printf '' |`).
std::string run_cli(const std::vector<std::string>& args, int& code) {
    pulp::platform::ProcessOptions options;
    options.working_directory = kRepoRoot;
    options.timeout_ms = 60000;
    const auto r = pulp::platform::ChildProcess::run(kCliBin, args, options);
    code = r.timed_out ? -1 : r.exit_code;
    return r.stdout_output + r.stderr_output;
}

fs::path make_bundle() {
    static int n = 0;
    fs::path d = fs::temp_directory_path() / ("pulp-cli-pack-" + std::to_string(++n));
    fs::remove_all(d);
    fs::create_directories(d);
    std::ofstream(d / "ui.js") << "img.setImageSource('a.png'); pulp.exec('ls');";
    return d;
}
std::string read_file(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}
}  // namespace

TEST_CASE("pulp ship swap-pack signs a bundle with inferred capabilities", "[cli][ship][swap-pack]") {
    const fs::path bundle = make_bundle();
    const fs::path key = bundle.parent_path() / (bundle.filename().string() + ".key");
    fs::remove(key);
    const fs::path manifest = bundle / "swap-pack.manifest.json";

    int code = 0;
    const std::string out = run_cli({"ship", "swap-pack", "--bundle", bundle.string(),
                                     "--plugin-id", "com.pulp.clitest", "--pack-version", "4",
                                     "--sign-key", key.string(), "--yes"}, code);

    REQUIRE(code == 0);
    // Content-aware summary with capabilities inferred from the JS.
    REQUIRE(out.find("About to sign swap pack") != std::string::npos);
    REQUIRE(out.find("exec, filesystem") != std::string::npos);
    // A fresh key screams.
    REQUIRE(out.find("back it up") != std::string::npos);

    // The written manifest parses and VERIFIES under the generated key.
    REQUIRE(fs::exists(manifest));
    std::string err;
    auto m = parse_swap_pack_manifest(read_file(manifest), err);
    REQUIRE(m.has_value());
    REQUIRE(m->plugin_id == "com.pulp.clitest");
    REQUIRE(m->pack_version == 4u);
    REQUIRE(m->declared_capabilities == std::vector<std::string>{"exec", "filesystem"});
    auto km = parse_key_blob(read_file(key));
    REQUIRE(km.has_value());
    REQUIRE(verify_swap_pack(bundle, *m, km->public_key).ok());

    fs::remove_all(bundle);
    fs::remove(key);
}

TEST_CASE("pulp ship swap-pack --backup-github refuses the core Pulp repo",
          "[cli][ship][swap-pack]") {
    const fs::path bundle = make_bundle();
    const fs::path key = bundle.parent_path() / (bundle.filename().string() + "-bk.key");
    fs::remove(key);
    int code = 0;
    const std::string out = run_cli({"ship", "swap-pack", "--bundle", bundle.string(),
                                     "--plugin-id", "com.pulp.clitest",
                                     "--sign-key", key.string(), "--backup-github",
                                     "--repo", "Generous-Corp/pulp", "--yes"}, code);
    // The pack signs, but publishing the key to the core repo is refused (no gh call).
    REQUIRE(code != 0);
    REQUIRE(out.find("refusing to back up") != std::string::npos);
    REQUIRE(fs::exists(bundle / "swap-pack.manifest.json"));  // signing itself succeeded
    fs::remove_all(bundle);
    fs::remove(key);
}

TEST_CASE("pulp ship swap-pack refuses to sign without confirmation", "[cli][ship][swap-pack]") {
    const fs::path bundle = make_bundle();
    const fs::path key = bundle.parent_path() / (bundle.filename().string() + "-noyes.key");
    fs::remove(key);
    const fs::path manifest = bundle / "swap-pack.manifest.json";

    // No --yes and empty stdin → the confirm gate blocks; nothing is signed/written.
    int code = 0;
    const std::string out = run_cli({"ship", "swap-pack", "--bundle", bundle.string(),
                                     "--plugin-id", "com.pulp.clitest",
                                     "--sign-key", key.string()}, code);
    REQUIRE(code != 0);
    REQUIRE_FALSE(fs::exists(manifest));
    REQUIRE_FALSE(fs::exists(key));  // no key generated when we never got to signing
    fs::remove_all(bundle);
}
