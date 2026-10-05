// Tests for WidgetBridge runtime-import handlers.
//
// Contract for WidgetBridge::install_runtime_import_handlers() — the
// C++ side of @pulp/react/runtime-import. These tests don't depend on
// a real Claude bundle (parse_claude_bundle returns nullopt for
// arbitrary HTML, which the handler correctly reports through
// __pulpRuntimeImportErr__). The handler's job is to:
//   1. Register __pulpRuntimeImport__(html, source) and
//      __pulpRuntimeSettle__(rounds).
//   2. Surface bundle-parse failures via __pulpRuntimeImportErr__
//      rather than crashing the engine.
//   3. Be idempotent on repeat install.

#include <catch2/catch_test_macros.hpp>
#include <pulp/view/design_sources.hpp>
#include <pulp/view/js_engine.hpp>
#include <pulp/view/scripted_ui_prewarm.hpp>
#include <pulp/view/widget_bridge.hpp>

using namespace pulp::view;
using namespace pulp::state;

// Local copy of the JS single-quote escaper that test_widget_bridge.cpp
// also defines. The original is `static` (file-local) and not exported
// from any header, so a future shared `test/test_widget_bridge_helpers.hpp`
// could centralise it — until then this small copy keeps the split
// behavior-neutral.
static std::string js_single_quoted(std::string value) {
    std::string out;
    out.reserve(value.size() + 8);
    for (char c : value) {
        switch (c) {
            case '\\': out += "\\\\"; break;
            case '\'': out += "\\'"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            default: out += c; break;
        }
    }
    return out;
}

TEST_CASE("WidgetBridge install_runtime_import_handlers registers native functions",
          "[view][bridge][runtime-import]") {
    ScriptEngine engine;
    View root;
    StateStore store;
    WidgetBridge bridge(engine, root, store);
    bridge.install_runtime_import_handlers();

    // typeof __pulpRuntimeImport__ should be 'function' on the engine.
    auto result = engine.evaluate(
        "(typeof __pulpRuntimeImport__) + '|' + (typeof __pulpRuntimeSettle__)");
    REQUIRE(result.getWithDefault<std::string>("") == "function|function");
}

TEST_CASE("WidgetBridge __pulpRuntimeImport__ surfaces parse failure as soft error",
          "[view][bridge][runtime-import]") {
    ScriptEngine engine;
    View root;
    StateStore store;
    WidgetBridge bridge(engine, root, store);
    bridge.install_runtime_import_handlers();

    // Non-Claude HTML — parse_claude_bundle() returns nullopt; the
    // handler must capture that as a string error on __pulpRuntimeImportErr__,
    // not throw.
    engine.evaluate(
        "globalThis.__pulpRuntimeImportErr__ = null;"
        "try { __pulpRuntimeImport__('<html><body>plain</body></html>', 'plain'); }"
        "catch (e) { globalThis.__pulpRuntimeImportErr__ = 'threw:' + String(e); }");
    const auto err_str = engine.evaluate(
        "String(globalThis.__pulpRuntimeImportErr__ || '')")
        .getWithDefault<std::string>("");
    // Must contain the bundle envelope marker — and must NOT be a 'threw:' string.
    REQUIRE(err_str.find("threw:") == std::string::npos);
    REQUIRE(err_str.find("claude bundle") != std::string::npos);
}

TEST_CASE("WidgetBridge __pulpRuntimeImport__ accepts a materialized browser sidecar",
          "[view][bridge][runtime-import][materialized-browser]") {
    ScriptEngine engine;
    View root;
    StateStore store;
    WidgetBridge bridge(engine, root, store);
    bridge.install_runtime_import_handlers();

    const std::string sidecar = R"JSON({
      "schema":"pulp-materialized-browser-document-v1",
      "version":1,
      "html":"<html><body><script src=\"pulp-materialized-asset-93a17c7b5a173be2da95f76cb62a26ae30c0e13ce230acd243be63023258bf82\"></script></body></html>",
      "assets":[{
        "id":"pulp-materialized-asset-93a17c7b5a173be2da95f76cb62a26ae30c0e13ce230acd243be63023258bf82",
        "mime_type":"text/javascript",
        "byte_length":20,
        "data_base64":"Z2xvYmFsVGhpcy5va1RydWU9MTs=",
        "sha256":"93a17c7b5a173be2da95f76cb62a26ae30c0e13ce230acd243be63023258bf82"
      }]
    })JSON";
    engine.evaluate(
        "__pulpRuntimeImport__('" + js_single_quoted(sidecar) +
        "','materialized-browser');");
    REQUIRE(engine.evaluate("globalThis.okTrue === 1").getWithDefault(false));
    REQUIRE(engine.evaluate("String(globalThis.__pulpRuntimeImportErr__ || '')")
                .getWithDefault<std::string>("").empty());
}

TEST_CASE("WidgetBridge __pulpRuntimeImport__ finds inline scripts by the tag rules",
          "[view][bridge][runtime-import][materialized-browser]") {
    // The inline-script scan replaced std::regex
    //   <script\b([^>]*)>([\s\S]*?)</script>        (case-insensitive)
    // Each script pins one rule: the tag and its close are case-insensitive,
    // `<scripts>` is not a script (\b), a body ends at the FIRST close, JSON
    // payloads are not run, and a tag that is never closed is not a script.
    ScriptEngine engine;
    View root;
    StateStore store;
    WidgetBridge bridge(engine, root, store);
    bridge.install_runtime_import_handlers();

    const std::string sidecar = R"JSON({
      "schema":"pulp-materialized-browser-document-v1",
      "version":1,
      "html":"<html><body><script>globalThis.__order = ['plain'];</script><SCRIPT TYPE=\"text/javascript\">__order.push('upper');</SCRIPT><scripts>__order.push('scripts');</scripts><script type=\"application/json\">{\"x\":1}</script><script>__order.push('first');</script><script >__order.push('second');</Script><script>__order.push('never');</body></html>",
      "assets":[]
    })JSON";
    engine.evaluate(
        "__pulpRuntimeImport__('" + js_single_quoted(sidecar) +
        "','materialized-browser');");
    REQUIRE(engine.evaluate("String(globalThis.__pulpRuntimeImportErr__ || '')")
                .getWithDefault<std::string>("").empty());
    CHECK(engine.evaluate("JSON.stringify(globalThis.__order || null)")
              .getWithDefault<std::string>("") ==
          R"(["plain","upper","first","second"])");
}

TEST_CASE("WidgetBridge __pulpRuntimeImport__ dispatches v0 parser by source label",
          "[view][bridge][runtime-import-dispatch][v0][phase-6.6.2]") {
    ScriptEngine engine;
    View root;
    StateStore store;
    WidgetBridge bridge(engine, root, store);
    bridge.install_runtime_import_handlers();

    const std::string v0_tsx = R"(
        import { useState } from "react";
        export default function DispatchProbe() {
          const [level, setLevel] = useState(0.5);
          return (
            <div id="v0-dispatch-probe" style={{ display: "flex", flexDirection: "column" }}>
              <span>Level</span>
              <input
                type="range"
                min={0}
                max={1}
                step={0.01}
                value={level}
                onChange={(event) => setLevel(Number(event.currentTarget.value))}
              />
            </div>
          );
        }
    )";

    engine.evaluate(
        "globalThis.__pulpRuntimeImportErr__ = null;"
        "try { __pulpRuntimeImport__('" + js_single_quoted(v0_tsx) + "', 'v0'); }"
        "catch (e) { globalThis.__pulpRuntimeImportErr__ = 'threw:' + String(e); }");

    const auto err_str = engine.evaluate(
        "String(globalThis.__pulpRuntimeImportErr__ || '')")
        .getWithDefault<std::string>("");

    // A bare WidgetBridge test engine has no host React/ReactDOM installed,
    // so payload eval should soft-fail there. The important contract is that
    // source='v0' reached the v0 parser instead of the Claude envelope branch.
    REQUIRE(err_str.find("threw:") == std::string::npos);
    REQUIRE(err_str.find("claude bundle") == std::string::npos);
    REQUIRE(err_str.find("host React and ReactDOM") != std::string::npos);
}

TEST_CASE("WidgetBridge __pulpRuntimeImport__ dispatches Figma parser by source label",
          "[view][bridge][runtime-import-dispatch][figma][phase-6.6.3]") {
    ScriptEngine engine;
    View root;
    StateStore store;
    WidgetBridge bridge(engine, root, store);
    bridge.install_runtime_import_handlers();

    const std::string figma_tsx = R"(
        // Source: Figma Make export (sanitized for Pulp runtime import)
        import { useState } from "react";
        export default function DispatchProbe() {
          const [level, setLevel] = useState(0.5);
          return (
            <div id="figma-dispatch-probe" style={{ display: "flex", flexDirection: "column" }}>
              <span>Level</span>
              <input
                type="range"
                min={0}
                max={1}
                step={0.01}
                value={level}
                onChange={(event) => setLevel(Number(event.currentTarget.value))}
              />
            </div>
          );
        }
    )";

    engine.evaluate(
        "globalThis.__pulpRuntimeImportErr__ = null;"
        "try { __pulpRuntimeImport__('" + js_single_quoted(figma_tsx) + "', 'figma'); }"
        "catch (e) { globalThis.__pulpRuntimeImportErr__ = 'threw:' + String(e); }");

    const auto err_str = engine.evaluate(
        "String(globalThis.__pulpRuntimeImportErr__ || '')")
        .getWithDefault<std::string>("");

    REQUIRE(err_str.find("threw:") == std::string::npos);
    REQUIRE(err_str.find("claude bundle") == std::string::npos);
    REQUIRE(err_str.find("Figma Make runtime import requires host React and ReactDOM") != std::string::npos);
}

TEST_CASE("WidgetBridge __pulpRuntimeImport__ dispatches Stitch parser by source label",
          "[view][bridge][runtime-import-dispatch][stitch][phase-6.6.4]") {
    ScriptEngine engine;
    View root;
    StateStore store;
    WidgetBridge bridge(engine, root, store);
    bridge.install_runtime_import_handlers();

    const std::string stitch_tsx = R"(
        import { useState } from "react";
        export default function DispatchProbe() {
          const [level, setLevel] = useState(0.5);
          return (
            <div id="stitch-dispatch-probe" data-stitch-screen="probe" style={{ display: "flex", flexDirection: "column" }}>
              <span>Level</span>
              <input
                type="range"
                min={0}
                max={1}
                step={0.01}
                value={level}
                onChange={(event) => setLevel(Number(event.currentTarget.value))}
              />
            </div>
          );
        }
    )";

    engine.evaluate(
        "globalThis.__pulpRuntimeImportErr__ = null;"
        "try { __pulpRuntimeImport__('" + js_single_quoted(stitch_tsx) + "', 'stitch'); }"
        "catch (e) { globalThis.__pulpRuntimeImportErr__ = 'threw:' + String(e); }");

    const auto err_str = engine.evaluate(
        "String(globalThis.__pulpRuntimeImportErr__ || '')")
        .getWithDefault<std::string>("");

    REQUIRE(err_str.find("threw:") == std::string::npos);
    REQUIRE(err_str.find("claude bundle") == std::string::npos);
    REQUIRE(err_str.find("Stitch runtime import requires host React and ReactDOM") != std::string::npos);
}

TEST_CASE("WidgetBridge __pulpRuntimeImport__ dispatches RN parser by source label",
          "[view][bridge][runtime-import-dispatch][rn][phase-6.6.5]") {
    ScriptEngine engine;
    View root;
    StateStore store;
    WidgetBridge bridge(engine, root, store);
    bridge.install_runtime_import_handlers();

    const std::string rn_tsx = R"(
        import React, { useState } from "react";
        import { Pressable, StyleSheet, Text, View } from "react-native";
        export default function DispatchProbe() {
          const [armed, setArmed] = useState(true);
          return (
            <View id="rn-dispatch-probe" style={styles.panel}>
              <Text>React Native export</Text>
              <Text>Gain Stage</Text>
              <Pressable onPress={() => setArmed(!armed)} style={styles.button}>
                <Text>{armed ? "ARMED" : "BYPASS"}</Text>
              </Pressable>
            </View>
          );
        }
        const styles = StyleSheet.create({
          panel: { padding: 18, backgroundColor: "#111827" },
          button: { minHeight: 36 }
        });
    )";

    engine.evaluate(
        "globalThis.__pulpRuntimeImportErr__ = null;"
        "try { __pulpRuntimeImport__('" + js_single_quoted(rn_tsx) + "', 'rn'); }"
        "catch (e) { globalThis.__pulpRuntimeImportErr__ = 'threw:' + String(e); }");

    const auto err_str = engine.evaluate(
        "String(globalThis.__pulpRuntimeImportErr__ || '')")
        .getWithDefault<std::string>("");

    REQUIRE(err_str.find("threw:") == std::string::npos);
    REQUIRE(err_str.find("claude bundle") == std::string::npos);
    REQUIRE(err_str.find("React Native runtime import requires host React and ReactDOM") != std::string::npos);
}

TEST_CASE("WidgetBridge __pulpRuntimeImport__ auto-detects RN parser",
          "[view][bridge][runtime-import-dispatch][rn][phase-6.6.5]") {
    ScriptEngine engine;
    View root;
    StateStore store;
    WidgetBridge bridge(engine, root, store);
    bridge.install_runtime_import_handlers();

    const std::string rn_tsx = R"(
        import React from "react";
        import { StyleSheet, Text, View } from "react-native";
        export default function AutoNativeProbe() {
          return (
            <View id="rn-auto-dispatch-probe" style={styles.panel}>
              <Text>React Native export</Text>
              <Text>Auto detected</Text>
            </View>
          );
        }
        const styles = StyleSheet.create({
          panel: { padding: 18, backgroundColor: "#111827" }
        });
    )";

    engine.evaluate(
        "globalThis.__pulpRuntimeImportErr__ = null;"
        "try { __pulpRuntimeImport__('" + js_single_quoted(rn_tsx) + "'); }"
        "catch (e) { globalThis.__pulpRuntimeImportErr__ = 'threw:' + String(e); }");

    const auto err_str = engine.evaluate(
        "String(globalThis.__pulpRuntimeImportErr__ || '')")
        .getWithDefault<std::string>("");

    REQUIRE(err_str.find("threw:") == std::string::npos);
    REQUIRE(err_str.find("claude bundle") == std::string::npos);
    REQUIRE(err_str.find("React Native runtime import requires host React and ReactDOM") != std::string::npos);
}

TEST_CASE("WidgetBridge __pulpRuntimeImport__ auto-detects RN only when parse succeeds",
          "[view][bridge][runtime-import-dispatch][rn][phase-6.6.5]") {
    ScriptEngine engine;
    View root;
    StateStore store;
    WidgetBridge bridge(engine, root, store);
    bridge.install_runtime_import_handlers();

    const std::string claude_bundle_with_rn_text = R"(
        <script type="__bundler/manifest">{"main.js":{"mime":"text/javascript","data":"Z2xvYmFsVGhpcy5fX2NsYXVkZUZhbGxiYWNrSGl0ID0gJ3JlYWN0LW5hdGl2ZSBmYWxsYmFjayc7"}}</script>
        <script type="__bundler/template">"\u003cdiv id=\"root\"\u003eClaude bundle mentions react-native\u003c/div\u003e\u003cscript src=\"main.js\"\u003e\u003c/script\u003e"</script>
    )";

    engine.evaluate(
        "globalThis.__pulpRuntimeImportErr__ = null;"
        "globalThis.__claudeFallbackHit = null;"
        "try { __pulpRuntimeImport__('" + js_single_quoted(claude_bundle_with_rn_text) + "'); }"
        "catch (e) { globalThis.__pulpRuntimeImportErr__ = 'threw:' + String(e); }");

    const auto err_str = engine.evaluate(
        "String(globalThis.__pulpRuntimeImportErr__ || '')")
        .getWithDefault<std::string>("");

    REQUIRE(err_str.find("threw:") == std::string::npos);
    REQUIRE(err_str.find("React Native export") == std::string::npos);
    REQUIRE(err_str.find("claude bundle") == std::string::npos);
    REQUIRE(engine.evaluate("String(globalThis.__claudeFallbackHit || '')")
                .getWithDefault<std::string>("") == "react-native fallback");
}

TEST_CASE("WidgetBridge __pulpRuntimeImport__ dispatches Pencil parser by source label",
          "[view][bridge][runtime-import-dispatch][pencil][phase-6.6.6]") {
    ScriptEngine engine;
    View root;
    StateStore store;
    WidgetBridge bridge(engine, root, store);
    bridge.install_runtime_import_handlers();

    const std::string pencil_tsx = R"(
        import { useState } from "react";
        export default function DispatchProbe() {
          const [level, setLevel] = useState(0.5);
          return (
            <div id="pencil-dispatch-probe" data-pencil-export="tailwind-jsx-sanitized" style={{ display: "flex", flexDirection: "column" }}>
              <span>Level</span>
              <input
                type="range"
                min={0}
                max={1}
                step={0.01}
                value={level}
                onChange={(event) => setLevel(Number(event.currentTarget.value))}
              />
            </div>
          );
        }
    )";

    engine.evaluate(
        "globalThis.__pulpRuntimeImportErr__ = null;"
        "try { __pulpRuntimeImport__('" + js_single_quoted(pencil_tsx) + "', 'pencil'); }"
        "catch (e) { globalThis.__pulpRuntimeImportErr__ = 'threw:' + String(e); }");

    const auto err_str = engine.evaluate(
        "String(globalThis.__pulpRuntimeImportErr__ || '')")
        .getWithDefault<std::string>("");

    REQUIRE(err_str.find("threw:") == std::string::npos);
    REQUIRE(err_str.find("claude bundle") == std::string::npos);
    REQUIRE(err_str.find("Pencil runtime import requires host React and ReactDOM") != std::string::npos);

    const std::string unsupported_pencil_tsx = R"(
        import { useState } from "react";
        export default function UnsupportedPencilProbe() {
          const [value, setValue] = useState("");
          return <textarea value={value} onChange={(event) => setValue(event.currentTarget.value)} />;
        }
    )";

    engine.evaluate(
        "globalThis.__pulpRuntimeImportErr__ = null;"
        "try { __pulpRuntimeImport__('" + js_single_quoted(unsupported_pencil_tsx) + "', 'open-pencil'); }"
        "catch (e) { globalThis.__pulpRuntimeImportErr__ = 'threw:' + String(e); }");

    const auto unsupported_err = engine.evaluate(
        "String(globalThis.__pulpRuntimeImportErr__ || '')")
        .getWithDefault<std::string>("");

    REQUIRE(unsupported_err.find("threw:") == std::string::npos);
    REQUIRE(unsupported_err.find("unsupported Pencil React export (got 'open-pencil')") != std::string::npos);
}

TEST_CASE("WidgetBridge __pulpRuntimeSettle__ defers work to host frame boundary",
          "[view][bridge][runtime-import]") {
    ScriptEngine engine;
    View root;
    StateStore store;
    WidgetBridge bridge(engine, root, store);
    bridge.install_runtime_import_handlers();

    engine.evaluate(
        "globalThis.__settledFrameHits__ = 0;"
        "requestAnimationFrame(function () { __settledFrameHits__ += 1; });");

    // The native call must not recursively evaluate QuickJS. The queued frame
    // remains pending until the production host-frame boundary services it.
    auto result = engine.evaluate(
        "__pulpRuntimeSettle__(8); __pulpRuntimeSettle__(0); __pulpRuntimeSettle__(999); 'ok'");
    REQUIRE(result.getWithDefault<std::string>("") == "ok");
    REQUIRE(engine.evaluate("__settledFrameHits__").getWithDefault<int>(-1) == 0);

    bridge.service_frame_callbacks();
    REQUIRE(engine.evaluate("__settledFrameHits__").getWithDefault<int>(-1) == 1);
}

TEST_CASE("WidgetBridge install_runtime_import_handlers is idempotent",
          "[view][bridge][runtime-import]") {
    ScriptEngine engine;
    View root;
    StateStore store;
    WidgetBridge bridge(engine, root, store);

    // Calling twice must not throw and must leave the registrations intact.
    bridge.install_runtime_import_handlers();
    bridge.install_runtime_import_handlers();

    auto result = engine.evaluate("typeof __pulpRuntimeImport__");
    REQUIRE(result.getWithDefault<std::string>("") == "function");
}

// Every transient error global must be cleared between runtime-import attempts.
// Reason: the aggregator (in the runtime-import callback path) joins
// __pulpRuntimeImportErr__, __pulpEvalErr__, __pulpFlushSyncErr__, and
// any __pulpPayloadErr_<key>__. If clear_err() only resets the first
// one, an error from a previous import attempt would silently leak into
// the next aggregation and surface as a false-positive failure.
TEST_CASE("__pulpRuntimeImport__ clears all transient error globals before each call",
          "[view][bridge][runtime-import][issue-1856]") {
    ScriptEngine engine;
    View root;
    StateStore store;
    WidgetBridge bridge(engine, root, store);

    bridge.install_runtime_import_handlers();

    // Seed every transient error global with a stale value, simulating
    // a previous import attempt that surfaced via every code path.
    engine.evaluate(
        "globalThis.__pulpRuntimeImportErr__ = 'stale-import-err';"
        "globalThis.__pulpEvalErr__ = 'stale-eval-err';"
        "globalThis.__pulpFlushSyncErr__ = 'stale-flush-err';"
        "globalThis.__pulpCreateRootRenderErr__ = 'stale-render-err';"
        "globalThis.__pulpPayloadErr_block0__ = 'stale-payload-err';"
        "globalThis.__pulpPayloadErr_block1__ = 'stale-payload-err-2';"
        "void 0");

    // Trigger __pulpRuntimeImport__ with an HTML that has no claude
    // envelope — this exercises clear_err() at the top of the handler
    // BEFORE set_err() runs, so any leaked stale value would survive
    // *only if* clear_err is incomplete.
    engine.evaluate(
        "__pulpRuntimeImport__('<html><body>plain</body></html>', 'plain');"
        "void 0");

    // Read every slot back. clear_err runs first → all should be empty,
    // then set_err writes the new no-envelope error to __pulpRuntimeImportErr__.
    auto eval_after = engine.evaluate(
        "String(globalThis.__pulpEvalErr__ || '')")
        .getWithDefault<std::string>("");
    auto flush_after = engine.evaluate(
        "String(globalThis.__pulpFlushSyncErr__ || '')")
        .getWithDefault<std::string>("");
    auto render_after = engine.evaluate(
        "String(globalThis.__pulpCreateRootRenderErr__ || '')")
        .getWithDefault<std::string>("");
    auto payload0_after = engine.evaluate(
        "String(globalThis.__pulpPayloadErr_block0__ || '')")
        .getWithDefault<std::string>("");
    auto payload1_after = engine.evaluate(
        "String(globalThis.__pulpPayloadErr_block1__ || '')")
        .getWithDefault<std::string>("");

    REQUIRE(eval_after.empty());
    REQUIRE(flush_after.empty());
    REQUIRE(render_after.empty());
    REQUIRE(payload0_after.empty());
    REQUIRE(payload1_after.empty());

    // __pulpRuntimeImportErr__ should contain the NEW error (no envelope),
    // NOT the stale value. Distinguishes "did clear_err run?" from
    // "did set_err overwrite later?".
    auto import_after = engine.evaluate(
        "String(globalThis.__pulpRuntimeImportErr__ || '')")
        .getWithDefault<std::string>("");
    REQUIRE(import_after.find("stale-import-err") == std::string::npos);
    REQUIRE(import_after.find("no claude bundle envelope") != std::string::npos);
}

namespace {
// A materialized document with one verified asset and one inline script past
// the bytecode-cache threshold. `tag` makes a byte-distinct document.
std::string reopen_sidecar(const std::string& tag) {
    std::string pad(3000, 'x');
    return R"JSON({
      "schema":"pulp-materialized-browser-document-v1",
      "version":1,
      "html":"<html><body><script src=\"pulp-materialized-asset-93a17c7b5a173be2da95f76cb62a26ae30c0e13ce230acd243be63023258bf82\"></script><script>globalThis.__inlineRuns = (globalThis.__inlineRuns || 0) + 1; /* )JSON" + tag + pad + R"JSON( */</script></body></html>",
      "assets":[{
        "id":"pulp-materialized-asset-93a17c7b5a173be2da95f76cb62a26ae30c0e13ce230acd243be63023258bf82",
        "mime_type":"text/javascript",
        "byte_length":20,
        "data_base64":"Z2xvYmFsVGhpcy5va1RydWU9MTs=",
        "sha256":"93a17c7b5a173be2da95f76cb62a26ae30c0e13ce230acd243be63023258bf82"
      }]
    })JSON";
}

// Import `sidecar` into a fresh QuickJS realm, as a reopened editor does.
// Returns the bytecode-cache counters across the import call alone.
struct ImportDelta { std::uint64_t compiles = 0; std::uint64_t hits = 0; };
ImportDelta import_in_fresh_realm(const std::string& sidecar) {
    ScriptEngine engine(JsEngineType::quickjs);
    View root;
    StateStore store;
    WidgetBridge bridge(engine, root, store);
    bridge.install_runtime_import_handlers();
    const auto before = script_bytecode_cache_stats();
    engine.evaluate("__pulpRuntimeImport__('" + js_single_quoted(sidecar) +
                    "','materialized-browser');");
    const auto after = script_bytecode_cache_stats();
    REQUIRE(engine.evaluate("String(globalThis.__pulpRuntimeImportErr__ || '')")
                .getWithDefault<std::string>("").empty());
    CHECK(engine.evaluate("globalThis.okTrue === 1").getWithDefault(false));
    CHECK(engine.evaluate("globalThis.__inlineRuns").getWithDefault<int>(0) == 1);
    return {after.compiles - before.compiles, after.hits - before.hits};
}
} // namespace

TEST_CASE("WidgetBridge __pulpRuntimeImport__ verifies a document once per process",
          "[view][bridge][runtime-import][materialized-browser][editor-open]") {
    // Every editor open imports the same captured document into a new realm.
    // The decode + SHA-256 verification runs on the first open only; each
    // reopen reuses the verified bundle and the inline script's bytecode.
    clear_materialized_document_cache();
    clear_script_bytecode_cache();
    const auto doc = reopen_sidecar("a");

    const auto first = import_in_fresh_realm(doc);
    CHECK(first.compiles == 1);  // the inline script
    auto stats = materialized_document_cache_stats();
    CHECK(stats.verifies == 1);
    CHECK(stats.hits == 0);

    const auto second = import_in_fresh_realm(doc);
    CHECK(second.compiles == 0);
    CHECK(second.hits == 1);
    stats = materialized_document_cache_stats();
    CHECK(stats.verifies == 1);
    CHECK(stats.hits == 1);
    CHECK(stats.entries == 1);

    // Control: a byte-distinct document is verified on its own, never a hit.
    import_in_fresh_realm(reopen_sidecar("b"));
    stats = materialized_document_cache_stats();
    CHECK(stats.verifies == 2);
    CHECK(stats.hits == 1);
    CHECK(stats.entries == 2);

    clear_materialized_document_cache();
    clear_script_bytecode_cache();
}

TEST_CASE("prewarm_scripted_ui prepares a materialized document before the editor imports it",
          "[view][bridge][runtime-import][materialized-browser][editor-open]") {
    // A plug-in prewarms at instantiation; the editor's first import then
    // neither verifies the document nor compiles its scripts.
    clear_materialized_document_cache();
    clear_script_bytecode_cache();
    const auto doc = reopen_sidecar("prewarm");
    std::string runtime = "var __prewarmRuntime = 0;\n";
    while (runtime.size() < 16 * 1024)
        runtime += "__prewarmRuntime += 1; // stands in for a bundled UI runtime\n";

    const auto before = scripted_ui_prewarm_stats();
    prewarm_scripted_ui({{runtime}, {doc}});
    REQUIRE(wait_for_scripted_ui_prewarm(std::chrono::seconds(20)));
    const auto after = scripted_ui_prewarm_stats();
    CHECK(after.requests == before.requests + 1);
    CHECK(after.documents_verified == before.documents_verified + 1);
    CHECK(after.documents_rejected == before.documents_rejected);
    CHECK(after.scripts_compiled == before.scripts_compiled + 2);  // runtime + inline
    // Compiled as WidgetBridge::load_script() evaluates it, so a realm that
    // loads the runtime reads the prewarmed bytecode instead of compiling.
    CHECK(script_bytecode_cached(WidgetBridge::loaded_script_source(runtime)));
    CHECK_FALSE(script_bytecode_cached(runtime));
    CHECK(materialized_document_cache_stats().verifies == 1);
    {
        ScriptEngine engine(JsEngineType::quickjs);
        View root;
        StateStore store;
        WidgetBridge bridge(engine, root, store);
        const auto before_load = script_bytecode_cache_stats();
        bridge.load_script(runtime);
        const auto after_load = script_bytecode_cache_stats();
        CHECK(after_load.compiles == before_load.compiles);
        CHECK(after_load.hits == before_load.hits + 1);
    }

    const auto first_open = import_in_fresh_realm(doc);
    CHECK(first_open.compiles == 0);
    CHECK(first_open.hits == 1);
    const auto stats = materialized_document_cache_stats();
    CHECK(stats.verifies == 1);
    CHECK(stats.hits == 1);

    // Asking again is free: everything is cached.
    prewarm_scripted_ui({{runtime}, {doc}});
    REQUIRE(wait_for_scripted_ui_prewarm(std::chrono::seconds(20)));
    CHECK(scripted_ui_prewarm_stats().scripts_compiled == after.scripts_compiled);
    CHECK(materialized_document_cache_stats().verifies == 1);

    clear_materialized_document_cache();
    clear_script_bytecode_cache();
}

TEST_CASE("WidgetBridge __pulpRuntimeImport__ never keeps a rejected document",
          "[view][bridge][runtime-import][materialized-browser][editor-open]") {
    clear_materialized_document_cache();
    auto doc = reopen_sidecar("a");
    // Corrupt the payload so its SHA-256 no longer matches.
    const auto at = doc.find("Z2xvYmFsVGhpcy5va1RydWU9MTs=");
    REQUIRE(at != std::string::npos);
    doc.replace(at, 4, "Y2xv");

    for (int open = 0; open < 2; ++open) {
        ScriptEngine engine(JsEngineType::quickjs);
        View root;
        StateStore store;
        WidgetBridge bridge(engine, root, store);
        bridge.install_runtime_import_handlers();
        engine.evaluate("__pulpRuntimeImport__('" + js_single_quoted(doc) +
                        "','materialized-browser');");
        CHECK(engine.evaluate("String(globalThis.__pulpRuntimeImportErr__ || '')")
                  .getWithDefault<std::string>("")
                  .find("invalid materialized browser document") != std::string::npos);
        CHECK_FALSE(engine.evaluate("globalThis.okTrue === 1").getWithDefault(false));
    }
    const auto stats = materialized_document_cache_stats();
    CHECK(stats.verifies == 2);
    CHECK(stats.rejected == 2);
    CHECK(stats.hits == 0);
    CHECK(stats.entries == 0);
    clear_materialized_document_cache();
}
