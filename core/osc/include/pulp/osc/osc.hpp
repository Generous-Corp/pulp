#pragma once

#include <string>
#include <vector>
#include <variant>
#include <cstdint>
#include <functional>
#include <memory>
#include <string_view>
#include <utility>

namespace pulp::osc {

// ── OSC Types ────────────────────────────────────────────────────────────────

/// 32-bit RGBA color — the OSC 1.0 optional `'r'` type tag.
/// Stored as four 8-bit channels in transmission order.

struct ColorRgba {
    uint8_t r = 0;
    uint8_t g = 0;
    uint8_t b = 0;
    uint8_t a = 0xff;

    constexpr ColorRgba() = default;
    constexpr ColorRgba(uint8_t r_, uint8_t g_, uint8_t b_, uint8_t a_ = 0xff)
        : r(r_), g(g_), b(b_), a(a_) {}

    bool operator==(const ColorRgba& o) const {
        return r == o.r && g == o.g && b == o.b && a == o.a;
    }
};

// OSC argument types (OSC 1.0 spec + the optional RGBA color type).
using Argument = std::variant<int32_t, float, std::string,
                              std::vector<uint8_t>, ColorRgba>;

// An OSC message: address pattern + typed arguments
struct Message {
    std::string address;          // e.g., "/synth/freq"
    std::vector<Argument> args;

    // Convenience constructors
    Message() = default;
    explicit Message(std::string addr) : address(std::move(addr)) {}

    Message& add(int32_t v)       { args.emplace_back(v); return *this; }
    Message& add(float v)         { args.emplace_back(v); return *this; }
    Message& add(std::string v)   { args.emplace_back(std::move(v)); return *this; }
    Message& add(std::vector<uint8_t> v) { args.emplace_back(std::move(v)); return *this; }
    Message& add(ColorRgba c)    { args.emplace_back(c); return *this; }

    // Get argument at index (returns default if wrong type or out of range)
    int32_t get_int(size_t i, int32_t def = 0) const;
    float get_float(size_t i, float def = 0) const;
    std::string get_string(size_t i, const std::string& def = "") const;
    ColorRgba get_colour(size_t i, ColorRgba def = {}) const;
};

struct Bundle;

// ── OSC Encoding/Decoding ────────────────────────────────────────────────────

// Encode a message to OSC binary format
std::vector<uint8_t> encode(const Message& msg);

// Decode an OSC binary packet into a message
// Returns an empty-address message if the data is malformed.
Message decode(const uint8_t* data, size_t size);

// ── OSC Sender ───────────────────────────────────────────────────────────────

// Sends OSC messages over UDP
class Sender {
public:
    Sender();
    ~Sender();

    // Connect to a target host:port
    bool connect(const std::string& host, uint16_t port);

    // Send a message
    bool send(const Message& msg);

    // Send a bundle
    bool send(const Bundle& bundle);

    // Send a raw, already-encoded OSC datagram (e.g., a #bundle or any
    // byte-exact payload the caller has pre-built). Returns true if the
    // full payload was sent.
    bool send_raw(const uint8_t* data, size_t size);

    // Disconnect
    void disconnect();

    bool is_connected() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// ── OSC Receiver ─────────────────────────────────────────────────────────────

// Receives OSC messages over UDP
using MessageHandler = std::function<void(const Message&)>;
using BundleHandler = std::function<void(const Bundle&)>;
using FormatErrorHandler = std::function<void(std::string_view)>;

struct ReceiverRoute {
    std::string address_pattern;
    MessageHandler handler;
};

struct ReceiverOptions {
    MessageHandler on_message;
    BundleHandler on_bundle;
    FormatErrorHandler on_error;
    std::vector<ReceiverRoute> routes;
};

class Receiver {
public:
    Receiver();
    ~Receiver();

    Receiver(const Receiver&) = delete;
    Receiver& operator=(const Receiver&) = delete;
    Receiver(Receiver&&) = delete;
    Receiver& operator=(Receiver&&) = delete;

    // Start listening on a port
    bool listen(uint16_t port, MessageHandler handler);

    // Start listening with bundle, error, and address-route callbacks.
    // Receiver callbacks may call stop(); shutdown short-circuits remaining
    // callbacks for the current datagram. Destroying a Receiver from its own
    // callback is supported; worker state stays alive until the callback and
    // receive thread finish unwinding.
    bool listen_with_options(uint16_t port, ReceiverOptions options);

    // Stop listening
    void stop();

    bool is_listening() const;
    uint16_t local_port() const;

private:
    void stop_impl(bool destroying);

    struct Impl;
    std::shared_ptr<Impl> impl_;
};

} // namespace pulp::osc
