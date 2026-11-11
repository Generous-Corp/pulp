// The decoder corpus replay's WAV cases: the registry and byte-span decoder
// differential, truncation recovery, integer scaling, and the WAV
// reproducers. The cross-format replay is in test_audio_decoder_corpus.cpp.
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
    std::size_t truncated_decoded = 0;
    for (const auto& file : wav_files) {
        const auto name = file.filename().string();
        INFO(name);
        const auto bytes = read_bytes(file);
        const auto registry_info = registry.read_info(file.string());
        const auto span_info = pulp::audio::inspect_wav(bytes);
        const auto registry_data = registry.read(file.string(), limits);
        const auto span_data = pulp::audio::decode_wav(bytes, span_limits);

        // One policy: the two paths accept and refuse the same files and
        // report the same facts about them.
        CHECK(registry_info.has_value() == span_info.has_value());
        CHECK(registry_data.has_value() == span_data.has_value());
        CHECK(registry_info.has_value() == registry_data.has_value());
        if (registry_info && span_info) {
            CHECK(registry_info->num_frames == span_info->num_frames);
            CHECK(registry_info->declared_frames == span_info->declared_frames);
            CHECK(registry_info->truncated == span_info->truncated);
            CHECK(registry_info->bits_per_sample == span_info->bits_per_sample);
        }
        if (!registry_data || !span_data)
            continue;
        ++both_decoded;
        if (registry_info && registry_info->truncated)
            ++truncated_decoded;

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
    // Well-formed and recovered seeds must both reach the comparison, or the
    // differential measured nothing about the recovery path.
    REQUIRE(both_decoded > 0);
    CHECK(truncated_decoded > 0);
}

TEST_CASE("a truncated WAV decodes the whole frames present and reports the declaration",
          "[fuzz][audio][wav]") {
    // The canonical base cut after a third of its samples: 1600 bytes of
    // mono 16-bit declared, 533 present, so 266 whole frames decode and the
    // header's 800 are reported alongside.
    auto& registry = pulp::audio::FormatRegistry::instance();
    for (const char* name : {"wav-data-past-eof.wav", "wav-rf64-ds64-data-past-eof.wav"}) {
        INFO(name);
        const auto path = corpus_dir() / name;
        REQUIRE(fs::exists(path));
        const auto info = registry.read_info(path.string());
        REQUIRE(info.has_value());
        CHECK(info->truncated);
        CHECK(info->num_frames == 266u);
        CHECK(info->declared_frames == 800u);
        const auto data = registry.read(path.string(), pulp_fuzz::audio_decoder::fuzz_limits());
        REQUIRE(data.has_value());
        CHECK(data->num_frames() == 266u);
    }
    // The untouched base declares exactly what it holds.
    const auto base =
        registry.read_info((corpus_dir() / "wav-pcm16-mono-canonical44.wav").string());
    REQUIRE(base.has_value());
    CHECK_FALSE(base->truncated);
    CHECK(base->declared_frames == base->num_frames);
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

TEST_CASE("a WAV whose bext chunk is shorter than its fixed fields is refused without allocating",
          "[fuzz][audio][wav]") {
    // libFuzzer reproducer: a 128-byte bext chunk; a reader that parsed the
    // BWAV fixed fields ran past it and the remaining-length subtraction
    // wrapped to about 4 GiB, which std::string::resize then requested. The
    // WAV walk reads no metadata payload, and the file has no data chunk, so
    // info and decode are both refused, without that request.
    const auto path = corpus_dir() / "wav-bext-declares-4gib.wav";
    REQUIRE(fs::exists(path));
    auto& registry = pulp::audio::FormatRegistry::instance();
    const auto started = std::chrono::steady_clock::now();
    CHECK_FALSE(pulp::audio::read_audio_file_info(path.string()).has_value());
    CHECK_FALSE(registry.read_info(path.string()).has_value());
    CHECK_FALSE(pulp::audio::read_audio_file(path.string()).has_value());
    CHECK_FALSE(registry.read(path.string(), pulp_fuzz::audio_decoder::fuzz_limits()).has_value());
    CHECK(std::chrono::steady_clock::now() - started < std::chrono::seconds(1));
}

TEST_CASE("a WAV whose block align is smaller than its channels' samples is refused",
          "[fuzz][audio][wav]") {
    // libFuzzer reproducer: two 8-bit channels declared with nBlockAlign 1,
    // which once walked a reader off the end of its raw block. The fmt chunk
    // contradicts itself, so every path refuses it.
    const auto path = corpus_dir() / "wav-block-align-smaller-than-frame.wav";
    REQUIRE(fs::exists(path));
    auto& registry = pulp::audio::FormatRegistry::instance();
    CHECK_FALSE(registry.read_info(path.string()).has_value());
    CHECK_FALSE(registry.read(path.string(), pulp_fuzz::audio_decoder::fuzz_limits()).has_value());
    const auto bytes = read_bytes(path);
    CHECK_FALSE(pulp::audio::inspect_wav(bytes).has_value());
    CHECK_FALSE(pulp::audio::decode_wav(bytes).has_value());
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

    // The ceilings bind the frames a decode produces. A valid seed is
    // refused one frame under its length and decodes at it.
    auto& registry = pulp::audio::FormatRegistry::instance();
    const auto seed = corpus_dir() / "wav-pcm16-mono-canonical44.wav";
    REQUIRE(fs::exists(seed));
    pulp::audio::DecodeLimits limits;
    limits.max_frames = 799u;
    CHECK_FALSE(registry.read(seed.string(), limits).has_value());
    limits.max_frames = 800u;
    CHECK(registry.read(seed.string(), limits).has_value());
    // A header declaring 100,000,001 frames over 800 is bound by the 800 it
    // holds, never by the declaration.
    const auto lying = corpus_dir() / "wav-declares-100000001-frames.wav";
    REQUIRE(fs::exists(lying));
    const auto recovered = registry.read(lying.string(), limits);
    REQUIRE(recovered.has_value());
    CHECK(recovered->num_frames() == 800u);
    CHECK(pulp::audio::read_audio_file(lying.string()).has_value());
}
