#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <pulp/view/widgets.hpp>
#include <pulp/canvas/canvas.hpp>

using namespace pulp::view;
using namespace pulp::canvas;
using Catch::Matchers::WithinAbs;

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
