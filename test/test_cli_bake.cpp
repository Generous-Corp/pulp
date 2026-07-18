// End-to-end CLI test for `pulp bake` / `pulp bake verify`: shell out to the built
// binary, bake a real .pulpgraph into a signed .pulpbake, and prove verify accepts the
// trusted artifact and rejects tampering / an untrusted signer.

#include "test_cli_shellout_util.hpp"

#include <catch2/catch_test_macros.hpp>

#include <pulp/host/graph_serializer.hpp>
#include <pulp/host/signal_graph.hpp>
#include <pulp/platform/child_process.hpp>

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {
const char* kCliBin = PULP_CLI_BIN;
const char* kRepoRoot = PULP_REPO_ROOT;

// Run the CLI directly — no shell in between.
//
// This used to compose a `cd '<root>' && <cmd> 2>&1` string for popen(), which is
// POSIX-only twice over: MSVC has neither `popen` nor `WEXITSTATUS`, and cmd.exe
// does not treat '...' as quoting, so single-quoted paths would have reached the
// binary as literal apostrophes. Handing argv to ChildProcess (posix_spawn /
// CreateProcess) skips the shell and its quoting rules entirely, so a path with
// spaces is safe on every platform.
std::string run_cli(const std::vector<std::string>& args, int& code) {
    pulp::platform::ProcessOptions options;
    options.working_directory = kRepoRoot;
    options.timeout_ms = pulp_test_cli::shellout_timeout_ms();  // shared hang guard
    const auto r = pulp::platform::ChildProcess::run(kCliBin, args, options);
    code = r.timed_out ? -1 : r.exit_code;
    return r.stdout_output + r.stderr_output;  // callers grep the merged stream
}

// A minimal lowerable graph: input -> gain -> output. Serialize it to a .pulpgraph.
fs::path write_gain_graph(const fs::path& dir) {
    pulp::host::SignalGraph g;
    const auto in = g.add_input_node(1, "In");
    const auto gain = g.add_gain_node("G");
    const auto out = g.add_output_node(1, "Out");
    g.set_node_gain(gain, 0.5f);
    (void)g.connect(in, 0, gain, 0);
    (void)g.connect(gain, 0, out, 0);
    fs::create_directories(dir);
    fs::path p = dir / "graph.pulpgraph";
    std::ofstream(p) << pulp::host::GraphSerializer::to_json(g);
    return p;
}

fs::path unique_dir() {
    static int n = 0;
    fs::path d = fs::temp_directory_path() / ("pulp-cli-bake-" + std::to_string(++n));
    fs::remove_all(d);
    fs::create_directories(d);
    return d;
}
}  // namespace

TEST_CASE("pulp bake signs a graph and verify accepts it under trust", "[cli][bake]") {
    const fs::path d = unique_dir();
    const fs::path graph = write_gain_graph(d);
    const fs::path out = d / "out.pulpbake";
    const fs::path key = d / "signing.key";  // minted on first use by load_or_generate_key_file

    int code = 0;
    const std::string baked = run_cli(
        {"bake", graph.string(), "-o", out.string(), "--sign-key", key.string()}, code);
    INFO(baked);
    REQUIRE(code == 0);
    REQUIRE(baked.find("signer public key") != std::string::npos);
    REQUIRE(fs::exists(out));

    const std::string verified =
        run_cli({"bake", "verify", out.string(), "--trust", key.string()}, code);
    INFO(verified);
    REQUIRE(code == 0);
    REQUIRE(verified.find("ACCEPTED") != std::string::npos);
}

TEST_CASE("pulp bake verify rejects tampering and an untrusted signer", "[cli][bake]") {
    const fs::path d = unique_dir();
    const fs::path graph = write_gain_graph(d);
    const fs::path out = d / "out.pulpbake";
    const fs::path key = d / "signing.key";
    const fs::path otherkey = d / "other.key";

    int code = 0;
    run_cli({"bake", graph.string(), "-o", out.string(), "--sign-key", key.string()}, code);
    REQUIRE(code == 0);

    SECTION("untrusted signer (verify under a different key) -> REJECTED") {
        // Mint an unrelated key by baking to a throwaway with it.
        run_cli({"bake", graph.string(), "-o", (d / "throwaway.pulpbake").string(),
                 "--sign-key", otherkey.string()}, code);
        const std::string v =
            run_cli({"bake", "verify", out.string(), "--trust", otherkey.string()}, code);
        INFO(v);
        REQUIRE(code != 0);
        REQUIRE(v.find("REJECTED") != std::string::npos);
    }
    SECTION("tampered artifact -> REJECTED") {
        std::fstream f(out, std::ios::in | std::ios::out | std::ios::binary);
        f.seekp(-1, std::ios::end);
        char last = 0;
        f.seekg(-1, std::ios::end);
        f.read(&last, 1);
        last ^= 0xFF;
        f.seekp(-1, std::ios::end);
        f.write(&last, 1);
        f.close();
        const std::string v =
            run_cli({"bake", "verify", out.string(), "--trust", key.string()}, code);
        INFO(v);
        REQUIRE(code != 0);
        REQUIRE(v.find("REJECTED") != std::string::npos);
    }
}

TEST_CASE("pulp bake fails cleanly on a missing input", "[cli][bake]") {
    int code = 0;
    const std::string out = run_cli({"bake", "/nonexistent/nope.pulpgraph", "-o",
                                     "/tmp/x.pulpbake", "--sign-key", "/tmp/k"}, code);
    INFO(out);
    REQUIRE(code != 0);
}

TEST_CASE("pulp bake enforces arg guards, --force, --no-mint, and empty-trust reject",
          "[cli][bake]") {
    const fs::path d = unique_dir();
    const fs::path graph = write_gain_graph(d);
    const fs::path out = d / "out.pulpbake";
    const fs::path key = d / "signing.key";
    int code = 0;

    SECTION("malformed --sr / --block -> exit 2, no crash") {
        const std::string a = run_cli({"bake", graph.string(), "-o", out.string(),
                                       "--sign-key", key.string(), "--sr", "not-a-number"}, code);
        INFO(a);
        REQUIRE(code == 2);
        const std::string b = run_cli({"bake", graph.string(), "-o", out.string(),
                                       "--sign-key", key.string(), "--block", "zero"}, code);
        INFO(b);
        REQUIRE(code == 2);
    }
    SECTION("--no-mint with an absent key -> exit 2 (no silent identity mint)") {
        const fs::path absent = d / "does-not-exist.key";
        const std::string a = run_cli({"bake", graph.string(), "-o", out.string(),
                                       "--sign-key", absent.string(), "--no-mint"}, code);
        INFO(a);
        REQUIRE(code == 2);
        REQUIRE_FALSE(fs::exists(absent));  // did NOT mint
    }
    SECTION("--force guard") {
        run_cli({"bake", graph.string(), "-o", out.string(), "--sign-key", key.string()}, code);
        REQUIRE(code == 0);
        // Second bake without --force is refused.
        run_cli({"bake", graph.string(), "-o", out.string(), "--sign-key", key.string()}, code);
        REQUIRE(code != 0);
        // With --force it succeeds.
        run_cli({"bake", graph.string(), "-o", out.string(), "--sign-key", key.string(),
                 "--force"}, code);
        REQUIRE(code == 0);
    }
    SECTION("verify with NO --trust -> REJECTED (no anchor = reject)") {
        run_cli({"bake", graph.string(), "-o", out.string(), "--sign-key", key.string()}, code);
        REQUIRE(code == 0);
        const std::string v =
            run_cli({"bake", "verify", out.string()}, code);
        INFO(v);
        REQUIRE(code != 0);
        REQUIRE(v.find("REJECTED") != std::string::npos);
    }
}
