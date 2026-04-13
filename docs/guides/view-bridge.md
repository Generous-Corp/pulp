# ViewBridge

ViewBridge is the editor-lifecycle layer that sits between your `Processor`
and every plugin format (VST3, AU, CLAP, standalone, AAX). It owns the
constructed view tree, dispatches open / close / resize callbacks, and lets
one processor serve multiple simultaneous views (editor + inspector + remote).

## Why it exists

Before ViewBridge each format adapter called `build_editor_ui(store, …)`
directly. That meant:

- No `create_view()` hook on `Processor` — you could not return a custom
  view tree without forking the adapter.
- No open / close / resize callbacks — plugins could not react when the
  host actually showed or resized the editor.
- AU v2 on macOS instantiated a *second* `Processor` for the Cocoa view,
  which silently desynchronized parameter state.
- CLAP had no editor wiring at all.

ViewBridge fixes all of those by giving every adapter a single, uniform
path to construct and tear down views.

## The API

```cpp
class Processor {
  // ...existing methods...

  /// Size hints for this editor. Default derives from editor_size().
  virtual ViewSize view_size() const;

  /// Return a custom view tree. Default returns nullptr, which tells
  /// ViewBridge to fall back to the scripted UI (if configured) or AutoUi.
  virtual std::unique_ptr<view::View> create_view();

  virtual void on_view_opened(view::View&);
  virtual void on_view_closed(view::View&);
  virtual void on_view_resized(view::View&, uint32_t w, uint32_t h);
};
```

`ViewSize` carries preferred / min / max dimensions in logical pixels. A
zero max means unbounded in that axis.

```cpp
class ViewBridge {
public:
  struct Options {
    bool enable_hot_reload = false;  // Poll scripted UI + theme.json for changes
    ViewRole role = ViewRole::Editor;
  };

  ViewBridge(Processor&, state::StateStore&, Options = {});

  bool open(std::string* error = nullptr);  // Build view (does NOT fire on_view_opened yet)
  void notify_attached();                   // Fire on_view_opened AFTER host attach succeeds
  void close();                             // Fire on_view_closed (only if attached), destroy view
  void resize(uint32_t w, uint32_t h);      // Fire on_view_resized (only if attached)

  bool is_open() const;
  view::View* view();
  const ViewSize& size_hints() const;
  uint32_t width() const;
  uint32_t height() const;

  // Secondary views (inspector, remote preview, …)
  view::View* attach_secondary_view(std::unique_ptr<view::View>, ViewRole);
  bool        detach_secondary_view(view::View*);
  size_t      view_count() const;
  view::View* view_at(size_t);
  ViewRole    role_at(size_t) const;
};
```

## Usage patterns

### Default AutoUi editor

Do nothing. `Processor::create_view()` returns `nullptr`, and ViewBridge
builds the AutoUi (or scripted UI, if one is configured via
`PULP_UI_SCRIPT_PATH`). Every plugin gets a working editor for free.

### Custom view tree

```cpp
std::unique_ptr<view::View> MyPlugin::create_view() {
    auto root = std::make_unique<view::View>();
    root->set_theme(view::Theme::dark());
    // … build custom widget hierarchy …
    return root;
}
```

### Reacting to lifecycle

```cpp
void MyPlugin::on_view_opened(view::View& v) {
    // Register parameter listeners, start meters, etc.
}

void MyPlugin::on_view_resized(view::View&, uint32_t w, uint32_t h) {
    // Recompute layout constants, update cached metrics, etc.
}

void MyPlugin::on_view_closed(view::View&) {
    // Tear down anything registered in on_view_opened.
}
```

### Multiple views for one processor

ViewBridge can host a primary editor plus secondary views (inspector,
remote preview). They all share the processor's `StateStore`, so parameter
binding keeps them in sync.

```cpp
format::ViewBridge bridge(processor, store);
bridge.open();

// Attach an inspector as a secondary view
auto inspector = make_inspector_view(processor);
bridge.attach_secondary_view(std::move(inspector), ViewRole::Inspector);
```

## Thread model

- All ViewBridge methods run on the UI / host thread.
- `open()` builds the view but does **not** fire `on_view_opened`. The
  adapter calls `notify_attached()` only after the host has successfully
  attached the view to its native parent window (`attach_to_parent`,
  CLAP `gui_set_parent`, VST3 `CPluginView::attached`). This guarantees
  `on_view_opened` fires only when host-window-dependent resources can
  actually be used, and keeps open/close dispatch balanced — if attach
  fails, `close()` tears down the view without a spurious `on_view_closed`.
- `close()` fires `on_view_closed` only when `notify_attached` had fired.
- `resize()` dispatches `on_view_resized` only when attached; pre-attach
  resizes just update the cached dimensions.
- Parameter propagation to all attached views happens through the
  existing `Binding::poll()` loop driven by each view's frame clock —
  ViewBridge itself does not touch the audio thread.

## Format adapter mapping

| Format     | Adapter code                              | Construction site             |
|------------|-------------------------------------------|-------------------------------|
| VST3       | `core/format/src/vst3_plug_view.cpp`      | `createView()`                |
| AU v2      | `core/format/src/au_v2_cocoa_view.mm`     | `CocoaView` init              |
| AU v3      | `core/format/src/au_adapter.mm`           | `AUAudioUnitViewController`   |
| CLAP       | `core/format/include/pulp/format/clap_entry.hpp` | `gui_create` / `gui_show` |
| Standalone | `core/format/src/standalone.cpp`          | `run()` / window open         |

Each adapter constructs one `ViewBridge` per editor window and forwards
host lifecycle events to `open()` / `close()` / `resize()`.

## Example

See `examples/view-bridge-demo/` for a runnable demo that:

1. Implements a `Processor` subclass with a custom `create_view()`.
2. Attaches a secondary inspector view via `attach_secondary_view()`.
3. Exercises lifecycle callbacks and a resize.

## Phase 4: remote views (deferred)

WebSocket-driven `attach_remote_view(url)` is planned but not yet
implemented. The local multi-view machinery in `ViewBridge` is the
foundation for that work.
