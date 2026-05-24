#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <pulp/audio/audio_file.hpp>
#include <pulp/audio/format_registry.hpp>
#include <pulp/audio/mmap_reader.hpp>
#include <pulp/audio/offline_processor.hpp>
#include <pulp/audio/streaming_writer.hpp>
#include <pulp/runtime/base64.hpp>
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
        return ext == ".probe";
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
        return ext == ".probe";
    }

    std::string format_name() const override { return "Probe"; }

private:
    std::shared_ptr<RegistryProbeState> state_;
};

class RegistryNamedProbeReader : public FormatReader {
public:
    RegistryNamedProbeReader(std::shared_ptr<RegistryProbeState> state,
                             std::string extension,
                             std::string name)
        : state_(std::move(state))
        , extension_(std::move(extension))
        , name_(std::move(name)) {}

    std::optional<AudioFileInfo> read_info(const std::string& path) override {
        state_->info_calls++;
        state_->last_info_path = path;

        AudioFileInfo info;
        info.sample_rate = 16000;
        info.num_channels = 1;
        info.num_frames = 2;
        info.bits_per_sample = 16;
        info.duration_seconds = 2.0 / 16000.0;
        info.format = name_;
        return info;
    }

    std::optional<AudioFileData> read(const std::string& path) override {
        state_->read_calls++;
        state_->last_read_path = path;

        AudioFileData data;
        data.sample_rate = 16000;
        data.channels = {{0.125f, -0.125f}};
        return data;
    }

    bool supports_extension(std::string_view ext) const override {
        return ext == extension_;
    }

    std::string format_name() const override { return name_; }

private:
    std::shared_ptr<RegistryProbeState> state_;
    std::string extension_;
    std::string name_;
};

class RegistryNamedProbeWriter : public FormatWriter {
public:
    RegistryNamedProbeWriter(std::shared_ptr<RegistryProbeState> state,
                             std::string extension,
                             std::string name)
        : state_(std::move(state))
        , extension_(std::move(extension))
        , name_(std::move(name)) {}

    bool write(const std::string& path, const AudioFileData& data) override {
        state_->write_calls++;
        state_->last_write_path = path;
        state_->last_written_channels = data.num_channels();
        return data.sample_rate != 0 && !data.empty();
    }

    bool supports_extension(std::string_view ext) const override {
        return ext == extension_;
    }

    std::string format_name() const override { return name_; }

private:
    std::shared_ptr<RegistryProbeState> state_;
    std::string extension_;
    std::string name_;
};

static void write_base64_audio_fixture(const std::filesystem::path& path,
                                       std::string_view encoded) {
    auto decoded = pulp::runtime::base64_decode(encoded);
    REQUIRE(decoded.has_value());

    std::ofstream file(path, std::ios::binary);
    REQUIRE(file.good());
    file.write(reinterpret_cast<const char*>(decoded->data()),
               static_cast<std::streamsize>(decoded->size()));
    REQUIRE(file.good());
}

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

static std::vector<unsigned char> make_ssnd_chunk_with_offset(
    const std::vector<unsigned char>& offset_bytes,
    const std::vector<unsigned char>& sample_bytes) {
    std::vector<unsigned char> ssnd;
    append_be32(ssnd, static_cast<uint32_t>(offset_bytes.size()));
    append_be32(ssnd, 0);
    ssnd.insert(ssnd.end(), offset_bytes.begin(), offset_bytes.end());
    ssnd.insert(ssnd.end(), sample_bytes.begin(), sample_bytes.end());
    return ssnd;
}

static std::vector<unsigned char> make_ssnd_chunk(const std::vector<unsigned char>& sample_bytes) {
    return make_ssnd_chunk_with_offset({}, sample_bytes);
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

TEST_CASE("int24 to float preserves sign-extension boundaries",
          "[audio][convert][coverage][phase3]") {
    const uint8_t packed[] = {
        0x01, 0x00, 0x00, // +1
        0xFE, 0xFF, 0xFF, // -2
        0x00, 0x00, 0x40, // +0.5
        0x00, 0x00, 0xC0, // -0.5
    };

    float dst[4] = {};
    int24_to_float(packed, dst, 4);

    REQUIRE_THAT(dst[0], WithinAbs(1.0f / 8388608.0f, 0.0000001f));
    REQUIRE_THAT(dst[1], WithinAbs(-2.0f / 8388608.0f, 0.0000001f));
    REQUIRE_THAT(dst[2], WithinAbs(0.5f, 0.0000001f));
    REQUIRE_THAT(dst[3], WithinAbs(-0.5f, 0.0000001f));
}

TEST_CASE("float to int32 scales fractional values symmetrically",
          "[audio][convert][coverage][phase3]") {
    const float src[] = {-0.75f, -0.25f, 0.25f, 0.75f};
    int32_t dst[4] = {};

    float_to_int32(src, dst, 4);

    REQUIRE(std::abs(dst[0] - static_cast<int32_t>(-0.75 * 2147483647.0)) <= 1);
    REQUIRE(std::abs(dst[1] - static_cast<int32_t>(-0.25 * 2147483647.0)) <= 1);
    REQUIRE(std::abs(dst[2] - static_cast<int32_t>(0.25 * 2147483647.0)) <= 1);
    REQUIRE(std::abs(dst[3] - static_cast<int32_t>(0.75 * 2147483647.0)) <= 1);
}

TEST_CASE("float to integer conversion maps non-finite samples to silence",
          "[audio][convert][coverage][phase3]") {
    const float src[] = {
        std::numeric_limits<float>::quiet_NaN(),
        std::numeric_limits<float>::infinity(),
        -std::numeric_limits<float>::infinity(),
        0.25f,
    };
    int16_t dst16[4] = {};
    int32_t dst32[4] = {};

    float_to_int16(src, dst16, 4);
    float_to_int32(src, dst32, 4);

    REQUIRE(dst16[0] == 0);
    REQUIRE(dst16[1] == 0);
    REQUIRE(dst16[2] == 0);
    REQUIRE(dst16[3] == 8191);
    REQUIRE(dst32[0] == 0);
    REQUIRE(dst32[1] == 0);
    REQUIRE(dst32[2] == 0);
    REQUIRE(std::abs(dst32[3] - static_cast<int32_t>(0.25 * 2147483647.0)) <= 1);
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

TEST_CASE("WAV reader reports zero-frame metadata but rejects loading data",
          "[audio][file][coverage]") {
    auto path = unique_temp_audio_path("_zero_frames.wav");
    std::filesystem::remove(path);

    const std::vector<uint8_t> wav = {
        'R', 'I', 'F', 'F',
        36, 0, 0, 0,
        'W', 'A', 'V', 'E',
        'f', 'm', 't', ' ',
        16, 0, 0, 0,
        1, 0,
        1, 0,
        0x44, 0xAC, 0, 0,
        0x88, 0x58, 0x01, 0,
        2, 0,
        16, 0,
        'd', 'a', 't', 'a',
        0, 0, 0, 0,
    };
    {
        std::ofstream f(path, std::ios::binary);
        f.write(reinterpret_cast<const char*>(wav.data()),
                static_cast<std::streamsize>(wav.size()));
    }

    auto info = read_audio_file_info(path.string());
    REQUIRE(info.has_value());
    REQUIRE(info->sample_rate == 44100);
    REQUIRE(info->num_channels == 1);
    REQUIRE(info->num_frames == 0);
    REQUIRE(info->duration_seconds == 0.0);

    REQUIRE_FALSE(read_audio_file(path.string()).has_value());
    std::filesystem::remove(path);
}

TEST_CASE("WAV reader rejects zero sample-rate metadata",
          "[audio][file][coverage][phase3]") {
    auto path = unique_temp_audio_path("_read_zero_rate.wav");
    std::filesystem::remove(path);

    const std::vector<uint8_t> wav = {
        'R', 'I', 'F', 'F',
        38, 0, 0, 0,
        'W', 'A', 'V', 'E',
        'f', 'm', 't', ' ',
        16, 0, 0, 0,
        1, 0,
        1, 0,
        0, 0, 0, 0,
        0, 0, 0, 0,
        2, 0,
        16, 0,
        'd', 'a', 't', 'a',
        2, 0, 0, 0,
        0, 0,
    };
    {
        std::ofstream f(path, std::ios::binary);
        f.write(reinterpret_cast<const char*>(wav.data()),
                static_cast<std::streamsize>(wav.size()));
    }

    REQUIRE_FALSE(read_audio_file_info(path.string()).has_value());
    REQUIRE_FALSE(read_audio_file(path.string()).has_value());
    std::filesystem::remove(path);
}

TEST_CASE("AudioFileData shape helpers and WAV writer reject first-channel empties",
          "[audio][file][issue-640]") {
    AudioFileData data;
    REQUIRE(data.num_channels() == 0);
    REQUIRE(data.num_frames() == 0);
    REQUIRE(data.empty());

    data.sample_rate = 44100;
    data.channels = {{}, {0.25f, 0.5f}};
    REQUIRE(data.num_channels() == 2);
    REQUIRE(data.num_frames() == 0);
    REQUIRE(data.empty());

    auto path = unique_temp_audio_path("_empty_first_channel.wav");
    std::filesystem::remove(path);
    REQUIRE_FALSE(write_wav_file(path.string(), data));
    REQUIRE_FALSE(std::filesystem::exists(path));

    data.channels = {{0.0f, 0.5f, 1.0f}, {-0.5f}};
    REQUIRE(data.num_channels() == 2);
    REQUIRE(data.num_frames() == 3);
    REQUIRE_FALSE(data.empty());
    REQUIRE_FALSE(write_wav_file(path.string(), data));
    REQUIRE_FALSE(std::filesystem::exists(path));

    auto ragged_path = unique_temp_audio_path("_ragged_channels.wav");
    std::filesystem::remove(ragged_path);
    REQUIRE_FALSE(write_wav_file(ragged_path.string(), data));
    REQUIRE_FALSE(std::filesystem::exists(ragged_path));
}

TEST_CASE("WAV writer rejects zero sample-rate data without creating a file",
          "[audio][file][coverage][phase3]") {
    AudioFileData data;
    data.sample_rate = 0;
    data.channels = {{0.0f, 0.5f}};

    auto path = unique_temp_audio_path("_zero_rate.wav");
    std::filesystem::remove(path);

    REQUIRE_FALSE(write_wav_file(path.string(), data));
    REQUIRE_FALSE(std::filesystem::exists(path));
}

TEST_CASE("WAV and AIFF writers reject channel counts that cannot fit file headers",
          "[audio][file][coverage][phase3]") {
    AudioFileData data;
    data.sample_rate = 44100;
    data.channels.resize(static_cast<size_t>(std::numeric_limits<uint16_t>::max()) + 1u);
    for (auto& channel : data.channels) {
        channel.push_back(0.0f);
    }

    auto wav_path = unique_temp_audio_path("_too_many_channels.wav");
    auto aiff_path = unique_temp_audio_path("_too_many_channels.aiff");
    std::filesystem::remove(wav_path);
    std::filesystem::remove(aiff_path);

    REQUIRE_FALSE(write_wav_file(wav_path.string(), data));
    REQUIRE_FALSE(FormatRegistry::instance().write(aiff_path.string(), data));
    REQUIRE_FALSE(std::filesystem::exists(wav_path));
    REQUIRE_FALSE(std::filesystem::exists(aiff_path));
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

TEST_CASE("MemoryMappedAudioReader fails closed for unsupported mapped files",
          "[audio][file][mmap][issue-640]") {
    auto wav_path = unique_temp_audio_path("_mmap_valid.wav");
    auto text_path = unique_temp_audio_path("_mmap_unsupported.txt");
    std::filesystem::remove(wav_path);
    std::filesystem::remove(text_path);

    AudioFileData data;
    data.sample_rate = 44100;
    data.channels = {{0.0f, 0.25f, 0.5f}};
    REQUIRE(write_wav_file(wav_path.string(), data));
    {
        std::ofstream f(text_path, std::ios::binary);
        f << "not audio";
        REQUIRE(f.good());
    }

    MemoryMappedAudioReader reader;
    REQUIRE(reader.open(wav_path.string()));
    REQUIRE(reader.is_open());
    REQUIRE(reader.info().num_frames == 3);

    REQUIRE_FALSE(reader.open(text_path.string()));
    REQUIRE_FALSE(reader.is_open());
    REQUIRE(reader.info().num_frames == 0);
    REQUIRE(reader.data() == nullptr);
    REQUIRE(reader.size() == 0);

    std::filesystem::remove(wav_path);
    std::filesystem::remove(text_path);
}

TEST_CASE("MemoryMappedAudioReader leaves destinations untouched past EOF",
          "[audio][file][mmap][issue-640]") {
    auto path = unique_temp_audio_path("_mmap_eof.wav");
    std::filesystem::remove(path);

    AudioFileData data;
    data.sample_rate = 32000;
    data.channels = {{0.0f, 0.5f}};
    REQUIRE(write_wav_file(path.string(), data));

    MemoryMappedAudioReader reader;
    REQUIRE(reader.open(path.string()));

    float dest[2] = {-7.0f, -8.0f};
    float* channels[] = {dest};
    REQUIRE(reader.read_frames(channels, 1, 99, 2));
    REQUIRE(dest[0] == -7.0f);
    REQUIRE(dest[1] == -8.0f);

    reader.close();
    std::filesystem::remove(path);
}

TEST_CASE("MemoryMappedAudioReader rejects invalid destinations before decoding",
          "[audio][file][mmap][coverage][phase3]") {
    auto path = unique_temp_audio_path("_mmap_destinations.wav");
    std::filesystem::remove(path);

    AudioFileData data;
    data.sample_rate = 44100;
    data.channels = {
        {0.0f, 0.25f, 0.5f},
        {1.0f, 0.75f, 0.5f},
    };
    REQUIRE(write_wav_file(path.string(), data));

    MemoryMappedAudioReader reader;
    REQUIRE(reader.open(path.string()));

    float left[2] = {-1.0f, -1.0f};
    float* missing_channels[] = {left, nullptr};
    REQUIRE_FALSE(reader.read_frames(nullptr, 2, 0, 1));
    REQUIRE_FALSE(reader.read_frames(missing_channels, 2, 0, 1));
    REQUIRE(left[0] == -1.0f);
    REQUIRE(left[1] == -1.0f);

    REQUIRE(reader.read_frames(nullptr, 0, 0, 1));
    REQUIRE(reader.read_frames(missing_channels, 2, 0, 0));
    float** no_channels = nullptr;
    REQUIRE(reader.read_frames(no_channels, 0, 0, 1));

    reader.close();
    std::filesystem::remove(path);
}

TEST_CASE("MemoryMappedAudioReader reopens files and clamps destination channels",
          "[audio][file][mmap][codecov]") {
    auto stereo_path = unique_temp_audio_path("_mmap_reopen_stereo.wav");
    auto mono_path = unique_temp_audio_path("_mmap_reopen_mono.wav");
    std::filesystem::remove(stereo_path);
    std::filesystem::remove(mono_path);

    AudioFileData stereo;
    stereo.sample_rate = 48000;
    stereo.channels = {
        {0.0f, 0.25f, 0.5f},
        {-0.5f, -0.25f, 0.0f},
    };
    AudioFileData mono;
    mono.sample_rate = 22050;
    mono.channels = {{1.0f, -1.0f}};

    REQUIRE(write_wav_file(stereo_path.string(), stereo));
    REQUIRE(write_wav_file(mono_path.string(), mono));

    MemoryMappedAudioReader reader;
    REQUIRE(reader.open(stereo_path.string()));
    REQUIRE(reader.info().sample_rate == 48000);
    REQUIRE(reader.info().num_channels == 2);

    float left[2] = {-9.0f, -9.0f};
    float right[2] = {-8.0f, -8.0f};
    float extra[2] = {-7.0f, -7.0f};
    float* channels[] = {left, right, extra};
    REQUIRE(reader.read_frames(channels, 3, 1, 2));
    REQUIRE_THAT(left[0], WithinAbs(0.25f, 0.001f));
    REQUIRE_THAT(left[1], WithinAbs(0.5f, 0.001f));
    REQUIRE_THAT(right[0], WithinAbs(-0.25f, 0.001f));
    REQUIRE_THAT(right[1], WithinAbs(0.0f, 0.001f));
    REQUIRE(extra[0] == -7.0f);
    REQUIRE(extra[1] == -7.0f);

    REQUIRE(reader.open(mono_path.string()));
    REQUIRE(reader.info().sample_rate == 22050);
    REQUIRE(reader.info().num_channels == 1);
    REQUIRE(reader.info().num_frames == 2);
    REQUIRE(reader.size() > 44);
    REQUIRE(reader.data() != nullptr);

    auto all = reader.read_all();
    REQUIRE(all.has_value());
    REQUIRE(all->sample_rate == 22050);
    REQUIRE(all->num_channels() == 1);
    REQUIRE(all->num_frames() == 2);
    REQUIRE_THAT(all->channels[0][0], WithinAbs(1.0f, 0.001f));
    REQUIRE_THAT(all->channels[0][1], WithinAbs(-1.0f, 0.001f));

    reader.close();
    REQUIRE_FALSE(reader.is_open());

    std::filesystem::remove(stereo_path);
    std::filesystem::remove(mono_path);
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

    auto ragged = input;
    ragged.channels[1].pop_back();
    bool called = false;
    REQUIRE_FALSE(offline_process(
        ragged,
        [&](const float*, float*, int, int, double) {
            called = true;
        },
        2).has_value());
    REQUIRE_FALSE(called);

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
    REQUIRE_FALSE(writer.open(
        path.string(), 44100, static_cast<uint32_t>(std::numeric_limits<uint16_t>::max()) + 1, 16));
    REQUIRE_FALSE(writer.open(
        path.string(), std::numeric_limits<uint32_t>::max(), std::numeric_limits<uint16_t>::max(), 32));
    REQUIRE_FALSE(writer.open(path.string(), 44100, std::numeric_limits<uint32_t>::max(), 16));
    REQUIRE_FALSE(writer.open(path.string(), std::numeric_limits<uint32_t>::max(), 2, 16));
    REQUIRE_FALSE(writer.is_open());

    REQUIRE(writer.open(path.string(), 44100, 2, 16));
    REQUIRE(writer.write_frames(static_cast<const float*>(nullptr), 1) == 0);
    REQUIRE(writer.write_frames(&sample, 0) == 0);
    REQUIRE(writer.write_frames(&sample, -1) == 0);
    REQUIRE(writer.write_frames(&sample, std::numeric_limits<int>::max()) == 0);

    const float* invalid_channels[] = {&sample, nullptr};
    REQUIRE(writer.write_frames(static_cast<const float* const*>(nullptr), 2, 1) == 0);
    REQUIRE(writer.write_frames(invalid_channels, 2, 1) == 0);
    REQUIRE(writer.write_frames(invalid_channels, 2, 0) == 0);
    REQUIRE(writer.write_frames(invalid_channels, 2, -1) == 0);

    const float* valid_channels[] = {&sample, &sample};
    REQUIRE(writer.write_frames(valid_channels, 2, std::numeric_limits<int>::max()) == 0);
    REQUIRE(writer.frames_written() == 0);

    writer.close();
    writer.close();
    std::filesystem::remove(path);
}

TEST_CASE("StreamingWriter finalizes the active file before a failed reopen",
          "[audio][file][streaming][issue-640]") {
    auto path = unique_temp_audio_path("_stream_reopen_fail.wav");
    std::filesystem::remove(path);

    StreamingWriter writer;
    REQUIRE(writer.open(path.string(), 44100, 1, 16));

    const float frames[] = {-0.25f, 0.25f};
    REQUIRE(writer.write_frames(frames, 2) == 2);
    REQUIRE(writer.frames_written() == 2);

    REQUIRE_FALSE(writer.open(std::filesystem::temp_directory_path().string(), 44100, 1, 16));
    REQUIRE_FALSE(writer.is_open());
    REQUIRE(writer.frames_written() == 0);

    auto bytes = read_binary_file(path);
    require_wav_header(bytes, 1, 44100, 16, 4);
    REQUIRE(static_cast<int16_t>(read_le16(bytes, 44)) == -8191);
    REQUIRE(static_cast<int16_t>(read_le16(bytes, 46)) == 8191);

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

TEST_CASE("StreamingWriter finalizes empty files with zero data size",
          "[audio][file][streaming][coverage][phase3]") {
    auto path = unique_temp_audio_path("_stream_empty.wav");
    std::filesystem::remove(path);

    StreamingWriter writer;
    REQUIRE(writer.open(path.string(), 22050, 2, 16));
    REQUIRE(writer.is_open());
    REQUIRE(writer.frames_written() == 0);

    writer.close();
    REQUIRE_FALSE(writer.is_open());
    REQUIRE(writer.frames_written() == 0);

    auto bytes = read_binary_file(path);
    require_wav_header(bytes, 2, 22050, 16, 0);

    std::filesystem::remove(path);
}

TEST_CASE("StreamingWriter accumulates multiple interleaved writes before close",
          "[audio][file][streaming][coverage][phase3]") {
    auto path = unique_temp_audio_path("_stream_multi_write.wav");
    std::filesystem::remove(path);

    StreamingWriter writer;
    REQUIRE(writer.open(path.string(), 48000, 1, 16));

    const float first[] = {-0.5f, 0.0f};
    const float second[] = {0.25f, 0.75f, 1.0f};
    REQUIRE(writer.write_frames(first, 2) == 2);
    REQUIRE(writer.frames_written() == 2);
    REQUIRE(writer.write_frames(second, 3) == 3);
    REQUIRE(writer.frames_written() == 5);

    writer.close();
    auto bytes = read_binary_file(path);
    require_wav_header(bytes, 1, 48000, 16, 10);

    REQUIRE(static_cast<int16_t>(read_le16(bytes, 44)) == -16383);
    REQUIRE(static_cast<int16_t>(read_le16(bytes, 46)) == 0);
    REQUIRE(static_cast<int16_t>(read_le16(bytes, 48)) == 8191);
    REQUIRE(static_cast<int16_t>(read_le16(bytes, 50)) == 24575);
    REQUIRE(static_cast<int16_t>(read_le16(bytes, 52)) == 32767);

    std::filesystem::remove(path);
}

TEST_CASE("StreamingWriter successful reopen finalizes prior file and resets state",
          "[audio][file][streaming][coverage][phase3]") {
    auto first_path = unique_temp_audio_path("_stream_reopen_first.wav");
    auto second_path = unique_temp_audio_path("_stream_reopen_second.wav");
    std::filesystem::remove(first_path);
    std::filesystem::remove(second_path);

    StreamingWriter writer;
    REQUIRE(writer.open(first_path.string(), 44100, 1, 16));
    const float first_frames[] = {-1.0f, 1.0f};
    REQUIRE(writer.write_frames(first_frames, 2) == 2);
    REQUIRE(writer.frames_written() == 2);

    REQUIRE(writer.open(second_path.string(), 32000, 2, 24));
    REQUIRE(writer.is_open());
    REQUIRE(writer.frames_written() == 0);

    const float second_frames[] = {
        -0.25f, 0.25f,
         0.5f, -0.5f,
    };
    REQUIRE(writer.write_frames(second_frames, 2) == 2);
    REQUIRE(writer.frames_written() == 2);
    writer.close();

    auto first_bytes = read_binary_file(first_path);
    require_wav_header(first_bytes, 1, 44100, 16, 4);
    REQUIRE(static_cast<int16_t>(read_le16(first_bytes, 44)) == -32767);
    REQUIRE(static_cast<int16_t>(read_le16(first_bytes, 46)) == 32767);

    auto second_bytes = read_binary_file(second_path);
    require_wav_header(second_bytes, 2, 32000, 24, 12);
    REQUIRE(read_le24_signed(second_bytes, 44) == -2097151);
    REQUIRE(read_le24_signed(second_bytes, 47) == 2097151);
    REQUIRE(read_le24_signed(second_bytes, 50) == 4194303);
    REQUIRE(read_le24_signed(second_bytes, 53) == -4194303);

    std::filesystem::remove(first_path);
    std::filesystem::remove(second_path);
}

TEST_CASE("StreamingWriter writes deinterleaved mono through channel dispatch",
          "[audio][file][streaming][coverage][phase3]") {
    auto path = unique_temp_audio_path("_stream_deinterleaved_mono.wav");
    std::filesystem::remove(path);

    StreamingWriter writer;
    REQUIRE(writer.open(path.string(), 96000, 1, 32));

    const float mono[] = {-1.0f, -0.25f, 0.0f, 0.25f, 1.0f};
    const float* channels[] = {mono};
    REQUIRE(writer.write_frames(channels, 1, 5) == 5);
    REQUIRE(writer.frames_written() == 5);
    writer.close();

    auto bytes = read_binary_file(path);
    require_wav_header(bytes, 1, 96000, 32, 20);
    REQUIRE(read_le32_signed(bytes, 44) == std::numeric_limits<int32_t>::min());
    REQUIRE(read_le32_signed(bytes, 48) == -536870912);
    REQUIRE(read_le32_signed(bytes, 52) == 0);
    REQUIRE(read_le32_signed(bytes, 56) == 536870912);
    REQUIRE(read_le32_signed(bytes, 60) == std::numeric_limits<int32_t>::max());

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
    REQUIRE(registry.find_reader(".aifc") != nullptr);
    REQUIRE(registry.find_writer(".aiff") != nullptr);
    REQUIRE(registry.find_writer("AIF") != nullptr);
    REQUIRE(registry.find_writer(".aifc") == nullptr);
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
    REQUIRE(contains_extension(read_extensions, ".aifc"));

    auto write_extensions = registry.supported_write_extensions();
    REQUIRE(contains_extension(write_extensions, ".wav"));
    REQUIRE(contains_extension(write_extensions, ".wave"));
    REQUIRE(contains_extension(write_extensions, ".aiff"));
    REQUIRE(contains_extension(write_extensions, ".aif"));
    REQUIRE_FALSE(contains_extension(write_extensions, ".aifc"));
}

TEST_CASE("FormatRegistry ignores null custom handlers",
          "[audio][file][registry][coverage][phase3]") {
    auto& registry = FormatRegistry::instance();

    const auto read_before = registry.supported_read_extensions();
    const auto write_before = registry.supported_write_extensions();

    registry.register_reader(nullptr);
    registry.register_writer(nullptr);

    REQUIRE(registry.supported_read_extensions() == read_before);
    REQUIRE(registry.supported_write_extensions() == write_before);
    REQUIRE(registry.find_reader(".wav") != nullptr);
    REQUIRE(registry.find_writer(".wav") != nullptr);
}

TEST_CASE("FormatRegistry rejects missing and extension-only paths",
          "[audio][file][registry][coverage][phase3]") {
    auto& registry = FormatRegistry::instance();

    REQUIRE(registry.find_reader("") == nullptr);
    REQUIRE(registry.find_writer("") == nullptr);
    REQUIRE_FALSE(registry.read_info("pulp_audio_without_extension").has_value());
    REQUIRE_FALSE(registry.read("pulp_audio_without_extension").has_value());

    AudioFileData data;
    data.sample_rate = 44100;
    data.channels = {{0.0f, 0.25f}};
    REQUIRE_FALSE(registry.write("pulp_audio_without_extension", data));
    REQUIRE_FALSE(registry.write(".wav", data));
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

TEST_CASE("FormatRegistry reads a valid MP3 fixture through the built-in reader",
          "[audio][file][registry][codecov]") {
    auto mp3_path = unique_temp_audio_path("_valid.MP3");
    write_base64_audio_fixture(
        mp3_path,
        "SUQzBAAAAAAAIlRTU0UAAAAOAAADTGF2ZjYxLjcuMTAwAAAAAAAAAAAAAAD/40jAAAAAAAAAAAAA"
        "SW5mbwAAAA8AAAADAAAEgACAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgIDAwMDAwMDA"
        "wMDAwMDAwMDAwMDAwMDAwMDAwMDAwMDAwMD/////////////////////////////////////////"
        "//8AAAAATGF2YzYxLjE5AAAAAAAAAAAAAAAAJAOQAAAAAAAABIBXuUwoAAAAAAAAAAAAAAAAAAAA"
        "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
        "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAD/40jEAAAAA0gA"
        "AAAATEFNRTMuMTAwVVVVVVVVVVVVVVVMQU1FMy4xMDBVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVV"
        "VVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVV"
        "VVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVV"
        "VVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVV"
        "VVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVX/40jEAAAA"
        "A0gAAAAATEFNRTMuMTAwVVVVVVVVVVVVVVVMQU1FMy4xMDBVVVVVVVVVVVVVVVVVVVVVVVVVVVVV"
        "VVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVV"
        "VVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVV"
        "VVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVV"
        "VVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVX/40jE"
        "AAAAA0gAAAAATEFNRTMuMTAwVVVVVVVVVVVVVVVMQU1FMy4xMDBVVVVVVVVVVVVVVVVVVVVVVVVV"
        "VVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVV"
        "VVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVV"
        "VVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVV"
        "VVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVU=");

    auto& registry = FormatRegistry::instance();
    auto* wav_reader = registry.find_reader(".wav");
    auto* wav_writer = registry.find_writer(".wave");
    auto* flac_reader = registry.find_reader("flac");
    auto* mp3_reader = registry.find_reader(".MP3");

    REQUIRE(wav_reader != nullptr);
    REQUIRE(wav_writer != nullptr);
    REQUIRE(flac_reader != nullptr);
    REQUIRE(mp3_reader != nullptr);
    REQUIRE(wav_reader->format_name() == "WAV");
    REQUIRE(wav_writer->format_name() == "WAV");
    REQUIRE(flac_reader->format_name() == "FLAC");
    REQUIRE(mp3_reader->format_name() == "MP3");
    REQUIRE(wav_reader->supports_extension(".wav"));
    REQUIRE(wav_reader->supports_extension(".wave"));
    REQUIRE_FALSE(wav_reader->supports_extension(".mp3"));
    REQUIRE(flac_reader->supports_extension(".flac"));
    REQUIRE_FALSE(flac_reader->supports_extension(".FLAC"));
    REQUIRE_FALSE(flac_reader->supports_extension(".wav"));
    REQUIRE(mp3_reader->supports_extension(".mp3"));
    REQUIRE_FALSE(mp3_reader->supports_extension(".MP3"));
    REQUIRE_FALSE(mp3_reader->supports_extension(".wav"));

    auto read_extensions = registry.supported_read_extensions();
    REQUIRE(contains_extension(read_extensions, ".mp3"));
    REQUIRE(contains_extension(read_extensions, ".flac"));

    auto info = mp3_reader->read_info(mp3_path.string());
    REQUIRE(info.has_value());
    REQUIRE(info->sample_rate == 8000);
    REQUIRE(info->num_channels == 1);
    REQUIRE(info->num_frames > 0);
    REQUIRE(info->bits_per_sample == 16);
    REQUIRE(info->format == "MP3");
    REQUIRE(info->duration_seconds > 0.0);

    auto registry_info = registry.read_info(mp3_path.string());
    REQUIRE(registry_info.has_value());
    REQUIRE(registry_info->format == "MP3");
    REQUIRE(registry_info->num_channels == 1);

    auto data = registry.read(mp3_path.string());
    REQUIRE(data.has_value());
    REQUIRE(data->sample_rate == 8000);
    REQUIRE(data->num_channels() == 1);
    REQUIRE(data->num_frames() > 0);
    REQUIRE(data->num_frames() == info->num_frames);
    REQUIRE(data->channels.front().size() == data->num_frames());

    std::filesystem::remove(mp3_path);
}

TEST_CASE("FormatRegistry rejects paths without dispatchable extensions",
          "[audio][file][registry][coverage][phase3]") {
    auto& registry = FormatRegistry::instance();

    REQUIRE(registry.find_reader("") == nullptr);
    REQUIRE(registry.find_writer("") == nullptr);
    REQUIRE(registry.find_reader(".") == nullptr);
    REQUIRE(registry.find_writer(".") == nullptr);

    auto no_extension = unique_temp_audio_path("");
    {
        std::ofstream file(no_extension, std::ios::binary);
        file << "RIFF";
        REQUIRE(file.good());
    }

    REQUIRE_FALSE(registry.read_info(no_extension.string()).has_value());
    REQUIRE_FALSE(registry.read(no_extension.string()).has_value());

    AudioFileData data;
    data.sample_rate = 44100;
    data.channels = {{0.0f, 0.25f}};
    REQUIRE_FALSE(registry.write(no_extension.string(), data));

    auto hidden_name = std::filesystem::temp_directory_path() / ".pulp_audio_hidden";
    {
        std::ofstream file(hidden_name, std::ios::binary);
        file << "not audio";
        REQUIRE(file.good());
    }

    REQUIRE_FALSE(registry.read_info(hidden_name.string()).has_value());
    REQUIRE_FALSE(registry.read(hidden_name.string()).has_value());

    std::filesystem::remove(no_extension);
    std::filesystem::remove(hidden_name);
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

    REQUIRE(registry.find_reader("PROBE") != nullptr);
    REQUIRE(registry.find_writer(".PROBE") != nullptr);

    auto info_path = (std::filesystem::temp_directory_path() / "pulp_probe_info.PROBE").string();
    auto info = registry.read_info(info_path);
    REQUIRE(info.has_value());
    REQUIRE(info->format == "Probe");
    REQUIRE(info->sample_rate == 22050);
    REQUIRE(info->num_channels == 2);
    REQUIRE(info->num_frames == 3);
    REQUIRE(info->bits_per_sample == 24);
    REQUIRE(state->info_calls == 1);
    REQUIRE(state->last_info_path == info_path);

    auto read_path = (std::filesystem::temp_directory_path() / "pulp_probe_read.probe").string();
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
    auto write_path = (std::filesystem::temp_directory_path() / "pulp_probe_write.PROBE").string();
    REQUIRE(registry.write(write_path, out));
    REQUIRE(state->write_calls == 1);
    REQUIRE(state->last_write_path == write_path);
    REQUIRE(state->last_written_channels == 1);
    REQUIRE_FALSE(registry.write(write_path, AudioFileData{}));
    REQUIRE(state->write_calls == 2);
}

TEST_CASE("FormatRegistry keeps the first custom handler for duplicate extensions",
          "[audio][file][registry][codecov]") {
    auto& registry = FormatRegistry::instance();
    auto first = std::make_shared<RegistryProbeState>();
    auto second = std::make_shared<RegistryProbeState>();
    auto writer_state = std::make_shared<RegistryProbeState>();

    registry.register_reader(std::make_unique<RegistryNamedProbeReader>(
        first, ".dupeaudio", "FirstDupe"));
    registry.register_reader(std::make_unique<RegistryNamedProbeReader>(
        second, ".dupeaudio", "SecondDupe"));
    registry.register_writer(std::make_unique<RegistryNamedProbeWriter>(
        writer_state, ".dupeaudio", "DupeWriter"));

    auto* reader = registry.find_reader("DUPEAUDIO");
    REQUIRE(reader != nullptr);
    REQUIRE(reader->format_name() == "FirstDupe");

    auto read_path = (std::filesystem::temp_directory_path()
        / "pulp_probe.multi.part.DUPEAUDIO").string();
    auto info = registry.read_info(read_path);
    REQUIRE(info.has_value());
    REQUIRE(info->format == "FirstDupe");
    REQUIRE(first->info_calls == 1);
    REQUIRE(second->info_calls == 0);
    REQUIRE(first->last_info_path == read_path);

    auto data = registry.read(read_path);
    REQUIRE(data.has_value());
    REQUIRE(data->num_channels() == 1);
    REQUIRE(data->num_frames() == 2);
    REQUIRE(first->read_calls == 1);
    REQUIRE(second->read_calls == 0);

    AudioFileData out;
    out.sample_rate = 32000;
    out.channels = {{0.0f, 0.25f}};
    auto write_path = (std::filesystem::temp_directory_path()
        / "pulp_probe_write.DUPEAUDIO").string();
    REQUIRE(registry.write(write_path, out));
    REQUIRE(writer_state->write_calls == 1);
    REQUIRE(writer_state->last_write_path == write_path);
    REQUIRE(writer_state->last_written_channels == 1);
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

    auto ragged = data;
    ragged.channels[1].pop_back();
    auto ragged_path = unique_temp_audio_path("_aiff_ragged.aiff");
    std::filesystem::remove(ragged_path);
    REQUIRE_FALSE(registry.write(ragged_path.string(), ragged));
    REQUIRE_FALSE(std::filesystem::exists(ragged_path));

    std::filesystem::remove(tmp_path);
}

TEST_CASE("AIFF writer rejects invalid source data without creating files",
          "[audio][file][registry][aiff][coverage][phase3]") {
    auto& registry = FormatRegistry::instance();

    SECTION("zero sample rate") {
        auto tmp_path = unique_temp_audio_path("_aiff_writer_zero_rate.aiff");
        std::filesystem::remove(tmp_path);

        AudioFileData data;
        data.sample_rate = 0;
        data.channels = {{0.0f, 0.5f}};

        REQUIRE_FALSE(registry.write(tmp_path.string(), data));
        REQUIRE_FALSE(std::filesystem::exists(tmp_path));
    }

    SECTION("ragged channel lengths") {
        auto tmp_path = unique_temp_audio_path("_aiff_writer_ragged.aiff");
        std::filesystem::remove(tmp_path);

        AudioFileData data;
        data.sample_rate = 44100;
        data.channels = {{0.0f, 0.5f}, {1.0f}};

        REQUIRE_FALSE(registry.write(tmp_path.string(), data));
        REQUIRE_FALSE(std::filesystem::exists(tmp_path));
    }
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
    auto tmp_path = std::filesystem::temp_directory_path() / "pulp_test_audio_aifc_8bit.aifc";
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

TEST_CASE("AIFF reader rejects unsupported AIFC compression types",
          "[audio][file][registry][aiff]") {
    auto tmp_path = unique_temp_audio_path("_aiff_ulaw.aifc");
    auto comm = make_comm_chunk(1, 2, 8, true);
    comm[18] = 'u';
    comm[19] = 'l';
    comm[20] = 'a';
    comm[21] = 'w';

    write_aiff_fixture(tmp_path,
                       "AIFC",
                       {
                           {"COMM", comm},
                           {"SSND", make_ssnd_chunk({0x80, 0x40})},
                       });

    auto& registry = FormatRegistry::instance();
    auto info = registry.read_info(tmp_path.string());
    REQUIRE(info.has_value());
    REQUIRE(info->format == "AIFF-C");
    REQUIRE_FALSE(registry.read(tmp_path.string()).has_value());

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

TEST_CASE("AIFF reader honors SSND offsets and rejects invalid offsets",
          "[audio][file][registry][aiff][coverage][phase3]") {
    auto& registry = FormatRegistry::instance();

    SECTION("valid offset skips pad bytes before PCM") {
        auto tmp_path = unique_temp_audio_path("_aiff_ssnd_offset.aiff");

        write_aiff_fixture(tmp_path,
                           "AIFF",
                           {
                               {"COMM", make_comm_chunk(1, 1, 16)},
                               {"SSND", make_ssnd_chunk_with_offset({0x7F, 0x7F}, {0x40, 0x00})},
                           });

        auto data = registry.read(tmp_path.string());
        REQUIRE(data.has_value());
        REQUIRE(data->num_channels() == 1);
        REQUIRE(data->num_frames() == 1);
        REQUIRE_THAT(data->channels[0][0], WithinAbs(0.5f, 0.001f));
        std::filesystem::remove(tmp_path);
    }

    SECTION("offset beyond payload is malformed") {
        auto tmp_path = unique_temp_audio_path("_aiff_bad_ssnd_offset.aiff");
        std::vector<unsigned char> ssnd;
        append_be32(ssnd, 4);
        append_be32(ssnd, 0);
        ssnd.insert(ssnd.end(), {0x40, 0x00});

        write_aiff_fixture(tmp_path,
                           "AIFF",
                           {
                               {"COMM", make_comm_chunk(1, 1, 16)},
                               {"SSND", ssnd},
                           });

        REQUIRE_FALSE(registry.read(tmp_path.string()).has_value());
        std::filesystem::remove(tmp_path);
    }
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

TEST_CASE("AIFF reader rejects invalid COMM metadata and unsupported PCM depths",
          "[audio][file][registry][aiff][issue-640]") {
    auto& registry = FormatRegistry::instance();

    SECTION("zero sample rate") {
        auto tmp_path = unique_temp_audio_path("_aiff_zero_rate.aiff");
        auto comm = make_comm_chunk(1, 1, 16);
        std::fill(comm.begin() + 8, comm.begin() + 18, 0);

        write_aiff_fixture(tmp_path,
                           "AIFF",
                           {
                               {"COMM", comm},
                               {"SSND", make_ssnd_chunk({0x40, 0x00})},
                           });

        REQUIRE_FALSE(registry.read_info(tmp_path.string()).has_value());
        REQUIRE_FALSE(registry.read(tmp_path.string()).has_value());
        std::filesystem::remove(tmp_path);
    }

    SECTION("zero channels") {
        auto tmp_path = unique_temp_audio_path("_aiff_zero_channels.aiff");

        write_aiff_fixture(tmp_path,
                           "AIFF",
                           {
                               {"COMM", make_comm_chunk(0, 1, 16)},
                               {"SSND", make_ssnd_chunk({0x40, 0x00})},
                           });

        REQUIRE_FALSE(registry.read_info(tmp_path.string()).has_value());
        REQUIRE_FALSE(registry.read(tmp_path.string()).has_value());
        std::filesystem::remove(tmp_path);
    }

    SECTION("unsupported PCM bit depth") {
        auto tmp_path = unique_temp_audio_path("_aiff_20bit.aiff");

        write_aiff_fixture(tmp_path,
                           "AIFF",
                           {
                               {"COMM", make_comm_chunk(1, 1, 20)},
                               {"SSND", make_ssnd_chunk({0x40, 0x00})},
                           });

        auto info = registry.read_info(tmp_path.string());
        REQUIRE(info.has_value());
        REQUIRE(info->bits_per_sample == 20);
        REQUIRE_FALSE(registry.read(tmp_path.string()).has_value());
        std::filesystem::remove(tmp_path);
    }
}

TEST_CASE("AIFF reader rejects malformed container headers and missing chunks",
          "[audio][file][registry][aiff][coverage][phase3]") {
    auto& registry = FormatRegistry::instance();

    SECTION("missing file") {
        auto missing = unique_temp_audio_path("_aiff_missing.aiff");
        std::filesystem::remove(missing);

        REQUIRE_FALSE(registry.read_info(missing.string()).has_value());
        REQUIRE_FALSE(registry.read(missing.string()).has_value());
    }

    SECTION("short header") {
        auto tmp_path = unique_temp_audio_path("_aiff_short_header.aiff");
        {
            std::ofstream file(tmp_path, std::ios::binary);
            file.write("FORM", 4);
        }

        REQUIRE_FALSE(registry.read_info(tmp_path.string()).has_value());
        REQUIRE_FALSE(registry.read(tmp_path.string()).has_value());
        std::filesystem::remove(tmp_path);
    }

    SECTION("non-FORM header") {
        auto tmp_path = unique_temp_audio_path("_aiff_bad_form.aiff");
        {
            const unsigned char bytes[] = {
                'F', 'A', 'K', 'E', 0, 0, 0, 4, 'A', 'I', 'F', 'F',
            };
            std::ofstream file(tmp_path, std::ios::binary);
            file.write(reinterpret_cast<const char*>(bytes), sizeof(bytes));
        }

        REQUIRE_FALSE(registry.read_info(tmp_path.string()).has_value());
        REQUIRE_FALSE(registry.read(tmp_path.string()).has_value());
        std::filesystem::remove(tmp_path);
    }

    SECTION("unsupported FORM type") {
        auto tmp_path = unique_temp_audio_path("_aiff_bad_type.aiff");
        write_aiff_fixture(tmp_path, "8SVX", {{"COMM", make_comm_chunk(1, 1, 16)}});

        REQUIRE_FALSE(registry.read_info(tmp_path.string()).has_value());
        REQUIRE_FALSE(registry.read(tmp_path.string()).has_value());
        std::filesystem::remove(tmp_path);
    }

    SECTION("missing COMM chunk") {
        auto tmp_path = unique_temp_audio_path("_aiff_missing_comm.aiff");
        write_aiff_fixture(tmp_path, "AIFF", {{"SSND", make_ssnd_chunk({0x40, 0x00})}});

        REQUIRE_FALSE(registry.read_info(tmp_path.string()).has_value());
        REQUIRE_FALSE(registry.read(tmp_path.string()).has_value());
        std::filesystem::remove(tmp_path);
    }

    SECTION("missing SSND chunk") {
        auto tmp_path = unique_temp_audio_path("_aiff_missing_ssnd.aiff");
        write_aiff_fixture(tmp_path, "AIFF", {{"COMM", make_comm_chunk(1, 1, 16)}});

        auto info = registry.read_info(tmp_path.string());
        REQUIRE(info.has_value());
        REQUIRE_FALSE(registry.read(tmp_path.string()).has_value());
        std::filesystem::remove(tmp_path);
    }

    SECTION("zero-frame COMM metadata has info but no readable audio") {
        auto tmp_path = unique_temp_audio_path("_aiff_zero_frames.aiff");
        write_aiff_fixture(tmp_path,
                           "AIFF",
                           {
                               {"COMM", make_comm_chunk(1, 0, 16)},
                               {"SSND", make_ssnd_chunk({})},
                           });

        auto info = registry.read_info(tmp_path.string());
        REQUIRE(info.has_value());
        REQUIRE(info->num_frames == 0);
        REQUIRE_FALSE(registry.read(tmp_path.string()).has_value());
        std::filesystem::remove(tmp_path);
    }

    SECTION("zero-channel COMM metadata is rejected by info and full reads") {
        auto tmp_path = unique_temp_audio_path("_aiff_zero_channels_comm.aiff");
        write_aiff_fixture(tmp_path,
                           "AIFF",
                           {
                               {"COMM", make_comm_chunk(0, 1, 16)},
                               {"SSND", make_ssnd_chunk({0x40, 0x00})},
                           });

        REQUIRE_FALSE(registry.read_info(tmp_path.string()).has_value());
        REQUIRE_FALSE(registry.read(tmp_path.string()).has_value());
        std::filesystem::remove(tmp_path);
    }
}

TEST_CASE("AIFF reader covers padding metadata and signed extended rates",
          "[audio][file][registry][aiff][coverage][phase3]") {
    auto& registry = FormatRegistry::instance();

    SECTION("COMM extra bytes and odd SSND padding") {
        auto tmp_path = unique_temp_audio_path("_aiff_extra_comm_odd_ssnd.aif");
        auto comm = make_comm_chunk(1, 1, 8);
        comm.insert(comm.end(), {'N', 'A', 'M', 'E', 0, 'x', 'y'});

        write_aiff_fixture(tmp_path,
                           "AIFF",
                           {
                               {"COMM", comm},
                               {"SSND", make_ssnd_chunk({0x40})},
                           });

        auto info = registry.read_info(tmp_path.string());
        REQUIRE(info.has_value());
        REQUIRE(info->format == "AIFF");
        REQUIRE(info->num_channels == 1);
        REQUIRE(info->num_frames == 1);
        REQUIRE(info->bits_per_sample == 8);

        auto data = registry.read(tmp_path.string());
        REQUIRE(data.has_value());
        REQUIRE(data->sample_rate == 44100);
        REQUIRE_THAT(data->channels[0][0], WithinAbs(0.5f, 0.001f));
        std::filesystem::remove(tmp_path);
    }

    SECTION("negative extended sample rate is rejected") {
        auto tmp_path = unique_temp_audio_path("_aiff_negative_rate.aiff");
        auto comm = make_comm_chunk(1, 1, 16);
        comm[8] |= 0x80;

        write_aiff_fixture(tmp_path,
                           "AIFF",
                           {
                               {"COMM", comm},
                               {"SSND", make_ssnd_chunk({0x40, 0x00})},
                           });

        REQUIRE_FALSE(registry.read_info(tmp_path.string()).has_value());
        REQUIRE_FALSE(registry.read(tmp_path.string()).has_value());
        std::filesystem::remove(tmp_path);
    }

    SECTION("overflow extended sample rate is rejected") {
        auto tmp_path = unique_temp_audio_path("_aiff_overflow_rate.aiff");
        auto comm = make_comm_chunk(1, 1, 16);
        const unsigned char huge_rate[10] = {0x40, 0x1F, 0x80, 0, 0, 0, 0, 0, 0, 0};
        std::copy(std::begin(huge_rate), std::end(huge_rate), comm.begin() + 8);

        write_aiff_fixture(tmp_path,
                           "AIFF",
                           {
                               {"COMM", comm},
                               {"SSND", make_ssnd_chunk({0x40, 0x00})},
                           });

        REQUIRE_FALSE(registry.read_info(tmp_path.string()).has_value());
        REQUIRE_FALSE(registry.read(tmp_path.string()).has_value());
        std::filesystem::remove(tmp_path);
    }

    SECTION("non-finite extended sample rate is rejected") {
        auto tmp_path = unique_temp_audio_path("_aiff_nonfinite_rate.aiff");
        auto comm = make_comm_chunk(1, 1, 16);
        const unsigned char nonfinite_rate[10] = {0x7F, 0xFF, 0xFF, 0xFF, 0xFF,
                                                  0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
        std::copy(std::begin(nonfinite_rate), std::end(nonfinite_rate), comm.begin() + 8);

        write_aiff_fixture(tmp_path,
                           "AIFF",
                           {
                               {"COMM", comm},
                               {"SSND", make_ssnd_chunk({0x40, 0x00})},
                           });

        REQUIRE_FALSE(registry.read_info(tmp_path.string()).has_value());
        REQUIRE_FALSE(registry.read(tmp_path.string()).has_value());
        std::filesystem::remove(tmp_path);
    }

    SECTION("AIFC COMM without compression type is rejected on read") {
        auto tmp_path = unique_temp_audio_path("_aiff_short_aifc_comm.aifc");

        write_aiff_fixture(tmp_path,
                           "AIFC",
                           {
                               {"COMM", make_comm_chunk(1, 1, 16)},
                               {"SSND", make_ssnd_chunk({0x40, 0x00})},
                           });

        auto info = registry.read_info(tmp_path.string());
        REQUIRE(info.has_value());
        REQUIRE_FALSE(registry.read(tmp_path.string()).has_value());
        std::filesystem::remove(tmp_path);
    }
}

TEST_CASE("AIFF writer rejects edge shapes and writes AIF extension",
          "[audio][file][registry][aiff][coverage][phase3]") {
    auto& registry = FormatRegistry::instance();

    SECTION("empty first channel") {
        auto tmp_path = unique_temp_audio_path("_aiff_empty_first_channel.aiff");
        AudioFileData data;
        data.sample_rate = 44100;
        data.channels = {{}};

        REQUIRE_FALSE(registry.write(tmp_path.string(), data));
    }

    SECTION("zero channels with a sample rate") {
        auto tmp_path = unique_temp_audio_path("_aiff_zero_channels.aiff");
        AudioFileData data;
        data.sample_rate = 44100;

        REQUIRE_FALSE(registry.write(tmp_path.string(), data));
    }

    SECTION("aif extension round-trips and clamps samples") {
        auto tmp_path = unique_temp_audio_path("_aiff_writer_extension.aif");
        std::filesystem::remove(tmp_path);

        AudioFileData data;
        data.sample_rate = 44100;
        data.channels = {{2.0f, -2.0f, 0.0f}};

        REQUIRE(registry.write(tmp_path.string(), data));
        auto info = registry.read_info(tmp_path.string());
        REQUIRE(info.has_value());
        REQUIRE(info->format == "AIFF");
        REQUIRE(info->sample_rate == 44100);
        REQUIRE(info->num_frames == 3);

        auto read_data = registry.read(tmp_path.string());
        REQUIRE(read_data.has_value());
        REQUIRE(read_data->num_channels() == 1);
        REQUIRE(read_data->num_frames() == 3);
        REQUIRE_THAT(read_data->channels[0][0], WithinAbs(0.999f, 0.002f));
        REQUIRE_THAT(read_data->channels[0][1], WithinAbs(-0.999f, 0.002f));
        REQUIRE_THAT(read_data->channels[0][2], WithinAbs(0.0f, 0.001f));
        std::filesystem::remove(tmp_path);
    }
}
