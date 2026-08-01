#include <pulp/project_package/atomic_publisher.hpp>

#include "native_io.hpp"
#include "project_package_test_access.hpp"

#include <pulp/timeline/asset_path.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <system_error>
#include <utility>
#include <vector>

namespace pulp::project_package {
namespace fs = std::filesystem;

namespace {

template <typename T>
runtime::Result<T, PackageError> failure(PackageErrorCode code, const fs::path& path) {
    return runtime::Result<T, PackageError>(runtime::Err(PackageError{code, path}));
}

fs::path staging_sibling(const fs::path& destination, std::uint64_t serial) {
    auto parent = destination.parent_path();
    if (parent.empty())
        parent = ".";
    const auto seed =
        static_cast<std::uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count());
    return parent / (".pulp-staging-" + std::to_string(seed ^ serial));
}

fs::path path_from_utf8(std::string_view value) {
#if defined(_WIN32)
    return fs::path(std::u8string(reinterpret_cast<const char8_t*>(value.data()), value.size()));
#else
    return fs::path(value);
#endif
}

} // namespace

struct AtomicPublisher::Impl {
    fs::path destination;
    fs::path staging;
    bool committed = false;
};

AtomicPublisher::AtomicPublisher(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}
AtomicPublisher::~AtomicPublisher() {
    cancel();
}
AtomicPublisher::AtomicPublisher(AtomicPublisher&&) noexcept = default;
AtomicPublisher& AtomicPublisher::operator=(AtomicPublisher&& other) noexcept {
    if (this != &other) {
        cancel();
        impl_ = std::move(other.impl_);
    }
    return *this;
}

runtime::Result<AtomicPublisher, PackageError>
AtomicPublisher::create(const fs::path& destination) noexcept {
    if (destination.empty())
        return failure<AtomicPublisher>(PackageErrorCode::InvalidPath, destination);
    std::error_code error;
    const auto status = fs::symlink_status(destination, error);
    if ((!error && status.type() != fs::file_type::not_found) ||
        (error && error != std::errc::no_such_file_or_directory))
        return failure<AtomicPublisher>(PackageErrorCode::PublicationConflict, destination);
    error.clear();
    auto parent = destination.parent_path();
    if (parent.empty())
        parent = ".";
    const auto parent_status = fs::symlink_status(parent, error);
    if (error || parent_status.type() != fs::file_type::directory)
        return failure<AtomicPublisher>(PackageErrorCode::InvalidPath, parent);

    static std::atomic<std::uint64_t> serial{0};
    for (std::size_t attempt = 0; attempt < 128; ++attempt) {
        auto staging = staging_sibling(destination, serial.fetch_add(1) + attempt);
        if (fs::create_directory(staging, error)) {
            auto impl = std::make_unique<Impl>();
            impl->destination = destination;
            impl->staging = std::move(staging);
            return runtime::Result<AtomicPublisher, PackageError>(
                runtime::Ok(AtomicPublisher(std::move(impl))));
        }
        if (!error)
            continue;
        if (error != std::errc::file_exists)
            return failure<AtomicPublisher>(PackageErrorCode::IoError, staging);
        error.clear();
    }
    return failure<AtomicPublisher>(PackageErrorCode::PublicationConflict, destination);
}

const fs::path& AtomicPublisher::staging_directory() const noexcept {
    static const fs::path empty;
    return impl_ ? impl_->staging : empty;
}

runtime::Result<bool, PackageError>
AtomicPublisher::write(std::string_view relative_utf8,
                       std::span<const std::uint8_t> bytes) noexcept {
    if (!impl_ || impl_->committed ||
        !timeline::package_relative_path_is_lexically_safe(relative_utf8))
        return failure<bool>(PackageErrorCode::InvalidPath, impl_ ? impl_->staging : fs::path{});
    const fs::path relative = path_from_utf8(relative_utf8);
    if (relative.empty() || relative.is_absolute() || relative.has_root_name() ||
        relative.has_root_directory())
        return failure<bool>(PackageErrorCode::InvalidPath, relative);
    const auto output = impl_->staging / relative;
    std::error_code error;
    fs::create_directories(output.parent_path(), error);
    if (error || !detail::write_exclusive_and_fence(output, bytes,
                                                    detail::PackageFaultPoint::StagedFileWritten,
                                                    detail::PackageFaultPoint::StagedFileFenced))
        return failure<bool>(PackageErrorCode::IoError, output);
    return runtime::Result<bool, PackageError>(runtime::Ok(true));
}

runtime::Result<bool, PackageError> AtomicPublisher::write(std::string_view relative_utf8,
                                                           std::string_view text) noexcept {
    return write(relative_utf8,
                 std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(text.data()),
                                               text.size()));
}

runtime::Result<AtomicPublishOutcome, PackageError>
AtomicPublisher::commit_file(const fs::path& staged_file) noexcept {
    if (!impl_ || impl_->committed || staged_file.parent_path() != impl_->staging ||
        !detail::regular_file_no_links(staged_file))
        return failure<AtomicPublishOutcome>(PackageErrorCode::InvalidPath, staged_file);
    if (!detail::fence_file(staged_file))
        return failure<AtomicPublishOutcome>(PackageErrorCode::IoError, staged_file);
    detail::invoke_fault_hook(detail::PackageFaultPoint::StagedFileFenced);
    if (!detail::publish_no_replace(staged_file, impl_->destination))
        return runtime::Result<AtomicPublishOutcome, PackageError>(
            runtime::Ok(AtomicPublishOutcome::NotPublished));
    impl_->committed = true;
    detail::invoke_fault_hook(detail::PackageFaultPoint::DirectoryPublished);
    std::error_code ignored;
    fs::remove(impl_->staging, ignored);
    auto parent = impl_->destination.parent_path();
    if (parent.empty())
        parent = ".";
    if (!detail::fence_directory(parent))
        return runtime::Result<AtomicPublishOutcome, PackageError>(
            runtime::Ok(AtomicPublishOutcome::PublishedDurabilityUncertain));
    return runtime::Result<AtomicPublishOutcome, PackageError>(
        runtime::Ok(AtomicPublishOutcome::PublishedDurably));
}

runtime::Result<AtomicPublishOutcome, PackageError> AtomicPublisher::commit_directory() noexcept {
    if (!impl_ || impl_->committed)
        return failure<AtomicPublishOutcome>(PackageErrorCode::InvalidLayout, {});
    std::vector<fs::path> directories;
    std::vector<fs::path> files;
    directories.push_back(impl_->staging);
    std::error_code error;
    for (fs::recursive_directory_iterator iterator(impl_->staging, error), end;
         !error && iterator != end; iterator.increment(error)) {
        const auto status = iterator->symlink_status(error);
        if (error || status.type() == fs::file_type::symlink)
            break;
        if (status.type() == fs::file_type::directory)
            directories.push_back(iterator->path());
        else if (status.type() == fs::file_type::regular)
            files.push_back(iterator->path());
        else
            error = std::make_error_code(std::errc::invalid_argument);
    }
    if (error)
        return failure<AtomicPublishOutcome>(PackageErrorCode::InvalidLayout, impl_->staging);
    for (const auto& file : files)
        if (!detail::regular_file_no_links(file) || !detail::fence_file(file))
            return failure<AtomicPublishOutcome>(PackageErrorCode::IoError, file);
    std::sort(directories.begin(), directories.end(), [](const auto& lhs, const auto& rhs) {
        return std::distance(lhs.begin(), lhs.end()) > std::distance(rhs.begin(), rhs.end());
    });
    for (const auto& directory : directories)
        if (!detail::fence_directory(directory))
            return failure<AtomicPublishOutcome>(PackageErrorCode::IoError, directory);
    detail::invoke_fault_hook(detail::PackageFaultPoint::DirectoryTreeFenced);
    if (!detail::publish_no_replace(impl_->staging, impl_->destination))
        return runtime::Result<AtomicPublishOutcome, PackageError>(
            runtime::Ok(AtomicPublishOutcome::NotPublished));
    impl_->committed = true;
    detail::invoke_fault_hook(detail::PackageFaultPoint::DirectoryPublished);
    auto parent = impl_->destination.parent_path();
    if (parent.empty())
        parent = ".";
    if (!detail::fence_directory(parent))
        return runtime::Result<AtomicPublishOutcome, PackageError>(
            runtime::Ok(AtomicPublishOutcome::PublishedDurabilityUncertain));
    return runtime::Result<AtomicPublishOutcome, PackageError>(
        runtime::Ok(AtomicPublishOutcome::PublishedDurably));
}

void AtomicPublisher::cancel() noexcept {
    if (!impl_ || impl_->committed || impl_->staging.empty())
        return;
    std::error_code ignored;
    fs::remove_all(impl_->staging, ignored);
    impl_->staging.clear();
}

} // namespace pulp::project_package
