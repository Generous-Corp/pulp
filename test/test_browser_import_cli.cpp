#include "tools/import-design/browser_import_cli.hpp"
#include "tools/import-design/browser_import_cli_internal.hpp"
#include "tools/import-design/browser_import_session.hpp"
#include "tools/import-design/sprite_skins.hpp"

#include <catch2/catch_test_macros.hpp>
#include <pulp/view/design_import.hpp>
#include <pulp/view/screenshot.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace fs = std::filesystem;
namespace id = pulp::import_design;

namespace {

struct TempTree {
    fs::path root =
        fs::temp_directory_path() /
        ("pulp-browser-import-cli-test-" +
         std::to_string(std::chrono::steady_clock::now()
                            .time_since_epoch()
                            .count()));

    TempTree() { fs::create_directories(root); }
    ~TempTree() {
        std::error_code ec;
        fs::remove_all(root, ec);
    }

    void write(const fs::path& path, std::string_view content) const {
        fs::create_directories(path.parent_path());
        std::ofstream output(path, std::ios::binary);
        output << content;
    }

    void write(const fs::path& path,
               const std::vector<std::uint8_t>& content) const {
        fs::create_directories(path.parent_path());
        std::ofstream output(path, std::ios::binary);
        output.write(
            reinterpret_cast<const char*>(content.data()),
            static_cast<std::streamsize>(content.size()));
    }

    std::string read(const fs::path& path) const {
        std::ifstream input(path, std::ios::binary);
        return {std::istreambuf_iterator<char>(input), {}};
    }
};

id::BrowserHtmlImportResult captured_import(
    const id::BrowserHtmlImportRequest& request,
    const TempTree& tree) {
    const auto transient = tree.root / "transient-capture";
    fs::create_directories(transient);
    tree.write(transient / "browser.png", "reference");
    tree.write(transient / "capture.json", "{}");

    pulp::view::DesignIR design_ir;
    design_ir.source = request.source;
    design_ir.root.name = "Captured";
    design_ir.root.style.width = 640.0f;
    design_ir.root.style.height = 960.0f;
    fs::path durable_capture;
    if (!request.dry_run) {
        auto parent = request.output_file.parent_path();
        if (parent.empty()) parent = fs::current_path();
        durable_capture =
            parent /
            (request.output_file.stem().string() + "-browser-capture");
    }
    return id::BrowserHtmlCaptured{
        "generic-html",
        std::move(design_ir),
        transient,
        std::move(durable_capture),
        transient / "browser.png",
        {},
        {}};
}

id::BrowserImportCliRequest request_for(const TempTree& tree) {
    id::BrowserImportCliRequest request;
    request.input_file = tree.root / "input.html";
    request.output_file = tree.root / "nested/output/ui.js";
    request.importer_executable = tree.root / "pulp-import-design";
    request.source = pulp::view::DesignSource::claude;
    request.initial_width = 1280;
    request.initial_height = 800;
    return request;
}

id::BrowserImportSession require_live_session(
    id::BrowserImportCliResult result) {
    auto normalized =
        id::internal::make_browser_import_session(std::move(result));
    REQUIRE(std::holds_alternative<id::BrowserImportSession>(normalized));
    return std::get<id::BrowserImportSession>(std::move(normalized));
}

}  // namespace

TEST_CASE("browser CLI adapter tags non-browser input as not applicable",
          "[import-design][browser-capture][cli-adapter]") {
    TempTree tree;
    auto request = request_for(tree);
    request.validate = true;
    request.reference_image = "explicit.png";

    id::internal::BrowserImportCliOperations operations;
    operations.import_html =
        [](const id::BrowserHtmlImportRequest&, std::string_view) {
            return id::BrowserHtmlImportResult{};
        };
    operations.validate_capture =
        [](const pulp::view::DesignIR&,
           const id::BrowserCaptureValidationOptions&) {
            FAIL("non-browser input must not validate");
            return id::BrowserCaptureValidationResult{};
        };
    operations.localize_assets =
        [](pulp::view::DesignIR&, const std::string&, std::string*) {
            FAIL("non-browser input must not localize");
            return false;
        };

    const auto result =
        id::internal::run_browser_import_cli_with_operations(
            request, "not html", operations);
    CHECK(std::holds_alternative<id::BrowserImportNotApplicable>(result));
}

TEST_CASE("browser CLI forwards a plan and rejects non-browser input",
          "[import-design][browser-capture][cli-adapter]") {
    TempTree tree;
    auto request = request_for(tree);
    request.browser_interactions = tree.root / "interactions.json";
    std::optional<fs::path> observed;

    id::internal::BrowserImportCliOperations operations;
    operations.import_html =
        [&](const id::BrowserHtmlImportRequest& capture_request,
            std::string_view) {
            observed = capture_request.browser_interactions;
            return id::BrowserHtmlNotApplicable{};
        };
    operations.validate_capture =
        [](const pulp::view::DesignIR&,
           const id::BrowserCaptureValidationOptions&) {
            FAIL("non-browser input must not validate");
            return id::BrowserCaptureValidationResult{};
        };
    operations.localize_assets =
        [](pulp::view::DesignIR&, const std::string&, std::string*) {
            FAIL("non-browser input must not localize");
            return false;
        };

    const auto result =
        id::internal::run_browser_import_cli_with_operations(
            request, "not html", operations);
    const auto* failure = std::get_if<id::BrowserImportFailure>(&result);
    REQUIRE(failure);
    CHECK(failure->exit_code == 2);
    REQUIRE(observed);
    CHECK(*observed == *request.browser_interactions);
}

TEST_CASE("browser interactions cannot select the offline parser",
          "[import-design][browser-capture][cli-adapter]") {
    TempTree tree;
    auto request = request_for(tree);
    request.offline = true;
    request.browser_interactions = tree.root / "interactions.json";

    id::internal::BrowserImportCliOperations operations;
    operations.import_html =
        [](const id::BrowserHtmlImportRequest&, std::string_view) {
            return id::BrowserHtmlLegacyFallback{"generic-html"};
        };
    operations.validate_capture =
        [](const pulp::view::DesignIR&,
           const id::BrowserCaptureValidationOptions&) {
            FAIL("offline input must not validate");
            return id::BrowserCaptureValidationResult{};
        };
    operations.localize_assets =
        [](pulp::view::DesignIR&, const std::string&, std::string*) {
            FAIL("offline input must not localize");
            return false;
        };

    const auto result =
        id::internal::run_browser_import_cli_with_operations(
            request, "<html>", operations);
    const auto* failure = std::get_if<id::BrowserImportFailure>(&result);
    REQUIRE(failure);
    CHECK(failure->exit_code == 2);
}

TEST_CASE("browser import session preserves non-capture result policy",
          "[import-design][browser-capture][session]") {
    std::ostringstream diagnostics;

    SECTION("source-parsed imports pass through publication") {
        auto session =
            require_live_session(id::BrowserImportNotApplicable{});
        CHECK_FALSE(session.has_capture());
        CHECK(session.preparation_policy().runs_source_analysis());
        CHECK_FALSE(session.take_capture_adoption());

        const fs::path destination = "ui.js";
        const auto staged =
            session.stage_primary_output(destination, diagnostics);
        REQUIRE(staged);
        CHECK(*staged == destination);
        CHECK(session.publish(diagnostics));
        CHECK(diagnostics.str().empty());
    }

    SECTION("failures retain their adapter exit code") {
        auto normalized =
            id::internal::make_browser_import_session(
                id::BrowserImportFailure{7});
        const auto* failure =
            std::get_if<id::BrowserImportFailure>(&normalized);
        REQUIRE(failure);
        CHECK(failure->exit_code == 7);
    }
}

TEST_CASE("browser CLI adapter fails before localization when validation fails",
          "[import-design][browser-capture][cli-adapter]") {
    TempTree tree;
    auto request = request_for(tree);
    int localization_calls = 0;

    id::internal::BrowserImportCliOperations operations;
    operations.import_html =
        [&](const id::BrowserHtmlImportRequest& capture_request,
            std::string_view) {
            return captured_import(capture_request, tree);
        };
    operations.validate_capture =
        [](const pulp::view::DesignIR&,
           const id::BrowserCaptureValidationOptions&) {
            id::BrowserCaptureValidationResult result;
            result.error = "fixture validation failure";
            return result;
        };
    operations.localize_assets =
        [&](pulp::view::DesignIR&, const std::string&, std::string*) {
            ++localization_calls;
            return true;
        };

    const auto result =
        id::internal::run_browser_import_cli_with_operations(
            request, "<html>", operations);
    const auto* failure = std::get_if<id::BrowserImportFailure>(&result);
    REQUIRE(failure);
    CHECK(failure->exit_code == 1);
    CHECK(localization_calls == 0);
}

TEST_CASE("browser validation is required with and without --validate",
          "[import-design][browser-capture][cli-adapter][validation-contract]") {
    TempTree tree;
    auto request = request_for(tree);
    bool publish_convenience_artifacts = false;
    SECTION("no flag retains proof only in durable capture evidence") {
        request.validate = false;
    }
    SECTION("--validate also publishes render and diff beside output") {
        request.validate = true;
        publish_convenience_artifacts = true;
    }

    int validation_calls = 0;
    id::internal::BrowserImportCliOperations operations;
    operations.import_html =
        [&](const id::BrowserHtmlImportRequest& capture_request,
            std::string_view) {
            return captured_import(capture_request, tree);
        };
    operations.validate_capture =
        [&](const pulp::view::DesignIR&,
            const id::BrowserCaptureValidationOptions& options) {
            ++validation_calls;
            tree.write(options.rendered, "render");
            tree.write(options.diff, "diff");
            id::BrowserCaptureValidationResult result;
            result.valid = true;
            result.passes = true;
            result.similarity = 1.0f;
            result.total_pixels = 1;
            return result;
        };
    operations.localize_assets =
        [](pulp::view::DesignIR&, const std::string&, std::string*) {
            return true;
        };

    auto result =
        id::internal::run_browser_import_cli_with_operations(
            request, "<html>", operations);
    auto session = require_live_session(std::move(result));
    REQUIRE(session.take_capture_adoption());
    CHECK(validation_calls == 1);

    std::ostringstream diagnostics;
    const auto staged_primary =
        session.stage_primary_output(request.output_file, diagnostics);
    REQUIRE(staged_primary);
    tree.write(*staged_primary, "primary");
    REQUIRE(session.publish(diagnostics));
    CHECK(diagnostics.str().empty());

    const auto durable =
        request.output_file.parent_path() / "ui-browser-capture";
    CHECK(tree.read(
              durable / "validation-proof/render/render.png") ==
          "render");
    CHECK(tree.read(
              durable / "validation-proof/diff/diff.png") ==
          "diff");
    const auto published_render =
        request.output_file.parent_path() /
        "ui-claude-design-render.png";
    const auto published_diff =
        request.output_file.parent_path() /
        "ui-claude-design-diff.png";
    CHECK(fs::exists(published_render) ==
          publish_convenience_artifacts);
    CHECK(fs::exists(published_diff) ==
          publish_convenience_artifacts);
}

TEST_CASE("browser CLI adapter stages proof and commits evidence once",
          "[import-design][browser-capture][cli-adapter][transaction]") {
    TempTree tree;
    auto request = request_for(tree);
    request.diff_output = tree.root / "elsewhere/capture.json";
    std::vector<std::string> order;
    id::BrowserCaptureValidationOptions observed;

    id::internal::BrowserImportCliOperations operations;
    operations.import_html =
        [&](const id::BrowserHtmlImportRequest& capture_request,
            std::string_view) {
            return captured_import(capture_request, tree);
        };
    operations.validate_capture =
        [&](const pulp::view::DesignIR&,
            const id::BrowserCaptureValidationOptions& options) {
            order.push_back("validate");
            observed = options;
            tree.write(options.rendered, "render");
            tree.write(options.diff, "diff");
            id::BrowserCaptureValidationResult result;
            result.valid = true;
            result.passes = true;
            result.similarity = 1.0f;
            result.total_pixels = 1;
            return result;
        };
    operations.localize_assets =
        [&](pulp::view::DesignIR& ir, const std::string& output,
            std::string*) {
            order.push_back("localize");
            const auto staged_output = fs::path(output);
            CHECK(staged_output.parent_path() ==
                  tree.root / "transient-capture/localized-output");
            tree.write(
                staged_output.parent_path() / "assets/reference.png",
                "localized-pixels");
            pulp::view::IRAssetRef asset;
            asset.asset_id = "reference";
            asset.local_path = "assets/reference.png";
            ir.asset_manifest.assets.push_back(std::move(asset));
            return true;
        };

    auto result =
        id::internal::run_browser_import_cli_with_operations(
            request, "<html>", operations);
    auto session = require_live_session(std::move(result));
    REQUIRE(session.has_capture());
    CHECK(session.preparation_policy().is_captured_frame());
    auto adoption = session.take_capture_adoption();
    REQUIRE(adoption);
    CHECK(adoption->render_width == 640);
    CHECK(adoption->render_height == 960);
    CHECK(adoption->design_ir.root.name == "Captured");
    CHECK_FALSE(session.take_capture_adoption());
    REQUIRE(order == std::vector<std::string>{"validate", "localize"});

    const auto transient = tree.root / "transient-capture";
    const auto staging = transient / "validation-proof";
    CHECK(observed.rendered == staging / "render/render.png");
    CHECK(observed.diff == staging / "diff/diff.png");
    CHECK(observed.rendered.parent_path() != fs::current_path());
    CHECK(fs::file_size(transient / "capture.json") == 2);

    const auto durable =
        request.output_file.parent_path() / "ui-browser-capture";
    const auto published_render =
        request.output_file.parent_path() / "ui-claude-design-render.png";
    const auto published_diff = fs::path(request.diff_output);
    const auto published_asset =
        request.output_file.parent_path() / "assets/reference.png";
    CHECK_FALSE(fs::exists(durable));
    CHECK_FALSE(fs::exists(published_render));
    CHECK_FALSE(fs::exists(published_diff));
    CHECK_FALSE(fs::exists(published_asset));
    std::ostringstream diagnostics;
    tree.write(request.output_file, "old-primary");
    const auto staged_primary =
        session.stage_primary_output(request.output_file, diagnostics);
    REQUIRE(staged_primary);
    tree.write(*staged_primary, "new-primary");
    CHECK(tree.read(request.output_file) == "old-primary");
    REQUIRE(session.publish(diagnostics));
    CHECK(diagnostics.str().empty());
    CHECK(fs::exists(
        durable / "validation-proof/render/render.png"));
    CHECK(fs::exists(durable / "validation-proof/diff/diff.png"));
    CHECK(fs::exists(published_render));
    CHECK(fs::exists(published_diff));
    CHECK(tree.read(published_asset) == "localized-pixels");
    CHECK(tree.read(request.output_file) == "new-primary");

    fs::remove_all(transient);
    CHECK(session.publish(diagnostics));

    // A fresh import to the same output must accept its own identical
    // localized asset and replace the owned capture generation.
    order.clear();
    auto rerun =
        id::internal::run_browser_import_cli_with_operations(
            request, "<html>", operations);
    auto rerun_session = require_live_session(std::move(rerun));
    REQUIRE(rerun_session.take_capture_adoption());
    const auto rerun_primary =
        rerun_session.stage_primary_output(
            request.output_file, diagnostics);
    REQUIRE(rerun_primary);
    tree.write(*rerun_primary, "rerun-primary");
    REQUIRE(rerun_session.publish(diagnostics));
    CHECK(tree.read(request.output_file) == "rerun-primary");
    CHECK(tree.read(published_asset) == "localized-pixels");
}

TEST_CASE("browser CLI adapter rejects unowned evidence before localization",
          "[import-design][browser-capture][cli-adapter][transaction]") {
    TempTree tree;
    auto request = request_for(tree);
    const auto durable =
        request.output_file.parent_path() / "ui-browser-capture";
    tree.write(durable / "someones-file.txt", "unowned");

    id::internal::BrowserImportCliOperations operations;
    operations.import_html =
        [&](const id::BrowserHtmlImportRequest& capture_request,
            std::string_view) {
            return captured_import(capture_request, tree);
        };
    operations.validate_capture =
        [&](const pulp::view::DesignIR&,
            const id::BrowserCaptureValidationOptions& options) {
            tree.write(options.rendered, "render");
            tree.write(options.diff, "diff");
            id::BrowserCaptureValidationResult result;
            result.valid = true;
            result.passes = true;
            return result;
        };
    operations.localize_assets =
        [](pulp::view::DesignIR&, const std::string&, std::string*) {
            FAIL("unowned evidence must fail before asset localization");
            return false;
        };

    const auto result =
        id::internal::run_browser_import_cli_with_operations(
            request, "<html>", operations);
    const auto* failure = std::get_if<id::BrowserImportFailure>(&result);
    REQUIRE(failure);
    CHECK(failure->exit_code == 2);
    CHECK(tree.read(durable / "someones-file.txt") == "unowned");
}

TEST_CASE("browser CLI adapter validates dry-run fail-below in transient state",
          "[import-design][browser-capture][cli-adapter][dry-run]") {
    TempTree tree;
    auto request = request_for(tree);
    request.dry_run = true;
    request.fail_below_percent = 90.0f;
    int validation_calls = 0;

    id::internal::BrowserImportCliOperations operations;
    operations.import_html =
        [&](const id::BrowserHtmlImportRequest& capture_request,
            std::string_view) {
            return captured_import(capture_request, tree);
        };
    operations.validate_capture =
        [&](const pulp::view::DesignIR&,
            const id::BrowserCaptureValidationOptions& options) {
            ++validation_calls;
            CHECK(options.rendered.parent_path() ==
                  tree.root /
                      "transient-capture/validation-proof/render");
            CHECK(options.fail_below_percent == 90.0f);
            id::BrowserCaptureValidationResult result;
            result.valid = true;
            result.passes = false;
            result.similarity = 0.5f;
            result.diff_pixels = 1;
            result.total_pixels = 1;
            return result;
        };
    operations.localize_assets =
        [](pulp::view::DesignIR&, const std::string&, std::string*) {
            FAIL("dry-run must not localize output assets");
            return false;
        };

    auto result =
        id::internal::run_browser_import_cli_with_operations(
            request, "<html>", operations);
    auto* captured = std::get_if<id::BrowserCapturedImport>(&result);
    REQUIRE(captured);
    CHECK(validation_calls == 1);
    CHECK(captured->similarity_failed());
    std::string error;
    CHECK(captured->commit_evidence(error));
}

TEST_CASE("browser CLI adapter separates same-basename proof staging",
          "[import-design][browser-capture][cli-adapter][transaction]") {
    TempTree tree;
    auto request = request_for(tree);
    const auto shared_name = "ui-claude-design-render.png";
    request.diff_output = (tree.root / "elsewhere" / shared_name).string();

    id::internal::BrowserImportCliOperations operations;
    operations.import_html =
        [&](const id::BrowserHtmlImportRequest& capture_request,
            std::string_view) {
            return captured_import(capture_request, tree);
        };
    operations.validate_capture =
        [&](const pulp::view::DesignIR&,
            const id::BrowserCaptureValidationOptions& options) {
            CHECK(options.rendered.filename() == "render.png");
            CHECK(options.diff.filename() == "diff.png");
            CHECK(options.rendered != options.diff);
            tree.write(options.rendered, "render-bytes");
            tree.write(options.diff, "diff-bytes");
            id::BrowserCaptureValidationResult result;
            result.valid = true;
            result.passes = true;
            return result;
        };
    operations.localize_assets =
        [](pulp::view::DesignIR&, const std::string&, std::string*) {
            return true;
        };

    auto result =
        id::internal::run_browser_import_cli_with_operations(
            request, "<html>", operations);
    auto* captured = std::get_if<id::BrowserCapturedImport>(&result);
    REQUIRE(captured);
    std::string error;
    REQUIRE(captured->commit_evidence(error));
    CHECK(tree.read(request.output_file.parent_path() / shared_name) ==
          "render-bytes");
    CHECK(tree.read(request.diff_output) == "diff-bytes");
}

TEST_CASE("browser CLI adapter rejects proof publication collisions",
          "[import-design][browser-capture][cli-adapter][transaction]") {
    TempTree tree;
    auto request = request_for(tree);
    int validation_calls = 0;

    id::internal::BrowserImportCliOperations operations;
    operations.import_html =
        [&](const id::BrowserHtmlImportRequest& capture_request,
            std::string_view) {
            return captured_import(capture_request, tree);
        };
    operations.validate_capture =
        [&](const pulp::view::DesignIR&,
            const id::BrowserCaptureValidationOptions&) {
            ++validation_calls;
            return id::BrowserCaptureValidationResult{};
        };
    operations.localize_assets =
        [](pulp::view::DesignIR&, const std::string&, std::string*) {
            FAIL("colliding destinations must fail before localization");
            return false;
        };

    const auto render_destination =
        request.output_file.parent_path() / "ui-claude-design-render.png";
    const auto durable =
        request.output_file.parent_path() / "ui-browser-capture";

    SECTION("explicit diff cannot overwrite primary output") {
        request.diff_output = request.output_file.string();
    }
    SECTION("explicit diff cannot overwrite a reserved C++ header") {
        const auto header =
            request.output_file.parent_path() / "imported_ui.hpp";
        request.reserved_output_paths.push_back(header);
        request.diff_output = header.string();
    }
    SECTION("explicit diff cannot overwrite source input") {
        request.diff_output = request.input_file.string();
    }
    SECTION("explicit diff cannot overwrite interaction plan") {
        request.browser_interactions =
            tree.root / "interaction-plan.json";
        request.diff_output =
            request.browser_interactions->string();
    }
    SECTION("explicit diff cannot overwrite explicit reference") {
        request.reference_image =
            (tree.root / "reference.png").string();
        request.diff_output = request.reference_image;
    }
    SECTION("explicit diff cannot alias the render target") {
        request.diff_output = render_destination.string();
    }
    SECTION("explicit diff cannot land inside durable evidence") {
        request.diff_output = (durable / "proof.png").string();
    }
    SECTION("durable evidence cannot contain a reserved sidecar") {
        request.reserved_output_paths.push_back(durable / "tokens.json");
    }
    SECTION("canonical-equivalent aliases are collisions") {
        fs::create_directories(request.output_file.parent_path());
        const auto alias = tree.root / "output-alias";
        std::error_code ec;
        fs::create_directory_symlink(
            request.output_file.parent_path(), alias, ec);
        REQUIRE_FALSE(ec);
        request.diff_output =
            (alias / render_destination.filename()).string();
    }
#if defined(__APPLE__) || defined(_WIN32)
    SECTION("nonexistent mixed-case output aliases are collisions") {
        request.diff_output =
            request.output_file.parent_path() / "UI.JS";
    }
    SECTION("nonexistent mixed-case durable aliases are contained") {
        request.diff_output =
            request.output_file.parent_path() /
            "UI-BROWSER-CAPTURE/proof.png";
    }
    SECTION("nonexistent mixed-case interaction aliases are collisions") {
        request.browser_interactions =
            request.output_file.parent_path() / "Interaction-Plan.JSON";
        request.diff_output =
            request.output_file.parent_path() / "interaction-plan.json";
    }
#endif

    const auto result =
        id::internal::run_browser_import_cli_with_operations(
            request, "<html>", operations);
    const auto* failure = std::get_if<id::BrowserImportFailure>(&result);
    REQUIRE(failure);
    CHECK(failure->exit_code == 2);
    CHECK(validation_calls == 0);
    CHECK_FALSE(fs::exists(request.output_file));
}

TEST_CASE("browser CLI adapter protects localized assets from publication",
          "[import-design][browser-capture][cli-adapter][transaction]") {
    TempTree tree;
    auto request = request_for(tree);
    request.diff_output =
        (request.output_file.parent_path() / "assets/capture.png").string();

    id::internal::BrowserImportCliOperations operations;
    operations.import_html =
        [&](const id::BrowserHtmlImportRequest& capture_request,
            std::string_view) {
            return captured_import(capture_request, tree);
        };
    operations.validate_capture =
        [&](const pulp::view::DesignIR&,
            const id::BrowserCaptureValidationOptions& options) {
            tree.write(options.rendered, "render");
            tree.write(options.diff, "diff");
            id::BrowserCaptureValidationResult result;
            result.valid = true;
            result.passes = true;
            return result;
        };
    operations.localize_assets =
        [](pulp::view::DesignIR& ir, const std::string&, std::string*) {
            pulp::view::IRAssetRef asset;
            asset.asset_id = "capture";
            asset.local_path = "assets/capture.png";
            ir.asset_manifest.assets.push_back(std::move(asset));
            return true;
        };

    const auto result =
        id::internal::run_browser_import_cli_with_operations(
            request, "<html>", operations);
    const auto* failure = std::get_if<id::BrowserImportFailure>(&result);
    REQUIRE(failure);
    CHECK(failure->exit_code == 2);
    CHECK_FALSE(fs::exists(request.diff_output));
}

TEST_CASE("evidence commit rechecks filesystem aliases",
          "[import-design][browser-capture][cli-adapter][transaction]") {
    TempTree tree;
    auto request = request_for(tree);
    request.diff_output = tree.root / "proof/diff.png";

    id::internal::BrowserImportCliOperations operations;
    operations.import_html =
        [&](const id::BrowserHtmlImportRequest& capture_request,
            std::string_view) {
            return captured_import(capture_request, tree);
        };
    operations.validate_capture =
        [&](const pulp::view::DesignIR&,
            const id::BrowserCaptureValidationOptions& options) {
            tree.write(options.rendered, "render");
            tree.write(options.diff, "diff");
            id::BrowserCaptureValidationResult result;
            result.valid = true;
            result.passes = true;
            return result;
        };
    operations.localize_assets =
        [](pulp::view::DesignIR&, const std::string&, std::string*) {
            return true;
        };

    auto result =
        id::internal::run_browser_import_cli_with_operations(
            request, "<html>", operations);
    auto session = require_live_session(std::move(result));
    REQUIRE(session.has_capture());
    tree.write(request.output_file, "primary-output");
    fs::create_directories(fs::path(request.diff_output).parent_path());
    fs::create_hard_link(request.output_file, request.diff_output);

    std::ostringstream diagnostics;
    CHECK_FALSE(session.publish(diagnostics));
    CHECK(diagnostics.str().find("Error: ") == 0);
    CHECK(diagnostics.str().find("aliases a protected path") !=
          std::string::npos);
    CHECK(tree.read(request.output_file) == "primary-output");
}

TEST_CASE("evidence commit rechecks aliases against the interaction plan",
          "[import-design][browser-capture][cli-adapter][transaction][security]") {
    TempTree tree;
    auto request = request_for(tree);
    request.browser_interactions =
        tree.root / "interaction-plan.json";
    request.diff_output = tree.root / "proof/diff.png";
    tree.write(*request.browser_interactions, "interaction-plan");

    id::internal::BrowserImportCliOperations operations;
    operations.import_html =
        [&](const id::BrowserHtmlImportRequest& capture_request,
            std::string_view) {
            return captured_import(capture_request, tree);
        };
    operations.validate_capture =
        [&](const pulp::view::DesignIR&,
            const id::BrowserCaptureValidationOptions& options) {
            tree.write(options.rendered, "render");
            tree.write(options.diff, "diff");
            id::BrowserCaptureValidationResult result;
            result.valid = true;
            result.passes = true;
            return result;
        };
    operations.localize_assets =
        [](pulp::view::DesignIR&, const std::string&, std::string*) {
            return true;
        };

    auto result =
        id::internal::run_browser_import_cli_with_operations(
            request, "<html>", operations);
    auto session = require_live_session(std::move(result));
    REQUIRE(session.has_capture());
    fs::create_directories(fs::path(request.diff_output).parent_path());
    fs::create_hard_link(
        *request.browser_interactions, request.diff_output);

    std::ostringstream diagnostics;
    CHECK_FALSE(session.publish(diagnostics));
    CHECK(diagnostics.str().find("aliases a protected path") !=
          std::string::npos);
    CHECK(tree.read(*request.browser_interactions) == "interaction-plan");
}

TEST_CASE("browser primary output cannot replace the interaction plan",
          "[import-design][browser-capture][cli-adapter][transaction][security]") {
    TempTree tree;
    auto request = request_for(tree);
    request.browser_interactions =
        tree.root / "interaction-plan.json";
    request.output_file = *request.browser_interactions;
    tree.write(*request.browser_interactions, "interaction-plan");
    bool capture_called = false;

    id::internal::BrowserImportCliOperations operations;
    operations.import_html =
        [&](const id::BrowserHtmlImportRequest&, std::string_view) {
            capture_called = true;
            return id::BrowserHtmlImportResult{};
        };
    operations.validate_capture =
        [](const pulp::view::DesignIR&,
           const id::BrowserCaptureValidationOptions&) {
            FAIL("colliding primary output must not validate");
            return id::BrowserCaptureValidationResult{};
        };
    operations.localize_assets =
        [](pulp::view::DesignIR&, const std::string&, std::string*) {
            FAIL("colliding primary output must not localize");
            return false;
        };

    const auto result =
        id::internal::run_browser_import_cli_with_operations(
            request, "<html>", operations);
    const auto* failure = std::get_if<id::BrowserImportFailure>(&result);
    REQUIRE(failure);
    CHECK(failure->exit_code == 2);
    CHECK_FALSE(capture_called);
    CHECK(tree.read(*request.browser_interactions) == "interaction-plan");
}

TEST_CASE("late evidence failure restores primary, tokens, and required assets",
          "[import-design][browser-capture][cli-adapter][transaction]") {
    TempTree tree;
    auto request = request_for(tree);
    const auto w3c_tokens =
        request.output_file.parent_path() / "design-tokens.json";
    request.reserved_output_paths.push_back(w3c_tokens);
    tree.write(request.output_file, "old-primary");
    tree.write(w3c_tokens, "old-tokens");

    id::internal::BrowserImportCliOperations operations;
    operations.import_html =
        [&](const id::BrowserHtmlImportRequest& capture_request,
            std::string_view) {
            return captured_import(capture_request, tree);
        };
    operations.validate_capture =
        [&](const pulp::view::DesignIR&,
            const id::BrowserCaptureValidationOptions& options) {
            tree.write(options.rendered, "render");
            tree.write(options.diff, "diff");
            id::BrowserCaptureValidationResult result;
            result.valid = true;
            result.passes = true;
            return result;
        };
    operations.localize_assets =
        [&](pulp::view::DesignIR& ir, const std::string& output,
            std::string*) {
            const auto asset =
                fs::path(output).parent_path() / "assets/reference.png";
            tree.write(asset, "new-asset");
            pulp::view::IRAssetRef reference;
            reference.asset_id = "reference";
            reference.local_path = "assets/reference.png";
            ir.asset_manifest.assets.push_back(std::move(reference));
            return true;
        };

    auto result =
        id::internal::run_browser_import_cli_with_operations(
            request, "<html>", operations);
    auto session = require_live_session(std::move(result));
    REQUIRE(session.has_capture());
    std::ostringstream diagnostics;
    const auto staged_tokens =
        session.stage_primary_output(w3c_tokens, diagnostics);
    const auto staged_primary =
        session.stage_primary_output(request.output_file, diagnostics);
    REQUIRE(staged_tokens);
    REQUIRE(staged_primary);
    CHECK(*staged_tokens != w3c_tokens);
    CHECK(*staged_primary != request.output_file);
    tree.write(*staged_tokens, "new-tokens");
    tree.write(*staged_primary, "new-primary");
    CHECK(tree.read(w3c_tokens) == "old-tokens");
    CHECK(tree.read(request.output_file) == "old-primary");

    const auto durable =
        request.output_file.parent_path() / "ui-browser-capture";
    tree.write(durable / "unowned.txt", "race");
    REQUIRE_FALSE(session.publish(diagnostics));
    CHECK(diagnostics.str().find("unowned capture directory") !=
          std::string::npos);
    CHECK(tree.read(request.output_file) == "old-primary");
    CHECK(tree.read(w3c_tokens) == "old-tokens");
    CHECK_FALSE(fs::exists(
        request.output_file.parent_path() / "assets/reference.png"));
    CHECK(tree.read(durable / "unowned.txt") == "race");

    std::error_code cleanup_error;
    fs::remove_all(durable, cleanup_error);
    REQUIRE_FALSE(cleanup_error);
    diagnostics.str({});
    diagnostics.clear();

    REQUIRE(session.publish(diagnostics));
    CHECK(tree.read(request.output_file) == "new-primary");
    CHECK(tree.read(w3c_tokens) == "new-tokens");
    CHECK(tree.read(
        request.output_file.parent_path() / "assets/reference.png")
          == "new-asset");
}

TEST_CASE("browser output transaction rejects non-file primary destinations",
          "[import-design][browser-capture][transaction]") {
    TempTree tree;
    auto request = request_for(tree);
    fs::create_directories(request.output_file);
    id::internal::BrowserImportCliOperations operations;
    operations.import_html =
        [&](const id::BrowserHtmlImportRequest& capture_request,
            std::string_view) {
            return captured_import(capture_request, tree);
        };
    operations.validate_capture =
        [&](const pulp::view::DesignIR&,
            const id::BrowserCaptureValidationOptions& options) {
            tree.write(options.rendered, "render");
            tree.write(options.diff, "diff");
            id::BrowserCaptureValidationResult result;
            result.valid = true;
            result.passes = true;
            return result;
        };
    operations.localize_assets =
        [](pulp::view::DesignIR&, const std::string&, std::string*) {
            return true;
        };
    auto result =
        id::internal::run_browser_import_cli_with_operations(
            request, "<html>", operations);
    auto session = require_live_session(std::move(result));
    REQUIRE(session.has_capture());
    std::ostringstream diagnostics;
    CHECK_FALSE(session.stage_primary_output(
        request.output_file, diagnostics));
    CHECK(diagnostics.str().find("Error: ") == 0);
    CHECK(diagnostics.str().find("regular file or absent") !=
          std::string::npos);
}

TEST_CASE("browser capture validator creates nested proof directories",
          "[import-design][browser-capture][validation]") {
    TempTree tree;
    pulp::view::DesignIR ir;
    ir.root.type = "frame";
    ir.root.style.width = 32.0f;
    ir.root.style.height = 32.0f;
    auto root =
        pulp::view::build_native_view_tree(ir, ir.asset_manifest);
    REQUIRE(root);
    const auto reference = pulp::view::render_to_png(
        *root, 32, 32, 2.0f, pulp::view::ScreenshotBackend::skia);
    REQUIRE_FALSE(reference.empty());
    const auto reference_path = tree.root / "reference.png";
    tree.write(reference_path, reference);

    const auto rendered =
        tree.root / "proof/render/deep/render.png";
    const auto diff = tree.root / "proof/diff/deep/diff.png";
    const auto result = id::validate_browser_capture_design_ir(
        ir,
        {.reference = reference_path,
         .rendered = rendered,
         .diff = diff,
         .width = 32,
         .height = 32});
    INFO(result.error);
    CHECK(result.valid);
    CHECK(fs::is_regular_file(rendered));
    CHECK(fs::is_regular_file(diff));
}

TEST_CASE("asset localization stamps a portable path from asset_ref",
          "[import-design][browser-capture][assets]") {
    TempTree tree;
    const auto source = tree.root / "capture/browser.png";
    const auto output = tree.root / "published/ui.js";
    tree.write(source, "browser-pixels");

    pulp::view::DesignIR ir;
    pulp::view::IRAssetRef asset;
    asset.asset_id = "reference:browser";
    asset.local_path = source.string();
    ir.asset_manifest.assets.push_back(asset);
    ir.root.attributes["asset_ref"] = asset.asset_id;

    std::string error;
    REQUIRE(id::localize_ir_assets(
        ir, output.string(), &error));
    REQUIRE(ir.asset_manifest.assets[0].local_path.has_value());
    CHECK(fs::path(*ir.asset_manifest.assets[0].local_path).is_relative());
    REQUIRE(ir.root.attributes.contains("asset_path"));
    CHECK(ir.root.attributes.at("asset_path") ==
          *ir.asset_manifest.assets[0].local_path);
    CHECK(fs::is_regular_file(
        output.parent_path() / ir.root.attributes.at("asset_path")));
}

TEST_CASE("browser CLI detection and direct inference preserve CLI disposition",
          "[import-design][browser-capture][cli-adapter][detect]") {
    TempTree tree;
    const auto html = tree.root / "Screen.HTML";
    tree.write(html, "<!doctype html><main>screen</main>");

    SECTION("browser readiness is metadata after a positive match") {
        const auto result = id::run_browser_detect_cli(
            html, tree.root / "missing-browser");
        REQUIRE(result.has_value());
        CHECK(*result == 0);
    }

    SECTION("read failure is handled instead of falling through") {
        const auto result = id::run_browser_detect_cli(
            tree.root / "missing.html");
        REQUIRE(result.has_value());
        CHECK(*result == 1);
    }

    SECTION("normalized HTML and HTM paths infer the browser source") {
        std::string source;
        CHECK(id::infer_browser_html_source_cli(html, source));
        CHECK(source == "html");

        source.clear();
        CHECK(id::infer_browser_html_source_cli(
            tree.root / "legacy.HTM", source));
        CHECK(source == "html");

        const auto extensionless = tree.root / "export";
        tree.write(extensionless, "  <!doctype html><main>screen</main>");
        source.clear();
        CHECK(id::infer_browser_html_source_cli(extensionless, source));
        CHECK(source == "html");

        const auto claude = tree.root / "component.dc.html";
        tree.write(claude,
                   R"(<script type="text/x-dc">export default {}</script>)");
        source.clear();
        CHECK(id::infer_browser_html_source_cli(claude, source));
        CHECK(source == "claude");

        source = "stitch";
        CHECK_FALSE(id::infer_browser_html_source_cli(html, source));
        CHECK(source == "stitch");
    }
}
