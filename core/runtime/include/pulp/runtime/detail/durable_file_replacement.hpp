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
    DurableFileCommitOutcome commit() noexcept;
    void cancel() noexcept;

  private:
    struct Impl;
    friend struct DurableFileReplacementTestAccess;
    explicit DurableFileReplacement(std::unique_ptr<Impl> impl) noexcept;
    std::unique_ptr<Impl> impl_;
};

} // namespace pulp::runtime::detail
