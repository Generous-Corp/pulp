#pragma once

// CV To OSC — watch a control voltage, and tell something about it over OSC.
//
// An insert, not a generator: the input passes through to the output bit-exactly,
// so this sits anywhere in a chain without changing the voltage that reaches the
// jack. What it adds is a side-channel — the value of each channel, sent as an OSC
// float to an address and a destination the user types.
//
// Each channel is **off by default**, and that is a safety property rather than a
// style choice. A plug-in that begins emitting UDP the moment a project loads will
// trip a firewall prompt on some machines and quietly stream a user's patch on
// others. Nothing leaves this process until a switch is on.
//
// The send happens on a background thread. See brew/cv_osc.hpp for why the audio
// thread must not touch a socket. The audio thread's entire contribution is one
// relaxed atomic store per channel per block.

#include <brew/channels.hpp>
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
#include <span>
#include <string>
#include <thread>
#include <utility>
#include <vector>

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

class CvOscProcessor : public format::Processor {
public:
    // Parameter IDs are part of the persisted state contract. Never renumber.
    //
    // Id 2 was a `Port` knob before the destination became text. It stays retired
    // rather than reused: a float parameter cannot hold `host:port`, and a stale
    // project restoring a port into whatever id 2 means next is exactly the class
    // of bug an id contract exists to prevent.
    enum ParamId : state::ParamID {
        kEnable = 1,       ///< Per channel. Right is kEnable + kRightChannelStride.
        kRateHz = 3,       ///< Global: the sender thread has one clock.
        kThreshold = 4,    ///< Per channel.
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

    struct ControlSpec {
        state::ParamID id;
        const char* name;
        const char* unit;
        state::ParamRange range;
    };

    /// Registered once per channel: the two OSC streams are independent.
    static constexpr std::array<ControlSpec, 2> controls() {
        return {{
            // Off until asked. Nothing leaves the process before this is on.
            {kEnable, "Enable", "", {0.0f, 1.0f, 0.0f, 1.0f}},
            // Movement smaller than this is not worth a packet.
            {kThreshold, "Threshold", "", {0.0f, 0.5f, 0.001f, 0.0001f}},
        }};
    }

    void define_parameters(state::StateStore& store) override {
        for (std::size_t ch = 0; ch < kOscChannels; ++ch)
            for (const auto& c : controls())
                store.add_parameter(
                    {.id = static_cast<state::ParamID>(param_for(c.id, ch)),
                     .name = std::string(c.name) + channel_suffix(ch),
                     .unit = c.unit,
                     .range = c.range});

        // One rate, because one thread sends both channels. Per-channel rates
        // would need per-channel threads to mean anything, and a second thread to
        // send a second float is not a trade worth making.
        store.add_parameter({.id = kRateHz,
                             .name = "Rate",
                             .unit = "Hz",
                             .range = {kMinRateHz, kMaxRateHz, 60.0f, 1.0f}});
    }

    void prepare(const format::PrepareContext&) override { start_sender(); }

    std::pair<uint32_t, uint32_t> editor_size() const override { return {380, 640}; }

    std::unique_ptr<view::View> create_view() override;

    // ── Text state ───────────────────────────────────────────────────────────
    //
    // A hostname is not a float, so the destination and the two OSC addresses
    // live outside the StateStore, in the plug-in blob every adapter already
    // round-trips. They are read on the sender thread and written on the UI
    // thread, so every access takes the send lock.

    [[nodiscard]] OscSettings osc_settings() const {
        const std::lock_guard<std::mutex> lock(send_mutex_);
        return settings_;
    }

    /// Rejects a target it cannot parse, and says so, so the editor can put the
    /// old text back rather than leave a typo looking accepted.
    bool set_target(std::string_view text) {
        const auto parsed = parse_osc_target(text);
        if (!parsed) return false;
        const std::lock_guard<std::mutex> lock(send_mutex_);
        if (settings_.target == *parsed) return true;
        settings_.target = *parsed;
        // A new destination has never heard the resting value.
        connected_ = false;
        return true;
    }

    /// Rejects an address pattern, an empty path, or anything the OSC spec
    /// forbids. A sender may not send `/cv/*`.
    bool set_path(std::size_t channel, std::string_view path) {
        if (channel >= kOscChannels || !is_valid_osc_path(path)) return false;
        const std::lock_guard<std::mutex> lock(send_mutex_);
        settings_.paths[channel] = std::string(path);
        return true;
    }

    std::vector<uint8_t> serialize_plugin_state() const override {
        const std::string text = serialize_osc_settings(osc_settings());
        return {text.begin(), text.end()};
    }

    bool deserialize_plugin_state(std::span<const uint8_t> data) override {
        // An empty blob is a project saved before this plug-in had text state, and
        // a fresh instance's defaults are the right answer for it.
        if (data.empty()) return true;
        const auto parsed = deserialize_osc_settings(
            std::string_view(reinterpret_cast<const char*>(data.data()), data.size()));
        if (!parsed) return false;
        const std::lock_guard<std::mutex> lock(send_mutex_);
        settings_ = *parsed;
        connected_ = false;
        return true;
    }

    // ── Observation ──────────────────────────────────────────────────────────

    /// The last sample seen on a channel. Read by the editor and by the sender.
    [[nodiscard]] float latest(std::size_t channel) const noexcept {
        return channel < kOscChannels
                   ? latest_[channel].load(std::memory_order_relaxed)
                   : 0.0f;
    }

    /// Messages this channel's sink accepted since construction. The editor's lamp
    /// reads it — a counter that never moves is how a user learns the receiver is
    /// not listening, or that they never turned the channel on.
    [[nodiscard]] std::uint64_t sent_count(std::size_t channel) const noexcept {
        return channel < kOscChannels
                   ? sent_count_[channel].load(std::memory_order_relaxed)
                   : 0;
    }

    [[nodiscard]] std::uint64_t sent_count() const noexcept {
        std::uint64_t total = 0;
        for (std::size_t c = 0; c < kOscChannels; ++c) total += sent_count(c);
        return total;
    }

    [[nodiscard]] bool enabled(std::size_t channel) const noexcept {
        return channel < kOscChannels &&
               as_toggle(state().get_value(
                   static_cast<state::ParamID>(param_for(kEnable, channel))));
    }

    /// Publish one tick's worth of values. Exposed so a test can drive the send
    /// logic deterministically instead of waiting on a thread's clock — which
    /// means it can run concurrently with the sender thread, so the send state it
    /// mutates is locked. Never called from the audio thread.
    void publish_once() {
        const std::lock_guard<std::mutex> lock(send_mutex_);

        bool any_enabled = false;
        for (std::size_t c = 0; c < kOscChannels; ++c) any_enabled |= enabled(c);
        // Connecting a socket for a plug-in whose channels are both off is exactly
        // the firewall prompt the default-off rule exists to avoid.
        if (!any_enabled) return;

        if (!connected_) {
            if (!sink_->connect(settings_.target.host, settings_.target.port)) return;
            connected_ = true;
            for (auto& f : first_send_) f = true;
        }

        for (std::size_t c = 0; c < kOscChannels; ++c) {
            if (!enabled(c)) {
                // A channel switched off should re-announce its resting value when
                // it comes back, not resume mid-deadband against a value the
                // receiver last heard minutes ago.
                first_send_[c] = true;
                continue;
            }
            const float threshold = state().get_value(
                static_cast<state::ParamID>(param_for(kThreshold, c)));
            const float v = latest(c);
            if (!should_send(first_send_[c], last_sent_[c], v, threshold)) continue;
            osc::Message msg(settings_.paths[c]);
            msg.add(v);
            if (!sink_->send(msg)) continue;   // a dropped send is not a state change
            last_sent_[c] = v;
            first_send_[c] = false;
            sent_count_[c].fetch_add(1, std::memory_order_relaxed);
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
            latest_[c].store(src ? src[frames - 1] : 0.0f, std::memory_order_relaxed);
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
            const std::lock_guard<std::mutex> lock(wake_mutex_);
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

    // Guarded by send_mutex_: shared between the sender thread and the UI thread,
    // never touched by the audio thread.
    mutable std::mutex send_mutex_;
    OscSettings settings_{};
    std::array<float, kOscChannels> last_sent_{};
    std::array<bool, kOscChannels> first_send_{true, true};
    bool connected_ = false;

    std::array<std::atomic<std::uint64_t>, kOscChannels> sent_count_{};
    std::atomic<bool> running_{false};
    std::thread thread_;
    std::mutex wake_mutex_;
    std::condition_variable wake_;
};

inline std::unique_ptr<format::Processor> create_cv_osc() {
    return std::make_unique<CvOscProcessor>();
}

}  // namespace pulp::examples::brew
