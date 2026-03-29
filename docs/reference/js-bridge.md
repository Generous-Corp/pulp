# JS Bridge API Reference

Complete reference for all JavaScript functions available in Pulp UI scripts. These functions are registered by the `WidgetBridge` and callable from any `.js` file loaded by the script engine.

**Status**: experimental

## Widget Creation

### Basic Controls

```js
createKnob(id, parentId)           // Rotary knob control
createFader(id, orientation, parentId) // Linear slider ("vertical" | "horizontal")
createToggle(id, parentId)         // On/off switch
createCheckbox(id, parentId)       // Checkbox control
createToggleButton(id, parentId)   // Stateful push button
createLabel(id, text, parentId)    // Text display
createIcon(id, type, parentId)     // Icon glyph ("image_upload" | "send" | "search" | "close")
```

### Data Visualization

```js
createMeter(id, orientation, parentId)  // Level meter with peak hold
createWaveform(id, parentId)            // Waveform display
createSpectrum(id, parentId)            // Frequency spectrum analyzer
createXYPad(id, parentId)              // 2D parameter control
createProgress(id, parentId)            // Progress bar
```

### Input

```js
createTextEditor(id, parentId)     // Text input field
createCombo(id, parentId)          // Dropdown selector
```

### Custom Drawing

```js
createCanvas(id, parentId)         // Canvas for 2D draw commands
```

### Layout Containers

```js
createRow(id, parentId)            // Flex row (horizontal)
createCol(id, parentId)            // Flex column (vertical)
createGrid(id, parentId)           // CSS Grid container
createPanel(id, parentId)          // Generic container with styling
createScrollView(id, parentId)     // Scrollable container
```

All creation functions return the widget `id` string. The `parentId` parameter specifies where in the view tree to insert the widget.

## Widget Values

```js
setValue(id, value)    // Set normalized value (0-1) on Knob, Fader, Toggle, Checkbox, ToggleButton
getValue(id)           // Get normalized value — returns number
setLabel(id, text)     // Set label text on Knob, Fader, Toggle, ToggleButton, Label
setText(id, text)      // Set text on TextEditor or Label
getText(id)            // Get text from TextEditor or Label — returns string
```

### Widget-Specific Data

```js
setItems(id, ["Option A", "Option B"])   // Set Combo dropdown items
setProgress(id, 0.75)                     // Set Progress bar value (0-1)
setMeterLevel(id, peak, rms)              // Set Meter peak and RMS levels
setXY(id, x, y)                           // Set XYPad position (0-1 each axis)
setWaveformData(id, [0.1, -0.3, ...])     // Set WaveformView sample data
setSpectrumData(id, [0.5, 0.8, ...])      // Set SpectrumView frequency bins
setPlaceholder(id, "Type here...")         // Set TextEditor placeholder text
setScrollContentSize(id, width, height)    // Set ScrollView content dimensions
setPanelStyle(id, bgToken, borderToken, radius, width) // Style Panel via tokens
```

## Parameter Store

```js
getParam(name)          // Read normalized parameter value by name — returns number
setParam(name, value)   // Write normalized parameter value by name
```

Parameters are defined in C++ (`define_parameters`) and shared with the audio thread via lock-free atomics. `setParam` triggers DAW host notification automatically.

## Flexbox Layout

```js
setFlex(id, property, value)
```

| Property | Type | Example | Description |
|----------|------|---------|-------------|
| `direction` | string | `"row"`, `"column"` | Flex direction |
| `gap` | number | `8` | Gap between children (px) |
| `row_gap` | number | `8` | Row gap for wrap layouts |
| `column_gap` | number | `12` | Column gap for wrap layouts |
| `padding_top` | number | `16` | Top padding (px) |
| `padding_right` | number | `16` | Right padding |
| `padding_bottom` | number | `16` | Bottom padding |
| `padding_left` | number | `16` | Left padding |
| `margin_top` | number | `8` | Top margin |
| `margin_right` | number | `8` | Right margin |
| `margin_bottom` | number | `8` | Bottom margin |
| `margin_left` | number | `8` | Left margin |
| `width` | number | `200` | Fixed width (px) |
| `height` | number | `40` | Fixed height (px) |
| `min_width` | number | `100` | Minimum width |
| `max_width` | number | `400` | Maximum width |
| `min_height` | number | `30` | Minimum height |
| `max_height` | number | `300` | Maximum height |
| `flex_grow` | number | `1` | Flex grow factor |
| `flex_shrink` | number | `0` | Flex shrink factor |
| `flex_basis` | number | `100` | Flex basis (px) |
| `order` | number | `2` | Layout order |
| `justify_content` | string | `"center"` | Main axis alignment |
| `align_items` | string | `"center"` | Cross axis alignment |
| `align_self` | string | `"stretch"` | Self alignment override |
| `justify_self` | string | `"start"` | Self justify override |

## Grid Layout

```js
setGrid(id, property, value)
```

| Property | Type | Example | Description |
|----------|------|---------|-------------|
| `template_columns` | string | `"1fr 2fr 1fr"` | Column track definitions |
| `template_rows` | string | `"auto 1fr"` | Row track definitions |
| `column_gap` | number | `8` | Column gap (px) |
| `row_gap` | number | `8` | Row gap (px) |
| `gap` | number | `8` | Both column and row gap |
| `column_start` | number | `1` | Grid column start line |
| `column_end` | number | `3` | Grid column end line |
| `row_start` | number | `1` | Grid row start line |
| `row_end` | number | `2` | Grid row end line |

## Typography

```js
setFontSize(id, 14)              // Font size in pixels
setFontWeight(id, 700)           // Font weight (100-900)
setFontStyle(id, "italic")       // "normal" | "italic"
setLetterSpacing(id, 0.5)        // Letter spacing (px)
setLineHeight(id, 1.5)           // Line height multiplier
setTextAlign(id, "center")       // "left" | "center" | "right"
setTextColor(id, "#ffffff")      // Text color (CSS Color L4)
setTextTransform(id, "uppercase") // "uppercase" | "lowercase" | "capitalize" | "none"
setTextDecoration(id, "underline") // "underline" | "line-through" | "overline" | "none"
setTextOverflow(id, "ellipsis")  // "ellipsis" | "clip"
setMultiLine(id, 1)              // Enable multi-line text (0 or 1)
```

## Visual Styling

```js
setBackground(id, "#1a1a2e")                          // Background color
setBackgroundGradient(id, "linear-gradient(to right, #ff0000, #0000ff)") // CSS gradient
setBorder(id, "#333333", 1, 8)                        // Border color, width, radius
setOpacity(id, 0.5)                                    // Opacity (0-1)
setBoxShadow(id, 0, 4, 8, 0, "rgba(0,0,0,0.3)")     // Shadow: offsetX, offsetY, blur, spread, color
setFilter(id, "blur(4px)")                             // CSS filter string
setOverflow(id, "hidden")                              // "visible" | "hidden"
setCursor(id, "pointer")                               // "pointer" | "crosshair" | "text" | "grab" | "default"
setVisible(id, true)                                   // Show/hide widget
setZIndex(id, 10)                                      // Stacking order
```

## Transforms

```js
setTranslate(id, x, y)          // Translate in pixels
setScale(id, sx, sy)            // Scale factor (1.0 = normal)
setRotation(id, degrees)        // Rotation in degrees
setTransformOrigin(id, x, y)   // Origin (0-1 normalized, 0.5 = center)
setPosition(id, "absolute")    // "static" | "relative" | "absolute" | "fixed" | "sticky"
setTop(id, px)                  // CSS top offset
setRight(id, px)                // CSS right offset
setBottom(id, px)               // CSS bottom offset
setLeft(id, px)                 // CSS left offset
```

## Animation

```js
// Animate a single property
animate(id, property, targetValue, durationMs, easingName)
// Example: animate("knob", "opacity", 1.0, 300, "ease_out_cubic")
// Properties: "value", "opacity", "x", "y", "scale", "rotation"

// Define keyframe sequences
defineKeyframes("pulse", [
    { offset: 0,   opacity: 0.5, scale: 1.0 },
    { offset: 0.5, opacity: 1.0, scale: 1.1 },
    { offset: 1,   opacity: 0.5, scale: 1.0 },
])

// Apply keyframe animation
setAnimation(id, "pulse", 1000)  // name, duration in ms

// Set transition duration for property changes
setTransitionDuration(id, 200)   // milliseconds

// Motion tokens (theme-integrated timing)
setMotionToken("motion.duration.fast", 80)
getMotionToken("motion.duration.fast")  // returns 80
```

### Easing Functions

`linear`, `ease_in_quad`, `ease_out_quad`, `ease_in_out_quad`, `ease_in_cubic`, `ease_out_cubic`, `ease_in_out_cubic`, `ease_in_quart`, `ease_out_quart`, `ease_in_out_quart`, `ease_in_expo`, `ease_out_expo`, `ease_in_out_expo`

## Events

```js
// Register event types (required before on() will fire)
registerClick(id)     // Enable click events
registerHover(id)     // Enable mouseenter/mouseleave events
registerPointer(id)   // Enable pointer events (pointerdown/pointermove/pointerup)
registerGesture(id)   // Enable gesture events (gesturestart/gesturechange/gestureend)

// Listen for events
on(id, "change", (value) => { })        // Knob, Fader, Checkbox value changed
on(id, "toggle", (state) => { })        // Toggle, ToggleButton state changed
on(id, "click", () => { })              // Click (requires registerClick)
on(id, "mouseenter", () => { })         // Mouse enter (requires registerHover)
on(id, "mouseleave", () => { })         // Mouse leave (requires registerHover)

// Inspector (developer tool)
enableInspectClick()
on("__inspect__", "click", (id) => { }) // Cmd+click reports widget ID
```

### Pointer Events (W3C PointerEvent)

Unified input model — same code works for mouse (macOS), touch (iOS), and stylus (Apple Pencil):

```js
el.addEventListener("pointerdown", (e) => {
    console.log(e.pointerId);       // Stable per-finger ID (0 = primary)
    console.log(e.pointerType);     // "mouse", "touch", or "pen"
    console.log(e.isPrimary);       // true for first finger / mouse
    console.log(e.clientX, e.clientY);
    console.log(e.pressure);        // 0.0–1.0 (Apple Pencil force)
    console.log(e.altitudeAngle);   // Pencil tilt (radians)
    console.log(e.azimuthAngle);    // Pencil rotation (radians)
});

el.addEventListener("pointermove", (e) => {
    // Coalesced events: all touch samples between frames (120Hz+ on ProMotion)
    for (const pt of e.getCoalescedEvents()) {
        drawLine(pt.clientX, pt.clientY);
    }
});

el.addEventListener("pointerup", (e) => { /* finger/mouse released */ });
el.addEventListener("pointercancel", (e) => { /* touch cancelled by system */ });
```

### Pointer Capture

Lock pointer events to an element during drag, even if the pointer leaves its bounds:

```js
el.addEventListener("pointerdown", (e) => {
    el.setPointerCapture(e.pointerId);
});
el.addEventListener("gotpointercapture", (e) => { /* capture acquired */ });
el.addEventListener("lostpointercapture", (e) => { /* capture released */ });
// Capture is automatically released on pointerup
```

### Gesture Events

High-level multi-touch and trackpad gestures:

```js
el.addEventListener("gesturestart", (e) => { /* two-finger gesture began */ });
el.addEventListener("gesturechange", (e) => {
    console.log(e.scale);     // Pinch zoom factor (1.0 = no change)
    console.log(e.rotation);  // Rotation in radians
});
el.addEventListener("gestureend", (e) => { /* gesture ended */ });
```

Works on macOS trackpad (magnifyWithEvent/rotateWithEvent) and iOS multi-touch.

### touch-action CSS

Control default gesture handling on touch platforms:

```js
el.style.touchAction = "none";          // Disable all default gestures
el.style.touchAction = "pan-x";         // Allow horizontal panning only
el.style.touchAction = "manipulation";  // Allow pan + pinch, disable double-tap zoom
```

## Canvas 2D Drawing

All canvas functions take the canvas widget `id` as the first parameter.

### State

```js
canvasSave(id)                    // Push graphics state
canvasRestore(id)                 // Pop graphics state
canvasClear(id)                   // Clear all draw commands
```

### Shapes

```js
canvasRect(id, x, y, w, h, color)                    // Filled rectangle
canvasStrokeRect(id, x, y, w, h, color, lineWidth)   // Rectangle outline
canvasFillCircle(id, x, y, radius, color)             // Filled circle
canvasStrokeLine(id, x1, y1, x2, y2, color, lineWidth) // Line segment
canvasFillText(id, text, x, y, fontSize, color)       // Rendered text
```

### Paths

```js
canvasBeginPath(id)               // Start new path
canvasMoveTo(id, x, y)           // Move pen
canvasLineTo(id, x, y)           // Line segment
canvasQuadTo(id, cpx, cpy, x, y) // Quadratic Bezier curve
canvasCubicTo(id, cp1x, cp1y, cp2x, cp2y, x, y) // Cubic Bezier curve
canvasClosePath(id)               // Close path to start point
canvasFillPath(id)                // Fill current path
canvasStrokePath(id)              // Stroke current path
```

### Style

```js
canvasSetFillColor(id, color)     // Fill color for shapes/paths
canvasSetStrokeColor(id, color)   // Stroke color for outlines/paths
canvasSetLineWidth(id, width)     // Line width in pixels
canvasSetFont(id, fontFamily, fontSize) // Font for text rendering
```

### Transforms

```js
canvasTranslate(id, x, y)        // Translate origin
canvasScale(id, sx, sy)          // Scale drawing
canvasRotate(id, radians)        // Rotate drawing
```

## Theme

```js
setTheme("dark")                  // Built-in: "dark", "light", "pro_audio"
getThemeJson()                    // Returns full theme as JSON string
applyTokenDiff(jsonString)        // Apply partial theme overrides from JSON
```

## Utility

```js
layout()                          // Trigger layout recalculation on all widgets
removeWidget(id)                  // Remove widget from tree
measureText(text, fontSize)       // Returns { width, ascent, descent, lineHeight }
compileShader(skslCode)           // Validate SkSL shader — returns { success, error }
exec(command)                     // Run shell command — returns output string
```

## Color Format

All color parameters accept CSS Color Level 4 syntax:

```js
"#RGB"                  // Short hex
"#RRGGBB"              // Hex
"#RRGGBBAA"            // Hex with alpha
"rgb(255, 128, 0)"     // RGB
"rgba(255, 128, 0, 0.5)" // RGBA
"hsl(30, 100%, 50%)"   // HSL
"hsla(30, 100%, 50%, 0.5)" // HSLA
"transparent"           // Named colors
```

## Legacy Positioning API

Widget creation functions also accept absolute coordinates for backward compatibility:

```js
createKnob(id, x, y, w, h)        // Absolute position
createFader(id, x, y, w, h, orientation)
createToggle(id, x, y, w, h)
createLabel(id, text, x, y, w, h)
```

Prefer the `parentId` API with flex/grid layout for new code.
