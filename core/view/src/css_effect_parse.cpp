#include <pulp/view/css_effect_parse.hpp>

#include <cctype>
#include <cstdlib>
#include <unordered_map>

namespace pulp::view {
namespace {

using FilterChainEntry = canvas::Canvas::FilterChainEntry;

/// The argument of one filter function, as the spec's relative amount.
///
/// `50%` and `0.5` are the same value; a unit suffix (`px`, `deg`) is left for
/// the caller to interpret, since `strtof` stops at it. Returns nothing when
/// there is no number at all, which is how a malformed function is rejected
/// without rejecting its neighbours.
std::optional<float> filter_amount(const std::string& argument) {
    const char* begin = argument.c_str();
    char* end = nullptr;
    const float value = std::strtof(begin, &end);
    if (end == begin) return std::nullopt;
    while (end != nullptr && *end != '\0' &&
           std::isspace(static_cast<unsigned char>(*end)) != 0) {
        ++end;
    }
    return (end != nullptr && *end == '%') ? value / 100.0f : value;
}

}  // namespace

std::optional<float> css_blur_radius(const std::string& filter) {
    const auto open = filter.find("blur(");
    if (open == std::string::npos) return std::nullopt;
    const auto close = filter.find(')', open);
    if (close == std::string::npos) return std::nullopt;
    const auto inner = filter.substr(open + 5, close - open - 5);
    try {
        const float radius = std::stof(inner);  // stof stops at the unit
        if (!(radius > 0.0f)) return std::nullopt;
        return radius;
    } catch (...) {
        return std::nullopt;
    }
}

std::vector<canvas::Canvas::FilterChainEntry> css_filter_chain(
    const std::string& filter) {
    using Kind = FilterChainEntry::Kind;
    static const std::unordered_map<std::string, Kind> kFunctions{
        {"blur", Kind::blur},
        {"brightness", Kind::brightness},
        {"contrast", Kind::contrast},
        {"grayscale", Kind::grayscale},
        {"greyscale", Kind::grayscale},
        {"hue-rotate", Kind::hue_rotate},
        {"invert", Kind::invert},
        {"opacity", Kind::opacity},
        {"saturate", Kind::saturate},
        {"sepia", Kind::sepia},
    };

    std::vector<FilterChainEntry> chain;
    size_t cursor = 0;
    while (cursor < filter.size()) {
        const auto open = filter.find('(', cursor);
        if (open == std::string::npos) break;
        // Nested parentheses do not occur in the functions above, but a
        // `drop-shadow(0 0 2px rgb(0,0,0))` in the same list does contain them.
        // Matching the FIRST close would then leave `)` as the next token and
        // desynchronize every function after it, so the depth is tracked.
        size_t close = open;
        int depth = 0;
        for (; close < filter.size(); ++close) {
            if (filter[close] == '(') ++depth;
            else if (filter[close] == ')' && --depth == 0) break;
        }
        if (close >= filter.size()) break;

        auto name = filter.substr(cursor, open - cursor);
        const auto first = name.find_first_not_of(" \t\n\r");
        name = first == std::string::npos ? std::string{} : name.substr(first);
        for (auto& c : name) c = static_cast<char>(
            std::tolower(static_cast<unsigned char>(c)));

        const auto known = kFunctions.find(name);
        const auto amount =
            filter_amount(filter.substr(open + 1, close - open - 1));
        if (known != kFunctions.end() && amount) {
            FilterChainEntry entry;
            entry.kind = known->second;
            // `hue-rotate` is the one function whose argument is an angle
            // rather than an amount, and it is carried in its own field.
            if (entry.kind == Kind::hue_rotate) entry.angle_deg = *amount;
            else entry.amount = *amount;
            chain.push_back(entry);
        }
        cursor = close + 1;
    }
    return chain;
}

std::optional<canvas::Canvas::BlendMode> css_blend_mode(const std::string& keyword) {
    using BlendMode = canvas::Canvas::BlendMode;
    static const std::unordered_map<std::string, BlendMode> kModes{
        {"normal", BlendMode::normal},
        {"multiply", BlendMode::multiply},
        {"screen", BlendMode::screen},
        {"overlay", BlendMode::overlay},
        {"darken", BlendMode::darken},
        {"lighten", BlendMode::lighten},
        {"color-dodge", BlendMode::color_dodge},
        {"color-burn", BlendMode::color_burn},
        {"hard-light", BlendMode::hard_light},
        {"soft-light", BlendMode::soft_light},
        {"difference", BlendMode::difference},
        {"exclusion", BlendMode::exclusion},
        {"hue", BlendMode::hue},
        {"saturation", BlendMode::saturation},
        {"color", BlendMode::color},
        {"luminosity", BlendMode::luminosity},
    };
    const auto it = kModes.find(keyword);
    return it == kModes.end() ? std::nullopt
                              : std::optional<BlendMode>(it->second);
}

}  // namespace pulp::view
