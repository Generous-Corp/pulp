#pragma once

// CV To OSC — watch a control voltage, and tell something about it over OSC.
//
// An insert, not a generator: the input passes through to the output bit-exactly,
// so this sits anywhere in a chain without changing the voltage that reaches the
// jack. What it adds is a side-channel — the value of each channel, sent as an
// OSC float to `127.0.0.1` on a port you choose.
//
// It is **off by default**, and that is a safety property rather than a style
// choice. A plug-in that begins emitting UDP the moment a project loads will trip
// a firewall prompt on some machines and quietly stream a user's patch on others.
// Nothing leaves this process until the switch is on.
//
// The send happens on a background thread. See brew/cv_osc.hpp for why the audio
// thread must not touch a socket. The audio thread's entire contribution is one
// relaxed atomic store per channel per block.

#include <brew/cv.hpp>
#include <brew/cv_osc.hpp>

#include <pulp/format/processor.hpp>
#include <pulp/osc/osc.hpp>
#include <pulp/state/store.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>

namespace pulp::examples::brew {

/// Where a message goes. Injected so the tests can pin what is sent without
/// binding a socket, and so the one test that *does* bind a socket is the only
/// one that can be flaky.
class MessageSink {
public:
    virtual ~MessageSink() = default;
    virtual bool connect(const std::string& host, std::uint16_t port) = 0;
    virtual bool send(const osc::Message& msg) = 0;
};

/// The real one.
class UdpSink final : public MessageSink {
public:
    bool connect(const std::string& host, std::uint16_t port) override {
        return sender_.connect(host, port);
    }
    bool send(const osc::Message& msg) override { return sender_.send(msg); }

private:
    osc::Sender sender_;
};

inline constexpr std::size_t kOscChannels = 2;

class CvOscProcessor : public format::Processor {
public:
    // Parameter IDs are part of the persisted state contract. Never renumber.
    enum ParamId : state::ParamID {
        kEnabled = 1,
        kPort = 2,
        kRateHz = 3,
        kDeadband = 4,
    };

    explicit CvOscProcessor(std::unique_ptr<MessageSink> sink = nullptr)
        : sink_(sink ? std::move(sink) : std::make_unique<UdpSink>()) {}

    ~CvOscProcessor() override { stop_sender(); }

    format::PluginDescriptor descriptor() const override {
        return {
            .name = "CV To OSC",
            .manufacturer = "Bitches Brew",
            .bundle_id = "com.bitchesbrew.cvosc",
            .version = "0.1.0",
            .category = format::PluginCategory::Effect,
            .input_buses = {{"CV In", 2}},
            .output_buses = {{"CV Thru", 2}},
        };
    }

    void define_parameters(state::StateStore& store) override {
        // Off until asked. Nothing leaves the process before this is on.
        store.add_parameter({.id = kEnabled,
                             .name = "Send",
                             .unit = "",
                             .range = {0.0f, 1.0f, 0.0f, 1.0f}});
        // Above the privileged range: binding below 1024 needs root, and a CV
        // plug-in has no business asking for it.
        store.add_parameter({.id = kPort,
                             .name = "Port",
                             .unit = "",
                             .range = {1024.0f, 65535.0f, 9000.0f, 1.0f}});
        store.add_parameter({.id = kRateHz,
                             .name = "Rate",
                             .unit = "Hz",
                             .range = {kMinRateHz, kMaxRateHz, 60.0f, 1.0f}});
        // Movement smaller than this is not worth a packet.
        store.add_parameter({.id = kDeadband,
                             .name = "Deadband",
                             .unit = "",
                             .range = {0.0f, 0.5f, 0.001f, 0.0001f}});
    }

    void prepare(const format::PrepareContext&) override { start_sender(); }

    std::pair<uint32_t, uint32_t> editor_size() const override {
        return {360, 274};
    }

    std::unique_ptr<view::View> create_view() override;

    /// The last sample seen on a channel. Read by the editor and by the sender.
    [[nodiscard]] float latest(std::size_t channel) const noexcept {
        return channel < kOscChannels
                   ? latest_[channel].load(std::memory_order_relaxed)
                   : 0.0f;
    }

    /// Messages actually handed to the sink since construction. The editor's lamp
    /// reads it — a counter that never moves is how a user learns the receiver
    /// is not listening, or that they never turned Send on.
    [[nodiscard]] std::uint64_t sent_count() const noexcept {
        return sent_count_.load(std::memory_order_relaxed);
    }

    /// Publish one tick's worth of values. Exposed so a test can drive the send
    /// logic deterministically instead of waiting on a thread's clock — which
    /// means it can run concurrently with the sender thread, so the send state it
    /// mutates is locked. Never called from the audio thread.
    void publish_once() {
        std::lock_guard<std::mutex> lock(send_mutex_);
        if (!as_toggle(state().get_value(kEnabled))) return;

        const auto port = static_cast<std::uint16_t>(
            std::lround(state().get_value(kPort)));
        if (port != connected_port_) {
            if (!sink_->connect(kOscHost, port)) return;
            connected_port_ = port;
            // A new destination has never heard the resting value.
            for (auto& f : first_send_) f = true;
        }

        const float deadband = state().get_value(kDeadband);
        for (std::size_t c = 0; c < kOscChannels; ++c) {
            const float v = latest(c);
            if (!should_send(first_send_[c], last_sent_[c], v, deadband)) continue;
            osc::Message msg(osc_address(c));
            msg.add(v);
            if (!sink_->send(msg)) continue;   // a dropped send is not a state change
            last_sent_[c] = v;
            first_send_[c] = false;
            sent_count_.fetch_add(1, std::memory_order_relaxed);
        }
    }

    void process(audio::BufferView<float>& output,
                 const audio::BufferView<const float>& input,
                 midi::MidiBuffer&,
                 midi::MidiBuffer&,
                 const format::ProcessContext& ctx) override {
        const std::size_t out_channels = output.num_channels();
        const std::size_t frames = output.num_samples();
        if (out_channels == 0 || frames == 0) return;

        const std::size_t shared = std::min(out_channels, input.num_channels());

        // A wire. An insert's bypass is a pass-through, not a mute — and its
        // non-bypassed path is the same pass-through, because this plug-in exists
        // to observe a voltage rather than to change one.
        for (std::size_t c = 0; c < shared; ++c) {
            const float* src = input.channel_ptr(c);
            float* dst = output.channel_ptr(c);
            if (src == nullptr || dst == nullptr) continue;
            for (std::size_t n = 0; n < frames; ++n) dst[n] = src[n];
        }
        for (std::size_t c = shared; c < out_channels; ++c)
            if (float* dst = output.channel_ptr(c))
                for (std::size_t n = 0; n < frames; ++n) dst[n] = 0.0f;

        // Bypassed means out of the circuit: keep passing the voltage, stop
        // reporting on it. Freezing the published value instead would leave the
        // receiver believing a stale reading is current.
        if (ctx.is_bypassed) return;

        // The audio thread's entire contribution. One relaxed store per channel:
        // no syscall, no allocation, no lock.
        for (std::size_t c = 0; c < kOscChannels; ++c) {
            const float* src = c < shared ? input.channel_ptr(c) : nullptr;
            latest_[c].store(src ? src[frames - 1] : 0.0f,
                             std::memory_order_relaxed);
        }
    }

private:
    void start_sender() {
        if (thread_.joinable()) return;
        running_.store(true, std::memory_order_relaxed);
        thread_ = std::thread([this] { sender_loop(); });
    }

    void stop_sender() {
        if (!thread_.joinable()) return;
        {
            std::lock_guard<std::mutex> lock(wake_mutex_);
            running_.store(false, std::memory_order_relaxed);
        }
        wake_.notify_all();
        thread_.join();
    }

    /// Wakes on its own clock, not the audio thread's. Sleeps on a condition
    /// variable rather than a bare sleep so destruction does not wait out a full
    /// interval — at 1 Hz that would be a one-second hang on every plug-in close.
    void sender_loop() {
        while (running_.load(std::memory_order_relaxed)) {
            publish_once();
            const auto interval = std::chrono::duration<double>(
                send_interval_seconds(state().get_value(kRateHz)));
            std::unique_lock<std::mutex> lock(wake_mutex_);
            wake_.wait_for(lock, interval, [this] {
                return !running_.load(std::memory_order_relaxed);
            });
        }
    }

    std::unique_ptr<MessageSink> sink_;
    std::array<std::atomic<float>, kOscChannels> latest_{};

    // Sender-thread state. Not shared with the audio thread.
    std::array<float, kOscChannels> last_sent_{};
    std::array<bool, kOscChannels> first_send_{true, true};
    std::uint16_t connected_port_ = 0;
    std::mutex send_mutex_;

    std::atomic<std::uint64_t> sent_count_{0};
    std::atomic<bool> running_{false};
    std::thread thread_;
    std::mutex wake_mutex_;
    std::condition_variable wake_;
};

inline std::unique_ptr<format::Processor> create_cv_osc() {
    return std::make_unique<CvOscProcessor>();
}

}  // namespace pulp::examples::brew
