#pragma once

/// @file midi_source_reservation.hpp
/// A way for a plug-in to say "these MIDI messages are already spoken for", so
/// MIDI Learn neither binds them nor lets an existing mapping fire on them. The
/// preset router that consumes Bank Select and Program Change is one such owner.

#include <pulp/state/midi_source.hpp>

namespace pulp::state {

class MidiSourceReservation {
  public:
    virtual ~MidiSourceReservation() = default;

    /// True when `source` is claimed. `source.channel` may be `MidiSource::kOmni`,
    /// meaning "any channel": claim it when any channel the owner listens on
    /// overlaps. Called on the audio thread: no allocation, locks or exceptions.
    virtual bool claims(const MidiSource& source) const noexcept = 0;

    /// Why the claim exists, for a UI to show. A string with static lifetime.
    virtual const char* reason() const noexcept = 0;
};

} // namespace pulp::state
