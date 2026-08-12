#include "browser_html_import.hpp"

#include "browser_capture_backend.hpp"
#include "browser_capture_ir.hpp"
#include "browser_capture_workspace.hpp"
#include "browser_knob_sprites.hpp"
#include "claude_html_dependencies.hpp"
#include "html_intake.hpp"
#include "html_project_stager.hpp"

#include <string>
#include <system_error>

namespace pulp::import_design {

namespace fs = std::filesystem;

BrowserImportReadiness probe_browser_import_readiness(
    const std::optional<fs::path>& browser_executable) {
    browser_capture::BrowserDiscoveryOptions options;
    options.explicit_path = browser_executable;
    const auto discovered = browser_capture::discover_browser(options);
    BrowserImportReadiness result;
    if (!discovered.ok()) {
        result.error =
            browser_capture::browser_unavailable_human(discovered);
        return result;
    }
    result.available = true;
    result.executable = discovered.selected->executable;
    result.product = discovered.selected->product;
    result.version = discovered.selected->version;
    return result;
}

BrowserHtmlImportResult import_browser_html(
    const BrowserHtmlImportRequest& request,
    std::string_view content) {
    // An explicit non-HTML source is authoritative. Payloads such as
    // serialized DesignIR or JSX can legitimately contain "<script" strings;
    // re-sniffing those here would steal them from their named parser.
    if (request.source != pulp::view::DesignSource::claude &&
        request.source != pulp::view::DesignSource::html &&
        request.source != pulp::view::DesignSource::stitch) {
        return BrowserHtmlNotApplicable{};
    }
    const auto intake = classify_html_intake(request.input_file, content);
    const auto shape = html_export_shape_name(intake.shape);
    if (!intake.use_browser) return BrowserHtmlNotApplicable{};
    if (request.offline) {
        return BrowserHtmlLegacyFallback{shape};
    }
    if (!request.supports_faithful_capture) {
        return BrowserHtmlFailure{
            2,
            "this output format cannot represent browser faithful_capture "
            "backing; use --emit js, --emit ir-json, or --emit cpp",
            shape,
            {}};
    }
    if (!request.skia_validation) {
        return BrowserHtmlFailure{
            2,
            "browser-backed HTML A/B requires --screenshot-backend skia; "
            "CoreGraphics renders file-backed images as placeholders",
            shape,
            {}};
    }

    std::vector<std::shared_ptr<BrowserCaptureWorkspace>> workspaces;
    std::string workspace_error;
    auto workspace = BrowserCaptureWorkspace::create(
        request.dry_run ? "pulp-browser-dry-run" : "pulp-browser-import",
        workspace_error);
    if (!workspace) {
        return BrowserHtmlFailure{
            2, std::move(workspace_error), shape, {}};
    }
    const auto capture_directory = workspace->root() / "capture";
    workspaces.push_back(std::move(workspace));
    fs::path durable_capture_directory;
    if (!request.dry_run) {
        fs::path artifact_root = request.output_file.parent_path();
        if (artifact_root.empty()) artifact_root = fs::current_path();
        durable_capture_directory =
            artifact_root /
            (request.output_file.stem().string() + "-browser-capture");
    }

    HtmlProjectStageOptions stage_options;
    if (intake.shape == HtmlExportShape::claude_project_bundle ||
        intake.shape == HtmlExportShape::claude_design_component ||
        intake.shape == HtmlExportShape::claude_standalone_bundle) {
        stage_options.discover_additional_roots =
            claude_html_dependency_roots;
    }
    auto staged =
        stage_html_project(request.input_file, content, stage_options);
    if (!staged) {
        return BrowserHtmlFailure{
            2,
            "could not stage the HTML project: " + staged.error,
            shape,
            std::move(workspaces)};
    }
    workspaces.push_back(staged.workspace);

    browser_capture::BrowserDiscoveryOptions discovery;
    discovery.explicit_path = request.browser_executable;

    browser_capture::CaptureRequest capture;
    capture.input_file = staged.entry;
    capture.staged_root = staged.root;
    capture.output_directory = capture_directory;
    capture.initial_width = request.initial_width;
    capture.initial_height = request.initial_height;
    capture.interaction_plan = request.browser_interactions;
    capture.allow_network = request.allow_browser_network;

    std::error_code ec;
    const auto executable =
        fs::weakly_canonical(request.importer_executable, ec);
    for (const auto directory : {
             browser_capture::kBrowserCaptureRuntimeDirectory,
             browser_capture::kLegacyBrowserCaptureRuntimeDirectory}) {
        const auto sibling =
            executable.parent_path() / directory / "capture.mjs";
        if (!ec && fs::is_regular_file(sibling)) {
            discovery.capture_script = sibling;
            capture.capture_script = sibling;
            break;
        }
    }

    auto captured =
        browser_capture::discover_and_capture(discovery, capture);
    if (!captured.discovery.ok()) {
        return BrowserHtmlFailure{
            2,
            browser_capture::browser_unavailable_human(captured.discovery),
            shape,
            std::move(workspaces)};
    }
    if (!captured.capture.ok()) {
        std::string error =
            "browser HTML capture failed [" +
            captured.capture.diagnostic.code + "]: " +
            captured.capture.diagnostic.message;
        return BrowserHtmlFailure{
            2, std::move(error), shape, std::move(workspaces)};
    }

    auto lowered = lower_browser_capture_to_ir(
        captured.capture.artifacts->envelope,
        {.source = request.source,
         // The staged capture envelope carries a safe entry basename. Do not
         // serialize the importing machine's absolute path into portable IR.
         .source_file = {},
         .require_interaction_report =
             request.browser_interactions.has_value(),
         .native_panel_lowering = request.native_panel_lowering,
         .materialized_canvas_composition =
             request.materialized_canvas_composition});
    if (!lowered) {
        return BrowserHtmlFailure{
            3,
            "could not lower browser capture to DesignIR: " + lowered.error,
            shape,
            std::move(workspaces)};
    }
    // Give each knob/fader whose author declared an indicator the capture
    // slices needed to clean the frozen instance and move the authored art with
    // the parameter. Fails the
    // import rather than dropping the pointer silently: a declared indicator
    // that produced nothing is exactly the failure that reads as "it works" in
    // every pixel gate.
    //
    // Keyed on the error string, not on the count: zero knobs skinned is the
    // ordinary result for a panel that declared no indicators, so a caller that
    // reads the count as the verdict swallows every failure as "nothing to do".
    std::string sprite_error;
    apply_browser_capture_control_sprites(
        *lowered.design_ir, lowered.reference_png, capture_directory,
        &sprite_error, capture_directory / "browser-static.png");
    if (!sprite_error.empty()) {
        return BrowserHtmlFailure{
            3, std::move(sprite_error), shape, std::move(workspaces)};
    }
    return BrowserHtmlCaptured{
        shape,
        std::move(*lowered.design_ir),
        capture_directory,
        durable_capture_directory,
        lowered.reference_png,
        lowered.semantic_report,
        std::move(workspaces),
        std::move(lowered.warnings)};
}

}  // namespace pulp::import_design
