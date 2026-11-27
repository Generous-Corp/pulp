#include <pulp/vellum_audio/piano_roll_pitch_ruler.hpp>

namespace pulp::vellum_audio {

PianoRollPitchRuler::PianoRollPitchRuler() {
    set_orientation(MidiKeyboard::Orientation::vertical_chromatic_rows);
    set_show_note_names(true);
}

void PianoRollPitchRuler::set_pitch_projection(const timeline_editor::PitchProjection& projection) {
    set_range(projection.lowest_pitch(), projection.highest_pitch());
    const auto frame = bounds();
    const auto pixels = projection.pixels();
    set_bounds({frame.x, pixels.origin, frame.width, pixels.extent});
    request_repaint();
}

} // namespace pulp::vellum_audio
