#include <catch2/catch_test_macros.hpp>

#include <pulp/audio/audio_file_layout.hpp>
#include <pulp/audio/wav_decoder.hpp>

#include "support/wav_fixture_builders.hpp"

#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstring>
#include <cstdint>
#include <limits>
#include <span>
#include <vector>

using namespace pulp_test::wav;

const char* consumer_drwav_version();

TEST_CASE("byte-span WAV decoder inspects and deinterleaves PCM16", "[audio][wav-decoder]") {
    REQUIRE(consumer_drwav_version()[0] != '\0');

    Bytes samples;
    append_sample(samples, std::int16_t{-32768});
    append_sample(samples, std::int16_t{16384});
    append_sample(samples, std::int16_t{32767});
    append_sample(samples, std::int16_t{-16384});
    const auto wav = make_wav(make_format(1u, 2u, 48'000u, 16u), samples);

    const auto info = pulp::audio::inspect_wav(wav);
    REQUIRE(info);
    CHECK(info->sample_rate == 48'000u);
    CHECK(info->num_channels == 2u);
    CHECK(info->num_frames == 2u);
    CHECK(info->bits_per_sample == 16u);
    CHECK(info->format == "WAV");

    const auto decoded = pulp::audio::decode_wav(wav);
    REQUIRE(decoded);
    REQUIRE(decoded->channels.size() == 2u);
    CHECK(decoded->channels[0] == std::vector<float>{-1.0f, 32767.0f / 32768.0f});
    CHECK(decoded->channels[1] == std::vector<float>{0.5f, -0.5f});
}

TEST_CASE("byte-span WAV decoder returns a zero-frame file as no audio, not a failure",
          "[audio][wav-decoder][zero-frames]") {
    const auto format = make_format(1u, 2u, 48'000u, 16u);

    SECTION("empty data chunk") {
        const auto wav = make_wav(format, Bytes{});
        const auto info = pulp::audio::inspect_wav(wav);
        REQUIRE(info);
        CHECK(info->sample_rate == 48'000u);
        CHECK(info->num_channels == 2u);
        CHECK(info->num_frames == 0u);
        CHECK(info->duration_seconds == 0.0);

        const auto decoded = pulp::audio::decode_wav(wav);
        REQUIRE(decoded);
        CHECK(decoded->sample_rate == 48'000u);
        REQUIRE(decoded->channels.size() == 2u);
        CHECK(decoded->channels[0].empty());
        CHECK(decoded->channels[1].empty());
        CHECK(decoded->num_frames() == 0u);
        CHECK(decoded->empty());
    }
    SECTION("empty data chunk ahead of fmt") {
        const auto wav = make_wav(format, Bytes{}, true);
        const auto decoded = pulp::audio::decode_wav(wav);
        REQUIRE(decoded);
        CHECK(decoded->num_channels() == 2u);
        CHECK(decoded->num_frames() == 0u);
    }
    SECTION("the channel ceiling still applies with no frames to allocate") {
        const auto wav = make_wav(format, Bytes{});
        pulp::audio::WavDecodeLimits limits;
        limits.max_channels = 1u;
        CHECK_FALSE(pulp::audio::decode_wav(wav, limits));
        // A zero frame ceiling is refused as a configuration error before any
        // header is read, so it says nothing about zero-frame files; the
        // channel ceiling above is the one this file can reach.
        limits = {};
        limits.max_frames = 0u;
        CHECK_FALSE(pulp::audio::decode_wav(wav, limits));
    }
    SECTION("a zero channel count is still a defect") {
        const auto wav = make_wav(make_format(1u, 0u, 48'000u, 16u), Bytes{});
        CHECK_FALSE(pulp::audio::inspect_wav(wav));
        CHECK_FALSE(pulp::audio::decode_wav(wav));
    }
}

TEST_CASE("byte-span WAV decoder supports PCM and float sample widths", "[audio][wav-decoder]") {
    SECTION("PCM8") {
        const Bytes samples{0x00u, 0x80u, 0xffu};
        const auto decoded =
            pulp::audio::decode_wav(make_wav(make_format(1u, 1u, 8'000u, 8u), samples));
        REQUIRE(decoded);
        CHECK(decoded->channels[0][0] == -1.0f);
        CHECK(decoded->channels[0][1] == 0.0f);
        CHECK(decoded->channels[0][2] == 127.0f / 128.0f);
    }
    SECTION("PCM24") {
        const Bytes samples{0x00u, 0x00u, 0x80u, 0xffu, 0xffu, 0x7fu};
        const auto decoded =
            pulp::audio::decode_wav(make_wav(make_format(1u, 1u, 44'100u, 24u), samples));
        REQUIRE(decoded);
        CHECK(decoded->channels[0][0] == -1.0f);
        CHECK(decoded->channels[0][1] == 8388607.0f / 8388608.0f);
    }
    SECTION("PCM32") {
        Bytes samples;
        append_sample(samples, std::int32_t{-2147483647 - 1});
        append_sample(samples, std::int32_t{1073741824});
        const auto decoded =
            pulp::audio::decode_wav(make_wav(make_format(1u, 1u, 96'000u, 32u), samples));
        REQUIRE(decoded);
        CHECK(decoded->channels[0] == std::vector<float>{-1.0f, 0.5f});
    }
    SECTION("IEEE float32") {
        Bytes samples;
        append_sample(samples, 1.25f);
        append_sample(samples, -0.25f);
        const auto decoded =
            pulp::audio::decode_wav(make_wav(make_format(3u, 1u, 48'000u, 32u), samples));
        REQUIRE(decoded);
        CHECK(decoded->channels[0] == std::vector<float>{1.25f, -0.25f});
    }
    SECTION("IEEE float64") {
        Bytes samples;
        append_sample(samples, 0.125);
        append_sample(samples, -2.0);
        const auto decoded =
            pulp::audio::decode_wav(make_wav(make_format(3u, 1u, 48'000u, 64u), samples));
        REQUIRE(decoded);
        CHECK(decoded->channels[0] == std::vector<float>{0.125f, -2.0f});
    }
    SECTION("WAVE_FORMAT_EXTENSIBLE PCM16") {
        Bytes samples;
        append_sample(samples, std::int16_t{4096});
        const auto decoded = pulp::audio::decode_wav(
            make_wav(make_extensible_pcm_format(1u, 48'000u, 16u), samples));
        REQUIRE(decoded);
        CHECK(decoded->channels[0][0] == 0.125f);
    }
}

TEST_CASE("byte-span WAV decoder handles chunk order and odd padding", "[audio][wav-decoder]") {
    Bytes samples;
    append_sample(samples, std::int16_t{8192});
    const auto decoded =
        pulp::audio::decode_wav(make_wav(make_format(1u, 1u, 22'050u, 16u), samples, true, true));
    REQUIRE(decoded);
    CHECK(decoded->channels[0][0] == 0.25f);
}

TEST_CASE("byte-span WAV decoder recovers the whole frames of a truncated file",
          "[audio][wav-decoder][truncated]") {
    Bytes samples;
    append_sample(samples, std::int16_t{0});
    append_sample(samples, std::int16_t{1});
    const auto valid = make_wav(make_format(1u, 1u, 48'000u, 16u), samples);
    constexpr std::size_t kHeader = 44u;
    constexpr std::size_t kFrameBytes = 2u;
    REQUIRE(valid.size() == kHeader + 2u * kFrameBytes);

    CHECK_FALSE(pulp::audio::inspect_wav({}));
    CHECK_FALSE(pulp::audio::decode_wav({}));
    // Every prefix holding the header and at least one whole frame decodes
    // the frames it holds and says it is truncated; a shorter one holds no
    // frame of the bytes its data chunk declares and is refused.
    std::size_t recovered = 0u;
    for (std::size_t length = 1u; length < valid.size(); ++length) {
        INFO("prefix of " << length << " bytes");
        const auto prefix = std::span(valid).first(length);
        const auto info = pulp::audio::inspect_wav(prefix);
        const auto decoded = pulp::audio::decode_wav(prefix);
        if (length < kHeader + kFrameBytes) {
            CHECK_FALSE(info);
            CHECK_FALSE(decoded);
            continue;
        }
        const auto whole_frames = (length - kHeader) / kFrameBytes;
        REQUIRE(info);
        CHECK(info->truncated);
        CHECK(info->num_frames == whole_frames);
        CHECK(info->declared_frames == 2u);
        REQUIRE(decoded);
        CHECK(decoded->num_frames() == whole_frames);
        CHECK(decoded->channels[0][0] == 0.0f);
        ++recovered;
    }
    CHECK(recovered == 2u);
    const auto whole = pulp::audio::inspect_wav(valid);
    REQUIRE(whole);
    CHECK_FALSE(whole->truncated);
    CHECK(whole->declared_frames == 2u);

    auto corrupt = valid;
    corrupt[0] = 'N';
    CHECK_FALSE(pulp::audio::inspect_wav(corrupt));

    // A data size far past the file: the two frames present decode.
    corrupt = valid;
    set_u32(corrupt, 40u, 0xfffffff0u);
    const auto lying = pulp::audio::inspect_wav(corrupt);
    REQUIRE(lying);
    CHECK(lying->truncated);
    CHECK(lying->num_frames == 2u);
    CHECK(lying->declared_frames == 0xfffffff0u / 2u);
    const auto lying_decoded = pulp::audio::decode_wav(corrupt);
    REQUIRE(lying_decoded);
    CHECK(lying_decoded->channels[0] == std::vector<float>{0.0f, 1.0f / 32768.0f});

    // Three data bytes of two-byte frames: one frame and a partial one.
    const auto partial_frame = make_wav(make_format(1u, 1u, 48'000u, 16u), Bytes{0u, 0u, 0u});
    const auto partial = pulp::audio::inspect_wav(partial_frame);
    REQUIRE(partial);
    CHECK(partial->truncated);
    CHECK(partial->num_frames == 1u);
    CHECK(partial->declared_frames == 2u);
    REQUIRE(pulp::audio::decode_wav(partial_frame));

    // A data chunk that declares bytes and holds no whole frame is refused;
    // zero frames are valid only when the data chunk declares zero bytes.
    const auto no_frame = make_wav(make_format(1u, 1u, 48'000u, 16u), Bytes{0u});
    CHECK_FALSE(pulp::audio::inspect_wav(no_frame));
    CHECK_FALSE(pulp::audio::decode_wav(no_frame));
}

TEST_CASE("byte-span WAV decoder treats a wrong RIFF size as a warning only",
          "[audio][wav-decoder]") {
    Bytes samples;
    append_sample(samples, std::int16_t{0});
    const auto valid = make_wav(make_format(1u, 1u, 48'000u, 16u), samples);

    auto undersized_envelope = valid;
    set_u32(undersized_envelope, 4u, 28u); // WAVE + complete fmt chunk only.
    const auto undersized = pulp::audio::inspect_wav(undersized_envelope);
    REQUIRE(undersized);
    CHECK(undersized->num_frames == 1u);
    CHECK_FALSE(undersized->truncated);
    REQUIRE(pulp::audio::decode_wav(undersized_envelope));

    auto trailing_chunk = valid;
    const Bytes payload{0x42u, 0x43u};
    append_chunk(trailing_chunk, "JUNK", payload);
    const auto trailing = pulp::audio::inspect_wav(trailing_chunk);
    REQUIRE(trailing);
    CHECK(trailing->num_frames == 1u);
    CHECK_FALSE(trailing->truncated);
    REQUIRE(pulp::audio::decode_wav(trailing_chunk));
}

TEST_CASE("byte-span WAV decoder accepts a repeated fmt and refuses a contradicting one",
          "[audio][wav-decoder]") {
    Bytes samples;
    append_sample(samples, std::int16_t{8192});
    const auto format = make_format(1u, 1u, 48'000u, 16u);

    const auto repeated = make_wav_with_chunks(format, samples, {{"fmt ", format}});
    const auto info = pulp::audio::inspect_wav(repeated);
    REQUIRE(info);
    CHECK(info->num_frames == 1u);
    const auto decoded = pulp::audio::decode_wav(repeated);
    REQUIRE(decoded);
    CHECK(decoded->channels[0][0] == 0.25f);

    // Any interpreted field differing is a contradiction: rate, channels,
    // width, block align, or the tag.
    const std::vector<Bytes> contradictions = {
        make_format(1u, 1u, 44'100u, 16u), make_format(1u, 2u, 48'000u, 16u),
        make_format(1u, 1u, 48'000u, 8u), make_format(3u, 1u, 48'000u, 16u)};
    for (const auto& second : contradictions) {
        const auto bytes = make_wav_with_chunks(format, samples, {{"fmt ", second}});
        CHECK_FALSE(pulp::audio::inspect_wav(bytes));
        CHECK_FALSE(pulp::audio::decode_wav(bytes));
    }
    // The byte rate is not interpreted, so a repeat differing only there is
    // still a repeat.
    auto byte_rate_only = format;
    byte_rate_only[8] ^= 0x01u;
    REQUIRE(
        pulp::audio::decode_wav(make_wav_with_chunks(format, samples, {{"fmt ", byte_rate_only}})));
}

TEST_CASE("byte-span WAV decoder reads RF64 and BW64 like their RIFF twin",
          "[audio][wav-decoder][rf64]") {
    Bytes samples;
    for (int i = 0; i < 6; ++i)
        append_sample(samples, static_cast<std::int16_t>(i * 1000 - 2500));
    const auto format = make_format(1u, 2u, 44'100u, 16u);
    const auto riff = make_wav(format, samples);
    const auto rf64 = make_rf64(format, samples);
    const auto bw64 = [&] {
        auto bytes = rf64;
        std::copy_n("BW64", 4, bytes.begin());
        return bytes;
    }();

    const auto expected_info = pulp::audio::inspect_wav(riff);
    const auto expected = pulp::audio::decode_wav(riff);
    REQUIRE(expected_info);
    REQUIRE(expected);
    for (const auto* envelope : {&rf64, &bw64}) {
        INFO((*envelope)[0] << (*envelope)[1] << (*envelope)[2] << (*envelope)[3]);
        const auto info = pulp::audio::inspect_wav(*envelope);
        REQUIRE(info);
        CHECK(info->sample_rate == expected_info->sample_rate);
        CHECK(info->num_channels == expected_info->num_channels);
        CHECK(info->num_frames == expected_info->num_frames);
        CHECK(info->bits_per_sample == expected_info->bits_per_sample);
        CHECK_FALSE(info->truncated);
        const auto decoded = pulp::audio::decode_wav(*envelope);
        REQUIRE(decoded);
        REQUIRE(decoded->channels.size() == expected->channels.size());
        for (std::size_t c = 0; c < expected->channels.size(); ++c) {
            REQUIRE(decoded->channels[c].size() == expected->channels[c].size());
            CHECK(std::memcmp(decoded->channels[c].data(), expected->channels[c].data(),
                              expected->channels[c].size() * sizeof(float)) == 0);
        }
    }

    // RF64 under 4 GiB may carry real 32-bit sizes beside ds64's.
    const auto plain_sizes = make_rf64(format, samples, static_cast<std::uint32_t>(samples.size()));
    REQUIRE(pulp::audio::decode_wav(plain_sizes));
    // ...but the two must agree.
    const auto conflicting =
        make_rf64(format, samples, static_cast<std::uint32_t>(samples.size()), samples.size() + 4u);
    CHECK_FALSE(pulp::audio::inspect_wav(conflicting));
    CHECK_FALSE(pulp::audio::decode_wav(conflicting));

    // A ds64 data size past the end of the file recovers the frames present.
    const auto past_eof = make_rf64(format, samples, 0xffffffffu, samples.size() + 400u);
    const auto truncated = pulp::audio::inspect_wav(past_eof);
    REQUIRE(truncated);
    CHECK(truncated->truncated);
    CHECK(truncated->num_frames == 3u);
    CHECK(truncated->declared_frames == (samples.size() + 400u) / 4u);
    const auto recovered = pulp::audio::decode_wav(past_eof);
    REQUIRE(recovered);
    CHECK(std::memcmp(recovered->channels[0].data(), expected->channels[0].data(),
                      3u * sizeof(float)) == 0);

    // An RF64 envelope with no ds64 resolves nothing: the sentinel data
    // size runs to the end of the file, which is recovered and reported.
    Bytes no_ds64;
    append_id(no_ds64, "RF64");
    append_u32(no_ds64, 0xffffffffu);
    append_id(no_ds64, "WAVE");
    append_chunk(no_ds64, "fmt ", format);
    append_id(no_ds64, "data");
    append_u32(no_ds64, 0xffffffffu);
    no_ds64.insert(no_ds64.end(), samples.begin(), samples.end());
    const auto sentinel = pulp::audio::inspect_wav(no_ds64);
    REQUIRE(sentinel);
    CHECK(sentinel->truncated);
    CHECK(sentinel->num_frames == 3u);
}

TEST_CASE("byte-span WAV decoder applies hostile-input allocation ceilings",
          "[audio][wav-decoder]") {
    Bytes samples;
    append_sample(samples, std::int16_t{0});
    append_sample(samples, std::int16_t{1});
    const auto wav = make_wav(make_format(1u, 1u, 48'000u, 16u), samples);

    auto limits = pulp::audio::WavDecodeLimits{};
    limits.max_frames = 1u;
    CHECK_FALSE(pulp::audio::decode_wav(wav, limits));

    limits = {};
    limits.max_channels = 0u;
    CHECK_FALSE(pulp::audio::decode_wav(wav, limits));

    Bytes stereo_samples;
    append_sample(stereo_samples, std::int16_t{0});
    append_sample(stereo_samples, std::int16_t{0});
    const auto stereo_wav = make_wav(make_format(1u, 2u, 48'000u, 16u), stereo_samples);
    limits = {};
    limits.max_channels = 1u;
    CHECK_FALSE(pulp::audio::decode_wav(stereo_wav, limits));

    limits = {};
    limits.max_output_bytes = sizeof(float);
    CHECK_FALSE(pulp::audio::decode_wav(wav, limits));

    limits = {};
    limits.max_frames = 2u;
    limits.max_channels = 1u;
    limits.max_output_bytes = 2u * sizeof(float);
    REQUIRE(pulp::audio::decode_wav(wav, limits));
}

namespace {

// The refusal the shared policy reaches for these bytes, and the reason the
// layout report prints for it.
pulp::audio::WaveRefusal refusal_of(std::span<const std::uint8_t> bytes) {
    const auto layout = pulp::audio::walk_wave_layout(pulp::audio::WaveByteCursor::over(bytes));
    std::optional<pulp::audio::WavFormatFacts> facts;
    if (layout.refusal == pulp::audio::WaveRefusal::none)
        facts = pulp::audio::interpret_wav_format(layout.fmt.bytes(), 0u);
    return pulp::audio::decide_wave(layout, facts).refusal;
}

std::string report_reason(std::span<const std::uint8_t> bytes) {
    const auto report = pulp::audio::inspect_audio_file_layout(bytes);
    return report.decode ? report.decode->reason : std::string();
}

} // namespace

TEST_CASE("byte-span WAV decoder names the fmt field it refuses", "[audio][wav-decoder][refusal]") {
    using pulp::audio::wave_refusal_reason;
    using pulp::audio::WaveRefusal;
    Bytes samples(4u, 0u);
    // dr_wav declines these at open; the reason still names the field.
    const auto no_channels = make_wav(make_format(1u, 0u, 48'000u, 16u), samples);
    CHECK(refusal_of(no_channels) == WaveRefusal::zero_channels);
    CHECK(report_reason(no_channels) == "zero channels");
    CHECK(report_reason(no_channels) == wave_refusal_reason(WaveRefusal::zero_channels));
    CHECK_FALSE(pulp::audio::inspect_wav(no_channels));

    const auto no_rate = make_wav(make_format(1u, 1u, 0u, 16u), samples);
    CHECK(refusal_of(no_rate) == WaveRefusal::zero_sample_rate);
    CHECK(report_reason(no_rate) == "zero sample rate");
    CHECK_FALSE(pulp::audio::decode_wav(no_rate));

    const auto wide = make_wav(make_format(1u, 257u, 48'000u, 16u), Bytes(257u * 2u, 0u));
    CHECK(refusal_of(wide) == WaveRefusal::too_many_channels);
    CHECK(report_reason(wide) == "more than 256 channels");

    // The control: a well-formed file is not refused at all.
    CHECK(refusal_of(make_wav(make_format(1u, 1u, 48'000u, 16u), samples)) == WaveRefusal::none);
}

TEST_CASE("byte-span WAV decoder refuses an MS ADPCM block too small for its headers",
          "[audio][wav-decoder][adpcm]") {
    using pulp::audio::WaveRefusal;
    // MS ADPCM carries a 7-byte header per channel at the start of every
    // block, so a stereo block needs 14 bytes; 13 cannot hold it.
    auto adpcm = [](std::uint16_t block_align) {
        auto format = make_format(2u, 2u, 22'050u, 4u);
        format[12] = static_cast<std::uint8_t>(block_align);
        format[13] = static_cast<std::uint8_t>(block_align >> 8u);
        return make_wav(format, Bytes(static_cast<std::size_t>(block_align) * 2u, 0u));
    };
    const auto short_block = adpcm(13u);
    CHECK(refusal_of(short_block) == WaveRefusal::block_align_too_small);
    CHECK_FALSE(pulp::audio::inspect_wav(short_block));
    CHECK_FALSE(pulp::audio::decode_wav(short_block));

    // The smallest block that holds both headers decodes their two samples.
    const auto header_only = adpcm(14u);
    const auto info = pulp::audio::inspect_wav(header_only);
    REQUIRE(info);
    CHECK(info->num_frames == 4u);
    const auto decoded = pulp::audio::decode_wav(header_only);
    REQUIRE(decoded);
    CHECK(decoded->num_frames() == 4u);
}

TEST_CASE("an RF64 data size of 0xFFFFFFFF with a zero ds64 entry runs to the end of the file",
          "[audio][wav-decoder][rf64]") {
    // A recorder that stopped before back-filling ds64 leaves its data size
    // at zero: that resolves nothing, so the data runs to EOF and the file is
    // reported truncated, as it is with no ds64 at all.
    Bytes samples;
    for (int i = 0; i < 6; ++i)
        append_sample(samples, static_cast<std::int16_t>(i * 1000));
    const auto format = make_format(1u, 2u, 44'100u, 16u);
    auto bytes = make_rf64(format, samples);
    constexpr std::size_t kDs64DataSize = 12u + 8u + 8u;
    for (std::size_t i = 0; i < 8u; ++i)
        bytes[kDs64DataSize + i] = 0u;

    const auto layout = pulp::audio::walk_wave_layout(pulp::audio::WaveByteCursor::over(bytes));
    CHECK(layout.has_warning(pulp::audio::LayoutWarningKind::sentinel_size_without_ds64));
    const auto info = pulp::audio::inspect_wav(bytes);
    REQUIRE(info);
    CHECK(info->truncated);
    CHECK(info->num_frames == 3u);
    CHECK(info->declared_frames == 0xffffffffu / 4u + 1u);
    const auto decoded = pulp::audio::decode_wav(bytes);
    REQUIRE(decoded);
    CHECK(decoded->channels[1][2] == 5000.0f / 32768.0f);
}
