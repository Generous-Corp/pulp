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

// What each committed seed must do, so "both refused" cannot read as
// agreement for a seed that is supposed to decode. Valid seeds (per the
// corpus README) must decode through the registry; malformed seeds may be
// refused or decoded, and the oracle judges whatever comes back. A seed in
// neither list fails the case until it is classified here.
const std::set<std::string>& seeds_that_must_decode() {
    static const std::set<std::string> seeds = {
        "caf-pcm16-mono.caf",
        "flac-fixed-blocksize-mono.flac",
        "flac-seektable-padding.flac",
        "m4a-aac-lc-mono.m4a",
        "m4a-alac-stereo.m4a",
        "mp3-cbr-mono.mp3",
        "mp3-id3v2-xing-vbr.mp3",
        "ogg-vorbis-mono.ogg",
        "aifc-none-pcm16-mono.aifc",
        "aiff-comm-after-ssnd.aiff",
        "aiff-pcm16-mono.aiff",
        "flac-variable-blocksize-verbatim.flac",
        "m4a-aac-co64.m4a",
        "m4a-aac-mdat-size1.m4a",
        "ogg-chained-two-streams.ogg",
        "wav-bwf-bext-before-data.wav",
        "wav-extensible-pcm24in32-stereo.wav",
        "wav-float32-mono-fmt18.wav",
        "wav-float64-mono.wav",
        "wav-list-info-heavy.wav",
        "wav-pcm16-mono-canonical44.wav",
        "wav-pcm24-stereo.wav",
        "wav-pcm32-stereo.wav",
        "wav-pcm8-stereo.wav",
        "wav-rf64-ds64.wav",
    };
    return seeds;
}
const std::set<std::string>& seeds_that_may_refuse() {
    static const std::set<std::string> seeds = {
        "aiff-ssnd-past-eof.aiff",
        "flac-streaminfo-huge-total-samples.flac",
        "flac-truncated-midframe.flac",
        "mp3-truncated-frame.mp3",
        "ogg-page-bad-crc.ogg",
        "ogg-truncated-page.ogg",
        "ogg-vendor-length-negative.ogg",
        "wav-data-past-eof.wav",
        "wav-data-size-ffffffff-no-ds64.wav",
        "wav-declares-100000001-frames.wav",
        "wav-duplicate-fmt.wav",
        "wav-fmt-after-data.wav",
        "wav-odd-chunk-eof-no-pad.wav",
        "wav-overlapping-chunks.wav",
        "wav-riff-header-only.wav",
        "wav-riff-size-lying-small.wav",
        // Valid per the README (CBR base plus an APEv2 tag), refused by
        // dr_mp3's trailing-tag handling; tracked in Generous-Corp/pulp
        // issue 11788.
        "mp3-apev2-footer.mp3",
        // Valid AIFF-C with little-endian ("sowt") PCM, which the AIFF reader
        // does not support; tracked in Generous-Corp/pulp issue 11788.
        "aifc-sowt-pcm16-stereo.aifc",
        // Valid zero-frame WAV, which read_audio_file refuses as empty; tracked
        // in Generous-Corp/pulp issue 11788.
        "wav-data-size-zero.wav",
        // Valid Sony Wave64 under a .wav name; Pulp registers no Wave64 reader
        // and the RIFF reader refuses the GUID envelope by design.
        "wav-w64-pcm16-mono.wav",
        "mp3-xing-declares-more-than-stream.mp3",
    };
    return seeds;
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

    // Both Ogg files declare no length (0), and stb_vorbis reports neither a
    // truncated page nor a bad page CRC as a decoder error: it treats both as
    // the end of the stream. So both decode what the stream held, and the
    // oracle checks rate and channels against the header.
    struct OggExpectation {
        const char* name;
        bool decodes;
    };
    for (const auto& expected : {OggExpectation{"ogg-truncated-page.ogg", true},
                                 OggExpectation{"ogg-page-bad-crc.ogg", true}}) {
        const auto path = corpus_dir() / expected.name;
        INFO(expected.name);
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

TEST_CASE("an MP3 whose Xing header declares more frames than the stream delivers is refused",
          "[fuzz][audio][mp3]") {
    // Found by the fuzzer as an info/decode disagreement (6400 declared, a
    // few dozen decoded); this seed is the Xing VBR seed cut at 80%.
    const auto path = corpus_dir() / "mp3-xing-declares-more-than-stream.mp3";
    REQUIRE(fs::exists(path));
    auto& registry = pulp::audio::FormatRegistry::instance();
    const auto info = registry.read_info(path.string());
    REQUIRE(info.has_value());
    CHECK(info->num_frames == 6400u);
    CHECK_FALSE(registry.read(path.string(), pulp_fuzz::audio_decoder::fuzz_limits()).has_value());
    // The untouched seed still declares and delivers the same count.
    const auto whole = corpus_dir() / "mp3-id3v2-xing-vbr.mp3";
    const auto decoded = registry.read(whole.string(), pulp_fuzz::audio_decoder::fuzz_limits());
    REQUIRE(decoded.has_value());
    CHECK(decoded->num_frames() == 6400u);
}

TEST_CASE("a comment header with a negative string length is refused without allocating",
          "[fuzz][audio][ogg]") {
    // libFuzzer reproducer: the vendor-string length is a negative 32-bit
    // value, which once became a negative-sized setup allocation.
    const auto path = corpus_dir() / "ogg-vendor-length-negative.ogg";
    REQUIRE(fs::exists(path));
    auto& registry = pulp::audio::FormatRegistry::instance();
    const auto started = std::chrono::steady_clock::now();
    CHECK_FALSE(registry.read_info(path.string()).has_value());
    CHECK_FALSE(registry.read(path.string(), pulp_fuzz::audio_decoder::fuzz_limits()).has_value());
    CHECK(std::chrono::steady_clock::now() - started < std::chrono::seconds(1));
}

TEST_CASE("read_audio_file applies the default decode ceilings to a declared header",
          "[fuzz][audio][wav]") {
    // Header-only WAVs: the data chunk declares the size, the file holds none
    // of it. Without the ceiling the declared size was allocated.
    auto header_only = [](uint16_t channels, uint32_t data_size) {
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
        tag("RIFF");
        put32(36u + data_size);
        tag("WAVE");
        tag("fmt ");
        put32(16u);
        put16(1u);
        put16(channels);
        put32(44100u);
        put32(44100u * 2u * channels);
        put16(static_cast<uint16_t>(2u * channels));
        put16(16u);
        tag("data");
        put32(data_size);
        return out;
    };
    TempDir dir;
    auto write = [&](const char* name, const std::vector<uint8_t>& bytes) {
        const auto path = dir.path / name;
        std::ofstream output(path, std::ios::binary);
        REQUIRE(output.good());
        output.write(reinterpret_cast<const char*>(bytes.data()),
                     static_cast<std::streamsize>(bytes.size()));
        return path.string();
    };
    // 2^30 + 1 mono frames: just past the default max_frames.
    const auto frames_path =
        write("too-many-frames.wav", header_only(1u, static_cast<uint32_t>((1ull << 31) + 2u)));
    // 65 channels: past the default max_channels.
    const auto channels_path = write("too-many-channels.wav", header_only(65u, 65u * 2u * 8u));

    const auto started = std::chrono::steady_clock::now();
    CHECK_FALSE(pulp::audio::read_audio_file(frames_path).has_value());
    CHECK_FALSE(pulp::audio::read_audio_file(channels_path).has_value());
    CHECK(std::chrono::steady_clock::now() - started < std::chrono::seconds(1));

    // Under the ceiling: the corpus seed that declares 100,000,001 frames
    // still decodes through the same overload.
    const auto seed = corpus_dir() / "wav-declares-100000001-frames.wav";
    REQUIRE(fs::exists(seed));
    CHECK(pulp::audio::read_audio_file(seed.string()).has_value());
}
