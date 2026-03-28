"You are executing Phase 13: Native Web Semantics Completion for Pulp's native JS -> View -> Canvas -> Skia/Graphite/Dawn UI path.

This phase starts **after** Phase 11. Do not rewrite `planning/ralph-loop-prompt-11.md`. Treat Phase 11 as the baseline design-tool parity milestone and continue forward from there.

GOAL:
Make Pulp's native UI system feel substantially more like a modern frontend authoring platform for app-style JS/CSS/HTML concepts, while staying fully native and GPU-first. This is **not** a browser-engine phase.

PRIMARY REFERENCES:
- `CLAUDE.md`
- `planning/web-standards-native-ui-roadmap.md`
- `planning/ralph-loop-prompt-11.md`
- `planning/w3c-standards-coverage-map.md`
- `planning/css-flexbox-parity-spec.md`
- `planning/design-tool-phase-plan.md`

NON-NEGOTIABLES:
- No WebView/browser as the primary UI path
- No DOM/CSSOM/browser-engine architecture
- Keep render backends unaware of HTML/CSS semantics
- Every feature must be covered by automated tests and screenshot/reference validation where appropriate
- Prefer translation into native Pulp concepts over browser emulation

WORKING ASSUMPTION:
By the time this phase runs, Phase 11 has already delivered:
- design-tool parity for the original HTML app or near-parity
- the critical layout/styling fixes required for that app
- screenshot-backed validation of the design tool

WHAT THIS PHASE MUST DELIVER:

Phase 13.1 — Complete The Remaining Flexbox Semantics
Implement the highest-value frontend layout semantics still missing after Phase 11:

1. margin / per-side margin
2. `align-self`
3. `flex-basis`
4. `order`
5. `row-gap` / `column-gap`
6. `visibility: hidden` vs `display:none` semantics
7. fuller overflow behavior (`hidden`, `visible`, `scroll`, `auto`) mapped sanely onto native clipping/ScrollView

Files likely involved:
- `core/view/include/pulp/view/geometry.hpp`
- `core/view/include/pulp/view/view.hpp`
- `core/view/src/view.cpp`
- `core/view/src/widget_bridge.cpp`
- new/expanded layout tests

Acceptance criteria:
- margin, `align-self`, `flex-basis`, `order`, and split gap semantics are implemented in native layout and exposed in the bridge where appropriate
- `visibility:hidden` and `display:none` behave distinctly and deterministically
- overflow semantics are mapped cleanly to native clipping and/or scroll behavior without leaking browser concepts downward
- dedicated layout tests cover the new semantics and pass

Phase 13.2 — Typography And Styling Semantics
Close the largest styling gaps that frontend developers will feel immediately.

Required:
1. font weight
2. font style
3. letter spacing / tracking
4. line height
5. per-view text color as a first-class bridge feature
6. view-level background/border/opacity APIs fully exposed in JS
7. cursor style API
8. reduced-motion platform query bridge

Where possible, use the existing token system rather than raw one-off styling hacks.

Acceptance criteria:
- the bridge exposes typography and style controls in a way that frontend-minded authors can use predictably
- token-backed styling remains the preferred path rather than scattered one-off style hacks
- dedicated tests cover the new style semantics and pass

Phase 13.3 — GPU-Friendly Effects And Vector Semantics
Make common frontend visuals translatable to the native GPU pipeline.

Required:
1. box-shadow / drop-shadow support that maps cleanly to Canvas/effects
2. path builder API from JS:
   - beginPath / moveTo / lineTo / bezierTo / quadTo / closePath / fillPath / strokePath
3. SVG path/icon support from JS for common icon workflows
4. screenshot tests for shadows, icons, and smooth vector paths

This is where the native path must feel better, not worse, than a browser.

Acceptance criteria:
- common shadow, vector path, and icon workflows are available from JS without browser-only abstractions
- screenshots/reference cases validate the visual output deterministically
- the implementation keeps the renderer backend-agnostic with style semantics terminating before render

Phase 13.4 — Bridge Ergonomics For Frontend Authors
The bridge should stop feeling like a thin imperative shim and start feeling like a real authoring surface.

Required:
1. expose the new layout/styling semantics cleanly in JS
2. ensure the API naming is consistent and frontend-legible
3. preserve hot reload and state snapshot/restore behavior
4. add targeted error handling for invalid bridge calls where reasonable
5. keep the bridge data-oriented and predictable

DO NOT:
- build a DOM runtime
- introduce a selector engine

Acceptance criteria:
- the new APIs are consistent, frontend-legible, and hot-reload-safe
- invalid bridge calls fail in a targeted, debuggable way where reasonable
- the bridge remains data-oriented rather than turning into a DOM-like runtime

Phase 13.5 — Validation Corpus
Build a small but serious reference corpus proving that native Pulp can reproduce modern app-style web layouts.

Required:
1. a flex-heavy reference screen
2. a typography/state-heavy reference screen
3. a vector/icon/effects reference screen
4. screenshot-based verification for each

These should be native Pulp examples, but clearly inspired by real frontend layout expectations.

Acceptance criteria:
- the validation corpus covers every feature delivered in Phase 13
- screenshot/reference verification is automated and reproducible
- if Grid is still demonstrably needed after this corpus is built, capture that as a concrete Phase 13 input rather than expanding Phase 13 mid-flight

SUCCESS CRITERIA:
- frontend-minded developers can express common app layouts without fighting the bridge
- styling and typography stop feeling underpowered
- common web-style visuals (shadows, icons, paths) render natively and predictably
- tests and screenshots prove the semantics, not just the happy path
- render/backend abstractions remain clean: layout/style semantics terminate before the renderer

EXPLICIT NON-GOALS FOR PHASE 12:
- full browser compatibility
- full DOM
- full CSS cascade/selectors
- arbitrary third-party HTML/CSS component parity
- network/browser APIs

TESTING REQUIREMENTS:
- run `ctest --test-dir build --exclude-regex 'AudioWorkgroup|CoreAudio' --output-on-failure` after each sub-phase
- add dedicated tests for new layout/style semantics
- use screenshot/reference verification for visuals
- prefer deterministic tests over manual inspection

GIT / WORKTREE DISCIPLINE:
- create/use a dedicated Phase 13 worktree/branch
- commit after each sub-phase
- clean commit messages
- do not pollute `main` with speculative half-finished semantics

EACH ITERATION:
1. Read `CLAUDE.md`
2. Read `planning/ralph-loop-prompt-12.md`
3. Read the relevant section of `planning/web-standards-native-ui-roadmap.md`
4. Pick the next incomplete sub-phase
5. Implement
6. Build and test
7. Verify with screenshots/reference cases
8. Commit

COMPLETION CONDITION:
- all Phase 13 sub-phases are implemented and validated
- the JS bridge feels materially more like a native frontend authoring surface
- the new semantics are proven by tests/screenshots
- the result clearly sets up the next phase: constrained HTML/CSS compatibility, and demand-driven Grid if real consumers still require it

ONLY WHEN ALL CONDITIONS ARE MET:
Output exactly: PHASE 12 COMPLETE"
