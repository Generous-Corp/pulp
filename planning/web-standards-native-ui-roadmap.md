# Web Standards Native UI Roadmap

Date: 2026-03-27

## Purpose

Define what "full support" should mean for Pulp's native JS -> View -> Canvas -> Skia/Graphite/Dawn path, without turning Pulp into a browser engine.

This roadmap is aligned with `CLAUDE.md`:

- primary UI path remains native GPU rendering
- no WebView/browser as the default UI runtime
- work is phased, testable, and spec-driven
- each phase must improve real product quality, not just theoretical standards coverage

This roadmap does **not** replace `planning/ralph-loop-prompt-11.md`.
It starts from that phase and defines what comes next.

## What "Full Support" Should Mean

For Pulp, "full support" should mean:

- modern JS-authored app UIs feel natural to frontend developers
- a large class of HTML/CSS app layouts can be translated into native Pulp UIs with low friction
- layout, styling, animation, accessibility, and interaction semantics are mature enough that a design-heavy tool or plugin UI does not feel constrained
- the same authored UI maps cleanly onto RecordingCanvas, CoreGraphics, Skia, and the Dawn/Graphite render path

It should **not** mean:

- full DOM
- full CSS cascade and selector engine
- browser network/runtime APIs
- arbitrary third-party web component compatibility
- browser-perfect rendering of any random webpage

Pulp should aim for a **world-class native web-inspired UI platform**, not a second browser.

## Current Baseline On `main`

As of this audit, `main` already has more than the original W3C summary suggests:

- flex direction, gap, flex-grow, min sizes
- `justify_content` variants implemented in layout and covered by `test/test_layout_w3c.cpp`
- `flex_shrink`
- per-side padding
- `max_width` / `max_height`
- `Overflow` mode on `View`
- view-level background, border, corner radius, opacity
- focus ring painting
- TextEditor with clipboard/undo/redo at the widget/platform layer
- SVG parsing/raster support in canvas
- JS bridge support for themes, token diffs, canvas drawing, and major widget creation

Important limitation:

- many of these features exist in C++ core or tests but are not yet fully exposed as a coherent frontend-style authoring surface
- `planning/ralph-loop-prompt-11.md` is therefore partly a parity milestone and partly a catch-up milestone

## What Phase 11 Gives You

If `planning/ralph-loop-prompt-11.md` lands successfully, Pulp should gain:

- strong parity for the specific AI Style Designer HTML app
- much better Flexbox-like layout behavior for app-style panels and toolbars
- richer design-tool styling surfaces
- better JS bridge coverage for current design-tool needs
- better screenshot-backed confidence that native Pulp can match a real HTML/CSS reference app

That is a big milestone, but it is still **not** the same as full native web-style support.

## What Still Exists After Phase 11

Even after 11, the major remaining gaps are likely:

### Layout Model Gaps

- margin
- `align-self`
- `flex-basis`
- `order`
- `row-gap` / `column-gap`
- `visibility: hidden`
- real overflow semantics beyond hidden/visible
- minimal Grid support
- better intrinsic sizing behavior for text/content-driven layouts

### Styling Model Gaps

- font weight / style / tracking / line height
- per-view text style overrides as first-class bridge features
- shadows and elevation as a system, not isolated draw hacks
- SVG path and icon ergonomics in the JS layer
- reusable style objects / class-like patterns without implementing full CSS selectors

### Interaction Model Gaps

- tab order and focus traversal semantics
- cursor semantics in the bridge
- clipboard access in the bridge
- drag/drop and file-drop surfaces
- better keyboard shortcut routing
- platform query surface (`prefers-reduced-motion`, density, scale)

### Authoring Model Gaps

- HTML-like declarative tree authoring
- CSS-like style declaration and reuse
- component-local styling
- reusable JS components instead of imperative bridge calls everywhere

### Validation Gaps

- reference corpus of web-style layouts
- backend parity validation across renderers
- screenshot regression for layout/style semantics
- clear statement of supported vs unsupported web features

## Definition Of Completion

Pulp should consider this effort mature when a frontend-minded developer can:

1. author a modern control-heavy UI in JS using HTML/CSS-like concepts
2. map layout and styles into native Pulp constructs without major friction
3. get a visually stable result across supported GPU/native frontends
4. use animation, icons, typography, and interaction states without custom hacks
5. inspect and test the result deterministically

Again: that is "mature native web-style authoring," not "browser compatibility."

## Recommended Phases After 11

## Phase 12: Native Web Semantics Completion

Goal:

- finish the missing layout/styling/interaction semantics required for modern app-style UIs
- make the JS bridge feel substantially more like a frontend authoring layer

Deliverables:

- missing Flexbox semantics
- richer typography/styling bridge
- cursor / clipboard / reduced-motion / visibility / overflow semantics
- backend-safe shadows, paths, and icon support

Explicitly defer minimal Grid unless a concrete Phase 11 consumer proves it is needed immediately. Phase 12 should improve real product quality first, not chase theoretical standards coverage.

This is the next immediate phase and is the subject of `planning/ralph-loop-prompt-12.md`.

## Phase 13: HTML/CSS Compatibility Layer + Demand-Driven Grid

Goal:

- allow a constrained HTML/CSS mental model to compile into native Pulp views

Deliverables:

- minimal Grid, but only for proven app-style use cases that actually need it
- define supported markup subset
- define supported CSS subset
- compile style declarations into Pulp view properties and tokens
- support component-scoped style reuse without implementing a full browser cascade

Recommended supported elements:

- container / `div`
- text / label
- button
- text input / textarea
- select / option
- canvas-like custom drawing node
- icon / svg node

Recommended non-goals:

- arbitrary browser HTML
- full forms API
- iframes
- Shadow DOM
- browser CSSOM / selector engine

## Phase 14: Animation + Interaction Authoring Model

Goal:

- make motion and interaction feel like first-class web-style authoring concepts

Deliverables:

- WAAPI-like animation controller surface over `FrameClock` + `ValueAnimation`
- transition presets for hover / focus / reveal / dismiss / layout
- reduced-motion policy
- gesture and pointer-capture semantics where appropriate
- keyboard/focus semantics that feel deliberate and complete

## Phase 15: Accessibility + Text Fidelity

Goal:

- make the native web-style layer production-safe for serious tools

Deliverables:

- stronger accessibility semantics and focus order
- richer text measurement and typography behavior
- better multiline/editor semantics
- validation for screen-reader/accessibility metadata

## Phase 16: Backend Parity + Validation Corpus

Goal:

- ensure the translation layer holds across actual render backends

Deliverables:

- reference corpus of app-style layouts and controls
- screenshot parity tests across RecordingCanvas / CoreGraphics / Skia
- GPU frontend validation for Graphite/Dawn path
- clear supported-feature matrix and rendering caveats

## Phase 17: Public Authoring Surface

Goal:

- make the capability usable and legible to outside developers

Deliverables:

- docs and examples for the supported web-style subset
- showcase/demo pages
- comparison examples: HTML reference -> native Pulp result
- design-tool integration using the same compatibility layer

## Architecture Guidance

## Keep The Core Model Native

Do not build a browser shell around the view system.

Instead:

- keep `View` as the retained native scene tree
- keep `Theme` / tokens as the styling backbone
- keep `Canvas` as the paint contract
- keep render backends unaware of HTML/CSS semantics

The compatibility layer should compile or translate **into** native Pulp concepts, not leak web-engine concepts downward.

## Prefer Translation Over Emulation

Good:

- HTML-like templates compiling to Pulp view trees
- CSS-like style objects compiling to layout/style fields
- WAAPI-like animation declarations mapping to native animators

Bad:

- a mini browser runtime
- DOM mutation model as the primary architecture
- selector-heavy cascade machinery in the renderer path

## Smart Translation Targets

Use the translation layer to map web concepts onto the GPU-native pipeline intelligently:

- Flex/Grid -> `View` layout engine
- CSS colors / opacity / borders / radius -> view styling + canvas paint
- box-shadow / drop-shadow -> canvas/effects layer
- transitions / keyframes -> `FrameClock` + motion tokens + animation controllers
- SVG icons / paths -> vector commands and cached geometry
- HTML-like containers -> retained native nodes

## Recommended Testing Strategy

Every phase after 11 should validate three things:

### 1. Semantics Tests

- layout math
- visibility / overflow / sizing behavior
- bridge property mapping

### 2. Rendering Tests

- screenshot/reference tests
- shape/icon/shadow/path regression
- backend-specific parity where practical

### 3. Authoring Tests

- HTML/CSS-like input compiles to the expected native tree
- style declarations resolve deterministically
- animation declarations produce stable controller behavior

## Suggested Future Files

If this roadmap is adopted, the next useful planning docs after prompt 12 would be:

- `planning/html-css-compatibility-layer-spec.md`
- `planning/native-animation-controller-spec.md`
- `planning/backend-parity-validation-spec.md`

## Bottom Line

To get to "full support," Pulp does **not** need a browser.

It needs:

- mature native layout semantics
- mature styling semantics
- mature interaction semantics
- a constrained HTML/CSS-like authoring layer
- strong backend translation and validation

Phase 11 gets the project much closer.
Phase 12 should finish the missing native semantics.
The later phases should make that power feel like modern JS/CSS/HTML authoring, translated intentionally into Pulp's native GPU-first architecture.
