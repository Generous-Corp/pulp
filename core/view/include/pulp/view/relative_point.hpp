#pragma once

// RelativePoint — expression-based positioning primitive.
//
// Pulp's flex + grid layout covers most authored layouts; RelativePoint
// fills the remaining "I want this anchored to another element's edge" cases
// without forcing a full constraint solver.
//
// Grammar:
//
//   point      ::= expr ',' expr             // (x, y) pair
//   expr       ::= term  (('+' | '-') term)*
//   term       ::= factor (('*' | '/') factor)*
//   factor     ::= number
//                | identifier               // 'left', 'right', 'top', 'bottom',
//                                           //   'width', 'height',
//                                           //   'centerx', 'centery'
//                | qualified                // <name>.<identifier> e.g.
//                                           //   parent.right, prev.left
//                | '(' expr ')'
//                | '-' factor                // unary minus
//   number     ::= [0-9]+ ('.' [0-9]+)?
//   identifier ::= [a-zA-Z_][a-zA-Z0-9_]*
//   qualified  ::= identifier '.' identifier
//
// Built-in names (unqualified) refer to the `parent` rectangle. Qualified
// names route to the `named` map at evaluate time. Unknown qualified
// names cause `evaluate()` to throw — that's a runtime error, not a
// parse error, because the named-rect environment is only known at
// layout time.
//
// Numeric values are treated as device pixels (no DimensionUnit support —
// keep this primitive narrow; if you need %/vw/vh, compose with the
// existing Dimension type at the caller).

#include "pulp/view/geometry.hpp"

#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <variant>
#include <vector>

namespace pulp::view {

class RelativeExpression {
public:
    /// Evaluate this expression against the given parent rectangle and
    /// optional environment of named rectangles. Throws std::runtime_error
    /// if a qualified name refers to a missing rectangle.
    float evaluate(const Rect& parent,
                   const std::map<std::string, Rect>& named = {}) const;

    /// Round-trip text form for diagnostics. Re-parses to the same tree
    /// (numbers may be reformatted).
    std::string to_string() const;

    /// Convenience: parse + evaluate in one shot. Throws on parse error.
    static float evaluate(const std::string& source,
                          const Rect& parent,
                          const std::map<std::string, Rect>& named = {});

    /// Parse a single-axis expression (e.g. "right - 10"). Throws
    /// std::invalid_argument on syntax error with a position-bearing
    /// message.
    static RelativeExpression parse(const std::string& source);

    // Internal AST node (public for to_string traversal). Hidden from
    // generic consumers behind opaque ptr.
    struct Node;

private:
    explicit RelativeExpression(std::shared_ptr<Node> root) : root_(std::move(root)) {}
    std::shared_ptr<Node> root_;
};

class RelativePoint {
public:
    /// Parse a "x, y" expression. Throws std::invalid_argument on syntax
    /// error.
    static RelativePoint parse(const std::string& source);

    /// Evaluate both axes against the given environment.
    Point evaluate(const Rect& parent,
                   const std::map<std::string, Rect>& named = {}) const;

    const RelativeExpression& x() const noexcept { return x_; }
    const RelativeExpression& y() const noexcept { return y_; }

private:
    RelativePoint(RelativeExpression x, RelativeExpression y)
        : x_(std::move(x)), y_(std::move(y)) {}
    RelativeExpression x_;
    RelativeExpression y_;
};

} // namespace pulp::view
