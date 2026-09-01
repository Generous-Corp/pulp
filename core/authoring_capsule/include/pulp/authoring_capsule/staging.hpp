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

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

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

/// Read one member back out of a staged tree.
///
/// This exists so a consumer never joins a manifest path to a staging root
/// itself. The path rules and the join belong to the same layer: a consumer
/// doing `staging.root() / entry.path` by hand is a path-admission decision
/// made outside the module that owns path admission, and it silently loses the
/// NFC, depth, byte-budget, reserved-name, and containment checks. So the row
/// is re-admitted here — this is a public entry point taking an arbitrary
/// `FileEntry`, and it cannot assume a caller ran preview or extraction first.
///
/// Every hop is opened relative to the staging area's pinned root refusing to
/// follow links, and only a plain file is read: nothing in this module creates
/// a symlink, a device, or a directory where a member belongs, so one standing
/// there means the tree is not the one extraction wrote.
///
/// The size on disk must equal `entry.bytes`. That is checked before the bytes
/// are copied out, so the read is bounded by what the owner-private tree
/// actually holds rather than by a size the row asserted, and a truncated or
/// replaced member is refused rather than returned as if it were whole. The
/// content digest is *not* recomputed: `extract_declared()` verified it before
/// the member was allowed to land, and the staging directory is readable only
/// by its owner, so re-hashing on every read would charge a repeated cost for
/// a property already established. A caller that wants the stronger check can
/// hash the returned bytes against `entry.sha256` itself.
///
/// Failures are exact. A path that fails admission reports whatever
/// `admit_member_path()` decided — `path_rejected` for a path that could
/// escape or that the grammar refuses. Everything else is `staging_failed`,
/// with the declared and found sizes in `required` and `found` when the
/// disagreement is a size.
runtime::Result<std::vector<std::uint8_t>, CapsuleError>
read_staged_member(const StagingArea& staging, const FileEntry& entry);

/// Reads a declared member from a staging root the caller does not own.
///
/// `ProfileValidator::validate_staged()` receives the staging root as a path
/// rather than as a `StagingArea`, and a `StagingArea` cannot be adopted from
/// an existing tree — `create()` makes a new one. Without this overload the
/// entry point that exists so a consumer never joins an untrusted member path
/// to a directory by hand is unreachable from the one place every consumer
/// needs exactly that, and each profile hand-rolls the join instead.
///
/// Identical to the overload above in every respect: the same admission, the
/// same directory-identity checks before and after the read, the same errors.
/// The `StagingArea` is only ever read for its root.
runtime::Result<std::vector<std::uint8_t>, CapsuleError>
read_staged_member(const std::filesystem::path& staging_root, const FileEntry& entry);

}  // namespace pulp::authoring_capsule
