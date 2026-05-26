#pragma once

// VirtualUmpEndpoint — purely in-process UMP endpoint.
//
// macOS plan item 8.1 (VirtualEndpoint half). A `VirtualUmpEndpoint`
// is a `UmpEndpoint` with no OS handle: packets sent into it are
// queued and re-delivered to the registered receive callback
// synchronously inside `send()`. This is the workhorse for
// headless tests (the CoreMIDI side rejects connections without a
// real graphics session) and for in-process loopback between a
// plugin's internal Generator and Sink (think: a built-in test
// signal arpeggiator wired straight into the synth).
//
// Thread model:
//   - `send()` is callable from any thread. It runs the receive
//     callback on the calling thread, immediately, with no locking
//     beyond a single std::mutex around the callback pointer swap.
//   - `set_receive_callback()` may be called from any thread; it
//     swaps under the same mutex so a concurrent `send()` either
//     sees the old or the new callback (never half-installed).
//
// Lifetime:
//   - The endpoint is registered with a `UmpSession`; the session
//     owns it. Destroying the session destroys all virtual
//     endpoints. Callers can also retain a `std::shared_ptr<VirtualUmpEndpoint>`
//     handle they got from `register_virtual_endpoint` for direct
//     `send()` access.
//
// Loopback variant:
//   - For round-trip tests, set `loopback = true`. Packets sent
//     into the endpoint are also re-emitted to the callback.
//     (Default false to keep the unidirectional case behaving
//     like a real physical sink.)

#include <pulp/midi/ump_endpoint.hpp>

#include <atomic>
#include <mutex>
#include <string>

namespace pulp::midi {

/// Configuration for a newly-registered virtual endpoint. All fields
/// have sensible defaults; only `name` is normally set.
struct VirtualUmpEndpointConfig {
    std::string name = "Pulp Virtual UMP";
    /// True if `send()` should also fire the receive callback (round-trip
    /// loopback). False (default) makes the endpoint look like a sink:
    /// `send()` consumes, the receive callback only fires for packets
    /// pushed in via `deliver()`.
    bool loopback = false;
    /// Direction the endpoint advertises in its `info()`. Defaults to
    /// fully bidirectional; tests that want to assert "this endpoint
    /// rejects outbound" can flip `can_send` off.
    UmpEndpointDirection direction { true, true };
};

class VirtualUmpEndpoint : public UmpEndpoint {
public:
    explicit VirtualUmpEndpoint(VirtualUmpEndpointConfig cfg);

    const UmpEndpointInfo& info() const noexcept override { return info_; }

    void set_receive_callback(UmpReceiveCallback cb) override;

    /// `send()` writes a packet "out" of this endpoint. If `loopback`
    /// is true the receive callback also fires. Returns false only if
    /// the endpoint advertises `can_send = false`.
    bool send(const UmpPacket& packet) override;

    bool is_open() const noexcept override { return open_.load(std::memory_order_acquire); }

    /// Deliver a packet directly to the receive callback as if it
    /// arrived from elsewhere. This is the "into" side of the
    /// endpoint: a test partner uses it to feed packets without
    /// pretending to be the OS.
    bool deliver(const UmpPacket& packet, double timestamp_sec = 0.0);

    /// Atomic counters useful for tests; never load-bearing for the
    /// real backend (CoreMIDI counters live elsewhere).
    std::uint64_t sent_count() const noexcept { return sent_.load(std::memory_order_relaxed); }
    std::uint64_t delivered_count() const noexcept { return delivered_.load(std::memory_order_relaxed); }

    /// Tear-down hook — marks `is_open()` false and detaches the
    /// callback. After `close()`, `send()` returns false and
    /// `deliver()` is a no-op. Idempotent.
    void close();

private:
    UmpEndpointInfo info_;
    bool loopback_;

    mutable std::mutex cb_mu_;
    UmpReceiveCallback cb_;

    std::atomic<bool> open_{true};
    std::atomic<std::uint64_t> sent_{0};
    std::atomic<std::uint64_t> delivered_{0};
};

} // namespace pulp::midi
