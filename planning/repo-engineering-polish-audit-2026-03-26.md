# Repo Engineering Polish Audit

Date: 2026-03-26
Audience: maintainers
Purpose: identify where the repo feels transitional, over-extended, or "AI slop"-adjacent, and suggest how to make the codebase feel sharper to experienced engineers without changing the product direction.

## Executive Summary

Pulp has real engineering value. The strongest parts of the repo do not look like slop:

- the public processor contract in `core/format/include/pulp/format/processor.hpp`
- the parameter/state surface in `core/state/include/pulp/state/store.hpp`
- the core view tree abstraction in `core/view/include/pulp/view/view.hpp`

Those read like intentional framework design.

What does feel weak is not mostly low-level code quality. It is repo curation quality. The codebase often reads like several successful implementation pushes were never followed by a consolidation pass. That creates a specific kind of "AI slop" smell:

- ambitious surface area
- transitional abstractions left public
- stubs and placeholders visible in product-facing areas
- trust drift between status language and actual implementation maturity
- multiple ways to do the same thing surviving at once

That is fixable. The repo does not need a rewrite. It needs stronger curation pressure.

This is also a polish-and-trust audit, not a full correctness audit. In a real-time audio framework, concurrency correctness across audio/UI/main-thread boundaries is at least as important as repo polish and deserves its own dedicated review.

## What Already Feels Strong

These are the parts that help the repo land well with experienced engineers:

### 1. Core processing model

`core/format/include/pulp/format/processor.hpp` is clear about:

- plugin category
- bus model
- prepare/process lifecycle
- thread-safety expectations
- format-adapter role

That kind of API surface feels deliberate rather than generated.

### 2. State model

`core/state/include/pulp/state/store.hpp` has a clean conceptual center:

- one source of truth
- lock-free value access
- gesture callbacks
- explicit serialization boundary

It reads like a framework primitive, not just an implementation detail.

### 3. View model

`core/view/include/pulp/view/view.hpp` is a credible base abstraction:

- simple tree semantics
- clear layout/hit-test/paint responsibilities
- focus and accessibility hooks

It is not perfect, but it feels like real framework code.

## Where The Repo Feels Less Engineered

## 1. Trust drift is still the biggest reputational risk

The repo often sounds more consolidated than it actually is.

Examples:

- `planning/STATUS.md:3` says `640 tests`, while `planning/STATUS.md:39` says `592 pass (macOS; 540/540 validated on Linux VM)`.
- `planning/STATUS.md:27-37` marks large phases as complete, while several repo surfaces are still explicitly stubbed, placeholder-backed, or experimental.
- example and docs surfaces still expose internal source-file includes, such as:
  - `examples/pulp-gain/main.cpp:6`
  - `docs/guides/getting-started.md:105`

Why this matters:

Senior engineers are usually tolerant of ambitious work in progress. They are not tolerant of feeling misled. When the status story gets ahead of the code, the whole repo starts to feel less trustworthy.

Improvement direction:

- require every "complete" or "usable" claim to survive a hostile read from someone who did not write the code
- make the status tracker numerically and semantically consistent before adding more triumphal language
- separate "implemented somewhere" from "productized, documented, and supported"

## 2. Render integration still looks transitional, even where the code has improved

The current render code is better than the earlier version that had two separate Dawn device stacks. But it still reads as an abstraction under active renegotiation rather than a settled subsystem.

Examples:

- `core/render/include/pulp/render/gpu_surface.hpp:29-34` tunnels platform-specific native handles through a raw `void*`
- `core/render/include/pulp/render/gpu_surface.hpp:53-62` exposes raw `void*` handle escape hatches for Dawn device/queue/instance/texture
- `core/render/src/gpu_surface_dawn.cpp:171-183` depends on those opaque handle bridges for Skia integration
- `core/render/src/skia_surface.cpp:84-125` has a presentable path plus an offscreen fallback, which is useful, but also signals that the ownership model is still in flux

None of this is inherently wrong. But it feels transitional. The code is explaining itself very hard because the abstraction still leaks.

Why this matters:

Engineers are impressed by subsystems that look inevitable. They become skeptical when an abstraction needs raw pointer tunneling, elaborate lifetime commentary, and multiple fallback identities to remain coherent.

Improvement direction:

- keep tightening the render contract until `GpuSurface` and `SkiaSurface` no longer need "same-module secret handshake" APIs
- prefer one strong render story over multiple partially overlapping modes
- make offscreen rendering, presentable rendering, and fallback rendering explicit modes rather than side effects of the same abstraction

## 3. Placeholder and stub debt is too visible

There is a difference between strategically incomplete and visibly placeholder-backed.

Examples:

- `core/midi/CMakeLists.txt:42-44` writes `src/placeholder.cpp` into the source tree during configure
- `ship/src/appcast.cpp:157-168` has a signing stub in a shipping-related subsystem
- `ship/platform/win/codesign_win.cpp:41-91` exposes a broad API surface where several functions are effectively non-implemented
- `core/audio/include/pulp/audio/workgroup.hpp:129-133` returns success on Windows as a stub
- `core/platform/platform/linux/clipboard_linux.cpp:1-3` calls itself a clipboard stub, but actually implements only in-process storage

This is the kind of thing that makes a repo feel more generated than curated, even when the code is understandable.

Why this matters:

Stubs are fine. Stubs that look indistinguishable from supported features are not. They train readers to doubt the repo's labels.

Improvement direction:

- stop writing placeholder source files into tracked source directories at configure time
- centralize "not implemented on this platform" behavior into clearly named files or modules
- avoid success-shaped stubs like `return true; // stub`
- make unsupported or fake implementations impossible to mistake for real platform support

## 4. The public/private boundary is still too porous

This is one of the fastest ways to make a framework feel pre-product.

Examples:

- `examples/pulp-gain/main.cpp:6` includes `../../core/format/src/standalone.hpp`
- `examples/pulp-effect/au_v2_entry.cpp:2` includes `../../core/format/src/au_v2_adapter.cpp`
- `docs/guides/getting-started.md:105` tells users to include `core/format/src/au_v2_adapter.cpp`

Even if this works, it sends the wrong signal:

- the public API boundary is not stable
- internal implementation files are part of expected usage
- examples are validating internals rather than teaching supported integration

Improvement direction:

- treat examples and docs as enforcement points for public API quality
- ban consumer-facing examples from including `src/`
- if something requires internal includes, either promote it properly or stop presenting it as standard usage

## 5. Some tooling surfaces are still too stringly-typed

The CLI documentation reader works, but it still feels like a hand-rolled convenience layer rather than a dependable tool surface.

Examples:

- `tools/cli/pulp_cli.cpp:440-441` defines a "simple YAML line parser"
- `tools/cli/pulp_cli.cpp:617-761` manually walks support data with indentation assumptions
- `tools/cli/pulp_cli.cpp:777-907` manually parses command manifests with state flags like `in_subcommands`, `in_args`, `in_sub_args`

This kind of code is normal in a prototype. It looks much less convincing once it sits in the main CLI of a framework that is presenting itself as mature.

Why this matters:

Experienced engineers do not mind scripts. They do mind brittle scripts masquerading as infrastructure.

Improvement direction:

- prefer one typed source of truth over repeated ad hoc parsing passes
- either use a real parser or generate simpler machine-friendly indices explicitly for the CLI
- reserve hand-rolled parsing for build-time tooling, not user-facing command surfaces

## 6. A few critical adapter paths still show unfinished assumptions

These are the kinds of details senior engineers notice immediately.

Example:

- `core/format/src/vst3_adapter.cpp:124-125` sets `ctx.input_channels = 2` and `ctx.output_channels = 2` with a TODO to query the bus arrangement

That is not just a missing edge case. In a plugin framework, bus/layout correctness is central. A TODO in that part of the adapter makes the surface feel less production-ready than the surrounding docs/status language suggests.

Improvement direction:

- identify any adapter path where a default constant currently stands in for host truth
- treat these as "credibility bugs," not just future enhancements
- maintain a small list of format-adapter invariants that must be true before claiming maturity

## 7. The repo still contains too many "completion-shaped" surfaces

This is the broader pattern underneath the individual examples above.

Examples:

- phase completion language in `planning/STATUS.md`
- shipping APIs with partial implementation
- platform layers with mixed real/stub behavior
- examples that prove internals instead of product surfaces

Why this matters:

AI-generated or agent-accelerated repos often fail in the same way: they accumulate convincing top-layer artifacts faster than their internal contracts are stabilized. That is the main smell here. Not syntax quality. Not lack of tests. Contract discipline.

## Important Missing Dimension

This memo is intentionally focused on engineering impression and repo polish.

It does not answer the deeper correctness question an experienced audio engineer will also care about:

- are the lock-free and cross-thread primitives actually being used correctly under real-time constraints?

That includes things like:

- relaxed atomic ordering choices
- audio/UI handoff correctness
- `SeqLock`, `TripleBuffer`, FIFOs, and listener dispatch behavior
- whether the documented thread model matches the implementation in adapters and UI bridges

If Pulp wants to impress senior audio engineers specifically, that should be a separate audit alongside this one rather than silently assumed by a polish pass.

## Recommendations

## 1. Do a "credibility pass," not just a cleanup pass

The right framing is:

- what would make a skeptical senior engineer trust this repo faster?

That usually means:

- fewer claims
- sharper boundaries
- clearer support labels
- fewer transitional escape hatches

## 2. Harden the public API boundary aggressively

This is the highest-leverage impression win.

If examples and docs only use supported headers and stable entry points, the whole framework feels more intentional immediately.

## 3. Quarantine stubs and placeholders

Do not let partial implementations sit next to real ones without unmistakable labeling.

Good repos are not judged for having stubs. They are judged for how easy it is to tell the difference.

## 4. Simplify the render story until it feels inevitable

The render subsystem is strategically important, so it cannot feel like a set of cooperating experiments forever.

The current direction is improving, but it still needs one more consolidation pass before it reads as "clean architecture" instead of "active integration work."

## 5. Treat repo storytelling as part of engineering quality

`STATUS.md`, examples, manifests, and CLI behavior are not marketing. They are part of the product for contributors.

If those surfaces are sharper, experienced engineers will assume the code behind them is sharper too.

## A Good Internal Bar To Aim For

A strong repo impression would look like this:

- a new engineer can identify the supported public API without guessing
- every major subsystem has one obvious ownership story
- platform support is easy to classify as real, partial, or absent
- examples demonstrate the intended architecture, not internal shortcuts
- tooling looks dependable, not hand-assembled
- status language never needs apology or explanation

## Bottom Line

Pulp does not primarily have an "AI slop" coding problem.

It has a curation problem caused by shipping too much surface area before doing the narrowing pass that makes a framework feel authored.

The encouraging part is that the underlying codebase already contains real, impressive material.

The work now is to make the repo present that material with the same discipline the best subsystems already show.
