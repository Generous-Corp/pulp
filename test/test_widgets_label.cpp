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
#include <pulp/canvas/text_shaper.hpp>
#include <pulp/canvas/bundled_fonts.hpp>
#include <pulp/canvas/font_resolver.hpp>

#include <string>

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
    // A string ending with `\n` used to count an
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
          "[view][widget][baseline]") {
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
    // pulp #943: when text_direction_ is vertical, paint() rotates the canvas
    // 90° so the horizontal footprint is the
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
    // pulp #943: letter_spacing must count code points, not raw UTF-8 bytes.
    // A 4-character CJK string takes 12 bytes in UTF-8, so byte-counted spacing
    // would be over-applied 3x and inflate the label.
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
    // CSS adds the spacing after every character, so 4 glyphs → 4 steps →
    // 8.0 px extra (subject to ceil rounding).
    REQUIRE(ascii_delta >= 7.0f);
    REQUIRE(ascii_delta <= 9.0f);

    // Multibyte path: 4 CJK characters in UTF-8 are 12 bytes. With the
    // old byte-count math the spacing delta would be ~24 px (12 steps);
    // with the glyph-count math it's the same ~8 px as the ASCII case.
    Label cjk_no_spacing("\xE6\x97\xA5\xE6\x9C\xAC\xE8\xAA\x9E\xE6\x96\x87"); // 日本語文
    cjk_no_spacing.set_font_size(14.0f);
    Label cjk_with_spacing("\xE6\x97\xA5\xE6\x9C\xAC\xE8\xAA\x9E\xE6\x96\x87");
    cjk_with_spacing.set_font_size(14.0f);
    cjk_with_spacing.set_letter_spacing(2.0f);

    float cjk_delta = cjk_with_spacing.intrinsic_width()
                    - cjk_no_spacing.intrinsic_width();
    REQUIRE(cjk_delta >= 7.0f);
    REQUIRE(cjk_delta <= 9.0f);
    // The invariant the byte-vs-codepoint distinction actually protects: the
    // same number of characters reserves the same spacing whatever their
    // encoding costs.
    CHECK_THAT(cjk_delta, WithinAbs(ascii_delta, 1.0f));
}

TEST_CASE("Label letter_spacing is added after every glyph, including the last",
          "[view][widget][text]") {
    // CSS letter-spacing follows each character rather than sitting between
    // them, and SkParagraph — which paints the run — adds it that way too. A
    // width that reserves one step per GAP is one step short of what gets
    // drawn, so the glyphs run past the box Yoga sized from this number.
    //
    // Measured against Chrome's own line boxes over 89 letter-spaced runs in
    // the design corpus: per-character lands on -0.02% median width error,
    // per-gap on -2.86%.
    const float spacing = 3.0f;
    for (const char* text : {"A", "AB", "Handgloves"}) {
        INFO("text: " << text);
        std::size_t glyphs = 0;
        for (const char* c = text; *c; ++c) ++glyphs;

        Label tight(text);
        tight.set_font_size(16.0f);
        Label spaced(text);
        spaced.set_font_size(16.0f);
        spaced.set_letter_spacing(spacing);

        const float delta = spaced.intrinsic_width() - tight.intrinsic_width();
        CHECK_THAT(delta,
                   WithinAbs(spacing * static_cast<float>(glyphs), 1.0f));
    }

    // The single-character case is the one the per-gap convention cannot fake:
    // it reserves nothing at all, however wide the spacing.
    Label one_tight("A");
    Label one_spaced("A");
    one_spaced.set_letter_spacing(10.0f);
    CHECK(one_spaced.intrinsic_width() > one_tight.intrinsic_width());
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
    REQUIRE(canvas.count(DrawCommand::Type::stroke_line) == 2);
}

TEST_CASE("Label decoration follows every captured line without double alignment",
          "[view][widget][label-cache][decoration]") {
    Label label("alpha beta");
    label.set_font_family("Inter");
    label.set_font_size(12.0f);
    label.set_text_align(LabelAlign::center);
    label.set_text_decoration(Label::TextDecoration::underline);
    label.set_multi_line(true);
    label.set_bounds({0, 0, 100, 40});
    const auto face = resolved_face_identity("Inter", 400.0f);
    label.set_cached_line_boxes(
        {{5, 0, 30, 16, 0, 5}, {8, 16, 24, 16, 6, 4}}, 100.0f, face,
        true);

    // A GPU-off build has no resolvable face and must fail closed into normal
    // responsive wrapping.  The captured-position assertions below apply only
    // when the cache basis can actually be verified.
    if (face.empty()) {
        CHECK(label.cached_line_boxes().empty());
        CHECK(label.captured_wrap_fallback());
        RecordingCanvas fallback;
        label.paint(fallback);
        CHECK_FALSE(commands_of(fallback, DrawCommand::Type::fill_text).empty());
        return;
    }

    RecordingCanvas canvas;
    label.paint(canvas);
    const auto fills = commands_of(canvas, DrawCommand::Type::fill_text);
    const auto strokes = commands_of(canvas, DrawCommand::Type::stroke_line);
    REQUIRE(fills.size() == 2);
    REQUIRE(strokes.size() == 2);
    CHECK(fills[0].f[0] == Catch::Approx(5.0f));
    CHECK(fills[1].f[0] == Catch::Approx(8.0f));
    CHECK(strokes[0].f[0] == Catch::Approx(5.0f));
    CHECK(strokes[1].f[0] == Catch::Approx(8.0f));
    CHECK(strokes[0].f[1] != Catch::Approx(strokes[1].f[1]));
}

TEST_CASE("Label honors Chromium captured vertical line positions",
          "[view][widget][label-cache][alignment]") {
    Label label("CENTERED");
    label.set_font_family("Inter");
    label.set_font_size(12.0f);
    label.set_bounds({0, 0, 100, 40});
    const auto face = resolved_face_identity("Inter", 400.0f);
    label.set_cached_line_boxes(
        {{7, 3, 58, 16, 0, 8}}, 100.0f, face, false);

    RecordingCanvas canvas;
    label.paint(canvas);
    const auto fills = commands_of(canvas, DrawCommand::Type::fill_text);
    REQUIRE(fills.size() == 1);
    if (face.empty()) {
        CHECK(label.cached_line_boxes().empty());
        return;
    }
    CHECK(fills[0].f[0] == Catch::Approx(7.0f));
    // The exact ascent is backend/font dependent, but moving the captured top
    // by 3px must move the baseline by the same 3px instead of re-centering.
    Label at_zero("CENTERED");
    at_zero.set_font_family("Inter");
    at_zero.set_font_size(12.0f);
    at_zero.set_bounds({0, 0, 100, 40});
    at_zero.set_cached_line_boxes(
        {{7, 0, 58, 16, 0, 8}}, 100.0f, face, false);
    RecordingCanvas zero_canvas;
    at_zero.paint(zero_canvas);
    const auto zero_fills = commands_of(zero_canvas, DrawCommand::Type::fill_text);
    REQUIRE(zero_fills.size() == 1);
    CHECK(fills[0].f[1] - zero_fills[0].f[1] == Catch::Approx(3.0f));

    // Chromium splits the 4px difference between a 16px line box and the
    // 12px font em evenly. Native paint must retain the 2px top half-leading;
    // otherwise compact controls look high even though their boxes match.
    Label em_height("CENTERED");
    em_height.set_font_family("Inter");
    em_height.set_font_size(12.0f);
    em_height.set_bounds({0, 0, 100, 40});
    em_height.set_cached_line_boxes(
        {{7, 3, 58, 12, 0, 8}}, 100.0f, face, false);
    RecordingCanvas em_canvas;
    em_height.paint(em_canvas);
    const auto em_fills = commands_of(em_canvas, DrawCommand::Type::fill_text);
    REQUIRE(em_fills.size() == 1);
    CHECK(fills[0].f[1] - em_fills[0].f[1] == Catch::Approx(2.0f));
}

TEST_CASE("Attributed Label honors each Chromium captured line position",
          "[view][widget][label-cache][alignment][attributed]") {
    Label label("TOP BOTTOM");
    label.set_font_family("Inter");
    label.set_font_size(12.0f);
    label.set_bounds({0, 0, 120, 50});
    AttributedString attributed;
    TextSpan span;
    span.text = "TOP BOTTOM";
    span.font_family = "Inter";
    span.font_size = 12.0f;
    attributed.append(span);
    label.set_attributed_string(std::move(attributed));
    const auto face = resolved_face_identity("Inter", 400.0f);
    label.set_cached_line_boxes(
        {{4, 2, 24, 15, 0, 3}, {6, 25, 48, 15, 4, 6}},
        120.0f, face, true);

    RecordingCanvas canvas;
    label.paint(canvas);
    const auto fills = commands_of(canvas, DrawCommand::Type::fill_text);
    if (face.empty()) {
        CHECK(label.cached_line_boxes().empty());
        return;
    }
    REQUIRE(fills.size() == 2);
    CHECK(fills[0].f[0] == Catch::Approx(4.0f));
    CHECK(fills[1].f[0] == Catch::Approx(6.0f));
    CHECK(fills[1].f[1] - fills[0].f[1] == Catch::Approx(23.0f));
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

namespace {

// Serialize the painted fill_text stream (per-line text + x/y baseline) so two
// paints can be compared for byte-identical rendering output.
std::string fill_text_signature(const RecordingCanvas& canvas) {
    std::string sig;
    for (const auto& command : canvas.commands()) {
        if (command.type == DrawCommand::Type::fill_text) {
            sig += command.text;
            sig += '@';
            sig += std::to_string(command.f[0]);
            sig += ',';
            sig += std::to_string(command.f[1]);
            sig += ';';
        }
    }
    return sig;
}

// A multi-line, bounded-width Label that routes through the shaper soft-wrap
// path (the one the cache covers). The text wraps to several lines at width 120.
std::unique_ptr<Label> make_wrapped_label() {
    auto label = std::make_unique<Label>(
        "The quick brown fox jumps over the lazy dog near the river bank.");
    label->set_multi_line(true);
    label->set_font_size(13.0f);
    label->set_bounds({0, 0, 120, 200});
    return label;
}

}  // namespace

TEST_CASE("Label reuses the cached shaped layout across identical paints",
          "[view][widget][label-cache]") {
    auto label = make_wrapped_label();

    // First paint computes and caches the shaped layout (one prepare()).
    const uint64_t before = text_shaper_prepare_call_count();
    RecordingCanvas first;
    label->paint(first);
    const uint64_t after_first = text_shaper_prepare_call_count();
    REQUIRE(after_first - before == 1);

    // Sanity: it actually wrapped (multiple lines emitted via the shaper path).
    REQUIRE(commands_of(first, DrawCommand::Type::fill_text).size() >= 2);

    // Second identical paint must hit the cache: no further prepare().
    RecordingCanvas second;
    label->paint(second);
    REQUIRE(text_shaper_prepare_call_count() - after_first == 0);

    // And the rendered output must be byte-identical (behavior preserved).
    REQUIRE(fill_text_signature(first) == fill_text_signature(second));
}

TEST_CASE("Label re-shapes when text changes", "[view][widget][label-cache]") {
    auto label = make_wrapped_label();

    RecordingCanvas warm;
    label->paint(warm);  // populate cache
    const uint64_t after_warm = text_shaper_prepare_call_count();

    label->set_text("A completely different sentence that also needs to wrap here.");
    RecordingCanvas after;
    label->paint(after);

    // Text change invalidates the key → exactly one fresh prepare().
    REQUIRE(text_shaper_prepare_call_count() - after_warm == 1);
}

TEST_CASE("Label re-shapes when wrap width changes", "[view][widget][label-cache]") {
    auto label = make_wrapped_label();

    RecordingCanvas warm;
    label->paint(warm);
    const uint64_t after_warm = text_shaper_prepare_call_count();

    // A resize changes bounds().width, which is part of the cache key.
    label->set_bounds({0, 0, 80, 200});
    RecordingCanvas resized;
    label->paint(resized);
    REQUIRE(text_shaper_prepare_call_count() - after_warm == 1);

    // Painting again at the new width hits the cache (no further prepare()).
    const uint64_t after_resize = text_shaper_prepare_call_count();
    RecordingCanvas again;
    label->paint(again);
    REQUIRE(text_shaper_prepare_call_count() - after_resize == 0);
}

TEST_CASE("Label attributed cache distinguishes nowrap from multiline",
          "[view][widget][label-cache][attributed]") {
    Label label("alpha beta gamma delta");
    AttributedString attributed;
    TextSpan span;
    span.text = "alpha beta gamma delta";
    span.font_family = "Inter";
    span.font_size = 12.0f;
    attributed.append(span);
    label.set_attributed_string(std::move(attributed));
    label.set_bounds({0, 0, 45, 100});
    label.set_multi_line(false);

    RecordingCanvas one_line;
    label.paint(one_line);
    const auto after_one_line = text_shaper_prepare_call_count();
    const auto one_line_fills = commands_of(one_line, DrawCommand::Type::fill_text);
    REQUIRE_FALSE(one_line_fills.empty());
    const float first_baseline = one_line_fills.front().f[1];
    CHECK(std::all_of(one_line_fills.begin(), one_line_fills.end(),
                      [&](const auto& command) {
                          return command.f[1] == Catch::Approx(first_baseline);
                      }));

    label.set_multi_line(true);
    RecordingCanvas multiline;
    label.paint(multiline);
    // The line-layout policy changed, so the layout cache is rebuilt, but the
    // attributed glyph preparation remains valid and is reused.
    CHECK(text_shaper_prepare_call_count() == after_one_line);
    const auto multiline_fills = commands_of(multiline, DrawCommand::Type::fill_text);
    REQUIRE(multiline_fills.size() > 1);
    CHECK(std::any_of(multiline_fills.begin(), multiline_fills.end(),
                      [&](const auto& command) {
                          return command.f[1] != Catch::Approx(multiline_fills.front().f[1]);
                      }));
}

TEST_CASE("Label reuses attributed preparation across identical paints",
          "[view][widget][label-cache][attributed]") {
    Label label("1111 9999");
    label.set_bounds({0, 0, 48, 80});
    label.set_multi_line(true);
    label.set_font_variant("tabular-nums");
    AttributedString attributed;
    TextSpan first;
    first.text = "1111 ";
    first.font_family = "Inter";
    first.font_size = 12.0f;
    attributed.append(first);
    TextSpan second = first;
    second.text = "9999";
    second.font_weight = 700;
    attributed.append(second);
    label.set_attributed_string(std::move(attributed));

    RecordingCanvas first_paint;
    label.paint(first_paint);
    const auto after_first = text_shaper_prepare_call_count();
    REQUIRE(commands_of(first_paint, DrawCommand::Type::fill_text).size() >= 2);

    RecordingCanvas second_paint;
    label.paint(second_paint);
    CHECK(text_shaper_prepare_call_count() == after_first);
    CHECK(fill_text_signature(first_paint) == fill_text_signature(second_paint));
}

TEST_CASE("Label attributed cache follows an inherited family change",
          "[view][widget][label-cache][attributed][inheritance]") {
    View parent;
    parent.set_inheritable_font_family("Inter");
    auto owned = std::make_unique<Label>("alpha beta gamma");
    auto* label = owned.get();
    AttributedString attributed;
    TextSpan span;
    span.text = "alpha beta gamma";
    span.inherit_font_family = true;
    attributed.append(span);
    label->set_attributed_string(std::move(attributed));
    label->set_bounds({0, 0, 80, 100});
    label->set_multi_line(true);
    parent.add_child(std::move(owned));

    RecordingCanvas first;
    label->paint(first);
    parent.set_inheritable_font_family("Courier");
    RecordingCanvas second;
    label->paint(second);
    const auto fonts = commands_of(second, DrawCommand::Type::set_font_full);
    REQUIRE_FALSE(fonts.empty());
    CHECK(std::any_of(fonts.begin(), fonts.end(), [](const auto& command) {
        return command.text == "Courier";
    }));
}

TEST_CASE("clearing attributed text invalidates Label layout",
          "[view][widget][label-cache][attributed][layout]") {
    Label label("wide");
    AttributedString attributed;
    TextSpan span;
    span.text = "wide";
    span.font_size = 48.0f;
    attributed.append(span);
    label.set_attributed_string(std::move(attributed));
    label.clear_layout_dirty();
    label.clear_attributed_string();
    CHECK(label.layout_dirty());
}

TEST_CASE("Label captured single-line fallback reflows attributed spans without losing style",
          "[view][widget][label-cache][attributed]") {
    Label label("alpha beta gamma");
    label.set_font_family("Inter");
    label.set_font_size(12.0f);

    AttributedString attributed;
    TextSpan first;
    first.text = "alpha ";
    first.font_family = "Inter";
    first.font_size = 10.0f;
    first.font_weight = 400;
    first.color = Color::rgba8(255, 0, 0);
    attributed.append(first);
    TextSpan second;
    second.text = "beta gamma";
    second.font_family = "Inter";
    second.font_size = 14.0f;
    second.font_weight = 700;
    second.italic = true;
    second.letter_spacing = 2.0f;
    second.color = Color::rgba8(0, 255, 0);
    attributed.append(second);
    label.set_attributed_string(std::move(attributed));

    const auto face = resolved_face_identity("Inter", 400.0f);
    label.set_bounds({0, 0, 200, 60});
    label.set_cached_line_boxes({{0, 0, 100, 15, 0, 16}}, 200.0f,
                                face, true);

    // A narrower Yoga constraint invalidates the captured one-line decision.
    // Height must grow with the same attributed reflow paint will use.
    const float narrow_height = label.measured_height(50.0f);
    CHECK(narrow_height > label.intrinsic_height());

    label.set_bounds({0, 0, 50, narrow_height});
    RecordingCanvas canvas;
    label.paint(canvas);

    const auto fills = commands_of(canvas, DrawCommand::Type::fill_text);
    REQUIRE(fills.size() >= 2);
    bool saw_regular = false;
    bool saw_bold_italic_tracked = false;
    for (const auto& command : canvas.commands()) {
        if (command.type != DrawCommand::Type::set_font_full) continue;
        if (command.f[0] == Catch::Approx(10.0f) &&
            command.f[1] == Catch::Approx(400.0f))
            saw_regular = true;
        if (command.f[0] == Catch::Approx(14.0f) &&
            command.f[1] == Catch::Approx(700.0f) &&
            command.f[2] == Catch::Approx(1.0f) &&
            command.f[3] == Catch::Approx(2.0f))
            saw_bold_italic_tracked = true;
    }
    CHECK(saw_regular);
    CHECK(saw_bold_italic_tracked);
}

TEST_CASE("Label mixed-size attributed runs share measurement and paint metrics",
          "[view][widget][label-cache][attributed][metrics]") {
    Label small("small BIG");
    small.set_font_family("Inter");
    small.set_font_size(10.0f);

    Label mixed("small BIG");
    mixed.set_font_family("Inter");
    mixed.set_font_size(10.0f);
    AttributedString attributed;
    TextSpan first;
    first.text = "small ";
    first.font_family = "Inter";
    first.font_size = 10.0f;
    attributed.append(first);
    TextSpan second;
    second.text = "BIG";
    second.font_family = "Inter";
    second.font_size = 28.0f;
    second.font_weight = 700;
    attributed.append(second);
    mixed.set_attributed_string(std::move(attributed));

    CHECK(mixed.intrinsic_height() > small.intrinsic_height() * 2.0f);
    CHECK(mixed.baseline_y() > small.baseline_y() * 2.0f);
    CHECK(mixed.intrinsic_width() > small.intrinsic_width());

    mixed.set_bounds({0, 0, 500, mixed.intrinsic_height()});
    RecordingCanvas canvas;
    mixed.paint(canvas);
    const auto fills = commands_of(canvas, DrawCommand::Type::fill_text);
    REQUIRE(fills.size() >= 2);
    for (const auto& fill : fills)
        CHECK(fill.f[1] == Catch::Approx(mixed.baseline_y()));
}

TEST_CASE("Label automatic attributed line boxes use per-line metrics",
          "[view][widget][label-cache][attributed][metrics]") {
    Label label("BIG\nsmall");
    AttributedString attributed;
    TextSpan large;
    large.text = "BIG\n";
    large.font_family = "Inter";
    large.font_size = 28.0f;
    attributed.append(large);
    TextSpan small;
    small.text = "small";
    small.font_family = "Inter";
    small.font_size = 10.0f;
    attributed.append(small);
    label.set_attributed_string(std::move(attributed));
    label.set_multi_line(true);

    const float measured = label.measured_height(200.0f);
    TextShaper shaper;
    AttributedString expected_text;
    TextSpan expected_large;
    expected_large.text = "BIG\n";
    expected_large.font_family = "Inter";
    expected_large.font_size = 28.0f;
    expected_text.append(expected_large);
    TextSpan expected_small;
    expected_small.text = "small";
    expected_small.font_family = "Inter";
    expected_small.font_size = 10.0f;
    expected_text.append(expected_small);
    const auto prepared = shaper.prepare(expected_text);
    const auto layout = shaper.layout_with_lines(prepared, 200.0f);
    REQUIRE(layout.lines.size() == 2);
    CHECK(measured == Catch::Approx(std::ceil(layout.total_height)));
    CHECK(measured < std::ceil(prepared.line_height() * 2.0f));

    label.set_bounds({0, 0, 200, measured});
    label.set_vertical_align(TextVerticalAlign::top);
    RecordingCanvas canvas;
    label.paint(canvas);
    const auto fills = commands_of(canvas, DrawCommand::Type::fill_text);
    REQUIRE(fills.size() == 2);
    CHECK(fills[1].f[1] - fills[0].f[1] ==
          Catch::Approx(layout.lines[0].height - layout.lines[0].ascent +
                        layout.lines[1].ascent));
}

TEST_CASE("Label attributed mutation invalidates an older captured line basis",
          "[view][widget][label-cache][attributed]") {
    Label label("alpha beta");
    label.set_font_family("Inter");
    label.set_bounds({0, 0, 100, 40});
    label.set_cached_line_boxes(
        {{0, 0, 100, 16, 0, 10}}, 100.0f,
        "captured-inter-regular", true);
    REQUIRE(label.cached_line_boxes().size() == 1);

    AttributedString replacement;
    TextSpan span;
    span.text = "alpha beta";
    span.font_weight = 800;
    replacement.append(span);
    label.set_attributed_string(std::move(replacement));

    CHECK(label.cached_line_boxes().empty());
}

TEST_CASE("Label styled clamp paints ellipsis on an empty visible line",
          "[view][widget][label-cache][attributed][ellipsis]") {
    Label label("alpha\n\nbeta");
    label.set_multi_line(true);
    label.set_line_clamp(2);
    label.set_bounds({0, 0, 100, 60});
    AttributedString attributed;
    TextSpan span;
    span.text = "alpha\n\nbeta";
    span.color = Color::rgba8(12, 34, 56);
    attributed.append(span);
    label.set_attributed_string(std::move(attributed));

    RecordingCanvas canvas;
    label.paint(canvas);
    const auto fills = commands_of(canvas, DrawCommand::Type::fill_text);
    REQUIRE(fills.size() == 2);
    CHECK(fills[0].text == "alpha");
    CHECK(fills[1].text == "\xe2\x80\xa6");
}

TEST_CASE("Label attributed ellipsis aligns the truncated width inside its bounds",
          "[view][widget][attributed][ellipsis]") {
    for (const auto align : {LabelAlign::center, LabelAlign::right}) {
        Label label("alpha beta gamma delta");
        label.set_bounds({0, 0, 60, 24});
        label.set_text_align(align);
        label.set_white_space_nowrap(true);
        label.set_multi_line(false);
        label.set_text_overflow_ellipsis(true);
        AttributedString attributed;
        TextSpan first;
        first.text = "alpha beta ";
        first.font_family = "Inter";
        attributed.append(first);
        TextSpan second = first;
        second.text = "gamma delta";
        second.font_weight = 700;
        attributed.append(second);
        label.set_attributed_string(std::move(attributed));

        RecordingCanvas canvas;
        label.paint(canvas);
        const auto fills = commands_of(canvas, DrawCommand::Type::fill_text);
        REQUIRE_FALSE(fills.empty());
        CHECK(fills.front().f[0] >= 0.0f);
    }
}

TEST_CASE("Label attributed explicit none cancels a dominant decoration for that run",
          "[view][widget][attributed][decoration]") {
    Label label("underplain");
    label.set_bounds({0, 0, 200, 24});
    label.set_text_decoration(Label::TextDecoration::underline);
    AttributedString attributed;
    TextSpan under;
    under.text = "under";
    under.font_family = "Inter";
    attributed.append(under);
    TextSpan plain = under;
    plain.text = "plain";
    plain.decoration = TextDecoration::none;
    plain.decoration_override = true;
    attributed.append(plain);
    label.set_attributed_string(std::move(attributed));

    RecordingCanvas canvas;
    label.paint(canvas);
    REQUIRE(commands_of(canvas, DrawCommand::Type::fill_text).size() >= 2);
    CHECK(canvas.count(DrawCommand::Type::stroke_line) == 1);
}

TEST_CASE("Label attributed ellipsis reserves space across the whole styled line",
          "[view][widget][attributed][ellipsis]") {
    RecordingCanvas metrics;
    metrics.set_font_full("Inter", 14.0f, 400, 0, 0.0f);
    const float first_width = metrics.measure_text("aaaa");

    Label label("aaaabbbb");
    label.set_bounds({0, 0, first_width, 24});
    label.set_white_space_nowrap(true);
    label.set_multi_line(false);
    label.set_text_overflow_ellipsis(true);
    AttributedString attributed;
    TextSpan first;
    first.text = "aaaa";
    first.font_family = "Inter";
    attributed.append(first);
    TextSpan second = first;
    second.text = "bbbb";
    second.font_weight = 700;
    attributed.append(second);
    label.set_attributed_string(std::move(attributed));

    RecordingCanvas canvas;
    label.paint(canvas);
    const auto fills = commands_of(canvas, DrawCommand::Type::fill_text);
    REQUIRE(fills.size() == 1);
    CHECK(fills[0].text.ends_with("\xe2\x80\xa6"));
}

TEST_CASE("Label rejects a cached line that splits a UTF-16 surrogate pair",
          "[view][widget][label-cache][utf16]") {
    Label label(std::string("A") + "\xf0\x9f\x98\x80" + "B");
    label.set_font_family("Inter");
    label.set_font_size(14.0f);
    label.set_bounds({0, 0, 100, 24});
    label.set_cached_line_boxes(
        {{0, 0, 100, 18, 1, 1}}, 100.0f,
        resolved_face_identity("Inter", 400.0f), true);
    REQUIRE(label.cached_line_boxes().empty());

    Label::reset_line_break_path_counts();
    RecordingCanvas canvas;
    label.paint(canvas);
    CHECK(Label::line_break_path_counts().cached == 0);
}

TEST_CASE("Label rejects invalid captured line geometry at its public boundary",
          "[view][widget][label-cache][validation]") {
    Label label("valid");
    const auto face = resolved_face_identity("Inter", 400.0f);
    label.set_cached_line_boxes(
        {{0, 0, -1, 18, 0, 5}}, 100.0f, face, true);
    CHECK(label.cached_line_boxes().empty());
    label.set_cached_line_boxes(
        {{0, 0, 20, 18, 0, 5}}, 0.0f, face, true);
    CHECK(label.cached_line_boxes().empty());
    label.set_cached_line_boxes(
        {{0, 0, 20, 18, 0, 5}}, 100.0f, "", true);
    CHECK(label.cached_line_boxes().empty());
    CHECK(label.captured_wrap_fallback());
}

TEST_CASE("Label ellipsis preserves a captured single-line horizontal offset",
          "[view][widget][label-cache][ellipsis][alignment]") {
    Label label("short");
    label.set_font_family("Inter");
    label.set_font_size(14.0f);
    label.set_text_align(LabelAlign::center);
    label.set_text_overflow_ellipsis(true);
    label.set_bounds({0, 0, 100, 24});
    const auto face = resolved_face_identity("Inter", 400.0f);
    label.set_cached_line_boxes(
        {{30, 0, 40, 18, 0, 5}}, 100.0f,
        face, false);

    RecordingCanvas canvas;
    label.paint(canvas);
    const auto fills = commands_of(canvas, DrawCommand::Type::fill_text);
    REQUIRE(fills.size() == 1);
    if (face.empty()) {
        CHECK(label.cached_line_boxes().empty());
        CHECK(fills[0].f[0] == Catch::Approx(50.0f));
        return;
    }
    CHECK(fills[0].f[0] == Catch::Approx(30.0f));
}

TEST_CASE("Label re-shapes when font size or line height changes",
          "[view][widget][label-cache]") {
    auto label = make_wrapped_label();

    RecordingCanvas warm;
    label->paint(warm);
    uint64_t mark = text_shaper_prepare_call_count();

    label->set_font_size(18.0f);
    RecordingCanvas after_fs;
    label->paint(after_fs);
    REQUIRE(text_shaper_prepare_call_count() - mark == 1);
    mark = text_shaper_prepare_call_count();

    label->set_line_height(40.0f);
    RecordingCanvas after_lh;
    label->paint(after_lh);
    REQUIRE(text_shaper_prepare_call_count() - mark == 1);
}

TEST_CASE("Label re-shapes when a font registration bumps the generation",
          "[view][widget][label-cache]") {
    // measure_segment() resamples the resolved typeface (and thus glyph
    // advances / wrap points) whenever font_registration_generation() changes —
    // e.g. an async register_font_url() completing after the first paint. The
    // cache key snapshots that generation so a late registration invalidates the
    // cached wrap instead of serving the fallback-measured layout forever.
    auto label = make_wrapped_label();

    RecordingCanvas warm;
    label->paint(warm);  // shapes once against the current font set
    const uint64_t after_warm = text_shaper_prepare_call_count();

    // Simulate a process-global font-state mutation (what async font
    // registration triggers) WITHOUT touching text / width / size / lh / break.
    pulp::canvas::bump_font_registration_generation();

    RecordingCanvas after;
    label->paint(after);

    // The generation bump alone must invalidate the cache → exactly one
    // fresh prepare(). (Before the font_gen key field, this was 0 — stale.)
    //
    // Guarded because the assertion only holds when font registration can
    // actually move the generation counter. The real
    // bump_font_registration_generation() is compiled only under PULP_HAS_SKIA;
    // otherwise it's a no-op stub (bundled_fonts.cpp / font_registry_stubs.cpp),
    // so the generation field in the cache key never changes → the key still
    // matches → cache hit → prepare() is NOT re-invoked (delta 0). The soft-wrap
    // cache IS still populated in a no-Skia build (the sibling prepare()-delta==1
    // tests above pass on no-GPU lanes); it's specifically the generation bump
    // that can't fire. uses_real_shaping() (tied to PULP_HAS_TEXT_SHAPING) is a
    // safe predicate: it's only ever true when PULP_HAS_SKIA is too, so it never
    // asserts when the bump is a stub — at worst it conservatively SKIPS the
    // check in the rare PULP_HAS_SKIA + PULP_TEXT_SHAPING=OFF config (real bump
    // compiled, but no shaper), which is acceptable. The fill_text assertion
    // below still verifies the label re-paints in every build configuration.
    if (pulp::canvas::global_text_shaper().uses_real_shaping()) {
        REQUIRE(text_shaper_prepare_call_count() - after_warm == 1);
    }
    REQUIRE(commands_of(after, DrawCommand::Type::fill_text).size() >= 2);
}
