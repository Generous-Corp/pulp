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

#if defined(_WIN32)
#include <process.h>
#else
#include <unistd.h>
#endif

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

long long current_process_id() {
#if defined(_WIN32)
    return static_cast<long long>(::_getpid());
#else
    return static_cast<long long>(::getpid());
#endif
}

#if !defined(PULP_FIXTURE_RUNNER_BINARY)
// Without the binary path every case below would early-SUCCEED and the whole
// gate-contract suite would report green while testing nothing. Refuse to build
// a suite that cannot run rather than ship a silent pass.
#error "PULP_FIXTURE_RUNNER_BINARY must be defined; see test/cmake/timeline_tests.cmake"
#endif

fs::path runner_binary() { return fs::path(PULP_FIXTURE_RUNNER_BINARY); }

#if !defined(PULP_TIMELINE_CORPUS_DIR)
#error "PULP_TIMELINE_CORPUS_DIR must be defined; see test/cmake/timeline_tests.cmake"
#endif

/// The smallest valid project envelope: one empty root sequence. Written
/// literally rather than copied from the corpus so a corpus edit can never
/// silently change what these cases assert.
constexpr const char* kMinimalProject =
    R"({"data":{"assets":[],"id":"1","name":"cli-fixture","next_item_id":"3",)"
    R"("root_sequence_id":"2","sequences":[{"data":{"absolute_duration":null,)"
    R"("id":"2","musical_duration":"0","name":"root","tracks":[]},)"
    R"("type_name":"pulp.timeline.sequence","version":1}]},)"
    R"("type_name":"pulp.timeline.project","version":1})";

/// A project whose authored track order is deliberately NOT the order its
/// tracks are stored in: `track_order` names 12, 9, 3 while `tracks` holds
/// 9, 3, 12. Written literally for the same reason as kMinimalProject, and
/// because the difference between those two orders is the entire property the
/// order cases below assert.
constexpr const char* kTrackOrderProject =
    R"JSON({"data":{"assets":[],"id":"1","identities":[{"active":true,"clip_id":"0","id":"1","kind":"project","parent_id":"0","sequence_id":"0","track_id":"0"},{"active":true,"clip_id":"0","id":"2","kind":"sequence","parent_id":"1","sequence_id":"2","track_id":"0"},{"active":true,"clip_id":"0","id":"3","kind":"track","parent_id":"2","sequence_id":"2","track_id":"3"},{"active":true,"clip_id":"0","id":"9","kind":"track","parent_id":"2","sequence_id":"2","track_id":"9"},{"active":true,"clip_id":"0","id":"12","kind":"track","parent_id":"2","sequence_id":"2","track_id":"12"}],"meter_map":[{"denominator":4,"numerator":4,"tick":"0"}],"name":"track order","next_item_id":"13","root_sequence_id":"2","sequences":[{"data":{"absolute_duration":null,"chord_scale_lane":[],"groove":{"name":"","step":"0","steps":[],"swing_denominator":"2","swing_grid":"0","swing_numerator":"1","timing_strength":1000,"velocity_strength":1000},"id":"2","markers":[],"musical_duration":"7680","name":"root","regions":[],"scenes":[],"track_order":["12","9","3"],"tracks":[{"data":{"active_take_lane_id":"0","automation_lanes":[],"clips":[],"device_chain":[],"id":"9","name":"bass","record_armed":false,"take_lanes":[]},"type_name":"pulp.timeline.track","version":7},{"data":{"active_take_lane_id":"0","automation_lanes":[],"clips":[],"device_chain":[],"id":"3","name":"drums","record_armed":false,"take_lanes":[]},"type_name":"pulp.timeline.track","version":7},{"data":{"active_take_lane_id":"0","automation_lanes":[],"clips":[],"device_chain":[],"id":"12","name":"keys","record_armed":false,"take_lanes":[]},"type_name":"pulp.timeline.track","version":7}]},"type_name":"pulp.timeline.sequence","version":6}],"tempo_map":[{"bpm_bits":"4638144666238189568","curve":"constant","tick":"0"}]},"type_name":"pulp.timeline.project","version":2})JSON";

/// Returns the value of `key` in a manifest, or empty when absent.
std::string manifest_value(const std::string& manifest, const std::string& key) {
    const auto line_start = manifest.find("\n" + key + " ");
    if (line_start == std::string::npos)
        return {};
    const auto value_start = line_start + key.size() + 2;
    const auto line_end = manifest.find('\n', value_start);
    return manifest.substr(value_start, line_end == std::string::npos ? line_end
                                                                     : line_end - value_start);
}

struct TempCorpus {
    fs::path root;

    TempCorpus() {
        // catch_discover_tests registers every case as its own ctest test, so
        // these run as concurrent processes under `ctest -j`. A clock-only name
        // can collide, and a collision means one case's destructor removes
        // another's corpus mid-run — an unreproducible red. The pid separates
        // processes; the clock separates cases within one.
        root = fs::temp_directory_path() /
               ("pulp-fixture-corpus-" + std::to_string(current_process_id()) + "-" +
                std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        std::error_code created;
        fs::create_directories(root / "v1", created);
        REQUIRE_FALSE(created);
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

} // namespace

TEST_CASE("fixture runner verifies a generated corpus", "[cli][fixture-runner]") {
    TempCorpus corpus;

    const auto generated = update(corpus);
    INFO("stdout=" << generated.stdout_output << " stderr=" << generated.stderr_output);
    REQUIRE(generated.exit_code == 0);
    REQUIRE_FALSE(corpus.read("v1/minimal.json.expect").empty());

    const auto verified = check(corpus);
    INFO("stdout=" << verified.stdout_output << " stderr=" << verified.stderr_output);
    REQUIRE(verified.exit_code == 0);
}

TEST_CASE("fixture runner verifies a document whose name carries surrounding whitespace",
          "[cli][fixture-runner]") {
    // A gate that raises a false failure on a legal document is as useless as one
    // that cannot fail at all. An authored name is arbitrary user text, and the
    // manifest is a whitespace-delimited text format, so recording the name made
    // a generated manifest fail against the very document it came from. The name
    // is no longer recorded; this pins that, since the failure is invisible until
    // a fixture happens to carry such a name.
    TempCorpus corpus;
    std::string spacey = kMinimalProject;
    const auto position = spacey.find(R"("name":"cli-fixture")");
    REQUIRE(position != std::string::npos);
    spacey.replace(position, std::string(R"("name":"cli-fixture")").size(),
                   R"("name":" padded name ")");
    corpus.write("v1/minimal.json", spacey);

    REQUIRE(update(corpus).exit_code == 0);
    const auto result = check(corpus);
    INFO("stdout=" << result.stdout_output << " stderr=" << result.stderr_output);
    REQUIRE(result.exit_code == 0);
}

TEST_CASE("every file in the real corpus is listed in the index", "[cli][fixture-runner]") {
    // corpus.index claims an explicit index turns an unreferenced fixture into a
    // visible omission. Nothing in the portable runner can enforce that — it
    // deliberately does not walk directories, because directory iteration is
    // unreliable in the WASM and emulator lanes it exists to serve. So the sweep
    // lives here, on the host, where std::filesystem is available. Without it the
    // claim is prose asserting behaviour the code does not have, and a fixture
    // added but never indexed is invisible to every gate.
    const fs::path corpus(PULP_TIMELINE_CORPUS_DIR);
    REQUIRE(fs::is_directory(corpus));

    std::vector<std::string> indexed;
    {
        std::ifstream index(corpus / "corpus.index", std::ios::binary);
        REQUIRE(index.good());
        std::string line;
        while (std::getline(index, line)) {
            while (!line.empty() && (line.back() == '\r' || line.back() == ' '))
                line.pop_back();
            if (line.empty() || line.front() == '#')
                continue;
            const auto space = line.find(' ');
            REQUIRE(space != std::string::npos);
            indexed.push_back(line.substr(space + 1));
        }
    }
    REQUIRE_FALSE(indexed.empty());

    std::vector<std::string> unlisted;
    for (const auto& entry : fs::recursive_directory_iterator(corpus)) {
        if (!entry.is_regular_file())
            continue;
        const auto relative = fs::relative(entry.path(), corpus).generic_string();
        // Generated manifests are derived from their document, and the index
        // itself is not a corpus member.
        if (relative == "corpus.index" || relative.ends_with(".expect"))
            continue;
        if (std::find(indexed.begin(), indexed.end(), relative) == indexed.end())
            unlisted.push_back(relative);
    }

    INFO("unlisted: " << [&] {
        std::string joined;
        for (const auto& path : unlisted)
            joined += path + " ";
        return joined;
    }());
    REQUIRE(unlisted.empty());
}

TEST_CASE("fixture runner rejects an index entry naming a missing file",
          "[cli][fixture-runner]") {
    // fragment and payload entries are declared but not validated. If nothing
    // opened them, renaming or deleting one would degrade the index into a
    // comment while the run stayed green — the exact blindness the index exists
    // to prevent.
    TempCorpus corpus;
    REQUIRE(update(corpus).exit_code == 0);
    corpus.append("corpus.index", "payload v1/does-not-exist.bin\n");

    const auto result = check(corpus);
    REQUIRE(result.exit_code == 1);
    INFO("stderr=" << result.stderr_output);
    REQUIRE(result.stderr_output.find("missing payload") != std::string::npos);
}

TEST_CASE("fixture runner rejects a corpus with no manifest", "[cli][fixture-runner]") {
    TempCorpus corpus;

    const auto result = check(corpus);
    REQUIRE(result.exit_code == 1);
    REQUIRE(result.stderr_output.find("cannot read manifest") != std::string::npos);
}

TEST_CASE("fixture runner rejects a structural count that drifted", "[cli][fixture-runner]") {
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

TEST_CASE("fixture runner rejects an authored order that collapsed to identity order",
          "[cli][fixture-runner]") {
    // The regression this guards is not a lost entity but a lost *ordering*. A
    // decoder that drops an authored track order keeps every count intact and
    // re-serializes consistently, so counts, census, and idempotence all still
    // agree — the sequence just silently adopts the identity order of its
    // tracks. Only comparing the recorded order can see it.
    TempCorpus corpus;
    REQUIRE(fs::create_directories(corpus.root / "v6"));
    corpus.write("v6/track-order.json", kTrackOrderProject);
    corpus.write("corpus.index", "document v6/track-order.json\n");
    REQUIRE(update(corpus).exit_code == 0);

    auto manifest = corpus.read("v6/track-order.json.expect");
    const auto authored = std::string("order.sequence.2.track_order 12,9,3");
    const auto position = manifest.find(authored);
    REQUIRE(position != std::string::npos);
    manifest.replace(position, authored.size(), "order.sequence.2.track_order 9,3,12");
    corpus.write("v6/track-order.json.expect", manifest);

    const auto result = check(corpus);
    REQUIRE(result.exit_code == 1);
    INFO("stderr=" << result.stderr_output);
    REQUIRE(result.stderr_output.find(
                "order.sequence.2.track_order: expected 9,3,12, got 12,9,3") != std::string::npos);
}

TEST_CASE("shipped corpus records an authored order that differs from identity order",
          "[cli][fixture-runner]") {
    // An order assertion only detects a collapse while the two orders actually
    // differ. `--update` regenerates manifests from observed output without
    // comparing, so regenerating against a regressed decoder would bake the
    // identity order into both keys and leave a gate that still runs and can no
    // longer fail. Assert the difference in the shipped manifest directly, so
    // that collapse is a red test rather than a quiet one.
    const fs::path manifest_path =
        fs::path(PULP_TIMELINE_CORPUS_DIR) / "v6" / "sequence-track-order.json.expect";
    std::ifstream stream(manifest_path, std::ios::binary);
    REQUIRE(stream.good());
    const std::string manifest((std::istreambuf_iterator<char>(stream)),
                               std::istreambuf_iterator<char>());

    const auto identity = manifest_value(manifest, "order.sequence.2.tracks");
    const auto authored = manifest_value(manifest, "order.sequence.2.track_order");
    REQUIRE_FALSE(identity.empty());
    REQUIRE_FALSE(authored.empty());
    REQUIRE(identity != authored);
}

TEST_CASE("fixture runner rejects a manifest entry with no observed value",
          "[cli][fixture-runner]") {
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
    TempCorpus corpus;
    REQUIRE(update(corpus).exit_code == 0);
    corpus.append("corpus.index", "bogus v1/minimal.json\n");

    const auto result = check(corpus);
    REQUIRE(result.exit_code == 1);
    REQUIRE(result.stderr_output.find("unknown kind") != std::string::npos);
}

TEST_CASE("fixture runner rejects an index entry with no kind", "[cli][fixture-runner]") {
    TempCorpus corpus;
    REQUIRE(update(corpus).exit_code == 0);
    corpus.append("corpus.index", "v1/minimal.json\n");

    const auto result = check(corpus);
    REQUIRE(result.exit_code == 1);
    REQUIRE(result.stderr_output.find("missing a kind") != std::string::npos);
}

TEST_CASE("fixture runner reports a missing corpus rather than passing vacuously",
          "[cli][fixture-runner]") {
    const auto result = run_runner({"--corpus", (fs::temp_directory_path() / "pulp-no-such-corpus")
                                                    .string()});
    REQUIRE(result.exit_code == 1);
    REQUIRE(result.stderr_output.find("cannot read corpus index") != std::string::npos);
}

TEST_CASE("fixture runner treats a missing --corpus as a usage error", "[cli][fixture-runner]") {
    // Exit 2 distinguishes "you invoked me wrong" from "the corpus is broken",
    // so a CI misconfiguration can never be read as a conformance failure.
    REQUIRE(run_runner({}).exit_code == 2);
}
