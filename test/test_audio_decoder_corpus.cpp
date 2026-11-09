// Deterministic replay of the audio decoder seed corpus through the oracle the
// libFuzzer target uses. This is the lane that runs on the required gate, and
// the only lane that reaches the platform CoreAudio reader, which the fuzzer
// excludes. It also holds the WAV differential: the registry's CHOC path and
// the byte-span dr_wav decoder must agree bit for bit on every seed.
// approach after SimZhou/vscode-audiolens c5c5a24; re-authored

#include "fuzz/audio_decoder_fuzz_oracle.hpp"
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

std::vector<uint8_t> read_bytes(const fs::path& path) {
    std::ifstream input(path, std::ios::binary);
    REQUIRE(input.good());
    return std::vector<uint8_t>(std::istreambuf_iterator<char>(input), {});
}

// A canonical little-endian PCM WAV at 44.1 kHz: `codes` are interleaved
// integer samples at the given depth (8-bit written unsigned, as the format
// requires). `declared_data_size` lets the data chunk claim more than the
// file holds, for header-only fixtures; by default it is the real size.
std::vector<uint8_t> make_pcm_wav(uint16_t channels, uint16_t bits,
                                  const std::vector<int32_t>& codes,
                                  std::optional<uint32_t> declared_data_size = std::nullopt) {
    const uint16_t bytes_per_sample = bits / 8;
    const uint32_t actual_size = static_cast<uint32_t>(codes.size()) * bytes_per_sample;
    const uint32_t data_size = declared_data_size.value_or(actual_size);
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
    const uint32_t pad = actual_size & 1u; // RIFF chunks are word aligned
    tag("RIFF");
    put32(36u + data_size + pad);
    tag("WAVE");
    tag("fmt ");
    put32(16u);
    put16(1u); // PCM
    put16(channels);
    put32(44100u);
    put32(44100u * bytes_per_sample * channels);
    put16(static_cast<uint16_t>(bytes_per_sample * channels));
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

std::string write_file(const fs::path& path, const std::vector<uint8_t>& bytes) {
    std::ofstream output(path, std::ios::binary);
    REQUIRE(output.good());
    output.write(reinterpret_cast<const char*>(bytes.data()),
                 static_cast<std::streamsize>(bytes.size()));
    return path.string();
}

// What each committed seed must do, one row per seed, so "both refused"
// cannot read as agreement for a seed that is supposed to decode and so
// the WAV differential reads its expected splits from the same table.
//   must_decode        valid per the corpus README: read_info and read succeed
//                      through the registry; false means it may be refused
//   span_decodes       WAV seeds only: decode_wav's outcome WHEN it differs
//                      from the registry's (a listed split); empty means the
//                      two WAV paths agree on accepting or refusing
//   libsndfile_decodes mirror of the lab differential's table (checked there)
//   reason             why a refusal or split is expected
// A seed in the corpus without a row fails the replay until it is classified.
struct SeedExpectation {
    bool must_decode;
    std::optional<bool> span_decodes;
    std::optional<bool> libsndfile_decodes;
    const char* reason;
};
const std::map<std::string, SeedExpectation>& seed_expectations() {
    using E = SeedExpectation;
    static const std::map<std::string, SeedExpectation> rows = {
        // valid seeds
        {"aifc-none-pcm16-mono.aifc", E{true, {}, {}, ""}},
        {"aifc-sowt-pcm16-stereo.aifc", E{true, {}, {}, ""}},
        {"aiff-comm-after-ssnd.aiff", E{true, {}, {}, ""}},
        {"aiff-pcm16-mono.aiff", E{true, {}, {}, ""}},
        {"caf-pcm16-mono.caf", E{true, {}, {}, ""}},
        {"flac-fixed-blocksize-mono.flac", E{true, {}, {}, ""}},
        {"flac-seektable-padding.flac", E{true, {}, {}, ""}},
        {"flac-variable-blocksize-verbatim.flac", E{true, {}, {}, ""}},
        {"m4a-aac-co64.m4a", E{true, {}, {}, ""}},
        {"m4a-aac-lc-mono.m4a", E{true, {}, {}, ""}},
        {"m4a-aac-mdat-size1.m4a", E{true, {}, {}, ""}},
        {"m4a-alac-stereo.m4a", E{true, {}, {}, ""}},
        {"mp3-apev1-footer.mp3", E{true, {}, {}, ""}},
        {"mp3-apev2-footer.mp3", E{true, {}, {}, ""}},
        {"mp3-apev2-size-high-bit.mp3", E{true, {}, {}, ""}},
        {"mp3-cbr-mono.mp3", E{true, {}, {}, ""}},
        {"mp3-id3v2-xing-vbr.mp3", E{true, {}, {}, ""}},
        {"ogg-chained-two-streams.ogg", E{true, {}, {}, ""}},
        {"ogg-vorbis-mono.ogg", E{true, {}, {}, ""}},
        {"wav-bwf-bext-before-data.wav", E{true, {}, {}, ""}},
        {"wav-data-size-zero.wav", E{true, {}, {}, ""}},
        {"wav-extensible-pcm24in32-stereo.wav", E{true, {}, {}, ""}},
        {"wav-float32-mono-fmt18.wav", E{true, {}, {}, ""}},
        {"wav-float64-mono.wav", E{true, {}, {}, ""}},
        {"wav-list-info-heavy.wav", E{true, {}, {}, ""}},
        {"wav-pcm16-mono-canonical44.wav", E{true, {}, {}, ""}},
        {"wav-pcm24-stereo.wav", E{true, {}, {}, ""}},
        {"wav-pcm32-stereo.wav", E{true, {}, {}, ""}},
        {"wav-pcm8-stereo.wav", E{true, {}, {}, ""}},
        {"wav-rf64-ds64.wav",
         E{true, false, {}, "RF64 envelope: the registry (CHOC) reads it, decode_wav does not"}},
        // valid per the README, refused by Pulp for a stated reason
        {"wav-w64-pcm16-mono.wav",
         E{false, {}, true, "Sony Wave64 envelope; Pulp registers no Wave64 reader"}},
        // malformed seeds: refused or decoded, the oracle judges what comes back
        {"aiff-ssnd-past-eof.aiff",
         E{false, {}, true, "SSND runs past the end of the file; Pulp refuses truncated streams"}},
        {"flac-streaminfo-huge-total-samples.flac", E{false, {}, {}, ""}},
        {"flac-truncated-midframe.flac", E{false, {}, {}, "declared 800 frames, stream holds 384"}},
        {"mp3-truncated-frame.mp3", E{false, {}, {}, ""}},
        {"mp3-xing-declares-more-than-stream.mp3",
         E{false, {}, {}, "Xing declares 6400 frames, stream delivers 2351"}},
        {"mp3-id3v2-scan-counts-undecodable-frame.mp3",
         E{false, {}, {}, "no declared length; header scan counts a frame the decoder rejects"}},
        {"ogg-page-bad-crc.ogg", E{false, {}, {}, ""}},
        {"ogg-truncated-page.ogg", E{false, {}, {}, ""}},
        {"ogg-vendor-length-negative.ogg", E{false, {}, {}, "negative vendor-string length"}},
        {"ogg-comment-length-int-max.ogg",
         E{false, {}, {}, "comment-string length near INT_MAX; len + 1 overflowed the allocator"}},
        {"wav-bext-declares-4gib.wav",
         E{false,
           {},
           {},
           "bext chunk shorter than its fixed fields; CHOC once sized a 4 GiB string read"}},
        {"wav-block-align-smaller-than-frame.wav",
         E{false, true, true,
           "nBlockAlign smaller than channels x sample width: the registry refuses the layout, "
           "dr_wav and libsndfile derive the stride"}},
        {"wav-data-past-eof.wav",
         E{false,
           false,
           {},
           "data chunk runs past EOF: CHOC decodes what is there, dr_wav refuses"}},
        {"wav-data-size-ffffffff-no-ds64.wav",
         E{false, false, {}, "RF64 sentinel without ds64: CHOC decodes, dr_wav refuses"}},
        {"wav-declares-100000001-frames.wav",
         E{false,
           false,
           {},
           "declared frames exceed the data: CHOC decodes what is there, dr_wav refuses"}},
        {"wav-duplicate-fmt.wav",
         E{false,
           {},
           false,
           "two fmt chunks: Pulp decodes by the last one, libsndfile refuses; issue 11788"}},
        {"wav-fmt-after-data.wav",
         E{false, true, {}, "fmt after data: CHOC refuses, dr_wav decodes"}},
        {"wav-odd-chunk-eof-no-pad.wav",
         E{false,
           false,
           {},
           "odd chunk at EOF without its pad byte: CHOC decodes, dr_wav refuses"}},
        {"wav-overlapping-chunks.wav", E{false, {}, {}, ""}},
        {"wav-riff-header-only.wav", E{false, {}, {}, ""}},
        {"wav-riff-size-lying-small.wav",
         E{false, false, {}, "RIFF size smaller than the chunks: CHOC decodes, dr_wav refuses"}},
    };
    return rows;
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
            // A listed split: the row names decode_wav's outcome where it
            // differs from the registry's.
            const auto row = seed_expectations().find(name);
            if (row != seed_expectations().end() && row->second.span_decodes.has_value() &&
                *row->second.span_decodes == span_data.has_value()) {
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
    for (const auto& [name, row] : seed_expectations()) {
        if (!row.span_decodes.has_value())
            continue;
        INFO("expected split no longer observed: " << name);
        CHECK(splits_seen.count(name) == 1);
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

    pulp::test::ScopedTempDir dir("pulp-audio-decoder-corpus");
    auto& registry = pulp::audio::FormatRegistry::instance();
    for (const auto& depth : depths) {
        INFO("bits = " << depth.bits);
        const auto bytes = make_pcm_wav(1u, depth.bits, depth.codes);
        const auto path =
            write_file(dir.path / ("pcm" + std::to_string(depth.bits) + ".wav"), bytes);

        const auto registry_data = registry.read(path);
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
        const auto data = registry.read(path.string(), limits);
        CHECK(data.has_value() == expected.decodes);
        if (data) {
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
    // The untouched seed still declares and delivers the same count, and a
    // ceiling below its declared count refuses it at the pre-check, before
    // any frame is decoded.
    const auto whole = corpus_dir() / "mp3-id3v2-xing-vbr.mp3";
    const auto decoded = registry.read(whole.string(), pulp_fuzz::audio_decoder::fuzz_limits());
    REQUIRE(decoded.has_value());
    CHECK(decoded->num_frames() == 6400u);
    pulp::audio::DecodeLimits below_claim;
    below_claim.max_frames = 6399u;
    CHECK_FALSE(registry.read(whole.string(), below_claim).has_value());
}

TEST_CASE("an MP3 with no declared length reports the frames the decoder delivers",
          "[fuzz][audio][mp3]") {
    // libFuzzer reproducer: the Xing frame is damaged, so nothing is
    // declared; dr_mp3's header scan counts four frames while the decoder
    // delivers three. read_info once reported the scan.
    const auto path = corpus_dir() / "mp3-id3v2-scan-counts-undecodable-frame.mp3";
    REQUIRE(fs::exists(path));
    auto& registry = pulp::audio::FormatRegistry::instance();
    const auto info = registry.read_info(path.string());
    const auto data = registry.read(path.string(), pulp_fuzz::audio_decoder::fuzz_limits());
    REQUIRE(info.has_value());
    REQUIRE(data.has_value());
    CHECK(data->num_frames() == 3456u);
    CHECK(info->num_frames == data->num_frames());
}

namespace {

// The bytes in front of a trailing APE tag. The footer's size field (bytes
// 12-15) counts the items and the footer; a 32-byte header also precedes the
// items when flags bit 31 (footer byte 23, bit 7) is set.
std::vector<uint8_t> strip_ape_tag(const std::vector<uint8_t>& bytes) {
    REQUIRE(bytes.size() > 32u);
    const auto footer = bytes.end() - 32;
    REQUIRE(std::string(footer, footer + 8) == "APETAGEX");
    const uint32_t tag_size = uint32_t(footer[12]) | (uint32_t(footer[13]) << 8) |
                              (uint32_t(footer[14]) << 16) | (uint32_t(footer[15]) << 24);
    const uint32_t header_size = (footer[23] & 0x80u) != 0 ? 32u : 0u;
    REQUIRE(uint64_t(tag_size) + header_size < bytes.size());
    return std::vector<uint8_t>(bytes.begin(), bytes.end() - (tag_size + header_size));
}

void require_same_audio(const pulp::audio::AudioFileData& tagged,
                        const pulp::audio::AudioFileData& stripped) {
    REQUIRE(stripped.num_frames() > 0u);
    REQUIRE(tagged.sample_rate == stripped.sample_rate);
    REQUIRE(tagged.num_channels() == stripped.num_channels());
    REQUIRE(tagged.num_frames() == stripped.num_frames());
    for (uint32_t ch = 0; ch < tagged.num_channels(); ++ch)
        CHECK(std::memcmp(tagged.channels[ch].data(), stripped.channels[ch].data(),
                          tagged.channels[ch].size() * sizeof(float)) == 0);
}

} // namespace

TEST_CASE("an MP3 behind an APE tag decodes exactly as the same MP3 without it",
          "[fuzz][audio][mp3]") {
    // APEv2 with a header, APEv1 without one, and an APEv2 tag whose size
    // byte has its high bit set. Each stripped stream is the untagged base.
    const auto base = read_bytes(corpus_dir() / "mp3-cbr-mono.mp3");
    pulp::test::ScopedTempDir dir("pulp-audio-decoder-ape");
    auto& registry = pulp::audio::FormatRegistry::instance();
    const auto limits = pulp_fuzz::audio_decoder::fuzz_limits();
    for (const char* name :
         {"mp3-apev2-footer.mp3", "mp3-apev1-footer.mp3", "mp3-apev2-size-high-bit.mp3"}) {
        INFO(name);
        const auto tagged = read_bytes(corpus_dir() / name);
        const auto stripped = strip_ape_tag(tagged);
        REQUIRE(stripped.size() < tagged.size());
        CHECK(stripped == base);

        const auto tagged_path = write_file(dir.path / (std::string("tagged-") + name), tagged);
        const auto stripped_path =
            write_file(dir.path / (std::string("stripped-") + name), stripped);
        const auto tagged_info = registry.read_info(tagged_path);
        const auto stripped_info = registry.read_info(stripped_path);
        REQUIRE(tagged_info.has_value());
        REQUIRE(stripped_info.has_value());
        CHECK(tagged_info->num_frames == stripped_info->num_frames);
        const auto tagged_data = registry.read(tagged_path, limits);
        const auto stripped_data = registry.read(stripped_path, limits);
        REQUIRE(tagged_data.has_value());
        REQUIRE(stripped_data.has_value());
        require_same_audio(*tagged_data, *stripped_data);

        auto tagged_reader = pulp::audio::AudioReader::open_memory(tagged, ".mp3", limits);
        auto stripped_reader = pulp::audio::AudioReader::open_memory(stripped, ".mp3", limits);
        REQUIRE(tagged_reader != nullptr);
        REQUIRE(stripped_reader != nullptr);
        REQUIRE(tagged_reader->info().num_frames == stripped_reader->info().num_frames);
        const auto frames = stripped_reader->info().num_frames;
        const auto tagged_range = tagged_reader->read(0u, frames);
        const auto stripped_range = stripped_reader->read(0u, frames);
        REQUIRE(tagged_range.has_value());
        REQUIRE(stripped_range.has_value());
        require_same_audio(*tagged_range, *stripped_range);
        CHECK(stripped_range->num_frames() == stripped_data->num_frames());
    }
}

TEST_CASE("a WAV whose bext chunk is shorter than its fixed fields is refused without allocating",
          "[fuzz][audio][wav]") {
    // libFuzzer reproducer: a 128-byte bext chunk; the BWAV reader's fixed
    // fields ran past it and the remaining-length subtraction wrapped to
    // about 4 GiB, which std::string::resize then requested. The file has no
    // data chunk, so info reports zero frames (as a zero-length data chunk
    // does) and the decode is refused; both must finish without that request.
    const auto path = corpus_dir() / "wav-bext-declares-4gib.wav";
    REQUIRE(fs::exists(path));
    auto& registry = pulp::audio::FormatRegistry::instance();
    const auto started = std::chrono::steady_clock::now();
    const auto direct = pulp::audio::read_audio_file_info(path.string());
    REQUIRE(direct.has_value());
    CHECK(direct->num_frames == 0u);
    const auto info = registry.read_info(path.string());
    REQUIRE(info.has_value());
    CHECK(info->num_frames == 0u);
    CHECK_FALSE(pulp::audio::read_audio_file(path.string()).has_value());
    CHECK_FALSE(registry.read(path.string(), pulp_fuzz::audio_decoder::fuzz_limits()).has_value());
    CHECK(std::chrono::steady_clock::now() - started < std::chrono::seconds(1));
}

TEST_CASE("a WAV whose block align is smaller than its channels' samples is refused",
          "[fuzz][audio][wav]") {
    // libFuzzer reproducer: two 8-bit channels declared with nBlockAlign 1,
    // which once walked the reader off the end of its raw block.
    const auto path = corpus_dir() / "wav-block-align-smaller-than-frame.wav";
    REQUIRE(fs::exists(path));
    auto& registry = pulp::audio::FormatRegistry::instance();
    CHECK_FALSE(registry.read_info(path.string()).has_value());
    CHECK_FALSE(registry.read(path.string(), pulp_fuzz::audio_decoder::fuzz_limits()).has_value());
}

TEST_CASE("a comment header with an impossible string length is refused without allocating",
          "[fuzz][audio][ogg]") {
    // libFuzzer reproducers: a negative vendor-string length, which once
    // became a negative-sized setup allocation, and a comment-string length
    // near INT_MAX, where len + 1 wrapped negative, slipped past the arena
    // check and the byte loop wrote past the arena.
    auto& registry = pulp::audio::FormatRegistry::instance();
    for (const char* name : {"ogg-vendor-length-negative.ogg", "ogg-comment-length-int-max.ogg"}) {
        const auto path = corpus_dir() / name;
        INFO(name);
        REQUIRE(fs::exists(path));
        const auto started = std::chrono::steady_clock::now();
        CHECK_FALSE(registry.read_info(path.string()).has_value());
        CHECK_FALSE(
            registry.read(path.string(), pulp_fuzz::audio_decoder::fuzz_limits()).has_value());
        CHECK(std::chrono::steady_clock::now() - started < std::chrono::seconds(1));
    }
}

TEST_CASE("read_audio_file applies the default decode ceilings to a declared header",
          "[fuzz][audio][wav]") {
    // Header-only WAVs: the data chunk declares the size, the file holds none
    // of it. Without the ceiling the declared size was allocated.
    pulp::test::ScopedTempDir dir("pulp-audio-decoder-corpus");
    auto write = [&](const char* name, const std::vector<uint8_t>& bytes) {
        return write_file(dir.path / name, bytes);
    };
    // 2^30 + 1 mono frames: just past the default max_frames.
    const auto frames_path = write(
        "too-many-frames.wav", make_pcm_wav(1u, 16u, {}, static_cast<uint32_t>((1ull << 31) + 2u)));
    // 65 channels: past the default max_channels.
    const auto channels_path =
        write("too-many-channels.wav", make_pcm_wav(65u, 16u, {}, 65u * 2u * 8u));

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
