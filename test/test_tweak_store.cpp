// Phase 0b PR-A: TweakStore + Inspector.applyTweak/listTweaks/clearTweaks/setBypass.
// Spec: planning/2026-05-18-inspector-direct-manipulation-roadmap.md

#include <catch2/catch_test_macros.hpp>

#include <pulp/inspect/domain_handler.hpp>
#include <pulp/inspect/protocol.hpp>
#include <pulp/inspect/tweak_store.hpp>

#include <choc/text/choc_JSON.h>

using namespace pulp::inspect;

namespace {

// Tiny request builder so call sites stay readable.
InspectorMessage req(std::string method, std::string params) {
    return make_request(/*id=*/1, std::move(method), std::move(params));
}

// Wire a DomainHandler with just the TweakStore for these tests.
struct Fixture {
    TweakStore store;
    DomainHandler handler;
    Fixture() { handler.set_tweak_store(&store); }
};

}  // namespace

// ── TweakStore unit tests ───────────────────────────────────────────────

TEST_CASE("TweakStore: apply_tweak records new entries", "[inspect][tweak-store]") {
    TweakStore s;
    REQUIRE(s.count() == 0);

    auto n = s.apply_tweak("anchor:a", "layout.padding",
                           choc::value::createInt32(12), "drag");
    REQUIRE(n == 1);
    REQUIRE(s.count() == 1);

    auto v = s.lookup("anchor:a", "layout.padding");
    REQUIRE(v.has_value());
    REQUIRE(v->getInt32() == 12);
}

TEST_CASE("TweakStore: apply_tweak overwrites the same anchor + path",
          "[inspect][tweak-store]") {
    TweakStore s;
    s.apply_tweak("anchor:a", "paint.bg", choc::value::createString("#abc"), "color-picker");
    s.apply_tweak("anchor:a", "paint.bg", choc::value::createString("#def"), "color-picker");
    REQUIRE(s.count() == 1);
    REQUIRE(s.lookup("anchor:a", "paint.bg")->getString() == "#def");
}

TEST_CASE("TweakStore: multiple paths under one anchor coexist",
          "[inspect][tweak-store]") {
    TweakStore s;
    s.apply_tweak("anchor:a", "layout.padding", choc::value::createInt32(12), {});
    s.apply_tweak("anchor:a", "paint.bg", choc::value::createString("#abc"), {});
    REQUIRE(s.count() == 2);
    auto recs = s.list_tweaks();
    REQUIRE(recs.size() == 2);
}

TEST_CASE("TweakStore: remove_tweak removes a single entry",
          "[inspect][tweak-store]") {
    TweakStore s;
    s.apply_tweak("anchor:a", "layout.padding", choc::value::createInt32(12), {});
    s.apply_tweak("anchor:a", "paint.bg", choc::value::createString("#abc"), {});
    REQUIRE(s.remove_tweak("anchor:a", "layout.padding"));
    REQUIRE_FALSE(s.remove_tweak("anchor:a", "layout.padding"));
    REQUIRE(s.count() == 1);
    REQUIRE(s.lookup("anchor:a", "paint.bg").has_value());
}

TEST_CASE("TweakStore: remove_anchor wipes all paths under an anchor",
          "[inspect][tweak-store]") {
    TweakStore s;
    s.apply_tweak("anchor:a", "layout.padding", choc::value::createInt32(12), {});
    s.apply_tweak("anchor:a", "paint.bg", choc::value::createString("#abc"), {});
    s.apply_tweak("anchor:b", "layout.gap", choc::value::createInt32(4), {});
    REQUIRE(s.remove_anchor("anchor:a") == 2);
    REQUIRE(s.count() == 1);
    REQUIRE(s.lookup("anchor:b", "layout.gap").has_value());
}

TEST_CASE("TweakStore: clear wipes tweaks + bypass overlay",
          "[inspect][tweak-store]") {
    TweakStore s;
    s.apply_tweak("anchor:a", "layout.padding", choc::value::createInt32(12), {});
    s.set_bypass("anchor:a", true);
    s.clear();
    REQUIRE(s.count() == 0);
    REQUIRE_FALSE(s.bypass_for("anchor:a").has_value());
}

// ── Bypass overlay ──────────────────────────────────────────────────────

TEST_CASE("TweakStore: bypass=true bypasses every path under the anchor",
          "[inspect][tweak-store]") {
    TweakStore s;
    s.set_bypass("anchor:a", true);
    REQUIRE(s.is_bypassed("anchor:a", "layout.padding"));
    REQUIRE(s.is_bypassed("anchor:a", "paint.bg"));
    REQUIRE_FALSE(s.is_bypassed("anchor:b", "layout.padding"));
}

TEST_CASE("TweakStore: bypass=string[] bypasses only listed paths",
          "[inspect][tweak-store]") {
    TweakStore s;
    s.set_bypass("anchor:a", std::vector<std::string>{"layout.padding"});
    REQUIRE(s.is_bypassed("anchor:a", "layout.padding"));
    REQUIRE_FALSE(s.is_bypassed("anchor:a", "paint.bg"));
}

TEST_CASE("TweakStore: bypass=false clears the overlay",
          "[inspect][tweak-store]") {
    TweakStore s;
    s.set_bypass("anchor:a", true);
    REQUIRE(s.is_bypassed("anchor:a", "any.path"));
    s.set_bypass("anchor:a", false);
    REQUIRE_FALSE(s.is_bypassed("anchor:a", "any.path"));
    REQUIRE_FALSE(s.bypass_for("anchor:a").has_value());
}

TEST_CASE("TweakStore: bypass=empty vector clears the overlay (per Codex spec)",
          "[inspect][tweak-store]") {
    TweakStore s;
    s.set_bypass("anchor:a", std::vector<std::string>{"x"});
    s.set_bypass("anchor:a", std::vector<std::string>{});
    REQUIRE_FALSE(s.bypass_for("anchor:a").has_value());
}

// ── Protocol round-trip via DomainHandler ───────────────────────────────

TEST_CASE("Inspector.applyTweak records the edit and returns tweakCount",
          "[inspect][protocol][applyTweak]") {
    Fixture f;
    auto resp = f.handler.handle(req(methods::kInspectorApplyTweak,
        R"({"anchorId":"figma:0:1","propertyPath":"layout.padding","value":12,"source":"drag"})"));
    REQUIRE_FALSE(resp.is_error);
    auto parsed = choc::json::parse(resp.params_json);
    REQUIRE(parsed["ok"].getBool());
    REQUIRE(parsed["tweakCount"].getInt64() == 1);
    REQUIRE(f.store.count() == 1);
    // choc::json parses JSON integer literals as int64 — the
    // applyTweak handler stores the parsed Value verbatim, so the
    // round-tripped lookup is int64, not int32.
    REQUIRE(f.store.lookup("figma:0:1", "layout.padding")->getInt64() == 12);
}

TEST_CASE("Inspector.applyTweak accepts string / object / array values",
          "[inspect][protocol][applyTweak]") {
    Fixture f;
    f.handler.handle(req(methods::kInspectorApplyTweak,
        R"({"anchorId":"a","propertyPath":"paint.bg","value":"#abcdef"})"));
    f.handler.handle(req(methods::kInspectorApplyTweak,
        R"({"anchorId":"a","propertyPath":"paint.color","value":{"r":255,"g":128,"b":0}})"));
    f.handler.handle(req(methods::kInspectorApplyTweak,
        R"({"anchorId":"a","propertyPath":"paint.shadow","value":[1,2,3]})"));
    REQUIRE(f.store.count() == 3);
    REQUIRE(f.store.lookup("a", "paint.bg")->getString() == "#abcdef");
    REQUIRE(f.store.lookup("a", "paint.color")->isObject());
    REQUIRE(f.store.lookup("a", "paint.shadow")->isArray());
}

TEST_CASE("Inspector.applyTweak without a tweak store errors cleanly",
          "[inspect][protocol][applyTweak]") {
    DomainHandler h;  // no tweak store wired
    auto resp = h.handle(req(methods::kInspectorApplyTweak,
        R"({"anchorId":"a","propertyPath":"p","value":1})"));
    REQUIRE(resp.is_error);
}

TEST_CASE("Inspector.applyTweak with malformed params errors cleanly",
          "[inspect][protocol][applyTweak]") {
    Fixture f;
    auto resp = f.handler.handle(req(methods::kInspectorApplyTweak,
        R"({"anchorId":"a"})"));  // missing propertyPath + value
    REQUIRE(resp.is_error);
}

TEST_CASE("Inspector.listTweaks returns the schema-shaped tweaks + bypassed maps",
          "[inspect][protocol][listTweaks]") {
    Fixture f;
    f.store.apply_tweak("a", "layout.padding", choc::value::createInt32(12), "drag");
    f.store.apply_tweak("a", "paint.bg", choc::value::createString("#abc"), "picker");
    f.store.apply_tweak("b", "layout.gap", choc::value::createInt32(4), "drag");
    f.store.set_bypass("b", true);

    auto resp = f.handler.handle(req(methods::kInspectorListTweaks, "{}"));
    REQUIRE_FALSE(resp.is_error);
    auto parsed = choc::json::parse(resp.params_json);

    REQUIRE(parsed["count"].getInt64() == 3);
    // choc::json int literals → int64. The applyTweak handler stored
    // them via parsed Value; values written directly with
    // createInt32() (as the test fixtures do here) retain int32. To
    // keep the assertion tolerant of either width, use getWithDefault.
    REQUIRE(parsed["tweaks"]["a"]["layout.padding"].getWithDefault<int64_t>(0) == 12);
    REQUIRE(parsed["tweaks"]["a"]["paint.bg"].getString() == "#abc");
    REQUIRE(parsed["tweaks"]["b"]["layout.gap"].getWithDefault<int64_t>(0) == 4);
    REQUIRE(parsed["bypassed"]["b"].getBool());
    // Anchor with no bypass shouldn't appear in `bypassed`.
    REQUIRE_FALSE(parsed["bypassed"].hasObjectMember("a"));
}

TEST_CASE("Inspector.clearTweaks with no selector wipes the table",
          "[inspect][protocol][clearTweaks]") {
    Fixture f;
    f.store.apply_tweak("a", "x", choc::value::createInt32(1), {});
    f.store.apply_tweak("b", "y", choc::value::createInt32(2), {});

    auto resp = f.handler.handle(req(methods::kInspectorClearTweaks, "{}"));
    REQUIRE_FALSE(resp.is_error);
    auto parsed = choc::json::parse(resp.params_json);
    REQUIRE(parsed["removed"].getInt64() == 2);
    REQUIRE(f.store.count() == 0);
}

TEST_CASE("Inspector.clearTweaks with anchorId removes one anchor's entries",
          "[inspect][protocol][clearTweaks]") {
    Fixture f;
    f.store.apply_tweak("a", "x", choc::value::createInt32(1), {});
    f.store.apply_tweak("a", "y", choc::value::createInt32(2), {});
    f.store.apply_tweak("b", "z", choc::value::createInt32(3), {});

    auto resp = f.handler.handle(req(methods::kInspectorClearTweaks,
        R"({"anchorId":"a"})"));
    REQUIRE_FALSE(resp.is_error);
    auto parsed = choc::json::parse(resp.params_json);
    REQUIRE(parsed["removed"].getInt64() == 2);
    REQUIRE(f.store.count() == 1);
    REQUIRE(f.store.lookup("b", "z").has_value());
}

TEST_CASE("Inspector.clearTweaks with anchorId + propertyPath removes one entry",
          "[inspect][protocol][clearTweaks]") {
    Fixture f;
    f.store.apply_tweak("a", "x", choc::value::createInt32(1), {});
    f.store.apply_tweak("a", "y", choc::value::createInt32(2), {});

    auto resp = f.handler.handle(req(methods::kInspectorClearTweaks,
        R"({"anchorId":"a","propertyPath":"x"})"));
    REQUIRE_FALSE(resp.is_error);
    auto parsed = choc::json::parse(resp.params_json);
    REQUIRE(parsed["removed"].getInt64() == 1);
    REQUIRE_FALSE(f.store.lookup("a", "x").has_value());
    REQUIRE(f.store.lookup("a", "y").has_value());
}

TEST_CASE("Inspector.setBypass with bool=true bypasses the whole anchor",
          "[inspect][protocol][setBypass]") {
    Fixture f;
    auto resp = f.handler.handle(req(methods::kInspectorSetBypass,
        R"({"anchorId":"a","value":true})"));
    REQUIRE_FALSE(resp.is_error);
    REQUIRE(f.store.is_bypassed("a", "any.path"));
}

TEST_CASE("Inspector.setBypass with string[] bypasses only listed paths",
          "[inspect][protocol][setBypass]") {
    Fixture f;
    auto resp = f.handler.handle(req(methods::kInspectorSetBypass,
        R"({"anchorId":"a","value":["layout.padding","paint.bg"]})"));
    REQUIRE_FALSE(resp.is_error);
    REQUIRE(f.store.is_bypassed("a", "layout.padding"));
    REQUIRE(f.store.is_bypassed("a", "paint.bg"));
    REQUIRE_FALSE(f.store.is_bypassed("a", "layout.gap"));
}

TEST_CASE("Inspector.setBypass with bool=false clears the bypass overlay",
          "[inspect][protocol][setBypass]") {
    Fixture f;
    f.store.set_bypass("a", true);
    auto resp = f.handler.handle(req(methods::kInspectorSetBypass,
        R"({"anchorId":"a","value":false})"));
    REQUIRE_FALSE(resp.is_error);
    REQUIRE_FALSE(f.store.is_bypassed("a", "any.path"));
}

TEST_CASE("Inspector.setBypass with non-bool/non-array value errors",
          "[inspect][protocol][setBypass]") {
    Fixture f;
    auto resp = f.handler.handle(req(methods::kInspectorSetBypass,
        R"({"anchorId":"a","value":42})"));
    REQUIRE(resp.is_error);
}

TEST_CASE("Inspector.listTweaks round-trips string[] bypass for an anchor",
          "[inspect][protocol][listTweaks]") {
    Fixture f;
    f.store.apply_tweak("a", "layout.padding", choc::value::createInt32(8), {});
    f.store.set_bypass("a", std::vector<std::string>{"layout.padding"});

    auto resp = f.handler.handle(req(methods::kInspectorListTweaks, "{}"));
    REQUIRE_FALSE(resp.is_error);
    auto parsed = choc::json::parse(resp.params_json);
    // Array-form bypass should serialize back as a JSON array, not a bool.
    // ValueView is a returned-by-value view; bind by value, not ref.
    auto bypassed_a = parsed["bypassed"]["a"];
    REQUIRE(bypassed_a.isArray());
    REQUIRE(bypassed_a.size() == 1);
    REQUIRE(bypassed_a[0].getString() == "layout.padding");
}

TEST_CASE("Inspector.applyTweak with un-parseable JSON errors cleanly",
          "[inspect][protocol][applyTweak]") {
    Fixture f;
    auto resp = f.handler.handle(req(methods::kInspectorApplyTweak, "not json"));
    REQUIRE(resp.is_error);
}

TEST_CASE("Inspector.clearTweaks with un-parseable JSON errors cleanly",
          "[inspect][protocol][clearTweaks]") {
    Fixture f;
    auto resp = f.handler.handle(req(methods::kInspectorClearTweaks, "{not json"));
    REQUIRE(resp.is_error);
}

TEST_CASE("Inspector.setBypass without a `value` key errors with a clear message",
          "[inspect][protocol][setBypass]") {
    Fixture f;
    auto resp = f.handler.handle(req(methods::kInspectorSetBypass,
        R"({"anchorId":"a"})"));
    REQUIRE(resp.is_error);
}

TEST_CASE("Inspector.setBypass with un-parseable JSON errors cleanly",
          "[inspect][protocol][setBypass]") {
    Fixture f;
    auto resp = f.handler.handle(req(methods::kInspectorSetBypass, "{not json"));
    REQUIRE(resp.is_error);
}

TEST_CASE("Inspector.listTweaks / clearTweaks / setBypass without store error",
          "[inspect][protocol][no-store]") {
    DomainHandler h;  // no tweak store
    REQUIRE(h.handle(req(methods::kInspectorListTweaks, "{}")).is_error);
    REQUIRE(h.handle(req(methods::kInspectorClearTweaks, "{}")).is_error);
    REQUIRE(h.handle(req(methods::kInspectorSetBypass,
        R"({"anchorId":"a","value":true})")).is_error);
}

TEST_CASE("Inspector.getInfo surfaces tweak_count when a store is attached",
          "[inspect][protocol][getInfo]") {
    Fixture f;
    f.store.apply_tweak("a", "x", choc::value::createInt32(1), {});
    f.store.apply_tweak("b", "y", choc::value::createInt32(2), {});

    auto resp = f.handler.handle(req(methods::kInspectorGetInfo, "{}"));
    REQUIRE_FALSE(resp.is_error);
    auto parsed = choc::json::parse(resp.params_json);
    REQUIRE(parsed["tweak_count"].getInt64() == 2);
}
