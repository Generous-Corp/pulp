# UI Preview

**Category**: experimental
**Type**: Standalone application (not a plugin)
**Path**: `examples/ui-preview/`

## Summary

A standalone application for testing the view/widget system and GPU rendering pipeline without building a full plugin. It creates a window with knobs, a toggle, a fader, and a label, all built from JavaScript via the `WidgetBridge` scripting layer.

## What It Demonstrates

- The full rendering pipeline: JS script -> WidgetBridge -> View tree -> layout -> paint -> CoreGraphics -> screen
- `ScriptEngine` (QuickJS) integration for building UIs from JavaScript
- `WidgetBridge` binding JavaScript widget creation functions to the View hierarchy
- `StateStore` parameter setup outside of a plugin context
- `WindowHost` for native window creation and event loop
- Dark theme application via `Theme::dark()`
- Flexbox layout configuration (`FlexDirection::column`, padding, gap)
- Widget types: `createLabel`, `createKnob`, `createToggle`, `createFader`
- Setting widget values from JavaScript via `setValue()`

## Supported Formats

This is not a plugin. It builds as a standalone executable only.

## Supported Platforms

| Platform | Supported |
|----------|-----------|
| macOS | Yes |
| Windows | No (guarded by `if(APPLE)` in CMakeLists.txt) |
| Linux | No (guarded by `if(APPLE)` in CMakeLists.txt) |

## Key Files

| File | Purpose |
|------|---------|
| `main.cpp` | Application entry point: creates StateStore, View tree, ScriptEngine, WidgetBridge, and WindowHost |
| `CMakeLists.txt` | Build configuration; links `pulp::view` and `pulp::state` |

## Known Limitations

- macOS only. The CMakeLists.txt wraps the entire target in an `if(APPLE)` guard.
- Links `pulp::view` and `pulp::state` but does not link any audio or format libraries. This is intentional -- it validates the UI stack in isolation.
- No test file. Validation is visual (launch the app and inspect the window).
- The rendering backend depends on CoreGraphics. The Dawn/Skia GPU path is not exercised by this example in its current form.

## Related Examples

- [PulpGain](example-pulp-gain.html) -- the parameters used in this preview (Gain, Mix, Bypass) mirror PulpGain's parameter set
