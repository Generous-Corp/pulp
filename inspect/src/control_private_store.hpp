#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <vector>

namespace pulp::inspect::detail {

enum class OwnerPrivateFilePublishResult {
    Published,
    Exists,
    Failed,
};

bool ensure_owner_private_directory(const std::filesystem::path& directory);

std::optional<std::vector<std::uint8_t>> read_owner_private_file(const std::filesystem::path& path,
                                                                 std::size_t maximum_bytes);

bool write_owner_private_file_atomic(const std::filesystem::path& destination,
                                     std::span<const std::uint8_t> contents);

/// Publishes a new private file without replacing an existing destination.
/// This is the content-addressed-store primitive: concurrent equal publishers
/// observe one Published result and the rest observe Exists.
OwnerPrivateFilePublishResult publish_owner_private_file(const std::filesystem::path& destination,
                                                         std::span<const std::uint8_t> contents);

bool remove_owner_private_file_durable(const std::filesystem::path& path);

} // namespace pulp::inspect::detail
