#pragma once

#include <pulp/timeline/utf8.hpp>

#include <string_view>

namespace pulp::timeline {

/** @addtogroup timeline_model
 * @{
 */

/// Returns whether a persisted package-relative locator preserves Timeline's
/// historical lexical contract: relative, NUL-free, and without parent traversal.
inline bool package_relative_path_is_lexically_safe(std::string_view path) noexcept {
    if (path.empty() || path.find('\0') != std::string_view::npos || path.front() == '/' ||
        path.front() == '\\')
        return false;
    const auto ascii_alpha = [](char value) noexcept {
        return (value >= 'A' && value <= 'Z') || (value >= 'a' && value <= 'z');
    };
    if (path.size() >= 2 && ascii_alpha(path.front()) && path[1] == ':')
        return false;

    std::size_t component_begin = 0;
    for (std::size_t index = 0; index <= path.size(); ++index) {
        if (index != path.size() && path[index] != '/' && path[index] != '\\')
            continue;
        if (path.substr(component_begin, index - component_begin) == "..")
            return false;
        component_begin = index + 1;
    }
    return true;
}

/// Returns whether a new package-publication path is portable across supported
/// filesystems and cannot be reinterpreted as a device or alternate data stream.
inline bool package_relative_path_is_portable(std::string_view path) noexcept {
    if (!package_relative_path_is_lexically_safe(path) || !is_valid_utf8(path) ||
        path.find('\\') != std::string_view::npos)
        return false;

    const auto ascii_upper = [](char value) noexcept {
        return value >= 'a' && value <= 'z' ? static_cast<char>(value - ('a' - 'A')) : value;
    };
    const auto equals_ascii_case_insensitive = [&](std::string_view lhs,
                                                   std::string_view rhs) noexcept {
        if (lhs.size() != rhs.size())
            return false;
        for (std::size_t index = 0; index < lhs.size(); ++index)
            if (ascii_upper(lhs[index]) != rhs[index])
                return false;
        return true;
    };
    const auto windows_component_is_safe = [&](std::string_view component) noexcept {
        if (component.empty() || component == "." || component == ".." || component.back() == '.' ||
            component.back() == ' ')
            return false;
        for (const auto value : component) {
            const auto byte = static_cast<unsigned char>(value);
            if (byte < 0x20 || value == '<' || value == '>' || value == ':' || value == '"' ||
                value == '|' || value == '?' || value == '*')
                return false;
        }

        const auto base = component.substr(0, component.find('.'));
        if (equals_ascii_case_insensitive(base, "CON") ||
            equals_ascii_case_insensitive(base, "PRN") ||
            equals_ascii_case_insensitive(base, "AUX") ||
            equals_ascii_case_insensitive(base, "NUL") ||
            equals_ascii_case_insensitive(base, "CLOCK$") ||
            equals_ascii_case_insensitive(base, "CONIN$") ||
            equals_ascii_case_insensitive(base, "CONOUT$"))
            return false;
        if (base.size() >= 4 && ((ascii_upper(base[0]) == 'C' && ascii_upper(base[1]) == 'O' &&
                                  ascii_upper(base[2]) == 'M') ||
                                 (ascii_upper(base[0]) == 'L' && ascii_upper(base[1]) == 'P' &&
                                  ascii_upper(base[2]) == 'T'))) {
            const bool ascii_device_digit = base.size() == 4 && base[3] >= '1' && base[3] <= '9';
            const bool superscript_device_digit = base.size() == 5 &&
                                                  static_cast<unsigned char>(base[3]) == 0xc2 &&
                                                  (static_cast<unsigned char>(base[4]) == 0xb9 ||
                                                   static_cast<unsigned char>(base[4]) == 0xb2 ||
                                                   static_cast<unsigned char>(base[4]) == 0xb3);
            if (ascii_device_digit || superscript_device_digit)
                return false;
        }
        return true;
    };

    std::size_t component_begin = 0;
    for (std::size_t index = 0; index <= path.size(); ++index) {
        if (index != path.size() && path[index] != '/')
            continue;
        if (!windows_component_is_safe(path.substr(component_begin, index - component_begin)))
            return false;
        component_begin = index + 1;
    }
    return true;
}

/// @}

} // namespace pulp::timeline
