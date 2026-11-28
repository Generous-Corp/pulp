#pragma once

// Attaches an EditorBridge to a view-side message source: a WebViewPanel's
// structured message channel, a native ScriptEngine, or a ScriptedUiSession.
//
// The bridge's dispatch core (pulp/view/editor_bridge.hpp) is the view-free
// pulp::editor-bridge library. These attachments name view and view-script
// types, so they live in pulp::view-tooling and are declared here, apart from
// the core: a consumer that links only the core cannot call one by mistake.
//
// Each attachment installs a callback that refers to the bridge, so the bridge
// must outlive it. Detach (or destroy the source) before destroying the bridge.

#include <pulp/view/editor_bridge.hpp>

#include <string_view>

namespace pulp::view {

class WebViewPanel;
class ScriptEngine;
class ScriptedUiSession;
class JsRuntime;

/// Route `panel`'s structured messages through `bridge`. Equivalent to
///   panel.set_message_handler([&bridge](const WebViewMessage& m) {
///       return bridge.dispatch_webview_message(m.type, m.payload_json);
///   });
void attach_webview(EditorBridge& bridge, WebViewPanel& panel);

/// Clear the message handler attach_webview() installed. Safe to call when no
/// handler is installed.
void detach_webview(EditorBridge& bridge, WebViewPanel& panel);

/// The integration seam for a native JS runtime's postMessage source.
/// `handler_name` is the symbol the runtime exposes that calls back into C++.
/// Until JsRuntime has a postMessage-style primitive this records nothing, so
/// a call site does not have to feature-flag itself out.
void attach_native_runtime(EditorBridge& bridge, JsRuntime& runtime, std::string_view handler_name);

/// Register a global function on `engine` that takes one JSON envelope string
/// and returns `bridge`'s JSON response string. Throws std::invalid_argument
/// for an empty `handler_name`.
void attach_native_runtime(EditorBridge& bridge, ScriptEngine& engine,
                           std::string_view handler_name);

/// Attach to a ScriptedUiSession without borrowing its replaceable engine. The
/// endpoint survives scripted-UI reloads. Throws std::invalid_argument for an
/// empty `handler_name`. Call detach_native_runtime() before teardown.
void attach_native_runtime(EditorBridge& bridge, ScriptedUiSession& session,
                           std::string_view handler_name);
void detach_native_runtime(EditorBridge& bridge, ScriptedUiSession& session,
                           std::string_view handler_name);

} // namespace pulp::view
