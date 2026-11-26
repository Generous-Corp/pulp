#pragma once

/// @file piano_roll_pitch_ruler.hpp
/// The pitch ruler beside a piano roll.

#include <pulp/timeline_editor/viewport_projection.hpp>
#include <pulp/vellum_audio/audio_widgets.hpp>

namespace pulp::vellum_audio {

/// Pitch ruler for a piano roll, using the shared MIDI keyboard interaction and
/// C4 naming convention over the editor kernel's resolved pitch projection.
///
/// Assign `on_note_on` and `on_note_off` to receive audition requests. The
/// ruler deliberately does not own an audio engine or decide how those requests
/// are rendered.
class PianoRollPitchRuler final : public MidiKeyboard {
  public:
    /// Creates a named vertical chromatic ruler ready for pitch projection.
    PianoRollPitchRuler();

    /// Aligns this ruler's frame and inclusive note range to a piano-roll
    /// projection while preserving its current horizontal frame.
    void set_pitch_projection(const timeline_editor::PitchProjection& projection);
};

} // namespace pulp::vellum_audio
