#pragma once

/// @file text_edit.hpp
/// Inline text editing for the inspector's Text tool: which views carry
/// editable copy, and the UTF-8 buffer with a caret and a selection that
/// edits it.
///
/// Both work on a view or a string and read no overlay state, so a panel or a
/// test can use the same caret and word rules the overlay applies.

#include <pulp/vellum_components/aliases.hpp>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <utility>

namespace pulp::vellum_components::overlay {

/// True when `view` carries copy the Text tool can edit: a Label or a
/// TextEditor.
bool view_has_editable_text(const View* view);

/// The copy of an editable view, or empty for a null or non-text view.
std::string editable_text_of(const View* view);

/// Replace the copy of an editable view and mark layout dirty up to the root
/// so the new text reflows and repaints. A non-text view is left alone.
void set_editable_text_of(View* view, const std::string& text);

/// The system clipboard as the text edit sees it. The default talks to the
/// platform clipboard; a host or test supplies its own pair.
struct TextClipboard {
    std::function<bool(const std::string&)> set;
    std::function<std::optional<std::string>()> get;

    /// The platform clipboard.
    static TextClipboard system();
};

/// A UTF-8 string with a caret and a selection anchor. Offsets are byte
/// indices kept on code point boundaries, so an arrow key or a delete never
/// splits a multibyte character.
class TextEditBuffer {
  public:
    /// Start editing `text`, caret at the end, nothing selected.
    void begin(std::string text);
    void clear();

    const std::string& text() const {
        return text_;
    }
    std::size_t caret() const {
        return caret_;
    }
    bool has_selection() const {
        return caret_ != anchor_;
    }
    /// The selected byte range, low then high; empty at the caret.
    std::pair<std::size_t, std::size_t> selection() const;
    std::string selected_text() const;

    /// Move the caret one code point left (negative) or right. With
    /// `extend` the selection grows; otherwise a selection collapses to its
    /// near edge first and the caret does not also move.
    void move_caret(int delta, bool extend);
    /// Move to the previous (-1) or next (+1) word boundary.
    void move_word(int direction, bool extend);
    void move_home(bool extend);
    void move_end(bool extend);
    void select_all();

    /// The editing calls return true when the text changed, so a caller knows
    /// whether to push it to the view.
    ///
    /// Insert `utf8` at the caret, replacing any selection.
    bool insert(const std::string& utf8);
    /// Remove the selection, or the code point before (backward) or after
    /// the caret.
    bool delete_backward();
    bool delete_forward();
    /// Remove the selection. False when nothing was selected.
    bool erase_selection();

    /// Frames since the last edit or caret move, for blinking the caret: a
    /// new edit starts it solid.
    std::uint32_t blink_ticks() const {
        return blink_ticks_;
    }
    /// Advance the blink by one painted frame and say whether the caret is on.
    bool tick_caret_visible(std::uint32_t period = 30);

  private:
    std::string text_;
    std::size_t caret_ = 0;
    std::size_t anchor_ = 0;
    std::uint32_t blink_ticks_ = 0;
};

} // namespace pulp::vellum_components::overlay
