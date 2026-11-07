// Deterministic replay of the audio decoder seed corpus through the oracle the
// libFuzzer target uses. This is the lane that runs on the required gate, and
// the only lane that reaches the platform CoreAudio reader, which the fuzzer
// excludes. It also holds the WAV differential: the registry's CHOC path and
// the byte-span dr_wav decoder must agree bit for bit on every seed.
// approach after SimZhou/vscode-audiolens c5c5a24; re-authored

#include "fuzz/audio_decoder_fuzz_oracle.hpp"
#include "support/fixture_root.hpp"

#include <catch2/catch_test_macros.hpp>
#include <pulp/audio/format_registry.hpp>
#include <pulp/audio/wav_decoder.hpp>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace fs = std::filesystem;
using pulp_fuzz::audio_decoder::OracleFailure;

fs::path corpus_dir() {
    return pulp_test::fixture_root() / "test" / "fixtures" / "fuzz" / "audio-decoder";
}

std::string lower_extension(const fs::path& path) {
    std::string ext = path.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return ext;
}

// Every file in the corpus directory except its README, sorted so a failure
// report is stable from run to run.
std::vector<fs::path> corpus_files() {
    std::vector<fs::path> files;
    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(corpus_dir(), ec)) {
        if (!entry.is_regular_file())
            continue;
        const auto name = entry.path().filename().string();
        if (name == "README.md" || name.empty() || name.front() == '.')
            continue;
        files.push_back(entry.path());
    }
    std::sort(files.begin(), files.end());
    return files;
}

// The extensions each fuzzable format may carry; a corpus missing any one of
// these formats is a mis-pathed or half-populated corpus, not a passing one.
const std::map<std::string, std::set<std::string>>& fuzzable_formats() {
    static const std::map<std::string, std::set<std::string>> formats = {
        {"wav", {".wav", ".wave"}},
        {"aiff", {".aiff", ".aif", ".aifc"}},
        {"flac", {".flac"}},
        {"mp3", {".mp3"}},
        {"ogg", {".ogg", ".oga"}},
    };
    return formats;
}

std::vector<uint8_t> read_bytes(const fs::path& path) {
    std::ifstream input(path, std::ios::binary);
    REQUIRE(input.good());
    return std::vector<uint8_t>(std::istreambuf_iterator<char>(input), {});
}

std::string describe(const OracleFailure& failure) {
    return fs::path(failure.path).filename().string() + ": " + failure.field + ": " +
           failure.detail;
}

} // namespace

TEST_CASE("audio decoder corpus holds a seed for every fuzzable format", "[fuzz][audio]") {
    const auto files = corpus_files();
    INFO("corpus directory: " << corpus_dir().string());
    REQUIRE_FALSE(files.empty());

    std::set<std::string> present;
    for (const auto& file : files)
        present.insert(lower_extension(file));

    for (const auto& [format, extensions] : fuzzable_formats()) {
        const bool found = std::any_of(extensions.begin(), extensions.end(),
                                       [&](const std::string& ext) { return present.count(ext) > 0; });
        INFO("format without a seed: " << format);
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
        if (registry.find_reader(ext) == nullptr) {
            // CoreAudio-only seeds have no reader off Apple; on Apple every
            // committed seed must reach a reader.
            unclaimed.push_back(file.filename().string());
            continue;
        }
        ++exercised;
        (void)pulp_fuzz::audio_decoder::check_registry(
            file.string(), limits,
            [&](const OracleFailure& failure) { failures.push_back(describe(failure)); });
    }

#if defined(__APPLE__)
    INFO("seeds no registered reader claims: " << unclaimed.size());
    CHECK(unclaimed.empty());
#endif
    REQUIRE(exercised > 0);
    for (const auto& failure : failures)
        FAIL_CHECK(failure);
}

TEST_CASE("WAV seeds decode identically through the registry and the byte-span decoder",
          "[fuzz][audio][wav]") {
    std::vector<fs::path> wav_files;
    for (const auto& file : corpus_files()) {
        const auto ext = lower_extension(file);
        if (ext == ".wav" || ext == ".wave")
            wav_files.push_back(file);
    }
    REQUIRE_FALSE(wav_files.empty());

    auto& registry = pulp::audio::FormatRegistry::instance();
    const auto limits = pulp_fuzz::audio_decoder::fuzz_limits();
    pulp::audio::WavDecodeLimits span_limits;
    span_limits.max_frames = limits.max_frames;
    span_limits.max_channels = limits.max_channels;
    span_limits.max_output_bytes = limits.max_output_bytes;

    std::size_t both_decoded = 0;
    for (const auto& file : wav_files) {
        const auto name = file.filename().string();
        const auto bytes = read_bytes(file);
        const auto registry_data = registry.read(file.string(), limits);
        const auto span_data = pulp::audio::decode_wav(bytes, span_limits);

        if (registry_data.has_value() != span_data.has_value()) {
            FAIL_CHECK(name << ": registry (CHOC) "
                            << (registry_data ? "decoded" : "refused") << ", decode_wav (dr_wav) "
                            << (span_data ? "decoded" : "refused"));
            continue;
        }
        if (!registry_data)
            continue;
        ++both_decoded;

        INFO(name);
        CHECK(registry_data->sample_rate == span_data->sample_rate);
        CHECK(registry_data->num_channels() == span_data->num_channels());
        CHECK(registry_data->num_frames() == span_data->num_frames());
        if (registry_data->num_channels() != span_data->num_channels() ||
            registry_data->num_frames() != span_data->num_frames())
            continue;

        for (std::size_t c = 0; c < registry_data->channels.size(); ++c) {
            const auto& lhs = registry_data->channels[c];
            const auto& rhs = span_data->channels[c];
            std::size_t first_mismatch = lhs.size();
            for (std::size_t i = 0; i < lhs.size(); ++i) {
                if (std::memcmp(&lhs[i], &rhs[i], sizeof(float)) != 0) {
                    first_mismatch = i;
                    break;
                }
            }
            if (first_mismatch != lhs.size()) {
                FAIL_CHECK(name << ": channel " << c << " sample " << first_mismatch
                                << " differs: registry " << lhs[first_mismatch]
                                << ", decode_wav " << rhs[first_mismatch]);
            }
        }
    }
    // At least one seed must be well formed, or the differential measured nothing.
    REQUIRE(both_decoded > 0);
}
