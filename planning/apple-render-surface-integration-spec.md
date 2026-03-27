# Apple Render Surface Integration Spec

Date: 2026-03-26
Audience: implementation agent
Status: proposed next milestone for Apple rendering

## Purpose

Define the next Apple-render milestone for Pulp:

- move from offscreen or loosely connected GPU foundation work
- to a coherent, presentable, testable Metal-backed render path on macOS and iOS
- while keeping docs and support manifests honest about actual maturity

This spec is the concrete follow-up to:

- [metal-integration-review-and-mpp-takeaways.md](/Users/danielraffel/Code/pulp/planning/metal-integration-review-and-mpp-takeaways.md)
- [12-rendering-strategy.md](/Users/danielraffel/Code/pulp/planning/12-rendering-strategy.md)

## Goal

Establish one trustworthy Apple render story.

At the end of this milestone, Pulp should be able to describe the Apple rendering path in a way that is technically defensible:

- one clear native surface model on macOS
- one clear native surface model on iOS
- one coherent ownership boundary between platform, render, and view
- one explicit frame lifecycle from drawable acquisition to present
- one honest set of docs and support manifests matching the implementation

## Problem Statement

The current Apple render path appears to have three gaps:

1. surface ownership is unclear
2. Dawn/WebGPU and Skia Graphite ownership are duplicated or split
3. the visible render path appears offscreen-first rather than explicitly presentable

That creates two problems:

- implementation risk, because multiple abstractions appear to overlap without a single end-to-end presentation model
- credibility risk, because the docs can read as more integrated than the code currently shows

## Scope

This milestone has five tracks:

1. **Architecture**
Define the Apple render architecture precisely enough that an implementation can be judged against it.

2. **Native surface integration**
Specify the supported native presentation surfaces for macOS and iOS and how they enter the render stack.

3. **Ownership boundaries**
Clarify what belongs to `platform`, `render`, and `view`.

4. **Validation**
Define how Apple presentation is verified locally and in docs/support claims.

5. **Docs honesty**
Bring render docs and support manifests in line with the actual Apple path.

## Non-Goals

Not part of this milestone:

- Metal Performance Primitives optimization work
- Apple-specific ML or tensor compute
- Windows or Linux rendering redesign
- large widget or JS runtime expansion
- speculative performance tuning before presentation correctness exists
- making the Apple path look more mature than it is

## Desired Architecture

The Apple render architecture should read like this:

```text
Platform host
  -> owns native view/layer/window attachment
  -> provides native surface handles and lifecycle events

Render subsystem
  -> owns one Dawn/WebGPU instance/device/queue path
  -> owns one presentable GPU surface abstraction
  -> owns acquire/record/submit/present lifecycle

Skia Graphite integration
  -> records into the render subsystem's presentable target
  -> does not create a parallel GPU ownership path

View subsystem
  -> performs layout, input, invalidation, and paint traversal
  -> does not own native Metal objects
```

The important constraint is that the Apple path should not have:

- one object creating a native surface
- another object creating an unrelated GPU stack
- and a third object rendering offscreen with no explicit on-screen destination

## Native Surface API Requirements

The implementation must choose and document a concrete native surface model for each Apple platform.

### macOS

Supported options are expected to be one of:

- `NSView` backed by `CAMetalLayer`
- `MTKView`

The chosen path must make explicit:

- who creates the view/layer
- who owns resize and scale-factor updates
- how the render system receives the native presentable surface
- how plugin-host embedding differs from standalone-window ownership, if it differs at all

### iOS

Supported options are expected to be one of:

- `UIView` backed by `CAMetalLayer`
- `MTKView`

The chosen path must make explicit:

- safe-area handling
- display scale / Retina handling
- resize and orientation handling
- background/foreground lifecycle interaction
- AUv3 or embedded-view constraints if relevant

### Cross-Platform Apple Requirement

The macOS and iOS paths do not need to use identical Cocoa/UIKit types, but they should satisfy the same logical contract:

- create or attach native presentable surface
- expose it to the render subsystem
- let the render subsystem acquire, render, submit, and present

## Ownership Boundaries

This milestone should define the following boundaries clearly.

### `platform`

Responsible for:

- native window/view ownership
- Cocoa/UIKit lifecycle
- size, scale, safe-area, and visibility events
- host embedding concerns

Not responsible for:

- maintaining a second GPU device stack
- paint traversal
- Skia rendering logic

### `render`

Responsible for:

- Dawn/WebGPU instance, adapter, device, and queue ownership
- presentable surface creation or attachment
- frame lifecycle:
  - acquire drawable/texture
  - prepare render target
  - submit GPU work
  - present
- integration boundary for Skia Graphite

Not responsible for:

- owning widget trees
- input dispatch
- platform window policy

### `view`

Responsible for:

- layout
- invalidation
- paint traversal
- input handling
- accessibility semantics

Not responsible for:

- native Apple layer ownership
- GPU device creation
- presentation policy

## Required Design Decisions

The implementation milestone should resolve these questions explicitly:

1. Does `GpuSurface` remain the public abstraction, or is it replaced by something more truthful?
2. Where does the native drawable come from on macOS?
3. Where does the native drawable come from on iOS?
4. How does Skia Graphite target the presentable surface rather than an orphaned offscreen render target?
5. Where do resize, scale-factor, and safe-area updates enter the frame lifecycle?
6. What is the minimum supported Apple render path that can be described as real, even if still experimental?

These answers should be written down in the implementation results, not left implicit in code.

## Deliverables

### 1. Apple render architecture note

Produce a short implementation-facing architecture note describing:

- the chosen native surface types
- the ownership model
- the frame lifecycle
- the difference between current state and target state

This may live in an existing render guide or as a dedicated internal doc, but it must be easy to find.

### 2. Explicit present-path design

Document the end-to-end present path:

- native drawable source
- Dawn/WebGPU surface ownership
- Skia Graphite target binding
- submit/present boundary

The milestone is incomplete if rendering still conceptually ends at an offscreen texture with no explicit presentation step.

### 3. Apple platform surface contract

Create or update documentation so that macOS and iOS render contracts are explicit.

At minimum the docs should answer:

- how a renderable surface is created or attached
- how size and scale are updated
- how view embedding works
- what is currently supported
- what is still experimental

### 4. Validation plan and evidence

Define how Apple rendering is validated.

The milestone should include validation evidence or a validation checklist for:

- macOS view-backed rendering
- iOS view-backed rendering
- resize correctness
- Retina scale correctness
- safe-area correctness on iOS
- orientation or geometry change handling on iOS
- background/foreground or suspend/resume behavior where applicable

### 5. Honest docs and support manifests

Update the render-facing docs and manifests so they make only claims the code can support.

At minimum this includes:

- `docs/guides/modules/render.md`
- `docs/status/support-matrix.yaml`
- any Apple platform guide that mentions Metal rendering

If parts of the Apple path remain offscreen-only or partial, the docs should say so directly.

## Suggested Implementation Areas

The implementation agent will likely need to inspect at least:

- `core/render/`
- `core/platform/`
- Apple view host code on macOS and iOS
- render docs and support manifests

This spec does not prescribe exact file edits, but the result should remove ambiguity in the Apple render story.

## Validation Criteria

The Apple render path should be evaluated against these criteria.

### Architecture criteria

1. There is one coherent GPU ownership path per render context.
2. Native Apple presentation surfaces are explicit, not implied.
3. Skia Graphite targets a presentable surface path, not just an offscreen surface.
4. The responsibilities of `platform`, `render`, and `view` are understandable from code and docs.

### Runtime criteria

1. A macOS render surface can be created and described accurately.
2. An iOS render surface can be created and described accurately.
3. Resize and scale changes are handled intentionally.
4. iOS safe-area behavior is accounted for intentionally.
5. The frame lifecycle includes a real presentation step.

### Documentation criteria

1. Render docs no longer imply acquire/present behavior unless it actually exists.
2. Apple support claims in manifests match the real path.
3. Experimental status is used honestly where integration is incomplete.

## Honest Status Language

Until this milestone is complete, status language should distinguish between these states:

- `offscreen foundation exists`
- `native surface helper exists`
- `presentable rendering path exists`
- `validated on device or host`

Avoid compressing all of those into a single label like `experimental Metal rendering` if the underlying capability is actually narrower.

Examples of acceptable honesty:

- "Dawn/Skia device bring-up exists on macOS, but onscreen presentation path is still incomplete."
- "iOS has a CAMetalLayer-backed surface helper, but it is not yet the default active render path."
- "Apple rendering is experimental and not yet validated as a complete presentable pipeline."

## Acceptance Criteria

This milestone is done when:

1. The Apple render architecture is explicitly documented.
2. macOS and iOS native surface APIs are named and their lifecycle responsibilities are clear.
3. The render subsystem has one coherent ownership model rather than split or duplicate GPU bootstrap paths.
4. The present path is explicit from native surface to final present.
5. Validation criteria exist and are actionable.
6. Render docs and support manifests describe the Apple path honestly.

## Suggested Prompt For An Agent

```text
Implement the Apple render milestone described in planning/apple-render-surface-integration-spec.md.

Use these planning docs for context:
- planning/metal-integration-review-and-mpp-takeaways.md
- planning/12-rendering-strategy.md

Goals:
- define and implement one coherent Apple render surface model for macOS and iOS
- clarify ownership boundaries between platform, render, and view
- make the present path explicit
- make docs and support manifests honest about current Apple render maturity

Requirements:
- do not broaden this into Metal Performance Primitives or Apple-only compute work
- do not optimize for speculative performance before presentation correctness exists
- preserve clear separation between platform lifecycle, render lifecycle, and view/layout logic
- verify the resulting Apple render story is technically defensible

Before editing, inspect the current Apple render code paths, native view hosts, and render docs. Then implement the smallest coherent milestone that satisfies the acceptance criteria.
```

## Bottom Line

This milestone is about making Pulp's Apple GPU story real and trustworthy.

The target is not "faster Metal."

The target is:

- one honest architecture
- one real present path
- one credible set of docs

so that future Apple-specific optimization work has a stable foundation instead of moving targets.
