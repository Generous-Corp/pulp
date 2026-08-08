#include <pulp/runtime/socket.hpp>

#ifdef __APPLE__
#include <TargetConditionals.h>
#endif

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
#else
#include <cerrno>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <netdb.h>
#include <fcntl.h>
#include <poll.h>
#if defined(__APPLE__) && !TARGET_OS_IPHONE
#include <bsm/libbsm.h>
#include <sys/acl.h>
#elif defined(__linux__)
#include <sys/xattr.h>
#endif
#endif

#include <cstring>
#include <algorithm>
#include <climits>
#include <limits>

namespace pulp::runtime {

#ifndef _WIN32
namespace {

bool has_private_local_parent(std::string_view path) {
    if (path.empty() || path.front() != '/')
        return false;
    const auto separator = path.find_last_of('/');
    if (separator == std::string_view::npos)
        return false;
    const auto parent = separator == 0 ? std::string{"/"}
                                       : std::string{path.substr(0, separator)};
    const int parent_fd =
        ::open(parent.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_DIRECTORY);
    if (parent_fd < 0)
        return false;
    struct stat parent_status {};
    if (::fstat(parent_fd, &parent_status) != 0 ||
        !S_ISDIR(parent_status.st_mode) || parent_status.st_uid != ::geteuid()) {
        ::close(parent_fd);
        return false;
    }
    if ((parent_status.st_mode & (S_IRWXG | S_IRWXO)) != 0) {
        ::close(parent_fd);
        return false;
    }
#if defined(__APPLE__) && !TARGET_OS_IPHONE
    errno = 0;
    acl_t acl = ::acl_get_fd_np(parent_fd, ACL_TYPE_EXTENDED);
    const int acl_error = errno;
    ::close(parent_fd);
    if (acl != nullptr) {
        ::acl_free(acl);
        return false;
    }
    return acl_error == ENOENT || acl_error == ENOATTR;
#elif defined(__linux__)
    errno = 0;
    const auto acl_size =
        ::fgetxattr(parent_fd, "system.posix_acl_access", nullptr, 0);
    const int acl_error = errno;
    ::close(parent_fd);
    if (acl_size > 0)
        return false;
    if (acl_size == 0 || acl_error == ENODATA || acl_error == ENOTSUP ||
        acl_error == EOPNOTSUPP) {
        return true;
    }
    return false;
#elif defined(__APPLE__)
    ::close(parent_fd);
    return false;
#else
    ::close(parent_fd);
    return true;
#endif
}

}  // namespace
#endif

#ifdef _WIN32
static bool winsock_init() {
    static bool initialized = false;
    if (!initialized) {
        WSADATA wsa;
        initialized = (WSAStartup(MAKEWORD(2, 2), &wsa) == 0);
    }
    return initialized;
}
static constexpr std::uintptr_t kInvalidSocketHandle =
    static_cast<std::uintptr_t>(INVALID_SOCKET);
#define NATIVE_SOCKET(fd) static_cast<SOCKET>(fd)
#define SOCKET_CLOSE closesocket
#define SOCKET_SHUTDOWN(fd) ::shutdown((fd), SD_BOTH)
#else
static constexpr int kInvalidSocketHandle = -1;
#define NATIVE_SOCKET(fd) (fd)
#define SOCKET_CLOSE ::close
#define SOCKET_SHUTDOWN(fd) ::shutdown((fd), SHUT_RDWR)
#endif

Socket::~Socket() { close(); }

Socket::Socket(Socket&& other) noexcept
    : fd_(other.fd_),
      type_(other.type_),
      bound_local_path_(std::move(other.bound_local_path_)),
      bound_local_device_(other.bound_local_device_),
      bound_local_inode_(other.bound_local_inode_) {
    other.fd_ = kInvalidSocketHandle;
    other.bound_local_path_.clear();
    other.bound_local_device_ = 0;
    other.bound_local_inode_ = 0;
}

Socket& Socket::operator=(Socket&& other) noexcept {
    if (this != &other) {
        close();
        fd_ = other.fd_;
        type_ = other.type_;
        bound_local_path_ = std::move(other.bound_local_path_);
        bound_local_device_ = other.bound_local_device_;
        bound_local_inode_ = other.bound_local_inode_;
        other.fd_ = kInvalidSocketHandle;
        other.bound_local_path_.clear();
        other.bound_local_device_ = 0;
        other.bound_local_inode_ = 0;
    }
    return *this;
}

bool Socket::create(SocketType type) {
    close();
    type_ = type;

#ifdef _WIN32
    if (!winsock_init()) return false;
#endif

    int family = AF_INET;
    int sock_type = type == SocketType::UDP ? SOCK_DGRAM : SOCK_STREAM;
    int protocol = type == SocketType::TCP ? IPPROTO_TCP
                                           : type == SocketType::UDP ? IPPROTO_UDP : 0;
    if (type == SocketType::Local) {
#ifdef _WIN32
        return false;
#else
        family = AF_UNIX;
#endif
    }
    fd_ = static_cast<decltype(fd_)>(::socket(family, sock_type, protocol));
#ifdef SO_NOSIGPIPE
    if (fd_ != kInvalidSocketHandle) {
        const int enabled = 1;
        ::setsockopt(NATIVE_SOCKET(fd_), SOL_SOCKET, SO_NOSIGPIPE,
                     &enabled, sizeof(enabled));
    }
#endif
    return fd_ != kInvalidSocketHandle;
}

bool Socket::bind(std::string_view address, uint16_t port) {
    if (fd_ == kInvalidSocketHandle || type_ == SocketType::Local) return false;

    struct sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    std::string addr_str(address);
    if (addr_str.empty() || addr_str == "0.0.0.0")
        addr.sin_addr.s_addr = INADDR_ANY;
    else
        inet_pton(AF_INET, addr_str.c_str(), &addr.sin_addr);

    int opt = 1;
    setsockopt(NATIVE_SOCKET(fd_), SOL_SOCKET, SO_REUSEADDR,
               reinterpret_cast<const char*>(&opt), sizeof(opt));

    return ::bind(NATIVE_SOCKET(fd_), reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) == 0;
}

bool Socket::bind_local(std::string_view path) {
#ifdef _WIN32
    (void)path;
    return false;
#else
    if (fd_ == kInvalidSocketHandle || type_ != SocketType::Local ||
        !has_private_local_parent(path)) {
        return false;
    }

    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    const std::string path_string(path);
    if (path.size() >= sizeof(address.sun_path) ||
        ::access(path_string.c_str(), F_OK) == 0) {
        return false;
    }
    std::memcpy(address.sun_path, path.data(), path.size());
    address.sun_path[path.size()] = '\0';
    if (::bind(fd_, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0)
        return false;
    if (::chmod(address.sun_path, S_IRUSR | S_IWUSR) != 0) {
        ::unlink(address.sun_path);
        return false;
    }
    struct stat endpoint_status {};
    if (::lstat(address.sun_path, &endpoint_status) != 0 ||
        !S_ISSOCK(endpoint_status.st_mode)) {
        ::unlink(address.sun_path);
        return false;
    }
    bound_local_path_ = std::string(path);
    bound_local_device_ = static_cast<std::uint64_t>(endpoint_status.st_dev);
    bound_local_inode_ = static_cast<std::uint64_t>(endpoint_status.st_ino);
    return true;
#endif
}

bool Socket::listen(int backlog) {
    if (fd_ == kInvalidSocketHandle) return false;
    return ::listen(NATIVE_SOCKET(fd_), backlog) == 0;
}

std::optional<Socket> Socket::accept(std::chrono::milliseconds timeout) {
    if (fd_ == kInvalidSocketHandle) return std::nullopt;

    struct sockaddr_storage client_addr{};
    socklen_t addr_len = sizeof(client_addr);
#ifdef _WIN32
    SOCKET client_fd = INVALID_SOCKET;
#else
    int client_fd = -1;
#endif

    if (timeout > std::chrono::milliseconds(0)) {
#ifdef _WIN32
        u_long nonblocking = 1;
        if (::ioctlsocket(NATIVE_SOCKET(fd_), FIONBIO, &nonblocking) != 0)
            return std::nullopt;
        const auto restore_blocking = [&] {
            u_long blocking = 0;
            return ::ioctlsocket(NATIVE_SOCKET(fd_), FIONBIO, &blocking) == 0;
        };
#else
        const int original_flags = ::fcntl(NATIVE_SOCKET(fd_), F_GETFL, 0);
        if (original_flags < 0 ||
            ::fcntl(NATIVE_SOCKET(fd_), F_SETFL, original_flags | O_NONBLOCK) != 0) {
            return std::nullopt;
        }
        const auto restore_blocking = [&] {
            return ::fcntl(NATIVE_SOCKET(fd_), F_SETFL, original_flags) == 0;
        };
#endif
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        for (;;) {
            const auto now = std::chrono::steady_clock::now();
            if (now >= deadline)
                break;
            const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
                deadline - now);
            const int wait_ms = static_cast<int>(
                std::min<std::int64_t>(
                    std::max<std::int64_t>(remaining.count(), 1), INT_MAX));
#ifdef _WIN32
            WSAPOLLFD descriptor{};
            descriptor.fd = NATIVE_SOCKET(fd_);
            descriptor.events = POLLRDNORM;
            const int ready = ::WSAPoll(&descriptor, 1, wait_ms);
            if (ready == SOCKET_ERROR && ::WSAGetLastError() == WSAEINTR)
                continue;
#else
            struct pollfd descriptor {
                NATIVE_SOCKET(fd_), POLLIN, 0
            };
            const int ready = ::poll(&descriptor, 1, wait_ms);
            if (ready < 0 && errno == EINTR)
                continue;
#endif
            if (ready <= 0) break;

            addr_len = sizeof(client_addr);
            client_fd = ::accept(
                NATIVE_SOCKET(fd_),
                reinterpret_cast<struct sockaddr*>(&client_addr), &addr_len);
            if (client_fd != static_cast<decltype(client_fd)>(kInvalidSocketHandle))
                break;
#ifdef _WIN32
            const int accept_error = ::WSAGetLastError();
            if (accept_error == WSAEINTR || accept_error == WSAEWOULDBLOCK)
                continue;
#else
            if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)
                continue;
#endif
            break;
        }
        if (!restore_blocking()) {
            if (client_fd != static_cast<decltype(client_fd)>(kInvalidSocketHandle))
                SOCKET_CLOSE(client_fd);
            return std::nullopt;
        }
        if (client_fd != static_cast<decltype(client_fd)>(kInvalidSocketHandle)) {
#ifdef _WIN32
            u_long blocking = 0;
            if (::ioctlsocket(client_fd, FIONBIO, &blocking) != 0) {
                SOCKET_CLOSE(client_fd);
                return std::nullopt;
            }
#else
            const int client_flags = ::fcntl(client_fd, F_GETFL, 0);
            if (client_flags < 0 ||
                ::fcntl(client_fd, F_SETFL, client_flags & ~O_NONBLOCK) != 0) {
                SOCKET_CLOSE(client_fd);
                return std::nullopt;
            }
#endif
        }
    } else {
        client_fd = ::accept(
            NATIVE_SOCKET(fd_), reinterpret_cast<struct sockaddr*>(&client_addr),
            &addr_len);
    }

    if (client_fd == static_cast<decltype(client_fd)>(kInvalidSocketHandle)) return std::nullopt;

    Socket client;
    client.fd_ = static_cast<decltype(client.fd_)>(client_fd);
    client.type_ = type_;
#ifdef SO_NOSIGPIPE
    const int enabled = 1;
    ::setsockopt(NATIVE_SOCKET(client.fd_), SOL_SOCKET, SO_NOSIGPIPE,
                 &enabled, sizeof(enabled));
#endif
    return client;
}

bool Socket::connect(std::string_view address, uint16_t port,
                     std::chrono::milliseconds timeout) {
    if (fd_ == kInvalidSocketHandle || type_ == SocketType::Local) return false;

    struct sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    std::string addr_str(address);
    if (inet_pton(AF_INET, addr_str.c_str(), &addr.sin_addr) != 1) {
        struct addrinfo hints {};
        hints.ai_family = AF_INET;
        hints.ai_socktype =
            type_ == SocketType::TCP ? SOCK_STREAM : SOCK_DGRAM;
        hints.ai_protocol =
            type_ == SocketType::TCP ? IPPROTO_TCP : IPPROTO_UDP;

        struct addrinfo* results = nullptr;
        if (::getaddrinfo(addr_str.c_str(), nullptr, &hints, &results) != 0)
            return false;

        bool resolved = false;
        for (auto* candidate = results; candidate != nullptr;
             candidate = candidate->ai_next) {
            if (candidate->ai_family != AF_INET ||
                candidate->ai_addr == nullptr ||
                candidate->ai_addrlen < sizeof(addr)) {
                continue;
            }
            addr.sin_addr = reinterpret_cast<const struct sockaddr_in*>(
                                candidate->ai_addr)
                                ->sin_addr;
            resolved = true;
            break;
        }
        ::freeaddrinfo(results);
        if (!resolved)
            return false;
    }

    return connect_address(&addr, sizeof(addr), timeout);
}

bool Socket::connect_local(std::string_view path,
                           std::chrono::milliseconds timeout) {
#ifdef _WIN32
    (void)path;
    (void)timeout;
    return false;
#else
    if (fd_ == kInvalidSocketHandle || type_ != SocketType::Local ||
        !has_private_local_parent(path)) {
        return false;
    }
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    if (path.size() >= sizeof(address.sun_path))
        return false;
    std::memcpy(address.sun_path, path.data(), path.size());
    address.sun_path[path.size()] = '\0';
    return connect_address(&address, sizeof(address), timeout);
#endif
}

bool Socket::connect_address(const void* address, std::size_t address_size,
                             std::chrono::milliseconds timeout) {
    if (fd_ == kInvalidSocketHandle || address == nullptr || address_size == 0)
        return false;
    auto* socket_address = const_cast<struct sockaddr*>(
        static_cast<const struct sockaddr*>(address));
    const auto socket_address_size = static_cast<socklen_t>(address_size);
    if (timeout <= std::chrono::milliseconds(0)) {
        return ::connect(NATIVE_SOCKET(fd_), socket_address,
                         socket_address_size) == 0;
    }

#ifdef _WIN32
    u_long nonblocking = 1;
    if (::ioctlsocket(
            NATIVE_SOCKET(fd_), FIONBIO, &nonblocking) != 0) {
        return false;
    }
    const auto restore_blocking = [&] {
        u_long blocking = 0;
        return ::ioctlsocket(
                   NATIVE_SOCKET(fd_), FIONBIO, &blocking) == 0;
    };
#else
    const int original_flags = ::fcntl(NATIVE_SOCKET(fd_), F_GETFL, 0);
    if (original_flags < 0 ||
        ::fcntl(
            NATIVE_SOCKET(fd_), F_SETFL,
            original_flags | O_NONBLOCK) != 0) {
        return false;
    }
    const auto restore_blocking = [&] {
        return ::fcntl(
                   NATIVE_SOCKET(fd_), F_SETFL, original_flags) == 0;
    };
#endif

    const int result = ::connect(
        NATIVE_SOCKET(fd_), socket_address, socket_address_size);
    if (result == 0)
        return restore_blocking();

#ifdef _WIN32
    const int connect_error = ::WSAGetLastError();
    const bool pending =
        connect_error == WSAEWOULDBLOCK ||
        connect_error == WSAEINPROGRESS ||
        connect_error == WSAEALREADY;
#else
    const bool pending =
        errno == EINPROGRESS || errno == EWOULDBLOCK ||
        errno == EAGAIN;
#endif
    if (!pending) {
        (void)restore_blocking();
        return false;
    }

    const auto started = std::chrono::steady_clock::now();
    bool connected = false;
    for (;;) {
        const auto elapsed =
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - started);
        if (elapsed >= timeout)
            break;
        const auto remaining = timeout - elapsed;
        const int wait_ms = static_cast<int>(
            std::min<std::int64_t>(
                std::max<std::int64_t>(remaining.count(), 1),
                INT_MAX));
#ifdef _WIN32
        WSAPOLLFD descriptor{};
        descriptor.fd = NATIVE_SOCKET(fd_);
        descriptor.events = POLLWRNORM;
        const int ready = ::WSAPoll(&descriptor, 1, wait_ms);
        if (ready == SOCKET_ERROR &&
            ::WSAGetLastError() == WSAEINTR) {
            continue;
        }
#else
        struct pollfd descriptor {
            NATIVE_SOCKET(fd_), POLLOUT, 0
        };
        const int ready = ::poll(&descriptor, 1, wait_ms);
        if (ready < 0 && errno == EINTR)
            continue;
#endif
        if (ready <= 0)
            break;

        int socket_error = 0;
#ifdef _WIN32
        int error_size = sizeof(socket_error);
        if (::getsockopt(
                NATIVE_SOCKET(fd_), SOL_SOCKET, SO_ERROR,
                reinterpret_cast<char*>(&socket_error),
                &error_size) == 0) {
            connected = socket_error == 0;
        }
#else
        socklen_t error_size = sizeof(socket_error);
        if (::getsockopt(
                NATIVE_SOCKET(fd_), SOL_SOCKET, SO_ERROR,
                &socket_error, &error_size) == 0) {
            connected = socket_error == 0;
        }
#endif
        break;
    }

    const bool restored = restore_blocking();
    return connected && restored;
}

int Socket::send(const uint8_t* data, size_t length) {
    if (fd_ == kInvalidSocketHandle) return -1;
    int flags = 0;
#ifdef MSG_NOSIGNAL
    flags |= MSG_NOSIGNAL;
#endif
    return static_cast<int>(::send(NATIVE_SOCKET(fd_), reinterpret_cast<const char*>(data),
                                   static_cast<int>(length), flags));
}

int Socket::send(std::string_view data) {
    return send(reinterpret_cast<const uint8_t*>(data.data()), data.size());
}

int Socket::send_to(const uint8_t* data, size_t length,
                    std::string_view address, uint16_t port) {
    if (fd_ == kInvalidSocketHandle) return -1;

    struct sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    std::string addr_str(address);
    inet_pton(AF_INET, addr_str.c_str(), &addr.sin_addr);

    int flags = 0;
#ifdef MSG_NOSIGNAL
    flags |= MSG_NOSIGNAL;
#endif
    return static_cast<int>(::sendto(NATIVE_SOCKET(fd_), reinterpret_cast<const char*>(data),
                                     static_cast<int>(length), flags,
                                     reinterpret_cast<struct sockaddr*>(&addr),
                                     sizeof(addr)));
}

int Socket::receive(uint8_t* buffer, size_t buffer_size) {
    if (fd_ == kInvalidSocketHandle) return -1;
    return static_cast<int>(::recv(NATIVE_SOCKET(fd_), reinterpret_cast<char*>(buffer),
                                   static_cast<int>(buffer_size), 0));
}

std::optional<LocalPeerCredentials> Socket::local_peer_credentials() const {
#if defined(_WIN32)
    return std::nullopt;
#elif defined(__APPLE__) && !TARGET_OS_IPHONE
    if (fd_ == kInvalidSocketHandle || type_ != SocketType::Local)
        return std::nullopt;
    uid_t uid = 0;
    gid_t gid = 0;
    if (::getpeereid(fd_, &uid, &gid) != 0)
        return std::nullopt;
    pid_t pid = 0;
    socklen_t pid_size = sizeof(pid);
    if (::getsockopt(fd_, SOL_LOCAL, LOCAL_PEERPID, &pid, &pid_size) != 0 ||
        pid <= 0) {
        return std::nullopt;
    }
    audit_token_t token{};
    socklen_t token_size = sizeof(token);
    if (::getsockopt(fd_, SOL_LOCAL, LOCAL_PEERTOKEN, &token, &token_size) != 0 ||
        token_size != sizeof(token) || audit_token_to_euid(token) != uid ||
        audit_token_to_egid(token) != gid || audit_token_to_pid(token) != pid) {
        return std::nullopt;
    }
    const auto pid_version = audit_token_to_pidversion(token);
    if (pid_version <= 0)
        return std::nullopt;
    return LocalPeerCredentials{static_cast<std::uint64_t>(uid),
                                static_cast<std::uint64_t>(gid),
                                static_cast<std::int64_t>(pid),
                                static_cast<std::uint64_t>(pid_version)};
#elif defined(__linux__)
    if (fd_ == kInvalidSocketHandle || type_ != SocketType::Local)
        return std::nullopt;
    struct {
        pid_t pid;
        uid_t uid;
        gid_t gid;
    } credentials{};
    socklen_t credentials_size = sizeof(credentials);
    if (::getsockopt(fd_, SOL_SOCKET, SO_PEERCRED, &credentials,
                     &credentials_size) != 0 || credentials.pid <= 0) {
        return std::nullopt;
    }
    return LocalPeerCredentials{
        static_cast<std::uint64_t>(credentials.uid),
        static_cast<std::uint64_t>(credentials.gid),
        static_cast<std::int64_t>(credentials.pid),
        0};
#else
    return std::nullopt;
#endif
}

bool Socket::set_write_timeout(std::chrono::milliseconds timeout) {
    if (fd_ == kInvalidSocketHandle) return false;
    const auto bounded = std::max(timeout, std::chrono::milliseconds(0));
#ifdef _WIN32
    const DWORD value = static_cast<DWORD>(
        std::min<std::int64_t>(
            bounded.count(), std::numeric_limits<DWORD>::max()));
    return ::setsockopt(NATIVE_SOCKET(fd_), SOL_SOCKET, SO_SNDTIMEO,
                        reinterpret_cast<const char*>(&value),
                        sizeof(value)) == 0;
#else
    const struct timeval value {
        static_cast<time_t>(bounded.count() / 1000),
        static_cast<suseconds_t>((bounded.count() % 1000) * 1000)
    };
    return ::setsockopt(NATIVE_SOCKET(fd_), SOL_SOCKET, SO_SNDTIMEO,
                        &value, sizeof(value)) == 0;
#endif
}

bool Socket::set_read_timeout(std::chrono::milliseconds timeout) {
    if (fd_ == kInvalidSocketHandle) return false;
    const auto bounded = std::max(timeout, std::chrono::milliseconds(0));
#ifdef _WIN32
    const DWORD value = static_cast<DWORD>(
        std::min<std::int64_t>(
            bounded.count(), std::numeric_limits<DWORD>::max()));
    return ::setsockopt(NATIVE_SOCKET(fd_), SOL_SOCKET, SO_RCVTIMEO,
                        reinterpret_cast<const char*>(&value),
                        sizeof(value)) == 0;
#else
    const struct timeval value {
        static_cast<time_t>(bounded.count() / 1000),
        static_cast<suseconds_t>((bounded.count() % 1000) * 1000)
    };
    return ::setsockopt(NATIVE_SOCKET(fd_), SOL_SOCKET, SO_RCVTIMEO,
                        &value, sizeof(value)) == 0;
#endif
}

int Socket::receive_from(uint8_t* buffer, size_t buffer_size,
                         std::string& from_address, uint16_t& from_port) {
    if (fd_ == kInvalidSocketHandle) return -1;

    struct sockaddr_in addr{};
    socklen_t addr_len = sizeof(addr);
    int bytes = static_cast<int>(::recvfrom(NATIVE_SOCKET(fd_), reinterpret_cast<char*>(buffer),
                                            static_cast<int>(buffer_size), 0,
                                            reinterpret_cast<struct sockaddr*>(&addr),
                                            &addr_len));
    if (bytes >= 0) {
        char ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &addr.sin_addr, ip, sizeof(ip));
        from_address = ip;
        from_port = ntohs(addr.sin_port);
    }
    return bytes;
}

void Socket::close() {
    if (fd_ != kInvalidSocketHandle) {
        if (type_ != SocketType::UDP) {
            (void)SOCKET_SHUTDOWN(NATIVE_SOCKET(fd_));
        }
        SOCKET_CLOSE(NATIVE_SOCKET(fd_));
        fd_ = kInvalidSocketHandle;
    }
#ifndef _WIN32
    if (!bound_local_path_.empty()) {
        struct stat endpoint_status {};
        if (::lstat(bound_local_path_.c_str(), &endpoint_status) == 0 &&
            S_ISSOCK(endpoint_status.st_mode) &&
            static_cast<std::uint64_t>(endpoint_status.st_dev) ==
                bound_local_device_ &&
            static_cast<std::uint64_t>(endpoint_status.st_ino) ==
                bound_local_inode_) {
            (void)::unlink(bound_local_path_.c_str());
        }
        bound_local_path_.clear();
        bound_local_device_ = 0;
        bound_local_inode_ = 0;
    }
#endif
}

void Socket::shutdown() {
    if (fd_ != kInvalidSocketHandle && type_ != SocketType::UDP) {
        (void)SOCKET_SHUTDOWN(NATIVE_SOCKET(fd_));
    }
}

bool Socket::is_open() const {
    return fd_ != kInvalidSocketHandle;
}

uint16_t Socket::local_port() const {
    if (fd_ == kInvalidSocketHandle || type_ == SocketType::Local) return 0;

    struct sockaddr_in addr{};
    socklen_t addr_len = sizeof(addr);
    if (::getsockname(NATIVE_SOCKET(fd_), reinterpret_cast<struct sockaddr*>(&addr),
                      &addr_len) != 0) {
        return 0;
    }
    return ntohs(addr.sin_port);
}

}  // namespace pulp::runtime
