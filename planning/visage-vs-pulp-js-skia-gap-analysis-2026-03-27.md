# Visage vs Pulp JS->Skia Gap Analysis

Date: 2026-03-27

## Scope

This note compares the visible capabilities of Visage with the Pulp JS -> QuickJS -> native View -> Canvas -> Skia/CoreGraphics path, with one important assumption:

- Treat `planning/ralph-loop-prompt-10.md` as the near-term baseline.
- In other words, assume the prompt-10 items either already exist on `phase/ai-designer` or land soon enough that they should not dominate the long-term gap analysis.

This is not a code-port recommendation. It is a product and architecture gap read: what still separates "Pulp has a serious UI system" from "Pulp feels like a complete, polished platform."

## Evidence Used

Pulp:

- `planning/ralph-loop-prompt-10.md`
- `/Users/danielraffel/Code/pulp-ai-designer/core/view/src/widget_bridge.cpp`
- `/Users/danielraffel/Code/pulp-ai-designer/examples/design-tool/design-tool.js`
- `docs/guides/animation.md`
- `examples/ui-preview/main.cpp`

Visage:

- `/Users/danielraffel/Code/visage/README.md`
- `/Users/danielraffel/Code/visage/visage_graphics/animation.h`
- `/Users/danielraffel/Code/visage/visage_ui/frame.h`
- `/Users/danielraffel/Code/visage/visage_ui/popup_menu.h`
- `/Users/danielraffel/Code/visage/visage_app/window_event_handler.cpp`
- `/Users/danielraffel/Code/visage/examples/Showcase/*`

Claude alignment:

- Claude’s useful summary was: once the basic JS-authored native UI stack exists, the remaining “feels complete” gaps are usually compositing/invalidation quality, text/input polish, accessibility/focus quality, hit-testing over transformed/clipped layers, scrolling/gesture smoothness, and a robust effect pipeline.

## What Pulp Already Has Or Clearly Has In Flight

Assuming the `phase/ai-designer` branch and prompt-10 work are representative, Pulp already has the right foundational direction:

- JS-authored native UI through QuickJS, not a browser wrapper
- a retained view tree with flex layout
- theme/token-driven styling with runtime token diffs
- a shared animation clock and widget-local animation model
- shipped audio-native primitives like knobs, faders, meters, XY pads, waveform, and spectrum
- an inspectorable, testable, local-first architecture
- a design-tool direction where AI modifies tokens and JS rather than generating dead mockups

That is already a real platform direction, not a toy.

## What Visage Still Communicates Better

Visage currently projects “complete system” more clearly because it visibly demonstrates:

- a broader everyday widget and overlay surface
- stronger event/input coverage and window handling
- popup/menu/scroll/editor behavior that looks productized
- richer graphics/effects/compositing features
- a public showcase that makes the platform legible at a glance

The important point is not that Pulp lacks all of these primitives. It is that Visage presents them as a coherent whole.

## Gap Analysis

### 1. Pulp Still Needs A More Complete Interaction Layer

Even if prompt 10 lands, Pulp still appears behind Visage in the “full app-quality UI behavior” layer:

- overlay stack discipline: popup menus, callouts, tooltips, combo popups, modal dismissal, click-outside behavior, z-order, focus return
- richer selection controls and list controls: property lists, tree views, table/grid views, inspector panels, virtualized collections
- drag/drop as a first-class authoring and app behavior surface
- keyboard navigation beyond “text input works”: tab order, arrow traversal, default actions, escape semantics, focus ring consistency

This is where many frameworks feel 80% done for a long time.

### 2. Animation Is Present, But The Motion Language Is Not Yet Broad Enough

Pulp’s clock + motion-token + widget-local animation model is directionally right. The remaining gap is not “add tweening.” It is making motion feel systemic:

- overlay enter/exit transitions
- menu and popup choreography
- layout transitions when panels resize or reflow
- scroll inertia / momentum / easing consistency
- animation policies for hover, press, release, enable/disable, focus, selection, reveal, dismiss
- animation-aware teardown so removed UI does not pop out abruptly

Visage feels more mature here because motion is woven into more surfaces, not because it has a more academically elaborate animation engine.

### 3. Text, Input, And Focus Need To Feel Boringly Reliable

This is one of the biggest “senior engineer trust” thresholds.

Prompt 10 already identifies text input as a blocker. Even after that is fixed, the next bar is:

- IME and composition correctness
- selection handles and word/line navigation quality
- clipboard and undo/redo depth
- multi-line editing polish
- focus transfer across popups, menus, text fields, and modal surfaces
- stable keyboard shortcut routing

Visage projects maturity because text and input are clearly treated as core infrastructure, not as widgets bolted on last.

### 4. Pulp Still Needs Better Compositing, Effects, And Presentation Surfaces

Visage advertises partial rendering, batching, blend modes, shaders, blur, bloom, and creative-graphics workflows directly in its public face.

For Pulp, the likely missing or under-demonstrated areas are:

- dirty-region or selective redraw discipline
- layer/cached-surface strategy for expensive UI
- blur, backdrop, shadow, glow, and blend/compositing surfaces that are usable from the UI layer
- clipped/transformed hit-testing that remains correct under overlays and special effects
- authorable “visual surfaces” beyond standard widgets

This matters because AI-driven restyling gets much more compelling once the platform can express actual visual atmosphere, not just nicer controls.

### 5. The JS Authoring Layer Is Useful, But Not Yet A Fully Comfortable Product Surface

The `phase/ai-designer` bridge is already materially better than `main`. It has containers, flex setters, more widgets, callbacks, theme switching, token diffs, and authoring helpers. That is a strong step.

The remaining likely gaps are ergonomic rather than existential:

- reusable JS components, not just a flat script of imperative calls
- stronger state model and composition patterns
- clearer separation between authoring data, live state, and plugin state
- better error reporting when a script or token diff fails
- custom drawing hooks that feel intentional, not “escape hatch only”
- a more declarative authoring layer on top of the low-level bridge once the bridge stabilizes

The risk is not that the bridge is too weak forever. The risk is that it remains correct but slightly awkward, which prevents the design tool from feeling joyful.

### 6. Theme/Tokens Need To Expand From “Can Restyle” To “Can Express A Design Language”

Pulp already has the right architectural instinct: themes are structured data, and token diffs can drive restyling.

To feel truly complete, the token system should likely grow in these directions:

- richer state tokens: hover, active, selected, disabled, focus, pressed
- typography tokens beyond size: weights, tracking, line heights, text roles
- elevation/shadow/surface-depth tokens
- component density and spacing scales
- motion personality tokens by context, not only raw durations
- validation and schema guarantees for style packs
- scoped overrides that are easy to inspect and reason about

This is also where Pulp can surpass Visage: AI-friendly token diffs are a more powerful story than a traditional theme editor if the token model is clean and inspectable.

### 7. The Inspector Story Needs To Become A Platform Feature, Not Just A Utility

Pulp has a major opportunity Visage does not emphasize as strongly:

- inspect the full UI tree
- inspect resolved tokens
- inspect parameter attachments
- inspect animation state and frame timing
- inspect dirty/invalidation reasons
- inspect layout constraints and final bounds

If this becomes great, Pulp can feel more modern than Visage for both humans and agents.

Right now, the likely gap is not whether an inspector exists, but whether it is deep enough to debug real design-tool and plugin UI sessions.

### 8. “Plugin-Ready” Still Needs A More Explicit Story

The most differentiated promise in Pulp is not “we have widgets.” It is:

- design in JS
- preview against a real plugin
- accept changes
- lock style into durable plugin-owned artifacts
- ship the result

That story is stronger than Visage’s public positioning, but it only lands if the platform makes these things obvious:

- style pack format
- apply/save/reload flow
- screenshot diff and regression story
- plugin preview host
- example plugins that demonstrate the workflow end to end

Without that, the AI-designer story risks feeling like a nice demo instead of a shipping tool.

## Highest-Leverage Next Improvements

These are the improvements most likely to make experienced engineers more impressed.

### Tier 1: Complete The “Common App UI” Surfaces

- overlays/popups/modals with correct focus and dismissal
- robust text/input/focus routing
- real ScrollView behavior with momentum and clipping correctness
- list/tree/property-grid style controls
- drag/drop and file-drop surfaces

### Tier 2: Make Motion Feel Systemic

- standard enter/exit/reveal/dismiss transitions
- layout transition policy
- scroll motion policy
- component-level animation conventions documented and testable
- motion tokens expanded into a true motion system

### Tier 3: Deepen The Design/AI Layer

- style pack schema and validation
- token-scoped diff application
- better inspector support for resolved styles and bindings
- reusable JS component patterns
- custom drawing/canvas surface for authoring richer controls

### Tier 4: Improve The Visual Ceiling

- blur/shadow/backdrop/effect pipeline
- layer caching / dirty-region strategy
- more visually expressive shipped primitives
- better support for polished audio UX surfaces like envelopes, routing, modulation overlays, keyboards, and scopes

## Where Pulp Can Beat Visage

Pulp should not try to win by copying Visage’s public shape. It can win by being stronger in areas Visage does not foreground:

- AI-native restyling with inspectable token diffs
- local-first authoring with Claude Max and hot reload
- deterministic screenshot and animation testing
- audio-native primitives and plugin-oriented workflows
- “lock it in and ship it” style packs bound to real plugins
- inspector and automation surfaces that agents can use without web calls

That is a better strategic position than “Visage, but with a different renderer.”

## Recommended Framing For The Next Milestone

After prompt 10, the next milestone should not be “more widgets” in the abstract. It should be:

1. Make the interaction layer feel complete.
2. Make motion feel like a system, not a set of isolated hover effects.
3. Make the design-tool and plugin-shipping workflow undeniable.
4. Make the inspector and testing story a selling point.
5. Then build the public showcase around those truths.

## Bottom Line

If prompt 10 lands, Pulp is no longer missing the basics. The remaining gap versus Visage is mostly in completeness, polish, and presentation:

- more complete interaction surfaces
- more systemic motion
- better text/input/focus reliability
- higher visual/compositing ceiling
- a better “show me the whole platform” public face

That is a good place to be. It means the next gains come from turning strong pieces into a convincingly finished platform.
