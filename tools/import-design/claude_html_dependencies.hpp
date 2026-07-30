// SPDX-License-Identifier: MIT
#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace pulp::import_design {

/// Discover Claude project-export dependency roots that are assembled into
/// URLs at runtime rather than referenced through ordinary HTML/CSS/JS import
/// syntax. Returned strings are untrusted references; html_project_stager owns
/// containment validation and staging limits.
std::vector<std::string> claude_html_dependency_roots(
    std::string_view content);

}  // namespace pulp::import_design
