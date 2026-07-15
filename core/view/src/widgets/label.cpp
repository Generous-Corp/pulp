#include <pulp/view/widgets.hpp>
#include <pulp/view/text_editor.hpp>
#include <pulp/view/animation.hpp>
#include <pulp/view/frame_clock.hpp>
#include <pulp/view/image_cache.hpp>
#include <pulp/view/text_overflow.hpp>
#include <pulp/view/window_host.hpp>
#include <pulp/canvas/text_shaper.hpp>
#include <pulp/canvas/bundled_fonts.hpp>  // font_registration_generation() for the shaped-layout cache key
#include <choc/text/choc_JSON.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <string>

namespace pulp::view {

// ── Label ────────────────────────────────────────────────────────────────────

float Label::intrinsic_height() const {
    // Cascade font_size before computing height so descendants of a parent
    // that called setInheritableFontSize report a height that matches what
    // paint() will draw.
    float effective_font_size = font_size_;
    if (!has_own_font_size_) {
        if (auto inh = inheritable_font_size(); inh.has_value())
            effective_font_size = inh.value();
    }

    // Prefer the shaper's real metrics (worst-case ascent + descent from
    // SkFontMetrics fTop/fBottom plus the PULP_FONT_NO_SAFETY_MARGIN-gated
    // empirical safety margin) over the `font_size * 1.6` / `font_size * 1.4`
    // multiplier. Real metrics make `intrinsic_height` track what paint()
    // draws, because the same shaper cache feeds Label::paint baseline math
    // and the Yoga measure callback. Falls back to the multiplier only when
    // the shaper hasn't resolved real metrics (no Skia / family unresolvable).
    //
    // Small fonts (< 12px) use `font_size * 1.6` instead of
    // `font_size * 1.4` because the fixed-multiplier fallback needs extra
    // headroom at small sizes, where a 1.4 line box left descenders clipped
    // under the GPU clip-rect. Real fTop/fBottom metrics already cover that
    // case (they include caps + descenders by construction) plus the empirical
    // safety margin.
    std::string effective_family = font_family_;
    if (effective_family.empty()) {
        if (auto inh = inheritable_font_family(); inh.has_value())
            effective_family = inh.value();
    }
    if (effective_family.empty()) effective_family = "Inter";

    auto& shaper = canvas::global_text_shaper();
    auto prepared = shaper.prepare(text_.empty() ? std::string(" ") : text_,
                                   effective_family, effective_font_size);
    const float lh_mult = effective_font_size < 12.0f ? 1.6f : 1.4f;
    float lh;
    if (line_height_ > 0) {
        lh = line_height_;
    } else if (prepared.line_height() > 0) {
        lh = prepared.line_height();
    } else {
        lh = effective_font_size * lh_mult;
    }

    // When the Label is multi_line, the reserved height must reflect the
    // number of lines paint() will emit, not a hard-coded one-line metric.
    // Otherwise Yoga reserves only `lh` of vertical room and the parent
    // (overflow:hidden on the toolbar / row gap on Settings-modal section
    // subtitles) clips every line after the first.
    //
    // The \n count is the lower bound here. Soft-wrap (no \n but text
    // exceeds the available width) needs the width Yoga passes to the
    // measure callback — see `measured_height(available_width)` below.
    //
    // Single-line labels (multi_line_ == false) keep the legacy one-line
    // return so single-line widths/heights match exactly what paint()
    // computes for `text_h = effective_font_size` (the contract every
    // existing test depends on).
    if (multi_line_ && !text_.empty()) {
        int line_count = 1;
        for (char c : text_) {
            if (c == '\n') ++line_count;
        }
        // Don't reserve a phantom line for a trailing newline.
        // Label::paint()'s `\n`-split loop emits one line per non-trailing
        // `\n` plus the final segment, so `"Title\n"` paints exactly one
        // visible line. If we keep the naive `\n`-count + 1 here, Yoga
        // reserves extra vertical whitespace that paint never fills, breaking
        // CSS-style vertical-align centering and shifting siblings down.
        // Matches the line-box counting CSS uses for `white-space: pre`.
        if (text_.back() == '\n') --line_count;
        // Honor line-clamp if explicitly set — paint() will only emit
        // `line_clamp_` lines, so reserving more height is wasteful and
        // confuses CSS vertical-align centering.
        if (line_clamp_ > 0 && line_clamp_ < line_count)
            line_count = line_clamp_;
        return lh * static_cast<float>(line_count);
    }
    return lh;
}

float Label::measured_height(float available_width) const {
    // Width-aware height for multi_line Labels with soft-wrap. The Yoga
    // measure callback receives the available width during layout, which is
    // the only place we can run the shaper to figure out how many lines a
    // soft-wrap block will actually produce. Without this hook, Yoga reserves
    // exactly one line `lh` and any wrapped line past the first paints into
    // sibling territory and is visually clipped.
    //
    // Contract:
    //   • single-line labels                 → intrinsic_height() (legacy).
    //   • multi-line + zero/unbounded width  → intrinsic_height() (\n only).
    //   • multi-line + finite width          → shaper line count * lh.
    //
    // The shaper itself is the same one paint() uses, so the count we
    // return here matches what paint() draws — no off-by-one, no
    // double-shape penalty (TextShaper::prepare() caches per
    // (text, family, size); paint will hit the same cache entry).
    if (!multi_line_ || text_.empty() || available_width <= 0.0f)
        return intrinsic_height();

    float effective_font_size = font_size_;
    if (!has_own_font_size_) {
        if (auto inh = inheritable_font_size(); inh.has_value())
            effective_font_size = inh.value();
    }
    // Match intrinsic_height's small-font multiplier so the measured line
    // height is consistent with what paint() draws.
    const float lh_mult = effective_font_size < 12.0f ? 1.6f : 1.4f;
    const float lh = line_height_ > 0 ? line_height_ : effective_font_size * lh_mult;

    // Mirror paint()'s text-transform — the line count for an
    // ALL-CAPS-via-text-transform string can differ from the source
    // string's because uppercase advances are typically wider.
    std::string display_text = text_;
    if (text_transform_ == TextTransform::uppercase) {
        for (auto& ch : display_text)
            ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
    } else if (text_transform_ == TextTransform::lowercase) {
        for (auto& ch : display_text)
            ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    } else if (text_transform_ == TextTransform::capitalize) {
        bool cap_next = true;
        for (auto& ch : display_text) {
            if (cap_next && std::isalpha(static_cast<unsigned char>(ch))) {
                ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
                cap_next = false;
            }
            if (ch == ' ') cap_next = true;
        }
    }

    const std::string& family = font_family_.empty() ? std::string("Inter") : font_family_;
    auto& shaper = canvas::global_text_shaper();
    auto prepared = shaper.prepare(display_text, family, effective_font_size);

    // Use the same break_mode paint uses (CSS word-break / overflow-wrap;
    // Label paint reads `View::word_break()` at draw time, the measure path
    // mirrors that decision).
    const std::string wb = word_break();
    canvas::BreakMode break_mode = canvas::BreakMode::normal;
    if      (wb == "break-word") break_mode = canvas::BreakMode::break_word;
    else if (wb == "anywhere")   break_mode = canvas::BreakMode::anywhere;
    auto layout = shaper.layout(prepared, available_width, lh, /*max_lines=*/0, break_mode);

    int line_count = std::max(1, layout.line_count);
    if (line_clamp_ > 0 && line_clamp_ < line_count)
        line_count = line_clamp_;
    return std::ceil(lh * static_cast<float>(line_count));
}

float Label::baseline_y() const {
    // Baseline offset from the top of the Label's box, used by Yoga's
    // YGNodeSetBaselineFunc to honor `align-items: baseline` on flex
    // containers. Without that channel, `align-items: baseline` silently
    // degrades to top-align of unequal-height boxes.
    //
    // The baseline of a single line of text sits at `ascent` distance
    // below the top of the worst-case glyph box (SkFontMetrics::fTop
    // semantics). For multi-line Labels we honor the first line's
    // baseline — that's what RN, the CSS spec, and Yoga's other
    // baseline-bearing children all do.
    float effective_font_size = font_size_;
    if (!has_own_font_size_) {
        if (auto inh = inheritable_font_size(); inh.has_value())
            effective_font_size = inh.value();
    }

    std::string effective_family = font_family_;
    if (effective_family.empty()) {
        if (auto inh = inheritable_font_family(); inh.has_value())
            effective_family = inh.value();
    }
    if (effective_family.empty()) effective_family = "Inter";

    // Skia's SkFontMetrics-derived ascent (PreparedText::ascent() flips
    // SkFontMetrics::fAscent positive). The painter computes baseline_y
    // from the same prepared metrics — see paint() —
    // so what Yoga sees here matches where the glyphs actually land.
    // For a Label with no text we still need a sensible baseline so a
    // baseline-aligned row of widgets (some text, some not) doesn't
    // collapse — feed the shaper a single space to pin the metric.
    auto& shaper = canvas::global_text_shaper();
    auto prepared = shaper.prepare(text_.empty() ? std::string(" ") : text_,
                                   effective_family, effective_font_size);
    float ascent = prepared.ascent();
    if (ascent <= 0.0f) {
        // Fallback when shaper metrics aren't real (no Skia, family
        // unresolvable): use the 0.85 × font_size heuristic. Better than
        // returning 0 and collapsing the baseline-aligned row.
        ascent = effective_font_size * 0.85f;
    }
    return ascent;
}

float Label::intrinsic_width() const {
    // Report the natural shaped-text width so Yoga reserves enough horizontal
    // space for the full label content. Without this, long labels in flex-row
    // containers inherit a small parent width and clip mid-word.
    //
    // For multi-line labels we deliberately return 0 so the parent
    // container's available width drives line wrapping instead of the
    // single-line text width.
    if (text_.empty() || multi_line_) return 0;

    // Intrinsic measurement must match what paint() will actually draw, so
    // honor the same own→inherited cascade for font_size and letter_spacing.
    float effective_font_size = font_size_;
    if (!has_own_font_size_) {
        if (auto inh = inheritable_font_size(); inh.has_value())
            effective_font_size = inh.value();
    }
    float effective_letter_spacing = letter_spacing_;
    if (!has_own_letter_spacing_) {
        if (auto inh = inheritable_letter_spacing(); inh.has_value())
            effective_letter_spacing = inh.value();
    }

    // When paint() rotates the text 90°, the horizontal footprint is just the
    // line height, not the shaped string advance. Reporting the advance here
    // makes Yoga reserve far too much width for vertical labels and starves
    // siblings.
    bool vertical = (text_direction_ == canvas::TextDirection::top_to_bottom ||
                     text_direction_ == canvas::TextDirection::bottom_to_top);
    if (vertical) {
        // Same small-font-size bump as intrinsic_height.
        const float lh_mult = effective_font_size < 12.0f ? 1.6f : 1.4f;
        return std::ceil(line_height_ > 0 ? line_height_ : effective_font_size * lh_mult);
    }

    // Mirror paint()'s text-transform — measurement must match what's
    // drawn or a row of uppercase chars will wrap unexpectedly.
    std::string display_text = text_;
    if (text_transform_ == TextTransform::uppercase) {
        for (auto& ch : display_text)
            ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
    } else if (text_transform_ == TextTransform::lowercase) {
        for (auto& ch : display_text)
            ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    } else if (text_transform_ == TextTransform::capitalize) {
        bool cap_next = true;
        for (auto& ch : display_text) {
            if (cap_next && std::isalpha(static_cast<unsigned char>(ch))) {
                ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
                cap_next = false;
            }
            if (ch == ' ') cap_next = true;
        }
    }

    // Shape with the same font + size the painter will use. TextShaper
    // uses the global Skia/HarfBuzz path when available and falls back
    // to a character-width estimator otherwise — same fallback that
    // Canvas::measure_text() uses on the recording / non-Skia backends.
    //
    // Use the Label's actual font_family, not a hardcoded "Inter". Different
    // families have very different metrics: monospaced fonts such as IBM Plex
    // Mono are materially wider than proportional Inter at the same size.
    // Under-reserving width clips imported labels (for example,
    // "XOVER → lo_freq") because Yoga reserves Inter's width while the painter
    // draws the requested family. Mirror paint()'s precedence: font_family_ if
    // set, else inheritable_font_family() cascade, else default "Inter".
    std::string effective_family = font_family_;
    if (effective_family.empty()) {
        if (auto inh = inheritable_font_family(); inh.has_value())
            effective_family = inh.value();
    }
    if (effective_family.empty()) effective_family = "Inter";

    auto& shaper = canvas::global_text_shaper();
    auto prepared = shaper.prepare(display_text, effective_family, effective_font_size);
    float width = prepared.total_width();

    // Letter-spacing adds extra advance per glyph break that isn't
    // captured by HarfBuzz shaping. Count UTF-8 *code points*, not
    // bytes — using `size()` over-applies spacing on multibyte input
    // (CJK, accented Latin, emoji) and inflates intrinsic width.
    if (effective_letter_spacing != 0 && !display_text.empty()) {
        std::size_t glyph_count = 0;
        for (unsigned char c : display_text) {
            // Count any byte that is not a UTF-8 continuation byte
            // (0b10xxxxxx) — that's one glyph per code point.
            if ((c & 0xC0) != 0x80) ++glyph_count;
        }
        if (glyph_count > 1) {
            width += effective_letter_spacing * static_cast<float>(glyph_count - 1);
        }
    }

    // Sub-pixel-safe ceil so layout never clips on rounding.
    return std::ceil(width);
}

// Shared style/origin resolver for paint() and text_edit_metrics(). Both call
// this so the inspector edit overlay's caret/selection geometry can never
// drift from the rendered glyphs.
// Resolves the inherited size/weight/letter-spacing cascade, the family
// fallback ("Inter"), slant, and the full text-align resolution (own →
// inherited → match-parent walk → auto). `baseline_y` is the SINGLE-LINE
// first-line baseline using the same vertical-align formula paint() uses
// (text_h == font_size); the multi-line painter recomputes text_h locally.
Label::ResolvedTextStyle Label::resolve_text_style() const {
    ResolvedTextStyle rs;

    rs.font_size = font_size_;
    if (!has_own_font_size_) {
        if (auto inh = inheritable_font_size(); inh.has_value())
            rs.font_size = inh.value();
    }
    rs.font_weight = font_weight_;
    if (!has_own_font_weight_) {
        if (auto inh = inheritable_font_weight(); inh.has_value())
            rs.font_weight = inh.value();
    }
    rs.letter_spacing = letter_spacing_;
    if (!has_own_letter_spacing_) {
        if (auto inh = inheritable_letter_spacing(); inh.has_value())
            rs.letter_spacing = inh.value();
    }
    rs.family = font_family_.empty() ? std::string("Inter") : font_family_;
    rs.font_slant = font_style_;

    // text-align cascade — own value wins, else inherited.
    LabelAlign align = text_align_;
    if (!has_own_text_align_) {
        if (auto inh = inheritable_text_align(); inh.has_value()) {
            int v = inh.value();
            if (v == 1) align = LabelAlign::center;
            else if (v == 2) align = LabelAlign::right;
            else if (v == 3) align = LabelAlign::auto_;
            else if (v == 4) align = LabelAlign::justify;
            else if (v == 5) align = LabelAlign::match_parent;
            else align = LabelAlign::left;
        }
    }
    // match-parent resolution — walk ancestors for the first non-5 SET
    // value. Mirrors paint() exactly.
    if (align == LabelAlign::match_parent) {
        LabelAlign parent_resolved = LabelAlign::left;
        for (auto* anc = parent(); anc != nullptr; anc = anc->parent()) {
            auto inh = anc->inheritable_text_align();
            if (!inh.has_value()) continue;
            int v = inh.value();
            if (v == 5) continue;
            if      (v == 1) parent_resolved = LabelAlign::center;
            else if (v == 2) parent_resolved = LabelAlign::right;
            else if (v == 3) parent_resolved = LabelAlign::auto_;
            else if (v == 4) parent_resolved = LabelAlign::justify;
            else             parent_resolved = LabelAlign::left;
            break;
        }
        align = parent_resolved;
    }
    if (align == LabelAlign::auto_) align = LabelAlign::left;  // LTR-only
    rs.text_align = align;

    // Single-line first-line baseline — same formula as paint() with
    // text_h == font_size (multi-line paint recomputes text_h itself).
    switch (vertical_align_) {
        case canvas::TextVerticalAlign::top:
            rs.baseline_y = rs.font_size * 0.85f;
            break;
        case canvas::TextVerticalAlign::bottom:
            rs.baseline_y = bounds().height - rs.font_size + rs.font_size * 0.85f;
            break;
        case canvas::TextVerticalAlign::baseline:
            rs.baseline_y = bounds().height * 0.75f;
            break;
        case canvas::TextVerticalAlign::center:
        default:
            rs.baseline_y = (bounds().height - rs.font_size) * 0.5f
                            + rs.font_size * 0.85f;
            break;
    }
    return rs;
}

std::string Label::apply_text_transform(const std::string& in) const {
    std::string out = in;
    if (text_transform_ == TextTransform::uppercase) {
        for (auto& ch : out) ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
    } else if (text_transform_ == TextTransform::lowercase) {
        for (auto& ch : out) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    } else if (text_transform_ == TextTransform::capitalize) {
        bool cap_next = true;
        for (auto& ch : out) {
            if (cap_next && std::isalpha(static_cast<unsigned char>(ch))) {
                ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
                cap_next = false;
            }
            if (ch == ' ') cap_next = true;
        }
    }
    return out;
}

// Translate the CSS font-variant CSV to SkShaper Feature tags and apply them
// to the canvas. Wires the storage-only View::font_variant_ slot into the
// active paint. Empty CSV clears features so the previous view's settings
// don't bleed across paint calls. Each CSS keyword maps to its OpenType
// feature tag (per CSS Fonts Module 4 §7.3):
//   tabular-nums       → tnum
//   small-caps         → smcp
//   oldstyle-nums      → onum
//   lining-nums        → lnum
//   proportional-nums  → pnum
// Unknown values are silently ignored (forward-compat).
//
// Shared by paint() and text_edit_metrics() — the WYSIWYG caret invariant: the
// inline-edit caret/selection must shape with the SAME features the painter
// renders, or the per-byte caret x drifts for font-variant labels.
void Label::apply_font_features(canvas::Canvas& canvas) const {
    const std::string& fv = font_variant();
    if (fv.empty()) {
        canvas.clear_font_features();
        return;
    }
    std::vector<canvas::Canvas::FontFeature> features;
    size_t i = 0;
    while (i < fv.size()) {
        while (i < fv.size() && (std::isspace(static_cast<unsigned char>(fv[i])) || fv[i] == ',')) ++i;
        if (i >= fv.size()) break;
        size_t end = i;
        while (end < fv.size() && fv[end] != ',') ++end;
        std::string token(fv, i, end - i);
        while (!token.empty() && std::isspace(static_cast<unsigned char>(token.back()))) token.pop_back();
        if      (token == "tabular-nums")      features.push_back({canvas::Canvas::make_font_feature_tag("tnum"), 1});
        else if (token == "small-caps")        features.push_back({canvas::Canvas::make_font_feature_tag("smcp"), 1});
        else if (token == "oldstyle-nums")     features.push_back({canvas::Canvas::make_font_feature_tag("onum"), 1});
        else if (token == "lining-nums")       features.push_back({canvas::Canvas::make_font_feature_tag("lnum"), 1});
        else if (token == "proportional-nums") features.push_back({canvas::Canvas::make_font_feature_tag("pnum"), 1});
        // Unknown token → silently ignored.
        i = end + 1;
    }
    if (!features.empty()) canvas.set_font_features(std::move(features));
    else                   canvas.clear_font_features();
}

Label::TextEditMetrics Label::text_edit_metrics(canvas::Canvas& canvas,
                                                std::string_view edit_text) const {
    TextEditMetrics m;
    const ResolvedTextStyle rs = resolve_text_style();

    // Apply the SAME text-transform paint() applies, so the measured run
    // matches the rendered run (e.g. an uppercase ENVELOPE label).
    m.display_text = apply_text_transform(std::string(edit_text));

    // Set the canvas font via the SAME full setter paint() uses, so
    // text_x_for_byte shapes with identical state (family/size/weight/
    // slant/letter-spacing).
    canvas.set_font_full(rs.family, rs.font_size, rs.font_weight,
                         rs.font_slant, rs.letter_spacing);

    // Apply the SAME font-variant OpenType features paint() applies, so the
    // caret/selection x shapes with identical glyph advances (tabular-nums,
    // small-caps, …). Without this the caret drifts for font-variant labels.
    apply_font_features(canvas);

    // Per-byte caret offsets over the FULL shaped run.
    m.caret_x_by_byte.resize(m.display_text.size() + 1, 0.0f);
    for (std::size_t i = 0; i <= m.display_text.size(); ++i) {
        m.caret_x_by_byte[i] = canvas.text_x_for_byte(m.display_text, i);
    }

    // Alignment-dependent left origin. paint() centers/right-aligns by
    // anchoring the canvas text-align at width/2 or width; the resulting
    // glyph left edge is what the overlay needs.
    const float shaped_w = m.caret_x_by_byte.back();
    switch (rs.text_align) {
        case LabelAlign::center:
            m.local_text_left = (bounds().width - shaped_w) * 0.5f;
            break;
        case LabelAlign::right:
            m.local_text_left = bounds().width - shaped_w;
            break;
        case LabelAlign::left:
        case LabelAlign::justify:
        case LabelAlign::auto_:        // resolved to left in resolve_text_style()
        case LabelAlign::match_parent: // resolved above
        default:
            m.local_text_left = 0.0f;
            break;
    }

    // Band aligned to the TOP of the text (where top-aligned label text
    // renders), matching the overlay's existing top-anchored band. The
    // ascent top sits ~0.85*font_size above the baseline.
    m.local_band_y = rs.baseline_y - rs.font_size * 0.85f;
    m.band_height = rs.font_size * 1.3f;
    return m;
}

LabelAlign Label::resolve_effective_align_() {
    // text-align cascade. Own value wins, otherwise inherited.
    LabelAlign effective = text_align_;
    if (!has_own_text_align_) {
        if (auto inh = inheritable_text_align(); inh.has_value()) {
            int v = inh.value();
            if (v == 1) effective = LabelAlign::center;
            else if (v == 2) effective = LabelAlign::right;
            else if (v == 3) effective = LabelAlign::auto_;
            else if (v == 4) effective = LabelAlign::justify;
            else if (v == 5) effective = LabelAlign::match_parent;
            else effective = LabelAlign::left;
        }
    }
    // Resolve `match-parent` at paint time: the computed value matches the
    // parent's resolved text-align. Walk the ancestor chain manually (skipping
    // intermediate match-parent ancestors); fall back to `left` (CSS default)
    // if no ancestor sets a concrete value.
    if (effective == LabelAlign::match_parent) {
        LabelAlign parent_resolved = LabelAlign::left;
        for (auto* anc = parent(); anc != nullptr; anc = anc->parent()) {
            auto inh = anc->inheritable_text_align();
            if (!inh.has_value()) continue;
            int v = inh.value();
            if (v == 5) continue;  // intermediate match-parent — keep walking
            if      (v == 1) parent_resolved = LabelAlign::center;
            else if (v == 2) parent_resolved = LabelAlign::right;
            else if (v == 3) parent_resolved = LabelAlign::auto_;
            else if (v == 4) parent_resolved = LabelAlign::justify;
            else             parent_resolved = LabelAlign::left;
            break;
        }
        effective = parent_resolved;
    }
    // `auto` is writing-direction-relative; Pulp is LTR-only for now, so
    // `auto` degrades to `left`.
    if (effective == LabelAlign::auto_) effective = LabelAlign::left;
    return effective;
}

void Label::paint_attributed_(canvas::Canvas& canvas) {
    const auto& spans = attributed_runs_.spans();
    if (spans.empty()) return;
    // Vertical-center the single line within the box (close to the default
    // single-line label band). Each span paints with its own font + color,
    // advancing x by the span's measured width — the native equivalent of the
    // web-compat nested <span> path.
    const float fs = spans.front().font_size;
    const float baseline = bounds().height * 0.5f + fs * 0.32f;
    // Each span is drawn left-anchored at an explicit x, so honor text-align
    // by shifting the STARTING x rather than the canvas anchor (which only
    // positions a single draw call). Measure the full run first.
    canvas.set_text_align(canvas::TextAlign::left);
    float total = 0.0f;
    for (const auto& s : spans) {
        const std::string fam = s.font_family.empty() ? std::string("Inter") : s.font_family;
        canvas.set_font_full(fam, s.font_size, s.font_weight, s.italic ? 1 : 0, s.letter_spacing);
        total += canvas.measure_text(s.text);
    }
    float x = 0.0f;
    switch (resolve_effective_align_()) {
        case LabelAlign::center: x = (bounds().width - total) * 0.5f; break;
        case LabelAlign::right:  x = bounds().width - total; break;
        default: break;  // left / justify → leading edge at 0
    }
    for (const auto& s : spans) {
        const std::string fam = s.font_family.empty() ? std::string("Inter") : s.font_family;
        canvas.set_font_full(fam, s.font_size, s.font_weight, s.italic ? 1 : 0, s.letter_spacing);
        canvas.set_fill_color({s.color.r, s.color.g, s.color.b, s.color.a});
        canvas.fill_text(s.text, x, baseline);
        x += canvas.measure_text(s.text);
    }
}

void Label::paint(canvas::Canvas& canvas) {
    // While the inline editor is open it IS the label's visible surface; the
    // static text underneath would show through the field's own background
    // wherever that background is transparent.
    if (editor_ != nullptr) return;
    if (text_.empty()) return;
    // Per-range styled (mixed) text on a single line lowers to the span draw;
    // multi-line / unstyled falls through to the single-style path below.
    if (has_attributed_ && !multi_line_) { paint_attributed_(canvas); return; }

    // CSS-style typography cascade. For each property:
    //   1. Use the Label's own value if explicitly set.
    //   2. Otherwise walk up the parent chain via View::inheritable_*().
    //   3. Otherwise fall back to the existing theme/default behavior.
    canvas::Color text_color;
    if (has_own_text_color_) {
        text_color = text_color_;
    } else if (auto inherited = inheritable_text_color(); inherited.has_value()) {
        text_color = inherited.value();
    } else {
        text_color = resolve_color("text.primary", canvas::Color::rgba8(200, 200, 200));
    }
    canvas.set_fill_color({text_color.r, text_color.g, text_color.b, text_color.a});

    float effective_font_size = font_size_;
    if (!has_own_font_size_) {
        if (auto inh = inheritable_font_size(); inh.has_value())
            effective_font_size = inh.value();
    }
    int effective_font_weight = font_weight_;
    if (!has_own_font_weight_) {
        if (auto inh = inheritable_font_weight(); inh.has_value())
            effective_font_weight = inh.value();
    }
    float effective_letter_spacing = letter_spacing_;
    if (!has_own_letter_spacing_) {
        if (auto inh = inheritable_letter_spacing(); inh.has_value())
            effective_letter_spacing = inh.value();
    }

    // Propagate setFontFamily / setFontWeight / setLetterSpacing through to
    // the canvas backend so JS calls actually change rasterised glyphs. Empty
    // font_family_ falls back to the default theme face.
    const std::string& family = font_family_.empty() ? std::string("Inter") : font_family_;
    canvas.set_font_full(family, effective_font_size, effective_font_weight,
                          font_style_, effective_letter_spacing);

    // Apply the CSS font-variant CSV as SkShaper OpenType feature tags.
    // Factored into apply_font_features() so text_edit_metrics() shapes the
    // caret with the IDENTICAL features (WYSIWYG caret invariant).
    apply_font_features(canvas);

    // Apply text-transform
    std::string display_text = text_;
    if (text_transform_ == TextTransform::uppercase) {
        for (auto& ch : display_text) ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
    } else if (text_transform_ == TextTransform::lowercase) {
        for (auto& ch : display_text) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    } else if (text_transform_ == TextTransform::capitalize) {
        bool cap_next = true;
        for (auto& ch : display_text) {
            if (cap_next && std::isalpha(static_cast<unsigned char>(ch))) {
                ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
                cap_next = false;
            }
            if (ch == ' ') cap_next = true;
        }
    }

    // Vertical text direction — rotate canvas for top-to-bottom / bottom-to-top
    bool vertical = (text_direction_ == canvas::TextDirection::top_to_bottom ||
                     text_direction_ == canvas::TextDirection::bottom_to_top);
    if (vertical) {
        canvas.save();
        if (text_direction_ == canvas::TextDirection::top_to_bottom) {
            canvas.translate(bounds().width * 0.5f + effective_font_size * 0.35f, 0);
            canvas.rotate(3.14159265f / 2.0f);
        } else {
            canvas.translate(bounds().width * 0.5f - effective_font_size * 0.35f, bounds().height);
            canvas.rotate(-3.14159265f / 2.0f);
        }
    }

    // Vertical alignment
    // Match intrinsic_height's small-font-size bump so multi-line /
    // shaper-wrapped layout uses the same line-box height that Yoga reserved.
    // Without matching here, an fs=10 multi-line label would size to 16px
    // boxes but paint at 14px line height and siblings would not align.
    const float lh_mult = effective_font_size < 12.0f ? 1.6f : 1.4f;
    float lh = line_height_ > 0 ? line_height_ : effective_font_size * lh_mult;

    // CSS `white-space` / `overflow-wrap` / `word-break` soft-wrap path.
    // Route any multi-line, bounded-width Label through TextShaper so CSS
    // default `white-space: normal` soft-wraps at word boundaries
    // (whitespace), and `break-word` / `anywhere` additionally split inside
    // over-wide words.
    //
    // Default `normal` mode also routes through TextShaper, so Labels without
    // an explicit word-break still soft-wrap at whitespace. TextShaper's
    // `BreakMode::normal` preserves whole-word overflow for a single unbroken
    // word (pinned by the BreakMode::normal cases in test_text_shaper.cpp),
    // which keeps Label in line with the CSS spec without regressing the
    // single-long-word case.
    //
    // The shaped layout is computed ONCE here and reused both for the
    // vertical-align metrics (source_lines / text_h) and the rendering
    // loop further down. This avoids a double-shape and keeps line-clamp /
    // ellipsis / vertical-align integration coherent across modes.
    const std::string wb = word_break();
    canvas::BreakMode break_mode = canvas::BreakMode::normal;
    if      (wb == "break-word") break_mode = canvas::BreakMode::break_word;
    else if (wb == "anywhere")   break_mode = canvas::BreakMode::anywhere;
    const bool use_shaper_wrap =
        multi_line_ &&
        bounds().width > 0.0f;

    // Reuse the cached shaped layout when nothing the shaper depends on has
    // changed, so paint() avoids re-running prepare() + layout_with_lines()
    // (which allocate a PreparedText and per-line strings) every frame inside
    // View::paint_all's no-alloc region. The key captures every shaper input:
    // display_text, the resolved family/size, the wrap width (bounds().width,
    // which changes on every resize), the resolved line height, and the break
    // mode. max_lines is always 0 here. On a hit the layout is read in place
    // (no copy, no re-shape); the output is byte-identical to a recompute.
    const canvas::ShapedLayout* shaped_layout = nullptr;
    if (use_shaper_wrap) {
        // font_registration_generation() is part of the key because the
        // shaper's measure_segment() resamples the resolved typeface (and thus
        // glyph advances → wrap points) whenever a font is (re)registered —
        // e.g. an async register_font_url() completing after the first paint.
        // Without it a Label that first shaped against the fallback face would
        // serve that stale wrap until some other key field happened to change.
        ShapedLayoutKey key{display_text, family, effective_font_size,
                            bounds().width, lh, static_cast<int>(break_mode),
                            canvas::font_registration_generation()};
        if (!shaped_cache_valid_ || !(shaped_cache_key_ == key)) {
            auto& shaper = canvas::global_text_shaper();
            auto prepared = shaper.prepare(display_text, family, effective_font_size);
            shaped_cache_layout_ = shaper.layout_with_lines(
                prepared, bounds().width, lh, /*max_lines=*/0, break_mode);
            shaped_cache_key_ = std::move(key);
            shaped_cache_valid_ = true;
        }
        shaped_layout = &shaped_cache_layout_;
    }

    // When line-clamp drops source lines, the painted block height must
    // reflect the visible line count, not the full newline count. Otherwise
    // vertical-align: center / bottom positions the block as if the hidden
    // lines were still rendered, leaving the visible lines offset upward.
    //
    // `source_lines` for the soft-wrap path is the shaped layout's line count
    // (which already accounts for inside-word breaks). For the legacy path it
    // stays count('\n') + 1.
    //
    // Drop a trailing newline before counting in the legacy path. The
    // split-and-emit loop below stops once `pos == display_text.size()`, so a
    // trailing `\n` doesn't actually paint an extra line — but feeding the
    // inflated count into `text_h` would mis-position vertical-align:
    // center/bottom. The shaper path doesn't need a fix here because
    // shaped_layout.line_count is the count the shaper actually produced.
    int source_lines = multi_line_
        ? (use_shaper_wrap
               ? std::max(1, shaped_layout->line_count)
               : static_cast<int>(std::count(display_text.begin(),
                                             display_text.end(), '\n')) + 1
                 - (!display_text.empty() && display_text.back() == '\n' ? 1 : 0))
        : 1;
    int visible_lines = source_lines;
    if (multi_line_ && line_clamp_ > 0 && line_clamp_ < source_lines)
        visible_lines = line_clamp_;
    float text_h = multi_line_ ? lh * static_cast<float>(visible_lines) : effective_font_size;
    float baseline_y;
    switch (vertical_align_) {
        case canvas::TextVerticalAlign::top:
            baseline_y = effective_font_size * 0.85f;
            break;
        case canvas::TextVerticalAlign::bottom:
            baseline_y = bounds().height - text_h + effective_font_size * 0.85f;
            break;
        case canvas::TextVerticalAlign::baseline:
            baseline_y = bounds().height * 0.75f;
            break;
        case canvas::TextVerticalAlign::center:
        default:
            // Center the visible block within bounds, then offset to the
            // first line's baseline. For single-line this collapses to
            // bounds.h/2 + 0.35*font_size (the historic formula) because
            // text_h == effective_font_size and 0.85 - 0.5 == 0.35.
            baseline_y = (bounds().height - text_h) * 0.5f + effective_font_size * 0.85f;
            break;
    }

    // text-align cascade with `auto` + `match-parent` fully resolved. Shared
    // with paint_attributed_() so per-range styled text honors the same
    // alignment.
    const LabelAlign effective_text_align = resolve_effective_align_();

    float x = 0;
    switch (effective_text_align) {
        case LabelAlign::left:
        case LabelAlign::auto_:         // unreachable — resolved above; keeps switch exhaustive
        case LabelAlign::match_parent:  // unreachable — resolved above; keeps switch exhaustive
            canvas.set_text_align(canvas::TextAlign::left);
            break;
        case LabelAlign::center:
            canvas.set_text_align(canvas::TextAlign::center);
            x = bounds().width * 0.5f;
            break;
        case LabelAlign::right:
            canvas.set_text_align(canvas::TextAlign::right);
            x = bounds().width;
            break;
        case LabelAlign::justify:
            // Emit canvas TextAlign::justify so backends that wire
            // SkParagraph kJustify can render true justified text.
            // RecordingCanvas / CG fall back to left-alignment semantics (no
            // kerning-controlled space distribution). Anchor x at 0 so the
            // first line's leading edge matches a left-aligned label.
            canvas.set_text_align(canvas::TextAlign::justify);
            break;
    }

    // Track the actually-painted single-line string so the decoration block
    // below measures the truncated text, not the original. Multi-line keeps
    // using display_text since it paints the full string across multiple draw
    // calls.
    std::string draw_text = display_text;
    if (!multi_line_) {
        // CSS `text-overflow: ellipsis`. Truncate with U+2026 when the
        // measured text exceeds the content-box, regardless of text-align
        // (CSS truncates at the trailing edge for all three). UTF-8-safe via
        // codepoint binary-search in truncate_to_width().
        if (text_overflow_ellipsis())
            draw_text = truncate_to_width(canvas, display_text, bounds().width);
        canvas.fill_text(draw_text, x, baseline_y);
    } else {
        // CSS `line-clamp` / `-webkit-line-clamp`. When the clamp count is
        // set and the text would emit more lines than allowed, paint at most N
        // lines and append the U+2026 ellipsis to the last visible line if any
        // source lines were dropped. 0 disables clamping (matches CSS spec;
        // spec uses `none`). visible_lines / source_lines / text_h are
        // computed earlier so vertical-align positioning reflects the clamped
        // block height.
        const bool need_ellipsis = (line_clamp_ > 0 && source_lines > line_clamp_);

        // Start from the clamped baseline so the first visible line
        // sits at the top of the centered/bottom-aligned block.
        float y = baseline_y;
        int emitted = 0;

        if (use_shaper_wrap) {
            // Shaped-layout iteration path. TextShaper already split
            // display_text into shaped_layout.lines using the active
            // BreakMode (break-word / anywhere). Iterate those lines instead
            // of `\n`-splitting. Line-clamp + ellipsis logic is identical,
            // driven by source_lines (= shaped_layout.line_count) and
            // visible_lines.
            for (const auto& shaped_line : shaped_layout->lines) {
                if (emitted >= visible_lines) break;
                std::string line = shaped_line.text;
                if (need_ellipsis && (emitted + 1 == visible_lines)) {
                    line.append("\xe2\x80\xa6");
                }
                canvas.fill_text(line, x, y);
                y += lh;
                ++emitted;
            }
        } else {
            // Legacy `\n`-only split path. Used when bounds().width is 0 /
            // unbounded (the shaper has no max_width to break against).
            // Existing consumers see exactly the previous behavior in that
            // case.
            size_t pos = 0;
            while (pos < display_text.size()) {
                if (emitted >= visible_lines) break;
                size_t nl = display_text.find('\n', pos);
                if (nl == std::string::npos) nl = display_text.size();
                std::string line = display_text.substr(pos, nl - pos);
                // Last visible line under a clamp that truncated source lines:
                // append U+2026 (UTF-8: 0xE2 0x80 0xA6) to signal truncation.
                // Matches the CSS "block-axis ellipsis" intent for
                // line-clamp. The spec ties this to text-overflow: ellipsis;
                // Label honors that intent without requiring the separate
                // text-overflow keyword to also be set.
                if (need_ellipsis && (emitted + 1 == visible_lines)) {
                    line.append("\xe2\x80\xa6");
                }
                canvas.fill_text(line, x, y);
                y += lh;
                pos = nl + 1;
                ++emitted;
            }
        }
    }

    // Text decoration (underline, line-through, overline)
    if (text_decoration_ != TextDecoration::none) {
        auto dec_color = has_decoration_color_ ? decoration_color_ : text_color;
        canvas.set_stroke_color(dec_color);
        canvas.set_line_width(1.0f);
        // Measure the actually-drawn (possibly truncated) text so a decoration
        // line on an ellipsised label doesn't escape past the visible glyphs.
        float text_w = canvas.measure_text(draw_text);
        float draw_x = x;
        if (effective_text_align == LabelAlign::center) draw_x = x - text_w * 0.5f;
        else if (effective_text_align == LabelAlign::right) draw_x = x - text_w;

        if (text_decoration_ == TextDecoration::underline)
            canvas.stroke_line(draw_x, baseline_y + 2, draw_x + text_w, baseline_y + 2);
        else if (text_decoration_ == TextDecoration::line_through)
            canvas.stroke_line(draw_x, baseline_y - effective_font_size * 0.2f, draw_x + text_w, baseline_y - effective_font_size * 0.2f);
        else if (text_decoration_ == TextDecoration::overline)
            canvas.stroke_line(draw_x, baseline_y - effective_font_size * 0.7f, draw_x + text_w, baseline_y - effective_font_size * 0.7f);
    }

    if (vertical) canvas.restore();

    // Clear font_features at end of paint so subsequent widgets in the same
    // paint pass don't inherit this Label's OpenType state. The canvas keeps
    // font_features as mutable canvas-level state, and TextButton /
    // TextEditor / sibling Labels that call measure_text/fill_text without
    // setting features would otherwise pick up tnum/smcp/etc. from the
    // previous fontVariant-bearing Label, causing unintended typography drift
    // outside that label's box.
    canvas.clear_font_features();
}



// ── Click-to-edit ────────────────────────────────────────────────────────

void Label::on_mouse_down(Point pos) {
    (void)pos;
    if (edit_trigger_ == EditTrigger::none || editor_ != nullptr) return;

    // Double-click detection. The view layer delivers plain downs, so the
    // Label counts them itself against a 400 ms window — the same threshold
    // the platform hosts use for their own double-click synthesis.
    constexpr double kDoubleClickWindow = 0.400;
    const double now = std::chrono::duration<double>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    click_count_ = (now - last_click_time_ <= kDoubleClickWindow) ? click_count_ + 1 : 1;
    last_click_time_ = now;

    const bool armed = (edit_trigger_ == EditTrigger::single_click && click_count_ >= 1) ||
                       (edit_trigger_ == EditTrigger::double_click && click_count_ >= 2);
    if (armed) show_editor();
}

void Label::show_editor() {
    if (editor_ != nullptr) return;

    auto owned = std::make_unique<TextEditor>();
    editor_ = owned.get();
    editor_->set_bounds({0, 0, local_bounds().width, local_bounds().height});
    editor_->set_text(text_);
    editor_->set_font_size(font_size_);
    editor_->select_on_focus = true;
    editor_->multi_line = false;

    // Carry the Label's alignment into the field so the text does not jump
    // sideways at the moment the editor opens — the single most visible tell
    // that an inline rename is not really "in place".
    switch (text_align_) {
        case LabelAlign::center: editor_->set_text_align(canvas::TextAlign::center); break;
        case LabelAlign::right:  editor_->set_text_align(canvas::TextAlign::right);  break;
        default:                 editor_->set_text_align(canvas::TextAlign::left);   break;
    }

    editor_->on_return = [this](const std::string&) { hide_editor(/*commit=*/true); };
    editor_->on_escape = [this] { hide_editor(/*commit=*/false); };
    editor_->on_focus_lost = [this](const std::string&) { hide_editor(/*commit=*/true); };

    add_child(std::move(owned));
    editor_->set_focus(true);
    editor_->on_focus_changed(true);
    click_count_ = 0;
    if (on_editor_show) on_editor_show();
    request_repaint();
}

void Label::hide_editor(bool commit) {
    if (editor_ == nullptr) return;

    // Detach FIRST, then run the callbacks. A commit handler is entitled to
    // delete the Label's owner (a rename that closes the row it renamed), and
    // it must not do that while we still hold a raw pointer into our own child
    // vector. `keep_alive` also holds the editor past its own on_focus_lost —
    // which is what is calling us — so the callback's stack frame stays valid.
    TextEditor* ed = editor_;
    editor_ = nullptr;
    const std::string new_text = ed->text();
    auto keep_alive = remove_child(ed);

    if (commit && new_text != text_) {
        set_text(new_text);
        if (on_text_change) on_text_change(new_text);
    }
    if (on_editor_hide) on_editor_hide();
    request_repaint();
}

} // namespace pulp::view
