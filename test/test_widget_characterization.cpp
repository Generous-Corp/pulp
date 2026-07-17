// Drives the black-box widget-characterization harness against real stock
// widgets to (a) prove the harness recovers an implicit sizing law by pure
// headless measurement, and (b) pin each widget's current law as a regression
// guard. No window and no GPU: every measurement runs through a layout seam
// that already computes geometry with no canvas.

#include "support/widget_characterization.hpp"

#include <pulp/canvas/text_shaper.hpp>
#include <pulp/view/buttons.hpp>
#include <pulp/view/context_menu.hpp>
#include <pulp/view/widgets.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <string>
#include <vector>

using Catch::Matchers::WithinAbs;
using namespace pulp::test;

namespace {

// The font a ContextMenu measures its rows with, mirrored here so the
// harness's input axis (row text width) is computed the same way the widget
// computes it internally. These are the widget's own stock values.
constexpr float kMenuFontSize = 12.0f;
const char* const kMenuFontFamily = "Inter";

double shaped_text_width(const std::string& text, float size) {
    return static_cast<double>(
        pulp::canvas::global_text_shaper().prepare(text, kMenuFontFamily, size).total_width());
}

}  // namespace

TEST_CASE("ContextMenu panel width is linear in widest-row text width",
          "[view][widget-metrics][characterization]") {
    // Sweep a single-row menu whose one label grows in length. The panel width
    // the menu resolves has no public padding constant a caller can read; the
    // harness recovers it as the intercept of the fit. Rows are kept well past
    // the panel's minimum-width floor so the floor never confounds the line.
    const std::vector<int> char_counts = {30, 35, 40, 45, 50, 55, 60, 65};

    const auto probe = [](const int& count) -> std::pair<double, double> {
        const std::string label(static_cast<std::size_t>(count), 'M');

        pulp::view::ContextMenu menu;
        menu.set_items({{1, label}});

        const double text_width = shaped_text_width(label, kMenuFontSize);
        const double panel_width = static_cast<double>(menu.measured_width());
        return {text_width, panel_width};
    };

    const CharacterizationReport report = characterize<int>(
        "ContextMenu", "widest_row_text_width_px", "panel_width_px",
        char_counts, probe);
    INFO(report.to_json());

    // Every sampled row must clear the panel's minimum-width floor, or the
    // clamp would flatten the small end of the sweep and corrupt the fit.
    for (const double width : report.inputs) {
        REQUIRE(width > 90.0);
    }

    REQUIRE(report.fit.count == char_counts.size());
    // The recovered law: panel width == row text width + a fixed horizontal
    // padding. Slope is unity; the intercept is the widget's hidden padding.
    CHECK(report.fit.r_squared > 0.9999);
    CHECK_THAT(report.fit.a, WithinAbs(1.0, 0.01));
    CHECK_THAT(report.fit.b, WithinAbs(34.0, 0.5));

    // Coefficients are stable: the same sweep run again yields the same law.
    const CharacterizationReport again = characterize<int>(
        "ContextMenu", "widest_row_text_width_px", "panel_width_px",
        char_counts, probe);
    CHECK_THAT(again.fit.a, WithinAbs(report.fit.a, 1e-9));
    CHECK_THAT(again.fit.b, WithinAbs(report.fit.b, 1e-9));
    CHECK_THAT(again.fit.r_squared, WithinAbs(report.fit.r_squared, 1e-9));
}

TEST_CASE("Label intrinsic width is linear in glyph count",
          "[view][widget-metrics][characterization]") {
    // A Label's intrinsic width is the shaped advance of its text. Swept over a
    // repeated glyph, the harness recovers the widget's implicit per-glyph
    // advance as the slope and confirms there is no fixed width padding
    // (intercept near zero).
    const std::vector<int> glyph_counts = {4, 8, 12, 16, 20, 24, 28, 32};
    constexpr float kLabelFontSize = 13.0f;

    const auto probe = [](const int& count) -> std::pair<double, double> {
        pulp::view::Label label(std::string(static_cast<std::size_t>(count), 'M'));
        label.set_font_size(kLabelFontSize);
        return {static_cast<double>(count),
                static_cast<double>(label.intrinsic_width())};
    };

    const CharacterizationReport report = characterize<int>(
        "Label", "glyph_count", "intrinsic_width_px", glyph_counts, probe);
    INFO(report.to_json());

    REQUIRE(report.fit.count == glyph_counts.size());
    CHECK(report.fit.r_squared > 0.9999);
    // Positive per-glyph advance in a plausible pixel range for a 13px face,
    // and effectively no constant offset.
    CHECK(report.fit.a > 2.0);
    CHECK(report.fit.a < 30.0);
    CHECK_THAT(report.fit.b, WithinAbs(0.0, 3.0));

    // The recovered slope should equal a directly-measured single-glyph advance
    // (shaped 'MM' minus 'M'), which is what "per-glyph advance" means.
    const double measured_advance =
        shaped_text_width("MM", kLabelFontSize) - shaped_text_width("M", kLabelFontSize);
    CHECK_THAT(report.fit.a, WithinAbs(measured_advance, 0.5));
}

TEST_CASE("TextButton height does not depend on label length",
          "[view][widget-metrics][characterization]") {
    // Not every widget metric is a growing line. A TextButton's intrinsic
    // height is a fixed control height, independent of its label. The harness
    // must report that honestly: a zero slope, with the constant recovered as
    // the intercept. This pins that the button does NOT grow with its text.
    const std::vector<int> label_lengths = {2, 6, 10, 14, 18, 22};

    const auto probe = [](const int& length) -> std::pair<double, double> {
        pulp::view::TextButton button(std::string(static_cast<std::size_t>(length), 'W'));
        return {static_cast<double>(length),
                static_cast<double>(button.intrinsic_height())};
    };

    const CharacterizationReport report = characterize<int>(
        "TextButton", "label_length", "intrinsic_height_px", label_lengths, probe);
    INFO(report.to_json());

    REQUIRE(report.fit.count == label_lengths.size());
    // Flat line: no dependence on the swept input.
    CHECK_THAT(report.fit.a, WithinAbs(0.0, 1e-9));
    CHECK_THAT(report.fit.b, WithinAbs(36.0, 1e-6));
    // A constant output reproduces the flat-line r-squared convention exactly.
    CHECK_THAT(report.fit.r_squared, WithinAbs(1.0, 1e-9));
}

TEST_CASE("fit_linear reports goodness of fit on noisy samples",
          "[view][widget-metrics][characterization]") {
    // A direct unit test of the fitter so a regression in the least-squares
    // math is caught independent of any widget: a clean line fits with
    // r-squared 1, and an off-line point drops it below 1.
    const std::vector<double> xs = {0.0, 1.0, 2.0, 3.0, 4.0};
    const std::vector<double> clean = {5.0, 7.0, 9.0, 11.0, 13.0};  // y = 2x + 5
    const LinearFit exact = fit_linear(xs, clean);
    CHECK_THAT(exact.a, WithinAbs(2.0, 1e-9));
    CHECK_THAT(exact.b, WithinAbs(5.0, 1e-9));
    CHECK_THAT(exact.r_squared, WithinAbs(1.0, 1e-9));

    std::vector<double> noisy = clean;
    noisy[2] += 3.0;  // perturb one point off the line
    const LinearFit approx = fit_linear(xs, noisy);
    CHECK(approx.r_squared < 1.0);
    CHECK(approx.r_squared > 0.5);

    // Degenerate inputs: fewer than two points, and constant x.
    CHECK(fit_linear({1.0}, {2.0}).r_squared == 0.0);
    CHECK(fit_linear({3.0, 3.0, 3.0}, {1.0, 2.0, 3.0}).a == 0.0);
}
