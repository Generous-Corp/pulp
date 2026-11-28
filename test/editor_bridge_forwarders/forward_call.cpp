// Calls every deprecated EditorBridge attachment member. Compiled with
// -fsyntax-only by test_editor_bridge_forwarders.cmake: with
// PULP_TEST_INCLUDE_ATTACH the members resolve to the free functions in
// editor_bridge_attach.hpp; without it each use must be a compile error.

#include <pulp/view/editor_bridge.hpp>

#ifdef PULP_TEST_INCLUDE_ATTACH
#include <pulp/view/editor_bridge_attach.hpp>
#endif

namespace pulp::view {
class WebViewPanel;
class ScriptEngine;
class ScriptedUiSession;
class JsRuntime;
} // namespace pulp::view

void call_deprecated_members(pulp::view::EditorBridge& bridge, pulp::view::WebViewPanel& panel,
                             pulp::view::JsRuntime& runtime, pulp::view::ScriptEngine& engine,
                             pulp::view::ScriptedUiSession& session) {
    bridge.attach_webview(panel);
    bridge.detach_webview(panel);
    bridge.attach_native_runtime(runtime, "handler");
    bridge.attach_native_runtime(engine, "handler");
    bridge.attach_native_runtime(session, "handler");
    bridge.detach_native_runtime(session, "handler");
}
