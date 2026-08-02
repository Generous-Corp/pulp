#include <pulp/project_package/atomic_publisher.hpp>
#include <pulp/project_package/project_package.hpp>

#include <type_traits>

static_assert(std::is_move_constructible_v<pulp::project_package::AtomicPublisher>);
static_assert(!std::is_copy_constructible_v<pulp::project_package::AtomicPublisher>);

int main() {
    using pulp::project_package::AtomicPublisher;
    using pulp::project_package::AtomicPublishOutcome;
    using pulp::project_package::BlobStore;
    using pulp::project_package::PackageErrorCode;
    using pulp::project_package::PackageLimits;

    const PackageLimits limits;
    if (limits.max_project_bytes == 0 || limits.max_blob_bytes < limits.max_project_bytes)
        return 1;
    if (BlobStore::Media == BlobStore::Receipt)
        return 2;
    if (AtomicPublishOutcome::PublishedDurably ==
        AtomicPublishOutcome::PublishedDurabilityUncertain)
        return 3;

    auto empty_path = AtomicPublisher::create({});
    auto empty_file_path = AtomicPublisher::create_file({});
    const auto staging_file = &AtomicPublisher::staging_file;
    (void)staging_file;
    return !empty_path && empty_path.error().code == PackageErrorCode::InvalidPath &&
                   !empty_file_path &&
                   empty_file_path.error().code == PackageErrorCode::InvalidPath
               ? 0
               : 4;
}
