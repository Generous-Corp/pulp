#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <pulp/audio/audio_file.hpp>
#include <pulp/audio/format_registry.hpp>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <cmath>
#include <utility>

using namespace pulp::audio;
using Catch::Matchers::WithinAbs;

static bool contains_extension(const std::vector<std::string>& extensions,
                               std::string_view extension) {
    return std::find(extensions.begin(), extensions.end(), extension) != extensions.end();
}

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
