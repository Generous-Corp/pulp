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
    /// The destination is visible, but final permission adoption or namespace fencing failed.
    PublishedDurabilityUncertain,
};

/// Builds a private sibling directory and publishes it at a previously absent
/// destination. Every staged file and directory entry is fenced before the
/// destination name becomes visible. The private stage coordinates trusted
/// writers; another process running as the same account must not rename or
/// replace its entries behind the publisher.
class AtomicPublisher {
  public:
    /// Creates a private staging sibling for a destination that must not exist.
    static runtime::Result<AtomicPublisher, PackageError>
    create(const std::filesystem::path& destination) noexcept;
    /// Creates a private, pre-permissioned file slot for one external writer.
    static runtime::Result<AtomicPublisher, PackageError>
    create_file(const std::filesystem::path& destination) noexcept;

    ~AtomicPublisher();
    AtomicPublisher(AtomicPublisher&&) noexcept;
    AtomicPublisher& operator=(AtomicPublisher&&) noexcept;
    AtomicPublisher(const AtomicPublisher&) = delete;
    AtomicPublisher& operator=(const AtomicPublisher&) = delete;

    /// Returns the private root for a directory publication. External writers must stop
    /// before `commit_directory()` or `cancel()` begins.
    const std::filesystem::path& staging_directory() const noexcept;
    /// Returns the pre-created private file slot for a file publication. The external writer
    /// may truncate and fill this file, but must stop before `commit_file()` or `cancel()` begins.
    const std::filesystem::path& staging_file() const noexcept;
    /// Writes and durably fences one lexically safe relative file.
    runtime::Result<bool, PackageError> write(std::string_view relative_utf8,
                                              std::span<const std::uint8_t> bytes) noexcept;
    /// Writes and durably fences one UTF-8 text file.
    runtime::Result<bool, PackageError> write(std::string_view relative_utf8,
                                              std::string_view text) noexcept;
    /// Publishes the pre-created `staging_file()` at the destination.
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
    static runtime::Result<AtomicPublisher, PackageError>
    create_impl(const std::filesystem::path& destination, bool file) noexcept;
    explicit AtomicPublisher(std::unique_ptr<Impl> impl) noexcept;
    std::unique_ptr<Impl> impl_;
};

} // namespace pulp::project_package
