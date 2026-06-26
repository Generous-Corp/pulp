# Component Showcase

A single JS file that demonstrates every built-in Pulp widget type. Use as a visual regression baseline and widget reference.

**Source**: `examples/showcase/showcase.js`

## What It Shows

| Section | Widgets |
|---------|---------|
| Knobs | 4 `Knob` widgets at different values |
| Faders | 4 vertical `Fader` + 1 horizontal `Fader` |
| Toggles & Checkboxes | `Toggle`, `Checkbox`, `ToggleButton` in on/off states |
| Labels & Typography | Font size, weight, italic, uppercase, letter spacing |
| Meters | 6 vertical `Meter` widgets at different levels |
| Spectrum & Waveform | `SpectrumView` with sample curve, `WaveformView` with sine |
| XY Pad | `XYPad` with initial position |
| Combo & Text Input | `Combo` dropdown, `TextEditor` with placeholder |
| Progress | 4 `Progress` bars at 25%, 50%, 75%, 100% |
| Scroll View | `ScrollView` with 20 alternating-color items |
| Canvas | `CanvasWidget` with rectangles, circles, paths, text |
| Panels & Styling | Plain, shadowed, and glass-effect `Panel` variants |
| Icons | All 4 icon types: image_upload, send, search, close |

## Running

Load the showcase in the UI preview app:

```bash
pulp build --target pulp-ui-preview
./build/examples/ui-preview/pulp-ui-preview --script examples/showcase/showcase.js
```

## Screenshot Testing

Capture a headless screenshot for visual regression:

```bash
./build/examples/ui-preview/pulp-ui-preview --script examples/showcase/showcase.js --screenshot /tmp/pulp-showcase.png
# Output: /tmp/pulp-showcase.png
```

Compare against a stored baseline to detect visual regressions in widget rendering.
