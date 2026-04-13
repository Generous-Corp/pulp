---
name: view-bridge
description: Editor lifecycle and multi-view attach for Pulp plugins — when to override Processor::create_view(), the open → notify_attached → resize → close protocol, release_view() ownership rules, and secondary-view roles.
---

# ViewBridge skill

**TL;DR.** Every Pulp plugin format adapter (VST3, AU v2, AU v3, CLAP,
Standalone) opens its editor through
`pulp::format::ViewBridge`. You only touch the bridge when you override
`Processor::create_view()` or write a new adapter. This skill captures
the invariants the API enforces and the pitfalls that bit us enough
times to be worth remembering.

## When to use ViewBridge

| Task | Touch ViewBridge? |
|---|---|
| Add a knob to the auto-generated editor | No — just `define_parameters` |
| Return a hand-built `view::View` tree from a processor | Yes — override `Processor::create_view()` |
| React when the host actually shows / resizes / closes the editor | Yes — override `on_view_opened / on_view_closed / on_view_resized` |
| Add a new format adapter | Yes — construct a `ViewBridge` and follow the lifecycle protocol |
| Hand the built view to an external container (TabPanel, WindowHost) | Yes — call `ViewBridge::release_view()` |
| Ship a paint-only overlay (inspector) | No — but `attach_secondary_view(…, ViewRole::Inspector)` is the primitive you'll eventually use |

## Lifecycle protocol — adapter author side

```
bridge.open(&err)                 // builds the view; does NOT fire on_view_opened
  |
  +-- host.attach_to_parent(window)  // platform-specific
  |
bridge.notify_attached()          // fires Processor::on_view_opened
  |
bridge.resize(w, h)               // on each host resize; fires on_view_resized
  |
bridge.close()                    // fires Processor::on_view_closed iff attached
```

**Do not** fire `on_view_opened` before the host has actually attached
the view to a native parent. That was PR #140's Codex P2 finding — if
attach fails after `open()`, a naive implementation would fire
`on_view_opened` then destroy without a matching `on_view_closed` and
the plugin would leak host-window-dependent resources. `ViewBridge`
guarantees balance **only when the adapter honors the two-step
protocol** (`open()` then `notify_attached()`).

### Per-format attach point

| Format | `open()` is called here | `notify_attached()` is called here |
|---|---|---|
| VST3 | `CPluginView::attached()` before `attach_to_parent` | After `CPluginView::attached` returns `kResultTrue` |
| CLAP | `gui_create` | Inside `gui_set_parent` on the matched window API |
| AU v2 | `uiViewForAudioUnit` — after fetching context via `kPulpEditorContextProperty` | After `PluginViewHost::create` succeeds |
| AU v3 | `viewDidLoad` | After `PluginViewHost::attach_to_parent` in `viewDidLoad` |
| Standalone | Before `WindowHost::create` | After `WindowHost::create` succeeds |

## `release_view()` — for containers that own the view

`TabPanel::add_tab` and similar widgets take `std::unique_ptr<view::View>`.
Call `bridge.release_view()` to hand ownership over. The bridge keeps a
raw pointer so `notify_attached`, `resize`, and `close` continue to
dispatch `Processor::on_view_*` on the same view instance.

**Contract:** the caller must keep the released view alive until
`bridge.close()` runs (or the bridge is destroyed). The standalone
adapter handles this by calling `bridge.close()` explicitly after
`run_event_loop()` returns, before the `TabPanel` falls out of scope.

## AU v2 dual-Processor gotcha (fixed)

Pre-ViewBridge, the AU v2 Cocoa view factory called
`format::registered_factory()` and built a **second** `Processor` +
`StateStore` for the UI, syncing parameters one-shot at construction.
Any parameter change on the audio thread drifted from the UI.

Post-fix: `au_v2_adapter.{hpp,cpp}` exposes the host `Processor*` +
`StateStore*` via a private AU property `kPulpEditorContextProperty`
(`'PuEd'`). The view factory fetches it with `AudioUnitGetProperty`
and drives a `ViewBridge` against the host's single Processor.

AU v3 does the same thing via the ObjC accessors
`-[PulpAudioUnit pulpProcessor]` and `-[PulpAudioUnit pulpStore]`.

**If you add another AU-like adapter, expose the same pair.** Never
spin up a second Processor for the UI.

## Secondary views

```cpp
view::View* ViewBridge::attach_secondary_view(std::unique_ptr<view::View>, ViewRole);
bool        ViewBridge::detach_secondary_view(view::View*);
size_t      ViewBridge::view_count()                const;
view::View* ViewBridge::view_at(size_t index);
ViewRole    ViewBridge::role_at(size_t index)       const;  // Editor, Inspector, Remote
```

Roles are opaque — the bridge only uses them for introspection.
Parameter bindings propagate automatically because every attached view
polls the same `StateStore`; there is no explicit broadcast step.

Phase 4's `attach_remote_view(url)` (WebSocket-backed) will land as a
`ViewRole::Remote` secondary view.

## Common pitfalls

1. **Forgetting `notify_attached()` after a successful attach.** The
   host will show the view but `Processor::on_view_opened` never fires,
   so any lifecycle-tied setup (listener registration, meter startup)
   silently skips.
2. **Calling `close()` after destroying the released view.** Lifecycle
   dispatch then runs on a dangling pointer. Always close the bridge
   first, then let the caller-owned view destruct.
3. **Creating a second Processor in the editor path.** Fetch the host
   Processor via the format-specific accessor (AU v2 property, AU v3
   ObjC selector, CLAP / VST3 constructor param).
4. **Hard-coding `editor_size()` when you actually want resize
   bounds.** Override `view_size()` and return a `ViewSize` with
   real min/max; hosts use those to constrain user-drag resize.

## Tests

`test/test_view_bridge.cpp` is the canonical reference:
- `"ViewBridge falls back to AutoUi when create_view returns nullptr"`
- `"ViewBridge honors custom create_view()"`
- `"ViewBridge supports secondary views"`
- `"ViewBridge defers on_view_opened until notify_attached"`
- `"ViewBridge close without attach does not fire on_view_closed"`
- `"ViewBridge destructor closes view"`
- `"ViewBridge cross-format lifecycle invariants"` (VST3 / CLAP / AU v2 /
  AU v3 / Standalone / failed-attach replay)

7 cases, 67 assertions. Run with
`ctest --test-dir build -R ViewBridge --output-on-failure`.

## References

- `core/format/include/pulp/format/view_bridge.hpp` — public API
- `core/format/src/view_bridge.cpp` — implementation
- `core/format/include/pulp/format/processor.hpp` — `create_view`,
  `view_size`, `on_view_*`
- `docs/guides/view-bridge.md` — user-facing guide
- `examples/view-bridge-demo/main.cpp` — runnable headless demo
- `planning/next-features-plan.md` § Feature 1 — phase tracking
