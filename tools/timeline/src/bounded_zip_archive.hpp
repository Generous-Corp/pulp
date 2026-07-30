#pragma once

#include <pulp/runtime/result.hpp>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <memory_resource>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace pulp::interchange {
struct ExportArtifacts;
}

namespace pulp::tools::timeline::detail {

inline constexpr std::uint64_t kZipStdioReserveBytes = 64u * 1024u;
inline constexpr std::uint64_t kExternalArtifactMetadataReserveBytes = 64u;
inline constexpr std::uint64_t kZipControlAndApiReserveBytes = 4u * 1024u;

class AllocationBudget {
  public:
    explicit AllocationBudget(std::uint64_t limit);
    AllocationBudget(AllocationBudget&&) noexcept;
    AllocationBudget& operator=(AllocationBudget&&) noexcept;
    AllocationBudget(const AllocationBudget&) = delete;
    AllocationBudget& operator=(const AllocationBudget&) = delete;
    ~AllocationBudget();
    std::pmr::memory_resource* resource() noexcept;
    bool acquire_external(std::uint64_t bytes) noexcept;
    void release_external(std::uint64_t bytes) noexcept;
    std::uint64_t peak_bytes() const noexcept;
    std::uint64_t balance_bytes() const noexcept;
    std::uint64_t remaining_bytes() const noexcept;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

enum class DawProjectArchiveErrorCode : std::uint8_t { Export, Publish };
struct DawProjectArchiveError {
    DawProjectArchiveErrorCode code = DawProjectArchiveErrorCode::Export;
    std::string message;
};

struct ZipBudgetStats {
    std::uint64_t peak_bytes = 0;
    std::uint64_t final_balance_bytes = 0;
};

// Budget contract: PMR/miniz allocations are charged by their actual allocator
// request plus the ledger header/alignment allowance. CRT stdio is represented
// by kZipStdioReserveBytes because its private buffering is not observable.
// Caller-owned export artifacts are represented separately by their retained
// capacities plus an explicit per-allocation metadata reserve.

class BoundedZipArchive {
  public:
    BoundedZipArchive(BoundedZipArchive&&) noexcept;
    BoundedZipArchive& operator=(BoundedZipArchive&&) noexcept;
    BoundedZipArchive(const BoundedZipArchive&) = delete;
    BoundedZipArchive& operator=(const BoundedZipArchive&) = delete;
    ~BoundedZipArchive();

    std::optional<std::span<const std::uint8_t>> find(std::string_view name) const noexcept;
    bool acquire_external(std::uint64_t bytes) noexcept;
    void release_external(std::uint64_t bytes) noexcept;
    std::uint64_t peak_bytes() const noexcept;
    ZipBudgetStats close() noexcept;

  private:
    struct Impl;
    explicit BoundedZipArchive(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
    friend runtime::Result<BoundedZipArchive, std::string>
    read_bounded_zip_archive(const std::filesystem::path&, std::uint64_t, std::size_t, std::size_t,
                             std::size_t);
};

runtime::Result<BoundedZipArchive, std::string>
read_bounded_zip_archive(const std::filesystem::path& input, std::uint64_t max_working_set_bytes,
                         std::size_t max_file_entries, std::size_t max_archive_entries,
                         std::size_t max_path_bytes);

runtime::Result<std::uint64_t, DawProjectArchiveError>
write_dawproject_archive_no_replace(const pulp::interchange::ExportArtifacts& artifacts,
                                    const std::filesystem::path& destination,
                                    std::uint64_t max_working_set_bytes);

std::optional<std::uint64_t>
retained_external_artifact_reserve(const pulp::interchange::ExportArtifacts& artifacts) noexcept;

runtime::Result<ZipBudgetStats, std::string>
inspect_dawproject_archive(const std::filesystem::path& input, std::uint64_t max_working_set_bytes,
                           std::size_t max_file_entries, std::size_t max_archive_entries,
                           std::size_t max_path_bytes);

} // namespace pulp::tools::timeline::detail
