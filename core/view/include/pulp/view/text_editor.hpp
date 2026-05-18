#pragma once

/// @file text_editor.hpp
/// Full-featured text editor widget with selection, clipboard, undo/redo.
/// Inspired by Visage TextEditor patterns (see ~/Code/visage).

#include <pulp/view/view.hpp>
#include <pulp/view/input_events.hpp>
#include <pulp/platform/clipboard.hpp>
#include <functional>
#include <string>
#include <vector>
#include <utility>

namespace pulp::view {

/// Text editing widget with comprehensive keyboard/mouse interaction.
///
/// Features:
/// - Single-line and multi-line modes
/// - Text selection (click, shift-click, double-click word, triple-click line)
/// - Clipboard: Cmd+C copy, Cmd+V paste, Cmd+X cut, Cmd+A select all
/// - Undo/redo with history stack (Cmd+Z, Cmd+Shift+Z)
/// - Cursor movement (arrows, word jump with Option, line start/end with Cmd)
/// - Up=start, Down=end in single-line mode
/// - Numeric-only mode for parameter value entry
/// - Password mode (masked display)
/// - Return to confirm, Escape to cancel
/// - Select-on-focus option
///
/// @code
/// auto editor = std::make_unique<TextEditor>();
/// editor->set_text("Hello");
/// editor->on_return = [&](const std::string& text) { apply_value(text); };
/// editor->on_escape = [&] { revert(); };
/// @endcode
class TextEditor : public View {
public:
    TextEditor() { set_focusable(true); set_cursor(CursorStyle::text); }

    bool accepts_text_input() const override { return true; }

    static constexpr int kMaxUndoHistory = 1000;

    // ── Configuration ────────────────────────────────────────────────────

    /// Multi-line mode (default: single-line).
    bool multi_line = false;

    /// Numeric-only mode — only digits, decimal point, minus sign allowed.
    bool numeric_only = false;

    /// Password mode — display mask character instead of actual text.
    bool password_mode = false;
    char password_char = '*';

    /// Automatically select all text when this editor gains focus.
    bool select_on_focus = false;

    /// Placeholder text when empty.
    std::string placeholder;

    // ── Callbacks ─────────────────────────────────────────────────────────

    /// Called when Return/Enter is pressed (single-line) or Cmd+Return (multi-line).
    std::function<void(const std::string& text)> on_return;

    /// Called when Escape is pressed.
    std::function<void()> on_escape;

    /// Called whenever the text content changes.
    std::function<void(const std::string& text)> on_change;

    // ── Text access ──────────────────────────────────────────────────────

    const std::string& text() const { return text_; }
    void set_text(const std::string& t);

    /// Get the currently selected text (empty if no selection).
    std::string selected_text() const;

    bool has_selection() const { return selection_start_ != selection_end_; }
    bool is_empty() const { return text_.empty(); }

    // ── Selection ─────────────────────────────────────────────────────────

    void select_all();
    void clear_selection();

    // ── Clipboard ─────────────────────────────────────────────────────────

    bool copy_to_clipboard();
    bool cut_to_clipboard();
    bool paste_from_clipboard();

    // ── Undo/Redo ────────────────────────────────────────────────────────

    bool undo();
    bool redo();

    // ── Painting ──────────────────────────────────────────────────────────

    void paint(canvas::Canvas& canvas) override;

    // ── Event handling ────────────────────────────────────────────────────

    void on_mouse_event(const MouseEvent& event) override;
    bool on_key_event(const KeyEvent& event) override;
    void on_text_input(const TextInputEvent& event) override;
    void on_focus_changed(bool gained) override;

    // ── Style ─────────────────────────────────────────────────────────────

    void set_font_size(float size) { font_size_ = size; }
    float font_size() const { return font_size_; }

    // ── IME composition (marked text) ────────────────────────────────────

    /// Set composition text from input method. Replaces any existing marked text.
    void set_marked_text(const std::string& marked, int selected_pos, int selected_len);

    /// Commit composition (clear marked text, the final text was already inserted via on_text_input).
    void unmark_text();

    /// Whether there is active IME composition text.
    bool has_marked_text() const { return !marked_text_.empty(); }

    /// The marked text range relative to the full text (start, length).
    std::pair<int, int> marked_range() const { return {marked_start_, static_cast<int>(marked_text_.size())}; }

    /// Caret position in the text, for IME cursor rect queries.
    int caret_pos() const { return caret_position_; }

    /// Caret bounding rect in local view coordinates. Returns the position
    /// the caret occupies after the most recent paint — both single-line
    /// (uses measured text width and the editor's vertical centering) and
    /// multi-line (uses the cached wrapped-line layout so the rect rides
    /// the correct visual row even when wrap is engaged). When `paint()`
    /// has not yet been called the rect collapses to the inner padding
    /// origin so IME hosts still get a sane (if non-precise) anchor.
    Rect caret_rect() const;

private:
    std::string text_;
    int caret_position_ = 0;     ///< Cursor position (character index)
    int selection_start_ = 0;    ///< Selection anchor
    int selection_end_ = 0;      ///< Selection active end (= caret)
    float font_size_ = 13.0f;
    float scroll_offset_ = 0.0f; ///< Horizontal scroll for single-line
    float caret_blink_time_ = 0.0f; ///< Accumulated time for caret blinking

    // IME composition state
    std::string marked_text_;        ///< Active composition string
    int marked_start_ = 0;          ///< Position in text_ where marked text starts
    int marked_selected_pos_ = 0;   ///< Selected range within marked text
    int marked_selected_len_ = 0;

    // Undo history: (text, caret_position) pairs
    std::vector<std::pair<std::string, int>> undo_history_;
    std::vector<std::pair<std::string, int>> redo_history_;

    /// Snapshot of the most recent paint's layout, populated for both
    /// single-line and multi-line modes. The mouse handler and
    /// `caret_rect()` consult this so click-to-caret in line 2+ of a
    /// wrapped paragraph picks the right visual row and the IME caret
    /// rect reflects what the user actually sees.
    struct LayoutSnapshot {
        struct Line {
            int start = 0;
            int end = 0;
            float baseline_y = 0.f;
            float top_y = 0.f;
            float inner_x = 0.f;
            float line_height = 0.f;
            /// Cumulative x of each char start; size = (end-start)+1.
            /// Built once per cache-key change rather than per paint.
            std::vector<float> x_offsets;
        };
        std::vector<Line> lines;
        bool multi_line = false;
        float fallback_char_w = 0.f;
    };
    mutable LayoutSnapshot last_layout_;

    /// Cache key for `last_layout_`. The expensive `x_offsets` arrays
    /// only rebuild when one of these inputs changes (text edit, font
    /// change, viewport resize, mode flip, scroll), NOT on every paint
    /// — paint is a 60Hz hot path on the UI thread.
    struct LayoutCacheKey {
        std::size_t text_hash = 0;
        float font_size = 0.f;
        float bounds_width = 0.f;
        float bounds_height = 0.f;
        float scroll_offset = 0.f;
        bool multi_line = false;
        bool password_mode = false;
        bool placeholder_visible = false;
        bool operator==(const LayoutCacheKey& o) const noexcept {
            return text_hash == o.text_hash
                && font_size == o.font_size
                && bounds_width == o.bounds_width
                && bounds_height == o.bounds_height
                && scroll_offset == o.scroll_offset
                && multi_line == o.multi_line
                && password_mode == o.password_mode
                && placeholder_visible == o.placeholder_visible;
        }
    };
    mutable LayoutCacheKey last_layout_key_;

    void push_undo();
    void insert_text(const std::string& t);
    void delete_selection();
    void delete_char_before_caret();
    void delete_char_after_caret();

    void move_caret(int delta, bool extend_selection);
    void move_word(int direction, bool extend_selection);
    void move_to_line_start(bool extend_selection);
    void move_to_line_end(bool extend_selection);
    void move_to_start(bool extend_selection);
    void move_to_end(bool extend_selection);

    int char_index_at_x(float x) const;
    /// Multi-line aware hit-test. When `paint()` has populated a layout
    /// snapshot the y coordinate selects the visual row; the x coordinate
    /// then picks the nearest character within that row's measured glyph
    /// offsets. Falls back to `char_index_at_x` when no snapshot exists.
    int char_index_at_point(float x, float y) const;
    bool is_word_char(char c) const;

    void notify_change();
};

} // namespace pulp::view
