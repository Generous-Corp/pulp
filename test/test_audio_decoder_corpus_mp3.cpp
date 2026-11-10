// The decoder corpus replay's MP3 cases: declared and scanned lengths, tag
// and trailing-byte handling, and the sniffing of PCM that looks like frames.
// The cross-format replay is in test_audio_decoder_corpus.cpp.
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

TEST_CASE("an MP3 whose Xing header declares more frames than the stream delivers is refused",
          "[fuzz][audio][mp3]") {
    // The Xing VBR seed cut at 80%: the header declares 6400 frames and the
    // stream delivers a few dozen.
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

uint32_t le32(std::vector<uint8_t>::const_iterator at) {
    return uint32_t(at[0]) | (uint32_t(at[1]) << 8) | (uint32_t(at[2]) << 16) |
           (uint32_t(at[3]) << 24);
}

// The bytes in front of a trailing APE tag. The footer's size field (bytes
// 12-15) counts the items and the footer, and is never less than the footer's
// own 32 bytes. An APEv2 tag (version 2000 or later, bytes 8-11) also has a
// 32-byte header in front of the items when flags bit 31 (footer byte 23, bit
// 7) is set; APEv1 never has one.
std::vector<uint8_t> strip_ape_tag(const std::vector<uint8_t>& bytes) {
    REQUIRE(bytes.size() > 32u);
    const auto footer = bytes.cbegin() + static_cast<std::ptrdiff_t>(bytes.size() - 32u);
    REQUIRE(std::string(footer, footer + 8) == "APETAGEX");
    const uint32_t version = le32(footer + 8);
    const uint32_t tag_size = std::max<uint32_t>(le32(footer + 12), 32u);
    const uint32_t header_size = version >= 2000u && (footer[23] & 0x80u) != 0 ? 32u : 0u;
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
    // APEv2 with a header, APEv1 without one, APEv1 whose flags claim a header
    // anyway, a footer whose size is below its own 32 bytes, and an APEv2 tag
    // whose size byte has its high bit set. Each stripped stream is the
    // untagged base.
    const auto base = read_bytes(corpus_dir() / "mp3-cbr-mono.mp3");
    pulp::test::ScopedTempDir dir("pulp-audio-decoder-ape");
    auto& registry = pulp::audio::FormatRegistry::instance();
    const auto limits = pulp_fuzz::audio_decoder::fuzz_limits();
    for (const char* name :
         {"mp3-apev2-footer.mp3", "mp3-apev1-footer.mp3", "mp3-apev1-header-flag.mp3",
          "mp3-ape-size-below-footer.mp3", "mp3-apev2-size-high-bit.mp3"}) {
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

TEST_CASE("an MP3 followed by bytes no tag reader claims decodes exactly as the stream alone",
          "[fuzz][audio][mp3]") {
    // A Lyrics3v2 block before an ID3v1 tag, plain junk, and an ID3v1 tag in
    // front of an APE tag: each leaves bytes behind the last frame that are
    // not a frame. The stream must end there with every frame decoded.
    const auto base = read_bytes(corpus_dir() / "mp3-cbr-mono.mp3");
    auto& registry = pulp::audio::FormatRegistry::instance();
    const auto limits = pulp_fuzz::audio_decoder::fuzz_limits();
    const auto base_path = (corpus_dir() / "mp3-cbr-mono.mp3").string();
    const auto base_info = registry.read_info(base_path);
    const auto base_data = registry.read(base_path, limits);
    REQUIRE(base_info.has_value());
    REQUIRE(base_data.has_value());
    auto base_reader = pulp::audio::AudioReader::open_memory(base, ".mp3", limits);
    REQUIRE(base_reader != nullptr);
    const auto base_range = base_reader->read(0u, base_reader->info().num_frames);
    REQUIRE(base_range.has_value());
    for (const char* name : {"mp3-lyrics3v2-before-id3v1.mp3", "mp3-trailing-junk.mp3",
                             "mp3-id3v1-before-apev2.mp3"}) {
        INFO(name);
        const auto path = corpus_dir() / name;
        const auto bytes = read_bytes(path);
        REQUIRE(bytes.size() > base.size());
        REQUIRE(std::equal(base.begin(), base.end(), bytes.begin()));

        const auto info = registry.read_info(path.string());
        REQUIRE(info.has_value());
        CHECK(info->num_frames == base_info->num_frames);
        const auto data = registry.read(path.string(), limits);
        REQUIRE(data.has_value());
        require_same_audio(*data, *base_data);

        auto reader = pulp::audio::AudioReader::open_memory(bytes, ".mp3", limits);
        REQUIRE(reader != nullptr);
        REQUIRE(reader->info().num_frames == base_reader->info().num_frames);
        const auto range = reader->read(0u, reader->info().num_frames);
        REQUIRE(range.has_value());
        require_same_audio(*range, *base_range);
    }
}

namespace {

// 64 KiB of 16-bit little-endian PCM silence with +-2 LSB of dither, from a
// fixed splitmix64 seed. The first sample is -1, so the buffer starts with
// the bytes FF FF: an 11-bit frame sync that names Layer I. Such buffers are
// full of byte pairs that read as Layer I and II frame headers.
std::vector<uint8_t> dithered_pcm_silence(uint64_t seed) {
    std::vector<uint8_t> bytes(64 * 1024);
    for (std::size_t i = 0; i < bytes.size(); i += 2) {
        uint64_t z = (seed += 0x9E3779B97F4A7C15ull);
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
        z ^= z >> 31;
        const auto sample = static_cast<int16_t>(i == 0 ? -1 : static_cast<int>(z % 5) - 2);
        const auto raw = static_cast<uint16_t>(sample);
        bytes[i] = static_cast<uint8_t>(raw & 0xFF);
        bytes[i + 1] = static_cast<uint8_t>(raw >> 8);
    }
    return bytes;
}

} // namespace

TEST_CASE("dithered PCM silence without a format hint is not sniffed as MP3",
          "[fuzz][audio][mp3]") {
    // dr_mp3 accepts this buffer as a run of Layer I frames when told it is
    // MP3. A bare frame sync at the start used to be enough to sniff it as
    // MP3; only a Layer III header is now.
    const auto bytes = dithered_pcm_silence(5);
    REQUIRE(bytes[0] == 0xFF);
    REQUIRE((bytes[1] & 0xE0) == 0xE0);
    CHECK(pulp::audio::AudioReader::open_memory(bytes) == nullptr);
    CHECK(pulp::audio::AudioReader::open_memory(bytes, ".bin") == nullptr);

    // With an .mp3 hint the decoder still takes the false Layer I sync it
    // has always taken: Layer I and II keep dr_mp3's strict frame-chain rule.
    auto hinted = pulp::audio::AudioReader::open_memory(bytes, ".mp3");
    REQUIRE(hinted != nullptr);
    CHECK(hinted->info().num_frames == 2304u);
}

TEST_CASE("dithered PCM silence with an .mp3 hint gains no frames from the trailing-byte rules",
          "[fuzz][audio][mp3]") {
    // The Layer I headers in this buffer fail dr_mp3's strict frame-chain
    // rule. Extending the trailing-byte relaxation to Layer I would accept it
    // as about 150000 frames of noise.
    const auto bytes = dithered_pcm_silence(1);
    CHECK(pulp::audio::AudioReader::open_memory(bytes, ".mp3") == nullptr);
    CHECK(pulp::audio::AudioReader::open_memory(bytes) == nullptr);

    // dr_mp3 has always synced on this buffer as Layer I and decoded 1536
    // frames. Decoding a synced Layer I frame whatever follows it would
    // stretch the false run to 1920.
    const auto synced = dithered_pcm_silence(1402);
    auto reader = pulp::audio::AudioReader::open_memory(synced, ".mp3");
    REQUIRE(reader != nullptr);
    CHECK(reader->info().num_frames == 1536u);
}
