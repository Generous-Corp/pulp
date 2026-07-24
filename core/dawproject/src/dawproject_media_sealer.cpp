#include "dawproject_media_sealer.hpp"

#include "dawproject_import_support.hpp"

#include <pulp/audio/wav_decoder.hpp>
#include <pulp/runtime/crypto.hpp>
#include <pulp/timeline/asset_path.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <string_view>
#include <utility>

namespace pulp::timeline::detail {
namespace {

bool declared_duration_matches_frames(long double declared_frames,
                                      std::uint64_t actual_frames) noexcept {
    if (!std::isfinite(declared_frames) || actual_frames == 0)
        return false;
    const auto actual = static_cast<long double>(actual_frames);
    return declared_frames >= actual - 0.5L && declared_frames < actual + 0.5L;
}

} // namespace

DawProjectMediaSealer::DawProjectMediaSealer(DawProjectMediaResolver resolver,
                                             const DawProjectImportLimits& limits,
                                             std::uint64_t& next_item_id)
    : resolver_(std::move(resolver)), limits_(limits), next_item_id_(next_item_id) {}

std::optional<DawProjectImportError> DawProjectMediaSealer::seal(const pugi::xml_node& audio_node,
                                                                 ItemId& asset_id_out,
                                                                 std::uint64_t& frame_count_out) {
    const auto file = audio_node.child("File");
    if (!file)
        return import_error(DawProjectImportErrorCode::UnsupportedFeature,
                            "<Audio> without a <File> reference is not supported");
    const auto path_attribute = file.attribute("path");
    if (path_attribute.empty())
        return import_error(DawProjectImportErrorCode::MissingAttribute,
                            "<File> is missing required attribute 'path'");
    const std::string_view path_view = path_attribute.as_string();
    if (path_view.empty())
        return import_error(DawProjectImportErrorCode::InvalidValue,
                            "<File path> must not be empty");
    if (path_view.size() > limits_.max_package_path_bytes)
        return import_error(DawProjectImportErrorCode::LimitExceeded,
                            "<File path> exceeds max_package_path_bytes");
    if (!package_relative_path_is_lexically_safe(path_view))
        return import_error(DawProjectImportErrorCode::InvalidValue,
                            "<File path> must be a safe package-relative path");
    const std::string path(path_view);

    double duration_seconds = 0.0;
    double sample_rate = 0.0;
    if (auto error = require_double(audio_node, "duration", "<Audio>", duration_seconds))
        return error;
    if (auto error = require_double(audio_node, "sampleRate", "<Audio>", sample_rate))
        return error;
    if (!std::isfinite(duration_seconds) || !(duration_seconds > 0.0))
        return import_error(DawProjectImportErrorCode::InvalidValue,
                            "<Audio duration> must be positive");
    if (!std::isfinite(sample_rate) || !(sample_rate > 0.0))
        return import_error(DawProjectImportErrorCode::InvalidValue,
                            "<Audio sampleRate> must be positive");

    std::optional<long long> declared_channels;
    const auto channels_attribute = audio_node.attribute("channels");
    if (!channels_attribute.empty()) {
        long long channels = 0;
        if (!parse_number(channels_attribute, channels) || channels <= 0 ||
            channels > std::numeric_limits<std::uint16_t>::max())
            return import_error(
                DawProjectImportErrorCode::InvalidValue,
                "<Audio channels> must be an integer in the supported channel domain");
        declared_channels = channels;
    }
    const auto declared_frames =
        static_cast<long double>(duration_seconds) * static_cast<long double>(sample_rate);
    if (!std::isfinite(declared_frames) || declared_frames <= 0.0L)
        return import_error(DawProjectImportErrorCode::InvalidValue,
                            "<Audio> duration exceeds the supported frame domain");

    if (!resolver_)
        return import_error(DawProjectImportErrorCode::MissingMediaBytes,
                            "media bytes are required to seal '" + path + "'");
    if (resolver_calls_ >= limits_.max_media_resolver_calls)
        return import_error(DawProjectImportErrorCode::LimitExceeded,
                            "media resolver call count exceeds max_media_resolver_calls");
    ++resolver_calls_;
    const auto bytes = resolver_(path);
    if (!bytes)
        return import_error(DawProjectImportErrorCode::MissingMediaBytes,
                            "media resolver did not provide '" + path + "'");
    const auto byte_count = static_cast<std::uint64_t>(bytes->size());
    if (byte_count > limits_.max_media_bytes_per_resolver_call)
        return import_error(DawProjectImportErrorCode::LimitExceeded,
                            "resolved media '" + path +
                                "' exceeds max_media_bytes_per_resolver_call");
    if (resolved_bytes_ > limits_.max_total_media_bytes ||
        byte_count > limits_.max_total_media_bytes - resolved_bytes_)
        return import_error(DawProjectImportErrorCode::LimitExceeded,
                            "resolved media exceeds max_total_media_bytes");
    resolved_bytes_ += byte_count;

    const auto info = audio::inspect_wav(*bytes);
    if (!info)
        return import_error(DawProjectImportErrorCode::InvalidValue,
                            "media resolver provided an invalid or unsupported WAV for '" + path +
                                "'");
    if (sample_rate != static_cast<double>(info->sample_rate))
        return import_error(DawProjectImportErrorCode::InvalidValue,
                            "<Audio sampleRate> does not match resolved media '" + path + "'");
    if (declared_channels && static_cast<std::uint64_t>(*declared_channels) != info->num_channels)
        return import_error(DawProjectImportErrorCode::InvalidValue,
                            "<Audio channels> does not match resolved media '" + path + "'");
    if (!declared_duration_matches_frames(declared_frames, info->num_frames))
        return import_error(DawProjectImportErrorCode::InvalidValue,
                            "<Audio duration> does not match resolved media '" + path + "'");

    const auto hash = ContentHash::from_hex(runtime::sha256_hex(bytes->data(), bytes->size()));
    if (!hash)
        return import_error(DawProjectImportErrorCode::InvalidValue,
                            "failed to derive a content hash for '" + path + "'");
    const auto hash_hex = hash->to_hex();
    if (const auto path_hash = hash_by_path_.find(path);
        path_hash != hash_by_path_.end() && path_hash->second != hash_hex)
        return import_error(DawProjectImportErrorCode::InvalidValue,
                            "package path resolved to different content for '" + path + "'");

    if (const auto existing = asset_by_hash_.find(hash_hex); existing != asset_by_hash_.end()) {
        const auto asset = std::find_if(assets_.begin(), assets_.end(), [&](const auto& candidate) {
            return candidate.id == existing->second;
        });
        if (asset == assets_.end())
            return import_error(DawProjectImportErrorCode::ModelRejected,
                                "internal content-hash index lost resolved media '" + path + "'");
        if (asset->frame_count != info->num_frames ||
            asset->sample_rate != timebase::RationalRate{info->sample_rate, 1})
            return import_error(DawProjectImportErrorCode::InvalidValue,
                                "content-identical media has inconsistent audio metadata for '" +
                                    path + "'");
        const AssetLocator locator{AssetLocatorKind::PackageRelative, path};
        if (std::find(asset->locators.begin(), asset->locators.end(), locator) ==
            asset->locators.end())
            asset->locators.push_back(locator);
        hash_by_path_.emplace(path, hash_hex);
        asset_id_out = asset->id;
        frame_count_out = asset->frame_count;
        return std::nullopt;
    }

    if (assets_.size() >= limits_.max_media_assets)
        return import_error(DawProjectImportErrorCode::LimitExceeded,
                            "DAWproject media asset count exceeds max_media_assets");

    const ItemId asset_id{next_item_id_++};
    MediaAsset asset;
    asset.id = asset_id;
    asset.name = path;
    asset.frame_count = info->num_frames;
    asset.sample_rate = timebase::RationalRate{info->sample_rate, 1};
    asset.content_hash = *hash;
    asset.storage_policy = AssetStoragePolicy::External;
    asset.locators.push_back(AssetLocator{AssetLocatorKind::PackageRelative, path});
    frame_count_out = info->num_frames;
    assets_.push_back(std::move(asset));
    hash_by_path_.emplace(path, hash_hex);
    asset_by_hash_.emplace(hash_hex, asset_id);
    asset_id_out = asset_id;
    return std::nullopt;
}

std::vector<MediaAsset> DawProjectMediaSealer::take_assets() {
    return std::move(assets_);
}

} // namespace pulp::timeline::detail
