#pragma once

/// @file designed_control_painter.hpp
/// The composition contract for a control that carries its own design.
///
/// A designed control node describes its own body — gradient cap, hairline
/// border, inset highlight, drop shadow — and the DesignIR box pipeline paints
/// exactly that. The widget must then contribute ONLY what the design cannot
/// express, because it depends on a value the design never saw: the value ring
/// and the pointer.
///
/// Installing this skin makes the widget's stock body rendering unreachable, so
/// the two painters can no longer fight over the same pixels. Interaction, hit
/// testing and accessibility stay with the widget; only the paint moves.

#include <pulp/canvas/canvas.hpp>
#include <pulp/view/widget_painter.hpp>

namespace pulp::view {

class View;

/// Colours and geometry for the value layer, taken from the design's own
/// tokens so the ring belongs to the palette that asked for it.
struct DesignedControlSkin {
    canvas::Color track{0.25f, 0.28f, 0.32f, 1.0f};     ///< the inactive arc / track
    canvas::Color accent{0.08f, 0.72f, 0.65f, 1.0f};    ///< the value arc / fill
    canvas::Color indicator{0.92f, 0.94f, 0.96f, 1.0f}; ///< the pointer / thumb
    float ring_width = 4.0f;
    float indicator_width = 2.5f;
    /// Ring radius as a fraction of the shorter side of the control box.
    ///
    /// The control box IS the design's body box, so a value at or below 0.5
    /// paints the ring ON the body — which is what "the design owns the body"
    /// forbids. Callers derive this from the body edge (see
    /// apply_designed_body_skin) so the ring rides just OUTSIDE the dial, in
    /// the gap a decorative bezel or tick ring already occupies.
    ///
    /// The 0.46 here is only the struct's own default, for a caller that has no
    /// box to measure. It is deliberately not the value the import lane uses.
    float ring_radius_scale = 0.46f;
};

/// Install the value-only painter on `control`. After this the widget draws no
/// body pixels — the design's are the only ones.
void apply_designed_control_skin(View& control, const DesignedControlSkin& skin);

}  // namespace pulp::view
