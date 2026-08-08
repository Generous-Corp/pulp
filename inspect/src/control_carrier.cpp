#include <pulp/inspect/control_carrier.hpp>

#include <system_error>

#ifndef _WIN32
#include <cerrno>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#if defined(__APPLE__)
#include <TargetConditionals.h>
#if !TARGET_OS_IPHONE
#include <sys/acl.h>
#endif
#elif defined(__linux__)
#include <sys/xattr.h>
#endif
#endif

namespace pulp::inspect {
namespace {

constexpr auto kControlEndpointName = "broker.sock";

std::filesystem::path owner_directory_name() {
#ifdef _WIN32
    return "Pulp-Control";
#else
    return "pulp-control-" + std::to_string(::geteuid());
#endif
}

bool usable_endpoint_path(const std::filesystem::path& endpoint_path) {
    if (!endpoint_path.is_absolute() || endpoint_path.filename().empty() ||
        endpoint_path.filename() == "." || endpoint_path.filename() == "..") {
        return false;
    }
    return endpoint_path.lexically_normal() == endpoint_path;
}

#ifndef _WIN32
class DescriptorOwner {
  public:
    explicit DescriptorOwner(int descriptor) : descriptor_(descriptor) {}
    ~DescriptorOwner() {
        if (descriptor_ >= 0)
            ::close(descriptor_);
    }

    DescriptorOwner(const DescriptorOwner&) = delete;
    DescriptorOwner& operator=(const DescriptorOwner&) = delete;

    int get() const {
        return descriptor_;
    }

  private:
    int descriptor_ = -1;
};

bool has_no_extended_acl(int descriptor) {
#if defined(__APPLE__) && !TARGET_OS_IPHONE
    errno = 0;
    acl_t acl = ::acl_get_fd_np(descriptor, ACL_TYPE_EXTENDED);
    const int error = errno;
    if (acl != nullptr) {
        ::acl_free(acl);
        return false;
    }
    return error == ENOENT || error == ENOATTR;
#elif defined(__linux__)
    errno = 0;
    const auto size = ::fgetxattr(descriptor, "system.posix_acl_access", nullptr, 0);
    return size == 0 || (size < 0 && (errno == ENODATA || errno == ENOTSUP || errno == EOPNOTSUPP));
#elif defined(__APPLE__)
    (void)descriptor;
    return false;
#else
    (void)descriptor;
    return true;
#endif
}

bool clear_extended_acl(int descriptor) {
#if defined(__APPLE__) && !TARGET_OS_IPHONE
    acl_t empty = ::acl_init(0);
    if (empty == nullptr)
        return false;
    const bool cleared = ::acl_set_fd_np(descriptor, empty, ACL_TYPE_EXTENDED) == 0;
    ::acl_free(empty);
    return cleared;
#elif defined(__linux__)
    if (::fremovexattr(descriptor, "system.posix_acl_access") == 0)
        return true;
    return errno == ENODATA || errno == ENOTSUP || errno == EOPNOTSUPP;
#elif defined(__APPLE__)
    (void)descriptor;
    return false;
#else
    (void)descriptor;
    return true;
#endif
}

bool private_directory_descriptor(int descriptor) {
    struct stat status{};
    return ::fstat(descriptor, &status) == 0 && S_ISDIR(status.st_mode) &&
           status.st_uid == ::geteuid() && (status.st_mode & 07777) == 0700 &&
           has_no_extended_acl(descriptor);
}

int open_private_directory(const std::filesystem::path& directory) {
    struct stat before{};
    if (::lstat(directory.c_str(), &before) != 0 || !S_ISDIR(before.st_mode) ||
        S_ISLNK(before.st_mode)) {
        return -1;
    }
    int flags = O_RDONLY;
#ifdef O_CLOEXEC
    flags |= O_CLOEXEC;
#endif
#ifdef O_DIRECTORY
    flags |= O_DIRECTORY;
#endif
#ifdef O_NOFOLLOW
    flags |= O_NOFOLLOW;
#endif
    const int descriptor = ::open(directory.c_str(), flags);
    struct stat opened{};
    if (descriptor < 0 || ::fstat(descriptor, &opened) != 0 || opened.st_dev != before.st_dev ||
        opened.st_ino != before.st_ino || !private_directory_descriptor(descriptor)) {
        if (descriptor >= 0)
            ::close(descriptor);
        return -1;
    }
    return descriptor;
}

std::optional<ControlEndpointIdentity> endpoint_identity_at(int parent_descriptor,
                                                            const std::filesystem::path& filename) {
    struct stat status{};
    if (::fstatat(parent_descriptor, filename.c_str(), &status, AT_SYMLINK_NOFOLLOW) != 0 ||
        !S_ISSOCK(status.st_mode) || status.st_uid != ::geteuid() ||
        (status.st_mode & 07777) != 0600) {
        return std::nullopt;
    }
    return ControlEndpointIdentity{
        .device = static_cast<std::uint64_t>(status.st_dev),
        .inode = static_cast<std::uint64_t>(status.st_ino),
    };
}

int open_endpoint_parent(const std::filesystem::path& endpoint_path) {
    if (!usable_endpoint_path(endpoint_path))
        return -1;
    return open_private_directory(endpoint_path.parent_path());
}
#endif

} // namespace

std::filesystem::path default_control_runtime_directory(const std::filesystem::path& runtime_root) {
    return (runtime_root / owner_directory_name()).lexically_normal();
}

std::filesystem::path default_control_runtime_directory() {
    std::error_code error;
    const auto runtime_root = std::filesystem::temp_directory_path(error);
    if (error)
        return {};
    return default_control_runtime_directory(runtime_root);
}

std::filesystem::path default_control_endpoint_path(const std::filesystem::path& runtime_root) {
    return default_control_runtime_directory(runtime_root) / kControlEndpointName;
}

std::filesystem::path default_control_endpoint_path() {
    const auto runtime_directory = default_control_runtime_directory();
    if (runtime_directory.empty())
        return {};
    return runtime_directory / kControlEndpointName;
}

bool prepare_control_runtime_directory(const std::filesystem::path& runtime_directory) {
    if (!runtime_directory.is_absolute() || runtime_directory.filename().empty() ||
        runtime_directory.lexically_normal() != runtime_directory) {
        return false;
    }
#ifdef _WIN32
    return false;
#else
    DescriptorOwner existing(open_private_directory(runtime_directory));
    if (existing.get() >= 0)
        return true;

    struct stat status{};
    if (::lstat(runtime_directory.c_str(), &status) == 0 || errno != ENOENT)
        return false;

    std::error_code error;
    std::filesystem::create_directories(runtime_directory.parent_path(), error);
    if (error || ::mkdir(runtime_directory.c_str(), 0700) != 0)
        return false;

    int flags = O_RDONLY;
#ifdef O_CLOEXEC
    flags |= O_CLOEXEC;
#endif
#ifdef O_DIRECTORY
    flags |= O_DIRECTORY;
#endif
#ifdef O_NOFOLLOW
    flags |= O_NOFOLLOW;
#endif
    DescriptorOwner created(::open(runtime_directory.c_str(), flags));
    return created.get() >= 0 && clear_extended_acl(created.get()) &&
           ::fchmod(created.get(), 0700) == 0 && private_directory_descriptor(created.get());
#endif
}

std::optional<ControlEndpointIdentity>
control_endpoint_identity(const std::filesystem::path& endpoint_path) {
#ifdef _WIN32
    (void)endpoint_path;
    return std::nullopt;
#else
    DescriptorOwner parent(open_endpoint_parent(endpoint_path));
    if (parent.get() < 0)
        return std::nullopt;
    return endpoint_identity_at(parent.get(), endpoint_path.filename());
#endif
}

bool remove_stale_control_endpoint(const std::filesystem::path& endpoint_path,
                                   const ControlEndpointIdentity& expected_identity) {
#ifdef _WIN32
    (void)endpoint_path;
    (void)expected_identity;
    return false;
#else
    DescriptorOwner parent(open_endpoint_parent(endpoint_path));
    if (parent.get() < 0)
        return false;
    const auto current = endpoint_identity_at(parent.get(), endpoint_path.filename());
    if (!current || *current != expected_identity)
        return false;
    return ::unlinkat(parent.get(), endpoint_path.filename().c_str(), 0) == 0;
#endif
}

bool remove_stale_control_endpoint(const std::filesystem::path& endpoint_path) {
    const auto identity = control_endpoint_identity(endpoint_path);
    return identity && remove_stale_control_endpoint(endpoint_path, *identity);
}

} // namespace pulp::inspect
