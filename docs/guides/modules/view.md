# View Module

The view module provides the widget system, layout engine, theming, JS scripting, hot-reload, and the audio-to-UI bridge. It is the primary UI framework for Pulp plugins.

**Status**: experimental
**Dependencies**: canvas, events, state
**Headers**: `pulp/view/view.hpp`, `pulp/view/widgets.hpp`, `pulp/view/theme.hpp`, `pulp/view/script_engine.hpp`, and more

## View Hierarchy

Views form a tree. Each view has zero or more children and one optional parent. The root view represents the plugin editor window.

```cpp
auto root = std::make_unique<View>();
root->set_bounds({0, 0, 600, 400});

auto knob = std::make_unique<Knob>();
knob->set_id("gain-knob");
knob->flex().width = 60;
knob->flex().height = 60;
root->add_child(std::move(knob));
```

### Layout

Views use a flex layout system. Set flex properties on each view to control positioning:

```cpp
view.flex().direction = FlexDirection::row;   // or column
view.flex().justify = FlexJustify::center;
view.flex().align = FlexAlign::center;
view.flex().width = 200;
view.flex().height = 40;
view.flex().padding = 8;
view.flex().gap = 4;
```

Call `layout_children()` on the root to compute bounds for all children.

### Painting

Views paint to a Canvas. Override `paint()` in custom views:

```cpp
class MyPanel : public View {
    void paint(Canvas& canvas) override {
        auto bg = resolve_color("surface", Color::hex(0x1a1a2e));
        canvas.set_fill_color(bg);
        canvas.fill_rounded_rect(0, 0, bounds().width, bounds().height, 8);
    }
};
```

Call `paint_all(canvas)` on the root to paint the entire tree.

## Widgets

Built-in widgets for audio plugin UIs:

| Widget | Purpose | Access Role |
|--------|---------|-------------|
| `Knob` | Rotary control for continuous parameters | slider |
| `Fader` | Linear slider for continuous parameters | slider |
| `Toggle` | On/off switch for boolean parameters | toggle |
| `Label` | Text display | label |
| `Meter` | Level meter with peak hold | meter |
| `XYPad` | 2D parameter control | slider |
| `WaveformView` | Audio waveform display | image |
| `SpectrumView` | Frequency spectrum display | image |

```cpp
auto knob = std::make_unique<Knob>();
knob->set_value(0.5f);
knob->set_label("Gain");
knob->on_change = [&](float v) { gain_binding.set_normalized(v); };
```

## Theming

Themes are structured data (design tokens), not code. Each view can have a theme; color resolution walks up the parent chain.

```cpp
Theme dark_theme;
dark_theme.colors["background"] = Color::hex(0x1a1a2e);
dark_theme.colors["surface"] = Color::hex(0x16213e);
dark_theme.colors["accent"] = Color::hex(0xe94560);
dark_theme.dimensions["knob_size"] = 60.0f;

root->set_theme(dark_theme);

// Any child can resolve:
Color bg = view.resolve_color("background");
```

Themes can be loaded from JSON files and hot-reloaded.

## JS Scripting

The ScriptEngine (QuickJS via CHOC) lets you define UIs in JavaScript:

```cpp
ScriptEngine engine;
engine.evaluate(R"(
    const knob = createKnob("gain", 0.5);
    knob.setLabel("Gain");
    knob.setPosition(20, 20, 60, 60);
)");
```

### Hot-Reload

The HotReloader watches JS files and re-evaluates them when they change:

```cpp
HotReloader reloader(engine, "ui/");
// Edit ui/main.js → changes appear instantly
```

## Audio Bridge

Lock-free meter data transfer from the audio thread to the UI:

```cpp
AudioBridge bridge;

// Audio thread:
bridge.push_meter({peak_l, peak_r, rms_l, rms_r});

// UI thread:
if (bridge.poll_meter()) {
    auto data = bridge.meter_data();
    meter.set_level(data.peak_left, data.peak_right);
}
```

Uses TripleBuffer internally — no allocation, no blocking, latest-value semantics.

## Synthetic Events (Testing)

Simulate user interaction without a window:

```cpp
root.simulate_click({30, 30});           // Click at (30, 30)
root.simulate_drag({30, 30}, {30, 80});  // Drag from top to bottom

// Keyboard focus traversal
View::focus_next(root, current_focus);
View::focus_prev(root, current_focus);
```

## Accessibility

Every widget has an access role, label, and value for screen readers:

```cpp
knob.set_access_role(View::AccessRole::slider);
knob.set_access_label("Gain");
knob.set_access_value("-6.0 dB");
```

On macOS, `PulpAccessibilityElement` maps these to `NSAccessibilityRole` for VoiceOver support.

## Inspector

The ViewInspector serializes the view tree to JSON for debugging:

```cpp
ViewInspector inspector;
auto json = inspector.to_json(root);
auto* found = inspector.find_by_id(root, "gain-knob");
```

Used by the MCP server for AI-driven UI testing and the component inspector tool.
