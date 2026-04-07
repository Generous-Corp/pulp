#include <pulp/view/midi_keyboard.hpp>
#include <algorithm>

namespace pulp::view {

MidiKeyboard::MidiKeyboard() {
    set_focusable(true);
    set_access_role(AccessRole::group);
}

void MidiKeyboard::set_range(int first, int last) {
    first_note_ = std::clamp(first, 0, 127);
    last_note_ = std::clamp(last, first_note_, 127);
}

void MidiKeyboard::note_on(int note, float velocity) {
    if (note >= 0 && note < 128) {
        note_states_[note] = {velocity, true};
    }
}

void MidiKeyboard::note_off(int note) {
    if (note >= 0 && note < 128) {
        note_states_[note] = {0, false};
    }
}

void MidiKeyboard::all_notes_off() {
    for (auto& s : note_states_) s = {0, false};
}

bool MidiKeyboard::is_note_on(int note) const {
    if (note < 0 || note >= 128) return false;
    return note_states_[note].active;
}

bool MidiKeyboard::is_black_key(int note) const {
    int n = note % 12;
    return n == 1 || n == 3 || n == 6 || n == 8 || n == 10;
}

int MidiKeyboard::white_key_count() const {
    int count = 0;
    for (int n = first_note_; n <= last_note_; ++n) {
        if (!is_black_key(n)) ++count;
    }
    return count;
}

float MidiKeyboard::white_key_width() const {
    int wk = white_key_count();
    if (wk <= 0) return 0;
    auto b = local_bounds();
    return (orientation_ == Orientation::horizontal) ? b.width / static_cast<float>(wk) : b.height / static_cast<float>(wk);
}

Rect MidiKeyboard::key_rect(int note) const {
    if (note < first_note_ || note > last_note_) return {};

    auto b = local_bounds();
    float wkw = white_key_width();
    bool horiz = (orientation_ == Orientation::horizontal);

    // Count white keys before this note
    int white_index = 0;
    for (int n = first_note_; n < note; ++n) {
        if (!is_black_key(n)) ++white_index;
    }

    if (!is_black_key(note)) {
        // White key
        if (horiz)
            return {b.x + white_index * wkw, b.y, wkw, b.height};
        else
            return {b.x, b.y + white_index * wkw, b.width, wkw};
    } else {
        // Black key — positioned relative to preceding white key
        float bk_width = wkw * 0.6f;
        float bk_height = (horiz ? b.height : b.width) * 0.6f;
        float offset = wkw - bk_width * 0.5f;

        if (horiz)
            return {b.x + white_index * wkw + offset - wkw, b.y, bk_width, bk_height};
        else
            return {b.x, b.y + white_index * wkw + offset - wkw, bk_height, bk_width};
    }
}

int MidiKeyboard::note_at_position(Point pos) const {
    // Check black keys first (they overlap white keys)
    for (int n = last_note_; n >= first_note_; --n) {
        if (is_black_key(n)) {
            auto r = key_rect(n);
            if (pos.x >= r.x && pos.x < r.x + r.width &&
                pos.y >= r.y && pos.y < r.y + r.height)
                return n;
        }
    }
    // Then white keys
    for (int n = first_note_; n <= last_note_; ++n) {
        if (!is_black_key(n)) {
            auto r = key_rect(n);
            if (pos.x >= r.x && pos.x < r.x + r.width &&
                pos.y >= r.y && pos.y < r.y + r.height)
                return n;
        }
    }
    return -1;
}

void MidiKeyboard::paint(canvas::Canvas& canvas) {
    auto b = local_bounds();
    auto white_color = resolve_color("bg.elevated", Color::rgba8(240, 240, 240));
    auto black_color = resolve_color("bg.primary", Color::rgba8(30, 30, 30));
    auto active_color = highlight_color_.a > 0
        ? highlight_color_
        : resolve_color("accent.primary", Color::rgba8(100, 160, 255));
    auto border_c = resolve_color("control.border", Color::rgba8(100, 100, 100));
    auto text_c = resolve_color("text.secondary", Color::rgba8(120, 120, 120));

    // Draw white keys first
    for (int n = first_note_; n <= last_note_; ++n) {
        if (is_black_key(n)) continue;
        auto r = key_rect(n);
        auto fill = note_states_[n].active ? active_color : white_color;
        canvas.set_fill_color(fill);
        canvas.fill_rounded_rect(r.x, r.y, r.width, r.height, 2.0f);
        canvas.set_stroke_color(border_c);
        canvas.set_line_width(0.5f);
        canvas.stroke_rounded_rect(r.x, r.y, r.width, r.height, 2.0f);

        if (show_names_ && n % 12 == 0) {
            int octave = (n / 12) - 1;
            std::string name = "C" + std::to_string(octave);
            canvas.set_fill_color(text_c);
            canvas.set_font("", 9.0f);
            canvas.fill_text(name, r.x + 2, r.y + r.height - 12);
        }
    }

    // Draw black keys on top
    for (int n = first_note_; n <= last_note_; ++n) {
        if (!is_black_key(n)) continue;
        auto r = key_rect(n);
        auto fill = note_states_[n].active ? active_color : black_color;
        canvas.set_fill_color(fill);
        canvas.fill_rounded_rect(r.x, r.y, r.width, r.height, 2.0f);
    }
}

void MidiKeyboard::on_mouse_down(Point pos) {
    int note = note_at_position(pos);
    if (note >= 0) {
        mouse_note_ = note;
        note_on(note, 0.8f);
        if (on_note_on) on_note_on(note, 0.8f);
    }
}

void MidiKeyboard::on_mouse_drag(Point pos) {
    int note = note_at_position(pos);
    if (note != mouse_note_) {
        if (mouse_note_ >= 0) {
            note_off(mouse_note_);
            if (on_note_off) on_note_off(mouse_note_);
        }
        if (note >= 0) {
            mouse_note_ = note;
            note_on(note, 0.8f);
            if (on_note_on) on_note_on(note, 0.8f);
        } else {
            mouse_note_ = -1;
        }
    }
}

void MidiKeyboard::on_mouse_up(Point pos) {
    (void)pos;
    if (mouse_note_ >= 0) {
        note_off(mouse_note_);
        if (on_note_off) on_note_off(mouse_note_);
        mouse_note_ = -1;
    }
}

void MidiKeyboard::on_mouse_event(const MouseEvent& event) {
    View::on_mouse_event(event);
}

} // namespace pulp::view
