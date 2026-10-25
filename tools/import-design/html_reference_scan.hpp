// SPDX-License-Identifier: MIT
#pragma once

#include <array>
#include <cstddef>
#include <regex>
#include <string>
#include <string_view>
#include <vector>

namespace pulp::import_design {

/// A case-insensitive pattern HTML staging scans source text with, and the
/// literals its alternatives begin with. Every alternative of `pattern` must
/// start with one of `starts`, so the pattern cannot match at an offset where
/// none of them begins.
struct ReferencePattern {
    std::regex pattern;
    std::vector<std::string_view> starts;
};

namespace detail {

inline char ascii_lower(char c) noexcept {
    return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
}

inline bool starts_at(const std::string& text, std::size_t offset, std::string_view literal) {
    if (text.size() - offset < literal.size())
        return false;
    for (std::size_t i = 0; i < literal.size(); ++i)
        if (ascii_lower(text[offset + i]) != ascii_lower(literal[i]))
            return false;
    return true;
}

} // namespace detail

/// Visits, in order, exactly the matches std::sregex_iterator yields for
/// `reference.pattern` over `text`, calling `visit(const std::smatch&)` for
/// each. The iterator runs the regex engine at every offset, which on a
/// megabyte bundle is seconds of backtracking that finds nothing; this runs it
/// only at offsets where one of `reference.starts` begins, the only offsets
/// where the pattern can match. Returns how many offsets it ran the engine at.
template <typename Visit>
std::size_t for_each_reference_match(const std::string& text, const ReferencePattern& reference,
                                     Visit&& visit) {
    std::array<bool, 256> first{};
    for (const auto literal : reference.starts) {
        if (literal.empty())
            continue;
        const auto c = static_cast<unsigned char>(literal.front());
        first[c] = true;
        first[static_cast<unsigned char>(detail::ascii_lower(static_cast<char>(c)))] = true;
        if (c >= 'a' && c <= 'z')
            first[c - 'a' + 'A'] = true;
    }
    std::size_t attempts = 0;
    std::size_t offset = 0;
    std::smatch match;
    while (offset < text.size()) {
        if (!first[static_cast<unsigned char>(text[offset])]) {
            ++offset;
            continue;
        }
        bool candidate = false;
        for (const auto literal : reference.starts) {
            if (!literal.empty() && detail::starts_at(text, offset, literal)) {
                candidate = true;
                break;
            }
        }
        if (!candidate) {
            ++offset;
            continue;
        }
        ++attempts;
        // match_prev_avail lets a leading \b see the byte before the offset,
        // as it does when the iterator searches through it.
        auto flags = std::regex_constants::match_continuous;
        if (offset > 0)
            flags |= std::regex_constants::match_prev_avail;
        const auto begin = text.cbegin() + static_cast<std::ptrdiff_t>(offset);
        if (std::regex_search(begin, text.cend(), match, reference.pattern, flags) &&
            match.length(0) > 0) {
            visit(match);
            offset += static_cast<std::size_t>(match.length(0));
        } else {
            ++offset;
        }
    }
    return attempts;
}

/// The three patterns stage_html_project follows dependencies with.
const ReferencePattern& html_reference_pattern();
const ReferencePattern& html_scannable_reference_pattern();
const ReferencePattern& html_srcset_pattern();

} // namespace pulp::import_design
