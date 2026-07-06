#pragma once

// Small ASCII string helpers shared by the design-contract cores. These are
// implementation details of the design parsers (handoff, variants, …), not part
// of the public design API — hence the `text` sub-namespace and header-only
// `inline` definitions. ASCII-only by design: the inputs are Markdown keys,
// enum labels, and token names, so `<cctype>` on `unsigned char` is correct and
// avoids pulling in a locale/Unicode dependency.

#include <cctype>
#include <string>
#include <string_view>

namespace pulp::design::text {

// Lower-case an ASCII string. Accepts a view so callers holding a std::string,
// const char*, or string_view all reuse one definition without copying at the
// call boundary beyond the single owned result.
inline std::string to_lower(std::string_view s) {
    std::string out(s);
    for (char& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

// Trim leading/trailing ASCII whitespace, returning a view into the same
// storage (no allocation). The caller must keep the underlying buffer alive.
inline std::string_view trim(std::string_view s) {
    auto issp = [](char c) { return std::isspace(static_cast<unsigned char>(c)) != 0; };
    while (!s.empty() && issp(s.front())) s.remove_prefix(1);
    while (!s.empty() && issp(s.back())) s.remove_suffix(1);
    return s;
}

}  // namespace pulp::design::text
