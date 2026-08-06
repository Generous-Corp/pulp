#pragma once

#include <pulp/project_package/atomic_publisher.hpp>
#include <pulp/timeline/assets.hpp>
#include <pulp/timeline/model.hpp>
#include <pulp/timeline/schema_registry.hpp>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <vector>

namespace pulp::project_package {

/// Content-addressed store within a project package.
enum class BlobStore : std::uint8_t { Media, State, Artifact, Receipt };

/// Exact package location of one SHA-256-addressed blob.
struct BlobReference {
    /// Store containing the blob.
    BlobStore store = BlobStore::Media;
    /// SHA-256 identity used as the blob filename.
    timeline::ContentHash hash;
    constexpr auto operator<=>(const BlobReference&) const = default;
};

/// Admission limits for untrusted project and blob bytes.
struct PackageLimits {
    /// Maximum admitted bytes for one content-addressed blob.
    std::uint64_t max_blob_bytes = 8ull * 1024ull * 1024ull * 1024ull;
    /// Maximum admitted bytes for `project.json`.
    std::uint64_t max_project_bytes = 64ull * 1024ull * 1024ull;
};

/// Validated package generation and its package-owned working directories.
struct OpenPackageResult {
    /// Fully decoded, reference-validated generation.
    timeline::Project project;
    /// Stable directory in which FileJournal state may be opened.
    std::filesystem::path journal_directory;
    /// Recoverable derived-data directory.
    std::filesystem::path cache_directory;
    /// Whether this open recreated a missing cache directory.
    bool cache_recreated = false;
};

/// Cooperative exclusive writer for one stable package root. Other writers
/// must honor the package lock and must not rename or replace package entries
/// out of band while a writer is open.
class PackageWriter {
  public:
    /// Creates or opens the package layout and acquires its non-blocking writer lock.
    static runtime::Result<PackageWriter, PackageError>
    create(const std::filesystem::path& root, timeline::SchemaRegistry registry,
           const PackageLimits& limits = {}) noexcept;

    ~PackageWriter();
    PackageWriter(PackageWriter&&) noexcept;
    PackageWriter& operator=(PackageWriter&&) noexcept;
    PackageWriter(const PackageWriter&) = delete;
    PackageWriter& operator=(const PackageWriter&) = delete;

    /// Returns the canonical stable package root.
    const std::filesystem::path& root() const noexcept;
    /// Reports whether opening reclaimed a private temp left by an interrupted writer.
    bool recovered_staging() const noexcept;

    /// Hash-verifies, fences, and no-replace publishes one blob before it can be referenced.
    runtime::Result<BlobReference, PackageError>
    stage_blob(BlobStore store, const timeline::ContentHash& expected,
               std::span<const std::uint8_t> bytes) noexcept;
    /// Publishes `project.json` only after validating every packaged asset reference.
    runtime::Result<AtomicPublishOutcome, PackageError>
    publish(const timeline::Project& project) noexcept;

  private:
    struct Impl;
    explicit PackageWriter(std::unique_ptr<Impl> impl) noexcept;
    std::unique_ptr<Impl> impl_;
};

/// Opens and validates the sole referencing generation in a stable package root.
runtime::Result<OpenPackageResult, PackageError>
open_package(const std::filesystem::path& root, const timeline::SchemaRegistry& registry,
             const PackageLimits& limits = {}) noexcept;

/// Reads a bounded blob and verifies its bytes against the reference hash.
runtime::Result<std::vector<std::uint8_t>, PackageError>
read_blob(const std::filesystem::path& root, const BlobReference& reference,
          std::uint64_t maximum_bytes) noexcept;

} // namespace pulp::project_package
