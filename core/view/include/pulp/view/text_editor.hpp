#pragma once

/// @file text_editor.hpp
/// Full-featured text editor widget with selection, clipboard, undo/redo.
/// Inspired by Visage TextEditor patterns (see ~/Code/visage).

#include <pulp/view/view.hpp>
#include <pulp/view/caret.hpp>
#include <pulp/view/input_events.hpp>
#include <pulp/platform/clipboard.hpp>
#include <cstddef>
#include <functional>
#include <string>
#include <string_view>
#include <vector>
#include <utility>

namespace pulp::view {

/// Text editing widget with comprehensive keyboard/mouse interaction.
///
/// Features:
/// - Single-line and multi-line modes
/// - Grapheme-safe caret movement/deletion for UTF-8 text
/// - Text selection (click, shift-click, double-click word, triple-click line)
/// - Double-click drag expands selection by whole words
/// - Clipboard: Cmd/Ctrl+C, V, X, A; password copy/cut disabled by default
/// - Undo/redo with text, caret, selection, and scroll restoration
/// - Native keyboard movement: arrows, word, line, document, page, and selection variants
/// - Delete variants: character, word, line-start, and line-end shortcuts
/// - Configurable Tab, multi-line Return, clipboard, and line-ending behavior
/// - Numeric-only, max-length, input-filter, and whole-buffer validation hooks
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
    TextEditor() {
        set_focusable(true);
        set_cursor(CursorStyle::text);
        on_context_menu = [this](Point pos) { show_default_context_menu(pos); };
    }
    ~TextEditor() override;

    bool accepts_text_input() const override { return enabled() && !read_only; }

    static constexpr int kMaxUndoHistory = 1000;

    enum class TabBehavior {
        move_focus,  ///< Return false from Tab so the host/focus system can advance focus.
        insert_tab,  ///< Insert a literal tab character into the editor.
        commit,      ///< Consume Tab and call on_tab_commit (or on_return when unset).
        ignore,      ///< Consume Tab without mutating text or moving focus.
    };

    enum class MultiLineReturnBehavior {
        insert_newline,        ///< Return inserts a newline; main modifier commits.
        commit,                ///< Return commits; no newline is inserted.
        shift_inserts_newline, ///< Return commits; Shift+Return inserts a newline.
    };

    enum class ClipboardPolicy {
        standard,                 ///< Native behavior; password contents are protected.
        disabled,                 ///< Disable copy, cut, and paste through this control.
        allow_password_contents,  ///< Allow selected password text to leave the control.
    };

    enum class LineEndingPolicy {
        normalize, ///< CRLF/CR/LF become '\n' in multi-line fields, spaces in single-line fields.
        strip,     ///< Remove CR/LF during insertion and paste.
        preserve,  ///< Keep inserted CR/LF bytes in multi-line fields; single-line fields still flatten.
    };

    // ── Configuration ────────────────────────────────────────────────────

    /// Multi-line mode (default: single-line).
    bool multi_line = false;

    /// Numeric-only mode — only digits, decimal point, minus sign allowed.
    bool numeric_only = false;

    /// Read-only mode — allow focus, caret navigation, selection, and copy,
    /// but reject typed input, paste, cut, delete, and IME mutation.
    bool read_only = false;

    /// Password mode — display mask character instead of actual text.
    bool password_mode = false;
    char password_char = '*';

    /// Automatically select all text when this editor gains focus.
    bool select_on_focus = false;

    /// Placeholder text when empty.
    std::string placeholder;

    /// Tab policy. Defaults to native text-field behavior: let focus move.
    TabBehavior tab_behavior = TabBehavior::move_focus;

    /// Multi-line Return policy. Defaults to native text-area behavior.
    MultiLineReturnBehavior multi_line_return_behavior = MultiLineReturnBehavior::insert_newline;

    /// Clipboard policy. Password contents are never copied/cut unless either
    /// this is `allow_password_contents` or the legacy `allow_password_clipboard`
    /// compatibility flag is true.
    ClipboardPolicy clipboard_policy = ClipboardPolicy::standard;

    /// Line-ending normalization for typed input, paste, and IME composition.
    LineEndingPolicy line_ending_policy = LineEndingPolicy::normalize;

    /// Maximum accepted length in grapheme clusters. 0 means unlimited.
    std::size_t max_length = 0;

    /// Optional input filter. Receives normalized UTF-8 insertion text and
    /// returns the text to insert. Return an empty string to reject it.
    std::function<std::string(std::string_view)> input_filter;

    /// Optional paste-specific sanitizer. Runs only for clipboard paste after
    /// line-ending normalization and before numeric/input filtering.
    std::function<std::string(std::string_view)> paste_sanitizer;

    /// Optional whole-buffer validator. Receives the candidate text after the
    /// insertion/deletion. Return false to reject the edit.
    std::function<bool(std::string_view)> validator;

    /// Password fields do not copy/cut their hidden contents by default.
    bool allow_password_clipboard = false;

    // ── Callbacks ─────────────────────────────────────────────────────────

    /// Called when Return/Enter is pressed (single-line) or Cmd+Return (multi-line).
    std::function<void(const std::string& text)> on_return;

    /// Called when TabBehavior::commit is selected. Falls back to on_return.
    std::function<void(const std::string& text)> on_tab_commit;

    /// Called when Escape is pressed.
    std::function<void()> on_escape;

    /// Called whenever the text content changes.
    std::function<void(const std::string& text)> on_change;

    // ── Text access ──────────────────────────────────────────────────────

    const std::string& text() const { return text_; }
    /// Programmatically replace the text. This is intentionally not undoable:
    /// host/state sync should not appear as a user edit in the undo stack.
    void set_text(const std::string& t);

    /// Get the currently selected text (empty if no selection).
    std::string selected_text() const;

    bool has_selection() const { return selection_start_ != selection_end_; }
    bool is_empty() const { return text_.empty(); }

    // ── Selection ─────────────────────────────────────────────────────────

    void select_all();
    void clear_selection();
    void set_caret_pos(int byte_offset);
    void set_selection(int anchor_byte_offset, int active_byte_offset);
    int selection_anchor() const { return selection_start_; }
    int selection_active() const { return selection_end_; }
    std::pair<int, int> selection_range() const;

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
    void on_mouse_down(Point pos) override;
    void on_mouse_drag(Point pos) override;
    void on_mouse_up(Point pos) override;
    bool on_key_event(const KeyEvent& event) override;
    void on_text_input(const TextInputEvent& event) override;
    void on_focus_changed(bool gained) override;
    bool wants_mouse_input() const override { return true; }
    void on_attached() override;
    void on_resized() override { invalidate_layout_cache(); }

    // ── Style ─────────────────────────────────────────────────────────────

    void set_font_size(float size) {
        if (font_size_ == size) return;
        font_size_ = size;
        invalidate_layout_cache();
    }
    float font_size() const { return font_size_; }

    /// Extra left inset (px) for the text / placeholder / caret — used to clear a
    /// leading icon (an imported search field's magnifier). 0 = default padding.
    void set_content_inset_left(float px) {
        if (content_inset_left_ == px) return;
        content_inset_left_ = px;
        invalidate_layout_cache();
    }
    float content_inset_left() const { return content_inset_left_; }

    // ── IME composition (marked text) ────────────────────────────────────

    /// Set composition text from an input method. Replaces any existing marked
    /// text. Selection offsets are UTF-8 byte offsets within `marked`, matching
    /// the rest of TextEditor's public caret/selection APIs.
    void set_marked_text(const std::string& marked,
                         int selected_byte_offset,
                         int selected_byte_length);

    /// Convenience for native IME APIs that report marked-text selection ranges
    /// as UTF-16 code-unit offsets, such as macOS NSTextInputClient.
    void set_marked_text_utf16(const std::string& marked,
                               int selected_utf16_offset,
                               int selected_utf16_length);

    /// Commit composition (clear marked text, the final text was already inserted via on_text_input).
    void unmark_text();

    /// Whether there is active IME composition text.
    bool has_marked_text() const { return !marked_text_.empty(); }

    /// The marked text range relative to the full text (start, length).
    std::pair<int, int> marked_range() const { return {marked_start_, static_cast<int>(marked_text_.size())}; }

    /// Caret position in the text, for IME cursor rect queries.
    int caret_pos() const { return caret_position_; }

    /// Caret shape. Defaults to the process-wide style (`CaretStyle::ibeam`).
    void set_caret_style(CaretStyle style) { caret_style_ = style; request_repaint(); }
    CaretStyle caret_style() const { return caret_style_; }

    /// Blink timing. Defaults to the process-wide config. The caret still holds
    /// solid across caret movement and edits regardless of the rate chosen.
    void set_caret_blink(const CaretBlinkConfig& config) {
        caret_blink_.set_config(config);
        request_repaint();
    }
    const CaretBlinkConfig& caret_blink() const { return caret_blink_.config(); }

    /// Caret bounding rect in local view coordinates. Returns the position
    /// the caret occupies after the most recent paint — both single-line
    /// (uses measured text width and the editor's vertical centering) and
    /// multi-line (uses the cached wrapped-line layout so the rect rides
    /// the correct visual row even when wrap is engaged). When `paint()`
    /// has not yet been called the rect collapses to the inner padding
    /// origin so IME hosts still get a sane (if non-precise) anchor.
    Rect caret_rect() const;

    /// Current horizontal scroll offset (single-line) or vertical
    /// scroll (multi-line). Exposed for IME hosts + tests that need to
    /// reason about visible-vs-logical coordinates.
    float scroll_offset() const { return scroll_offset_; }

private:
    std::string text_;
    int caret_position_ = 0;     ///< Cursor position as a UTF-8 byte offset at a grapheme boundary.
    int selection_start_ = 0;    ///< Selection anchor as a UTF-8 byte offset.
    int selection_end_ = 0;      ///< Selection active end (= caret) as a UTF-8 byte offset.
    float font_size_ = 13.0f;
    float content_inset_left_ = 0.0f; ///< Extra left inset to clear a leading icon
    float scroll_offset_ = 0.0f; ///< Horizontal scroll for single-line
    CaretBlink caret_blink_;     ///< Solid-while-moving, blinking-while-still state machine
    CaretStyle caret_style_ = default_caret_style();
    bool has_preferred_horizontal_ = false; ///< Active while walking vertical lines.
    float preferred_visual_x_ = 0.0f;       ///< Layout-space caret x for visual up/down.
    int preferred_text_column_ = 0;         ///< Fallback hard-line column before paint.
    int caret_blink_sub_ = -1;      ///< Frame-clock subscription that drives blink repaints while focused
    FrameClock* caret_blink_clock_ = nullptr;  ///< Clock the subscription lives on; cached so we can
                                               ///< always unsubscribe even after the editor is detached
                                               ///< from the view tree (frame_clock() walks parent_ and
                                               ///< would return null once detached → leaked sub + UAF).

    // IME composition state
    std::string marked_text_;        ///< Active composition string
    int marked_start_ = 0;          ///< Position in text_ where marked text starts
    int marked_selected_pos_ = 0;   ///< Selected range within marked text
    int marked_selected_len_ = 0;
    bool marked_undo_active_ = false;
    bool drag_selecting_ = false;
    bool drag_selecting_words_ = false;
    int drag_anchor_ = 0;
    int drag_word_start_ = 0;
    int drag_word_end_ = 0;
    int drag_pointer_id_ = 0;
    bool has_drag_pointer_capture_ = false;
    bool suppress_next_legacy_mouse_down_ = false;

    struct UndoSnapshot {
        std::string text;
        int caret_position = 0;
        int selection_start = 0;
        int selection_end = 0;
        float scroll_offset = 0.0f;
    };
    enum class UndoCoalesce {
        none,
        typing,
        backspace,
        delete_forward,
    };
    std::vector<UndoSnapshot> undo_history_;
    std::vector<UndoSnapshot> redo_history_;
    UndoCoalesce last_undo_coalesce_ = UndoCoalesce::none;

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
            /// UTF-8 byte offset in text_ for each x_offsets entry.
            std::vector<int> byte_offsets;
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

    void push_undo(UndoCoalesce coalesce = UndoCoalesce::none);
    void break_undo_coalescing();
    UndoSnapshot snapshot() const;
    void restore_snapshot(const UndoSnapshot& snapshot);
    void insert_text(const std::string& t);
    void delete_selection();
    void delete_char_before_caret();
    void delete_char_after_caret();
    void delete_word_before_caret();
    void delete_word_after_caret();
    void delete_to_line_start();
    void delete_to_line_end();
    bool replace_selection_or_insert(std::string text,
                                     UndoCoalesce coalesce = UndoCoalesce::none,
                                     bool from_paste = false);
    bool replace_marked_text(std::string text);
    bool delete_range(int start, int end, UndoCoalesce coalesce = UndoCoalesce::none);
    bool candidate_is_valid(int replace_start, int replace_end, std::string_view insertion) const;
    bool can_edit() const { return enabled() && !read_only; }
    bool clipboard_import_allowed() const;
    bool clipboard_export_allowed() const;
    bool password_contents_allowed() const;
    void invalidate_layout_cache() const;

    void move_caret(int delta, bool extend_selection);
    void move_word(int direction, bool extend_selection);
    void move_visual_line(int direction, bool extend_selection);
    void move_page(int direction, bool extend_selection);
    void move_paragraph(int direction, bool extend_selection);
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
    std::pair<int, int> word_range_at_position(int position) const;
    std::pair<int, int> line_range_at_position(int position) const;
    void show_default_context_menu(Point local_pos);
    void cancel_drag_selection(int pointer_id);
    void reset_preferred_horizontal();
    void keep_caret_solid();
    void advance_caret_blink(float dt);
    bool should_paint_caret() const;
    void ensure_caret_blink_subscription();
    void clear_caret_blink_subscription();

    void notify_change();
};

} // namespace pulp::view
