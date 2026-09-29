#pragma once

// Canvas2D drawing state as a CanvasWidget replay established it, plus the
// save/restore stack over it.
//
// Canvas2D's save()/restore() covers the whole drawing state: fill and stroke
// style, line width/cap/join/miter/dash, font and text alignment, global alpha
// and composite operation, shadows, filter, direction, image smoothing. The
// canvas backends do not agree on how much of that their own save()/restore()
// reverts. SkiaCanvas keeps paint state in members that survive restore() and
// reverts only the matrix and clip. CoreGraphicsCanvas reverts what lives in
// the CGContext gstate (line width, cap, join, ...) but keeps its fill and
// stroke colours in members. Left alone, the same recorded stream would draw
// differently per backend after a restore().
//
// The replay therefore tracks what it has set and, after each restore(),
// re-applies every slot whose value differs from the one it held at the
// matching save(). That gives Canvas2D semantics on every backend, and it is
// what lets the JS shim put back its record of what was sent on restore()
// instead of re-sending the whole drawing state before the next draw.
//
// A slot that had never been set when save() ran is left as the backend
// restores it. The shim's record of that slot is empty too, so the shim sends
// it before the next draw that depends on it. Fill and stroke paint servers
// (gradients and patterns) are the exception: the shim does not re-send a
// solid colour it believes is already active, so an unrecorded paint server is
// cleared rather than left in place.

#include <pulp/view/canvas_widget.hpp>

#include <array>
#include <cstddef>
#include <optional>
#include <vector>

namespace pulp::view::detail {

/// Apply one sticky-state setter command (see CanvasReplayState::slot_for) to
/// the canvas. Commands of any other type are ignored.
void apply_canvas_state_setter(canvas::Canvas& canvas, const CanvasDrawCmd& cmd);

class CanvasReplayState {
public:
    enum Slot : std::size_t {
        font,
        text_align,
        line_cap,
        line_join,
        miter_limit,
        image_smoothing,
        global_alpha,
        blend_mode,
        shadow_color,
        shadow_blur,
        shadow_offset_x,
        shadow_offset_y,
        direction,
        filter,
        line_dash,
        fill_paint,
        stroke_paint,
        slot_count
    };

    /// The slot a setter command owns, or nullopt for any other command.
    static std::optional<Slot> slot_for(CanvasDrawCmd::Type type);

    /// Record that `cmd` (which must outlive the replay) now owns `slot`.
    void note_setter(Slot slot, const CanvasDrawCmd& cmd) {
        current_.setters[slot] = &cmd;
    }
    /// A solid fill colour replaces any fill gradient or pattern on every
    /// backend, so it also resets the fill paint server.
    void note_fill_color(canvas::Color color) {
        current_.fill_color = color;
        current_.setters[fill_paint] = &clear_fill_paint();
    }
    void note_stroke_color(canvas::Color color) { current_.stroke_color = color; }
    void note_line_width(float width) { current_.line_width = width; }

    void save() { stack_.push_back(current_); }

    /// Call after canvas.restore(): re-applies every slot that differs from
    /// the matching save(). A restore() with no matching save() changes
    /// nothing, as Canvas2D specifies.
    void restore(canvas::Canvas& canvas);

    /// Re-establish fill colour, fill paint and font after code that drew
    /// with its own values inside a backend save()/restore() pair.
    void reassert_fill_and_font(canvas::Canvas& canvas) const;

    std::size_t depth() const noexcept { return stack_.size(); }

private:
    struct State {
        std::array<const CanvasDrawCmd*, slot_count> setters{};
        std::optional<canvas::Color> fill_color;
        std::optional<canvas::Color> stroke_color;
        std::optional<float> line_width;
    };

    static const CanvasDrawCmd& clear_fill_paint();
    static const CanvasDrawCmd& clear_stroke_paint();

    State current_;
    std::vector<State> stack_;
};

} // namespace pulp::view::detail
