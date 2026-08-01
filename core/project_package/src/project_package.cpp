#include <pulp/project_package/project_package.hpp>

#include "native_io.hpp"
#include "project_package_test_access.hpp"

#include <pulp/runtime/crypto.hpp>
#include <pulp/timeline/asset_path.hpp>
#include <pulp/timeline/serialize.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <string_view>
#include <system_error>
#include <utility>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#endif

namespace pulp::project_package {
namespace fs = std::filesystem;

namespace {

constexpr std::string_view kProjectFile = "project.json";
constexpr std::string_view kLockFile = ".pulp-package.lock";
constexpr std::string_view kStagePrefix = ".pulp-stage-";

template <typename T>
runtime::Result<T, PackageError> failure(PackageErrorCode code, const fs::path& path) {
    return runtime::Result<T, PackageError>(runtime::Err(PackageError{code, path}));
}

std::string_view store_name(BlobStore store) noexcept {
    switch (store) {
    case BlobStore::Media:
        return "media";
    case BlobStore::State:
        return "state";
    case BlobStore::Artifact:
        return "artifacts";
    case BlobStore::Receipt:
        return "receipts";
    }
    return {};
}

bool valid_store(BlobStore store) noexcept {
    return store == BlobStore::Media || store == BlobStore::State || store == BlobStore::Artifact ||
           store == BlobStore::Receipt;
}

fs::path path_from_utf8(std::string_view value) {
#if defined(_WIN32)
    return fs::path(std::u8string(reinterpret_cast<const char8_t*>(value.data()), value.size()));
#else
    return fs::path(value);
#endif
}

fs::path blob_path(const fs::path& root, const BlobReference& reference) {
    auto name = reference.hash.to_hex();
    if (reference.store == BlobStore::Receipt)
        name += ".json";
    return root / store_name(reference.store) / name;
}

bool is_real_directory(const fs::path& path) {
    std::error_code error;
    return fs::symlink_status(path, error).type() == fs::file_type::directory && !error;
}

bool hash_matches(const fs::path& path, const timeline::ContentHash& expected,
                  std::uint64_t maximum) {
    if (!expected.valid())
        return false;
    auto file = detail::PinnedFile::open(path, false);
    if (!file)
        return false;
    if (!file->hash_matches(expected.to_hex(), maximum))
        return false;
    detail::invoke_fault_hook(detail::PackageFaultPoint::BlobReferenceVerified);
    return file->still_named_by(path);
}

std::uint64_t next_serial() noexcept {
    static std::atomic<std::uint64_t> serial{0};
    return static_cast<std::uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count()) ^
           serial.fetch_add(1, std::memory_order_relaxed);
}

fs::path stage_path(const fs::path& directory) {
    return directory / (std::string(kStagePrefix) + std::to_string(next_serial()));
}

bool validate_locator(const fs::path& root, const timeline::AssetLocator& locator,
                      const timeline::ContentHash& expected, std::uint64_t maximum) {
    if (locator.kind != timeline::AssetLocatorKind::PackageRelative)
        return true;
    if (!timeline::package_relative_path_is_lexically_safe(locator.hint))
        return false;
    const auto canonical = std::string("media/") + expected.to_hex();
    return locator.hint == canonical &&
           hash_matches(root / path_from_utf8(locator.hint), expected, maximum);
}

bool validate_project_references(const fs::path& root, const timeline::Project& project,
                                 std::uint64_t maximum) {
    for (const auto& asset : project.assets()) {
        const bool embedded = asset.storage_policy == timeline::AssetStoragePolicy::Embedded;
        if (embedded && !hash_matches(blob_path(root, {BlobStore::Media, asset.content_hash}),
                                      asset.content_hash, maximum))
            return false;
        for (const auto& locator : asset.locators)
            if (!validate_locator(root, locator, asset.content_hash, maximum))
                return false;
        for (const auto& representation : asset.representations) {
            const bool representation_embedded =
                representation.storage_policy == timeline::AssetStoragePolicy::Embedded;
            if (representation_embedded &&
                !hash_matches(blob_path(root, {BlobStore::Media, representation.content_hash}),
                              representation.content_hash, maximum))
                return false;
            for (const auto& locator : representation.locators)
                if (!validate_locator(root, locator, representation.content_hash, maximum))
                    return false;
        }
    }
    return true;
}

runtime::Result<std::vector<std::uint8_t>, PackageError> read_file(const fs::path& path,
                                                                   std::uint64_t maximum) {
    std::vector<std::uint8_t> bytes;
    const auto outcome = detail::read_file_bounded(path, maximum, bytes);
    if (outcome == detail::NativeReadOutcome::InvalidFile)
        return failure<std::vector<std::uint8_t>>(PackageErrorCode::InvalidLayout, path);
    if (outcome == detail::NativeReadOutcome::LimitExceeded)
        return failure<std::vector<std::uint8_t>>(PackageErrorCode::LimitExceeded, path);
    if (outcome != detail::NativeReadOutcome::Ok)
        return failure<std::vector<std::uint8_t>>(PackageErrorCode::IoError, path);
    return runtime::Result<std::vector<std::uint8_t>, PackageError>(runtime::Ok(std::move(bytes)));
}

struct PackageLock {
#if defined(_WIN32)
    HANDLE handle = INVALID_HANDLE_VALUE;
#else
    int descriptor = -1;
#endif
    PackageLock() = default;
    PackageLock(const PackageLock&) = delete;
    PackageLock& operator=(const PackageLock&) = delete;
    PackageLock(PackageLock&& other) noexcept {
#if defined(_WIN32)
        handle = std::exchange(other.handle, INVALID_HANDLE_VALUE);
#else
        descriptor = std::exchange(other.descriptor, -1);
#endif
    }
    ~PackageLock() {
#if defined(_WIN32)
        if (handle != INVALID_HANDLE_VALUE)
            ::CloseHandle(handle);
#else
        if (descriptor >= 0) {
            ::flock(descriptor, LOCK_UN);
            ::close(descriptor);
        }
#endif
    }
    bool acquire(const fs::path& path) noexcept {
#if defined(_WIN32)
        handle = ::CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_ALWAYS,
                               FILE_ATTRIBUTE_HIDDEN, nullptr);
        return handle != INVALID_HANDLE_VALUE;
#else
        descriptor = ::open(path.c_str(),
                            O_CREAT | O_RDWR | O_CLOEXEC
#ifdef O_NOFOLLOW
                                | O_NOFOLLOW
#endif
                            ,
                            0600);
        return descriptor >= 0 && ::flock(descriptor, LOCK_EX | LOCK_NB) == 0;
#endif
    }
};

runtime::Result<bool, PackageError> ensure_layout(const fs::path& root) {
    constexpr std::string_view directories[] = {"media",    "state",   "artifacts",
                                                "receipts", "journal", "cache"};
    for (const auto name : directories) {
        const auto path = root / name;
        std::error_code error;
        if (!fs::create_directory(path, error) && error)
            return failure<bool>(PackageErrorCode::IoError, path);
        if (!is_real_directory(path))
            return failure<bool>(PackageErrorCode::InvalidLayout, path);
        if (!detail::fence_directory(path))
            return failure<bool>(PackageErrorCode::IoError, path);
    }
    if (!detail::fence_directory(root))
        return failure<bool>(PackageErrorCode::IoError, root);
    return runtime::Result<bool, PackageError>(runtime::Ok(true));
}

runtime::Result<bool, PackageError> recover_staging_files(const fs::path& root) {
    bool recovered = false;
    const fs::path directories[] = {root, root / "media", root / "state", root / "artifacts",
                                    root / "receipts"};
    for (const auto& directory : directories) {
        std::error_code error;
        for (fs::directory_iterator iterator(directory, error), end; !error && iterator != end;
             iterator.increment(error)) {
            const auto filename = iterator->path().filename().string();
            if (!filename.starts_with(kStagePrefix))
                continue;
            const auto status = iterator->symlink_status(error);
            if (error || status.type() != fs::file_type::regular)
                return failure<bool>(PackageErrorCode::InvalidLayout, iterator->path());
            if (!fs::remove(iterator->path(), error) || error)
                return failure<bool>(PackageErrorCode::IoError, iterator->path());
            recovered = true;
        }
        if (error || (recovered && !detail::fence_directory(directory)))
            return failure<bool>(PackageErrorCode::IoError, directory);
    }
    return runtime::Result<bool, PackageError>(runtime::Ok(recovered));
}

} // namespace

struct PackageWriter::Impl {
    fs::path root;
    timeline::SchemaRegistry registry;
    PackageLimits limits;
    PackageLock lock;
    bool recovered_staging = false;
};

PackageWriter::PackageWriter(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}
PackageWriter::~PackageWriter() = default;
PackageWriter::PackageWriter(PackageWriter&&) noexcept = default;
PackageWriter& PackageWriter::operator=(PackageWriter&&) noexcept = default;

runtime::Result<PackageWriter, PackageError>
PackageWriter::create(const fs::path& root, timeline::SchemaRegistry registry,
                      const PackageLimits& limits) noexcept {
    if (root.empty() || limits.max_blob_bytes == 0 || limits.max_project_bytes == 0)
        return failure<PackageWriter>(PackageErrorCode::InvalidPath, root);
    std::error_code error;
    const bool root_exists = fs::exists(root, error);
    if (error)
        return failure<PackageWriter>(PackageErrorCode::IoError, root);
    if (!root_exists) {
        auto publisher_result = AtomicPublisher::create(root);
        if (!publisher_result)
            return failure<PackageWriter>(publisher_result.error().code,
                                          publisher_result.error().path);
        auto publisher = std::move(publisher_result).value();
        constexpr std::string_view markers[] = {"media/.keep",     "state/.keep",
                                                "artifacts/.keep", "receipts/.keep",
                                                "journal/.keep",   "cache/.keep"};
        for (const auto marker : markers) {
            auto written = publisher.write(marker, std::string_view{});
            if (!written)
                return failure<PackageWriter>(written.error().code, written.error().path);
        }
        auto committed = publisher.commit_directory();
        if (!committed)
            return failure<PackageWriter>(committed.error().code, committed.error().path);
        if (*committed == AtomicPublishOutcome::NotPublished)
            return failure<PackageWriter>(PackageErrorCode::PublicationConflict, root);
        if (*committed == AtomicPublishOutcome::PublishedDurabilityUncertain)
            return failure<PackageWriter>(PackageErrorCode::DurabilityUncertain, root);
        for (const auto marker : markers)
            fs::remove(root / path_from_utf8(marker), error);
    }
    if (!is_real_directory(root))
        return failure<PackageWriter>(PackageErrorCode::InvalidLayout, root);
    auto layout = ensure_layout(root);
    if (!layout)
        return failure<PackageWriter>(layout.error().code, layout.error().path);
    auto impl = std::make_unique<Impl>();
    impl->root = fs::weakly_canonical(root, error);
    if (error)
        return failure<PackageWriter>(PackageErrorCode::InvalidPath, root);
    impl->registry = std::move(registry);
    impl->limits = limits;
    if (!impl->lock.acquire(impl->root / kLockFile))
        return failure<PackageWriter>(PackageErrorCode::AlreadyOpen, impl->root);
    auto recovered = recover_staging_files(impl->root);
    if (!recovered)
        return failure<PackageWriter>(recovered.error().code, recovered.error().path);
    impl->recovered_staging = recovered.value();
    return runtime::Result<PackageWriter, PackageError>(
        runtime::Ok(PackageWriter(std::move(impl))));
}

const fs::path& PackageWriter::root() const noexcept {
    static const fs::path empty;
    return impl_ ? impl_->root : empty;
}

bool PackageWriter::recovered_staging() const noexcept {
    return impl_ && impl_->recovered_staging;
}

runtime::Result<BlobReference, PackageError>
PackageWriter::stage_blob(BlobStore store, const timeline::ContentHash& expected,
                          std::span<const std::uint8_t> bytes) noexcept {
    if (!impl_ || !valid_store(store) || !expected.valid())
        return failure<BlobReference>(PackageErrorCode::InvalidPath, {});
    if (bytes.size() > impl_->limits.max_blob_bytes)
        return failure<BlobReference>(PackageErrorCode::LimitExceeded, impl_->root);
    if (runtime::sha256_hex(bytes.data(), bytes.size()) != expected.to_hex())
        return failure<BlobReference>(PackageErrorCode::HashMismatch, impl_->root);
    const BlobReference reference{store, expected};
    const auto destination = blob_path(impl_->root, reference);
    std::error_code error;
    if (fs::exists(destination, error)) {
        auto file = detail::PinnedFile::open(destination, true);
        if (error || !file || !file->hash_matches(expected.to_hex(), impl_->limits.max_blob_bytes))
            return failure<BlobReference>(PackageErrorCode::HashMismatch, destination);
        detail::invoke_fault_hook(detail::PackageFaultPoint::ExistingBlobVerified);
        if (!file->fence() || !file->still_named_by(destination))
            return failure<BlobReference>(PackageErrorCode::DurabilityUncertain, destination);
        if (!detail::fence_directory(destination.parent_path()))
            return failure<BlobReference>(PackageErrorCode::DurabilityUncertain, destination);
        return runtime::Result<BlobReference, PackageError>(runtime::Ok(reference));
    }
    const auto temporary = stage_path(destination.parent_path());
    if (!detail::write_exclusive_and_fence(temporary, bytes,
                                           detail::PackageFaultPoint::StagedFileWritten,
                                           detail::PackageFaultPoint::StagedFileFenced))
        return failure<BlobReference>(PackageErrorCode::IoError, temporary);
    if (!detail::publish_no_replace(temporary, destination)) {
        fs::remove(temporary, error);
        auto file = detail::PinnedFile::open(destination, true);
        if (!file || !file->hash_matches(expected.to_hex(), impl_->limits.max_blob_bytes))
            return failure<BlobReference>(PackageErrorCode::PublicationConflict, destination);
        if (!file->fence() || !file->still_named_by(destination))
            return failure<BlobReference>(PackageErrorCode::DurabilityUncertain, destination);
    }
    detail::invoke_fault_hook(detail::PackageFaultPoint::BlobPublished);
    if (!detail::fence_directory(destination.parent_path()))
        return failure<BlobReference>(PackageErrorCode::DurabilityUncertain, destination);
    detail::invoke_fault_hook(detail::PackageFaultPoint::BlobDirectoryFenced);
    return runtime::Result<BlobReference, PackageError>(runtime::Ok(reference));
}

runtime::Result<AtomicPublishOutcome, PackageError>
PackageWriter::publish(const timeline::Project& project) noexcept {
    if (!impl_)
        return failure<AtomicPublishOutcome>(PackageErrorCode::InvalidLayout, {});
#if defined(PULP_PROJECT_PACKAGE_ENABLE_TEST_MUTATIONS)
    if (!detail::skip_reference_validation_for_test() &&
        !validate_project_references(impl_->root, project, impl_->limits.max_blob_bytes))
#else
    if (!validate_project_references(impl_->root, project, impl_->limits.max_blob_bytes))
#endif
        return failure<AtomicPublishOutcome>(PackageErrorCode::InvalidGeneration, impl_->root);
    const auto output_limit =
        detail::checked_size_limit<std::size_t>(impl_->limits.max_project_bytes);
    if (!output_limit)
        return failure<AtomicPublishOutcome>(PackageErrorCode::LimitExceeded,
                                             impl_->root / kProjectFile);
    auto serialized = timeline::serialize_project(project, impl_->registry,
                                                  timeline::SerializeOptions{*output_limit});
    if (!serialized)
        return failure<AtomicPublishOutcome>(PackageErrorCode::InvalidGeneration,
                                             impl_->root / kProjectFile);
    const auto bytes = std::span<const std::uint8_t>(
        reinterpret_cast<const std::uint8_t*>(serialized->json.data()), serialized->json.size());
    const auto destination = impl_->root / kProjectFile;
    const auto temporary = stage_path(impl_->root);
    if (!detail::write_exclusive_and_fence(temporary, bytes,
                                           detail::PackageFaultPoint::GenerationWritten,
                                           detail::PackageFaultPoint::GenerationFenced))
        return failure<AtomicPublishOutcome>(PackageErrorCode::IoError, temporary);
    if (!detail::replace_path(temporary, destination)) {
        std::error_code ignored;
        fs::remove(temporary, ignored);
        return runtime::Result<AtomicPublishOutcome, PackageError>(
            runtime::Ok(AtomicPublishOutcome::NotPublished));
    }
    detail::invoke_fault_hook(detail::PackageFaultPoint::GenerationPublished);
    if (!detail::fence_directory(impl_->root))
        return runtime::Result<AtomicPublishOutcome, PackageError>(
            runtime::Ok(AtomicPublishOutcome::PublishedDurabilityUncertain));
    detail::invoke_fault_hook(detail::PackageFaultPoint::GenerationDirectoryFenced);
    return runtime::Result<AtomicPublishOutcome, PackageError>(
        runtime::Ok(AtomicPublishOutcome::PublishedDurably));
}

runtime::Result<OpenPackageResult, PackageError>
open_package(const fs::path& root, const timeline::SchemaRegistry& registry,
             const PackageLimits& limits) noexcept {
    if (!is_real_directory(root))
        return failure<OpenPackageResult>(PackageErrorCode::InvalidLayout, root);
    constexpr std::string_view required[] = {"media", "state", "artifacts", "receipts", "journal"};
    for (const auto directory : required)
        if (!is_real_directory(root / directory))
            return failure<OpenPackageResult>(PackageErrorCode::InvalidLayout, root / directory);
    bool cache_recreated = false;
    const auto cache = root / "cache";
    if (!is_real_directory(cache)) {
        std::error_code error;
        const bool created = fs::create_directory(cache, error);
        if ((!created && error) || !is_real_directory(cache) ||
            (created && !detail::fence_directory(root)))
            return failure<OpenPackageResult>(PackageErrorCode::IoError, cache);
        cache_recreated = created;
    }
    auto encoded = read_file(root / kProjectFile, limits.max_project_bytes);
    if (!encoded)
        return failure<OpenPackageResult>(encoded.error().code, encoded.error().path);
    const std::string_view json(reinterpret_cast<const char*>(encoded->data()), encoded->size());
    auto decoded = timeline::deserialize_project(json, registry);
    if (!decoded || !validate_project_references(root, decoded.value(), limits.max_blob_bytes))
        return failure<OpenPackageResult>(PackageErrorCode::InvalidGeneration, root / kProjectFile);
    return runtime::Result<OpenPackageResult, PackageError>(runtime::Ok(
        OpenPackageResult{std::move(decoded).value(), root / "journal", cache, cache_recreated}));
}

runtime::Result<std::vector<std::uint8_t>, PackageError>
read_blob(const fs::path& root, const BlobReference& reference,
          std::uint64_t maximum_bytes) noexcept {
    if (!valid_store(reference.store) || !reference.hash.valid())
        return failure<std::vector<std::uint8_t>>(PackageErrorCode::InvalidPath, root);
    if (!is_real_directory(root) || !is_real_directory(root / store_name(reference.store)))
        return failure<std::vector<std::uint8_t>>(PackageErrorCode::InvalidLayout, root);
    const auto path = blob_path(root, reference);
    auto bytes = read_file(path, maximum_bytes);
    if (!bytes)
        return bytes;
    if (runtime::sha256_hex(bytes->data(), bytes->size()) != reference.hash.to_hex())
        return failure<std::vector<std::uint8_t>>(PackageErrorCode::HashMismatch, path);
    return bytes;
}

} // namespace pulp::project_package
