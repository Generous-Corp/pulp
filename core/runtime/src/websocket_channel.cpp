#include <pulp/runtime/websocket_channel.hpp>
#include <pulp/runtime/base64.hpp>
#include <pulp/runtime/crypto.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>
#include <random>
#include <sstream>

namespace pulp::runtime {

namespace {

constexpr const char* kMagicGuid = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";

constexpr uint8_t kOpCont = 0x0;
constexpr uint8_t kOpText = 0x1;
constexpr uint8_t kOpBinary = 0x2;
constexpr uint8_t kOpClose = 0x8;
constexpr uint8_t kOpPing = 0x9;
constexpr uint8_t kOpPong = 0xA;

// Read exactly `n` bytes from the stream into `out`. Returns false on
// close or error. Loops over short reads — the Stream contract allows
// them and sockets frequently produce them.
bool read_exactly(Stream& s, std::uint8_t* out, std::size_t n) {
    std::size_t read = 0;
    while (read < n) {
        auto r = s.read(out + read, n - read);
        if (!r.ok()) {
            if (r.would_block()) continue;  // spin; simple but works for tests
            return false;
        }
        if (r.bytes == 0) return false;
        read += r.bytes;
    }
    return true;
}

bool write_all(Stream& s, const std::uint8_t* data, std::size_t n) {
    std::size_t written = 0;
    while (written < n) {
        auto r = s.write(data + written, n - written);
        if (!r.ok()) return false;
        if (r.bytes == 0) return false;
        written += r.bytes;
    }
    return true;
}

std::string make_client_key() {
    std::array<uint8_t, 16> nonce{};
    std::random_device rd;
    std::mt19937 eng(rd());
    for (auto& b : nonce) b = static_cast<uint8_t>(eng() & 0xFF);
    return base64_encode(nonce.data(), nonce.size());
}

std::string lowercase(std::string_view s) {
    std::string out(s);
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    return out;
}

/// Read an HTTP-style header block (until "\r\n\r\n"). Returns the raw
/// text. On failure returns std::nullopt.
std::optional<std::string> read_http_headers(Stream& s, std::size_t limit = 16 * 1024) {
    std::string buf;
    buf.reserve(512);
    std::uint8_t ch = 0;
    while (buf.size() < limit) {
        auto r = s.read(&ch, 1);
        if (!r.ok() || r.bytes == 0) return std::nullopt;
        buf.push_back(static_cast<char>(ch));
        if (buf.size() >= 4 &&
            buf.compare(buf.size() - 4, 4, "\r\n\r\n") == 0) {
            return buf;
        }
    }
    return std::nullopt;
}

std::optional<std::string> find_header(std::string_view block, std::string_view name) {
    auto lower_block = lowercase(block);
    auto lower_name = lowercase(name);
    std::size_t pos = 0;
    while (pos < lower_block.size()) {
        auto line_end = lower_block.find("\r\n", pos);
        if (line_end == std::string::npos) break;
        auto line = lower_block.substr(pos, line_end - pos);
        auto colon = line.find(':');
        if (colon != std::string::npos && line.substr(0, colon) == lower_name) {
            std::string_view original(block);
            auto value = original.substr(pos + colon + 1, line_end - pos - colon - 1);
            // trim leading/trailing whitespace
            while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front())))
                value.remove_prefix(1);
            while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back())))
                value.remove_suffix(1);
            return std::string(value);
        }
        pos = line_end + 2;
    }
    return std::nullopt;
}

}  // namespace

// ─── compute_accept_key ───────────────────────────────────────────────────

std::string WebSocketChannel::compute_accept_key(std::string_view client_key) {
    std::string concat(client_key);
    concat.append(kMagicGuid);
    auto digest = sha1(concat);
    return base64_encode(digest.data(), digest.size());
}

// ─── connect (client) ─────────────────────────────────────────────────────

std::unique_ptr<WebSocketChannel> WebSocketChannel::connect(
    std::unique_ptr<TcpStream> tcp,
    std::string_view host,
    std::string_view path,
    WebSocketOptions options) {
    if (!tcp || !tcp->is_open()) return nullptr;

    const std::string key = make_client_key();
    std::ostringstream req;
    req << "GET " << path << " HTTP/1.1\r\n"
        << "Host: " << host << "\r\n"
        << "Upgrade: websocket\r\n"
        << "Connection: Upgrade\r\n"
        << "Sec-WebSocket-Key: " << key << "\r\n"
        << "Sec-WebSocket-Version: 13\r\n\r\n";
    auto request = req.str();

    if (!write_all(*tcp, reinterpret_cast<const std::uint8_t*>(request.data()),
                   request.size())) {
        return nullptr;
    }

    auto resp = read_http_headers(*tcp);
    if (!resp) return nullptr;
    if (resp->find("101") == std::string::npos) return nullptr;

    auto accept = find_header(*resp, "Sec-WebSocket-Accept");
    if (!accept || *accept != compute_accept_key(key)) return nullptr;

    std::unique_ptr<WebSocketChannel> chan(
        new WebSocketChannel(std::move(tcp), Role::Client, std::move(options)));
    chan->open_.store(true);
    chan->reader_ = std::thread([raw = chan.get()] { raw->reader_main(); });
    return chan;
}

// ─── accept (server) ──────────────────────────────────────────────────────

std::unique_ptr<WebSocketChannel> WebSocketChannel::accept(
    std::unique_ptr<TcpStream> tcp,
    WebSocketOptions options) {
    if (!tcp || !tcp->is_open()) return nullptr;

    auto req = read_http_headers(*tcp);
    if (!req) return nullptr;

    auto upgrade = find_header(*req, "Upgrade");
    if (!upgrade || lowercase(*upgrade) != "websocket") return nullptr;

    auto key = find_header(*req, "Sec-WebSocket-Key");
    if (!key) return nullptr;

    const std::string accept_key = compute_accept_key(*key);
    std::ostringstream resp;
    resp << "HTTP/1.1 101 Switching Protocols\r\n"
         << "Upgrade: websocket\r\n"
         << "Connection: Upgrade\r\n"
         << "Sec-WebSocket-Accept: " << accept_key << "\r\n\r\n";
    auto response = resp.str();

    if (!write_all(*tcp, reinterpret_cast<const std::uint8_t*>(response.data()),
                   response.size())) {
        return nullptr;
    }

    std::unique_ptr<WebSocketChannel> chan(
        new WebSocketChannel(std::move(tcp), Role::Server, std::move(options)));
    chan->open_.store(true);
    chan->reader_ = std::thread([raw = chan.get()] { raw->reader_main(); });
    return chan;
}

// ─── instance ─────────────────────────────────────────────────────────────

WebSocketChannel::WebSocketChannel(std::unique_ptr<TcpStream> tcp, Role role,
                                   WebSocketOptions options)
    : tcp_(std::move(tcp)), role_(role), options_(std::move(options)) {}

WebSocketChannel::~WebSocketChannel() {
    close();
    if (reader_.joinable()) reader_.join();
    if (tcp_) tcp_->close();
}

void WebSocketChannel::on_message(MessageCallback cb) {
    std::lock_guard<std::mutex> lock(mutex_);
    on_message_ = std::move(cb);
}

void WebSocketChannel::on_closed(ChannelClosedCallback cb) {
    std::lock_guard<std::mutex> lock(mutex_);
    on_closed_ = std::move(cb);
}

void WebSocketChannel::on_error(ChannelErrorCallback cb) {
    std::lock_guard<std::mutex> lock(mutex_);
    on_error_ = std::move(cb);
}

bool WebSocketChannel::is_open() const { return open_.load(); }

void WebSocketChannel::close() {
    if (!open_.exchange(false)) {
        fire_closed();
        return;
    }
    if (tcp_) tcp_->shutdown();
    fire_closed();
}

bool WebSocketChannel::send(const std::uint8_t* data, std::size_t size) {
    if (!open_.load()) return false;
    return send_frame(kOpBinary, data, size);
}

bool WebSocketChannel::send_text(std::string_view text) {
    if (!open_.load()) return false;
    return send_frame(kOpText,
                      reinterpret_cast<const std::uint8_t*>(text.data()),
                      text.size());
}

bool WebSocketChannel::send_frame(uint8_t opcode, const std::uint8_t* data, std::size_t size) {
    std::lock_guard<std::mutex> lock(write_mutex_);
    if (!tcp_ || !tcp_->is_open()) return false;

    std::vector<std::uint8_t> hdr;
    hdr.reserve(14);
    hdr.push_back(static_cast<std::uint8_t>(0x80 | (opcode & 0x0F)));  // FIN=1

    const bool mask = (role_ == Role::Client);
    const std::uint8_t mask_bit = mask ? 0x80 : 0x00;

    if (size < 126) {
        hdr.push_back(mask_bit | static_cast<std::uint8_t>(size));
    } else if (size <= 0xFFFF) {
        hdr.push_back(mask_bit | 126);
        hdr.push_back(static_cast<std::uint8_t>((size >> 8) & 0xFF));
        hdr.push_back(static_cast<std::uint8_t>(size & 0xFF));
    } else {
        hdr.push_back(mask_bit | 127);
        for (int i = 7; i >= 0; --i) {
            hdr.push_back(static_cast<std::uint8_t>((static_cast<uint64_t>(size) >> (i * 8)) & 0xFF));
        }
    }

    std::array<std::uint8_t, 4> mask_key{};
    if (mask) {
        std::random_device rd;
        std::mt19937 eng(rd());
        for (auto& b : mask_key) b = static_cast<std::uint8_t>(eng() & 0xFF);
        hdr.insert(hdr.end(), mask_key.begin(), mask_key.end());
    }

    if (!write_all(*tcp_, hdr.data(), hdr.size())) return false;

    if (!mask) {
        return size == 0 || write_all(*tcp_, data, size);
    }

    // Client side: XOR with mask_key before writing. Stream in chunks to
    // bound stack/heap pressure for large frames.
    std::array<std::uint8_t, 4096> buf{};
    std::size_t off = 0;
    while (off < size) {
        const std::size_t chunk = std::min<std::size_t>(buf.size(), size - off);
        for (std::size_t i = 0; i < chunk; ++i) {
            buf[i] = data[off + i] ^ mask_key[(off + i) & 0x3];
        }
        if (!write_all(*tcp_, buf.data(), chunk)) return false;
        off += chunk;
    }
    return true;
}

void WebSocketChannel::reader_main() {
    std::vector<std::uint8_t> assembled;
    MessageKind assembled_kind = MessageKind::Binary;

    // Bound the TOTAL reassembled message size across continuation frames.
    // `max_payload` bounds one frame; without this a peer streaming endless
    // non-final fragments grows `assembled` without limit (memory-exhaustion
    // DoS). On overflow, mirror the shutdown idiom used by the default-opcode
    // case: fire an error, mark the channel closed, and let the while-loop
    // condition tear the reader down.
    auto append_bounded = [&](const std::vector<std::uint8_t>& next) -> bool {
        if (next.size() > options_.max_message - assembled.size()) {
            fire_error("reassembled message exceeds max_message");
            open_.store(false);
            return false;
        }
        assembled.insert(assembled.end(), next.begin(), next.end());
        return true;
    };

    while (open_.load()) {
        std::uint8_t hdr[2];
        if (!read_exactly(*tcp_, hdr, 2)) {
            if (open_.exchange(false)) fire_error("read header failed");
            break;
        }
        const bool fin = (hdr[0] & 0x80) != 0;
        const std::uint8_t opcode = hdr[0] & 0x0F;
        const bool masked = (hdr[1] & 0x80) != 0;
        std::uint64_t len = hdr[1] & 0x7F;

        if (len == 126) {
            std::uint8_t ext[2];
            if (!read_exactly(*tcp_, ext, 2)) {
                if (open_.exchange(false)) fire_error("read len16");
                break;
            }
            len = (std::uint64_t(ext[0]) << 8) | ext[1];
        } else if (len == 127) {
            std::uint8_t ext[8];
            if (!read_exactly(*tcp_, ext, 8)) {
                if (open_.exchange(false)) fire_error("read len64");
                break;
            }
            len = 0;
            for (int i = 0; i < 8; ++i) len = (len << 8) | ext[i];
        }

        if (len > options_.max_payload) {
            fire_error("payload exceeds max_payload");
            break;
        }

        std::array<std::uint8_t, 4> mask_key{};
        if (masked) {
            if (!read_exactly(*tcp_, mask_key.data(), 4)) {
                if (open_.exchange(false)) fire_error("read mask");
                break;
            }
        }

        std::vector<std::uint8_t> payload(len);
        if (len > 0 && !read_exactly(*tcp_, payload.data(), len)) {
            if (open_.exchange(false)) fire_error("read payload");
            break;
        }
        if (masked) {
            for (std::size_t i = 0; i < payload.size(); ++i) {
                payload[i] ^= mask_key[i & 0x3];
            }
        }

        switch (opcode) {
            case kOpClose:
                // Echo a close frame and shut down.
                send_frame(kOpClose, payload.data(), payload.size());
                open_.store(false);
                break;
            case kOpPing:
                send_frame(kOpPong, payload.data(), payload.size());
                break;
            case kOpPong:
                break;  // nothing to do
            case kOpText:
            case kOpBinary: {
                if (opcode == kOpText) assembled_kind = MessageKind::Text;
                else assembled_kind = MessageKind::Binary;
                if (fin) {
                    if (!assembled.empty()) {
                        if (!append_bounded(payload)) break;
                        payload.swap(assembled);
                        assembled.clear();
                    }
                    MessageCallback cb;
                    {
                        std::lock_guard<std::mutex> lock(mutex_);
                        cb = on_message_;
                    }
                    if (cb) {
                        Message m{assembled_kind, std::move(payload)};
                        dispatch([cb = std::move(cb), m = std::move(m)]() mutable {
                            cb(m);
                        });
                    }
                } else {
                    if (!append_bounded(payload)) break;
                }
                break;
            }
            case kOpCont:
                if (fin && !assembled.empty()) {
                    if (!append_bounded(payload)) break;
                    MessageCallback cb;
                    {
                        std::lock_guard<std::mutex> lock(mutex_);
                        cb = on_message_;
                    }
                    if (cb) {
                        Message m{assembled_kind, std::move(assembled)};
                        assembled.clear();
                        dispatch([cb = std::move(cb), m = std::move(m)]() mutable {
                            cb(m);
                        });
                    }
                } else {
                    if (!append_bounded(payload)) break;
                }
                break;
            default:
                fire_error("unknown opcode");
                open_.store(false);
                break;
        }
    }

    open_.store(false);
    fire_closed();
}

void WebSocketChannel::fire_error(std::string_view reason) {
    ChannelErrorCallback cb;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        cb = on_error_;
    }
    if (cb) {
        std::string copy(reason);
        dispatch([cb = std::move(cb), copy = std::move(copy)] { cb(copy); });
    }
}

void WebSocketChannel::fire_closed() {
    if (closed_fired_.exchange(true)) return;
    ChannelClosedCallback cb;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        cb = on_closed_;
    }
    if (cb) dispatch(std::move(cb));
}

void WebSocketChannel::dispatch(std::function<void()> task) {
    if (options_.executor) options_.executor(std::move(task));
    else task();
}

}  // namespace pulp::runtime
