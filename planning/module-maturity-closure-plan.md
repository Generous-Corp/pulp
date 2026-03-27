# Module Maturity Closure Plan

Date: 2026-03-25
Purpose: close the planning gap between "the module exists" and "the module is mature enough to present confidently to experienced framework users."

## Why This Exists

`planning/STATUS.md` marks phases 0-10 complete, but the public support story still shows a meaningful maturity spread:

- `runtime`, `state`: stable
- `events`, `audio`, `midi`, `signal`, `format`, `platform`: usable
- `canvas`, `render`, `view`, `osc`: experimental

That is not a contradiction. It means implementation work landed faster than maturity closure work.

The missing piece is an explicit public-facing path from current status to a mature status for each module, with exit criteria that are stronger than "code exists" or "tests pass on macOS once."

## Maturity Model

Use the existing support vocabulary, but interpret it as a progression:

- `experimental`: available, interesting, test-covered in spots, but API/behavior/support expectations are still fluid
- `usable`: credible for production on the supported scope, with docs, examples, validation, and known limitations documented
- `stable`: public contract is intentionally durable, docs/tests/examples are in sync, and the module is no longer presented as provisional

For Pulp, "mature" usually means:

- at least `usable` on the currently supported scope
- documented honestly
- covered by deterministic tests and examples
- not dependent on undocumented caveats

## Current Public Snapshot

Source: `docs/status/support-matrix.yaml` and `docs/reference/modules.md`

| Module | Current status | Main maturity gap |
|--------|----------------|-------------------|
| runtime | stable | Mostly documentation depth and long-term API discipline |
| events | usable | More narrative docs and broader integration examples |
| audio | usable | Cross-platform validation and stress coverage |
| midi | usable | Cross-platform validation and expanded MIDI coverage |
| signal | usable | Broader DSP surface and stronger plugin-level usage examples |
| state | stable | Mostly documentation depth and compatibility discipline |
| format | usable | AUv3/LV2/AAX completion and stronger validator automation |
| platform | usable | Windows/Linux runtime validation and deployment docs |
| canvas | experimental | Backend parity, public drawing contract, richer examples |
| render | experimental | Cross-platform GPU backend validation and failure-mode coverage |
| view | experimental | Missing core widgets/input model/application framework |
| osc | experimental | No explicit maturity phase or exit criteria today |

## Modules Missing a Clear Path to Mature

The highest-priority planning gap is not that `canvas`, `render`, and `view` lack roadmap items. They already have them. The gap is that their roadmap items are not yet framed as maturity gates.

The second gap is `osc`: it is still experimental, but the current roadmap does not clearly say what makes it leave that state.

### canvas

Current strengths:

- multiple drawing backends already exist
- SVG and effect support exist
- unit tests and widget usage already exercise the module

What still blocks maturity:

- no explicit backend conformance contract
- no published "what canvas guarantees" surface for text, transforms, images, and effects
- no direct example path that teaches canvas as a first-class module
- no cross-platform rendering parity story yet

Path to mature:

1. Phase 11: write the module deep-dive and document backend-specific caveats explicitly
2. Phase 14: exercise canvas through richer widgets and the showcase app
3. Phase 15-16: validate non-macOS backends and publish a backend capability table
4. Phase 17: include visual examples and screenshots that make the module teachable

Exit criteria:

- module guide explains the public contract and limitations
- backend capability matrix is published
- rendering regression suite exists for representative primitives
- examples and showcase use the module without undocumented caveats

Recommended target:

- `usable` after Phase 16
- `stable` only after one release cycle with the contract unchanged

### render

Current strengths:

- Dawn/Metal and Skia Graphite integration exist
- GPU surface creation is implemented and test-covered on the primary platform

What still blocks maturity:

- only macOS GPU path is meaningfully validated today
- D3D12 and Vulkan are still roadmap items, not delivered capability
- device-loss, fallback, and renderer selection are not yet a crisp public contract
- there is no published compatibility matrix by backend/platform

Path to mature:

1. Phase 11: document the current renderer model honestly, including experimental scope
2. Phase 15: validate D3D12 path on Windows
3. Phase 16: validate Vulkan path on Linux
4. Phase 17: add rendered examples, screenshots, and troubleshooting docs

Exit criteria:

- Metal, D3D12, and Vulkan backends each have runtime validation
- renderer capability matrix is published and branch-accurate
- device creation, fallback, and recovery behavior are documented and tested
- view/showcase/examples exercise the supported renderer paths on each platform

Recommended target:

- `usable` after Phase 16
- keep `experimental` until cross-platform runtime validation is real

### view

Current strengths:

- layout, basic widgets, scripting, inspector, drag-drop, screenshot, hot-reload, and auto-UI all exist
- the module is already interesting and differentiated

What still blocks maturity:

- missing input/text foundation
- missing standard widgets that real plugin UIs expect
- missing preset-management and application-framework surfaces
- showcase/examples are not yet broad enough to make the module self-explanatory

Path to mature:

1. Phase 11: publish a deep-dive that makes the current public surface understandable
2. Phase 14: complete the input model, text editor, list/tab/dropdown/progress/dialog surfaces, preset management, and application framework
3. Phase 17: publish a showcase, screenshots, and visual walkthroughs

Exit criteria:

- real plugin UIs can be built without custom reinvention of text input, list controls, or presets
- keyboard/focus/modal behavior is part of the public contract
- example gallery demonstrates a credible range of plugin UI patterns
- accessibility and scripting limitations are clearly documented

Recommended target:

- `usable` after Phase 14
- `stable` after Phase 17 if the public contract settles

### osc

Current strengths:

- OSC 1.0 message codec exists
- UDP sender/receiver exists
- loopback and malformed-packet tests exist
- basic docs exist

What blocks maturity right now:

- no explicit roadmap phase takes `osc` from experimental to usable
- current scope boundaries are not frozen tightly enough
- lifecycle/backpressure/thread-handoff behavior is not yet a public contract
- cross-platform socket validation is still missing
- there is no first-class example showing safe integration with state/view

Recommended closure work:

1. Scope freeze:
   - declare whether Pulp supports messages only, or messages plus bundles/timetags
   - declare whether UDP-only is intentional
   - document pattern matching, size limits, and threading guarantees explicitly
2. Reliability hardening:
   - add rapid start/stop and teardown-under-load tests
   - add malformed-packet corpus tests
   - add queue/handoff example for forwarding OSC safely to UI or audio-facing state
3. Cross-platform validation:
   - verify socket behavior on Windows and Linux when those platforms land
4. Example and docs:
   - add one example that maps OSC input to parameters or view state

Exit criteria:

- supported OSC feature subset is explicit and documented
- thread and lifecycle guarantees are documented
- codec and UDP behavior have deterministic tests beyond loopback happy paths
- at least one real example demonstrates safe use

Recommended target:

- `usable` after Phase 11 plus dedicated hardening work
- `stable` only after cross-platform socket validation in Phases 15-16

## Usable Modules That Still Need Stability Closure

These modules already have a path, but not yet a fully closed maturity story:

- `audio`, `midi`, `platform`: become truly mature after Windows and Linux runtime validation in Phases 15-16
- `format`: becomes materially stronger in Phase 13, but only feels complete in Phase 20 once AUv3/LV2/AAX gaps are addressed
- `signal`: already reads well as a usable DSP layer, but Phase 19 is where it becomes broad enough to feel like a flagship library

## Recommended Planning Changes

This should change how Pulp talks about progress internally:

1. Keep implementation progress in `STATUS.md`
2. Add a separate maturity view for public modules:
   - current status
   - target status
   - blocking gaps
   - phase that closes the gap
3. Do not promote a module from `experimental` to `usable` until docs, examples, and validation are present together

## Honest "Good Shape" Readout

If the question is "when will the modules page look mature to experienced developers?":

- core/macOS credibility: Phase 11
- UI stack credibility: Phase 14
- cross-platform module credibility: Phase 16
- examples-and-docs credibility: Phase 17
- full format credibility: Phase 20

That is the practical reading of the current plan.
