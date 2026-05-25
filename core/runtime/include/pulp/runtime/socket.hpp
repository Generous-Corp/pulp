#pragma once

// TCP and UDP socket abstraction.

#include <string>
#include <string_view>
#include <vector>
#include <optional>
#include <cstdint>

namespace pulp::runtime {

enum class SocketType { TCP, UDP };

class Socket {
public:
    Socket() = default;
    ~Socket();

    /// Create a socket.
    bool create(SocketType type);

    /// Bind to a local address and port.
    bool bind(std::string_view address, uint16_t port);

    /// Listen for incoming connections (TCP only).
    bool listen(int backlog = 5);

    /// Accept an incoming connection (TCP only). Returns a new Socket.
    std::optional<Socket> accept();

    /// Connect to a remote address (TCP).
    bool connect(std::string_view address, uint16_t port);

    /// Send data. Returns bytes sent, or -1 on error.
    int send(const uint8_t* data, size_t length);
    int send(std::string_view data);

    /// Send UDP datagram to specific address.
    int send_to(const uint8_t* data, size_t length,
                std::string_view address, uint16_t port);

    /// Receive data. Returns bytes received, or -1 on error. 0 = connection closed.
    int receive(uint8_t* buffer, size_t buffer_size);

    /// Receive UDP datagram. Returns bytes received and source address.
    int receive_from(uint8_t* buffer, size_t buffer_size,
                     std::string& from_address, uint16_t& from_port);

    /// Close the socket.
    void close();

    /// Interrupt blocking TCP operations without closing the socket handle.
    void shutdown();

    /// Whether the socket is open.
    bool is_open() const;

    /// Bound local port, or 0 when unavailable/unbound.
    uint16_t local_port() const;

    // No copy, move OK
    Socket(const Socket&) = delete;
    Socket& operator=(const Socket&) = delete;
    Socket(Socket&& other) noexcept;
    Socket& operator=(Socket&& other) noexcept;

private:
#ifdef _WIN32
    std::uintptr_t fd_ = ~std::uintptr_t{0};
#else
    int fd_ = -1;
#endif
    SocketType type_ = SocketType::TCP;
};

}  // namespace pulp::runtime
