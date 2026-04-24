#pragma once

// Renderer-agnostic editor↔processor message bridge for plugin editors.
//
// Plugin editors (WebView panels today, native-JS-runtime imports
// from Claude Design tomorrow) send JSON envelopes to the C++
// processor and consume JSON responses. EditorBridge owns the
// envelope parse, the type→handler dispatch table, the response
// builders, and the standard error vocabulary so each plugin only
// writes its own domain handlers.
//
// ── Envelope ───────────────────────────────────────────────────────────
//
// Inbound envelopes are JSON objects of this shape:
//
//   { "type": "<kind>", "payload": { ... } }   // payload optional
//
// Responses are always one of:
//
//   { "ok": true }                       // success, no extras
//   { "ok": true, ... }                  // success, handler-supplied extras
//   { "ok": false, "error": "..." }      // failure
//
// dispatch_json(...) and dispatch(...) are noexcept and always return
// a well-formed response envelope. Handlers may throw — the catch path
// emits an `internal_error` response.
//
// ── Standard error vocabulary ──────────────────────────────────────────
//
// Envelope-level failures emit errors whose message contains one of
// these substrings (lowercase tokens are the categories; the human
// strings are designed to be substring-compatible with existing
// consumers, notably Spectr's editor_bridge tests):
//
//   malformed_json   → "malformed JSON"           (parse failed)
//                      "envelope must be an object"
//   unknown_type     → "unknown message type"     (no handler registered)
//   missing_field    → "envelope missing 'type'"  (or handler-emitted)
//   wrong_type       → handler-emitted via err_response()
//   internal_error   → "internal error"           (handler threw)
//
// Plugin-level handlers may use err_response("...") with any message.
// The framework reserves the substrings above only for envelope-level
// failures it emits itself.

#include <choc/containers/choc_Value.h>

#include <cstddef>
#include <functional>
#include <memory>
#include <string>
#include <string_view>

namespace pulp::view {

class WebViewPanel;

// Forward-declared so EditorBridge can expose attach_native_runtime()
// before the JS engine surface is fully available in this header. The
// concrete JsRuntime type lives in pulp/view/js_engine.hpp.
class JsRuntime;

/// Renderer-agnostic dispatcher for editor↔processor JSON messages.
///
/// Typical usage from a plugin editor:
///
///   pulp::view::EditorBridge bridge;
///   bridge.add_handler("paint_start", [this](const auto&) {
///       drag_snap_ = BandSnapshot::capture(field_);
///       return pulp::view::EditorBridge::ok_response();
///   });
///   bridge.add_handler("morph", [this](const auto& payload) {
///       const auto t = std::clamp(
///           pulp::view::EditorBridge::get_float(payload, "t", 0.0f),
///           0.0f, 1.0f);
///       apply_morph_to_live(t);
///       return pulp::view::EditorBridge::ok_response();
///   });
///   bridge.attach_webview(*panel_);
class EditorBridge {
public:
    /// Handler signature. Receives the parsed payload (or an empty
    /// object if the envelope had no payload). Returns the full JSON
    /// response envelope to send back to the editor. Use the static
    /// ok_response / err_response helpers to build it.
    using Handler = std::function<std::string(const choc::value::ValueView& payload)>;

    EditorBridge();
    ~EditorBridge();

    // Non-copyable AND non-movable.
    //
    // attach_webview() and attach_native_runtime() install callbacks
    // that reference this bridge's internal state. Allowing moves
    // would let an attached bridge be relocated out from under those
    // callbacks (vector reallocation, std::optional emplace, move-
    // construction of an editor that contains one), and the next
    // inbound message would dereference a moved-from instance with
    // a null impl. Requiring construction-in-place surfaces that
    // mistake at compile time. (Codex review on PR #711.)
    EditorBridge(const EditorBridge&) = delete;
    EditorBridge& operator=(const EditorBridge&) = delete;
    EditorBridge(EditorBridge&&) = delete;
    EditorBridge& operator=(EditorBridge&&) = delete;

    /// Register a handler for the given message `type`. Replacing an
    /// existing handler for the same type is allowed and silent.
    void add_handler(std::string_view type, Handler fn);

    /// Remove a handler. No-op if no handler was registered.
    void remove_handler(std::string_view type);

    /// True if a handler is registered for `type`.
    bool has_handler(std::string_view type) const noexcept;

    /// Number of registered handlers.
    std::size_t handler_count() const noexcept;

    /// Dispatch an already-parsed envelope. The caller has already
    /// extracted `type` and `payload` from the JSON envelope.
    /// Returns the JSON response envelope. Always well-formed.
    std::string dispatch(std::string_view type,
                         const choc::value::ValueView& payload) const noexcept;

    /// Parse a JSON envelope and dispatch. Returns the same well-formed
    /// JSON response envelope as `dispatch`. Malformed JSON, missing
    /// `type`, or any handler exception all return a `{ok: false}`
    /// response with a descriptive error string.
    std::string dispatch_json(std::string_view json) const noexcept;

    /// Convenience: dispatch a WebViewPanel message that has already
    /// been split into `type` + `payload_json` by the platform layer.
    /// Used internally by attach_webview but also useful directly when
    /// a plugin wraps WebViewPanel itself.
    std::string dispatch_webview_message(std::string_view type,
                                         std::string_view payload_json) const noexcept;

    /// Hook this bridge up to a WebViewPanel's structured message
    /// channel. Equivalent to:
    ///   panel.set_message_handler([this](const WebViewMessage& m) {
    ///       return dispatch_webview_message(m.type, m.payload_json);
    ///   });
    void attach_webview(WebViewPanel& panel);

    /// Clear the message handler installed by attach_webview(). Safe
    /// to call even when no WebView handler is currently installed.
    /// Consumers that own both the bridge and panel can use this to
    /// make teardown order explicit before the panel or native child
    /// view is detached.
    void detach_webview(WebViewPanel& panel);

    /// Hook this bridge up to a native JS runtime's postMessage source.
    /// `handler_name` is the symbol the runtime exposes that calls back
    /// into C++ (the equivalent of `__pulpPostMessage` in WebView).
    ///
    /// NOTE: This is the integration seam for pulp #468 (Claude Design
    /// import lane). The full wiring lands when the import lane's
    /// JsRuntime postMessage shape is concrete; until then the body is
    /// a stub that records the binding so the call site doesn't have
    /// to feature-flag itself out.
    void attach_native_runtime(JsRuntime& runtime, std::string_view handler_name);

    // ── Static value-coercion helpers for handler authors ────────────
    //
    // ValueView's typed getters throw on type mismatch. These helpers
    // never throw; they return the default (or empty string) when the
    // key is absent or carries the wrong JSON type. Numeric helpers
    // accept any of int32 / int64 / float64 and coerce.

    static float       get_float (const choc::value::ValueView& v,
                                  const char* key,
                                  float dflt) noexcept;
    static std::size_t get_uint  (const choc::value::ValueView& v,
                                  const char* key,
                                  std::size_t dflt) noexcept;
    static std::string get_string(const choc::value::ValueView& v,
                                  const char* key) noexcept;

    // ── Static response builders ─────────────────────────────────────

    /// `{"ok":true}`.
    static std::string ok_response() noexcept;

    /// `{"ok":true, ...members of `extras` merged in}`. `extras` must
    /// be an object value or the call returns `ok_response()`.
    static std::string ok_response(const choc::value::ValueView& extras) noexcept;

    /// `{"ok":false,"error":"<msg>"}`.
    static std::string err_response(std::string_view msg) noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace pulp::view
