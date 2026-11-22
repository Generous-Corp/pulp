#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <utility>

namespace pulp::runtime::detail {

enum class DurableFileCommitOutcome : std::uint8_t {
    NotReplaced,
    ReplacedDurably,
    ReplacedButDirectorySyncFailed,
};

/// How hard commit() pushes the new bytes toward media.
///   full:    flush through the drive cache (F_FULLFSYNC on Apple platforms).
///            Survives power loss; costs tens of milliseconds on a small file.
///   relaxed: fsync the file and the parent directory, without the drive-cache
///            flush. The replacement is still atomic (a reader sees the old or
///            the new bytes, never a torn file) and ordered (data reaches the
///            OS before the rename). Only the last write can be lost on power
///            loss. Intended for settings files saved from a UI thread.
enum class DurableFileDurability : std::uint8_t {
    full,
    relaxed,
};

struct DurableFileReplacementTestAccess;

class DurableFileReplacement {
  public:
    static std::optional<DurableFileReplacement>
    create(const std::filesystem::path& destination) noexcept;

    ~DurableFileReplacement();
    DurableFileReplacement(DurableFileReplacement&&) noexcept;
    DurableFileReplacement& operator=(DurableFileReplacement&&) noexcept;

    DurableFileReplacement(const DurableFileReplacement&) = delete;
    DurableFileReplacement& operator=(const DurableFileReplacement&) = delete;

    bool valid() const noexcept;
    int native_descriptor() const noexcept;
    const std::filesystem::path& temporary_path() const noexcept;
    bool write_all(std::span<const std::uint8_t> bytes) noexcept;
    DurableFileCommitOutcome
    commit(DurableFileDurability durability = DurableFileDurability::full) noexcept;
    void cancel() noexcept;

  private:
    struct Impl;
    friend struct DurableFileReplacementTestAccess;
    explicit DurableFileReplacement(std::unique_ptr<Impl> impl) noexcept;
    std::unique_ptr<Impl> impl_;
};

} // namespace pulp::runtime::detail
