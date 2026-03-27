# Animation, Tweening, and JS Bridge Proposal

Date: 2026-03-26
Type: Audit + proposal
Scope: Pulp animation/tweening ergonomics, Visage pattern extraction, JS bridge implications

## Bottom Line

Pulp does support animation primitives today, but not yet in a way that feels easy, complete, or convincingly integrated.

The current state is:

- `pulp/view/animation.hpp` exists and provides easing functions, a `Tween`, and an `AnimationManager`
- the `View` layer and input event model are richer than the scripting bridge
- the JS `WidgetBridge` is severely underpowered compared to the rest of the view/widget system
- there is little evidence that animation is wired into the actual frame/update/invalidation loop as a first-class subsystem

So the answer is:

- **yes, Pulp has the beginnings of animation/tweening**
- **no, it is not yet easy or platform-defining**

Visage is useful here, not as source to imitate, but as proof of a good pattern:

- small, local animation primitives
- consistent motion timings
- animation tied directly to widget interaction state
- repaint while animating
- rich input events feeding those states

That pattern fits Pulp well if it is translated into Pulp-native architecture.

## Current Pulp State

### 1. Animation exists at the primitive level

`core/view/include/pulp/view/animation.hpp` contains:

- easing functions
- `Tween`
- `AnimationManager`

This is a real start. It is enough to animate values over time.

What appears missing is the surrounding system:

- a shared animation clock or frame clock
- automatic invalidation while animations are active
- clear widget-level usage patterns
- JS bridge exposure
- docs/tests proving this is a supported workflow rather than a utility header

### 2. The View layer is more capable than the JS bridge

`core/view/include/pulp/view/view.hpp` and `core/view/include/pulp/view/input_events.hpp` already show a richer model:

- retained view tree
- flex layout
- rich `MouseEvent`, `KeyEvent`, and `TextInputEvent`
- focus management
- accessibility roles

`core/view/include/pulp/view/widgets.hpp` also exposes more than the bridge currently reaches:

- style enums for knobs and toggles
- widgets like `Meter`, `XYPad`, and `WaveformView`
- widget callbacks like `on_change` and `on_toggle`

So the limiting factor is not only the widget set. The limiting factor is the bridge and the missing animation/invalidation story around it.

### 3. The JS bridge is much thinner than the surrounding architecture

`core/view/src/widget_bridge.cpp` currently exposes only:

- `createKnob`
- `createFader`
- `createToggle`
- `createLabel`
- `setValue` / `getValue`
- `getParam` / `setParam`

That matches the user’s diagnosis closely. The bridge is real, but it is not yet strong enough to support a serious design tool, animation system, or modern scripted UI authoring surface.

It is especially limited because it currently lacks first-class APIs for:

- layout
- richer widgets
- styles/theme updates
- event callbacks
- token diffs
- animation control

### 4. The docs are ahead of the implementation

There is a real mismatch between the code and the surrounding story.

Examples:

- `docs/examples/ui-preview.md` describes a JS-driven preview pipeline and mentions flexbox layout, but the JS bridge itself is still absolute-positioned widget creation
- `docs/reference/modules.md` presents animation, scripting, and the widget system as a stronger integrated surface than the bridge currently demonstrates
- `planning/pulp-ui-architecture-spec.md` already identifies several AI-designer-related gaps, including missing token validation, style pack format, and theme JSON hot reload

This is not fatal, but it does mean animation/tweening should be treated as a real gap-closure effort, not as “already solved.”

## What Visage Appears To Do Well

This proposal uses only pattern-level observations from `~/Code/visage`.

Relevant files observed:

- `visage_graphics/animation.h`
- `visage_ui/events.h`
- `visage_widgets/button.cpp`
- `visage_ui/scroll_bar.cpp`
- `visage_ui/popup_menu.cpp`
- `examples/Showcase/showcase.cpp`

### 1. Animation is ubiquitous, not special

Visage uses a small `Animation<T>` primitive directly inside widgets and UI surfaces.

Examples observed:

- hover animation on buttons
- opacity animation on popup menus
- width/color animation on scroll bars
- overlay animation in the showcase

This matters because it makes motion feel like part of the widget model, not a separate subsystem that only appears in demos.

### 2. Repaint happens naturally while animation is active

The observed pattern is:

- widget receives a state change
- widget updates an animation target
- widget updates animation during draw/timer flow
- widget keeps repainting while the animation is active

That creates a coherent feeling without requiring a huge timeline engine for common interactions.

### 3. The event model is rich enough to drive polished motion

`visage_ui/events.h` includes:

- modifier-aware mouse/key events
- pointer IDs
- click counts
- relative/window coordinates

Pulp already has a reasonably strong equivalent direction in `input_events.hpp`, which is good. The opportunity is not “copy Visage events,” but “use Pulp’s existing event richness more fully.”

### 4. Motion timings are treated as part of the design language

Observed Visage timing constants:

- fast
- regular
- slow

This is a small but useful pattern. It suggests Pulp should treat motion as themed behavior, not just hardcoded durations buried in widgets.

## What Pulp Should Learn, Not Copy

Pulp should import the pattern, not the implementation.

The useful lessons are:

1. Animation should be a normal part of widget behavior.
2. Common UI motion should not require a separate orchestration layer every time.
3. Rich events make polished motion much easier.
4. Motion timing belongs in the design system.
5. Repaint/invalidation must be tied to animation activity.

Pulp should not import:

- Visage API names
- class structure
- bgfx-centric assumptions
- widget drawing styles as-is
- any source-level implementation details

## Proposed Pulp Direction

### 1. Make animation a first-class view concern

This is the core architectural recommendation.

Pulp should introduce an explicit animation/frame mechanism inside the view system:

- one authoritative animation clock or frame clock
- integrated with the native window/render loop
- animation-aware invalidation
- deterministic ticking for tests

This aligns with Claude’s most useful pushback:

> make animation real in the view/frame system before expanding the JS bridge

That is the correct order.

### 2. Adopt two layers of animation

#### Layer A: widget-local interaction animation

For most UI polish:

- hover
- press
- focus ring fade
- toggle thumb slide
- menu fade
- tooltip fade
- scroll thumb width/color transitions
- panel open/close opacity/translation

This should feel easy and cheap.

The ergonomic model should be:

- widget owns local animation state
- widget sets targets in response to events
- shared clock advances the state
- view invalidates while active

#### Layer B: scripted/timeline animation

For the design tool, showcase, and richer UX:

- choreographed transitions
- staggered entry/exit
- panel transitions
- animated theme changes
- modulation overlays
- preset browser transitions

This can sit on top of the core view animation substrate and later become part of the JS surface.

### 3. Expand the JS bridge, but only after the native substrate is real

The user’s architecture summary is directionally right:

`JS script -> ScriptEngine -> WidgetBridge -> C++ View tree -> Canvas -> GPU/CoreGraphics`

But the current bridge is too weak to carry a design tool yet.

The correct evolution order is:

1. native animation clock + invalidation
2. widget-local motion patterns in C++
3. token-aware style and motion hooks
4. bridge expansion for layout/widgets/styles/events/theme diffs
5. higher-level scripted design tools

Do not start by pouring animation semantics into the JS bridge while the view/frame model is still incomplete.

### 4. Treat motion as part of the token system

Pulp should add motion tokens or motion presets for:

- hover duration
- press duration
- enter/exit duration
- easing family
- spring/damped profile if added later
- optional overshoot/bounce intensity

That makes the designer materially more useful:

- AI can change “feel,” not just color
- design languages can include motion personality
- styles remain data, not scattered constants

### 5. Define a baseline animation pack for shipped primitives

Pulp should ship baseline, tasteful animation behavior for common controls:

- knob hover/focus state
- fader hover and thumb emphasis
- toggle state transition
- button hover/press/focus
- dropdown open/close
- tooltip fade
- tab transition
- meter decay and peak hold presentation

This should be subtle by default.

The goal is not “flashy.” The goal is “polished enough that experienced developers immediately notice that motion was designed intentionally.”

## First Milestone Recommendation

The first milestone should not be “full JS timeline support.”

It should be:

### Milestone 1: AnimationClock + invalidation + two or three real widgets

Specifically:

- add one shared `AnimationClock` or equivalent view-run-loop time source
- make active animations trigger repaints/invalidations automatically
- wire local animations into a small number of shipped widgets
- prove the pattern on:
  - button/toggle hover/press
  - popup or panel fade
  - meter decay / peak hold visual behavior

This follows Claude’s most useful one-line guidance:

> Wire a single `AnimationClock` into the view run-loop that widgets can subscribe to...

That is the right first step because it turns animation from a header into a system.

## Bridge Proposal

Once Milestone 1 exists, the bridge can expand in a way that matches Pulp architecture.

### Next JS bridge capabilities that make sense

Layout:

- `createRow()`
- `createColumn()`
- `setFlexGrow()`
- `setGap()`
- `setPadding()`

Widgets:

- `createMeter()`
- `createComboBox()`
- `createXYPad()`
- `createWaveform()`
- `createSpectrum()`
- `createTextEditor()`
- `createScrollView()`
- `createTabPanel()`

Styles and tokens:

- `setTheme(json)`
- `getTheme()`
- `applyTokenDiff(json)`
- widget-style setters where needed

Events:

- `onValueChange(id, callback)`
- `onClick(id, callback)`
- focus and selection events as needed

Animation:

- motion preset application
- animated token transition triggers
- scripted transitions only after the native substrate is in place

## Testing Proposal

This needs stronger testing than “it looks smooth on my machine.”

### Deterministic unit tests

- easing output samples
- tween progression under fixed `dt`
- cancel/complete behavior
- clock subscription and invalidation behavior
- motion token parsing/validation

### Widget behavior tests

- hover target changes update local animation state
- press/release transitions behave correctly
- repaint is requested while animation is active
- widgets settle to the expected end state

### Bridge tests

- JS event callbacks are invoked correctly
- JS layout/widget/style APIs create the expected native view state
- token diffs apply safely and deterministically

### Visual regression tests

- sample keyframes at fixed times
- compare before/midpoint/final states
- cover at least the shipped primitive set and one example plugin target

### Design-tool tests

- style change preview
- accept/reject
- save/reload style pack
- style pack reapplication to another compatible example

## What This Means For The AI Designer

This proposal directly supports the AI-designer direction.

If Pulp adopts this path, the design tool becomes much more credible because it can operate on:

- real widgets
- real motion
- real token diffs
- real hot reload
- real plugin/example previews

Instead of only changing colors, it can change feel:

- softer/faster hover response
- snappier toggles
- calmer panel transitions
- more analog meter behavior
- different motion personalities per style pack

That makes the platform meaningfully better.

## Recommendation

Pulp should treat animation/tweening as an important gap to close.

Not because animation is a vanity feature, but because:

- it improves perceived quality quickly
- it makes the view system feel complete
- it makes the JS bridge worth expanding
- it materially strengthens the AI Designer story

The best next move is:

1. make animation real in the view/frame system
2. prove it in a few core widgets
3. then expand the JS bridge around that reality

## Alignment With Claude

Claude’s most useful feedback on this direction was:

- yes, make animation a first-class view concern before expanding the JS bridge
- the biggest risk is overbuilding the bridge before the frame/invalidation model is real
- the first milestone should be a single shared animation clock integrated into the view run-loop

That aligns with this proposal.
