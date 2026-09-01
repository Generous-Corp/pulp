#pragma once

/// @file archive.hpp
/// Bounded, deterministic capsule archive access.
///
/// Reading is memory-bounded: the reader charges every allocation against a
/// budget and refuses rather than growing. Writing is deterministic: the same
/// inventory produces byte-identical output, with no timestamps, no host
/// paths, and a fixed member order.

#include <pulp/authoring_capsule/limits.hpp>
#include <pulp/authoring_capsule/status.hpp>
#include <pulp/runtime/result.hpp>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace pulp::authoring_capsule {

struct MemberInfo {
    std::string path;
    std::uint64_t compressed_bytes = 0;
    std::uint64_t expanded_bytes = 0;
    /// Store (0) or deflate (8). Any other method is `unsafe_archive`.
    std::uint16_t method = 0;
};

/// A reader over an admitted archive. Construction validates the container
/// shape — budgets, methods, paths, collisions, manifest position — but does
/// not expand any member and never executes anything.
class CapsuleArchive {
public:
    CapsuleArchive(CapsuleArchive&&) noexcept;
    CapsuleArchive& operator=(CapsuleArchive&&) noexcept;
    CapsuleArchive(const CapsuleArchive&) = delete;
    CapsuleArchive& operator=(const CapsuleArchive&) = delete;
    ~CapsuleArchive();

    /// Members in archive order. The manifest is always index 0.
    std::span<const MemberInfo> members() const noexcept;

    /// The bounded root manifest bytes. Available without expanding anything
    /// else, which is what makes preview cheap and safe.
    std::span<const std::uint8_t> manifest_bytes() const noexcept;

    /// Expand one member. Charges the working-set budget; returns
    /// `archive_budget_exceeded` rather than allocating past it.
    runtime::Result<std::vector<std::uint8_t>, CapsuleError> read(std::string_view path) const;

    /// Peak bytes charged so far. Reported in receipts so a budget regression
    /// is visible rather than inferred.
    std::uint64_t peak_bytes() const noexcept;

private:
    struct Impl;
    explicit CapsuleArchive(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;

    friend runtime::Result<CapsuleArchive, CapsuleError>
    open_archive(const std::filesystem::path&, const CapsuleLimits&);
};

runtime::Result<CapsuleArchive, CapsuleError>
open_archive(const std::filesystem::path& path, const CapsuleLimits& limits = kCapsuleLimitsV1);

/// One member to write. `path` must already be normalized.
struct WriteMember {
    std::string path;
    std::vector<std::uint8_t> bytes;
};

/// Write deterministically to a path that must not already exist. Members are
/// emitted in the given order with fixed metadata, so repeating an export of
/// unchanged content reproduces the file byte for byte. The destination
/// appears only once it is complete: a cancelled or failed write leaves no
/// partial file for a person to mistake for a good one.
runtime::Result<std::uint64_t, CapsuleError>
write_archive_no_replace(const std::vector<WriteMember>& members,
                         const std::filesystem::path& destination,
                         const CapsuleLimits& limits = kCapsuleLimitsV1);

}  // namespace pulp::authoring_capsule
