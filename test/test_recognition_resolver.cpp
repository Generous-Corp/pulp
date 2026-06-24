// Tests for the key-based recognition merge layer (issue #4676).
//
// Covers the four acceptance criteria:
//   1. A non-Pulp-Library design WITH a recognition manifest wires the mapped
//      controls (synthetic figma-plugin envelope + synthetic user manifest →
//      recognized kinds + binding metadata).
//   2. WITHOUT a manifest, behavior is unchanged (built-in library authoritative,
//      already-recognized kinds untouched, unknown third-party keys NOT guessed).
//   3. A present-but-unmatched component instance is surfaced (never silent).
//   4. The merge precedence: a user manifest entry overrides the built-in on a
//      key collision; the built-in table matches the published library JSON.

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include <pulp/view/design_sources.hpp>
#include <pulp/view/design_ir.hpp>
#include <pulp/view/design_frame_view.hpp>
#include <pulp/view/design_import.hpp>
#include <pulp/view/recognition_resolver.hpp>

#include <choc/text/choc_JSON.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#if defined(_WIN32)
#include <process.h>
#define PULP_TEST_GETPID _getpid
#else
#include <unistd.h>
#define PULP_TEST_GETPID ::getpid
#endif

using namespace pulp::view;
namespace fs = std::filesystem;

#ifndef PULP_REPO_ROOT
#define PULP_REPO_ROOT "."
#endif

namespace {

// A synthetic third-party figma-plugin envelope: a frame containing one INSTANCE
// node whose component identity (figma.component_key / main_component_name) is
// the designer's OWN component, NOT a Pulp-Library key — so the in-Figma TS
// plugin left it with no audio_widget. This mirrors the live Ink & Signal
// "NumberBox" case from the issue.
std::string third_party_envelope(const std::string& component_key,
                                 const std::string& main_component_name,
                                 const std::string& node_name = "Cutoff Stepper") {
    std::ostringstream ss;
    ss << R"({
        "format_version": "v1",
        "parser_version": "0.1.0",
        "provenance": { "adapter": "figma-plugin", "version": "0.1.0" },
        "root": {
            "type": "frame",
            "name": "Panel",
            "children": [
                {
                    "type": "frame",
                    "name": ")" << node_name << R"(",
                    "figma": {
                        "component_key": ")" << component_key << R"(",
                        "main_component_name": ")" << main_component_name << R"("
                    },
                    "children": []
                }
            ]
        }
    })";
    return ss.str();
}

const IRNode* first_child(const IRNode& root) {
    return root.children.empty() ? nullptr : &root.children.front();
}

} // namespace

TEST_CASE("recognition manifest wires a third-party design's own component keys",
          "[view][import][recognition][issue-4676]") {
    // Designer's own NumberBox component-set key → Pulp knob.
    const std::string user_manifest = R"({
        "widgets": {
            "number_box": {
                "kind": "knob",
                "component_set_key": "abc123designerkey",
                "name_prefix": "InkSignal / NumberBox"
            }
        }
    })";

    auto ir = parse_figma_plugin_json(
        third_party_envelope("abc123designerkey", "InkSignal / NumberBox"));

    // Before resolution: NOT recognized (the TS plugin didn't know the key).
    const auto* node = first_child(ir.root);
    REQUIRE(node != nullptr);
    REQUIRE(node->audio_widget == AudioWidgetType::none);
    // The component identity survived the parse (parse_ir_node stamps it).
    REQUIRE(node->attributes.at("figmaComponentKey") == "abc123designerkey");

    auto resolver = RecognitionResolver::with_builtin_library();
    std::string err;
    auto src = RecognitionResolver::parse_manifest_json(user_manifest, "user-manifest", &err);
    REQUIRE(err.empty());
    REQUIRE(src.has_value());
    resolver.add_source(std::move(*src));

    std::vector<UnmatchedComponent> unmatched;
    const int wired = apply_recognition_resolver(ir.root, resolver, &unmatched);

    REQUIRE(wired == 1);
    REQUIRE(unmatched.empty());
    const auto* resolved = first_child(ir.root);
    REQUIRE(resolved->audio_widget == AudioWidgetType::knob);
    // Binding/provenance metadata stamped so downstream can trace the source.
    REQUIRE(resolved->attributes.at("recognitionSource") == "user-manifest");
    REQUIRE(resolved->attributes.at("recognitionVia") == "key");
}

TEST_CASE("recognition manifest name-prefix fallback wires when the key differs",
          "[view][import][recognition][issue-4676]") {
    // A different instance key, but the component name matches the manifest's
    // name_prefix — the fallback rung resolves it.
    const std::string user_manifest = R"({
        "widgets": {
            "house_fader": {
                "kind": "fader",
                "component_set_key": "the_published_key",
                "name_prefix": "Studio / BigFader"
            }
        }
    })";

    auto ir = parse_figma_plugin_json(
        third_party_envelope("a_DIFFERENT_instance_key", "Studio / BigFader Large"));

    auto resolver = RecognitionResolver::with_builtin_library();
    std::string err;
    auto src = RecognitionResolver::parse_manifest_json(user_manifest, "user-manifest", &err);
    REQUIRE(src.has_value());
    resolver.add_source(std::move(*src));

    std::vector<UnmatchedComponent> unmatched;
    const int wired = apply_recognition_resolver(ir.root, resolver, &unmatched);

    REQUIRE(wired == 1);
    const auto* resolved = first_child(ir.root);
    REQUIRE(resolved->audio_widget == AudioWidgetType::fader);
    REQUIRE(resolved->attributes.at("recognitionVia") == "name_prefix");
}

TEST_CASE("without a manifest, a third-party component is left unrecognized and surfaced",
          "[view][import][recognition][issue-4676]") {
    // No user manifest: only the built-in library is active. A designer's own key
    // matches nothing, so it must NOT be guessed — and it must be surfaced.
    auto ir = parse_figma_plugin_json(
        third_party_envelope("unknown_designer_key", "InkSignal / NumberBox"));

    auto resolver = RecognitionResolver::with_builtin_library();
    std::vector<UnmatchedComponent> unmatched;
    const int wired = apply_recognition_resolver(ir.root, resolver, &unmatched);

    REQUIRE(wired == 0);  // never guessed (P7 never-silent-knob)
    const auto* node = first_child(ir.root);
    REQUIRE(node->audio_widget == AudioWidgetType::none);
    // Surfaced for the import report.
    REQUIRE(unmatched.size() == 1);
    REQUIRE(unmatched.front().component_key == "unknown_designer_key");
    REQUIRE(unmatched.front().name == "InkSignal / NumberBox");
}

TEST_CASE("the built-in Pulp Library key still resolves with no user manifest",
          "[view][import][recognition][issue-4676]") {
    // A genuine Pulp-Library knob key (from library-manifest.json). Even if the
    // TS plugin somehow left audio_widget unset, the C++ resolver recovers it via
    // the built-in source — so the built-in path is never regressed.
    auto ir = parse_figma_plugin_json(third_party_envelope(
        "f74264ffa9108521fb0d3398dc8f5ea88e23a84e", "Pulp / Knob", "Gain"));

    auto resolver = RecognitionResolver::with_builtin_library();
    std::vector<UnmatchedComponent> unmatched;
    const int wired = apply_recognition_resolver(ir.root, resolver, &unmatched);

    REQUIRE(wired == 1);
    REQUIRE(unmatched.empty());
    REQUIRE(first_child(ir.root)->audio_widget == AudioWidgetType::knob);
    REQUIRE(first_child(ir.root)->attributes.at("recognitionSource") == "builtin-library");
}

TEST_CASE("an already-recognized widget is never overridden by the resolver",
          "[view][import][recognition][issue-4676]") {
    // The TS plugin already stamped audio_widget=meter. A user manifest that maps
    // the same key to fader must NOT clobber the existing recognition (additive).
    const std::string envelope = R"({
        "format_version": "v1",
        "provenance": { "adapter": "figma-plugin", "version": "0.1.0" },
        "root": {
            "type": "frame", "name": "Panel",
            "children": [
                {
                    "type": "frame", "name": "VU",
                    "audio_widget": "meter",
                    "figma": { "component_key": "collide_key", "main_component_name": "X / Y" },
                    "children": []
                }
            ]
        }
    })";
    const std::string user_manifest = R"({
        "widgets": { "x": { "kind": "fader", "component_set_key": "collide_key" } }
    })";

    auto ir = parse_figma_plugin_json(envelope);
    REQUIRE(first_child(ir.root)->audio_widget == AudioWidgetType::meter);

    auto resolver = RecognitionResolver::with_builtin_library();
    std::string err;
    auto src = RecognitionResolver::parse_manifest_json(user_manifest, "user-manifest", &err);
    REQUIRE(src.has_value());
    resolver.add_source(std::move(*src));

    std::vector<UnmatchedComponent> unmatched;
    const int wired = apply_recognition_resolver(ir.root, resolver, &unmatched);

    REQUIRE(wired == 0);
    REQUIRE(first_child(ir.root)->audio_widget == AudioWidgetType::meter);  // unchanged
}

TEST_CASE("user-manifest source overrides the built-in on a key collision",
          "[view][import][recognition][issue-4676]") {
    // The user maps the built-in KNOB key to a fader. Later sources win.
    auto resolver = RecognitionResolver::with_builtin_library();
    const std::string user_manifest = R"({
        "widgets": {
            "override": {
                "kind": "fader",
                "component_set_key": "f74264ffa9108521fb0d3398dc8f5ea88e23a84e"
            }
        }
    })";
    std::string err;
    auto src = RecognitionResolver::parse_manifest_json(user_manifest, "user-manifest", &err);
    REQUIRE(src.has_value());
    resolver.add_source(std::move(*src));

    const auto resolved =
        resolver.resolve("f74264ffa9108521fb0d3398dc8f5ea88e23a84e", "Pulp / Knob");
    REQUIRE(resolved.matched);
    REQUIRE(resolved.kind == AudioWidgetType::fader);          // user override won
    REQUIRE(resolved.source_name == "user-manifest");
}

TEST_CASE("a custom-control manifest entry resolves a factory_id (issue #4677 hook)",
          "[view][import][recognition][issue-4676]") {
    // Forward-compat: an entry may carry a factory_id instead of a built-in kind,
    // which is exactly the shape #4677's installed-package design_controls
    // fragments will add as another source.
    const std::string manifest = R"({
        "widgets": {
            "fancy": {
                "component_set_key": "pkg_control_key",
                "factory_id": "acme.spinner"
            }
        }
    })";
    std::string err;
    auto src = RecognitionResolver::parse_manifest_json(manifest, "acme-pkg", &err);
    REQUIRE(err.empty());
    REQUIRE(src.has_value());

    RecognitionResolver resolver;
    resolver.add_source(std::move(*src));

    auto ir = parse_figma_plugin_json(third_party_envelope("pkg_control_key", "Acme / Spinner"));
    std::vector<UnmatchedComponent> unmatched;
    const int wired = apply_recognition_resolver(ir.root, resolver, &unmatched);

    REQUIRE(wired == 1);
    const auto* node = first_child(ir.root);
    REQUIRE(node->audio_widget == AudioWidgetType::none);  // not a built-in widget
    REQUIRE(node->attributes.at("recognitionFactoryId") == "acme.spinner");
}

TEST_CASE("malformed and empty manifests are rejected with a reason",
          "[view][import][recognition][issue-4676]") {
    std::string err;
    REQUIRE_FALSE(RecognitionResolver::parse_manifest_json("not json", "u", &err).has_value());
    REQUIRE_FALSE(err.empty());

    err.clear();
    // Object without "widgets".
    REQUIRE_FALSE(RecognitionResolver::parse_manifest_json("{}", "u", &err).has_value());
    REQUIRE_FALSE(err.empty());

    err.clear();
    // A widget with neither an identity nor a target is skipped → no usable entries.
    REQUIRE_FALSE(RecognitionResolver::parse_manifest_json(
        R"({ "widgets": { "x": { "kind": "knob" } } })", "u", &err).has_value());
    REQUIRE_FALSE(err.empty());
}

// Drift guard: the in-code built-in table MUST match the published
// library-manifest.json so the C++ resolver and the TS plugin agree on which
// keys map to which kinds.
TEST_CASE("the built-in recognition table matches library-manifest.json",
          "[view][import][recognition][issue-4676]") {
    const fs::path manifest_path =
        fs::path(PULP_REPO_ROOT) / "tools/figma-plugin/library-manifest.json";
    std::ifstream in(manifest_path, std::ios::binary);
    REQUIRE(in.good());
    std::ostringstream ss;
    ss << in.rdbuf();

    auto json = choc::json::parse(ss.str());
    REQUIRE(json.hasObjectMember("widgets"));
    auto widgets = json["widgets"];

    auto resolver = RecognitionResolver::with_builtin_library();

    for (uint32_t i = 0; i < widgets.size(); ++i) {
        const auto member = widgets.getObjectMemberAt(i);
        const std::string kind_id = member.name != nullptr ? member.name : "";
        const auto w = member.value;
        if (!w.hasObjectMember("component_set_key")) continue;
        const std::string key = std::string(w["component_set_key"].toString());
        if (key.rfind("TBD-", 0) == 0) continue;  // placeholder, excluded by design

        const auto resolved = resolver.resolve(key, /*name=*/"");
        INFO("widget " << kind_id << " key " << key);
        REQUIRE(resolved.matched);
        REQUIRE(resolved.source_name == "builtin-library");
        REQUIRE(resolved.kind == audio_widget_kind_from_manifest_id(kind_id));
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// P8 — installed-package custom controls: install → merge → resolve → materialize
// ─────────────────────────────────────────────────────────────────────────────

namespace {

// Write a project's packages.lock.json + registry.json into a fresh temp dir and
// return the dir. `registry_packages_json` is the inner `"packages": {...}` body.
fs::path write_project(const std::string& lock_packages_json,
                       const std::string& registry_packages_json) {
    static int counter = 0;
    fs::path dir = fs::temp_directory_path() /
                   ("pulp-p8-pkg-" + std::to_string(++counter) + "-" +
                    std::to_string(PULP_TEST_GETPID()));
    fs::create_directories(dir);
    {
        std::ofstream lock(dir / "packages.lock.json", std::ios::binary);
        lock << R"({ "lockfile_version": 1, "packages": )"
             << lock_packages_json << " }";
    }
    {
        // The registry lives at <root>/tools/packages/registry.json — the real
        // CLI layout (find_registry_path), NOT a sibling of the lockfile. Writing
        // it here is what makes this fixture exercise the production discovery path.
        fs::create_directories(dir / "tools" / "packages");
        std::ofstream reg(dir / "tools" / "packages" / "registry.json", std::ios::binary);
        reg << R"({ "registry_version": 2, "packages": )"
            << registry_packages_json << " }";
    }
    return dir;
}

// Minimal locked-package body (the fields the gather path doesn't read are
// elided — gather only needs the package id key).
std::string locked(const std::string& id) {
    return "{ \"" + id +
           "\": { \"version\": \"1.0.0\", \"resolved\": \"https://x/y\", "
           "\"integrity\": \"sha256-abc\", \"commit\": \"deadbeef\" } }";
}

} // namespace

TEST_CASE("installed-package design_controls merge into the resolver and resolve",
          "[view][import][recognition][issue-4677]") {
    // The cross-boundary acceptance path: a schema-valid design_controls fragment
    // on an INSTALLED package resolves end-to-end to the right factory_id — not a
    // happy-path on each side in isolation.
    const auto dir = write_project(
        locked("acme-controls"),
        R"({ "acme-controls": { "design_controls": [
              { "factory_id": "acme.spinner", "component_set_key": "pkg_spinner_key" }
        ] } })");

    auto pkg = pulp::view::discover_package_design_controls(dir);
    REQUIRE(pkg.warnings.empty());
    REQUIRE(pkg.sources.size() == 1);
    REQUIRE(pkg.sources[0].name == "acme-controls");

    auto resolver = RecognitionResolver::with_builtin_library();
    for (auto& s : pkg.sources) resolver.add_source(std::move(s));

    auto ir = parse_figma_plugin_json(
        third_party_envelope("pkg_spinner_key", "Acme / Spinner"));
    const int wired = apply_recognition_resolver(ir.root, resolver, nullptr);
    REQUIRE(wired == 1);

    const auto* node = first_child(ir.root);
    REQUIRE(node->audio_widget == AudioWidgetType::none);  // custom, not a knob
    REQUIRE(node->attributes.at("recognitionFactoryId") == "acme.spinner");
    REQUIRE(node->attributes.at("recognitionSource") == "acme-controls");

    fs::remove_all(dir);
}

TEST_CASE("no installed custom-control package leaves behavior unchanged",
          "[view][import][recognition][issue-4677]") {
    // Additive contract: a project with no packages.lock.json (or a lockfile with
    // no design-control package) contributes zero sources.
    const auto empty_dir = fs::temp_directory_path() /
                           ("pulp-p8-empty-" + std::to_string(PULP_TEST_GETPID()));
    fs::create_directories(empty_dir);
    auto none = pulp::view::discover_package_design_controls(empty_dir);
    REQUIRE(none.sources.empty());
    REQUIRE(none.warnings.empty());
    fs::remove_all(empty_dir);

    // A package that declares NO design_controls also contributes nothing.
    const auto dir = write_project(
        locked("plain-dsp"),
        R"({ "plain-dsp": { "category": "dsp" } })");
    auto pkg = pulp::view::discover_package_design_controls(dir);
    REQUIRE(pkg.sources.empty());
    fs::remove_all(dir);
}

TEST_CASE("duplicate component_set_key across entries: last-added source wins (pinned)",
          "[view][import][recognition][issue-4677]") {
    // Two packages declare the SAME component_set_key with DIFFERENT factory_ids.
    // The resolver merges later sources OVER earlier ones, and gather emits one
    // source per package in lockfile order — so the LAST package in lock order
    // wins, deterministically. Pin that so a re-order is a visible change.
    const auto dir = write_project(
        // Lock order is alphabetical by id in the JSON object we write; choc
        // preserves insertion order, so "first-pkg" then "second-pkg".
        R"({ "first-pkg": { "version": "1.0.0", "resolved": "https://x", "integrity": "sha256-a", "commit": "aaaaaaa" },
             "second-pkg": { "version": "1.0.0", "resolved": "https://x", "integrity": "sha256-b", "commit": "bbbbbbb" } })",
        R"({ "first-pkg":  { "design_controls": [ { "factory_id": "first.control",  "component_set_key": "shared_key" } ] },
             "second-pkg": { "design_controls": [ { "factory_id": "second.control", "component_set_key": "shared_key" } ] } })");

    auto pkg = pulp::view::discover_package_design_controls(dir);
    REQUIRE(pkg.sources.size() == 2);
    REQUIRE(pkg.sources[0].name == "first-pkg");
    REQUIRE(pkg.sources[1].name == "second-pkg");

    RecognitionResolver resolver;  // no built-in; only the two packages
    for (auto& s : pkg.sources) resolver.add_source(std::move(s));

    const auto resolved = resolver.resolve("shared_key", /*name=*/"");
    REQUIRE(resolved.matched);
    // Last-added source (second-pkg) wins the key collision.
    REQUIRE(resolved.factory_id == "second.control");
    REQUIRE(resolved.source_name == "second-pkg");
    REQUIRE(resolved.via == "key");

    fs::remove_all(dir);
}

TEST_CASE("name_prefix collision across merged packages: last-added source wins (pinned)",
          "[view][import][recognition][issue-4677]") {
    // Same identity via the name_prefix fallback rather than the key. The
    // resolver walks sources in reverse, so the last-added package's prefix entry
    // wins a collision — deterministic across the multi-package merge.
    const auto dir = write_project(
        R"({ "alpha-pkg": { "version": "1.0.0", "resolved": "https://x", "integrity": "sha256-a", "commit": "aaaaaaa" },
             "beta-pkg":  { "version": "1.0.0", "resolved": "https://x", "integrity": "sha256-b", "commit": "bbbbbbb" } })",
        R"({ "alpha-pkg": { "design_controls": [ { "factory_id": "alpha.widget", "name_prefix": "Shared / Widget" } ] },
             "beta-pkg":  { "design_controls": [ { "factory_id": "beta.widget",  "name_prefix": "Shared / Widget" } ] } })");

    auto pkg = pulp::view::discover_package_design_controls(dir);
    REQUIRE(pkg.sources.size() == 2);

    RecognitionResolver resolver;
    for (auto& s : pkg.sources) resolver.add_source(std::move(s));

    // No key match → prefix fallback. beta-pkg (last added) wins.
    const auto resolved = resolver.resolve(/*component_key=*/"", "Shared / Widget XL");
    REQUIRE(resolved.matched);
    REQUIRE(resolved.via == "name_prefix");
    REQUIRE(resolved.factory_id == "beta.widget");
    REQUIRE(resolved.source_name == "beta-pkg");

    fs::remove_all(dir);
}

TEST_CASE("malformed registry/lockfile is non-fatal (warns, merges nothing)",
          "[view][import][recognition][issue-4677]") {
    // A broken registry must never break an import — gather returns no sources
    // and a human-readable warning.
    static int n = 0;
    fs::path dir = fs::temp_directory_path() /
                   ("pulp-p8-bad-" + std::to_string(PULP_TEST_GETPID()) + "-" +
                    std::to_string(++n));
    fs::create_directories(dir);
    { std::ofstream l(dir / "packages.lock.json");
      l << R"({ "lockfile_version": 1, "packages": )" << locked("acme") << " }"; }
    { std::ofstream r(dir / "registry.json"); r << "{ this is not json"; }

    auto pkg = pulp::view::discover_package_design_controls(dir);
    REQUIRE(pkg.sources.empty());
    REQUIRE_FALSE(pkg.warnings.empty());
    fs::remove_all(dir);
}

TEST_CASE("materialize: a resolved custom control becomes a kind=custom IR element",
          "[view][import][recognition][issue-4677]") {
    // After the resolver stamps recognitionFactoryId, the materialize half turns
    // it into a kind=custom IRInteractiveElement carrying the factory_id and the
    // node's geometry — what the native materializer consumes.
    auto ir = parse_figma_plugin_json(
        third_party_envelope("pkg_spinner_key", "Acme / Spinner"));
    auto* node = const_cast<IRNode*>(first_child(ir.root));
    REQUIRE(node != nullptr);
    node->attributes["recognitionFactoryId"] = "acme.spinner";
    node->source_node_id = "1273:33424";
    node->style.left = 10.0f;
    node->style.top = 20.0f;
    node->style.width = 64.0f;
    node->style.height = 64.0f;

    const int n = pulp::view::materialize_recognized_custom_controls(ir.root);
    REQUIRE(n == 1);
    REQUIRE(node->interactive_elements.size() == 1);
    const auto& el = node->interactive_elements.front();
    REQUIRE(el.kind == InteractiveElementKind::custom);
    REQUIRE(el.factory_id == "acme.spinner");
    REQUIRE(el.w == Catch::Approx(64.0f));
    REQUIRE(el.h == Catch::Approx(64.0f));
    REQUIRE(el.source_node_id.has_value());
    REQUIRE(*el.source_node_id == "1273:33424");

    // Idempotent: a second pass does not double-stamp.
    REQUIRE(pulp::view::materialize_recognized_custom_controls(ir.root) == 0);
    REQUIRE(node->interactive_elements.size() == 1);
}

TEST_CASE("inert path: an unregistered custom factory renders inert + diagnoses",
          "[view][import][recognition][issue-4677]") {
    // The never-silent-knob contract: a kind=custom element whose factory_id is
    // NOT registered must render inert (the baked SVG still shows) AND emit a
    // diagnostic — never silently become a working knob. Exercise the REAL native
    // materializer so the diagnostic comes from the shipping path.
    clear_design_control_factories();

    DesignIR ir;
    ir.source = DesignSource::figma_plugin;
    ir.root.type = "frame";
    ir.root.name = "Panel";
    ir.root.render_mode = NodeRenderMode::faithful_svg;
    ir.root.svg_asset_id = "asset-svg";

    IRInteractiveElement custom;
    custom.kind = InteractiveElementKind::custom;
    custom.factory_id = "unregistered.control";
    custom.x = 10.0f; custom.y = 10.0f; custom.w = 40.0f; custom.h = 40.0f;
    ir.root.interactive_elements.push_back(custom);

    IRAssetRef asset;
    asset.asset_id = "asset-svg";
    asset.original_uri =
        "data:image/svg+xml,"
        "%3Csvg%20width%3D%22100%22%20height%3D%22100%22%20"
        "xmlns%3D%22http%3A%2F%2Fwww.w3.org%2F2000%2Fsvg%22%3E"
        "%3Crect%20x%3D%2210%22%20y%3D%2210%22%20width%3D%2280%22%20height%3D%2280%22%2F%3E"
        "%3C%2Fsvg%3E";
    asset.mime = "image/svg+xml";
    ir.asset_manifest.assets.push_back(asset);

    std::vector<ImportDiagnostic> diagnostics;
    auto root = build_native_view_tree(ir, ir.asset_manifest,
                                       {.diagnostics_out = &diagnostics});
    REQUIRE(root != nullptr);

    const bool inert_diag = std::any_of(
        diagnostics.begin(), diagnostics.end(), [](const ImportDiagnostic& d) {
            return d.code == "native-materialize-custom-factory-unregistered";
        });
    REQUIRE(inert_diag);

    // Now register the factory and confirm NO inert diagnostic fires.
    bool built = false;
    register_design_control_factory(
        "unregistered.control",
        [&](const DesignControlContext&) -> std::unique_ptr<View> {
            built = true;
            return std::make_unique<View>();
        });
    diagnostics.clear();
    auto root2 = build_native_view_tree(ir, ir.asset_manifest,
                                        {.diagnostics_out = &diagnostics});
    REQUIRE(root2 != nullptr);
    REQUIRE(built);
    const bool inert_diag2 = std::any_of(
        diagnostics.begin(), diagnostics.end(), [](const ImportDiagnostic& d) {
            return d.code == "native-materialize-custom-factory-unregistered";
        });
    REQUIRE_FALSE(inert_diag2);

    clear_design_control_factories();
}
