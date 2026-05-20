#include <pulp/runtime/memory_message_channel.hpp>

namespace pulp::runtime {

std::pair<std::unique_ptr<MemoryMessageChannel>,
          std::unique_ptr<MemoryMessageChannel>>
MemoryMessageChannel::make_pair() {
    auto a = std::unique_ptr<MemoryMessageChannel>(new MemoryMessageChannel);
    auto b = std::unique_ptr<MemoryMessageChannel>(new MemoryMessageChannel);
    a->self_ = std::make_shared<MemoryMessageChannel*>(a.get());
    b->self_ = std::make_shared<MemoryMessageChannel*>(b.get());
    a->peer_ = b->self_;
    b->peer_ = a->self_;
    return {std::move(a), std::move(b)};
}

MemoryMessageChannel::~MemoryMessageChannel() {
    close();
}

bool MemoryMessageChannel::peer_alive() const {
    auto p = peer_.lock();
    return p && *p != nullptr;
}

bool MemoryMessageChannel::send(const std::uint8_t* data, std::size_t size) {
    if (!open_.load()) return false;
    if (data == nullptr && size > 0) return false;
    auto p = peer_.lock();
    if (!p || *p == nullptr) return false;
    Message m{MessageKind::Binary, {}};
    if (size > 0)
        m.payload.assign(data, data + size);
    (*p)->deliver(std::move(m));
    return true;
}

bool MemoryMessageChannel::send_text(std::string_view text) {
    if (!open_.load()) return false;
    auto p = peer_.lock();
    if (!p || *p == nullptr) return false;
    Message m{MessageKind::Text,
              std::vector<std::uint8_t>(
                  reinterpret_cast<const std::uint8_t*>(text.data()),
                  reinterpret_cast<const std::uint8_t*>(text.data() + text.size()))};
    (*p)->deliver(std::move(m));
    return true;
}

void MemoryMessageChannel::deliver(Message msg) {
    MessageCallback cb;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        cb = on_message_;
    }
    if (cb) cb(msg);
}

void MemoryMessageChannel::on_message(MessageCallback cb) {
    std::lock_guard<std::mutex> lock(mutex_);
    on_message_ = std::move(cb);
}

void MemoryMessageChannel::on_closed(ChannelClosedCallback cb) {
    std::lock_guard<std::mutex> lock(mutex_);
    on_closed_ = std::move(cb);
}

void MemoryMessageChannel::on_error(ChannelErrorCallback cb) {
    std::lock_guard<std::mutex> lock(mutex_);
    on_error_ = std::move(cb);
}

void MemoryMessageChannel::close() {
    if (!open_.exchange(false)) return;
    // Break the pointer so peer.send() fails cleanly.
    if (self_) *self_ = nullptr;

    ChannelClosedCallback self_closed;
    ChannelClosedCallback peer_closed;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        self_closed = std::move(on_closed_);
    }

    // Transition the peer to closed too and consume its callback so that
    // peer->close() (which frequently happens via the destructor) does
    // not fire on_closed a second time — ChannelClosedCallback is
    // contractually called exactly once per channel.
    if (auto p = peer_.lock(); p && *p) {
        MemoryMessageChannel* peer = *p;
        peer->open_.store(false);
        {
            std::lock_guard<std::mutex> lock(peer->mutex_);
            peer_closed = std::move(peer->on_closed_);
        }
    }
    if (self_closed) self_closed();
    if (peer_closed) peer_closed();
}

}  // namespace pulp::runtime
