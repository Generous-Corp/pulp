#pragma once

#include <pulp/view/view.hpp>
#include <functional>
#include <set>

namespace pulp::view {

// ── MidiKeyboard ────────────────────────────────────────────────────────────
// Piano keyboard widget for note input/display.

class MidiKeyboard : public View {
public:
    MidiKeyboard();

    // Range (MIDI note numbers 0-127)
    void set_range(int first_note, int last_note);
    int first_note() const { return first_note_; }
    int last_note() const { return last_note_; }

    // Active/held notes (for display from external MIDI)
    void note_on(int note, float velocity = 1.0f);
    void note_off(int note);
    void all_notes_off();
    bool is_note_on(int note) const;

    // Interaction
    std::function<void(int note, float velocity)> on_note_on;
    std::function<void(int note)> on_note_off;

    // Orientation
    enum class Orientation { horizontal, vertical };
    void set_orientation(Orientation o) { orientation_ = o; }
    Orientation orientation() const { return orientation_; }

    // Display options
    void set_show_note_names(bool show) { show_names_ = show; }
    bool show_note_names() const { return show_names_; }

    void set_highlight_color(Color c) { highlight_color_ = c; }

    void paint(canvas::Canvas& canvas) override;
    void on_mouse_down(Point pos) override;
    void on_mouse_up(Point pos) override;
    void on_mouse_drag(Point pos) override;
    void on_mouse_event(const MouseEvent& event) override;
    bool wants_mouse_input() const override { return true; }

    float intrinsic_height() const override { return 80.0f; }

private:
    int first_note_ = 36;   // C2
    int last_note_ = 96;    // C7
    Orientation orientation_ = Orientation::horizontal;
    bool show_names_ = false;
    // Transparent sentinel (a==0) means "no explicit highlight set" — paint()
    // then falls back to the theme's accent.primary for active keys. A default
    // Color{} is OPAQUE black (a=1), which would make every pressed key render
    // black instead of the accent; keep this explicitly transparent.
    Color highlight_color_{0.0f, 0.0f, 0.0f, 0.0f};

    struct NoteState {
        float velocity = 0;
        bool active = false;
    };
    NoteState note_states_[128] = {};
    int mouse_note_ = -1;

    bool is_black_key(int note) const;
    int note_at_position(Point pos) const;
    Rect key_rect(int note) const;
    float white_key_width() const;
    int white_key_count() const;
};

} // namespace pulp::view
