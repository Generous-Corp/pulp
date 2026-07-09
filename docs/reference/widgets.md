# Widget Catalog

Every developer-facing UI primitive Pulp ships in `core/view`. These are the
`pulp::view` classes you instantiate and place in a view tree. For the live,
interactive version of this set (dark + light), build and run
`examples/ink-signal-showcase`.

- **Naming:** some primitives carry a design-system alias matching the Figma
  "Ink & Signal" library — see [design-system-naming.md](design-system-naming.md).
- **Theming:** primitives paint from theme tokens, so a token/theme swap
  restyles them with no code change — see [design-tokens.md](../guides/design-tokens.md).
- **Keeping this in sync:** `tools/scripts/widgets_doc_check.py` (run by
  `tools/check-docs.sh` / `pulp docs check`) fails if a developer-facing
  `View` primitive is added without a row here. Add the row in the same change.

## Controls & values

| Widget | Purpose | Key capabilities | Header |
|--------|---------|------------------|--------|
| `Knob` | Rotary parameter control | value 0–1, default, label, format fn, modulation rings (Saturn), custom SkSL shader, sprite strip, hover glow, wheel | `widgets.hpp` |
| `Fader` | Linear parameter slider | value 0–1, orientation, thumb shape/size, skin overrides, shader/sprite, hover-grow, wheel | `widgets.hpp` |
| `RangeSlider` | Min/max/step slider (HTML range) | min/max/step, orientation, accent, track thickness, quantize, hover-grow, wheel | `widgets.hpp` |
| `DualRangeSlider` | Two-thumb min–max range slider | low/high values, no-cross clamp, orientation, accent, hover-grow, per-thumb drag | `widgets.hpp` |
| `InlineValueEditor` | Inline readout that becomes an editor | label + value, click-to-type, range clamp + danger ring, suffix, change callback, configurable caret style + blink | `widgets.hpp` |
| `PanControl` | Bipolar pan with centre detent | value −1..+1, hover-grow, wheel | `gap_widgets.hpp` |
| `XYPad` | 2-D parameter surface | x/y 0–1, axis labels, drag + gesture callbacks | `widgets.hpp` |
| `Toggle` | Animated on/off switch | on state, label, animated thumb + hover | `widgets.hpp` |
| `Checkbox` | Check box | checked state, change callback | `widgets.hpp` |
| `Stepper` | `[−] value [+]` numeric stepper | value/range/step/suffix, click-to-type, decimal wheel, blinking caret | `gap_widgets.hpp` |
| `NumberBox` | Compact `‹ value ›` numeric pill | value/range/step/suffix, chevron step zones, wheel | `gap_widgets.hpp` |

## Buttons

| Widget | Purpose | Key capabilities | Header |
|--------|---------|------------------|--------|
| `TextButton` | Text push button | primary/secondary/ghost styles, enabled, click, hover/pressed | `buttons.hpp` |
| `ToggleButton` | Full-width toggle button | on state, label, per-state color/radius/font overrides | `widgets.hpp` |
| `ArrowButton` | Directional arrow button | up/down/left/right, click | `buttons.hpp` |
| `ShapeButton` | Custom vector-shape button | shape draw fn with state, click | `buttons.hpp` |
| `ImageButton` | Image button | normal/hover/pressed images, click | `buttons.hpp` |
| `HyperlinkButton` | Opens a URL | text, URL, hover, click | `buttons.hpp` |
| `ResizableCorner` | Drag-to-resize handle | resize callback (dx,dy) | `buttons.hpp` |

## Text input

| Widget | Purpose | Key capabilities | Header |
|--------|---------|------------------|--------|
| `TextEditor` | Single/multi-line text editor | `multi_line`, `numeric_only`, `password_mode`, `placeholder`, select-on-focus, clipboard, undo/redo, IME, configurable caret style + blink | `text_editor.hpp` |
| `Label` | Static/dynamic text | font/weight/align/transform/decoration, multi-line, line-clamp, RTL, attributed runs | `widgets.hpp` |

### Caret

`caret.hpp` owns caret shape and blink policy for every editable widget —
`TextEditor`, `InlineValueEditor`, and any custom widget that draws its own
caret. Both concerns are configurable per widget, with process-wide defaults
(`default_caret_style` / `set_default_caret_style`, `default_caret_blink` /
`set_default_caret_blink`).

| Style | Renders as | Covers |
|-------|-----------|--------|
| `CaretStyle::ibeam` *(default)* | `be│` | a `stroke`-wide vertical rule spanning the glyph cell |
| `CaretStyle::underline` | `be_` | a rule at the underline position, one glyph cell wide |
| `CaretStyle::block` | `be█` | the filled glyph cell; the covered glyph is redrawn in the background color |

All three anchor at the same caret x — the boundary between the glyph before
the caret and the glyph after it. `ibeam` straddles that boundary, so the bar
marks the boundary itself rather than overlapping the glyph after it; the
cell-covering styles start at the boundary and extend right over that glyph.
The `underline` style sits **on the text baseline**, where a `_` glyph would land;
it is not a rule at the bottom of the widget's box, and it has nothing to do
with text-decoration underline. Because it occupies the cell of the glyph *at*
the caret rather than the text behind it, it never displaces the text.

At the end of the text there is no glyph under the caret, so its `advance` is 0
and `underline` and `block` size themselves from `CaretMetrics::nominal_advance`
instead. Leave `nominal_advance` at 0 and those two styles collapse to nothing;
set it from a representative glyph (a digit, in a numeric field). `ibeam` is
unaffected — it is always `stroke` wide.

**Blink policy.** The caret holds solid for `solid_hold_seconds` after any
caret movement or edit, then resumes blinking with `period_seconds` at `duty`
on-fraction. That hold is what makes an arrow-key sweep read as a continuous
caret rather than a strobe: every arrow, word jump, home/end, keystroke,
delete, and mouse drag calls `keep_solid()`, and blinking only restarts once
the caret has been still. The caret is also solid whenever a selection exists,
and it is never painted while the widget lacks focus.

The phase advances from a `FrameClock` subscription held while focused, so the
blink rate is the same on a 60 Hz and a 120 Hz display.

A blinking caret is animated content, so `CaretBlink` honors
[`MotionPreferences`](../guides/motion-observability.md): under
`MotionPolicy::Off` the caret is always visible and never blinks.
`CaretBlinkConfig::enabled = false` does the same thing unconditionally.

```cpp
#include <pulp/view/caret.hpp>

// App-wide: an underline caret that blinks slowly.
pulp::view::set_default_caret_style(pulp::view::CaretStyle::underline);
pulp::view::CaretBlinkConfig slow;
slow.period_seconds = 1.6f;   // full on+off cycle
slow.duty = 0.5f;             // visible for half of it
slow.solid_hold_seconds = 0.35f;
pulp::view::set_default_caret_blink(slow);

// ...but this one field keeps the stock I-beam and stock timing.
field.set_caret_style(pulp::view::CaretStyle::ibeam);
field.set_caret_blink({});
```

A custom widget that draws its own caret uses the same pieces: hold a
`CaretBlink`, call `keep_solid()` from every movement path, `advance(dt)` from a
`FrameClock` subscription taken while focused, and paint via
`caret_rect_for_style()` / `paint_caret()` — or `paint_caret_over_text()`, which
redraws the covered glyph in the background color so a `block` caret stays
legible. `caret_style_to_string` / `caret_style_from_string` give stable
lowercase names for serialization and the JS bridge.

## Lists & data

| Widget | Purpose | Key capabilities | Header |
|--------|---------|------------------|--------|
| `ComboBox` | Drop-down selector | items, separators, label fit, keyboard nav, scroll-aware flip, close-on-scroll | `ui_components.hpp` |
| `ListBox` | Scrollable selectable list | items, selection, double-click activate, keyboard nav, ensure-visible | `ui_components.hpp` |
| `VirtualList` | Recycling list for large rich row sets | fixed row height, overscan, bounded row pool, row bind/release callbacks, selection/focus, type-to-search | `virtual_list.hpp` |
| `VirtualGrid` | Recycling 2D grid for large cell sets | fixed cell size / column count, overscan, bounded cell pool, cell bind/release callbacks, selection/focus, 2D keyboard nav | `virtual_grid.hpp` |
| `TableListBox` (`Table`) | Sortable column table | columns (header/width/sortable/align), `TableModel`/`SimpleTableModel`, click-to-sort, themed rows | `table.hpp` |
| `TreeView` | Hierarchical tree | expand/collapse, selection, toggle/select/activate callbacks, keyboard nav | `tree_view.hpp` |
| `PresetBrowser` | Factory/user preset browser | show-mode filter, search filter, selection | `preset_browser.hpp` |

## Navigation & menus

| Widget | Purpose | Key capabilities | Header |
|--------|---------|------------------|--------|
| `SegmentedControl` | Mutually-exclusive section selector | labelled segments, selected/hovered index, click + arrow-key selection, change callback | `ui_components.hpp` |
| `TabPanel` | Tabbed container | tabs (title+content), active index, hide-bar card-stack mode, change callback | `ui_components.hpp` |
| `Toolbar` | Tool bar of items | button/toggle/separator/spacer/custom items, orientation, enable/toggle by id | `toolbar.hpp` |
| `Breadcrumb` | Breadcrumb trail | items, separator, push/pop/pop-to, navigate callback | `breadcrumb.hpp` |
| `ScrollBar` | Standalone scrollbar | orientation, range/value/page, arrow/page step, keyboard + drag + track-page | `scroll_bar.hpp` |
| `SidePanel` (`Sidebar`) | Slide-in edge panel | edge, extent, animated open/close, slide offset, state callback | `side_panel.hpp` |
| `ContextMenu` (`PopupMenu`) | View-tree popup menu | items (id/label/enabled/checked/separator), anchor, keyboard nav, outside/Esc dismiss | `context_menu.hpp` |

## Indicators & feedback

| Widget | Purpose | Key capabilities | Header |
|--------|---------|------------------|--------|
| `Meter` | Audio level meter | orientation, RMS+peak, ballistics, peak hold, skin gradient | `widgets.hpp` |
| `MultiMeter` | Multi-channel meter | layout, continuous/segmented, N channels, ballistics | `widgets.hpp` |
| `CorrelationMeter` | Stereo correlation −1..+1 | smoothed correlation, update(value,dt) | `widgets.hpp` |
| `ProgressBar` | Linear progress | progress 0–1 (`<0` indeterminate), optional label | `ui_components.hpp` |
| `Spinner` | Loading spinner | track ring + accent arc, indeterminate sweep or determinate fraction | `gap_widgets.hpp` |
| `Badge` | Compact pill label | text, tone (neutral/info/success/warning/danger) | `gap_widgets.hpp` |
| `InlineBanner` | Full-width status message | tone bar, label, message | `gap_widgets.hpp` |
| `Toast` | Transient raised card | title, subtitle, action + callback | `gap_widgets.hpp` |
| `EmptyState` | Dashed-border placeholder | message, action + callback | `gap_widgets.hpp` |
| `Tooltip` | Hover tooltip | text, show_at fade-in, hide fade-out | `ui_components.hpp` |
| `CallOutBox` | Floating alert/notification | message, confirm/cancel, auto-dismiss, `confirm()`/`notify()` factories | `ui_components.hpp` |
| `AnchoredCallout` | Popover anchored to a target rect (JUCE `CallOutBox` parity) | arbitrary child content, pointer triangle, side auto-flip when clipped, edge clamping, arrow tracking | `callout_box.hpp` |
| `ReorderList` | Drag-to-reorder container | lift dragged child, neighbours slide by measured pitch, drop tween + landing glow, `on_reorder(from,to)` commit | `reorder_list.hpp` |

## Audio-specific

| Widget | Purpose | Key capabilities | Header |
|--------|---------|------------------|--------|
| `MidiKeyboard` | Piano keyboard | range, note on/off, orientation, names, highlight, note callbacks | `midi_keyboard.hpp` |
| `WaveformView` | Waveform display | sample data or `AudioThumbnail`, trigger mode, oscillator preview shape, multi-channel | `widgets.hpp` |
| `WaveformEditor` | Interactive waveform | selection, zoom/scroll, playhead, named regions, selection callback | `waveform_editor.hpp` |
| `SpectrumView` | FFT magnitude spectrum | dB magnitudes, bars/line/filled, dB range | `widgets.hpp` |
| `SpectrogramView` | Scrolling STFT spectrogram | push frames, history/freq config, colormap, dB range | `widgets.hpp` |
| `EqCurveView` | Parametric EQ curve | draggable bands (freq/gain/Q/type), spectrum overlay, band callbacks | `eq_curve_view.hpp` |
| `ChannelStrip` | Mixer strip | label, level + pan (draggable), meter, wheel, callbacks | `gap_widgets.hpp` |
| `WaveformRecorder` | Three-state record/preview widget | armed / recording / captured states, live level, captured-waveform preview | `widgets.hpp` |
| `ModulationMatrixWidget` | Mod source→dest matrix | sources/dests, route lines, selected-route depth/curve | `modulation_matrix_widget.hpp` |
| `StepGridView` | Step-sequencer grid | backed by a `SequencerStateChannel` (audio-safe non-param state); submit-only editing with engine-echo replay, playhead overlay, bounded invalidation. `StepGridView` is the reference 12×32×32 alias of `StepGridViewT<Config, CellPolicy>`; `StepGridViewBase` is its non-template base. See [sequencer-state-channel](sequencer-state-channel.md). | `step_grid_view.hpp` |

## Containers & layout

| Widget | Purpose | Key capabilities | Header |
|--------|---------|------------------|--------|
| `Panel` | Styled container | background/border tokens, corner radius, border width | `widgets.hpp` |
| `GroupBox` | Titled (optionally collapsible) container | title chip, collapse chevron, header-click toggle, child show/hide | `widgets.hpp` |
| `ScrollView` | Scrollable container | direction, content size, smooth scroll, fading bars, wheel/track-page | `ui_components.hpp` |
| `SplitView` | Resizable split pane | orientation, split fraction, min sizes, divider, change callback | `split_view.hpp` |
| `ConcertinaPanel` | Accordion sections | sections (title+content), expand/collapse/toggle, exclusive mode | `concertina_panel.hpp` |
| `ModalOverlay` | Modal overlay | backdrop opacity, dismiss-on-backdrop, focus trap, Esc close | `modal.hpp` |
| `CanvasWidget` | Replays recorded Canvas2D | clear/add command, 50+ draw-command types, NaN-sanitized | `canvas_widget.hpp` |
| `NativeViewHost` | Embeds a platform-native child view | wraps host attach/bounds/clip/detach (WebView / native text field / video layer), scroll-tracking, clip-to-ancestor, fixed z-order (native above GPU), headless snapshot forwarding; mac/iOS-only | `native_view_host.hpp` |
| `MultiDocumentPanel` | Multi-document container | tabbed/tiled documents, active tracking | `file_browser.hpp` |

## Overlays

| Widget | Purpose | Key capabilities | Header |
|--------|---------|------------------|--------|
| `Popover` | Floating panel + tail | title, panel chrome; children laid out by host | `gap_widgets.hpp` |
| `InCanvasDialog` (`Dialog`) | In-canvas modal alert | title/message, confirm/cancel labels, destructive flag, callbacks | `gap_widgets.hpp` |

## Forms & properties

| Widget | Purpose | Key capabilities | Header |
|--------|---------|------------------|--------|
| `PropertyPanel` | Stack of property sections | sections of property rows | `property_panel.hpp` |
| `PropertyList` | List of labeled property rows | label + control rows | `property_panel.hpp` |
| `ColorPicker` | HSL/HSB/hex color picker | color/HSL/hex getters, swatches, mode, alpha, change callback | `color_picker.hpp` |
| `CodeEditor` | Native code editor | text buffer, language mode, line numbers, lightweight token coloring, read-only flag, append/line navigation; marker API is currently a no-op | `code_editor.hpp` |
| `FileDropZone` | Drag-and-drop file target | accepted extensions, hover/valid state, drop callback | `file_drop_zone.hpp` |
| `FileBrowser` | In-canvas file browser | directory listing + selection | `file_browser.hpp` |
| `FileTree` | Hierarchical file tree | directory expansion, file selection | `file_browser.hpp` |

## App shell

| Widget | Purpose | Key capabilities | Header |
|--------|---------|------------------|--------|
| `ThemeModeControl` | System / light / dark theme picker | 3-segment icon control; pairs with `ThemeManager.set_mode()`; `on_mode_change(ThemeMode)` | `ui_components.hpp` |
| `PreferencesPanel` | Tabbed preferences UI | setting categories, multi-page | `preferences_panel.hpp` |
| `KeyMappingEditor` | Keyboard-shortcut editor | interactive key binding edit | `key_mapping_editor.hpp` |
| `SplashScreen` | Startup splash | image, fade timing | `splash_screen.hpp` |
| `LassoComponent` | Drag-to-select lasso | visual selection feedback | `lasso.hpp` |

## Graphics & icons

| Widget | Purpose | Key capabilities | Header |
|--------|---------|------------------|--------|
| `Icon` | Built-in vector icons | type (image_upload/send/search/close) | `widgets.hpp` |
| `ImageView` | Image display | file/resource/memory URIs, image cache, value-driven silhouette fill | `widgets.hpp` |
| `SvgPathWidget` | Inline SVG `<path>` icon | path data, viewBox, fill/stroke, gradient, fill rule | `svg_path_widget.hpp` |
| `LottieView` | Lottie / Bodymovin animation | JSON load, FrameClock playhead, reduced-motion honoring, opt-in (`PULP_LOTTIE`) | `lottie_view.hpp` |
