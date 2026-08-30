#pragma once

/// @file staging.hpp
/// Extract privately, publish atomically, never replace.
///
/// Import writes into a directory only its owner can see, then makes the
/// result visible under a name that was previously absent. A matching incoming
/// project identity therefore cannot silently overwrite a project the user
/// already has, and a failure at any point leaves the last good state exactly
/// as it was.

#include <pulp/authoring_capsule/archive.hpp>
#include <pulp/authoring_capsule/manifest.hpp>
#include <pulp/authoring_capsule/status.hpp>
#include <pulp/runtime/result.hpp>

#include <filesystem>
#include <functional>
#include <memory>
#include <string>

namespace pulp::authoring_capsule {

/// A private directory that is removed when it goes out of scope unless it was
/// published. Owner-only permissions: another account must not be able to read
/// a capsule's contents mid-import, nor swap a member under the validator.
class StagingArea {
public:
    static runtime::Result<StagingArea, CapsuleError> create(const std::filesystem::path& parent);

    StagingArea(StagingArea&&) noexcept;
    StagingArea& operator=(StagingArea&&) noexcept;
    StagingArea(const StagingArea&) = delete;
    StagingArea& operator=(const StagingArea&) = delete;
    ~StagingArea();

    const std::filesystem::path& root() const noexcept;

    /// Rename the staged tree to `destination`, which must not exist. Fails
    /// with `publication_conflict` if it appears in the meantime — a race
    /// resolves to a refusal, never to a clobber.
    runtime::Result<void, CapsuleError> publish_no_replace(const std::filesystem::path& destination);

private:
    struct Impl;
    explicit StagingArea(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};

/// Progress and cancellation. Returning false cancels; the staging area is
/// discarded and nothing outside it was ever touched.
using ExtractionProgress = std::function<bool(std::size_t member_index, std::size_t member_count)>;

/// Extract only the members the manifest declares, verifying each digest as it
/// lands. An undeclared member is never written, so a capsule cannot smuggle a
/// file past the closure it published.
runtime::Result<void, CapsuleError> extract_declared(const CapsuleArchive& archive,
                                                     const Manifest& manifest,
                                                     const StagingArea& staging,
                                                     const ExtractionProgress& progress = {});

}  // namespace pulp::authoring_capsule
