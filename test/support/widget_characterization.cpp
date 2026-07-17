#include "widget_characterization.hpp"

#include <cmath>
#include <sstream>

namespace pulp::test {

LinearFit fit_linear(const std::vector<double>& xs, const std::vector<double>& ys) {
    LinearFit fit;
    const std::size_t n = (xs.size() == ys.size()) ? xs.size() : 0;
    fit.count = n;
    if (n < 2) return fit;

    double sum_x = 0.0, sum_y = 0.0;
    for (std::size_t i = 0; i < n; ++i) {
        sum_x += xs[i];
        sum_y += ys[i];
    }
    const double mean_x = sum_x / static_cast<double>(n);
    const double mean_y = sum_y / static_cast<double>(n);

    // Covariance(x, y) and Variance(x), both as sums of squares about the mean.
    double sxx = 0.0, sxy = 0.0, syy = 0.0;
    for (std::size_t i = 0; i < n; ++i) {
        const double dx = xs[i] - mean_x;
        const double dy = ys[i] - mean_y;
        sxx += dx * dx;
        sxy += dx * dy;
        syy += dy * dy;
    }

    // No spread in the inputs: there is nothing to fit a slope against.
    if (sxx <= 0.0) return fit;

    fit.a = sxy / sxx;
    fit.b = mean_y - fit.a * mean_x;

    // Residual sum of squares against the fitted line.
    double ss_res = 0.0;
    for (std::size_t i = 0; i < n; ++i) {
        const double predicted = fit.a * xs[i] + fit.b;
        const double residual = ys[i] - predicted;
        ss_res += residual * residual;
    }

    // r-squared = 1 - ss_res / ss_tot. When the outputs are constant
    // (ss_tot == 0) the ratio is undefined; report the flat-line case — a
    // perfect fit reproduces the constant (ss_res == 0) so r-squared is 1,
    // otherwise 0. This is the "metric does not depend on the input" shape.
    if (syy <= 0.0) {
        fit.r_squared = (ss_res <= 0.0) ? 1.0 : 0.0;
    } else {
        fit.r_squared = 1.0 - ss_res / syy;
    }
    return fit;
}

CharacterizationReport make_report(std::string widget, std::string input_axis,
                                   std::string output_axis,
                                   std::vector<double> inputs,
                                   std::vector<double> outputs) {
    CharacterizationReport report;
    report.widget = std::move(widget);
    report.input_axis = std::move(input_axis);
    report.output_axis = std::move(output_axis);
    report.inputs = std::move(inputs);
    report.outputs = std::move(outputs);
    report.fit = fit_linear(report.inputs, report.outputs);
    return report;
}

namespace {

// A JSON string literal with the handful of characters a widget/axis name can
// plausibly carry escaped. Names here are code-authored, not user data, so this
// stays deliberately small rather than pulling in a full JSON writer.
std::string json_string(const std::string& value) {
    std::string out;
    out.reserve(value.size() + 2);
    out.push_back('"');
    for (const char ch : value) {
        switch (ch) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\t': out += "\\t"; break;
            default: out.push_back(ch); break;
        }
    }
    out.push_back('"');
    return out;
}

}  // namespace

std::string CharacterizationReport::to_json() const {
    std::ostringstream os;
    os.setf(std::ios::fixed);
    os.precision(6);
    os << "{"
       << "\"widget\":" << json_string(widget) << ","
       << "\"input_axis\":" << json_string(input_axis) << ","
       << "\"output_axis\":" << json_string(output_axis) << ","
       << "\"a\":" << fit.a << ","
       << "\"b\":" << fit.b << ","
       << "\"r_squared\":" << fit.r_squared << ","
       << "\"count\":" << fit.count << ","
       << "\"samples\":[";
    const std::size_t n = (inputs.size() == outputs.size()) ? inputs.size() : 0;
    for (std::size_t i = 0; i < n; ++i) {
        if (i != 0) os << ",";
        os << "{\"x\":" << inputs[i] << ",\"y\":" << outputs[i] << "}";
    }
    os << "]}";
    return os.str();
}

}  // namespace pulp::test
