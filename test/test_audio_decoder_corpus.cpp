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
#include <chrono>
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
        {"wav", {".wav", ".wave"}}, {"aiff", {".aiff", ".aif", ".aifc"}},
        {"flac", {".flac"}},        {"mp3", {".mp3"}},
        {"ogg", {".ogg", ".oga"}},
    };
    return formats;
}

std::vector<uint8_t> read_bytes(const fs::path& path) {
    std::ifstream input(path, std::ios::binary);
    REQUIRE(input.good());
    return std::vector<uint8_t>(std::istreambuf_iterator<char>(input), {});
}

// A canonical little-endian PCM WAV holding `codes` as mono samples at the
// given integer depth (8-bit written unsigned, as the format requires).
std::vector<uint8_t> make_pcm_wav(uint16_t bits, const std::vector<int32_t>& codes) {
    const uint16_t bytes_per_sample = bits / 8;
    const uint32_t data_size = static_cast<uint32_t>(codes.size()) * bytes_per_sample;
    std::vector<uint8_t> out;
    auto put32 = [&](uint32_t v) {
        for (int i = 0; i < 4; ++i)
            out.push_back(static_cast<uint8_t>(v >> (8 * i)));
    };
    auto put16 = [&](uint16_t v) {
        out.push_back(static_cast<uint8_t>(v));
        out.push_back(static_cast<uint8_t>(v >> 8));
    };
    auto tag = [&](const char* t) { out.insert(out.end(), t, t + 4); };
    const uint32_t pad = data_size & 1u; // RIFF chunks are word aligned
    tag("RIFF");
    put32(36u + data_size + pad);
    tag("WAVE");
    tag("fmt ");
    put32(16u);
    put16(1u); // PCM
    put16(1u); // mono
    put32(44100u);
    put32(44100u * bytes_per_sample);
    put16(bytes_per_sample);
    put16(bits);
    tag("data");
    put32(data_size);
    for (int32_t code : codes) {
        const uint32_t raw =
            bits == 8 ? static_cast<uint32_t>(code + 128) : static_cast<uint32_t>(code);
        for (uint16_t b = 0; b < bytes_per_sample; ++b)
            out.push_back(static_cast<uint8_t>(raw >> (8 * b)));
    }
    if (pad)
        out.push_back(0u);
    return out;
}

struct TempDir {
    fs::path path;
    TempDir() {
        path = fs::temp_directory_path() /
               ("pulp-audio-decoder-corpus-" +
                std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        fs::create_directories(path);
    }
    ~TempDir() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
};

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
        const bool found =
            std::any_of(extensions.begin(), extensions.end(),
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

// Seeds where exactly one WAV path decodes, by design or by a tracked
// difference (Generous-Corp/pulp issue 11788): the registry's CHOC reader is
// lenient about chunk extents and knows RF64, the byte-span dr_wav decoder is
// strict about the declared RIFF extent and refuses RF64. Each row is
// (registry decodes, decode_wav decodes). A split not listed here is a
// finding, and so is a row whose split no longer happens; the sample
// comparison itself is never relaxed.
struct ExpectedWavSplit {
    const char* seed;
    bool registry_decodes;
    bool span_decodes;
};
const std::vector<ExpectedWavSplit>& expected_wav_splits() {
    static const std::vector<ExpectedWavSplit> splits = {
        {"wav-data-past-eof.wav", true, false},
        {"wav-data-size-ffffffff-no-ds64.wav", true, false},
        {"wav-declares-100000001-frames.wav", true, false},
        {"wav-odd-chunk-eof-no-pad.wav", true, false},
        {"wav-rf64-ds64.wav", true, false},
        {"wav-riff-size-lying-small.wav", true, false},
        {"wav-fmt-after-data.wav", false, true},
    };
    return splits;
}

const ExpectedWavSplit* find_expected_wav_split(const std::string& name) {
    for (const auto& split : expected_wav_splits())
        if (name == split.seed)
            return &split;
    return nullptr;
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
    std::set<std::string> splits_seen;
    for (const auto& file : wav_files) {
        const auto name = file.filename().string();
        const auto bytes = read_bytes(file);
        const auto registry_data = registry.read(file.string(), limits);
        const auto span_data = pulp::audio::decode_wav(bytes, span_limits);

        if (registry_data.has_value() != span_data.has_value()) {
            const auto* expected = find_expected_wav_split(name);
            if (expected != nullptr && expected->registry_decodes == registry_data.has_value() &&
                expected->span_decodes == span_data.has_value()) {
                splits_seen.insert(name);
                continue;
            }
            FAIL_CHECK(name << ": registry (CHOC) " << (registry_data ? "decoded" : "refused")
                            << ", decode_wav (dr_wav) " << (span_data ? "decoded" : "refused"));
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
                                << " differs: registry " << lhs[first_mismatch] << ", decode_wav "
                                << rhs[first_mismatch]);
            }
        }
    }
    // A listed split that no longer happens is stale and must be removed.
    for (const auto& split : expected_wav_splits()) {
        INFO("expected split no longer observed: " << split.seed);
        CHECK(splits_seen.count(split.seed) == 1);
    }
    // At least one seed must be well formed, or the differential measured nothing.
    REQUIRE(both_decoded > 0);
}

TEST_CASE("integer PCM WAV extremes decode by 2^(bits-1) on both WAV paths", "[fuzz][audio][wav]") {
    struct Depth {
        uint16_t bits;
        std::vector<int32_t> codes;
        std::vector<float> expected;
    };
    const std::vector<Depth> depths = {
        {8, {-128, 0, 127}, {-1.0f, 0.0f, 127.0f / 128.0f}},
        {16, {-32768, 0, 32767}, {-1.0f, 0.0f, 32767.0f / 32768.0f}},
        {24, {-8388608, 0, 8388607}, {-1.0f, 0.0f, 8388607.0f / 8388608.0f}},
    };

    TempDir dir;
    auto& registry = pulp::audio::FormatRegistry::instance();
    for (const auto& depth : depths) {
        INFO("bits = " << depth.bits);
        const auto bytes = make_pcm_wav(depth.bits, depth.codes);
        const auto path = dir.path / ("pcm" + std::to_string(depth.bits) + ".wav");
        {
            std::ofstream output(path, std::ios::binary);
            REQUIRE(output.good());
            output.write(reinterpret_cast<const char*>(bytes.data()),
                         static_cast<std::streamsize>(bytes.size()));
        }

        const auto registry_data = registry.read(path.string());
        REQUIRE(registry_data.has_value());
        REQUIRE(registry_data->num_channels() == 1u);
        REQUIRE(registry_data->num_frames() == depth.codes.size());
        for (std::size_t i = 0; i < depth.codes.size(); ++i) {
            INFO("code " << depth.codes[i]);
            CHECK(registry_data->channels[0][i] == depth.expected[i]);
        }

        const auto span_data = pulp::audio::decode_wav(bytes);
        REQUIRE(span_data.has_value());
        REQUIRE(span_data->num_frames() == depth.codes.size());
        for (std::size_t i = 0; i < depth.codes.size(); ++i) {
            INFO("code " << depth.codes[i] << ": registry " << registry_data->channels[0][i]
                         << ", decode_wav " << span_data->channels[0][i]);
            CHECK(std::memcmp(&registry_data->channels[0][i], &span_data->channels[0][i],
                              sizeof(float)) == 0);
        }
    }
}

TEST_CASE("a declared length the stream does not deliver is refused, an undeclared one decoded",
          "[fuzz][audio]") {
    // Pinned by name so the contract survives changes to the generic oracle
    // case: the FLAC's STREAMINFO declares 800 frames the stream does not
    // hold, so read refuses while read_info still reports the declaration;
    // the two Ogg files declare no length (0), so read_info reports 0 and a
    // decode, if the decoder ends cleanly, is accepted under the limits.
    auto& registry = pulp::audio::FormatRegistry::instance();
    const auto limits = pulp_fuzz::audio_decoder::fuzz_limits();

    {
        const auto path = corpus_dir() / "flac-truncated-midframe.flac";
        REQUIRE(fs::exists(path));
        const auto info = registry.read_info(path.string());
        REQUIRE(info.has_value());
        CHECK(info->num_frames == 800u);
        CHECK_FALSE(registry.read(path.string(), limits).has_value());
    }

    for (const char* name : {"ogg-page-bad-crc.ogg", "ogg-truncated-page.ogg"}) {
        const auto path = corpus_dir() / name;
        INFO(name);
        REQUIRE(fs::exists(path));
        const auto info = registry.read_info(path.string());
        REQUIRE(info.has_value());
        CHECK(info->num_frames == 0u);
        if (const auto data = registry.read(path.string(), limits)) {
            CHECK(data->sample_rate == info->sample_rate);
            CHECK(data->num_channels() == info->num_channels);
        }
    }
}
