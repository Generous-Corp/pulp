#include <pulp/inspect/control_trusted_host_inventory.hpp>

#include "control_private_store.hpp"
#include "control_static_code_identity.hpp"

#include <pulp/runtime/crypto.hpp>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <map>
#include <mutex>
#include <system_error>

#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif

#ifndef _WIN32
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace pulp::inspect {
namespace fs = std::filesystem;
namespace {

constexpr bool platform_supported() {
#if defined(__APPLE__) && TARGET_OS_OSX && !TARGET_OS_IPHONE
    return true;
#else
    return false;
#endif
}

std::optional<std::string> random_id(std::string_view prefix) {
    const auto bytes = runtime::secure_random_bytes(16);
    if (!bytes || bytes->size() != 16)
        return std::nullopt;
    constexpr char digits[] = "0123456789abcdef";
    std::string result(prefix);
    result.reserve(prefix.size() + bytes->size() * 2);
    for (const auto byte : *bytes) {
        result.push_back(digits[byte >> 4]);
        result.push_back(digits[byte & 0xf]);
    }
    return result;
}

bool forbidden_artifact_path(const fs::path& path) {
    static constexpr std::string_view extensions[] = {".app",  ".appex", ".component", ".vst3",
                                                      ".clap", ".lv2",   ".aaxplugin"};
    for (const auto& component : path) {
        auto extension = component.extension().string();
        std::transform(extension.begin(), extension.end(), extension.begin(), [](char value) {
            return static_cast<char>(std::tolower(static_cast<unsigned char>(value)));
        });
        if (std::find(std::begin(extensions), std::end(extensions), extension) !=
            std::end(extensions))
            return true;
    }
    return false;
}

bool path_has_nul(const fs::path& path) {
    return path.string().find('\0') != std::string::npos;
}

bool no_symlink_components(const fs::path& path) {
    if (!path.is_absolute())
        return false;
    fs::path current = path.root_path();
    for (const auto& component : path.relative_path()) {
        current /= component;
        std::error_code error;
        const auto status = fs::symlink_status(current, error);
        if (error || fs::is_symlink(status))
            return false;
    }
    return true;
}

bool private_directory(const fs::path& path) {
#ifdef _WIN32
    return detail::ensure_owner_private_directory(path);
#else
    struct stat status{};
    return ::lstat(path.c_str(), &status) == 0 && S_ISDIR(status.st_mode) &&
           !S_ISLNK(status.st_mode) && status.st_uid == ::geteuid() && (status.st_mode & 077) == 0;
#endif
}

bool prepare_private_directory(const fs::path& path) {
    std::error_code error;
    const bool exists = fs::exists(path, error);
    if (error)
        return false;
    if (exists)
        return no_symlink_components(path) && private_directory(path);
    if (!no_symlink_components(path.parent_path()))
        return false;
    return detail::ensure_owner_private_directory(path) && no_symlink_components(path) &&
           private_directory(path);
}

bool private_snapshot_file(const fs::path& path, bool executable) {
#ifdef _WIN32
    (void)path;
    (void)executable;
    return true;
#else
    struct stat status{};
    if (::lstat(path.c_str(), &status) != 0 || !S_ISREG(status.st_mode) ||
        S_ISLNK(status.st_mode) || status.st_uid != ::geteuid() || status.st_nlink != 1 ||
        (status.st_mode & 077) != 0)
        return false;
    return !executable || (status.st_mode & 0100) != 0;
#endif
}

struct SourcePair {
    std::vector<std::uint8_t> executable;
    std::vector<std::uint8_t> manifest;
    fs::path sidecar;
};

#ifdef _WIN32
std::optional<SourcePair> read_source_pair(const fs::path&, std::size_t, std::size_t) {
    return std::nullopt;
}
#else
int open_directory_without_symlinks(const fs::path& path) {
    if (!path.is_absolute() || path_has_nul(path))
        return -1;
    int flags = O_RDONLY | O_CLOEXEC | O_NOFOLLOW;
#ifdef O_DIRECTORY
    flags |= O_DIRECTORY;
#endif
    int descriptor = ::open(path.root_path().c_str(), flags);
    if (descriptor < 0)
        return -1;
    for (const auto& component : path.relative_path()) {
        if (component.empty() || component == "." || component == "..") {
            ::close(descriptor);
            return -1;
        }
        const int next = ::openat(descriptor, component.c_str(), flags);
        ::close(descriptor);
        if (next < 0)
            return -1;
        descriptor = next;
    }
    return descriptor;
}

std::optional<std::vector<std::uint8_t>> read_source_file_at(int parent, const fs::path& filename,
                                                             std::size_t maximum_bytes,
                                                             bool require_executable) {
    if (filename.empty() || filename == "." || filename == ".." || path_has_nul(filename))
        return std::nullopt;
    int flags = O_RDONLY | O_CLOEXEC | O_NOFOLLOW;
#ifdef O_NONBLOCK
    flags |= O_NONBLOCK;
#endif
    const int descriptor = ::openat(parent, filename.c_str(), flags);
    if (descriptor < 0)
        return std::nullopt;
    struct stat before{};
    if (::fstat(descriptor, &before) != 0 || !S_ISREG(before.st_mode) ||
        before.st_uid != ::geteuid() || before.st_nlink != 1 || before.st_size <= 0 ||
        static_cast<std::uint64_t>(before.st_size) > maximum_bytes ||
        (require_executable && (before.st_mode & 0111) == 0)) {
        ::close(descriptor);
        return std::nullopt;
    }
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(before.st_size));
    std::size_t offset = 0;
    while (offset < bytes.size()) {
        const auto count = ::read(descriptor, bytes.data() + offset, bytes.size() - offset);
        if (count < 0 && errno == EINTR)
            continue;
        if (count <= 0) {
            ::close(descriptor);
            return std::nullopt;
        }
        offset += static_cast<std::size_t>(count);
    }
    struct stat after{};
    const bool stable = ::fstat(descriptor, &after) == 0 && before.st_dev == after.st_dev &&
                        before.st_ino == after.st_ino && before.st_uid == after.st_uid &&
                        before.st_mode == after.st_mode && before.st_size == after.st_size &&
                        before.st_nlink == after.st_nlink;
    ::close(descriptor);
    if (!stable)
        return std::nullopt;
    return bytes;
}

std::optional<SourcePair> read_source_pair(const fs::path& executable,
                                           std::size_t maximum_executable_bytes,
                                           std::size_t maximum_manifest_bytes) {
    const auto sidecar = executable.parent_path() /
                         (executable.filename().string() + ".inspector-capabilities.json");
    const int parent = open_directory_without_symlinks(executable.parent_path());
    if (parent < 0)
        return std::nullopt;
    auto executable_bytes =
        read_source_file_at(parent, executable.filename(), maximum_executable_bytes, true);
    auto manifest_bytes =
        read_source_file_at(parent, sidecar.filename(), maximum_manifest_bytes, false);
    ::close(parent);
    if (!executable_bytes || !manifest_bytes)
        return std::nullopt;
    return SourcePair{std::move(*executable_bytes), std::move(*manifest_bytes), sidecar};
}
#endif

bool valid_launch_intent(const ControlTrustedHostLaunchIntent& intent) {
    const bool supported_tier = intent.host_tier == ControlHostTier::OfflineJob ||
                                intent.host_tier == ControlHostTier::Standalone;
    if (!intent.executable.is_absolute() || path_has_nul(intent.executable) ||
        forbidden_artifact_path(intent.executable) || !supported_tier ||
        !intent.working_directory.is_absolute() || path_has_nul(intent.working_directory) ||
        !no_symlink_components(intent.working_directory))
        return false;
    std::error_code error;
    if (!fs::is_directory(intent.working_directory, error) || error ||
        intent.arguments.size() > 128)
        return false;
    std::size_t argument_bytes = 0;
    for (const auto& argument : intent.arguments) {
        argument_bytes += argument.size();
        if (argument.find('\0') != std::string::npos || argument_bytes > 64u * 1024u)
            return false;
    }
    return true;
}

ControlArtifactExpectation artifact_expectation(const ControlManifest& manifest) {
    ControlArtifactExpectation expectation;
    expectation.profile_id = std::string(control_profile_id(manifest.profile));
    expectation.manifest_digest = control_manifest_digest(manifest);
    expectation.endpoint_included = manifest.endpoint_included;
    expectation.runtime_eval_included = manifest.unsafe_runtime_eval_acknowledged;
    for (const auto capability : manifest.capabilities)
        expectation.capability_ids.emplace_back(capability_id(capability));
    return expectation;
}

void remove_snapshot_pair(const fs::path& directory, const fs::path& executable,
                          const fs::path& manifest) {
    std::error_code error;
    fs::remove(executable, error);
    error.clear();
    fs::remove(manifest, error);
    error.clear();
    fs::remove(directory, error);
}

} // namespace

struct ControlTrustedHostSnapshot::Impl {
    fs::path directory;
    fs::path executable;
    fs::path manifest;
    std::vector<std::string> arguments;
    fs::path working_directory;
    ControlRegistrationRequest registration;
    ControlTrustedHostStaticExpectation static_expectation;
    std::uint64_t generation = 0;
    std::chrono::steady_clock::time_point expiry;

    ~Impl() {
        remove_snapshot_pair(directory, executable, manifest);
    }
};

ControlTrustedHostSnapshot::ControlTrustedHostSnapshot(std::unique_ptr<Impl> impl)
    : impl_(std::move(impl)) {}
ControlTrustedHostSnapshot::ControlTrustedHostSnapshot(ControlTrustedHostSnapshot&&) noexcept =
    default;
ControlTrustedHostSnapshot&
ControlTrustedHostSnapshot::operator=(ControlTrustedHostSnapshot&&) noexcept = default;
ControlTrustedHostSnapshot::~ControlTrustedHostSnapshot() = default;
const fs::path& ControlTrustedHostSnapshot::executable() const {
    return impl_->executable;
}
const std::vector<std::string>& ControlTrustedHostSnapshot::arguments() const {
    return impl_->arguments;
}
const fs::path& ControlTrustedHostSnapshot::working_directory() const {
    return impl_->working_directory;
}
const ControlRegistrationRequest& ControlTrustedHostSnapshot::registration() const {
    return impl_->registration;
}
const ControlTrustedHostStaticExpectation& ControlTrustedHostSnapshot::static_expectation() const {
    return impl_->static_expectation;
}
std::uint64_t ControlTrustedHostSnapshot::broker_generation() const {
    return impl_->generation;
}
std::chrono::steady_clock::time_point ControlTrustedHostSnapshot::expires_at() const {
    return impl_->expiry;
}

struct ControlTrustedHostInventory::Impl {
    struct Entry {
        std::unique_ptr<ControlTrustedHostSnapshot::Impl> snapshot;
    };

    ControlTrustedHostInventoryConfig config;
    Clock clock;
    fs::path generation_root;
    mutable std::mutex mutex;
    std::map<std::string, Entry, std::less<>> entries;
    std::size_t preparing = 0;
    bool ready = false;

    Impl(ControlTrustedHostInventoryConfig next_config, Clock next_clock)
        : config(std::move(next_config)), clock(std::move(next_clock)) {
        if (config.staging_root.is_absolute() && !path_has_nul(config.staging_root) &&
            config.broker_generation != 0 && config.maximum_entries != 0 &&
            config.maximum_executable_bytes != 0 && config.maximum_manifest_bytes != 0 &&
            config.ttl.count() > 0 && clock && prepare_private_directory(config.staging_root)) {
            generation_root =
                config.staging_root / ("generation-" + std::to_string(config.broker_generation));
            ready = prepare_private_directory(generation_root);
        }
    }

    bool reserve_preparation() {
        std::lock_guard lock(mutex);
        if (entries.size() + preparing >= config.maximum_entries)
            return false;
        ++preparing;
        return true;
    }

    void release_preparation() {
        std::lock_guard lock(mutex);
        if (preparing != 0)
            --preparing;
    }

    bool commit_preparation(std::string inventory_id,
                            std::unique_ptr<ControlTrustedHostSnapshot::Impl> snapshot) {
        std::lock_guard lock(mutex);
        if (preparing == 0)
            return false;
        --preparing;
        return entries.emplace(std::move(inventory_id), Entry{std::move(snapshot)}).second;
    }
};

ControlTrustedHostInventory::ControlTrustedHostInventory(ControlTrustedHostInventoryConfig config,
                                                         Clock clock)
    : impl_(std::make_unique<Impl>(std::move(config), std::move(clock))) {}
ControlTrustedHostInventory::~ControlTrustedHostInventory() = default;

ControlTrustedHostInventoryPrepareResult
ControlTrustedHostInventory::prepare(const ControlTrustedHostLaunchIntent& intent) {
    if (!platform_supported())
        return {ControlTrustedHostInventoryStatus::PlatformUnavailable, std::nullopt};
    if (!impl_->ready)
        return {ControlTrustedHostInventoryStatus::InvalidRequest, std::nullopt};
    if (intent.host_tier == ControlHostTier::SharedPluginHost ||
        forbidden_artifact_path(intent.executable))
        return {ControlTrustedHostInventoryStatus::UnsupportedArtifact, std::nullopt};
    if (!valid_launch_intent(intent))
        return {ControlTrustedHostInventoryStatus::InvalidRequest, std::nullopt};

    if (!impl_->reserve_preparation())
        return {ControlTrustedHostInventoryStatus::ResourceExhausted, std::nullopt};
    struct PreparationReservation {
        Impl* inventory;
        ~PreparationReservation() {
            if (inventory)
                inventory->release_preparation();
        }
    } reservation{impl_.get()};

    const auto source = read_source_pair(intent.executable, impl_->config.maximum_executable_bytes,
                                         impl_->config.maximum_manifest_bytes);
    if (!source)
        return {ControlTrustedHostInventoryStatus::UnsafePath, std::nullopt};
    const auto& sidecar = source->sidecar;

    const auto inventory_id = random_id("inventory-");
    const auto storage_id = random_id("snapshot-");
    const auto session_id = random_id("session-");
    const auto instance_id = random_id("instance-");
    const auto publication_id = random_id("publication-");
    if (!inventory_id || !storage_id || !session_id || !instance_id || !publication_id)
        return {ControlTrustedHostInventoryStatus::EntropyUnavailable, std::nullopt};

    const auto staging = impl_->generation_root / (*storage_id + ".new");
    const auto final = impl_->generation_root / *storage_id;
    std::error_code staging_error;
    const bool staging_exists = fs::exists(staging, staging_error);
    std::error_code final_error;
    const bool final_exists = fs::exists(final, final_error);
    if (staging_error || final_error || staging_exists || final_exists)
        return {ControlTrustedHostInventoryStatus::SnapshotFailed, std::nullopt};
    if (!prepare_private_directory(staging))
        return {ControlTrustedHostInventoryStatus::SnapshotFailed, std::nullopt};
    const auto staged_executable = staging / intent.executable.filename();
    const auto staged_manifest = staging / sidecar.filename();
    auto cleanup = [&] {
        remove_snapshot_pair(staging, staged_executable, staged_manifest);
        remove_snapshot_pair(final, final / intent.executable.filename(),
                             final / sidecar.filename());
    };
    if (!detail::write_owner_private_file_atomic(staged_executable, source->executable) ||
        !detail::write_owner_private_file_atomic(staged_manifest, source->manifest)) {
        cleanup();
        return {ControlTrustedHostInventoryStatus::SnapshotFailed, std::nullopt};
    }
#ifndef _WIN32
    if (::chmod(staged_executable.c_str(), 0500) != 0) {
        cleanup();
        return {ControlTrustedHostInventoryStatus::SnapshotFailed, std::nullopt};
    }
#endif
    std::error_code rename_error;
    fs::rename(staging, final, rename_error);
    if (rename_error) {
        cleanup();
        return {ControlTrustedHostInventoryStatus::SnapshotFailed, std::nullopt};
    }
    const auto final_executable = final / intent.executable.filename();
    const auto final_manifest = final / sidecar.filename();
    if (!private_snapshot_file(final_executable, true) ||
        !private_snapshot_file(final_manifest, false)) {
        cleanup();
        return {ControlTrustedHostInventoryStatus::SnapshotFailed, std::nullopt};
    }
    const auto snap_executable =
        detail::read_owner_private_file(final_executable, impl_->config.maximum_executable_bytes);
    const auto snap_manifest =
        detail::read_owner_private_file(final_manifest, impl_->config.maximum_manifest_bytes);
    if (!snap_executable || !snap_manifest) {
        cleanup();
        return {ControlTrustedHostInventoryStatus::SnapshotFailed, std::nullopt};
    }

    const std::string manifest_json(snap_manifest->begin(), snap_manifest->end());
    ControlManifestDiagnostics diagnostics;
    const auto manifest = parse_control_manifest(manifest_json, &diagnostics);
    if (!manifest || serialize_control_manifest(*manifest) != manifest_json ||
        !manifest->endpoint_included) {
        cleanup();
        return {ControlTrustedHostInventoryStatus::ManifestInvalid, std::nullopt};
    }
    const std::string_view executable_view(reinterpret_cast<const char*>(snap_executable->data()),
                                           snap_executable->size());
    const auto artifact_validation =
        validate_control_artifact_bytes(executable_view, artifact_expectation(*manifest));
    if (!artifact_validation.valid) {
        cleanup();
        return {ControlTrustedHostInventoryStatus::ArtifactInvalid, std::nullopt};
    }
    const auto static_identity = detail::inspect_static_code_identity(final_executable);
    if (!static_identity) {
        cleanup();
        return {ControlTrustedHostInventoryStatus::SignatureInvalid, std::nullopt};
    }
    const auto signed_executable =
        detail::read_owner_private_file(final_executable, impl_->config.maximum_executable_bytes);
    const auto signed_manifest =
        detail::read_owner_private_file(final_manifest, impl_->config.maximum_manifest_bytes);
    const auto repeated_identity = detail::inspect_static_code_identity(final_executable);
    if (!signed_executable || !signed_manifest || *signed_executable != *snap_executable ||
        *signed_manifest != *snap_manifest || !repeated_identity ||
        !private_snapshot_file(final_executable, true) ||
        !private_snapshot_file(final_manifest, false) ||
        repeated_identity->executable_identity != static_identity->executable_identity ||
        repeated_identity->publisher_id != static_identity->publisher_id) {
        cleanup();
        return {ControlTrustedHostInventoryStatus::SignatureInvalid, std::nullopt};
    }

    const auto now = impl_->clock();
    auto snapshot = std::make_unique<ControlTrustedHostSnapshot::Impl>();
    snapshot->directory = final;
    snapshot->executable = final_executable;
    snapshot->manifest = final_manifest;
    snapshot->arguments = intent.arguments;
    snapshot->working_directory = intent.working_directory;
    snapshot->registration.host_tier = intent.host_tier;
    snapshot->registration.session_id = *session_id;
    snapshot->registration.instance_id = *instance_id;
    snapshot->registration.publication_id = *publication_id;
    snapshot->registration.manifest = *manifest;
    snapshot->registration.artifact_digest =
        runtime::sha256_hex(snap_executable->data(), snap_executable->size());
    snapshot->static_expectation = *static_identity;
    snapshot->generation = impl_->config.broker_generation;
    snapshot->expiry = now + impl_->config.ttl;

    const bool committed = impl_->commit_preparation(*inventory_id, std::move(snapshot));
    reservation.inventory = nullptr;
    if (!committed)
        return {ControlTrustedHostInventoryStatus::SnapshotFailed, std::nullopt};
    return {ControlTrustedHostInventoryStatus::Prepared,
            ControlTrustedHostInventoryTicket{*inventory_id, now + impl_->config.ttl}};
}

std::optional<ControlTrustedHostSnapshot>
ControlTrustedHostInventory::consume(std::string_view inventory_id) {
    std::lock_guard lock(impl_->mutex);
    const auto found = impl_->entries.find(inventory_id);
    if (found == impl_->entries.end())
        return std::nullopt;
    if (impl_->clock() >= found->second.snapshot->expiry) {
        impl_->entries.erase(found);
        return std::nullopt;
    }
    auto snapshot = std::move(found->second.snapshot);
    impl_->entries.erase(found);
    return ControlTrustedHostSnapshot(std::move(snapshot));
}

std::size_t ControlTrustedHostInventory::sweep() {
    std::lock_guard lock(impl_->mutex);
    const auto now = impl_->clock();
    const auto before = impl_->entries.size();
    std::erase_if(impl_->entries,
                  [&](const auto& entry) { return now >= entry.second.snapshot->expiry; });
    return before - impl_->entries.size();
}

std::size_t ControlTrustedHostInventory::size() const {
    std::lock_guard lock(impl_->mutex);
    return impl_->entries.size();
}

std::string_view
control_trusted_host_inventory_status_id(ControlTrustedHostInventoryStatus status) {
    switch (status) {
    case ControlTrustedHostInventoryStatus::Prepared:
        return "prepared";
    case ControlTrustedHostInventoryStatus::PlatformUnavailable:
        return "platform_unavailable";
    case ControlTrustedHostInventoryStatus::InvalidRequest:
        return "invalid_request";
    case ControlTrustedHostInventoryStatus::UnsupportedArtifact:
        return "unsupported_artifact";
    case ControlTrustedHostInventoryStatus::UnsafePath:
        return "unsafe_path";
    case ControlTrustedHostInventoryStatus::SnapshotFailed:
        return "snapshot_failed";
    case ControlTrustedHostInventoryStatus::ManifestInvalid:
        return "manifest_invalid";
    case ControlTrustedHostInventoryStatus::ArtifactInvalid:
        return "artifact_invalid";
    case ControlTrustedHostInventoryStatus::SignatureInvalid:
        return "signature_invalid";
    case ControlTrustedHostInventoryStatus::ResourceExhausted:
        return "resource_exhausted";
    case ControlTrustedHostInventoryStatus::EntropyUnavailable:
        return "entropy_unavailable";
    }
    return "invalid_request";
}

} // namespace pulp::inspect
