#pragma once

#import <Foundation/Foundation.h>

#include <cstddef>
#include <limits>

namespace pulp::view::mac_text_input {

// NSRange as UTF-16 offsets for NSTextInputClient. AppKit's "no range" is
// {NSNotFound, 0} and reads as offset 0; an end past size_t saturates rather
// than wrapping. Shared by the standalone and plug-in text-input categories.
inline std::size_t nsrange_location_or_zero(NSRange range) noexcept {
    return range.location == NSNotFound ? 0 : static_cast<std::size_t>(range.location);
}

inline std::size_t nsrange_end_or_zero(NSRange range) noexcept {
    if (range.location == NSNotFound)
        return 0;
    const auto start = static_cast<std::size_t>(range.location);
    const auto length = static_cast<std::size_t>(range.length);
    const auto max = std::numeric_limits<std::size_t>::max();
    return length > max - start ? max : start + length;
}

} // namespace pulp::view::mac_text_input
