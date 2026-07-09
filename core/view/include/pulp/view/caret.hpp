#pragma once

/// @file caret.hpp
/// Text-caret appearance and blink policy, shared by every editable widget.
///
/// A caret has two independent concerns, and this header owns both:
///
///   1. **Shape** — `CaretStyle`. Three shapes, all anchored at the same
///      `x` (the boundary between the glyph before the caret and the glyph
///      after it). They differ only in what they cover:
///
///        ibeam      be│    a thin vertical rule spanning the glyph cell
///        underline  be_    a rule at the underline position, on the baseline
///        block      be█    the filled glyph cell, glyph drawn in the bg color
///
///      `ibeam` is the default. `underline` is the terminal convention's
///      name for this shape and is unrelated to text-decoration underline;
///      it occupies the cell of the glyph *at* the caret rather than the
///      text behind it, so it never displaces the text. `block` is the
///      terminal block cursor.
///
///   2. **Blink** — `CaretBlinkConfig` + `CaretBlink`. The caret holds solid
///      for `solid_hold_seconds` after any caret movement or edit, then
///      resumes blinking with `period_seconds` at `duty` on-fraction. That
///      hold is what makes an arrow-key sweep read as a continuous caret
///      instead of a strobe: every arrow, word jump, home/end, keystroke,
///      delete, and mouse drag calls `keep_solid()`, and blinking only
///      restarts once the caret has been still for the hold duration.
///
/// A blinking caret is animated content, so `CaretBlink` honors
/// `MotionPreferences`: under `MotionPolicy::Off` the caret is always
/// visible and never blinks. Setting `CaretBlinkConfig::enabled = false`
/// does the same thing unconditionally.
///
/// Widgets drive the blink from a `FrameClock` subscription taken while
/// focused, so the phase advances in real time and not once per painted
/// frame — a caret must blink at the same rate on a 60 Hz and a 120 Hz
/// display.
///
/// Defaults are process-wide and overridable per widget:
///
/// @code
///   // Everything in this app gets an underline caret that blinks slowly.
///   pulp::view::set_default_caret_style(pulp::view::CaretStyle::underline);
///   pulp::view::CaretBlinkConfig slow;
///   slow.period_seconds = 1.6f;
///   pulp::view::set_default_caret_blink(slow);
///
///   // ...but this one field keeps the stock I-beam.
///   field.set_caret_style(pulp::view::CaretStyle::ibeam);
///   field.set_caret_blink({});
/// @endcode

#include <pulp/canvas/canvas.hpp>
#include <pulp/view/geometry.hpp>

#include <string>
#include <string_view>

namespace pulp::view {

/// Caret shape. All three anchor at the same caret x.
enum class CaretStyle {
    ibeam,      ///< Vertical rule spanning the glyph cell. The default.
    underline,  ///< Rule at the underline position, one glyph cell wide.
    block,      ///< Filled glyph cell; the covered glyph is drawn in the bg color.
};

/// Stable lowercase names, for serialization and the JS bridge.
const char* caret_style_to_string(CaretStyle style) noexcept;

/// Parse a name produced by `caret_style_to_string`. Unknown names, including
/// the empty string, return `CaretStyle::ibeam` rather than failing — a caret
/// that renders is always better than one that does not.
CaretStyle caret_style_from_string(std::string_view name) noexcept;

/// Blink timing. The defaults are the values Pulp has always used.
struct CaretBlinkConfig {
    /// Full on+off cycle, in seconds. Clamped to >= 0.05 s when applied.
    float period_seconds = 1.06f;
    /// Fraction of the period the caret is visible. Clamped to [0, 1].
    float duty = 0.5f;
    /// How long the caret stays solid after a movement or an edit.
    float solid_hold_seconds = 0.35f;
    /// False → the caret never blinks and is always visible while focused.
    bool enabled = true;
};

/// Process-wide caret defaults. A widget that never calls its own setter
/// picks these up. Not synchronized; set them during app startup.
CaretStyle default_caret_style() noexcept;
void set_default_caret_style(CaretStyle style) noexcept;
const CaretBlinkConfig& default_caret_blink() noexcept;
void set_default_caret_blink(const CaretBlinkConfig& config) noexcept;

/// The "solid while moving, blinking while still" state machine.
///
/// Owns no clock. The widget advances it from a `FrameClock` subscription
/// held while focused, and calls `keep_solid()` from every movement and
/// edit path. `visible()` answers only the blink question — the caller is
/// still responsible for not painting a caret on an unfocused widget.
class CaretBlink {
public:
    /// Adopts `default_caret_blink()` as it stands at construction, so an app
    /// that sets a process-wide default before building its view tree gets it.
    CaretBlink() noexcept;

    void set_config(const CaretBlinkConfig& config) noexcept;
    const CaretBlinkConfig& config() const noexcept { return config_; }

    /// The caret moved or the text changed: show it, and restart the hold.
    void keep_solid() noexcept;

    /// Focus gained: solid, with the blink phase rewound.
    void reset() noexcept;

    /// Advance real time by `dt` seconds. Consumes the solid hold first, so
    /// a single large `dt` cannot skip over it.
    void advance(float dt) noexcept;

    /// True when the caret should be painted this frame. Always true while
    /// the solid hold is running, while `config().enabled` is false, and
    /// under `MotionPolicy::Off`.
    bool visible() const noexcept;

    /// True while the post-movement solid hold is still running. Exposed for
    /// tests and for widgets that suppress an unrelated animation while the
    /// user is actively navigating.
    bool holding_solid() const noexcept { return solid_remaining_ > 0.0f; }

private:
    CaretBlinkConfig config_{};
    float phase_ = 0.0f;            ///< Position within the blink period, seconds.
    float solid_remaining_ = 0.0f;  ///< Remaining post-movement solid hold, seconds.
};

/// Where a caret goes, in the coordinate space the widget paints text in.
///
/// `advance` is the width of the glyph the caret sits on, which sizes the
/// `underline` and `block` shapes. At the end of the text there is no such
/// glyph, so `advance` is 0 and `nominal_advance` is used instead — measure
/// a representative glyph (a digit, for a numeric field) rather than leaving
/// it 0, or the caret collapses to nothing in those two styles.
struct CaretMetrics {
    float x = 0.0f;                ///< Left edge of the caret, for every style.
    float cell_top = 0.0f;         ///< Top of the glyph cell.
    float cell_height = 0.0f;      ///< Height of the glyph cell.
    float baseline = 0.0f;         ///< Text baseline, for the underline shape.
    float advance = 0.0f;          ///< Width of the glyph at the caret; 0 at end of text.
    float nominal_advance = 0.0f;  ///< Fallback width when `advance` is 0.
    float stroke = 1.5f;           ///< I-beam width and underline thickness.
};

/// The width the underline and block shapes cover: `advance` when there is a
/// glyph at the caret, else `nominal_advance`.
float caret_advance(const CaretMetrics& metrics) noexcept;

/// The rect a caret of `style` occupies. `ibeam` is `stroke` wide; `underline`
/// is `caret_advance()` wide and sits just below the baseline; `block` is the
/// whole glyph cell.
Rect caret_rect_for_style(CaretStyle style, const CaretMetrics& metrics) noexcept;

/// Fill the caret rect. For `block` this covers the glyph — use
/// `paint_caret_over_text` unless the caller redraws the glyph itself.
void paint_caret(canvas::Canvas& canvas, CaretStyle style,
                 const CaretMetrics& metrics, canvas::Color color);

/// Paint the caret, and for `block` redraw the covered glyph in `text_bg` so
/// it stays legible against the fill. `text` is the full string the widget
/// just painted, drawn from `text_x` at `metrics.baseline`; only the caret
/// cell is repainted, clipped to the block rect. Fonts and alignment must be
/// set on `canvas` exactly as they were for the original `fill_text`.
void paint_caret_over_text(canvas::Canvas& canvas, CaretStyle style,
                           const CaretMetrics& metrics, canvas::Color color,
                           canvas::Color text_bg, const std::string& text,
                           float text_x);

}  // namespace pulp::view
