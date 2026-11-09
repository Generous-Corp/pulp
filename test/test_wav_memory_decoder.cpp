#include <catch2/catch_test_macros.hpp>

#include <pulp/audio/wav_decoder.hpp>

#include "support/wav_fixture_builders.hpp"

#include <bit>
#include <cstddef>
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

TEST_CASE("byte-span WAV decoder rejects truncation and lying sizes", "[audio][wav-decoder]") {
    Bytes samples;
    append_sample(samples, std::int16_t{0});
    append_sample(samples, std::int16_t{1});
    const auto valid = make_wav(make_format(1u, 1u, 48'000u, 16u), samples);

    CHECK_FALSE(pulp::audio::inspect_wav({}));
    CHECK_FALSE(pulp::audio::decode_wav({}));
    for (std::size_t length = 1u; length < valid.size(); ++length) {
        const auto prefix = std::span(valid).first(length);
        CHECK_FALSE(pulp::audio::inspect_wav(prefix));
        CHECK_FALSE(pulp::audio::decode_wav(prefix));
    }

    auto corrupt = valid;
    corrupt[0] = 'N';
    CHECK_FALSE(pulp::audio::inspect_wav(corrupt));
    corrupt = valid;
    set_u32(corrupt, 40u, 0xfffffff0u);
    CHECK_FALSE(pulp::audio::decode_wav(corrupt));

    const auto partial_frame = make_wav(make_format(1u, 1u, 48'000u, 16u), Bytes{0u, 0u, 0u});
    CHECK_FALSE(pulp::audio::inspect_wav(partial_frame));
    CHECK_FALSE(pulp::audio::decode_wav(partial_frame));
}

TEST_CASE("byte-span WAV decoder enforces the exact RIFF envelope", "[audio][wav-decoder]") {
    Bytes samples;
    append_sample(samples, std::int16_t{0});
    const auto valid = make_wav(make_format(1u, 1u, 48'000u, 16u), samples);

    auto undersized_envelope = valid;
    set_u32(undersized_envelope, 4u, 28u); // WAVE + complete fmt chunk only.
    CHECK_FALSE(pulp::audio::inspect_wav(undersized_envelope));
    CHECK_FALSE(pulp::audio::decode_wav(undersized_envelope));

    auto trailing_chunk = valid;
    const Bytes payload{0x42u, 0x43u};
    append_chunk(trailing_chunk, "JUNK", payload);
    CHECK_FALSE(pulp::audio::inspect_wav(trailing_chunk));
    CHECK_FALSE(pulp::audio::decode_wav(trailing_chunk));
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
