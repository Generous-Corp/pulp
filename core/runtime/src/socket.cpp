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
#define SOCKET_CLOSE closesocket
#define SOCKET_SHUTDOWN(fd) ::shutdown((fd), SD_BOTH)
#define SOCKET_INVALID INVALID_SOCKET
#else
#define SOCKET_CLOSE ::close
#define SOCKET_SHUTDOWN(fd) ::shutdown((fd), SHUT_RDWR)
#define SOCKET_INVALID (-1)
#endif

Socket::~Socket() { close(); }

Socket::Socket(Socket&& other) noexcept : fd_(other.fd_), type_(other.type_) {
    other.fd_ = -1;
}

Socket& Socket::operator=(Socket&& other) noexcept {
    if (this != &other) {
        close();
        fd_ = other.fd_;
        type_ = other.type_;
        other.fd_ = -1;
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
    fd_ = static_cast<int>(::socket(AF_INET, sock_type, protocol));
    return fd_ != SOCKET_INVALID;
}

bool Socket::bind(std::string_view address, uint16_t port) {
    if (fd_ == SOCKET_INVALID) return false;

    struct sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    std::string addr_str(address);
    if (addr_str.empty() || addr_str == "0.0.0.0")
        addr.sin_addr.s_addr = INADDR_ANY;
    else
        inet_pton(AF_INET, addr_str.c_str(), &addr.sin_addr);

    int opt = 1;
    setsockopt(fd_, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&opt), sizeof(opt));

    return ::bind(fd_, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) == 0;
}

bool Socket::listen(int backlog) {
    if (fd_ == SOCKET_INVALID) return false;
    return ::listen(fd_, backlog) == 0;
}

std::optional<Socket> Socket::accept() {
    if (fd_ == SOCKET_INVALID) return std::nullopt;

    struct sockaddr_in client_addr{};
    socklen_t addr_len = sizeof(client_addr);
    int client_fd = static_cast<int>(
        ::accept(fd_, reinterpret_cast<struct sockaddr*>(&client_addr), &addr_len));

    if (client_fd == SOCKET_INVALID) return std::nullopt;

    Socket client;
    client.fd_ = client_fd;
    client.type_ = SocketType::TCP;
    return client;
}

bool Socket::set_non_blocking(bool non_blocking) {
    if (fd_ == SOCKET_INVALID) return false;

#ifdef _WIN32
    u_long mode = non_blocking ? 1UL : 0UL;
    return ::ioctlsocket(fd_, FIONBIO, &mode) == 0;
#else
    int flags = ::fcntl(fd_, F_GETFL, 0);
    if (flags == -1) return false;
    if (non_blocking)
        flags |= O_NONBLOCK;
    else
        flags &= ~O_NONBLOCK;
    return ::fcntl(fd_, F_SETFL, flags) == 0;
#endif
}

bool Socket::connect(std::string_view address, uint16_t port) {
    if (fd_ == SOCKET_INVALID) return false;

    struct sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    std::string addr_str(address);
    inet_pton(AF_INET, addr_str.c_str(), &addr.sin_addr);

    return ::connect(fd_, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) == 0;
}

int Socket::send(const uint8_t* data, size_t length) {
    if (fd_ == SOCKET_INVALID) return -1;
    return static_cast<int>(::send(fd_, reinterpret_cast<const char*>(data),
                                   static_cast<int>(length), 0));
}

int Socket::send(std::string_view data) {
    return send(reinterpret_cast<const uint8_t*>(data.data()), data.size());
}

int Socket::send_to(const uint8_t* data, size_t length,
                    std::string_view address, uint16_t port) {
    if (fd_ == SOCKET_INVALID) return -1;

    struct sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    std::string addr_str(address);
    inet_pton(AF_INET, addr_str.c_str(), &addr.sin_addr);

    return static_cast<int>(::sendto(fd_, reinterpret_cast<const char*>(data),
                                     static_cast<int>(length), 0,
                                     reinterpret_cast<struct sockaddr*>(&addr),
                                     sizeof(addr)));
}

int Socket::receive(uint8_t* buffer, size_t buffer_size) {
    if (fd_ == SOCKET_INVALID) return -1;
    return static_cast<int>(::recv(fd_, reinterpret_cast<char*>(buffer),
                                   static_cast<int>(buffer_size), 0));
}

int Socket::receive_from(uint8_t* buffer, size_t buffer_size,
                         std::string& from_address, uint16_t& from_port) {
    if (fd_ == SOCKET_INVALID) return -1;

    struct sockaddr_in addr{};
    socklen_t addr_len = sizeof(addr);
    int bytes = static_cast<int>(::recvfrom(fd_, reinterpret_cast<char*>(buffer),
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
    if (fd_ != SOCKET_INVALID) {
        if (type_ == SocketType::TCP) {
            (void)SOCKET_SHUTDOWN(fd_);
        }
        SOCKET_CLOSE(fd_);
        fd_ = -1;
    }
}

bool Socket::is_open() const {
    return fd_ != SOCKET_INVALID;
}

}  // namespace pulp::runtime
