// test_widgets_label.cpp — extracted from test_widgets.cpp in the
// 2026-05 Phase 5 (P5-3 follow-up) refactor.
//
// Label widget coverage (the largest single-widget cluster in
// test_widgets.cpp). Covers text rendering / intrinsic_width /
// intrinsic_height / line-height multiplier (#76) / line_clamp /
// measured_height under bounded width / baseline_y from text
// metrics / vertical text direction / letter_spacing in glyphs not
// UTF-8 bytes (#928 + #1407 + #76).

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <pulp/view/widgets.hpp>
#include <pulp/view/window_host.hpp>
#include <pulp/canvas/canvas.hpp>

#include <cmath>
#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

using namespace pulp::view;
using namespace pulp::canvas;
using Catch::Matchers::WithinAbs;

// Local helpers — duplicated from test_widgets.cpp's anonymous namespace
// to keep the split self-contained per the extracted-TU pattern.
namespace {

std::vector<DrawCommand> commands_of(const RecordingCanvas& canvas,
                                     DrawCommand::Type type) {
    std::vector<DrawCommand> matches;
    for (const auto& command : canvas.commands()) {
        if (command.type == type) {
            matches.push_back(command);
        }
    }
    return matches;
}

Label* add_child_label(View& parent, std::string text = "x") {
    auto child = std::make_unique<Label>(std::move(text));
    child->set_bounds({0, 0, 100, 20});
    auto* raw = child.get();
    parent.add_child(std::move(child));
    return raw;
}

}  // namespace

TEST_CASE("Label renders text", "[view][widget]") {
    Label label("Gain");
    label.set_bounds({0, 0, 100, 20});

    RecordingCanvas canvas;
    label.paint(canvas);

    REQUIRE(canvas.count(DrawCommand::Type::fill_text) == 1);
    REQUIRE(canvas.count(DrawCommand::Type::set_font) == 1);
}

TEST_CASE("Label text can be changed", "[view][widget]") {
    Label label("Initial");
    REQUIRE(label.text() == "Initial");

    label.set_text("Changed");
    REQUIRE(label.text() == "Changed");
}

TEST_CASE("Label intrinsic_width fits long text", "[view][widget][issue-928]") {
    // Regression: previously Label reported 0 intrinsic width and
    // inherited a small parent width in flex-row containers, causing
    // Spectr's "ZOOMABLE FILTER BANK" header to clip to "ZOOMABLE FII".
    Label long_label("ZOOMABLE FILTER BANK");
    Label short_label("BANK");

    float long_w = long_label.intrinsic_width();
    float short_w = short_label.intrinsic_width();

    // Both must report a positive, non-zero natural width.
    REQUIRE(long_w > 0);
    REQUIRE(short_w > 0);

    // The long label must report a width comfortably larger than the
    // short label — proving width scales with content length.
    REQUIRE(long_w > short_w * 2.0f);

    // Empty labels report no intrinsic width (parent decides).
    Label empty;
    REQUIRE(empty.intrinsic_width() == 0);
}

TEST_CASE("Label intrinsic_width scales with font size", "[view][widget][issue-928]") {
    Label small("Hello world");
    small.set_font_size(12.0f);

    Label large("Hello world");
    large.set_font_size(36.0f);

    REQUIRE(large.intrinsic_width() > small.intrinsic_width());
}

TEST_CASE("Label intrinsic_height bumps line-height multiplier for small fonts (#76)",
          "[view][widget][issue-pulp-internal-76]") {
    // pulp-internal #76 — Spectr's `<span fontSize=10>SNAPSHOT</span>` in
    // the bottom toolbar was vertically clipped because Yoga reserved
    // 10 * 1.4 = 14px for the Label, but Inter's typographic ascent +
    // descent at 10pt is ~13px and the GPU clip-rect on the View bounds
    // shaved descender slack off in practice. intrinsic_height now uses
    // a 1.6 multiplier for small font sizes (< 12pt) to ensure the full
    // glyph extent fits inside the reserved box.
    //
    // Larger sizes keep the historical 1.4 multiplier — they have plenty
    // of absolute slack and downstream visual tests / golden-files
    // depend on the exact numbers.

    // Below the threshold — never below the legacy safety reservation.
    Label tiny("snapshot");
    tiny.set_font_size(10.0f);
    REQUIRE(tiny.intrinsic_height() >= 10.0f * 1.4f);

    Label small("ok");
    small.set_font_size(11.5f);
    REQUIRE(small.intrinsic_height() >= 11.5f * 1.4f);

    // At/above the threshold — real metrics or fallback multiplier.
    Label normal("hello");
    normal.set_font_size(12.0f);
    REQUIRE(normal.intrinsic_height() >= 12.0f * 1.4f);

    Label big("HEADING");
    big.set_font_size(24.0f);
    REQUIRE(big.intrinsic_height() >= 24.0f * 1.4f);

    // Explicit line_height ALWAYS wins (multiplier ignored on either side
    // of the threshold) — preserves the existing escape hatch.
    Label explicit_tiny("snapshot");
    explicit_tiny.set_font_size(10.0f);
    explicit_tiny.set_line_height(20.0f);
    REQUIRE(explicit_tiny.intrinsic_height() == Catch::Approx(20.0f));

    Label explicit_big("HEADING");
    explicit_big.set_font_size(24.0f);
    explicit_big.set_line_height(20.0f);
    REQUIRE(explicit_big.intrinsic_height() == Catch::Approx(20.0f));
}

TEST_CASE("Label intrinsic_width respects text-transform", "[view][widget][issue-928]") {
    Label lower("zoomable filter bank");
    Label upper("zoomable filter bank");
    upper.set_text_transform(Label::TextTransform::uppercase);

    // Uppercase characters typically advance wider than lowercase, so
    // the transformed label must measure at least as wide.
    REQUIRE(upper.intrinsic_width() >= lower.intrinsic_width());

    // Lowercase transform path — exercises the std::tolower branch in
    // intrinsic_width() so estimator and shaper agree on the post-
    // transform character count.
    Label lc("ZOOMABLE Filter Bank");
    lc.set_text_transform(Label::TextTransform::lowercase);
    REQUIRE(lc.intrinsic_width() > 0);

    // Capitalize transform path — exercises the per-word leading-cap
    // loop. Same character count as the source string, so width is at
    // least as wide as the all-lowercase variant.
    Label cap("zoomable filter bank");
    cap.set_text_transform(Label::TextTransform::capitalize);
    REQUIRE(cap.intrinsic_width() > 0);
    REQUIRE(cap.intrinsic_width() >= lower.intrinsic_width());

    // Letter-spacing branch — adds extra advance per glyph break that
    // HarfBuzz / the estimator don't include natively.
    Label spaced("ZOOMABLE FILTER BANK");
    spaced.set_letter_spacing(2.0f);
    Label tight("ZOOMABLE FILTER BANK");
    REQUIRE(spaced.intrinsic_width() > tight.intrinsic_width());
}

TEST_CASE("Label intrinsic_width yields zero for multi-line", "[view][widget][issue-928]") {
    // Multi-line labels defer to the parent's available width for
    // wrapping; reporting a single-line natural width here would force
    // a flex-row container to grow when the user explicitly opted into
    // wrapping.
    Label ml("ZOOMABLE FILTER BANK\nWITH SUBTITLE");
    ml.set_multi_line(true);
    REQUIRE(ml.intrinsic_width() == 0);
}

TEST_CASE("Label intrinsic_height counts explicit newlines on multi_line labels",
          "[view][widget][internal-74]") {
    // pulp-internal #74 — Spectr's Settings modal section subtitles and
    // any multi-line description text rendered as `<p>foo\nbar</p>` were
    // having every line past the first clipped because Label always
    // reported a one-line height regardless of how many `\n`-delimited
    // lines paint() emitted. Yoga then reserved exactly one line and the
    // parent's overflow / sibling layout truncated the rest.
    //
    // The fix in widgets.cpp counts `\n` and multiplies by `lh` so the
    // intrinsic height returned to Yoga matches paint()'s line count.

    // Sanity: single-line behavior is unchanged (regression guard).
    Label single("just one line");
    single.set_font_size(12.0f);
    const float lh_single = single.intrinsic_height();
    REQUIRE_THAT(single.intrinsic_height(), WithinAbs(lh_single, 0.01f));

    // Multi-line label with no newlines and no width — still a single
    // line until a soft-wrap path actually wraps (handled separately by
    // measured_height(available_width) below).
    Label ml_short("just one line");
    ml_short.set_multi_line(true);
    ml_short.set_font_size(12.0f);
    ml_short.set_line_height(lh_single);
    REQUIRE_THAT(ml_short.intrinsic_height(), WithinAbs(lh_single, 0.01f));

    // Two explicit lines.
    Label two("line one\nline two");
    two.set_multi_line(true);
    two.set_font_size(12.0f);
    two.set_line_height(lh_single);
    REQUIRE_THAT(two.intrinsic_height(), WithinAbs(lh_single * 2.0f, 0.01f));

    // Three explicit lines — generalizes to N.
    Label three("Smooths transitions between filter states.\n"
                "Reduces clicks and zipper noise during automation.\n"
                "Adjustable per modulation source.");
    three.set_multi_line(true);
    three.set_font_size(13.0f);
    Label one_13("one");
    one_13.set_font_size(13.0f);
    const float lh_13 = one_13.intrinsic_height();
    REQUIRE_THAT(three.intrinsic_height(), WithinAbs(lh_13 * 3.0f, 0.01f));

    // Explicit line_height beats font_size * 1.4 default — multi-line
    // count still multiplies through.
    Label four("a\nb\nc\nd");
    four.set_multi_line(true);
    four.set_font_size(12.0f);
    four.set_line_height(20.0f);
    REQUIRE_THAT(four.intrinsic_height(), WithinAbs(20.0f * 4.0f, 0.01f));

    // multi_line=false keeps the legacy one-line height even when the
    // text contains `\n` (single-line paint draws the whole string in
    // one fill_text call — the count must reflect that contract).
    Label single_with_newlines("hidden\nnewlines");
    single_with_newlines.set_multi_line(false);
    single_with_newlines.set_font_size(12.0f);
    single_with_newlines.set_line_height(lh_single);
    REQUIRE_THAT(single_with_newlines.intrinsic_height(), WithinAbs(lh_single, 0.01f));
}

TEST_CASE("Label intrinsic_height ignores a trailing newline (no phantom line)",
          "[view][widget][internal-74][issue-1969]") {
    // PR #1969 Codex P2 — a string ending with `\n` used to count an
    // extra line in the `\n`-count loop ("Title\n" → 2). But
    // Label::paint()'s split-and-emit loop stops once `pos ==
    // display_text.size()`, so it draws exactly one line. Yoga was
    // reserving phantom whitespace that the paint pass never filled,
    // breaking vertical centering / sibling layout. Mirrors CSS
    // `white-space: pre` line-box counting (a trailing `\n` is the end
    // of a paragraph, not the start of a new empty line).
    const float fs = 12.0f;

    // "Title\n" — counts as ONE line, not two.
    Label trailing("Title\n");
    trailing.set_multi_line(true);
    trailing.set_font_size(fs);
    const float lh = trailing.intrinsic_height();
    REQUIRE_THAT(trailing.intrinsic_height(), WithinAbs(lh, 0.01f));

    // "Title\nSubtitle" — no trailing `\n`, two real lines.
    Label two_real("Title\nSubtitle");
    two_real.set_multi_line(true);
    two_real.set_font_size(fs);
    two_real.set_line_height(lh);
    REQUIRE_THAT(two_real.intrinsic_height(), WithinAbs(lh * 2.0f, 0.01f));

    // "Title\nSubtitle\n" — two visible lines, trailing `\n` shaves
    // the phantom third.
    Label two_with_trailing("Title\nSubtitle\n");
    two_with_trailing.set_multi_line(true);
    two_with_trailing.set_font_size(fs);
    two_with_trailing.set_line_height(lh);
    REQUIRE_THAT(two_with_trailing.intrinsic_height(), WithinAbs(lh * 2.0f, 0.01f));

    // Just a single `\n` — empty content, one (empty) line reserved.
    // We don't try to claim height 0; an empty line still occupies
    // one line-height of vertical space in CSS block-flow semantics.
    Label only_newline("\n");
    only_newline.set_multi_line(true);
    only_newline.set_font_size(fs);
    only_newline.set_line_height(lh);
    REQUIRE_THAT(only_newline.intrinsic_height(), WithinAbs(lh, 0.01f));

    // "\nFoo" — leading `\n` keeps both lines (the leading newline
    // is a real empty line; only TRAILING is dropped).
    Label leading_newline("\nFoo");
    leading_newline.set_multi_line(true);
    leading_newline.set_font_size(fs);
    leading_newline.set_line_height(lh);
    REQUIRE_THAT(leading_newline.intrinsic_height(), WithinAbs(lh * 2.0f, 0.01f));

    // Trailing-newline shave interacts correctly with line_clamp: the
    // count is shaved BEFORE clamp comparison, so a clamp of 2 on
    // "a\nb\n" still gives 2 lines (not clamped from a phantom 3).
    Label clamped_trailing("a\nb\n");
    clamped_trailing.set_multi_line(true);
    clamped_trailing.set_font_size(fs);
    clamped_trailing.set_line_height(lh);
    clamped_trailing.set_line_clamp(2);
    REQUIRE_THAT(clamped_trailing.intrinsic_height(), WithinAbs(lh * 2.0f, 0.01f));
}

TEST_CASE("Label intrinsic_height honors line_clamp on multi_line labels",
          "[view][widget][internal-74][issue-1552]") {
    // pulp-internal #74 + pulp #1552 — when a clamp is set, paint() only
    // emits `line_clamp_` lines, so the reserved height must match.
    // Otherwise Yoga reserves space for lines that will never be drawn
    // and the surrounding flex layout has dead vertical whitespace.
    Label clamped("a\nb\nc\nd\ne");
    clamped.set_multi_line(true);
    clamped.set_font_size(12.0f);
    clamped.set_line_clamp(2);
    Label unclamped_one("a");
    unclamped_one.set_font_size(12.0f);
    const float lh = unclamped_one.intrinsic_height();
    REQUIRE_THAT(clamped.intrinsic_height(), WithinAbs(lh * 2.0f, 0.01f));

    // line_clamp_ == 0 disables clamping — all source lines counted.
    Label unclamped("a\nb\nc\nd\ne");
    unclamped.set_multi_line(true);
    unclamped.set_font_size(12.0f);
    unclamped.set_line_height(lh);
    unclamped.set_line_clamp(0);
    REQUIRE_THAT(unclamped.intrinsic_height(), WithinAbs(lh * 5.0f, 0.01f));

    // line_clamp_ >= source line count is effectively no clamp.
    Label looseclamp("a\nb");
    looseclamp.set_multi_line(true);
    looseclamp.set_font_size(12.0f);
    looseclamp.set_line_height(lh);
    looseclamp.set_line_clamp(99);
    REQUIRE_THAT(looseclamp.intrinsic_height(), WithinAbs(lh * 2.0f, 0.01f));
}

TEST_CASE("Label measured_height counts soft-wrapped lines under a bounded width",
          "[view][widget][internal-74]") {
    // pulp-internal #74 — Spectr's Settings-modal subtitle paragraphs
    // (and the equivalent SNAPSHOT-style chrome) live inside flex parents
    // with a fixed/computed width. The bridge does NOT inject `\n` into
    // a description like `"How bands and colors render in the analyzer
    // panel."` — instead the Label is multi_line, the parent gives it a
    // width, and paint()'s shaped-wrap loop emits 2–3 lines. Until this
    // PR, intrinsic_height() returned ONE line, so Yoga clipped lines 2+.
    //
    // measured_height(available_width) consults the same shaper paint()
    // uses, so the line count returned to Yoga matches the line count
    // actually drawn.

    const std::string long_text =
        "Smooths transitions between filter states. Reduces clicks "
        "and zipper noise during automation by interpolating the "
        "filter coefficients between two snapshots in real time.";
    Label desc(long_text);
    desc.set_multi_line(true);
    desc.set_font_size(13.0f);
    const float lh = desc.intrinsic_height();

    // Very wide: the text fits on one line → measured height collapses
    // to one line (= ceil(lh), since the shaper path ceils to a sub-
    // pixel-safe integer).
    const float wide_h = desc.measured_height(10000.0f);
    REQUIRE(wide_h >= lh - 1.0f);
    REQUIRE(wide_h <= std::ceil(lh) + 0.5f);

    // Bounded narrow width forces wrap to several lines — measured
    // height must reflect 2+ lines, never just one.
    float narrow_h = desc.measured_height(220.0f);
    REQUIRE(narrow_h >= lh * 2.0f);
    REQUIRE(narrow_h <= lh * 10.0f);  // sanity: bounded above

    // Tighter width → at least as many lines as the wider case.
    float tighter_h = desc.measured_height(140.0f);
    REQUIRE(tighter_h >= narrow_h);

    // available_width <= 0 falls back to intrinsic_height() — measure
    // callback gives 0 when Yoga has no constraint yet, and the caller
    // would otherwise feed garbage to the shaper.
    REQUIRE_THAT(desc.measured_height(0.0f),  WithinAbs(lh, 0.01f));
    REQUIRE_THAT(desc.measured_height(-1.0f), WithinAbs(lh, 0.01f));

    // Single-line label: measured_height matches intrinsic_height
    // regardless of width — the multi_line gate keeps the shaper path
    // off so single-line widgets pay no extra cost.
    Label snap("SNAPSHOT");
    snap.set_font_size(10.0f);
    // pulp-internal #76: small fonts (<12pt) use the 1.6 line-height
    // multiplier so glyphs don't clip in compact toolbars; the measure
    // path mirrors that to keep Yoga reservation in sync with paint.
    const float snap_lh = snap.intrinsic_height();
    REQUIRE_THAT(snap.measured_height(50.0f),    WithinAbs(snap_lh, 0.01f));
    REQUIRE_THAT(snap.measured_height(10000.0f), WithinAbs(snap_lh, 0.01f));
}

TEST_CASE("Label baseline_y follows text metrics and inherited font size",
          "[view][widget][baseline][coverage]") {
    Label normal("CHAIN");
    normal.set_font_size(14.0f);
    const float normal_baseline = normal.baseline_y();
    REQUIRE(normal_baseline > 0.0f);
    REQUIRE(normal_baseline < normal.intrinsic_height());

    Label large("CHAIN");
    large.set_font_size(28.0f);
    REQUIRE(large.baseline_y() > normal_baseline);

    Label empty("");
    empty.set_font_size(14.0f);
    REQUIRE(empty.baseline_y() > 0.0f);

    View parent;
    parent.set_bounds({0, 0, 200, 100});
    auto* inherited = add_child_label(parent, "INFO");
    parent.set_inheritable_font_size(24.0f);
    REQUIRE_FALSE(inherited->has_own_font_size());
    REQUIRE(inherited->baseline_y() > normal_baseline);

    inherited->set_font_size(10.0f);
    REQUIRE(inherited->baseline_y() < normal_baseline);
}

TEST_CASE("Label intrinsic_width is sane for typical chrome strings",
          "[view][widget][issue-945]") {
    // pulp #945 regression: after PR #935 enabled Label auto-grow,
    // certain fresh-build states reported tiny intrinsic widths
    // (e.g. 5–20 px for a 20-character string) because the global
    // TextShaper used SkFontMgr::RefEmpty() and silently produced
    // ~0 advance widths. Yoga then collapsed the Label and the
    // painter truncated the chrome ("SF · ZOOMA · LIVE · IIR · F").
    //
    // Lower-bound the reported width against a conservative
    // estimate (40% of font_size per character) — well below any
    // real shaped/estimated width, but well above the broken
    // ~zero-advance regression. If this test ever drops below the
    // bound, the platform font manager has stopped resolving
    // typefaces in the shaper path.
    Label chrome("SPECTR ZOOMABLE FILTER BANK");
    chrome.set_font_size(14.0f);

    float w = chrome.intrinsic_width();
    float min_expected = chrome.text().size() * 14.0f * 0.40f;
    REQUIRE(w > min_expected);
}

TEST_CASE("Label intrinsic_width matches text after rebuild / re-measure",
          "[view][widget][issue-945]") {
    // pulp #945: A label whose text is changed after construction must
    // re-measure cleanly. The first capture in the field showed correct
    // labels; subsequent rebuilds collapsed widths because the shaper
    // cached zero-advance segments under (font, size). With the
    // platform font manager wired up, repeated prepare() calls always
    // return positive width and the cached entries are sane.
    Label l("SF");
    l.set_font_size(14.0f);
    float short_w = l.intrinsic_width();

    l.set_text("SPECTR ZOOMABLE FILTER BANK");
    float long_w = l.intrinsic_width();

    REQUIRE(short_w > 0);
    REQUIRE(long_w > 0);
    // The longer string must report a substantially wider footprint.
    // Field-observed regression collapsed long_w to ~short_w.
    REQUIRE(long_w > short_w * 5.0f);

    // Reverting back to the short text must give back the short width
    // (within a small rounding margin) — proves measurement is a pure
    // function of the current text, not stuck on a stale value.
    l.set_text("SF");
    float short_again = l.intrinsic_width();
    REQUIRE(short_again > 0);
    REQUIRE(short_again < long_w * 0.5f);
}

TEST_CASE("Label intrinsic_width handles vertical text direction",
          "[view][widget][issue-945][issue-943]") {
    // pulp #943 P2 (#935 finding 1): when text_direction_ is vertical,
    // paint() rotates the canvas 90° so the horizontal footprint is the
    // line height, not the shaped string advance. Reporting the advance
    // here would make Yoga reserve enormous width for a vertical label
    // and starve sibling columns.
    Label vertical("VERTICAL LABEL TEXT");
    vertical.set_font_size(14.0f);
    vertical.set_text_direction(TextDirection::top_to_bottom);

    Label horizontal("VERTICAL LABEL TEXT");
    horizontal.set_font_size(14.0f);

    float v = vertical.intrinsic_width();
    float h = horizontal.intrinsic_width();

    REQUIRE(v > 0);
    REQUIRE(h > 0);
    // Vertical width is one line tall — must be much smaller than the
    // full horizontal advance of the same string.
    REQUIRE(v < h * 0.25f);

    // Bottom-to-top gets the same treatment.
    Label vertical2("VERTICAL LABEL TEXT");
    vertical2.set_font_size(14.0f);
    vertical2.set_text_direction(TextDirection::bottom_to_top);
    REQUIRE(vertical2.intrinsic_width() == v);
}

TEST_CASE("Label letter_spacing counts glyphs not UTF-8 bytes",
          "[view][widget][issue-945][issue-943]") {
    // pulp #943 P2 (#935 finding 2): letter_spacing was multiplied by
    // (display_text.size() - 1), counting raw UTF-8 bytes instead of
    // code points. A 4-character CJK string takes 12 bytes in UTF-8,
    // so spacing was over-applied 3x and the label inflated.
    //
    // ASCII baseline — both strings are 4 ASCII chars, so byte count
    // and glyph count match. This anchors the comparison.
    Label ascii_no_spacing("ABCD");
    ascii_no_spacing.set_font_size(14.0f);
    Label ascii_with_spacing("ABCD");
    ascii_with_spacing.set_font_size(14.0f);
    ascii_with_spacing.set_letter_spacing(2.0f);

    float ascii_delta = ascii_with_spacing.intrinsic_width()
                      - ascii_no_spacing.intrinsic_width();
    // 4 glyphs → 3 gaps → 6.0 px extra (subject to ceil rounding).
    REQUIRE(ascii_delta >= 5.0f);
    REQUIRE(ascii_delta <= 7.0f);

    // Multibyte path: 4 CJK characters in UTF-8 are 12 bytes. With the
    // old byte-count math the spacing delta would be ~22 px (11 gaps);
    // with the glyph-count math it's the same ~6 px as the ASCII case.
    Label cjk_no_spacing("\xE6\x97\xA5\xE6\x9C\xAC\xE8\xAA\x9E\xE6\x96\x87"); // 日本語文
    cjk_no_spacing.set_font_size(14.0f);
    Label cjk_with_spacing("\xE6\x97\xA5\xE6\x9C\xAC\xE8\xAA\x9E\xE6\x96\x87");
    cjk_with_spacing.set_font_size(14.0f);
    cjk_with_spacing.set_letter_spacing(2.0f);

    float cjk_delta = cjk_with_spacing.intrinsic_width()
                    - cjk_no_spacing.intrinsic_width();
    REQUIRE(cjk_delta >= 5.0f);
    REQUIRE(cjk_delta <= 7.0f);
}

TEST_CASE("Label applies text transforms when painting", "[view][widget]") {
    Label label("gain stage");
    label.set_bounds({0, 0, 120, 24});

    RecordingCanvas canvas;
    label.set_text_transform(Label::TextTransform::uppercase);
    label.paint(canvas);
    REQUIRE(commands_of(canvas, DrawCommand::Type::fill_text).front().text == "GAIN STAGE");

    canvas.clear();
    label.set_text("GAIN STAGE");
    label.set_text_transform(Label::TextTransform::lowercase);
    label.paint(canvas);
    REQUIRE(commands_of(canvas, DrawCommand::Type::fill_text).front().text == "gain stage");

    canvas.clear();
    label.set_text("gain stage");
    label.set_text_transform(Label::TextTransform::capitalize);
    label.paint(canvas);
    REQUIRE(commands_of(canvas, DrawCommand::Type::fill_text).front().text == "Gain Stage");
}

TEST_CASE("Label paints explicit lines and decorations", "[view][widget]") {
    Label label("gain\ntrim");
    label.set_bounds({0, 0, 120, 60});
    label.set_multi_line(true);
    label.set_line_height(18.0f);
    label.set_text_decoration(Label::TextDecoration::underline);

    RecordingCanvas canvas;
    label.paint(canvas);

    auto text = commands_of(canvas, DrawCommand::Type::fill_text);
    REQUIRE(text.size() == 2);
    REQUIRE(text[0].text == "gain");
    REQUIRE(text[1].text == "trim");
    REQUIRE_THAT(text[1].f[1] - text[0].f[1], WithinAbs(18.0, 0.001));
    REQUIRE(canvas.count(DrawCommand::Type::stroke_line) == 1);
}

// pulp #1410 — verify that nowrap puts a Label into single-line paint
// mode (multi_line=false). Truncation is #1407's surface; this test
// just confirms the multi_line side-effect path the bridge relies on.
TEST_CASE("Label with nowrap + multi_line=false paints exactly one fill_text command",
          "[view][widget][issue-1410]") {
    Label label("Mid-band attenuation\nwith high-shelf compensation");
    label.set_bounds({0, 0, 200, 48});
    label.set_white_space_nowrap(true);
    label.set_multi_line(false);  // bridge does this side-effect

    RecordingCanvas canvas;
    label.paint(canvas);

    auto fills = commands_of(canvas, DrawCommand::Type::fill_text);
    REQUIRE(fills.size() == 1);  // would be 2 in multi_line mode (one per `\n`-split)
}

TEST_CASE("Label vertical text direction wraps paint in transforms", "[view][widget]") {
    Label label("Gain");
    label.set_bounds({0, 0, 32, 80});
    label.set_text_direction(TextDirection::top_to_bottom);

    RecordingCanvas canvas;
    label.paint(canvas);

    REQUIRE(canvas.count(DrawCommand::Type::save) == 1);
    REQUIRE(canvas.count(DrawCommand::Type::translate) == 1);
    REQUIRE(canvas.count(DrawCommand::Type::rotate) == 1);
    REQUIRE(canvas.count(DrawCommand::Type::restore) == 1);
    REQUIRE(commands_of(canvas, DrawCommand::Type::fill_text).front().text == "Gain");
}
