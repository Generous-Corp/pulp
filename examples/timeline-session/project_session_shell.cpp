#include "project_session_shell.hpp"

#include <system_error>
#include <utility>

namespace pulp::examples::timeline_session {
namespace {

ProjectSessionError package_error(project_package::PackageError error) {
    ProjectSessionError result;
    result.code = ProjectSessionErrorCode::Package;
    result.package_error = std::move(error);
    return result;
}

ProjectSessionError publication_error(project_package::AtomicPublishOutcome outcome) {
    ProjectSessionError result;
    result.code = ProjectSessionErrorCode::PackagePublicationNotDurable;
    result.publish_outcome = outcome;
    return result;
}

ProjectSessionError file_journal_error(timeline::FileJournalError error) {
    ProjectSessionError result;
    result.code = ProjectSessionErrorCode::FileJournal;
    result.file_journal_error = std::move(error);
    return result;
}

ProjectSessionError transaction_error(timeline::TransactionError error) {
    ProjectSessionError result;
    result.code = ProjectSessionErrorCode::Transaction;
    result.transaction_error = std::move(error);
    return result;
}

ProjectSessionError not_open_error() {
    return ProjectSessionError{.code = ProjectSessionErrorCode::NotOpen};
}

ProjectSessionError already_exists_error() {
    return ProjectSessionError{.code = ProjectSessionErrorCode::AlreadyExists};
}

ProjectSessionError packaged_asset_unsupported_error() {
    return ProjectSessionError{.code = ProjectSessionErrorCode::PackagedAssetUnsupported};
}

bool path_entry_exists(const std::filesystem::path& path,
                       std::optional<project_package::PackageError>& error) {
    std::error_code status_error;
    const auto status = std::filesystem::symlink_status(path, status_error);
    if (!status_error)
        return status.type() != std::filesystem::file_type::not_found;
    if (status_error == std::errc::no_such_file_or_directory)
        return false;
    error = project_package::PackageError{project_package::PackageErrorCode::IoError, path};
    return false;
}

} // namespace

struct ProjectSessionShell::Impl {
    std::filesystem::path root;
    std::filesystem::path journal_file;
    timeline::SchemaRegistry registry;
    ProjectSessionLimits limits;
    std::optional<project_package::PackageWriter> package_writer;
    std::shared_ptr<timeline::FileJournal> file_journal;
    std::unique_ptr<timeline::DocumentSession> session;
    std::optional<timeline::WriterToken> writer;
    bool recovered_existing = false;
    bool repaired_torn_tail = false;
    bool cache_recreated = false;

    Impl(std::filesystem::path package_root, timeline::SchemaRegistry schema_registry,
         ProjectSessionLimits resource_limits)
        : root(std::move(package_root)), registry(std::move(schema_registry)),
          limits(std::move(resource_limits)) {}

    runtime::Result<bool, ProjectSessionError>
    open_with_writer(project_package::PackageWriter package_lock) {
        root = package_lock.root();

        auto opened = project_package::open_package(root, registry, limits.package);
        if (!opened)
            return runtime::Err(package_error(std::move(opened).error()));
        auto package = std::move(opened).value();
        journal_file = package.journal_directory / "session.ptlj";

        auto recovered = timeline::FileJournal::open(journal_file, std::move(package.project),
                                                     registry, limits.file_journal);
        if (!recovered)
            return runtime::Err(file_journal_error(std::move(recovered).error()));
        auto journal_open = std::move(recovered).value();

        auto restored = timeline::DocumentSession::restore(
            std::move(journal_open.checkpoint), journal_open.revision, limits.session,
            journal_open.sink);
        if (!restored)
            return runtime::Err(transaction_error(std::move(restored).error()));
        auto restored_session = std::move(restored).value();

        auto registered = restored_session->register_writer();
        if (!registered)
            return runtime::Err(transaction_error(std::move(registered).error()));

        recovered_existing = journal_open.recovered_existing;
        repaired_torn_tail = journal_open.repaired_torn_tail;
        cache_recreated = package.cache_recreated;
        package_writer.emplace(std::move(package_lock));
        file_journal = std::move(journal_open.sink);
        session = std::move(restored_session);
        writer.emplace(std::move(registered).value());
        return runtime::Ok(true);
    }

    void close() noexcept {
        writer.reset();
        session.reset();
        file_journal.reset();
        package_writer.reset();
    }
};

ProjectSessionShell::ProjectSessionShell(std::unique_ptr<Impl> impl) noexcept
    : impl_(std::move(impl)) {}

ProjectSessionShell::~ProjectSessionShell() = default;
ProjectSessionShell::ProjectSessionShell(ProjectSessionShell&&) noexcept = default;
ProjectSessionShell& ProjectSessionShell::operator=(ProjectSessionShell&&) noexcept = default;

runtime::Result<std::unique_ptr<ProjectSessionShell>, ProjectSessionError>
ProjectSessionShell::create(const std::filesystem::path& package_root, timeline::Project initial,
                            timeline::SchemaRegistry registry, ProjectSessionLimits limits) {
    auto impl = std::make_unique<Impl>(package_root, std::move(registry), std::move(limits));
    auto writer = project_package::PackageWriter::create(impl->root, impl->registry,
                                                         impl->limits.package);
    if (!writer)
        return runtime::Err(package_error(std::move(writer).error()));
    auto package_lock = std::move(writer).value();
    impl->root = package_lock.root();

    std::optional<project_package::PackageError> status_error;
    const bool has_generation = path_entry_exists(impl->root / "project.json", status_error);
    if (status_error)
        return runtime::Err(package_error(std::move(*status_error)));
    const bool has_journal = path_entry_exists(impl->root / "journal" / "session.ptlj",
                                               status_error);
    if (status_error)
        return runtime::Err(package_error(std::move(*status_error)));
    if (has_generation || has_journal)
        return runtime::Err(already_exists_error());

    auto published = package_lock.publish(initial);
    if (!published)
        return runtime::Err(package_error(std::move(published).error()));
    if (published.value() != project_package::AtomicPublishOutcome::PublishedDurably)
        return runtime::Err(publication_error(published.value()));

    auto opened = impl->open_with_writer(std::move(package_lock));
    if (!opened)
        return runtime::Err(std::move(opened).error());
    return runtime::Ok(std::unique_ptr<ProjectSessionShell>(
        new ProjectSessionShell(std::move(impl))));
}

runtime::Result<std::unique_ptr<ProjectSessionShell>, ProjectSessionError>
ProjectSessionShell::open(const std::filesystem::path& package_root,
                          timeline::SchemaRegistry registry, ProjectSessionLimits limits) {
    auto impl = std::make_unique<Impl>(package_root, std::move(registry), std::move(limits));
    auto writer = project_package::PackageWriter::create(impl->root, impl->registry,
                                                         impl->limits.package);
    if (!writer)
        return runtime::Err(package_error(std::move(writer).error()));

    auto opened = impl->open_with_writer(std::move(writer).value());
    if (!opened)
        return runtime::Err(std::move(opened).error());
    return runtime::Ok(std::unique_ptr<ProjectSessionShell>(
        new ProjectSessionShell(std::move(impl))));
}

runtime::Result<timeline::CommitResult, ProjectSessionError>
ProjectSessionShell::submit(std::vector<timeline::Command> commands) {
    if (!is_open())
        return runtime::Err(not_open_error());
    for (const auto& command : commands)
        if (std::holds_alternative<timeline::CreateAsset>(command))
            return runtime::Err(packaged_asset_unsupported_error());

    timeline::Transaction transaction;
    transaction.id = impl_->writer->allocate_transaction_id();
    transaction.expected_revision = impl_->session->revision();
    transaction.commands.reserve(commands.size());
    for (auto& command : commands)
        transaction.commands.push_back(
            {impl_->writer->allocate_command_id(), std::move(command)});

    auto committed = impl_->session->submit(*impl_->writer, std::move(transaction));
    if (!committed)
        return runtime::Err(transaction_error(std::move(committed).error()));
    return runtime::Ok(std::move(committed).value());
}

runtime::Result<project_package::AtomicPublishOutcome, ProjectSessionError>
ProjectSessionShell::save() {
    if (!is_open())
        return runtime::Err(not_open_error());

    const auto current = impl_->session->current();
    if (!impl_->session->checkpoint(current.revision)) {
        timeline::TransactionError error;
        error.code = timeline::ConflictCode::JournalDurability;
        error.current_revision = current.revision;
        return runtime::Err(transaction_error(std::move(error)));
    }

    auto published = impl_->package_writer->publish(*current.snapshot);
    if (!published)
        return runtime::Err(package_error(std::move(published).error()));
    if (published.value() != project_package::AtomicPublishOutcome::PublishedDurably)
        return runtime::Err(publication_error(published.value()));
    return runtime::Ok(published.value());
}

void ProjectSessionShell::close() noexcept {
    if (impl_)
        impl_->close();
}

runtime::Result<bool, ProjectSessionError> ProjectSessionShell::reopen() {
    if (!impl_)
        return runtime::Err(not_open_error());
    impl_->close();

    auto writer = project_package::PackageWriter::create(impl_->root, impl_->registry,
                                                         impl_->limits.package);
    if (!writer)
        return runtime::Err(package_error(std::move(writer).error()));
    return impl_->open_with_writer(std::move(writer).value());
}

bool ProjectSessionShell::is_open() const noexcept {
    return impl_ && impl_->package_writer && impl_->file_journal && impl_->session && impl_->writer;
}

std::shared_ptr<const timeline::Project> ProjectSessionShell::project() const noexcept {
    return is_open() ? impl_->session->current().snapshot : nullptr;
}

timeline::DocumentRevision ProjectSessionShell::revision() const noexcept {
    return is_open() ? impl_->session->current().revision : timeline::DocumentRevision{};
}

const std::filesystem::path& ProjectSessionShell::package_root() const noexcept {
    static const std::filesystem::path empty;
    return impl_ ? impl_->root : empty;
}

const std::filesystem::path& ProjectSessionShell::journal_path() const noexcept {
    static const std::filesystem::path empty;
    return impl_ ? impl_->journal_file : empty;
}

bool ProjectSessionShell::recovered_existing() const noexcept {
    return impl_ && impl_->recovered_existing;
}

bool ProjectSessionShell::repaired_torn_tail() const noexcept {
    return impl_ && impl_->repaired_torn_tail;
}

bool ProjectSessionShell::cache_recreated() const noexcept {
    return impl_ && impl_->cache_recreated;
}

} // namespace pulp::examples::timeline_session
