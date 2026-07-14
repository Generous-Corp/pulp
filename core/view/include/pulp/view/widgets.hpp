#pragma once

#include <algorithm>
#include <pulp/view/view.hpp>
#include <pulp/view/accessibility.hpp>
#include <pulp/view/caret.hpp>
#include <pulp/view/custom_shader_host.hpp>
#include <pulp/view/frame_clock.hpp>
#include <pulp/canvas/attributed_string.hpp>
#include <pulp/canvas/text_shaper.hpp>  // canvas::ShapedLayout for Label's shaped-layout cache
#include <pulp/view/audio_bridge.hpp>
#include <pulp/view/animation.hpp>
#include <pulp/view/slider_core.hpp>
#include <pulp/view/sprite_strip.hpp>
#include <pulp/view/widget_painter.hpp>
#include <pulp/view/value_source.hpp>
#include <pulp/signal/spectrogram.hpp>
#include <pulp/signal/multi_channel_meter.hpp>
#include <string>
#include <string_view>
#include <cstddef>
#include <cstdint>
#include <cmath>
#include <functional>
#include <array>
#include <memory>
#include <vector>
#include <optional>

namespace pulp::view {

class TextEditor;   // pulp/view/text_editor.hpp — the Label's inline edit field

// ── Label ────────────────────────────────────────────────────────────────────
// Static or dynamic text display

/// Text alignment for Label.
/// `auto_` resolves at paint time to left (LTR) or right (RTL);
/// pulp doesn't model RTL yet so `auto_` currently degrades to
/// `left`. `justify` wires through to the canvas `TextAlign::justify`
/// enum value; SkParagraph kJustify rendering lands in a follow-up —
/// existing canvas backends treat it as `left` until then. Both values
/// are claimed in the rn/css catalog (Figma exports + Tailwind classes
/// emit `auto` and `justify` routinely).
/// `match_parent` resolves at paint time by
/// walking the View parent chain (via `inheritable_text_align()`) and
/// adopting the first ancestor's resolved value. If no ancestor set
/// one, falls back to `left` (the CSS spec default for `text-align`).
/// Symmetric with `auto_` in that the resolution is paint-time, not
/// setter-time — matches CSS `match-parent` semantics which inherit
/// from the parent's *computed* (not specified) value.
enum class LabelAlign { left, center, right, auto_, justify, match_parent };

class Label : public View {
public:
    Label() { set_access_role(AccessRole::label); }
    explicit Label(std::string text) : text_(std::move(text)) {
        set_access_role(AccessRole::label);
        set_derived_access_label(text_);
    }

    void set_text(std::string text) {
        if (text == text_) return;
        text_ = std::move(text);
        // The text IS the accessible name for a label — the two-arg ctor set it
        // and set_text() did not, so every Label built by the JS bridge
        // (<span>, <p>, <label>) or by set_text() carried an empty name. It is
        // the DERIVED name: an author-set aria-label wins over content, and
        // react-reconciler rewrites textContent on every re-render.
        set_derived_access_label(text_);
        // A plain set_text() supersedes any per-range styled runs from a
        // prior set_attributed_string(): the old spans index into the OLD
        // string and are stale. Drop them so paint() takes the single-style
        // path for the new text.
        has_attributed_ = false;
        // The Yoga measure callback (yoga_measure ->
        // Label::intrinsic_width / measured_height) re-runs TextShaper::prepare
        // keyed on the CURRENT text_, so a text change re-shapes correctly —
        // but only if a re-layout is actually triggered. Mark this Label's
        // layout dirty here so the new copy re-measures + reflows; without it
        // the laid-out width stays stale at the old text's advance. The
        // PreText-style shaper cache is keyed by (text, family, size), so the
        // new text simply hits a different cache entry — no algorithm change,
        // just cache-correct re-measurement.
        invalidate_layout();
    }
    const std::string& text() const { return text_; }

    // Per-range styled text (design-import mixed text). When set with >=1 span,
    // a single-line Label paints each span with its own font/color via
    // paint_attributed_() instead of the single-style path. Empty / multi-line
    // falls back to the dominant single-style text().
    void set_attributed_string(canvas::AttributedString a) {
        attributed_runs_ = std::move(a);
        has_attributed_ = !attributed_runs_.spans().empty();
        invalidate_layout();
    }
    void clear_attributed_string() { has_attributed_ = false; }
    bool has_attributed_string() const { return has_attributed_; }
    std::size_t attributed_span_count() const { return attributed_runs_.spans().size(); }

    // Each setter marks the corresponding has_own_* flag so
    // paint() can distinguish "default value" from "explicitly set" and
    // fall through to inheritable_*() for unset properties.
    void set_font_size(float size) { font_size_ = size; has_own_font_size_ = true; }
    float font_size() const { return font_size_; }
    bool has_own_font_size() const { return has_own_font_size_; }

    /// CSS font-family string (e.g. "Inter", "JetBrains Mono"). Empty means
    /// the widget falls back to the default theme family ("Inter").
    void set_font_family(std::string family) { font_family_ = std::move(family); }
    const std::string& font_family() const { return font_family_; }

    void set_font_weight(int weight) { font_weight_ = weight; has_own_font_weight_ = true; }  // 100-900, 400=normal, 700=bold
    int font_weight() const { return font_weight_; }
    bool has_own_font_weight() const { return has_own_font_weight_; }

    void set_font_style(int style) { font_style_ = style; }  // 0=normal, 1=italic
    int font_style() const { return font_style_; }

    void set_letter_spacing(float sp) { letter_spacing_ = sp; has_own_letter_spacing_ = true; }
    float letter_spacing() const { return letter_spacing_; }
    bool has_own_letter_spacing() const { return has_own_letter_spacing_; }

    void set_line_height(float lh) { line_height_ = lh; }
    float line_height() const { return line_height_; }

    void set_text_align(LabelAlign align) { text_align_ = align; has_own_text_align_ = true; }
    LabelAlign text_align() const { return text_align_; }
    bool has_own_text_align() const { return has_own_text_align_; }

    // Explicit text color override on the Label itself. When
    // set, overrides any inherited typography color and the theme token.
    void set_text_color(canvas::Color c) { text_color_ = c; has_own_text_color_ = true; }
    bool has_own_text_color() const { return has_own_text_color_; }
    canvas::Color text_color() const { return text_color_; }

    void set_multi_line(bool ml) { multi_line_ = ml; }
    bool multi_line() const { return multi_line_; }

    /// CSS `line-clamp` / `-webkit-line-clamp`. Maximum number of visible
    /// text lines for a multi-line label; 0 disables clamping.
    /// When set on a `multi_line_` Label, paint() emits at most N lines
    /// (newline- or wrap-broken) and appends the U+2026 ellipsis to the
    /// trailing visible line if any source lines were dropped. Matches the
    /// CSS spec's "block-axis line clamp" behavior at the keyword level
    /// (no `none` token — set 0 to clear). Wiring is shared between
    /// `line-clamp` and `-webkit-line-clamp` in the JS shim and the
    /// @pulp/react prop-applier; both keys funnel through the same case.
    void set_line_clamp(int n) { line_clamp_ = (n < 0) ? 0 : n; }
    int line_clamp() const { return line_clamp_; }

    /// CSS text-transform: uppercase, lowercase, capitalize, none
    enum class TextTransform { none, uppercase, lowercase, capitalize };
    void set_text_transform(TextTransform t) { text_transform_ = t; }
    TextTransform text_transform() const { return text_transform_; }

    /// CSS text-decoration: none, underline, line-through, overline
    enum class TextDecoration { none, underline, line_through, overline };
    void set_text_decoration(TextDecoration d) { text_decoration_ = d; }
    TextDecoration text_decoration() const { return text_decoration_; }
    void set_text_decoration_color(canvas::Color c) { decoration_color_ = c; has_decoration_color_ = true; }
    canvas::Color text_decoration_color() const { return decoration_color_; }
    bool has_text_decoration_color() const { return has_decoration_color_; }

    /// CSS text-decoration-style: solid, double, dotted, dashed, wavy.
    /// Accepted via the JS CSS shim and the bridge so authors can express
    /// the per-style longhand. Today the paint path always renders as
    /// `solid`; the value is stored so future paint logic can honor it
    /// without an API break (matches the spec's optional fallback to solid
    /// for renderers that don't implement non-solid styles).
    enum class TextDecorationStyle { solid, double_, dotted, dashed, wavy };
    void set_text_decoration_style(TextDecorationStyle s) { text_decoration_style_ = s; }
    TextDecorationStyle text_decoration_style() const { return text_decoration_style_; }

    void paint(canvas::Canvas& canvas) override;

    /// Intrinsic width — measured text width.
    /// Uses TextShaper (HarfBuzz/SkParagraph) when available, otherwise
    /// falls back to the same character-width estimate the base Canvas
    /// uses. This lets Yoga reserve enough horizontal space for the full
    /// label content instead of clipping to a parent-inherited width.
    /// Applies the same text-transform as paint() so measurement matches
    /// what is actually drawn.
    float intrinsic_width() const override;

    /// Intrinsic height based on font size and line height.
    /// Walks the inheritance cascade so an unset font_size
    /// picks up an ancestor View's setInheritableFontSize value.
    /// Counts visible explicit `\n`-delimited lines for multi_line labels,
    /// ignoring a trailing newline and honoring `line_clamp_` when set.
    /// Soft-wrap (multi_line + bounded width) is handled by the width-aware
    /// `measured_height()` overload below.
    float intrinsic_height() const override;

    /// Width-aware height: shaper-true line count × line-height for a
    /// multi_line Label given the parent's available content width.
    /// Invoked by the Yoga measure callback when a multi-line Label is laid
    /// out in a bounded-width parent (CSS
    /// `flex: 1`, `width: 100%`, fixed-width section subtitles). Falls
    /// back to `intrinsic_height()` when `multi_line_` is false or
    /// `available_width <= 0`, so single-line labels and unbounded
    /// containers keep the legacy one-line metric.
    float measured_height(float available_width) const;

    /// Baseline offset from the top of the Label's measured box, used by Yoga's
    /// `YGNodeSetBaselineFunc` to honor `align-items: baseline` on
    /// flex containers. Returns
    /// `ascent + (leading / 2)` from `TextShaper::measure_metrics` so
    /// the value tracks the same per-(family, size) cache the painter
    /// uses; mixed-size text in a row therefore aligns on its true
    /// typographic baselines rather than top-of-box.
    float baseline_y() const;

    /// WYSIWYG text-edit metrics — the geometry an inspector edit overlay
    /// needs to draw a caret + selection band that EXACTLY matches the
    /// glyphs `paint()` renders. The label's painter resolves inherited
    /// size/weight/letter-spacing, family fallback, slant, text-transform,
    /// and an alignment-dependent draw origin; an overlay that re-measures
    /// with the Label's OWN fields drifts whenever any of those differ
    /// (e.g. a PARENT sets `letter-spacing`, or the label is center/right
    /// aligned). This factors the same style/origin resolution `paint()`
    /// uses so the two can never disagree.
    ///
    /// `caret_x_by_byte[i]` is the caret x (local, measured from
    /// `local_text_left`) for the byte boundary at index `i` over the
    /// supplied `edit_text` — the buffer the overlay is currently editing,
    /// NOT `text()` (which is mid-edit). It has `edit_text.size() + 1`
    /// entries (one per boundary, including end-of-text).
    struct TextEditMetrics {
        std::string display_text;
        float local_text_left = 0.0f;   ///< local x where text starts (handles center/right align)
        float local_band_y = 0.0f;      ///< local y of the caret/selection band (top of text)
        float band_height = 0.0f;
        std::vector<float> caret_x_by_byte;  ///< size == display_text.size()+1
    };
    TextEditMetrics text_edit_metrics(canvas::Canvas& canvas,
                                      std::string_view edit_text) const;

    // ── Click-to-edit ────────────────────────────────────────────────────
    //
    // A Label can turn itself into a text field in place. `set_editable`
    // arms the gesture; a matching click spawns a child TextEditor sized to
    // the Label's bounds, seeded with the Label's text, focused and
    // select-all'd. Return commits (the Label's text is replaced and
    // `on_text_change` fires), Escape reverts, and clicking away commits —
    // the three endings a rename field is expected to have.
    //
    // The editor is a real child View, so a painter or metrics delegate
    // installed on an ancestor styles it exactly like any other text field.

    enum class EditTrigger {
        none,          ///< not editable
        single_click,
        double_click,
    };

    void set_editable(EditTrigger trigger) {
        edit_trigger_ = trigger;
        set_cursor(trigger == EditTrigger::none ? CursorStyle::default_ : CursorStyle::text);
    }
    EditTrigger editable() const { return edit_trigger_; }

    /// Fires when an edit is COMMITTED with new text (not on every keystroke,
    /// and not at all when the edit is reverted).
    std::function<void(const std::string& text)> on_text_change;

    /// Fires when the inline editor opens / closes, for owners that need to
    /// suppress a hover highlight or a drag gesture for the duration.
    std::function<void()> on_editor_show;
    std::function<void()> on_editor_hide;

    /// Open the inline editor now, regardless of the click trigger. This is
    /// the programmatic route — a rename command in a menu, or a keyboard
    /// shortcut.
    void show_editor();
    /// Close the inline editor. `commit` false discards the edit.
    void hide_editor(bool commit);
    bool editor_active() const { return editor_ != nullptr; }
    /// The live inline editor, or null. For tests and for owners that want to
    /// configure it (an input filter, a max length) after it opens.
    TextEditor* editor() const { return editor_; }

    void on_mouse_down(Point pos) override;
    bool wants_mouse_input() const override { return edit_trigger_ != EditTrigger::none; }

private:
    EditTrigger edit_trigger_ = EditTrigger::none;
    TextEditor* editor_ = nullptr;      ///< non-owning; the child vector owns it
    int click_count_ = 0;
    double last_click_time_ = 0.0;

    /// Resolved typography + origin shared by paint() and
    /// text_edit_metrics(). Factoring this out is the WYSIWYG invariant:
    /// the caret overlay and the painter resolve the SAME inherited
    /// size/weight/letter-spacing, family fallback, slant, alignment, and
    /// vertical band so a letter-spaced or center/right-aligned label can't
    /// drift between the two. `apply_text_transform()` mirrors paint()'s
    /// transform so the measured run matches the rendered run.
    struct ResolvedTextStyle {
        std::string family;
        float font_size = 14.0f;
        int font_weight = 400;
        int font_slant = 0;
        float letter_spacing = 0.0f;
        LabelAlign text_align = LabelAlign::left;
        float baseline_y = 0.0f;   ///< first-line baseline in local space
    };
    ResolvedTextStyle resolve_text_style() const;
    std::string apply_text_transform(const std::string& in) const;
    /// Translate the CSS `font-variant` CSV (View::font_variant_) into SkShaper
    /// OpenType feature tags and apply them to `canvas` (empty CSV → clear).
    /// paint() AND text_edit_metrics() both call this so the caret/selection
    /// geometry shapes with the SAME features the painter renders — without it
    /// a tabular-nums / small-caps label's caret x drifts (pulp WYSIWYG sweep).
    void apply_font_features(canvas::Canvas& canvas) const;

    std::string text_;
    std::string font_family_;     ///< Empty == widget default ("Inter")
    float font_size_ = 14.0f;
    int font_weight_ = 400;       ///< 400=normal, 700=bold
    int font_style_ = 0;          ///< 0=normal, 1=italic
    float letter_spacing_ = 0;    ///< Extra spacing between characters (px)
    float line_height_ = 0;       ///< 0=auto (font_size * 1.4)
    LabelAlign text_align_ = LabelAlign::left;
    bool multi_line_ = false;
    int line_clamp_ = 0;          ///< 0=no clamp, >=1=max lines
    TextTransform text_transform_ = TextTransform::none;
    TextDecoration text_decoration_ = TextDecoration::none;
    canvas::Color decoration_color_{};
    bool has_decoration_color_ = false;
    TextDecorationStyle text_decoration_style_ = TextDecorationStyle::solid;
    canvas::TextDirection text_direction_ = canvas::TextDirection::left_to_right;
    canvas::TextVerticalAlign vertical_align_ = canvas::TextVerticalAlign::top;
    // Explicit-vs-inherited tracking. Fields keep their default
    // values until a setter is called; the has_own_* flag tells paint()
    // whether to honor the field or fall through to View::inheritable_*().
    canvas::Color text_color_{};
    bool has_own_text_color_ = false;
    canvas::AttributedString attributed_runs_;  ///< per-range styled text (mixed)
    bool has_attributed_ = false;
    void paint_attributed_(canvas::Canvas& canvas);  ///< single-line span draw
    LabelAlign resolve_effective_align_();            ///< text-align cascade (auto/match-parent resolved)
    bool has_own_font_size_ = false;
    bool has_own_font_weight_ = false;
    bool has_own_letter_spacing_ = false;
    bool has_own_text_align_ = false;

    // Cache of the soft-wrap shaped layout so paint() reuses it instead of
    // re-running the expensive TextShaper prepare()+layout each frame. Keyed on
    // every input the shaper reads; a key mismatch recomputes, so no stale hit
    // is possible. Weight/style/letter-spacing are intentionally absent — they
    // affect rasterization, not line breaking.
    struct ShapedLayoutKey {
        std::string display_text;  // text_ after text-transform
        std::string family;        // resolved family ("Inter" fallback)
        float font_size = 0.0f;
        float width = 0.0f;        // bounds().width — changes every resize
        float line_height = 0.0f;
        int break_mode = 0;        // canvas::BreakMode as int
        std::uint64_t font_gen = 0;  // font_registration_generation() snapshot
        bool operator==(const ShapedLayoutKey& o) const {
            return display_text == o.display_text && family == o.family &&
                   font_size == o.font_size && width == o.width &&
                   line_height == o.line_height && break_mode == o.break_mode &&
                   font_gen == o.font_gen;
        }
    };
    ShapedLayoutKey shaped_cache_key_;
    canvas::ShapedLayout shaped_cache_layout_;
    bool shaped_cache_valid_ = false;

public:
    /// Set text direction (LTR, RTL, vertical top-to-bottom, vertical bottom-to-top).
    void set_text_direction(canvas::TextDirection d) { text_direction_ = d; }
    canvas::TextDirection text_direction() const { return text_direction_; }

    /// Set vertical text alignment within the label's bounds.
    void set_vertical_align(canvas::TextVerticalAlign a) { vertical_align_ = a; }
    canvas::TextVerticalAlign vertical_align() const { return vertical_align_; }
};

// ── Knob ─────────────────────────────────────────────────────────────────────
// Rotary control for audio parameters (gain, frequency, etc.)

/// Rendering style for audio widgets.
/// `standard` = interactive widget (arcs, thumbs, fills).
/// `minimal` = design-preview mode (simple shapes matching design tools).
/// `silver` = skeuomorphic chrome body, radial-gradient highlight, indicator
///           notch. Vector all the way — works on CPU raster + GPU Graphite.
///           Used by the figma-import lane as a native-vector alternative to
///           sprite-strip PNGs (no PNG bleed, crisp at any scale).
enum class WidgetRenderStyle { standard, minimal, silver };

// AccessibilityValueInterface: `AccessRole::slider` advertises the UIA
// RangeValue/Value patterns and the AT-SPI Value interface. A slider with no
// value source announces its name and then reads nothing (macOS) or reports
// 0 in a degenerate 0..0 range (Windows). The role and the value ship together.
class Knob : public View, public CustomShaderHost, public AccessibilityValueInterface {
public:
    Knob() {
        set_access_role(AccessRole::slider);
        set_focusable(true);
        // A knob's pointer travel is shorter than a fader's: 150px crosses the
        // whole range, which is what a compact plugin panel expects.
        core_.set_drag_sensitivity(150);
    }

    // ── Value engine ─────────────────────────────────────────────────────
    //
    // The knob's REAL-WORLD value, range, interval, response curve and drag law
    // live in a SliderCore (slider_core.hpp). `value()` below stays the
    // normalized [0,1] position in that range — that is the space a parameter
    // store and the design/JS bridges speak — so a knob with the default 0..1
    // range behaves exactly as it always has, and `set_range()` is what turns it
    // into a real control (20..20000 Hz, -60..+12 dB, quantized to an interval).

    SliderCore& slider() { return core_; }
    const SliderCore& slider() const { return core_; }

    /// Real-world range and quantization interval. Re-derives the real value from
    /// the current normalized position; silent (a reconfiguration is not an edit).
    void set_range(double min_value, double max_value, double interval = 0.0) {
        core_.set_range(min_value, max_value, interval);
        core_.set_normalized(value_, Notify::none);
        value_ = static_cast<float>(core_.normalized());
        request_repaint();
    }
    double real_value() const { return core_.value(); }
    /// Write in real-world units. Snapped to the interval, clamped to the range.
    bool set_real_value(double v, Notify notify = Notify::sync) {
        core_.set_value(v, Notify::none);
        return store_normalized(static_cast<float>(core_.normalized()), notify);
    }

    // ── AccessibilityValueInterface (normalized 0..1, like value()) ──────
    double get_current_value() const override { return value_; }
    void set_current_value(double v) override {
        const float prev = value_;
        set_value(static_cast<float>(v));
        if (value_ != prev && on_change) on_change(value_);
    }
    double get_minimum_value() const override { return 0.0; }
    double get_maximum_value() const override { return 1.0; }
    /// Prefer the widget's own display formatter ("-6.0 dB") over the generic
    /// percentage — it is the string the sighted user reads.
    std::string get_value_string() const override {
        return format_ ? format_(value_)
                       : AccessibilityValueInterface::get_value_string();
    }

    void set_render_style(WidgetRenderStyle s) { render_style_ = s; }
    WidgetRenderStyle render_style() const { return render_style_; }

    // Programmatic set_value() must request_repaint() when the value actually
    // changes. The host's mouseDown handler invalidates after every input event,
    // so user drags repaint correctly via that side channel; preset application
    // and other JS-driven bridge mutations go through this code path and would
    // otherwise leave the painted state stale until the next user input.
    //
    // Gate the invalidation on a real change. WidgetBridge::sync_from_store()
    // and restore_values(...) call set_value()/set_on() in tight loops during
    // sync/reload; firing a host repaint per call when the value is unchanged
    // burns wall-clock on large widget trees with no visible benefit.
    void set_value(float v) { store_normalized(v, Notify::none); }

    /// Notifying overload. `Notify::none` matches the historical `set_value(v)`
    /// (repaint, no callback) — the right choice when the knob is being synced
    /// FROM the thing it drives. `Notify::sync` fires `on_change` before
    /// returning; `Notify::async` defers it to the next
    /// `pulp::view::flush_async_notifications()`.
    bool set_value(float v, Notify notify) { return store_normalized(v, notify); }

    float value() const { return value_; }

    void set_default_value(float v) {
        default_value_ = std::clamp(v, 0.0f, 1.0f);
        core_.set_default_value(core_.value_for_normalized(default_value_));
    }
    float default_value() const { return default_value_; }

    /// Skew / response curve (see RangeSlider::set_skew). 1 = linear (default);
    /// <1 gives finer control near the low end (the law a frequency/time knob
    /// wants). Value is normalized 0..1; set_skew_from_midpoint takes a
    /// normalized midpoint. Applies to the value arc, dot, and drag; modulation
    /// rings are unaffected.
    void set_skew(float s) { core_.set_skew(s); }
    float skew() const { return static_cast<float>(core_.skew()); }
    /// `mid_normalized` is in the knob's NORMALIZED space (see value()), so this
    /// call means the same thing with or without a real range configured.
    void set_skew_from_midpoint(float mid_normalized) {
        core_.set_skew_from_midpoint(
            core_.value_for_normalized(std::clamp(mid_normalized, 1e-4f, 1.0f - 1e-4f)));
    }
    float position_for_value() const {
        const float s = skew();
        return s == 1.0f ? value_ : std::pow(value_, s);
    }
    float value_for_position(float p) const {
        p = std::clamp(p, 0.0f, 1.0f);
        const float s = skew();
        return s == 1.0f ? p : std::pow(p, 1.0f / s);
    }

    void set_label(std::string text) {
        if (label_ == text) return;
        label_ = std::move(text);
        set_derived_access_label(label_);  // the visible label is the accessible name
        request_repaint();
    }
    const std::string& label() const { return label_; }

    void set_show_label(bool show) {
        if (show_label_ == show) return;
        show_label_ = show;
        request_repaint();
    }
    bool show_label() const { return show_label_; }

    // Display format: called with normalized value to produce display text.
    // No equality check (std::function doesn't compare) — formatters are
    // set at construction or theme-change time, not in tight sync loops.
    void set_format(std::function<std::string(float)> fmt) {
        format_ = std::move(fmt);
        request_repaint();
    }

    /// Called when the value changes (from user interaction or programmatic).
    std::function<void(float)> on_change;
    std::function<void()> on_gesture_begin;
    std::function<void()> on_gesture_end;

    void paint(canvas::Canvas& canvas) override;
    void on_mouse_event(const MouseEvent& event) override;
    void on_mouse_enter() override;
    void on_mouse_leave() override;
    void on_mouse_down(Point pos) override;
    void on_mouse_up(Point pos) override;
    void on_mouse_drag(Point pos) override;
    bool wants_mouse_input() const override { return true; }

    // Animation accessors for testing
    float hover_glow() const { return hover_glow_.value(); }
    void advance_animations(float dt);

    // Arc range in radians (default: 270-degree sweep)
    static constexpr float start_angle = 2.356f;  // 135 degrees (bottom-left)
    static constexpr float end_angle = 7.069f;    // 405 degrees (bottom-right via top)

    // Custom body shader comes from CustomShaderHost — it replaces the
    // body/track/fill arc drawing; labels, value text, and hover glow still
    // draw in C++.

    // Declarative widget schema: JSON defining appearance as data (Rive-inspired)
    void set_widget_schema(std::string json) { widget_schema_ = std::move(json); }
    const std::string& widget_schema() const { return widget_schema_; }

    // ── Modulation rings (Saturn) ───────────────────────────────────────
    // Thin concentric rings drawn OUTSIDE the value arc, one per modulation
    // source. Matches the Figma "Knob Modulation" set (LFO / ENV / VEL / MACRO).
    // Empty by default (a plain knob).
    struct ModulationRing {
        // Two INDEPENDENT signed offsets from the base value, one per arc end —
        // dragging one handle moves only that end (not a symmetric grow/shrink
        // off a single depth). Unipolar-positive: lo=0, hi>0. Unipolar-negative:
        // lo<0, hi=0. Bipolar: lo<0, hi>0. The range is [base+lo, base+hi]; the
        // endpoints sort themselves, so the two may cross without breaking paint.
        float lo = 0.0f;             ///< low-end offset from base (−1..1)
        float hi = 0.0f;             ///< high-end offset from base (−1..1)
        canvas::Color color;         ///< per-source colour
    };
    void set_modulation_rings(std::vector<ModulationRing> rings) {
        mod_rings_ = std::move(rings);
        request_repaint();
    }
    const std::vector<ModulationRing>& modulation_rings() const { return mod_rings_; }

    // Live modulation phase in [-1, 1] — the instantaneous output of the
    // assigned source(s). Drives the indicator dot that rides the modulation
    // arc, showing where the parameter actually IS right now. Set it from a
    // master/macro control or animate it from the host (e.g. an LFO).
    void set_modulation_phase(float p) {
        float c = std::clamp(p, -1.0f, 1.0f);
        if (c != mod_phase_) { mod_phase_ = c; request_repaint(); }
    }
    float modulation_phase() const { return mod_phase_; }

    // The modulation range is [value+lo, value+hi] clipped to [0,1], sorted so
    // the returned .first ≤ .second regardless of which end is which. Exposes the
    // clipped endpoints of ring i for tests / hosts. Returns {lo_v, hi_v}.
    std::pair<float, float> modulation_range(size_t ring) const;
    // The live modulated value of ring i: phase≥0 sweeps toward base+hi, phase<0
    // toward base+lo, clipped to [0,1].
    float modulated_value(size_t ring) const;

    // Fired when dragging a modulation-arc handle changes a ring's endpoints.
    // Reports BOTH offsets (the dragged one moved, the other is unchanged).
    std::function<void(int ring, float lo, float hi)> on_modulation_change;
    // True while a modulation-arc handle is being dragged (vs the value).
    bool dragging_modulation() const { return mod_drag_ring_ >= 0; }

    // Scroll-wheel adjusts the value (hover + wheel).
    bool wants_wheel_value() const override { return true; }
    void on_wheel(float delta_y) override {
        // A QUANTIZED control moves by exactly one interval per notch. The
        // continuous nudge below is smaller than most intervals, so snapping
        // would return the value to the step it started on and the wheel would
        // do nothing at all — a control with an interval had a dead wheel.
        if (core_.interval() > 0.0) {
            const double step = (delta_y < 0.0f ? 1.0 : -1.0) * core_.interval();
            set_real_value(core_.value() + step, Notify::sync);
            return;
        }
        store_normalized(value_ + (-delta_y) * 0.004f, Notify::sync);
    }

    // True while a value drag (or modulation-handle drag) is in progress so a
    // declarative param binding yields the value to the user mid-turn.
    bool is_gesture_active() const override { return gesture_active_ || mod_drag_ring_ >= 0; }

private:
    std::vector<ModulationRing> mod_rings_;
    float mod_phase_ = 0.0f;       ///< live source value in [-1,1] (indicator)
    int mod_drag_ring_ = -1;       ///< ring whose handle is being dragged (-1 none)
    bool mod_drag_is_high_ = true; ///< dragging the high (vs low) handle
    float mod_drag_last_angle_ = 0.0f;  ///< continuous (unwrapped) drag angle; the
                                        ///< bottom gap is a hard wall — see on_mouse_drag
    /// The knob's position in its range, normalized to [0,1]. Mirrors
    /// `core_.normalized()`; the two are kept in lockstep by store_normalized().
    float value_ = 0.0f;
    SliderCore core_;              ///< range, interval, curve, default, drag law
    float default_value_ = 0.5f;   ///< the reset target, in normalized units
    std::string label_;
    std::function<std::string(float)> format_;
    ValueAnimation hover_glow_{0.0f};
    float drag_start_y_ = 0;
    float drag_start_proportion_ = 0;   ///< travel proportion when the drag began
    float drag_last_y_ = 0;             ///< previous tick (velocity-mode integration)
    bool gesture_active_ = false;

    /// The single place `value_` and `core_` are written. Clamps to [0,1], snaps
    /// through the core's interval, repaints on a real change, and notifies per
    /// the policy. Returns true if the stored value moved.
    bool store_normalized(float n, Notify notify) {
        core_.set_normalized(std::clamp(n, 0.0f, 1.0f), Notify::none);
        const float snapped = static_cast<float>(core_.normalized());
        if (snapped == value_) return false;
        value_ = snapped;
        request_repaint();
        if (notify == Notify::sync) {
            if (on_change) on_change(value_);
        } else if (notify == Notify::async && on_change) {
            auto fn = on_change;
            const float v = value_;
            queue_async_notification([fn, v] { fn(v); });
        }
        return true;
    }
    bool show_label_ = true;
    std::string widget_schema_;   // JSON declarative schema
    std::string lottie_json_;     // Lottie animation JSON
    WidgetRenderStyle render_style_ = WidgetRenderStyle::standard;
    float lottie_time_ = 0;       // Current playback position (0-1)

public:
    void set_lottie_json(std::string json) { lottie_json_ = std::move(json); }
    const std::string& lottie_json() const { return lottie_json_; }
    void set_lottie_time(float t) { lottie_time_ = std::clamp(t, 0.0f, 1.0f); }
    float lottie_time() const { return lottie_time_; }

    /// Sprite strip: designer-created filmstrip for knob appearance.
    /// The current value selects which frame to display.
    void set_sprite_strip(std::shared_ptr<SpriteStrip> strip) { sprite_strip_ = std::move(strip); }
    const std::shared_ptr<SpriteStrip>& sprite_strip() const { return sprite_strip_; }

    /// Opaque-core rectangle of the sprite body frame, in the FRAME's own
    /// pixel space (x, y, w, h). When set on a single-frame strip, Knob::paint
    /// scales the frame so the core fills the layout box (the soft drop-shadow
    /// bleed extends beyond) and centers the core — then draws the native
    /// rotating indicator notch over the static disc, sized to that core.
    /// Mirrors the importer's compute_opaque_core / have_core sizing so an
    /// imported sprite knob turns at the right size. Zero w/h leaves it unset
    /// (the legacy natural/2× centering path).
    void set_sprite_core(float x, float y, float w, float h) {
        sprite_core_x_ = x; sprite_core_y_ = y;
        sprite_core_w_ = w; sprite_core_h_ = h;
    }
    bool has_sprite_core() const { return sprite_core_w_ > 0.0f && sprite_core_h_ > 0.0f; }

    /// Geometry of the design's OWN pointer (e.g. the Figma "Vector 7" hairline)
    /// captured by hoist_captured_art_knobs, expressed relative to the disc core
    /// so the renderer can reproduce the design's indicator instead of the
    /// generic synthetic notch. `r_in`/`r_out` are fractions of the core's half
    /// extent (the radius the line runs between); `width_px` is the logical
    /// stroke width; `color` is the line color. When set on a single-frame
    /// captured-art knob, Knob::paint draws THIS pointer — pivoted at the disc
    /// core center on the [-135°,+135°] arc — and skips the synthetic notch, so
    /// the moving line rides the disc's baked min/center/max reference ticks.
    void set_captured_indicator(float r_in, float r_out, float width_px,
                                canvas::Color color) {
        ind_r_in_ = r_in; ind_r_out_ = r_out;
        ind_width_ = width_px; ind_color_ = color;
        has_captured_indicator_ = true;
    }
    bool has_captured_indicator() const { return has_captured_indicator_; }
    float captured_indicator_r_in() const { return ind_r_in_; }
    float captured_indicator_r_out() const { return ind_r_out_; }

private:
    std::shared_ptr<SpriteStrip> sprite_strip_;
    float sprite_core_x_ = 0.0f;
    float sprite_core_y_ = 0.0f;
    float sprite_core_w_ = 0.0f;
    float sprite_core_h_ = 0.0f;
    bool has_captured_indicator_ = false;
    float ind_r_in_ = 0.0f;
    float ind_r_out_ = 0.0f;
    float ind_width_ = 0.0f;
    canvas::Color ind_color_ = canvas::Color::rgba(1.0f, 1.0f, 1.0f, 1.0f);
};

// ── Fader ────────────────────────────────────────────────────────────────────
// Linear slider for audio parameters

class Fader : public View, public CustomShaderHost, public AccessibilityValueInterface {
public:
    enum class Orientation { vertical, horizontal };
    enum class ThumbShape { circle, rectangle };

    Fader() { set_access_role(AccessRole::slider); set_focusable(true); }

    // ── AccessibilityValueInterface (normalized 0..1) ────────────────────
    double get_current_value() const override { return value_; }
    void set_current_value(double v) override {
        const float prev = value_;
        set_value(static_cast<float>(v));
        if (value_ != prev && on_change) on_change(value_);
    }
    double get_minimum_value() const override { return 0.0; }
    double get_maximum_value() const override { return 1.0; }

    void set_render_style(WidgetRenderStyle s) { render_style_ = s; }
    WidgetRenderStyle render_style() const { return render_style_; }

    // Same programmatic repaint contract as Knob::set_value(), including the
    // no-change guard for bridge sync/reload loops.
    void set_value(float v) {
        float clamped = std::clamp(v, 0.0f, 1.0f);
        if (clamped == value_) return;
        value_ = clamped;
        request_repaint();
    }
    float value() const { return value_; }

    // Scroll-wheel adjusts the value (hover + wheel).
    bool wants_wheel_value() const override { return true; }
    void on_wheel(float delta_y) override {
        float nv = std::clamp(value_ + (-delta_y) * 0.004f, 0.0f, 1.0f);
        if (nv != value_) { set_value(nv); if (on_change) on_change(value_); }
    }

    // True while the thumb is being dragged so a param binding yields mid-drag.
    bool is_gesture_active() const override { return dragging_; }

    void set_orientation(Orientation o) { orientation_ = o; }
    Orientation orientation() const { return orientation_; }

    void set_thumb_shape(ThumbShape shape) {
        if (thumb_shape_ == shape) return;
        thumb_shape_ = shape;
        request_repaint();
    }
    ThumbShape thumb_shape() const { return thumb_shape_; }

    void set_thumb_size(float width, float height) {
        thumb_width_ = std::max(0.0f, width);
        thumb_height_ = std::max(0.0f, height);
        request_repaint();
    }
    float thumb_width() const { return thumb_width_; }
    float thumb_height() const { return thumb_height_; }

    void set_thumb_corner_radius(float radius) {
        thumb_corner_radius_ = std::max(0.0f, radius);
        request_repaint();
    }
    float thumb_corner_radius() const { return thumb_corner_radius_; }

    void set_label(std::string text) {
        if (label_ == text) return;
        label_ = std::move(text);
        set_derived_access_label(label_);  // the visible label is the accessible name
        request_repaint();
    }
    const std::string& label() const { return label_; }

    /// Called when the value changes.
    std::function<void(float)> on_change;
    std::function<void()> on_gesture_begin;
    std::function<void()> on_gesture_end;

    void paint(canvas::Canvas& canvas) override;
    void on_mouse_enter() override;
    void on_mouse_leave() override;
    void on_mouse_down(Point pos) override;
    void on_mouse_up(Point pos) override;
    void on_mouse_event(const MouseEvent& event) override;
    void on_mouse_drag(Point pos) override;
    bool wants_mouse_input() const override { return true; }

    // Animation accessors for testing
    float hover_scale() const { return hover_thumb_scale_.value(); }
    void advance_animations(float dt);

    /// Skew / response curve (see RangeSlider::set_skew). 1 = linear (default);
    /// <1 gives finer control near the bottom of the fader. Value is normalized
    /// 0..1, so set_skew_from_midpoint takes a normalized midpoint.
    void set_skew(float s) { skew_ = std::max(0.0001f, s); }
    float skew() const { return skew_; }
    void set_skew_from_midpoint(float mid_normalized) {
        float p = std::clamp(mid_normalized, 1e-4f, 1.0f - 1e-4f);
        skew_ = std::max(0.0001f, std::log(0.5f) / std::log(p));
    }
    /// Track position [0,1] (skew-mapped) for the current value, and the
    /// inverse used while dragging.
    float position_for_value() const { return skew_ == 1.0f ? value_ : std::pow(value_, skew_); }
    float value_for_position(float p) const {
        p = std::clamp(p, 0.0f, 1.0f);
        return skew_ == 1.0f ? p : std::pow(p, 1.0f / skew_);
    }

    // Custom body shader comes from CustomShaderHost.
    void set_widget_schema(std::string json) { widget_schema_ = std::move(json); }
    const std::string& widget_schema() const { return widget_schema_; }
    void set_lottie_json(std::string json) { lottie_json_ = std::move(json); }
    const std::string& lottie_json() const { return lottie_json_; }
    void set_lottie_time(float t) { lottie_time_ = std::clamp(t, 0.0f, 1.0f); }
    float lottie_time() const { return lottie_time_; }

private:
    float value_ = 0.0f;
    float skew_ = 1.0f;   ///< 1 = linear; <1 = finer control at the low end
    Orientation orientation_ = Orientation::vertical;
    // Ink & Signal faders use a slab/handle thumb by default (matches the Figma
    // design language); callers can opt back to a circle per-widget.
    ThumbShape thumb_shape_ = ThumbShape::rectangle;
    float thumb_width_ = 0.0f;
    float thumb_height_ = 0.0f;
    float thumb_corner_radius_ = 0.0f;
    std::string label_;
    ValueAnimation hover_thumb_scale_{1.0f};
    bool dragging_ = false;
    std::string widget_schema_;
    std::string lottie_json_;
    float lottie_time_ = 0.0f;
    WidgetRenderStyle render_style_ = WidgetRenderStyle::standard;

public:
    /// Sprite strip: designer-created filmstrip for fader appearance.
    void set_sprite_strip(std::shared_ptr<SpriteStrip> strip) { sprite_strip_ = std::move(strip); }
    const std::shared_ptr<SpriteStrip>& sprite_strip() const { return sprite_strip_; }

    // ── Skin overrides ────────────────────────────────────────────────────
    // Per-widget appearance, generalising the knob sprite-strip path to the
    // fader: instead of baking the captured Figma art (which would freeze the
    // thumb at its captured value), the importer derives the track / fill /
    // thumb colours from the design and the widget redraws them procedurally
    // so the thumb still MOVES with set_value(). Unset → today's theme-token
    // behaviour (back-compat). Mirrors RangeSlider's accent override.
    void set_skin_track_color(canvas::Color c) { track_color_ = c; has_skin_track_ = true; request_repaint(); }
    void set_skin_fill_color(canvas::Color c)  { fill_color_  = c; has_skin_fill_  = true; request_repaint(); }
    void set_skin_thumb_color(canvas::Color c) { thumb_color_ = c; has_skin_thumb_ = true; request_repaint(); }
    void set_skin_thumb_border_color(canvas::Color c) { thumb_border_color_ = c; has_skin_thumb_border_ = true; request_repaint(); }
    // Outline of the empty track: the lighter edge the captured art draws
    // around the dark channel. When set, the skinned fader strokes the track
    // rect so it doesn't read as a flat dark slab.
    void set_skin_track_border_color(canvas::Color c) { track_border_color_ = c; has_skin_track_border_ = true; request_repaint(); }
    // Derived thin track width (logical px). When set, the skinned fader draws
    // its track / fill at exactly this width (centred) instead of a fraction of
    // the widget box, matching the captured art's narrow track.
    void set_skin_track_width(float w) {
        if (w > 0.0f) { skin_track_width_ = w; has_skin_track_width_ = true; request_repaint(); }
    }
    float skin_track_width() const { return skin_track_width_; }
    bool has_skin_track_width() const { return has_skin_track_width_; }
    void clear_skin() {
        has_skin_track_ = has_skin_fill_ = has_skin_thumb_ = has_skin_thumb_border_ = false;
        has_skin_track_border_ = false;
        has_skin_track_width_ = false;
        request_repaint();
    }
    bool has_skin() const {
        return has_skin_track_ || has_skin_fill_ || has_skin_thumb_ ||
               has_skin_thumb_border_ || has_skin_track_border_;
    }
    bool has_skin_track_color() const { return has_skin_track_; }
    bool has_skin_fill_color() const { return has_skin_fill_; }
    bool has_skin_thumb_color() const { return has_skin_thumb_; }
    bool has_skin_thumb_border_color() const { return has_skin_thumb_border_; }
    bool has_skin_track_border_color() const { return has_skin_track_border_; }
    canvas::Color skin_track_color() const { return track_color_; }
    canvas::Color skin_fill_color() const { return fill_color_; }
    canvas::Color skin_thumb_color() const { return thumb_color_; }
    canvas::Color skin_thumb_border_color() const { return thumb_border_color_; }
    canvas::Color skin_track_border_color() const { return track_border_color_; }

private:
    std::shared_ptr<SpriteStrip> sprite_strip_;
    canvas::Color track_color_{};
    canvas::Color fill_color_{};
    canvas::Color thumb_color_{};
    canvas::Color thumb_border_color_{};
    canvas::Color track_border_color_{};
    bool has_skin_track_ = false;
    bool has_skin_fill_ = false;
    bool has_skin_thumb_ = false;
    bool has_skin_thumb_border_ = false;
    bool has_skin_track_border_ = false;
    float skin_track_width_ = 0.0f;
    bool has_skin_track_width_ = false;
};

// ── RangeSlider ──────────────────────────────────────────────────────────────
// Generic min/max/step slider mapped to HTML <input type="range">.
//
// Distinct from Knob (rotary, normalised 0..1) and Fader (DSP linear with
// decorative track + large thumb). RangeSlider is the plain rectangular
// track + circular handle the web uses for volume / morph / scrubber UIs.
// Caller-supplied min/max/step are honoured natively — the bridge does
// not preprocess them. Quantisation happens inside the widget so JS-side
// callers see the same value the renderer paints.
//
class RangeSlider : public View, public AccessibilityValueInterface {
public:
    enum class Orientation { horizontal, vertical };

    RangeSlider() {
        set_access_role(AccessRole::slider);
        set_focusable(true);
    }

    // ── AccessibilityValueInterface (caller's min/max/step units) ────────
    double get_current_value() const override { return value_; }
    void set_current_value(double v) override {
        const float prev = value_;
        set_value(static_cast<float>(v));
        if (value_ != prev && on_change) on_change(value_);
    }
    double get_minimum_value() const override { return min_; }
    double get_maximum_value() const override { return max_; }
    double get_step_size() const override {
        return step_ > 0.0f
            ? step_
            : AccessibilityValueInterface::get_step_size();
    }

    /// Inclusive lower bound (default 0).
    void set_min(float v) { min_ = v; clamp_and_quantize_(); }
    float min_value() const { return min_; }

    /// Inclusive upper bound (default 1). If max < min, value falls
    /// back to min — matches HTMLInputElement behaviour for invalid ranges.
    void set_max(float v) { max_ = v; clamp_and_quantize_(); }
    float max_value() const { return max_; }

    /// Step size for quantisation. Zero or negative = no quantisation
    /// (any value in [min,max] is allowed). HTML default is 1, but the
    /// pulp default is 0 because most plugin UIs want continuous values
    /// and explicitly opt in via `step` when they want stepping.
    void set_step(float v) { step_ = v; clamp_and_quantize_(); }
    float step() const { return step_; }

    /// Set the current value. The value is clamped to [min,max] and
    /// quantised to the nearest step if step > 0.
    ///
    /// request_repaint() lets programmatic preset application reach the screen,
    /// not just the next user-input event. Gate on actual changes to avoid
    /// redundant invalidations during sync_from_store / restore_values loops.
    void set_value(float v) {
        float prev = value_;
        value_ = v;
        clamp_and_quantize_();
        if (value_ != prev) request_repaint();
    }
    float value() const { return value_; }

    // Scroll-wheel adjusts the value (hover + wheel), scaled to the range.
    bool wants_wheel_value() const override { return true; }
    void on_wheel(float delta_y) override {
        float prev = value_;
        set_value(value_ + (-delta_y) * 0.004f * (max_ - min_));
        if (value_ != prev && on_change) on_change(value_);
    }

    // True while the handle is being dragged so a param binding yields mid-drag.
    bool is_gesture_active() const override { return dragging_; }

    void set_orientation(Orientation o) { orientation_ = o; }
    Orientation orientation() const { return orientation_; }

    /// Override the accent color for the active fill and handle. Empty
    /// (the default) means the widget pulls `control.fill` / `control.thumb`
    /// from the active theme.
    void set_accent_color(canvas::Color c) {
        accent_color_ = c;
        has_accent_color_ = true;
    }
    void clear_accent_color() { has_accent_color_ = false; }
    bool has_accent_color() const { return has_accent_color_; }
    canvas::Color accent_color() const { return accent_color_; }

    /// Track thickness in pixels (default 4). Anything in 4–6 matches
    /// the visual weight of common HTML range styling.
    void set_track_thickness(float t) { track_thickness_ = std::max(1.0f, t); }
    float track_thickness() const { return track_thickness_; }

    /// Skew / response curve, shaping the mapping between track position and
    /// value. skew == 1 is linear (default). The drawn thumb position is
    /// pow(valueProportion, skew); dragging is the inverse, so a linear drag
    /// yields a non-linear value. skew < 1 spends more travel (finer control)
    /// near the minimum — the law a frequency or time control wants; skew > 1
    /// biases resolution toward the maximum.
    void set_skew(float s) { skew_ = std::max(0.0001f, s); }
    float skew() const { return skew_; }
    /// Choose skew so `mid_value` lands at the middle of the track (position
    /// 0.5). E.g. a 20 Hz–20 kHz slider with set_skew_from_midpoint(1000).
    void set_skew_from_midpoint(float mid_value);
    /// Track position [0,1] (skew-mapped) the thumb is drawn at for the value.
    float position_for_value() const { return value_to_position_(); }

    /// Fired when the value changes — from drag, click, or set_value(). The
    /// callback receives the post-quantisation value, exactly the same
    /// number value() will return.
    std::function<void(float)> on_change;

    /// Fired when an interactive drag begins / ends, so a binding can
    /// bracket the edit in a host automation gesture.
    std::function<void()> on_gesture_begin;
    std::function<void()> on_gesture_end;

    void paint(canvas::Canvas& canvas) override;
    void on_mouse_event(const MouseEvent& event) override;
    void on_mouse_drag(Point pos) override;
    void on_mouse_enter() override;
    void on_mouse_leave() override;
    bool wants_mouse_input() const override { return true; }
    void advance_animations(float dt) { hover_scale_.advance(dt); }
    float hover_scale() const { return hover_scale_.value(); }

private:
    ValueAnimation hover_scale_{1.0f};  ///< thumb grows on hover (matches Fader)
    /// Convert a position along the track (0=start, 1=end) to a value
    /// after applying clamp + step quantisation.
    float position_to_value_(float t) const;

    /// Clamp value_ to [min_,max_] and snap it to the nearest step, then
    /// fire on_change if the post-quantisation value actually changed.
    void clamp_and_quantize_();

    /// Common drag/click handler — `pos` is in local coordinates.
    void update_from_position_(Point pos);

    /// Track position [0,1] the thumb is drawn at for the current value,
    /// applying the skew curve.
    float value_to_position_() const;

    float min_ = 0.0f;
    float max_ = 1.0f;
    float step_ = 0.0f;
    float value_ = 0.0f;
    float skew_ = 1.0f;   ///< 1 = linear; <1 = finer control at the low end
    Orientation orientation_ = Orientation::horizontal;
    bool dragging_ = false;
    float track_thickness_ = 4.0f;
    canvas::Color accent_color_{};
    bool has_accent_color_ = false;
};

// ── Toggle ───────────────────────────────────────────────────────────────────
// Boolean on/off switch

class Toggle : public View, public CustomShaderHost {
public:
    Toggle() : thumb_position_(0.0f), hover_opacity_(0.0f) {
        set_access_role(AccessRole::toggle);
        // Seed the ARIA state; Toggle::set_on() keeps it in sync. A `toggle`
        // (ARIA switch) with no state announces nothing about on/off.
        set_access_checked("false");
        set_focusable(true);
    }

    /// Set the on/off state. By default the thumb animates to the new position;
    /// pass `animate = false` to snap immediately. Use the snapping form for the
    /// initial seed from stored/preset state: there is nothing to animate from,
    /// and a single headless paint (screenshot baseline) never advances the
    /// animation clock, so an animated seed would render stuck in the off
    /// position regardless of the logical state.
    void set_on(bool v, bool animate = true);
    bool is_on() const { return on_; }

    void set_label(std::string text) {
        label_ = std::move(text);
        set_derived_access_label(label_);  // the visible label is the accessible name
    }
    const std::string& label() const { return label_; }

    /// Called when the toggle state changes.
    std::function<void(bool)> on_toggle;

    void paint(canvas::Canvas& canvas) override;
    void on_mouse_down(Point pos) override;
    void on_mouse_enter() override;
    void on_mouse_leave() override;
    bool wants_mouse_input() const override { return true; }

    // Animation accessors for testing
    float thumb_position() const { return thumb_position_.value(); }
    float hover_opacity() const { return hover_opacity_.value(); }
    void advance_animations(float dt);

    // Custom body shader comes from CustomShaderHost.
    void set_widget_schema(std::string json) { widget_schema_ = std::move(json); }
    const std::string& widget_schema() const { return widget_schema_; }
    void set_lottie_json(std::string json) { lottie_json_ = std::move(json); }
    const std::string& lottie_json() const { return lottie_json_; }
    void set_lottie_time(float t) { lottie_time_ = std::clamp(t, 0.0f, 1.0f); }
    float lottie_time() const { return lottie_time_; }

private:
    bool on_ = false;
    std::string label_;
    ValueAnimation thumb_position_;
    ValueAnimation hover_opacity_;
    std::string widget_schema_;
    std::string lottie_json_;
    float lottie_time_ = 0.0f;
};

// ── Checkbox ────────────────────────────────────────────────────────────────
// Rounded-square checkbox with check glyph

class Checkbox : public View {
public:
    // `checkbox`, not `toggle`: ARIA/AT-SPI/UIA all distinguish a checkbox
    // from a switch. Toggle/ToggleButton keep `toggle` (ARIA `switch`).
    // access_checked is the state slot every bridge reads (NSAccessibility
    // accessibilityValue, UIA ToggleState, the AT-SPI CHECKED state): a
    // `checkbox` role with an unset state announces the control and then says
    // nothing about whether it is checked. Seeded here, kept in sync below.
    Checkbox() {
        set_access_role(AccessRole::checkbox);
        set_access_checked("false");
        set_focusable(true);
    }

    // Same programmatic repaint contract as Knob::set_value(), including the
    // no-change guard for bridge sync/reload loops.
    void set_checked(bool v) {
        if (checked_ == v) return;
        checked_ = v;
        set_access_checked(v ? "true" : "false");
        request_repaint();
    }
    bool is_checked() const { return checked_; }

    std::function<void(bool)> on_change;

    void paint(canvas::Canvas& canvas) override;
    void on_mouse_down(Point pos) override;
    bool wants_mouse_input() const override { return true; }

private:
    bool checked_ = false;
};

// ── ToggleButton ────────────────────────────────────────────────────────────
// Full-width rounded button that toggles on/off

class ToggleButton : public View {
public:
    ToggleButton() {
        set_access_role(AccessRole::toggle);
        set_access_checked("false");
        set_focusable(true);
    }

    // Same programmatic repaint contract as Knob::set_value(), including the
    // no-change guard for bridge sync/reload loops.
    void set_on(bool v) {
        if (on_ == v) return;
        on_ = v;
        set_access_checked(v ? "true" : "false");
        request_repaint();
    }
    bool is_on() const { return on_; }

    void set_label(std::string text) {
        if (label_ == text) return;
        label_ = std::move(text);
        set_derived_access_label(label_);  // the visible label is the accessible name
        request_repaint();
    }
    const std::string& label() const { return label_; }

    void set_on_background_color(canvas::Color color) { on_background_color_ = color; request_repaint(); }
    void set_off_background_color(canvas::Color color) { off_background_color_ = color; request_repaint(); }
    void set_on_text_color(canvas::Color color) { on_text_color_ = color; request_repaint(); }
    void set_off_text_color(canvas::Color color) { off_text_color_ = color; request_repaint(); }
    void set_on_border_color(canvas::Color color) { on_border_color_ = color; request_repaint(); }
    void set_off_border_color(canvas::Color color) { off_border_color_ = color; request_repaint(); }
    void set_corner_radius(float radius) { corner_radius_ = std::max(0.0f, radius); request_repaint(); }
    void set_font_size(float size) { font_size_ = std::max(1.0f, size); request_repaint(); }

    /// Radio-group id (0 = independent toggle, the default). Sibling
    /// ToggleButtons under the same parent that share a non-zero id are
    /// mutually exclusive: clicking one turns it on and the others off, and
    /// clicking the already-on one is a no-op (it stays selected). Grouping is
    /// by shared parent: the id is only compared against siblings, so the same
    /// id under a different parent forms an independent group.
    void set_radio_group(int id) { radio_group_ = id; }
    int radio_group() const { return radio_group_; }

    const std::optional<canvas::Color>& on_background_color_override() const { return on_background_color_; }
    const std::optional<canvas::Color>& off_background_color_override() const { return off_background_color_; }
    const std::optional<canvas::Color>& on_text_color_override() const { return on_text_color_; }
    const std::optional<canvas::Color>& off_text_color_override() const { return off_text_color_; }
    const std::optional<canvas::Color>& on_border_color_override() const { return on_border_color_; }
    const std::optional<canvas::Color>& off_border_color_override() const { return off_border_color_; }
    std::optional<float> corner_radius_override() const { return corner_radius_; }
    std::optional<float> font_size_override() const { return font_size_; }

    std::function<void(bool)> on_toggle;

    void paint(canvas::Canvas& canvas) override;
    void on_mouse_down(Point pos) override;
    bool wants_mouse_input() const override { return true; }

    float intrinsic_height() const override { return 36.0f; }

private:
    bool on_ = false;
    int radio_group_ = 0;   ///< 0 = independent; shared non-zero id = mutually exclusive
    std::string label_;
    std::optional<canvas::Color> on_background_color_;
    std::optional<canvas::Color> off_background_color_;
    std::optional<canvas::Color> on_text_color_;
    std::optional<canvas::Color> off_text_color_;
    std::optional<canvas::Color> on_border_color_;
    std::optional<canvas::Color> off_border_color_;
    std::optional<float> corner_radius_;
    std::optional<float> font_size_;
};

// ── Icon ────────────────────────────────────────────────────────────────────
// Simple vector icons drawn with canvas strokes and fills

class Icon : public View {
public:
    enum class Type { image_upload, send, search, close };

    Icon() {}
    explicit Icon(Type t) : type_(t) {}

    void set_type(Type t) { type_ = t; }
    Type type() const { return type_; }

    void paint(canvas::Canvas& canvas) override;

private:
    Type type_ = Type::image_upload;
};

// ── ImageView ───────────────────────────────────────────────────────────────
// Displays an image loaded from a file path

class ImageCache;  // fwd; include pulp/view/image_cache.hpp in consumers

class ImageView : public View {
public:
    ImageView() {}

    /// Legacy API: filesystem path. Internally routed to set_image_source
    /// as `file://<path>` so the cache can normalise keys.
    void set_image_path(const std::string& path) {
        set_image_source(path.empty() ? std::string{} : "file://" + path);
    }
    const std::string& image_path() const { return path_; }

    /// URI-keyed image source. Accepts file://, resource://, or
    /// memory://sha256=<hex>. When an ImageCache is
    /// attached via set_image_cache(), paint() consults it instead of
    /// re-reading the file; the cache owns decode + eviction.
    void set_image_source(const std::string& uri) {
        if (uri != path_) {
            path_ = uri;
            loaded_ = false;
            cached_data_.clear();
        }
    }
    const std::string& image_source() const { return path_; }

    /// Attach an ImageCache. Lifetime: cache must outlive the view.
    /// Multiple views can share a cache — eviction is global. Passing
    /// nullptr detaches the cache and returns the view to the
    /// decode-on-paint legacy path.
    void set_image_cache(ImageCache* cache) { cache_ = cache; }
    ImageCache* image_cache() const { return cache_; }

    /// Value-driven silhouette fill (design-import shape-fill, e.g. ELYSIUM's
    /// prism / cube / cylinder illustrations). When `value` is in [0,1], paint()
    /// overlays `fill_color` from the bottom up to `value` of the height, masked
    /// to this image's own alpha silhouette via the canvas url() mask — so the
    /// shape "fills" with color as the bound value rises. A negative value (the
    /// default) disables the overlay and the image renders plainly.
    void set_fill_value(float value) {
        fill_value_ = value;
        request_repaint();
    }
    float fill_value() const { return fill_value_; }
    void set_fill_color(canvas::Color color) {
        fill_color_ = color;
        request_repaint();
    }
    bool has_fill() const { return fill_value_ >= 0.0f; }

    /// Per-shape gradient fill (design-import shape-fill enhancement). When ≥2
    /// stops are set, the value-driven silhouette fill paints THIS gradient
    /// (stop[0] at the shape bottom → stop[last] at the top) revealed up to
    /// `fill_value`, instead of the flat `fill_color`. The importer samples
    /// each shape's OWN colors from its art and stamps them here so the fill
    /// reproduces the original look — only adjustable. Storing stops is inert
    /// until `fill_value >= 0`, so it never changes a plainly-rendered image
    /// (keeps the capability opt-in). Fewer than 2 stops clears the gradient
    /// and the flat-color path is used.
    void set_fill_gradient(std::vector<canvas::Color> stops) {
        fill_gradient_ = std::move(stops);
        request_repaint();
    }
    const std::vector<canvas::Color>& fill_gradient() const { return fill_gradient_; }
    bool has_fill_gradient() const { return fill_gradient_.size() >= 2; }

    void paint(canvas::Canvas& canvas) override;

private:
    /// Sample the fill gradient at t∈[0,1] (0 = bottom stop, 1 = top stop).
    canvas::Color fill_gradient_color_at(float t) const;

    std::string path_;
    bool loaded_ = false;
    std::vector<uint8_t> cached_data_;  // File bytes cached after first successful load
    ImageCache* cache_ = nullptr;       // optional; owned externally
    float fill_value_ = -1.0f;          // <0 disables the silhouette fill overlay
    canvas::Color fill_color_ = canvas::Color::rgba(0.49f, 0.42f, 1.0f, 0.55f);
    std::vector<canvas::Color> fill_gradient_;  // ≥2 stops ⇒ gradient fill
};

// ── Meter ────────────────────────────────────────────────────────────────────
// Audio level meter with peak hold

class Meter : public View, public AccessibilityValueInterface {
public:
    enum class Orientation { vertical, horizontal };

    Meter() { set_access_role(AccessRole::meter); }

    // ── AccessibilityValueInterface (read-only gauge, normalized 0..1) ───
    // `meter` maps to a UIA ProgressBar / AT-SPI LEVEL_BAR, both of which
    // advertise a range. Without a source they report 0 in a 0..0 range.
    double get_current_value() const override { return display_peak(); }
    void set_current_value(double) override {}  // a meter is not writable
    double get_minimum_value() const override { return 0.0; }
    double get_maximum_value() const override { return 1.0; }

    void set_render_style(WidgetRenderStyle s) { render_style_ = s; }
    WidgetRenderStyle render_style() const { return render_style_; }

    // Set levels directly (normalized 0-1)
    void set_level(float rms, float peak);

    // Update from MeterBallistics (call once per frame with dt)
    void update(float raw_peak, float raw_rms, float dt);

    void set_orientation(Orientation o) { orientation_ = o; }
    Orientation orientation() const { return orientation_; }

    // Ballistics accessors for testing
    float display_rms() const { return ballistics_.display_rms; }
    float display_peak() const { return ballistics_.display_peak; }
    float held_peak() const { return ballistics_.held_peak; }

    // ── Live host→view source ────────────────────────────────────────────────
    // Bind a lock-free MeterSource whose `channel` this meter reads once per
    // frame (through the ballistic `update()` path) while attached to a
    // FrameClock. The host publishes on the audio/host thread; the meter reads
    // paint-safe. A bound-and-attached meter is a FrameClock subscriber, so it
    // also keeps an editor's frames alive (see needs_continuous_frames) while a
    // source is present. Pass nullptr to unbind. Safe to call before or after
    // the view is hosted — the subscription attaches whenever a clock becomes
    // reachable. UI thread.
    //
    // Lifetime: the bound FrameClock must outlive the meter, OR the host must
    // clear it (`root->set_frame_clock(nullptr)`) / detach the meter before
    // destroying the clock — both drop the subscription first. Pulp's GPU hosts
    // clear the root clock during teardown, so this holds for them; a custom
    // host that owns a clock must observe the same order.
    void set_source(std::shared_ptr<MeterSource> source, int channel = 0);
    bool has_source() const { return static_cast<bool>(source_); }
    int source_channel() const { return source_channel_; }

    ~Meter() override { stop_source_subscription(); }

    void on_attached() override { try_source_subscription(); }
    void on_detached() override { stop_source_subscription(); }
    void on_frame_clock_changed() override { try_source_subscription(); }

    void paint(canvas::Canvas& canvas) override;

    // ── Skin overrides ────────────────────────────────────────────────────
    // Per-widget gradient + background, generalising the knob sprite-strip
    // path to the meter. The importer samples the captured Figma PNG's
    // gradient (green→orange→red) and hands the stops here; the meter redraws
    // that gradient procedurally, CLIPPED to the current level, so the fill
    // still animates with set_level()/update() instead of freezing the
    // captured image. Stops are ordered low→high (bottom→top for vertical).
    // Unset → today's theme-token threshold behaviour (back-compat).
    void set_skin_gradient(std::vector<canvas::Color> stops) {
        gradient_stops_ = std::move(stops);
        request_repaint();
    }
    void set_skin_background_color(canvas::Color c) {
        background_color_ = c;
        has_skin_background_ = true;
        request_repaint();
    }
    void clear_skin() {
        gradient_stops_.clear();
        has_skin_background_ = false;
        bar_fill_ratio_ = 1.0f;
        request_repaint();
    }
    // Fraction of the widget's cross-axis width occupied by the coloured bar
    // (0..1). The captured meter draws a narrow coloured fill recessed inside a
    // wider dark housing slot; this ratio reproduces that inset so the rendered
    // bar isn't edge-to-edge paint. Derived from the captured asset
    // (colored-bar width / housing width); defaults to 1.0 (full width) when the
    // importer didn't supply it.
    void set_bar_fill_ratio(float r) {
        bar_fill_ratio_ = std::clamp(r, 0.05f, 1.0f);
        request_repaint();
    }
    float bar_fill_ratio() const { return bar_fill_ratio_; }
    bool has_skin_gradient() const { return gradient_stops_.size() >= 2; }
    bool has_skin_background_color() const { return has_skin_background_; }
    const std::vector<canvas::Color>& skin_gradient() const { return gradient_stops_; }
    canvas::Color skin_background_color() const { return background_color_; }

    // Sample the skin gradient at normalized position t (0=low/bottom,
    // 1=high/top). Linear interpolation across the supplied stops.
    canvas::Color gradient_color_at(float t) const;

private:
    // (Re)subscribe to the reachable FrameClock if a source is bound and we
    // aren't already subscribed. Idempotent; a no-op when no clock is reachable
    // yet (a preview, or a tree built before hosting — the subscription attaches
    // later via on_attached / on_frame_clock_changed).
    void try_source_subscription();
    // Unsubscribe from the cached clock, if subscribed. Safe to call repeatedly.
    void stop_source_subscription();
    // FrameClock callback: pull the latest frame and drive the ballistic path.
    // Returns false (auto-unsubscribing) once the source is gone or the reachable
    // clock no longer matches the one we subscribed to — the robust teardown for
    // a detach that does not fire on_detached on every descendant.
    bool on_source_frame(float dt);

    Orientation orientation_ = Orientation::vertical;
    MeterBallistics ballistics_;
    float current_rms_ = 0;
    float current_peak_ = 0;
    WidgetRenderStyle render_style_ = WidgetRenderStyle::standard;
    std::vector<canvas::Color> gradient_stops_;
    canvas::Color background_color_{};
    bool has_skin_background_ = false;
    float bar_fill_ratio_ = 1.0f;

    std::shared_ptr<MeterSource> source_;
    int source_channel_ = 0;
    int source_sub_id_ = -1;               // FrameClock subscription id, -1 = none
    FrameClock* subscribed_clock_ = nullptr; // cached for unsubscribe + detach detection
};

// ── XYPad ────────────────────────────────────────────────────────────────────
// 2D parameter control surface (e.g., filter frequency × resonance)

class XYPad : public View {
public:
    XYPad() = default;

    void set_x(float v) { x_ = std::clamp(v, 0.0f, 1.0f); request_repaint(); }
    void set_y(float v) { y_ = std::clamp(v, 0.0f, 1.0f); request_repaint(); }
    float x_value() const { return x_; }
    float y_value() const { return y_; }

    void set_x_label(std::string l) { x_label_ = std::move(l); }
    void set_y_label(std::string l) { y_label_ = std::move(l); }

    std::function<void(float, float)> on_change;
    std::function<void()> on_gesture_begin;
    std::function<void()> on_gesture_end;

    void paint(canvas::Canvas& canvas) override;
    void on_mouse_down(Point pos) override;
    void on_mouse_drag(Point pos) override;
    void on_mouse_up(Point pos) override;
    bool wants_mouse_input() const override { return true; }

    // True while the puck is being dragged so a param binding yields mid-drag.
    bool is_gesture_active() const override { return dragging_; }

private:
    void update_from_pos(Point pos);
    float x_ = 0.5f, y_ = 0.5f;
    std::string x_label_, y_label_;
    bool dragging_ = false;
};

// ── WaveformView ─────────────────────────────────────────────────────────────
// Displays audio waveform data
//
// Two backing sources are supported:
//
//   set_data(...)       — raw normalised samples, redrawn directly. The
//                         existing behaviour, used by oscilloscope-style
//                         displays of live audio.
//   set_thumbnail(...)  — points the widget at a pre-decoded
//                         `pulp::audio::AudioThumbnail`. Paint reads peak
//                         pairs from the cached level instead of decoding
//                         per redraw, wiring AudioThumbnail into the existing
//                         WaveformView without inventing a new widget.
//
// This remains a display-only waveform widget. Sampler/editor behaviors
// such as trim handles, fade curves, loop regions, slice markers, playheads,
// hit testing, and snapping should live in dedicated editor/viewport modules
// that compose this renderer instead of expanding WaveformView itself.

}  // namespace pulp::view
namespace pulp::audio { class AudioThumbnail; }
namespace pulp::view {

class WaveformView : public View {
public:
    // Triggering mode for periodic signal display.
    // free_run:     no triggering — caller is responsible for continuity
    // rising_zero:  align to the first rising-edge zero crossing
    // falling_zero: align to the first falling-edge zero crossing
    enum class TriggerMode { free_run, rising_zero, falling_zero };
    enum class PreviewShape { none, saw, sine, square, triangle };

    WaveformView() = default;

    // Set waveform data (normalized -1 to 1). If a non-free_run trigger
    // mode is active, the buffer is rotated so the first matching
    // crossing becomes index 0, producing a stable display for periodic
    // signals. If no crossing is found, the buffer is stored as-is.
    //
    // Calling set_data clears any active thumbnail source.
    void set_data(const float* samples, size_t count);
    void set_data(std::vector<float> samples);

    // Wire the widget to read peak (min, max) pairs from a pre-decoded
    // `AudioThumbnail`. The shared_ptr overload is the
    // editor/cache-safe path: the widget retains the CPU thumbnail summary
    // while it may paint from it. Setting a thumbnail clears any raw sample
    // buffer.
    //
    // `channel == UINT32_MAX` folds all channels into one display row.
    void set_thumbnail(std::shared_ptr<const pulp::audio::AudioThumbnail> thumb,
                       uint32_t channel = static_cast<uint32_t>(-1));
    [[deprecated("set_thumbnail(const AudioThumbnail*) is borrowed; prefer shared_ptr overload or set_thumbnail_borrowed()")]]
    void set_thumbnail(const pulp::audio::AudioThumbnail* thumb,
                       uint32_t channel = static_cast<uint32_t>(-1));
    // Explicit borrowed escape hatch for stack-owned tests or owners that
    // manage lifetime externally. Prefer set_thumbnail(shared_ptr) for editor
    // code and cache-backed thumbnails.
    void set_thumbnail_borrowed(const pulp::audio::AudioThumbnail* thumb,
                                uint32_t channel = static_cast<uint32_t>(-1));
    void clear_thumbnail();

    bool has_thumbnail() const noexcept { return thumbnail_ != nullptr; }
    const pulp::audio::AudioThumbnail* thumbnail() const noexcept { return thumbnail_; }

    size_t sample_count() const { return samples_.size(); }

    // Static oscillator-preview lane for imported synth-style designs.
    // Live sample data and thumbnails still take precedence when present.
    void set_preview_shape(std::string_view shape);
    PreviewShape preview_shape() const { return preview_shape_; }

    void set_trigger_mode(TriggerMode mode) { trigger_mode_ = mode; }
    TriggerMode trigger_mode() const { return trigger_mode_; }

    // Locate the first crossing of `mode` in the buffer without mutating
    // it. Returns the crossing index, or 0 if none is found or
    // mode is free_run.
    static size_t find_trigger_index(const float* samples, size_t count,
                                     TriggerMode mode);

    void paint(canvas::Canvas& canvas) override;

private:
    void apply_trigger();

    std::vector<float> samples_;
    TriggerMode trigger_mode_ = TriggerMode::free_run;
    PreviewShape preview_shape_ = PreviewShape::none;

    std::shared_ptr<const pulp::audio::AudioThumbnail> thumbnail_owner_;
    const pulp::audio::AudioThumbnail* thumbnail_ = nullptr;
    std::vector<float> thumbnail_min_max_;
    std::vector<float> thumbnail_envelope_;
    uint32_t thumbnail_channel_ = static_cast<uint32_t>(-1);
};

// ── SpectrumView ─────────────────────────────────────────────────────────────
// Displays frequency spectrum (magnitude data from FFT)

class SpectrumView : public View {
public:
    enum class Style { bars, line, filled };

    SpectrumView() = default;

    // Set spectrum magnitudes (dB, typically -100 to 0)
    void set_spectrum(const float* magnitudes_db, size_t bin_count);
    void set_spectrum(std::vector<float> magnitudes_db);

    size_t bin_count() const { return bins_.size(); }

    void set_style(Style s) { style_ = s; }
    Style style() const { return style_; }

    // dB range for display
    void set_range(float min_db, float max_db) { min_db_ = min_db; max_db_ = max_db; }

    void paint(canvas::Canvas& canvas) override;

private:
    std::vector<float> bins_;
    Style style_ = Style::filled;
    float min_db_ = -80.0f;
    float max_db_ = 0.0f;
};

// ── Panel ────────────────────────────────────────────────────────────────────
// Styled container with background, border, and rounding. For visual chrome.

class Panel : public View {
public:
    // Seed the View-side radius slot so the default 8px rounding paints.
    Panel() { View::set_border_radius(8.0f); }

    void set_background_token(std::string token) { bg_token_ = std::move(token); }
    void set_border_token(std::string token) { border_token_ = std::move(token); }
    // Route through View::set_border_radius so the px slot the painter reads is
    // updated and the pct slot is cleared, matching documented semantics.
    void set_corner_radius(float r) { View::set_border_radius(r); }
    void set_border_width(float w) { border_width_ = w; }

    const std::string& background_token() const { return bg_token_; }
    const std::string& border_token() const { return border_token_; }
    float corner_radius() const { return View::corner_radius(); }
    float border_width() const { return border_width_; }

    void paint(canvas::Canvas& canvas) override;

private:
    std::string bg_token_ = "bg.surface";
    std::string border_token_ = "control.border";
    float border_width_ = 1.0f;
};

// ── GroupBox ───────────────────────────────────────────────────────────────
// A titled container: rounded frame, a title chip at the top-left, and an
// optional collapse chevron at the top-right. Collapsing hides the child
// content (the caller sizes a collapsed box to header height). Matches the
// Figma "Group Box" specimen (default / collapsible-expanded / collapsed /
// empty-title-only). Children are added via add_child() and positioned by the
// caller within the content area (origin = content_top()).
class GroupBox : public View {
public:
    static constexpr float header_height = 30.0f;

    void set_title(std::string t) { title_ = std::move(t); request_repaint(); }
    const std::string& title() const { return title_; }

    void set_collapsible(bool c) { collapsible_ = c; request_repaint(); }
    bool collapsible() const { return collapsible_; }

    // Collapsed hides all child content; expanded shows it. Fires on_toggle.
    void set_collapsed(bool c);
    bool collapsed() const { return collapsed_; }

    // Y at which child content begins (below the header band).
    float content_top() const { return header_height + 6.0f; }

    std::function<void(bool collapsed)> on_toggle;

    void paint(canvas::Canvas& canvas) override;
    void on_mouse_event(const MouseEvent& event) override;
    bool wants_mouse_input() const override { return true; }

private:
    void apply_child_visibility();
    std::string title_;
    bool collapsible_ = false;
    bool collapsed_ = false;
};

// ── DualRangeSlider ──────────────────────────────────────────────────────────
// Two-thumb min↔max range selector (the Figma "Range Slider"): a track with a
// filled segment between an independently-draggable low and high thumb. (The
// single-thumb pulp::view::RangeSlider is a plain value slider; this is the
// dual-handle variant.) Horizontal or vertical; optional disabled state.
class DualRangeSlider : public View {
public:
    enum class Orientation { horizontal, vertical };
    void set_orientation(Orientation o) { orientation_ = o; request_repaint(); }
    void set_range(float min, float max) { min_ = min; max_ = std::max(min, max); clamp_(); }
    void set_low(float v) { low_ = v; clamp_(); }
    void set_high(float v) { high_ = v; clamp_(); }
    float low() const { return low_; }
    float high() const { return high_; }
    float min_value() const { return min_; }
    float max_value() const { return max_; }
    void set_enabled(bool e) { enabled_ = e; request_repaint(); }
    bool enabled() const { return enabled_; }
    // When true, a dragged thumb stops at the other thumb instead of crossing it
    // (low ≤ high always). Default false (ends move freely / may cross).
    void set_no_cross(bool b) { no_cross_ = b; }
    bool no_cross() const { return no_cross_; }

    // Fired on a thumb drag with the (possibly-updated) low and high values.
    std::function<void(float low, float high)> on_change;

    void paint(canvas::Canvas& canvas) override;
    void on_mouse_down(Point pos) override;
    void on_mouse_drag(Point pos) override;
    void on_mouse_up(Point pos) override;
    bool wants_mouse_input() const override { return true; }

private:
    void clamp_();
    float pos_for_(float v) const;        // value → 0..1 along the track
    float value_for_pos_(float t) const;  // 0..1 → value
    float pointer_t_(Point pos) const;    // pointer → 0..1 along the track
    void apply_(Point pos);               // move the active thumb to the pointer
    Orientation orientation_ = Orientation::horizontal;
    float min_ = 0.0f, max_ = 1.0f, low_ = 0.25f, high_ = 0.70f;
    bool enabled_ = true;
    bool no_cross_ = false;
    int drag_ = -1;   // 0 = low thumb, 1 = high thumb, -1 = none
};

// ── InlineValueEditor ────────────────────────────────────────────────────────
// A click-to-type numeric value readout (the Figma "Inline Value Editor"): shows
// `value + suffix` idle; click to edit (caret), Enter commits, Esc cancels. A
// committed value outside [min,max] shows a danger ring and is rejected. Pair it
// with a Knob/Fader (the readout drives the control via on_change; the control
// updates the readout via set_value). States: idle / editing / invalid / disabled.
class InlineValueEditor : public View {
public:
    InlineValueEditor() { set_focusable(true); }

    void set_value(double v) { value_ = v; request_repaint(); }
    double value() const { return value_; }
    void set_range(double min, double max) { min_ = min; max_ = max; }
    void set_suffix(std::string s) { suffix_ = std::move(s); request_repaint(); }
    void set_decimals(int d) { decimals_ = d; request_repaint(); }
    void set_enabled(bool e) { enabled_ = e; if (!e) editing_ = false; request_repaint(); }
    bool enabled() const { return enabled_; }
    bool editing() const { return editing_; }
    bool invalid() const { return invalid_; }

    // Fired when a typed value is committed in range.
    std::function<void(double)> on_change;

    void begin_edit();
    void commit_edit();
    void cancel_edit();

    /// Caret shape. Defaults to the process-wide style (`CaretStyle::ibeam`).
    void set_caret_style(CaretStyle style) { caret_style_ = style; request_repaint(); }
    CaretStyle caret_style() const { return caret_style_; }

    /// Blink timing. Defaults to the process-wide config.
    void set_caret_blink(const CaretBlinkConfig& config) {
        caret_blink_.set_config(config);
        request_repaint();
    }
    const CaretBlinkConfig& caret_blink() const { return caret_blink_.config(); }

    ~InlineValueEditor() override;

    void paint(canvas::Canvas& canvas) override;
    void on_mouse_down(Point pos) override;
    void on_text_input(const TextInputEvent& event) override;
    bool on_key_event(const KeyEvent& event) override;
    void on_focus_changed(bool gained) override;
    /// Re-home the caret-blink subscription when the tree's FrameClock changes —
    /// same dangling-cached-clock hazard as TextEditor. See the definition.
    void on_frame_clock_changed() override;
    bool wants_mouse_input() const override { return true; }

private:
    std::string display_() const;     // value + suffix
    void subscribe_caret_blink_();
    void unsubscribe_caret_blink_();
    double value_ = 0.0, min_ = -1e9, max_ = 1e9;
    int decimals_ = 1;
    std::string suffix_;
    bool enabled_ = true, editing_ = false, invalid_ = false;
    std::string edit_buffer_;
    CaretBlink caret_blink_;
    CaretStyle caret_style_ = default_caret_style();
    int caret_blink_sub_ = -1;
    FrameClock* caret_blink_clock_ = nullptr;  ///< cached: frame_clock() returns
                                               ///< null once detached, which would
                                               ///< leak the sub with a dangling `this`
};

// ── SpectrogramView ──────────────────────────────────────────────────────────
// Scrolling time-frequency display. Each STFT frame becomes a column of
// colored pixels, scrolling left as new frames arrive.

class SpectrogramView : public View {
public:
    SpectrogramView() = default;

    /// Push a new STFT frame (dB magnitudes) for display.
    void push_spectrum(const float* magnitudes_db, int num_bins);

    /// Configure display dimensions and color mapping.
    void configure(int history_columns, int freq_rows,
                   signal::ColorRamp ramp = signal::ColorRamp::inferno,
                   float min_db = -80.0f, float max_db = 0.0f);

    void set_color_ramp(signal::ColorRamp ramp) { mapper_.set_ramp(ramp); }
    void set_range(float min_db, float max_db) { min_db_ = min_db; max_db_ = max_db; }

    int history_columns() const { return buffer_.width(); }
    int freq_rows() const { return buffer_.height(); }

    void paint(canvas::Canvas& canvas) override;

private:
    signal::SpectrogramBuffer buffer_;
    signal::ColorMapper mapper_{signal::ColorRamp::inferno};
    float min_db_ = -80.0f;
    float max_db_ = 0.0f;
    bool configured_ = false;
};

// ── MultiMeter ──────────────────────────────────────────────────────────────
// Multi-channel level meter with configurable layout. Supports arbitrary
// channel counts (mono through ambisonic). Uses MultiChannelBallistics
// for smooth display.

class MultiMeter : public View {
public:
    enum class Layout { vertical, horizontal };
    enum class DisplayStyle { continuous, segmented };

    MultiMeter() { set_access_role(AccessRole::meter); }

    /// Update from multi-channel meter data. Call once per UI frame.
    void update(const signal::MultiChannelMeterData& data, float dt);

    void set_layout(Layout l) { layout_ = l; }
    Layout layout() const { return layout_; }

    void set_display_style(DisplayStyle s) { display_style_ = s; }
    DisplayStyle display_style() const { return display_style_; }

    void set_channel_count(int count);
    int channel_count() const { return ballistics_.num_channels; }

    /// Access ballistics for testing.
    const signal::MultiChannelBallistics& ballistics() const { return ballistics_; }

    void paint(canvas::Canvas& canvas) override;

private:
    Layout layout_ = Layout::vertical;
    DisplayStyle display_style_ = DisplayStyle::continuous;
    signal::MultiChannelBallistics ballistics_;
};

// ── CorrelationMeter ────────────────────────────────────────────────────────
// Stereo correlation display (-1 to +1). Shows phase relationship between
// left and right channels.

class CorrelationMeter : public View {
public:
    CorrelationMeter() { set_access_role(AccessRole::meter); }

    /// Update with new correlation value (-1 to +1). Call once per UI frame.
    void update(float correlation, float dt);

    float display_correlation() const { return display_correlation_; }

    void paint(canvas::Canvas& canvas) override;

private:
    float display_correlation_ = 0.0f;
    float smoothing_coeff_ = 0.1f; // Exponential smoothing
};

// ── WaveformRecorder ─────────────────────────────────────────────────────────
// Three-state recorder control (Ink & Signal "Recorder" component). One widget
// composes a filled waveform display, a bottom level meter with a draggable
// threshold thumb, a center transport button, and a top-right status badge,
// and re-skins all four for each state:
//
//   armed     — faint/empty waveform, red record dot, "READY" badge; the
//               threshold thumb is draggable to arm the capture trigger.
//   recording — waveform tinted with accent.error, stop square, live "REC"
//               badge; the meter shows the incoming level but the thumb is
//               locked (you can't re-arm mid-take).
//   captured  — waveform in accent.primary (teal), play triangle, "CAPTURED"
//               badge with a check; the meter is inert.
//
// Clicking the center transport button advances the state in the cycle
// armed → recording → captured → armed and fires on_state_change plus the
// matching transport callback (on_record / on_stop / on_play). All chrome is
// drawn from theme tokens so it stays legible on the dark Ink & Signal theme.
class WaveformRecorder : public View {
public:
    enum class State { armed, recording, captured };

    WaveformRecorder() {
        set_access_role(AccessRole::group);
        set_focusable(true);
    }

    /// Transport state. Setting a new state repaints and fires on_state_change.
    void set_state(State s);
    State state() const { return state_; }
    std::function<void(State)> on_state_change;

    /// Normalized waveform samples (-1..1) drawn across the main area.
    void set_waveform(std::vector<float> samples) {
        waveform_ = std::move(samples);
        request_repaint();
    }
    const std::vector<float>& waveform() const { return waveform_; }

    /// Live input level (0..1) shown by the bottom meter fill.
    void set_level(float level) {
        float c = std::clamp(level, 0.0f, 1.0f);
        if (c == level_) return;
        level_ = c;
        request_repaint();
    }
    float level() const { return level_; }

    /// Capture threshold (0..1) marked by the draggable meter thumb. The setter
    /// is silent (programmatic); user drags fire on_threshold_change.
    void set_threshold(float threshold) {
        float c = std::clamp(threshold, 0.0f, 1.0f);
        if (c == threshold_) return;
        threshold_ = c;
        request_repaint();
    }
    float threshold() const { return threshold_; }
    std::function<void(float)> on_threshold_change;

    /// Optional transport-action callbacks, fired alongside the state advance.
    std::function<void()> on_record;
    std::function<void()> on_stop;
    std::function<void()> on_play;

    void paint(canvas::Canvas& canvas) override;
    void on_mouse_down(Point pos) override;
    void on_mouse_drag(Point pos) override;
    void on_mouse_up(Point pos) override;
    bool wants_mouse_input() const override { return true; }

private:
    /// Bottom level-meter strip (the draggable threshold track).
    Rect meter_rect_() const;
    /// Main waveform area (between the badge row and the meter).
    Rect waveform_rect_() const;
    float transport_radius_() const;
    Point transport_center_() const;
    /// Button-driven cycle: fires the current state's transport callback,
    /// then advances armed→recording→captured→armed (via set_state).
    void advance_state_();
    /// Map a local x within the meter to a threshold and fire on_threshold_change.
    void update_threshold_from_x_(float x);

    State state_ = State::armed;
    std::vector<float> waveform_;
    float level_ = 0.0f;
    float threshold_ = 0.5f;
    bool dragging_threshold_ = false;
};

} // namespace pulp::view
