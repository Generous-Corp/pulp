#include <catch2/catch_test_macros.hpp>

#include <pulp/audio/wav_decoder.hpp>

#include <bit>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <vector>

namespace {

using Bytes = std::vector<std::uint8_t>;

void append_id(Bytes& bytes, const char (&id)[5]) {
    for (std::size_t i = 0; i < 4u; ++i)
        bytes.push_back(static_cast<std::uint8_t>(id[i]));
}

void append_u16(Bytes& bytes, std::uint16_t value) {
    bytes.push_back(static_cast<std::uint8_t>(value));
    bytes.push_back(static_cast<std::uint8_t>(value >> 8u));
}

void append_u32(Bytes& bytes, std::uint32_t value) {
    for (unsigned shift = 0; shift < 32u; shift += 8u)
        bytes.push_back(static_cast<std::uint8_t>(value >> shift));
}

void set_u32(Bytes& bytes, std::size_t offset, std::uint32_t value) {
    for (unsigned shift = 0; shift < 32u; shift += 8u)
        bytes[offset + shift / 8u] = static_cast<std::uint8_t>(value >> shift);
}

void append_chunk(Bytes& bytes, const char (&id)[5],
                  std::span<const std::uint8_t> payload) {
    append_id(bytes, id);
    append_u32(bytes, static_cast<std::uint32_t>(payload.size()));
    bytes.insert(bytes.end(), payload.begin(), payload.end());
    if ((payload.size() & 1u) != 0u)
        bytes.push_back(0u);
}

Bytes make_format(std::uint16_t encoding, std::uint16_t channels,
                  std::uint32_t sample_rate, std::uint16_t bits) {
    Bytes format;
    const auto align = static_cast<std::uint16_t>(channels * (bits / 8u));
    append_u16(format, encoding);
    append_u16(format, channels);
    append_u32(format, sample_rate);
    append_u32(format, sample_rate * align);
    append_u16(format, align);
    append_u16(format, bits);
    return format;
}

Bytes make_wav(const Bytes& format, const Bytes& data,
               bool data_first = false, bool odd_junk = false) {
    Bytes bytes;
    append_id(bytes, "RIFF");
    append_u32(bytes, 0u);
    append_id(bytes, "WAVE");
    if (odd_junk) {
        const Bytes junk{0x55u};
        append_chunk(bytes, "JUNK", junk);
    }
    if (data_first)
        append_chunk(bytes, "data", data);
    append_chunk(bytes, "fmt ", format);
    if (!data_first)
        append_chunk(bytes, "data", data);
    set_u32(bytes, 4u, static_cast<std::uint32_t>(bytes.size() - 8u));
    return bytes;
}

template <typename T>
void append_sample(Bytes& bytes, T value) {
    if constexpr (sizeof(T) == 2u) {
        append_u16(bytes, std::bit_cast<std::uint16_t>(value));
    } else if constexpr (sizeof(T) == 4u) {
        append_u32(bytes, std::bit_cast<std::uint32_t>(value));
    } else {
        const auto bits = std::bit_cast<std::uint64_t>(value);
        append_u32(bytes, static_cast<std::uint32_t>(bits));
        append_u32(bytes, static_cast<std::uint32_t>(bits >> 32u));
    }
}

} // namespace

const char* consumer_drwav_version();

TEST_CASE("byte-span WAV decoder inspects and deinterleaves PCM16",
          "[audio][wav-decoder]") {
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
    CHECK(decoded->channels[0] ==
          std::vector<float>{-1.0f, 32767.0f / 32768.0f});
    CHECK(decoded->channels[1] == std::vector<float>{0.5f, -0.5f});
}

TEST_CASE("byte-span WAV decoder supports PCM and float sample widths",
          "[audio][wav-decoder]") {
    SECTION("PCM8") {
        const Bytes samples{0x00u, 0x80u, 0xffu};
        const auto decoded = pulp::audio::decode_wav(
            make_wav(make_format(1u, 1u, 8'000u, 8u), samples));
        REQUIRE(decoded);
        CHECK(decoded->channels[0][0] == -1.0f);
        CHECK(decoded->channels[0][1] == 0.0f);
        CHECK(decoded->channels[0][2] == 127.0f / 128.0f);
    }
    SECTION("PCM24") {
        const Bytes samples{0x00u, 0x00u, 0x80u, 0xffu, 0xffu, 0x7fu};
        const auto decoded = pulp::audio::decode_wav(
            make_wav(make_format(1u, 1u, 44'100u, 24u), samples));
        REQUIRE(decoded);
        CHECK(decoded->channels[0][0] == -1.0f);
        CHECK(decoded->channels[0][1] == 8388607.0f / 8388608.0f);
    }
    SECTION("PCM32") {
        Bytes samples;
        append_sample(samples, std::int32_t{-2147483647 - 1});
        append_sample(samples, std::int32_t{1073741824});
        const auto decoded = pulp::audio::decode_wav(
            make_wav(make_format(1u, 1u, 96'000u, 32u), samples));
        REQUIRE(decoded);
        CHECK(decoded->channels[0] == std::vector<float>{-1.0f, 0.5f});
    }
    SECTION("IEEE float32") {
        Bytes samples;
        append_sample(samples, 1.25f);
        append_sample(samples, -0.25f);
        const auto decoded = pulp::audio::decode_wav(
            make_wav(make_format(3u, 1u, 48'000u, 32u), samples));
        REQUIRE(decoded);
        CHECK(decoded->channels[0] == std::vector<float>{1.25f, -0.25f});
    }
    SECTION("IEEE float64") {
        Bytes samples;
        append_sample(samples, 0.125);
        append_sample(samples, -2.0);
        const auto decoded = pulp::audio::decode_wav(
            make_wav(make_format(3u, 1u, 48'000u, 64u), samples));
        REQUIRE(decoded);
        CHECK(decoded->channels[0] == std::vector<float>{0.125f, -2.0f});
    }
}

TEST_CASE("byte-span WAV decoder handles chunk order and odd padding",
          "[audio][wav-decoder]") {
    Bytes samples;
    append_sample(samples, std::int16_t{8192});
    const auto decoded = pulp::audio::decode_wav(
        make_wav(make_format(1u, 1u, 22'050u, 16u), samples, false, true));
    REQUIRE(decoded);
    CHECK(decoded->channels[0][0] == 0.25f);
}

TEST_CASE("byte-span WAV decoder rejects truncation and lying sizes",
          "[audio][wav-decoder]") {
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
}

TEST_CASE("byte-span WAV decoder enforces the exact RIFF envelope",
          "[audio][wav-decoder]") {
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

    limits = {};
    limits.max_output_bytes = sizeof(float);
    CHECK_FALSE(pulp::audio::decode_wav(wav, limits));

    limits = {};
    limits.max_frames = 2u;
    limits.max_channels = 1u;
    limits.max_output_bytes = 2u * sizeof(float);
    REQUIRE(pulp::audio::decode_wav(wav, limits));
}
