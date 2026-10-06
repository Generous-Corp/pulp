#pragma once

/// @file safe_relative_path.hpp
/// Screening for untrusted relative paths that will be joined beneath a
/// destination directory: archive entry names, manifest member paths, and
/// declared asset paths.
///
/// `std::filesystem::path::is_absolute()` is not enough on its own. On Windows
/// `/etc/passwd`, `\rooted` and `C:foo` are all *not* absolute, yet joining any
/// of them onto a directory replaces that directory's root. The checks here
/// refuse every rooted form on every platform, and additionally refuse `\` and
/// `:` inside a component everywhere, so a name that would escape on Windows is
/// refused on macOS and Linux too.

#include <filesystem>
#include <iterator>
#include <string>

namespace pulp::runtime {

/// True only if @p path is safe to join beneath a destination directory: a
/// non-empty relative path with no root name (`C:`, `\\server`), no root
/// directory (`/` or `\`), and no component that is empty, `.`, `..`, or
/// contains `\`, `:` or NUL.
inline bool is_safe_relative_path(const std::filesystem::path& path) {
    if (path.empty() || path.is_absolute() || path.has_root_name() || path.has_root_directory())
        return false;
    for (const auto& part : path) {
        const std::u8string text = part.u8string();
        if (text.empty() || text == u8"." || text == u8"..")
            return false;
        for (const char8_t c : text)
            if (c == u8'\\' || c == u8':' || c == u8'\0')
                return false;
    }
    return true;
}

/// True if @p candidate, compared lexically after normalization, is
/// @p directory itself or lies beneath it. An empty @p directory contains
/// nothing. Use it after joining a screened
/// relative path onto its destination, as a second line of defence.
inline bool is_within_directory(const std::filesystem::path& directory,
                                const std::filesystem::path& candidate) {
    const auto root = directory.lexically_normal();
    const auto path = candidate.lexically_normal();
    // An empty directory constrains nothing, so it cannot vouch for anything.
    if (root.empty())
        return false;
    // `.` (or `a/..`) is the current directory: any relative candidate that
    // does not climb out of it lies within it.
    if (root == ".") {
        if (path.has_root_name() || path.has_root_directory())
            return false;
        return path.empty() || *path.begin() != "..";
    }
    auto r = root.begin();
    auto p = path.begin();
    for (; r != root.end(); ++r) {
        // A trailing separator on the directory normalizes to an empty final
        // element; it does not constrain the candidate.
        if (r->empty() && std::next(r) == root.end())
            break;
        if (p == path.end() || *p != *r)
            return false;
        ++p;
    }
    return true;
}

} // namespace pulp::runtime
