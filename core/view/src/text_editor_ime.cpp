#include <pulp/view/text_editor.hpp>
#include <pulp/canvas/text_utf8.hpp>
#include "text_edit_model.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <limits>

namespace pulp::view {

namespace {

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

std::size_t saturating_add(std::size_t a, std::size_t b) noexcept {
    const auto max = std::numeric_limits<std::size_t>::max();
    return b > max - a ? max : a + b;
}

int nonnegative_range_end(int start, int length) noexcept {
    start = std::max(0, start);
    length = std::max(0, length);
    const auto max = std::numeric_limits<int>::max();
    return length > max - start ? max : start + length;
}

} // namespace

void TextEditor::on_text_input(const TextInputEvent& event) {
    if (!can_edit()) return;
    if (event.text.empty()) return;

    if (numeric_only) {
        for (char c : event.text) {
            if (!std::isdigit(static_cast<unsigned char>(c)) && c != '.' && c != '-') return;
        }
    }

    if (!marked_text_.empty()) {
        replace_marked_text(event.text);
        return;
    }

    replace_selection_or_insert(event.text, UndoCoalesce::typing);
}

bool TextEditor::replace_marked_text(std::string text) {
    if (!can_edit() || marked_text_.empty()) return false;

    int replace_start = text_edit::clamp_boundary(text_, marked_start_);
    int replace_end = text_edit::clamp_boundary(
        text_, marked_start_ + static_cast<int>(marked_text_.size()));
    if (replace_end < replace_start) std::swap(replace_start, replace_end);

    text = text_edit::normalize_insert_text(
        std::move(text), multi_line, line_ending_mode_for(line_ending_policy));
    if (numeric_only) text = text_edit::filter_numeric(std::move(text));
    if (input_filter) text = input_filter(text);
    if (text.empty()) return false;

    std::string existing_after_replace = text_;
    existing_after_replace.erase(static_cast<std::size_t>(replace_start),
                                 static_cast<std::size_t>(replace_end - replace_start));
    text = text_edit::truncate_to_cluster_count(existing_after_replace, std::move(text), max_length);
    if (text.empty()) return false;
    if (!candidate_is_valid(replace_start, replace_end, text)) return false;

    if (!marked_undo_active_) {
        push_undo(UndoCoalesce::none);
        marked_undo_active_ = true;
    }

    const std::string before = text_;
    text_.replace(static_cast<std::size_t>(replace_start),
                  static_cast<std::size_t>(replace_end - replace_start),
                  text);
    caret_position_ = text_edit::clamp_boundary(text_, replace_start + static_cast<int>(text.size()));
    selection_start_ = selection_end_ = caret_position_;
    marked_text_.clear();
    marked_start_ = 0;
    marked_selected_pos_ = 0;
    marked_selected_len_ = 0;
    marked_undo_active_ = false;
    invalidate_layout_cache();
    reset_preferred_horizontal();
    break_undo_coalescing();
    keep_caret_solid();
    if (text_ != before) notify_change();
    return true;
}

void TextEditor::set_marked_text_utf16(const std::string& marked,
                                       int selected_utf16_offset,
                                       int selected_utf16_length) {
    const auto selected_start16 = static_cast<std::size_t>(std::max(0, selected_utf16_offset));
    const auto selected_end16 = saturating_add(
        selected_start16, static_cast<std::size_t>(std::max(0, selected_utf16_length)));
    const auto selected_start8 = pulp::canvas::utf8_offset_for_utf16_offset(marked, selected_start16);
    const auto selected_end8 = pulp::canvas::utf8_offset_for_utf16_offset(marked, selected_end16);
    set_marked_text(marked,
                    static_cast<int>(std::min(selected_start8, selected_end8)),
                    static_cast<int>(selected_end8 > selected_start8
                        ? selected_end8 - selected_start8
                        : selected_start8 - selected_end8));
}

void TextEditor::set_marked_text(const std::string& marked,
                                 int selected_byte_offset,
                                 int selected_byte_length) {
    if (!can_edit()) return;
    if (marked.empty() && marked_text_.empty()) return;

    int replace_start = text_edit::clamp_boundary(text_, caret_position_);
    int replace_end = replace_start;
    if (!marked_text_.empty()) {
        replace_start = text_edit::clamp_boundary(text_, marked_start_);
        replace_end = text_edit::clamp_boundary(
            text_, marked_start_ + static_cast<int>(marked_text_.size()));
    } else if (has_selection()) {
        replace_start = text_edit::clamp_boundary(text_, std::min(selection_start_, selection_end_));
        replace_end = text_edit::clamp_boundary(text_, std::max(selection_start_, selection_end_));
    }
    if (replace_end < replace_start) std::swap(replace_start, replace_end);

    if (marked.empty() && !marked_text_.empty()) {
        const std::string before = text_;
        if (marked_undo_active_ && !undo_history_.empty()) {
            UndoSnapshot original = undo_history_.back();
            undo_history_.pop_back();
            restore_snapshot(original);
        } else {
            text_.erase(static_cast<std::size_t>(replace_start),
                        static_cast<std::size_t>(replace_end - replace_start));
            caret_position_ = text_edit::clamp_boundary(text_, replace_start);
            selection_start_ = selection_end_ = caret_position_;
            marked_text_.clear();
            marked_start_ = 0;
            marked_selected_pos_ = 0;
            marked_selected_len_ = 0;
            marked_undo_active_ = false;
            invalidate_layout_cache();
            reset_preferred_horizontal();
        }
        redo_history_.clear();
        break_undo_coalescing();
        keep_caret_solid();
        if (text_ != before) notify_change();
        return;
    }

    std::string next_marked = text_edit::normalize_insert_text(
        marked, multi_line, line_ending_mode_for(line_ending_policy));
    if (numeric_only) next_marked = text_edit::filter_numeric(std::move(next_marked));
    if (input_filter) next_marked = input_filter(next_marked);

    std::string existing_after_replace = text_;
    existing_after_replace.erase(static_cast<std::size_t>(replace_start),
                                 static_cast<std::size_t>(replace_end - replace_start));
    next_marked = text_edit::truncate_to_cluster_count(
        existing_after_replace, std::move(next_marked), max_length);
    if (!marked.empty() && next_marked.empty()) return;
    if (!candidate_is_valid(replace_start, replace_end, next_marked)) return;

    if (!marked_undo_active_ && (replace_start != replace_end || !next_marked.empty())) {
        push_undo(UndoCoalesce::none);
        marked_undo_active_ = true;
    }

    text_.replace(static_cast<std::size_t>(replace_start),
                  static_cast<std::size_t>(replace_end - replace_start),
                  next_marked);
    marked_text_ = std::move(next_marked);
    marked_start_ = replace_start;
    const int marked_sel_start = text_edit::clamp_boundary(
        marked_text_, std::max(0, selected_byte_offset));
    const int marked_sel_end = text_edit::clamp_boundary(
        marked_text_, nonnegative_range_end(selected_byte_offset, selected_byte_length));
    marked_selected_pos_ = std::min(marked_sel_start, marked_sel_end);
    marked_selected_len_ = std::abs(marked_sel_end - marked_sel_start);
    const int selected_start = marked_start_ + marked_selected_pos_;
    const int selected_end = selected_start + marked_selected_len_;
    selection_start_ = text_edit::clamp_boundary(text_, selected_start);
    selection_end_ = text_edit::clamp_boundary(text_, selected_end);
    caret_position_ = selection_end_;
    invalidate_layout_cache();
    keep_caret_solid();
    notify_change();
}

void TextEditor::unmark_text() {
    marked_text_.clear();
    marked_start_ = 0;
    marked_selected_pos_ = 0;
    marked_selected_len_ = 0;
    marked_undo_active_ = false;
    break_undo_coalescing();
}

} // namespace pulp::view
