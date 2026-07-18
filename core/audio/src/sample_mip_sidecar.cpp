#include <pulp/audio/sample_mip_sidecar.hpp>

#include <pulp/audio/audio_file.hpp>
#include <pulp/audio/sample_mip_builder.hpp>
#include <pulp/runtime/crypto.hpp>
#include <pulp/runtime/inter_process_lock.hpp>
#include <pulp/runtime/memory_mapped_file.hpp>
#include <pulp/runtime/scope_guard.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <optional>
#include <random>
#include <stdexcept>
#include <thread>
#include <type_traits>
#include <unordered_set>
#include <utility>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <process.h>
#include <aclapi.h>
#include <sddl.h>
#include <windows.h>
#else
#include <cerrno>
#include <fcntl.h>
#if defined(__APPLE__)
#include <sys/acl.h>
#elif defined(__linux__)
#include <sys/xattr.h>
#endif
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace pulp::audio {

namespace {

std::atomic<detail::SampleMipBuildFaultForTesting> sample_mip_build_fault_for_testing{};

} // namespace

void detail::set_sample_mip_build_fault_for_testing(SampleMipBuildFaultForTesting fault) noexcept {
    sample_mip_build_fault_for_testing.store(fault, std::memory_order_release);
}

namespace {

std::filesystem::path coordination_source_path(std::string_view source_path) {
    std::error_code error;
    auto normalized =
        std::filesystem::weakly_canonical(std::filesystem::path(std::string(source_path)), error);
    if (error) {
        error.clear();
        normalized =
            std::filesystem::absolute(std::filesystem::path(std::string(source_path)), error);
    }
    return error ? std::filesystem::path{} : normalized.lexically_normal();
}

bool source_filesystem_is_case_insensitive(const std::filesystem::path& source_path) {
    auto alternate_name = source_path.filename().string();
    const auto letter = std::find_if(alternate_name.begin(), alternate_name.end(),
                                     [](unsigned char value) { return std::isalpha(value) != 0; });
    if (letter == alternate_name.end())
        return false;
    *letter = std::islower(static_cast<unsigned char>(*letter))
                  ? static_cast<char>(std::toupper(static_cast<unsigned char>(*letter)))
                  : static_cast<char>(std::tolower(static_cast<unsigned char>(*letter)));
    std::error_code error;
    return std::filesystem::equivalent(source_path, source_path.parent_path() / alternate_name,
                                       error) &&
           !error;
}

} // namespace

std::string detail::sample_mip_coordination_key_for_manifest_path(
    std::string_view normalized_manifest_path, bool case_insensitive) {
    auto identity = std::string(normalized_manifest_path);
    if (case_insensitive) {
        std::transform(identity.begin(), identity.end(), identity.begin(), [](unsigned char value) {
            return static_cast<char>(std::tolower(value));
        });
    }
    const auto digest =
        runtime::sha256(reinterpret_cast<const std::uint8_t*>(identity.data()), identity.size());
    return runtime::hex_encode(digest.data(), 16);
}

std::string detail::sample_mip_coordination_key(std::string_view source_path,
                                                const runtime::FileIdentity&) {
    const auto source = coordination_source_path(source_path);
    if (source.empty())
        return {};
    auto manifest_path = source;
    manifest_path += ".pulpmip";
    return detail::sample_mip_coordination_key_for_manifest_path(
        manifest_path.generic_string(), source_filesystem_is_case_insensitive(source));
}

namespace {

constexpr std::array<std::uint8_t, 8> kMagic{'P', 'U', 'L', 'P', 'M', 'I', 'P', 0};
constexpr std::uint16_t kVersion = 5;
constexpr std::uint16_t kHeaderBytes = 116;
constexpr std::uint32_t kRecordBytes = 80;
constexpr std::uint32_t kBuilderRevision = 5;
constexpr std::uint64_t kMaximumManifestBytes =
    kHeaderBytes + SampleMipSidecar::kMaximumLevels * kRecordBytes;
constexpr std::uint64_t kMaximumWavContainerOverhead = 1024ull * 1024ull;

struct ManifestLevel {
    std::uint32_t octave = 0;
    std::uint32_t decimation = 0;
    std::uint64_t frames = 0;
    std::uint32_t rate_numerator = 0;
    std::uint32_t rate_denominator = 0;
    std::uint64_t payload_bytes = 0;
    std::array<std::uint8_t, 32> payload_sha256{};
};

using ManifestNamespace = std::array<std::uint8_t, 16>;

class ByteCursor {
  public:
    explicit ByteCursor(const std::vector<std::uint8_t>& bytes) : bytes_(bytes) {}

    bool read_bytes(std::uint8_t* destination, std::size_t count) noexcept {
        if (count > bytes_.size() - position_)
            return false;
        std::copy_n(bytes_.data() + position_, count, destination);
        position_ += count;
        return true;
    }

    template <typename Integer> bool read_le(Integer& value) noexcept {
        static_assert(std::is_unsigned_v<Integer>);
        if (sizeof(Integer) > bytes_.size() - position_)
            return false;
        value = 0;
        for (std::size_t byte = 0; byte < sizeof(Integer); ++byte)
            value |= static_cast<Integer>(bytes_[position_ + byte]) << (byte * 8);
        position_ += sizeof(Integer);
        return true;
    }

    bool read_zeroes(std::size_t count) noexcept {
        if (count > bytes_.size() - position_)
            return false;
        const auto begin = bytes_.begin() + static_cast<std::ptrdiff_t>(position_);
        if (!std::all_of(begin, begin + static_cast<std::ptrdiff_t>(count),
                         [](std::uint8_t byte) { return byte == 0; }))
            return false;
        position_ += count;
        return true;
    }

    bool at_end() const noexcept {
        return position_ == bytes_.size();
    }

  private:
    const std::vector<std::uint8_t>& bytes_;
    std::size_t position_ = 0;
};

bool regular_file(const std::filesystem::path& path) noexcept {
    std::error_code error;
    const auto status = std::filesystem::symlink_status(path, error);
    return !error && std::filesystem::is_regular_file(status);
}

std::filesystem::path normalized_source_path(std::string_view source_path) {
    std::error_code error;
    auto normalized =
        std::filesystem::weakly_canonical(std::filesystem::path(std::string(source_path)), error);
    if (error)
        return {};
    return normalized.lexically_normal();
}

std::filesystem::path canonical_parent_path(const std::filesystem::path& path) {
    std::error_code error;
    auto parent = path.parent_path();
    if (parent.empty())
        parent = ".";
    auto canonical = std::filesystem::weakly_canonical(parent, error);
    return error ? std::filesystem::path{} : canonical.lexically_normal();
}

ManifestNamespace default_manifest_namespace(std::string_view source_path) {
    const auto source = std::filesystem::path(std::string(source_path));
    const auto parent = canonical_parent_path(source);
    if (parent.empty() || source.filename().empty())
        return {};
    const auto spelling = (parent / source.filename()).lexically_normal().generic_string();
    const auto digest =
        runtime::sha256(reinterpret_cast<const std::uint8_t*>(spelling.data()), spelling.size());
    ManifestNamespace result{};
    std::copy_n(digest.begin(), result.size(), result.begin());
    return result;
}

std::string sample_mip_build_lock_name(std::string_view source_path,
                                       const runtime::FileIdentity& source_identity) {
    return "sampler_mip_build_" + detail::sample_mip_coordination_key(source_path, source_identity);
}

std::string sample_mip_publication_lock_name(std::string_view source_path,
                                             const runtime::FileIdentity& source_identity) {
    return "sampler_mip_publish_" +
           detail::sample_mip_coordination_key(source_path, source_identity);
}

#ifndef _WIN32
bool parent_protects_private_child_entries(const std::filesystem::path& path) noexcept {
    const int directory = ::open(path.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
    if (directory < 0)
        return false;
    struct stat status{};
    const bool read = ::fstat(directory, &status) == 0;
    ::close(directory);
    if (!read || !S_ISDIR(status.st_mode))
        return false;
    const bool shared_writable = (status.st_mode & (S_IWGRP | S_IWOTH)) != 0;
    const bool trusted_sticky_owner = status.st_uid == ::geteuid() || status.st_uid == 0;
    return !shared_writable ||
           ((status.st_mode & S_ISVTX) != 0 && trusted_sticky_owner);
}

bool make_directory_private(const std::filesystem::path& path) noexcept {
    const int directory = ::open(path.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
    if (directory < 0)
        return false;
    bool ok = ::fchmod(directory, 0700) == 0;
#if defined(__APPLE__)
    acl_t empty = ::acl_init(0);
    errno = 0;
    const bool acl_cleared =
        empty != nullptr && ::acl_set_fd_np(directory, empty, ACL_TYPE_EXTENDED) == 0;
    const bool acl_unsupported = !acl_cleared && errno == EOPNOTSUPP;
    ok = ok && (acl_cleared || acl_unsupported);
    if (empty != nullptr)
        ::acl_free(empty);
#elif defined(__linux__)
    for (const char* name : {"system.posix_acl_access", "system.posix_acl_default"}) {
        if (::fremovexattr(directory, name) != 0 && errno != ENODATA && errno != ENOTSUP)
            ok = false;
    }
#endif
    struct stat status{};
    ok = ok && ::fstat(directory, &status) == 0 && (status.st_mode & 0777) == 0700 &&
         status.st_uid == ::geteuid();
    ::close(directory);
    return ok;
}
#endif

std::filesystem::path unique_temporary_directory(const std::filesystem::path& parent) {
#ifndef _WIN32
    if (!parent_protects_private_child_entries(parent))
        return {};
#endif
    std::random_device random;
    for (int attempt = 0; attempt < 128; ++attempt) {
        const std::array<std::uint32_t, 4> nonce{random(), random(), random(), random()};
        const auto name =
            ".pulp-mip-tmp-" +
            runtime::hex_encode(reinterpret_cast<const std::uint8_t*>(nonce.data()), sizeof(nonce));
        const auto path = parent / name;
#ifdef _WIN32
        PSECURITY_DESCRIPTOR descriptor = nullptr;
        if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
                L"D:P(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)(A;OICI;FA;;;OW)", SDDL_REVISION_1,
                &descriptor,
                nullptr)) {
            return {};
        }
        SECURITY_ATTRIBUTES attributes{sizeof(SECURITY_ATTRIBUTES), descriptor, FALSE};
        const bool created = CreateDirectoryW(path.c_str(), &attributes) != 0;
        LocalFree(descriptor);
        if (created)
            return path;
        if (GetLastError() != ERROR_ALREADY_EXISTS)
            return {};
#else
        if (::mkdir(path.c_str(), 0700) == 0) {
            if (make_directory_private(path))
                return path;
            std::error_code cleanup_error;
            std::filesystem::remove(path, cleanup_error);
            return {};
        }
        if (errno != EEXIST)
            return {};
#endif
    }
    return {};
}

runtime::FileIdentity temporary_directory_identity(const std::filesystem::path& path) noexcept {
#ifdef _WIN32
    HANDLE handle = CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES,
                                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                OPEN_EXISTING,
                                FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (handle == INVALID_HANDLE_VALUE)
        return {};
    BY_HANDLE_FILE_INFORMATION information{};
    const bool read = GetFileInformationByHandle(handle, &information) != 0;
    CloseHandle(handle);
    if (!read || (information.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0 ||
        (information.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0)
        return {};
    return {.volume = information.dwVolumeSerialNumber,
            .file = (static_cast<std::uint64_t>(information.nFileIndexHigh) << 32) |
                    information.nFileIndexLow,
            .generation = 0,
            .valid = true};
#else
    struct stat status{};
    if (::lstat(path.c_str(), &status) != 0 || !S_ISDIR(status.st_mode))
        return {};
    return {.volume = static_cast<std::uint64_t>(status.st_dev),
            .file = static_cast<std::uint64_t>(status.st_ino),
            .generation = 0,
            .valid = true};
#endif
}

#ifdef _WIN32
bool temporary_directory_has_private_security(HANDLE handle) noexcept {
    PSECURITY_DESCRIPTOR expected_descriptor = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
            L"D:P(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)(A;OICI;FA;;;OW)", SDDL_REVISION_1,
            &expected_descriptor, nullptr)) {
        return false;
    }
    PACL expected_dacl = nullptr;
    BOOL expected_present = FALSE;
    BOOL expected_defaulted = FALSE;
    if (!GetSecurityDescriptorDacl(expected_descriptor, &expected_present, &expected_dacl,
                                   &expected_defaulted) ||
        !expected_present || expected_dacl == nullptr) {
        LocalFree(expected_descriptor);
        return false;
    }

    PSID owner = nullptr;
    PACL actual_dacl = nullptr;
    PSECURITY_DESCRIPTOR actual_descriptor = nullptr;
    const auto security_error = GetSecurityInfo(
        handle, SE_FILE_OBJECT, OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION, &owner,
        nullptr, &actual_dacl, nullptr, &actual_descriptor);
    HANDLE token = nullptr;
    DWORD token_bytes = 0;
    bool token_opened = OpenThreadToken(GetCurrentThread(), TOKEN_QUERY, TRUE, &token) != 0;
    if (!token_opened && GetLastError() == ERROR_NO_TOKEN)
        token_opened = OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token) != 0;
    if (token_opened)
        GetTokenInformation(token, TokenOwner, nullptr, 0, &token_bytes);
    std::vector<std::uint8_t> token_storage(token_bytes);
    const bool token_read = token_opened && token_bytes != 0 &&
                            GetTokenInformation(token, TokenOwner, token_storage.data(),
                                                token_bytes, &token_bytes) != 0;
    if (token != nullptr)
        CloseHandle(token);

    SECURITY_DESCRIPTOR_CONTROL control = 0;
    DWORD revision = 0;
    const bool control_read =
        actual_descriptor != nullptr &&
        GetSecurityDescriptorControl(actual_descriptor, &control, &revision) != 0;
    const auto* token_owner =
        token_read ? reinterpret_cast<const TOKEN_OWNER*>(token_storage.data()) : nullptr;
    const bool matches =
        security_error == ERROR_SUCCESS && owner != nullptr && token_owner != nullptr &&
        EqualSid(owner, token_owner->Owner) != 0 && control_read &&
        (control & SE_DACL_PROTECTED) != 0 && actual_dacl != nullptr &&
        actual_dacl->AclSize == expected_dacl->AclSize &&
        std::memcmp(actual_dacl, expected_dacl, expected_dacl->AclSize) == 0;
    if (actual_descriptor != nullptr)
        LocalFree(actual_descriptor);
    LocalFree(expected_descriptor);
    return matches;
}
#endif

struct TemporaryDirectory {
    std::filesystem::path path;
    runtime::FileIdentity identity;
#ifdef _WIN32
    void* handle = nullptr;
#endif

    explicit TemporaryDirectory(std::filesystem::path created_path)
        : path(std::move(created_path)) {
#ifdef _WIN32
        HANDLE opened = CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES | READ_CONTROL | DELETE,
                                    FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                                    FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT,
                                    nullptr);
        if (opened != INVALID_HANDLE_VALUE) {
            BY_HANDLE_FILE_INFORMATION information{};
            DWORD filesystem_flags = 0;
            const bool information_read = GetFileInformationByHandle(opened, &information) != 0;
            const bool safe_directory =
                information_read &&
                (information.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0 &&
                (information.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) == 0;
            if (safe_directory &&
                GetVolumeInformationByHandleW(opened, nullptr, 0, nullptr, nullptr,
                                              &filesystem_flags, nullptr, 0) != 0 &&
                (filesystem_flags & FILE_PERSISTENT_ACLS) != 0 &&
                temporary_directory_has_private_security(opened)) {
                handle = opened;
                identity = {.volume = information.dwVolumeSerialNumber,
                            .file = (static_cast<std::uint64_t>(information.nFileIndexHigh) << 32) |
                                    information.nFileIndexLow,
                            .generation = 0,
                            .valid = true};
            } else {
                if (safe_directory) {
                    FILE_DISPOSITION_INFO disposition{TRUE};
                    SetFileInformationByHandle(opened, FileDispositionInfo, &disposition,
                                               sizeof(disposition));
                }
                CloseHandle(opened);
            }
        } else {
            RemoveDirectoryW(path.c_str());
        }
#else
        identity = temporary_directory_identity(path);
#endif
    }

    TemporaryDirectory(const TemporaryDirectory&) = delete;
    TemporaryDirectory& operator=(const TemporaryDirectory&) = delete;
    TemporaryDirectory(TemporaryDirectory&&) = delete;
    TemporaryDirectory& operator=(TemporaryDirectory&&) = delete;

    bool valid() const noexcept {
        if (path.empty() || !identity.valid)
            return false;
#ifdef _WIN32
        if (handle == nullptr)
            return false;
#endif
        const auto current = temporary_directory_identity(path);
        return current.valid && current.volume == identity.volume && current.file == identity.file;
    }

    ~TemporaryDirectory() {
#ifdef _WIN32
        if (valid()) {
            std::error_code error;
            for (std::filesystem::directory_iterator iterator(path, error), end;
                 !error && iterator != end; iterator.increment(error)) {
                std::error_code status_error;
                const auto status =
                    std::filesystem::symlink_status(iterator->path(), status_error);
                if (status_error)
                    continue;
                if (std::filesystem::is_regular_file(status)) {
                    std::error_code remove_error;
                    std::filesystem::remove(iterator->path(), remove_error);
                }
            }
        }
        if (handle != nullptr) {
            FILE_DISPOSITION_INFO disposition{TRUE};
            SetFileInformationByHandle(static_cast<HANDLE>(handle), FileDispositionInfo,
                                       &disposition, sizeof(disposition));
            CloseHandle(static_cast<HANDLE>(handle));
            handle = nullptr;
        }
#else
        if (!valid())
            return;
        std::error_code error;
        std::filesystem::remove_all(path, error);
#endif
    }
};

struct PublishedPayloadRollback {
    std::array<std::filesystem::path, SampleMipSidecar::kMaximumLevels> paths;
    std::array<bool, SampleMipSidecar::kMaximumLevels> remove_on_failure{};
    std::size_t count = 0;
    bool committed = false;
    ~PublishedPayloadRollback() {
        if (committed)
            return;
        for (std::size_t index = 0; index < count; ++index) {
            if (!remove_on_failure[index])
                continue;
            std::error_code error;
            std::filesystem::remove(paths[index], error);
        }
    }

    void prepare(const std::filesystem::path& path) {
        paths[count] = path;
        remove_on_failure[count] = true;
        ++count;
    }

    void retain_last_on_failure() noexcept {
        remove_on_failure[count - 1] = false;
    }

    void discard_last() noexcept {
        --count;
    }
};

enum class PublishResult { Published, AlreadyExists, Failed };

PublishResult publish_no_replace(const std::filesystem::path& from,
                                 const std::filesystem::path& to) noexcept {
#ifdef _WIN32
    if (MoveFileExW(from.c_str(), to.c_str(), MOVEFILE_WRITE_THROUGH) != 0)
        return PublishResult::Published;
    const auto error = GetLastError();
    return error == ERROR_FILE_EXISTS || error == ERROR_ALREADY_EXISTS
               ? PublishResult::AlreadyExists
               : PublishResult::Failed;
#else
    if (::link(from.c_str(), to.c_str()) == 0) {
        ::unlink(from.c_str());
        return PublishResult::Published;
    }
    return errno == EEXIST ? PublishResult::AlreadyExists : PublishResult::Failed;
#endif
}

bool replace_by_rename(const std::filesystem::path& from,
                       const std::filesystem::path& to) noexcept {
#ifdef _WIN32
    return MoveFileExW(from.c_str(), to.c_str(),
                       MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
#else
    return std::rename(from.c_str(), to.c_str()) == 0;
#endif
}

bool sync_file_for_publication(const std::filesystem::path& path) noexcept {
#ifdef _WIN32
    HANDLE handle = CreateFileW(path.c_str(), GENERIC_WRITE,
                                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE)
        return false;
    const bool synced = FlushFileBuffers(handle) != 0;
    CloseHandle(handle);
    return synced;
#else
    const int descriptor = ::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (descriptor < 0)
        return false;
    const bool synced = ::fsync(descriptor) == 0;
    ::close(descriptor);
    return synced;
#endif
}

bool sync_parent_directory(const std::filesystem::path& path) noexcept {
#ifdef _WIN32
    (void)path;
    return true;
#else
    auto parent = path.parent_path();
    if (parent.empty())
        parent = ".";
    const int descriptor = ::open(parent.c_str(), O_RDONLY | O_CLOEXEC | O_DIRECTORY);
    if (descriptor < 0)
        return false;
    const bool synced = ::fsync(descriptor) == 0;
    ::close(descriptor);
    return synced;
#endif
}

template <typename Integer> void append_le(std::vector<std::uint8_t>& bytes, Integer value) {
    static_assert(std::is_unsigned_v<Integer>);
    for (std::size_t byte = 0; byte < sizeof(Integer); ++byte)
        bytes.push_back(static_cast<std::uint8_t>(value >> (byte * 8)));
}

void append_bytes(std::vector<std::uint8_t>& bytes, const std::uint8_t* data, std::size_t count) {
    bytes.insert(bytes.end(), data, data + count);
}

bool write_manifest(const std::filesystem::path& path, const FileFrameReader& source,
                    const runtime::FileIdentity& source_identity,
                    const ManifestNamespace& manifest_namespace,
                    const std::vector<ManifestLevel>& levels) {
    std::vector<std::uint8_t> bytes;
    bytes.reserve(kHeaderBytes + levels.size() * kRecordBytes);
    append_bytes(bytes, kMagic.data(), kMagic.size());
    append_le(bytes, kVersion);
    append_le(bytes, kHeaderBytes);
    append_le(bytes, static_cast<std::uint32_t>(levels.size()));
    append_bytes(bytes, source.content_sha256.data(), source.content_sha256.size());
    append_le(bytes, source.mapped_byte_size);
    append_le(bytes, source.channels);
    append_le(bytes, source.total_frames);
    append_le(bytes, source.sample_rate);
    append_le(bytes, kBuilderRevision);
    append_le(bytes, source_identity.volume);
    append_le(bytes, source_identity.file);
    append_le(bytes, source_identity.generation);
    append_bytes(bytes, manifest_namespace.data(), manifest_namespace.size());
    for (const auto& level : levels) {
        append_le(bytes, level.octave);
        append_le(bytes, level.decimation);
        append_le(bytes, level.frames);
        append_le(bytes, level.rate_numerator);
        append_le(bytes, level.rate_denominator);
        append_le(bytes, level.payload_bytes);
        append_bytes(bytes, level.payload_sha256.data(), level.payload_sha256.size());
        bytes.insert(bytes.end(), 16, 0);
    }
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write(reinterpret_cast<const char*>(bytes.data()),
                 static_cast<std::streamsize>(bytes.size()));
    output.flush();
    if (!output.good())
        return false;
    output.close();
    return !output.fail();
}

bool bounded_payload_file(const std::filesystem::path& path, std::uint64_t expected_bytes,
                          std::uint64_t frames, std::uint32_t channels) noexcept {
    if (!regular_file(path) || channels == 0 ||
        frames > std::numeric_limits<std::uint64_t>::max() / channels)
        return false;
    const auto samples = frames * channels;
    if (samples >
        (std::numeric_limits<std::uint64_t>::max() - kMaximumWavContainerOverhead) / sizeof(float))
        return false;
    const auto maximum_bytes = samples * sizeof(float) + kMaximumWavContainerOverhead;
    if (expected_bytes == 0 || expected_bytes > maximum_bytes)
        return false;
    std::error_code error;
    return std::filesystem::file_size(path, error) == expected_bytes && !error;
}

std::string sample_mip_payload_prefix(
    const ManifestNamespace& manifest_namespace,
    const std::array<std::uint8_t, 32>& source_sha256) {
    return ".pulp-mip-" + runtime::hex_encode(manifest_namespace.data(), 12) + "-" +
           runtime::hex_encode(source_sha256.data(), 8) + "-";
}

std::string sample_mip_namespace_prefix(const ManifestNamespace& manifest_namespace) {
    return ".pulp-mip-" + runtime::hex_encode(manifest_namespace.data(), 12) + "-";
}

std::string sample_mip_payload_path_for_namespace(
    std::string_view source_path, const ManifestNamespace& manifest_namespace,
    const std::array<std::uint8_t, 32>& source_sha256,
    const std::array<std::uint8_t, 32>& payload_sha256, std::uint32_t octave) {
    const auto source = normalized_source_path(source_path);
    if (source.empty())
        return {};
    auto name = sample_mip_payload_prefix(manifest_namespace, source_sha256) + "L";
    if (octave < 10)
        name += '0';
    name += std::to_string(octave);
    name += "-";
    name += runtime::hex_encode(payload_sha256.data(), 16);
    name += ".wav";
    return (source.parent_path() / name).string();
}

std::optional<ManifestNamespace>
read_manifest_namespace(const std::filesystem::path& path, const FileFrameReader& source,
                        const runtime::FileIdentity& source_identity) {
    runtime::MemoryMappedFile manifest;
    if (!manifest.open(path.string(), runtime::MapMode::ReadOnly,
                       static_cast<std::size_t>(kMaximumManifestBytes)) ||
        manifest.size() < kHeaderBytes || manifest.size() > kMaximumManifestBytes) {
        return std::nullopt;
    }
    std::vector<std::uint8_t> bytes(manifest.data(), manifest.data() + manifest.size());
    ByteCursor cursor(bytes);
    std::array<std::uint8_t, 8> magic{};
    std::uint16_t version = 0;
    std::uint16_t header_bytes = 0;
    std::uint32_t level_count = 0;
    std::array<std::uint8_t, 32> source_sha{};
    std::uint64_t source_bytes = 0;
    std::uint32_t source_channels = 0;
    std::uint64_t source_frames = 0;
    std::uint32_t source_rate = 0;
    std::uint32_t builder_revision = 0;
    std::uint64_t source_volume = 0;
    std::uint64_t source_file = 0;
    std::uint64_t source_generation = 0;
    ManifestNamespace manifest_namespace{};
    if (!cursor.read_bytes(magic.data(), magic.size()) || magic != kMagic ||
        !cursor.read_le(version) || version != kVersion || !cursor.read_le(header_bytes) ||
        header_bytes != kHeaderBytes || !cursor.read_le(level_count) || level_count == 0 ||
        level_count > SampleMipSidecar::kMaximumLevels ||
        manifest.size() != kHeaderBytes + level_count * kRecordBytes ||
        !cursor.read_bytes(source_sha.data(), source_sha.size()) || !cursor.read_le(source_bytes) ||
        !cursor.read_le(source_channels) || !cursor.read_le(source_frames) ||
        !cursor.read_le(source_rate) || !cursor.read_le(builder_revision) ||
        builder_revision != kBuilderRevision || !cursor.read_le(source_volume) ||
        !cursor.read_le(source_file) || !cursor.read_le(source_generation) ||
        !cursor.read_bytes(manifest_namespace.data(), manifest_namespace.size())) {
        return std::nullopt;
    }
    if (source_sha != source.content_sha256 || source_bytes != source.mapped_byte_size ||
        source_channels != source.channels || source_frames != source.total_frames ||
        source_rate != source.sample_rate || !source_identity.valid ||
        source_volume != source_identity.volume || source_file != source_identity.file ||
        source_generation != source_identity.generation) {
        return std::nullopt;
    }
    return manifest_namespace;
}

} // namespace

bool sample_mip_sidecar_exists(std::string_view source_path) noexcept {
    std::error_code error;
    const auto path = std::filesystem::path(std::string(source_path) + ".pulpmip");
    return std::filesystem::exists(path, error) && !error;
}

std::string sample_mip_payload_path(std::string_view source_path,
                                    const std::array<std::uint8_t, 32>& source_sha256,
                                    const std::array<std::uint8_t, 32>& payload_sha256,
                                    std::uint32_t octave) {
    return sample_mip_payload_path_for_namespace(source_path,
                                                 default_manifest_namespace(source_path),
                                                 source_sha256, payload_sha256, octave);
}

SampleMipSidecar load_sample_mip_sidecar_from_manifest(
    std::string_view source_path, const std::filesystem::path& manifest_path,
    const FileFrameReader& source, const runtime::FileIdentity& retained_identity) {
    SampleMipSidecar result;
    std::error_code error;
    if (!std::filesystem::exists(manifest_path, error) || error)
        return result;
    result.status = SampleMipSidecarStatus::Invalid;
    if (!source.valid || !source.has_content_identity || !regular_file(manifest_path))
        return result;

    runtime::MemoryMappedFile manifest;
    if (!manifest.open(manifest_path.string(), runtime::MapMode::ReadOnly,
                       static_cast<std::size_t>(kMaximumManifestBytes)))
        return result;
    const auto manifest_size = manifest.size();
    if (manifest_size < kHeaderBytes || manifest_size > kMaximumManifestBytes)
        return result;
    std::vector<std::uint8_t> bytes(manifest.data(), manifest.data() + manifest.size());

    ByteCursor cursor(bytes);
    std::array<std::uint8_t, 8> magic{};
    std::uint16_t version = 0;
    std::uint16_t header_bytes = 0;
    std::uint32_t level_count = 0;
    std::array<std::uint8_t, 32> source_sha{};
    std::uint64_t source_bytes = 0;
    std::uint32_t source_channels = 0;
    std::uint64_t source_frames = 0;
    std::uint32_t source_rate = 0;
    std::uint32_t builder_revision = 0;
    std::uint64_t source_volume = 0;
    std::uint64_t source_file = 0;
    std::uint64_t source_generation = 0;
    ManifestNamespace manifest_namespace{};
    if (!cursor.read_bytes(magic.data(), magic.size()) || magic != kMagic ||
        !cursor.read_le(version) || version != kVersion || !cursor.read_le(header_bytes) ||
        header_bytes != kHeaderBytes || !cursor.read_le(level_count) || level_count == 0 ||
        level_count > SampleMipSidecar::kMaximumLevels ||
        manifest_size != kHeaderBytes + level_count * kRecordBytes ||
        !cursor.read_bytes(source_sha.data(), source_sha.size()) || !cursor.read_le(source_bytes) ||
        !cursor.read_le(source_channels) || !cursor.read_le(source_frames) ||
        !cursor.read_le(source_rate) || !cursor.read_le(builder_revision) ||
        builder_revision != kBuilderRevision || !cursor.read_le(source_volume) ||
        !cursor.read_le(source_file) || !cursor.read_le(source_generation) ||
        !cursor.read_bytes(manifest_namespace.data(), manifest_namespace.size())) {
        return result;
    }
    if (source_sha != source.content_sha256 || source_bytes != source.mapped_byte_size ||
        source_channels != source.channels || source_frames != source.total_frames ||
        source_rate != source.sample_rate || !retained_identity.valid ||
        retained_identity.volume != source_volume || retained_identity.file != source_file ||
        retained_identity.generation != source_generation)
        return result;

    std::array<SampleMipSidecar::Level, SampleMipSidecar::kMaximumLevels> staged{};
    std::uint64_t previous_frames = source.total_frames;
    for (std::uint32_t index = 0; index < level_count; ++index) {
        ManifestLevel level;
        if (!cursor.read_le(level.octave) || !cursor.read_le(level.decimation) ||
            !cursor.read_le(level.frames) || !cursor.read_le(level.rate_numerator) ||
            !cursor.read_le(level.rate_denominator) || !cursor.read_le(level.payload_bytes) ||
            !cursor.read_bytes(level.payload_sha256.data(), level.payload_sha256.size()) ||
            !cursor.read_zeroes(16))
            return result;
        const auto octave = index + 1;
        const auto decimation = std::uint32_t{1} << octave;
        const auto expected_frames = (previous_frames + 1) / 2;
        if (level.octave != octave || level.decimation != decimation ||
            level.frames != expected_frames || level.rate_numerator != source.sample_rate ||
            level.rate_denominator != decimation || level.payload_bytes == 0)
            return result;
        const auto logical_rate =
            static_cast<double>(level.rate_numerator) / static_cast<double>(level.rate_denominator);
        const auto encoded_rate = static_cast<std::uint32_t>(std::llround(logical_rate));
        if (encoded_rate == 0)
            return result;
        const auto path = sample_mip_payload_path_for_namespace(
            source_path, manifest_namespace, source.content_sha256, level.payload_sha256, octave);
        if (!bounded_payload_file(path, level.payload_bytes, level.frames, source.channels))
            return result;
        auto opened = make_memory_mapped_frame_reader(path, true, true, level.payload_bytes);
        if (!opened.valid || !opened.has_content_identity ||
            opened.mapped_byte_size != level.payload_bytes ||
            opened.content_sha256 != level.payload_sha256 || opened.channels != source.channels ||
            opened.total_frames != level.frames || opened.sample_rate != encoded_rate)
            return result;
        staged[index] = {
            .reader = std::move(opened), .sample_rate = logical_rate, .octave = octave};
        previous_frames = level.frames;
    }
    if (!cursor.at_end())
        return result;
    result.levels = std::move(staged);
    result.level_count = level_count;
    result.status = SampleMipSidecarStatus::Valid;
    return result;
}

SampleMipSidecar
load_sample_mip_sidecar(std::string_view source_path, const FileFrameReader& source,
                        const std::shared_ptr<MemoryMappedAudioReader>& retained_source) {
    if (!retained_source)
        return {.status = SampleMipSidecarStatus::Invalid};
    const auto retained_identity = retained_source->opened_file_identity();
    if (!retained_identity.valid)
        return {.status = SampleMipSidecarStatus::Invalid};
    runtime::InterProcessLock load_lock(
        sample_mip_publication_lock_name(source_path, retained_identity));
    const auto lock_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    while (!load_lock.try_lock_shared()) {
        if (std::chrono::steady_clock::now() >= lock_deadline)
            return {.status = SampleMipSidecarStatus::Invalid};
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return load_sample_mip_sidecar_from_manifest(
        source_path, std::filesystem::path(std::string(source_path) + ".pulpmip"), source,
        retained_identity);
}

SampleMipBuildResult build_sample_mip_sidecar(std::string_view source_path,
                                              const SampleMipBuildOptions& options) {
    SampleMipBuildResult result;
    result.manifest_path = std::string(source_path) + ".pulpmip";
    if (options.level_count == 0 || options.level_count > SampleMipSidecar::kMaximumLevels) {
        result.error = "level count must be between 1 and 2";
        return result;
    }
    const auto test_fault = sample_mip_build_fault_for_testing.load(std::memory_order_acquire);
    std::shared_ptr<MemoryMappedAudioReader> source_identity;
    auto source = make_memory_mapped_frame_reader(source_path, true, true,
                                                  options.maximum_source_bytes, &source_identity);
    if (!source.valid || !source.has_content_identity || !source_identity) {
        result.error = "source must be a readable seekable audio file";
        return result;
    }
    const auto opened_source_identity = source_identity->opened_file_identity();
    if (!opened_source_identity.valid) {
        result.error = "failed to capture the source file identity";
        return result;
    }
    const auto normalized_source = normalized_source_path(source_path);
    const auto normalized_string = normalized_source.generic_string();
    if (normalized_source.empty() ||
        runtime::file_identity(normalized_string) != opened_source_identity) {
        result.error = "source path changed while opening the mip input";
        return result;
    }
    const auto publication_source =
        canonical_parent_path(std::filesystem::path(std::string(source_path))) /
        std::filesystem::path(std::string(source_path)).filename();
    if (publication_source.empty()) {
        result.error = "failed to resolve the mip publication directory";
        return result;
    }
    result.manifest_path = publication_source.generic_string() + ".pulpmip";
    runtime::InterProcessLock build_lock(
        sample_mip_build_lock_name(source_path, opened_source_identity));
    const auto lock_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    while (!build_lock.try_lock()) {
        if (std::chrono::steady_clock::now() >= lock_deadline) {
            result.error = "timed out waiting for another mip builder";
            return result;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    const auto source_unchanged = [&] {
        return source_identity->path_refers_to_open_file(source_path) &&
               source_identity->opened_file_identity() == opened_source_identity;
    };
    if (!source_unchanged() ||
        runtime::file_identity(normalized_string) != opened_source_identity) {
        result.error = "source path changed before mip construction";
        return result;
    }
    std::optional<ManifestNamespace> reusable_namespace;
    const auto prior_sidecar = load_sample_mip_sidecar_from_manifest(
        source_path, result.manifest_path, source, opened_source_identity);
    if (prior_sidecar.status == SampleMipSidecarStatus::Valid) {
        reusable_namespace =
            read_manifest_namespace(result.manifest_path, source, opened_source_identity);
    }
    const auto manifest_namespace =
        reusable_namespace.value_or(default_manifest_namespace(source_path));
    if (source.total_frames > std::numeric_limits<std::uint64_t>::max() / source.channels ||
        source.total_frames * source.channels > options.maximum_source_bytes / sizeof(float)) {
        result.error = "decoded source exceeds the configured byte limit";
        return result;
    }
    AudioFileData decoded;
    decoded.sample_rate = source.sample_rate;
    decoded.channels.resize(source.channels);
    std::vector<float*> decoded_channels(source.channels);
    for (std::uint32_t channel = 0; channel < source.channels; ++channel) {
        decoded.channels[channel].resize(static_cast<std::size_t>(source.total_frames));
        decoded_channels[channel] = decoded.channels[channel].data();
    }
    BufferView<float> decoded_view(decoded_channels.data(), source.channels,
                                   static_cast<std::size_t>(source.total_frames));
    if (source.reader(0, decoded_view, source.total_frames) != source.total_frames) {
        result.error = "failed to decode the mapped source identity";
        return result;
    }
    const auto coefficients = design_sample_mip_decimator();
    if (coefficients.empty() || (coefficients.size() & 1u) == 0u) {
        result.error = "failed to design the mip decimator";
        return result;
    }

    const auto source_file = normalized_source;
    TemporaryDirectory temporary{unique_temporary_directory(source_file.parent_path().empty()
                                                                ? std::filesystem::path{"."}
                                                                : source_file.parent_path())};
    if (!temporary.valid()) {
        result.error = "failed to create a private temporary directory";
        return result;
    }
    PublishedPayloadRollback published_payloads;
    std::vector<std::string> payload_paths;
    std::vector<ManifestLevel> manifest_levels;
    std::uint64_t output_bytes = 0;
    auto previous = std::move(decoded);
    for (std::uint32_t index = 0; index < options.level_count; ++index) {
        if (!temporary.valid()) {
            result.error = "private mip staging directory changed identity";
            break;
        }
        const auto octave = index + 1;
        const auto frames = (previous.num_frames() + 1) / 2;
        if (frames == 0 ||
            previous.num_channels() > std::numeric_limits<std::uint64_t>::max() / frames) {
            result.error = "mip dimensions overflow";
            break;
        }
        const auto samples = frames * previous.num_channels();
        if (samples > std::numeric_limits<std::uint64_t>::max() / sizeof(float) ||
            samples * sizeof(float) > options.maximum_output_bytes - output_bytes) {
            result.error = "mip payloads exceed the configured byte limit";
            break;
        }
        AudioFileData next;
        next.sample_rate = static_cast<std::uint32_t>(
            std::llround(static_cast<double>(source.sample_rate) /
                         static_cast<double>(std::uint32_t{1} << octave)));
        if (next.sample_rate == 0) {
            result.error = "mip sample rate rounds to zero";
            break;
        }
        next.channels.resize(previous.channels.size());
        for (std::size_t channel = 0; channel < previous.channels.size(); ++channel) {
            next.channels[channel].resize(static_cast<std::size_t>(frames));
            decimate_sample_mip_2x(previous.channels[channel].data(), previous.num_frames(),
                                   next.channels[channel].data(), frames, coefficients);
        }
        const auto temporary_payload =
            temporary.path / ("level-" + std::to_string(octave) + ".wav");
        if (!write_wav_file(temporary_payload.string(), next, WavBitDepth::Float32)) {
            result.error = "failed to write a temporary mip payload";
            break;
        }
        if (!temporary.valid()) {
            result.error = "private mip staging directory changed identity";
            break;
        }
        if (!sync_file_for_publication(temporary_payload)) {
            result.error = "failed to durably write a temporary mip payload";
            break;
        }
        std::error_code payload_size_error;
        const auto temporary_payload_bytes =
            std::filesystem::file_size(temporary_payload, payload_size_error);
        if (payload_size_error || !bounded_payload_file(temporary_payload, temporary_payload_bytes,
                                                        frames, source.channels)) {
            result.error = "temporary mip payload exceeded its physical bound";
            break;
        }
        if (temporary_payload_bytes > options.maximum_output_bytes - output_bytes) {
            result.error = "mip payloads exceed the configured byte limit";
            break;
        }
        output_bytes += temporary_payload_bytes;
        auto payload = make_memory_mapped_frame_reader(temporary_payload.string(), true, true,
                                                       temporary_payload_bytes);
        if (!payload.valid || !payload.has_content_identity) {
            result.error = "failed to verify a temporary mip payload";
            break;
        }
        const auto final_path = sample_mip_payload_path_for_namespace(
            source_path, manifest_namespace, source.content_sha256, payload.content_sha256, octave);
        const auto payload_bytes = payload.mapped_byte_size;
        const auto payload_sha256 = payload.content_sha256;
        payload = {};
        auto policy_target =
            source_identity->prepare_access_policy_target(temporary_payload.string());
        if (!policy_target || !policy_target.sync()) {
            result.error = "failed to apply the source access policy to a mip payload";
            break;
        }
        if (!source_unchanged()) {
            result.error = "source pathname changed during mip construction";
            break;
        }
        if (!temporary.valid()) {
            result.error = "private mip staging directory changed identity";
            break;
        }
        published_payloads.prepare(final_path);
        const auto publication = publish_no_replace(temporary_payload, final_path);
        if (publication == PublishResult::AlreadyExists) {
            if (!bounded_payload_file(final_path, payload_bytes, frames, source.channels)) {
                published_payloads.discard_last();
                result.error = "hash-addressed mip payload conflicts with existing file";
                break;
            }
            std::shared_ptr<MemoryMappedAudioReader> retained_existing;
            auto existing = make_memory_mapped_frame_reader(final_path, true, true, payload_bytes,
                                                            &retained_existing);
            if (!existing.valid || existing.mapped_byte_size != payload_bytes ||
                existing.content_sha256 != payload_sha256) {
                published_payloads.discard_last();
                result.error = "hash-addressed mip payload conflicts with existing file";
                break;
            }
            retained_existing.reset();
            if (!replace_by_rename(temporary_payload, final_path)) {
                published_payloads.discard_last();
                result.error = "failed to replace the mip payload access policy";
                break;
            }
            published_payloads.retain_last_on_failure();
            if (!policy_target.finalize_after_move() || !policy_target.sync()) {
                result.error = "failed to finalize the mip payload access policy";
                break;
            }
        } else if (publication == PublishResult::Published) {
            if (test_fault ==
                detail::SampleMipBuildFaultForTesting::PayloadPublicationException) {
                throw std::runtime_error("injected mip payload publication exception");
            }
            if (!policy_target.finalize_after_move() || !policy_target.sync()) {
                result.error = "failed to finalize the mip payload access policy";
                break;
            }
        } else {
            published_payloads.discard_last();
            result.error = "failed to publish a mip payload";
            break;
        }
        payload_paths.push_back(final_path);
        manifest_levels.push_back({.octave = octave,
                                   .decimation = std::uint32_t{1} << octave,
                                   .frames = frames,
                                   .rate_numerator = source.sample_rate,
                                   .rate_denominator = std::uint32_t{1} << octave,
                                   .payload_bytes = payload_bytes,
                                   .payload_sha256 = payload_sha256});
        previous = std::move(next);
    }

    if (!result.error.empty())
        return result;
    if (!sync_parent_directory(source_file)) {
        result.error = "failed to durably publish mip payloads";
        return result;
    }

    const auto manifest_file = std::filesystem::path(result.manifest_path);
    TemporaryDirectory manifest_temporary{
        unique_temporary_directory(canonical_parent_path(manifest_file))};
    if (!manifest_temporary.valid()) {
        result.error = "failed to create a private manifest staging directory";
        return result;
    }
    const auto manifest_temp = manifest_temporary.path / "manifest.pulpmip";
    if (!write_manifest(manifest_temp, source, opened_source_identity, manifest_namespace,
                        manifest_levels)) {
        result.error = "failed to finish the mip manifest";
        return result;
    }
    if (!manifest_temporary.valid()) {
        result.error = "private manifest staging directory changed identity";
        return result;
    }
    if (!sync_file_for_publication(manifest_temp)) {
        result.error = "failed to durably write the mip manifest";
        return result;
    }
    const auto staged = load_sample_mip_sidecar_from_manifest(source_path, manifest_temp, source,
                                                              opened_source_identity);
    if (staged.status != SampleMipSidecarStatus::Valid ||
        staged.level_count != options.level_count) {
        result.error = "staged mip manifest failed self-verification";
        return result;
    }
    auto manifest_policy = source_identity->prepare_access_policy_target(manifest_temp.string());
    if (!manifest_policy || !manifest_policy.sync()) {
        result.error = "failed to apply the source access policy to the mip manifest";
        return result;
    }
    runtime::InterProcessLock publication_lock(
        sample_mip_publication_lock_name(source_path, opened_source_identity));
    const auto publication_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    while (!publication_lock.try_lock()) {
        if (std::chrono::steady_clock::now() >= publication_deadline) {
            result.error = "timed out waiting to publish the mip sidecar";
            return result;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    const auto previous_manifest = manifest_temporary.path / "previous.pulpmip";
    runtime::AccessPolicyTarget previous_policy;
    bool previous_saved = false;
    if (regular_file(result.manifest_path)) {
        runtime::MemoryMappedFile opened_previous;
        if (!opened_previous.open_no_follow(result.manifest_path, runtime::MapMode::ReadOnly,
                                            static_cast<std::size_t>(kMaximumManifestBytes))) {
            result.error = "failed to preserve the previous mip manifest";
            return result;
        }
        const auto previous_identity = opened_previous.opened_file_identity();
        std::vector<std::uint8_t> previous_bytes(opened_previous.data(),
                                                 opened_previous.data() + opened_previous.size());
        if (!previous_identity.valid ||
            opened_previous.opened_file_identity() != previous_identity ||
            !opened_previous.path_refers_to_open_file(result.manifest_path)) {
            result.error = "previous mip manifest changed while preserving it";
            return result;
        }
        std::ofstream previous_output(previous_manifest, std::ios::binary | std::ios::trunc);
        previous_output.write(reinterpret_cast<const char*>(previous_bytes.data()),
                              static_cast<std::streamsize>(previous_bytes.size()));
        previous_output.flush();
        if (!previous_output.good()) {
            result.error = "failed to preserve the previous mip manifest";
            return result;
        }
        previous_output.close();
        if (previous_output.fail()) {
            result.error = "failed to preserve the previous mip manifest";
            return result;
        }
        previous_policy = source_identity->prepare_access_policy_target(previous_manifest.string());
        if (!previous_policy || !previous_policy.sync()) {
            result.error = "failed to preserve the previous manifest policy";
            return result;
        }
        previous_saved = true;
    }
    const auto rollback_manifest = [&]() {
        if (previous_saved)
            return replace_by_rename(previous_manifest, manifest_file) &&
                   previous_policy.finalize_after_move() &&
                   previous_policy.sync() &&
                   sync_parent_directory(manifest_file);
        std::error_code remove_error;
        const bool removed = std::filesystem::remove(manifest_file, remove_error);
        std::error_code exists_error;
        const bool still_exists = std::filesystem::exists(manifest_file, exists_error);
        return !remove_error && !exists_error && (removed || !still_exists) &&
               sync_parent_directory(manifest_file);
    };
    if (!source_unchanged()) {
        result.error = "source pathname changed before mip publication";
        return result;
    }
    if (!manifest_temporary.valid()) {
        result.error = "private manifest staging directory changed identity";
        return result;
    }
    if (!replace_by_rename(manifest_temp, manifest_file)) {
        std::error_code error;
        std::filesystem::remove(manifest_temp, error);
        result.error = "failed to publish the mip manifest";
        return result;
    }
    auto manifest_rollback_guard = runtime::make_scope_guard([&] {
        if (!rollback_manifest())
            published_payloads.committed = true;
    });
    if (!sync_parent_directory(manifest_file)) {
        const bool rolled_back = rollback_manifest();
        manifest_rollback_guard.dismiss();
        if (!rolled_back)
            published_payloads.committed = true;
        result.error = "failed to durably publish the mip manifest";
        if (!rolled_back)
            result.error += " and failed to restore the previous manifest";
        return result;
    }
    if (test_fault == detail::SampleMipBuildFaultForTesting::ManifestPolicyFinalization ||
        !manifest_policy.finalize_after_move() || !manifest_policy.sync()) {
        const bool rolled_back = rollback_manifest();
        manifest_rollback_guard.dismiss();
        if (!rolled_back)
            published_payloads.committed = true;
        result.error = "failed to finalize the mip manifest access policy";
        if (!rolled_back)
            result.error += " and failed to restore the previous manifest";
        return result;
    }
    if (test_fault ==
            detail::SampleMipBuildFaultForTesting::SourceChangedAfterManifestPublication ||
        !source_unchanged()) {
        const bool rolled_back = rollback_manifest();
        manifest_rollback_guard.dismiss();
        if (!rolled_back)
            published_payloads.committed = true;
        result.error = "source pathname changed during mip publication";
        if (!rolled_back)
            result.error += " and failed to restore the previous manifest";
        return result;
    }
    if (test_fault ==
        detail::SampleMipBuildFaultForTesting::PublishedManifestVerificationException) {
        throw std::runtime_error("injected mip verification exception");
    }
    auto verified = test_fault ==
                            detail::SampleMipBuildFaultForTesting::PublishedManifestVerification
                        ? SampleMipSidecar{.status = SampleMipSidecarStatus::Invalid}
                        : load_sample_mip_sidecar_from_manifest(source_path, result.manifest_path,
                                                                source, opened_source_identity);
    if (verified.status != SampleMipSidecarStatus::Valid ||
        verified.level_count != options.level_count) {
        const bool rolled_back = rollback_manifest();
        manifest_rollback_guard.dismiss();
        if (!rolled_back)
            published_payloads.committed = true;
        result.error = "published mip sidecar failed self-verification";
        if (!rolled_back)
            result.error += " and failed to restore the previous manifest";
        return result;
    }
    manifest_rollback_guard.dismiss();
    published_payloads.committed = true;
    try {
        if (test_fault ==
            detail::SampleMipBuildFaultForTesting::PostCommitGarbageCollectionException) {
            throw std::runtime_error("injected post-commit garbage-collection exception");
        }
        std::unordered_set<std::string> keep;
        for (const auto& payload : payload_paths)
            keep.insert(std::filesystem::path(payload).filename().string());
        const auto prefix = manifest_namespace == default_manifest_namespace(source_path)
                                ? sample_mip_namespace_prefix(manifest_namespace)
                                : sample_mip_payload_prefix(manifest_namespace,
                                                            source.content_sha256);
        std::error_code scan_error;
        const auto parent = source_file.parent_path().empty() ? std::filesystem::path{"."}
                                                              : source_file.parent_path();
        for (std::filesystem::directory_iterator iterator(parent, scan_error), end;
             !scan_error && iterator != end; iterator.increment(scan_error)) {
            const auto& candidate = iterator->path();
            const auto name = candidate.filename().string();
            if (!name.starts_with(prefix) || candidate.extension() != ".wav" ||
                keep.contains(name))
                continue;
            std::error_code status_error;
            if (!std::filesystem::is_regular_file(
                    std::filesystem::symlink_status(candidate, status_error)) ||
                status_error)
                continue;
            std::error_code remove_error;
            std::filesystem::remove(candidate, remove_error);
        }
    } catch (...) {
        // Publication is already committed. Garbage collection is retried by later builds.
    }
    result.payload_paths = std::move(payload_paths);
    result.ok = true;
    return result;
}

} // namespace pulp::audio
