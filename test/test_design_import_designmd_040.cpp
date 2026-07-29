// DESIGN.md 0.4 compatibility and malformed-input regressions.

#include <catch2/catch_test_macros.hpp>
#include <pulp/view/design_import.hpp>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

using namespace pulp::view;
namespace fs = std::filesystem;

namespace {

std::string read_fixture(const std::string& relative_path) {
    const auto path = fs::path(PULP_REPO_ROOT) / relative_path;
    std::ifstream file(path);
    if (!file.is_open()) return {};
    std::ostringstream content;
    content << file.rdbuf();
    return content.str();
}

bool has_diag_code(const std::vector<DesignMdDiagnostic>& diagnostics,
                   std::string_view code) {
    return std::any_of(diagnostics.begin(), diagnostics.end(),
                       [&](const auto& diagnostic) {
                           return diagnostic.code == code;
                       });
}

std::string nested_spacing_document(int levels) {
    std::string markdown =
        "---\n"
        "name: Bounded\n"
        "colors:\n"
        "  primary: \"#123456\"\n"
        "spacing:\n";
    for (int i = 0; i < levels; ++i) {
        markdown += std::string(static_cast<size_t>(i + 1) * 2, ' ') +
                    "level" + std::to_string(i) + ":\n";
    }
    markdown += std::string(static_cast<size_t>(levels + 1) * 2, ' ') +
                "value: 8px\n"
                "  valid: 4px\n"
                "---\n";
    return markdown;
}

std::string nested_spacing_token_name(int levels) {
    std::string name = "spacing";
    for (int i = 0; i < levels; ++i) {
        name += i == 0 ? "-level0" : ".level" + std::to_string(i);
    }
    return name + ".value";
}

} // namespace

TEST_CASE("omitted accepts bare and structured declarations and suppresses missing findings",
          "[view][import][designmd][parse][designmd040]") {
    const auto text = read_fixture(
        "test/fixtures/imports/designmd/alpha/designmd-0.4-compat.md");
    REQUIRE_FALSE(text.empty());

    auto parsed = parse_designmd(text);
    REQUIRE(parsed.omitted_sections.size() == 2);
    CHECK(parsed.omitted_sections[0].section == "spacing");
    CHECK(parsed.omitted_sections[1].section == "rounded");
    CHECK(parsed.omitted_sections[1].reason == "Rectangular hardware language");
    CHECK_FALSE(has_diag_code(parsed.diagnostics, "unknown-key"));

    const auto findings = lint_designmd(parsed);
    CHECK_FALSE(std::any_of(findings.begin(), findings.end(), [](const auto& d) {
        return d.code == "missing-sections" &&
               (d.path == "spacing" || d.path == "rounded");
    }));
    CHECK(std::count_if(findings.begin(), findings.end(), [](const auto& d) {
        return d.code == "declared-omission";
    }) == 2);
}

TEST_CASE("omitted lint reports unknown and redundant declarations",
          "[view][import][designmd][lint][designmd040]") {
    auto parsed = parse_designmd(
        "---\n"
        "name: Omitted checks\n"
        "omitted:\n"
        "  - colors\n"
        "  - animation\n"
        "colors:\n"
        "  primary: \"#000000\"\n"
        "---\n");
    const auto findings = lint_designmd(parsed);
    CHECK(has_diag_code(findings, "redundant-omission"));
    CHECK(has_diag_code(findings, "unknown-omission"));
}

TEST_CASE("unrecognized typography sub-properties warn without dropping source data",
          "[view][import][designmd][lint][designmd040]") {
    auto parsed = parse_designmd(
        "---\n"
        "name: Typography typo\n"
        "typography:\n"
        "  heading:\n"
        "    fontFamily: Inter\n"
        "    fontSze: 24px\n"
        "---\n");
    REQUIRE(has_diag_code(parsed.diagnostics, "unknown-typography-property"));
    CHECK(parsed.ir.tokens.strings.at("typography.heading.fontSze") == "24px");
}

TEST_CASE("flat and grouped token names that serialize identically are rejected",
          "[view][import][designmd][lint][designmd040]") {
    auto parsed = parse_designmd(
        "---\n"
        "name: Collision\n"
        "colors:\n"
        "  brand-primary: \"#111111\"\n"
        "  brand:\n"
        "    primary: \"#222222\"\n"
        "---\n");
    REQUIRE(has_diag_code(parsed.diagnostics, "token-name-collision"));
    const auto finding = std::find_if(
        parsed.diagnostics.begin(), parsed.diagnostics.end(), [](const auto& d) {
            return d.code == "token-name-collision";
        });
    REQUIRE(finding != parsed.diagnostics.end());
    CHECK(finding->severity == DesignMdSeverity::error);
}

TEST_CASE("token nesting accepts 21 path segments and rejects 22 branch-locally",
          "[view][import][designmd][security][designmd040]") {
    SECTION("20 nested objects plus the scalar leaf are accepted") {
        const auto parsed = parse_designmd(nested_spacing_document(20));

        CHECK_FALSE(has_diag_code(parsed.diagnostics, "token-nesting-depth"));
        CHECK(parsed.ir.tokens.dimensions.size() == 2);
        CHECK(parsed.ir.tokens.dimensions.at(nested_spacing_token_name(20)) == 8.0f);
        CHECK(parsed.ir.tokens.dimensions.at("spacing-valid") == 4.0f);
        CHECK(parsed.ir.tokens.colors.at("primary") == "#123456");
    }

    SECTION("21 nested objects reject only that branch") {
        const auto parsed = parse_designmd(nested_spacing_document(21));

        CHECK(has_diag_code(parsed.diagnostics, "token-nesting-depth"));
        CHECK(parsed.ir.tokens.dimensions.size() == 1);
        CHECK(parsed.ir.tokens.dimensions.at("spacing-valid") == 4.0f);
        CHECK(parsed.ir.tokens.colors.at("primary") == "#123456");
    }
}

TEST_CASE("dimension recognition rejects oversized and post-conversion nonfinite values",
          "[view][import][designmd][security][designmd040]") {
    const std::string oversized(65, '9');
    const std::string overflowing_rem =
        "30000000000000000000000000000000000000rem";
    auto parsed = parse_designmd(
        "---\n"
        "name: Dimensions\n"
        "spacing:\n"
        "  oversized: \"" + oversized + "px\"\n"
        "  overflow: \"" + overflowing_rem + "\"\n"
        "  valid: 2rem\n"
        "---\n");

    CHECK(parsed.ir.tokens.dimensions.count("spacing-oversized") == 0);
    CHECK(parsed.ir.tokens.strings.at("spacing-oversized") == oversized + "px");
    CHECK(parsed.ir.tokens.dimensions.count("spacing-overflow") == 0);
    CHECK(parsed.ir.tokens.strings.at("spacing-overflow") == overflowing_rem);
    CHECK(parsed.ir.tokens.dimensions.at("spacing-valid") == 32.0f);
}
