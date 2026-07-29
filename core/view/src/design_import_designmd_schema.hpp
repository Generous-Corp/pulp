#pragma once

#include <pulp/view/design_sources.hpp>

#include <yaml-cpp/yaml.h>

#include <string_view>

namespace pulp::view {

inline constexpr int kDesignMdMaxTokenNestingDepth = 20;

/// Parse and validate frontmatter features that describe the DESIGN.md schema
/// rather than producing Pulp tokens.
void apply_designmd_frontmatter_schema(const YAML::Node& root,
                                       DesignMdParseResult& result);

bool designmd_section_is_omitted(const DesignMdParseResult& parsed,
                                 std::string_view section);

void append_designmd_omitted_findings(
    const DesignMdParseResult& parsed,
    std::vector<DesignMdDiagnostic>& findings);

} // namespace pulp::view
