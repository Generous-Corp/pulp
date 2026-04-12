#include <pulp/runtime/network_stream.hpp>

#include <algorithm>
#include <cstring>
#include <utility>

namespace pulp::runtime {

// ─── TcpStream ────────────────────────────────────────────────────────────

TcpStream::TcpStream(Socket&& connected)
    : socket_(std::move(connected)), open_(true) {}

TcpStream::~TcpStream() { close(); }

TcpStream::TcpStream(TcpStream&& other) noexcept
    : socket_(std::move(other.socket_)), open_(other.open_) {
    other.open_ = false;
}

TcpStream& TcpStream::operator=(TcpStream&& other) noexcept {
    if (this != &other) {
        close();
        socket_ = std::move(other.socket_);
        open_ = other.open_;
        other.open_ = false;
    }
    return *this;
}

bool TcpStream::connect(std::string_view host, std::uint16_t port) {
    close();
    if (!socket_.create(SocketType::TCP)) return false;
    if (!socket_.connect(host, port)) {
        socket_.close();
        return false;
    }
    open_ = true;
    return true;
}

StreamResult TcpStream::read(std::uint8_t* buffer, std::size_t size) {
    if (!open_) return StreamResult::fail(StreamError::Closed);
    if (size == 0) return StreamResult::make(0);

    int n = socket_.receive(buffer, size);
    if (n < 0) return StreamResult::fail(StreamError::IoError);
    if (n == 0) {
        open_ = false;
        return StreamResult::fail(StreamError::Closed);
    }
    return StreamResult::make(static_cast<std::size_t>(n));
}

StreamResult TcpStream::write(const std::uint8_t* buffer, std::size_t size) {
    if (!open_) return StreamResult::fail(StreamError::Closed);
    if (size == 0) return StreamResult::make(0);

    int n = socket_.send(buffer, size);
    if (n < 0) return StreamResult::fail(StreamError::IoError);
    return StreamResult::make(static_cast<std::size_t>(n));
}

void TcpStream::close() {
    if (open_) {
        socket_.close();
        open_ = false;
    }
}

// ─── HttpStream ───────────────────────────────────────────────────────────

HttpStream::HttpStream(const Request& request) {
    fetch(request);
}

bool HttpStream::fetch(const Request& request) {
    read_pos_ = 0;
    closed_ = false;

    if (request.method == "POST") {
        response_ = http_post(request.url, request.body, request.content_type,
                              request.timeout_seconds);
    } else {
        response_ = http_get(request.url, request.timeout_seconds);
    }
    return response_.error.empty() && response_.status_code != 0;
}

std::unique_ptr<HttpStream> HttpStream::get(std::string_view url, int timeout_seconds) {
    Request r;
    r.url.assign(url);
    r.timeout_seconds = timeout_seconds;
    return std::make_unique<HttpStream>(r);
}

std::unique_ptr<HttpStream> HttpStream::post(std::string_view url,
                                             std::string_view body,
                                             std::string_view content_type,
                                             int timeout_seconds) {
    Request r;
    r.url.assign(url);
    r.method = "POST";
    r.body.assign(body);
    r.content_type.assign(content_type);
    r.timeout_seconds = timeout_seconds;
    return std::make_unique<HttpStream>(r);
}

StreamResult HttpStream::read(std::uint8_t* buffer, std::size_t size) {
    if (closed_) return StreamResult::fail(StreamError::Closed);
    if (size == 0) return StreamResult::make(0);
    if (!response_.error.empty()) return StreamResult::fail(StreamError::IoError);

    auto total = response_.body.size();
    if (read_pos_ >= total) return StreamResult::fail(StreamError::Closed);

    auto available = total - read_pos_;
    auto n = std::min(size, available);
    std::memcpy(buffer, response_.body.data() + read_pos_, n);
    read_pos_ += n;
    return StreamResult::make(n);
}

StreamResult HttpStream::write(const std::uint8_t*, std::size_t) {
    // HttpStream is read-only (response body). Request-body streaming is
    // Phase 4 of the stream feature plan.
    return StreamResult::fail(StreamError::Invalid);
}

void HttpStream::close() { closed_ = true; }

bool HttpStream::is_open() const {
    return !closed_ && response_.error.empty() && read_pos_ < response_.body.size();
}

bool HttpStream::eof() const {
    return read_pos_ >= response_.body.size();
}

}  // namespace pulp::runtime
