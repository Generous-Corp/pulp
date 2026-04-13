#pragma once

#include <string>
#include <vector>
#include <variant>
#include <cstdint>
#include <functional>
#include <memory>

namespace pulp::osc {

// ── OSC Types ────────────────────────────────────────────────────────────────

// OSC argument types (OSC 1.0 spec)
using Argument = std::variant<int32_t, float, std::string, std::vector<uint8_t>>;

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

    // Get argument at index (returns default if wrong type or out of range)
    int32_t get_int(size_t i, int32_t def = 0) const;
    float get_float(size_t i, float def = 0) const;
    std::string get_string(size_t i, const std::string& def = "") const;
};

// ── OSC Encoding/Decoding ────────────────────────────────────────────────────

// Encode a message to OSC binary format
std::vector<uint8_t> encode(const Message& msg);

// Decode an OSC binary packet into a message
// Returns nullopt if the data is malformed
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

class Receiver {
public:
    Receiver();
    ~Receiver();

    // Start listening on a port
    bool listen(uint16_t port, MessageHandler handler);

    // Stop listening
    void stop();

    bool is_listening() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace pulp::osc
