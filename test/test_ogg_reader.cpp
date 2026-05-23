#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <pulp/audio/format_registry.hpp>
#include <pulp/runtime/base64.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#ifdef _WIN32
#include <process.h>
#else
#include <unistd.h>
#endif

namespace pulp::audio {
std::unique_ptr<FormatReader> create_ogg_reader();
}

namespace {

namespace fs = std::filesystem;
using Catch::Matchers::WithinAbs;

constexpr std::string_view kTinyOggVorbisBase64 = R"ogg(
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
)ogg";

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
        path = fs::temp_directory_path()
             / ("pulp-ogg-reader-" + std::to_string(current_process_id()) + "-"
                + std::to_string(stamp) + "-" + std::to_string(unique_id));
        fs::create_directories(path);
    }

    ~TempDir() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
};

void write_binary(const fs::path& path, std::string_view bytes) {
    std::ofstream output(path, std::ios::binary);
    REQUIRE(output.good());
    output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    REQUIRE(output.good());
}

void write_base64_audio_fixture(const fs::path& path, std::string_view encoded) {
    auto decoded = pulp::runtime::base64_decode(encoded);
    REQUIRE(decoded.has_value());

    std::ofstream output(path, std::ios::binary);
    REQUIRE(output.good());
    output.write(reinterpret_cast<const char*>(decoded->data()),
                 static_cast<std::streamsize>(decoded->size()));
    REQUIRE(output.good());
}

} // namespace

TEST_CASE("OggReader reports supported extensions and format name",
          "[audio][ogg][issue-640]") {
    auto reader = pulp::audio::create_ogg_reader();
    REQUIRE(reader != nullptr);

    REQUIRE(reader->format_name() == "OGG Vorbis");
    REQUIRE(reader->supports_extension(".ogg"));
    REQUIRE(reader->supports_extension(".oga"));
    REQUIRE_FALSE(reader->supports_extension(".wav"));
    REQUIRE_FALSE(reader->supports_extension(".OGG"));
}

TEST_CASE("OggReader rejects missing and malformed files",
          "[audio][ogg][issue-640]") {
    auto reader = pulp::audio::create_ogg_reader();
    TempDir temp;

    auto missing = (temp.path / "missing.ogg").string();
    REQUIRE_FALSE(reader->read_info(missing).has_value());
    REQUIRE_FALSE(reader->read(missing).has_value());

    auto malformed = temp.path / "malformed.oga";
    write_binary(malformed, "OggS_not_a_valid_vorbis_stream");

    REQUIRE_FALSE(reader->read_info(malformed.string()).has_value());
    REQUIRE_FALSE(reader->read(malformed.string()).has_value());
}

TEST_CASE("OggReader reports metadata for a valid Vorbis stream",
          "[audio][ogg][coverage][phase3-audio]") {
    auto reader = pulp::audio::create_ogg_reader();
    TempDir temp;
    auto path = temp.path / "tiny.ogg";
    write_base64_audio_fixture(path, kTinyOggVorbisBase64);

    REQUIRE(reader != nullptr);
    auto info = reader->read_info(path.string());
    REQUIRE(info.has_value());
    REQUIRE(info->sample_rate == 8000u);
    REQUIRE(info->num_channels == 2u);
    REQUIRE(info->num_frames == 160u);
    REQUIRE(info->bits_per_sample == 16u);
    REQUIRE(info->format == "OGG");
    REQUIRE_THAT(info->duration_seconds, WithinAbs(0.02, 0.000001));
}

TEST_CASE("OggReader decodes a valid Vorbis stream into deinterleaved channels",
          "[audio][ogg][coverage][phase3-audio]") {
    auto reader = pulp::audio::create_ogg_reader();
    TempDir temp;
    auto path = temp.path / "tiny.oga";
    write_base64_audio_fixture(path, kTinyOggVorbisBase64);

    REQUIRE(reader != nullptr);
    auto data = reader->read(path.string());
    REQUIRE(data.has_value());
    REQUIRE(data->sample_rate == 8000u);
    REQUIRE(data->num_channels() == 2u);
    REQUIRE(data->num_frames() == 160u);
    REQUIRE_FALSE(data->empty());
    REQUIRE(data->channels[0].size() == 160u);
    REQUIRE(data->channels[1].size() == 160u);

    auto left_peak = std::max_element(data->channels[0].begin(), data->channels[0].end());
    auto right_peak = std::max_element(data->channels[1].begin(), data->channels[1].end());
    REQUIRE(left_peak != data->channels[0].end());
    REQUIRE(right_peak != data->channels[1].end());
    REQUIRE(*left_peak > 0.0f);
    REQUIRE(*right_peak > 0.0f);
    REQUIRE(*left_peak <= 1.0f);
    REQUIRE(*right_peak <= 1.0f);
}

TEST_CASE("FormatRegistry routes OGG metadata and decode through the built-in reader",
          "[audio][ogg][coverage][phase3-audio]") {
    TempDir temp;
    auto path = temp.path / "registry.ogg";
    auto uppercase_path = temp.path / "registry-uppercase.OGA";
    write_base64_audio_fixture(path, kTinyOggVorbisBase64);
    write_base64_audio_fixture(uppercase_path, kTinyOggVorbisBase64);

    auto& registry = pulp::audio::FormatRegistry::instance();
    auto* reader = registry.find_reader(".ogg");
    REQUIRE(reader != nullptr);
    REQUIRE(registry.find_reader(".OGA") == reader);
    REQUIRE(reader->format_name() == "OGG Vorbis");
    REQUIRE(reader->supports_extension(".ogg"));
    REQUIRE(reader->supports_extension(".oga"));

    auto info = registry.read_info(path.string());
    auto data = registry.read(path.string());
    auto uppercase_info = registry.read_info(uppercase_path.string());
    REQUIRE(info.has_value());
    REQUIRE(data.has_value());
    REQUIRE(uppercase_info.has_value());
    REQUIRE(info->sample_rate == data->sample_rate);
    REQUIRE(info->num_channels == data->num_channels());
    REQUIRE(info->num_frames == data->num_frames());
    REQUIRE(uppercase_info->num_frames == info->num_frames);
    REQUIRE(uppercase_info->format == "OGG");
    REQUIRE(info->format == "OGG");

    REQUIRE(std::all_of(data->channels[0].begin(), data->channels[0].end(),
                        [](float sample) { return sample >= -1.0f && sample <= 1.0f; }));
    REQUIRE(std::all_of(data->channels[1].begin(), data->channels[1].end(),
                        [](float sample) { return sample >= -1.0f && sample <= 1.0f; }));
}
