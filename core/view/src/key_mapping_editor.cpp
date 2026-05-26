// key_mapping_editor.cpp — Pulp-native UI for rebinding command shortcuts.
//
// See `core/view/include/pulp/view/key_mapping_editor.hpp` for the contract.

#include <pulp/view/key_mapping_editor.hpp>
#include <pulp/state/properties_file.hpp>

#include <cmath>
#include <utility>

namespace pulp::view {

namespace {

const char* keycode_label(KeyCode k) {
    using K = KeyCode;
    switch (k) {
        case K::left: return "Left";
        case K::right: return "Right";
        case K::up: return "Up";
        case K::down: return "Down";
        case K::home: return "Home";
        case K::end_: return "End";
        case K::page_up: return "PageUp";
        case K::page_down: return "PageDown";
        case K::backspace: return "Backspace";
        case K::delete_: return "Delete";
        case K::tab: return "Tab";
        case K::enter: return "Enter";
        case K::escape: return "Escape";
        case K::space: return "Space";
        case K::f1: return "F1"; case K::f2: return "F2";
        case K::f3: return "F3"; case K::f4: return "F4";
        case K::f5: return "F5"; case K::f6: return "F6";
        case K::f7: return "F7"; case K::f8: return "F8";
        case K::f9: return "F9"; case K::f10: return "F10";
        case K::f11: return "F11"; case K::f12: return "F12";
        case K::unknown: return "—";
        default: break;
    }
    return nullptr;
}

bool is_modifier_only_keycode(KeyCode k) {
    // No dedicated modifier-key enum values in input_events.hpp yet, so
    // capture commits on the first non-unknown key. This matches the
    // contract documented in the header (commit the next non-modifier
    // chord) without requiring new enum surface.
    return k == KeyCode::unknown;
}

} // namespace

std::string format_chord(KeyCode key, std::uint16_t modifiers) {
    if (key == KeyCode::unknown && modifiers == 0) return "—";
    std::string out;
    auto add = [&](const char* s) {
        if (!out.empty()) out += "+";
        out += s;
    };
    if (modifiers & kModCtrl)  add("Ctrl");
    if (modifiers & kModAlt)   add("Alt");
    if (modifiers & kModShift) add("Shift");
    if (modifiers & kModCmd)   add("Cmd");
    else if (modifiers & kModMeta) add("Meta");

    // Key portion.
    if (key != KeyCode::unknown) {
        if (const char* lbl = keycode_label(key)) {
            add(lbl);
        } else {
            // Printable ASCII — uppercase for display.
            int code = static_cast<int>(key);
            if (code >= 'a' && code <= 'z') code -= 32;
            char buf[2] = {static_cast<char>(code), 0};
            add(buf);
        }
    }
    return out;
}

// ── KeyMappingEditor ────────────────────────────────────────────────────

KeyMappingEditor::KeyMappingEditor(CommandRegistry& registry)
    : registry_(registry) {
    set_focusable(true);
}

void KeyMappingEditor::set_persistence(pulp::state::PropertiesFile* props,
                                       std::string auto_save_path) {
    props_ = props;
    auto_save_path_ = std::move(auto_save_path);
}

void KeyMappingEditor::set_persistence_prefix(std::string prefix) {
    persistence_prefix_ = std::move(prefix);
}

void KeyMappingEditor::begin_capture(CommandID id) {
    if (!registry_.command_info(id)) return;
    capture_id_ = id;
}

void KeyMappingEditor::cancel_capture() {
    capture_id_ = kInvalidCommandID;
}

void KeyMappingEditor::rebind(CommandID id, KeyCode key,
                              std::uint16_t modifiers) {
    if (!registry_.command_info(id)) return;
    commit(id, key, modifiers);
}

void KeyMappingEditor::commit(CommandID id, KeyCode key,
                              std::uint16_t modifiers) {
    auto& map = registry_.shortcuts();
    // Drop any other chord previously pointing at this command so each
    // command has at most one binding (matches the editor's one-row-per-
    // command UI).
    map.unbind_command(id);
    if (key != KeyCode::unknown) {
        // If another command already owns this chord, replace it (the
        // user's intent is "give this chord to me now").
        map.bind(key, modifiers, id);
    }
    capture_id_ = kInvalidCommandID;

    if (props_) {
        map.save_to(*props_, persistence_prefix_);
        if (!auto_save_path_.empty()) {
            props_->save(auto_save_path_);
        }
    }
    if (on_rebound) on_rebound(id, key, modifiers);
}

int KeyMappingEditor::row_at(float y) const {
    if (row_height_ <= 0.0f) return -1;
    // Row 0 is the header.
    int idx = static_cast<int>(std::floor(y / row_height_)) - 1;
    if (idx < 0 || idx >= static_cast<int>(registry_.commands().size())) {
        return -1;
    }
    return idx;
}

void KeyMappingEditor::on_mouse_down(Point pos) {
    int idx = row_at(pos.y);
    if (idx < 0) {
        cancel_capture();
        return;
    }
    auto cmd = registry_.commands()[static_cast<std::size_t>(idx)];
    begin_capture(cmd.id);
}

bool KeyMappingEditor::on_key_event(const KeyEvent& event) {
    if (!is_capturing()) return false;
    if (!event.is_down) return true; // consume key-up while capturing

    // Escape cancels capture without rebinding.
    if (event.key == KeyCode::escape && event.modifiers == 0) {
        cancel_capture();
        return true;
    }
    if (is_modifier_only_keycode(event.key)) return true;

    commit(capture_id_, event.key, event.modifiers);
    return true;
}

float KeyMappingEditor::intrinsic_height() const {
    return row_height_ * (1.0f + static_cast<float>(registry_.commands().size()));
}

void KeyMappingEditor::paint(canvas::Canvas& canvas) {
    const auto r = local_bounds();
    const Color bg       = resolve_color("background.surface",
                                         Color::rgba(0.10f, 0.10f, 0.11f, 1.0f));
    const Color text     = resolve_color("text.primary",
                                         Color::rgba(0.92f, 0.92f, 0.95f, 1.0f));
    const Color text_dim = resolve_color("text.secondary",
                                         Color::rgba(0.62f, 0.62f, 0.66f, 1.0f));
    const Color sel      = resolve_color("accent.primary",
                                         Color::rgba(0.20f, 0.45f, 0.85f, 1.0f));
    const Color border   = resolve_color("border.subtle",
                                         Color::rgba(0.20f, 0.20f, 0.22f, 1.0f));

    canvas.set_fill_color(bg);
    canvas.fill_rect(r.x, r.y, r.width, r.height);

    const float pad = 8.0f;
    const float name_x = r.x + pad;
    const float chord_x = r.x + r.width * 0.55f;

    // Header row.
    canvas.set_fill_color(text_dim);
    canvas.fill_text("Command", name_x, r.y + row_height_ * 0.7f);
    canvas.fill_text("Shortcut", chord_x, r.y + row_height_ * 0.7f);

    canvas.set_fill_color(border);
    canvas.fill_rect(r.x, r.y + row_height_ - 1.0f, r.width, 1.0f);

    // One row per command.
    const auto& cmds = registry_.commands();
    for (std::size_t i = 0; i < cmds.size(); ++i) {
        const float row_y = r.y + row_height_ * (1.0f + static_cast<float>(i));
        const bool is_capturing_row = (cmds[i].id == capture_id_);

        if (is_capturing_row) {
            canvas.set_fill_color(sel);
            canvas.fill_rect(r.x, row_y, r.width, row_height_);
        }

        canvas.set_fill_color(is_capturing_row ? bg : text);
        canvas.fill_text(cmds[i].name, name_x, row_y + row_height_ * 0.7f);

        auto chords = registry_.shortcuts().chords_for(cmds[i].id);
        std::string chord_text =
            is_capturing_row ? std::string("Press a key…")
            : chords.empty() ? std::string("—")
            : format_chord(chords.front().key, chords.front().modifiers);
        canvas.fill_text(chord_text, chord_x, row_y + row_height_ * 0.7f);
    }
}

} // namespace pulp::view
