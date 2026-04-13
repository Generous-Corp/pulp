#pragma once

// OscChannel — adapts pulp::osc Sender/Receiver to the runtime
// MessageChannel contract.
//
// Each incoming UDP packet is delivered as a `Message{Binary, bytes}`.
// The bytes are the raw OSC-encoded packet; callers can decode with
// `pulp::osc::decode()` if they want the structured form. Outgoing
// messages are treated the same way — `send(bytes)` transmits the
// bytes verbatim, or the overload `send(const osc::Message&)` encodes
// first.
//
// This wrapper is not a replacement for `osc::Sender` / `osc::Receiver`
// — it lets higher-level code (JSON-RPC transport, remote-control,
// MCP bridges) treat OSC as "just another MessageChannel" so a single
// consumer can swap transports without branching.

#include <pulp/osc/osc.hpp>
#include <pulp/runtime/message_channel.hpp>

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>

namespace pulp::osc {

struct OscChannelOptions {
    /// Dispatch executor (same semantics as other channels). When empty,
    /// callbacks run on the Receiver's internal thread.
    pulp::runtime::MessageExecutor executor;
};

class OscChannel : public pulp::runtime::MessageChannel {
public:
    /// Construct a bidirectional channel: sender targets (host, remote_port)
    /// and the receiver listens on local_port. Returns nullptr if either
    /// endpoint fails to open.
    static std::unique_ptr<OscChannel> open(
        std::string_view host, std::uint16_t remote_port,
        std::uint16_t local_port,
        OscChannelOptions options = {});

    ~OscChannel() override;

    // MessageChannel
    bool send(const std::uint8_t* data, std::size_t size) override;
    void on_message(pulp::runtime::MessageCallback callback) override;
    void on_closed(pulp::runtime::ChannelClosedCallback callback) override;
    void on_error(pulp::runtime::ChannelErrorCallback callback) override;
    void close() override;
    bool is_open() const override { return open_.load(); }

    /// Convenience — encode and send a structured OSC message.
    bool send(const Message& msg);

private:
    OscChannel(OscChannelOptions options);

    void dispatch_message(const Message& msg);
    void dispatch(std::function<void()> fn);

    OscChannelOptions options_;
    Sender sender_;
    Receiver receiver_;
    std::atomic<bool> open_{false};
    std::atomic<bool> closed_fired_{false};

    std::mutex mutex_;
    pulp::runtime::MessageCallback on_message_;
    pulp::runtime::ChannelClosedCallback on_closed_;
    pulp::runtime::ChannelErrorCallback on_error_;
};

}  // namespace pulp::osc
