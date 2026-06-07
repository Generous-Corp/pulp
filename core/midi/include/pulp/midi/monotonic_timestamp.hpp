#pragma once

#include <chrono>

namespace pulp::midi {

/// Monotonic timestamp source for MIDI input events, measured in seconds
/// from a base captured when the port opened.
///
/// `MidiEvent::timestamp` is documented as "absolute time in seconds"
/// (buffer.hpp). The Windows backend reports seconds-since-open via QPC
/// (winmidi_device.cpp) and CoreMIDI reports host-time seconds; the Linux
/// ALSA raw-midi path previously hardcoded `0.0`, losing all timing. This
/// helper gives the ALSA path the same seconds-since-open semantics as
/// Windows using `std::chrono::steady_clock`, which is guaranteed monotonic.
///
/// The `now`-taking overloads are pure, so the conversion is unit-testable
/// with synthetic time points without depending on the wall clock.
class MonotonicMidiClock {
public:
    using clock = std::chrono::steady_clock;

    /// Capture the open-time base. Call once when the port opens.
    void reset(clock::time_point now) {
        base_ = now;
        has_base_ = true;
    }
    void reset() { reset(clock::now()); }

    /// Seconds elapsed since the captured base. Returns 0 before the first
    /// reset() so an un-opened source never reports a bogus large value.
    double seconds_since_open(clock::time_point now) const {
        if (!has_base_) return 0.0;
        return std::chrono::duration<double>(now - base_).count();
    }
    double seconds_since_open() const { return seconds_since_open(clock::now()); }

    bool has_base() const { return has_base_; }

private:
    clock::time_point base_{};
    bool has_base_ = false;
};

}  // namespace pulp::midi
