#include <pulp/audio/sample_asset_io.hpp>

#include <pulp/audio/format_registry.hpp>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <limits>
#include <optional>

namespace pulp::audio {
namespace {

bool contains_extension(const std::vector<std::string>& extensions,
                        const std::string& extension) {
    if (extensions.empty()) return true;
    return std::any_of(extensions.begin(), extensions.end(), [&](const std::string& candidate) {
        return sample_asset_normalize_extension(candidate) == extension;
    });
}

bool contains_sample_rate(const std::vector<std::uint32_t>& rates,
                          std::uint32_t sample_rate) {
    if (rates.empty()) return true;
    return std::find(rates.begin(), rates.end(), sample_rate) != rates.end();
}

bool consistent_channel_lengths(const AudioFileData& audio) {
    if (audio.sample_rate == 0 || audio.empty()) return false;
    const auto frames = audio.num_frames();
    return std::all_of(audio.channels.begin(), audio.channels.end(), [&](const auto& channel) {
        return channel.size() == frames;
    });
}

SampleAssetStatus validate_audio_budget(std::uint32_t channels,
                                         std::uint64_t frames,
                                         std::uint32_t sample_rate,
                                         const SampleAssetPolicy& policy,
                                         std::uint64_t* decoded_bytes) noexcept {
    if (channels == 0 || frames == 0 || sample_rate == 0) {
        return SampleAssetStatus::invalid_audio;
    }
    if (policy.max_channels > 0 && channels > policy.max_channels) {
        return SampleAssetStatus::channel_budget_exceeded;
    }
    if (policy.max_frames > 0 && frames > policy.max_frames) {
        return SampleAssetStatus::frame_budget_exceeded;
    }
    if (!contains_sample_rate(policy.allowed_sample_rates, sample_rate)) {
        return SampleAssetStatus::sample_rate_not_allowed;
    }

    const auto bytes = sample_asset_decoded_bytes(channels, frames);
    if (decoded_bytes) *decoded_bytes = bytes;
    if (bytes == 0) {
        return SampleAssetStatus::invalid_audio;
    }
    if (policy.max_decoded_bytes > 0 && bytes > policy.max_decoded_bytes) {
        return SampleAssetStatus::byte_budget_exceeded;
    }
    return SampleAssetStatus::ok;
}

SampleAssetDropItem drop_from_descriptor(const SampleAssetDescriptor& descriptor) {
    return SampleAssetDropItem{
        .path = descriptor.path,
        .extension = descriptor.extension,
        .kind = descriptor.kind,
        .status = descriptor.status,
        .audio_info = descriptor.info,
        .decoded_bytes = descriptor.decoded_bytes,
    };
}

}  // namespace

const char* sample_asset_status_name(SampleAssetStatus status) noexcept {
    switch (status) {
        case SampleAssetStatus::ok: return "ok";
        case SampleAssetStatus::empty_path: return "empty_path";
        case SampleAssetStatus::unsupported_extension: return "unsupported_extension";
        case SampleAssetStatus::unsupported_asset_kind: return "unsupported_asset_kind";
        case SampleAssetStatus::metadata_read_failed: return "metadata_read_failed";
        case SampleAssetStatus::read_failed: return "read_failed";
        case SampleAssetStatus::write_failed: return "write_failed";
        case SampleAssetStatus::invalid_audio: return "invalid_audio";
        case SampleAssetStatus::channel_budget_exceeded: return "channel_budget_exceeded";
        case SampleAssetStatus::frame_budget_exceeded: return "frame_budget_exceeded";
        case SampleAssetStatus::byte_budget_exceeded: return "byte_budget_exceeded";
        case SampleAssetStatus::sample_rate_not_allowed: return "sample_rate_not_allowed";
    }
    return "unknown";
}

std::string sample_asset_normalize_extension(std::string_view extension) {
    std::string out(extension);
    if (!out.empty() && out.front() != '.') {
        out.insert(out.begin(), '.');
    }
    std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return out;
}

SampleAssetKind classify_sample_asset_extension(std::string_view extension) {
    const auto ext = sample_asset_normalize_extension(extension);
    if (ext.empty()) return SampleAssetKind::unknown;
    if (ext == ".mid" || ext == ".midi" || ext == ".smf") {
        return SampleAssetKind::midi;
    }
    if (FormatRegistry::instance().find_reader(ext) != nullptr) {
        return SampleAssetKind::audio;
    }
    return SampleAssetKind::unknown;
}

std::uint64_t sample_asset_decoded_bytes(std::uint32_t channels,
                                         std::uint64_t frames) noexcept {
    if (channels == 0 || frames == 0) return 0;
    const auto max = std::numeric_limits<std::uint64_t>::max();
    if (frames > max / channels) return 0;
    const auto samples = frames * channels;
    if (samples > max / sizeof(float)) return 0;
    return samples * sizeof(float);
}

std::vector<std::string> SampleAssetImporter::supported_audio_extensions() const {
    return FormatRegistry::instance().supported_read_extensions();
}

bool SampleAssetImporter::supports_audio_extension(std::string_view extension) const {
    const auto ext = sample_asset_normalize_extension(extension);
    return !ext.empty() && FormatRegistry::instance().find_reader(ext) != nullptr;
}

SampleAssetDescriptor SampleAssetImporter::describe_audio_file(
    const std::string& path,
    const SampleAssetPolicy& policy) const {
    SampleAssetDescriptor descriptor;
    descriptor.path = path;

    if (path.empty()) {
        descriptor.status = SampleAssetStatus::empty_path;
        return descriptor;
    }

    try {
        descriptor.extension = sample_asset_normalize_extension(
            std::filesystem::path(path).extension().string());
    } catch (...) {
        descriptor.status = SampleAssetStatus::unsupported_extension;
        return descriptor;
    }

    descriptor.kind = classify_sample_asset_extension(descriptor.extension);
    if (descriptor.kind != SampleAssetKind::audio) {
        descriptor.status = descriptor.kind == SampleAssetKind::unknown
            ? SampleAssetStatus::unsupported_extension
            : SampleAssetStatus::unsupported_asset_kind;
        return descriptor;
    }
    if (!supports_audio_extension(descriptor.extension) ||
        !contains_extension(policy.allowed_audio_read_extensions, descriptor.extension)) {
        descriptor.status = SampleAssetStatus::unsupported_extension;
        return descriptor;
    }

    auto info = FormatRegistry::instance().read_info(path);
    if (!info) {
        descriptor.status = SampleAssetStatus::metadata_read_failed;
        return descriptor;
    }

    descriptor.info = *info;
    descriptor.status = validate_audio_budget(info->num_channels,
                                              info->num_frames,
                                              info->sample_rate,
                                              policy,
                                              &descriptor.decoded_bytes);
    return descriptor;
}

SampleAssetImportResult SampleAssetImporter::import_audio_file(
    const std::string& path,
    const SampleAssetPolicy& policy) const {
    SampleAssetImportResult result;
    result.descriptor = describe_audio_file(path, policy);
    if (!result.descriptor.ok()) {
        return result;
    }

    auto audio = FormatRegistry::instance().read(path);
    if (!audio) {
        result.descriptor.status = SampleAssetStatus::read_failed;
        return result;
    }
    if (!consistent_channel_lengths(*audio)) {
        result.descriptor.status = SampleAssetStatus::invalid_audio;
        return result;
    }

    result.descriptor.status = validate_audio_budget(audio->num_channels(),
                                                     audio->num_frames(),
                                                     audio->sample_rate,
                                                     policy,
                                                     &result.descriptor.decoded_bytes);
    if (!result.descriptor.ok()) {
        return result;
    }

    result.audio = std::move(*audio);
    return result;
}

std::vector<std::string> SampleAssetExporter::supported_audio_extensions() const {
    return FormatRegistry::instance().supported_write_extensions();
}

bool SampleAssetExporter::supports_audio_extension(std::string_view extension) const {
    const auto ext = sample_asset_normalize_extension(extension);
    return !ext.empty() && FormatRegistry::instance().find_writer(ext) != nullptr;
}

SampleAssetExportResult SampleAssetExporter::export_audio_file(
    const std::string& path,
    const AudioFileData& audio,
    const SampleAssetPolicy& policy) const {
    SampleAssetExportResult result;
    result.path = path;

    if (path.empty()) {
        result.status = SampleAssetStatus::empty_path;
        return result;
    }

    try {
        result.extension = sample_asset_normalize_extension(
            std::filesystem::path(path).extension().string());
    } catch (...) {
        result.status = SampleAssetStatus::unsupported_extension;
        return result;
    }

    if (!supports_audio_extension(result.extension) ||
        !contains_extension(policy.allowed_audio_write_extensions, result.extension)) {
        result.status = SampleAssetStatus::unsupported_extension;
        return result;
    }
    if (!consistent_channel_lengths(audio)) {
        result.status = SampleAssetStatus::invalid_audio;
        return result;
    }

    result.status = validate_audio_budget(audio.num_channels(),
                                          audio.num_frames(),
                                          audio.sample_rate,
                                          policy,
                                          &result.decoded_bytes);
    if (!result.ok()) {
        return result;
    }

    if (!FormatRegistry::instance().write(path, audio)) {
        result.status = SampleAssetStatus::write_failed;
        return result;
    }

    result.frames_written = audio.num_frames();
    return result;
}

std::vector<SampleAssetDropItem> classify_sample_asset_drop(
    const std::vector<std::string>& paths,
    const SampleAssetPolicy& policy) {
    std::vector<SampleAssetDropItem> items;
    items.reserve(paths.size());

    SampleAssetImporter importer;
    for (const auto& path : paths) {
        if (path.empty()) {
            items.push_back(SampleAssetDropItem{.path = path, .status = SampleAssetStatus::empty_path});
            continue;
        }

        std::string extension;
        try {
            extension = sample_asset_normalize_extension(std::filesystem::path(path).extension().string());
        } catch (...) {
            items.push_back(SampleAssetDropItem{.path = path,
                                                .status = SampleAssetStatus::unsupported_extension});
            continue;
        }

        const auto kind = classify_sample_asset_extension(extension);
        if (kind == SampleAssetKind::audio) {
            const bool supported = importer.supports_audio_extension(extension) &&
                contains_extension(policy.allowed_audio_read_extensions, extension);
            items.push_back(SampleAssetDropItem{
                .path = path,
                .extension = extension,
                .kind = kind,
                .status = supported ? SampleAssetStatus::ok
                                     : SampleAssetStatus::unsupported_extension,
            });
        } else if (kind == SampleAssetKind::midi) {
            items.push_back(SampleAssetDropItem{
                .path = path,
                .extension = extension,
                .kind = kind,
                .status = policy.allow_midi_drop ? SampleAssetStatus::ok
                                                 : SampleAssetStatus::unsupported_asset_kind,
            });
        } else {
            items.push_back(SampleAssetDropItem{
                .path = path,
                .extension = extension,
                .kind = kind,
                .status = SampleAssetStatus::unsupported_extension,
            });
        }
    }

    return items;
}

std::vector<SampleAssetDropItem> probe_sample_asset_drop(
    const std::vector<std::string>& paths,
    const SampleAssetPolicy& policy) {
    std::vector<SampleAssetDropItem> items;
    items.reserve(paths.size());

    SampleAssetImporter importer;
    for (const auto& item : classify_sample_asset_drop(paths, policy)) {
        if (item.kind == SampleAssetKind::audio && item.status == SampleAssetStatus::ok) {
            items.push_back(drop_from_descriptor(importer.describe_audio_file(item.path, policy)));
        } else {
            items.push_back(item);
        }
    }

    return items;
}

}  // namespace pulp::audio
