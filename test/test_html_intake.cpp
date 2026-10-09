#include <catch2/catch_test_macros.hpp>

#include "support/unique_temp_dir.hpp"
#include "tools/import-design/browser_html_import.hpp"
#include "tools/import-design/html_intake.hpp"

#include <filesystem>
#include <fstream>

using pulp::import_design::HtmlExportShape;
using pulp::import_design::classify_html_intake;
using pulp::import_design::import_browser_html;

namespace {

namespace fs = std::filesystem;

struct CaptureRuntimeFixture {
    fs::path root = pulp::test::make_unique_temp_dir("pulp-browser-provenance-import");
    fs::path importer;

    CaptureRuntimeFixture() {
        importer = root / "pulp-import-design";
        fs::copy_file(PULP_BROWSER_CAPTURE_FIXTURE_PATH, importer);
        fs::permissions(importer,
                        fs::perms::owner_read | fs::perms::owner_write | fs::perms::owner_exec,
                        fs::perm_options::add);
        const auto runtime = root / "browser_capture";
        fs::create_directories(runtime);
        std::ofstream(runtime / "capture.mjs") << "// fixture";
        fs::copy_file(PULP_BROWSER_CAPTURE_FIXTURE_PATH, runtime / "node");
        fs::permissions(runtime / "node",
                        fs::perms::owner_read | fs::perms::owner_write | fs::perms::owner_exec,
                        fs::perm_options::add);
    }

    ~CaptureRuntimeFixture() {
        std::error_code ec;
        fs::remove_all(root, ec);
    }
};

} // namespace

TEST_CASE("HTML intake chooses one browser evaluator across Claude export shapes",
          "[import-design][browser-capture][intake]") {
    SECTION("project bundle containing text/x-dc") {
        const auto decision = classify_html_intake(
            "Delay.html",
            R"(<script type="__bundler/manifest"></script>
               <script type="text/x-dc">export default function App(){}</script>)");
        REQUIRE(decision.use_browser);
        REQUIRE(decision.shape == HtmlExportShape::claude_project_bundle);
    }

    SECTION("standalone bundle") {
        const auto decision = classify_html_intake(
            "editor.html",
            R"(<html><script type="__bundler/template"></script></html>)");
        REQUIRE(decision.use_browser);
        REQUIRE(decision.shape == HtmlExportShape::claude_standalone_bundle);
    }

    SECTION("design component with a relative support script") {
        const auto decision = classify_html_intake(
            "ForgeModular.dc.html",
            R"(<script src="./support.js"></script>
               <script type="text/x-dc">export default function App(){}</script>)");
        REQUIRE(decision.use_browser);
        REQUIRE(decision.shape == HtmlExportShape::claude_design_component);
    }

    SECTION("ordinary HTML needs no source-specific incantation") {
        const auto decision =
            classify_html_intake("screen.html", "<!doctype html><main>Hello</main>");
        REQUIRE(decision.use_browser);
        REQUIRE(decision.shape == HtmlExportShape::generic_html);
    }

    SECTION("extensionless HTML recognizes common roots after a UTF-8 BOM") {
        const auto decision = classify_html_intake(
            "screen", "\xEF\xBB\xBF  <main>Hello</main>");
        REQUIRE(decision.use_browser);
        REQUIRE(decision.shape == HtmlExportShape::generic_html);
    }

    SECTION("HTML extension matching is case-insensitive and includes htm") {
        REQUIRE(classify_html_intake("SCREEN.HTML", "<main>Hello</main>")
                    .use_browser);
        REQUIRE(classify_html_intake("screen.htm", "<main>Hello</main>")
                    .use_browser);
    }

    SECTION("non-HTML stays on its existing source lane") {
        const auto decision =
            classify_html_intake("tokens.json", R"({"colors":{"accent":"#f0f"}})");
        REQUIRE_FALSE(decision.use_browser);
        REQUIRE(decision.shape == HtmlExportShape::not_html);
    }

    SECTION("HTML-like text inside serialized JSON is not markup") {
        const auto decision = classify_html_intake(
            "design.json",
            R"({"source":"claude","root":{"name":"<script","type":"frame"}})");
        REQUIRE_FALSE(decision.use_browser);
    }
}

TEST_CASE("explicit non-HTML source cannot be stolen by browser sniffing",
          "[import-design][browser-capture][intake]") {
    pulp::import_design::BrowserHtmlImportRequest request;
    request.input_file = "design.json";
    request.source = pulp::view::DesignSource::figma;
    const auto result = import_browser_html(
        request,
        R"({"root":{"name":"<script","type":"frame"}})");
    REQUIRE(std::holds_alternative<
            pulp::import_design::BrowserHtmlNotApplicable>(result));

    request.source = pulp::view::DesignSource::claude;
    const auto claude_result = import_browser_html(
        request,
        R"({"source":"claude","root":{"name":"<body","type":"frame"}})");
    REQUIRE(std::holds_alternative<
            pulp::import_design::BrowserHtmlNotApplicable>(claude_result));
}

TEST_CASE("browser HTML import rejects a capture without provenance before lowering",
          "[import-design][browser-capture][provenance][negative]") {
    CaptureRuntimeFixture runtime;
    pulp::import_design::BrowserHtmlImportRequest request;
    request.input_file = runtime.root / "editor.html";
    request.output_file = runtime.root / "out/ui.js";
    request.importer_executable = runtime.importer;
    request.browser_executable = PULP_BROWSER_CAPTURE_FIXTURE_PATH;
    request.source = pulp::view::DesignSource::claude;
    std::ofstream(runtime.root / "editor.html") << "<!doctype html><main>fixture</main>";

    const auto result = import_browser_html(request, "<!doctype html><main>fixture</main>");
    const auto* failure = std::get_if<pulp::import_design::BrowserHtmlFailure>(&result);
    REQUIRE(failure);
    INFO(failure->error);
    CHECK(failure->exit_code == 3);
    CHECK(failure->error.find("browser-capture-provenance-invalid") != std::string::npos);
}
