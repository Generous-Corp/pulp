#pragma once

// TCP, UDP, and OS-local socket abstraction.

#include <string>
#include <string_view>
#include <vector>
#include <optional>
#include <cstddef>
#include <cstdint>
#include <chrono>

namespace pulp::runtime {

enum class SocketType { TCP, UDP, Local };

/// Credentials reported by the kernel for the process on the other end of an
/// OS-local stream. These values are carrier evidence, not request metadata.
struct LocalPeerCredentials {
    std::uint64_t user_id = 0;
    std::uint64_t group_id = 0;
    std::int64_t process_id = 0;
    /// Kernel process-generation discriminator when the platform exposes one.
    /// Zero means unavailable and must not be accepted by a verifier that
    /// requires PID-reuse resistance.
    std::uint64_t process_generation_id = 0;
};

class Socket {
public:
    Socket() = default;
    ~Socket();

    /// Create a socket.
    bool create(SocketType type);

    /// Bind to a local address and port.
    bool bind(std::string_view address, uint16_t port);

    /// Bind an OS-local stream endpoint. The path must not already exist and
    /// its parent directory must already be private to the owning user.
    bool bind_local(std::string_view path);

    /// Listen for incoming TCP or OS-local stream connections.
    bool listen(int backlog = 5);

    /// Accept an incoming TCP or OS-local connection, blocking until one is
    /// available.
    std::optional<Socket> accept();

    /// Wait up to `timeout` for and accept an incoming connection. A
    /// non-positive timeout preserves the blocking behavior of accept().
    std::optional<Socket> accept(std::chrono::milliseconds timeout);

    /// Connect to a remote address (TCP). A positive timeout bounds the
    /// nonblocking connect phase; non-positive preserves blocking behavior.
    bool connect(std::string_view address, uint16_t port,
                 std::chrono::milliseconds timeout = {});

    /// Connect to an OS-local stream endpoint with the same bounded-connect
    /// semantics as TCP.
    bool connect_local(std::string_view path,
                       std::chrono::milliseconds timeout = {});

    /// Kernel-observed credentials for the connected OS-local peer. Unsupported
    /// transports and platforms return std::nullopt.
    std::optional<LocalPeerCredentials> local_peer_credentials() const;

    /// Send data. Returns bytes sent, or -1 on error.
    int send(const uint8_t* data, size_t length);
    int send(std::string_view data);

    /// Send UDP datagram to specific address.
    int send_to(const uint8_t* data, size_t length,
                std::string_view address, uint16_t port);

    /// Receive data. Returns bytes received, or -1 on error. 0 = connection closed.
    int receive(uint8_t* buffer, size_t buffer_size);

    /// Bound blocking send operations. A non-positive duration clears the
    /// deadline. Applies to subsequently issued operations.
    bool set_write_timeout(std::chrono::milliseconds timeout);

    /// Bound blocking receive operations. A non-positive duration clears the
    /// deadline. Applies to subsequently issued operations.
    bool set_read_timeout(std::chrono::milliseconds timeout);

    /// Receive UDP datagram. Returns bytes received and source address.
    int receive_from(uint8_t* buffer, size_t buffer_size,
                     std::string& from_address, uint16_t& from_port);

    /// Close the socket.
    void close();

    /// Interrupt blocking stream operations without closing the socket handle.
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
    std::string bound_local_path_;
    std::uint64_t bound_local_device_ = 0;
    std::uint64_t bound_local_inode_ = 0;

    bool connect_address(const void* address, std::size_t address_size,
                         std::chrono::milliseconds timeout);
};

}  // namespace pulp::runtime
