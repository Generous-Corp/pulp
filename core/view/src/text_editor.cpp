#include <pulp/view/text_editor.hpp>
#include <algorithm>
#include <cctype>
#include <cmath>

namespace pulp::view {

void TextEditor::set_text(const std::string& t) {
    push_undo();
    text_ = t;
    caret_position_ = static_cast<int>(t.size());
    selection_start_ = selection_end_ = caret_position_;
    notify_change();
}

std::string TextEditor::selected_text() const {
    if (!has_selection()) return {};
    int start = std::min(selection_start_, selection_end_);
    int end = std::max(selection_start_, selection_end_);
    return text_.substr(static_cast<size_t>(start),
                        static_cast<size_t>(end - start));
}

void TextEditor::select_all() {
    selection_start_ = 0;
    selection_end_ = static_cast<int>(text_.size());
    caret_position_ = selection_end_;
}

void TextEditor::clear_selection() {
    selection_start_ = selection_end_ = caret_position_;
}

// ── Clipboard ───────────────────────────────────────────────────────────

bool TextEditor::copy_to_clipboard() {
    if (!has_selection()) return false;
    platform::Clipboard::set_text(selected_text());
    return true;
}

bool TextEditor::cut_to_clipboard() {
    if (!has_selection()) return false;
    push_undo();
    platform::Clipboard::set_text(selected_text());
    delete_selection();
    notify_change();
    return true;
}

bool TextEditor::paste_from_clipboard() {
    if (!platform::Clipboard::has_text()) return false;
    auto opt_text = platform::Clipboard::get_text();
    if (!opt_text || opt_text->empty()) return false;

    push_undo();
    if (has_selection()) delete_selection();

    std::string text = *opt_text;
    if (numeric_only) {
        // Filter to numeric characters
        std::string filtered;
        for (char c : text) {
            if (std::isdigit(c) || c == '.' || c == '-') filtered += c;
        }
        text = filtered;
    }

    insert_text(text);
    notify_change();
    return true;
}

// ── Undo/Redo ────────────────────────────────────────────────────────────

void TextEditor::push_undo() {
    undo_history_.emplace_back(text_, caret_position_);
    if (undo_history_.size() > kMaxUndoHistory) {
        undo_history_.erase(undo_history_.begin());
    }
    redo_history_.clear();
}

bool TextEditor::undo() {
    if (undo_history_.empty()) return false;
    redo_history_.emplace_back(text_, caret_position_);
    auto [t, pos] = undo_history_.back();
    undo_history_.pop_back();
    text_ = t;
    caret_position_ = pos;
    selection_start_ = selection_end_ = caret_position_;
    notify_change();
    return true;
}

bool TextEditor::redo() {
    if (redo_history_.empty()) return false;
    undo_history_.emplace_back(text_, caret_position_);
    auto [t, pos] = redo_history_.back();
    redo_history_.pop_back();
    text_ = t;
    caret_position_ = pos;
    selection_start_ = selection_end_ = caret_position_;
    notify_change();
    return true;
}

// ── Text manipulation ────────────────────────────────────────────────────

void TextEditor::insert_text(const std::string& t) {
    text_.insert(static_cast<size_t>(caret_position_), t);
    caret_position_ += static_cast<int>(t.size());
    selection_start_ = selection_end_ = caret_position_;
}

void TextEditor::delete_selection() {
    int start = std::min(selection_start_, selection_end_);
    int end = std::max(selection_start_, selection_end_);
    text_.erase(static_cast<size_t>(start), static_cast<size_t>(end - start));
    caret_position_ = start;
    selection_start_ = selection_end_ = caret_position_;
}

void TextEditor::delete_char_before_caret() {
    if (caret_position_ > 0) {
        push_undo();
        text_.erase(static_cast<size_t>(caret_position_ - 1), 1);
        --caret_position_;
        selection_start_ = selection_end_ = caret_position_;
        notify_change();
    }
}

void TextEditor::delete_char_after_caret() {
    if (caret_position_ < static_cast<int>(text_.size())) {
        push_undo();
        text_.erase(static_cast<size_t>(caret_position_), 1);
        selection_start_ = selection_end_ = caret_position_;
        notify_change();
    }
}

// ── Caret movement ───────────────────────────────────────────────────────

void TextEditor::move_caret(int delta, bool extend) {
    caret_position_ = std::clamp(caret_position_ + delta, 0, static_cast<int>(text_.size()));
    if (extend)
        selection_end_ = caret_position_;
    else
        selection_start_ = selection_end_ = caret_position_;
}

void TextEditor::move_word(int direction, bool extend) {
    int pos = caret_position_;
    int len = static_cast<int>(text_.size());
    if (direction > 0) {
        while (pos < len && !is_word_char(text_[static_cast<size_t>(pos)])) ++pos;
        while (pos < len && is_word_char(text_[static_cast<size_t>(pos)])) ++pos;
    } else {
        while (pos > 0 && !is_word_char(text_[static_cast<size_t>(pos - 1)])) --pos;
        while (pos > 0 && is_word_char(text_[static_cast<size_t>(pos - 1)])) --pos;
    }
    caret_position_ = pos;
    if (extend) selection_end_ = pos;
    else selection_start_ = selection_end_ = pos;
}

void TextEditor::move_to_line_start(bool extend) {
    // For single-line, start = 0
    caret_position_ = 0;
    if (extend) selection_end_ = 0;
    else selection_start_ = selection_end_ = 0;
}

void TextEditor::move_to_line_end(bool extend) {
    caret_position_ = static_cast<int>(text_.size());
    if (extend) selection_end_ = caret_position_;
    else selection_start_ = selection_end_ = caret_position_;
}

void TextEditor::move_to_start(bool extend) { move_to_line_start(extend); }
void TextEditor::move_to_end(bool extend) { move_to_line_end(extend); }

bool TextEditor::is_word_char(char c) const {
    return std::isalnum(static_cast<unsigned char>(c)) || c == '_';
}

// ── Event handling ───────────────────────────────────────────────────────

void TextEditor::on_mouse_event(const MouseEvent& event) {
    caret_blink_time_ = 0;  // reset blink on click
    if (!event.is_down) return;

    int pos = char_index_at_x(event.position.x);

    if (event.click_count == 3) {
        // Triple-click: select all (line in multi-line)
        select_all();
    } else if (event.click_count == 2) {
        // Double-click: select word
        int start = pos, end = pos;
        while (start > 0 && is_word_char(text_[static_cast<size_t>(start - 1)])) --start;
        while (end < static_cast<int>(text_.size()) && is_word_char(text_[static_cast<size_t>(end)])) ++end;
        selection_start_ = start;
        selection_end_ = end;
        caret_position_ = end;
    } else if (event.isShiftDown()) {
        // Shift-click: extend selection
        selection_end_ = pos;
        caret_position_ = pos;
    } else {
        // Single click: place caret
        caret_position_ = pos;
        selection_start_ = selection_end_ = pos;
    }
}

bool TextEditor::on_key_event(const KeyEvent& event) {
    if (!event.is_down) return false;

    bool shift = event.isShiftDown();
    bool mod = event.isMainModifier();  // Cmd on macOS, Ctrl on Win/Linux
    bool alt = event.isAltDown();

    switch (event.key) {
        case KeyCode::left:
            if (mod) move_to_line_start(shift);
            else if (alt) move_word(-1, shift);
            else move_caret(-1, shift);
            return true;

        case KeyCode::right:
            if (mod) move_to_line_end(shift);
            else if (alt) move_word(1, shift);
            else move_caret(1, shift);
            return true;

        case KeyCode::up:
            if (!multi_line) { move_to_start(shift); return true; }
            // TODO: multi-line up movement
            return true;

        case KeyCode::down:
            if (!multi_line) { move_to_end(shift); return true; }
            // TODO: multi-line down movement
            return true;

        case KeyCode::home:
            move_to_start(shift);
            return true;

        case KeyCode::end_:
            move_to_end(shift);
            return true;

        case KeyCode::backspace:
            if (has_selection()) { push_undo(); delete_selection(); notify_change(); }
            else delete_char_before_caret();
            return true;

        case KeyCode::delete_:
            if (has_selection()) { push_undo(); delete_selection(); notify_change(); }
            else delete_char_after_caret();
            return true;

        case KeyCode::enter:
            if (on_return) on_return(text_);
            return true;

        case KeyCode::escape:
            if (on_escape) on_escape();
            return true;

        case KeyCode::a:
            if (mod) { select_all(); return true; }
            break;
        case KeyCode::c:
            if (mod) { copy_to_clipboard(); return true; }
            break;
        case KeyCode::v:
            if (mod) { paste_from_clipboard(); return true; }
            break;
        case KeyCode::x:
            if (mod) { cut_to_clipboard(); return true; }
            break;
        case KeyCode::z:
            if (mod && shift) { redo(); return true; }
            if (mod) { undo(); return true; }
            break;

        default:
            break;
    }
    return false;
}

void TextEditor::on_text_input(const TextInputEvent& event) {
    caret_blink_time_ = 0;  // reset blink on input
    if (event.text.empty()) return;

    if (numeric_only) {
        for (char c : event.text) {
            if (!std::isdigit(c) && c != '.' && c != '-') return;
        }
    }

    push_undo();
    if (has_selection()) delete_selection();
    insert_text(event.text);
    notify_change();
}

void TextEditor::on_focus_changed(bool gained) {
    View::on_focus_changed(gained);  // sets has_focus_ for border rendering
    if (gained && select_on_focus) {
        select_all();
    }
}

// ── Painting ─────────────────────────────────────────────────────────────

void TextEditor::paint(canvas::Canvas& canvas) {
    auto b = local_bounds();

    // Background
    auto bg_color = has_focus() ? resolve_color("text_editor_focus_bg", canvas::Color::hex(0x2a2a4a))
                                : resolve_color("text_editor_bg", canvas::Color::hex(0x1a1a2e));
    canvas.set_fill_color(bg_color);
    canvas.fill_rounded_rect(b.x, b.y, b.width, b.height, 4);

    // Border — prominent accent border when focused (like original HTML)
    if (has_focus()) {
        // Always use a visible accent color for focus border
        canvas.set_stroke_color(canvas::Color::rgba(140, 120, 255, 255));
        canvas.set_line_width(2.0f);
    } else {
        canvas.set_stroke_color(resolve_color("border", canvas::Color::hex(0x3a3a5a)));
        canvas.set_line_width(1.0f);
    }
    canvas.stroke_rounded_rect(b.x, b.y, b.width, b.height, 6);

    canvas.set_font("system", font_size_);

    // Display text
    std::string display = text_;
    if (password_mode) {
        display = std::string(text_.size(), password_char);
    }

    float text_x = b.x + 6 - scroll_offset_;
    float text_y = b.y + b.height / 2 + font_size_ * 0.35f;

    // Selection highlight (using measured text widths for tight selection)
    if (has_selection()) {
        int start = std::min(selection_start_, selection_end_);
        int end = std::max(selection_start_, selection_end_);
        // Measure actual text width for accurate selection bounds
        float sel_x = text_x + canvas.measure_text(display.substr(0, static_cast<size_t>(start)));
        float sel_w = canvas.measure_text(display.substr(static_cast<size_t>(start),
                                                          static_cast<size_t>(end - start)));
        canvas.set_fill_color(resolve_color("selection", canvas::Color::rgba(65, 105, 225, 128)));
        canvas.fill_rect(sel_x, b.y + 2, sel_w, b.height - 4);
    }

    // Text or placeholder
    if (display.empty() && !placeholder.empty() && !has_focus()) {
        canvas.set_fill_color(resolve_color("text_muted", canvas::Color::hex(0x808090)));
        canvas.fill_text(placeholder, text_x, text_y);
    } else {
        canvas.set_fill_color(resolve_color("text", canvas::Color::hex(0xe0e0e0)));
        canvas.fill_text(display, text_x, text_y);
    }

    // Caret with blinking (530ms on, 530ms off)
    if (has_focus()) {
        caret_blink_time_ += 1.0f / 60.0f;  // approximate frame time
        bool caret_visible = std::fmod(caret_blink_time_, 1.06f) < 0.53f;
        if (caret_visible || has_selection()) {
            float caret_x = text_x + canvas.measure_text(display.substr(0, static_cast<size_t>(caret_position_)));
            canvas.set_stroke_color(resolve_color("text", canvas::Color::hex(0xe0e0e0)));
            canvas.set_line_width(1.5f);
            canvas.stroke_line(caret_x, b.y + 4, caret_x, b.y + b.height - 4);
        }
    }
}

int TextEditor::char_index_at_x(float x) const {
    float char_w = font_size_ * 0.6f;
    float text_x = 6 - scroll_offset_;
    int index = static_cast<int>((x - text_x) / char_w + 0.5f);
    return std::clamp(index, 0, static_cast<int>(text_.size()));
}

void TextEditor::notify_change() {
    if (on_change) on_change(text_);
}

// ── IME composition ─────────────────────────────────────────────────────────

void TextEditor::set_marked_text(const std::string& marked, int selected_pos, int selected_len) {
    // Remove any previous marked text
    if (!marked_text_.empty()) {
        text_.erase(static_cast<size_t>(marked_start_), marked_text_.size());
        caret_position_ = marked_start_;
    }

    marked_text_ = marked;
    marked_start_ = caret_position_;
    marked_selected_pos_ = selected_pos;
    marked_selected_len_ = selected_len;

    // Insert marked text at caret
    if (!marked.empty()) {
        text_.insert(static_cast<size_t>(marked_start_), marked);
        caret_position_ = marked_start_ + static_cast<int>(marked.size());
    }

    selection_start_ = selection_end_ = caret_position_;
}

void TextEditor::unmark_text() {
    // Marked text has already been inserted into text_; just clear the state
    marked_text_.clear();
    marked_start_ = 0;
    marked_selected_pos_ = 0;
    marked_selected_len_ = 0;
}

} // namespace pulp::view
