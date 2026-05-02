#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <pulp/audio/audio_file.hpp>
#include <pulp/audio/format_registry.hpp>
#include <pulp/audio/mmap_reader.hpp>
#include <pulp/audio/offline_processor.hpp>
#include <pulp/audio/streaming_writer.hpp>
#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <cmath>
#include <memory>
#include <string>
#include <vector>
#include <utility>

using namespace pulp::audio;
using Catch::Matchers::WithinAbs;

static bool contains_extension(const std::vector<std::string>& extensions,
                               std::string_view extension) {
    return std::find(extensions.begin(), extensions.end(), extension) != extensions.end();
}

static std::filesystem::path unique_temp_audio_path(std::string_view suffix) {
    static int counter = 0;
    auto name = "pulp_test_audio_" + std::to_string(reinterpret_cast<std::uintptr_t>(&counter))
              + "_" + std::to_string(counter++) + std::string(suffix);
    return std::filesystem::temp_directory_path() / name;
}

static std::vector<uint8_t> read_binary_file(const std::filesystem::path& path) {
    std::ifstream f(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
}

static uint16_t read_le16(const std::vector<uint8_t>& bytes, size_t offset) {
    return static_cast<uint16_t>(bytes[offset])
         | static_cast<uint16_t>(bytes[offset + 1] << 8);
}

static uint32_t read_le32(const std::vector<uint8_t>& bytes, size_t offset) {
    return static_cast<uint32_t>(bytes[offset])
         | (static_cast<uint32_t>(bytes[offset + 1]) << 8)
         | (static_cast<uint32_t>(bytes[offset + 2]) << 16)
         | (static_cast<uint32_t>(bytes[offset + 3]) << 24);
}

static int32_t read_le24_signed(const std::vector<uint8_t>& bytes, size_t offset) {
    int32_t value = static_cast<int32_t>(bytes[offset])
                  | (static_cast<int32_t>(bytes[offset + 1]) << 8)
                  | (static_cast<int32_t>(bytes[offset + 2]) << 16);
    if (value & 0x800000) value |= static_cast<int32_t>(0xFF000000u);
    return value;
}

static int32_t read_le32_signed(const std::vector<uint8_t>& bytes, size_t offset) {
    return static_cast<int32_t>(read_le32(bytes, offset));
}

static size_t require_riff_chunk_data_offset(const std::vector<uint8_t>& bytes,
                                             std::string_view chunk_id) {
    REQUIRE(chunk_id.size() == 4);
    REQUIRE(bytes.size() >= 12);

    size_t offset = 12;
    while (offset + 8 <= bytes.size()) {
        auto id = std::string_view(reinterpret_cast<const char*>(bytes.data() + offset), 4);
        auto size = read_le32(bytes, offset + 4);
        auto data_offset = offset + 8;
        REQUIRE(data_offset + size <= bytes.size());
        if (id == chunk_id) {
            return data_offset;
        }
        offset = data_offset + size + (size & 1u);
    }

    FAIL("RIFF chunk not found: " << chunk_id);
    return 0;
}

static void require_wav_header(const std::vector<uint8_t>& bytes,
                               uint16_t channels,
                               uint32_t sample_rate,
                               uint16_t bits_per_sample,
                               uint32_t data_size) {
    REQUIRE(bytes.size() == 44u + data_size);
    REQUIRE(std::string(reinterpret_cast<const char*>(bytes.data()), 4) == "RIFF");
    REQUIRE(read_le32(bytes, 4) == 36u + data_size);
    REQUIRE(std::string(reinterpret_cast<const char*>(bytes.data() + 8), 4) == "WAVE");
    REQUIRE(std::string(reinterpret_cast<const char*>(bytes.data() + 12), 4) == "fmt ");
    REQUIRE(read_le32(bytes, 16) == 16);
    REQUIRE(read_le16(bytes, 20) == 1);
    REQUIRE(read_le16(bytes, 22) == channels);
    REQUIRE(read_le32(bytes, 24) == sample_rate);
    REQUIRE(read_le32(bytes, 28) == sample_rate * channels * (bits_per_sample / 8));
    REQUIRE(read_le16(bytes, 32) == channels * (bits_per_sample / 8));
    REQUIRE(read_le16(bytes, 34) == bits_per_sample);
    REQUIRE(std::string(reinterpret_cast<const char*>(bytes.data() + 36), 4) == "data");
    REQUIRE(read_le32(bytes, 40) == data_size);
}

namespace {

struct RegistryProbeState {
    int info_calls = 0;
    int read_calls = 0;
    int write_calls = 0;
    std::string last_info_path;
    std::string last_read_path;
    std::string last_write_path;
    size_t last_written_channels = 0;
};

class RegistryProbeReader : public FormatReader {
public:
    explicit RegistryProbeReader(std::shared_ptr<RegistryProbeState> state)
        : state_(std::move(state)) {}

    std::optional<AudioFileInfo> read_info(const std::string& path) override {
        state_->info_calls++;
        state_->last_info_path = path;

        AudioFileInfo info;
        info.sample_rate = 22050;
        info.num_channels = 2;
        info.num_frames = 3;
        info.bits_per_sample = 24;
        info.duration_seconds = 3.0 / 22050.0;
        info.format = "Probe";
        return info;
    }

    std::optional<AudioFileData> read(const std::string& path) override {
        state_->read_calls++;
        state_->last_read_path = path;

        AudioFileData data;
        data.sample_rate = 22050;
        data.channels = {
            {0.0f, 0.25f, 0.5f},
            {-0.5f, -0.25f, 1.0f},
        };
        return data;
    }

    bool supports_extension(std::string_view ext) const override {
        return ext == ".aifc";
    }

    std::string format_name() const override { return "Probe"; }

private:
    std::shared_ptr<RegistryProbeState> state_;
};

class RegistryProbeWriter : public FormatWriter {
public:
    explicit RegistryProbeWriter(std::shared_ptr<RegistryProbeState> state)
        : state_(std::move(state)) {}

    bool write(const std::string& path, const AudioFileData& data) override {
        state_->write_calls++;
        state_->last_write_path = path;
        state_->last_written_channels = data.num_channels();
        return !data.empty();
    }

    bool supports_extension(std::string_view ext) const override {
        return ext == ".aifc";
    }

    std::string format_name() const override { return "Probe"; }

private:
    std::shared_ptr<RegistryProbeState> state_;
};

}  // namespace

static void append_be16(std::vector<unsigned char>& bytes, uint16_t value) {
    bytes.push_back(static_cast<unsigned char>((value >> 8) & 0xFF));
    bytes.push_back(static_cast<unsigned char>(value & 0xFF));
}

static void append_be32(std::vector<unsigned char>& bytes, uint32_t value) {
    bytes.push_back(static_cast<unsigned char>((value >> 24) & 0xFF));
    bytes.push_back(static_cast<unsigned char>((value >> 16) & 0xFF));
    bytes.push_back(static_cast<unsigned char>((value >> 8) & 0xFF));
    bytes.push_back(static_cast<unsigned char>(value & 0xFF));
}

static std::vector<unsigned char> extended_sample_rate_44100() {
    return {0x40, 0x0E, 0xAC, 0x44, 0, 0, 0, 0, 0, 0};
}

static void append_aiff_chunk(std::vector<unsigned char>& bytes,
                              std::string_view id,
                              const std::vector<unsigned char>& payload) {
    REQUIRE(id.size() == 4);
    bytes.insert(bytes.end(), id.begin(), id.end());
    append_be32(bytes, static_cast<uint32_t>(payload.size()));
    bytes.insert(bytes.end(), payload.begin(), payload.end());
    if (payload.size() & 1) {
        bytes.push_back(0);
    }
}

static std::vector<unsigned char> make_comm_chunk(uint16_t channels,
                                                  uint32_t frames,
                                                  uint16_t bits_per_sample,
                                                  bool aifc = false) {
    std::vector<unsigned char> comm;
    append_be16(comm, channels);
    append_be32(comm, frames);
    append_be16(comm, bits_per_sample);
    auto sample_rate = extended_sample_rate_44100();
    comm.insert(comm.end(), sample_rate.begin(), sample_rate.end());
    if (aifc) {
        comm.insert(comm.end(), {'N', 'O', 'N', 'E'});
    }
    return comm;
}

static std::vector<unsigned char> make_ssnd_chunk(const std::vector<unsigned char>& sample_bytes) {
    std::vector<unsigned char> ssnd;
    append_be32(ssnd, 0);
    append_be32(ssnd, 0);
    ssnd.insert(ssnd.end(), sample_bytes.begin(), sample_bytes.end());
    return ssnd;
}

static void write_aiff_fixture(const std::filesystem::path& path,
                               std::string_view form_type,
                               const std::vector<std::pair<std::string_view, std::vector<unsigned char>>>& chunks) {
    REQUIRE(form_type.size() == 4);

    std::vector<unsigned char> body;
    body.insert(body.end(), form_type.begin(), form_type.end());
    for (const auto& [id, payload] : chunks) {
        append_aiff_chunk(body, id, payload);
    }

    std::vector<unsigned char> file_bytes = {'F', 'O', 'R', 'M'};
    append_be32(file_bytes, static_cast<uint32_t>(body.size()));
    file_bytes.insert(file_bytes.end(), body.begin(), body.end());

    std::ofstream file(path, std::ios::binary);
    file.write(reinterpret_cast<const char*>(file_bytes.data()),
               static_cast<std::streamsize>(file_bytes.size()));
    REQUIRE(file.good());
}

// ── Sample format conversion ─────────────────────────────────────────────────

TEST_CASE("int16 to float conversion", "[audio][convert]") {
    int16_t src[] = {0, 16384, -16384, 32767, -32768};
    float dst[5];
    int16_to_float(src, dst, 5);

    REQUIRE_THAT(dst[0], WithinAbs(0.0, 0.001));
    REQUIRE_THAT(dst[1], WithinAbs(0.5, 0.001));
    REQUIRE_THAT(dst[2], WithinAbs(-0.5, 0.001));
    REQUIRE(dst[3] > 0.99f);
    REQUIRE(dst[4] < -0.99f);
}

TEST_CASE("float to int16 conversion", "[audio][convert]") {
    float src[] = {0.0f, 0.5f, -0.5f, 1.0f, -1.0f};
    int16_t dst[5];
    float_to_int16(src, dst, 5);

    REQUIRE(dst[0] == 0);
    REQUIRE(std::abs(dst[1] - 16383) < 2);
    REQUIRE(std::abs(dst[2] - (-16383)) < 2);
    REQUIRE(dst[3] == 32767);
    REQUIRE(dst[4] == -32767);
}

TEST_CASE("int32 to float conversion", "[audio][convert]") {
    int32_t src[] = {0, 1073741824, -1073741824};
    float dst[3];
    int32_to_float(src, dst, 3);

    REQUIRE_THAT(dst[0], WithinAbs(0.0, 0.001));
    REQUIRE_THAT(dst[1], WithinAbs(0.5, 0.001));
    REQUIRE_THAT(dst[2], WithinAbs(-0.5, 0.001));
}

TEST_CASE("float to int16 clamps", "[audio][convert]") {
    float src[] = {2.0f, -3.0f};
    int16_t dst[2];
    float_to_int16(src, dst, 2);

    REQUIRE(dst[0] == 32767);  // Clamped to max
    REQUIRE(dst[1] == -32767); // Clamped to min
}

TEST_CASE("int16 round-trip preserves values", "[audio][convert]") {
    float original[] = {0.0f, 0.25f, -0.25f, 0.75f, -0.75f};
    int16_t intermediate[5];
    float result[5];

    float_to_int16(original, intermediate, 5);
    int16_to_float(intermediate, result, 5);

    for (int i = 0; i < 5; ++i) {
        REQUIRE(std::abs(result[i] - original[i]) < 0.001f);
    }
}

TEST_CASE("sample conversion handles packed 24-bit and 32-bit edges",
          "[audio][convert][issue-640]") {
    const uint8_t packed24[] = {
        0x00, 0x00, 0x00,
        0xFF, 0xFF, 0x7F,
        0x00, 0x00, 0x80,
        0xFF, 0xFF, 0xFF,
    };
    float from24[4] = {};
    int24_to_float(packed24, from24, 4);
    REQUIRE_THAT(from24[0], WithinAbs(0.0f, 0.000001f));
    REQUIRE_THAT(from24[1], WithinAbs(8388607.0f / 8388608.0f, 0.000001f));
    REQUIRE_THAT(from24[2], WithinAbs(-1.0f, 0.000001f));
    REQUIRE_THAT(from24[3], WithinAbs(-1.0f / 8388608.0f, 0.000001f));

    const int32_t int32_samples[] = {
        0,
        1073741824,
        -1073741824,
        std::numeric_limits<int32_t>::max(),
        std::numeric_limits<int32_t>::min(),
    };
    float from32[5] = {};
    int32_to_float(int32_samples, from32, 5);
    REQUIRE_THAT(from32[0], WithinAbs(0.0f, 0.000001f));
    REQUIRE_THAT(from32[1], WithinAbs(0.5f, 0.000001f));
    REQUIRE_THAT(from32[2], WithinAbs(-0.5f, 0.000001f));
    REQUIRE_THAT(from32[3], WithinAbs(1.0f, 0.000001f));
    REQUIRE_THAT(from32[4], WithinAbs(-1.0f, 0.000001f));

    const float floats[] = {-2.0f, -1.0f, 0.0f, 0.5f, 1.0f, 2.0f};
    int32_t to32[6] = {};
    float_to_int32(floats, to32, 6);
    REQUIRE(to32[0] == std::numeric_limits<int32_t>::min());
    REQUIRE(to32[1] == std::numeric_limits<int32_t>::min());
    REQUIRE(to32[2] == 0);
    REQUIRE(std::abs(to32[3] - 1073741823) < 2);
    REQUIRE(to32[4] == std::numeric_limits<int32_t>::max());
    REQUIRE(to32[5] == std::numeric_limits<int32_t>::max());
}

TEST_CASE("sample conversion no-ops leave sentinel outputs untouched",
          "[audio][convert][issue-640]") {
    int16_t int16_src[] = {123};
    uint8_t int24_src[] = {1, 2, 3};
    int32_t int32_src[] = {456};
    float float_src[] = {0.5f};

    float float_out = 9.0f;
    int16_t int16_out = 17;
    int32_t int32_out = 23;

    int16_to_float(int16_src, &float_out, 0);
    REQUIRE(float_out == 9.0f);
    int24_to_float(int24_src, &float_out, 0);
    REQUIRE(float_out == 9.0f);
    int32_to_float(int32_src, &float_out, 0);
    REQUIRE(float_out == 9.0f);
    float_to_int16(float_src, &int16_out, 0);
    REQUIRE(int16_out == 17);
    float_to_int32(float_src, &int32_out, 0);
    REQUIRE(int32_out == 23);
}

// ── WAV file I/O ─────────────────────────────────────────────────────────────

TEST_CASE("Write and read WAV file", "[audio][file]") {
    auto tmp_path = std::filesystem::temp_directory_path() / "pulp_test_audio.wav";

    // Create test data: 1 second mono sine wave
    AudioFileData data;
    data.sample_rate = 44100;
    data.channels.resize(1);
    data.channels[0].resize(44100);
    for (int i = 0; i < 44100; ++i) {
        data.channels[0][i] = 0.5f * std::sin(2.0f * 3.14159f * 440.0f * i / 44100.0f);
    }

    // Write
    bool ok = write_wav_file(tmp_path.string(), data);
    REQUIRE(ok);
    REQUIRE(std::filesystem::exists(tmp_path));

    // Read back
    auto read_data = read_audio_file(tmp_path.string());
    REQUIRE(read_data.has_value());
    REQUIRE(read_data->sample_rate == 44100);
    REQUIRE(read_data->num_channels() == 1);
    REQUIRE(read_data->num_frames() == 44100);

    // Verify data (16-bit quantization means some loss)
    for (int i = 0; i < 100; ++i) {
        REQUIRE(std::abs(read_data->channels[0][i] - data.channels[0][i]) < 0.001f);
    }

    // Read info
    auto info = read_audio_file_info(tmp_path.string());
    REQUIRE(info.has_value());
    REQUIRE(info->sample_rate == 44100);
    REQUIRE(info->num_channels == 1);
    REQUIRE(info->duration_seconds > 0.99);

    std::filesystem::remove(tmp_path);
}

TEST_CASE("Read nonexistent file returns nullopt", "[audio][file]") {
    auto result = read_audio_file("/nonexistent/path.wav");
    REQUIRE_FALSE(result.has_value());

    auto info = read_audio_file_info("/nonexistent/path.wav");
    REQUIRE_FALSE(info.has_value());
}

TEST_CASE("WAV helpers write deinterleaved channel data and reject malformed input",
          "[audio][file][issue-640]") {
    auto path = unique_temp_audio_path("_helper_edges.wav");
    std::filesystem::remove(path);

    AudioFileData data;
    data.sample_rate = 22050;
    data.channels = {
        {-1.0f, 0.0f, 1.0f},
        {0.5f, -0.5f, 0.25f},
    };

    REQUIRE(write_wav_file(path.string(), data));
    auto bytes = read_binary_file(path);
    REQUIRE(bytes.size() >= 56);
    REQUIRE(std::string(reinterpret_cast<const char*>(bytes.data()), 4) == "RIFF");
    REQUIRE(std::string(reinterpret_cast<const char*>(bytes.data() + 8), 4) == "WAVE");
    auto fmt_offset = require_riff_chunk_data_offset(bytes, "fmt ");
    auto format_tag = read_le16(bytes, fmt_offset);
    REQUIRE((format_tag == 1 || format_tag == 0xFFFE));
    REQUIRE(read_le16(bytes, fmt_offset + 2) == 2);
    REQUIRE(read_le32(bytes, fmt_offset + 4) == 22050);
    REQUIRE(read_le16(bytes, fmt_offset + 14) == 16);

    auto data_offset = require_riff_chunk_data_offset(bytes, "data");
    REQUIRE(read_le32(bytes, data_offset - 4) == 12);
    REQUIRE(static_cast<int16_t>(read_le16(bytes, data_offset)) == -32767);
    REQUIRE(static_cast<int16_t>(read_le16(bytes, data_offset + 2)) == 16383);
    REQUIRE(static_cast<int16_t>(read_le16(bytes, data_offset + 4)) == 0);
    REQUIRE(static_cast<int16_t>(read_le16(bytes, data_offset + 6)) == -16383);
    REQUIRE(static_cast<int16_t>(read_le16(bytes, data_offset + 8)) == 32767);
    REQUIRE(static_cast<int16_t>(read_le16(bytes, data_offset + 10)) == 8191);

    auto read_data = read_audio_file(path.string());
    REQUIRE(read_data.has_value());
    REQUIRE(read_data->sample_rate == 22050);
    REQUIRE(read_data->num_channels() == 2);
    REQUIRE(read_data->num_frames() == 3);
    REQUIRE_THAT(read_data->channels[0][2], WithinAbs(1.0f, 0.001f));
    REQUIRE_THAT(read_data->channels[1][1], WithinAbs(-0.5f, 0.001f));

    auto malformed = unique_temp_audio_path("_malformed.wav");
    {
        std::ofstream f(malformed, std::ios::binary);
        f << "not a wav";
    }
    REQUIRE_FALSE(read_audio_file_info(malformed.string()).has_value());
    REQUIRE_FALSE(read_audio_file(malformed.string()).has_value());

    std::filesystem::remove(path);
    std::filesystem::remove(malformed);
}

TEST_CASE("OGG registry reader rejects invalid files deterministically",
          "[audio][file][ogg][issue-640]") {
    auto path = unique_temp_audio_path("_invalid.ogg");
    {
        std::ofstream f(path, std::ios::binary);
        f << "not an ogg stream";
    }

    auto& registry = FormatRegistry::instance();
    auto* reader = registry.find_reader(".oga");
    REQUIRE(reader != nullptr);
    REQUIRE(reader->format_name() == "OGG Vorbis");
    REQUIRE_FALSE(reader->read_info(path.string()).has_value());
    REQUIRE_FALSE(reader->read(path.string()).has_value());
    REQUIRE_FALSE(registry.read_info(path.string()).has_value());
    REQUIRE_FALSE(registry.read(path.string()).has_value());

    std::filesystem::remove(path);
}

TEST_CASE("MemoryMappedAudioReader opens WAV files and reads frame ranges",
          "[audio][file][mmap][issue-640]") {
    auto path = unique_temp_audio_path("_mmap.wav");
    std::filesystem::remove(path);

    AudioFileData data;
    data.sample_rate = 48000;
    data.channels = {
        {0.0f, 0.25f, 0.5f, 0.75f},
        {-0.75f, -0.5f, -0.25f, 0.0f},
    };
    REQUIRE(write_wav_file(path.string(), data));

    MemoryMappedAudioReader reader;
    REQUIRE_FALSE(reader.is_open());
    REQUIRE_FALSE(reader.read_all().has_value());

    REQUIRE(reader.open(path.string()));
    REQUIRE(reader.is_open());
    REQUIRE(reader.data() != nullptr);
    REQUIRE(reader.size() > 44);
    REQUIRE(reader.info().sample_rate == 48000);
    REQUIRE(reader.info().num_channels == 2);
    REQUIRE(reader.info().num_frames == 4);

    float left[2] = {};
    float right[2] = {};
    float* channels[] = {left, right};
    REQUIRE(reader.read_frames(channels, 2, 1, 2));
    REQUIRE_THAT(left[0], WithinAbs(0.25f, 0.001f));
    REQUIRE_THAT(left[1], WithinAbs(0.5f, 0.001f));
    REQUIRE_THAT(right[0], WithinAbs(-0.5f, 0.001f));
    REQUIRE_THAT(right[1], WithinAbs(-0.25f, 0.001f));

    float mono[2] = {-9.0f, -9.0f};
    float* mono_channel[] = {mono};
    REQUIRE(reader.read_frames(mono_channel, 1, 3, 8));
    REQUIRE_THAT(mono[0], WithinAbs(0.75f, 0.001f));
    REQUIRE_THAT(mono[1], WithinAbs(-9.0f, 0.001f));

    auto all = reader.read_all();
    REQUIRE(all.has_value());
    REQUIRE(all->sample_rate == 48000);
    REQUIRE(all->num_channels() == 2);
    REQUIRE(all->num_frames() == 4);

    reader.close();
    REQUIRE_FALSE(reader.is_open());
    REQUIRE(reader.info().num_frames == 0);
    REQUIRE_FALSE(reader.read_frames(channels, 2, 0, 1));
    REQUIRE_FALSE(reader.open((path.string() + ".missing")));

    std::filesystem::remove(path);
}

TEST_CASE("offline processing handles guards, block tails, and file dispatch",
          "[audio][file][offline][issue-640]") {
    AudioFileData input;
    input.sample_rate = 44100;
    input.channels = {
        {0.1f, 0.2f, 0.3f, 0.4f, 0.5f},
        {-0.1f, -0.2f, -0.3f, -0.4f, -0.5f},
    };

    REQUIRE_FALSE(offline_process(AudioFileData{}, [](const float*, float*, int, int, double) {}).has_value());
    REQUIRE_FALSE(offline_process(input, {}, 2).has_value());
    REQUIRE_FALSE(offline_process(input, [](const float*, float*, int, int, double) {}, 0).has_value());
    REQUIRE_FALSE(offline_process(input, [](const float*, float*, int, int, double) {}, -8).has_value());

    std::vector<int> block_frames;
    auto output = offline_process(
        input,
        [&](const float* in, float* out, int channels, int frames, double sample_rate) {
            REQUIRE(channels == 2);
            REQUIRE(sample_rate == 44100.0);
            block_frames.push_back(frames);
            for (int frame = 0; frame < frames; ++frame) {
                out[frame * channels] = in[frame * channels] * 2.0f;
                out[frame * channels + 1] = in[frame * channels + 1] * -3.0f;
            }
        },
        3);

    REQUIRE(output.has_value());
    REQUIRE(block_frames == std::vector<int>{3, 2});
    REQUIRE(output->sample_rate == 44100);
    REQUIRE(output->num_channels() == 2);
    REQUIRE(output->num_frames() == 5);
    REQUIRE_THAT(output->channels[0][4], WithinAbs(1.0f, 0.001f));
    REQUIRE_THAT(output->channels[1][4], WithinAbs(1.5f, 0.001f));

    auto gained = apply_gain(input, 0.5f);
    REQUIRE(gained.sample_rate == 44100);
    REQUIRE_THAT(gained.channels[0][2], WithinAbs(0.15f, 0.001f));
    REQUIRE_THAT(gained.channels[1][2], WithinAbs(-0.15f, 0.001f));

    auto in_path = unique_temp_audio_path("_offline_in.wav");
    auto out_path = unique_temp_audio_path("_offline_out.wav");
    std::filesystem::remove(in_path);
    std::filesystem::remove(out_path);

    REQUIRE(write_wav_file(in_path.string(), input));
    REQUIRE(offline_process_file(
        in_path.string(),
        out_path.string(),
        [](const float* in, float* out, int channels, int frames, double) {
            for (int i = 0; i < channels * frames; ++i) {
                out[i] = -in[i];
            }
        },
        4));

    auto from_file = read_audio_file(out_path.string());
    REQUIRE(from_file.has_value());
    REQUIRE(from_file->num_channels() == 2);
    REQUIRE(from_file->num_frames() == 5);
    REQUIRE_THAT(from_file->channels[0][1], WithinAbs(-0.2f, 0.001f));
    REQUIRE_THAT(from_file->channels[1][1], WithinAbs(0.2f, 0.001f));

    REQUIRE_FALSE(offline_process_file(
        (in_path.string() + ".missing"),
        out_path.string(),
        [](const float*, float*, int, int, double) {},
        4));
    REQUIRE_FALSE(offline_process_file(
        in_path.string(),
        (out_path.string() + ".unsupported"),
        [](const float* in, float* out, int channels, int frames, double) {
            std::copy(in, in + channels * frames, out);
        },
        4));

    std::filesystem::remove(in_path);
    std::filesystem::remove(out_path);
}

// ── Streaming WAV writer ────────────────────────────────────────────────────

TEST_CASE("StreamingWriter rejects invalid opens and closed writes",
          "[audio][file][streaming][issue-640]") {
    StreamingWriter writer;
    float sample = 0.0f;
    REQUIRE(writer.write_frames(&sample, 1) == 0);
    REQUIRE_FALSE(writer.is_open());

    auto path = unique_temp_audio_path("_stream_invalid.wav");
    std::filesystem::remove(path);

    REQUIRE_FALSE(writer.open(path.string(), 44100, 2, 12));
    REQUIRE_FALSE(writer.open(path.string(), 0, 2, 16));
    REQUIRE_FALSE(writer.open(path.string(), 44100, 0, 16));
    REQUIRE_FALSE(writer.is_open());

    REQUIRE(writer.open(path.string(), 44100, 2, 16));
    REQUIRE(writer.write_frames(static_cast<const float*>(nullptr), 1) == 0);
    REQUIRE(writer.write_frames(&sample, 0) == 0);
    REQUIRE(writer.write_frames(&sample, -1) == 0);

    const float* invalid_channels[] = {&sample, nullptr};
    REQUIRE(writer.write_frames(static_cast<const float* const*>(nullptr), 2, 1) == 0);
    REQUIRE(writer.write_frames(invalid_channels, 2, 1) == 0);
    REQUIRE(writer.write_frames(invalid_channels, 2, 0) == 0);
    REQUIRE(writer.write_frames(invalid_channels, 2, -1) == 0);
    REQUIRE(writer.frames_written() == 0);

    writer.close();
    writer.close();
    std::filesystem::remove(path);
}

TEST_CASE("StreamingWriter writes finalized 16-bit interleaved WAV data",
          "[audio][file][streaming][issue-640]") {
    auto path = unique_temp_audio_path("_stream_16.wav");
    std::filesystem::remove(path);

    StreamingWriter writer;
    REQUIRE(writer.open(path.string(), 48000, 2, 16));
    REQUIRE(writer.is_open());

    const float frames[] = {
        -2.0f, -1.0f,
         0.0f,  0.5f,
         1.0f,  2.0f,
    };
    REQUIRE(writer.write_frames(frames, 3) == 3);
    REQUIRE(writer.frames_written() == 3);

    writer.close();
    REQUIRE_FALSE(writer.is_open());
    writer.close();

    auto bytes = read_binary_file(path);
    require_wav_header(bytes, 2, 48000, 16, 12);

    REQUIRE(static_cast<int16_t>(read_le16(bytes, 44)) == -32767);
    REQUIRE(static_cast<int16_t>(read_le16(bytes, 46)) == -32767);
    REQUIRE(static_cast<int16_t>(read_le16(bytes, 48)) == 0);
    REQUIRE(static_cast<int16_t>(read_le16(bytes, 50)) == 16383);
    REQUIRE(static_cast<int16_t>(read_le16(bytes, 52)) == 32767);
    REQUIRE(static_cast<int16_t>(read_le16(bytes, 54)) == 32767);

    std::filesystem::remove(path);
}

TEST_CASE("StreamingWriter interleaves deinterleaved 24-bit channel data",
          "[audio][file][streaming][issue-640]") {
    auto path = unique_temp_audio_path("_stream_24.wav");
    std::filesystem::remove(path);

    StreamingWriter writer;
    REQUIRE(writer.open(path.string(), 44100, 2, 24));

    const float left[] = {1.0f, -1.0f};
    const float right[] = {0.25f, -0.25f};
    const float* channels[] = {left, right};
    REQUIRE(writer.write_frames(channels, 1, 2) == 0);
    REQUIRE(writer.write_frames(channels, 2, 2) == 2);
    REQUIRE(writer.frames_written() == 2);
    writer.close();

    auto bytes = read_binary_file(path);
    require_wav_header(bytes, 2, 44100, 24, 12);

    REQUIRE(read_le24_signed(bytes, 44) == 8388607);
    REQUIRE(read_le24_signed(bytes, 47) == 2097151);
    REQUIRE(read_le24_signed(bytes, 50) == -8388607);
    REQUIRE(read_le24_signed(bytes, 53) == -2097151);

    std::filesystem::remove(path);
}

TEST_CASE("StreamingWriter writes 32-bit PCM and destructor finalizes header",
          "[audio][file][streaming][issue-640]") {
    auto path = unique_temp_audio_path("_stream_32.wav");
    std::filesystem::remove(path);

    {
        StreamingWriter writer;
        REQUIRE(writer.open(path.string(), 32000, 1, 32));
        const float frames[] = {-0.5f, 0.5f};
        REQUIRE(writer.write_frames(frames, 2) == 2);
        REQUIRE(writer.frames_written() == 2);
    }

    auto bytes = read_binary_file(path);
    require_wav_header(bytes, 1, 32000, 32, 8);
    REQUIRE(read_le32_signed(bytes, 44) == -1073741824);
    REQUIRE(read_le32_signed(bytes, 48) == 1073741824);

    std::filesystem::remove(path);
}

// ── Format registry dispatch ─────────────────────────────────────────────────

TEST_CASE("FormatRegistry exposes built-in audio codecs", "[audio][file][registry]") {
    auto& registry = FormatRegistry::instance();

    REQUIRE(registry.find_reader(".wav") != nullptr);
    REQUIRE(registry.find_writer(".wav") != nullptr);
    REQUIRE(registry.find_reader("WAVE") != nullptr);
    REQUIRE(registry.find_reader(".flac") != nullptr);
    REQUIRE(registry.find_reader(".mp3") != nullptr);
    REQUIRE(registry.find_reader(".ogg") != nullptr);
    REQUIRE(registry.find_reader(".oga") != nullptr);
    REQUIRE(registry.find_reader(".aiff") != nullptr);
    REQUIRE(registry.find_reader(".AIF") != nullptr);
    REQUIRE(registry.find_writer(".aiff") != nullptr);
    REQUIRE(registry.find_writer("AIF") != nullptr);
    REQUIRE(registry.find_reader(".not-a-format") == nullptr);
    REQUIRE(registry.find_writer(".not-a-format") == nullptr);
    REQUIRE_FALSE(registry.read_info("pulp_test_audio.not-a-format").has_value());
    REQUIRE_FALSE(registry.read("pulp_test_audio.not-a-format").has_value());

    auto read_extensions = registry.supported_read_extensions();
    REQUIRE(contains_extension(read_extensions, ".wav"));
    REQUIRE(contains_extension(read_extensions, ".wave"));
    REQUIRE(contains_extension(read_extensions, ".flac"));
    REQUIRE(contains_extension(read_extensions, ".mp3"));
    REQUIRE(contains_extension(read_extensions, ".ogg"));
    REQUIRE(contains_extension(read_extensions, ".oga"));
    REQUIRE(contains_extension(read_extensions, ".aiff"));
    REQUIRE(contains_extension(read_extensions, ".aif"));

    auto write_extensions = registry.supported_write_extensions();
    REQUIRE(contains_extension(write_extensions, ".wav"));
    REQUIRE(contains_extension(write_extensions, ".wave"));
    REQUIRE(contains_extension(write_extensions, ".aiff"));
    REQUIRE(contains_extension(write_extensions, ".aif"));
}

TEST_CASE("FormatRegistry rejects malformed compressed files through built-in readers",
          "[audio][file][registry][issue-640]") {
    auto flac_path = unique_temp_audio_path("_invalid.FLAC");
    auto mp3_path = unique_temp_audio_path("_invalid.mp3");

    {
        const unsigned char bytes[] = {0x00, 0x11, 0x22, 0x33, 0x44};
        std::ofstream file(flac_path, std::ios::binary);
        file.write(reinterpret_cast<const char*>(bytes), sizeof(bytes));
        REQUIRE(file.good());
    }
    {
        const unsigned char bytes[] = {0x7F, 0x45, 0x4C, 0x46, 0x00};
        std::ofstream file(mp3_path, std::ios::binary);
        file.write(reinterpret_cast<const char*>(bytes), sizeof(bytes));
        REQUIRE(file.good());
    }

    auto& registry = FormatRegistry::instance();
    auto* flac_reader = registry.find_reader("FLAC");
    auto* mp3_reader = registry.find_reader(".MP3");
    REQUIRE(flac_reader != nullptr);
    REQUIRE(mp3_reader != nullptr);

    REQUIRE_FALSE(flac_reader->read_info(flac_path.string()).has_value());
    REQUIRE_FALSE(flac_reader->read(flac_path.string()).has_value());
    REQUIRE_FALSE(registry.read_info(flac_path.string()).has_value());
    REQUIRE_FALSE(registry.read(flac_path.string()).has_value());

    REQUIRE_FALSE(mp3_reader->read_info(mp3_path.string()).has_value());
    REQUIRE_FALSE(mp3_reader->read(mp3_path.string()).has_value());
    REQUIRE_FALSE(registry.read_info(mp3_path.string()).has_value());
    REQUIRE_FALSE(registry.read(mp3_path.string()).has_value());

    std::filesystem::remove(flac_path);
    std::filesystem::remove(mp3_path);
}

#ifdef __APPLE__
TEST_CASE("FormatRegistry routes CoreAudio compressed containers on Apple",
          "[audio][file][registry][coreaudio][issue-640]") {
    auto& registry = FormatRegistry::instance();

    auto read_extensions = registry.supported_read_extensions();
    REQUIRE(contains_extension(read_extensions, ".aac"));
    REQUIRE(contains_extension(read_extensions, ".m4a"));
    REQUIRE(contains_extension(read_extensions, ".alac"));
    REQUIRE(contains_extension(read_extensions, ".caf"));

    const std::array<std::string_view, 4> extensions = {
        ".M4A",
        ".aac",
        ".ALAC",
        ".caf",
    };

    for (auto ext : extensions) {
        auto* reader = registry.find_reader(ext);
        REQUIRE(reader != nullptr);
        REQUIRE(reader->format_name() == "CoreAudio");

        auto path = unique_temp_audio_path(std::string("_invalid") + std::string(ext));
        {
            const unsigned char bytes[] = {0x43, 0x41, 0x46, 0x46, 0x00, 0x01};
            std::ofstream file(path, std::ios::binary);
            file.write(reinterpret_cast<const char*>(bytes), sizeof(bytes));
            REQUIRE(file.good());
        }

        REQUIRE_FALSE(reader->read_info(path.string()).has_value());
        REQUIRE_FALSE(reader->read(path.string()).has_value());
        REQUIRE_FALSE(registry.read_info(path.string()).has_value());
        REQUIRE_FALSE(registry.read(path.string()).has_value());

        std::filesystem::remove(path);
    }
}
#endif

TEST_CASE("FormatRegistry dispatches custom readers and writers through normalized paths",
          "[audio][file][registry][issue-640]") {
    auto& registry = FormatRegistry::instance();
    auto state = std::make_shared<RegistryProbeState>();

    registry.register_reader(std::make_unique<RegistryProbeReader>(state));
    registry.register_writer(std::make_unique<RegistryProbeWriter>(state));

    REQUIRE(registry.find_reader("AIFC") != nullptr);
    REQUIRE(registry.find_writer(".AIFC") != nullptr);

    auto info_path = (std::filesystem::temp_directory_path() / "pulp_probe_info.AIFC").string();
    auto info = registry.read_info(info_path);
    REQUIRE(info.has_value());
    REQUIRE(info->format == "Probe");
    REQUIRE(info->sample_rate == 22050);
    REQUIRE(info->num_channels == 2);
    REQUIRE(info->num_frames == 3);
    REQUIRE(info->bits_per_sample == 24);
    REQUIRE(state->info_calls == 1);
    REQUIRE(state->last_info_path == info_path);

    auto read_path = (std::filesystem::temp_directory_path() / "pulp_probe_read.aifc").string();
    auto data = registry.read(read_path);
    REQUIRE(data.has_value());
    REQUIRE(data->sample_rate == 22050);
    REQUIRE(data->num_channels() == 2);
    REQUIRE(data->num_frames() == 3);
    REQUIRE_THAT(data->channels[1][2], WithinAbs(1.0f, 0.001f));
    REQUIRE(state->read_calls == 1);
    REQUIRE(state->last_read_path == read_path);

    AudioFileData out;
    out.sample_rate = 48000;
    out.channels = {{0.0f, 0.5f}};
    auto write_path = (std::filesystem::temp_directory_path() / "pulp_probe_write.AIFC").string();
    REQUIRE(registry.write(write_path, out));
    REQUIRE(state->write_calls == 1);
    REQUIRE(state->last_write_path == write_path);
    REQUIRE(state->last_written_channels == 1);
    REQUIRE_FALSE(registry.write(write_path, AudioFileData{}));
    REQUIRE(state->write_calls == 2);

    auto read_extensions = registry.supported_read_extensions();
    REQUIRE(contains_extension(read_extensions, ".aifc"));
    auto write_extensions = registry.supported_write_extensions();
    REQUIRE(contains_extension(write_extensions, ".aifc"));
}

TEST_CASE("FormatRegistry writes and reads AIFF files", "[audio][file][registry][aiff]") {
    auto tmp_path = std::filesystem::temp_directory_path() / "pulp_test_audio_registry.AIFF";
    std::filesystem::remove(tmp_path);

    AudioFileData data;
    data.sample_rate = 48000;
    data.channels = {
        {0.0f, 0.25f, -0.5f, 1.0f},
        {-1.0f, 0.5f, 0.125f, -0.25f},
    };

    auto& registry = FormatRegistry::instance();
    REQUIRE(registry.write(tmp_path.string(), data));
    REQUIRE(std::filesystem::exists(tmp_path));

    auto info = registry.read_info(tmp_path.string());
    REQUIRE(info.has_value());
    REQUIRE(info->format == "AIFF");
    REQUIRE(info->sample_rate == 48000);
    REQUIRE(info->num_channels == 2);
    REQUIRE(info->num_frames == 4);
    REQUIRE(info->bits_per_sample == 16);

    auto read_data = registry.read(tmp_path.string());
    REQUIRE(read_data.has_value());
    REQUIRE(read_data->sample_rate == 48000);
    REQUIRE(read_data->num_channels() == 2);
    REQUIRE(read_data->num_frames() == 4);

    for (size_t channel = 0; channel < data.channels.size(); ++channel) {
        for (size_t frame = 0; frame < data.channels[channel].size(); ++frame) {
            REQUIRE_THAT(read_data->channels[channel][frame],
                         WithinAbs(data.channels[channel][frame], 0.001));
        }
    }

    REQUIRE_FALSE(registry.write(
        (std::filesystem::temp_directory_path() / "pulp_test_audio_registry.unsupported").string(),
        data));
    REQUIRE_FALSE(registry.write(tmp_path.string(), AudioFileData{}));

    std::filesystem::remove(tmp_path);
}

TEST_CASE("AIFF reader rejects malformed files", "[audio][file][registry][aiff]") {
    auto tmp_path = std::filesystem::temp_directory_path() / "pulp_test_audio_malformed.aiff";
    std::filesystem::remove(tmp_path);

    {
        const unsigned char bytes[] = {
            'F', 'O', 'R', 'M', 0, 0, 0, 12, 'A', 'I', 'F', 'F',
            'C', 'O', 'M', 'M', 0, 0, 0, 4, 0, 1, 0, 0,
        };
        std::ofstream file(tmp_path, std::ios::binary);
        file.write(reinterpret_cast<const char*>(bytes), sizeof(bytes));
    }

    auto& registry = FormatRegistry::instance();
    REQUIRE_FALSE(registry.read_info(tmp_path.string()).has_value());
    REQUIRE_FALSE(registry.read(tmp_path.string()).has_value());

    std::filesystem::remove(tmp_path);
}

TEST_CASE("AIFF reader handles AIFC metadata and odd chunk padding", "[audio][file][registry][aiff]") {
    auto tmp_path = std::filesystem::temp_directory_path() / "pulp_test_audio_aifc_8bit.aif";
    std::filesystem::remove(tmp_path);

    write_aiff_fixture(tmp_path,
                       "AIFC",
                       {
                           {"JUNK", {'o', 'd', 'd'}},
                           {"COMM", make_comm_chunk(1, 2, 8, true)},
                           {"SSND", make_ssnd_chunk({0x80, 0x40})},
                       });

    auto& registry = FormatRegistry::instance();
    auto info = registry.read_info(tmp_path.string());
    REQUIRE(info.has_value());
    REQUIRE(info->format == "AIFF-C");
    REQUIRE(info->sample_rate == 44100);
    REQUIRE(info->num_channels == 1);
    REQUIRE(info->num_frames == 2);
    REQUIRE(info->bits_per_sample == 8);

    auto data = registry.read(tmp_path.string());
    REQUIRE(data.has_value());
    REQUIRE(data->sample_rate == 44100);
    REQUIRE(data->num_channels() == 1);
    REQUIRE(data->num_frames() == 2);
    REQUIRE_THAT(data->channels[0][0], WithinAbs(-1.0f, 0.001f));
    REQUIRE_THAT(data->channels[0][1], WithinAbs(0.5f, 0.001f));

    std::filesystem::remove(tmp_path);
}

TEST_CASE("AIFF reader decodes 24-bit PCM samples", "[audio][file][registry][aiff]") {
    auto tmp_path = std::filesystem::temp_directory_path() / "pulp_test_audio_24bit.aiff";
    std::filesystem::remove(tmp_path);

    write_aiff_fixture(tmp_path,
                       "AIFF",
                       {
                           {"COMM", make_comm_chunk(1, 2, 24)},
                           {"SSND", make_ssnd_chunk({0x00, 0x00, 0x00, 0x40, 0x00, 0x00})},
                       });

    auto data = FormatRegistry::instance().read(tmp_path.string());
    REQUIRE(data.has_value());
    REQUIRE(data->num_channels() == 1);
    REQUIRE(data->num_frames() == 2);
    REQUIRE_THAT(data->channels[0][0], WithinAbs(0.0f, 0.001f));
    REQUIRE_THAT(data->channels[0][1], WithinAbs(0.5f, 0.001f));

    std::filesystem::remove(tmp_path);
}

TEST_CASE("AIFF reader decodes 32-bit PCM samples", "[audio][file][registry][aiff]") {
    auto tmp_path = std::filesystem::temp_directory_path() / "pulp_test_audio_32bit.aiff";
    std::filesystem::remove(tmp_path);

    write_aiff_fixture(tmp_path,
                       "AIFF",
                       {
                           {"COMM", make_comm_chunk(1, 2, 32)},
                           {"SSND", make_ssnd_chunk({0x00, 0x00, 0x00, 0x00,
                                                      0x40, 0x00, 0x00, 0x00})},
                       });

    auto data = FormatRegistry::instance().read(tmp_path.string());
    REQUIRE(data.has_value());
    REQUIRE(data->num_channels() == 1);
    REQUIRE(data->num_frames() == 2);
    REQUIRE_THAT(data->channels[0][0], WithinAbs(0.0f, 0.001f));
    REQUIRE_THAT(data->channels[0][1], WithinAbs(0.5f, 0.001f));

    std::filesystem::remove(tmp_path);
}

TEST_CASE("AIFF reader skips malformed SSND chunks before valid data", "[audio][file][registry][aiff]") {
    auto tmp_path = std::filesystem::temp_directory_path() / "pulp_test_audio_ssnd_skip.aiff";
    std::filesystem::remove(tmp_path);

    write_aiff_fixture(tmp_path,
                       "AIFF",
                       {
                           {"COMM", make_comm_chunk(1, 1, 16)},
                           {"SSND", {0x00, 0x00, 0x00, 0x00}},
                           {"SSND", make_ssnd_chunk({0x40, 0x00})},
                       });

    auto data = FormatRegistry::instance().read(tmp_path.string());
    REQUIRE(data.has_value());
    REQUIRE(data->num_channels() == 1);
    REQUIRE(data->num_frames() == 1);
    REQUIRE_THAT(data->channels[0][0], WithinAbs(0.5f, 0.001f));

    std::filesystem::remove(tmp_path);
}

TEST_CASE("AIFF reader rejects truncated PCM payloads", "[audio][file][registry][aiff]") {
    auto tmp_path = std::filesystem::temp_directory_path() / "pulp_test_audio_truncated_payload.aiff";
    std::filesystem::remove(tmp_path);

    write_aiff_fixture(tmp_path,
                       "AIFF",
                       {
                           {"COMM", make_comm_chunk(1, 2, 16)},
                           {"SSND", make_ssnd_chunk({0x40, 0x00})},
                       });

    auto& registry = FormatRegistry::instance();
    REQUIRE(registry.read_info(tmp_path.string()).has_value());
    REQUIRE_FALSE(registry.read(tmp_path.string()).has_value());

    std::filesystem::remove(tmp_path);
}
