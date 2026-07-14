# EditorBridge Reference

`pulp::view::EditorBridge` is the renderer-agnostic JSON message
dispatcher that sits between a plugin editor (WebView panel today,
native JS runtime import lane tomorrow) and the C++ processor. Each plugin registers
its own per-message handlers; the framework owns the envelope parse,
the type→handler dispatch, the response builders, and a standard
error vocabulary.

The bridge keeps editor message dispatch shared across plugins so each editor
does not need to reinvent envelope parsing, handler routing, and error response
formatting.

- Header: `core/view/include/pulp/view/editor_bridge.hpp`
- Implementation: `core/view/src/editor_bridge.cpp`
- Tests: `test/test_editor_bridge.cpp`

## Envelope

Inbound JSON envelopes:

```json
{ "type": "<kind>", "payload": { ... } }
```

`payload` is optional. When omitted, handlers receive an empty
`choc::value::Value` object so they don't have to special-case the
absence of fields.

Responses are always one of:

```json
{ "ok": true }
{ "ok": true, "<extra>": "..." }
{ "ok": false, "error": "..." }
```

## Standard error vocabulary

`dispatch_json(...)` and `dispatch(...)` are `noexcept` and always
emit a well-formed response envelope. Envelope-level failures fall
into one of five categories. The on-the-wire `error` strings are
substring-compatible with existing plugin-editor tests so framework-level
dispatch can be adopted without changing plugin error assertions:

| Category         | Trigger                                                 | On-the-wire substring             |
|------------------|---------------------------------------------------------|-----------------------------------|
| `malformed_json` | JSON parse failed, or root is not an object             | `"malformed JSON"` / `"envelope must be an object"` |
| `missing_field`  | Envelope has no `type`, or `type` is non-string / empty | `"envelope missing 'type'"`       |
| `unknown_type`   | No handler registered for the given `type`              | `"unknown message type"`          |
| `wrong_type`     | Handler-emitted via `err_response("...")` for invalid payload values | (handler chooses)        |
| `internal_error` | Handler threw an exception                              | `"internal error"`                |

Plugin-level handlers may use `err_response(...)` with any message;
the framework reserves the substrings above only for envelope-level
failures it emits itself.

## API

```cpp
namespace pulp::view {

class EditorBridge {
public:
    using Handler = std::function<std::string(const choc::value::ValueView& payload)>;

    EditorBridge();
    ~EditorBridge();

    // Non-copyable AND non-movable. attach_webview / attach_native_runtime
    // install callbacks that reference this bridge instance, so moving an
    // attached bridge would dangle them. Construct in-place; static_asserts
    // in the test suite lock this in.
    EditorBridge(const EditorBridge&)            = delete;
    EditorBridge& operator=(const EditorBridge&) = delete;
    EditorBridge(EditorBridge&&)                 = delete;
    EditorBridge& operator=(EditorBridge&&)      = delete;

    // Registration.
    void          add_handler   (std::string_view type, Handler fn);
    void          remove_handler(std::string_view type);
    bool          has_handler   (std::string_view type) const noexcept;
    std::size_t   handler_count () const noexcept;

    // Dispatch.
    std::string dispatch        (std::string_view type,
                                 const choc::value::ValueView& payload) const noexcept;
    std::string dispatch_json   (std::string_view json) const noexcept;
    std::string dispatch_webview_message(std::string_view type,
                                         std::string_view payload_json) const noexcept;

    // Renderer attach helpers.
    void attach_webview        (WebViewPanel& panel);
    void detach_webview        (WebViewPanel& panel);
    void attach_native_runtime (JsRuntime& runtime, std::string_view handler_name);

    // Static value-coercion helpers (never throw).
    static float       get_float (const choc::value::ValueView&, const char* key, float dflt) noexcept;
    static std::size_t get_uint  (const choc::value::ValueView&, const char* key, std::size_t dflt) noexcept;
    static std::string get_string(const choc::value::ValueView&, const char* key) noexcept;

    // Static response builders.
    static std::string ok_response () noexcept;
    static std::string ok_response (const choc::value::ValueView& extras) noexcept;
    static std::string err_response(std::string_view msg) noexcept;
};

} // namespace pulp::view
```

### `attach_webview`

Routes a `WebViewPanel`'s structured message channel through this
bridge. Equivalent to:

```cpp
panel.set_message_handler([this](const WebViewMessage& m) {
    return dispatch_webview_message(m.type, m.payload_json);
});
```

`dispatch_webview_message` treats a `payload_json` of `"null"` (the
WebView default for "no payload") as an empty object so handlers see
the same shape regardless of whether the JS side passed a payload.

### `detach_webview`

Clears the message handler installed by `attach_webview`. Call this
before tearing down a panel or detaching its native child view when the
bridge and panel are owned side-by-side and you want explicit teardown
ordering:

```cpp
bridge.detach_webview(panel);
```

Calling `detach_webview` before an attach is safe; it is a no-op from
the caller's perspective.

### `attach_native_runtime`

Stub interface for the Claude Design import lane. The full wiring lands when
`JsRuntime` exposes a `postMessage`-equivalent primitive that calls back into
C++. Defining the interface here keeps native-runtime editors on the same
dispatch model as WebView editors.

## Usage example

```cpp
#include <pulp/view/editor_bridge.hpp>

class MyEditor {
public:
    void wire(pulp::view::WebViewPanel& panel) {
        bridge_.add_handler("set_value", [this](const auto& payload) {
            const auto v = pulp::view::EditorBridge::get_float(payload, "value", 0.0f);
            apply_to_processor(std::clamp(v, 0.0f, 1.0f));
            return pulp::view::EditorBridge::ok_response();
        });

        bridge_.add_handler("save_preset", [this](const auto&) {
            const auto preset_json = serialize_state();
            auto extras = choc::value::createObject("");
            extras.addMember("preset_json", preset_json);
            return pulp::view::EditorBridge::ok_response(extras);
        });

        bridge_.attach_webview(panel);
    }
private:
    pulp::view::EditorBridge bridge_;
};
```

The matching JS side (when running inside a Pulp WebView):

```javascript
const resp = await __pulpPostMessage({ type: "set_value",
                                       payload: { value: 0.42 } });
console.log(resp);   // {"ok":true}
```

## Non-goals (v1)

- No specific message types — every plugin owns its own schema.
- No drag-state helpers (`std::optional<DragSnapshot>`-style). Capture
  per-session state on `[this]` in the handler closure instead. A
  `DragBridge` add-on may follow if the pattern becomes ubiquitous.
- No C++ → JS push direction. That's a separate seam
  (`panel_->execute_script()` for WebView; runtime-specific for native
  JS) and deserves its own design pass.

## Related

- `view-bridge` skill — editor lifecycle (`create_view`,
  `open → notify_attached → resize → close`)
- `import-design` skill — Claude Design imports + the CLI bridge-handler
  scaffold (`pulp import-design --from claude --file <path>`)
- `core/format/include/pulp/format/view_bridge.hpp` — the lifecycle
  bridge that wraps `Processor::create_view()`


## Host parameters — `HostParamSurface`

A view can bind directly to a framework-agnostic parameter surface
(`View::host_params()`), which hides *which* parameter system is underneath — an
embedding framework's parameter tree, or Pulp's own `StateStore` — so one view runs
unchanged in either.

```cpp
view.set_host_params(&surface);
view.route_changes_to_host_params(true);   // OFF by default
```

With routing on, a user gesture on a key-tagged control drives the surface directly
(`begin_gesture` / `set_param` / `end_gesture`), and `sync_from_host_params()` pulls
current values and display text back at tick.

> ### ⚠️ Pick exactly ONE path — wiring both double-writes
>
> There are two ways a control's value can reach the host, and they are **not**
> alternatives you can safely have both of:
>
> - **The binder:** you wire `on_element_changed` and forward it into the parameter
>   store yourself.
> - **The surface:** the view drives it for you (above).
>
> `on_element_changed` **keeps firing when routing is on.** That is free for a
> consumer that merely *observes* it, and a **double write** for one that *writes*
> from it. Enable routing without deleting the write side of your handler and the
> host receives every value — **and every gesture bracket** — twice: a doubled
> automation write and an unbalanced begin/end pair.
>
> Routing is **off by default**, and that default is what keeps every existing embed
> correct. Moving to the surface? Turn routing on **and** drop the write side of
> `on_element_changed`, keeping it only for observation.
