#pragma once

/// @file midi_source_reservation.hpp
/// A claim on MIDI sources that some other feature already consumes, so MIDI
/// Learn can refuse to bind them and say why. For example, the preset program
/// router consumes Bank Select and Program Change.

#include <pulp/state/midi_source.hpp>

namespace pulp::state {

class MidiSourceReservation {
  public:
    virtual ~MidiSourceReservation() = default;

    /// True when `source` is consumed elsewhere. `source` may use an omni
    /// channel, which overlaps every channel. Safe from any thread, including
    /// the audio thread: no allocation, no lock.
    virtual bool claims(const MidiSource& source) const noexcept = 0;

    /// A static, user-readable reason for a refusal. Never null.
    virtual const char* reason() const noexcept = 0;
};

} // namespace pulp::state
