#include "design_import_designmd_schema.hpp"

#include <algorithm>
#include <cctype>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace pulp::view {
namespace {

int yaml_line(const YAML::Node& node) {
    return node && node.Mark().line >= 0 ? node.Mark().line + 1 : 0;
}

DesignMdDiagnostic diagnostic(DesignMdSeverity severity,
                              std::string code,
                              std::string path,
                              const YAML::Node& node,
                              std::string message) {
    return {severity, std::move(code), std::move(path), yaml_line(node), 0,
            std::move(message)};
}

std::string lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return value;
}

std::string flattened_name(std::string name) {
    std::replace(name.begin(), name.end(), '.', '-');
    return name;
}

void collect_leaf_paths(const YAML::Node& node,
                        const std::string& path,
                        int depth,
                        const std::string& category,
                        std::vector<std::pair<std::string, YAML::Node>>& leaves,
                        DesignMdParseResult& result) {
    if (depth > kDesignMdMaxTokenPathSegments) {
        result.diagnostics.push_back(diagnostic(
            DesignMdSeverity::error, "token-nesting-depth",
            category + "." + path, node,
            "token nesting exceeds the DESIGN.md limit of 20 levels"));
        return;
    }
    if (node.IsScalar()) {
        leaves.emplace_back(path, node);
        return;
    }
    if (!node.IsMap()) return;
    for (auto it = node.begin(); it != node.end(); ++it) {
        const auto key = it->first.as<std::string>("");
        if (key.empty()) continue;
        collect_leaf_paths(it->second, path.empty() ? key : path + "." + key,
                           depth + 1, category, leaves, result);
    }
}

void validate_collisions(const YAML::Node& node,
                         const std::string& category,
                         DesignMdParseResult& result) {
    if (!node || !node.IsMap()) return;
    std::vector<std::pair<std::string, YAML::Node>> leaves;
    collect_leaf_paths(node, "", 0, category, leaves, result);
    std::unordered_map<std::string, std::string> normalized_to_path;
    for (const auto& [path, source] : leaves) {
        const auto normalized = flattened_name(path);
        auto [it, inserted] = normalized_to_path.emplace(normalized, path);
        if (!inserted) {
            result.diagnostics.push_back(diagnostic(
                DesignMdSeverity::error, "token-name-collision",
                category + "." + path, source,
                "token path collides with `" + category + "." + it->second +
                "` after dot-notation flattening"));
        }
    }
}

void parse_omitted(const YAML::Node& node, DesignMdParseResult& result) {
    if (!node || !node.IsSequence()) return;
    for (const auto& item : node) {
        if (item.IsScalar()) {
            const auto section = item.as<std::string>("");
            if (!section.empty()) result.omitted_sections.push_back({section, {}});
            continue;
        }
        if (!item.IsMap() || !item["section"] || !item["section"].IsScalar()) continue;
        const auto section = item["section"].as<std::string>("");
        if (section.empty()) continue;
        const auto reason = item["reason"] && item["reason"].IsScalar()
            ? item["reason"].as<std::string>("") : std::string{};
        result.omitted_sections.push_back({section, reason});
    }
}

void validate_typography_properties(const YAML::Node& node,
                                    DesignMdParseResult& result) {
    if (!node || !node.IsMap()) return;
    static const std::unordered_set<std::string> valid = {
        "fontFamily", "fontSize", "fontWeight", "lineHeight",
        "letterSpacing", "fontFeature", "fontVariation",
    };
    for (auto level = node.begin(); level != node.end(); ++level) {
        const auto level_name = level->first.as<std::string>("");
        if (level_name.empty() || !level->second.IsMap()) continue;
        for (auto field = level->second.begin(); field != level->second.end(); ++field) {
            const auto field_name = field->first.as<std::string>("");
            if (field_name.empty() || valid.count(field_name) != 0) continue;
            result.diagnostics.push_back(diagnostic(
                DesignMdSeverity::warning, "unknown-typography-property",
                "typography." + level_name + "." + field_name, field->first,
                "`" + field_name + "` is not a recognized typography property"));
        }
    }
}

} // namespace

void apply_designmd_frontmatter_schema(const YAML::Node& root,
                                       DesignMdParseResult& result) {
    parse_omitted(root["omitted"], result);
    validate_typography_properties(root["typography"], result);
    validate_collisions(root["colors"], "colors", result);
    validate_collisions(root["rounded"], "rounded", result);
    validate_collisions(root["spacing"], "spacing", result);
    validate_collisions(root["shadows"], "shadows", result);
}

bool designmd_section_is_omitted(const DesignMdParseResult& parsed,
                                 std::string_view section) {
    const auto expected = lower(std::string(section));
    return std::any_of(parsed.omitted_sections.begin(), parsed.omitted_sections.end(),
                       [&](const DesignMdOmittedSection& item) {
                           return lower(item.section) == expected;
                       });
}

void append_designmd_omitted_findings(
    const DesignMdParseResult& parsed,
    std::vector<DesignMdDiagnostic>& findings) {
    static const std::unordered_set<std::string> valid = {
        "colors", "typography", "spacing", "rounded", "components",
    };
    const auto& tokens = parsed.ir.tokens;
    auto has_tokens = [&](const std::string& section) {
        if (section == "colors") return !tokens.colors.empty();
        if (section == "typography") {
            return std::any_of(tokens.strings.begin(), tokens.strings.end(),
                               [](const auto& entry) {
                                   return entry.first.rfind("typography.", 0) == 0;
                               });
        }
        if (section == "components") {
            return std::any_of(tokens.strings.begin(), tokens.strings.end(),
                               [](const auto& entry) {
                                   return entry.first.rfind("components.", 0) == 0;
                               });
        }
        const auto prefix = section + "-";
        return std::any_of(tokens.dimensions.begin(), tokens.dimensions.end(),
                           [&](const auto& entry) {
                               return entry.first.rfind(prefix, 0) == 0;
                           });
    };

    for (const auto& item : parsed.omitted_sections) {
        const auto section = lower(item.section);
        if (valid.count(section) == 0) {
            findings.push_back({DesignMdSeverity::warning, "unknown-omission",
                                "omitted", 0, 0,
                                "unknown section `" + item.section +
                                "` in omitted declaration"});
        } else if (has_tokens(section)) {
            findings.push_back({DesignMdSeverity::warning, "redundant-omission",
                                "omitted", 0, 0,
                                section + " is listed as omitted but defines tokens"});
        } else {
            auto message = section + " is intentionally omitted";
            if (!item.reason.empty()) message += ": " + item.reason;
            findings.push_back({DesignMdSeverity::info, "declared-omission",
                                "omitted." + section, 0, 0, std::move(message)});
        }
    }
}

} // namespace pulp::view
