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
    CHECK(vellum_import_spelling(Args{"--from", "fig", "--file", "a b.fig", "--page", "Main",
                                      "--frame", "Panel"}) ==
          "vellum import --source-type fig 'a b.fig' --page Main --frame Panel");
    CHECK(vellum_import_spelling(Args{"--from", "fig", "--file", "x.fig", "--outline"}) ==
          "vellum import --source-type fig x.fig --outline");
    CHECK(vellum_import_spelling(Args{"--from", "figma-plugin", "--file", "a.json", "--file",
                                      "b.json"}) ==
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
