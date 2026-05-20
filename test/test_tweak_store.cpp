// Phase 0b PR-A: TweakStore + Inspector.applyTweak/listTweaks/clearTweaks/setBypass.
// Phase 1: pulp-tweaks.json disk persistence + Inspector.load/save/setAutoSave.
// Spec: planning/2026-05-18-inspector-direct-manipulation-roadmap.md

#include <catch2/catch_test_macros.hpp>

#include <pulp/inspect/domain_handler.hpp>
#include <pulp/inspect/protocol.hpp>
#include <pulp/inspect/tweak_store.hpp>

#include <choc/text/choc_JSON.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

using namespace pulp::inspect;

namespace {

// Tiny request builder so call sites stay readable.
InspectorMessage req(std::string method, std::string params) {
    return make_request(/*id=*/1, std::move(method), std::move(params));
}

std::string path_params(const std::filesystem::path& path) {
    auto params = choc::value::createObject("");
    params.addMember("path", choc::value::createString(path.string()));
    return choc::json::toString(params, false);
}

std::string auto_save_params(bool enabled, const std::filesystem::path& path) {
    auto params = choc::value::createObject("");
    params.addMember("enabled", choc::value::createBool(enabled));
    params.addMember("path", choc::value::createString(path.string()));
    return choc::json::toString(params, false);
}

// Wire a DomainHandler with just the TweakStore for these tests.
struct Fixture {
    TweakStore store;
    DomainHandler handler;
    Fixture() { handler.set_tweak_store(&store); }
};

// Phase 1 helpers ────────────────────────────────────────────────────────
//
// Each disk test gets its own scratch directory under the system temp
// root so the suite stays parallel-safe. The destructor removes the
// directory (and any half-written .tmp files). PULP_TWEAKS_FILE is
// cleared on construction so default_tweaks_path() lookups stay
// deterministic — tests that need it should setenv/unsetenv inside
// the test body.
struct TempTweaksDir {
    std::filesystem::path dir;
    std::filesystem::path file;

    TempTweaksDir() {
        // Counter ensures uniqueness even when two TempTweaksDirs land
        // on the same epoch-millisecond.
        static std::atomic<uint64_t> counter{0};
        auto now = std::chrono::steady_clock::now().time_since_epoch().count();
        std::ostringstream name;
        name << "pulp-tweaks-test-" << now << "-" << counter.fetch_add(1);
        dir = std::filesystem::temp_directory_path() / name.str();
        std::filesystem::create_directories(dir);
        file = dir / "pulp-tweaks.json";
#ifdef _WIN32
        _putenv_s("PULP_TWEAKS_FILE", "");
#else
        ::unsetenv("PULP_TWEAKS_FILE");
#endif
    }

    ~TempTweaksDir() {
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
    }

    std::string read() const {
        std::ifstream in(file);
        std::ostringstream ss;
        ss << in.rdbuf();
        return ss.str();
    }

    void write(const std::string& s) const {
        std::ofstream out(file);
        out << s;
    }
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

// Codex P2 follow-up on #2300: listTweaks must include anchors that
// have ONLY a bypass (no tweak records). Otherwise setBypass on an
// anchor with no entries — or one whose entries were later cleared
// via clearTweaks — silently drops out of the protocol response and
// the Phase 1 disk-persistence path loses the bypass state.
TEST_CASE("Inspector.listTweaks includes bypass-only anchors (codex P2 #2300)",
          "[inspect][protocol][listTweaks][regression]") {
    Fixture f;
    // Anchor "a" has a tweak; anchor "b" has only a bypass.
    f.store.apply_tweak("a", "layout.padding",
                        choc::value::createInt32(12), "drag");
    f.store.set_bypass("b", true);

    auto resp = f.handler.handle(req(methods::kInspectorListTweaks, "{}"));
    REQUIRE_FALSE(resp.is_error);
    auto parsed = choc::json::parse(resp.params_json);

    // Tweak side reports only anchors with records.
    REQUIRE(parsed["tweaks"].hasObjectMember("a"));
    REQUIRE_FALSE(parsed["tweaks"].hasObjectMember("b"));
    // Bypassed side must report BOTH (a has none here but b has true).
    REQUIRE(parsed["bypassed"].hasObjectMember("b"));
    REQUIRE(parsed["bypassed"]["b"].getBool());
}

TEST_CASE("Inspector.listTweaks reports bypass-only anchor after clearTweaks "
          "removes its records (codex P2 #2300)",
          "[inspect][protocol][listTweaks][regression]") {
    // Tweak on anchor c + a path-bypass on the same anchor. Then
    // clearTweaks({anchorId: c}) wipes c's tweak entries but leaves
    // its bypass intact. listTweaks must still surface the bypass.
    Fixture f;
    f.store.apply_tweak("c", "layout.padding",
                        choc::value::createInt32(8), "drag");
    f.store.set_bypass("c", std::vector<std::string>{"layout.padding"});

    f.handler.handle(req(methods::kInspectorClearTweaks,
        R"({"anchorId":"c"})"));

    auto resp = f.handler.handle(req(methods::kInspectorListTweaks, "{}"));
    REQUIRE_FALSE(resp.is_error);
    auto parsed = choc::json::parse(resp.params_json);

    REQUIRE_FALSE(parsed["tweaks"].hasObjectMember("c"));
    REQUIRE(parsed["bypassed"].hasObjectMember("c"));
    auto bypass_c = parsed["bypassed"]["c"];
    REQUIRE(bypass_c.isArray());
    REQUIRE(bypass_c.size() == 1);
    REQUIRE(bypass_c[0].getString() == "layout.padding");
}

TEST_CASE("TweakStore::bypassed_anchors enumerates every bypass entry "
          "regardless of tweak presence (codex P2 #2300)",
          "[inspect][tweak-store][regression]") {
    TweakStore s;
    s.apply_tweak("with-tweak", "x", choc::value::createInt32(1), {});
    s.set_bypass("with-tweak", true);
    s.set_bypass("bypass-only", std::vector<std::string>{"layout.gap"});

    auto anchors = s.bypassed_anchors();
    std::sort(anchors.begin(), anchors.end());
    REQUIRE(anchors.size() == 2);
    REQUIRE(anchors[0] == "bypass-only");
    REQUIRE(anchors[1] == "with-tweak");
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

// ── Phase 1: disk persistence ──────────────────────────────────────────

TEST_CASE("TweakStore: save -> clear -> load round-trips state",
          "[inspect][tweak-store][disk]") {
    TempTweaksDir tmp;
    TweakStore s;
    s.apply_tweak("anchor:a", "layout.padding", choc::value::createInt32(12), "drag");
    s.apply_tweak("anchor:a", "paint.bg", choc::value::createString("#abcdef"), "picker");
    s.apply_tweak("anchor:b", "layout.gap", choc::value::createInt32(4), "drag");
    s.set_bypass("anchor:a", std::vector<std::string>{"paint.bg"});

    auto saved = s.save_to_disk(tmp.file.string());
    REQUIRE(saved.ok);
    REQUIRE(saved.tweak_count == 3);
    REQUIRE(std::filesystem::exists(tmp.file));

    s.clear();
    REQUIRE(s.count() == 0);

    auto loaded = s.load_from_disk(tmp.file.string());
    REQUIRE(loaded.ok);
    REQUIRE(loaded.tweak_count == 3);
    REQUIRE(s.count() == 3);

    // Values restored. choc::json::parse normalises numeric literals
    // to int64 / double — use getWithDefault to stay agnostic to the
    // serialized width.
    REQUIRE(s.lookup("anchor:a", "layout.padding")->getWithDefault<int64_t>(0) == 12);
    REQUIRE(s.lookup("anchor:a", "paint.bg")->getString() == "#abcdef");
    REQUIRE(s.lookup("anchor:b", "layout.gap")->getWithDefault<int64_t>(0) == 4);

    // Bypass overlay restored
    REQUIRE(s.is_bypassed("anchor:a", "paint.bg"));
    REQUIRE_FALSE(s.is_bypassed("anchor:a", "layout.padding"));

    // Sources restored via the optional sidecar map
    auto recs = s.list_tweaks();
    bool found_drag_source = false;
    for (auto& r : recs) {
        if (r.anchor_id == "anchor:a" && r.property_path == "layout.padding") {
            REQUIRE(r.source == "drag");
            found_drag_source = true;
        }
    }
    REQUIRE(found_drag_source);
}

TEST_CASE("TweakStore: atomic write leaves no .tmp file behind after success",
          "[inspect][tweak-store][disk]") {
    TempTweaksDir tmp;
    TweakStore s;
    s.apply_tweak("a", "x", choc::value::createInt32(1), {});

    auto saved = s.save_to_disk(tmp.file.string());
    REQUIRE(saved.ok);
    REQUIRE(std::filesystem::exists(tmp.file));

    auto tmp_sidecar = tmp.file;
    tmp_sidecar += ".tmp";
    REQUIRE_FALSE(std::filesystem::exists(tmp_sidecar));
}

TEST_CASE("TweakStore: save_to_disk on a bad path returns error, no partial write",
          "[inspect][tweak-store][disk]") {
    TempTweaksDir tmp;
    TweakStore s;
    s.apply_tweak("a", "x", choc::value::createInt32(1), {});

    // Put a regular file where the parent directory should be. This is
    // unwritable on POSIX and Windows without assuming anything about
    // root-drive permissions on the CI runner.
    const auto blocked_parent = tmp.dir / "not-a-directory";
    {
        std::ofstream out(blocked_parent);
        out << "blocks child creation";
    }
    REQUIRE(std::filesystem::is_regular_file(blocked_parent));
    const auto bad = blocked_parent / "pulp-tweaks.json";

    auto saved = s.save_to_disk(bad.string());
    REQUIRE_FALSE(saved.ok);
    REQUIRE_FALSE(saved.error.empty());
    REQUIRE_FALSE(std::filesystem::exists(bad));
    auto tmp_file = bad;
    tmp_file += ".tmp";
    REQUIRE_FALSE(std::filesystem::exists(tmp_file));
}

TEST_CASE("TweakStore: auto-save flushes after every mutation",
          "[inspect][tweak-store][disk][auto-save]") {
    TempTweaksDir tmp;
    TweakStore s;
    s.set_auto_save(true, tmp.file.string());
    REQUIRE(s.auto_save_enabled());
    REQUIRE(s.auto_save_path() == tmp.file.string());

    s.apply_tweak("a", "x", choc::value::createInt32(7), "drag");
    REQUIRE(std::filesystem::exists(tmp.file));

    auto first = tmp.read();
    REQUIRE(first.find("\"x\"") != std::string::npos);
    REQUIRE(first.find("7") != std::string::npos);

    // A second mutation overwrites with the new value.
    s.apply_tweak("a", "x", choc::value::createInt32(99), "drag");
    auto second = tmp.read();
    REQUIRE(second.find("99") != std::string::npos);

    // Bypass changes also flush.
    s.set_bypass("a", true);
    auto third = tmp.read();
    REQUIRE(third.find("\"bypassed\"") != std::string::npos);

    // Disabling auto-save stops further flushes.
    s.set_auto_save(false);
    REQUIRE_FALSE(s.auto_save_enabled());
    s.apply_tweak("a", "y", choc::value::createInt32(123), {});
    auto fourth = tmp.read();
    // 'y' should NOT have hit disk.
    REQUIRE(fourth.find("\"y\"") == std::string::npos);
}

TEST_CASE("TweakStore: load_from_disk on missing file returns error, leaves state intact",
          "[inspect][tweak-store][disk]") {
    TempTweaksDir tmp;
    TweakStore s;
    s.apply_tweak("a", "x", choc::value::createInt32(1), {});
    REQUIRE(s.count() == 1);

    auto loaded = s.load_from_disk(tmp.file.string());  // file doesn't exist
    REQUIRE_FALSE(loaded.ok);
    REQUIRE_FALSE(loaded.error.empty());
    // In-memory state preserved.
    REQUIRE(s.count() == 1);
    REQUIRE(s.lookup("a", "x")->getInt32() == 1);
}

TEST_CASE("TweakStore: load rejects unsupported schema version",
          "[inspect][tweak-store][disk][schema]") {
    TempTweaksDir tmp;
    tmp.write(R"({
        "$schema": "pulp-tweaks://v999",
        "version": 999,
        "tweaks": { "anchor:a": { "x": 1 } }
    })");
    TweakStore s;
    auto loaded = s.load_from_disk(tmp.file.string());
    REQUIRE_FALSE(loaded.ok);
    REQUIRE(loaded.error.find("999") != std::string::npos);
    REQUIRE(s.count() == 0);  // no partial apply
}

TEST_CASE("TweakStore: load tolerates files written without an integer version field",
          "[inspect][tweak-store][disk][schema]") {
    // TS-canonical files use `$schema: pulp-tweaks://v1` and no integer
    // `version` — we accept those as v1 for back-compat.
    TempTweaksDir tmp;
    tmp.write(R"({
        "$schema": "pulp-tweaks://v1",
        "meta": { "pulpVersion": "0.0.0", "importSession": "test" },
        "tweaks": { "anchor:a": { "paint.bg": "#abc" } }
    })");
    TweakStore s;
    auto loaded = s.load_from_disk(tmp.file.string());
    REQUIRE(loaded.ok);
    REQUIRE(s.lookup("anchor:a", "paint.bg")->getString() == "#abc");
}

TEST_CASE("TweakStore: load rejects malformed JSON, leaves state intact",
          "[inspect][tweak-store][disk]") {
    TempTweaksDir tmp;
    tmp.write("{ not json");
    TweakStore s;
    s.apply_tweak("a", "x", choc::value::createInt32(1), {});
    auto loaded = s.load_from_disk(tmp.file.string());
    REQUIRE_FALSE(loaded.ok);
    REQUIRE(s.count() == 1);
}

TEST_CASE("TweakStore: bypass=true round-trips through disk",
          "[inspect][tweak-store][disk]") {
    TempTweaksDir tmp;
    TweakStore s;
    s.set_bypass("anchor:a", true);
    REQUIRE(s.save_to_disk(tmp.file.string()).ok);

    TweakStore t;
    auto loaded = t.load_from_disk(tmp.file.string());
    REQUIRE(loaded.ok);
    REQUIRE(loaded.bypass_count == 1);
    REQUIRE(t.is_bypassed("anchor:a", "any.path"));
}

TEST_CASE("TweakStore: bypass=path-list round-trips through disk",
          "[inspect][tweak-store][disk]") {
    TempTweaksDir tmp;
    TweakStore s;
    s.set_bypass("anchor:a", std::vector<std::string>{"paint.bg", "layout.gap"});
    REQUIRE(s.save_to_disk(tmp.file.string()).ok);

    TweakStore t;
    REQUIRE(t.load_from_disk(tmp.file.string()).ok);
    REQUIRE(t.is_bypassed("anchor:a", "paint.bg"));
    REQUIRE(t.is_bypassed("anchor:a", "layout.gap"));
    REQUIRE_FALSE(t.is_bypassed("anchor:a", "paint.color"));
}

TEST_CASE("TweakStore: default_tweaks_path honors PULP_TWEAKS_FILE",
          "[inspect][tweak-store][disk]") {
    // Save + restore the env so other tests aren't disturbed.
    const char* prev = std::getenv("PULP_TWEAKS_FILE");
    std::string saved_prev = prev ? prev : "";
#ifdef _WIN32
    _putenv_s("PULP_TWEAKS_FILE", "/tmp/explicit-tweaks.json");
#else
    ::setenv("PULP_TWEAKS_FILE", "/tmp/explicit-tweaks.json", 1);
#endif
    REQUIRE(TweakStore::default_tweaks_path() == "/tmp/explicit-tweaks.json");
#ifdef _WIN32
    if (saved_prev.empty()) _putenv_s("PULP_TWEAKS_FILE", "");
    else _putenv_s("PULP_TWEAKS_FILE", saved_prev.c_str());
#else
    if (saved_prev.empty()) ::unsetenv("PULP_TWEAKS_FILE");
    else ::setenv("PULP_TWEAKS_FILE", saved_prev.c_str(), 1);
#endif
}

TEST_CASE("TweakStore: from_json round-trips without touching disk",
          "[inspect][tweak-store][disk]") {
    TweakStore s;
    s.apply_tweak("a", "p", choc::value::createInt32(5), "drag");
    s.set_bypass("a", true);
    auto serialized = s.to_json();

    TweakStore t;
    auto r = t.from_json(serialized);
    REQUIRE(r.ok);
    REQUIRE(t.lookup("a", "p")->getWithDefault<int64_t>(0) == 5);
    REQUIRE(t.is_bypassed("a", "p"));
}

// ── Phase 1: protocol surface ──────────────────────────────────────────

TEST_CASE("Inspector.saveTweaks writes the file and returns the resolved path",
          "[inspect][protocol][saveTweaks]") {
    TempTweaksDir tmp;
    Fixture f;
    f.store.apply_tweak("a", "x", choc::value::createInt32(1), "drag");

    auto resp = f.handler.handle(req(methods::kInspectorSaveTweaks,
                                     path_params(tmp.file)));
    REQUIRE_FALSE(resp.is_error);
    auto parsed = choc::json::parse(resp.params_json);
    REQUIRE(parsed["ok"].getBool());
    REQUIRE(parsed["path"].getString() == tmp.file.string());
    REQUIRE(parsed["tweakCount"].getInt64() == 1);
    REQUIRE(std::filesystem::exists(tmp.file));
}

TEST_CASE("Inspector.loadTweaks restores state from disk",
          "[inspect][protocol][loadTweaks]") {
    TempTweaksDir tmp;
    // Seed disk via a separate store so the read path is exercised.
    {
        TweakStore writer;
        writer.apply_tweak("a", "p", choc::value::createInt32(42), "drag");
        REQUIRE(writer.save_to_disk(tmp.file.string()).ok);
    }
    Fixture f;
    auto resp = f.handler.handle(req(methods::kInspectorLoadTweaks,
                                     path_params(tmp.file)));
    REQUIRE_FALSE(resp.is_error);
    auto parsed = choc::json::parse(resp.params_json);
    REQUIRE(parsed["ok"].getBool());
    REQUIRE(parsed["tweakCount"].getInt64() == 1);
    REQUIRE(f.store.lookup("a", "p")->getWithDefault<int64_t>(0) == 42);
}

TEST_CASE("Inspector.loadTweaks reports schema mismatch as a protocol error",
          "[inspect][protocol][loadTweaks][schema]") {
    TempTweaksDir tmp;
    tmp.write(R"({"version":999,"tweaks":{}})");

    Fixture f;
    auto resp = f.handler.handle(req(methods::kInspectorLoadTweaks,
                                     path_params(tmp.file)));
    REQUIRE(resp.is_error);
    REQUIRE(resp.params_json.find("999") != std::string::npos);
}

TEST_CASE("Inspector.setAutoSave arms and disarms the flush hook",
          "[inspect][protocol][setAutoSave]") {
    TempTweaksDir tmp;
    Fixture f;
    auto resp = f.handler.handle(req(methods::kInspectorSetAutoSave,
                                     auto_save_params(true, tmp.file)));
    REQUIRE_FALSE(resp.is_error);
    auto parsed = choc::json::parse(resp.params_json);
    REQUIRE(parsed["enabled"].getBool());
    REQUIRE(parsed["path"].getString() == tmp.file.string());
    REQUIRE(f.store.auto_save_enabled());

    // applyTweak now flushes through the protocol path too.
    f.handler.handle(req(methods::kInspectorApplyTweak,
        R"({"anchorId":"a","propertyPath":"p","value":1,"source":"drag"})"));
    REQUIRE(std::filesystem::exists(tmp.file));

    // Disable.
    auto resp2 = f.handler.handle(req(methods::kInspectorSetAutoSave,
        R"({"enabled":false})"));
    REQUIRE_FALSE(resp2.is_error);
    REQUIRE_FALSE(f.store.auto_save_enabled());
}

TEST_CASE("Inspector.setAutoSave with missing `enabled` errors cleanly",
          "[inspect][protocol][setAutoSave]") {
    Fixture f;
    auto resp = f.handler.handle(req(methods::kInspectorSetAutoSave, R"({})"));
    REQUIRE(resp.is_error);
}

TEST_CASE("Inspector.load/save/setAutoSave without a store error cleanly",
          "[inspect][protocol][no-store][phase1]") {
    DomainHandler h;  // no tweak store
    REQUIRE(h.handle(req(methods::kInspectorLoadTweaks, "{}")).is_error);
    REQUIRE(h.handle(req(methods::kInspectorSaveTweaks, "{}")).is_error);
    REQUIRE(h.handle(req(methods::kInspectorSetAutoSave,
        R"({"enabled":true})")).is_error);
}
