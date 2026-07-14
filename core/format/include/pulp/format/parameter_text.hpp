#pragma once

#include <pulp/format/detail/locale_independent_float.hpp>
#include <pulp/runtime/exceptions.hpp>
#include <pulp/state/parameter.hpp>

#include <charconv>
#include <cmath>
#include <optional>
#include <string>
#include <string_view>

namespace pulp::format {

/// Canonical plain-value formatting shared by format adapters and framework UI.
inline std::string format_parameter_text(const state::ParamInfo& info,
                                         float plain_value) {
    const float value = state::constrain_param_value(info, plain_value);
    if (info.to_string) {
        PULP_TRY {
            return info.to_string(value);
        } PULP_CATCH_ALL {
            return {};
        }
    }

    if (!info.value_labels.empty()) {
        const float step = info.range.step > 0.0f ? info.range.step : 1.0f;
        const auto raw = std::llround((value - info.range.min) / step);
        const auto index = static_cast<std::size_t>(std::clamp<int64_t>(
            raw, 0, static_cast<int64_t>(info.value_labels.size() - 1)));
        return info.value_labels[index];
    }

    char number[128];
    const int precision = info.kind == state::ParamKind::Continuous ? 2 : 0;
    const auto converted = std::to_chars(number, number + sizeof(number), value,
                                         std::chars_format::fixed, precision);
    if (converted.ec != std::errc{}) return {};
    std::string result(number, converted.ptr);
    if (!info.unit.empty()) {
        result += ' ';
        result += info.unit;
    }
    return result;
}

/// Parse host text into a constrained plain value. Enum/toggle parameters only
/// accept declared labels unless the author supplied a custom parser.
inline std::optional<float> parse_parameter_text(const state::ParamInfo& info,
                                                 std::string_view text) {
    if (info.from_string) {
        PULP_TRY {
            const float parsed = info.from_string(std::string(text));
            if (std::isfinite(parsed)) return state::constrain_param_value(info, parsed);
        } PULP_CATCH_ALL {
            return std::nullopt;
        }
    }

    if (auto labeled = state::param_value_for_label(info, text)) return labeled;
    if (info.kind == state::ParamKind::Toggle || info.kind == state::ParamKind::Enum)
        return std::nullopt;

    double parsed = 0.0;
    const auto result = detail::parse_double_c_locale(text, parsed);
    if (result.consumed == 0 || result.range_error || !std::isfinite(parsed))
        return std::nullopt;

    auto suffix = text.substr(result.consumed);
    while (!suffix.empty() && std::isspace(static_cast<unsigned char>(suffix.front())))
        suffix.remove_prefix(1);
    while (!suffix.empty() && std::isspace(static_cast<unsigned char>(suffix.back())))
        suffix.remove_suffix(1);
    if (!suffix.empty() && suffix != info.unit) return std::nullopt;

    return state::constrain_param_value(info, static_cast<float>(parsed));
}

} // namespace pulp::format
