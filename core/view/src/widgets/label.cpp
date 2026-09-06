#include <pulp/view/widgets.hpp>
#include <pulp/view/text_editor.hpp>
#include <pulp/view/animation.hpp>
#include <pulp/view/frame_clock.hpp>
#include <pulp/view/image_cache.hpp>
#include <pulp/view/text_overflow.hpp>
#include <pulp/view/window_host.hpp>
#include <pulp/canvas/font_resolver.hpp>
#include <pulp/canvas/text_shaper.hpp>
#include <pulp/canvas/bundled_fonts.hpp>  // font_registration_generation() for the shaped-layout cache key
#include <choc/text/choc_JSON.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <limits>
#include <string>

namespace pulp::view {

// ── Label ────────────────────────────────────────────────────────────────────

namespace {
/// Not atomic: every caller is the paint/layout thread, and a counter that
/// pretended to be thread-safe would invite use from somewhere it is not.
Label::LineBreakPathCounts& line_break_counts() {
    static Label::LineBreakPathCounts counts;
    return counts;
}

canvas::AttributedString transformed_attributed_string(
    const canvas::AttributedString& source, Label::TextTransform transform) {
    canvas::AttributedString result;
    bool capitalize_next = true;
    for (const auto& original : source.spans()) {
        auto span = original;
        for (auto& ch : span.text) {
            const auto value = static_cast<unsigned char>(ch);
            if (transform == Label::TextTransform::uppercase) {
                ch = static_cast<char>(std::toupper(value));
            } else if (transform == Label::TextTransform::lowercase) {
                ch = static_cast<char>(std::tolower(value));
            } else if (transform == Label::TextTransform::capitalize) {
                if (capitalize_next && std::isalpha(value)) {
                    ch = static_cast<char>(std::toupper(value));
                    capitalize_next = false;
                }
                if (std::isspace(value)) capitalize_next = true;
            }
        }
        result.append(std::move(span));
    }
    return result;
}

float fallback_attributed_ascent(const canvas::AttributedString& text,
                                 float default_font_size) {
    float largest_font_size = default_font_size;
    for (const auto& span : text.spans())
        largest_font_size = std::max(largest_font_size, span.font_size);
    return largest_font_size * 0.85f;
}
}  // namespace

Label::LineBreakPathCounts Label::line_break_path_counts() {
    return line_break_counts();
}

void Label::reset_line_break_path_counts() { line_break_counts() = {}; }

canvas::AttributedString Label::resolved_attributed_string() const {
    auto transformed = transformed_attributed_string(attributed_runs_, text_transform_);
    canvas::AttributedString resolved;
    const auto text_style = resolve_text_style();
    canvas::Color color;
    if (has_own_text_color_) color = text_color_;
    else if (auto inherited = inheritable_text_color(); inherited.has_value())
        color = inherited.value();
    else color = resolve_color("text.primary", canvas::Color::rgba8(200, 200, 200));

    for (auto span : transformed.spans()) {
        if (span.inherit_font_family) span.font_family = text_style.family;
        if (span.inherit_font_size) span.font_size = text_style.font_size;
        if (span.inherit_font_weight) span.font_weight = text_style.font_weight;
        if (span.inherit_font_slant) {
            span.font_slant = text_style.font_slant;
            span.italic = span.font_slant != 0;
        }
        if (span.inherit_color) span.color = color;
        if (span.inherit_letter_spacing)
            span.letter_spacing = text_style.letter_spacing;
        resolved.append(std::move(span));
    }
    return resolved;
}

void Label::set_cached_line_boxes(std::vector<CachedLineBox> boxes,
                                  float basis_width, std::string basis_face,
                                  bool wrap_on_cache_miss) {
    // Cache acceptance and responsive fallback are separate contracts. A
    // missing platform face or malformed capture must reject the boxes, but a
    // caller that observed wrappable browser text still needs normal reflow.
    captured_wrap_fallback_ = wrap_on_cache_miss;
    if (boxes.empty() || boxes.size() > 4096 ||
        !std::isfinite(basis_width) || basis_width <= 0.0f ||
        basis_face.empty())
        return;
    int64_t previous_end = 0;
    for (const auto& box : boxes) {
        const auto start = static_cast<int64_t>(box.start);
        const auto length = static_cast<int64_t>(box.length);
        if (!std::isfinite(box.left) || !std::isfinite(box.top) ||
            !std::isfinite(box.width) || !std::isfinite(box.height) ||
            box.width < 0.0f || box.height <= 0.0f ||
            !canvas::is_valid_utf16_scalar_range(
                text_, start, length, previous_end))
            return;
        previous_end = start + length;
    }
    cached_line_boxes_ = std::move(boxes);
    cached_line_basis_width_ = basis_width;
    cached_line_basis_face_ = std::move(basis_face);
    cached_line_basis_text_ = apply_text_transform(text_);
    cached_line_basis_font_variant_ = font_variant();
    const auto resolved = resolve_text_style();
    cached_line_basis_font_size_ = resolved.font_size;
    cached_line_basis_font_weight_ = resolved.font_weight;
    cached_line_basis_font_style_ = resolved.font_slant;
    cached_line_basis_letter_spacing_ = resolved.letter_spacing;
    shaped_cache_valid_ = false;
}

/// Slice a UTF-8 string by a UTF-16 offset and length.
///
/// Captured line offsets count UTF-16 code units, because that is how the
/// browser protocol indexes strings, while the text is UTF-8. Treating one as
/// the other splits a multi-byte sequence the moment a paragraph contains a
/// dash, a curly apostrophe or a multiplication sign, and emits bytes that are
/// not valid UTF-8 at all — not merely an off-by-one.
static std::string utf16_slice(const std::string& text, int start, int length) {
    if (start < 0 || length <= 0) return {};
    const auto begin = static_cast<size_t>(start);
    const auto count = static_cast<size_t>(length);
    size_t units = 0;
    size_t byte_begin = std::string::npos;
    size_t i = 0;
    while (i <= text.size()) {
        if (units == begin && byte_begin == std::string::npos) byte_begin = i;
        if (byte_begin != std::string::npos && units == begin + count)
            return text.substr(byte_begin, i - byte_begin);
        if (i >= text.size()) break;
        const auto lead = static_cast<unsigned char>(text[i]);
        size_t bytes = 1;
        if      ((lead & 0x80u) == 0x00u) bytes = 1;
        else if ((lead & 0xE0u) == 0xC0u) bytes = 2;
        else if ((lead & 0xF0u) == 0xE0u) bytes = 3;
        else if ((lead & 0xF8u) == 0xF0u) bytes = 4;
        units += (bytes == 4) ? 2 : 1;  // astral codepoints are a surrogate pair
        i += bytes;
    }
    if (byte_begin == std::string::npos) return {};
    return text.substr(byte_begin);
}

static bool same_attributed_source(const canvas::AttributedString& a,
                                   const canvas::AttributedString& b) {
    const auto& lhs = a.spans();
    const auto& rhs = b.spans();
    if (lhs.size() != rhs.size()) return false;
    for (std::size_t i = 0; i < lhs.size(); ++i) {
        const auto& x = lhs[i];
        const auto& y = rhs[i];
        if (x.text != y.text || x.font_family != y.font_family ||
            x.font_size != y.font_size || x.font_weight != y.font_weight ||
            x.italic != y.italic || x.font_slant != y.font_slant ||
            x.color != y.color || x.decoration != y.decoration ||
            x.decoration_override != y.decoration_override ||
            x.decoration_color != y.decoration_color ||
            x.letter_spacing != y.letter_spacing ||
            x.inherit_font_family != y.inherit_font_family ||
            x.inherit_font_size != y.inherit_font_size ||
            x.inherit_font_weight != y.inherit_font_weight ||
            x.inherit_font_slant != y.inherit_font_slant ||
            x.inherit_color != y.inherit_color ||
            x.inherit_letter_spacing != y.inherit_letter_spacing)
            return false;
    }
    return true;
}

bool Label::cached_line_layout_usable(
    const std::string& display_text, float effective_font_size,
    float effective_letter_spacing, float layout_width) const {
    if (cached_line_boxes_.empty()) return false;

    if (display_text != cached_line_basis_text_ ||
        font_variant() != cached_line_basis_font_variant_ ||
        std::abs(effective_font_size - cached_line_basis_font_size_) > 0.001f ||
        effective_font_weight() != cached_line_basis_font_weight_ ||
        font_style_ != cached_line_basis_font_style_ ||
        std::abs(effective_letter_spacing -
                 cached_line_basis_letter_spacing_) > 0.001f)
        return false;

    // 1. The face. Not the requested family — a family is a REQUEST, and the
    //    same request resolves to different faces on different machines or
    //    after a register_font. An empty basis face means the capture recorded
    //    none, which is unverifiable and therefore unusable.
    if (cached_line_basis_face_.empty()) return false;
    const auto resolved_style = resolve_text_style();
    const std::string& family = resolved_style.family;
    const auto slant = resolved_style.font_slant == 2 ? canvas::FontSlant::Oblique
                     : resolved_style.font_slant == 1 ? canvas::FontSlant::Italic
                                        : canvas::FontSlant::Normal;
    if (canvas::resolved_face_identity(
            family, static_cast<float>(effective_font_weight()), slant) !=
        cached_line_basis_face_) {
        return false;
    }
    // A browser capture records one dominant resolved face. Reusing those
    // breaks is only sound when every attributed span resolves to that same
    // face; otherwise a secondary family may have different advances or may
    // register asynchronously after the capture.
    if (has_attributed_) {
        for (const auto& span : attributed_runs_.spans()) {
            const int span_slant_value = span.font_slant != 0
                ? span.font_slant : (span.italic ? 1 : 0);
            const auto span_slant = span_slant_value == 2
                ? canvas::FontSlant::Oblique
                : span_slant_value == 1 ? canvas::FontSlant::Italic
                                        : canvas::FontSlant::Normal;
            if (canvas::resolved_face_identity(
                    span.font_family.empty() ? family : span.font_family,
                    static_cast<float>(span.font_weight), span_slant) !=
                cached_line_basis_face_) {
                return false;
            }
        }
    }

    // 2. The width the text was broken at. A break is a function of the box,
    //    so a box of a different width has different breaks — including the
    //    auto-width case this exists for, where the box was sized BY the text.
    const bool exact_basis_width =
        std::abs(layout_width - cached_line_basis_width_) <= 0.5f;
    const bool yoga_pixel_rounded_basis_width =
        std::abs(layout_width - std::ceil(cached_line_basis_width_)) <= 0.001f;
    if (!exact_basis_width && !yoga_pixel_rounded_basis_width) return false;

    // 3. The text itself. Offsets index the string the capture broke, so any
    //    edit — a translation, a text-transform, a bound value — makes every
    //    slice below name the wrong characters rather than merely re-breaking.
    int units = 0;
    for (size_t i = 0; i < display_text.size();) {
        const auto lead = static_cast<unsigned char>(display_text[i]);
        size_t bytes = 1;
        if      ((lead & 0xE0u) == 0xC0u) bytes = 2;
        else if ((lead & 0xF0u) == 0xE0u) bytes = 3;
        else if ((lead & 0xF8u) == 0xF0u) bytes = 4;
        units += (bytes == 4) ? 2 : 1;
        i += bytes;
    }
    for (const auto& box : cached_line_boxes_) {
        if (box.start < 0 || box.length <= 0) return false;
        if (box.length > units || box.start > units - box.length) return false;
        if (!canvas::is_utf16_scalar_boundary(
                display_text, static_cast<std::size_t>(box.start)) ||
            !canvas::is_utf16_scalar_boundary(
                display_text,
                static_cast<std::size_t>(box.start + box.length)))
            return false;
    }
    // Deliberately NOT invalidated on: font SIZE and letter-spacing. Both are
    // already folded into the resolved advances the browser broke with, and a
    // Label whose size or tracking changed after import has a different box
    // too, which condition 2 catches. Size is not tracked separately because
    // doing so without tracking every other advance input would look like a
    // completeness this check does not have.
    return true;
}

canvas::ShapedLayout Label::layout_from_cached_lines(
    const std::string& text, float line_height) const {
    canvas::ShapedLayout layout;
    layout.lines.reserve(cached_line_boxes_.size());
    for (const auto& box : cached_line_boxes_) {
        canvas::ShapedLayout::Line line;
        line.text = utf16_slice(text, box.start, box.length);
        line.width = box.width;
        line.x_offset = box.left;
        // Chromium's line rectangle is the layout evidence.  In particular,
        // its top is relative to the owning element's content box and is not
        // necessarily the value native vertical centering would derive from
        // font metrics.  Dropping it shifted button captions/icons and made
        // compact footer labels look clipped even when the captured face and
        // width were valid.
        line.y = box.top;
        line.height = box.height;
        line.first_segment = 0;
        line.segment_count = 0;
        layout.total_width = std::max(layout.total_width, line.width);
        layout.lines.push_back(std::move(line));
    }
    layout.line_count = static_cast<int>(layout.lines.size());
    layout.total_height = line_height * static_cast<float>(layout.line_count);
    return layout;
}

canvas::ShapedLayout Label::layout_attributed_from_cached_lines(
    const canvas::AttributedString& text, float line_height) const {
    canvas::ShapedLayout layout;
    layout.lines.reserve(cached_line_boxes_.size());
    for (const auto& box : cached_line_boxes_) {
        canvas::ShapedLayout::Line line;
        line.width = box.width;
        line.x_offset = box.left;
        line.y = box.top;
        line.height = box.height;

        int span_start = 0;
        const int line_start = box.start;
        const int line_end = box.start + box.length;
        for (std::size_t index = 0; index < text.spans().size(); ++index) {
            const auto& span = text.spans()[index];
            int span_units = 0;
            for (std::size_t i = 0; i < span.text.size();) {
                const auto lead = static_cast<unsigned char>(span.text[i]);
                std::size_t bytes = 1;
                if      ((lead & 0xE0u) == 0xC0u) bytes = 2;
                else if ((lead & 0xF0u) == 0xE0u) bytes = 3;
                else if ((lead & 0xF8u) == 0xF0u) bytes = 4;
                span_units += bytes == 4 ? 2 : 1;
                i += bytes;
            }
            const int span_end = span_start + span_units;
            const int clipped_start = std::max(line_start, span_start);
            const int clipped_end = std::min(line_end, span_end);
            if (clipped_end > clipped_start) {
                auto fragment = utf16_slice(
                    span.text, clipped_start - span_start,
                    clipped_end - clipped_start);
                line.text += fragment;
                line.fragments.push_back({std::move(fragment), 0.0f,
                                          static_cast<int>(index)});
            }
            span_start = span_end;
        }
        layout.total_width = std::max(layout.total_width, line.width);
        layout.lines.push_back(std::move(line));
    }
    layout.line_count = static_cast<int>(layout.lines.size());
    layout.total_height = line_height * static_cast<float>(layout.line_count);
    return layout;
}

int Label::effective_font_weight() const {
    if (has_own_font_weight_) return font_weight_;
    if (auto inherited = inheritable_font_weight(); inherited.has_value())
        return inherited.value();
    return font_weight_;
}

std::string Label::effective_font_family() const {
    if (!font_family_.empty()) return font_family_;
    if (auto inherited = inheritable_font_family(); inherited.has_value())
        if (!inherited.value().empty()) return inherited.value();
    return "Inter";
}

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
    std::string effective_family = effective_font_family();

    auto& shaper = canvas::global_text_shaper();
    auto resolved_attributed = has_attributed_
        ? resolved_attributed_string() : canvas::AttributedString{};
    auto prepared = has_attributed_
        ? shaper.prepare(resolved_attributed, resolved_font_features())
        : shaper.prepare(text_.empty() ? std::string(" ") : text_,
                         effective_family, effective_font_size,
                         effective_font_weight(), font_style_);
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
    if ((!multi_line_ && !captured_wrap_fallback_) || text_.empty() ||
        available_width <= 0.0f)
        return intrinsic_height();

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
    // Match intrinsic_height's small-font multiplier so the measured line
    // height is consistent with what paint() draws.
    const float lh_mult = effective_font_size < 12.0f ? 1.6f : 1.4f;
    float auto_lh = effective_font_size * lh_mult;
    if (has_attributed_) {
        auto& shaper = canvas::global_text_shaper();
        auto attributed = shaper.prepare(resolved_attributed_string(),
                                          resolved_font_features());
        if (attributed.line_height() > 0) auto_lh = attributed.line_height();
    }
    const float lh = line_height_ > 0 ? line_height_ : auto_lh;

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

    if (captured_wrap_fallback_ &&
        cached_line_layout_usable(display_text, effective_font_size,
                                  effective_letter_spacing, available_width)) {
        int line_count = static_cast<int>(cached_line_boxes_.size());
        if (line_clamp_ > 0 && line_clamp_ < line_count)
            line_count = line_clamp_;
        return std::ceil(lh * static_cast<float>(std::max(1, line_count)));
    }

    std::string family = effective_font_family();
    auto& shaper = canvas::global_text_shaper();
    auto prepared = has_attributed_
        ? shaper.prepare(resolved_attributed_string(), resolved_font_features())
        : shaper.prepare(display_text, family, effective_font_size,
                         effective_font_weight(), font_style_,
                         effective_letter_spacing, resolved_font_features());

    // Use the same break_mode paint uses (CSS word-break / overflow-wrap;
    // Label paint reads `View::word_break()` at draw time, the measure path
    // mirrors that decision).
    const std::string wb = word_break();
    canvas::BreakMode break_mode = canvas::BreakMode::normal;
    if      (wb == "break-word") break_mode = canvas::BreakMode::break_word;
    else if (wb == "anywhere")   break_mode = canvas::BreakMode::anywhere;
    const float shaping_line_height = has_attributed_ && line_height_ <= 0.0f
        ? 0.0f : lh;
    auto layout = shaper.layout(prepared, available_width, shaping_line_height,
                                /*max_lines=*/0, break_mode);

    int line_count = std::max(1, layout.line_count);
    if (line_clamp_ > 0 && line_clamp_ < line_count)
        line_count = line_clamp_;
    if (line_count == layout.line_count)
        return std::ceil(layout.total_height);
    float visible_height = 0.0f;
    for (int i = 0; i < line_count; ++i)
        visible_height += layout.lines[static_cast<std::size_t>(i)].height;
    return std::ceil(visible_height);
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

    std::string effective_family = effective_font_family();

    // Skia's SkFontMetrics-derived ascent (PreparedText::ascent() flips
    // SkFontMetrics::fAscent positive). The painter computes baseline_y
    // from the same prepared metrics — see paint() —
    // so what Yoga sees here matches where the glyphs actually land.
    // For a Label with no text we still need a sensible baseline so a
    // baseline-aligned row of widgets (some text, some not) doesn't
    // collapse — feed the shaper a single space to pin the metric.
    auto& shaper = canvas::global_text_shaper();
    auto resolved_attributed = has_attributed_
        ? resolved_attributed_string() : canvas::AttributedString{};
    auto prepared = has_attributed_
        ? shaper.prepare(resolved_attributed, resolved_font_features())
        : shaper.prepare(text_.empty() ? std::string(" ") : text_,
                         effective_family, effective_font_size,
                         effective_font_weight(), font_style_);
    float ascent = prepared.ascent();
    if (ascent <= 0.0f) {
        // Fallback when shaper metrics aren't real (no Skia, family
        // unresolvable): use the 0.85 × font_size heuristic. Better than
        // returning 0 and collapsing the baseline-aligned row.
        ascent = has_attributed_
            ? fallback_attributed_ascent(resolved_attributed,
                                         effective_font_size)
            : effective_font_size * 0.85f;
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
        // Same automatic line metric as intrinsic_height, including the
        // largest attributed run when a captured span changes font size.
        const float lh_mult = effective_font_size < 12.0f ? 1.6f : 1.4f;
        float auto_lh = effective_font_size * lh_mult;
        if (has_attributed_) {
            auto& shaper = canvas::global_text_shaper();
            auto attributed = shaper.prepare(resolved_attributed_string(),
                                              resolved_font_features());
            if (attributed.line_height() > 0) auto_lh = attributed.line_height();
        }
        return std::ceil(line_height_ > 0 ? line_height_ : auto_lh);
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
    // Measure the Label's actual family, not a hardcoded "Inter". Different
    // families have very different metrics: monospaced fonts such as IBM Plex
    // Mono are materially wider than proportional Inter at the same size, so
    // reserving Inter's width for a face the painter draws wider clips
    // imported labels (for example, "XOVER → lo_freq"). paint() resolves
    // the family through the same effective_font_family().
    std::string effective_family = effective_font_family();

    auto& shaper = canvas::global_text_shaper();
    auto prepared = has_attributed_
        ? shaper.prepare(resolved_attributed_string(), resolved_font_features())
        : shaper.prepare(display_text, effective_family, effective_font_size,
                         effective_font_weight(), font_style_,
                         effective_letter_spacing, resolved_font_features());
    float width = prepared.total_width();

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
    rs.font_weight = effective_font_weight();
    rs.letter_spacing = letter_spacing_;
    if (!has_own_letter_spacing_) {
        if (auto inh = inheritable_letter_spacing(); inh.has_value())
            rs.letter_spacing = inh.value();
    }
    rs.family = effective_font_family();
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
std::vector<canvas::Canvas::FontFeature> Label::resolved_font_features() const {
    const std::string& fv = font_variant();
    std::vector<canvas::Canvas::FontFeature> features;
    if (fv.empty()) return features;
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
    return features;
}

void Label::apply_font_features(canvas::Canvas& canvas) const {
    auto features = resolved_font_features();
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

void Label::paint_decoration_(canvas::Canvas& canvas, float x, float width,
                              float baseline, float font_size,
                              const canvas::Color& color) {
    if (text_decoration_ == TextDecoration::none || width <= 0.0f) return;
    canvas.set_stroke_color(color);
    canvas.set_line_width(1.0f);
    float y = baseline + 2.0f;
    if (text_decoration_ == TextDecoration::line_through)
        y = baseline - font_size * 0.2f;
    else if (text_decoration_ == TextDecoration::overline)
        y = baseline - font_size * 0.7f;
    canvas.stroke_line(x, y, x + width, y);
}

void Label::paint_attributed_lines_(canvas::Canvas& canvas,
                                    const canvas::ShapedLayout& layout,
                                    float baseline_y, float line_height,
                                    int visible_lines,
                                    bool append_ellipsis,
                                    bool single_line_ellipsis,
                                    bool captured_positions) {
    const auto resolved_runs = resolved_attributed_string();
    const auto& spans = resolved_runs.spans();
    if (spans.empty()) return;

    canvas.set_text_align(canvas::TextAlign::left);
    int emitted = 0;
    for (const auto& line : layout.lines) {
        if (emitted >= visible_lines) break;
        struct PaintedFragment {
            const canvas::TextSpan* style = nullptr;
            std::string text;
            float width = 0.0f;
        };
        std::vector<PaintedFragment> source;
        for (const auto& fragment : line.fragments) {
            if (fragment.style_index < 0 ||
                static_cast<std::size_t>(fragment.style_index) >= spans.size())
                continue;
            source.push_back({&spans[static_cast<std::size_t>(fragment.style_index)],
                              fragment.text, 0.0f});
        }

        const float available_width = captured_positions
            ? std::max(0.0f, bounds().width - line.x_offset) : bounds().width;
        const bool needs_ellipsis =
            (append_ellipsis && emitted + 1 == visible_lines) ||
            (single_line_ellipsis && line.width > available_width);
        std::vector<PaintedFragment> painted;
        float painted_line_width = 0.0f;
        for (std::size_t index = 0; index < source.size(); ++index) {
            auto fragment = source[index];
            const auto& style = *fragment.style;
            const std::string family = style.font_family.empty()
                ? std::string("Inter") : style.font_family;
            canvas.set_font_full(family, style.font_size, style.font_weight,
                                 style.font_slant != 0 ? style.font_slant
                                                       : (style.italic ? 1 : 0),
                                 style.letter_spacing);
            const float full_width = canvas.measure_text(fragment.text);
            if (!needs_ellipsis) {
                fragment.width = full_width;
                painted_line_width += full_width;
                painted.push_back(std::move(fragment));
                continue;
            }

            const float remaining = std::max(
                0.0f, available_width - painted_line_width);
            const float ellipsis_width = canvas.measure_text(kEllipsis);
            const bool last = index + 1 == source.size();
            if (!last && full_width + ellipsis_width <= remaining) {
                fragment.width = full_width;
                painted_line_width += full_width;
                painted.push_back(std::move(fragment));
                continue;
            }
            if (last && full_width + ellipsis_width <= remaining) {
                fragment.text.append(kEllipsis);
            } else {
                fragment.text = truncate_to_width(
                    canvas, fragment.text, remaining, true);
            }
            fragment.width = canvas.measure_text(fragment.text);
            painted_line_width += fragment.width;
            painted.push_back(std::move(fragment));
            break;
        }
        if (painted.empty() && needs_ellipsis) {
            const auto& style = spans.back();
            const std::string family = style.font_family.empty()
                ? std::string("Inter") : style.font_family;
            canvas.set_font_full(family, style.font_size, style.font_weight,
                                 style.font_slant != 0 ? style.font_slant
                                                       : (style.italic ? 1 : 0),
                                 style.letter_spacing);
            painted_line_width = canvas.measure_text(kEllipsis);
            painted.push_back({&style, kEllipsis, painted_line_width});
        }

        float x = captured_positions ? line.x_offset : 0.0f;
        if (!captured_positions) {
            switch (resolve_effective_align_()) {
                case LabelAlign::center:
                    x += (bounds().width - painted_line_width) * 0.5f;
                    break;
                case LabelAlign::right:
                    x += bounds().width - painted_line_width;
                    break;
                default:
                    break;
            }
        }

        for (const auto& fragment : painted) {
            const auto& style = *fragment.style;
            const std::string family = style.font_family.empty()
                ? std::string("Inter") : style.font_family;
            canvas.set_font_full(family, style.font_size, style.font_weight,
                                 style.font_slant != 0 ? style.font_slant
                                                       : (style.italic ? 1 : 0),
                                 style.letter_spacing);
            canvas.set_fill_color({style.color.r, style.color.g,
                                   style.color.b, style.color.a});
            const float fragment_baseline = captured_positions
                ? baseline_y + line.y -
                      (layout.lines.empty() ? 0.0f
                                            : layout.lines.front().y)
                : baseline_y + line.y + line.ascent -
                      (layout.lines.empty() ? 0.0f : layout.lines.front().ascent);
            canvas.fill_text(fragment.text, x, fragment_baseline);
            const float painted_width = fragment.width;
            auto paint_span_decoration = [&] {
                if (style.decoration == canvas::TextDecoration::none) return;
                canvas.set_stroke_color(style.color);
                canvas.set_line_width(1.0f);
                float decoration_y = fragment_baseline + 2.0f;
                if (style.decoration == canvas::TextDecoration::strikethrough)
                    decoration_y = fragment_baseline - style.font_size * 0.2f;
                else if (style.decoration == canvas::TextDecoration::overline)
                    decoration_y = fragment_baseline - style.font_size * 0.7f;
                canvas.stroke_line(x, decoration_y,
                                   x + painted_width, decoration_y);
            };
            paint_span_decoration();
            if (!style.decoration_override) {
                const auto base_decoration_color = has_decoration_color_
                    ? decoration_color_ : style.color;
                paint_decoration_(canvas, x, painted_width, fragment_baseline,
                                  style.font_size, base_decoration_color);
            }
            x += painted_width;
        }
        ++emitted;
    }
}

void Label::paint(canvas::Canvas& canvas) {
    // A Label that also has element children draws its own text inside the
    // anonymous inline box the layout pass reserved for it on the flex line.
    // Without that slot the text would start at the same content origin the
    // first child lays out from and the two would overlap.
    if (!has_own_text_box()) {
        paint_text_(canvas, {0.0f, 0.0f, bounds().width, bounds().height});
        return;
    }
    const Rect box = own_text_box();
    canvas.save();
    canvas.translate(box.x, box.y);
    paint_text_(canvas, {0.0f, 0.0f, box.width, box.height});
    canvas.restore();
}

void Label::paint_text_(canvas::Canvas& canvas, Rect text_box) {
    // While the inline editor is open it IS the label's visible surface; the
    // static text underneath would show through the field's own background
    // wherever that background is transparent.
    if (editor_ != nullptr) return;
    if (text_.empty()) return;
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
    const int weight = effective_font_weight();
    float effective_letter_spacing = letter_spacing_;
    if (!has_own_letter_spacing_) {
        if (auto inh = inheritable_letter_spacing(); inh.has_value())
            effective_letter_spacing = inh.value();
    }

    // Propagate setFontFamily / setFontWeight / setLetterSpacing through to
    // the canvas backend so JS calls actually change rasterised glyphs. The
    // family comes from effective_font_family() so the face drawn here is the
    // one intrinsic_width() reserved room for.
    std::string family = effective_font_family();
    canvas.set_font_full(family, effective_font_size, weight,
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
            canvas.translate(text_box.width * 0.5f + effective_font_size * 0.35f, 0);
            canvas.rotate(3.14159265f / 2.0f);
        } else {
            canvas.translate(text_box.width * 0.5f - effective_font_size * 0.35f, text_box.height);
            canvas.rotate(-3.14159265f / 2.0f);
        }
    }

    // Vertical alignment
    // Match intrinsic_height's small-font-size bump so multi-line /
    // shaper-wrapped layout uses the same line-box height that Yoga reserved.
    // Without matching here, an fs=10 multi-line label would size to 16px
    // boxes but paint at 14px line height and siblings would not align.
    const float lh_mult = effective_font_size < 12.0f ? 1.6f : 1.4f;
    float automatic_lh = effective_font_size * lh_mult;
    float first_line_ascent = effective_font_size * 0.85f;
    canvas::AttributedString resolved_attributed;
    std::vector<canvas::Canvas::FontFeature> attributed_features;
    if (has_attributed_) {
        auto& shaper = canvas::global_text_shaper();
        resolved_attributed = resolved_attributed_string();
        attributed_features = resolved_font_features();
        const auto font_gen = canvas::font_registration_generation();
        if (!attributed_prepare_valid_ ||
            attributed_prepare_font_gen_ != font_gen ||
            attributed_prepare_features_ != attributed_features ||
            !same_attributed_source(attributed_prepare_source_,
                                    resolved_attributed)) {
            attributed_prepare_cache_ =
                shaper.prepare(resolved_attributed, attributed_features);
            attributed_prepare_source_ = resolved_attributed;
            attributed_prepare_features_ = attributed_features;
            attributed_prepare_font_gen_ = font_gen;
            attributed_prepare_valid_ = true;
        }
        const auto& attributed = attributed_prepare_cache_;
        if (attributed.line_height() > 0)
            automatic_lh = attributed.line_height();
        if (attributed.ascent() > 0)
            first_line_ascent = attributed.ascent();
        else
            first_line_ascent = fallback_attributed_ascent(
                resolved_attributed, effective_font_size);
    }
    float lh = line_height_ > 0 ? line_height_ : automatic_lh;

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
    const bool captured_cache_usable =
        text_box.width > 0.0f &&
        cached_line_layout_usable(display_text, effective_font_size,
                                  effective_letter_spacing, text_box.width);
    const bool paint_as_lines = multi_line_ || captured_wrap_fallback_ ||
                                captured_cache_usable || has_attributed_;
    const float shape_width = text_box.width > 0.0f
        ? text_box.width : std::numeric_limits<float>::max();
    const bool use_shaper_wrap = paint_as_lines &&
        (text_box.width > 0.0f || has_attributed_);

    // Reuse the cached shaped layout when nothing the shaper depends on has
    // changed, so paint() avoids re-running prepare() + layout_with_lines()
    // (which allocate a PreparedText and per-line strings) every frame inside
    // View::paint_all's no-alloc region. The key captures every shaper input:
    // display_text, the resolved family/size, the wrap width (text_box.width,
    // which changes on every resize), the resolved line height, and the break
    // mode and the effective max-lines decision. On a hit the layout is read in place
    // (no copy, no re-shape); the output is byte-identical to a recompute.
    const canvas::ShapedLayout* shaped_layout = nullptr;
    if (use_shaper_wrap) {
        // font_registration_generation() is part of the key because the
        // shaper's measure_segment() resamples the resolved typeface (and thus
        // glyph advances → wrap points) whenever a font is (re)registered —
        // e.g. an async register_font_url() completing after the first paint.
        // Without it a Label that first shaped against the fallback face would
        // serve that stale wrap until some other key field happened to change.
        const int shaped_max_lines = has_attributed_ && !multi_line_ &&
                !captured_wrap_fallback_ ? 1 : 0;
        std::string shaped_family_key = family;
        if (has_attributed_) {
            shaped_family_key.clear();
            for (const auto& span : resolved_attributed.spans()) {
                shaped_family_key.append(std::to_string(span.font_family.size()));
                shaped_family_key.push_back(':');
                shaped_family_key.append(span.font_family);
                shaped_family_key.push_back(';');
            }
        }
        ShapedLayoutKey key{display_text, std::move(shaped_family_key), font_variant(),
                            effective_font_size, effective_font_weight(),
                            font_style_, effective_letter_spacing,
                            text_box.width, lh,
                            static_cast<int>(break_mode),
                            shaped_max_lines,
                            captured_cache_usable,
                            canvas::font_registration_generation()};
        if (!shaped_cache_valid_ || !(shaped_cache_key_ == key)) {
            auto& shaper = canvas::global_text_shaper();
            // A captured layout is used verbatim while the conditions that
            // produced it still hold, and reflowed the moment they do not.
            if (captured_cache_usable) {
                ++line_break_counts().cached;
                shaped_cache_layout_ = has_attributed_
                    ? layout_attributed_from_cached_lines(
                          resolved_attributed, lh)
                    : layout_from_cached_lines(display_text, lh);
            } else {
                if (cached_line_boxes_.empty()) ++line_break_counts().uncached;
                else ++line_break_counts().reflowed;
                if (has_attributed_) {
                    shaped_cache_layout_ = shaper.layout_with_lines(
                        attributed_prepare_cache_, shape_width,
                        line_height_ > 0.0f ? lh : 0.0f,
                        /*max_lines=*/shaped_max_lines, break_mode);
                } else {
                    auto prepared = shaper.prepare(
                        display_text, family, effective_font_size,
                        effective_font_weight(), font_style_,
                        effective_letter_spacing, resolved_font_features());
                    shaped_cache_layout_ = shaper.layout_with_lines(
                        prepared, shape_width, lh,
                        /*max_lines=*/shaped_max_lines, break_mode);
                }
            }
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
    int source_lines = paint_as_lines
        ? (use_shaper_wrap
               ? std::max(1, shaped_layout->line_count)
               : static_cast<int>(std::count(display_text.begin(),
                                             display_text.end(), '\n')) + 1
                 - (!display_text.empty() && display_text.back() == '\n' ? 1 : 0))
        : 1;
    int visible_lines = source_lines;
    if (paint_as_lines && line_clamp_ > 0 && line_clamp_ < source_lines)
        visible_lines = line_clamp_;
    const bool captured_single_line =
        captured_cache_usable && shaped_layout != nullptr &&
        shaped_layout->line_count == 1;
    const float single_line_text_height = has_attributed_
        ? automatic_lh : effective_font_size;
    if (has_attributed_ && shaped_layout != nullptr &&
        !captured_cache_usable && !shaped_layout->lines.empty() &&
        shaped_layout->lines.front().ascent > 0.0f)
        first_line_ascent = shaped_layout->lines.front().ascent;
    float text_h = paint_as_lines
        ? captured_single_line
            ? single_line_text_height
            : [&] {
                  if (!has_attributed_ || shaped_layout == nullptr ||
                      captured_cache_usable) return lh * static_cast<float>(visible_lines);
                  float height = 0.0f;
                  for (int i = 0; i < visible_lines; ++i)
                      height += shaped_layout->lines[static_cast<std::size_t>(i)].height;
                  return height;
              }()
        : single_line_text_height;
    float baseline_y;
    switch (vertical_align_) {
        case canvas::TextVerticalAlign::top:
            baseline_y = first_line_ascent;
            break;
        case canvas::TextVerticalAlign::bottom:
            baseline_y = text_box.height - text_h + first_line_ascent;
            break;
        case canvas::TextVerticalAlign::baseline:
            baseline_y = text_box.height * 0.75f;
            break;
        case canvas::TextVerticalAlign::center:
        default:
            // Center the visible block within bounds, then offset to the
            // first line's baseline. For single-line this collapses to
            // bounds.h/2 + 0.35*font_size (the historic formula) because
            // text_h == effective_font_size and 0.85 - 0.5 == 0.35.
            baseline_y = (text_box.height - text_h) * 0.5f + first_line_ascent;
            break;
    }
    if (captured_cache_usable && shaped_layout != nullptr &&
        !shaped_layout->lines.empty()) {
        // A cached browser line box already resolved padding, line-height and
        // vertical-align. CSS distributes the line box's extra leading above
        // and below the font em box; using only `top + ascent` drops that
        // half-leading and paints compact button captions visibly high. Keep
        // Chromium's captured top, add its half-leading, then the active face
        // ascent. Do not center the complete owner bounds a second time.
        const auto& first_line = shaped_layout->lines.front();
        const float half_leading = std::max(
            0.0f, (first_line.height - single_line_text_height) * 0.5f);
        baseline_y = first_line.y + half_leading + first_line_ascent;
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
            x = text_box.width * 0.5f;
            break;
        case LabelAlign::right:
            canvas.set_text_align(canvas::TextAlign::right);
            x = text_box.width;
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
    // Captured line boxes already contain Chrome's aligned left edge. Native
    // center/right anchoring would apply the same alignment a second time.
    if (captured_cache_usable) {
        canvas.set_text_align(canvas::TextAlign::left);
        x = 0.0f;
    }
    const auto decoration_color = has_decoration_color_
        ? decoration_color_ : text_color;
    auto decorate_plain = [&](const std::string& painted, float anchor_x,
                              float line_baseline, bool left_anchored) {
        const float width = canvas.measure_text(painted);
        float start = anchor_x;
        if (!left_anchored) {
            if (effective_text_align == LabelAlign::center)
                start -= width * 0.5f;
            else if (effective_text_align == LabelAlign::right)
                start -= width;
        }
        paint_decoration_(canvas, start, width, line_baseline,
                          effective_font_size, decoration_color);
    };

    // Track the actually-painted single-line string so the decoration block
    // below measures the truncated text, not the original. Multi-line keeps
    // using display_text since it paints the full string across multiple draw
    // calls.
    std::string draw_text = display_text;
    if (!has_attributed_ && (!paint_as_lines ||
        (captured_single_line && text_overflow_ellipsis()))) {
        // CSS `text-overflow: ellipsis`. Truncate with U+2026 when the
        // measured text exceeds the content-box, regardless of text-align
        // (CSS truncates at the trailing edge for all three). UTF-8-safe via
        // codepoint binary-search in truncate_to_width().
        float draw_x = x;
        float available_width = text_box.width;
        if (captured_single_line) {
            draw_x = shaped_layout->lines.front().x_offset;
            available_width = std::max(0.0f, text_box.width - draw_x);
        }
        if (text_overflow_ellipsis())
            draw_text = truncate_to_width(canvas, display_text, available_width);
        canvas.fill_text(draw_text, draw_x, baseline_y);
        decorate_plain(draw_text, draw_x, baseline_y,
                       captured_cache_usable);
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

        if (use_shaper_wrap && has_attributed_) {
            paint_attributed_lines_(canvas, *shaped_layout, baseline_y, lh,
                                    visible_lines, need_ellipsis,
                                    text_overflow_ellipsis() &&
                                        shaped_layout->line_count == 1 &&
                                        shaped_layout->lines.front().width >
                                            text_box.width,
                                    captured_cache_usable);
        } else if (use_shaper_wrap) {
            // Shaped-layout iteration path. TextShaper already split
            // display_text into shaped_layout.lines using the active
            // BreakMode (break-word / anywhere). Iterate those lines instead
            // of `\n`-splitting. Line-clamp + ellipsis logic is identical,
            // driven by source_lines (= shaped_layout.line_count) and
            // visible_lines.
            const float captured_first_top = captured_cache_usable &&
                    !shaped_layout->lines.empty()
                ? shaped_layout->lines.front().y : 0.0f;
            for (const auto& shaped_line : shaped_layout->lines) {
                if (emitted >= visible_lines) break;
                std::string line = shaped_line.text;
                if (need_ellipsis && (emitted + 1 == visible_lines)) {
                    line.append("\xe2\x80\xa6");
                }
                const float line_baseline = captured_cache_usable
                    ? baseline_y + shaped_line.y - captured_first_top
                    : y;
                canvas.fill_text(line, x + shaped_line.x_offset, line_baseline);
                decorate_plain(line, x + shaped_line.x_offset, line_baseline,
                               captured_cache_usable);
                y += lh;
                ++emitted;
            }
        } else {
            // Legacy `\n`-only split path. Used when text_box.width is 0 /
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
                decorate_plain(line, x, y, false);
                y += lh;
                pos = nl + 1;
                ++emitted;
            }
        }
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
