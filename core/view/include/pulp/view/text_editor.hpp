#pragma once

/// @file text_editor.hpp
/// Full-featured text editor widget with selection, clipboard, undo/redo.
/// Inspired by Visage TextEditor patterns (see ~/Code/visage).

#include <pulp/view/view.hpp>
#include <pulp/view/accessibility.hpp>
#include <pulp/view/widget_metrics.hpp>
#include <pulp/view/widget_painter.hpp>
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
class TextEditor : public View, public AccessibilityTextInterface {
public:
    TextEditor() {
        set_access_role(AccessRole::text_field);
        set_focusable(true);
        set_cursor(CursorStyle::text);
        on_context_menu = [this](Point pos) { show_default_context_menu(pos); };
    }
    ~TextEditor() override;

    bool accepts_text_input() const override { return enabled() && !read_only; }

    // ── AccessibilityTextInterface ───────────────────────────────────────
    //
    // Without this, `AccessRole::text_field` is a lie by omission: the role
    // says "edit field", and then macOS -accessibilityValue returns nil and
    // the Windows IValueProvider::get_Value returns a NULL BSTR, so VoiceOver
    // and Narrator announce "text field" and read NOTHING. The bridges resolve
    // an element's value through accessibility_value_string() (accessibility.
    // hpp), which finds this interface by dynamic_cast; the Windows provider
    // also routes IValueProvider::SetValue back through set_text() so
    // IsReadOnly and "editing works" finally agree.
    std::string get_text() const override { return text_; }
    void set_text(std::string_view t) override { set_text(std::string(t)); }
    /// Disambiguator: `set_text("literal")` would otherwise be ambiguous
    /// between the std::string and std::string_view overloads (both are
    /// user-defined conversions from const char*).
    void set_text(const char* t) { set_text(std::string(t ? t : "")); }
    bool is_editable() const override { return enabled() && !read_only; }
    std::pair<int, int> get_selection() const override { return selection_range(); }
    void set_selection(int start, int end) override;
    int get_character_count() const override {
        return static_cast<int>(text_.size());
    }

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

    /// Called when the field loses keyboard focus. The common inline-rename
    /// idiom is `on_return` = commit, `on_escape` = revert, `on_focus_lost` =
    /// commit — clicking away from a rename field should keep the edit, and
    /// without this hook a widget cannot tell the difference between "the user
    /// left" and "the user is still typing".
    std::function<void(const std::string& text)> on_focus_lost;

    // ── Frame geometry ───────────────────────────────────────────────────

    /// Horizontal alignment of the text inside the field. Default `left`.
    /// A one-line field in a narrow inline-rename slot is very often centerd.
    void set_text_align(canvas::TextAlign a) {
        text_align_ = a;
        invalidate_layout_cache();
        request_repaint();
    }
    canvas::TextAlign text_align() const { return text_align_; }

    /// The gap between the field's frame and its text, per side. Overrides the
    /// stock padding on every side that is set. A metrics delegate
    /// (`WidgetMetrics::text_field_insets`) supplies this when one is installed;
    /// this setter is the direct, per-widget route.
    void set_insets(EdgeInsets insets) {
        insets_ = insets;
        has_own_insets_ = true;
        invalidate_layout_cache();
        request_repaint();
    }
    EdgeInsets insets() const { return insets_; }

    /// Caret stroke width in pixels. Sub-pixel values render as an antialiased
    /// hairline. 0 restores the default. A metrics delegate
    /// (`WidgetMetrics::caret_width`) supplies this when one is installed.
    void set_caret_width(float px) {
        caret_width_ = px;
        request_repaint();
    }
    float caret_width() const;

    /// Force the caret hidden even while focused. A field that shows a
    /// selection band often wants the caret suppressed for the duration; this
    /// is the switch. Independent of blink: an invisible caret does not blink.
    void set_caret_visible(bool v) {
        caret_visible_ = v;
        request_repaint();
    }
    bool caret_visible() const { return caret_visible_; }

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
    // set_selection(anchor, active) is declared above as the
    // AccessibilityTextInterface override — same contract, one definition.
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

    // ── Layout ────────────────────────────────────────────────────────────

    /// The field's natural height: one line of text plus the frame insets.
    ///
    /// This is the hook the layout engine measures leaves through, so it is also
    /// where an installed `WidgetMetrics` delegate reaches the layout pass — the
    /// delegate supplies the font and the insets, and the height follows from
    /// them. Without this a field placed in a flex container has no opinion about
    /// its own height and collapses to zero, which is why a skin that only
    /// restyled the field's PIXELS could never have made it the right size.
    ///
    /// Width is deliberately left with no opinion (0): a text field is nearly
    /// always stretched by its container, not sized by its content.
    float intrinsic_height() const override;

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
    /// Re-home the caret-blink subscription when the tree's FrameClock changes.
    /// A host that OWNS its clock clears the tree's clock from its destructor and
    /// frees the clock immediately after; this is the cached pointer's last safe
    /// moment. See the definition.
    void on_frame_clock_changed() override;
    void on_resized() override { invalidate_layout_cache(); }

    // ── Style ─────────────────────────────────────────────────────────────

    void set_font_size(float size) {
        if (font_size_ == size) return;
        font_size_ = size;
        invalidate_layout_cache();
    }
    float font_size() const { return font_size_; }

    /// Typeface family. Empty means the default. A field whose skin draws in one
    /// face while the field measures and shapes in another is a field whose caret
    /// and selection band land in the wrong place, so the family belongs to the
    /// widget, not just to the painter.
    void set_font_family(std::string family) {
        font_family_ = std::move(family);
        invalidate_layout_cache();
        request_repaint();
    }
    const std::string& font_family() const { return font_family_; }

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
    std::string font_family_;
    float content_inset_left_ = 0.0f; ///< Extra left inset to clear a leading icon
    canvas::TextAlign text_align_ = canvas::TextAlign::left;
    EdgeInsets insets_{};
    bool has_own_insets_ = false;
    float caret_width_ = 0.0f;     ///< 0 = use the delegate's, else the default
    bool caret_visible_ = true;
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
