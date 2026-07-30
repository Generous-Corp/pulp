// SPDX-License-Identifier: MIT
#include "claude_html_dependencies.hpp"

#include <regex>

namespace pulp::import_design {

std::vector<std::string> claude_html_dependency_roots(
    std::string_view content) {
    // Claude project exports can load a bound design system by constructing
    // URLs at runtime (`base + "/" + tokenPath`). Those files are not visible
    // to the ordinary src/href/import graph, but the base declaration is.
    static const std::regex kDesignSystemBase{
        R"((?:const|let|var)\s+\w*base\w*\s*=\s*["']([^"']*_ds/[^"']+)["'])",
        std::regex::icase};
    std::vector<std::string> result;
    const std::string text(content);
    for (std::sregex_iterator it(
             text.begin(), text.end(), kDesignSystemBase), end;
         it != end; ++it) {
        result.push_back((*it)[1].str());
    }
    return result;
}

}  // namespace pulp::import_design
