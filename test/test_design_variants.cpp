// Tests for variant-set -> typed component-contract synthesis.

#include <pulp/design/design_variants.hpp>

#include <catch2/catch_test_macros.hpp>

#include <choc/text/choc_JSON.h>

#include <string>

using namespace pulp::design;

namespace {
std::pair<std::string, std::string> P(std::string n, std::string v) { return {std::move(n), std::move(v)}; }
}  // namespace

TEST_CASE("parse_variant_name splits Prop=Value segments, trimmed", "[variants]") {
    auto p = parse_variant_name("Size = Large , State=Hover");
    REQUIRE(p.size() == 2);
    CHECK(p[0].name == "Size");
    CHECK(p[0].value == "Large");
    CHECK(p[1].name == "State");
    CHECK(p[1].value == "Hover");
}

TEST_CASE("parse_variant_name skips segments with no '='", "[variants]") {
    auto p = parse_variant_name("Size=Small, Broken, State=Default");
    REQUIRE(p.size() == 2);  // "Broken" dropped
    CHECK(p[0].name == "Size");
    CHECK(p[1].name == "State");
}

TEST_CASE("build_component_contract collects value enums per prop, sorted", "[variants]") {
    ComponentContract c = build_component_contract(
        "Button", {"Size=Small, State=Default", "Size=Large, State=Hover",
                   "Size=Small, State=Hover"});
    REQUIRE(c.props.size() == 2);
    // Props sorted by name: Size, State.
    CHECK(c.props[0].name == "Size");
    CHECK(c.props[0].values == std::vector<std::string>{"Large", "Small"});
    CHECK(c.props[1].name == "State");
    CHECK(c.props[1].values == std::vector<std::string>{"Default", "Hover"});
    CHECK(c.variant_count == 3);
    CHECK(c.issues.empty());
}

TEST_CASE("build_component_contract picks an explicit Default value", "[variants]") {
    ComponentContract c = build_component_contract(
        "Chip", {"State=Default", "State=Active", "State=Disabled"});
    REQUIRE(c.props.size() == 1);
    CHECK(c.props[0].default_value == "Default");
}

TEST_CASE("build_component_contract defaults a boolean prop to the falsey value", "[variants]") {
    ComponentContract c = build_component_contract("Toggle", {"On=True", "On=False"});
    REQUIRE(c.props.size() == 1);
    CHECK(c.props[0].default_value == "False");
}

TEST_CASE("build_component_contract falls back to the modal value", "[variants]") {
    // Distinct Blue and Red each appear once (the repeated Blue is deduped) -> the
    // modal tie resolves to sorted-first Blue; no explicit Default/boolean.
    ComponentContract c = build_component_contract(
        "Badge", {"Color=Blue", "Color=Red", "Color=Blue"});
    REQUIRE(c.props.size() == 1);
    CHECK(c.props[0].default_value == "Blue");
}

TEST_CASE("build_component_contract flags a malformed variant name", "[variants]") {
    ComponentContract c = build_component_contract("X", {"Size=Small", "JustAName"});
    bool found = false;
    for (const auto& i : c.issues)
        if (i.kind == "malformed-name" && i.detail.find("JustAName") != std::string::npos) found = true;
    CHECK(found);
}

TEST_CASE("build_component_contract flags inconsistent prop keys", "[variants]") {
    // Two variants have {Size,State}; one has only {Size} -> inconsistent.
    ComponentContract c = build_component_contract(
        "Y", {"Size=S, State=Off", "Size=L, State=On", "Size=M"});
    bool found = false;
    for (const auto& i : c.issues)
        if (i.kind == "inconsistent-props") found = true;
    CHECK(found);
}

TEST_CASE("build_component_contract flags a single-variant set", "[variants]") {
    ComponentContract c = build_component_contract("Solo", {"Size=Only"});
    bool found = false;
    for (const auto& i : c.issues)
        if (i.kind == "single-variant") found = true;
    CHECK(found);
}

TEST_CASE("a wholly-malformed variant does not skew the modal key set", "[variants]") {
    // "bad" has no valid prop; it must not make [] the modal set and thereby
    // flag the genuinely-typed "Size=Small" variant as inconsistent.
    ComponentContract c = build_component_contract("Z", {"bad", "Size=Small", "Size=Large"});
    bool inconsistent = false, malformed = false;
    for (const auto& i : c.issues) {
        if (i.kind == "inconsistent-props") inconsistent = true;
        if (i.kind == "malformed-name" && i.detail.find("\"bad\"") != std::string::npos)
            malformed = true;
    }
    CHECK(malformed);         // the junk variant is reported
    CHECK_FALSE(inconsistent); // the valid variants are not falsely flagged
    REQUIRE(c.props.size() == 1);
    CHECK(c.props[0].name == "Size");
}

TEST_CASE("empty prop name or value is malformed, not an empty enum", "[variants]") {
    ComponentContract c = build_component_contract("E", {"=Primary, State=", "Kind=Ghost"});
    // Neither "=Primary" nor "State=" yields a usable property.
    for (const auto& p : c.props) {
        CHECK_FALSE(p.name.empty());
        for (const auto& v : p.values) CHECK_FALSE(v.empty());
    }
    int malformed = 0;
    for (const auto& i : c.issues)
        if (i.kind == "malformed-name") ++malformed;
    CHECK(malformed >= 2);  // both empty-sided segments flagged
    // Only the well-formed Kind property survives.
    REQUIRE(c.props.size() == 1);
    CHECK(c.props[0].name == "Kind");
}

TEST_CASE("a duplicate key within one variant is flagged and counted once", "[variants]") {
    // "State=On, State=Off" is ambiguous; the second State must not be a second
    // observation that skews the enum or the default.
    ComponentContract c = build_component_contract("D", {"State=On, State=Off", "State=On"});
    REQUIRE(c.props.size() == 1);
    CHECK(c.props[0].name == "State");
    // Only the first State=On of the ambiguous variant counts, plus the clean
    // State=On variant -> On is modal, Off never observed.
    bool has_off = false;
    for (const auto& v : c.props[0].values) if (v == "Off") has_off = true;
    CHECK_FALSE(has_off);
    bool dup = false;
    for (const auto& i : c.issues)
        if (i.kind == "malformed-name" && i.detail.find("duplicate") != std::string::npos) dup = true;
    CHECK(dup);
}

TEST_CASE("a repeated whole variant is counted once and flagged", "[variants]") {
    // A copy-paste duplicate must not double-count: with the repeat removed,
    // Red and Blue tie and the sorted-first Blue wins — a raw count would let
    // the duplicate Red take the default.
    ComponentContract c =
        build_component_contract("Badge", {"Color=Red", "Color=Red", "Color=Blue"});
    REQUIRE(c.props.size() == 1);
    CHECK(c.props[0].default_value == "Blue");
    CHECK(c.variant_count == 2);  // two distinct variants, not three
    bool dup = false;
    for (const auto& i : c.issues)
        if (i.kind == "duplicate-variant") dup = true;
    CHECK(dup);
}

TEST_CASE("reordered segments count as the same variant", "[variants]") {
    // "A=1, B=2" and "B=2, A=1" are one variant, not two.
    ComponentContract c =
        build_component_contract("R", {"Size=S, State=Off", "State=Off, Size=S"});
    CHECK(c.variant_count == 1);
    bool dup = false, single = false;
    for (const auto& i : c.issues) {
        if (i.kind == "duplicate-variant") dup = true;
        if (i.kind == "single-variant") single = true;  // effectively one variant
    }
    CHECK(dup);
    CHECK(single);
}

TEST_CASE("a set with one real and one malformed variant is single-variant", "[variants]") {
    // Raw list length is 2, but only one variant informs the contract, so the
    // "not really a set" signal must still fire.
    ComponentContract c = build_component_contract("X", {"bad", "Size=Small"});
    bool single = false;
    for (const auto& i : c.issues)
        if (i.kind == "single-variant") single = true;
    CHECK(single);
}

TEST_CASE("component_contract_json is deterministic and complete", "[variants]") {
    ComponentContract c = build_component_contract(
        "Button", {"Size=Small, State=Default", "Size=Large, State=Hover"});
    std::string json = component_contract_json(c);
    auto v = choc::json::parse(json);
    CHECK(v["format_version"].getString() == std::string(kVariantContractVersion));
    CHECK(v["component"].getString() == "Button");
    CHECK(v["variant_count"].getWithDefault<int>(0) == 2);
    REQUIRE(v["props"].size() == 2);
    CHECK(v["props"][0]["name"].getString() == "Size");
    // Small and Large each appear once -> modal tie resolves to the sorted-first value.
    CHECK(v["props"][0]["default"].getString() == "Large");
    // Byte-stable across repeated serialization.
    CHECK(component_contract_json(c) == json);
}
