// widget_bridge/runtime_import_api.cpp - runtime-import registrations for WidgetBridge.

#include <pulp/view/widget_bridge.hpp>
#include <pulp/view/design_import.hpp>
#include "api_registry.hpp"

#include <algorithm>
#include <cctype>
#include <exception>
#include <optional>
#include <string>

namespace pulp::view {

// Forward declaration of the claude_bundle.cpp wrapper that exposes its
// anonymous-namespace JSON string literal helper across translation units.
namespace detail {
std::string json_string_literal_for_widget_bridge(const std::string& s);
} // namespace detail

// ── Runtime design import ──────────────────────────────────────────
//
// Registers __pulpRuntimeImport__(html, source) and
// __pulpRuntimeSettle__(rounds) on the live bridge engine. The JS-side
// `@pulp/react/runtime-import` calls these to evaluate a design bundle
// in THIS engine (not a fresh one): one engine, one React, one reconciler.
//
// Architecturally distinct from the offline parse_claude_html_with_runtime
// path which allocates a fresh sandbox for IR extraction. That path stays
// available via `pulp import-design --execute-bundle` for inspection.
//
// Side effects (set on globalThis for the JS shim to inspect):
//   - __pulpRuntimeImportErr__ : string set on soft-fail (empty = ok)
//
// JS side reads/clears the err global; this function only writes to it.

void WidgetBridge::install_runtime_import_handlers() {
    // __pulpRuntimeImport__ evaluates an imported JS bundle (dynamic code load) —
    // the highest-risk surface after shell exec. Gated by the runtime_import
    // capability; an ungranted bridge never registers it, so JS cannot reach it.
    if (!granted_capabilities_.has(ReloadCapability::RuntimeImport)) return;
    // Idempotency guard: register once per bridge. The earlier
    // implementation used a static thread_local set keyed by `this`,
    // which broke under heap reuse (test bridges destroyed and
    // reconstructed at the same address skipped registration silently).
    if (runtime_import_installed_) return;
    runtime_import_installed_ = true;
    BridgeApiContext api{engine_};

    // The bundled-React asset evaluation can throw arbitrary JS errors
    // when the sandbox is missing a host primitive (DecompressionStream,
    // some HTMLElement, etc.). We surface those via the runtime-error
    // sink rather than throwing into the JS engine — the JS shim is
    // responsible for routing them to opts.onError.
    auto set_err = [this](const std::string& msg) {
        std::string js = "globalThis.__pulpRuntimeImportErr__ = ";
        js += detail::json_string_literal_for_widget_bridge(msg);
        js += ";void 0";
        try { engine_.evaluate(js); } catch (...) { /* best-effort */ }
    };

    auto clear_err = [this]() {
        // A stale error from a previous runtime-import attempt would otherwise
        // leak into the next aggregation. Clear every transient error global the
        // aggregator reads: __pulpRuntimeImportErr__ itself, the
        // babel-transform / flushSync / createRoot-render slots, and any
        // per-payload __pulpPayloadErr_*.
        try {
            engine_.evaluate(
                "(function(){"
                "  globalThis.__pulpRuntimeImportErr__ = '';"
                "  globalThis.__pulpEvalErr__ = '';"
                "  globalThis.__pulpFlushSyncErr__ = '';"
                "  globalThis.__pulpCreateRootRenderErr__ = '';"
                "  for (var k in globalThis) {"
                "    if (k.indexOf('__pulpPayloadErr_') === 0) globalThis[k] = '';"
                "  }"
                "})();void 0");
        } catch (...) { /* best-effort */ }
    };

    // __pulpRuntimeImport__(html, source_label) → void
    //
    // Parses the selected runtime-import source bundle, evaluates inline
    // text/javascript + text/babel scripts on THIS engine, dispatches
    // DOMContentLoaded.
    //
    // Crucially does NOT call buildDom or walkDomJson: the runtime path
    // is reconciler-owned (the JS-side ReactDOM capture shim catches
    // the React element directly).
    register_bridge_function(api, "__pulpRuntimeImport__",
        [this, set_err, clear_err](choc::javascript::ArgumentList args) -> choc::value::Value {
            clear_err();
            auto html = args.get<std::string>(0, "");
            auto src_label = args.get<std::string>(1, "auto");
            if (html.empty()) {
                set_err("__pulpRuntimeImport__: empty html");
                return choc::value::Value();
            }

            try {
                auto source_lc = src_label;
                std::transform(source_lc.begin(), source_lc.end(), source_lc.begin(),
                    [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

                std::optional<ClaudeBundle> bundle;
                if (source_lc == "v0" || source_lc == "v0.dev" || source_lc == "v0-dev") {
                    bundle = parse_v0_dev_react(html);
                    if (!bundle) {
                        set_err("__pulpRuntimeImport__: unsupported v0.dev React export (got '"
                                + src_label + "')");
                        return choc::value::Value();
                    }
                } else if (source_lc == "figma" || source_lc == "figma-make") {
                    bundle = parse_figma_make_react(html);
                    if (!bundle) {
                        set_err("__pulpRuntimeImport__: unsupported Figma Make React export (got '"
                                + src_label + "')");
                        return choc::value::Value();
                    }
                } else if (source_lc == "stitch" || source_lc == "google-stitch") {
                    bundle = parse_stitch_react(html);
                    if (!bundle) {
                        set_err("__pulpRuntimeImport__: unsupported Stitch React export (got '"
                                + src_label + "')");
                        return choc::value::Value();
                    }
                } else if (source_lc == "rn" || source_lc == "react-native" ||
                           source_lc == "reactnative") {
                    bundle = parse_react_native_export(html);
                    if (!bundle) {
                        set_err("__pulpRuntimeImport__: unsupported React Native export (got '"
                                + src_label + "')");
                        return choc::value::Value();
                    }
                } else if (source_lc == "pencil" || source_lc == "open-pencil" ||
                           source_lc == "openpencil") {
                    bundle = parse_pencil_react(html);
                    if (!bundle) {
                        set_err("__pulpRuntimeImport__: unsupported Pencil React export (got '"
                                + src_label + "')");
                        return choc::value::Value();
                    }
                } else if ((source_lc == "auto" || source_lc.empty())
                           && html.find("react-native") != std::string::npos) {
                    bundle = parse_react_native_export(html);
                    if (!bundle) bundle = parse_claude_bundle(html);
                    if (!bundle) bundle = parse_pencil_react(html);
                } else {
                    bundle = parse_react_native_export(html);
                    if (!bundle) bundle = parse_pencil_react(html);
                    if (!bundle) bundle = parse_claude_bundle(html);
                }
                if (!bundle) {
                    set_err("__pulpRuntimeImport__: no claude bundle envelope (got '"
                            + src_label + "')");
                    return choc::value::Value();
                }

                // The shared shim setup + payload eval logic lives in
                // a helper that both this path and the offline path
                // (parse_claude_html_with_runtime) can call. The runtime path
                // only needs the live-engine sequence: shims, asset eval,
                // inline script eval. It deliberately skips buildDom +
                // walkDomJson because reconciliation is owned by the JS-side
                // ReactDOM capture shim.
                evaluate_claude_bundle_in_live_engine(*bundle);
                // The shared pipeline writes per-payload eval failures to
                // `__pulpPayloadErr_<idx>__` and Babel-transform failures to
                // `__pulpEvalErr__`, but JS callers read
                // `__pulpRuntimeImportErr__`. Aggregate any soft errors into
                // the runtime-error sink so they're visible to onError /
                // lastError callers. Best-effort — if the engine is in a bad
                // state, leave the existing err value (set by set_err /
                // clear_err above) intact.
                try {
                    auto v = engine_.evaluate(
                        "(function(){"
                        "  var errs = [];"
                        "  var k = globalThis.__pulpRuntimeImportErr__;"
                        "  if (typeof k === 'string' && k.length) errs.push(k);"
                        "  if (typeof globalThis.__pulpEvalErr__ === 'string' && globalThis.__pulpEvalErr__.length)"
                        "    errs.push('babel-transform: ' + globalThis.__pulpEvalErr__);"
                        "  if (typeof globalThis.__pulpFlushSyncErr__ === 'string' && globalThis.__pulpFlushSyncErr__.length)"
                        "    errs.push('flushSync: ' + globalThis.__pulpFlushSyncErr__);"
                        // run_claude_bundle_payload_pipeline writes
                        // createRoot/render failures here. Without this branch,
                        // render-time exceptions never propagate to
                        // onError/lastError.
                        "  if (typeof globalThis.__pulpCreateRootRenderErr__ === 'string' && globalThis.__pulpCreateRootRenderErr__.length)"
                        "    errs.push('createRoot/render: ' + globalThis.__pulpCreateRootRenderErr__);"
                        "  for (var key in globalThis) {"
                        "    if (key.indexOf('__pulpPayloadErr_') === 0) {"
                        "      var pe = globalThis[key];"
                        "      if (typeof pe === 'string' && pe.length) errs.push('payload ' + key.slice(17, -2) + ': ' + pe);"
                        "    }"
                        "  }"
                        "  return errs.join(' | ');"
                        "})()");
                    auto aggregated = v.getWithDefault<std::string>("");
                    if (!aggregated.empty()) set_err(aggregated);
                } catch (...) { /* leave existing err */ }
            } catch (const std::exception& e) {
                set_err(std::string("__pulpRuntimeImport__ threw: ") + e.what());
            } catch (...) {
                set_err("__pulpRuntimeImport__ threw: unknown exception");
            }
            return choc::value::Value();
        });

    // __pulpRuntimeSettle__(rounds) → void
    //
    // Pump the bridge's message loop + service_frame_callbacks the
    // requested number of times. Used by the JS shim to drain
    // useEffect callbacks, rAF queues, and timers after React commits.
    register_bridge_function(api, "__pulpRuntimeSettle__",
        [this](choc::javascript::ArgumentList args) -> choc::value::Value {
            int rounds = static_cast<int>(args.get<double>(0, 8.0));
            if (rounds < 1) rounds = 1;
            if (rounds > 64) rounds = 64;  // sanity cap
            for (int i = 0; i < rounds; ++i) {
                try {
                    engine_.pump_message_loop();
                    service_frame_callbacks();
                } catch (...) { /* swallow — best-effort settle */ }
            }
            return choc::value::Value();
        });
}

} // namespace pulp::view
