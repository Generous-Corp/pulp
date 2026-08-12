#pragma once

#include <pulp/project_package/project_package.hpp>
#include <pulp/timeline/document_session.hpp>
#include <pulp/timeline/file_journal.hpp>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <vector>

namespace pulp::examples::timeline_session {

/// Stable failure categories for the durable project-session example.
enum class ProjectSessionErrorCode : std::uint8_t {
    /// The requested operation requires an open session.
    NotOpen,
    /// Create was asked to replace an existing package or session journal.
    AlreadyExists,
    /// The example was asked to add packaged media without a blob-staging API.
    PackagedAssetUnsupported,
    /// Package creation, validation, or publication failed.
    Package,
    /// The package generation became visible without confirmed durability.
    PackagePublicationNotDurable,
    /// Journal creation or recovery failed.
    FileJournal,
    /// Session restore, writer registration, or transaction submission failed.
    Transaction,
};

/// Typed failure from ProjectSessionShell.
///
/// The nested error matching code is populated when an underlying Pulp API
/// supplied structured detail. publish_outcome is populated when publication
/// returned a non-durable outcome rather than an error.
struct ProjectSessionError {
    ProjectSessionErrorCode code = ProjectSessionErrorCode::NotOpen;
    std::optional<project_package::PackageError> package_error;
    std::optional<project_package::AtomicPublishOutcome> publish_outcome;
    std::optional<timeline::FileJournalError> file_journal_error;
    std::optional<timeline::TransactionError> transaction_error;
};

/// Resource ceilings used by a durable project session.
struct ProjectSessionLimits {
    project_package::PackageLimits package;
    timeline::FileJournalLimits file_journal;
    timeline::SessionLimits session;
};

/// Reusable durable shell around a package, file journal, and DocumentSession.
///
/// `create()` publishes a new package generation and refuses to replace an
/// existing durable generation. `open()` validates the package, recovers its
/// journal, and restores a DocumentSession at the recovered revision. Ordinary commands
/// submitted through this class are journaled before their revision is
/// published. `save()` checkpoints that journal and then durably publishes the
/// matching package generation.
///
/// All member calls belong on one control thread. A shared project returned by
/// `project()` remains immutable and valid independently of later calls.
class ProjectSessionShell {
  public:
    /// Creates and opens a new package rooted at package_root.
    ///
    /// An existing `project.json` or shell journal is rejected with
    /// AlreadyExists; this function never replaces an existing durable
    /// generation. Acquiring the cooperative package writer may still repair
    /// package-owned staging or layout before this check.
    static runtime::Result<std::unique_ptr<ProjectSessionShell>, ProjectSessionError>
    create(const std::filesystem::path& package_root, timeline::Project initial,
           timeline::SchemaRegistry registry, ProjectSessionLimits limits = {});

    /// Opens an existing package and restores its durable journal state.
    static runtime::Result<std::unique_ptr<ProjectSessionShell>, ProjectSessionError>
    open(const std::filesystem::path& package_root, timeline::SchemaRegistry registry,
         ProjectSessionLimits limits = {});

    ~ProjectSessionShell();
    ProjectSessionShell(ProjectSessionShell&&) noexcept;
    ProjectSessionShell& operator=(ProjectSessionShell&&) noexcept;
    ProjectSessionShell(const ProjectSessionShell&) = delete;
    ProjectSessionShell& operator=(const ProjectSessionShell&) = delete;

    /// Applies one document-only transaction at the current revision.
    ///
    /// The shell allocates writer-scoped transaction and command IDs. An empty
    /// command list is passed to DocumentSession and receives its normal typed
    /// EmptyTransaction rejection. CreateAsset is rejected before journaling:
    /// this example deliberately has no blob-staging API, so it cannot safely
    /// admit a package generation that may be impossible to publish.
    runtime::Result<timeline::CommitResult, ProjectSessionError>
    submit(std::vector<timeline::Command> commands);

    /// Checkpoints the current journal and durably publishes the same snapshot.
    ///
    /// A visible but durability-uncertain package generation is returned as an
    /// error; success is always PublishedDurably.
    runtime::Result<project_package::AtomicPublishOutcome, ProjectSessionError> save();

    /// Releases the session, journal file, and package writer lock, in that order.
    void close() noexcept;

    /// Closes and restores the package again using the original registry and limits.
    ///
    /// Failure leaves the shell closed and eligible for another reopen attempt.
    runtime::Result<bool, ProjectSessionError> reopen();

    /// Reports whether the package writer, journal, session, and writer token are live.
    bool is_open() const noexcept;
    /// Returns one immutable project snapshot from the restored session.
    std::shared_ptr<const timeline::Project> project() const noexcept;
    /// Returns the revision paired with the snapshot observed by this call.
    timeline::DocumentRevision revision() const noexcept;

    /// Returns the canonical package root after the first successful writer acquisition.
    const std::filesystem::path& package_root() const noexcept;
    /// Returns the package-owned journal file used by this shell.
    const std::filesystem::path& journal_path() const noexcept;
    /// Reports whether the most recent successful open recovered an existing
    /// journal.
    bool recovered_existing() const noexcept;
    /// Reports whether the most recent successful open repaired a torn journal tail.
    bool repaired_torn_tail() const noexcept;
    /// Reports whether the most recent successful package open recreated its
    /// cache directory.
    bool cache_recreated() const noexcept;

  private:
    struct Impl;
    explicit ProjectSessionShell(std::unique_ptr<Impl> impl) noexcept;
    std::unique_ptr<Impl> impl_;
};

} // namespace pulp::examples::timeline_session
