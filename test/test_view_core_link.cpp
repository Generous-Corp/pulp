#include <catch2/catch_test_macros.hpp>

#include <pulp/view/design_import.hpp>
#include <pulp/view/view.hpp>

#include <string>
#include <utility>
#include <vector>

using namespace pulp::view;

TEST_CASE("view core target links baked design import without script runtime",
          "[view][import][core-link][phase-9]") {
    DesignIR ir;
    ir.source = DesignSource::figma;
    ir.root.type = "frame";
    ir.root.name = "Panel";
    ir.root.layout.direction = LayoutDirection::column;
    ir.root.style.width = 240.0f;
    ir.root.style.height = 120.0f;
    ir.root.style.background_color = "#101820";

    IRNode label;
    label.type = "text";
    label.name = "Title";
    label.text_content = "Baked UI";
    label.style.font_size = 16.0f;
    label.style.color = "#f2f2f2";
    ir.root.children.push_back(std::move(label));

    std::vector<ImportDiagnostic> diagnostics;
    auto root = build_native_view_tree(ir, ir.asset_manifest, {.diagnostics_out = &diagnostics});
    REQUIRE(root != nullptr);
    REQUIRE(root->child_count() == 1);

    const auto generated = generate_pulp_cpp(ir, ir.asset_manifest, {});
    REQUIRE(generated.header.find("build_imported_ui") != std::string::npos);
    REQUIRE(generated.source.find("build_imported_ui") != std::string::npos);
}

TEST_CASE("view core classname extraction reads bundled template styles",
          "[view][import][core-link][phase-9]") {
    const std::string html =
        R"HTML(<html><head><script type="__bundler/template">)HTML"
        R"JSON("<html><head><style>.panel { color: #abcdef; background-color: #123456; }</style></head><body></body></html>")JSON"
        R"HTML(</script></head><body></body></html>)HTML";

    const auto rules = extract_claude_classnames(html);
    REQUIRE(rules.at("panel").at("color") == "#abcdef");
    REQUIRE(rules.at("panel").at("backgroundColor") == "#123456");
}
