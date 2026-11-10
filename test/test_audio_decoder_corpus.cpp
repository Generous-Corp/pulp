// Deterministic replay of the audio decoder seed corpus through the oracle the
// libFuzzer target uses. This is the lane that runs on the required gate, and
// the only lane that reaches the platform CoreAudio reader, which the fuzzer
// excludes. It also holds the WAV path differential: the registry's file
// reader and the byte-span decoder apply one acceptance policy and must agree
// on every seed, bit for bit where they decode.
// approach after SimZhou/vscode-audiolens c5c5a24; re-authored

#include "fuzz/audio_decoder_fuzz_oracle.hpp"
#include "support/audio_decoder_corpus.hpp"
#include "support/audio_decoder_corpus_expectations.hpp"
#include "support/fixture_root.hpp"
#include "support/unique_temp_dir.hpp"

#include <catch2/catch_test_macros.hpp>
#include <pulp/audio/audio_file.hpp>
#include <pulp/audio/audio_reader.hpp>
#include <pulp/audio/format_registry.hpp>
#include <pulp/audio/wav_decoder.hpp>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace fs = std::filesystem;
using namespace pulp_test::audio_decoder_corpus;
using pulp_fuzz::audio_decoder::OracleFailure;

TEST_CASE("audio decoder corpus holds a seed for every fuzzable format", "[fuzz][audio]") {
    const auto files = corpus_files();
    INFO("corpus directory: " << corpus_dir().string());
    REQUIRE_FALSE(files.empty());

    std::set<std::string> present;
    for (const auto& file : files)
        present.insert(lower_extension(file));

    for (const auto& format : pulp_fuzz::audio_decoder::kFuzzableFormats) {
        const bool found = std::any_of(
            format.extensions.begin(), format.extensions.end(), [&](std::string_view ext) {
                return !ext.empty() && present.count(std::string(ext)) > 0;
            });
        INFO("format without a seed: " << format.name);
        CHECK(found);
    }
}

TEST_CASE("every audio decoder seed satisfies the shared oracle through the registry",
          "[fuzz][audio]") {
    const auto files = corpus_files();
    REQUIRE_FALSE(files.empty());

    auto& registry = pulp::audio::FormatRegistry::instance();
    const auto limits = pulp_fuzz::audio_decoder::fuzz_limits();

    std::vector<std::string> failures;
    std::size_t exercised = 0;
    std::vector<std::string> unclaimed;
    for (const auto& file : files) {
        const auto ext = lower_extension(file);
        const auto name = file.filename().string();
        if (registry.find_reader(ext) == nullptr) {
            // CoreAudio-only seeds have no reader off Apple; on Apple every
            // committed seed must reach a reader.
            unclaimed.push_back(name);
            continue;
        }
        ++exercised;
        const auto outcome = pulp_fuzz::audio_decoder::check_registry(
            file.string(), limits,
            [&](const OracleFailure& failure) { failures.push_back(describe(failure)); });
        const auto row = seed_expectations().find(name);
        if (row == seed_expectations().end()) {
            failures.push_back(name + ": no row in seed_expectations()");
        } else if (row->second.must_decode && (!outcome.info_ok || !outcome.decode_ok)) {
            failures.push_back(name + ": valid seed, read_info " +
                               (outcome.info_ok ? "ok" : "refused") + ", read " +
                               (outcome.decode_ok ? "ok" : "refused"));
        } else if (row->second.truncated && outcome.info &&
                   outcome.info->truncated != *row->second.truncated) {
            failures.push_back(name + ": read_info reports truncated=" +
                               (outcome.info->truncated ? "true" : "false") + ", expected " +
                               (*row->second.truncated ? "true" : "false"));
        }
    }

#if defined(__APPLE__)
    INFO("seeds no registered reader claims: " << unclaimed.size());
    CHECK(unclaimed.empty());
#endif
    REQUIRE(exercised > 0);
    for (const auto& failure : failures)
        FAIL_CHECK(failure);
}
