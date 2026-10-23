#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <pulp/audio/audio_file.hpp>
#include <pulp/audio/format_registry.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#ifdef _WIN32
#include <process.h>
#else
#include <unistd.h>
#endif

namespace {

namespace fs = std::filesystem;
using Catch::Matchers::WithinAbs;
using pulp::audio::AudioFileData;
using pulp::audio::DecodeLimits;
using pulp::audio::FormatRegistry;

uint64_t current_process_id() {
#ifdef _WIN32
    return static_cast<uint64_t>(_getpid());
#else
    return static_cast<uint64_t>(getpid());
#endif
}

struct TempDir {
    fs::path path;

    TempDir() {
        static std::atomic<uint64_t> next_id{0};
        auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        auto unique_id = next_id.fetch_add(1, std::memory_order_relaxed);
        path = fs::temp_directory_path() /
               ("pulp-format-registry-limits-" + std::to_string(current_process_id()) + "-" +
                std::to_string(stamp) + "-" + std::to_string(unique_id));
        fs::create_directories(path);
    }

    ~TempDir() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
};

void write_bytes(const fs::path& path, const std::vector<uint8_t>& bytes) {
    std::ofstream output(path, std::ios::binary);
    REQUIRE(output.good());
    output.write(reinterpret_cast<const char*>(bytes.data()),
                 static_cast<std::streamsize>(bytes.size()));
    REQUIRE(output.good());
}

// FLAC frame header CRC: polynomial 0x07, no init, no reflection.
uint8_t flac_crc8(const std::vector<uint8_t>& bytes) {
    uint8_t crc = 0;
    for (uint8_t byte : bytes) {
        crc ^= byte;
        for (int bit = 0; bit < 8; ++bit)
            crc = static_cast<uint8_t>((crc & 0x80u) ? ((crc << 1) ^ 0x07u) : (crc << 1));
    }
    return crc;
}

// FLAC frame footer CRC: polynomial 0x8005, no init, no reflection.
uint16_t flac_crc16(const std::vector<uint8_t>& bytes) {
    uint16_t crc = 0;
    for (uint8_t byte : bytes) {
        crc ^= static_cast<uint16_t>(byte << 8);
        for (int bit = 0; bit < 8; ++bit)
            crc = static_cast<uint16_t>((crc & 0x8000u) ? ((crc << 1) ^ 0x8005u) : (crc << 1));
    }
    return crc;
}

constexpr uint32_t kFixtureSampleRate = 44100u;
constexpr uint32_t kFixtureChannels = 2u;
constexpr uint32_t kFixtureBlockFrames = 192u;
constexpr float kFixtureLeft = 4096.0f / 32768.0f;
constexpr float kFixtureRight = -4096.0f / 32768.0f;

// A native FLAC stream: "fLaC", one STREAMINFO block (44.1 kHz, stereo,
// 16-bit) whose total-samples field is whatever the caller claims, and
// optionally one fixed-blocksize frame of 192 CONSTANT samples per channel.
// The claim is the attack surface; the frame proves the stream is otherwise
// real and decodable.
std::vector<uint8_t> build_flac(uint64_t claimed_total_samples, bool with_frame) {
    std::vector<uint8_t> out = {'f', 'L', 'a', 'C'};
    // Block header: last-block flag set, type 0 (STREAMINFO), length 34.
    out.insert(out.end(), {0x80, 0x00, 0x00, 0x22});
    // Min/max block size.
    out.insert(out.end(), {0x00, 0xC0, 0x00, 0xC0});
    // Min/max frame size: unknown.
    out.insert(out.end(), 6, 0x00);
    // 20 bits sample rate | 3 bits channels-1 | 5 bits bps-1 | 36 bits total samples.
    const uint64_t props = (static_cast<uint64_t>(kFixtureSampleRate) << 44) |
                           (static_cast<uint64_t>(kFixtureChannels - 1u) << 41) |
                           (static_cast<uint64_t>(15u) << 36) |
                           (claimed_total_samples & 0xFFFFFFFFFull);
    for (int shift = 56; shift >= 0; shift -= 8)
        out.push_back(static_cast<uint8_t>((props >> shift) & 0xFFu));
    // MD5 signature: unset.
    out.insert(out.end(), 16, 0x00);

    if (!with_frame)
        return out;

    // Frame header: sync + fixed blocksize, blocksize code 192, rate code
    // 44.1 kHz, independent stereo, 16-bit, frame number 0, then CRC-8.
    std::vector<uint8_t> frame = {0xFF, 0xF8, 0x19, 0x18, 0x00};
    frame.push_back(flac_crc8(frame));
    // Two CONSTANT subframes (header 0x00, 16-bit value): +4096 and -4096.
    frame.insert(frame.end(), {0x00, 0x10, 0x00, 0x00, 0xF0, 0x00});
    const uint16_t crc16 = flac_crc16(frame);
    frame.push_back(static_cast<uint8_t>(crc16 >> 8));
    frame.push_back(static_cast<uint8_t>(crc16 & 0xFFu));

    out.insert(out.end(), frame.begin(), frame.end());
    return out;
}

void push_be16(std::vector<uint8_t>& out, uint16_t value) {
    out.push_back(static_cast<uint8_t>(value >> 8));
    out.push_back(static_cast<uint8_t>(value & 0xFFu));
}

void push_be32(std::vector<uint8_t>& out, uint32_t value) {
    for (int shift = 24; shift >= 0; shift -= 8)
        out.push_back(static_cast<uint8_t>((value >> shift) & 0xFFu));
}

void push_le16(std::vector<uint8_t>& out, uint16_t value) {
    out.push_back(static_cast<uint8_t>(value & 0xFFu));
    out.push_back(static_cast<uint8_t>(value >> 8));
}

void push_le32(std::vector<uint8_t>& out, uint32_t value) {
    for (int shift = 0; shift <= 24; shift += 8)
        out.push_back(static_cast<uint8_t>((value >> shift) & 0xFFu));
}

// A 16-bit PCM AIFF: COMM claims `channels` x `claimed_frames` and SSND
// carries `payload` bytes of big-endian samples (possibly none).
std::vector<uint8_t> build_aiff(uint16_t channels, uint32_t claimed_frames,
                                const std::vector<uint8_t>& payload) {
    std::vector<uint8_t> body = {'A', 'I', 'F', 'F'};
    body.insert(body.end(), {'C', 'O', 'M', 'M'});
    push_be32(body, 18u);
    push_be16(body, channels);
    push_be32(body, claimed_frames);
    push_be16(body, 16u);
    // 80-bit extended 44100.0: exponent 0x400E, mantissa 0xAC44 << 48.
    body.insert(body.end(), {0x40, 0x0E, 0xAC, 0x44, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00});
    body.insert(body.end(), {'S', 'S', 'N', 'D'});
    push_be32(body, static_cast<uint32_t>(8u + payload.size()));
    push_be32(body, 0u);
    push_be32(body, 0u);
    body.insert(body.end(), payload.begin(), payload.end());
    if (payload.size() % 2u != 0u)
        body.push_back(0x00);

    std::vector<uint8_t> out = {'F', 'O', 'R', 'M'};
    push_be32(out, static_cast<uint32_t>(body.size()));
    out.insert(out.end(), body.begin(), body.end());
    return out;
}

// A 16-bit PCM WAV whose data chunk declares `data_bytes` but carries none.
std::vector<uint8_t> build_wav_header_only(uint16_t channels, uint32_t data_bytes) {
    std::vector<uint8_t> out = {'R', 'I', 'F', 'F'};
    push_le32(out, 36u + data_bytes);
    out.insert(out.end(), {'W', 'A', 'V', 'E', 'f', 'm', 't', ' '});
    push_le32(out, 16u);
    push_le16(out, 1u);
    push_le16(out, channels);
    push_le32(out, kFixtureSampleRate);
    push_le32(out, kFixtureSampleRate * channels * 2u);
    push_le16(out, static_cast<uint16_t>(channels * 2u));
    push_le16(out, 16u);
    out.insert(out.end(), {'d', 'a', 't', 'a'});
    push_le32(out, data_bytes);
    return out;
}

void require_fixture_frame(const AudioFileData& data) {
    REQUIRE(data.sample_rate == kFixtureSampleRate);
    REQUIRE(data.channels.size() == kFixtureChannels);
    REQUIRE(data.num_frames() == kFixtureBlockFrames);
    REQUIRE_THAT(data.channels[0].front(), WithinAbs(kFixtureLeft, 1e-6));
    REQUIRE_THAT(data.channels[0].back(), WithinAbs(kFixtureLeft, 1e-6));
    REQUIRE_THAT(data.channels[1].front(), WithinAbs(kFixtureRight, 1e-6));
    REQUIRE_THAT(data.channels[1].back(), WithinAbs(kFixtureRight, 1e-6));
}

} // namespace

TEST_CASE("FormatRegistry refuses a FLAC whose STREAMINFO claims more output than the decode "
          "limits allow",
          "[audio][flac][decode-limits]") {
    auto& registry = FormatRegistry::instance();
    TempDir temp;
    const auto path = (temp.path / "huge-claim.flac").string();
    constexpr uint64_t kClaimed = 0xFFFFFFFFFull;
    write_bytes(path, build_flac(kClaimed, true));

    // Control: the claim reaches the reader intact, so a refusal below is
    // the limit check and not an open failure.
    auto info = registry.read_info(path);
    REQUIRE(info.has_value());
    REQUIRE(info->num_frames == kClaimed);
    REQUIRE(info->num_channels == kFixtureChannels);

    std::optional<AudioFileData> decoded;
    REQUIRE_NOTHROW(decoded = registry.read(path));
    REQUIRE_FALSE(decoded.has_value());
}

TEST_CASE(
    "FormatRegistry decodes a FLAC inside the default decode limits and honors custom ceilings",
    "[audio][flac][decode-limits]") {
    auto& registry = FormatRegistry::instance();
    TempDir temp;
    const auto path = (temp.path / "exact-claim.flac").string();
    write_bytes(path, build_flac(kFixtureBlockFrames, true));

    SECTION("default limits decode the whole stream") {
        auto decoded = registry.read(path);
        REQUIRE(decoded.has_value());
        require_fixture_frame(*decoded);
    }

    SECTION("a frame ceiling below the claim refuses the stream") {
        DecodeLimits limits;
        limits.max_frames = kFixtureBlockFrames - 1u;
        REQUIRE_FALSE(registry.read(path, limits).has_value());
    }

    SECTION("a channel ceiling below the stream refuses it") {
        DecodeLimits limits;
        limits.max_channels = 1u;
        REQUIRE_FALSE(registry.read(path, limits).has_value());
    }

    SECTION("an output-byte ceiling below the decoded size refuses it") {
        DecodeLimits limits;
        limits.max_output_bytes =
            static_cast<uint64_t>(kFixtureBlockFrames) * kFixtureChannels * sizeof(float) - 1u;
        REQUIRE_FALSE(registry.read(path, limits).has_value());
    }

    SECTION("a ceiling that exactly fits the stream decodes it") {
        DecodeLimits limits;
        limits.max_frames = kFixtureBlockFrames;
        limits.max_channels = kFixtureChannels;
        limits.max_output_bytes =
            static_cast<uint64_t>(kFixtureBlockFrames) * kFixtureChannels * sizeof(float);
        auto decoded = registry.read(path, limits);
        REQUIRE(decoded.has_value());
        require_fixture_frame(*decoded);
    }
}

TEST_CASE("FormatRegistry bounds a FLAC of unknown length by its decoded frame count",
          "[audio][flac][decode-limits]") {
    auto& registry = FormatRegistry::instance();
    TempDir temp;
    const auto path = (temp.path / "unknown-length.flac").string();
    write_bytes(path, build_flac(0u, true));

    auto decoded = registry.read(path);
    REQUIRE(decoded.has_value());
    require_fixture_frame(*decoded);

    DecodeLimits limits;
    limits.max_frames = kFixtureBlockFrames - 1u;
    REQUIRE_FALSE(registry.read(path, limits).has_value());
}

TEST_CASE("FormatRegistry returns nothing for a FLAC with a modest claim and no frames",
          "[audio][flac][decode-limits]") {
    auto& registry = FormatRegistry::instance();
    TempDir temp;
    const auto path = (temp.path / "frameless.flac").string();
    write_bytes(path, build_flac(kFixtureBlockFrames, false));

    auto info = registry.read_info(path);
    REQUIRE(info.has_value());
    REQUIRE(info->num_frames == kFixtureBlockFrames);

    std::optional<AudioFileData> decoded;
    REQUIRE_NOTHROW(decoded = registry.read(path));
    REQUIRE_FALSE(decoded.has_value());
}

TEST_CASE("FormatRegistry treats a zero decode limit as unlimited",
          "[audio][flac][decode-limits]") {
    auto& registry = FormatRegistry::instance();
    TempDir temp;
    const auto path = (temp.path / "exact-claim.flac").string();
    write_bytes(path, build_flac(kFixtureBlockFrames, true));

    DecodeLimits unlimited;
    unlimited.max_frames = 0u;
    unlimited.max_channels = 0u;
    unlimited.max_output_bytes = 0u;
    auto decoded = registry.read(path, unlimited);
    REQUIRE(decoded.has_value());
    require_fixture_frame(*decoded);
}

TEST_CASE("FormatRegistry reads a small WAV and refuses one beyond custom ceilings",
          "[audio][wav][decode-limits]") {
    auto& registry = FormatRegistry::instance();
    TempDir temp;
    const auto path = (temp.path / "small.wav").string();

    AudioFileData source;
    source.sample_rate = 48000u;
    source.channels.assign(2, std::vector<float>(64, 0.0f));
    for (size_t i = 0; i < 64; ++i) {
        source.channels[0][i] = 0.25f;
        source.channels[1][i] = -0.25f;
    }
    REQUIRE(registry.write(path, source));

    auto decoded = registry.read(path);
    REQUIRE(decoded.has_value());
    REQUIRE(decoded->sample_rate == 48000u);
    REQUIRE(decoded->channels.size() == 2u);
    REQUIRE(decoded->num_frames() == 64u);
    REQUIRE_THAT(decoded->channels[0][10], WithinAbs(0.25, 1e-3));
    REQUIRE_THAT(decoded->channels[1][10], WithinAbs(-0.25, 1e-3));

    auto decoded_with_limits = registry.read(path, DecodeLimits{});
    REQUIRE(decoded_with_limits.has_value());
    REQUIRE(decoded_with_limits->num_frames() == 64u);

    DecodeLimits frame_ceiling;
    frame_ceiling.max_frames = 63u;
    REQUIRE_FALSE(registry.read(path, frame_ceiling).has_value());

    DecodeLimits channel_ceiling;
    channel_ceiling.max_channels = 1u;
    REQUIRE_FALSE(registry.read(path, channel_ceiling).has_value());

    DecodeLimits byte_ceiling;
    byte_ceiling.max_output_bytes = 64u * 2u * sizeof(float) - 1u;
    REQUIRE_FALSE(registry.read(path, byte_ceiling).has_value());
}

// A 16-bit stereo PCM WAV of `frames` frames with a constant sample value.
std::vector<uint8_t> build_wav_frames(uint32_t frames, int16_t value) {
    auto out = build_wav_header_only(2u, frames * 2u * 2u);
    for (uint32_t i = 0; i < frames * 2u; ++i)
        push_le16(out, static_cast<uint16_t>(value));
    return out;
}

TEST_CASE("read_audio_file decodes the header it admitted rather than one a later open would see",
          "[audio][wav][decode-limits]") {
    using pulp::audio::AudioFileInfo;
    TempDir temp;
    const auto path = temp.path / "swapped.wav";
    const auto bigger = temp.path / "bigger.wav";
    write_bytes(path, build_wav_frames(64u, 4096));
    write_bytes(bigger, build_wav_frames(4096u, -4096));

    // Control: the admission sees the small header, and refusing it refuses
    // the file.
    size_t admitted_calls = 0;
    auto refused = pulp::audio::read_audio_file(path.string(), [&](const AudioFileInfo& info) {
        ++admitted_calls;
        REQUIRE(info.num_frames == 64u);
        REQUIRE(info.num_channels == 2u);
        return false;
    });
    REQUIRE(admitted_calls == 1u);
    REQUIRE_FALSE(refused.has_value());

    // While the small header is being admitted, the path is replaced by a file
    // whose header claims sixty-four times as many frames. The decode must
    // come from the open that was admitted: 64 frames of the original value.
    // (Replacing a file that is open fails on Windows; the admitted decode
    // then trivially matches, and the ancestor-check still proves the seam.)
    bool replaced = false;
    auto decoded = pulp::audio::read_audio_file(path.string(), [&](const AudioFileInfo& info) {
        REQUIRE(info.num_frames == 64u);
        std::error_code ec;
        fs::rename(bigger, path, ec);
        replaced = !ec;
        return true;
    });
#ifndef _WIN32
    REQUIRE(replaced);
#endif
    REQUIRE(decoded.has_value());
    REQUIRE(decoded->channels.size() == 2u);
    REQUIRE(decoded->num_frames() == 64u);
    // Int16 decode scales by 1/32767 or 1/32768 depending on the reader.
    REQUIRE_THAT(decoded->channels[0].front(), WithinAbs(0.125, 1e-4));
    REQUIRE_THAT(decoded->channels[1].back(), WithinAbs(0.125, 1e-4));

    // The replaced path now carries the bigger file, so a fresh open sees it.
    if (replaced) {
        auto info = pulp::audio::read_audio_file_info(path.string());
        REQUIRE(info.has_value());
        REQUIRE(info->num_frames == 4096u);
    }
}

TEST_CASE(
    "FormatRegistry refuses a WAV header that claims more frames than the decode limits allow",
    "[audio][wav][decode-limits]") {
    auto& registry = FormatRegistry::instance();
    TempDir temp;
    const auto path = (temp.path / "huge-claim.wav").string();
    // Two 16-bit channels with a 4 GiB data claim: ~2^30 frames, well past
    // the default frame ceiling.
    write_bytes(path, build_wav_header_only(2u, 0xFFFFFFF0u));

    std::optional<AudioFileData> decoded;
    REQUIRE_NOTHROW(decoded = registry.read(path));
    REQUIRE_FALSE(decoded.has_value());
}

TEST_CASE(
    "FormatRegistry refuses an AIFF header that claims more output than the decode limits allow",
    "[audio][aiff][decode-limits]") {
    auto& registry = FormatRegistry::instance();
    TempDir temp;
    const auto path = (temp.path / "huge-claim.aiff").string();
    write_bytes(path, build_aiff(64u, 0x80000000u, {}));

    // Control: the header parses and the claim reaches the reader intact.
    auto info = registry.read_info(path);
    REQUIRE(info.has_value());
    REQUIRE(info->num_channels == 64u);
    REQUIRE(info->num_frames == 0x80000000u);

    std::optional<AudioFileData> decoded;
    REQUIRE_NOTHROW(decoded = registry.read(path));
    REQUIRE_FALSE(decoded.has_value());
}

TEST_CASE("FormatRegistry decodes a small AIFF and refuses it beyond custom ceilings",
          "[audio][aiff][decode-limits]") {
    auto& registry = FormatRegistry::instance();
    TempDir temp;
    const auto path = (temp.path / "small.aiff").string();
    // Four stereo frames of +4096 / -4096.
    std::vector<uint8_t> payload;
    for (int frame = 0; frame < 4; ++frame)
        payload.insert(payload.end(), {0x10, 0x00, 0xF0, 0x00});
    write_bytes(path, build_aiff(2u, 4u, payload));

    auto decoded = registry.read(path);
    REQUIRE(decoded.has_value());
    REQUIRE(decoded->channels.size() == 2u);
    REQUIRE(decoded->num_frames() == 4u);
    REQUIRE_THAT(decoded->channels[0][3], WithinAbs(kFixtureLeft, 1e-6));
    REQUIRE_THAT(decoded->channels[1][3], WithinAbs(kFixtureRight, 1e-6));

    DecodeLimits frame_ceiling;
    frame_ceiling.max_frames = 3u;
    REQUIRE_FALSE(registry.read(path, frame_ceiling).has_value());

    DecodeLimits channel_ceiling;
    channel_ceiling.max_channels = 1u;
    REQUIRE_FALSE(registry.read(path, channel_ceiling).has_value());

    DecodeLimits byte_ceiling;
    byte_ceiling.max_output_bytes = 4u * 2u * sizeof(float) - 1u;
    REQUIRE_FALSE(registry.read(path, byte_ceiling).has_value());
}

TEST_CASE("FormatRegistry decodes a FLAC claiming a sixty-minute recording through the default "
          "limits while a 36-bit claim is still refused",
          "[audio][flac][decode-limits]") {
    auto& registry = FormatRegistry::instance();
    TempDir temp;
    // Sixty minutes of stereo at 96 kHz, as a real session would claim.
    constexpr uint64_t kHourAt96k = 345'600'000ull;
    const auto long_path = (temp.path / "sixty-minutes.flac").string();
    write_bytes(long_path, build_flac(kHourAt96k, true));

    auto decoded = registry.read(long_path);
    REQUIRE(decoded.has_value());
    require_fixture_frame(*decoded);

    const auto hostile_path = (temp.path / "huge-claim.flac").string();
    write_bytes(hostile_path, build_flac(0xFFFFFFFFFull, true));
    std::optional<AudioFileData> hostile;
    REQUIRE_NOTHROW(hostile = registry.read(hostile_path));
    REQUIRE_FALSE(hostile.has_value());
}

TEST_CASE("DecodeLimits::unbounded accepts a claim above a custom ceiling at the pre-check",
          "[audio][flac][decode-limits]") {
    auto& registry = FormatRegistry::instance();
    TempDir temp;
    constexpr uint64_t kHourAt96k = 345'600'000ull;
    const auto path = (temp.path / "sixty-minutes.flac").string();
    write_bytes(path, build_flac(kHourAt96k, true));

    // Below the claimed output: the pre-check refuses before any frame decodes.
    DecodeLimits below_claim;
    below_claim.max_output_bytes = kHourAt96k * kFixtureChannels * sizeof(float) - 1u;
    REQUIRE_FALSE(registry.read(path, below_claim).has_value());

    const auto unbounded = DecodeLimits::unbounded();
    REQUIRE(unbounded.max_frames == 0u);
    REQUIRE(unbounded.max_channels == 0u);
    REQUIRE(unbounded.max_output_bytes == 0u);
    auto decoded = registry.read(path, unbounded);
    REQUIRE(decoded.has_value());
    require_fixture_frame(*decoded);
}
