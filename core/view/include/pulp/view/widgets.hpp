#pragma once

#include <algorithm>
#include <pulp/view/view.hpp>
#include <pulp/view/audio_bridge.hpp>
#include <pulp/view/animation.hpp>
#include <pulp/view/sprite_strip.hpp>
#include <pulp/signal/spectrogram.hpp>
#include <pulp/signal/multi_channel_meter.hpp>
#include <string>
#include <string_view>
#include <cstddef>
#include <cmath>
#include <functional>
#include <array>
#include <vector>
#include <optional>

namespace pulp::view {

// ── Label ────────────────────────────────────────────────────────────────────
// Static or dynamic text display

/// Text alignment for Label.
/// pulp #1434 — `auto_` resolves at paint time to left (LTR) or right
/// (RTL); pulp doesn't model RTL yet so `auto_` currently degrades to
/// `left`. `justify` wires through to the canvas `TextAlign::justify`
/// enum value; SkParagraph kJustify rendering lands in a follow-up —
/// existing canvas backends treat it as `left` until then. Both values
/// are claimed in the rn/css catalog (Figma exports + Tailwind classes
/// emit `auto` and `justify` routinely).
/// `match_parent` (pulp #1434 follow-up) resolves at paint time by
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
        set_access_label(text_);
    }

    void set_text(std::string text) {
        if (text == text_) return;
        text_ = std::move(text);
        // WYSIWYG P5 FIX 4 — the Yoga measure callback (yoga_measure ->
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

    // issue-969: each setter marks the corresponding has_own_* flag so
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

    // issue-969: explicit text color override on the Label itself. When
    // set, overrides any inherited typography color and the theme token.
    void set_text_color(canvas::Color c) { text_color_ = c; has_own_text_color_ = true; }
    bool has_own_text_color() const { return has_own_text_color_; }
    canvas::Color text_color() const { return text_color_; }

    void set_multi_line(bool ml) { multi_line_ = ml; }
    bool multi_line() const { return multi_line_; }

    /// CSS `line-clamp` / `-webkit-line-clamp` (pulp #1552). Maximum number
    /// of visible text lines for a multi-line label; 0 disables clamping.
    /// When set on a `multi_line_` Label, paint() emits at most N
    /// newline-separated lines and replaces the trailing visible line with
    /// the U+2026 ellipsis if any source lines were dropped. Matches the
    /// CSS spec's "block-axis line clamp" behavior at the keyword level
    /// (no `none` token — set 0 to clear). Wiring is shared between
    /// `line-clamp` and `-webkit-line-clamp` in the JS shim and the
    /// @pulp/react prop-applier (pulp #1434 catalog: both keys funnel
    /// through the same case).
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
    /// pulp #1434 — accepted via the JS CSS shim and the bridge so authors
    /// can express the per-style longhand. Today the paint path always
    /// renders as `solid`; the value is stored so future paint logic can
    /// honor it without an API break (matches the spec's optional fallback
    /// to solid for renderers that don't implement non-solid styles).
    enum class TextDecorationStyle { solid, double_, dotted, dashed, wavy };
    void set_text_decoration_style(TextDecorationStyle s) { text_decoration_style_ = s; }
    TextDecorationStyle text_decoration_style() const { return text_decoration_style_; }

    void paint(canvas::Canvas& canvas) override;

    /// Intrinsic width — measured text width (issue-928).
    /// Uses TextShaper (HarfBuzz/SkParagraph) when available, otherwise
    /// falls back to the same character-width estimate the base Canvas
    /// uses. This lets Yoga reserve enough horizontal space for the full
    /// label content instead of clipping to a parent-inherited width.
    /// Applies the same text-transform as paint() so measurement matches
    /// what is actually drawn.
    float intrinsic_width() const override;

    /// Intrinsic height based on font size and line height.
    /// issue-969: walks the inheritance cascade so an unset font_size
    /// picks up an ancestor View's setInheritableFontSize value.
    /// pulp-internal #74: returns lh × (newline-count + 1) for
    /// multi_line labels so Yoga reserves space for every explicit
    /// `\n`-delimited line. Soft-wrap (multi_line + bounded width) is
    /// handled by the width-aware `measured_height()` overload below.
    float intrinsic_height() const override;

    /// Width-aware height: shaper-true line count × line-height for a
    /// multi_line Label given the parent's available content width.
    /// pulp-internal #74 — invoked by the Yoga measure callback when a
    /// multi-line Label is laid out in a bounded-width parent (CSS
    /// `flex: 1`, `width: 100%`, fixed-width section subtitles). Falls
    /// back to `intrinsic_height()` when `multi_line_` is false or
    /// `available_width <= 0`, so single-line labels and unbounded
    /// containers keep the legacy one-line metric.
    float measured_height(float available_width) const;

    /// pulp #2163 / font-v2 Slice 1.1.b — baseline offset from the top
    /// of the Label's measured box, used by Yoga's
    /// `YGNodeSetBaselineFunc` to honor `align-items: baseline` on
    /// flex containers (CHAIN INFO baseline-alignment canary). Returns
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

private:
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
    int line_clamp_ = 0;          ///< pulp #1552: 0=no clamp, >=1=max lines
    TextTransform text_transform_ = TextTransform::none;
    TextDecoration text_decoration_ = TextDecoration::none;
    canvas::Color decoration_color_{};
    bool has_decoration_color_ = false;
    TextDecorationStyle text_decoration_style_ = TextDecorationStyle::solid;
    canvas::TextDirection text_direction_ = canvas::TextDirection::left_to_right;
    canvas::TextVerticalAlign vertical_align_ = canvas::TextVerticalAlign::top;
    // issue-969: explicit-vs-inherited tracking. Fields keep their default
    // values until a setter is called; the has_own_* flag tells paint()
    // whether to honor the field or fall through to View::inheritable_*().
    canvas::Color text_color_{};
    bool has_own_text_color_ = false;
    bool has_own_font_size_ = false;
    bool has_own_font_weight_ = false;
    bool has_own_letter_spacing_ = false;
    bool has_own_text_align_ = false;

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
enum class WidgetRenderStyle { standard, minimal };

class Knob : public View {
public:
    Knob() { set_access_role(AccessRole::slider); set_focusable(true); }

    void set_render_style(WidgetRenderStyle s) { render_style_ = s; }
    WidgetRenderStyle render_style() const { return render_style_; }

    // pulp #73 — programmatic set_value MUST request_repaint() WHEN the
    // value actually changes. The host's mouseDown handler invalidates
    // after every input event, so user drags repaint correctly via that
    // side channel; preset application (or any other JS-driven mutation
    // through the bridge's setValue) goes through THIS code path and
    // would otherwise leave the painted state stale until the next user
    // input. The user's "preset-applied band-shape outline missing"
    // symptom was exactly this gap — the new state was stored but
    // never reached the screen.
    //
    // Codex P2 on PR #2013 — gate the invalidate on a real change.
    // WidgetBridge::sync_from_store() and restore_values(...) call
    // set_value()/set_on() in tight loops during sync/reload; firing a
    // host repaint per call (even when the value is unchanged) burns
    // wall-clock on large widget trees with no visible benefit.
    void set_value(float v) {
        float clamped = std::clamp(v, 0.0f, 1.0f);
        if (clamped == value_) return;
        value_ = clamped;
        request_repaint();
    }
    float value() const { return value_; }

    void set_default_value(float v) { default_value_ = std::clamp(v, 0.0f, 1.0f); }
    float default_value() const { return default_value_; }

    void set_label(std::string text) {
        if (label_ == text) return;
        label_ = std::move(text);
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

    // Animation accessors for testing
    float hover_glow() const { return hover_glow_.value(); }
    void advance_animations(float dt);

    // Arc range in radians (default: 270-degree sweep)
    static constexpr float start_angle = 2.356f;  // 135 degrees (bottom-left)
    static constexpr float end_angle = 7.069f;    // 405 degrees (bottom-right via top)

    // Custom shader: if set, replaces the body/track/fill arc drawing with SkSL GPU shader.
    // Labels, value text, and hover glow still draw in C++.
    void set_custom_shader(std::string sksl) { custom_sksl_ = std::move(sksl); }
    void clear_custom_shader() { custom_sksl_.clear(); }
    bool has_custom_shader() const { return !custom_sksl_.empty(); }
    const std::string& custom_shader() const { return custom_sksl_; }
    bool shader_uses_time() const { return custom_sksl_.find("time") != std::string::npos; }

    // Declarative widget schema: JSON defining appearance as data (Rive-inspired)
    void set_widget_schema(std::string json) { widget_schema_ = std::move(json); }
    const std::string& widget_schema() const { return widget_schema_; }

private:
    float value_ = 0.0f;
    float default_value_ = 0.5f;
    std::string label_;
    std::function<std::string(float)> format_;
    ValueAnimation hover_glow_{0.0f};
    float drag_start_y_ = 0;
    float drag_start_value_ = 0;
    bool gesture_active_ = false;
    bool show_label_ = true;
    std::string custom_sksl_;     // SkSL source for GPU shader body
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

private:
    std::shared_ptr<SpriteStrip> sprite_strip_;
};

// ── Fader ────────────────────────────────────────────────────────────────────
// Linear slider for audio parameters

class Fader : public View {
public:
    enum class Orientation { vertical, horizontal };
    enum class ThumbShape { circle, rectangle };

    Fader() { set_access_role(AccessRole::slider); set_focusable(true); }

    void set_render_style(WidgetRenderStyle s) { render_style_ = s; }
    WidgetRenderStyle render_style() const { return render_style_; }

    // pulp #73 — see Knob::set_value rationale (incl. the no-change
    // guard added per Codex P2 on PR #2013).
    void set_value(float v) {
        float clamped = std::clamp(v, 0.0f, 1.0f);
        if (clamped == value_) return;
        value_ = clamped;
        request_repaint();
    }
    float value() const { return value_; }

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

    // Animation accessors for testing
    float hover_scale() const { return hover_thumb_scale_.value(); }
    void advance_animations(float dt);

    void set_custom_shader(std::string sksl) { custom_sksl_ = std::move(sksl); }
    void clear_custom_shader() { custom_sksl_.clear(); }
    bool has_custom_shader() const { return !custom_sksl_.empty(); }
    const std::string& custom_shader() const { return custom_sksl_; }
    bool shader_uses_time() const { return custom_sksl_.find("time") != std::string::npos; }
    void set_widget_schema(std::string json) { widget_schema_ = std::move(json); }
    const std::string& widget_schema() const { return widget_schema_; }
    void set_lottie_json(std::string json) { lottie_json_ = std::move(json); }
    const std::string& lottie_json() const { return lottie_json_; }
    void set_lottie_time(float t) { lottie_time_ = std::clamp(t, 0.0f, 1.0f); }
    float lottie_time() const { return lottie_time_; }

private:
    float value_ = 0.0f;
    Orientation orientation_ = Orientation::vertical;
    ThumbShape thumb_shape_ = ThumbShape::circle;
    float thumb_width_ = 0.0f;
    float thumb_height_ = 0.0f;
    float thumb_corner_radius_ = 0.0f;
    std::string label_;
    ValueAnimation hover_thumb_scale_{1.0f};
    bool dragging_ = false;
    std::string custom_sksl_;
    std::string widget_schema_;
    std::string lottie_json_;
    float lottie_time_ = 0.0f;
    WidgetRenderStyle render_style_ = WidgetRenderStyle::standard;

public:
    /// Sprite strip: designer-created filmstrip for fader appearance.
    void set_sprite_strip(std::shared_ptr<SpriteStrip> strip) { sprite_strip_ = std::move(strip); }
    const std::shared_ptr<SpriteStrip>& sprite_strip() const { return sprite_strip_; }

private:
    std::shared_ptr<SpriteStrip> sprite_strip_;
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
// pulp issue-966.

class RangeSlider : public View {
public:
    enum class Orientation { horizontal, vertical };

    RangeSlider() {
        set_access_role(AccessRole::slider);
        set_focusable(true);
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
    /// pulp #73 — request_repaint() so programmatic preset application
    /// reaches the screen, not just the next user-input event.
    /// Codex P2 on PR #2013 — gate on actual change to avoid redundant
    /// invalidations during sync_from_store / restore_values loops.
    void set_value(float v) {
        float prev = value_;
        value_ = v;
        clamp_and_quantize_();
        if (value_ != prev) request_repaint();
    }
    float value() const { return value_; }

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

    /// Fired when the value changes — from drag, click, or set_value(). The
    /// callback receives the post-quantisation value, exactly the same
    /// number value() will return.
    std::function<void(float)> on_change;

    void paint(canvas::Canvas& canvas) override;
    void on_mouse_event(const MouseEvent& event) override;
    void on_mouse_drag(Point pos) override;

private:
    /// Convert a position along the track (0=start, 1=end) to a value
    /// after applying clamp + step quantisation.
    float position_to_value_(float t) const;

    /// Clamp value_ to [min_,max_] and snap it to the nearest step, then
    /// fire on_change if the post-quantisation value actually changed.
    void clamp_and_quantize_();

    /// Common drag/click handler — `pos` is in local coordinates.
    void update_from_position_(Point pos);

    float min_ = 0.0f;
    float max_ = 1.0f;
    float step_ = 0.0f;
    float value_ = 0.0f;
    Orientation orientation_ = Orientation::horizontal;
    bool dragging_ = false;
    float track_thickness_ = 4.0f;
    canvas::Color accent_color_{};
    bool has_accent_color_ = false;
};

// ── Toggle ───────────────────────────────────────────────────────────────────
// Boolean on/off switch

class Toggle : public View {
public:
    Toggle() : thumb_position_(0.0f), hover_opacity_(0.0f) {
        set_access_role(AccessRole::toggle); set_focusable(true);
    }

    void set_on(bool v);
    bool is_on() const { return on_; }

    void set_label(std::string text) { label_ = std::move(text); }
    const std::string& label() const { return label_; }

    /// Called when the toggle state changes.
    std::function<void(bool)> on_toggle;

    void paint(canvas::Canvas& canvas) override;
    void on_mouse_down(Point pos) override;
    void on_mouse_enter() override;
    void on_mouse_leave() override;

    // Animation accessors for testing
    float thumb_position() const { return thumb_position_.value(); }
    float hover_opacity() const { return hover_opacity_.value(); }
    void advance_animations(float dt);

    void set_custom_shader(std::string sksl) { custom_sksl_ = std::move(sksl); }
    void clear_custom_shader() { custom_sksl_.clear(); }
    bool has_custom_shader() const { return !custom_sksl_.empty(); }
    const std::string& custom_shader() const { return custom_sksl_; }
    bool shader_uses_time() const { return custom_sksl_.find("time") != std::string::npos; }
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
    std::string custom_sksl_;
    std::string widget_schema_;
    std::string lottie_json_;
    float lottie_time_ = 0.0f;
};

// ── Checkbox ────────────────────────────────────────────────────────────────
// Circular checkbox with check glyph

class Checkbox : public View {
public:
    Checkbox() { set_access_role(AccessRole::toggle); set_focusable(true); }

    // pulp #73 — see Knob::set_value (incl. no-change guard from Codex
    // P2 on PR #2013).
    void set_checked(bool v) {
        if (checked_ == v) return;
        checked_ = v;
        request_repaint();
    }
    bool is_checked() const { return checked_; }

    std::function<void(bool)> on_change;

    void paint(canvas::Canvas& canvas) override;
    void on_mouse_down(Point pos) override;

private:
    bool checked_ = false;
};

// ── ToggleButton ────────────────────────────────────────────────────────────
// Full-width rounded button that toggles on/off

class ToggleButton : public View {
public:
    ToggleButton() { set_access_role(AccessRole::toggle); set_focusable(true); }

    // pulp #73 — see Knob::set_value (incl. no-change guard from Codex
    // P2 on PR #2013).
    void set_on(bool v) {
        if (on_ == v) return;
        on_ = v;
        request_repaint();
    }
    bool is_on() const { return on_; }

    void set_label(std::string text) {
        if (label_ == text) return;
        label_ = std::move(text);
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

    float intrinsic_height() const override { return 36.0f; }

private:
    bool on_ = false;
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
// Simple vector icons drawn with canvas strokes

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

    /// URI-keyed image source (workstream 07 B4). Accepts file://,
    /// resource://, or memory://sha256=<hex>. When an ImageCache is
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

    void paint(canvas::Canvas& canvas) override;

private:
    std::string path_;
    bool loaded_ = false;
    std::vector<uint8_t> cached_data_;  // File bytes cached after first successful load
    ImageCache* cache_ = nullptr;       // optional; owned externally
};

// ── Meter ────────────────────────────────────────────────────────────────────
// Audio level meter with peak hold

class Meter : public View {
public:
    enum class Orientation { vertical, horizontal };

    Meter() { set_access_role(AccessRole::meter); }

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

    void paint(canvas::Canvas& canvas) override;

private:
    Orientation orientation_ = Orientation::vertical;
    MeterBallistics ballistics_;
    float current_rms_ = 0;
    float current_peak_ = 0;
    WidgetRenderStyle render_style_ = WidgetRenderStyle::standard;
};

// ── XYPad ────────────────────────────────────────────────────────────────────
// 2D parameter control surface (e.g., filter frequency × resonance)

class XYPad : public View {
public:
    XYPad() = default;

    void set_x(float v) { x_ = std::clamp(v, 0.0f, 1.0f); }
    void set_y(float v) { y_ = std::clamp(v, 0.0f, 1.0f); }
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
//                         per redraw. Pulp item 6.12 — wire AudioThumbnail
//                         into existing WaveformView without inventing a
//                         new widget.

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
    // `AudioThumbnail` (Pulp item 6.12). The thumbnail is borrowed —
    // callers (typically an `AudioThumbnailCache` owner) must outlive the
    // widget or call `clear_thumbnail()`. Setting a thumbnail clears any
    // raw sample buffer.
    //
    // `channel == UINT32_MAX` folds all channels into one display row.
    void set_thumbnail(const pulp::audio::AudioThumbnail* thumb,
                       uint32_t channel = static_cast<uint32_t>(-1));
    void clear_thumbnail();

    bool has_thumbnail() const noexcept { return thumbnail_ != nullptr; }
    const pulp::audio::AudioThumbnail* thumbnail() const noexcept { return thumbnail_; }

    size_t sample_count() const { return samples_.size(); }

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

    const pulp::audio::AudioThumbnail* thumbnail_ = nullptr;
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
    // pulp #1731 Codex P1 — Panel used to shadow View::corner_radius_
    // with its own field that paint() never read. Constructor now seeds
    // the View-side slot so the default 8px rounding actually paints.
    Panel() { View::set_border_radius(8.0f); }

    void set_background_token(std::string token) { bg_token_ = std::move(token); }
    void set_border_token(std::string token) { border_token_ = std::move(token); }
    // pulp #1731 Codex P1 — route through View::set_border_radius so the
    // px slot painter actually reads is updated (and the pct slot is
    // cleared, matching documented semantics).
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

    MultiMeter() { set_access_role(AccessRole::meter); }

    /// Update from multi-channel meter data. Call once per UI frame.
    void update(const signal::MultiChannelMeterData& data, float dt);

    void set_layout(Layout l) { layout_ = l; }
    Layout layout() const { return layout_; }

    void set_channel_count(int count);
    int channel_count() const { return ballistics_.num_channels; }

    /// Access ballistics for testing.
    const signal::MultiChannelBallistics& ballistics() const { return ballistics_; }

    void paint(canvas::Canvas& canvas) override;

private:
    Layout layout_ = Layout::vertical;
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

} // namespace pulp::view
