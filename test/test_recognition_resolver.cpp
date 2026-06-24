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
#include <pulp/view/recognition_resolver.hpp>

#include <choc/text/choc_JSON.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

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
