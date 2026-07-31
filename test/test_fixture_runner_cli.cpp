// pulp-fixture-runner CLI contract: shells out to the built binary and asserts
// exit codes and stderr content, per CLAUDE.md's "CLI behavior changes" rule.
//
// The runner is a conformance gate, so the load-bearing property is not that it
// passes on a good corpus — it is that it FAILS on a bad one. A gate that
// cannot go red is indistinguishable from no gate at all, so every failure mode
// below is asserted against a deliberately broken corpus built for the case.
//
// Each case builds its own temporary corpus rather than touching
// test/fixtures/timeline, so the suite never mutates the real corpus and cases
// stay independent of one another.

#include <catch2/catch_test_macros.hpp>
#include <pulp/platform/child_process.hpp>

#include "test_cli_shellout_util.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

fs::path runner_binary() {
#if defined(PULP_FIXTURE_RUNNER_BINARY)
    return fs::path(PULP_FIXTURE_RUNNER_BINARY);
#else
    return {};
#endif
}

/// The smallest valid project envelope: one empty root sequence. Written
/// literally rather than copied from the corpus so a corpus edit can never
/// silently change what these cases assert.
constexpr const char* kMinimalProject =
    R"({"data":{"assets":[],"id":"1","name":"cli-fixture","next_item_id":"3",)"
    R"("root_sequence_id":"2","sequences":[{"data":{"absolute_duration":null,)"
    R"("id":"2","musical_duration":"0","name":"root","tracks":[]},)"
    R"("type_name":"pulp.timeline.sequence","version":1}]},)"
    R"("type_name":"pulp.timeline.project","version":1})";

struct TempCorpus {
    fs::path root;

    TempCorpus() {
        root = fs::temp_directory_path() /
               ("pulp-fixture-corpus-" +
                std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        fs::create_directories(root / "v1");
        write("v1/minimal.json", kMinimalProject);
        write("corpus.index", "document v1/minimal.json\n");
    }

    ~TempCorpus() {
        std::error_code ec;
        fs::remove_all(root, ec);
    }

    void write(const std::string& relative, const std::string& contents) const {
        std::ofstream stream(root / relative, std::ios::binary | std::ios::trunc);
        stream << contents;
    }

    void append(const std::string& relative, const std::string& contents) const {
        std::ofstream stream(root / relative, std::ios::binary | std::ios::app);
        stream << contents;
    }

    std::string read(const std::string& relative) const {
        std::ifstream stream(root / relative, std::ios::binary);
        return std::string((std::istreambuf_iterator<char>(stream)),
                           std::istreambuf_iterator<char>());
    }
};

pulp::platform::ProcessResult run_runner(const std::vector<std::string>& args) {
    pulp::platform::ProcessOptions options;
    options.timeout_ms = pulp_test_cli::shellout_timeout_ms();
    return pulp::platform::ChildProcess::run(runner_binary().string(), args, options);
}

pulp::platform::ProcessResult check(const TempCorpus& corpus) {
    return run_runner({"--corpus", corpus.root.string()});
}

pulp::platform::ProcessResult update(const TempCorpus& corpus) {
    return run_runner({"--corpus", corpus.root.string(), "--update"});
}

bool runner_available() { return !runner_binary().empty(); }

} // namespace

TEST_CASE("fixture runner verifies a generated corpus", "[cli][fixture-runner]") {
    if (!runner_available()) {
        SUCCEED("pulp-fixture-runner not built");
        return;
    }
    TempCorpus corpus;

    const auto generated = update(corpus);
    INFO("stdout=" << generated.stdout_output << " stderr=" << generated.stderr_output);
    REQUIRE(generated.exit_code == 0);
    REQUIRE_FALSE(corpus.read("v1/minimal.json.expect").empty());

    const auto verified = check(corpus);
    INFO("stdout=" << verified.stdout_output << " stderr=" << verified.stderr_output);
    REQUIRE(verified.exit_code == 0);
}

TEST_CASE("fixture runner rejects a corpus with no manifest", "[cli][fixture-runner]") {
    if (!runner_available()) {
        SUCCEED("pulp-fixture-runner not built");
        return;
    }
    TempCorpus corpus;

    const auto result = check(corpus);
    REQUIRE(result.exit_code == 1);
    REQUIRE(result.stderr_output.find("cannot read manifest") != std::string::npos);
}

TEST_CASE("fixture runner rejects a structural count that drifted", "[cli][fixture-runner]") {
    if (!runner_available()) {
        SUCCEED("pulp-fixture-runner not built");
        return;
    }
    TempCorpus corpus;
    REQUIRE(update(corpus).exit_code == 0);

    // Rewrite one declared count so it disagrees with the document. This is the
    // shape of the regression the corpus exists to catch: the document changed
    // and nobody noticed.
    auto manifest = corpus.read("v1/minimal.json.expect");
    const auto position = manifest.find("counts.sequences 1");
    REQUIRE(position != std::string::npos);
    manifest.replace(position, std::string("counts.sequences 1").size(), "counts.sequences 7");
    corpus.write("v1/minimal.json.expect", manifest);

    const auto result = check(corpus);
    REQUIRE(result.exit_code == 1);
    REQUIRE(result.stderr_output.find("counts.sequences: expected 7, got 1") !=
            std::string::npos);
}

TEST_CASE("fixture runner rejects a manifest entry with no observed value",
          "[cli][fixture-runner]") {
    if (!runner_available()) {
        SUCCEED("pulp-fixture-runner not built");
        return;
    }
    TempCorpus corpus;
    REQUIRE(update(corpus).exit_code == 0);

    // A concept the document does not carry. Without this direction of the
    // check, a document that LOST an entity would still pass, because every
    // observed value would still match.
    corpus.append("v1/minimal.json.expect", "concept.clip.warp 3\n");

    const auto result = check(corpus);
    REQUIRE(result.exit_code == 1);
    REQUIRE(result.stderr_output.find("declared in manifest but not observed") !=
            std::string::npos);
}

TEST_CASE("fixture runner rejects an index entry with an unknown kind",
          "[cli][fixture-runner]") {
    if (!runner_available()) {
        SUCCEED("pulp-fixture-runner not built");
        return;
    }
    TempCorpus corpus;
    REQUIRE(update(corpus).exit_code == 0);
    corpus.append("corpus.index", "bogus v1/minimal.json\n");

    const auto result = check(corpus);
    REQUIRE(result.exit_code == 1);
    REQUIRE(result.stderr_output.find("unknown kind") != std::string::npos);
}

TEST_CASE("fixture runner rejects an index entry with no kind", "[cli][fixture-runner]") {
    if (!runner_available()) {
        SUCCEED("pulp-fixture-runner not built");
        return;
    }
    TempCorpus corpus;
    REQUIRE(update(corpus).exit_code == 0);
    corpus.append("corpus.index", "v1/minimal.json\n");

    const auto result = check(corpus);
    REQUIRE(result.exit_code == 1);
    REQUIRE(result.stderr_output.find("missing a kind") != std::string::npos);
}

TEST_CASE("fixture runner reports a missing corpus rather than passing vacuously",
          "[cli][fixture-runner]") {
    if (!runner_available()) {
        SUCCEED("pulp-fixture-runner not built");
        return;
    }
    const auto result = run_runner({"--corpus", (fs::temp_directory_path() / "pulp-no-such-corpus")
                                                    .string()});
    REQUIRE(result.exit_code == 1);
    REQUIRE(result.stderr_output.find("cannot read corpus index") != std::string::npos);
}

TEST_CASE("fixture runner treats a missing --corpus as a usage error", "[cli][fixture-runner]") {
    if (!runner_available()) {
        SUCCEED("pulp-fixture-runner not built");
        return;
    }
    // Exit 2 distinguishes "you invoked me wrong" from "the corpus is broken",
    // so a CI misconfiguration can never be read as a conformance failure.
    REQUIRE(run_runner({}).exit_code == 2);
}
