#include <pulp/view/text_editor.hpp>
#include <pulp/view/widget_metrics.hpp>
#include <pulp/view/context_menu.hpp>
#include <pulp/view/frame_clock.hpp>  // caret-blink subscription
#include "text_edit_model.hpp"
#include <algorithm>
#include <cmath>
#include <optional>
#include <vector>

namespace pulp::view {

namespace {

enum TextEditorMenuCommand {
    kTextEditorMenuCut = 1,
    kTextEditorMenuCopy,
    kTextEditorMenuPaste,
    kTextEditorMenuSelectAll,
};

View* root_view(View* view) {
    if (!view) return nullptr;
    while (view->parent()) view = view->parent();
    return view;
}

Point local_to_root(View* view, Point p) {
    for (auto* current = view; current; current = current->parent()) {
        const auto b = current->bounds();
        p.x += b.x;
        p.y += b.y;
    }
    return p;
}

bool row_owns_caret(int caret, int start, int end, bool has_next, int next_start) {
    if (caret < start || caret > end) return false;
    if (caret < end) return true;
    if (start == end) return true;
    return !has_next || next_start != end;
}

text_edit::LineEndingMode line_ending_mode_for(TextEditor::LineEndingPolicy policy) {
    switch (policy) {
        case TextEditor::LineEndingPolicy::strip:
            return text_edit::LineEndingMode::strip;
        case TextEditor::LineEndingPolicy::preserve:
            return text_edit::LineEndingMode::preserve;
        case TextEditor::LineEndingPolicy::normalize:
        default:
            return text_edit::LineEndingMode::normalize;
    }
}

} // namespace

void TextEditor::set_text(const std::string& t) {
    if (text_ == t) return;
    text_ = t;
    caret_position_ = has_focus() ? text_edit::clamp_boundary(t, static_cast<int>(t.size())) : 0;
    selection_start_ = selection_end_ = caret_position_;
    scroll_offset_ = 0.0f;
    marked_text_.clear();
    marked_start_ = 0;
    marked_selected_pos_ = 0;
    marked_selected_len_ = 0;
    marked_undo_active_ = false;
    undo_history_.clear();
    redo_history_.clear();
    invalidate_layout_cache();
    break_undo_coalescing();
    reset_preferred_horizontal();
    if (has_focus())
        keep_caret_solid();
    else
        request_repaint();
    notify_change();
}

std::string TextEditor::selected_text() const {
    if (!has_selection()) return {};
    if (password_mode && !password_contents_allowed()) return {};
    int start = text_edit::clamp_boundary(text_, std::min(selection_start_, selection_end_));
    int end = text_edit::clamp_boundary(text_, std::max(selection_start_, selection_end_));
    return text_.substr(static_cast<size_t>(start),
                        static_cast<size_t>(end - start));
}

void TextEditor::select_all() {
    selection_start_ = 0;
    selection_end_ = text_edit::clamp_boundary(text_, static_cast<int>(text_.size()));
    caret_position_ = selection_end_;
    reset_preferred_horizontal();
    request_repaint();
}

void TextEditor::clear_selection() {
    selection_start_ = selection_end_ = caret_position_;
    reset_preferred_horizontal();
    request_repaint();
}

void TextEditor::set_caret_pos(int byte_offset) {
    caret_position_ = text_edit::clamp_boundary(text_, byte_offset);
    selection_start_ = selection_end_ = caret_position_;
    reset_preferred_horizontal();
    break_undo_coalescing();
    keep_caret_solid();
}

void TextEditor::set_selection(int anchor_byte_offset, int active_byte_offset) {
    selection_start_ = text_edit::clamp_boundary(text_, anchor_byte_offset);
    selection_end_ = text_edit::clamp_boundary(text_, active_byte_offset);
    caret_position_ = selection_end_;
    reset_preferred_horizontal();
    break_undo_coalescing();
    keep_caret_solid();
}

std::pair<int, int> TextEditor::selection_range() const {
    const int start = text_edit::clamp_boundary(text_, std::min(selection_start_, selection_end_));
    const int end = text_edit::clamp_boundary(text_, std::max(selection_start_, selection_end_));
    return {start, end};
}

// ── Undo/Redo ────────────────────────────────────────────────────────────

TextEditor::UndoSnapshot TextEditor::snapshot() const {
    return {
        .text = text_,
        .caret_position = caret_position_,
        .selection_start = selection_start_,
        .selection_end = selection_end_,
        .scroll_offset = scroll_offset_,
    };
}

void TextEditor::restore_snapshot(const UndoSnapshot& s) {
    text_ = s.text;
    caret_position_ = text_edit::clamp_boundary(text_, s.caret_position);
    selection_start_ = text_edit::clamp_boundary(text_, s.selection_start);
    selection_end_ = text_edit::clamp_boundary(text_, s.selection_end);
    scroll_offset_ = std::max(0.0f, s.scroll_offset);
    marked_text_.clear();
    marked_start_ = 0;
    marked_selected_pos_ = 0;
    marked_selected_len_ = 0;
    marked_undo_active_ = false;
    invalidate_layout_cache();
    reset_preferred_horizontal();
}

void TextEditor::push_undo(UndoCoalesce coalesce) {
    if (coalesce != UndoCoalesce::none && last_undo_coalesce_ == coalesce && !undo_history_.empty())
        return;
    undo_history_.push_back(snapshot());
    if (undo_history_.size() > kMaxUndoHistory) {
        undo_history_.erase(undo_history_.begin());
    }
    redo_history_.clear();
    last_undo_coalesce_ = coalesce;
}

void TextEditor::break_undo_coalescing() {
    last_undo_coalesce_ = UndoCoalesce::none;
}

bool TextEditor::undo() {
    if (!can_edit()) return false;
    if (undo_history_.empty()) return false;
    redo_history_.push_back(snapshot());
    auto s = undo_history_.back();
    undo_history_.pop_back();
    restore_snapshot(s);
    break_undo_coalescing();
    keep_caret_solid();
    notify_change();
    return true;
}

bool TextEditor::redo() {
    if (!can_edit()) return false;
    if (redo_history_.empty()) return false;
    undo_history_.push_back(snapshot());
    auto s = redo_history_.back();
    redo_history_.pop_back();
    restore_snapshot(s);
    break_undo_coalescing();
    keep_caret_solid();
    notify_change();
    return true;
}

// ── Text manipulation ────────────────────────────────────────────────────

void TextEditor::insert_text(const std::string& t) {
    caret_position_ = text_edit::clamp_boundary(text_, caret_position_);
    text_.insert(static_cast<size_t>(caret_position_), t);
    caret_position_ = text_edit::clamp_boundary(text_, caret_position_ + static_cast<int>(t.size()));
    selection_start_ = selection_end_ = caret_position_;
    invalidate_layout_cache();
    reset_preferred_horizontal();
}

void TextEditor::delete_selection() {
    int start = text_edit::clamp_boundary(text_, std::min(selection_start_, selection_end_));
    int end = text_edit::clamp_boundary(text_, std::max(selection_start_, selection_end_));
    text_.erase(static_cast<size_t>(start), static_cast<size_t>(end - start));
    caret_position_ = start;
    selection_start_ = selection_end_ = caret_position_;
    invalidate_layout_cache();
    reset_preferred_horizontal();
}

void TextEditor::delete_char_before_caret() {
    if (caret_position_ > 0) {
        const int end = text_edit::clamp_boundary(text_, caret_position_);
        const int start = text_edit::previous_cluster(text_, end);
        delete_range(start, end, UndoCoalesce::backspace);
    }
}

void TextEditor::delete_char_after_caret() {
    if (caret_position_ < static_cast<int>(text_.size())) {
        const int start = text_edit::clamp_boundary(text_, caret_position_);
        const int end = text_edit::next_cluster(text_, start);
        delete_range(start, end, UndoCoalesce::delete_forward);
    }
}

void TextEditor::delete_word_before_caret() {
    const int end = text_edit::clamp_boundary(text_, caret_position_);
    delete_range(text_edit::previous_word_start(text_, end), end);
}

void TextEditor::delete_word_after_caret() {
    const int start = text_edit::clamp_boundary(text_, caret_position_);
    delete_range(start, text_edit::next_word_start(text_, start));
}

void TextEditor::delete_to_line_start() {
    const int end = text_edit::clamp_boundary(text_, caret_position_);
    delete_range(text_edit::line_start(text_, end), end);
}

void TextEditor::delete_to_line_end() {
    const int start = text_edit::clamp_boundary(text_, caret_position_);
    delete_range(start, text_edit::line_end(text_, start));
}

bool TextEditor::candidate_is_valid(int replace_start, int replace_end, std::string_view insertion) const {
    replace_start = text_edit::clamp_boundary(text_, replace_start);
    replace_end = text_edit::clamp_boundary(text_, replace_end);
    if (replace_end < replace_start) std::swap(replace_start, replace_end);
    std::string candidate = text_;
    candidate.replace(static_cast<std::size_t>(replace_start),
                      static_cast<std::size_t>(replace_end - replace_start),
                      insertion.data(), insertion.size());
    return !validator || validator(candidate);
}

bool TextEditor::delete_range(int start, int end, UndoCoalesce coalesce) {
    if (!can_edit()) return false;
    start = text_edit::clamp_boundary(text_, start);
    end = text_edit::clamp_boundary(text_, end);
    if (end < start) std::swap(start, end);
    if (start == end) return false;
    if (!candidate_is_valid(start, end, {})) return false;

    push_undo(coalesce);
    text_.erase(static_cast<std::size_t>(start), static_cast<std::size_t>(end - start));
    caret_position_ = start;
    selection_start_ = selection_end_ = caret_position_;
    invalidate_layout_cache();
    reset_preferred_horizontal();
    keep_caret_solid();
    notify_change();
    return true;
}

bool TextEditor::replace_selection_or_insert(std::string text, UndoCoalesce coalesce, bool from_paste) {
    if (!can_edit()) return false;
    text = text_edit::normalize_insert_text(
        std::move(text), multi_line, line_ending_mode_for(line_ending_policy));
    if (from_paste && paste_sanitizer) text = paste_sanitizer(text);
    if (numeric_only) text = text_edit::filter_numeric(std::move(text));
    if (input_filter) text = input_filter(text);
    if (text.empty()) return false;

    int start = has_selection()
        ? text_edit::clamp_boundary(text_, std::min(selection_start_, selection_end_))
        : text_edit::clamp_boundary(text_, caret_position_);
    int end = has_selection()
        ? text_edit::clamp_boundary(text_, std::max(selection_start_, selection_end_))
        : start;

    std::string existing_after_replace = text_;
    existing_after_replace.erase(static_cast<std::size_t>(start),
                                 static_cast<std::size_t>(end - start));
    text = text_edit::truncate_to_cluster_count(existing_after_replace, std::move(text), max_length);
    if (text.empty()) return false;
    if (!candidate_is_valid(start, end, text)) return false;

    push_undo(has_selection() ? UndoCoalesce::none : coalesce);
    if (has_selection()) delete_selection();
    insert_text(text);
    keep_caret_solid();
    notify_change();
    return true;
}

// ── Caret movement ───────────────────────────────────────────────────────

void TextEditor::move_caret(int delta, bool extend) {
    reset_preferred_horizontal();
    // A plain arrow (no Shift) with an active selection COLLAPSES the selection
    // to its edge in the direction of travel — without advancing an extra
    // cluster. Right arrow → caret at the selection end; Left arrow → caret at
    // the start. The old code always applied the delta first, so collapsing a
    // selection and pressing → landed one cluster PAST the selection (e.g.
    // skipping into the next word). Standard macOS/Windows text-edit behavior.
    if (!extend && has_selection()) {
        const int lo = std::min(selection_start_, selection_end_);
        const int hi = std::max(selection_start_, selection_end_);
        caret_position_ = (delta < 0) ? lo : hi;
        selection_start_ = selection_end_ = caret_position_;
        break_undo_coalescing();
        keep_caret_solid();
        return;
    }
    caret_position_ = text_edit::move_clusters(text_, caret_position_, delta);
    if (extend)
        selection_end_ = caret_position_;
    else
        selection_start_ = selection_end_ = caret_position_;
    break_undo_coalescing();
    keep_caret_solid();
}

void TextEditor::move_word(int direction, bool extend) {
    reset_preferred_horizontal();
    int pos = direction > 0
        ? text_edit::next_word_start(text_, caret_position_)
        : text_edit::previous_word_start(text_, caret_position_);
    caret_position_ = pos;
    if (extend) selection_end_ = pos;
    else selection_start_ = selection_end_ = pos;
    break_undo_coalescing();
    keep_caret_solid();
}

void TextEditor::move_visual_line(int direction, bool extend) {
    if (!multi_line) {
        if (direction < 0) move_to_start(extend);
        else move_to_end(extend);
        return;
    }

    auto apply_position = [&](int pos) {
        caret_position_ = text_edit::clamp_boundary(text_, pos);
        if (extend) selection_end_ = caret_position_;
        else selection_start_ = selection_end_ = caret_position_;
        break_undo_coalescing();
        keep_caret_solid();
    };

    if (last_layout_.multi_line && !last_layout_.lines.empty()) {
        int current_row = 0;
        for (int i = 0; i < static_cast<int>(last_layout_.lines.size()); ++i) {
            const auto& row = last_layout_.lines[static_cast<size_t>(i)];
            const bool has_next = i + 1 < static_cast<int>(last_layout_.lines.size());
            const int next_start = has_next
                ? last_layout_.lines[static_cast<size_t>(i + 1)].start
                : row.end;
            if (row_owns_caret(caret_position_, row.start, row.end, has_next, next_start)) {
                current_row = i;
                break;
            }
        }

        const int target_row = current_row + direction;
        if (target_row < 0 || target_row >= static_cast<int>(last_layout_.lines.size())) {
            apply_position(caret_position_);
            return;
        }

        const auto& current = last_layout_.lines[static_cast<size_t>(current_row)];
        if (!has_preferred_horizontal_) {
            const int current_col = text_edit::cluster_index_for_position(current.byte_offsets, caret_position_);
            preferred_text_column_ = current_col;
            preferred_visual_x_ = current.inner_x +
                (current.x_offsets.empty() ? 0.0f : current.x_offsets[static_cast<size_t>(current_col)]);
            has_preferred_horizontal_ = true;
        }

        const auto& target = last_layout_.lines[static_cast<size_t>(target_row)];
        if (target.x_offsets.empty()) {
            apply_position(target.start);
            return;
        }

        const float local_x = preferred_visual_x_ - target.inner_x;
        int best = 0;
        float best_dist = std::abs(local_x - target.x_offsets[0]);
        for (size_t i = 1; i < target.x_offsets.size(); ++i) {
            const float dist = std::abs(local_x - target.x_offsets[i]);
            if (dist < best_dist) {
                best_dist = dist;
                best = static_cast<int>(i);
            }
        }
        if (!target.byte_offsets.empty())
            apply_position(target.byte_offsets[static_cast<std::size_t>(best)]);
        else
            apply_position(target.start);
        return;
    }

    if (!has_preferred_horizontal_) {
        preferred_text_column_ = text_edit::cluster_column_in_line(
            text_, text_edit::line_start(text_, caret_position_), caret_position_);
        preferred_visual_x_ = static_cast<float>(preferred_text_column_);
        has_preferred_horizontal_ = true;
    }
    apply_position(text_edit::visual_line_position(text_, caret_position_, direction, preferred_text_column_));
}

void TextEditor::move_page(int direction, bool extend) {
    if (!multi_line) {
        if (direction < 0) move_to_start(extend);
        else move_to_end(extend);
        return;
    }

    int rows = 8;
    if (!last_layout_.lines.empty()) {
        const float line_h = std::max(1.0f, last_layout_.lines.front().line_height);
        rows = std::max(1, static_cast<int>(std::max(1.0f, local_bounds().height) / line_h) - 1);
    }
    for (int i = 0; i < rows; ++i) move_visual_line(direction, extend);
}

void TextEditor::move_paragraph(int direction, bool extend) {
    reset_preferred_horizontal();
    caret_position_ = direction < 0
        ? text_edit::line_start(text_, caret_position_)
        : text_edit::line_end(text_, caret_position_);
    if (extend) selection_end_ = caret_position_;
    else selection_start_ = selection_end_ = caret_position_;
    break_undo_coalescing();
    keep_caret_solid();
}

void TextEditor::move_to_line_start(bool extend) {
    if (!multi_line) {
        move_to_start(extend);
        return;
    }
    reset_preferred_horizontal();
    caret_position_ = text_edit::line_start(text_, caret_position_);
    if (extend) selection_end_ = caret_position_;
    else selection_start_ = selection_end_ = caret_position_;
    break_undo_coalescing();
    keep_caret_solid();
}

void TextEditor::move_to_line_end(bool extend) {
    if (!multi_line) {
        move_to_end(extend);
        return;
    }
    reset_preferred_horizontal();
    caret_position_ = text_edit::line_end(text_, caret_position_);
    if (extend) selection_end_ = caret_position_;
    else selection_start_ = selection_end_ = caret_position_;
    break_undo_coalescing();
    keep_caret_solid();
}

void TextEditor::move_to_start(bool extend) {
    reset_preferred_horizontal();
    caret_position_ = 0;
    if (extend) selection_end_ = caret_position_;
    else selection_start_ = selection_end_ = caret_position_;
    break_undo_coalescing();
    keep_caret_solid();
}

void TextEditor::move_to_end(bool extend) {
    reset_preferred_horizontal();
    caret_position_ = text_edit::clamp_boundary(text_, static_cast<int>(text_.size()));
    if (extend) selection_end_ = caret_position_;
    else selection_start_ = selection_end_ = caret_position_;
    break_undo_coalescing();
    keep_caret_solid();
}

std::pair<int, int> TextEditor::word_range_at_position(int position) const {
    return text_edit::word_range_at(text_, position);
}

std::pair<int, int> TextEditor::line_range_at_position(int position) const {
    return text_edit::line_range_at(text_, position);
}

void TextEditor::show_default_context_menu(Point local_pos) {
    auto* root = root_view(this);
    if (!root) return;

    std::vector<ContextMenu::Item> items{
        {kTextEditorMenuCut, "Cut",
         can_edit() && has_selection() && clipboard_export_allowed()},
        {kTextEditorMenuCopy, "Copy",
         has_selection() && clipboard_export_allowed()},
        {kTextEditorMenuPaste, "Paste",
         clipboard_import_allowed() && platform::Clipboard::has_text()},
        ContextMenu::Item::make_separator(),
        {kTextEditorMenuSelectAll, "Select All",
         enabled() && !text_.empty()},
    };

    auto alive = import_binding_lifetime_token();
    ContextMenu::show(root, local_to_root(this, local_pos), std::move(items),
        [alive, this](std::optional<int> command) {
            if (alive.expired()) return;
            if (command) {
                switch (*command) {
                    case kTextEditorMenuCut:       cut_to_clipboard(); break;
                    case kTextEditorMenuCopy:      copy_to_clipboard(); break;
                    case kTextEditorMenuPaste:     paste_from_clipboard(); break;
                    case kTextEditorMenuSelectAll: select_all(); break;
                    default: break;
                }
                if (alive.expired()) return;
            }
            if (enabled() && focusable()) {
                if (!has_focus()) on_focus_changed(true);
                claim_input_focus();
            }
        });
}

void TextEditor::invalidate_layout_cache() const {
    last_layout_.lines.clear();
    last_layout_key_ = {};
}

void TextEditor::cancel_drag_selection(int pointer_id) {
    drag_selecting_ = false;
    drag_selecting_words_ = false;
    suppress_next_legacy_mouse_down_ = false;
    if (has_drag_pointer_capture_) release_pointer_capture(drag_pointer_id_);
    release_pointer_capture(pointer_id);
    has_drag_pointer_capture_ = false;
}

void TextEditor::reset_preferred_horizontal() {
    has_preferred_horizontal_ = false;
    preferred_visual_x_ = 0.0f;
    preferred_text_column_ = 0;
}

void TextEditor::keep_caret_solid() {
    caret_blink_.keep_solid();
    request_repaint();
}

void TextEditor::advance_caret_blink(float dt) {
    if (!has_focus()) return;
    caret_blink_.advance(dt);
}

float TextEditor::intrinsic_height() const {
    EdgeInsets pad{};
    const float stock_pad = std::max(9.0f, border_width() + 7.0f);
    pad = EdgeInsets{stock_pad * 0.5f, stock_pad, stock_pad * 0.5f, stock_pad};
    if (auto* m = effective_metrics(); m != nullptr) {
        EdgeInsets from_delegate;
        if (m->text_field_insets(from_delegate, const_cast<TextEditor&>(*this)))
            pad = from_delegate;
    }
    if (has_own_insets_) pad = insets_;

    float size = font_size_;
    if (auto* m = effective_metrics(); m != nullptr) {
        FontSpec f;
        if (m->text_field_font(f, const_cast<TextEditor&>(*this)) && f.size > 0.0f)
            size = f.size;
    }
    // A conservative single-line box: the em size plus the usual ascender +
    // descender allowance, then the frame.
    const float line = std::ceil(size * 1.35f);
    return line + pad.vertical();
}

float TextEditor::caret_width() const {
    // Precedence mirrors the insets: an explicit per-widget width wins, then an
    // installed metrics delegate, then the stock stroke.
    if (caret_width_ > 0.0f) return caret_width_;
    if (auto* m = effective_metrics(); m != nullptr) {
        float from_delegate = 0.0f;
        if (m->caret_width(from_delegate, const_cast<TextEditor&>(*this)) && from_delegate > 0.0f)
            return from_delegate;
    }
    return 1.5f;
}

bool TextEditor::should_paint_caret() const {
    if (!caret_visible_) return false;
    if (!has_focus()) return false;
    // A selection already marks the insertion point, and a caret strobing at one
    // end of it reads as noise.
    if (has_selection()) return true;
    return caret_blink_.visible();
}

void TextEditor::ensure_caret_blink_subscription() {
    if (caret_blink_sub_ >= 0) return;
    if (auto* fc = frame_clock()) {
        caret_blink_clock_ = fc;
        caret_blink_sub_ = fc->subscribe([this](float dt) {
            advance_caret_blink(dt);
            request_repaint();
            return true;
        });
    }
}

void TextEditor::on_frame_clock_changed() {
    // The tree's clock was attached, swapped, or CLEARED. This notification is the
    // cached clock's last safe moment: a host that owns its FrameClock clears the
    // tree's clock (root->set_frame_clock(nullptr)) from its destructor, while the
    // clock is still alive, and frees it immediately after. Leave the subscription
    // in place and `caret_blink_clock_` outlives the clock — then ~TextEditor
    // unsubscribes through a dangling pointer (use-after-free on editor close, in
    // BOTH plugin-view hosts: the view tree belongs to the Processor and outlives
    // the host). So drop it here, then re-subscribe on the new clock if we still
    // hold focus. Same contract Meter already follows.
    clear_caret_blink_subscription();
    if (has_focus()) ensure_caret_blink_subscription();
}

void TextEditor::clear_caret_blink_subscription() {
    if (caret_blink_sub_ >= 0 && caret_blink_clock_)
        caret_blink_clock_->unsubscribe(caret_blink_sub_);
    caret_blink_sub_ = -1;
    caret_blink_clock_ = nullptr;
}

void TextEditor::on_mouse_event(const MouseEvent& event) {
    if (event.isRelease() || event.is_cancelled) {
        cancel_drag_selection(event.pointer_id);
        return;
    }
    if (!enabled()) {
        cancel_drag_selection(event.pointer_id);
        return;
    }
    if (multi_line && event.is_wheel) {
        scroll_offset_ = std::max(0.0f, scroll_offset_ + event.scroll_delta_y);
        return;
    }
    if (event.button == MouseButton::right && event.isPress()) {
        suppress_next_legacy_mouse_down_ = true;
        if (on_context_menu) on_context_menu(event.position);
        else show_default_context_menu(event.position);
        return;
    }
    if (event.isDrag()) {
        suppress_next_legacy_mouse_down_ = false;
        on_mouse_drag(event.position);
        return;
    }
    if (!event.is_down && !event.isPress()) {
        return;
    }
    suppress_next_legacy_mouse_down_ = true;
    set_pointer_capture(event.pointer_id);
    drag_pointer_id_ = event.pointer_id;
    has_drag_pointer_capture_ = true;

    reset_preferred_horizontal();
    int pos = char_index_at_point(event.position.x, event.position.y);

    if (event.click_count == 3) {
        if (multi_line) {
            auto [start, end] = line_range_at_position(pos);
            selection_start_ = start;
            selection_end_ = end;
            caret_position_ = end;
        } else {
            select_all();
        }
        drag_selecting_ = false;
        drag_selecting_words_ = false;
    } else if (event.click_count == 2) {
        auto [start, end] = word_range_at_position(pos);
        selection_start_ = start;
        selection_end_ = end;
        caret_position_ = end;
        drag_anchor_ = start;
        drag_word_start_ = start;
        drag_word_end_ = end;
        drag_selecting_ = start != end;
        drag_selecting_words_ = start != end;
    } else if (event.isShiftDown()) {
        selection_end_ = pos;
        caret_position_ = pos;
        drag_anchor_ = selection_start_;
        drag_selecting_ = true;
        drag_selecting_words_ = false;
    } else {
        caret_position_ = pos;
        selection_start_ = selection_end_ = pos;
        drag_anchor_ = pos;
        drag_selecting_ = true;
        drag_selecting_words_ = false;
    }
    break_undo_coalescing();
    keep_caret_solid();
}

void TextEditor::on_mouse_down(Point pos) {
    if (suppress_next_legacy_mouse_down_) {
        suppress_next_legacy_mouse_down_ = false;
        return;
    }

    MouseEvent event;
    event.position = pos;
    event.is_down = true;
    event.phase = MousePhase::press;
    on_mouse_event(event);
}

void TextEditor::on_mouse_drag(Point pos) {
    if (!enabled()) {
        cancel_drag_selection(0);
        return;
    }
    if (!drag_selecting_) return;
    auto b = local_bounds();
    bool scrolled = false;
    const float previous_scroll_offset = scroll_offset_;
    if (multi_line) {
        const float line_h = last_layout_.lines.empty()
            ? std::max(1.0f, font_size_ * 1.35f)
            : std::max(1.0f, last_layout_.lines.front().line_height);
        if (pos.y < b.y) {
            scroll_offset_ = std::max(0.0f, scroll_offset_ - line_h);
            scrolled = true;
        } else if (pos.y > b.y + b.height) {
            scroll_offset_ += line_h;
            scrolled = true;
        }
        if (!last_layout_.lines.empty()) {
            const float visible_h = std::max(line_h, b.height - 8.0f);
            const float total_h = std::max(
                visible_h, static_cast<float>(last_layout_.lines.size()) * line_h);
            scroll_offset_ = std::clamp(scroll_offset_, 0.0f,
                                        std::max(0.0f, total_h - visible_h));
        }
    } else {
        const float char_w = std::max(1.0f, font_size_ * 0.6f);
        if (pos.x < b.x) {
            scroll_offset_ = std::max(0.0f, scroll_offset_ - char_w);
            scrolled = true;
        } else if (pos.x > b.x + b.width) {
            scroll_offset_ += char_w;
            scrolled = true;
        }
    }
    if (scrolled) {
        const float delta = scroll_offset_ - previous_scroll_offset;
        if (delta != 0.0f && !last_layout_.lines.empty()) {
            if (multi_line) {
                for (auto& row : last_layout_.lines) {
                    row.top_y -= delta;
                    row.baseline_y -= delta;
                }
            } else if (!last_layout_.lines.empty()) {
                last_layout_.lines.front().inner_x -= delta;
            }
            last_layout_key_.scroll_offset = scroll_offset_;
        } else if (last_layout_.lines.empty()) {
            invalidate_layout_cache();
        }
    }
    caret_position_ = char_index_at_point(pos.x, pos.y);
    if (drag_selecting_words_) {
        auto [start, end] = word_range_at_position(caret_position_);
        if (caret_position_ < drag_word_start_) {
            selection_start_ = drag_word_end_;
            selection_end_ = start;
            caret_position_ = start;
        } else if (caret_position_ > drag_word_end_) {
            selection_start_ = drag_word_start_;
            selection_end_ = end;
            caret_position_ = end;
        } else {
            selection_start_ = drag_word_start_;
            selection_end_ = drag_word_end_;
            caret_position_ = drag_word_end_;
        }
        keep_caret_solid();
        return;
    }
    selection_start_ = drag_anchor_;
    selection_end_ = caret_position_;
    keep_caret_solid();
}

void TextEditor::on_mouse_up(Point) {
    cancel_drag_selection(0);
    break_undo_coalescing();
}

bool TextEditor::on_key_event(const KeyEvent& event) {
    if (!enabled()) return false;
    if (!event.is_down) return false;

    const bool shift = event.isShiftDown();
    const bool main_modifier = event.isMainModifier();  // Cmd on macOS, Ctrl on Win/Linux
    const bool alt = event.isAltDown();
#ifdef __APPLE__
    const bool ctrl = event.isCtrlDown();
    const bool command = event.isCmdDown() || (event.modifiers & kModMeta) != 0;
    const bool word_modifier = alt && !command;
    const bool line_arrow_modifier = command;
    const bool document_arrow_modifier = command;
    const bool paragraph_modifier = alt && !command;
    const bool document_home_end_modifier = command;
    const bool delete_word_modifier = alt && !command;
    const bool delete_line_start_backspace_modifier = command;
    const bool delete_line_start_modifier = ctrl && event.key == KeyCode::u;
    const bool delete_line_end_modifier = command;
#else
    const bool ctrl = event.isCtrlDown();
    const bool word_modifier = ctrl;
    const bool line_arrow_modifier = false;
    const bool document_arrow_modifier = false;
    const bool paragraph_modifier = ctrl;
    const bool document_home_end_modifier = ctrl;
    const bool delete_word_modifier = ctrl;
    const bool delete_line_start_backspace_modifier = false;
    const bool delete_line_start_modifier = ctrl && event.key == KeyCode::u;
    const bool delete_line_end_modifier = false;
#endif

    switch (event.key) {
        case KeyCode::left:
            if (line_arrow_modifier) move_to_line_start(shift);
            else if (word_modifier) move_word(-1, shift);
            else move_caret(-1, shift);
            return true;

        case KeyCode::right:
            if (line_arrow_modifier) move_to_line_end(shift);
            else if (word_modifier) move_word(1, shift);
            else move_caret(1, shift);
            return true;

        case KeyCode::up:
            if (document_arrow_modifier) { move_to_start(shift); return true; }
            if (paragraph_modifier) { move_paragraph(-1, shift); return true; }
            move_visual_line(-1, shift);
            return true;

        case KeyCode::down:
            if (document_arrow_modifier) { move_to_end(shift); return true; }
            if (paragraph_modifier) { move_paragraph(1, shift); return true; }
            move_visual_line(1, shift);
            return true;

        case KeyCode::page_up:
            move_page(-1, shift);
            return true;

        case KeyCode::page_down:
            move_page(1, shift);
            return true;

        case KeyCode::home:
            if (document_home_end_modifier) move_to_start(shift);
            else if (multi_line) move_to_line_start(shift);
            else move_to_start(shift);
            return true;

        case KeyCode::end_:
            if (document_home_end_modifier) move_to_end(shift);
            else if (multi_line) move_to_line_end(shift);
            else move_to_end(shift);
            return true;

        case KeyCode::backspace:
            if (read_only) return true;
            if (has_selection()) {
                const int start = std::min(selection_start_, selection_end_);
                const int end = std::max(selection_start_, selection_end_);
                delete_range(start, end);
            }
            else if (delete_line_start_backspace_modifier) delete_to_line_start();
            else if (delete_word_modifier) delete_word_before_caret();
            else delete_char_before_caret();
            return true;

        case KeyCode::delete_:
            if (read_only) return true;
            if (has_selection()) {
                const int start = std::min(selection_start_, selection_end_);
                const int end = std::max(selection_start_, selection_end_);
                delete_range(start, end);
            }
            else if (delete_line_end_modifier) delete_to_line_end();
            else if (delete_word_modifier) delete_word_after_caret();
            else delete_char_after_caret();
            return true;

        case KeyCode::tab:
            switch (tab_behavior) {
                case TabBehavior::insert_tab:
                    (void)replace_selection_or_insert("\t");
                    return true;
                case TabBehavior::commit:
                    break_undo_coalescing();
                    if (on_tab_commit) on_tab_commit(text_);
                    else if (on_return) on_return(text_);
                    return true;
                case TabBehavior::ignore:
                    break_undo_coalescing();
                    return true;
                case TabBehavior::move_focus:
                    break;
            }
            break_undo_coalescing();
            return false;

        case KeyCode::u:
            if (delete_line_start_modifier) { delete_to_line_start(); return true; }
            break;

        case KeyCode::k:
            if (ctrl) { delete_to_line_end(); return true; }
            break;

        case KeyCode::enter:
            if (multi_line) {
                if (main_modifier) {
                    if (on_return) on_return(text_);
                    return true;
                }
                const bool should_insert_newline =
                    multi_line_return_behavior == MultiLineReturnBehavior::insert_newline
                    || (multi_line_return_behavior == MultiLineReturnBehavior::shift_inserts_newline && shift);
                if (should_insert_newline)
                    return replace_selection_or_insert("\n");
            }
            if (on_return) on_return(text_);
            return true;

        case KeyCode::escape:
            if (on_escape) on_escape();
            return true;

        case KeyCode::a:
            if (main_modifier) { select_all(); return true; }
            break;
        case KeyCode::c:
            if (main_modifier) { copy_to_clipboard(); return true; }
            break;
        case KeyCode::v:
        {
#ifdef __APPLE__
            const bool paste_match_style = command && shift && alt;
#else
            const bool paste_match_style = main_modifier && shift;
#endif
            if (paste_match_style) { paste_from_clipboard(); return true; }
            if (main_modifier) { paste_from_clipboard(); return true; }
            break;
        }
        case KeyCode::x:
            if (main_modifier) { cut_to_clipboard(); return true; }
            break;
        case KeyCode::z:
            if (main_modifier && shift) { redo(); return true; }
            if (main_modifier) { undo(); return true; }
            break;

        default:
            break;
    }
    return false;
}

TextEditor::~TextEditor() {
    // Unsubscribe from the clock we actually subscribed to (cached), NOT
    // frame_clock() — by destruction the editor may already be detached from the
    // view tree, in which case frame_clock() walks a null parent_ and returns
    // nullptr, leaking the subscription with a dangling `this` (use-after-free on
    // the next tick).
    clear_caret_blink_subscription();
}

void TextEditor::on_focus_changed(bool gained) {
    View::on_focus_changed(gained);  // sets has_focus_ for border rendering
    if (gained) {
        if (select_on_focus) select_all();
        caret_blink_.reset();
        ensure_caret_blink_subscription();
    } else {
        clear_caret_blink_subscription();
        caret_blink_.reset();
        request_repaint();  // clear the caret now that focus is lost
        if (on_focus_lost) on_focus_lost(text_);
    }
}

void TextEditor::on_attached() {
    View::on_attached();
    if (has_focus())
        ensure_caret_blink_subscription();
}

void TextEditor::notify_change() {
    if (on_change) on_change(text_);
}

} // namespace pulp::view
