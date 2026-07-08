// End-to-end CLI test for `pulp bake` / `pulp bake verify`: shell out to the built
// binary, bake a real .pulpgraph into a signed .pulpbake, and prove verify accepts the
// trusted artifact and rejects tampering / an untrusted signer.

#include <catch2/catch_test_macros.hpp>

#include <pulp/host/graph_serializer.hpp>
#include <pulp/host/signal_graph.hpp>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

namespace fs = std::filesystem;

namespace {
const char* kCliBin = PULP_CLI_BIN;
const char* kRepoRoot = PULP_REPO_ROOT;

std::string run_capture(const std::string& cmd, int& code) {
    std::string full = "cd '" + std::string(kRepoRoot) + "' && " + cmd + " 2>&1";
    FILE* p = popen(full.c_str(), "r");
    std::string out;
    if (p) {
        char buf[512];
        while (fgets(buf, sizeof(buf), p)) out += buf;
        int raw = pclose(p);
        code = WIFEXITED(raw) ? WEXITSTATUS(raw) : -1;
    } else {
        code = -1;
    }
    return out;
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
    const std::string baked = run_capture(
        std::string(kCliBin) + " bake '" + graph.string() + "' -o '" + out.string() +
            "' --sign-key '" + key.string() + "'",
        code);
    INFO(baked);
    REQUIRE(code == 0);
    REQUIRE(baked.find("signer public key") != std::string::npos);
    REQUIRE(fs::exists(out));

    const std::string verified = run_capture(
        std::string(kCliBin) + " bake verify '" + out.string() + "' --trust '" + key.string() + "'",
        code);
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
    run_capture(std::string(kCliBin) + " bake '" + graph.string() + "' -o '" + out.string() +
                    "' --sign-key '" + key.string() + "'",
                code);
    REQUIRE(code == 0);

    SECTION("untrusted signer (verify under a different key) -> REJECTED") {
        // Mint an unrelated key by baking to a throwaway with it.
        run_capture(std::string(kCliBin) + " bake '" + graph.string() + "' -o '" +
                        (d / "throwaway.pulpbake").string() + "' --sign-key '" + otherkey.string() + "'",
                    code);
        const std::string v = run_capture(std::string(kCliBin) + " bake verify '" + out.string() +
                                              "' --trust '" + otherkey.string() + "'",
                                          code);
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
        const std::string v = run_capture(std::string(kCliBin) + " bake verify '" + out.string() +
                                              "' --trust '" + key.string() + "'",
                                          code);
        INFO(v);
        REQUIRE(code != 0);
        REQUIRE(v.find("REJECTED") != std::string::npos);
    }
}

TEST_CASE("pulp bake fails cleanly on a missing input", "[cli][bake]") {
    int code = 0;
    const std::string out = run_capture(
        std::string(kCliBin) + " bake /nonexistent/nope.pulpgraph -o /tmp/x.pulpbake --sign-key /tmp/k",
        code);
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
        const std::string a = run_capture(
            std::string(kCliBin) + " bake '" + graph.string() + "' -o '" + out.string() +
                "' --sign-key '" + key.string() + "' --sr not-a-number",
            code);
        INFO(a);
        REQUIRE(code == 2);
        const std::string b = run_capture(
            std::string(kCliBin) + " bake '" + graph.string() + "' -o '" + out.string() +
                "' --sign-key '" + key.string() + "' --block zero",
            code);
        INFO(b);
        REQUIRE(code == 2);
    }
    SECTION("--no-mint with an absent key -> exit 2 (no silent identity mint)") {
        const fs::path absent = d / "does-not-exist.key";
        const std::string a = run_capture(
            std::string(kCliBin) + " bake '" + graph.string() + "' -o '" + out.string() +
                "' --sign-key '" + absent.string() + "' --no-mint",
            code);
        INFO(a);
        REQUIRE(code == 2);
        REQUIRE_FALSE(fs::exists(absent));  // did NOT mint
    }
    SECTION("--force guard") {
        run_capture(std::string(kCliBin) + " bake '" + graph.string() + "' -o '" + out.string() +
                        "' --sign-key '" + key.string() + "'",
                    code);
        REQUIRE(code == 0);
        // Second bake without --force is refused.
        run_capture(std::string(kCliBin) + " bake '" + graph.string() + "' -o '" + out.string() +
                        "' --sign-key '" + key.string() + "'",
                    code);
        REQUIRE(code != 0);
        // With --force it succeeds.
        run_capture(std::string(kCliBin) + " bake '" + graph.string() + "' -o '" + out.string() +
                        "' --sign-key '" + key.string() + "' --force",
                    code);
        REQUIRE(code == 0);
    }
    SECTION("verify with NO --trust -> REJECTED (no anchor = reject)") {
        run_capture(std::string(kCliBin) + " bake '" + graph.string() + "' -o '" + out.string() +
                        "' --sign-key '" + key.string() + "'",
                    code);
        REQUIRE(code == 0);
        const std::string v =
            run_capture(std::string(kCliBin) + " bake verify '" + out.string() + "'", code);
        INFO(v);
        REQUIRE(code != 0);
        REQUIRE(v.find("REJECTED") != std::string::npos);
    }
}
