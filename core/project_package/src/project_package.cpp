#include <pulp/project_package/project_package.hpp>

#include "native_io.hpp"
#include "project_package_test_access.hpp"

#include <pulp/runtime/crypto.hpp>
#include <pulp/timeline/asset_path.hpp>
#include <pulp/timeline/serialize.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <set>
#include <string_view>
#include <system_error>
#include <utility>

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

bool path_has_embedded_nul(const fs::path& path) noexcept {
    const auto& native = path.native();
    return std::find(native.begin(), native.end(), fs::path::value_type{}) != native.end();
}

fs::path blob_path(const fs::path& root, const BlobReference& reference) {
    auto name = reference.hash.to_hex();
    if (reference.store == BlobStore::Receipt)
        name += ".json";
    return root / store_name(reference.store) / name;
}

fs::path blob_name(const BlobReference& reference) {
    auto name = reference.hash.to_hex();
    if (reference.store == BlobStore::Receipt)
        name += ".json";
    return path_from_utf8(name);
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

bool hash_matches(const detail::AnchoredDirectory& parent, const fs::path& name,
                  const timeline::ContentHash& expected, std::uint64_t maximum) {
    if (!expected.valid())
        return false;
    auto file = detail::PinnedFile::open(parent, name, false, true);
    if (!file || !file->hash_matches(expected.to_hex(), maximum))
        return false;
    detail::invoke_fault_hook(detail::PackageFaultPoint::BlobReferenceVerified);
    return file->still_named_by(parent, name);
}

std::uint64_t next_serial() noexcept {
    static std::atomic<std::uint64_t> serial{0};
    return static_cast<std::uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count()) ^
           serial.fetch_add(1, std::memory_order_relaxed);
}

fs::path stage_path(const fs::path& directory) {
    return directory / (std::string(kStagePrefix) + std::to_string(next_serial()));
}

bool validate_canonical_blob(const fs::path& root, const timeline::ContentHash& expected,
                             std::uint64_t maximum, std::set<timeline::ContentHash>& verified) {
    if (verified.contains(expected))
        return true;
    if (!hash_matches(blob_path(root, {BlobStore::Media, expected}), expected, maximum))
        return false;
    verified.insert(expected);
    return true;
}

bool validate_locator(const fs::path& root, const timeline::AssetLocator& locator,
                      const timeline::ContentHash& expected, std::uint64_t maximum,
                      std::set<timeline::ContentHash>& verified) {
    if (locator.kind != timeline::AssetLocatorKind::PackageRelative)
        return true;
    if (!timeline::package_relative_path_is_portable(locator.hint))
        return false;
    const auto canonical = std::string("media/") + expected.to_hex();
    return locator.hint == canonical && validate_canonical_blob(root, expected, maximum, verified);
}

bool validate_project_references(const fs::path& root, const timeline::Project& project,
                                 std::uint64_t maximum) {
    std::set<timeline::ContentHash> verified;
    for (const auto& asset : project.assets()) {
        const bool embedded = asset.storage_policy == timeline::AssetStoragePolicy::Embedded;
        if (embedded && !validate_canonical_blob(root, asset.content_hash, maximum, verified))
            return false;
        for (const auto& locator : asset.locators)
            if (!validate_locator(root, locator, asset.content_hash, maximum, verified))
                return false;
        for (const auto& representation : asset.representations) {
            const bool representation_embedded =
                representation.storage_policy == timeline::AssetStoragePolicy::Embedded;
            if (representation_embedded &&
                !validate_canonical_blob(root, representation.content_hash, maximum, verified))
                return false;
            for (const auto& locator : representation.locators)
                if (!validate_locator(root, locator, representation.content_hash, maximum,
                                      verified))
                    return false;
        }
    }
    return true;
}

bool validate_canonical_blob(const detail::AnchoredDirectory& media,
                             const timeline::ContentHash& expected, std::uint64_t maximum,
                             std::set<timeline::ContentHash>& verified) {
    if (verified.contains(expected))
        return true;
    const auto name = path_from_utf8(expected.to_hex());
    if (!hash_matches(media, name, expected, maximum))
        return false;
    verified.insert(expected);
    return true;
}

bool validate_locator(const detail::AnchoredDirectory& media, const timeline::AssetLocator& locator,
                      const timeline::ContentHash& expected, std::uint64_t maximum,
                      std::set<timeline::ContentHash>& verified) {
    if (locator.kind != timeline::AssetLocatorKind::PackageRelative)
        return true;
    if (!timeline::package_relative_path_is_portable(locator.hint))
        return false;
    const auto canonical = std::string("media/") + expected.to_hex();
    return locator.hint == canonical && validate_canonical_blob(media, expected, maximum, verified);
}

bool validate_project_references(const detail::AnchoredDirectory& media,
                                 const timeline::Project& project, std::uint64_t maximum) {
    std::set<timeline::ContentHash> verified;
    for (const auto& asset : project.assets()) {
        const bool embedded = asset.storage_policy == timeline::AssetStoragePolicy::Embedded;
        if (embedded && !validate_canonical_blob(media, asset.content_hash, maximum, verified))
            return false;
        for (const auto& locator : asset.locators)
            if (!validate_locator(media, locator, asset.content_hash, maximum, verified))
                return false;
        for (const auto& representation : asset.representations) {
            const bool representation_embedded =
                representation.storage_policy == timeline::AssetStoragePolicy::Embedded;
            if (representation_embedded &&
                !validate_canonical_blob(media, representation.content_hash, maximum, verified))
                return false;
            for (const auto& locator : representation.locators)
                if (!validate_locator(media, locator, representation.content_hash, maximum,
                                      verified))
                    return false;
        }
    }
    return true;
}

runtime::Result<std::vector<std::uint8_t>, PackageError>
read_file(const detail::AnchoredDirectory& parent, const fs::path& relative,
          const fs::path& display_path, std::uint64_t maximum) {
    auto file = detail::PinnedFile::open(parent, relative, false, true);
    if (!file)
        return failure<std::vector<std::uint8_t>>(PackageErrorCode::IoError, display_path);
    std::vector<std::uint8_t> bytes;
    const auto outcome = file->read_bounded(maximum, bytes);
    if (outcome == detail::NativeReadOutcome::InvalidFile)
        return failure<std::vector<std::uint8_t>>(PackageErrorCode::InvalidLayout, display_path);
    if (outcome == detail::NativeReadOutcome::LimitExceeded)
        return failure<std::vector<std::uint8_t>>(PackageErrorCode::LimitExceeded, display_path);
    if (outcome != detail::NativeReadOutcome::Ok)
        return failure<std::vector<std::uint8_t>>(PackageErrorCode::IoError, display_path);
    return runtime::Result<std::vector<std::uint8_t>, PackageError>(runtime::Ok(std::move(bytes)));
}

struct PackageLock {
    std::optional<detail::PinnedFile> file;

    bool acquire(const detail::AnchoredDirectory& root) noexcept {
        file = detail::PinnedFile::acquire_lock(root, path_from_utf8(kLockFile));
        return file.has_value();
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
    detail::AnchoredDirectory root_anchor;
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
    if (root.empty() || path_has_embedded_nul(root) || limits.max_blob_bytes == 0 ||
        limits.max_project_bytes == 0)
        return failure<PackageWriter>(PackageErrorCode::InvalidPath, root);
    std::error_code error;
    const auto anchored_root = fs::absolute(root, error).lexically_normal();
    if (error)
        return failure<PackageWriter>(PackageErrorCode::InvalidPath, root);
    const bool root_exists = fs::exists(anchored_root, error);
    if (error)
        return failure<PackageWriter>(PackageErrorCode::IoError, anchored_root);
    if (!root_exists) {
        auto publisher_result = AtomicPublisher::create(anchored_root);
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
            return failure<PackageWriter>(PackageErrorCode::PublicationConflict, anchored_root);
        if (*committed == AtomicPublishOutcome::PublishedDurabilityUncertain)
            return failure<PackageWriter>(PackageErrorCode::DurabilityUncertain, anchored_root);
        for (const auto marker : markers)
            fs::remove(anchored_root / path_from_utf8(marker), error);
    }
    if (!is_real_directory(anchored_root))
        return failure<PackageWriter>(PackageErrorCode::InvalidLayout, anchored_root);
    const auto canonical_root = fs::canonical(anchored_root, error);
    if (error)
        return failure<PackageWriter>(PackageErrorCode::InvalidPath, anchored_root);
    auto root_anchor = detail::AnchoredDirectory::open(canonical_root);
    if (!root_anchor || !root_anchor->still_named_by(canonical_root))
        return failure<PackageWriter>(PackageErrorCode::InvalidLayout, canonical_root);
    auto layout = ensure_layout(canonical_root);
    if (!layout)
        return failure<PackageWriter>(layout.error().code, layout.error().path);
    if (!root_anchor->still_named_by(canonical_root))
        return failure<PackageWriter>(PackageErrorCode::InvalidLayout, canonical_root);
    auto impl = std::make_unique<Impl>();
    impl->root = canonical_root;
    impl->registry = std::move(registry);
    impl->limits = limits;
    impl->root_anchor = std::move(*root_anchor);
    if (!impl->lock.acquire(impl->root_anchor))
        return failure<PackageWriter>(PackageErrorCode::AlreadyOpen, impl->root);
    if (!impl->root_anchor.still_named_by(impl->root))
        return failure<PackageWriter>(PackageErrorCode::InvalidLayout, impl->root);
    auto recovered = recover_staging_files(impl->root);
    if (!recovered)
        return failure<PackageWriter>(recovered.error().code, recovered.error().path);
    if (!impl->root_anchor.still_named_by(impl->root))
        return failure<PackageWriter>(PackageErrorCode::InvalidLayout, impl->root);
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
    if (!impl_->root_anchor.still_named_by(impl_->root))
        return failure<BlobReference>(PackageErrorCode::InvalidLayout, impl_->root);
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
        if (!impl_->root_anchor.still_named_by(impl_->root) || !file->fence() ||
            !file->still_named_by(destination))
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
    auto destination_parent = impl_->root_anchor.open_directory(path_from_utf8(store_name(store)));
    auto staged_file = detail::PinnedFile::open(temporary, true, true, true);
    if (!destination_parent || !staged_file || !staged_file->still_named_by(temporary)) {
        fs::remove(temporary, error);
        return failure<BlobReference>(PackageErrorCode::IoError, temporary);
    }
    if (!impl_->root_anchor.still_named_by(impl_->root))
        return failure<BlobReference>(PackageErrorCode::InvalidLayout, impl_->root);
    const auto publication = detail::publish_no_replace(temporary, destination,
                                                        detail::NoReplaceSourceKind::RegularFile);
    if (publication == detail::NoReplaceOutcome::PublishedSourceRetained) {
        error.clear();
        if (!fs::remove(temporary, error) || error)
            return failure<BlobReference>(PackageErrorCode::DurabilityUncertain, destination);
        auto file = detail::PinnedFile::open(destination, true);
        if (!file || !file->hash_matches(expected.to_hex(), impl_->limits.max_blob_bytes) ||
            !file->fence() || !file->still_named_by(destination))
            return failure<BlobReference>(PackageErrorCode::DurabilityUncertain, destination);
    } else if (publication != detail::NoReplaceOutcome::Published) {
        if (impl_->root_anchor.still_named_by(impl_->root))
            fs::remove(temporary, error);
        if (publication != detail::NoReplaceOutcome::DestinationExists)
            return failure<BlobReference>(PackageErrorCode::IoError, destination);
        auto file = detail::PinnedFile::open(destination, true);
        if (!file || !file->hash_matches(expected.to_hex(), impl_->limits.max_blob_bytes))
            return failure<BlobReference>(PackageErrorCode::PublicationConflict, destination);
        if (!file->fence() || !file->still_named_by(destination))
            return failure<BlobReference>(PackageErrorCode::DurabilityUncertain, destination);
    }
    if ((publication == detail::NoReplaceOutcome::Published ||
         publication == detail::NoReplaceOutcome::PublishedSourceRetained) &&
        (!staged_file->adopt_inherited_permissions_from(*destination_parent) ||
         !staged_file->fence() || !staged_file->still_named_by(destination)))
        return failure<BlobReference>(PackageErrorCode::DurabilityUncertain, destination);
    detail::invoke_fault_hook(detail::PackageFaultPoint::BlobPublished);
    if (!impl_->root_anchor.still_named_by(impl_->root) ||
        !detail::fence_directory(destination.parent_path()))
        return failure<BlobReference>(PackageErrorCode::DurabilityUncertain, destination);
    detail::invoke_fault_hook(detail::PackageFaultPoint::BlobDirectoryFenced);
    return runtime::Result<BlobReference, PackageError>(runtime::Ok(reference));
}

runtime::Result<AtomicPublishOutcome, PackageError>
PackageWriter::publish(const timeline::Project& project) noexcept {
    if (!impl_)
        return failure<AtomicPublishOutcome>(PackageErrorCode::InvalidLayout, {});
    if (!impl_->root_anchor.still_named_by(impl_->root))
        return failure<AtomicPublishOutcome>(PackageErrorCode::InvalidLayout, impl_->root);
    bool validate_references = true;
#if defined(PULP_PROJECT_PACKAGE_ENABLE_TEST_MUTATIONS)
    validate_references = !detail::skip_reference_validation_for_test();
    if (validate_references &&
        !validate_project_references(impl_->root, project, impl_->limits.max_blob_bytes))
#else
    if (!validate_project_references(impl_->root, project, impl_->limits.max_blob_bytes))
#endif
        return failure<AtomicPublishOutcome>(PackageErrorCode::InvalidGeneration, impl_->root);
    detail::invoke_fault_hook(detail::PackageFaultPoint::ReferenceSetVerified);
    const auto references_still_named = [&]() {
        if (!validate_references)
            return true;
        return impl_->root_anchor.still_named_by(impl_->root) &&
               validate_project_references(impl_->root, project, impl_->limits.max_blob_bytes);
    };
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
    auto staged_file = detail::PinnedFile::open(temporary, true, true, true);
    if (!staged_file || !staged_file->still_named_by(temporary)) {
        std::error_code ignored;
        fs::remove(temporary, ignored);
        return failure<AtomicPublishOutcome>(PackageErrorCode::IoError, temporary);
    }
    if (!references_still_named()) {
        std::error_code ignored;
        fs::remove(temporary, ignored);
        return failure<AtomicPublishOutcome>(PackageErrorCode::InvalidGeneration, impl_->root);
    }
    if (!detail::replace_path(temporary, destination)) {
        std::error_code ignored;
        fs::remove(temporary, ignored);
        return runtime::Result<AtomicPublishOutcome, PackageError>(
            runtime::Ok(AtomicPublishOutcome::NotPublished));
    }
    detail::invoke_fault_hook(detail::PackageFaultPoint::GenerationPublished);
    if (!staged_file->adopt_inherited_permissions_from(impl_->root_anchor) ||
        !staged_file->fence() || !staged_file->still_named_by(destination))
        return runtime::Result<AtomicPublishOutcome, PackageError>(
            runtime::Ok(AtomicPublishOutcome::PublishedDurabilityUncertain));
    if (!impl_->root_anchor.still_named_by(impl_->root) || !detail::fence_directory(impl_->root))
        return runtime::Result<AtomicPublishOutcome, PackageError>(
            runtime::Ok(AtomicPublishOutcome::PublishedDurabilityUncertain));
    detail::invoke_fault_hook(detail::PackageFaultPoint::GenerationDirectoryFenced);
    return runtime::Result<AtomicPublishOutcome, PackageError>(
        runtime::Ok(AtomicPublishOutcome::PublishedDurably));
}

runtime::Result<OpenPackageResult, PackageError>
open_package(const fs::path& root, const timeline::SchemaRegistry& registry,
             const PackageLimits& limits) noexcept {
    if (root.empty() || path_has_embedded_nul(root))
        return failure<OpenPackageResult>(PackageErrorCode::InvalidPath, root);
    std::error_code error;
    const auto anchored_root = fs::absolute(root, error).lexically_normal();
    if (error)
        return failure<OpenPackageResult>(PackageErrorCode::InvalidPath, root);
    if (!is_real_directory(anchored_root))
        return failure<OpenPackageResult>(PackageErrorCode::InvalidLayout, root);
    const auto canonical_root = fs::canonical(anchored_root, error);
    if (error || !is_real_directory(canonical_root))
        return failure<OpenPackageResult>(PackageErrorCode::InvalidPath, root);
    auto root_anchor = detail::AnchoredDirectory::open(canonical_root, true);
    if (!root_anchor || !root_anchor->still_named_by(canonical_root))
        return failure<OpenPackageResult>(PackageErrorCode::InvalidLayout, canonical_root);
    detail::invoke_fault_hook(detail::PackageFaultPoint::ReaderRootAnchored);
    constexpr std::string_view required[] = {"media", "state", "artifacts", "receipts", "journal"};
    std::vector<std::pair<fs::path, detail::AnchoredDirectory>> required_anchors;
    required_anchors.reserve(std::size(required));
    for (const auto directory : required) {
        const auto relative = path_from_utf8(directory);
        auto anchor = root_anchor->open_directory(relative, true, false, false);
        if (!anchor)
            return failure<OpenPackageResult>(PackageErrorCode::InvalidLayout,
                                              canonical_root / directory);
        required_anchors.emplace_back(relative, std::move(*anchor));
    }
    const auto media = std::find_if(required_anchors.begin(), required_anchors.end(),
                                    [](const auto& entry) { return entry.first == "media"; });
    if (media == required_anchors.end())
        return failure<OpenPackageResult>(PackageErrorCode::InvalidLayout,
                                          canonical_root / "media");
    bool cache_recreated = false;
    const auto cache = canonical_root / "cache";
    auto cache_anchor =
        root_anchor->open_or_create_directory(path_from_utf8("cache"), cache_recreated);
    if (!cache_anchor || (cache_recreated && !root_anchor->fence()))
        return failure<OpenPackageResult>(PackageErrorCode::IoError, cache);
    auto encoded = read_file(*root_anchor, path_from_utf8(kProjectFile),
                             canonical_root / kProjectFile, limits.max_project_bytes);
    if (!encoded)
        return failure<OpenPackageResult>(encoded.error().code, encoded.error().path);
    const std::string_view json(reinterpret_cast<const char*>(encoded->data()), encoded->size());
    auto decoded = timeline::deserialize_project(json, registry);
    if (!decoded ||
        !validate_project_references(media->second, decoded.value(), limits.max_blob_bytes))
        return failure<OpenPackageResult>(PackageErrorCode::InvalidGeneration,
                                          canonical_root / kProjectFile);
    if (!root_anchor->still_named_by(canonical_root) || !cache_anchor->still_named_by(cache))
        return failure<OpenPackageResult>(PackageErrorCode::InvalidLayout, canonical_root);
    for (const auto& [relative, anchor] : required_anchors)
        if (!anchor.still_named_by(canonical_root / relative))
            return failure<OpenPackageResult>(PackageErrorCode::InvalidLayout,
                                              canonical_root / relative);
    return runtime::Result<OpenPackageResult, PackageError>(runtime::Ok(OpenPackageResult{
        std::move(decoded).value(), canonical_root / "journal", cache, cache_recreated}));
}

runtime::Result<std::vector<std::uint8_t>, PackageError>
read_blob(const fs::path& root, const BlobReference& reference,
          std::uint64_t maximum_bytes) noexcept {
    if (root.empty() || path_has_embedded_nul(root) || !valid_store(reference.store) ||
        !reference.hash.valid())
        return failure<std::vector<std::uint8_t>>(PackageErrorCode::InvalidPath, root);
    std::error_code error;
    const auto anchored_root = fs::absolute(root, error).lexically_normal();
    if (error)
        return failure<std::vector<std::uint8_t>>(PackageErrorCode::InvalidPath, root);
    if (!is_real_directory(anchored_root))
        return failure<std::vector<std::uint8_t>>(PackageErrorCode::InvalidLayout, anchored_root);
    const auto canonical_root = fs::canonical(anchored_root, error);
    if (error)
        return failure<std::vector<std::uint8_t>>(PackageErrorCode::InvalidLayout, anchored_root);
    auto root_anchor = detail::AnchoredDirectory::open(canonical_root, true);
    if (!root_anchor || !root_anchor->still_named_by(canonical_root))
        return failure<std::vector<std::uint8_t>>(PackageErrorCode::InvalidLayout, anchored_root);
    detail::invoke_fault_hook(detail::PackageFaultPoint::ReaderRootAnchored);
    const auto store_relative = path_from_utf8(store_name(reference.store));
    auto store_anchor = root_anchor->open_directory(store_relative, true, false, false);
    if (!store_anchor)
        return failure<std::vector<std::uint8_t>>(PackageErrorCode::InvalidLayout,
                                                  canonical_root / store_relative);
    const auto path = blob_path(canonical_root, reference);
    auto bytes = read_file(*store_anchor, blob_name(reference), path, maximum_bytes);
    if (!bytes)
        return bytes;
    if (runtime::sha256_hex(bytes->data(), bytes->size()) != reference.hash.to_hex())
        return failure<std::vector<std::uint8_t>>(PackageErrorCode::HashMismatch, path);
    if (!root_anchor->still_named_by(canonical_root) ||
        !store_anchor->still_named_by(canonical_root / store_relative))
        return failure<std::vector<std::uint8_t>>(PackageErrorCode::InvalidLayout, anchored_root);
    return bytes;
}

} // namespace pulp::project_package
