#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include <pulp/audio/audio_file.hpp>

namespace pulp::audio {

enum class SampleAssetKind : std::uint8_t {
    unknown,
    audio,
    midi,
};

enum class SampleAssetStatus : std::uint8_t {
    ok,
    empty_path,
    unsupported_extension,
    unsupported_asset_kind,
    metadata_read_failed,
    read_failed,
    write_failed,
    invalid_audio,
    channel_budget_exceeded,
    frame_budget_exceeded,
    byte_budget_exceeded,
    sample_rate_not_allowed,
};

[[nodiscard]] const char* sample_asset_status_name(SampleAssetStatus status) noexcept;

struct SampleAssetPolicy {
    std::uint32_t max_channels = 0;
    std::uint64_t max_frames = 0;
    std::uint64_t max_decoded_bytes = 0;
    std::vector<std::uint32_t> allowed_sample_rates;
    std::vector<std::string> allowed_audio_read_extensions;
    std::vector<std::string> allowed_audio_write_extensions;
    bool allow_midi_drop = false;
};

struct SampleAssetDescriptor {
    std::string path;
    std::string extension;
    SampleAssetKind kind = SampleAssetKind::unknown;
    SampleAssetStatus status = SampleAssetStatus::empty_path;
    AudioFileInfo info{};
    std::uint64_t decoded_bytes = 0;

    [[nodiscard]] bool ok() const noexcept { return status == SampleAssetStatus::ok; }
};

struct SampleAssetImportResult {
    SampleAssetDescriptor descriptor{};
    AudioFileData audio{};

    [[nodiscard]] bool ok() const noexcept { return descriptor.ok() && !audio.empty(); }
};

struct SampleAssetExportResult {
    std::string path;
    std::string extension;
    SampleAssetStatus status = SampleAssetStatus::empty_path;
    std::uint64_t frames_written = 0;
    std::uint64_t decoded_bytes = 0;

    [[nodiscard]] bool ok() const noexcept { return status == SampleAssetStatus::ok; }
};

struct SampleAssetDropItem {
    std::string path;
    std::string extension;
    SampleAssetKind kind = SampleAssetKind::unknown;
    SampleAssetStatus status = SampleAssetStatus::empty_path;
    AudioFileInfo audio_info{};
    std::uint64_t decoded_bytes = 0;

    [[nodiscard]] bool supported() const noexcept { return status == SampleAssetStatus::ok; }
};

[[nodiscard]] std::string sample_asset_normalize_extension(std::string_view extension);
[[nodiscard]] SampleAssetKind classify_sample_asset_extension(std::string_view extension);
[[nodiscard]] std::uint64_t sample_asset_decoded_bytes(std::uint32_t channels,
                                                       std::uint64_t frames) noexcept;

// Control/background-thread importer policy over FormatRegistry. This may do
// file I/O and allocate decoded audio; never call it from the audio callback.
class SampleAssetImporter {
public:
    [[nodiscard]] std::vector<std::string> supported_audio_extensions() const;
    [[nodiscard]] bool supports_audio_extension(std::string_view extension) const;

    [[nodiscard]] SampleAssetDescriptor describe_audio_file(
        const std::string& path,
        const SampleAssetPolicy& policy = {}) const;

    [[nodiscard]] SampleAssetImportResult import_audio_file(
        const std::string& path,
        const SampleAssetPolicy& policy = {}) const;
};

// Control/background-thread exporter policy over FormatRegistry. This may do
// file I/O and allocate/copy encoded data; never call it from the audio callback.
class SampleAssetExporter {
public:
    [[nodiscard]] std::vector<std::string> supported_audio_extensions() const;
    [[nodiscard]] bool supports_audio_extension(std::string_view extension) const;

    [[nodiscard]] SampleAssetExportResult export_audio_file(
        const std::string& path,
        const AudioFileData& audio,
        const SampleAssetPolicy& policy = {}) const;
};

// Extension-only drop classification for platform/UI drag handlers. This does
// not read file metadata and can be followed by probe_sample_asset_drop() on a
// background/control thread when detailed audio info is needed.
[[nodiscard]] std::vector<SampleAssetDropItem> classify_sample_asset_drop(
    const std::vector<std::string>& paths,
    const SampleAssetPolicy& policy = {});

// Metadata-probing drop classification. This may do file I/O and codec metadata
// reads for audio-looking paths; never call it from audio callbacks or hot UI
// pointer handlers.
[[nodiscard]] std::vector<SampleAssetDropItem> probe_sample_asset_drop(
    const std::vector<std::string>& paths,
    const SampleAssetPolicy& policy = {});

}  // namespace pulp::audio
