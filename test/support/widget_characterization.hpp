#pragma once

/// @file widget_characterization.hpp
/// A black-box characterization harness for stock-widget layout metrics.
///
/// Some stock-widget sizing behavior is not expressed as a public constant a
/// caller can read: a popup panel's hidden horizontal padding, a label's
/// per-glyph advance. When a UI must be reproduced faithfully, those implicit
/// laws have to be RECOVERED rather than guessed — and the only way to recover
/// one is to MEASURE the widget's resolved geometry across a swept input and
/// fit the relationship.
///
/// This harness does exactly that. A widget is driven headlessly (no window,
/// no GPU), the geometry the layout pass computes is read directly, and a
/// linear model `output = a * input + b` is fitted with an r-squared
/// goodness-of-fit. The caller supplies a `probe` that, for each point in a
/// sweep, returns the measured {input-axis, output-axis} pair; the harness
/// collects the samples, fits the law, and emits a small structured report.
///
/// Measurement uses the layout seams that already run without a canvas — a
/// widget's `intrinsic_width()` / `intrinsic_height()`, an overlay panel's own
/// geometry entry point, and the shared text shaper — so nothing here needs a
/// live surface.
///
/// This is a test/tool helper. It is not shipped SDK API.

#include <cstddef>
#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace pulp::test {

/// A least-squares fit of `output = a * input + b`, plus its goodness-of-fit.
struct LinearFit {
    double a = 0.0;          ///< slope
    double b = 0.0;          ///< intercept
    double r_squared = 0.0;  ///< coefficient of determination, in [0, 1]
    std::size_t count = 0;   ///< number of sample points fitted
};

/// Fit `ys = a * xs + b` by ordinary least squares.
///
/// `xs` and `ys` must be the same length. With fewer than two points, or with
/// a constant `xs` (no input variation to fit against), the slope is reported
/// as 0 and `r_squared` as 0. When the outputs are constant (a widget whose
/// metric does not depend on the swept input) the slope is 0 and `r_squared`
/// collapses to the flat-line case: 1 if the fit reproduces the constant
/// exactly, 0 otherwise.
LinearFit fit_linear(const std::vector<double>& xs, const std::vector<double>& ys);

/// One characterization result: the widget and axes under test, the raw swept
/// samples, and the fitted law.
struct CharacterizationReport {
    std::string widget;       ///< e.g. "ContextMenu"
    std::string input_axis;   ///< e.g. "widest_row_text_width_px"
    std::string output_axis;  ///< e.g. "panel_width_px"
    std::vector<double> inputs;
    std::vector<double> outputs;
    LinearFit fit;

    /// A compact structured (JSON) rendering, safe to log or write as an
    /// artifact. Fields: widget, input_axis, output_axis, a, b, r_squared,
    /// count, and the raw samples.
    std::string to_json() const;
};

/// Build a report from pre-collected samples, fitting the model over them.
CharacterizationReport make_report(std::string widget, std::string input_axis,
                                   std::string output_axis,
                                   std::vector<double> inputs,
                                   std::vector<double> outputs);

/// Sweep helper. For each item in `probe_points`, `probe` returns the pair
/// {input-axis value, output-axis value} measured for that point; the harness
/// collects them and fits the law. Templated on the probe-point type so a
/// caller can sweep strings, item counts, or anything the widget takes.
template <typename Point>
CharacterizationReport characterize(
    std::string widget, std::string input_axis, std::string output_axis,
    const std::vector<Point>& probe_points,
    const std::function<std::pair<double, double>(const Point&)>& probe) {
    std::vector<double> inputs;
    std::vector<double> outputs;
    inputs.reserve(probe_points.size());
    outputs.reserve(probe_points.size());
    for (const auto& point : probe_points) {
        const auto sample = probe(point);
        inputs.push_back(sample.first);
        outputs.push_back(sample.second);
    }
    return make_report(std::move(widget), std::move(input_axis),
                       std::move(output_axis), std::move(inputs),
                       std::move(outputs));
}

}  // namespace pulp::test
