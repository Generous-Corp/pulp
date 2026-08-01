#pragma once

#include <pulp/runtime/result.hpp>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <string_view>

namespace pulp::project_package {

/// Stable failure categories for package layout and publication operations.
enum class PackageErrorCode : std::uint8_t {
    InvalidPath,
    InvalidLayout,
    AlreadyOpen,
    HashMismatch,
    IoError,
    PublicationConflict,
    DurabilityUncertain,
    InvalidGeneration,
    LimitExceeded,
};

/// Path-bearing package failure.
struct PackageError {
    /// Machine-readable failure category.
    PackageErrorCode code = PackageErrorCode::IoError;
    /// Path at which the operation failed, when one is available.
    std::filesystem::path path;
};

/// Visibility and durability state after a namespace-publication attempt.
enum class AtomicPublishOutcome : std::uint8_t {
    /// The destination name was not published.
    NotPublished,
    /// The destination and its parent namespace were durably fenced.
    PublishedDurably,
    /// The destination is visible, but its final namespace fence failed.
    PublishedDurabilityUncertain,
};

/// Builds a private sibling directory and publishes it at a previously absent
/// destination. Every staged file and directory entry is fenced before the
/// destination name becomes visible.
class AtomicPublisher {
  public:
    /// Creates a private staging sibling for a destination that must not exist.
    static runtime::Result<AtomicPublisher, PackageError>
    create(const std::filesystem::path& destination) noexcept;

    ~AtomicPublisher();
    AtomicPublisher(AtomicPublisher&&) noexcept;
    AtomicPublisher& operator=(AtomicPublisher&&) noexcept;
    AtomicPublisher(const AtomicPublisher&) = delete;
    AtomicPublisher& operator=(const AtomicPublisher&) = delete;

    /// Returns the private directory into which external writers may stage one file.
    const std::filesystem::path& staging_directory() const noexcept;
    /// Writes and durably fences one lexically safe relative file.
    runtime::Result<bool, PackageError> write(std::string_view relative_utf8,
                                              std::span<const std::uint8_t> bytes) noexcept;
    /// Writes and durably fences one UTF-8 text file.
    runtime::Result<bool, PackageError> write(std::string_view relative_utf8,
                                              std::string_view text) noexcept;
    /// Publishes one direct child of `staging_directory()` at the destination.
    /// The destination must not exist. A post-publication namespace-fence
    /// failure returns `PublishedDurabilityUncertain`, never `NotPublished`.
    runtime::Result<AtomicPublishOutcome, PackageError>
    commit_file(const std::filesystem::path& staged_file) noexcept;
    /// Publishes the complete staged tree without replacing an existing path.
    /// All file bytes and nested directory entries are durable before the
    /// destination name becomes visible.
    runtime::Result<AtomicPublishOutcome, PackageError> commit_directory() noexcept;
    /// Removes unpublished staging on a best-effort basis.
    void cancel() noexcept;

  private:
    struct Impl;
    explicit AtomicPublisher(std::unique_ptr<Impl> impl) noexcept;
    std::unique_ptr<Impl> impl_;
};

} // namespace pulp::project_package
