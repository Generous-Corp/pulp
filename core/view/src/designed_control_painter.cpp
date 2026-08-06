/// @file designed_control_painter.cpp
/// The composition contract for a control that carries its own design.
///
/// A designed control node arrives with a body already described — a gradient
/// cap, a hairline border, inset highlights, a drop shadow — and the DesignIR
/// box pipeline paints exactly that. The widget's job is then only the part the
/// design cannot express, because it depends on a value the design has never
/// seen: the value ring and the pointer.
///
/// Without this, both painters run. A model emitted the correct idiom for a
/// hero knob — a 182px ellipse, a brushed radial gradient, a 1px dark border,
/// an inset highlight, an inset shade and a drop shadow — and the stock
/// rendering drew its own arc, wedge and indicator on top, so the panel showed
/// a blue arc, a teal wedge and a large orange pie slice over a gradient that
/// only survived in patches. Two painters, same pixels, no contract.
///
/// The rule this file implements: **the design owns the body, the widget owns
/// only what tracks the value.** A skinned control therefore draws no body
/// pixels at all — returning true from paint_rotary suppresses the stock body —
/// and contributes only the ring and pointer, in colours taken from the
/// design's own tokens so they belong to the palette that asked for them.
///
/// Interaction and accessibility are untouched: the widget object still owns
/// the gesture, the hit rect and the accessible value. Only the paint moves.

#include <pulp/view/designed_control_painter.hpp>

#include <pulp/view/control_painters.hpp>
#include <pulp/view/view.hpp>
#include <pulp/view/widgets.hpp>

#include <algorithm>
#include <utility>

namespace pulp::view {

namespace {

/// Draws the value layer and nothing else.
class DesignedControlPainter final : public WidgetPainter {
public:
    explicit DesignedControlPainter(DesignedControlSkin skin) : skin_(std::move(skin)) {}

    bool paint_rotary(canvas::Canvas& canvas, const RotaryPaintState& state,
                      View& source) override {
        painters::KnobStyle style;
        // Resolved from the control itself, so a design whose palette was
        // projected into the theme colours its own ring rather than getting a
        // built-in accent that belongs to no one.
        style.track = source.resolve_color("control.track", skin_.track);
        style.ring = source.resolve_color("control.fill", skin_.accent);
        style.indicator = source.resolve_color("control.thumb", skin_.indicator);
        style.ring_width = skin_.ring_width;
        style.indicator_width = skin_.indicator_width;
        // The design's body fills the node box, so the ring rides just inside
        // its edge rather than at the stock radius, which was tuned for a knob
        // that draws its own smaller body.
        style.radius_scale = skin_.ring_radius_scale;
        // And the pointer rides the same gap. Without this it starts at the
        // box centre, which on a designed knob is the middle of the artwork.
        style.indicator_inner_scale = skin_.indicator_inner_scale;
        style.indicator_outer_scale = skin_.indicator_outer_scale;
        painters::paint_mod_ring_knob(canvas, state.bounds, state.position, style);
        return true;
    }

    bool paint_linear(canvas::Canvas& canvas, const LinearPaintState& state,
                      View& source) override {
        painters::FaderStyle style;
        style.horizontal = state.horizontal;
        style.track = source.resolve_color("control.track", skin_.track);
        style.fill = source.resolve_color("control.fill", skin_.accent);
        style.thumb = source.resolve_color("control.thumb", skin_.indicator);
        const float travel = std::abs(state.track_max - state.track_min);
        const float position =
            travel > 0.0f ? std::clamp((state.thumb_pos - state.track_min) /
                                           (state.track_max - state.track_min),
                                       0.0f, 1.0f)
                          : 0.0f;
        painters::paint_level_fader(canvas, state.bounds, position, style);
        return true;
    }

private:
    DesignedControlSkin skin_;
};

}  // namespace

void apply_designed_control_skin(View& control, const DesignedControlSkin& skin) {
    control.set_painter(std::make_shared<DesignedControlPainter>(skin));
}

}  // namespace pulp::view
