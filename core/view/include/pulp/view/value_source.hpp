#pragma once

/// @file value_source.hpp
/// Paint-safe host→view value channels. A plugin's audio/host thread publishes
/// meter levels or a scalar readout; a view reads the latest value paint-safe on
/// the FrameClock — no lock, no allocation on either side. This is the SDK
/// default for the pattern every native-import host otherwise hand-rolls (a
/// reader polled on the frame clock), and the subscription that lets a live
/// meter keep an editor's frames alive (see needs_continuous_frames).

#include <array>

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

/// Lock-free meter channel: the host publishes a `MeterFrame` from the
/// audio/host thread; the view reads the latest frame paint-safe. Exactly one
/// writer and one reader thread (the `TripleBuffer` contract).
class MeterSource {
public:
    /// Publish the latest reading. Call from the writer (audio/host) thread.
    /// Alloc-free and non-blocking — a fixed-size copy into the back buffer.
    void publish(const MeterFrame& frame) { buffer_.write(frame); }

    /// Read the latest published reading. Call from the reader (UI) thread.
    /// Returns by value so the caller never holds a reference into the buffer.
    MeterFrame read() { return buffer_.read(); }

private:
    runtime::TripleBuffer<MeterFrame> buffer_;
};

/// Lock-free scalar channel: a single paint-safe cached number (a readout value,
/// a modulation ring's base→modulated position). Same one-writer/one-reader
/// contract as `MeterSource`.
class ScalarSource {
public:
    /// Publish the latest value from the writer (audio/host) thread.
    void publish(float value) { buffer_.write(value); }

    /// Read the latest value from the reader (UI) thread.
    float read() { return buffer_.read(); }

private:
    runtime::TripleBuffer<float> buffer_{0.0f};
};

} // namespace pulp::view
