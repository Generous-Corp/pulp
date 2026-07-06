#pragma once

/// @file control_painters.hpp
/// Skinnable paint-space control painters.
///
/// A faithful port often needs to draw a control directly from a paint() in panel
/// coordinates — no View, no layout, no hit-testing — because the interaction is
/// owned elsewhere (a DesignFrameView overlay, a custom widget) and only the LOOK
/// is wanted, at an arbitrary rect, in an arbitrary skin. These are free functions
/// that take a style struct, a Canvas&, a rect, and a value, and emit the geometry.
/// They are the paint half of the JUCE LookAndFeel drawRotarySlider /
/// drawLinearSlider / drawToggleButton surface, decoupled from any widget class.
///
/// Every painter is pure w.r.t. the canvas (it only issues draw ops), so a
/// RecordingCanvas can assert the emitted geometry is value-dependent (the arc
/// sweep grows with the knob value, the fader thumb tracks the level, …) with no
/// GPU / Skia raster surface.

#include <cstddef>

#include <pulp/canvas/canvas.hpp>
#include <pulp/view/geometry.hpp>

namespace pulp::view::painters {

// ── Mod-ring knob ────────────────────────────────────────────────────────────

struct KnobStyle {
    canvas::Color track{0.25f, 0.28f, 0.32f, 1.0f};   ///< the full inactive arc
    canvas::Color ring{0.08f, 0.72f, 0.65f, 1.0f};    ///< the active value arc
    canvas::Color indicator{0.92f, 0.94f, 0.96f, 1.0f};///< the pointer line
    float start_angle_deg = 135.0f;   ///< angle of value 0 (screen coords, y-down, 0=+x, CW+)
    float sweep_deg = 270.0f;         ///< total travel from value 0 to value 1
    float ring_width = 4.0f;
    float indicator_width = 2.5f;
    float radius_scale = 0.5f;        ///< arc radius as a fraction of min(w,h)
};

/// Draw a rotary knob whose active ring sweeps from `start_angle_deg` by
/// `value * sweep_deg`, with a pointer line at the value angle. `value` is [0,1]
/// (clamped). Emits: the full track arc, the active value arc (LAST stroke_arc —
/// its sweep scales with `value`), and the indicator line.
void paint_mod_ring_knob(canvas::Canvas& canvas, const Rect& rect, float value,
                         const KnobStyle& style = {});

// ── Level fader ──────────────────────────────────────────────────────────────

struct FaderStyle {
    bool horizontal = false;          ///< default vertical (0=bottom, 1=top)
    canvas::Color track{0.20f, 0.22f, 0.26f, 1.0f};
    canvas::Color fill{0.08f, 0.72f, 0.65f, 1.0f};    ///< the filled portion up to value
    canvas::Color thumb{0.92f, 0.94f, 0.96f, 1.0f};
    float track_thickness = 6.0f;
    float thumb_length = 14.0f;        ///< thumb size along the travel axis
    float corner_radius = 3.0f;
};

/// Draw a linear fader with a filled level bar and a thumb at `value` [0,1].
/// Vertical: value 0 = bottom, 1 = top; horizontal: 0 = left, 1 = right. The thumb
/// rect (LAST fill_rounded_rect) tracks `value` along the travel axis.
void paint_level_fader(canvas::Canvas& canvas, const Rect& rect, float value,
                       const FaderStyle& style = {});

// ── Bipolar dual-thumb range slider ──────────────────────────────────────────

struct RangeSliderStyle {
    bool horizontal = true;
    canvas::Color track{0.20f, 0.22f, 0.26f, 1.0f};
    canvas::Color fill{0.08f, 0.72f, 0.65f, 1.0f};    ///< the span between the thumbs
    canvas::Color thumb{0.92f, 0.94f, 0.96f, 1.0f};
    float track_thickness = 6.0f;
    float thumb_radius = 7.0f;
};

/// Draw a two-thumb range slider spanning [lo, hi] (each [0,1], clamped, and
/// lo <= hi enforced). Emits the track, a fill between the two thumb positions,
/// and two thumb circles (first = lo, second = hi).
void paint_range_slider(canvas::Canvas& canvas, const Rect& rect,
                        float lo, float hi, const RangeSliderStyle& style = {});

// ── Toggle switch ─────────────────────────────────────────────────────────────

struct ToggleStyle {
    canvas::Color track_off{0.24f, 0.26f, 0.30f, 1.0f};
    canvas::Color track_on{0.08f, 0.72f, 0.65f, 1.0f};
    canvas::Color knob{0.96f, 0.97f, 0.98f, 1.0f};
    float knob_inset = 2.0f;
};

/// Draw a pill toggle. The knob (LAST fill_circle) sits at the left for `off`,
/// the right for `on`; the track uses the on/off color.
void paint_toggle(canvas::Canvas& canvas, const Rect& rect, bool on,
                  const ToggleStyle& style = {});

// ── Waveform ──────────────────────────────────────────────────────────────────

struct WaveformStyle {
    canvas::Color line{0.08f, 0.72f, 0.65f, 1.0f};
    canvas::Color baseline{0.30f, 0.33f, 0.38f, 1.0f};
    float line_width = 1.5f;
    bool draw_baseline = true;
};

/// Draw `count` samples (each ~[-1,1]) as a polyline filling `rect` horizontally,
/// centered vertically (sample +1 = top, -1 = bottom). Draws an optional center
/// baseline. No-op if `count < 2` or `samples` is null.
void paint_waveform(canvas::Canvas& canvas, const Rect& rect,
                    const float* samples, std::size_t count,
                    const WaveformStyle& style = {});

}  // namespace pulp::view::painters
