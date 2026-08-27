#include <catch2/catch_test_macros.hpp>

#include "tools/import-design/browser_html_import.hpp"
#include "tools/import-design/html_intake.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>

using pulp::import_design::HtmlExportShape;
using pulp::import_design::classify_html_intake;
using pulp::import_design::find_bundled_browser_runtime;
using pulp::import_design::import_browser_html;

namespace fs = std::filesystem;

namespace {

struct RuntimeTree {
    fs::path root =
        fs::temp_directory_path() /
        ("pulp-browser-runtime-test-" +
         std::to_string(std::chrono::steady_clock::now()
                            .time_since_epoch()
                            .count()));

    RuntimeTree() { fs::create_directories(root); }
    ~RuntimeTree() {
        std::error_code ec;
        fs::remove_all(root, ec);
    }

    fs::path write(const fs::path& relative) const {
        const auto path = root / relative;
        fs::create_directories(path.parent_path());
        std::ofstream(path) << "fixture";
        return path;
    }
};

}  // namespace

TEST_CASE("browser import discovers its bundled Node runtime beside the importer",
          "[import-design][browser-capture][runtime]") {
    RuntimeTree tree;
    const auto importer = tree.write("bin/pulp-import-design");
#ifdef _WIN32
    constexpr auto node_name = "node.exe";
#else
    constexpr auto node_name = "node";
#endif

    SECTION("the versioned runtime directory wins") {
        const auto expected_node =
            tree.write(fs::path("bin/browser_capture-v1") / node_name);
        const auto expected_script =
            tree.write("bin/browser_capture-v1/capture.mjs");
        tree.write(fs::path("bin/browser_capture") / node_name);
        tree.write("bin/browser_capture/capture.mjs");

        const auto runtime = find_bundled_browser_runtime(importer);
        REQUIRE(runtime.node_executable);
        REQUIRE(runtime.capture_script);
        REQUIRE(fs::equivalent(*runtime.node_executable, expected_node));
        REQUIRE(fs::equivalent(*runtime.capture_script, expected_script));
    }

    SECTION("Node and capture script fall back independently") {
        const auto expected_node =
            tree.write(fs::path("bin/browser_capture-v1") / node_name);
        const auto expected_script =
            tree.write("bin/browser_capture/capture.mjs");

        const auto runtime = find_bundled_browser_runtime(importer);
        REQUIRE(runtime.node_executable);
        REQUIRE(runtime.capture_script);
        REQUIRE(fs::equivalent(*runtime.node_executable, expected_node));
        REQUIRE(fs::equivalent(*runtime.capture_script, expected_script));
    }
}

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
