#include <pulp/runtime/socket.hpp>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <netdb.h>
#include <fcntl.h>
#endif

#include <cstring>

namespace pulp::runtime {

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

Socket::Socket(Socket&& other) noexcept : fd_(other.fd_), type_(other.type_) {
    other.fd_ = kInvalidSocketHandle;
}

Socket& Socket::operator=(Socket&& other) noexcept {
    if (this != &other) {
        close();
        fd_ = other.fd_;
        type_ = other.type_;
        other.fd_ = kInvalidSocketHandle;
    }
    return *this;
}

bool Socket::create(SocketType type) {
    close();
    type_ = type;

#ifdef _WIN32
    if (!winsock_init()) return false;
#endif

    int sock_type = (type == SocketType::TCP) ? SOCK_STREAM : SOCK_DGRAM;
    int protocol = (type == SocketType::TCP) ? IPPROTO_TCP : IPPROTO_UDP;
    fd_ = static_cast<decltype(fd_)>(::socket(AF_INET, sock_type, protocol));
    return fd_ != kInvalidSocketHandle;
}

bool Socket::bind(std::string_view address, uint16_t port) {
    if (fd_ == kInvalidSocketHandle) return false;

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

bool Socket::listen(int backlog) {
    if (fd_ == kInvalidSocketHandle) return false;
    return ::listen(NATIVE_SOCKET(fd_), backlog) == 0;
}

std::optional<Socket> Socket::accept() {
    if (fd_ == kInvalidSocketHandle) return std::nullopt;

    struct sockaddr_in client_addr{};
    socklen_t addr_len = sizeof(client_addr);
    auto client_fd =
        ::accept(NATIVE_SOCKET(fd_), reinterpret_cast<struct sockaddr*>(&client_addr), &addr_len);

    if (client_fd == static_cast<decltype(client_fd)>(kInvalidSocketHandle)) return std::nullopt;

    Socket client;
    client.fd_ = static_cast<decltype(client.fd_)>(client_fd);
    client.type_ = SocketType::TCP;
    return client;
}

bool Socket::connect(std::string_view address, uint16_t port) {
    if (fd_ == kInvalidSocketHandle) return false;

    struct sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    std::string addr_str(address);
    inet_pton(AF_INET, addr_str.c_str(), &addr.sin_addr);

    return ::connect(NATIVE_SOCKET(fd_), reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) == 0;
}

int Socket::send(const uint8_t* data, size_t length) {
    if (fd_ == kInvalidSocketHandle) return -1;
    return static_cast<int>(::send(NATIVE_SOCKET(fd_), reinterpret_cast<const char*>(data),
                                   static_cast<int>(length), 0));
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

    return static_cast<int>(::sendto(NATIVE_SOCKET(fd_), reinterpret_cast<const char*>(data),
                                     static_cast<int>(length), 0,
                                     reinterpret_cast<struct sockaddr*>(&addr),
                                     sizeof(addr)));
}

int Socket::receive(uint8_t* buffer, size_t buffer_size) {
    if (fd_ == kInvalidSocketHandle) return -1;
    return static_cast<int>(::recv(NATIVE_SOCKET(fd_), reinterpret_cast<char*>(buffer),
                                   static_cast<int>(buffer_size), 0));
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
        if (type_ == SocketType::TCP) {
            (void)SOCKET_SHUTDOWN(NATIVE_SOCKET(fd_));
        }
        SOCKET_CLOSE(NATIVE_SOCKET(fd_));
        fd_ = kInvalidSocketHandle;
    }
}

void Socket::shutdown() {
    if (fd_ != kInvalidSocketHandle && type_ == SocketType::TCP) {
        (void)SOCKET_SHUTDOWN(NATIVE_SOCKET(fd_));
    }
}

bool Socket::is_open() const {
    return fd_ != kInvalidSocketHandle;
}

uint16_t Socket::local_port() const {
    if (fd_ == kInvalidSocketHandle) return 0;

    struct sockaddr_in addr{};
    socklen_t addr_len = sizeof(addr);
    if (::getsockname(NATIVE_SOCKET(fd_), reinterpret_cast<struct sockaddr*>(&addr),
                      &addr_len) != 0) {
        return 0;
    }
    return ntohs(addr.sin_port);
}

}  // namespace pulp::runtime
