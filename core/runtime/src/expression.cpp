#include <pulp/runtime/expression.hpp>
#include <algorithm>
#include <cmath>
#include <cctype>
#include <stdexcept>

namespace pulp::runtime {

// ── Recursive descent parser ────────────────────────────────────────────

class Parser {
public:
    template<typename VarMap, typename FnMap>
    Parser(std::string_view expr, const VarMap& vars, const FnMap& fns)
        : input_(expr), pos_(0) {
        for (auto& [k, v] : vars) vars_[k] = v;
        for (auto& [k, v] : fns) fns_[k] = v;
    }

    double parse() {
        double result = parse_expr();
        skip_ws();
        if (pos_ < input_.size()) throw std::runtime_error("Unexpected character");
        return result;
    }

private:
    std::string_view input_;
    size_t pos_;
    std::map<std::string, double> vars_;
    std::map<std::string, std::function<double(double)>> fns_;

    void skip_ws() { while (pos_ < input_.size() && std::isspace(input_[pos_])) pos_++; }
    char peek() { skip_ws(); return pos_ < input_.size() ? input_[pos_] : '\0'; }
    char advance() { skip_ws(); return pos_ < input_.size() ? input_[pos_++] : '\0'; }
    bool match(char c) { if (peek() == c) { advance(); return true; } return false; }

    // expr = term (('+' | '-') term)*
    double parse_expr() {
        double left = parse_term();
        while (true) {
            if (match('+')) left += parse_term();
            else if (match('-')) left -= parse_term();
            else break;
        }
        return left;
    }

    // term = power (('*' | '/') power)*
    double parse_term() {
        double left = parse_power();
        while (true) {
            if (match('*')) left *= parse_power();
            else if (match('/')) { double d = parse_power(); left = d != 0 ? left / d : 0; }
            else break;
        }
        return left;
    }

    // power = unary ('^' unary)?
    double parse_power() {
        double base = parse_unary();
        if (match('^')) return std::pow(base, parse_unary());
        return base;
    }

    // unary = ('-' | '+')? primary
    double parse_unary() {
        if (match('-')) return -parse_primary();
        match('+');
        return parse_primary();
    }

    // primary = number | '(' expr ')' | function '(' expr ')' | variable
    double parse_primary() {
        skip_ws();

        // Parenthesized expression
        if (match('(')) {
            double v = parse_expr();
            if (!match(')')) throw std::runtime_error("Missing )");
            return v;
        }

        // Number
        if (pos_ < input_.size() && (std::isdigit(input_[pos_]) || input_[pos_] == '.')) {
            size_t start = pos_;
            while (pos_ < input_.size() && (std::isdigit(input_[pos_]) || input_[pos_] == '.'))
                pos_++;
            // Handle scientific notation
            if (pos_ < input_.size() && (input_[pos_] == 'e' || input_[pos_] == 'E')) {
                pos_++;
                if (pos_ < input_.size() && (input_[pos_] == '+' || input_[pos_] == '-')) pos_++;
                size_t exponent_start = pos_;
                while (pos_ < input_.size() && std::isdigit(input_[pos_])) pos_++;
                if (pos_ == exponent_start) throw std::runtime_error("Malformed exponent");
            }
            return std::stod(std::string(input_.substr(start, pos_ - start)));
        }

        // Identifier (variable or function)
        if (pos_ < input_.size() && (std::isalpha(input_[pos_]) || input_[pos_] == '_')) {
            size_t start = pos_;
            while (pos_ < input_.size() && (std::isalnum(input_[pos_]) || input_[pos_] == '_'))
                pos_++;
            std::string name(input_.substr(start, pos_ - start));

            // Built-in constants
            if (name == "pi") return 3.14159265358979323846;
            if (name == "e") return 2.71828182845904523536;

            // Function call
            if (match('(')) {
                double arg = parse_expr();
                double arg2 = 0;
                bool has_arg2 = false;
                if (match(',')) { arg2 = parse_expr(); has_arg2 = true; }
                if (!match(')')) throw std::runtime_error("Missing )");

                // Built-in functions
                if (name == "sin") return std::sin(arg);
                if (name == "cos") return std::cos(arg);
                if (name == "tan") return std::tan(arg);
                if (name == "sqrt") return std::sqrt(arg);
                if (name == "abs") return std::abs(arg);
                if (name == "log") return std::log(arg);
                if (name == "log10") return std::log10(arg);
                if (name == "exp") return std::exp(arg);
                if (name == "floor") return std::floor(arg);
                if (name == "ceil") return std::ceil(arg);
                if (name == "round") return std::round(arg);
                if (name == "min" && has_arg2) return std::min(arg, arg2);
                if (name == "max" && has_arg2) return std::max(arg, arg2);
                if (name == "pow" && has_arg2) return std::pow(arg, arg2);
                if (name == "clamp" && has_arg2) return std::clamp(arg, 0.0, arg2);

                // Custom functions
                auto it = fns_.find(name);
                if (it != fns_.end()) return it->second(arg);

                throw std::runtime_error("Unknown function: " + name);
            }

            // Variable
            auto it = vars_.find(name);
            if (it != vars_.end()) return it->second;

            throw std::runtime_error("Unknown variable: " + name);
        }

        throw std::runtime_error("Unexpected input");
    }
};

// ── Public API ──────────────────────────────────────────────────────────

std::optional<double> evaluate(std::string_view expression,
                                const std::map<std::string, double>& variables) {
    try {
        std::map<std::string, std::function<double(double)>, std::less<>> empty;
        Parser parser(expression, variables, empty);
        return parser.parse();
    } catch (...) {
        return std::nullopt;
    }
}

void ExpressionEvaluator::set(std::string_view name, double value) {
    variables_[std::string(name)] = value;
}

std::optional<double> ExpressionEvaluator::get(std::string_view name) const {
    auto it = variables_.find(name);
    return it != variables_.end() ? std::optional(it->second) : std::nullopt;
}

std::optional<double> ExpressionEvaluator::evaluate(std::string_view expression) const {
    try {
        Parser parser(expression, variables_, custom_functions_);
        return parser.parse();
    } catch (...) {
        return std::nullopt;
    }
}

void ExpressionEvaluator::register_function(std::string_view name, std::function<double(double)> fn) {
    custom_functions_[std::string(name)] = std::move(fn);
}

void ExpressionEvaluator::clear_variables() {
    variables_.clear();
}

}  // namespace pulp::runtime
