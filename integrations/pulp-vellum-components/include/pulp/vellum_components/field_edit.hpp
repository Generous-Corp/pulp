#pragma once

/// @file field_edit.hpp
/// The numeric box-model fields the inspector can edit in place, and the text
/// buffer that edits one: the field list, reading and writing a field on a
/// view, and a caret-and-digits buffer with arrow nudging.
///
/// Everything here works on a view or a string and reads no overlay state, so
/// a panel can show the same value, format and nudge an overlay applies.

#include <pulp/vellum_components/aliases.hpp>

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

namespace pulp::vellum_components::overlay {

/// The editable field paths in tab order: opacity, then the width, height,
/// padding and margin layout inputs.
inline constexpr std::string_view kEditableFieldPaths[] = {
    "style.opacity", "layout.width", "layout.height", "layout.padding", "layout.margin"};

/// The value of `path` on `view`, or 0 for an unknown path.
float read_field_value(const View& view, std::string_view path);

/// Write `value` to `path` on `view`. A layout input also invalidates layout
/// up to the root so the next paint reflows; opacity clamps to [0, 1]. An
/// unknown path writes nothing.
void write_field_value(View& view, std::string_view path, float value);

/// `value` as the digits a field shows: a whole number without a fraction,
/// anything else with two decimals.
std::string format_field_value(float value);

/// The text being typed into one field, with a caret.
class FieldEditBuffer {
  public:
    /// Start editing `path` with `initial` as both the seed text and the value
    /// to restore on cancel. The caret goes to the end.
    void begin(std::string path, float initial);
    void clear();

    bool active() const {
        return !path_.empty();
    }
    const std::string& path() const {
        return path_;
    }
    const std::string& text() const {
        return text_;
    }
    std::size_t caret() const {
        return caret_;
    }
    float original() const {
        return original_;
    }

    /// The text as a number, or nullopt while it is empty or not numeric.
    std::optional<float> value() const;

    /// Insert `digit` ('0' to '9') at the caret. Anything else is refused.
    bool insert_digit(char digit);
    /// Remove the character before the caret.
    void backspace();
    void move_left();
    void move_right();
    void move_home();
    void move_end();

    /// Add `step` to the current value (the original while the text is not
    /// numeric), rewrite the text and put the caret at the end. A result that
    /// and the value it started from are both whole numbers shows no
    /// fraction. Returns the new value.
    float nudge(float step);

  private:
    std::string path_;
    std::string text_;
    std::size_t caret_ = 0;
    float original_ = 0.0f;
};

} // namespace pulp::vellum_components::overlay
