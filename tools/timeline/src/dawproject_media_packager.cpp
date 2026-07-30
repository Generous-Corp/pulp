#include "dawproject_media_packager.hpp"

#include "bounded_zip_archive.hpp"
#include "timeline_agent_internal.hpp"

#include <pulp/interchange/export_plan.hpp>
#include <pulp/timeline/asset_path.hpp>

#include <limits>
#include <memory_resource>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

namespace pulp::tools::timeline::detail {

runtime::Result<std::size_t, DawProjectMediaError>
add_dawproject_media(const LoadedProject& loaded, pulp::interchange::ExportArtifacts& artifacts,
                     std::uint64_t limit) {
    const auto fail = [](DawProjectMediaErrorCode code, std::uint64_t id, std::string name,
                         std::string reason) {
        return runtime::Result<std::size_t, DawProjectMediaError>(
            runtime::Err(DawProjectMediaError{code, id, std::move(name), std::move(reason)}));
    };
    const auto* root = loaded.value.find_sequence(loaded.value.root_sequence_id());
    if (root == nullptr)
        return fail(DawProjectMediaErrorCode::MissingRootSequence, 0, {},
                    "project root sequence is missing");

    try {
        AllocationBudget budget(limit);
        std::pmr::unordered_set<std::pmr::string> names(budget.resource());
        names.reserve(artifacts.artifacts.size());
        for (const auto& artifact : artifacts.artifacts)
            names.emplace(artifact.name.data(), artifact.name.size());
        std::pmr::unordered_set<std::uint64_t> seen(budget.resource());
        std::pmr::vector<pulp::interchange::ExportArtifact> media(budget.resource());
        std::uint64_t logical = 0;
        std::uint64_t external = 0;
        for (const auto& track : root->tracks()) {
            for (const auto& clip : track.clips()) {
                if (clip.time_anchor() != pulp::timeline::ClipTimeAnchor::Musical)
                    continue;
                const auto* reference = std::get_if<pulp::timeline::MediaRef>(&clip.content());
                if (reference == nullptr || !seen.insert(reference->asset_id.value).second)
                    continue;
                const auto* asset = loaded.value.find_asset(reference->asset_id);
                if (asset == nullptr)
                    return fail(DawProjectMediaErrorCode::MissingAsset, reference->asset_id.value,
                                {}, "referenced asset is missing");
                if (asset->name.empty() ||
                    !pulp::timeline::package_relative_path_is_lexically_safe(asset->name))
                    return fail(DawProjectMediaErrorCode::InvalidAssetName,
                                reference->asset_id.value, asset->name,
                                "asset name is not a safe package-relative path");
                const auto name_size = static_cast<std::uint64_t>(asset->name.size()) + 6u;
                const auto name_reserve = name_size +
                                          kMediaArtifactMetadataReserveBytes;
                if (name_reserve > limit - std::min(limit, logical))
                    return fail(DawProjectMediaErrorCode::ByteLimitExceeded,
                                reference->asset_id.value, asset->name,
                                "media metadata exceeds the working-set limit");
                logical += name_reserve;
                if (!budget.acquire_external(name_reserve))
                    return fail(DawProjectMediaErrorCode::ByteLimitExceeded,
                                reference->asset_id.value, asset->name,
                                "media metadata exceeds the working-set limit");
                external += name_reserve;
                std::string archive_name = "audio/" + asset->name;
                if (!names.emplace(archive_name.data(), archive_name.size()).second)
                    return fail(DawProjectMediaErrorCode::DuplicateArchivePath,
                                reference->asset_id.value, asset->name,
                                "archive path collides with another export artifact");
                const auto read_allowance = std::min(limit - logical, budget.remaining_bytes());
                if (read_allowance == 0)
                    return fail(DawProjectMediaErrorCode::ByteLimitExceeded,
                                reference->asset_id.value, asset->name,
                                "media payload exceeds the remaining working-set limit");
                if (!budget.acquire_external(read_allowance))
                    return fail(DawProjectMediaErrorCode::ByteLimitExceeded,
                                reference->asset_id.value, asset->name,
                                "media payload exceeds the remaining working-set limit");
                external += read_allowance;
                auto bytes = read_verified_asset_bytes(loaded, *asset, read_allowance);
                if (!bytes)
                    return fail(
                        DawProjectMediaErrorCode::AssetReadFailed, reference->asset_id.value,
                        asset->name,
                        "asset bytes are unavailable, invalid, or exceed the remaining limit");
                if (bytes->size() > limit - logical)
                    return fail(DawProjectMediaErrorCode::ByteLimitExceeded,
                                reference->asset_id.value, asset->name,
                                "cumulative media payload exceeds the working-set limit");
                logical += bytes->size();
                if (bytes->capacity() > read_allowance)
                    return fail(DawProjectMediaErrorCode::ByteLimitExceeded,
                                reference->asset_id.value, asset->name,
                                "cumulative media payload exceeds the working-set limit");
                budget.release_external(read_allowance - bytes->capacity());
                external -= read_allowance - bytes->capacity();
                media.push_back({std::move(archive_name), std::move(*bytes)});
            }
        }
        if (media.size() > std::numeric_limits<std::size_t>::max() - artifacts.artifacts.size())
            return fail(DawProjectMediaErrorCode::ByteLimitExceeded, 0, {},
                        "combined artifact count exceeds the working-set limit");
        const auto count = media.size();
        const auto final_count = artifacts.artifacts.size() + media.size();
        if (final_count > artifacts.artifacts.capacity()) {
            if (final_count > (UINT64_MAX - kExternalArtifactMetadataReserveBytes) /
                                  (2u * sizeof(pulp::interchange::ExportArtifact)))
                return fail(DawProjectMediaErrorCode::ByteLimitExceeded, 0, {},
                            "combined artifact storage exceeds the working-set limit");
            const auto insertion_reserve = 2u * static_cast<std::uint64_t>(final_count) *
                                               sizeof(pulp::interchange::ExportArtifact) +
                                           kExternalArtifactMetadataReserveBytes;
            if (!budget.acquire_external(insertion_reserve))
                return fail(DawProjectMediaErrorCode::ByteLimitExceeded, 0, {},
                            "combined artifact storage exceeds the working-set limit");
            external += insertion_reserve;
            artifacts.artifacts.reserve(final_count);
        }
        artifacts.artifacts.insert(artifacts.artifacts.end(),
                                   std::make_move_iterator(media.begin()),
                                   std::make_move_iterator(media.end()));
        budget.release_external(external);
        return runtime::Ok(count);
    } catch (const std::bad_alloc&) {
        return fail(DawProjectMediaErrorCode::ByteLimitExceeded, 0, {},
                    "media packaging exceeds the working-set limit");
    }
}

} // namespace pulp::tools::timeline::detail
