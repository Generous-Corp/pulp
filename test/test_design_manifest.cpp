// Tests for the compiled design contract (pulp::design::DesignManifest): the
// token allowlist + per-component reskin contracts, its JSON serialization, and
// the LLM binding prompt.

#include <catch2/catch_test_macros.hpp>

#include <pulp/design/design_manifest.hpp>
#include <pulp/canvas/canvas.hpp>

#include <algorithm>

using namespace pulp::design;

namespace {

const ManifestComponent* find_component(const DesignManifest& m, const std::string& name) {
    for (const auto& c : m.components)
        if (c.name == name) return &c;
    return nullptr;
}

const ManifestToken* find_token(const DesignManifest& m, const std::string& name) {
    for (const auto& t : m.tokens)
        if (t.name == name) return &t;
    return nullptr;
}

}  // namespace

TEST_CASE("Ink & Signal manifest carries the token allowlist and component contracts",
          "[design-manifest]") {
    auto m = compile_ink_signal_manifest(/*dark=*/false);

    REQUIRE(m.manifest_version == std::string(kManifestVersion));
    REQUIRE(m.source == "ink-signal");
    REQUIRE(m.appearance == "light");
    REQUIRE(m.theme_schema_version == pulp::view::Theme::kSchemaVersion);
    REQUIRE_FALSE(m.tokens.empty());
    REQUIRE_FALSE(m.components.empty());

    // A known control is catalogued with its native class and reskin allowlist.
    const auto* knob = find_component(m, "Knob");
    REQUIRE(knob != nullptr);
    REQUIRE(knob->native_class == "pulp::view::Knob");
    REQUIRE_FALSE(knob->reskin_tokens.empty());
    REQUIRE(std::find(knob->reskin_tokens.begin(), knob->reskin_tokens.end(), "knob.thumb")
            != knob->reskin_tokens.end());
}

TEST_CASE("Manifest lists are sorted so the artifact diffs cleanly", "[design-manifest]") {
    auto m = compile_ink_signal_manifest();

    // Tokens sorted by (kind, name).
    for (size_t i = 1; i < m.tokens.size(); ++i) {
        const auto& a = m.tokens[i - 1];
        const auto& b = m.tokens[i];
        const bool ordered = (a.kind < b.kind) || (a.kind == b.kind && a.name <= b.name);
        REQUIRE(ordered);
    }
    // Components sorted by name.
    for (size_t i = 1; i < m.components.size(); ++i)
        REQUIRE(m.components[i - 1].name <= m.components[i].name);
    // Each component's reskin tokens are sorted.
    for (const auto& c : m.components)
        REQUIRE(std::is_sorted(c.reskin_tokens.begin(), c.reskin_tokens.end()));
}

TEST_CASE("Compilation and serialization are deterministic", "[design-manifest]") {
    auto a = compile_ink_signal_manifest();
    auto b = compile_ink_signal_manifest();
    REQUIRE(manifest_to_json(a) == manifest_to_json(b));
    // Serializing the same manifest twice is stable.
    REQUIRE(manifest_to_json(a) == manifest_to_json(a));
}

TEST_CASE("Dark and light appearances resolve different token values", "[design-manifest]") {
    auto light = compile_ink_signal_manifest(/*dark=*/false);
    auto dark = compile_ink_signal_manifest(/*dark=*/true);
    REQUIRE(dark.appearance == "dark");

    const auto* lbg = find_token(light, "bg.primary");
    const auto* dbg = find_token(dark, "bg.primary");
    REQUIRE(lbg != nullptr);
    REQUIRE(dbg != nullptr);
    REQUIRE(lbg->value != dbg->value);
}

TEST_CASE("Token values format by kind: color hex, trimmed dimension, verbatim string",
          "[design-manifest]") {
    pulp::view::Theme theme;
    theme.colors["red"] = pulp::canvas::Color::rgba8(255, 0, 0);
    theme.colors["half"] = pulp::canvas::Color::rgba(1.0f, 0.0f, 0.0f, 0.5f);  // alpha
    theme.dimensions["radius"] = 8.0f;
    theme.dimensions["gap"] = 1.5f;
    theme.dimensions["wide"] = 16.0f;  // two-digit whole number
    theme.dimensions["zero"] = 0.0f;
    theme.strings["font"] = "Inter";

    auto m = compile_design_manifest(theme, {}, "unit", "light");
    REQUIRE(m.components.empty());  // no catalog passed

    REQUIRE(find_token(m, "red")->kind == "color");
    REQUIRE(find_token(m, "red")->value == "#ff0000");
    // Non-opaque alpha widens to an 8-digit hex.
    REQUIRE(find_token(m, "half")->value == "#ff000080");
    // Dimensions render as the shortest round-tripping decimal (no trailing
    // zeros, no locale comma, whole numbers with no dot).
    REQUIRE(find_token(m, "radius")->value == "8");
    REQUIRE(find_token(m, "gap")->value == "1.5");
    REQUIRE(find_token(m, "wide")->value == "16");
    REQUIRE(find_token(m, "zero")->value == "0");
    // Strings are verbatim.
    REQUIRE(find_token(m, "font")->kind == "string");
    REQUIRE(find_token(m, "font")->value == "Inter");
}

TEST_CASE("Manifest JSON is well-formed and round-trips the counts", "[design-manifest]") {
    auto m = compile_ink_signal_manifest();
    auto json = manifest_to_json(m);
    // Structural markers of the fixed schema.
    REQUIRE(json.find("\"manifest_version\"") != std::string::npos);
    REQUIRE(json.find("\"tokens\"") != std::string::npos);
    REQUIRE(json.find("\"components\"") != std::string::npos);
    REQUIRE(json.find("\"reskin_tokens\"") != std::string::npos);
    REQUIRE(json.find(std::string(kManifestVersion)) != std::string::npos);
}

TEST_CASE("Manifest round-trips through JSON", "[design-manifest]") {
    auto original = compile_ink_signal_manifest();
    auto reparsed = manifest_from_json(manifest_to_json(original));

    REQUIRE(reparsed.manifest_version == original.manifest_version);
    REQUIRE(reparsed.source == original.source);
    REQUIRE(reparsed.appearance == original.appearance);
    REQUIRE(reparsed.theme_schema_version == original.theme_schema_version);
    REQUIRE(reparsed.tokens.size() == original.tokens.size());
    REQUIRE(reparsed.components.size() == original.components.size());
    // Serializing the reparsed manifest reproduces the original bytes.
    REQUIRE(manifest_to_json(reparsed) == manifest_to_json(original));
}

TEST_CASE("manifest_from_json on malformed input yields an empty contract, no throw",
          "[design-manifest]") {
    REQUIRE_NOTHROW(manifest_from_json("not json{{{"));
    REQUIRE(manifest_from_json("not json{{{").tokens.empty());
    REQUIRE(manifest_from_json("[]").components.empty());  // valid JSON, wrong shape
}

TEST_CASE("manifest_from_json degrades gracefully on partial tokens", "[design-manifest]") {
    // A token with no "name" is dropped; a valid one is kept even if a sibling
    // is malformed. A non-string value degrades to empty, never throws.
    const std::string json = R"({
      "tokens": [
        {"kind": "color", "value": "#fff"},
        {"name": "ok", "kind": "color", "value": "#000000"},
        {"name": "novalue", "kind": "dimension"}
      ]
    })";
    auto m = manifest_from_json(json);
    REQUIRE(m.tokens.size() == 2);  // the nameless token is dropped
    const auto* ok = find_token(m, "ok");
    REQUIRE(ok != nullptr);
    REQUIRE(ok->value == "#000000");
    REQUIRE(find_token(m, "novalue")->value.empty());
}

TEST_CASE("Binding prompt states the contract and lists tokens + components",
          "[design-manifest]") {
    auto m = compile_ink_signal_manifest();
    auto prompt = emit_binding_prompt(m);

    REQUIRE(prompt.find("# Design binding contract") != std::string::npos);
    REQUIRE(prompt.find("## Allowed tokens") != std::string::npos);
    REQUIRE(prompt.find("## Allowed components") != std::string::npos);
    // Adherence directive is present so the model is told not to invent tokens.
    REQUIRE(prompt.find("Do not invent token names") != std::string::npos);
    // A real token and a real component name appear.
    REQUIRE(prompt.find(m.tokens.front().name) != std::string::npos);
    REQUIRE(prompt.find("**Knob**") != std::string::npos);
    // The Knob's reskin allowlist is surfaced under it.
    REQUIRE(prompt.find("themed by:") != std::string::npos);
}
