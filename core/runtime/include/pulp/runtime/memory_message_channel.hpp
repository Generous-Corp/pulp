#pragma once

// In-memory MessageChannel pair. Primarily for tests and for local
// message-driven code that wants to talk to itself (plugins ↔ inspector
// in the same process, for example) without a socket hop.
//
// Usage:
//   auto [a, b] = MemoryMessageChannel::make_pair();
//   a->send(...);      // delivered to b->on_message
//   b->send(...);      // delivered to a->on_message

#include <pulp/runtime/message_channel.hpp>

#include <atomic>
#include <memory>
#include <mutex>
#include <utility>

namespace pulp::runtime {

class MemoryMessageChannel : public MessageChannel {
public:
    /// Create a connected pair. Each side delivers sends to the other's
    /// on_message.
    static std::pair<std::unique_ptr<MemoryMessageChannel>,
                     std::unique_ptr<MemoryMessageChannel>>
    make_pair();

    ~MemoryMessageChannel() override;

    // MessageChannel
    bool send(const std::uint8_t* data, std::size_t size) override;
    bool send_text(std::string_view text) override;
    void on_message(MessageCallback callback) override;
    void on_closed(ChannelClosedCallback callback) override;
    void on_error(ChannelErrorCallback callback) override;
    void close() override;
    bool is_open() const override { return open_.load() && peer_alive(); }

private:
    MemoryMessageChannel() = default;

    bool peer_alive() const;
    void deliver(Message msg);

    std::weak_ptr<MemoryMessageChannel*> peer_;  // weak-pointer to a raw* so
                                                 // destruction breaks the link
    std::shared_ptr<MemoryMessageChannel*> self_;
    std::atomic<bool> open_{true};

    std::mutex mutex_;
    MessageCallback on_message_;
    ChannelClosedCallback on_closed_;
    ChannelErrorCallback on_error_;
};

}  // namespace pulp::runtime
