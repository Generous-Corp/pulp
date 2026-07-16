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

### Discrete parameters — the divisor comes from the parameter

`param_step_count(key)` reports how many distinct values the **host's** parameter
exposes. It counts values, not intervals: a 6-way selector returns 6, a toggle
returns 2, and a **continuous parameter or an unknown key returns 0**.

`0` means "this parameter has no index domain". It is never a denominator — do not
divide by it. Use the helpers, which encode that guard:

```cpp
const int steps = surface.param_step_count("lfo_waveform");   // 6
const double n  = pulp::view::param_index_to_normalized(2, steps);   // 2/5
const int idx   = pulp::view::param_normalized_to_index(n, steps);   // 2
```

The denominator is `steps - 1`, so index `0` maps to `0.0` and index `steps - 1`
maps to `1.0` — the same mapping `ParamRange::normalize()` produces for a discrete
range.

> #### ⚠️ Scale by the parameter's count, never by what the UI draws
>
> A control that renders **3** visible positions may be bound to a **6**-value
> parameter. Dividing by the number of things drawn — `idx / (options - 1)` —
> silently emits a wrong normalized value: index 2 of a 3-way radio becomes `1.0`
> and slams the host to the parameter's *last* value. `param_step_count()` is the
> only authority on a parameter's divisor.
>
> If a control's option count disagrees with its parameter's step count, that is a
> binding bug. `DesignFrameView` scales against the parameter and reports the
> disagreement — see [Choice controls scale themselves](#choice-controls-scale-themselves).

`param_step_count()` matches the cardinality Pulp's format adapters advertise
(`state::param_value_count`) — what the AU and AAX adapters pass through directly.
It is **not** VST3's `stepCount` field, which counts intervals and is therefore one
less; the VST3 adapter applies that `-1` at its own boundary. "Step count" is
overloaded across plug-in APIs — this accessor always means values.

Only an author-declared `ParamKind` (`Integer` / `Toggle` / `Enum`) or a
`value_labels` list makes a parameter discrete. A `ParamRange::step` that merely
quantizes a `Continuous` parameter still reports `0` — quantization is not
semantics, and adapters treat it the same way.

A host surface that does not implement `do_param_step_count()` reports `0`, so an
older surface degrades to "continuous" rather than to a wrong count.

### Choice controls scale themselves

A `DesignFrameView` choice control (dropdown / tab group / stepper / toggle) does
not need a hand-written divisor. When a `HostParamSurface` resolves the element's
`param_key` and reports a non-zero step count, the element normalizes against
**that** count — so a 3-position tab group bound to a 6-value parameter emits
`idx / 5`, and index 2 lands on the parameter's index 2.

With **no surface installed** or an **unknown key**, the element scales against its
own option count and stays quiet. Those are the paths where the element is not
bound to a parameter at all — a preview render, a screenshot, a control the host
never knew — so nothing has an opinion to override the control's positions.

A **step count of 0 on a resolved key is different, and it is a trap.** `0` is
ambiguous: the parameter may be genuinely continuous, **or the surface may simply
be unable to answer**. `do_param_step_count()` is non-pure and defaults to `0`, so
a surface whose parameter system has not wired the accessor reports `0` for *every*
key — discrete ones included.

For a continuous control that distinction is moot (no index domain either way).
For a **discrete** control it is not: `0` there is an unanswered question, not
evidence of a continuous parameter, and scaling by the count of things drawn is a
guess — the original bug wearing a fix's clothes. The options remain the only
domain available, so they are still used, but the guess is **reported** rather than
absorbed.

> If you implement a `HostParamSurface` over your own parameter system, override
> `do_param_step_count()`. Until you do, every discrete control bound to it scales
> against what the UI draws, and every one of them reports a mismatch saying so.

You do **not** pass a denominator. A caller supplying its own divisor is
re-introducing the exact defect this removes: the count belongs to the parameter,
not to the view.

```cpp
dfv.set_host_params(&surface);
dfv.route_changes_to_host_params(true);

dfv.commit_value("gain", 0.75f);          // normalized 0..1
dfv.commit_bipolar("pan", -0.5f);         // -1..1  ->  0.25 (0 is center)
dfv.commit_discrete("lfo_waveform", 2);   // index  ->  2/5, divisor from the host
```

Each helper brackets its write in one gesture (begin → change → end) so the host
groups it as a single undo step, and routes through the same funnel a pointer
gesture uses. They suit a **discrete** edit — a click, a typed value, a step. A
continuous drag should bracket **once** around the whole drag, so drive
`emit_gesture_begin` / `emit_element_changed` / `emit_gesture_end` directly for
that; otherwise the host sees one undo step per pixel moved.

`commit_discrete` against a parameter with no index domain (continuous, or a key
nothing resolves) has no divisor, so it **refuses to emit** rather than guess, and
reports instead.

### Mismatch reporting — never a silent mis-scale

`set_on_param_scale_mismatch()` reports a control whose visible option count
disagrees with its parameter's value count, de-duplicated per key and replayed to
a callback attached late. It **adds a signal only** — the view already scales
against the host either way.

Read the two counts, not the mere arrival of a report:

| `ui_option_count` | `host_step_count` | Means |
|---|---|---|
| 3 | 6 | a mis-scaled control — the design draws 3 of 6 reachable values |
| 3 | 0 | unanswered — the view scaled by what it draws. Either a genuinely continuous parameter, or a surface that cannot answer (see above) |
| 0 | 6 | an unbound key — a commit to a key no element carries |
| 0 | 0 | a key nothing knows |

`param_scale_mismatches()` returns the same list without a callback, which suits a
`--validate` style assertion over a ported control table.

### The bind grid — one element per host parameter

`build_bind_grid(keys)` appends an invisible, zero-hit stand-in element for every
key the active frame draws no control for. Every parameter then has an element, so
both directions work with no per-parameter plumbing: `sync_from_host_params()`
pulls each key at tick (automation and preset recall land for free), and the
`commit_*` helpers resolve any key.

A stand-in draws nothing, never hit-tests, and costs one struct plus a value copy
per tick — a full plug-in's worth of parameters is not a measurable cost. Keys
that already have a real control are skipped, so a design's own control always
wins. The grid is re-fitted on every frame swap: a key drawn on frame A but absent
on frame B gets a real control on A and a stand-in on B. Repeated calls replace
the grid rather than accumulating.

The keys are **caller-supplied** because `HostParamSurface` deliberately exposes
no parameter *enumeration* — it answers questions about a key you already hold
(`has_param` / `get_param` / `param_step_count`). A host reaching this surface has
that list on its own side; passing it in is the honest wiring rather than a guess.
