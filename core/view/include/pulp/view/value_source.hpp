#pragma once
#include <cstdint>
#include <atomic>

/// @file value_source.hpp
/// Paint-safe host→view value channels. A plugin's audio/host thread publishes
/// meter levels or a scalar readout; a view reads the latest value paint-safe on
/// the FrameClock — no lock, no allocation on either side. This is the SDK
/// default for the pattern every native-import host otherwise hand-rolls (a
/// reader polled on the frame clock), and the subscription that lets a live
/// meter keep an editor's frames alive (see needs_continuous_frames).

#include <algorithm>
#include <array>
#include <cstddef>

#include <pulp/runtime/triple_buffer.hpp>

namespace pulp::view {

/// A single multi-channel meter reading. Fixed-capacity and trivially copyable
/// so it can ride a `TripleBuffer` with no allocation on the publish path.
/// Stereo is the common case; `kMaxChannels` is the surround ceiling.
struct MeterFrame {
    static constexpr int kMaxChannels = 8;
    std::array<float, kMaxChannels> rms{};
    std::array<float, kMaxChannels> peak{};
    /// Number of valid channels in `rms`/`peak`. Publishers should keep this in
    /// `[0, kMaxChannels]`; consumers must still bound their index by
    /// `min(channels, kMaxChannels)` since `publish()` stores the frame verbatim
    /// and never trusts the count to gate an array access.
    int channels = 0;
};

/// Monotonic publish counter, shared by every value source.
///
/// Staleness is "the writer stopped publishing", NOT "the value stopped
/// changing" — and the two are genuinely different. A compressor holding a
/// steady -6 dB of gain reduction publishes the same number every block and
/// must keep reading -6; a processor whose audio stopped publishes nothing and
/// must decay to its neutral. Comparing values cannot tell those apart, so a
/// reader watches this counter instead.
class PublishCounter {
public:
    /// Writer (audio/host) thread, from publish(). Relaxed: the counter orders
    /// no other memory, it only has to be monotonic and eventually visible.
    void note_publish() noexcept { publish_seq_.fetch_add(1, std::memory_order_relaxed); }

    /// Reader (UI) thread. Changes iff the writer published since the last read.
    std::uint32_t publish_seq() const noexcept {
        return publish_seq_.load(std::memory_order_relaxed);
    }

private:
    std::atomic<std::uint32_t> publish_seq_{0};
};

/// Lock-free meter channel: the host publishes a `MeterFrame` from the
/// audio/host thread; the view reads the latest frame paint-safe. Exactly one
/// writer and one reader thread (the `TripleBuffer` contract).
class MeterSource : public PublishCounter {
public:
    /// Publish the latest reading. Call from the writer (audio/host) thread.
    /// Alloc-free and non-blocking — a fixed-size copy into the back buffer.
    void publish(const MeterFrame& frame) { buffer_.write(frame); note_publish(); }

    /// Read the latest published reading. Call from the reader (UI) thread.
    /// Returns by value so the caller never holds a reference into the buffer.
    MeterFrame read() { return buffer_.read(); }

private:
    runtime::TripleBuffer<MeterFrame> buffer_;
};

/// A block of samples for a scope-style display — an envelope curve, a
/// detector's output, a window of the signal a processor is actually working on.
///
/// Fixed capacity with a runtime valid count, the same shape as `MeterFrame`
/// and `AudioProbeSnapshot`. That is what keeps it trivially copyable, which is
/// what lets it ride a `TripleBuffer` with no allocation on the publish path and
/// no new lock-free code to get wrong. The cost is a flat 3 × 16 KB per vector
/// channel whether or not the publisher fills it; a channel is opt-in, so a
/// plugin that declares none pays nothing.
struct VectorFrame {
    static constexpr int kMaxSamples = 4096;
    std::array<float, kMaxSamples> samples{};
    /// Valid samples in `samples`. Consumers must still bound their index by
    /// `min(count, kMaxSamples)` — `publish()` clamps, but a frame read back
    /// from anywhere else is not guaranteed to have.
    int count = 0;
};

/// Lock-free vector channel: the writer publishes a block of samples, the view
/// reads the latest block paint-safe. Same one-writer/one-reader contract as
/// `MeterSource`.
///
/// Latest-wins, like every channel here: a block the reader never got to is
/// silently replaced. That is correct for a visualization — a scope shows the
/// current state of the signal, not a lossless history of it.
class VectorSource : public PublishCounter {
public:
    /// Publish a block from the writer (audio/host) thread. Alloc-free and
    /// non-blocking: a bounded copy into the back buffer. More than
    /// `VectorFrame::kMaxSamples` is truncated rather than rejected, so an
    /// oversized block still renders its leading window instead of vanishing.
    void publish(const float* samples, int count) {
        VectorFrame frame;
        frame.count = count < 0 ? 0 : (count > VectorFrame::kMaxSamples
                                           ? VectorFrame::kMaxSamples
                                           : count);
        if (samples != nullptr && frame.count > 0) {
            std::copy_n(samples, static_cast<std::size_t>(frame.count), frame.samples.begin());
        }
        buffer_.write(frame);
        note_publish();
    }

    /// Read the latest published block from the reader (UI) thread.
    VectorFrame read() { return buffer_.read(); }

private:
    runtime::TripleBuffer<VectorFrame> buffer_;
};

/// Lock-free scalar channel: a single paint-safe cached number (a readout value,
/// a modulation ring's base→modulated position). Same one-writer/one-reader
/// contract as `MeterSource`.
class ScalarSource : public PublishCounter {
public:
    /// Publish the latest value from the writer (audio/host) thread.
    void publish(float value) { buffer_.write(value); note_publish(); }

    /// Read the latest value from the reader (UI) thread.
    float read() { return buffer_.read(); }

private:
    runtime::TripleBuffer<float> buffer_{0.0f};
};

} // namespace pulp::view
