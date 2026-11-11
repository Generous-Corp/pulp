// SPDX-License-Identifier: MIT
#pragma once

#include <choc/text/choc_JSON.h>

#include <charconv>
#include <cstdint>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string_view>

namespace pulp::host::detail::resolved_json {

// JSON objects have an empty CHOC class name. This private numeric conversion
// constructs the carrier, so a wire object cannot impersonate a number.
inline constexpr std::string_view unsigned_class = "pulp.identity.unsigned64";

inline choc::value::Value number(std::uint64_t value) {
    if (value <= static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))
        return choc::value::Value(static_cast<std::int64_t>(value));
    return choc::value::createObject(unsigned_class, "high", static_cast<std::int64_t>(value >> 32),
                                     "low", static_cast<std::int64_t>(value & 0xffffffffu));
}

inline std::optional<std::uint64_t> number(choc::value::ValueView value) {
    if (value.isInt() && value.getInt64() >= 0)
        return static_cast<std::uint64_t>(value.getInt64());
    if (!value.isObjectWithClassName(unsigned_class) || value.size() != 2 ||
        !value.hasObjectMember("high") || !value.hasObjectMember("low"))
        return {};
    const auto high = value["high"], low = value["low"];
    if (!high.isInt() || !low.isInt() || high.getInt64() < 0 || low.getInt64() < 0 ||
        static_cast<std::uint64_t>(high.getInt64()) > std::numeric_limits<std::uint32_t>::max() ||
        static_cast<std::uint64_t>(low.getInt64()) > std::numeric_limits<std::uint32_t>::max())
        return {};
    return (static_cast<std::uint64_t>(high.getInt64()) << 32) |
           static_cast<std::uint64_t>(low.getInt64());
}

inline choc::value::Value parse_number(std::string_view token, bool negative) {
    if (negative || token.empty() || (token.size() != 1 && token.front() == '0'))
        throw std::invalid_argument("expected canonical unsigned integer");
    std::uint64_t value = 0;
    const auto parsed = std::from_chars(token.data(), token.data() + token.size(), value);
    if (parsed.ec != std::errc{} || parsed.ptr != token.data() + token.size())
        throw std::invalid_argument("unsigned integer outside its source width");
    return number(value);
}

inline choc::value::Value parse(std::string_view text) {
    if (text.find('\0') != std::string_view::npos)
        throw std::invalid_argument("embedded NUL in resolved JSON");
    return choc::json::parseWithNumberTokenParser(text, parse_number);
}

} // namespace pulp::host::detail::resolved_json
