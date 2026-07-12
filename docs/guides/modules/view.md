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

`ScriptedUiSession` owns the usual JS UI reload path: pass
`enable_hot_reload = true`, call `poll()` from the host idle tick, and it will
rebuild the widget bridge while preserving widget values. The lower-level
`HotReloader` only watches files and delivers changed JS to a callback:

```cpp
HotReloader reloader("ui/main.js", [&](const std::string& code) {
    // Probe or rebuild the widget tree with the new code, then repaint.
});

// From the UI thread / host idle tick:
reloader.poll_reload();
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

`AccessRole` covers the widget kinds Pulp ships: `slider`, `toggle` (switch),
`checkbox`, `radio`, `button`, `link`, `text_field`, `combo_box`, `list` /
`list_item`, `table` / `row` / `cell`, `tab` / `tab_list`, `menu` /
`menu_item`, `progress_bar`, `meter`, `dialog`, `heading`, `image`,
`scroll_bar`, `label`, `group`. Built-in widgets set the right one in their
constructor, so a `TextButton` announces as a button and a `ComboBox` as a
combo box without any extra code. `AccessRole::none` removes the view from the
accessibility tree entirely.

### A role is not enough — name it, or it is not exposed

`pulp::view::is_accessibility_element()` (in `pulp/view/accessibility.hpp`) is
the single gate every platform bridge calls. A view enters the tree only when
it has a role AND something to say with it:

- a **structural role** (`group`, `list`, `table`, `row`, `menu`, `tab_list`,
  `dialog`) — it announces the children underneath it; or
- an **accessible name** — either author-set (`set_access_label`, what the JS
  bridge's `aria-label` maps to) or content-derived (a `Label`'s text, a
  `Knob`/`Fader`/`Toggle`/`TextButton`'s visible label, which land on
  `set_derived_access_label`). An author-set name WINS over content, per ARIA's
  accessible-name computation — so `set_access_label("Gain in decibels")` is not
  clobbered when the label's text is later set to `"dB"` (and React re-renders
  rewrite that text on every render); or
- a **value source** — `AccessibilityValueInterface` (`Knob`, `Fader`,
  `RangeSlider`, `ScrollBar`, `Meter`, `ProgressBar`),
  `AccessibilityTextInterface` (`TextEditor`), or a non-empty
  `access_value` (`ComboBox` publishes its selected item); or
- an **ARIA state** — `aria-checked` / `aria-pressed` (`Checkbox`, `Toggle`,
  `ToggleButton` keep theirs in sync automatically).

### One resolver, one interface set

`accessibility_value_string()` is what every bridge announces as the VALUE:
value interface → text interface → `access_value` → check/press state
("checked" / "unchecked" / "mixed"). The cross-platform snapshot
(`snapshot_accessibility_tree`) resolves through it too, so an offline test sees
the same string VoiceOver does.

`accessibility_interfaces()` says which interfaces a view can actually serve —
a numeric range (`value`) or string content (`text`). Linux AT-SPI exports
exactly those: `org.a11y.atspi.Value` for a slider/meter, `org.a11y.atspi.Text`
for a `TextEditor`'s content or a `ComboBox`'s selected item (AT-SPI has no
string-value interface, so a string value is read through Text). Check/press
state is not a value on AT-SPI — it rides in the state bitfield
(CHECKABLE / CHECKED / PRESSED / INDETERMINATE).

A role with none of those announces "button" (or "text field", or "image") and
then falls silent — a WCAG 4.1.2 (Name, Role, Value) failure and pure noise in
the tree. So `ArrowButton`, `ShapeButton` and `ImageButton` carry
`AccessRole::button` but stay OUT of the tree until you give them a name:

```cpp
auto play = std::make_unique<ImageButton>();
play->set_image("play.png");
play->set_access_label("Play");   // ← without this it is not announced at all
```

No name is invented for you: "Down" for an arrow or "star.png" for an icon
would be a plausible-sounding lie about what the control does.

Each role maps to a concrete platform role on all five bridges: macOS
NSAccessibility (`platform/ns_role_mapping.hpp`, shared by the standalone
window host and the plug-in editor host), iOS UIAccessibility traits, Windows
UIA control types (`platform/uia_mapping.hpp`), Linux AT-SPI2 role numbers
(`platform/atspi_mapping.hpp`), and Android TalkBack class names. JS UIs get
the same roles by setting the ARIA `role` attribute; the token table is
`pulp/view/aria_roles.hpp`.

**Known limits (roles are correct; interaction patterns are not all wired):**

- Composite widgets that PAINT their rows (`ListBox`, `TableListBox`,
  `TabPanel`) carry NO role. They hold no child Views for their rows and tabs,
  so a `list` / `table` / `tab_list` role would export an empty container
  ("list, 0 items") and a tab group whose children are content panels. The
  container role lands together with the per-row `list_item` / `row` / `cell`
  and per-tab `tab` elements. `VirtualList` does have child Views and does
  expose `list` + `list_item` today.
- The UIA provider implements exactly two patterns: **Value** and
  **RangeValue** (`IValueProvider`, `IRangeValueProvider`). There is no
  `IInvokeProvider`, `IToggleProvider` or `ITextProvider`, so Invoke, Toggle
  and Text are NOT advertised — a button, checkbox or label exposes its control
  type and no pattern. Likewise ExpandCollapse (combo box), Grid/Table and
  SelectionItem (list item, tab, radio) are unimplemented and unadvertised.
  Pattern availability is also gated on the concrete View having a source
  (`uia::exposes_value` / `exposes_range_value`), so a control never advertises
  a pattern whose getter would return a null BSTR or a degenerate 0..0 range.
- On iOS, `text_field` carries no trait: UIKit derives "text field" from
  `UITextInput` conformance, which Pulp's accessibility elements do not have.
- **Windows announces no check/press state.** UIA carries toggle state only on
  the Toggle pattern, and there is no `IToggleProvider` — so Narrator reads a
  Pulp checkbox's control type and name and says nothing about whether it is
  checked. macOS (`accessibilityValue` → `@YES`/`@NO`/`"mixed"`), iOS and
  Android (the "checked" / "unchecked" value string) and Linux (the AT-SPI
  CHECKED / PRESSED / INDETERMINATE state bits) all do announce it.
- **Linux text navigation is partial.** `org.a11y.atspi.Text` serves `GetText`,
  `GetCharacterAtOffset`, the caret and the selection, plus `CharacterCount` —
  enough for Orca to read a field's content. Word/line granularity
  (`GetTextAtOffset` / `GetStringAtOffset`) and text attributes are not
  implemented, and there is no `EditableText` interface, so an AT cannot type
  into a Pulp field on Linux.
- **Android: the accessibility surface is INERT.**
  `PulpAccessibilityDelegate` (`android/app/src/main/java/com/pulp/
  accessibility/PulpAccessibility.kt`) has no `getAccessibilityNodeProvider`
  override, so TalkBack sees ONE node — the host `SurfaceView` — and
  `onInitializeAccessibilityNodeInfo` overwrites that single node's
  className/contentDescription once per widget, leaving it carrying the LAST
  widget's role. The C++ → Kotlin role mapping is correct but currently
  unreachable: no Pulp widget is individually announced on Android. Fixing it
  means implementing `AccessibilityNodeProvider` with one virtual view id per
  accessible node. The Kotlin role table is written and its ordinals are locked
  against the C++ enum by a `static_assert`, but no CI lane compiles Kotlin, so
  that file is UNCOMPILED as well as unreachable.

## Inspector

The ViewInspector serializes the view tree to JSON for debugging:

```cpp
ViewInspector inspector;
auto json = inspector.to_json(root);
auto* found = inspector.find_by_id(root, "gain-knob");
```

Used by the MCP server for AI-driven UI testing and the component inspector tool.
