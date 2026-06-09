#include <catch2/catch_test_macros.hpp>

#include <pulp/audio/audio_file.hpp>
#include <pulp/audio/sample_asset_io.hpp>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <string>
#include <vector>

using pulp::audio::AudioFileData;
using pulp::audio::SampleAssetExporter;
using pulp::audio::SampleAssetImporter;
using pulp::audio::SampleAssetKind;
using pulp::audio::SampleAssetPolicy;
using pulp::audio::SampleAssetStatus;

namespace {

std::filesystem::path unique_path(const char* suffix) {
    static int counter = 0;
    const auto tick = std::chrono::steady_clock::now().time_since_epoch().count();
    auto name = std::string("pulp_sample_asset_io_") + std::to_string(tick) + "_" +
                std::to_string(counter++) + suffix;
    return std::filesystem::temp_directory_path() / name;
}

AudioFileData make_audio(std::uint32_t sample_rate = 48000,
                         std::uint64_t frames = 16,
                         std::uint32_t channels = 2) {
    AudioFileData audio;
    audio.sample_rate = sample_rate;
    audio.channels.resize(channels);
    for (std::uint32_t ch = 0; ch < channels; ++ch) {
        audio.channels[ch].resize(static_cast<std::size_t>(frames));
        for (std::uint64_t frame = 0; frame < frames; ++frame) {
            audio.channels[ch][static_cast<std::size_t>(frame)] =
                static_cast<float>(0.01 * static_cast<double>(frame + ch));
        }
    }
    return audio;
}

}  // namespace

TEST_CASE("SampleAssetImporter describes and imports WAV assets with budgets",
          "[audio][sampler][asset_io]") {
    const auto path = unique_path(".WAV");
    std::filesystem::remove(path);
    REQUIRE(pulp::audio::write_wav_file(path.string(), make_audio(44100, 12, 2)));

    SampleAssetImporter importer;
    REQUIRE(importer.supports_audio_extension("wav"));
    REQUIRE(importer.supports_audio_extension(".WAV"));

    SampleAssetPolicy policy;
    policy.max_channels = 2;
    policy.max_frames = 32;
    policy.max_decoded_bytes = 2 * 32 * sizeof(float);
    policy.allowed_sample_rates = {44100};
    policy.allowed_audio_read_extensions = {"wav"};

    auto descriptor = importer.describe_audio_file(path.string(), policy);
    REQUIRE(descriptor.ok());
    REQUIRE(descriptor.kind == SampleAssetKind::audio);
    REQUIRE(descriptor.extension == ".wav");
    REQUIRE(descriptor.info.sample_rate == 44100);
    REQUIRE(descriptor.info.num_channels == 2);
    REQUIRE(descriptor.info.num_frames == 12);
    REQUIRE(descriptor.decoded_bytes == 2 * 12 * sizeof(float));

    auto imported = importer.import_audio_file(path.string(), policy);
    REQUIRE(imported.ok());
    REQUIRE(imported.audio.sample_rate == 44100);
    REQUIRE(imported.audio.num_channels() == 2);
    REQUIRE(imported.audio.num_frames() == 12);

    policy.max_frames = 8;
    REQUIRE(importer.describe_audio_file(path.string(), policy).status ==
            SampleAssetStatus::frame_budget_exceeded);

    policy.max_frames = 32;
    policy.max_decoded_bytes = 4;
    REQUIRE(importer.describe_audio_file(path.string(), policy).status ==
            SampleAssetStatus::byte_budget_exceeded);

    policy.max_decoded_bytes = 2 * 32 * sizeof(float);
    policy.allowed_sample_rates = {48000};
    REQUIRE(importer.describe_audio_file(path.string(), policy).status ==
            SampleAssetStatus::sample_rate_not_allowed);

    std::filesystem::remove(path);
}

TEST_CASE("SampleAssetImporter rejects unsupported and unreadable paths",
          "[audio][sampler][asset_io]") {
    SampleAssetImporter importer;

    REQUIRE(importer.describe_audio_file("", {}).status == SampleAssetStatus::empty_path);
    REQUIRE(importer.describe_audio_file("/tmp/not-a-sample.txt", {}).status ==
            SampleAssetStatus::unsupported_extension);

    const auto missing_wav = unique_path(".wav");
    std::filesystem::remove(missing_wav);
    REQUIRE(importer.describe_audio_file(missing_wav.string(), {}).status ==
            SampleAssetStatus::metadata_read_failed);
}

TEST_CASE("SampleAssetExporter validates policy and round-trips WAV output",
          "[audio][sampler][asset_io]") {
    SampleAssetExporter exporter;
    REQUIRE(exporter.supports_audio_extension("wav"));

    auto audio = make_audio(32000, 10, 1);
    const auto path = unique_path(".wav");
    std::filesystem::remove(path);

    SampleAssetPolicy policy;
    policy.max_channels = 1;
    policy.max_frames = 16;
    policy.allowed_audio_write_extensions = {".wav"};

    auto exported = exporter.export_audio_file(path.string(), audio, policy);
    REQUIRE(exported.ok());
    REQUIRE(exported.frames_written == 10);
    REQUIRE(exported.decoded_bytes == 10 * sizeof(float));
    REQUIRE(std::filesystem::exists(path));

    auto read_back = pulp::audio::read_audio_file(path.string());
    REQUIRE(read_back.has_value());
    REQUIRE(read_back->sample_rate == 32000);
    REQUIRE(read_back->num_channels() == 1);
    REQUIRE(read_back->num_frames() == 10);

    auto unsupported = exporter.export_audio_file(unique_path(".asset").string(), audio, policy);
    REQUIRE(unsupported.status == SampleAssetStatus::unsupported_extension);

    policy.max_frames = 4;
    auto over_budget = exporter.export_audio_file(unique_path(".wav").string(), audio, policy);
    REQUIRE(over_budget.status == SampleAssetStatus::frame_budget_exceeded);

    std::filesystem::remove(path);
}

TEST_CASE("classify_sample_asset_drop separates audio midi and unsupported paths without metadata I/O",
          "[audio][sampler][asset_io]") {
    const auto wav = unique_path(".wav");
    const auto midi = unique_path(".mid");
    const auto text = unique_path(".txt");
    std::filesystem::remove(wav);
    std::filesystem::remove(midi);
    std::filesystem::remove(text);

    REQUIRE(pulp::audio::write_wav_file(wav.string(), make_audio(48000, 4, 1)));
    {
        std::ofstream out(midi, std::ios::binary);
        out << "MThd";
    }
    {
        std::ofstream out(text, std::ios::binary);
        out << "notes";
    }

    SampleAssetPolicy policy;
    auto blocked = pulp::audio::classify_sample_asset_drop(
        {wav.string(), midi.string(), text.string()}, policy);
    REQUIRE(blocked.size() == 3);
    REQUIRE(blocked[0].supported());
    REQUIRE(blocked[0].kind == SampleAssetKind::audio);
    REQUIRE(blocked[0].audio_info.num_frames == 0);
    REQUIRE(blocked[1].kind == SampleAssetKind::midi);
    REQUIRE(blocked[1].status == SampleAssetStatus::unsupported_asset_kind);
    REQUIRE(blocked[2].kind == SampleAssetKind::unknown);
    REQUIRE(blocked[2].status == SampleAssetStatus::unsupported_extension);

    auto probed = pulp::audio::probe_sample_asset_drop({wav.string()}, policy);
    REQUIRE(probed.size() == 1);
    REQUIRE(probed[0].supported());
    REQUIRE(probed[0].audio_info.num_frames == 4);
    REQUIRE(probed[0].decoded_bytes == 4 * sizeof(float));

    policy.allow_midi_drop = true;
    auto allowed = pulp::audio::classify_sample_asset_drop({midi.string()}, policy);
    REQUIRE(allowed.size() == 1);
    REQUIRE(allowed[0].supported());
    REQUIRE(allowed[0].kind == SampleAssetKind::midi);

    std::filesystem::remove(wav);
    std::filesystem::remove(midi);
    std::filesystem::remove(text);
}

TEST_CASE("sample asset helpers normalize extensions and report byte overflow",
          "[audio][sampler][asset_io]") {
    REQUIRE(pulp::audio::sample_asset_normalize_extension("WAV") == ".wav");
    REQUIRE(pulp::audio::sample_asset_normalize_extension(".AIFF") == ".aiff");
    REQUIRE(pulp::audio::classify_sample_asset_extension("mid") == SampleAssetKind::midi);
    REQUIRE(pulp::audio::sample_asset_status_name(SampleAssetStatus::ok) == std::string("ok"));

    REQUIRE(pulp::audio::sample_asset_decoded_bytes(2, 10) == 2 * 10 * sizeof(float));
    REQUIRE(pulp::audio::sample_asset_decoded_bytes(
                2,
                std::numeric_limits<std::uint64_t>::max()) == 0);
}
