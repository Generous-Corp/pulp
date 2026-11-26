#include <catch2/catch_test_macros.hpp>

#include "tools/cli/import_design_moved.hpp"

#include <iostream>
#include <sstream>
#include <string>
#include <vector>

using pulp::cli::report_import_design_moved;
using pulp::cli::vellum_import_spelling;

TEST_CASE("import-design maps each source lane to its vellum import spelling", "[cli][import]") {
    using Args = std::vector<std::string>;
    CHECK(vellum_import_spelling(Args{"--from", "pencil", "--file", "panel.pen"}) ==
          "vellum import pencil panel.pen");
    CHECK(vellum_import_spelling(Args{"--from", "designmd", "--file", "DESIGN.md"}) ==
          "vellum import designmd DESIGN.md");
    CHECK(vellum_import_spelling(Args{"--from", "claude", "--file", "export.html"}) ==
          "vellum import --source-type claude-design export.html");
    CHECK(vellum_import_spelling(
              Args{"--from", "fig", "--file", "a b.fig", "--page", "Main", "--frame", "Panel"}) ==
          "vellum import --source-type fig 'a b.fig' --page Main --frame Panel");
    CHECK(vellum_import_spelling(Args{"--from", "fig", "--file", "x.fig", "--outline"}) ==
          "vellum import --source-type fig x.fig --outline");
    CHECK(vellum_import_spelling(
              Args{"--from", "figma-plugin", "--file", "a.json", "--file", "b.json"}) ==
          "vellum import --source-type figma-plugin a.json --file b.json");
    CHECK(vellum_import_spelling(Args{"--from", "pdf", "--file", "art.pdf"}) ==
          "vellum import illustrator art.pdf");
    CHECK(vellum_import_spelling(Args{"--file", "design.json"}) == "vellum import design.json");
    CHECK(vellum_import_spelling(Args{"--from", "v0"}) == "vellum import v0 FILE");
}

TEST_CASE("import-design reports a source Vellum cannot import yet", "[cli][import]") {
    CHECK(vellum_import_spelling({"--from", "stitch", "--file", "screen.html"}).empty());

    std::ostringstream err;
    auto* old = std::cerr.rdbuf(err.rdbuf());
    const int code = report_import_design_moved({"--from", "stitch", "--file", "screen.html"});
    std::cerr.rdbuf(old);
    CHECK(code == 2);
    CHECK(err.str().find("now\nlives in the Vellum CLI") != std::string::npos);
    CHECK(err.str().find("Vellum has no import route for this source yet.") != std::string::npos);
}

TEST_CASE("import-design translates a baked emit in full", "[cli][import]") {
    using Args = std::vector<std::string>;
    CHECK(vellum_import_spelling(Args{"--from", "figma", "--file", "d.json", "--mode", "baked",
                                      "--emit", "cpp", "--output", "ui.cpp"}) ==
          "vellum import --source-type figma d.json --mode baked --emit cpp --out ui.cpp");
    CHECK(vellum_import_spelling(Args{"--from", "pencil", "--file", "p.pen", "--mode", "baked",
                                      "--emit", "swiftui", "--output", "Panel.swift"}) ==
          "vellum import pencil p.pen --mode baked --emit swiftui --out Panel.swift");
    CHECK(vellum_import_spelling(Args{"--from", "jsx", "--file", "b.js", "--mode", "baked",
                                      "--emit", "ir-json", "--output", "ui.json"}) ==
          "vellum import jsx b.js --mode baked --emit ir-json --out ui.json");
}

TEST_CASE("import-design names an argument vellum import cannot express", "[cli][import]") {
    using Args = std::vector<std::string>;
    const Args project_output{"--from", "figma", "--file", "d.json", "--output", "ui.js"};
    CHECK(vellum_import_spelling(project_output).empty());
    CHECK(pulp::cli::unsupported_import_design_option(project_output) == "--output");
    CHECK(pulp::cli::unsupported_import_design_option(
              Args{"--from", "figma", "--file", "d.json", "--tokens", "t.json"}) == "--tokens");
    CHECK(pulp::cli::unsupported_import_design_option(Args{"--url", "https://example.invalid"}) ==
          "--url");
    CHECK(pulp::cli::unsupported_import_design_option(
              Args{"--from", "jsx", "--file", "b.js", "--mode", "baked", "--emit", "cpp"}) ==
          "--emit cpp --from jsx");
    CHECK(pulp::cli::unsupported_import_design_option(Args{"--from", "pencil", "--file", "p.pen"})
              .empty());

    std::ostringstream err;
    auto* old = std::cerr.rdbuf(err.rdbuf());
    const int code = report_import_design_moved(project_output);
    std::cerr.rdbuf(old);
    CHECK(code == 2);
    CHECK(err.str().find("`vellum import` has no equivalent for --output.") != std::string::npos);
    CHECK(err.str().find("Equivalent:") == std::string::npos);
}

TEST_CASE("import-design quotes shell metacharacters in the spelling", "[cli][import]") {
    using Args = std::vector<std::string>;
    CHECK(vellum_import_spelling(Args{"--from", "pencil", "--file", "a;b(1)*.pen"}) ==
          "vellum import pencil 'a;b(1)*.pen'");
    CHECK(vellum_import_spelling(Args{"--from", "pencil", "--file", "x|y&z>w"}) ==
          "vellum import pencil 'x|y&z>w'");
}
