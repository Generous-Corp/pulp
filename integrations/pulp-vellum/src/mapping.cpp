#include <pulp/vellum/mapping.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace pulp::vellum {

namespace {

std::string_view trimmed(std::string_view text) noexcept {
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front())))
        text.remove_prefix(1);
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back())))
        text.remove_suffix(1);
    return text;
}

bool equal_ignoring_case(std::string_view a, std::string_view b) noexcept {
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
               return std::tolower(static_cast<unsigned char>(x)) ==
                      std::tolower(static_cast<unsigned char>(y));
           });
}

} // namespace

double to_normalized(const state::ParamInfo& info, float plain) noexcept {
    if (std::isnan(plain))
        return 0.0;
    return state::host_normalized_from_value(info, state::constrain_param_value(info, plain));
}

float to_plain(const state::ParamInfo& info, double normalized) noexcept {
    if (std::isnan(normalized))
        return info.range.default_value;
    const float clamped = static_cast<float>(std::clamp(normalized, 0.0, 1.0));
    return state::constrain_param_value(info, info.range.denormalize(clamped));
}

int value_count(const state::ParamInfo& info) noexcept {
    return static_cast<int>(state::param_value_count(info));
}

std::string format_plain(const state::ParamInfo& info, float plain) {
    if (info.to_string)
        return info.to_string(plain);
    if (!info.value_labels.empty()) {
        const float step = info.range.step > 0.0f ? info.range.step : 1.0f;
        const auto index = std::llround((plain - info.range.min) / step);
        if (index >= 0 && static_cast<std::size_t>(index) < info.value_labels.size())
            return info.value_labels[static_cast<std::size_t>(index)];
    }
    char number[48];
    std::snprintf(number, sizeof number, "%.2f", static_cast<double>(plain));
    std::string out = number;
    if (!info.unit.empty())
        out += " " + info.unit;
    return out;
}

std::optional<double> parse_normalized(const state::ParamInfo& info, std::string_view text) {
    text = trimmed(text);
    if (text.empty())
        return std::nullopt;
    if (const auto labelled = state::param_value_for_label(info, text))
        return to_normalized(info, *labelled);

    float plain = 0.0f;
    if (info.from_string) {
        plain = info.from_string(std::string(text));
    } else {
        // strtod needs a terminated string, and accepts "nan" and "inf".
        const std::string owned(text);
        char* end = nullptr;
        const double parsed = std::strtod(owned.c_str(), &end);
        if (end == owned.c_str())
            return std::nullopt;
        const std::string_view rest = trimmed(std::string_view(end));
        if (!rest.empty() && !(!info.unit.empty() && equal_ignoring_case(rest, info.unit)))
            return std::nullopt;
        plain = static_cast<float>(parsed);
    }
    if (!std::isfinite(plain))
        return std::nullopt;
    return to_normalized(info, plain);
}

} // namespace pulp::vellum
