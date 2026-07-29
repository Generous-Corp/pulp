// SPDX-License-Identifier: MIT
#include "tools/import-design/html_project_stager.hpp"
#include "tools/import-design/browser_capture_workspace.hpp"
#include "tools/import-design/claude_html_dependencies.hpp"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;
using pulp::import_design::stage_html_project;
using pulp::import_design::commit_browser_capture_directory;
using pulp::import_design::claude_html_dependency_roots;
using pulp::import_design::HtmlProjectStageOptions;

namespace {

struct TempTree {
    fs::path root =
        fs::temp_directory_path() /
        ("pulp-html-stage-test-" +
         std::to_string(std::chrono::steady_clock::now()
                            .time_since_epoch()
                            .count()));

    TempTree() { fs::create_directories(root); }
    ~TempTree() {
        std::error_code ec;
        fs::remove_all(root, ec);
    }

    void write(const fs::path& relative, std::string_view content) {
        fs::create_directories((root / relative).parent_path());
        std::ofstream output(root / relative, std::ios::binary);
        output << content;
    }
};

}  // namespace

TEST_CASE("HTML staging copies only the explicit contained dependency graph",
          "[import-design][browser-capture][staging]") {
    TempTree tree;
    tree.write("index.html",
               R"(<link href="assets/theme.css"><script src="app.js"></script>)");
    tree.write(
        "app.js",
        R"(import("./modules/ui.js"); import "./modules/setup.js";)");
    tree.write("modules/ui.js", "globalThis.ready = true;");
    tree.write("modules/setup.js", "globalThis.setup = true;");
    tree.write("assets/theme.css", R"(@font-face{src:url("../fonts/ui.woff2")})");
    tree.write("fonts/ui.woff2", "font");
    tree.write("unrelated-secret.txt", "do not stage");

    const auto source =
        R"(<link href="assets/theme.css"><script src="app.js"></script>)";
    auto staged = stage_html_project(tree.root / "index.html", source);
    INFO(staged.error);
    REQUIRE(staged);
    CHECK(fs::exists(staged.entry));
    CHECK(fs::exists(staged.root / "app.js"));
    CHECK(fs::exists(staged.root / "modules/ui.js"));
    CHECK(fs::exists(staged.root / "modules/setup.js"));
    CHECK(fs::exists(staged.root / "assets/theme.css"));
    CHECK(fs::exists(staged.root / "fonts/ui.woff2"));
    CHECK_FALSE(fs::exists(staged.root / "unrelated-secret.txt"));
}

TEST_CASE("HTML staging enforces the per-file limit on the entry itself",
          "[import-design][browser-capture][staging]") {
    TempTree tree;
    const auto entry = tree.root / "oversized.html";
    tree.write("oversized.html", "<!doctype html>");
    fs::resize_file(entry, 32ull * 1024ull * 1024ull + 1);

    const auto staged = stage_html_project(entry, "<!doctype html>");

    CHECK_FALSE(staged);
    CHECK(staged.error == "HTML entry exceeds capture staging limits");
}

TEST_CASE("HTML staging never follows parent traversal",
          "[import-design][browser-capture][staging]") {
    TempTree tree;
    tree.write("project/index.html", R"(<img src="../secret.png">)");
    tree.write("secret.png", "secret");

    auto staged = stage_html_project(
        tree.root / "project/index.html",
        R"(<img src="../secret.png">)");
    INFO(staged.error);
    REQUIRE(staged);
    CHECK_FALSE(fs::exists(staged.root / "secret.png"));
}

TEST_CASE("HTML staging treats root-relative URLs as project-root dependencies",
          "[import-design][browser-capture][staging]") {
    TempTree tree;
    tree.write("index.html", R"(<script src="/assets/app.js"></script>)");
    tree.write("assets/app.js", "globalThis.ready = true;");

    auto staged = stage_html_project(
        tree.root / "index.html",
        R"(<script src="/assets/app.js"></script>)");

    INFO(staged.error);
    REQUIRE(staged);
    CHECK(fs::exists(staged.root / "assets/app.js"));
}

TEST_CASE("HTML staging copies every local responsive image candidate",
          "[import-design][browser-capture][staging]") {
    TempTree tree;
    const auto source =
        R"(<picture><source srcset="images/a.webp 1x, images/b.webp 2x">)"
        R"(<img src="images/fallback.png" )"
        R"(srcset="images/c.png 320w, /images/d.png 640w"></picture>)";
    tree.write("index.html", source);
    for (const auto* image : {
             "images/a.webp",
             "images/b.webp",
             "images/c.png",
             "images/d.png",
             "images/fallback.png"}) {
        tree.write(image, image);
    }

    auto staged = stage_html_project(
        tree.root / "index.html",
        source);

    INFO(staged.error);
    REQUIRE(staged);
    CHECK(fs::exists(staged.root / "images/a.webp"));
    CHECK(fs::exists(staged.root / "images/b.webp"));
    CHECK(fs::exists(staged.root / "images/c.png"));
    CHECK(fs::exists(staged.root / "images/d.png"));
    CHECK(fs::exists(staged.root / "images/fallback.png"));
}

TEST_CASE("HTML staging preserves nested root-relative dependency semantics",
          "[import-design][browser-capture][staging]") {
    TempTree tree;
    tree.write(
        "index.html",
        R"(<script type="module" src="scripts/main.js"></script>)");
    tree.write("scripts/main.js", R"(import "/modules/child.js";)");
    tree.write("modules/child.js", "globalThis.ready = true;");

    auto staged = stage_html_project(
        tree.root / "index.html",
        R"(<script type="module" src="scripts/main.js"></script>)");

    INFO(staged.error);
    REQUIRE(staged);
    CHECK(fs::exists(staged.root / "scripts/main.js"));
    CHECK(fs::exists(staged.root / "modules/child.js"));
    CHECK_FALSE(fs::exists(staged.root / "scripts/modules/child.js"));
}

TEST_CASE("HTML staging scans transitive imports in extensionless modules",
          "[import-design][browser-capture][staging]") {
    TempTree tree;
    tree.write(
        "index.html",
        R"(<script type="module" src="bootstrap"></script>)");
    tree.write("bootstrap", R"(import "./modules/child.js";)");
    tree.write("modules/child.js", "globalThis.ready = true;");

    auto staged = stage_html_project(
        tree.root / "index.html",
        R"(<script type="module" src="bootstrap"></script>)");

    INFO(staged.error);
    REQUIRE(staged);
    CHECK(fs::exists(staged.root / "bootstrap"));
    CHECK(fs::exists(staged.root / "modules/child.js"));
}

TEST_CASE("HTML staging upgrades an already copied extensionless dependency",
          "[import-design][browser-capture][staging]") {
    TempTree tree;
    tree.write(
        "index.html",
        R"(<script src="a.js"></script><script type="module" src="b.js"></script>)");
    tree.write("a.js", R"(fetch("./bootstrap");)");
    tree.write("b.js", R"(import "./bootstrap";)");
    tree.write("bootstrap", R"(import "./modules/child.js";)");
    tree.write("modules/child.js", "globalThis.ready = true;");

    auto staged = stage_html_project(
        tree.root / "index.html",
        R"(<script src="a.js"></script><script type="module" src="b.js"></script>)");

    INFO(staged.error);
    REQUIRE(staged);
    CHECK(fs::exists(staged.root / "bootstrap"));
    CHECK(fs::exists(staged.root / "modules/child.js"));
}

TEST_CASE("HTML staging includes a runtime-bound Claude design system",
          "[import-design][browser-capture][staging]") {
    TempTree tree;
    tree.write(
        "index.html", R"(<script src="ds-base.js"></script>)");
    tree.write(
        "ds-base.js",
        R"(const base = "_ds/pulp-design-system-123";
           for (const p of ["tokens/fonts.css", "styles.css"]) {
             document.head.append(p, `${base}/${p}`);
           })");
    tree.write(
        "_ds/pulp-design-system-123/styles.css",
        R"(@import url("tokens/fonts.css");)");
    tree.write(
        "_ds/pulp-design-system-123/tokens/fonts.css",
        R"(@font-face { font-family: "Fixture"; })");
    tree.write("unrelated-secret.txt", "do not stage");

    auto staged = stage_html_project(
        tree.root / "index.html",
        R"(<script src="ds-base.js"></script>)",
        HtmlProjectStageOptions{claude_html_dependency_roots});
    INFO(staged.error);
    REQUIRE(staged);
    CHECK(fs::exists(
        staged.root /
        "_ds/pulp-design-system-123/tokens/fonts.css"));
    CHECK_FALSE(fs::exists(staged.root / "unrelated-secret.txt"));
}

TEST_CASE("HTML staging caps provider file discovery before copying",
          "[import-design][browser-capture][staging][limits]") {
    TempTree tree;
    const auto source =
        R"(<script>const base = "_ds/oversized-design-system";</script>)";
    tree.write("index.html", source);
    const auto dependency_root =
        tree.root / "_ds/oversized-design-system";
    fs::create_directories(dependency_root);
    for (std::size_t index = 0; index <= 4096; ++index) {
        std::ofstream(
            dependency_root /
            ("module-" + std::to_string(index) + ".mjs"));
    }

    const auto staged = stage_html_project(
        tree.root / "index.html", source,
        HtmlProjectStageOptions{claude_html_dependency_roots});

    CHECK_FALSE(staged);
    CHECK(staged.error == "HTML dependency graph exceeds 4096 files");
    CHECK(staged.dependencies.empty());
    CHECK_FALSE(fs::exists(
        staged.root / "_ds/oversized-design-system/module-0.mjs"));
}

TEST_CASE("HTML staging caps provider bytes during discovery before copying",
          "[import-design][browser-capture][staging][limits]") {
    TempTree tree;
    const auto source =
        R"(<script>const base = "_ds/oversized-design-system";</script>)";
    tree.write("index.html", source);
    const auto dependency_root =
        tree.root / "_ds/oversized-design-system";
    fs::create_directories(dependency_root);
    for (std::size_t index = 0; index < 9; ++index) {
        const auto dependency =
            dependency_root /
            ("chunk-" + std::to_string(index) + ".bin");
        std::ofstream output(dependency);
        output.close();
        fs::resize_file(dependency, 32ull * 1024ull * 1024ull);
    }

    const auto staged = stage_html_project(
        tree.root / "index.html", source,
        HtmlProjectStageOptions{claude_html_dependency_roots});

    CHECK_FALSE(staged);
    CHECK(staged.error ==
          "HTML dependency graph exceeds capture staging limits");
    CHECK(staged.dependencies.empty());
    CHECK_FALSE(fs::exists(
        staged.root / "_ds/oversized-design-system/chunk-0.bin"));
}

TEST_CASE("generic HTML staging does not infer Claude dependency roots",
          "[import-design][browser-capture][staging]") {
    TempTree tree;
    tree.write("index.html", R"(<script src="ds-base.js"></script>)");
    tree.write(
        "ds-base.js",
        R"(const base = "_ds/pulp-design-system-123";)");
    tree.write(
        "_ds/pulp-design-system-123/styles.css",
        "body { color: white; }");

    auto staged = stage_html_project(
        tree.root / "index.html",
        R"(<script src="ds-base.js"></script>)");

    INFO(staged.error);
    REQUIRE(staged);
    CHECK(fs::exists(staged.root / "ds-base.js"));
    CHECK_FALSE(fs::exists(
        staged.root / "_ds/pulp-design-system-123/styles.css"));
}

TEST_CASE("browser capture commit replaces only marked owned evidence",
          "[import-design][browser-capture][transaction]") {
    TempTree tree;
    tree.write("source/capture.json", "new");
    tree.write("destination/keep.txt", "unowned");
    std::string error;

    CHECK_FALSE(commit_browser_capture_directory(
        tree.root / "source", tree.root / "destination", error));
    CHECK(fs::exists(tree.root / "destination/keep.txt"));

    fs::remove_all(tree.root / "destination");
    tree.write("destination/.pulp-browser-capture-v1", "owned");
    tree.write("destination/capture.json", "old");
    error.clear();
    REQUIRE(commit_browser_capture_directory(
        tree.root / "source", tree.root / "destination", error));
    CHECK_FALSE(fs::exists(tree.root / "destination/keep.txt"));
    CHECK(fs::exists(
        tree.root / "destination/.pulp-browser-capture-v1"));
}
