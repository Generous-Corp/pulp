#pragma once

// Network Stream backends: TCP sockets and HTTP response bodies.
//
// These wrap the existing `Socket` and `http_get`/`http_post` helpers so
// that higher-level code can treat them uniformly with `FileStream`,
// `MemoryStream`, and `PipeStream` — and, layered under `AsyncStream`,
// gain non-blocking semantics without any network-specific plumbing.

#include <pulp/runtime/stream.hpp>
#include <pulp/runtime/socket.hpp>
#include <pulp/runtime/http.hpp>

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace pulp::runtime {

/// Stream backed by a connected TCP `Socket`.
///
/// Construction is deliberately two-phase: `connect(host, port)` performs
/// a blocking DNS + TCP handshake. Wrap in an `AsyncStream` to make it
/// non-blocking for the caller.
class TcpStream : public Stream {
public:
    TcpStream() = default;
    explicit TcpStream(Socket&& connected);
    ~TcpStream() override;

    TcpStream(TcpStream&&) noexcept;
    TcpStream& operator=(TcpStream&&) noexcept;

    bool connect(std::string_view host, std::uint16_t port);

    StreamResult read(std::uint8_t* buffer, std::size_t size) override;
    StreamResult write(const std::uint8_t* buffer, std::size_t size) override;
    void close() override;
    bool is_open() const override { return open_; }

    /// Access the underlying socket (advanced use: set options, inspect fd).
    Socket& socket() { return socket_; }

private:
    Socket socket_;
    bool open_ = false;
};

/// HTTP response as a readable Stream. The request is issued eagerly in the
/// constructor (or via `get`/`post`) and the body is exposed through the
/// Stream `read()` contract. Partial reads return chunks from the body
/// buffer; `status_code()` and `headers()` are available once construction
/// completes.
///
/// HttpStream is currently read-only. Writing to an HttpStream is a
/// Phase 4 feature (request-body streaming) and returns `Invalid` until
/// then.
class HttpStream : public Stream {
public:
    struct Request {
        std::string url;
        std::string method = "GET";      ///< "GET" or "POST".
        std::string body;                ///< Only used for POST.
        std::string content_type = "application/json";
        int timeout_seconds = 30;
    };

    HttpStream() = default;
    explicit HttpStream(const Request& request);

    /// Issue a new request, replacing any prior state.
    bool fetch(const Request& request);

    /// Convenience factory for GET requests.
    static std::unique_ptr<HttpStream> get(std::string_view url,
                                           int timeout_seconds = 30);

    /// Convenience factory for POST requests.
    static std::unique_ptr<HttpStream> post(std::string_view url,
                                            std::string_view body,
                                            std::string_view content_type = "application/json",
                                            int timeout_seconds = 30);

    StreamResult read(std::uint8_t* buffer, std::size_t size) override;
    StreamResult write(const std::uint8_t* buffer, std::size_t size) override;
    void close() override;
    bool is_open() const override;

    /// HTTP status code of the response, or 0 if the request failed.
    int status_code() const { return response_.status_code; }

    /// Parsed response headers (lowercased keys are *not* normalized — they
    /// come through verbatim from cpp-httplib).
    const std::map<std::string, std::string>& headers() const { return response_.headers; }

    /// Non-empty when the request failed locally (network error, DNS, etc.).
    const std::string& transport_error() const { return response_.error; }

    /// True when the HTTP response body was fully consumed by read().
    bool eof() const;

private:
    HttpResponse response_;
    std::size_t read_pos_ = 0;
    bool closed_ = false;
};

}  // namespace pulp::runtime
