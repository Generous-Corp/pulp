#include <pulp/view/text_editor.hpp>
#include <algorithm>
#include <cctype>
#include <cmath>

namespace pulp::view {

namespace {

int clamp_position(const std::string& text, int position) {
    return std::clamp(position, 0, static_cast<int>(text.size()));
}

int line_start_for_position(const std::string& text, int position) {
    position = clamp_position(text, position);
    while (position > 0 && text[static_cast<size_t>(position - 1)] != '\n') {
        --position;
    }
    return position;
}

int line_end_for_position(const std::string& text, int position) {
    position = clamp_position(text, position);
    const int len = static_cast<int>(text.size());
    while (position < len && text[static_cast<size_t>(position)] != '\n') {
        ++position;
    }
    return position;
}

int vertical_line_position(const std::string& text, int caret, int direction) {
    // Hard `\n` line breaks only. Soft-wrap navigation across
    // visually-wrapped rows requires moving column bookkeeping out of
    // `caret_position_` (so the caret can ride wrap-induced visual
    // rows without changing logical position) and reading the wrap
    // geometry from `last_layout_` — out of scope here.
    caret = clamp_position(text, caret);
    const int current_start = line_start_for_position(text, caret);
    const int current_end = line_end_for_position(text, caret);
    const int column = caret - current_start;

    if (direction < 0) {
        if (current_start == 0) return caret;
        const int previous_end = current_start - 1;
        const int previous_start = line_start_for_position(text, previous_end);
        return previous_start + std::min(column, previous_end - previous_start);
    }

    if (current_end >= static_cast<int>(text.size())) return caret;
    const int next_start = current_end + 1;
    const int next_end = line_end_for_position(text, next_start);
    return next_start + std::min(column, next_end - next_start);
}

} // namespace

void TextEditor::set_text(const std::string& t) {
    push_undo();
    text_ = t;
    caret_position_ = has_focus() ? static_cast<int>(t.size()) : 0;
    selection_start_ = selection_end_ = caret_position_;
    scroll_offset_ = 0.0f;
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
    caret_position_ = line_start_for_position(text_, caret_position_);
    if (extend) selection_end_ = caret_position_;
    else selection_start_ = selection_end_ = caret_position_;
}

void TextEditor::move_to_line_end(bool extend) {
    caret_position_ = line_end_for_position(text_, caret_position_);
    if (extend) selection_end_ = caret_position_;
    else selection_start_ = selection_end_ = caret_position_;
}

void TextEditor::move_to_start(bool extend) {
    caret_position_ = 0;
    if (extend) selection_end_ = caret_position_;
    else selection_start_ = selection_end_ = caret_position_;
}

void TextEditor::move_to_end(bool extend) {
    caret_position_ = static_cast<int>(text_.size());
    if (extend) selection_end_ = caret_position_;
    else selection_start_ = selection_end_ = caret_position_;
}

bool TextEditor::is_word_char(char c) const {
    return std::isalnum(static_cast<unsigned char>(c)) || c == '_';
}

// ── Event handling ───────────────────────────────────────────────────────

void TextEditor::on_mouse_event(const MouseEvent& event) {
    caret_blink_time_ = 0;  // reset blink on click
    if (multi_line && event.is_wheel) {
        scroll_offset_ = std::max(0.0f, scroll_offset_ + event.scroll_delta_y);
        return;
    }
    if (!event.is_down) return;

    // char_index_at_point reads from the cached LayoutSnapshot; for
    // single-line that snapshot is one row of measured advances, so
    // hit-test accuracy doesn't depend on the 0.6em fallback heuristic
    // anymore.
    int pos = char_index_at_point(event.position.x, event.position.y);

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
            caret_position_ = vertical_line_position(text_, caret_position_, -1);
            if (shift) selection_end_ = caret_position_;
            else selection_start_ = selection_end_ = caret_position_;
            return true;

        case KeyCode::down:
            if (!multi_line) { move_to_end(shift); return true; }
            caret_position_ = vertical_line_position(text_, caret_position_, 1);
            if (shift) selection_end_ = caret_position_;
            else selection_start_ = selection_end_ = caret_position_;
            return true;

        case KeyCode::home:
            if (multi_line) move_to_line_start(shift);
            else move_to_start(shift);
            return true;

        case KeyCode::end_:
            if (multi_line) move_to_line_end(shift);
            else move_to_end(shift);
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
            if (multi_line && !mod) {
                push_undo();
                if (has_selection()) delete_selection();
                insert_text("\n");
                notify_change();
                return true;
            }
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

    auto bg_color = has_background_color()
        ? background_color()
        : (has_focus()
            ? resolve_color("text_editor_focus_bg",
                            resolve_color("bg.elevated",
                                          resolve_color("bg.surface", canvas::Color::hex(0x2a2a4a))))
            : resolve_color("text_editor_bg",
                            resolve_color("bg.surface", canvas::Color::hex(0x1a1a2e))));
    float radius = corner_radius() > 0.0f ? corner_radius() : 6.0f;
    float max_radius = std::max(0.0f, std::min(b.width, b.height) * 0.5f - 0.5f);
    radius = std::min(radius, max_radius);

    // Border — use explicit per-view styling when present, otherwise theme defaults.
    auto stroke = has_border()
        ? border_color()
        : (has_focus()
            ? resolve_color("accent.primary", canvas::Color::rgba8(140, 120, 255, 255))
            : resolve_color("control.border",
                            resolve_color("border", canvas::Color::hex(0x3a3a5a))));
    float stroke_width = has_border() ? border_width() : (has_focus() ? 2.0f : 1.0f);
    if (stroke_width > 0.0f) {
        canvas.set_fill_color(stroke);
        canvas.fill_rounded_rect(b.x, b.y, b.width, b.height, radius);

        float inset = std::max(0.0f, stroke_width);
        float inner_w = std::max(0.0f, b.width - inset * 2.0f);
        float inner_h = std::max(0.0f, b.height - inset * 2.0f);
        float inner_max_radius = std::max(0.0f, std::min(inner_w, inner_h) * 0.5f - 0.5f);
        float inner_radius = std::min(inner_max_radius, std::max(0.0f, radius - inset - 0.5f));
        canvas.set_fill_color(bg_color);
        canvas.fill_rounded_rect(b.x + inset, b.y + inset, inner_w, inner_h, inner_radius);
    } else {
        canvas.set_fill_color(bg_color);
        canvas.fill_rounded_rect(b.x, b.y, b.width, b.height, radius);
    }

    canvas.set_font("Inter", font_size_);
    canvas.set_text_align(canvas::TextAlign::left);

    // Display text
    std::string display = text_;
    if (password_mode) {
        display = std::string(text_.size(), password_char);
    }

    if (multi_line) {
        canvas.set_font("Inter", font_size_);
        canvas.set_text_align(canvas::TextAlign::left);
        const float inner_x = b.x + 6.0f;
        const float inner_y = b.y + 4.0f;
        const float inner_w = std::max(20.0f, b.width - 12.0f);
        const float line_h = font_size_ * 1.35f;

        struct WrappedLine {
            std::string text;
            int start = 0;
            int end = 0;
        };

        std::vector<WrappedLine> lines;
        lines.push_back({"", 0, 0});
        int current_start = 0;
        std::string current;

        auto flush_line = [&](int end_index) {
            lines.back().text = current;
            lines.back().start = current_start;
            lines.back().end = end_index;
            current.clear();
            current_start = end_index;
            lines.push_back({"", current_start, current_start});
        };

        for (int i = 0; i < static_cast<int>(display.size()); ++i) {
            char c = display[static_cast<size_t>(i)];
            if (c == '\n') {
                flush_line(i);
                current_start = i + 1;
                lines.back().start = current_start;
                lines.back().end = current_start;
                continue;
            }

            std::string candidate = current + c;
            if (!current.empty() && canvas.measure_text(candidate) > inner_w) {
                flush_line(i);
                current_start = i;
            }
            current += c;
        }

        lines.back().text = current;
        lines.back().start = current_start;
        lines.back().end = static_cast<int>(display.size());
        if (lines.size() > 1 && lines.back().text.empty() && lines[lines.size() - 2].end == static_cast<int>(display.size())) {
            lines.pop_back();
        }

        if (lines.empty()) lines.push_back({"", 0, 0});

        int caret_line = 0;
        int caret_column = 0;
        for (int i = 0; i < static_cast<int>(lines.size()); ++i) {
            auto& line = lines[static_cast<size_t>(i)];
            if (caret_position_ >= line.start && caret_position_ <= line.end) {
                caret_line = i;
                caret_column = std::clamp(caret_position_ - line.start, 0, static_cast<int>(line.text.size()));
                break;
            }
        }

        float visible_h = std::max(line_h, b.height - 8.0f);
        float total_h = std::max(visible_h, static_cast<float>(lines.size()) * line_h);
        float caret_top = caret_line * line_h;
        float caret_bottom = caret_top + line_h;
        if (caret_bottom - scroll_offset_ > visible_h) {
            scroll_offset_ = caret_bottom - visible_h;
        } else if (caret_top - scroll_offset_ < 0) {
            scroll_offset_ = caret_top;
        }
        scroll_offset_ = std::clamp(scroll_offset_, 0.0f, std::max(0.0f, total_h - visible_h));

        // Cache-keyed snapshot: only rebuild the per-character offset
        // table when an input that affects layout changes (text, font,
        // bounds, scroll, mode). On a quiet 60Hz frame this collapses
        // to a key compare — `measure_text` is multi-millisecond on
        // the SkParagraph path and the legacy code rebuilt it every
        // paint, blowing the audio-UI thread budget.
        LayoutCacheKey key{
            std::hash<std::string>{}(display),
            font_size_,
            b.width,
            b.height,
            scroll_offset_,
            /*multi_line=*/true,
            password_mode,
            /*placeholder_visible=*/display.empty() && !placeholder.empty() && !has_focus(),
        };
        if (!(last_layout_key_ == key) || last_layout_.lines.size() != lines.size()) {
            last_layout_.multi_line = true;
            last_layout_.lines.clear();
            last_layout_.lines.reserve(lines.size());
            last_layout_.fallback_char_w = canvas.measure_text("M");
            for (int i = 0; i < static_cast<int>(lines.size()); ++i) {
                const auto& src = lines[static_cast<size_t>(i)];
                LayoutSnapshot::Line dst;
                dst.start = src.start;
                dst.end = src.end;
                dst.top_y = inner_y + i * line_h - scroll_offset_;
                dst.baseline_y = dst.top_y + font_size_;
                dst.inner_x = inner_x;
                dst.line_height = line_h;
                // O(N) single-char accumulation. Loses inter-glyph
                // kerning vs full-prefix shaping, but the legacy
                // substr-measure loop was O(N²) Skia-paragraph builds
                // per paint — unshippable on the hot path.
                dst.x_offsets.resize(src.text.size() + 1);
                dst.x_offsets[0] = 0.f;
                float cum = 0.f;
                for (size_t j = 0; j < src.text.size(); ++j) {
                    cum += canvas.measure_text(std::string(1, src.text[j]));
                    dst.x_offsets[j + 1] = cum;
                }
                last_layout_.lines.push_back(std::move(dst));
            }
            last_layout_key_ = key;
        } else {
            // Cache hit — refresh only the y/x baselines that depend on
            // scroll_offset_ + bounds origin (cheap; no measure calls).
            for (int i = 0; i < static_cast<int>(lines.size()); ++i) {
                auto& dst = last_layout_.lines[static_cast<size_t>(i)];
                dst.top_y = inner_y + i * line_h - scroll_offset_;
                dst.baseline_y = dst.top_y + font_size_;
                dst.inner_x = inner_x;
            }
        }

        if (display.empty() && !placeholder.empty() && !has_focus()) {
            canvas.set_fill_color(resolve_color("text.secondary", canvas::Color::hex(0x808090)));
            canvas.fill_text(placeholder, inner_x, inner_y + font_size_);
        } else {
            for (int i = 0; i < static_cast<int>(lines.size()); ++i) {
                float baseline_y = inner_y + font_size_ + i * line_h - scroll_offset_;
                if (baseline_y < b.y - line_h || baseline_y > b.y + b.height + line_h) continue;
                canvas.set_fill_color(resolve_color("text.primary", canvas::Color::hex(0xe0e0e0)));
                canvas.fill_text(lines[static_cast<size_t>(i)].text, inner_x, baseline_y);
            }
        }

        if (has_focus()) {
            caret_blink_time_ += 1.0f / 60.0f;
            bool caret_visible = std::fmod(caret_blink_time_, 1.06f) < 0.53f;
            if (caret_visible || has_selection()) {
                // Read caret x from the cached snapshot instead of
                // re-measuring a prefix substring every paint.
                const auto& snap_line =
                    last_layout_.lines[static_cast<size_t>(caret_line)];
                size_t col = std::clamp<size_t>(caret_column, 0,
                    snap_line.x_offsets.empty() ? 0 : snap_line.x_offsets.size() - 1);
                float caret_x = inner_x + (snap_line.x_offsets.empty()
                    ? 0.f : snap_line.x_offsets[col]);
                float caret_y = inner_y + caret_line * line_h - scroll_offset_;
                canvas.set_stroke_color(resolve_color("text.primary", canvas::Color::hex(0xe0e0e0)));
                canvas.set_line_width(1.5f);
                canvas.stroke_line(caret_x, caret_y, caret_x, caret_y + line_h - 2.0f);
            }
        }
        return;
    }

    const float text_pad_x = std::max(9.0f, border_width() + 7.0f);
    const float text_inner_x = b.x + text_pad_x;
    const float text_inner_w = std::max(0.0f, b.width - text_pad_x * 2.0f);
    const auto metrics = canvas.measure_text_full(display.empty() ? std::string("Ag") : display);
    const float text_y = b.y + std::max(0.0f, (b.height - metrics.line_height) * 0.5f) + metrics.ascent;

    // Cache-keyed single-line snapshot. Skips the per-char measure
    // rebuild on quiet frames. See multi-line block above for the
    // identical pattern + rationale.
    LayoutCacheKey key{
        std::hash<std::string>{}(display),
        font_size_,
        b.width,
        b.height,
        scroll_offset_,
        /*multi_line=*/false,
        password_mode,
        /*placeholder_visible=*/display.empty() && !placeholder.empty() && !has_focus(),
    };
    if (!(last_layout_key_ == key) || last_layout_.lines.size() != 1) {
        last_layout_.multi_line = false;
        last_layout_.fallback_char_w = canvas.measure_text("M");
        last_layout_.lines.clear();
        LayoutSnapshot::Line line_snap;
        line_snap.start = 0;
        line_snap.end = static_cast<int>(display.size());
        line_snap.top_y = b.y + std::max(0.0f, (b.height - metrics.line_height) * 0.5f);
        line_snap.baseline_y = text_y;
        line_snap.line_height = metrics.line_height > 0.f ? metrics.line_height : font_size_;
        line_snap.inner_x = text_inner_x;
        line_snap.x_offsets.resize(display.size() + 1);
        line_snap.x_offsets[0] = 0.f;
        float cum = 0.f;
        for (size_t j = 0; j < display.size(); ++j) {
            cum += canvas.measure_text(std::string(1, display[j]));
            line_snap.x_offsets[j + 1] = cum;
        }
        last_layout_.lines.push_back(std::move(line_snap));
        last_layout_key_ = key;
    } else if (!last_layout_.lines.empty()) {
        auto& dst = last_layout_.lines.front();
        dst.top_y = b.y + std::max(0.0f, (b.height - metrics.line_height) * 0.5f);
        dst.baseline_y = text_y;
        dst.inner_x = text_inner_x;
    }
    const float total_text_w = last_layout_.lines.front().x_offsets.back();
    const float caret_w = last_layout_.lines.front()
        .x_offsets[std::clamp<size_t>(caret_position_, 0, display.size())];

    if (has_focus() || has_selection()) {
        if (caret_w - scroll_offset_ > text_inner_w) {
            scroll_offset_ = caret_w - text_inner_w;
        } else if (caret_w - scroll_offset_ < 0.0f) {
            scroll_offset_ = caret_w;
        }
        scroll_offset_ = std::clamp(scroll_offset_, 0.0f, std::max(0.0f, total_text_w - text_inner_w));
    } else {
        scroll_offset_ = 0.0f;
    }

    float text_x = text_inner_x - scroll_offset_;
    auto text_primary = resolve_color("text.primary", canvas::Color::hex(0xe0e0e0));
    auto text_secondary = resolve_color("text.secondary", canvas::Color::hex(0x808090));
    auto selection_fill = resolve_color("accent.primary", canvas::Color::rgba8(65, 105, 225, 255));
    selection_fill.a = 168;
    auto selected_text_color = resolve_color("bg.primary", bg_color);

    canvas.save();
    canvas.clip_rect(text_inner_x - 2.0f, b.y + 2.0f, text_inner_w + 4.0f, std::max(0.0f, b.height - 4.0f));

    // Reuse the cached single-line snapshot for all per-glyph x lookups
    // (selection start/width, before/selected/after text origins). The
    // legacy code called `canvas.measure_text(substr(...))` four times
    // per paint, each going through SkParagraph build+layout.
    const auto& xoff = last_layout_.lines.front().x_offsets;
    auto x_at = [&](int idx) -> float {
        size_t i = std::clamp<size_t>(idx, 0, display.size());
        return xoff[i];
    };

    if (has_selection()) {
        int start = std::min(selection_start_, selection_end_);
        int end = std::max(selection_start_, selection_end_);
        float sel_x = text_x + x_at(start);
        float sel_w = x_at(end) - x_at(start);
        canvas.set_fill_color(selection_fill);
        canvas.fill_rect(sel_x, b.y + 2, sel_w, b.height - 4);
    }

    if (display.empty() && !placeholder.empty() && !has_focus()) {
        canvas.set_fill_color(text_secondary);
        canvas.fill_text(placeholder, text_x, text_y);
    } else {
        if (has_selection()) {
            int start = std::min(selection_start_, selection_end_);
            int end = std::max(selection_start_, selection_end_);
            auto before = display.substr(0, static_cast<size_t>(start));
            auto selected = display.substr(static_cast<size_t>(start), static_cast<size_t>(end - start));
            auto after = display.substr(static_cast<size_t>(end));
            float selected_x = text_x + x_at(start);
            float after_x = text_x + x_at(end);

            canvas.set_fill_color(text_primary);
            if (!before.empty()) canvas.fill_text(before, text_x, text_y);
            canvas.set_fill_color(selected_text_color);
            if (!selected.empty()) canvas.fill_text(selected, selected_x, text_y);
            canvas.set_fill_color(text_primary);
            if (!after.empty()) canvas.fill_text(after, after_x, text_y);
        } else {
            canvas.set_fill_color(text_primary);
            canvas.fill_text(display, text_x, text_y);
        }
    }

    // Caret with blinking (530ms on, 530ms off)
    if (has_focus()) {
        caret_blink_time_ += 1.0f / 60.0f;  // approximate frame time
        bool caret_visible = std::fmod(caret_blink_time_, 1.06f) < 0.53f;
        if (caret_visible || has_selection()) {
            float caret_x = text_x + canvas.measure_text(display.substr(0, static_cast<size_t>(caret_position_)));
            canvas.set_stroke_color(resolve_color("text.primary", canvas::Color::hex(0xe0e0e0)));
            canvas.set_line_width(1.5f);
            canvas.stroke_line(caret_x, b.y + 4, caret_x, b.y + b.height - 4);
        }
    }
    canvas.restore();
}

int TextEditor::char_index_at_x(float x) const {
    float char_w = font_size_ * 0.6f;
    float text_x = std::max(9.0f, border_width() + 7.0f) - scroll_offset_;
    int index = static_cast<int>((x - text_x) / char_w + 0.5f);
    return std::clamp(index, 0, static_cast<int>(text_.size()));
}

int TextEditor::char_index_at_point(float x, float y) const {
    // If paint() has not yet populated a layout snapshot (e.g. the first
    // mouse event arrives before the first frame), fall back to the
    // legacy x-only routine — the y is just lost in that case. Once the
    // first paint runs, the snapshot is authoritative for both single-
    // and multi-line: single-line records one row whose x_offsets are
    // real measured advances, so the row-walk below collapses to that
    // single row and picks the right column.
    if (last_layout_.lines.empty()) {
        return char_index_at_x(x);
    }

    // Pick the row by y: clamp to first/last row when the click is
    // above/below the content, otherwise find the row whose vertical
    // band contains y. This keeps a click in the gap between rows from
    // collapsing to row 0.
    const auto& rows = last_layout_.lines;
    int row_index = 0;
    if (y <= rows.front().top_y) {
        row_index = 0;
    } else if (y >= rows.back().top_y + rows.back().line_height) {
        row_index = static_cast<int>(rows.size()) - 1;
    } else {
        for (int i = 0; i < static_cast<int>(rows.size()); ++i) {
            const auto& row = rows[static_cast<size_t>(i)];
            if (y >= row.top_y && y < row.top_y + row.line_height) {
                row_index = i;
                break;
            }
        }
    }

    const auto& row = rows[static_cast<size_t>(row_index)];
    const float local_x = x - row.inner_x;
    if (row.x_offsets.empty()) return row.start;

    // Nearest-edge hit-test: find the char boundary whose x is closest
    // to the click. Matches the half-glyph convention single-line uses.
    int best = 0;
    float best_dist = std::abs(local_x - row.x_offsets[0]);
    for (size_t j = 1; j < row.x_offsets.size(); ++j) {
        float d = std::abs(local_x - row.x_offsets[j]);
        if (d < best_dist) {
            best_dist = d;
            best = static_cast<int>(j);
        }
    }
    return std::clamp(row.start + best, 0, static_cast<int>(text_.size()));
}

Rect TextEditor::caret_rect() const {
    // No paint has run yet: anchor the caret to the inner padding so an
    // IME host querying us before the first frame still gets a
    // non-degenerate rect. All coordinates returned by `caret_rect()`
    // are in local view space, matching what `paint()` records.
    if (last_layout_.lines.empty()) {
        Rect fallback;
        fallback.x = std::max(9.0f, border_width() + 7.0f);
        fallback.y = 2.0f;
        fallback.width = 1.5f;
        fallback.height = std::max(font_size_, local_bounds().height - 4.0f);
        return fallback;
    }

    // Find the row that owns the caret. The wrap path puts the caret on
    // the row whose [start,end] band brackets the codepoint; the
    // single-line path always has exactly one row so this still works.
    int row_index = 0;
    for (int i = 0; i < static_cast<int>(last_layout_.lines.size()); ++i) {
        const auto& row = last_layout_.lines[static_cast<size_t>(i)];
        if (caret_position_ >= row.start && caret_position_ <= row.end) {
            row_index = i;
            break;
        }
    }

    const auto& row = last_layout_.lines[static_cast<size_t>(row_index)];
    int col = std::clamp(caret_position_ - row.start, 0,
                         static_cast<int>(row.x_offsets.empty() ? 0 : row.x_offsets.size() - 1));
    float caret_x = row.inner_x + (row.x_offsets.empty() ? 0.f : row.x_offsets[static_cast<size_t>(col)]);

    Rect r;
    r.x = caret_x;
    r.y = row.top_y;
    r.width = 1.5f;
    r.height = row.line_height;
    return r;
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
