// AudioReader: ranged reads from a path, memory and a custom byte source must
// equal a full sequential decode at every offset, including across codec
// frame boundaries and at end of stream, for WAV, AIFF, FLAC, MP3 and Ogg
// Vorbis built from one known signal.

#include "audio_reader_fixtures.hpp"

#include <catch2/catch_test_macros.hpp>

#include <pulp/audio/audio_reader.hpp>
#include <pulp/audio/buffer.hpp>
#include <pulp/audio/format_registry.hpp>
#include <pulp/audio/mmap_reader.hpp>
#include <pulp/audio/streaming_sample_source.hpp>
#include <pulp/audio/streaming_sample_source_file.hpp>
#include <pulp/runtime/base64.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <numbers>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace fs = std::filesystem;
namespace fixtures = pulp::test::audio_reader_fixtures;
using pulp::audio::AudioByteSource;
using pulp::audio::AudioFileData;
using pulp::audio::AudioReader;
using pulp::audio::DecodeLimits;

constexpr std::uint32_t kRate = 22050;
constexpr std::uint64_t kFrames = 5513;

// The fixture signal, before 16-bit quantization.
float expected_sample(std::uint64_t frame, std::uint32_t channel) {
    const double hz = channel == 0 ? 440.0 : 660.0;
    return static_cast<float>(0.5 * std::sin(2.0 * std::numbers::pi * hz *
                                             static_cast<double>(frame) / kRate));
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
    std::uint64_t position() const override { return position_; }
    std::uint64_t size() const override { return bytes_.size(); }

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
    const std::uint64_t offsets[] = {3000, 0,    1023, 1024, 575, 576, 2047,
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

TEST_CASE("AudioReader decodes the known signal from each format",
          "[audio][audio-reader]") {
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

TEST_CASE("AudioReader refuses unknown, empty and truncated input",
          "[audio][audio-reader]") {
    const std::vector<std::uint8_t> garbage(512, 0x42);
    CHECK_FALSE(AudioReader::open_memory(garbage));
    CHECK_FALSE(AudioReader::open_memory({}));
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
            for (const std::uint64_t offset : {std::uint64_t{2500}, std::uint64_t{17},
                                               total - 900}) {
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

    auto ranged = AudioReader::open_memory(decode_fixture(fixtures::kFlacBase64));
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
