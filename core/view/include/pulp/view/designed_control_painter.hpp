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

    /// Where the POINTER starts and ends, as fractions of the shorter side.
    ///
    /// The ring was given a design-derived radius so it would stop crossing the
    /// body; the pointer was left running from the box CENTRE, so it kept doing
    /// exactly what the ring had stopped doing — a full-radius spoke over the
    /// design's own cap. Both ends are derived from real edges in
    /// apply_designed_body_skin: the body edge, and the ring's outer edge.
    ///
    /// Zero on either keeps the stock behaviour (centre, and the ring radius),
    /// so a caller with no box to measure is unaffected.
    float indicator_inner_scale = 0.0f;
    float indicator_outer_scale = 0.0f;

    /// Which rotary value layers the DESIGN authorized.
    ///
    /// The skin exists because the design owns the body; these say the design
    /// owns the value layer too. A design that drew no ring gets no ring —
    /// synthesizing one puts arcs in the render that the design's own capture
    /// does not show, in colours the author never placed there. `draw_ring` is
    /// set only when the design declared where a value ring belongs
    /// (design_ring_radius); `draw_indicator` when it authored a pointer
    /// (knob_ind_*) or declared that ring. Both clear: paint_rotary still
    /// returns true — suppressing the stock body is the skin's whole point —
    /// but contributes no pixels, so the authored body beneath is the entire
    /// appearance, exactly as captured.
    ///
    /// Defaults stay true so a standalone caller keeps the full arc gauge.
    /// Rotary-only: paint_linear ignores them.
    bool draw_ring = true;
    bool draw_indicator = true;
};

/// Install the value-only painter on `control`. After this the widget draws no
/// body pixels — the design's are the only ones.
void apply_designed_control_skin(View& control, const DesignedControlSkin& skin);

}  // namespace pulp::view
