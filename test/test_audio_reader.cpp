// AudioReader: ranged reads from a path, memory and a custom byte source must
// equal a full sequential decode at every offset, including across codec
// frame boundaries and at end of stream, for WAV, AIFF, FLAC, MP3 and Ogg
// Vorbis built from one known signal.

#include "audio_reader_fixtures.hpp"
#include "support/fixture_root.hpp"

#include <catch2/catch_test_macros.hpp>

#include <pulp/audio/audio_reader.hpp>
#include <pulp/audio/buffer.hpp>
#include <pulp/audio/format_registry.hpp>
#include <pulp/audio/mmap_reader.hpp>
#include <pulp/audio/streaming_sample_source.hpp>
#include <pulp/audio/streaming_sample_source_file.hpp>
#include <pulp/runtime/base64.hpp>

// Declarations only; pulp-audio compiles the implementation once.
#include "../external/dr_libs/dr_mp3.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <numbers>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

namespace {

namespace fs = std::filesystem;
namespace fixtures = pulp::test::audio_reader_fixtures;
using pulp::audio::AudioByteSource;
using pulp::audio::AudioFileData;
using pulp::audio::AudioReader;
using pulp::audio::AudioReaderFallback;
using pulp::audio::DecodeLimits;

constexpr std::uint32_t kRate = 22050;
constexpr std::uint64_t kFrames = 5513;

// The fixture signal, before 16-bit quantization.
float expected_sample(std::uint64_t frame, std::uint32_t channel) {
    const double hz = channel == 0 ? 440.0 : 660.0;
    return static_cast<float>(
        0.5 * std::sin(2.0 * std::numbers::pi * hz * static_cast<double>(frame) / kRate));
}

struct TempDir {
    fs::path path;
    TempDir() {
        static std::atomic<std::uint64_t> next{0};
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        path = fs::temp_directory_path() /
               ("pulp-audio-reader-" + std::to_string(stamp) + "-" +
                std::to_string(next.fetch_add(1, std::memory_order_relaxed)));
        fs::create_directories(path);
    }
    ~TempDir() {
        std::error_code ignored;
        fs::remove_all(path, ignored);
    }
};

std::vector<std::uint8_t> decode_fixture(std::string_view encoded) {
    auto bytes = pulp::runtime::base64_decode(encoded);
    REQUIRE(bytes.has_value());
    return std::move(*bytes);
}

void write_bytes(const fs::path& path, const std::vector<std::uint8_t>& bytes) {
    std::ofstream out(path, std::ios::binary);
    out.write(reinterpret_cast<const char*>(bytes.data()),
              static_cast<std::streamsize>(bytes.size()));
    REQUIRE(out.good());
}

std::vector<std::uint8_t> read_bytes(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

AudioFileData make_signal() {
    AudioFileData data;
    data.sample_rate = kRate;
    data.channels.assign(2, std::vector<float>(kFrames));
    for (std::uint32_t ch = 0; ch < 2; ++ch)
        for (std::uint64_t i = 0; i < kFrames; ++i)
            data.channels[ch][i] = expected_sample(i, ch);
    return data;
}

void append_be16(std::vector<std::uint8_t>& bytes, std::uint16_t value) {
    bytes.push_back(static_cast<std::uint8_t>(value >> 8));
    bytes.push_back(static_cast<std::uint8_t>(value));
}

void append_be32(std::vector<std::uint8_t>& bytes, std::uint32_t value) {
    for (int shift = 24; shift >= 0; shift -= 8)
        bytes.push_back(static_cast<std::uint8_t>(value >> shift));
}

void append_le16(std::vector<std::uint8_t>& bytes, std::uint16_t value) {
    bytes.push_back(static_cast<std::uint8_t>(value));
    bytes.push_back(static_cast<std::uint8_t>(value >> 8));
}

void append_le32(std::vector<std::uint8_t>& bytes, std::uint32_t value) {
    for (int shift = 0; shift < 32; shift += 8)
        bytes.push_back(static_cast<std::uint8_t>(value >> shift));
}

// 80-bit IEEE extended, the AIFF sample-rate encoding.
void append_extended(std::vector<std::uint8_t>& bytes, double value) {
    int exponent = 0;
    const double mantissa = std::frexp(value, &exponent);
    exponent += 16383 - 1;
    const auto m = static_cast<std::uint64_t>(mantissa * std::pow(2.0, 64));
    bytes.push_back(static_cast<std::uint8_t>((exponent >> 8) & 0x7F));
    bytes.push_back(static_cast<std::uint8_t>(exponent & 0xFF));
    for (int shift = 56; shift >= 0; shift -= 8)
        bytes.push_back(static_cast<std::uint8_t>((m >> shift) & 0xFF));
}

void append_iff_chunk(std::vector<std::uint8_t>& bytes, const char (&id)[5],
                      const std::vector<std::uint8_t>& payload) {
    bytes.insert(bytes.end(), id, id + 4);
    append_be32(bytes, static_cast<std::uint32_t>(payload.size()));
    bytes.insert(bytes.end(), payload.begin(), payload.end());
    if (payload.size() & 1u)
        bytes.push_back(0);
}

// FORM/AIFC holding `pcm16` (interleaved 16-bit words, already in the byte
// order the compression type names) under a four-character compression type
// and an empty compression name.
std::vector<std::uint8_t> make_aifc(std::string_view compression, std::uint16_t channels,
                                    std::uint32_t frames, const std::vector<std::uint8_t>& pcm16) {
    REQUIRE(compression.size() == 4);
    std::vector<std::uint8_t> comm;
    append_be16(comm, channels);
    append_be32(comm, frames);
    append_be16(comm, 16);
    append_extended(comm, kRate);
    comm.insert(comm.end(), compression.begin(), compression.end());
    comm.push_back(0); // empty pstring
    comm.push_back(0); // pad to even
    std::vector<std::uint8_t> ssnd;
    append_be32(ssnd, 0);
    append_be32(ssnd, 0);
    ssnd.insert(ssnd.end(), pcm16.begin(), pcm16.end());
    std::vector<std::uint8_t> body{'A', 'I', 'F', 'C'};
    append_iff_chunk(body, "COMM", comm);
    append_iff_chunk(body, "SSND", ssnd);
    std::vector<std::uint8_t> bytes{'F', 'O', 'R', 'M'};
    append_be32(bytes, static_cast<std::uint32_t>(body.size()));
    bytes.insert(bytes.end(), body.begin(), body.end());
    return bytes;
}

// The fixture signal quantized the way the registry's AIFF writer quantizes,
// interleaved, in the requested byte order.
std::vector<std::uint8_t> signal_pcm16(bool little_endian) {
    const auto signal = make_signal();
    std::vector<std::uint8_t> pcm;
    for (std::uint64_t i = 0; i < kFrames; ++i) {
        for (std::uint32_t ch = 0; ch < 2; ++ch) {
            const float sample = std::clamp(signal.channels[ch][i], -1.0f, 1.0f);
            const auto value =
                static_cast<std::uint16_t>(static_cast<std::int16_t>(sample * 32767.0f));
            if (little_endian)
                append_le16(pcm, value);
            else
                append_be16(pcm, value);
        }
    }
    return pcm;
}

// RIFF/WAVE, 16-bit PCM at kRate, with an empty data chunk.
std::vector<std::uint8_t> make_zero_frame_wav(std::uint16_t channels) {
    std::vector<std::uint8_t> bytes{'R', 'I', 'F', 'F'};
    append_le32(bytes, 36);
    bytes.insert(bytes.end(), {'W', 'A', 'V', 'E', 'f', 'm', 't', ' '});
    append_le32(bytes, 16);
    append_le16(bytes, 1);
    append_le16(bytes, channels);
    append_le32(bytes, kRate);
    append_le32(bytes, kRate * channels * 2u);
    append_le16(bytes, static_cast<std::uint16_t>(channels * 2u));
    append_le16(bytes, 16);
    bytes.insert(bytes.end(), {'d', 'a', 't', 'a'});
    append_le32(bytes, 0);
    REQUIRE(bytes.size() == 44u);
    return bytes;
}

// A byte source that serves a vector through the abstract interface only, so
// the reader cannot take a memory fast path.
class VectorSource final : public AudioByteSource {
  public:
    explicit VectorSource(std::vector<std::uint8_t> bytes) : bytes_(std::move(bytes)) {}
    std::size_t read(void* destination, std::size_t bytes) override {
        const auto count = std::min<std::size_t>(bytes, bytes_.size() - position_);
        std::memcpy(destination, bytes_.data() + position_, count);
        position_ += count;
        return count;
    }
    bool seek(std::uint64_t position) override {
        if (position > bytes_.size())
            return false;
        position_ = static_cast<std::size_t>(position);
        return true;
    }
    std::uint64_t position() const override {
        return position_;
    }
    std::uint64_t size() const override {
        return bytes_.size();
    }

  private:
    std::vector<std::uint8_t> bytes_;
    std::size_t position_ = 0;
};

struct Format {
    const char* name;
    const char* extension;
    std::vector<std::uint8_t> bytes;
    bool exact_length; // false for MP3, whose decoder adds encoder delay
};

std::vector<Format> make_formats(const TempDir& dir) {
    // WAV and AIFF are written here from the same signal the compressed
    // fixtures were encoded from.
    auto& registry = pulp::audio::FormatRegistry::instance();
    const auto signal = make_signal();
    const auto wav = dir.path / "signal.wav";
    const auto aiff = dir.path / "signal.aiff";
    REQUIRE(registry.write(wav.string(), signal));
    REQUIRE(registry.write(aiff.string(), signal));
    std::vector<Format> formats;
    formats.push_back({"WAV", ".wav", read_bytes(wav), true});
    formats.push_back({"AIFF", ".aiff", read_bytes(aiff), true});
    formats.push_back({"FLAC", ".flac", decode_fixture(fixtures::kFlacBase64), true});
    formats.push_back({"MP3", ".mp3", decode_fixture(fixtures::kMp3Base64), false});
    formats.push_back({"OGG", ".ogg", decode_fixture(fixtures::kOggVorbisBase64), true});
    return formats;
}

AudioFileData full_decode(AudioReader& reader) {
    auto data = reader.read(0, reader.info().num_frames);
    REQUIRE(data.has_value());
    REQUIRE(data->num_frames() == reader.info().num_frames);
    return std::move(*data);
}

// Reads [offset, offset + frames) and checks it against `reference`,
// including the zero-filled tail past the end.
void check_range(AudioReader& reader, const AudioFileData& reference, std::uint64_t offset,
                 std::uint64_t frames, float tolerance) {
    const auto total = reference.num_frames();
    std::vector<std::vector<float>> out(2, std::vector<float>(frames, 99.0f));
    float* pointers[2] = {out[0].data(), out[1].data()};
    const auto expected = offset < total ? std::min(frames, total - offset) : 0u;
    INFO("offset " << offset << " frames " << frames);
    REQUIRE(reader.read(offset, pointers, 2, frames) == expected);
    for (std::uint32_t ch = 0; ch < 2; ++ch) {
        for (std::uint64_t i = 0; i < expected; ++i) {
            INFO("channel " << ch << " frame " << offset + i);
            REQUIRE(std::fabs(out[ch][i] - reference.channels[ch][offset + i]) <= tolerance);
        }
        for (std::uint64_t i = expected; i < frames; ++i)
            REQUIRE(out[ch][i] == 0.0f);
    }
}

void check_ranges(AudioReader& reader, const AudioFileData& reference) {
    const auto total = reference.num_frames();
    // Offsets straddle FLAC block (1024) and MP3 frame (576) boundaries; the
    // order goes backwards and forwards so every read seeks.
    const std::uint64_t offsets[] = {3000, 0,    1023, 1024, 575,       576,        2047,
                                     1,    4097, 1152, 2304, total - 1, total - 300};
    for (const auto offset : offsets)
        check_range(reader, reference, offset, 700, 1e-6f);
    // Sequential blocks continue from the cursor without seeking.
    for (std::uint64_t offset = 0; offset < total; offset += 512)
        check_range(reader, reference, offset, 512, 1e-6f);
    check_range(reader, reference, total, 64, 0.0f);
    check_range(reader, reference, total + 1000, 64, 0.0f);
}

} // namespace

TEST_CASE("AudioReader ranged reads equal the full decode for every seekable format",
          "[audio][audio-reader]") {
    TempDir dir;
    for (auto& format : make_formats(dir)) {
        SECTION(std::string(format.name)) {
            const auto path = dir.path / (std::string("fixture") + format.extension);
            write_bytes(path, format.bytes);

            auto from_file = AudioReader::open_file(path.string());
            REQUIRE(from_file);
            CHECK(from_file->supports_ranged_read());
            CHECK(from_file->info().format == format.name);
            CHECK(from_file->info().sample_rate == kRate);
            CHECK(from_file->info().num_channels == 2u);
            if (format.exact_length)
                CHECK(from_file->info().num_frames == kFrames);
            else
                CHECK(from_file->info().num_frames >= kFrames);

            const auto reference = full_decode(*from_file);
            check_ranges(*from_file, reference);

            auto from_memory = AudioReader::open_memory(format.bytes, format.extension);
            REQUIRE(from_memory);
            CHECK(from_memory->supports_ranged_read());
            check_ranges(*from_memory, reference);

            auto from_source =
                AudioReader::open_source(std::make_unique<VectorSource>(format.bytes));
            REQUIRE(from_source);
            CHECK(from_source->info().format == format.name);
            check_ranges(*from_source, reference);
        }
    }
}

TEST_CASE("AudioReader decodes the known signal from each format", "[audio][audio-reader]") {
    TempDir dir;
    for (auto& format : make_formats(dir)) {
        SECTION(std::string(format.name)) {
            auto reader = AudioReader::open_memory(format.bytes);
            REQUIRE(reader);
            const auto decoded = full_decode(*reader);
            if (format.exact_length) {
                if (std::string_view(format.name) != "OGG") {
                    // Lossless: within one 16-bit step of the source signal.
                    for (std::uint32_t ch = 0; ch < 2; ++ch)
                        for (std::uint64_t i = 0; i < kFrames; ++i)
                            REQUIRE(std::fabs(decoded.channels[ch][i] - expected_sample(i, ch)) <=
                                    1.5f / 32768.0f);
                }
            }
            // Lossy codecs keep the level of a 0.5 sine (RMS 0.354) in the
            // steady middle of the clip.
            for (std::uint32_t ch = 0; ch < 2; ++ch) {
                double energy = 0.0;
                for (std::uint64_t i = 1500; i < 4500; ++i)
                    energy += double(decoded.channels[ch][i]) * decoded.channels[ch][i];
                const double rms = std::sqrt(energy / 3000.0);
                INFO(format.name << " channel " << ch << " rms " << rms);
                CHECK(std::fabs(rms - 0.3536) < 0.05);
            }
        }
    }
}

TEST_CASE("AudioReader matches the registry's whole-file decode", "[audio][audio-reader]") {
    TempDir dir;
    for (auto& format : make_formats(dir)) {
        SECTION(std::string(format.name)) {
            const auto path = dir.path / (std::string("fixture") + format.extension);
            write_bytes(path, format.bytes);
            auto whole = pulp::audio::FormatRegistry::instance().read(path.string());
            REQUIRE(whole.has_value());
            auto reader = AudioReader::open_file(path.string());
            REQUIRE(reader);
            const auto ranged = full_decode(*reader);
            REQUIRE(ranged.num_frames() == whole->num_frames());
            // The registry's Ogg reader decodes through 16-bit integers.
            const float tolerance =
                std::string_view(format.name) == "OGG" ? 1.0f / 32768.0f : 1e-6f;
            for (std::uint32_t ch = 0; ch < 2; ++ch)
                for (std::uint64_t i = 0; i < ranged.num_frames(); ++i)
                    REQUIRE(std::fabs(ranged.channels[ch][i] - whole->channels[ch][i]) <=
                            tolerance);
        }
    }
}

TEST_CASE("AudioReader agrees with the registry on AIFF-C byte orders",
          "[audio][audio-reader][aifc-pcm]") {
    // `NONE` and `twos` are the same big-endian words; `sowt` is the same
    // words little-endian. Every spelling must decode, through the registry
    // and through the reader, to the identical floats.
    TempDir dir;
    const auto big_endian = signal_pcm16(false);
    const auto little_endian = signal_pcm16(true);
    for (const char* compression : {"NONE", "twos", "sowt"}) {
        SECTION(compression) {
            const bool sowt = std::string_view(compression) == "sowt";
            const auto bytes = make_aifc(compression, 2, static_cast<std::uint32_t>(kFrames),
                                         sowt ? little_endian : big_endian);
            const auto path = dir.path / (std::string(compression) + ".aifc");
            write_bytes(path, bytes);
            auto whole = pulp::audio::FormatRegistry::instance().read(path.string());
            REQUIRE(whole.has_value());
            REQUIRE(whole->num_frames() == kFrames);
            // Not silence, and exactly the 16-bit quantized fixture sample.
            const auto quantized = static_cast<std::int16_t>(
                std::clamp(expected_sample(100, 0), -1.0f, 1.0f) * 32767.0f);
            REQUIRE(quantized != 0);
            REQUIRE(whole->channels[0][100] == static_cast<float>(quantized) / 32768.0f);

            auto reader = AudioReader::open_file(path.string());
            REQUIRE(reader);
            REQUIRE(reader->info().num_frames == kFrames);
            const auto ranged = full_decode(*reader);
            REQUIRE(ranged.channels == whole->channels);
            check_ranges(*reader, *whole);

            auto memory = AudioReader::open_memory(bytes, ".aifc");
            REQUIRE(memory);
            REQUIRE(full_decode(*memory).channels == whole->channels);
        }
    }
}

TEST_CASE("AudioReader agrees with the registry that a zero-frame file is valid and empty",
          "[audio][audio-reader][zero-frames]") {
    TempDir dir;
    struct Case {
        const char* name;
        const char* extension;
        std::vector<std::uint8_t> bytes;
    };
    const Case cases[] = {
        {"WAV", ".wav", make_zero_frame_wav(2)},
        {"AIFF", ".aiff",
         [] {
             auto bytes = make_aifc("NONE", 2, 0, {});
             std::memcpy(bytes.data() + 8, "AIFF", 4); // plain AIFF: strip the compression type
             return bytes;
         }()},
        {"AIFF-C sowt", ".aifc", make_aifc("sowt", 2, 0, {})},
    };
    for (const auto& item : cases) {
        SECTION(item.name) {
            const auto path = dir.path / (std::string("empty") + item.extension);
            write_bytes(path, item.bytes);
            auto whole = pulp::audio::FormatRegistry::instance().read(path.string());
            REQUIRE(whole.has_value());
            REQUIRE(whole->sample_rate == kRate);
            REQUIRE(whole->num_channels() == 2);
            REQUIRE(whole->num_frames() == 0);

            for (const auto fallback :
                 {AudioReaderFallback::DecodeOnce, AudioReaderFallback::Refuse}) {
                auto reader = AudioReader::open_file(path.string(), {}, fallback);
                REQUIRE(reader);
                REQUIRE(reader->info().sample_rate == kRate);
                REQUIRE(reader->info().num_channels == 2);
                REQUIRE(reader->info().num_frames == 0);
                REQUIRE(reader->info().duration_seconds == 0.0);
                std::vector<std::vector<float>> out(2, std::vector<float>(16, 9.0f));
                float* pointers[2] = {out[0].data(), out[1].data()};
                REQUIRE(reader->read(0, pointers, 2, 16) == 0);
                for (std::uint32_t ch = 0; ch < 2; ++ch)
                    for (float sample : out[ch])
                        REQUIRE(sample == 0.0f);
                const auto all = reader->read(0, 16);
                REQUIRE(all.has_value());
                REQUIRE(all->num_frames() == 0);
                REQUIRE(all->num_channels() == 2);
            }
            auto memory = AudioReader::open_memory(item.bytes, item.extension);
            REQUIRE(memory);
            REQUIRE(memory->info().num_frames == 0);
        }
    }
    SECTION("a data claim the file does not hold is still refused") {
        // Same 44 bytes, but the data chunk claims 4 GiB that is not there:
        // no whole frame of a non-zero declaration, so neither reader may
        // call it empty.
        auto lying = make_zero_frame_wav(2);
        lying[40] = 0xF0;
        lying[41] = 0xFF;
        lying[42] = 0xFF;
        lying[43] = 0xFF;
        const auto path = dir.path / "lying.wav";
        write_bytes(path, lying);
        REQUIRE_FALSE(pulp::audio::FormatRegistry::instance().read(path.string()).has_value());
        REQUIRE_FALSE(AudioReader::open_file(path.string()));
        REQUIRE_FALSE(AudioReader::open_memory(lying, ".wav"));
    }
}

TEST_CASE("AudioReader reads a channel subset and zero-fills surplus channels",
          "[audio][audio-reader]") {
    const auto bytes = decode_fixture(fixtures::kFlacBase64);
    auto reader = AudioReader::open_memory(bytes);
    REQUIRE(reader);
    const auto reference = full_decode(*reader);

    std::vector<float> left(5000, 9.0f);
    float* mono[1] = {left.data()};
    REQUIRE(reader->read(100, mono, 1, left.size()) == left.size());
    for (std::size_t i = 0; i < left.size(); ++i)
        REQUIRE(left[i] == reference.channels[0][100 + i]);

    std::vector<std::vector<float>> wide(3, std::vector<float>(10, 9.0f));
    float* three[3] = {wide[0].data(), wide[1].data(), wide[2].data()};
    REQUIRE(reader->read(2000, three, 3, 10) == 10u);
    for (std::size_t i = 0; i < 10; ++i) {
        REQUIRE(wide[1][i] == reference.channels[1][2000 + i]);
        REQUIRE(wide[2][i] == 0.0f);
    }
}

TEST_CASE("AudioReader enforces decode limits on open and on allocating reads",
          "[audio][audio-reader][limits]") {
    const auto bytes = decode_fixture(fixtures::kFlacBase64);

    DecodeLimits few_frames;
    few_frames.max_frames = kFrames - 1;
    CHECK_FALSE(AudioReader::open_memory(bytes, ".flac", few_frames));

    DecodeLimits mono_only;
    mono_only.max_channels = 1;
    CHECK_FALSE(AudioReader::open_memory(bytes, ".flac", mono_only));

    DecodeLimits small_reads = DecodeLimits::unbounded();
    small_reads.max_output_bytes = 1000u * 2u * sizeof(float);
    auto reader = AudioReader::open_memory(bytes, ".flac", small_reads);
    REQUIRE(reader);
    CHECK(reader->read(0, 1000).has_value());
    CHECK_FALSE(reader->read(0, 1001).has_value());
    // Caller-owned buffers are not bounded by the output ceiling.
    std::vector<std::vector<float>> out(2, std::vector<float>(kFrames));
    float* pointers[2] = {out[0].data(), out[1].data()};
    CHECK(reader->read(0, pointers, 2, kFrames) == kFrames);

    // An Ogg byte source is copied into memory, bounded by the same ceiling.
    const auto ogg = decode_fixture(fixtures::kOggVorbisBase64);
    DecodeLimits tiny = DecodeLimits::unbounded();
    tiny.max_output_bytes = ogg.size() - 1;
    CHECK_FALSE(AudioReader::open_source(std::make_unique<VectorSource>(ogg), ".ogg", tiny));
    tiny.max_output_bytes = ogg.size();
    CHECK(AudioReader::open_source(std::make_unique<VectorSource>(ogg), ".ogg", tiny));
}

TEST_CASE("AudioReader refuses unknown, empty and truncated input", "[audio][audio-reader]") {
    const std::vector<std::uint8_t> garbage(512, 0x42);
    CHECK_FALSE(AudioReader::open_memory(garbage));
    CHECK_FALSE(AudioReader::open_memory(std::span<const std::uint8_t>{}));
    CHECK_FALSE(AudioReader::open_source(nullptr));
    CHECK_FALSE(AudioReader::open_file("/no/such/file.flac"));

    // A FLAC cut mid-stream opens (its header still claims the full length)
    // and decodes short, zero-filling what the cut removed.
    auto flac = decode_fixture(fixtures::kFlacBase64);
    flac.resize(flac.size() / 2);
    auto reader = AudioReader::open_memory(flac, ".flac");
    REQUIRE(reader);
    std::vector<std::vector<float>> out(2, std::vector<float>(kFrames, 9.0f));
    float* pointers[2] = {out[0].data(), out[1].data()};
    const auto got = reader->read(0, pointers, 2, kFrames);
    CHECK(got < kFrames);
    for (std::uint64_t i = got; i < kFrames; ++i)
        REQUIRE(out[0][i] == 0.0f);
}

TEST_CASE("Memory-mapped readers range-read compressed files without a whole-file decode",
          "[audio][audio-reader][mmap]") {
    TempDir dir;
    for (auto& format : make_formats(dir)) {
        const std::string_view name = format.name;
        if (name != "FLAC" && name != "MP3" && name != "OGG")
            continue;
        SECTION(std::string(name)) {
            const auto path = dir.path / (std::string("fixture") + format.extension);
            write_bytes(path, format.bytes);
            auto reader = AudioReader::open_file(path.string());
            REQUIRE(reader);
            const auto reference = full_decode(*reader);
            const auto total = reference.num_frames();

            pulp::audio::MemoryMappedAudioReader mapped;
            REQUIRE(mapped.open(path.string()));
            REQUIRE(mapped.supports_ranged_read());
            CHECK(mapped.info().num_frames == total);
            std::vector<std::vector<float>> out(2, std::vector<float>(900));
            float* pointers[2] = {out[0].data(), out[1].data()};
            for (const std::uint64_t offset :
                 {std::uint64_t{2500}, std::uint64_t{17}, total - 900}) {
                REQUIRE(mapped.read_frames_ranged_only(pointers, 2, offset, 900));
                for (std::uint32_t ch = 0; ch < 2; ++ch)
                    for (std::uint64_t i = 0; i < 900; ++i)
                        REQUIRE(std::fabs(out[ch][i] - reference.channels[ch][offset + i]) <=
                                1e-6f);
            }

            // The streaming factory now admits compressed files under the
            // ranged-only policy and plays them through the streaming source.
            auto frame_reader = pulp::audio::make_memory_mapped_frame_reader(path.string(), true);
            REQUIRE(frame_reader.valid);
            REQUIRE(frame_reader.supports_ranged_read);
            REQUIRE(frame_reader.total_frames == total);

            pulp::audio::StreamingSampleSourceConfig config;
            config.channels = frame_reader.channels;
            config.total_frames = frame_reader.total_frames;
            config.sample_rate = frame_reader.sample_rate;
            config.preload_frames = 1024;
            config.ring_capacity_frames = 4096;
            config.read_chunk_frames = 1024;
            config.start_background_thread = false;
            pulp::audio::StreamingSampleSource source;
            REQUIRE(source.prepare(config, frame_reader.reader));

            constexpr std::uint64_t kBlock = 256;
            pulp::audio::Buffer<float> block(2, kBlock);
            std::uint64_t position = 0;
            while (position < total) {
                while (source.pump_background() > 0) {
                }
                const auto want = std::min(kBlock, total - position);
                REQUIRE(source.pull(block.view(), want) == want);
                for (std::uint32_t ch = 0; ch < 2; ++ch)
                    for (std::uint64_t i = 0; i < want; ++i)
                        REQUIRE(std::fabs(block.channel(ch)[i] -
                                          reference.channels[ch][position + i]) <= 1e-6f);
                position += want;
            }
            CHECK(source.finished());
        }
    }
}

namespace {

// Clears the 36-bit total-samples field of a native FLAC's STREAMINFO, which
// FLAC allows for a stream whose length was unknown when it was encoded.
std::vector<std::uint8_t> flac_without_length() {
    auto bytes = decode_fixture(fixtures::kFlacBase64);
    // "fLaC", the 4-byte block header, then 10 bytes of block and frame
    // sizes; the total sample count is the low 36 bits of the next 8 bytes.
    constexpr std::size_t kPacked = 4 + 4 + 10;
    REQUIRE(bytes.size() > kPacked + 8);
    bytes[kPacked + 3] &= 0xF0;
    for (std::size_t i = 4; i < 8; ++i)
        bytes[kPacked + i] = 0;
    return bytes;
}

} // namespace

TEST_CASE("AudioReader decodes a stream with no length claim once and serves it",
          "[audio][audio-reader]") {
    const auto bytes = flac_without_length();
    auto reader = AudioReader::open_memory(bytes, ".flac");
    REQUIRE(reader);
    CHECK_FALSE(reader->supports_ranged_read());
    CHECK(reader->info().num_frames == kFrames);

    const auto ranged_bytes = decode_fixture(fixtures::kFlacBase64);
    auto ranged = AudioReader::open_memory(ranged_bytes);
    REQUIRE(ranged);
    const auto reference = full_decode(*ranged);
    check_range(*reader, reference, 4000, 700, 0.0f);
    check_range(*reader, reference, 17, 300, 0.0f);

    // The one-time decode is held to every ceiling, max_output_bytes included.
    DecodeLimits small = DecodeLimits::unbounded();
    small.max_output_bytes = 1000u * 2u * sizeof(float);
    CHECK_FALSE(AudioReader::open_memory(bytes, ".flac", small));
}

TEST_CASE("Memory-mapped readers keep the decode-once fallback for a FLAC with no length claim",
          "[audio][audio-reader][mmap]") {
    TempDir dir;
    const auto path = dir.path / "unknown_length.flac";
    write_bytes(path, flac_without_length());

    pulp::audio::MemoryMappedAudioReader mapped;
    REQUIRE(mapped.open(path.string()));
    CHECK_FALSE(mapped.supports_ranged_read());
    CHECK(mapped.info().num_frames == kFrames);
    std::vector<std::vector<float>> out(2, std::vector<float>(64));
    float* pointers[2] = {out[0].data(), out[1].data()};
    CHECK_FALSE(mapped.read_frames_ranged_only(pointers, 2, 100, 64));
    REQUIRE(mapped.read_frames(pointers, 2, 100, 64));
    CHECK(std::fabs(out[0][0] - expected_sample(100, 0)) <= 1.5f / 32768.0f);

    // The strict streaming policy still refuses it.
    CHECK_FALSE(pulp::audio::make_memory_mapped_frame_reader(path.string(), true).valid);
    const auto permissive = pulp::audio::make_memory_mapped_frame_reader(path.string());
    REQUIRE(permissive.valid);
    CHECK_FALSE(permissive.supports_ranged_read);
    CHECK(permissive.total_frames == kFrames);
}

namespace {

// Once armed, fails every read that reaches past `fail_from` until `heal()`
// is called, as a network cache or flaky disk might. Seeks always succeed.
class FlakySource final : public AudioByteSource {
  public:
    FlakySource(std::vector<std::uint8_t> bytes, std::size_t fail_from)
        : bytes_(std::move(bytes)), fail_from_(fail_from) {}
    std::size_t read(void* destination, std::size_t bytes) override {
        auto count = std::min<std::size_t>(bytes, bytes_.size() - position_);
        if (failing_ && position_ + count > fail_from_)
            count = position_ < fail_from_ ? fail_from_ - position_ : 0;
        std::memcpy(destination, bytes_.data() + position_, count);
        position_ += count;
        return count;
    }
    bool seek(std::uint64_t position) override {
        if (position > bytes_.size())
            return false;
        position_ = static_cast<std::size_t>(position);
        return true;
    }
    std::uint64_t position() const override {
        return position_;
    }
    std::uint64_t size() const override {
        return bytes_.size();
    }
    void arm() {
        failing_ = true;
    }
    void heal() {
        failing_ = false;
    }

  private:
    std::vector<std::uint8_t> bytes_;
    std::size_t fail_from_;
    std::size_t position_ = 0;
    bool failing_ = false;
};

void put_le32(std::vector<std::uint8_t>& bytes, std::size_t at, std::uint32_t value) {
    for (int i = 0; i < 4; ++i)
        bytes[at + i] = static_cast<std::uint8_t>(value >> (8 * i));
}

void put_le64(std::vector<std::uint8_t>& bytes, std::size_t at, std::uint64_t value) {
    for (int i = 0; i < 8; ++i)
        bytes[at + i] = static_cast<std::uint8_t>(value >> (8 * i));
}

// Rewrites a canonical RIFF WAV (fmt then data) as RF64 whose ds64 chunk
// claims `claimed_frames`, more than the bytes hold.
std::vector<std::uint8_t> as_rf64(const std::vector<std::uint8_t>& wav,
                                  std::uint64_t claimed_frames, std::uint32_t frame_bytes) {
    // Locate fmt and data in the RIFF file.
    std::size_t fmt_at = 0, data_at = 0;
    std::uint32_t fmt_size = 0, data_size = 0;
    for (std::size_t pos = 12; pos + 8 <= wav.size();) {
        const std::uint32_t size = wav[pos + 4] | (wav[pos + 5] << 8) | (wav[pos + 6] << 16) |
                                   (static_cast<std::uint32_t>(wav[pos + 7]) << 24);
        if (std::memcmp(&wav[pos], "fmt ", 4) == 0) {
            fmt_at = pos;
            fmt_size = size;
        } else if (std::memcmp(&wav[pos], "data", 4) == 0) {
            data_at = pos;
            data_size = size;
            break;
        }
        pos += 8 + size + (size & 1u);
    }
    REQUIRE(fmt_at != 0);
    REQUIRE(data_at != 0);
    std::vector<std::uint8_t> out(12 + 36);
    std::memcpy(out.data(), "RF64", 4);
    put_le32(out, 4, 0xFFFFFFFFu);
    std::memcpy(out.data() + 8, "WAVE", 4);
    std::memcpy(out.data() + 12, "ds64", 4);
    put_le32(out, 16, 28);
    // RIFF size is filled in once the file is assembled.
    put_le64(out, 28, claimed_frames * frame_bytes); // data size claim
    put_le64(out, 36, claimed_frames);               // sample count claim
    put_le32(out, 44, 0);                            // table length
    out.insert(out.end(), wav.begin() + static_cast<std::ptrdiff_t>(fmt_at),
               wav.begin() + static_cast<std::ptrdiff_t>(fmt_at + 8 + fmt_size));
    const std::uint8_t data_header[8] = {'d', 'a', 't', 'a', 0xFF, 0xFF, 0xFF, 0xFF};
    out.insert(out.end(), data_header, data_header + 8);
    out.insert(out.end(), wav.begin() + static_cast<std::ptrdiff_t>(data_at + 8),
               wav.begin() + static_cast<std::ptrdiff_t>(data_at + 8 + data_size));
    put_le64(out, 20, out.size() - 8); // the real RIFF size; only the data claim lies
    return out;
}

} // namespace

TEST_CASE("AudioReaderFallback::Refuse opens only readers that seek", "[audio][audio-reader]") {
    const auto no_length = flac_without_length();
    CHECK_FALSE(AudioReader::open_memory(no_length, ".flac", {}, AudioReaderFallback::Refuse));
    CHECK_FALSE(AudioReader::open_source(std::make_unique<VectorSource>(no_length), ".flac", {},
                                         AudioReaderFallback::Refuse));
    const auto seekable_bytes = decode_fixture(fixtures::kFlacBase64);
    auto seekable =
        AudioReader::open_memory(seekable_bytes, ".flac", {}, AudioReaderFallback::Refuse);
    REQUIRE(seekable);
    CHECK(seekable->supports_ranged_read());

    TempDir dir;
    const auto path = dir.path / "unknown_length.flac";
    write_bytes(path, no_length);
    CHECK_FALSE(AudioReader::open_file(path.string(), {}, AudioReaderFallback::Refuse));
    CHECK(AudioReader::open_file(path.string()));
}

TEST_CASE("A strict memory-mapped open refuses a no-length FLAC without decoding it",
          "[audio][audio-reader][mmap]") {
    TempDir dir;
    const auto path = dir.path / "unknown_length.flac";
    write_bytes(path, flac_without_length());

    // The permissive open decodes once, so it learns the length.
    pulp::audio::MemoryMappedAudioReader permissive;
    REQUIRE(permissive.open(path.string()));
    CHECK(permissive.info().num_frames == kFrames);

    // The ranged-only open does not decode: it keeps the header's claim of
    // zero frames and reports no ranged reads, as main did before ranged
    // compressed reads existed.
    pulp::audio::MemoryMappedAudioReader strict;
    REQUIRE(strict.open(path.string(), std::numeric_limits<std::size_t>::max(), true));
    CHECK_FALSE(strict.supports_ranged_read());
    CHECK(strict.info().num_frames == 0);

    // A seekable FLAC still opens ranged under the strict policy.
    const auto seekable = dir.path / "seekable.flac";
    write_bytes(seekable, decode_fixture(fixtures::kFlacBase64));
    const auto file = pulp::audio::make_memory_mapped_frame_reader(seekable.string(), true);
    REQUIRE(file.valid);
    CHECK(file.supports_ranged_read);
    CHECK(file.total_frames == kFrames);
}

TEST_CASE("AudioReader re-seeks after a short decode instead of trusting the cursor",
          "[audio][audio-reader]") {
    for (auto [name, encoded, tolerance] : {std::tuple{".flac", fixtures::kFlacBase64, 1e-6f},
                                            std::tuple{".mp3", fixtures::kMp3Base64, 1e-6f}}) {
        INFO(name);
        const auto bytes = decode_fixture(encoded);
        auto reference_reader = AudioReader::open_memory(bytes, name);
        REQUIRE(reference_reader);
        const auto total = reference_reader->info().num_frames;
        const auto reference = full_decode(*reference_reader);

        auto source = std::make_unique<FlakySource>(bytes, bytes.size() / 2);
        auto* flaky = source.get();
        auto reader = AudioReader::open_source(std::move(source), name);
        REQUIRE(reader);
        REQUIRE(reader->info().num_frames == total);
        flaky->arm();

        std::vector<std::vector<float>> out(2, std::vector<float>(total));
        float* pointers[2] = {out[0].data(), out[1].data()};
        const auto first = reader->read(0, pointers, 2, total);
        REQUIRE(first < total);

        // The source recovers; reading on from where the short read stopped
        // must decode the right frames rather than continue from codec state
        // the failure left behind.
        flaky->heal();
        const auto rest = total - first;
        const auto second = reader->read(first, pointers, 2, rest);
        REQUIRE(second == rest);
        for (std::uint64_t i = 0; i < rest; ++i)
            REQUIRE(std::fabs(out[0][i] - reference.channels[0][first + i]) <= tolerance);
    }
}

TEST_CASE("AudioReader bounds an RF64 length claim by the bytes present", "[audio][audio-reader]") {
    TempDir dir;
    auto& registry = pulp::audio::FormatRegistry::instance();
    const auto wav_path = dir.path / "signal.wav";
    REQUIRE(registry.write(wav_path.string(), make_signal()));
    const auto rf64 = as_rf64(read_bytes(wav_path), kFrames * 10, 2u * 2u);
    auto reader = AudioReader::open_memory(rf64, ".wav");
    REQUIRE(reader);
    CHECK(reader->info().num_frames == kFrames);
    const auto data = reader->read(0, kFrames * 10);
    REQUIRE(data);
    CHECK(data->num_frames() == kFrames);
}

TEST_CASE("AudioReader and the memory-mapped reader agree with the registry on every WAV seed",
          "[audio][audio-reader][wav]") {
    // One WAV acceptance policy: whatever the registry's whole-file read
    // accepts, the streaming and memory-mapped readers accept with the same
    // facts and the same samples, truncated and RF64 seeds included; whatever
    // it refuses, they refuse.
    const auto corpus = pulp_test::fixture_root() / "test" / "fixtures" / "fuzz" / "audio-decoder";
    std::vector<fs::path> seeds;
    for (const auto& entry : fs::directory_iterator(corpus))
        if (entry.path().extension() == ".wav")
            seeds.push_back(entry.path());
    std::sort(seeds.begin(), seeds.end());
    REQUIRE(seeds.size() >= 20u);

    auto& registry = pulp::audio::FormatRegistry::instance();
    const auto limits = DecodeLimits::unbounded();
    std::size_t compared = 0;
    std::size_t truncated = 0;
    for (const auto& seed : seeds) {
        INFO(seed.filename().string());
        const auto info = registry.read_info(seed.string());
        const auto whole = registry.read(seed.string(), limits);
        const auto bytes = read_bytes(seed);
        auto from_file = AudioReader::open_file(seed.string(), limits, AudioReaderFallback::Refuse);
        auto from_memory =
            AudioReader::open_memory(bytes, ".wav", limits, AudioReaderFallback::Refuse);
        pulp::audio::MemoryMappedAudioReader mapped;
        const bool mapped_open = mapped.open(seed.string());

        CHECK(info.has_value() == whole.has_value());
        CHECK(static_cast<bool>(from_file) == whole.has_value());
        CHECK(static_cast<bool>(from_memory) == whole.has_value());
        CHECK(mapped_open == whole.has_value());
        if (!whole || !info || !from_file || !from_memory || !mapped_open)
            continue;
        ++compared;
        truncated += info->truncated ? 1u : 0u;

        for (auto* reader : {from_file.get(), from_memory.get()}) {
            const auto& reported = reader->info();
            CHECK(reported.num_frames == info->num_frames);
            CHECK(reported.num_channels == info->num_channels);
            CHECK(reported.sample_rate == info->sample_rate);
            CHECK(reported.truncated == info->truncated);
            CHECK(reported.declared_frames == info->declared_frames);
            const auto decoded = reader->read(0, reported.num_frames);
            REQUIRE(decoded.has_value());
            REQUIRE(decoded->num_frames() == whole->num_frames());
            for (std::size_t ch = 0; ch < whole->channels.size(); ++ch)
                CHECK(std::memcmp(decoded->channels[ch].data(), whole->channels[ch].data(),
                                  whole->channels[ch].size() * sizeof(float)) == 0);
        }

        CHECK(mapped.info().num_frames == info->num_frames);
        CHECK(mapped.info().truncated == info->truncated);
        CHECK(mapped.supports_ranged_read());
        const auto channels = whole->num_channels();
        std::vector<std::vector<float>> out(channels,
                                            std::vector<float>(whole->num_frames(), 9.0f));
        std::vector<float*> pointers;
        for (auto& channel : out)
            pointers.push_back(channel.data());
        REQUIRE(mapped.read_frames_ranged_only(pointers.data(), channels, 0, whole->num_frames()));
        for (std::size_t ch = 0; ch < channels; ++ch)
            CHECK(std::memcmp(out[ch].data(), whole->channels[ch].data(),
                              out[ch].size() * sizeof(float)) == 0);
    }
    CHECK(compared >= 20u);
    CHECK(truncated >= 4u);
}

TEST_CASE("AudioReader opens a zero-frame WAV and survives extreme offsets",
          "[audio][audio-reader]") {
    TempDir dir;
    // A zero-frame WAV is a valid empty file, not an empty stream: it opens
    // with zero frames (the registry decodes it the same way). The WAV writer
    // refuses to write one, so the bytes are built by hand.
    {
        const auto empty_bytes = make_zero_frame_wav(2);
        auto empty = AudioReader::open_memory(empty_bytes, ".wav");
        REQUIRE(empty);
        CHECK(empty->info().num_frames == 0);
    }

    for (auto& format : make_formats(dir)) {
        INFO(format.name);
        auto reader = AudioReader::open_memory(format.bytes, format.extension);
        REQUIRE(reader);
        std::vector<std::vector<float>> out(2, std::vector<float>(16, 9.0f));
        float* pointers[2] = {out[0].data(), out[1].data()};
        CHECK(reader->read(std::numeric_limits<std::uint64_t>::max() - 4, pointers, 2, 16) == 0);
        for (float sample : out[0])
            CHECK(sample == 0.0f);
    }
}

TEST_CASE("AudioReader decodes truncated MP3 and Ogg short without overrunning",
          "[audio][audio-reader]") {
    for (auto encoded : {fixtures::kMp3Base64, fixtures::kOggVorbisBase64}) {
        auto bytes = decode_fixture(encoded);
        bytes.resize(bytes.size() / 2);
        auto reader = AudioReader::open_memory(bytes);
        if (!reader)
            continue; // a cut before the header completes is refused, which is fine
        std::vector<std::vector<float>> out(2, std::vector<float>(kFrames * 2, 9.0f));
        float* pointers[2] = {out[0].data(), out[1].data()};
        const auto got = reader->read(0, pointers, 2, kFrames * 2);
        CHECK(got <= kFrames * 2);
        for (std::uint64_t i = got; i < kFrames * 2; ++i)
            REQUIRE(out[0][i] == 0.0f);
    }
}

// The MP3 decoder seeks by decoding forward instead of through dr_mp3's seek
// table. This pins the reason: in the vendored dr_mp3, a seek-table seek
// lands away from the requested frame for this fixture, while a decode-
// forward seek is exact. If an updated dr_mp3 makes this test fail, the seek
// table has become exact and Mp3Decoder can use it to make backward seeks
// cheap.
TEST_CASE("Vendored dr_mp3 seek-table seeks are inexact while decode-forward seeks are exact",
          "[audio][audio-reader]") {
    const auto bytes = decode_fixture(fixtures::kMp3Base64);
    drmp3 reference{};
    REQUIRE(drmp3_init_memory(&reference, bytes.data(), bytes.size(), nullptr));
    const auto total = drmp3_get_pcm_frame_count(&reference);
    std::vector<float> all(static_cast<std::size_t>(total) * reference.channels);
    REQUIRE(drmp3_seek_to_pcm_frame(&reference, 0));
    REQUIRE(drmp3_read_pcm_frames_f32(&reference, total, all.data()) == total);
    const auto channels = reference.channels;
    drmp3_uninit(&reference);

    drmp3 tabled{};
    REQUIRE(drmp3_init_memory(&tabled, bytes.data(), bytes.size(), nullptr));
    drmp3_uint32 count = 64;
    std::vector<drmp3_seek_point> points(count);
    REQUIRE(drmp3_calculate_seek_points(&tabled, &count, points.data()));
    REQUIRE(drmp3_bind_seek_table(&tabled, count, points.data()));

    auto mismatch = [&](drmp3& mp3, std::uint64_t target) {
        std::vector<float> got(64 * channels);
        if (!drmp3_seek_to_pcm_frame(&mp3, target))
            return true;
        const auto n = drmp3_read_pcm_frames_f32(&mp3, 64, got.data());
        for (std::uint64_t i = 0; i < n * channels; ++i)
            if (std::fabs(got[i] - all[target * channels + i]) > 1e-6f)
                return true;
        return n != 64;
    };

    bool any_tabled_mismatch = false;
    for (std::uint64_t target : {1500ull, 3000ull, 4500ull, 1200ull})
        if (target + 64 <= total)
            any_tabled_mismatch |= mismatch(tabled, target);
    drmp3_uninit(&tabled);
    CHECK(any_tabled_mismatch);

    drmp3 forward{};
    REQUIRE(drmp3_init_memory(&forward, bytes.data(), bytes.size(), nullptr));
    for (std::uint64_t target : {1500ull, 3000ull, 4500ull, 1200ull})
        if (target + 64 <= total)
            CHECK_FALSE(mismatch(forward, target));
    drmp3_uninit(&forward);
}

// The reader keeps a view of the bytes it is given, so a temporary vector is
// refused at compile time; passing one used to read freed memory once the
// reader decoded (an AddressSanitizer heap-use-after-free).
template <typename Bytes>
concept OpensFromMemory =
    requires(Bytes&& bytes) { AudioReader::open_memory(std::forward<Bytes>(bytes), ".flac"); };
static_assert(!OpensFromMemory<std::vector<std::uint8_t>>);
static_assert(OpensFromMemory<std::vector<std::uint8_t>&>);
static_assert(OpensFromMemory<const std::vector<std::uint8_t>&>);
static_assert(OpensFromMemory<std::span<const std::uint8_t>>);

TEST_CASE("AudioReader decodes the whole stream from memory the caller keeps alive",
          "[audio][audio-reader]") {
    // Each read reaches the end of the stream through the caller's bytes,
    // so a lifetime mistake here is an AddressSanitizer failure.
    for (auto encoded : {fixtures::kFlacBase64, fixtures::kMp3Base64, fixtures::kOggVorbisBase64}) {
        const auto bytes = decode_fixture(encoded);
        auto reader = AudioReader::open_memory(bytes);
        REQUIRE(reader);
        const auto total = reader->info().num_frames;
        const auto data = reader->read(0, total);
        REQUIRE(data);
        CHECK(data->num_frames() == total);
        const auto tail = reader->read(total / 2, total);
        REQUIRE(tail);
    }
}
