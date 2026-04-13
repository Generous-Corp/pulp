#include <pulp/osc/osc_channel.hpp>

#include <utility>

namespace pulp::osc {

std::unique_ptr<OscChannel> OscChannel::open(
    std::string_view host, std::uint16_t remote_port,
    std::uint16_t local_port,
    OscChannelOptions options) {
    std::unique_ptr<OscChannel> chan(new OscChannel(std::move(options)));

    if (!chan->sender_.connect(std::string(host), remote_port)) {
        return nullptr;
    }
    const bool listen_ok = chan->receiver_.listen(local_port,
        [raw = chan.get()](const Message& m) { raw->dispatch_message(m); });
    if (!listen_ok) {
        chan->sender_.disconnect();
        return nullptr;
    }
    chan->open_.store(true);
    return chan;
}

OscChannel::OscChannel(OscChannelOptions options) : options_(std::move(options)) {}

OscChannel::~OscChannel() { close(); }

bool OscChannel::send(const std::uint8_t* data, std::size_t size) {
    if (!open_.load() || !sender_.is_connected() || size == 0) return false;
    // Preserve the caller's bytes verbatim — bundles (#bundle), custom
    // encodings, or payloads this module doesn't yet decode must pass
    // through unmodified. Structured sends can use the Message overload.
    return sender_.send_raw(data, size);
}

bool OscChannel::send(const Message& msg) {
    if (!open_.load() || !sender_.is_connected()) return false;
    return sender_.send(msg);
}

void OscChannel::on_message(pulp::runtime::MessageCallback cb) {
    std::lock_guard<std::mutex> lock(mutex_);
    on_message_ = std::move(cb);
}

void OscChannel::on_closed(pulp::runtime::ChannelClosedCallback cb) {
    std::lock_guard<std::mutex> lock(mutex_);
    on_closed_ = std::move(cb);
}

void OscChannel::on_error(pulp::runtime::ChannelErrorCallback cb) {
    std::lock_guard<std::mutex> lock(mutex_);
    on_error_ = std::move(cb);
}

void OscChannel::close() {
    if (!open_.exchange(false)) {
        if (!closed_fired_.exchange(true)) {
            pulp::runtime::ChannelClosedCallback cb;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                cb = on_closed_;
            }
            if (cb) dispatch(std::move(cb));
        }
        return;
    }
    receiver_.stop();
    sender_.disconnect();
    if (!closed_fired_.exchange(true)) {
        pulp::runtime::ChannelClosedCallback cb;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            cb = on_closed_;
        }
        if (cb) dispatch(std::move(cb));
    }
}

void OscChannel::dispatch_message(const Message& msg) {
    auto bytes = encode(msg);
    pulp::runtime::Message m{
        pulp::runtime::MessageKind::Binary,
        std::move(bytes)
    };
    pulp::runtime::MessageCallback cb;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        cb = on_message_;
    }
    if (cb) {
        dispatch([cb = std::move(cb), m = std::move(m)] { cb(m); });
    }
}

void OscChannel::dispatch(std::function<void()> fn) {
    if (options_.executor) options_.executor(std::move(fn));
    else fn();
}

}  // namespace pulp::osc
