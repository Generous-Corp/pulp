#pragma once

// Expression — simple math expression evaluator.
// Supports: +, -, *, /, ^, (), variables, and common functions (sin, cos, sqrt, abs, min, max).
// No external dependencies (no exprtk).

#include <string>
#include <string_view>
#include <map>
#include <optional>
#include <functional>

namespace pulp::runtime {

/// Evaluate a math expression string.
/// Variables can be provided as a name→value map.
/// Returns nullopt on parse error.
///
/// Examples:
///   evaluate("2 + 3")                         → 5.0
///   evaluate("sin(pi / 2)")                   → 1.0
///   evaluate("x * 2 + 1", {{"x", 3.0}})      → 7.0
///   evaluate("min(a, b)", {{"a", 5}, {"b", 3}}) → 3.0
std::optional<double> evaluate(std::string_view expression,
                                const std::map<std::string, double>& variables = {});

/// Expression evaluator with persistent variable state.
class ExpressionEvaluator {
public:
    ExpressionEvaluator() = default;

    /// Set a variable value.
    void set(std::string_view name, double value);

    /// Get a variable value.
    std::optional<double> get(std::string_view name) const;

    /// Evaluate an expression using the stored variables.
    std::optional<double> evaluate(std::string_view expression) const;

    /// Register a custom function (1 argument).
    void register_function(std::string_view name, std::function<double(double)> fn);

    /// Clear all variables.
    void clear_variables();

private:
    std::map<std::string, double, std::less<>> variables_;
    std::map<std::string, std::function<double(double)>, std::less<>> custom_functions_;
};

}  // namespace pulp::runtime
