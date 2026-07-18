#pragma once

#include <pulp/canvas/canvas.hpp>

// Knob indicator-pointer paint helpers, quarantined out of widgets.cpp.
//
// These are the importer-coupled "captured disc + pointer" and synthetic
// rotating-notch drawing routines shared by Knob::paint's several branches
// (the silver vector knob, the single-frame sprite-body knob, and the
// imported-sprite captured-indicator overlay). They are pure free functions
// over canvas primitives — no Knob member state — so they live apart from the
// large Knob::paint body. The sprite-strip frame-blit body itself stays inline
// in Knob::paint because it is entangled with Knob's sprite_strip_/sprite_core_
// members.

namespace pulp::view {

// Rotating indicator notch, shared by the silver vector knob and the
// single-frame sprite-body knob. Draws a short radial line at the value's
// angle (value 0..1 → [-135°, +135°], the analog-synth convention), centered
// at (cx, cy): a dark backing stroke for contrast plus the bright pointer on
// top. `notch_r` is the extent — the line runs from 35% to 95% of it; the two
// stroke widths scale from `width_ref`.
void draw_knob_indicator_notch(canvas::Canvas& canvas,
                               float cx, float cy,
                               float notch_r, float width_ref,
                               float value);

// Pointer reproduced from the design's OWN indicator node
// (set_captured_indicator). Same [-135°,+135°] value→angle arc as the
// synthetic notch, but the radii, width and color come from the imported art,
// and it pivots at the disc core center (cx, cy).
void draw_knob_captured_pointer(canvas::Canvas& canvas,
                                float cx, float cy,
                                float r_in, float r_out, float width,
                                canvas::Color color, float value);

}  // namespace pulp::view
