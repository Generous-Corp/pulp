#include <pulp/view/design_import.hpp>
#include <pulp/view/layout_snapshot.hpp>
#include <pulp/view/view.hpp>

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

using namespace pulp::view;

namespace {

std::string read_text(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    REQUIRE(in.good());
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

bool has_diagnostic(const std::vector<ImportDiagnostic>& diagnostics,
                    const std::string& code) {
    for (const auto& diagnostic : diagnostics) {
        if (diagnostic.code == code) return true;
    }
    return false;
}

DesignIR make_provider_fixture_ir() {
    DesignIR ir;
    ir.source = DesignSource::figma_plugin;
    ir.source_adapter = "test-provider-source";
    ir.source_version = "1";
    ir.capture_method = "adapter_parse";
    ir.root.type = "frame";
    ir.root.name = "Provider Root";
    ir.root.stable_anchor_id = "source-root";
    ir.root.style.width = 320.0f;
    ir.root.style.height = 140.0f;
    ir.root.layout.display = "flex";
    ir.root.layout.direction = LayoutDirection::column;

    IRNode label;
    label.type = "text";
    label.name = "Provider Label";
    label.text_content = "Provider";
    label.stable_anchor_id = "source-label";
    label.style.width = 120.0f;
    label.style.height = 24.0f;
    ir.root.children.push_back(std::move(label));

    IRNode knob;
    knob.type = "frame";
    knob.name = "Provider Knob";
    knob.stable_anchor_id = "provider:test:knob";
    knob.audio_widget = AudioWidgetType::knob;
    knob.audio_label = "Gain";
    knob.audio_default = 0.25f;
    knob.attributes["value"] = "0.75";
    knob.style.width = 64.0f;
    knob.style.height = 64.0f;
    ir.root.children.push_back(std::move(knob));

    return ir;
}

class EchoProvider final : public NativeDesignProvider {
public:
    std::string provider_id() const override { return "test.echo"; }
    std::string provider_version() const override { return "1"; }

    NativeDesignProviderOutput import_design(const NativeDesignProviderRequest& request) override {
        ++calls;
        last_request_hash = request.source_design_hash;
        NativeDesignProviderOutput output;
        output.canonical_design_ir_json = request.canonical_source_design_ir_json;
        if (request.source_ir != nullptr) output.asset_manifest = request.source_ir->asset_manifest;
        output.provenance.provider_id = provider_id();
        output.provenance.provider_version = provider_version();
        output.provenance.source_design_hash = request.source_design_hash;
        return output;
    }

    int calls = 0;
    std::string last_request_hash;
};

class InvalidProvider final : public NativeDesignProvider {
public:
    std::string provider_id() const override { return "test.invalid"; }

    NativeDesignProviderOutput import_design(const NativeDesignProviderRequest& request) override {
        NativeDesignProviderOutput output;
        output.canonical_design_ir_json = "{not-json";
        output.provenance.provider_id = provider_id();
        output.provenance.source_design_hash = request.source_design_hash;
        return output;
    }
};

class FailingProvider final : public NativeDesignProvider {
public:
    std::string provider_id() const override { return "test.fail"; }

    NativeDesignProviderOutput import_design(const NativeDesignProviderRequest& request) override {
        NativeDesignProviderOutput output;
        output.ok = false;
        output.failure_reason = "fixture failure";
        output.provenance.provider_id = provider_id();
        output.provenance.source_design_hash = request.source_design_hash;
        return output;
    }
};

std::string layout_json(View& view, float width, float height) {
    view.set_bounds({0, 0, width, height});
    view.layout_children();
    return dump_layout_tree(view,
                             {.surface = "ruif-provider",
                             .fixture = "provider-route-parity",
                             .viewport_width = width,
                             .viewport_height = height});
}

} // namespace

TEST_CASE("native design provider registry is explicit and deterministic",
          "[view][import][provider][ruif-3]") {
    NativeDesignProviderRegistry registry;
    auto provider = std::make_shared<EchoProvider>();

    REQUIRE(registry.find_provider("test.echo") == nullptr);
    REQUIRE(registry.register_provider(provider));
    REQUIRE_FALSE(registry.register_provider(provider));
    REQUIRE(registry.find_provider("test.echo") == provider.get());

    const auto ids = registry.provider_ids();
    REQUIRE(ids.size() == 1);
    REQUIRE(ids[0] == "test.echo");
}

TEST_CASE("native design provider validation requires canonical DesignIR JSON",
          "[view][import][provider][ruif-3]") {
    NativeDesignProviderOutput output;
    output.canonical_design_ir_json = R"JSON({"root":{"type":"frame","name":"bare"}})JSON";
    output.provenance.provider_id = "test.provider";
    output.provenance.source_design_hash = "hash";

    const auto result = validate_native_design_provider_output(
        output,
        {.expected_provider_id = "test.provider", .source_design_hash = "hash"});

    REQUIRE_FALSE(result.ok);
    REQUIRE(has_diagnostic(result.diagnostics, "provider-noncanonical-json"));
}

TEST_CASE("native design provider validation bounds diagnostics and rejects duplicate source anchors by default",
          "[view][import][provider][ruif-3]") {
    auto ir = make_provider_fixture_ir();
    ir.root.children.front().stable_anchor_id = "source-root";

    NativeDesignProviderOutput output;
    output.canonical_design_ir_json = serialize_design_ir(ir);
    output.provenance.provider_id = "test.provider";
    output.provenance.source_design_hash = "hash";
    IRAssetRef escaped_asset;
    escaped_asset.asset_id = "escaped";
    escaped_asset.local_path = "C:\\temp\\escaped.png";
    escaped_asset.content_hash = "sha256:escaped";
    output.asset_manifest.assets.push_back(std::move(escaped_asset));
    for (int i = 0; i < 130; ++i) {
        ImportDiagnostic diagnostic;
        diagnostic.code = "provider-note";
        diagnostic.path = "$";
        diagnostic.message = std::string(5000, 'x');
        output.diagnostics.push_back(std::move(diagnostic));
    }

    const auto result = validate_native_design_provider_output(
        output,
        {.expected_provider_id = "test.provider", .source_design_hash = "hash"});

    REQUIRE_FALSE(result.ok);
    REQUIRE(has_diagnostic(result.diagnostics, "provider-duplicate-source-anchor"));
    REQUIRE(has_diagnostic(result.diagnostics, "provider-asset-path-escape"));
    REQUIRE(has_diagnostic(result.diagnostics, "provider-diagnostic-too-large"));
    REQUIRE(has_diagnostic(result.diagnostics, "provider-diagnostic-count-exceeded"));

    const auto compatibility_result = validate_native_design_provider_output(
        output,
        {.expected_provider_id = "test.provider",
         .source_design_hash = "hash",
         .reject_duplicate_source_anchors = false});
    REQUIRE_FALSE(compatibility_result.ok);
    REQUIRE(has_diagnostic(compatibility_result.diagnostics, "provider-duplicate-source-anchor"));
}

TEST_CASE("native design provider strict and fallback modes gate materialization",
          "[view][import][provider][ruif-3]") {
    const auto ir = make_provider_fixture_ir();
    NativeDesignProviderRegistry registry;
    auto echo = std::make_shared<EchoProvider>();
    REQUIRE(registry.register_provider(echo));
    REQUIRE(registry.register_provider(std::make_shared<InvalidProvider>()));
    REQUIRE(registry.register_provider(std::make_shared<FailingProvider>()));

    std::vector<ImportDiagnostic> default_diagnostics;
    auto baseline = build_native_view_tree(ir, ir.asset_manifest, {.diagnostics_out = &default_diagnostics});
    REQUIRE(baseline != nullptr);
    REQUIRE(echo->calls == 0);

    std::vector<ImportDiagnostic> provider_diagnostics;
    NativeDesignProviderAttempt provider_attempt;
    auto provider_view = build_native_view_tree_with_provider(
        ir,
        ir.asset_manifest,
        {.diagnostics_out = &provider_diagnostics},
        {.mode = NativeDesignProviderMode::provider_strict,
         .provider_id = "test.echo",
         .registry = &registry,
         .source_design_hash = "fixture-hash",
         .attempt_out = &provider_attempt});
    REQUIRE(provider_view != nullptr);
    REQUIRE(echo->calls == 1);
    REQUIRE(echo->last_request_hash == "fixture-hash");
    REQUIRE(provider_attempt.provider_attempted);
    REQUIRE(provider_attempt.strict_mode);
    REQUIRE_FALSE(provider_attempt.fallback_used);
    REQUIRE(provider_attempt.provider_id == "test.echo");
    REQUIRE(provider_attempt.provider_version == "1");
    REQUIRE(provider_attempt.source_design_hash == "fixture-hash");
    REQUIRE_FALSE(has_diagnostic(provider_diagnostics, "provider-not-found"));

    const auto baseline_json = layout_json(*baseline, 320.0f, 140.0f);
    const auto provider_json = layout_json(*provider_view, 320.0f, 140.0f);
    LayoutTreeDiff diff;
    REQUIRE(layout_tree_snapshots_equivalent(baseline_json, provider_json, {}, &diff));

    std::vector<ImportDiagnostic> strict_diagnostics;
    NativeDesignProviderAttempt strict_attempt;
    auto strict_bad = build_native_view_tree_with_provider(
        ir,
        ir.asset_manifest,
        {.diagnostics_out = &strict_diagnostics},
        {.mode = NativeDesignProviderMode::provider_strict,
         .provider_id = "test.invalid",
         .registry = &registry,
         .source_design_hash = "fixture-hash",
         .attempt_out = &strict_attempt});
    REQUIRE(strict_bad == nullptr);
    REQUIRE(strict_attempt.provider_attempted);
    REQUIRE(strict_attempt.strict_mode);
    REQUIRE_FALSE(strict_attempt.fallback_used);
    REQUIRE(has_diagnostic(strict_diagnostics, "provider-json-parse-failed"));
    REQUIRE(has_diagnostic(strict_attempt.diagnostics, "provider-json-parse-failed"));

    std::vector<ImportDiagnostic> fallback_diagnostics;
    NativeDesignProviderAttempt fallback_attempt;
    auto fallback = build_native_view_tree_with_provider(
        ir,
        ir.asset_manifest,
        {.diagnostics_out = &fallback_diagnostics},
        {.mode = NativeDesignProviderMode::provider_with_fallback,
         .provider_id = "test.invalid",
         .registry = &registry,
         .source_design_hash = "fixture-hash",
         .attempt_out = &fallback_attempt});
    REQUIRE(fallback != nullptr);
    REQUIRE(fallback_attempt.provider_attempted);
    REQUIRE_FALSE(fallback_attempt.strict_mode);
    REQUIRE(fallback_attempt.fallback_used);
    REQUIRE(has_diagnostic(fallback_diagnostics, "provider-json-parse-failed"));
    REQUIRE(has_diagnostic(fallback_attempt.diagnostics, "provider-json-parse-failed"));
    const auto fallback_json = layout_json(*fallback, 320.0f, 140.0f);
    REQUIRE(layout_tree_snapshots_equivalent(baseline_json, fallback_json, {}, &diff));

    std::vector<ImportDiagnostic> missing_diagnostics;
    NativeDesignProviderAttempt missing_attempt;
    auto missing = build_native_view_tree_with_provider(
        ir,
        ir.asset_manifest,
        {.diagnostics_out = &missing_diagnostics},
        {.mode = NativeDesignProviderMode::provider_strict,
         .provider_id = "test.missing",
         .registry = &registry,
         .source_design_hash = "fixture-hash",
         .attempt_out = &missing_attempt});
    REQUIRE(missing == nullptr);
    REQUIRE_FALSE(missing_attempt.provider_attempted);
    REQUIRE(missing_attempt.strict_mode);
    REQUIRE_FALSE(missing_attempt.fallback_used);
    REQUIRE(has_diagnostic(missing_diagnostics, "provider-not-found"));

    std::vector<ImportDiagnostic> failure_diagnostics;
    NativeDesignProviderAttempt failure_attempt;
    auto failed_provider = build_native_view_tree_with_provider(
        ir,
        ir.asset_manifest,
        {.diagnostics_out = &failure_diagnostics},
        {.mode = NativeDesignProviderMode::provider_with_fallback,
         .provider_id = "test.fail",
         .registry = &registry,
         .source_design_hash = "fixture-hash",
         .attempt_out = &failure_attempt});
    REQUIRE(failed_provider != nullptr);
    REQUIRE(failure_attempt.provider_attempted);
    REQUIRE(failure_attempt.fallback_used);
    REQUIRE(has_diagnostic(failure_diagnostics, "provider-output-failed"));
    REQUIRE(has_diagnostic(failure_attempt.diagnostics, "provider-output-failed"));
}

TEST_CASE("RUIF-1 ELYSIUM provider route preserves canonical DesignIR before materialization",
          "[view][import][provider][ruif-3][elysium]") {
    const auto fixture_path = std::filesystem::path(PULP_REPO_ROOT)
        / "planning/artifacts/rust-ui/ruif-1/cpp-baseline/ir/elysium.designir.json";
    const auto source_json = read_text(fixture_path);
    auto ir = parse_design_ir_json(source_json);
    const auto canonical = serialize_design_ir(ir);

    NativeDesignProviderOutput output;
    output.canonical_design_ir_json = canonical;
    output.asset_manifest = ir.asset_manifest;
    output.provenance.provider_id = "test.elysium";
    output.provenance.provider_version = "1";
    output.provenance.source_design_hash = "elysium-fixture";

    const auto validation = validate_native_design_provider_output(
        output,
        {.expected_provider_id = "test.elysium",
         .source_design_hash = "elysium-fixture",
         .reject_duplicate_source_anchors = false});

    REQUIRE(validation.ok);
    REQUIRE(validation.canonical_design_ir_json == canonical);
    REQUIRE(has_diagnostic(validation.diagnostics, "provider-duplicate-source-anchor"));

    NativeDesignProviderRegistry registry;
    class ElysiumProvider final : public NativeDesignProvider {
    public:
        ElysiumProvider(std::string json, IRAssetManifest manifest)
            : json_(std::move(json)), manifest_(std::move(manifest)) {}

        std::string provider_id() const override { return "test.elysium"; }
        std::string provider_version() const override { return "1"; }

        NativeDesignProviderOutput import_design(const NativeDesignProviderRequest& request) override {
            NativeDesignProviderOutput out;
            out.canonical_design_ir_json = json_;
            out.asset_manifest = manifest_;
            out.provenance.provider_id = provider_id();
            out.provenance.provider_version = provider_version();
            out.provenance.source_design_hash = request.source_design_hash;
            return out;
        }

    private:
        std::string json_;
        IRAssetManifest manifest_;
    };

    REQUIRE(registry.register_provider(std::make_shared<ElysiumProvider>(
        canonical,
        ir.asset_manifest)));

    std::vector<ImportDiagnostic> diagnostics;
    NativeDesignProviderAttempt attempt;
    auto provider_view = build_native_view_tree_with_provider(
        ir,
        ir.asset_manifest,
        {.diagnostics_out = &diagnostics},
        {.mode = NativeDesignProviderMode::provider_strict,
         .provider_id = "test.elysium",
         .registry = &registry,
         .source_design_hash = "elysium-fixture",
         .attempt_out = &attempt,
         .reject_duplicate_source_anchors = false});

    REQUIRE(provider_view != nullptr);
    REQUIRE(attempt.provider_attempted);
    REQUIRE_FALSE(attempt.fallback_used);
    REQUIRE(attempt.provider_version == "1");
    REQUIRE(has_diagnostic(diagnostics, "provider-duplicate-source-anchor"));
    REQUIRE(has_diagnostic(attempt.diagnostics, "provider-duplicate-source-anchor"));
}
