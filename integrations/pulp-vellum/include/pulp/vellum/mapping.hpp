#pragma once

/// @file
/// Pure value mapping between a Pulp parameter and Vellum's normalized domain.
///
/// Every function here depends only on a `state::ParamInfo`, never on a store,
/// a port or a model, so a test or a widget helper can call it without running
/// audio. They are thin over Pulp's own `ParamRange`, `param_value_count`,
/// `constrain_param_value` and label helpers: there is one implementation of
/// the curve, and it is Pulp's.

#include <optional>
#include <string>
#include <string_view>

namespace pulp::state {
struct ParamInfo;
}

namespace pulp::vellum {

/// The normalized [0, 1] position of `plain` on `info`'s range, after the
/// parameter's own constraint (clamp, step snap). 0 for NaN.
[[nodiscard]] double to_normalized(const state::ParamInfo& info, float plain) noexcept;

/// The plain value a UI edit at `normalized` means: clamped to [0, 1], mapped
/// through the range's curve and snapped to its step. The default for NaN.
[[nodiscard]] float to_plain(const state::ParamInfo& info, double normalized) noexcept;

/// Number of values the parameter takes; 0 for a continuous parameter. This
/// counts values, not intervals, which is what `vellum::value::ParamModel`
/// calls `value_count()`.
[[nodiscard]] int value_count(const state::ParamInfo& info) noexcept;

/// The text shown for `plain`: the parameter's own `to_string` when it has one,
/// else its value label, else the number with two decimals and the unit.
[[nodiscard]] std::string format_plain(const state::ParamInfo& info, float plain);

/// The normalized value a user meant by `text`, or nullopt when it is not a
/// value of this parameter. Accepts a value label, or a number with an
/// optional unit suffix; uses the parameter's `from_string` when it has one.
/// A number outside the range clamps to it.
[[nodiscard]] std::optional<double> parse_normalized(const state::ParamInfo& info,
                                                     std::string_view text);

} // namespace pulp::vellum
