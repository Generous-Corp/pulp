#pragma once

#include <pulp/runtime/result.hpp>

#include <cstddef>
#include <cstdint>
#include <string>

namespace pulp::interchange {
struct ExportArtifacts;
}
namespace pulp::tools::timeline::detail {
struct LoadedProject;

inline constexpr std::uint64_t kMediaArtifactMetadataReserveBytes = 128u;

enum class DawProjectMediaErrorCode : std::uint8_t {
    MissingRootSequence,
    MissingAsset,
    InvalidAssetName,
    DuplicateArchivePath,
    ByteLimitExceeded,
    AssetReadFailed,
};
struct DawProjectMediaError {
    DawProjectMediaErrorCode code = DawProjectMediaErrorCode::MissingAsset;
    std::uint64_t asset_id = 0;
    std::string asset_name;
    std::string reason;
};

runtime::Result<std::size_t, DawProjectMediaError>
add_dawproject_media(const LoadedProject& loaded, pulp::interchange::ExportArtifacts& artifacts,
                     std::uint64_t max_media_logical_bytes);

} // namespace pulp::tools::timeline::detail
