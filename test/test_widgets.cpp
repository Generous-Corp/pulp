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

std::shared_ptr<SpriteStrip> make_sprite_strip(
    int total_width,
    int total_height,
    int frame_count,
    SpriteStrip::Orientation orientation) {
    std::vector<uint8_t> pixels(static_cast<size_t>(total_width) *
                                static_cast<size_t>(total_height) * 4u,
                                0x7f);
    auto strip = std::make_shared<SpriteStrip>();
    strip->load(pixels.data(), pixels.size(), total_width, total_height,
                frame_count, orientation);
    return strip;
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

    // Below the threshold — bumped multiplier.
    Label tiny("snapshot");
    tiny.set_font_size(10.0f);
    REQUIRE(tiny.intrinsic_height() >= 10.0f * 1.5f);

    Label small("ok");
    small.set_font_size(11.5f);
    REQUIRE(small.intrinsic_height() >= 11.5f * 1.5f);

    // At/above the threshold — still reserves a positive metric-derived
    // line box. Exact values depend on whether this build has Skia text
    // shaping or the non-Skia estimator.
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
    const float lh_single = 12.0f * 1.4f;
    single.set_line_height(lh_single);
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
    const float lh_13 = 13.0f * 1.4f;
    three.set_line_height(lh_13);
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
    const float lh = fs * 1.4f;

    // "Title\n" — counts as ONE line, not two.
    Label trailing("Title\n");
    trailing.set_multi_line(true);
    trailing.set_font_size(fs);
    trailing.set_line_height(lh);
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
    const float lh = 12.0f * 1.4f;
    clamped.set_line_height(lh);
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
    const float lh = 13.0f * 1.4f;
    desc.set_line_height(lh);

    // Very wide: the text fits on one line → measured height collapses
    // to one line (= ceil(lh), since the shaper path ceils to a sub-
    // pixel-safe integer).
    REQUIRE_THAT(desc.measured_height(10000.0f), WithinAbs(std::ceil(lh), 0.5f));

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
    const float snap_lh = 10.0f * 1.6f;
    snap.set_line_height(snap_lh);
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

TEST_CASE("Knob value clamping", "[view][widget]") {
    Knob knob;
    knob.set_value(0.5f);
    REQUIRE_THAT(knob.value(), WithinAbs(0.5, 0.001));

    knob.set_value(1.5f);
    REQUIRE_THAT(knob.value(), WithinAbs(1.0, 0.001));

    knob.set_value(-0.5f);
    REQUIRE_THAT(knob.value(), WithinAbs(0.0, 0.001));
}

TEST_CASE("Knob renders arcs and indicator", "[view][widget]") {
    Knob knob;
    knob.set_bounds({0, 0, 48, 48});
    knob.set_value(0.5f);
    knob.set_label("Gain");

    RecordingCanvas canvas;
    knob.paint(canvas);

    // Should draw: track arc, value arc, thumb line, label text
    REQUIRE(canvas.count(DrawCommand::Type::stroke_arc) == 2);
    REQUIRE(canvas.count(DrawCommand::Type::stroke_line) == 1);
    REQUIRE(canvas.count(DrawCommand::Type::fill_text) >= 1);
}

TEST_CASE("Knob with format function shows value text", "[view][widget]") {
    Knob knob;
    knob.set_bounds({0, 0, 48, 48});
    knob.set_value(0.75f);
    knob.set_format([](float v) { return std::to_string(static_cast<int>(v * 100)) + "%"; });

    RecordingCanvas canvas;
    knob.paint(canvas);

    // Should have label text + value text
    REQUIRE(canvas.count(DrawCommand::Type::fill_text) >= 1);
}

TEST_CASE("Fader value clamping", "[view][widget]") {
    Fader fader;
    fader.set_value(0.7f);
    REQUIRE_THAT(fader.value(), WithinAbs(0.7, 0.001));

    fader.set_value(2.0f);
    REQUIRE_THAT(fader.value(), WithinAbs(1.0, 0.001));
}

TEST_CASE("Fader renders track and thumb", "[view][widget]") {
    Fader fader;
    fader.set_bounds({0, 0, 24, 200});
    fader.set_value(0.6f);
    fader.set_label("Volume");

    RecordingCanvas canvas;
    fader.paint(canvas);

    // Track + fill = 2 rounded rects, thumb = 1 circle, label text
    REQUIRE(canvas.count(DrawCommand::Type::fill_rounded_rect) == 2);
    REQUIRE(canvas.count(DrawCommand::Type::fill_circle) == 1);
    REQUIRE(canvas.count(DrawCommand::Type::fill_text) >= 1);
}

TEST_CASE("Fader horizontal orientation", "[view][widget]") {
    Fader fader;
    fader.set_orientation(Fader::Orientation::horizontal);
    fader.set_bounds({0, 0, 200, 24});
    fader.set_value(0.5f);

    RecordingCanvas canvas;
    fader.paint(canvas);

    REQUIRE(canvas.count(DrawCommand::Type::fill_rounded_rect) == 2);
    REQUIRE(canvas.count(DrawCommand::Type::fill_circle) == 1);
}

TEST_CASE("Knob and Fader render loaded sprite strips",
          "[view][widget][coverage]") {
    auto knob_strip = make_sprite_strip(2, 9, 3, SpriteStrip::Orientation::vertical);
    Knob knob;
    knob.set_bounds({0, 0, 20, 30});
    knob.set_value(0.75f);
    knob.set_sprite_strip(knob_strip);

    RecordingCanvas knob_canvas;
    knob.paint(knob_canvas);

    REQUIRE(knob_canvas.count(DrawCommand::Type::draw_image) == 1);
    REQUIRE(knob_canvas.count(DrawCommand::Type::save) == 1);
    REQUIRE(knob_canvas.count(DrawCommand::Type::restore) == 1);
    REQUIRE(knob_canvas.count(DrawCommand::Type::stroke_arc) == 0);
    REQUIRE(knob_canvas.count(DrawCommand::Type::stroke_line) == 0);

    auto knob_clip = commands_of(knob_canvas, DrawCommand::Type::clip_rect).front();
    REQUIRE_THAT(knob_clip.f[2], WithinAbs(20.0, 0.001));
    REQUIRE_THAT(knob_clip.f[3], WithinAbs(30.0, 0.001));

    auto knob_scale = commands_of(knob_canvas, DrawCommand::Type::scale).front();
    REQUIRE_THAT(knob_scale.f[0], WithinAbs(10.0, 0.001));
    REQUIRE_THAT(knob_scale.f[1], WithinAbs(10.0, 0.001));

    auto knob_translate = commands_of(knob_canvas, DrawCommand::Type::translate).front();
    REQUIRE_THAT(knob_translate.f[0], WithinAbs(0.0, 0.001));
    REQUIRE_THAT(knob_translate.f[1], WithinAbs(-6.0, 0.001));

    auto knob_image = commands_of(knob_canvas, DrawCommand::Type::draw_image).front();
    REQUIRE(knob_image.text.size() == knob_strip->data_size());
    REQUIRE_THAT(knob_image.f[2], WithinAbs(2.0, 0.001));
    REQUIRE_THAT(knob_image.f[3], WithinAbs(9.0, 0.001));

    auto fader_strip = make_sprite_strip(12, 4, 3, SpriteStrip::Orientation::horizontal);
    Fader fader;
    fader.set_bounds({0, 0, 40, 20});
    fader.set_value(0.5f);
    fader.set_sprite_strip(fader_strip);

    RecordingCanvas fader_canvas;
    fader.paint(fader_canvas);

    REQUIRE(fader_canvas.count(DrawCommand::Type::draw_image) == 1);
    REQUIRE(fader_canvas.count(DrawCommand::Type::save) == 1);
    REQUIRE(fader_canvas.count(DrawCommand::Type::restore) == 1);
    REQUIRE(fader_canvas.count(DrawCommand::Type::fill_rounded_rect) == 0);
    REQUIRE(fader_canvas.count(DrawCommand::Type::fill_circle) == 0);

    auto fader_clip = commands_of(fader_canvas, DrawCommand::Type::clip_rect).front();
    REQUIRE_THAT(fader_clip.f[2], WithinAbs(40.0, 0.001));
    REQUIRE_THAT(fader_clip.f[3], WithinAbs(20.0, 0.001));

    auto fader_scale = commands_of(fader_canvas, DrawCommand::Type::scale).front();
    REQUIRE_THAT(fader_scale.f[0], WithinAbs(10.0, 0.001));
    REQUIRE_THAT(fader_scale.f[1], WithinAbs(5.0, 0.001));

    auto fader_translate = commands_of(fader_canvas, DrawCommand::Type::translate).front();
    REQUIRE_THAT(fader_translate.f[0], WithinAbs(-4.0, 0.001));
    REQUIRE_THAT(fader_translate.f[1], WithinAbs(0.0, 0.001));

    auto fader_image = commands_of(fader_canvas, DrawCommand::Type::draw_image).front();
    REQUIRE(fader_image.text.size() == fader_strip->data_size());
    REQUIRE_THAT(fader_image.f[2], WithinAbs(12.0, 0.001));
    REQUIRE_THAT(fader_image.f[3], WithinAbs(4.0, 0.001));
}

// ── RangeSlider (pulp issue-966) ────────────────────────────────────────────

TEST_CASE("RangeSlider default state matches HTML <input type=\"range\">",
          "[view][widget][issue-966]") {
    RangeSlider rs;
    REQUIRE_THAT(rs.min_value(), WithinAbs(0.0, 0.0001));
    REQUIRE_THAT(rs.max_value(), WithinAbs(1.0, 0.0001));
    // Default step is 0 (continuous) — HTML defaults to 1 but plugins
    // overwhelmingly want continuous values. Callers opt into stepping.
    REQUIRE_THAT(rs.step(), WithinAbs(0.0, 0.0001));
    REQUIRE_THAT(rs.value(), WithinAbs(0.0, 0.0001));
    REQUIRE(rs.orientation() == RangeSlider::Orientation::horizontal);
    REQUIRE_FALSE(rs.has_accent_color());
}

TEST_CASE("RangeSlider clamps value to [min,max]", "[view][widget][issue-966]") {
    RangeSlider rs;
    rs.set_min(-1.0f);
    rs.set_max(1.0f);

    rs.set_value(2.0f);
    REQUIRE_THAT(rs.value(), WithinAbs(1.0, 0.0001));

    rs.set_value(-5.0f);
    REQUIRE_THAT(rs.value(), WithinAbs(-1.0, 0.0001));

    rs.set_value(0.5f);
    REQUIRE_THAT(rs.value(), WithinAbs(0.5, 0.0001));
}

TEST_CASE("RangeSlider quantises to step", "[view][widget][issue-966]") {
    RangeSlider rs;
    rs.set_min(0.0f);
    rs.set_max(1.0f);
    rs.set_step(0.1f);

    rs.set_value(0.34f);
    REQUIRE_THAT(rs.value(), WithinAbs(0.3, 0.0001));

    rs.set_value(0.36f);
    REQUIRE_THAT(rs.value(), WithinAbs(0.4, 0.0001));

    // Step that doesn't divide the range cleanly: max reachable step
    // before exceeding hi must clamp back inside the range.
    rs.set_step(0.3f);
    rs.set_value(1.0f);
    REQUIRE(rs.value() <= 1.0f + 1e-5f);
    REQUIRE(rs.value() >= 0.0f);
}

TEST_CASE("RangeSlider step=0 means continuous", "[view][widget][issue-966]") {
    RangeSlider rs;
    rs.set_min(0.0f);
    rs.set_max(100.0f);
    rs.set_step(0.0f);
    rs.set_value(33.7f);
    REQUIRE_THAT(rs.value(), WithinAbs(33.7, 0.0001));
}

TEST_CASE("RangeSlider re-clamps when bounds change", "[view][widget][issue-966]") {
    RangeSlider rs;
    rs.set_min(0.0f);
    rs.set_max(10.0f);
    rs.set_value(7.0f);
    REQUIRE_THAT(rs.value(), WithinAbs(7.0, 0.0001));

    // Tighten the upper bound — the existing value falls outside and
    // must snap back.
    rs.set_max(5.0f);
    REQUIRE_THAT(rs.value(), WithinAbs(5.0, 0.0001));

    // Raise the lower bound past the value — same idea in the other
    // direction.
    rs.set_min(6.0f);
    REQUIRE_THAT(rs.value(), WithinAbs(6.0, 0.0001));
}

TEST_CASE("RangeSlider invalid range collapses to min", "[view][widget][issue-966]") {
    RangeSlider rs;
    rs.set_min(10.0f);
    rs.set_max(0.0f);  // invalid: max < min
    rs.set_value(5.0f);
    // Per HTMLInputElement: invalid range pins value to min.
    REQUIRE_THAT(rs.value(), WithinAbs(10.0, 0.0001));
}

TEST_CASE("RangeSlider drag dispatches change with quantised value",
          "[view][widget][issue-966]") {
    RangeSlider rs;
    rs.set_bounds({0, 0, 200, 24});
    rs.set_min(0.0f);
    rs.set_max(1.0f);
    rs.set_step(0.25f);

    std::vector<float> changes;
    rs.on_change = [&](float v) { changes.push_back(v); };

    // Mouse-down at x=100 (50% across) → expected snap to 0.5.
    MouseEvent down;
    down.position = {100, 12};
    down.is_down = true;
    rs.on_mouse_event(down);

    REQUIRE(changes.size() == 1);
    REQUIRE_THAT(changes.back(), WithinAbs(0.5, 0.0001));
    REQUIRE_THAT(rs.value(), WithinAbs(0.5, 0.0001));

    // Drag to x=180 (90%) → should snap to 1.0.
    rs.on_mouse_drag({180, 12});
    REQUIRE_THAT(rs.value(), WithinAbs(1.0, 0.0001));
    REQUIRE(changes.size() >= 2);
    REQUIRE_THAT(changes.back(), WithinAbs(1.0, 0.0001));

    // Drag to x=10 (5%) → should snap to 0.
    rs.on_mouse_drag({10, 12});
    REQUIRE_THAT(rs.value(), WithinAbs(0.0, 0.0001));

    // Mouse up → dragging stops; subsequent drags must not move value.
    MouseEvent up = down; up.is_down = false;
    rs.on_mouse_event(up);
    rs.on_mouse_drag({180, 12});
    REQUIRE_THAT(rs.value(), WithinAbs(0.0, 0.0001));
}

TEST_CASE("RangeSlider vertical orientation maps y correctly",
          "[view][widget][issue-966]") {
    RangeSlider rs;
    rs.set_bounds({0, 0, 24, 200});
    rs.set_orientation(RangeSlider::Orientation::vertical);
    rs.set_min(0.0f);
    rs.set_max(1.0f);

    // y=0 is top → max value (1.0).
    MouseEvent ev;
    ev.position = {12, 0};
    ev.is_down = true;
    rs.on_mouse_event(ev);
    REQUIRE_THAT(rs.value(), WithinAbs(1.0, 0.0001));

    // y=200 is bottom → min value (0.0).
    rs.on_mouse_drag({12, 200});
    REQUIRE_THAT(rs.value(), WithinAbs(0.0, 0.0001));

    // y=100 → halfway = 0.5.
    rs.on_mouse_drag({12, 100});
    REQUIRE_THAT(rs.value(), WithinAbs(0.5, 0.001));
}

TEST_CASE("RangeSlider renders track + fill + handle",
          "[view][widget][issue-966]") {
    RangeSlider rs;
    rs.set_bounds({0, 0, 200, 24});
    rs.set_value(0.5f);

    RecordingCanvas canvas;
    rs.paint(canvas);

    // Track + active fill = 2 rounded rects, handle = 1 circle.
    REQUIRE(canvas.count(DrawCommand::Type::fill_rounded_rect) == 2);
    REQUIRE(canvas.count(DrawCommand::Type::fill_circle) == 1);
}

TEST_CASE("RangeSlider vertical paint draws lower fill and inverted handle",
          "[view][widget][issue-966][coverage]") {
    RangeSlider rs;
    rs.set_bounds({0, 0, 24, 200});
    rs.set_orientation(RangeSlider::Orientation::vertical);
    rs.set_track_thickness(6.0f);
    rs.set_value(0.25f);

    RecordingCanvas canvas;
    rs.paint(canvas);

    auto rects = commands_of(canvas, DrawCommand::Type::fill_rounded_rect);
    REQUIRE(rects.size() == 2);

    // Track is centered horizontally and spans the full vertical bounds.
    REQUIRE_THAT(rects[0].f[0], WithinAbs(9.0, 0.001));
    REQUIRE_THAT(rects[0].f[1], WithinAbs(0.0, 0.001));
    REQUIRE_THAT(rects[0].f[2], WithinAbs(6.0, 0.001));
    REQUIRE_THAT(rects[0].f[3], WithinAbs(200.0, 0.001));

    // Vertical fill grows upward from the bottom for the current value.
    REQUIRE_THAT(rects[1].f[0], WithinAbs(9.0, 0.001));
    REQUIRE_THAT(rects[1].f[1], WithinAbs(150.0, 0.001));
    REQUIRE_THAT(rects[1].f[2], WithinAbs(6.0, 0.001));
    REQUIRE_THAT(rects[1].f[3], WithinAbs(50.0, 0.001));

    auto handles = commands_of(canvas, DrawCommand::Type::fill_circle);
    REQUIRE(handles.size() == 1);
    REQUIRE_THAT(handles.front().f[0], WithinAbs(12.0, 0.001));
    REQUIRE_THAT(handles.front().f[1], WithinAbs(146.0, 0.001));
    REQUIRE_THAT(handles.front().f[2], WithinAbs(8.0, 0.001));
}

TEST_CASE("RangeSlider at minimum draws track but skips empty fill",
          "[view][widget][issue-966]") {
    RangeSlider rs;
    rs.set_bounds({0, 0, 200, 24});
    rs.set_value(0.0f);  // exactly at min — no active fill to draw

    RecordingCanvas canvas;
    rs.paint(canvas);

    // Only the background track rounded rect — fill is zero-width and
    // skipped. Handle still renders.
    REQUIRE(canvas.count(DrawCommand::Type::fill_rounded_rect) == 1);
    REQUIRE(canvas.count(DrawCommand::Type::fill_circle) == 1);
}

TEST_CASE("RangeSlider accent color overrides theme fill",
          "[view][widget][issue-966]") {
    RangeSlider rs;
    rs.set_bounds({0, 0, 100, 20});
    rs.set_value(0.6f);
    rs.set_accent_color(Color::rgba8(255, 64, 32, 255));
    REQUIRE(rs.has_accent_color());

    rs.clear_accent_color();
    REQUIRE_FALSE(rs.has_accent_color());
}

TEST_CASE("Toggle state", "[view][widget]") {
    Toggle toggle;
    REQUIRE_FALSE(toggle.is_on());

    toggle.set_on(true);
    REQUIRE(toggle.is_on());
}

TEST_CASE("Toggle renders switch", "[view][widget]") {
    Toggle toggle;
    toggle.set_bounds({0, 0, 50, 30});
    toggle.set_on(true);
    toggle.set_label("Bypass");

    RecordingCanvas canvas;
    toggle.paint(canvas);

    // Background rounded rect + thumb circle + label
    REQUIRE(canvas.count(DrawCommand::Type::fill_rounded_rect) == 1);
    REQUIRE(canvas.count(DrawCommand::Type::fill_circle) == 1);
    REQUIRE(canvas.count(DrawCommand::Type::fill_text) >= 1);
}

TEST_CASE("Audio widgets render declarative schemas and invalid schema fallback",
          "[view][widget][schema]") {
    Knob knob;
    knob.set_bounds({0, 0, 80, 80});
    knob.set_value(0.5f);
    knob.set_widget_schema(R"json({
        "elements": [
            { "type": "arc", "color": "control.fill", "radius": "80%", "width": 4,
              "startAngle": -120, "sweepAngle": { "bind": "value", "range": [0, 240] } },
            { "type": "circle", "color": "control.thumb", "radius": "25%" },
            { "type": "line", "color": "text.primary", "innerRadius": "15%",
              "outerRadius": "65%", "angle": { "bind": "value", "range": [-90, 90] } },
            { "type": "rect", "color": "bg.surface", "cornerRadius": "5" },
            { "type": "text", "color": "text.primary", "text": "dB", "fontSize": 12 }
        ]
    })json");

    RecordingCanvas knob_canvas;
    knob.paint(knob_canvas);
    REQUIRE(knob_canvas.count(DrawCommand::Type::stroke_arc) == 1);
    REQUIRE(knob_canvas.count(DrawCommand::Type::fill_circle) == 1);
    REQUIRE(knob_canvas.count(DrawCommand::Type::stroke_line) == 1);
    REQUIRE(knob_canvas.count(DrawCommand::Type::fill_rounded_rect) == 1);
    REQUIRE(knob_canvas.count(DrawCommand::Type::fill_text) == 1);
    REQUIRE(commands_of(knob_canvas, DrawCommand::Type::fill_text).front().text == "dB");

    Fader fader;
    fader.set_bounds({0, 0, 32, 120});
    fader.set_value(0.25f);
    fader.set_widget_schema(R"json({
        "elements": [
            { "type": "rect", "color": "control.track" },
            { "type": "line", "color": "control.fill", "angle": 90,
              "innerRadius": "4", "outerRadius": "24" }
        ]
    })json");

    RecordingCanvas fader_canvas;
    fader.paint(fader_canvas);
    REQUIRE(fader_canvas.count(DrawCommand::Type::fill_rect) == 1);
    REQUIRE(fader_canvas.count(DrawCommand::Type::stroke_line) == 1);

    Toggle toggle;
    toggle.set_bounds({0, 0, 64, 32});
    toggle.set_on(true);
    toggle.set_widget_schema(R"json({
        "elements": [
            { "type": "circle", "color": "accent.primary", "radius": "12" }
        ]
    })json");

    RecordingCanvas toggle_canvas;
    toggle.paint(toggle_canvas);
    REQUIRE(toggle_canvas.count(DrawCommand::Type::fill_circle) == 1);

    Knob invalid;
    invalid.set_bounds({0, 0, 48, 48});
    invalid.set_widget_schema("{ not valid json");

    RecordingCanvas invalid_canvas;
    invalid.paint(invalid_canvas);
    REQUIRE(invalid_canvas.count(DrawCommand::Type::fill_rect) == 1);
}

TEST_CASE("Audio widget schemas reject malformed dimension tokens without error fallback",
          "[view][widget][schema][coverage][phase3]") {
    Knob knob;
    knob.set_bounds({0, 0, 80, 80});
    knob.set_value(0.5f);
    knob.set_widget_schema(R"json({
        "elements": [
            { "type": "arc", "color": "control.fill", "radius": "80%junk", "width": 4,
              "startAngle": -120, "sweepAngle": { "bind": "value", "range": [0, 240] } },
            { "type": "circle", "color": "control.thumb", "radius": "" },
            { "type": "line", "color": "text.primary", "innerRadius": "15 %",
              "outerRadius": "65x", "angle": { "bind": "value", "range": [-90, 90] } },
            { "type": "rect", "color": "bg.surface", "cornerRadius": "5x" }
        ]
    })json");

    RecordingCanvas canvas;
    knob.paint(canvas);
    REQUIRE(canvas.count(DrawCommand::Type::stroke_arc) == 1);
    REQUIRE(canvas.count(DrawCommand::Type::fill_circle) == 1);
    REQUIRE(canvas.count(DrawCommand::Type::stroke_line) == 1);
    REQUIRE(canvas.count(DrawCommand::Type::fill_rect) == 1);
    REQUIRE(canvas.count(DrawCommand::Type::fill_rounded_rect) == 0);
}

TEST_CASE("Audio widgets custom shader paths fall back on recording canvas",
          "[view][widget][shader][issue-493]") {
    Knob knob;
    knob.set_bounds({0, 0, 80, 80});
    knob.set_value(0.5f);
    knob.set_label("Drive");
    knob.set_format([](float) { return "50%"; });
    knob.set_custom_shader("uniform float time; half4 main(float2 p) { return half4(time); }");
    REQUIRE(knob.has_custom_shader());
    REQUIRE(knob.shader_uses_time());

    RecordingCanvas knob_canvas;
    knob.paint(knob_canvas);
    REQUIRE(knob_canvas.count(DrawCommand::Type::fill_rect) == 1);
    REQUIRE(knob_canvas.count(DrawCommand::Type::fill_text) >= 2);

    Fader fader;
    fader.set_bounds({0, 0, 24, 120});
    fader.set_label("Level");
    fader.set_custom_shader("half4 main(float2 p) { return half4(1); }");
    REQUIRE(fader.has_custom_shader());
    REQUIRE_FALSE(fader.shader_uses_time());

    RecordingCanvas fader_canvas;
    fader.paint(fader_canvas);
    REQUIRE(fader_canvas.count(DrawCommand::Type::fill_rect) == 1);
    REQUIRE(fader_canvas.count(DrawCommand::Type::fill_text) == 1);

    Toggle toggle;
    toggle.set_bounds({0, 0, 64, 32});
    toggle.set_on(true);
    toggle.set_label("On");
    toggle.set_custom_shader("uniform float time; half4 main(float2 p) { return half4(time); }");
    REQUIRE(toggle.has_custom_shader());
    REQUIRE(toggle.shader_uses_time());

    RecordingCanvas toggle_canvas;
    toggle.paint(toggle_canvas);
    REQUIRE(toggle_canvas.count(DrawCommand::Type::fill_rect) == 1);
    REQUIRE(toggle_canvas.count(DrawCommand::Type::fill_text) == 1);
}

TEST_CASE("Audio widgets minimal render style paints simplified branches",
          "[view][widget][style][issue-493]") {
    Knob knob;
    knob.set_bounds({0, 0, 64, 64});
    knob.set_render_style(WidgetRenderStyle::minimal);

    RecordingCanvas knob_canvas;
    knob.paint(knob_canvas);
    REQUIRE(knob_canvas.count(DrawCommand::Type::fill_circle) == 1);
    REQUIRE(knob_canvas.count(DrawCommand::Type::stroke_circle) == 1);
    REQUIRE(knob_canvas.count(DrawCommand::Type::stroke_arc) == 0);

    Fader fader;
    fader.set_bounds({0, 0, 28, 120});
    fader.set_render_style(WidgetRenderStyle::minimal);

    RecordingCanvas fader_canvas;
    fader.paint(fader_canvas);
    REQUIRE(fader_canvas.count(DrawCommand::Type::fill_rounded_rect) == 1);
    REQUIRE(fader_canvas.count(DrawCommand::Type::fill_circle) == 0);

    Meter meter;
    meter.set_bounds({0, 0, 8, 6});
    meter.set_render_style(WidgetRenderStyle::minimal);

    RecordingCanvas meter_canvas;
    meter.paint(meter_canvas);
    REQUIRE(meter_canvas.count(DrawCommand::Type::fill_rect) == 6);
    REQUIRE(meter_canvas.count(DrawCommand::Type::fill_rounded_rect) == 0);
}

TEST_CASE("Knob mouse paths update value, hover animation, and default reset",
          "[view][widget][interaction][issue-493]") {
    Knob knob;
    knob.set_value(0.25f);
    knob.set_default_value(0.75f);

    std::vector<float> changes;
    knob.on_change = [&](float v) { changes.push_back(v); };

    knob.on_mouse_enter();
    knob.advance_animations(1.0f);
    REQUIRE(knob.hover_glow() > 0.9f);

    knob.on_mouse_leave();
    knob.advance_animations(1.0f);
    REQUIRE(knob.hover_glow() < 0.1f);

    knob.on_mouse_down({0, 100});
    knob.on_mouse_drag({0, 55});
    REQUIRE(knob.value() > 0.25f);
    REQUIRE_FALSE(changes.empty());

    MouseEvent reset;
    reset.is_down = true;
    reset.click_count = 2;
    knob.on_mouse_event(reset);
    REQUIRE_THAT(knob.value(), WithinAbs(0.75, 0.001));
    REQUIRE_THAT(changes.back(), WithinAbs(0.75, 0.001));
}

TEST_CASE("Fader and toggle mouse paths dispatch clamped interactive values",
          "[view][widget][interaction][issue-493]") {
    Fader vertical;
    vertical.set_bounds({0, 0, 24, 100});
    std::vector<float> fader_changes;
    vertical.on_change = [&](float v) { fader_changes.push_back(v); };

    MouseEvent down;
    down.is_down = true;
    down.position = {12, 75};
    vertical.on_mouse_event(down);
    REQUIRE_THAT(vertical.value(), WithinAbs(0.25, 0.001));

    vertical.on_mouse_drag({12, 20});
    REQUIRE_THAT(vertical.value(), WithinAbs(0.80, 0.001));

    MouseEvent up = down;
    up.is_down = false;
    vertical.on_mouse_event(up);
    vertical.on_mouse_drag({12, 100});
    REQUIRE_THAT(vertical.value(), WithinAbs(0.80, 0.001));
    REQUIRE(fader_changes.size() >= 2);

    Fader horizontal;
    horizontal.set_bounds({0, 0, 200, 24});
    horizontal.set_orientation(Fader::Orientation::horizontal);
    MouseEvent horizontal_down;
    horizontal_down.is_down = true;
    horizontal_down.position = {50, 12};
    horizontal.on_mouse_event(horizontal_down);
    REQUIRE_THAT(horizontal.value(), WithinAbs(0.25, 0.001));

    Toggle toggle;
    std::vector<bool> toggle_changes;
    toggle.on_toggle = [&](bool v) { toggle_changes.push_back(v); };

    toggle.on_mouse_enter();
    toggle.advance_animations(1.0f);
    REQUIRE(toggle.hover_opacity() > 0.9f);

    toggle.on_mouse_down({4, 4});
    REQUIRE(toggle.is_on());
    REQUIRE(toggle_changes == std::vector<bool>{true});

    toggle.on_mouse_leave();
    toggle.advance_animations(1.0f);
    REQUIRE(toggle.hover_opacity() < 0.1f);
}

TEST_CASE("Checkbox, toggle button, icons, and image placeholders cover widget paint edges",
          "[view][widget][controls]") {
    Checkbox checkbox;
    checkbox.set_bounds({0, 0, 24, 24});

    RecordingCanvas unchecked_canvas;
    checkbox.paint(unchecked_canvas);
    REQUIRE(unchecked_canvas.count(DrawCommand::Type::stroke_rounded_rect) == 1);

    int checkbox_changes = 0;
    bool last_checked = false;
    checkbox.on_change = [&](bool checked) {
        ++checkbox_changes;
        last_checked = checked;
    };
    checkbox.on_mouse_down({12, 12});
    REQUIRE(checkbox.is_checked());
    REQUIRE(checkbox_changes == 1);
    REQUIRE(last_checked);

    RecordingCanvas checked_canvas;
    checkbox.paint(checked_canvas);
    REQUIRE(checked_canvas.count(DrawCommand::Type::fill_rounded_rect) == 1);
    REQUIRE(checked_canvas.count(DrawCommand::Type::stroke_line) == 2);

    ToggleButton button;
    button.set_bounds({0, 0, 96, 32});
    button.set_label("Latch");

    RecordingCanvas off_canvas;
    button.paint(off_canvas);
    REQUIRE(off_canvas.count(DrawCommand::Type::fill_rounded_rect) == 1);
    REQUIRE(off_canvas.count(DrawCommand::Type::stroke_rounded_rect) == 1);
    REQUIRE(commands_of(off_canvas, DrawCommand::Type::fill_text).front().text == "Latch");

    int toggle_count = 0;
    bool toggle_state = false;
    button.on_toggle = [&](bool on) {
        ++toggle_count;
        toggle_state = on;
    };
    button.on_mouse_down({4, 4});
    REQUIRE(button.is_on());
    REQUIRE(toggle_count == 1);
    REQUIRE(toggle_state);

    RecordingCanvas on_canvas;
    button.paint(on_canvas);
    REQUIRE(on_canvas.count(DrawCommand::Type::fill_rounded_rect) == 1);
    REQUIRE(on_canvas.count(DrawCommand::Type::stroke_rounded_rect) == 0);

    for (auto type : {Icon::Type::image_upload, Icon::Type::send,
                      Icon::Type::search, Icon::Type::close}) {
        Icon icon(type);
        icon.set_bounds({0, 0, 32, 32});
        RecordingCanvas icon_canvas;
        icon.paint(icon_canvas);
        REQUIRE(icon_canvas.command_count() > 0);
    }

    ImageView empty_image;
    empty_image.set_bounds({0, 0, 96, 48});
    RecordingCanvas empty_image_canvas;
    empty_image.paint(empty_image_canvas);
    REQUIRE(empty_image_canvas.count(DrawCommand::Type::fill_rounded_rect) == 1);
    REQUIRE(commands_of(empty_image_canvas, DrawCommand::Type::fill_text).front().text == "IMG");

    ImageView path_image;
    path_image.set_bounds({0, 0, 120, 48});
    path_image.set_image_path("/tmp/pulp-widget-preview.png");
    REQUIRE(path_image.image_path() == "file:///tmp/pulp-widget-preview.png");

    RecordingCanvas path_image_canvas;
    path_image.paint(path_image_canvas);
    REQUIRE(path_image_canvas.count(DrawCommand::Type::draw_image) == 1);
    REQUIRE(commands_of(path_image_canvas, DrawCommand::Type::draw_image).front().text == "/tmp/pulp-widget-preview.png");
}

TEST_CASE("Meter set_level", "[view][widget]") {
    Meter meter;
    meter.set_bounds({0, 0, 12, 200});
    meter.set_level(0.5f, 0.8f);

    REQUIRE_THAT(meter.display_rms(), WithinAbs(0.5, 0.01));
    REQUIRE_THAT(meter.display_peak(), WithinAbs(0.8, 0.01));
}

TEST_CASE("Meter renders with levels", "[view][widget]") {
    Meter meter;
    meter.set_bounds({0, 0, 12, 200});
    meter.set_level(0.6f, 0.85f);

    RecordingCanvas canvas;
    meter.paint(canvas);

    // Background + RMS fill = 2 rects, peak line + held peak = 2 lines
    REQUIRE(canvas.count(DrawCommand::Type::fill_rounded_rect) == 1);
    REQUIRE(canvas.count(DrawCommand::Type::fill_rect) == 1);
    REQUIRE(canvas.count(DrawCommand::Type::stroke_line) >= 1);
}

TEST_CASE("Meter horizontal orientation", "[view][widget]") {
    Meter meter;
    meter.set_orientation(Meter::Orientation::horizontal);
    meter.set_bounds({0, 0, 200, 12});
    meter.set_level(0.4f, 0.7f);

    RecordingCanvas canvas;
    meter.paint(canvas);

    REQUIRE(canvas.count(DrawCommand::Type::fill_rounded_rect) == 1);
    REQUIRE(canvas.count(DrawCommand::Type::fill_rect) == 1);
}

TEST_CASE("Meter update with ballistics", "[view][widget]") {
    Meter meter;
    meter.set_bounds({0, 0, 12, 200});

    // Sudden peak
    meter.update(0.9f, 0.5f, 1.0f / 60.0f);
    REQUIRE(meter.display_peak() > 0);
    REQUIRE(meter.display_rms() > 0);

    // Decay
    for (int i = 0; i < 10; ++i) {
        meter.update(0.0f, 0.0f, 1.0f / 60.0f);
    }
    REQUIRE(meter.display_peak() < 0.9f);
    REQUIRE(meter.held_peak() > 0.8f); // Still held
}

TEST_CASE("XYPad value clamping", "[view][widget]") {
    XYPad pad;
    pad.set_x(0.3f);
    pad.set_y(0.7f);
    REQUIRE_THAT(pad.x_value(), WithinAbs(0.3, 0.001));
    REQUIRE_THAT(pad.y_value(), WithinAbs(0.7, 0.001));

    pad.set_x(1.5f);
    REQUIRE_THAT(pad.x_value(), WithinAbs(1.0, 0.001));
    pad.set_y(-0.5f);
    REQUIRE_THAT(pad.y_value(), WithinAbs(0.0, 0.001));
}

TEST_CASE("XYPad renders crosshair", "[view][widget]") {
    XYPad pad;
    pad.set_bounds({0, 0, 100, 100});
    pad.set_x(0.5f);
    pad.set_y(0.5f);
    pad.set_x_label("Freq");
    pad.set_y_label("Res");

    RecordingCanvas canvas;
    pad.paint(canvas);

    // Background + grid lines + crosshair lines + thumb + labels
    REQUIRE(canvas.count(DrawCommand::Type::fill_rounded_rect) == 1);
    REQUIRE(canvas.count(DrawCommand::Type::stroke_line) >= 4); // 2 grid + 2 crosshair
    REQUIRE(canvas.count(DrawCommand::Type::fill_circle) == 1);
    REQUIRE(canvas.count(DrawCommand::Type::fill_text) >= 2);
}

TEST_CASE("WaveformView renders waveform", "[view][widget]") {
    WaveformView waveform;
    waveform.set_bounds({0, 0, 200, 60});

    // Generate sine wave
    std::vector<float> data(200);
    for (size_t i = 0; i < data.size(); ++i) {
        data[i] = std::sin(2.0f * 3.14159f * i / data.size());
    }
    waveform.set_data(std::move(data));

    REQUIRE(waveform.sample_count() == 200);

    RecordingCanvas canvas;
    waveform.paint(canvas);

    // Background + center line + many waveform lines
    REQUIRE(canvas.count(DrawCommand::Type::fill_rounded_rect) == 1);
    REQUIRE(canvas.count(DrawCommand::Type::stroke_line) > 10);
}

TEST_CASE("WaveformView empty renders background only", "[view][widget]") {
    WaveformView waveform;
    waveform.set_bounds({0, 0, 200, 60});

    RecordingCanvas canvas;
    waveform.paint(canvas);

    REQUIRE(canvas.count(DrawCommand::Type::fill_rounded_rect) == 1);
    REQUIRE(canvas.count(DrawCommand::Type::stroke_line) == 0); // No waveform
}

TEST_CASE("WaveformView trigger -- rising zero crossing", "[view][widget][trigger]") {
    // Phase-shifted sine: starts at +0.5, crosses zero downward, crosses
    // zero upward roughly at index N/2. With rising_zero trigger, the
    // display should be rotated so the rising crossing is at index 0.
    constexpr size_t N = 256;
    std::vector<float> samples(N);
    const float phase_offset = 3.14159f / 4.0f;  // start 45° into the cycle
    for (size_t i = 0; i < N; ++i) {
        samples[i] = std::sin(phase_offset + 2.0f * 3.14159f * i / N);
    }

    // Locate the expected trigger index in the raw buffer before triggering.
    size_t expected = WaveformView::find_trigger_index(
        samples.data(), samples.size(),
        WaveformView::TriggerMode::rising_zero);
    REQUIRE(expected > 0);
    REQUIRE(expected < N);

    WaveformView waveform;
    waveform.set_trigger_mode(WaveformView::TriggerMode::rising_zero);
    waveform.set_data(samples);  // copy so we can compare

    REQUIRE(waveform.sample_count() == N);

    // After triggering, index 0 should correspond to old index `expected`,
    // which is the first sample > 0 following a sample <= 0.
    // We can't directly read the rotated buffer, but we can observe
    // stability: applying the same data twice should yield the same result.
    WaveformView waveform2;
    waveform2.set_trigger_mode(WaveformView::TriggerMode::rising_zero);
    waveform2.set_data(samples);
    REQUIRE(waveform2.sample_count() == N);
}

TEST_CASE("WaveformView trigger -- free run leaves buffer unchanged", "[view][widget][trigger]") {
    constexpr size_t N = 32;
    std::vector<float> samples(N);
    for (size_t i = 0; i < N; ++i) samples[i] = static_cast<float>(i);

    WaveformView waveform;
    REQUIRE(waveform.trigger_mode() == WaveformView::TriggerMode::free_run);
    waveform.set_data(samples);
    REQUIRE(waveform.sample_count() == N);
    // In free_run mode, find_trigger_index should return 0 regardless
    REQUIRE(WaveformView::find_trigger_index(
        samples.data(), samples.size(),
        WaveformView::TriggerMode::free_run) == 0);
}

TEST_CASE("WaveformView trigger -- falling zero crossing", "[view][widget][trigger]") {
    // A simple ramp that crosses zero downward at the midpoint.
    std::vector<float> samples = {2, 1, 0.5f, 0, -0.5f, -1, -2, -1};
    size_t idx = WaveformView::find_trigger_index(
        samples.data(), samples.size(),
        WaveformView::TriggerMode::falling_zero);
    // prev=0 at i=3 is not > 0, so the first falling crossing is i=4 (prev=0, curr=-0.5)
    REQUIRE(idx == 4);
}

TEST_CASE("WaveformView trigger -- no crossing leaves buffer alone", "[view][widget][trigger]") {
    // All positive samples — no rising zero crossing possible
    std::vector<float> samples = {0.5f, 0.6f, 0.7f, 0.8f, 0.9f};
    size_t idx = WaveformView::find_trigger_index(
        samples.data(), samples.size(),
        WaveformView::TriggerMode::rising_zero);
    REQUIRE(idx == 0);

    WaveformView waveform;
    waveform.set_trigger_mode(WaveformView::TriggerMode::rising_zero);
    waveform.set_data(samples);
    REQUIRE(waveform.sample_count() == samples.size());
}

TEST_CASE("SpectrumView renders bars", "[view][widget]") {
    SpectrumView spectrum;
    spectrum.set_bounds({0, 0, 300, 100});
    spectrum.set_style(SpectrumView::Style::bars);

    std::vector<float> bins(32);
    for (size_t i = 0; i < bins.size(); ++i) {
        bins[i] = -80.0f + 80.0f * (1.0f - static_cast<float>(i) / bins.size());
    }
    spectrum.set_spectrum(std::move(bins));

    REQUIRE(spectrum.bin_count() == 32);

    RecordingCanvas canvas;
    spectrum.paint(canvas);

    // Background + 32 bars + 3 grid lines
    REQUIRE(canvas.count(DrawCommand::Type::fill_rounded_rect) == 1);
    REQUIRE(canvas.count(DrawCommand::Type::fill_rect) >= 30);
}

TEST_CASE("SpectrumView renders filled line", "[view][widget]") {
    SpectrumView spectrum;
    spectrum.set_bounds({0, 0, 300, 100});
    spectrum.set_style(SpectrumView::Style::filled);

    std::vector<float> bins(64, -40.0f);
    spectrum.set_spectrum(std::move(bins));

    RecordingCanvas canvas;
    spectrum.paint(canvas);

    // GPU path uses draw_waveform (single call), CPU fallback uses stroke_line
    // At minimum: background rect + center line + waveform fallback lines
    REQUIRE(canvas.command_count() > 5);
}

TEST_CASE("SpectrumView empty renders background", "[view][widget]") {
    SpectrumView spectrum;
    spectrum.set_bounds({0, 0, 300, 100});

    RecordingCanvas canvas;
    spectrum.paint(canvas);

    REQUIRE(canvas.count(DrawCommand::Type::fill_rounded_rect) == 1);
    REQUIRE(canvas.count(DrawCommand::Type::fill_rect) == 0);
}

TEST_CASE("SpectrumView invalid dB range only draws background",
          "[view][widget][coverage]") {
    SpectrumView spectrum;
    spectrum.set_bounds({0, 0, 300, 100});
    spectrum.set_style(SpectrumView::Style::bars);
    spectrum.set_spectrum(std::vector<float>{-80.0f, -40.0f, -12.0f});
    spectrum.set_range(0.0f, -80.0f);

    RecordingCanvas canvas;
    spectrum.paint(canvas);

    REQUIRE(canvas.count(DrawCommand::Type::fill_rounded_rect) == 1);
    REQUIRE(canvas.count(DrawCommand::Type::fill_rect) == 0);
    REQUIRE(canvas.count(DrawCommand::Type::stroke_line) == 0);
}

TEST_CASE("SpectrogramView auto-configures and paints pushed spectrum",
          "[view][widget][coverage]") {
    SpectrogramView spectrogram;
    spectrogram.set_bounds({0, 0, 128, 32});

    RecordingCanvas empty_canvas;
    spectrogram.paint(empty_canvas);
    REQUIRE(empty_canvas.count(DrawCommand::Type::fill_rect) == 1);

    std::vector<float> magnitudes{-80.0f, -40.0f, 0.0f};
    spectrogram.push_spectrum(magnitudes.data(), static_cast<int>(magnitudes.size()));

    REQUIRE(spectrogram.history_columns() == 256);
    REQUIRE(spectrogram.freq_rows() == 3);

    RecordingCanvas painted_canvas;
    spectrogram.paint(painted_canvas);

    REQUIRE(painted_canvas.count(DrawCommand::Type::fill_rect) >
            empty_canvas.count(DrawCommand::Type::fill_rect));
    REQUIRE(painted_canvas.count(DrawCommand::Type::set_fill_color) >
            empty_canvas.count(DrawCommand::Type::set_fill_color));
}

TEST_CASE("SpectrogramView explicit configuration controls painted grid size",
          "[view][widget][coverage]") {
    SpectrogramView spectrogram;
    spectrogram.set_bounds({0, 0, 40, 20});
    spectrogram.configure(4, 2, pulp::signal::ColorRamp::heat, -60.0f, -20.0f);
    spectrogram.set_color_ramp(pulp::signal::ColorRamp::grayscale);
    spectrogram.set_range(-90.0f, -30.0f);

    std::vector<float> magnitudes{-90.0f, -30.0f};
    spectrogram.push_spectrum(magnitudes.data(), static_cast<int>(magnitudes.size()));

    REQUIRE(spectrogram.history_columns() == 4);
    REQUIRE(spectrogram.freq_rows() == 2);

    RecordingCanvas canvas;
    spectrogram.paint(canvas);

    REQUIRE(canvas.count(DrawCommand::Type::fill_rect) == 1 + 4 * 2);
}

TEST_CASE("MultiMeter paints vertical and horizontal channel indicators",
          "[view][widget][coverage]") {
    pulp::signal::MultiChannelMeterData data;
    data.num_channels = 3;
    data.channels[0].rms = 0.95f;
    data.channels[0].peak = 0.98f;
    data.channels[0].clipped = true;
    data.channels[1].rms = 0.75f;
    data.channels[1].peak = 0.82f;
    data.channels[2].rms = 0.35f;
    data.channels[2].peak = 0.42f;

    MultiMeter vertical;
    vertical.set_bounds({0, 0, 90, 120});
    vertical.set_channel_count(20);
    REQUIRE(vertical.channel_count() == pulp::signal::kMaxMeterChannels);

    vertical.update(data, 0.1f);
    REQUIRE(vertical.channel_count() == 3);
    REQUIRE(vertical.ballistics().channels[0].clip_indicator);

    RecordingCanvas vertical_canvas;
    vertical.paint(vertical_canvas);

    REQUIRE(vertical_canvas.count(DrawCommand::Type::fill_rounded_rect) == 1);
    REQUIRE(vertical_canvas.count(DrawCommand::Type::fill_rect) >= 4);
    REQUIRE(vertical_canvas.count(DrawCommand::Type::stroke_line) >= 6);

    MultiMeter horizontal;
    horizontal.set_bounds({0, 0, 120, 60});
    horizontal.set_layout(MultiMeter::Layout::horizontal);
    horizontal.update(data, 0.1f);

    RecordingCanvas horizontal_canvas;
    horizontal.paint(horizontal_canvas);

    REQUIRE(horizontal.layout() == MultiMeter::Layout::horizontal);
    REQUIRE(horizontal_canvas.count(DrawCommand::Type::fill_rounded_rect) == 1);
    REQUIRE(horizontal_canvas.count(DrawCommand::Type::fill_rect) >= 4);
    REQUIRE(horizontal_canvas.count(DrawCommand::Type::stroke_line) >= 6);
}

TEST_CASE("MultiMeter with no channels paints nothing",
          "[view][widget][coverage]") {
    MultiMeter meter;
    meter.set_bounds({0, 0, 80, 40});

    RecordingCanvas canvas;
    meter.paint(canvas);

    REQUIRE(canvas.command_count() == 0);
}

TEST_CASE("CorrelationMeter clamps updates and paints both polarities",
          "[view][widget][coverage]") {
    CorrelationMeter meter;
    meter.set_bounds({0, 0, 100, 20});

    meter.update(2.0f, 1.0f);
    REQUIRE(meter.display_correlation() > 0.99f);

    RecordingCanvas positive_canvas;
    meter.paint(positive_canvas);

    REQUIRE(positive_canvas.count(DrawCommand::Type::fill_rounded_rect) == 2);
    REQUIRE(positive_canvas.count(DrawCommand::Type::stroke_line) == 3);
    REQUIRE(positive_canvas.count(DrawCommand::Type::fill_rect) == 1);

    meter.update(-2.0f, 1.0f);
    REQUIRE(meter.display_correlation() < -0.99f);

    RecordingCanvas negative_canvas;
    meter.paint(negative_canvas);

    REQUIRE(negative_canvas.count(DrawCommand::Type::fill_rounded_rect) == 2);
    REQUIRE(negative_canvas.count(DrawCommand::Type::stroke_line) == 3);
    REQUIRE(negative_canvas.count(DrawCommand::Type::fill_rect) == 1);
}

TEST_CASE("View paint_all paints children", "[view][widget]") {
    View root;
    root.set_bounds({0, 0, 300, 200});

    auto label = std::make_unique<Label>("Test");
    label->set_bounds({10, 10, 100, 20});

    auto knob = std::make_unique<Knob>();
    knob->set_bounds({10, 40, 48, 48});
    knob->set_value(0.5f);

    root.add_child(std::move(label));
    root.add_child(std::move(knob));

    RecordingCanvas canvas;
    root.paint_all(canvas);

    // Should have commands from both children
    REQUIRE(canvas.count(DrawCommand::Type::fill_text) >= 1);
    REQUIRE(canvas.count(DrawCommand::Type::stroke_arc) >= 2);
    // save/restore pairs for root + 2 children = 3 pairs
    REQUIRE(canvas.count(DrawCommand::Type::save) == 3);
    REQUIRE(canvas.count(DrawCommand::Type::restore) == 3);
}

// ── pulp #927 — Label honors setFontFamily / setFontWeight / setLetterSpacing ──
//
// Before #927, Label::paint() called canvas.set_font("Inter", size) with no
// weight / family / spacing — so JS calls into setFontWeight, setFontFamily,
// and setLetterSpacing landed on Label members but were dropped on the floor
// at render time. These tests assert the propagation through the
// RecordingCanvas, which captures the rich state via DrawCommand::set_font_full.

TEST_CASE("Label propagates font_family to canvas", "[view][widget][issue-927]") {
    Label label("Hello");
    label.set_bounds({0, 0, 200, 24});
    label.set_font_family("JetBrains Mono");

    RecordingCanvas canvas;
    label.paint(canvas);

    REQUIRE(canvas.count(DrawCommand::Type::set_font_full) == 1);
    // Locate the rich set_font_full command and assert family is forwarded.
    bool found_family = false;
    for (const auto& cmd : canvas.commands()) {
        if (cmd.type == DrawCommand::Type::set_font_full) {
            REQUIRE(cmd.text == "JetBrains Mono");
            found_family = true;
        }
    }
    REQUIRE(found_family);
}

TEST_CASE("Label propagates font_weight to canvas", "[view][widget][issue-927]") {
    Label label("BOLD");
    label.set_bounds({0, 0, 200, 24});
    label.set_font_weight(700);

    RecordingCanvas canvas;
    label.paint(canvas);

    REQUIRE(canvas.count(DrawCommand::Type::set_font_full) == 1);
    bool found_weight = false;
    for (const auto& cmd : canvas.commands()) {
        if (cmd.type == DrawCommand::Type::set_font_full) {
            // f[1] carries CSS font-weight (100..900).
            REQUIRE(static_cast<int>(cmd.f[1]) == 700);
            found_weight = true;
        }
    }
    REQUIRE(found_weight);
}

TEST_CASE("Label propagates letter_spacing to canvas", "[view][widget][issue-927]") {
    Label label("SPECTR");
    label.set_bounds({0, 0, 200, 24});
    label.set_letter_spacing(1.5f);

    RecordingCanvas canvas;
    label.paint(canvas);

    REQUIRE(canvas.count(DrawCommand::Type::set_font_full) == 1);
    bool found_spacing = false;
    for (const auto& cmd : canvas.commands()) {
        if (cmd.type == DrawCommand::Type::set_font_full) {
            // f[3] carries CSS letter-spacing in px.
            REQUIRE_THAT(cmd.f[3], WithinAbs(1.5, 0.001));
            found_spacing = true;
        }
    }
    REQUIRE(found_spacing);
}

TEST_CASE("Label set_font_full carries default weight when no setter called",
          "[view][widget][issue-927]") {
    // Verifies that the rich command goes out even for the default
    // (font_weight_=400, no family) path. Important: we do NOT want a
    // regression where set_font_full only fires for "non-default" labels.
    Label label("plain");
    label.set_bounds({0, 0, 100, 20});

    RecordingCanvas canvas;
    label.paint(canvas);

    REQUIRE(canvas.count(DrawCommand::Type::set_font_full) == 1);
    for (const auto& cmd : canvas.commands()) {
        if (cmd.type == DrawCommand::Type::set_font_full) {
            REQUIRE(cmd.text == "Inter");          // default theme family
            REQUIRE(static_cast<int>(cmd.f[1]) == 400);
            REQUIRE_THAT(cmd.f[3], WithinAbs(0.0, 0.001));
        }
    }
}

TEST_CASE("Label getters round-trip font_family", "[view][widget][issue-927]") {
    Label label("x");
    REQUIRE(label.font_family().empty());          // default == theme fallback
    label.set_font_family("Inter Display");
    REQUIRE(label.font_family() == "Inter Display");
}

// ── pulp #1737 sweep — Codex P1 on #1791: clear font_features at end ──────
// Label::paint sets font_features on the shared canvas when fontVariant
// is non-empty (translates CSV → SkShaper Feature tags so HarfBuzz
// honors the OpenType lookup). Because the canvas keeps font_features
// as mutable canvas-level state, leaving them set across paint
// boundaries leaks the previous Label's typography onto sibling
// widgets that don't set features themselves (TextButton, TextEditor,
// adjacent Labels). The fix unconditionally clears at end of paint.
//
// We verify by subclassing RecordingCanvas and recording every
// set/clear call. The expected sequence for any Label::paint that
// touches the canvas is: setN clears or sets exactly once at the top
// (the existing fontVariant translation block), then exactly one
// clear at the bottom. So either the count of clears == 2 (empty
// fontVariant case: one clear in the translation block, one at end)
// or the count of clears == 1 with one set (non-empty fontVariant:
// translation set + end clear). Either way, the LAST call must be a
// clear so subsequent paints start clean.

namespace {
struct FontFeatureSpyCanvas : pulp::canvas::RecordingCanvas {
    enum class Kind { set, clear };
    std::vector<Kind> calls;

    void set_font_features(std::vector<pulp::canvas::Canvas::FontFeature> features) override {
        (void)features;
        calls.push_back(Kind::set);
    }
    void clear_font_features() override {
        calls.push_back(Kind::clear);
    }
};
}

TEST_CASE("Label::paint always ends with clear_font_features (Codex P1 on #1791)",
          "[view][widget][issue-1737][issue-1791-codex-p1]") {
    Label label("hello");
    label.set_bounds({0, 0, 120, 24});

    {
        // Empty fontVariant — Label still calls clear at the top
        // (translation block) AND at the end (the new safety clear).
        FontFeatureSpyCanvas canvas;
        label.paint(canvas);
        REQUIRE_FALSE(canvas.calls.empty());
        // The LAST call MUST be a clear — that's the contract that
        // prevents leakage onto subsequent widgets.
        REQUIRE(canvas.calls.back() == FontFeatureSpyCanvas::Kind::clear);
    }

    {
        // Non-empty fontVariant — translation block calls set;
        // end-of-paint MUST then clear so the next widget doesn't
        // inherit our tnum/smcp/etc.
        label.set_font_variant("tabular-nums,small-caps");
        FontFeatureSpyCanvas canvas;
        label.paint(canvas);
        REQUIRE_FALSE(canvas.calls.empty());
        // At least one set somewhere in the trace.
        bool saw_set = false;
        for (auto k : canvas.calls) if (k == FontFeatureSpyCanvas::Kind::set) saw_set = true;
        REQUIRE(saw_set);
        // The LAST call MUST be a clear regardless.
        REQUIRE(canvas.calls.back() == FontFeatureSpyCanvas::Kind::clear);
    }
}

// ── pulp #1737 PR-2 — Label soft-wrap via TextShaper::layout_with_lines ──
// PR-1 added the BreakMode plumbing inside TextShaper. PR-2 (this PR)
// wires Label::paint multi-line path to consume TextShaper when
// View::word_break_ opts into break-word or anywhere AND bounds().width
// is bounded. Default (`normal` / empty) keeps the legacy `\n`-only
// split — these tests pin both paths so a future change can't silently
// flip semantics for either.

TEST_CASE("Label soft-wrap: word_break='break-word' splits long unbroken word",
          "[view][widget][issue-1737]") {
    Label label("Antidisestablishmentarianism");
    label.set_bounds({0, 0, 60, 100});
    label.set_multi_line(true);
    label.set_word_break("break-word");

    RecordingCanvas canvas;
    label.paint(canvas);

    auto text_cmds = commands_of(canvas, DrawCommand::Type::fill_text);
    // Single 28-char word at width 60 — should produce >1 line under
    // break-word. Default `normal` would emit exactly 1 line that
    // overflows.
    REQUIRE(text_cmds.size() >= 2);

    // Reconstruction must be lossless for the non-whitespace characters.
    std::string reconstructed;
    for (const auto& cmd : text_cmds) reconstructed += cmd.text;
    REQUIRE(reconstructed == "Antidisestablishmentarianism");
}

TEST_CASE("Label soft-wrap: word_break='normal' on a single unbroken word keeps whole-word overflow",
          "[view][widget][issue-1737][issue-1924]") {
    Label label("Antidisestablishmentarianism");
    label.set_bounds({0, 0, 60, 100});
    label.set_multi_line(true);
    label.set_word_break("normal");  // explicit, but matches default

    RecordingCanvas canvas;
    label.paint(canvas);

    // pulp #1924: the default (`normal`) path now also routes through
    // TextShaper for bounded multi_line labels, but TextShaper's
    // BreakMode::normal preserves the "whole-word overflow for a single
    // unbroken word" CSS contract (see test_text_shaper.cpp
    // "BreakMode::normal preserves legacy whole-word overflow"). So an
    // input with no whitespace still emits exactly one fill_text — the
    // observable output for this probe is unchanged from pre-#1924.
    auto text_cmds = commands_of(canvas, DrawCommand::Type::fill_text);
    REQUIRE(text_cmds.size() == 1);
    REQUIRE(text_cmds[0].text == "Antidisestablishmentarianism");
}

// ── pulp #1924 — Label soft-wraps under CSS default `white-space: normal` ──
// Pre-#1924 the wrap gate required `word_break != normal` before the shaper
// fired, so default Labels (no explicit word-break) overflowed their bounds
// instead of soft-wrapping at whitespace. CSS `white-space: normal` is the
// default and MUST soft-wrap at word boundaries. Hit on Spectr's dropdowns
// (Settings / Preset Manager) — text overflowed the parent SChips /
// PatternRow container.

TEST_CASE("Label default word_break: text with whitespace soft-wraps at word boundaries (#1924)",
          "[view][widget][issue-1924]") {
    // Several short words separated by spaces. With bounds().width
    // narrower than the full string the shaper must break at whitespace,
    // emitting more than one line. NO explicit word_break is set — this
    // is the CSS default (`white-space: normal`) path that was broken
    // pre-#1924 (single overflowing line).
    Label label("Settings Preset Manager Dropdown");
    label.set_bounds({0, 0, 60, 200});
    label.set_multi_line(true);
    // NOTE: no set_word_break — defaults to "normal" / empty.

    RecordingCanvas canvas;
    label.paint(canvas);

    auto text_cmds = commands_of(canvas, DrawCommand::Type::fill_text);
    // Expect >1 fill_text — soft-wrap at whitespace inside the bounded
    // container. Pre-#1924 this would have been exactly 1 (the legacy
    // `\n`-only path with no newlines in the input).
    REQUIRE(text_cmds.size() >= 2);

    // Reconstruction across all emitted lines must contain every
    // non-whitespace character from the original (whitespace at line
    // boundaries is collapsed under CSS `white-space: normal` — the
    // exact preservation contract is shaper-defined, but no characters
    // beyond inter-word spaces should be dropped).
    std::string concatenated;
    for (const auto& cmd : text_cmds) concatenated += cmd.text;
    // Each individual word must still appear in the emitted output.
    for (const char* word : {"Settings", "Preset", "Manager", "Dropdown"}) {
        INFO("Expecting word " << word << " in '" << concatenated << "'");
        REQUIRE(concatenated.find(word) != std::string::npos);
    }
}

TEST_CASE("Label soft-wrap: word_break='anywhere' also splits, same as break-word for over-wide single word",
          "[view][widget][issue-1737]") {
    Label label("Supercalifragilisticexpialidocious");
    label.set_bounds({0, 0, 50, 100});
    label.set_multi_line(true);
    label.set_word_break("anywhere");

    RecordingCanvas canvas;
    label.paint(canvas);

    auto text_cmds = commands_of(canvas, DrawCommand::Type::fill_text);
    REQUIRE(text_cmds.size() >= 2);

    std::string reconstructed;
    for (const auto& cmd : text_cmds) reconstructed += cmd.text;
    REQUIRE(reconstructed == "Supercalifragilisticexpialidocious");
}

TEST_CASE("Label soft-wrap: line-clamp truncates wrapped lines + appends ellipsis",
          "[view][widget][issue-1737]") {
    Label label("Antidisestablishmentarianism");  // single long word
    label.set_bounds({0, 0, 60, 100});
    label.set_multi_line(true);
    label.set_word_break("break-word");
    label.set_line_clamp(2);

    RecordingCanvas canvas;
    label.paint(canvas);

    auto text_cmds = commands_of(canvas, DrawCommand::Type::fill_text);
    // line_clamp=2 caps emitted lines at 2 (assuming the word produces
    // ≥3 shaped lines at width 60).
    REQUIRE(text_cmds.size() <= 2);
    if (text_cmds.size() == 2) {
        // pulp #1552 — last visible line under line-clamp gets the
        // U+2026 (UTF-8 0xE2 0x80 0xA6) ellipsis appended.
        const std::string& last = text_cmds.back().text;
        REQUIRE(last.size() >= 3);
        REQUIRE((unsigned char)last[last.size()-3] == 0xE2);
        REQUIRE((unsigned char)last[last.size()-2] == 0x80);
        REQUIRE((unsigned char)last[last.size()-1] == 0xA6);
    }
}

TEST_CASE("Label soft-wrap: bounds().width == 0 falls through to legacy path (no shaper invocation)",
          "[view][widget][issue-1737]") {
    Label label("hello world how are you");
    // NO bounds set — bounds().width defaults to 0
    label.set_multi_line(true);
    label.set_word_break("break-word");

    RecordingCanvas canvas;
    label.paint(canvas);

    // With no bounded width, soft-wrap can't decide where to break.
    // Fall through to the legacy path which emits one fill_text per
    // \n-delimited line — exactly 1 fill_text since input has no \n.
    auto text_cmds = commands_of(canvas, DrawCommand::Type::fill_text);
    REQUIRE(text_cmds.size() == 1);
    REQUIRE(text_cmds[0].text == "hello world how are you");
}

namespace {

// pulp #73 — minimal WindowHost that counts repaint() calls. Mirrors
// the DummyWindowHost in test_view.cpp; duplicated here so the widget
// suite stays self-contained.
class CountingHost final : public WindowHost {
public:
    void show() override {}
    void hide() override {}
    bool is_visible() const override { return false; }
    void repaint() override { ++repaint_count; }
    void set_close_callback(std::function<void()>) override {}
    void run_event_loop() override {}

    int repaint_count = 0;
};

} // namespace

// pulp #73 — programmatic value mutation MUST schedule a repaint.
// User-input mutation (mouse drag) piggybacks on the host's per-event
// setNeedsDisplay path; preset application / JS bridge setValue go
// through THIS code path and would otherwise leave the painted state
// stale until the next user input. The "preset-applied band-shape
// outline missing (only renders during manual draw)" symptom in
// Spectr was exactly this gap. These regression tests fence each
// programmatic setter so a refactor that drops the request_repaint()
// call surfaces in CI before the user sees a silent paint.
TEST_CASE("Widget set_value programmatic mutation requests repaint [issue-73]",
          "[view][widget][issue-73]") {
    SECTION("Knob::set_value") {
        Knob knob;
        CountingHost host;
        knob.set_window_host(&host);
        REQUIRE(host.repaint_count == 0);

        knob.set_value(0.7f);
        REQUIRE(host.repaint_count >= 1);
    }

    SECTION("Fader::set_value") {
        Fader fader;
        CountingHost host;
        fader.set_window_host(&host);
        REQUIRE(host.repaint_count == 0);

        fader.set_value(0.4f);
        REQUIRE(host.repaint_count >= 1);
    }

    SECTION("RangeSlider::set_value") {
        RangeSlider slider;
        slider.set_min(0);
        slider.set_max(100);
        CountingHost host;
        slider.set_window_host(&host);
        int before = host.repaint_count;

        slider.set_value(50);
        REQUIRE(host.repaint_count > before);
    }

    SECTION("Toggle::set_on") {
        Toggle toggle;
        CountingHost host;
        toggle.set_window_host(&host);
        REQUIRE(host.repaint_count == 0);

        toggle.set_on(true);
        REQUIRE(host.repaint_count >= 1);
    }

    SECTION("Checkbox::set_checked") {
        Checkbox cb;
        CountingHost host;
        cb.set_window_host(&host);
        REQUIRE(host.repaint_count == 0);

        cb.set_checked(true);
        REQUIRE(host.repaint_count >= 1);
    }

    SECTION("ToggleButton::set_on") {
        ToggleButton tb;
        CountingHost host;
        tb.set_window_host(&host);
        REQUIRE(host.repaint_count == 0);

        tb.set_on(true);
        REQUIRE(host.repaint_count >= 1);
    }
}

// Codex P2 on PR #2013 — the no-change guard. WidgetBridge::sync_from_store
// and restore_values(...) call set_value() / set_on() in tight loops
// during sync/reload. Firing a host repaint when the value didn't change
// burns wall-clock on large widget trees. These tests fence the guard
// so a regression that drops the early-return surfaces as visible
// frame-time spikes long before a human notices.
TEST_CASE("Widget setters skip repaint when value is unchanged [issue-73]",
          "[view][widget][issue-73][issue-2013]") {
    SECTION("Knob::set_value") {
        Knob knob;
        CountingHost host;
        knob.set_window_host(&host);
        knob.set_value(0.5f);
        int after_first = host.repaint_count;
        REQUIRE(after_first >= 1);

        // Same value again — must NOT repaint.
        knob.set_value(0.5f);
        REQUIRE(host.repaint_count == after_first);

        // Different value — must repaint.
        knob.set_value(0.6f);
        REQUIRE(host.repaint_count > after_first);
    }

    SECTION("Fader::set_value idempotent") {
        Fader fader;
        CountingHost host;
        fader.set_window_host(&host);
        fader.set_value(0.3f);
        int after = host.repaint_count;

        fader.set_value(0.3f);
        REQUIRE(host.repaint_count == after);
    }

    SECTION("Toggle::set_on idempotent") {
        Toggle toggle;
        CountingHost host;
        toggle.set_window_host(&host);
        toggle.set_on(true);
        int after = host.repaint_count;

        toggle.set_on(true);
        REQUIRE(host.repaint_count == after);
    }

    SECTION("Checkbox::set_checked idempotent") {
        Checkbox cb;
        CountingHost host;
        cb.set_window_host(&host);
        cb.set_checked(true);
        int after = host.repaint_count;

        cb.set_checked(true);
        REQUIRE(host.repaint_count == after);
    }

    SECTION("ToggleButton::set_on idempotent") {
        ToggleButton tb;
        CountingHost host;
        tb.set_window_host(&host);
        tb.set_on(true);
        int after = host.repaint_count;

        tb.set_on(true);
        REQUIRE(host.repaint_count == after);
    }

    SECTION("RangeSlider::set_value idempotent") {
        RangeSlider slider;
        slider.set_min(0);
        slider.set_max(100);
        CountingHost host;
        slider.set_window_host(&host);
        slider.set_value(50);
        int after = host.repaint_count;

        slider.set_value(50);
        REQUIRE(host.repaint_count == after);
    }

    SECTION("Knob::set_label idempotent") {
        Knob knob;
        CountingHost host;
        knob.set_window_host(&host);
        knob.set_label("Cutoff");
        int after = host.repaint_count;

        knob.set_label("Cutoff");
        REQUIRE(host.repaint_count == after);
    }

    SECTION("Fader::set_label idempotent") {
        Fader fader;
        CountingHost host;
        fader.set_window_host(&host);
        fader.set_label("Volume");
        int after = host.repaint_count;

        fader.set_label("Volume");
        REQUIRE(host.repaint_count == after);
    }

    SECTION("ToggleButton::set_label idempotent") {
        ToggleButton tb;
        CountingHost host;
        tb.set_window_host(&host);
        tb.set_label("Bypass");
        int after = host.repaint_count;

        tb.set_label("Bypass");
        REQUIRE(host.repaint_count == after);
    }
}

TEST_CASE("Widget set_label programmatic mutation requests repaint [issue-73]",
          "[view][widget][issue-73]") {
    SECTION("Knob::set_label") {
        Knob knob;
        CountingHost host;
        knob.set_window_host(&host);
        int before = host.repaint_count;

        knob.set_label("Cutoff");
        REQUIRE(host.repaint_count > before);
    }

    SECTION("Fader::set_label") {
        Fader fader;
        CountingHost host;
        fader.set_window_host(&host);
        int before = host.repaint_count;

        fader.set_label("Volume");
        REQUIRE(host.repaint_count > before);
    }

    SECTION("ToggleButton::set_label") {
        ToggleButton tb;
        CountingHost host;
        tb.set_window_host(&host);
        int before = host.repaint_count;

        tb.set_label("Mute");
        REQUIRE(host.repaint_count > before);
    }
}

TEST_CASE("Knob format setter requests repaint without changing value [issue-73]",
          "[view][widget][issue-73]") {
    Knob knob;
    knob.set_value(0.25f);
    CountingHost host;
    knob.set_window_host(&host);
    int before = host.repaint_count;

    knob.set_format([](float value) {
        return std::to_string(static_cast<int>(value * 100.0f)) + "%";
    });

    REQUIRE(knob.value() == 0.25f);
    REQUIRE(host.repaint_count > before);
}
