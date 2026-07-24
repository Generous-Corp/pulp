#pragma once

#include <string_view>

namespace pulp::timeline {

/// Returns whether a UTF-8 package-relative asset locator is lexically
/// relative and contains no parent traversal component.
inline bool package_relative_path_is_lexically_safe(std::string_view path) noexcept {
    if (path.empty() || path.front() == '/' || path.front() == '\\')
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

} // namespace pulp::timeline
