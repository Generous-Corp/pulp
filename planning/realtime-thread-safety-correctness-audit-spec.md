# Real-Time And Thread-Safety Correctness Audit Spec

Date: 2026-03-26
Audience: review agent or senior engineer performing a correctness audit
Status: proposed follow-up audit

## Purpose

Define a dedicated audit for the highest-risk correctness surface in Pulp:

- real-time safety on the audio thread
- thread-safety across audio, UI, host, background, and platform threads
- correctness of lock-free and atomic communication patterns

This is intentionally separate from:

- repo polish / contributor-trust audits
- general docs audits
- format-validator or golden-audio validation plans

Those matter, but they do not answer the core question a senior audio engineer will ask:

- are the framework's real-time and cross-thread guarantees actually correct?

## Why A Separate Audit Exists

Pulp already has:

- a validation plan in [15-validation-plan.md](/Users/danielraffel/Code/pulp/planning/15-validation-plan.md)
- documented synchronization guidance in [runtime.hpp](/Users/danielraffel/Code/pulp/core/runtime/include/pulp/runtime/runtime.hpp)
- style and agent rules covering audio-thread safety in [code-style.md](/Users/danielraffel/Code/pulp/docs/policies/code-style.md) and [agent-contribution-rules.md](/Users/danielraffel/Code/pulp/docs/policies/agent-contribution-rules.md)

But those documents are not the same as an adversarial correctness audit.

This audit should verify:

- whether the documented thread model matches actual implementation
- whether the chosen primitives are appropriate for each boundary
- whether atomic ordering choices are justified and correctly applied
- whether "safe on the audio thread" claims survive close reading

## Primary Questions

The audit must answer these questions directly.

1. Is `Processor::process()` and everything it transitively relies on free of allocation, locks, blocking, exceptions, and I/O on the real-time path?
2. Are documented thread boundaries accurate across audio, UI, host, event-loop, background-loader, and platform callback paths?
3. Are `std::atomic`, `SeqLock`, `TripleBuffer`, and `SPSCQueue` used correctly for the specific data flows they are carrying?
4. Are memory ordering choices correct and justified, especially where relaxed atomics are used?
5. Are listeners, gesture callbacks, hot-reload signals, and platform bridges ever invoked from the wrong thread or with unsafe assumptions?
6. Are there any success-shaped stubs or fallback paths that undermine thread-safety guarantees?

## Scope

This audit should review the following surfaces.

### 1. Runtime primitives

- `SeqLock`
- `TripleBuffer`
- `SPSCQueue`
- logging / assertions where they are reachable from critical paths

Questions:

- are the primitives themselves correct?
- do their documented guarantees match implementation?
- are they used in ways that fit their design assumptions?

### 2. Parameter and state path

- `ParamValue`
- `StateStore`
- normalization / modulation paths
- gesture begin/end callbacks
- listener dispatch

Questions:

- are audio-thread reads and UI/host writes actually safe?
- are relaxed atomics sufficient for the usage pattern?
- are callbacks or listener flows ever re-entering unsafe code paths?

### 3. Audio/UI bridge path

- meter data flow
- visualization data flow
- hot-reload notifications that affect UI while audio is running
- any bridge from DSP data to UI state

Questions:

- is the chosen primitive appropriate?
- can the UI stall safely?
- can intermediate values be dropped safely?
- are there hidden allocations or ownership hazards?

### 4. Background-threaded audio utilities

- buffering/streaming readers
- audio file loading helpers
- any async resource loading

Questions:

- does the audio thread ever block on background work?
- do shared counters and completion flags use correct ordering?
- are shutdown and teardown races handled correctly?

### 5. Format adapters

- CLAP
- VST3
- AU / AUv3
- standalone host path
- LV2 where relevant

Questions:

- do host callbacks enter Pulp on the documented thread?
- are state/parameter changes forwarded safely?
- are platform-specific render/audio hooks making stronger assumptions than the framework contract allows?

### 6. View, event, and platform glue

- event loop
- window/view hosts
- plugin editor hosts
- drag/drop, clipboard, accessibility hooks where they interact with shared state

Questions:

- which thread owns each operation?
- are cross-thread dispatches explicit and safe?
- do platform callbacks ever mutate shared state without the documented primitive?

### 7. Render-adjacent concurrency

This is not primarily a render-performance audit, but it should inspect:

- repaint scheduling
- main-thread render dispatch
- handoff between UI invalidation and rendering

Questions:

- are render/UI interactions explicit about thread ownership?
- do any atomic flags or handoffs have questionable semantics?

## Non-Goals

Not required in this audit:

- subjective DSP quality review
- general architectural taste review
- docs style improvements beyond correcting incorrect thread claims
- deep platform API correctness unrelated to threading
- performance benchmarking except where it exposes blocking or unsafe synchronization

## Audit Method

This audit should be adversarial and evidence-based.

It should not stop at:

- grep for `std::mutex`
- grep for `std::atomic`
- trusting comments

It should instead build a concrete thread model and test it against the code.

### Step 1: Build a thread-boundary inventory

List the main thread domains used by the framework:

- audio thread
- UI thread
- host/main thread
- event-loop thread
- background worker thread(s)
- platform callback threads where distinct

For each important subsystem, identify:

- which thread enters it
- which thread mutates state
- which thread reads state
- what primitive coordinates the boundary

### Step 2: Audit primitive correctness

For each lock-free or atomic primitive:

- identify its intended contract
- inspect implementation
- inspect each major call site class
- check whether the call sites satisfy the primitive's assumptions

This includes:

- single-writer vs multi-writer assumptions
- snapshot/coherency assumptions
- latest-value vs ordered-stream semantics
- shutdown / teardown behavior

### Step 3: Audit real-time safety claims

For every path documented or implied to be real-time-safe:

- inspect for allocation
- inspect for locks
- inspect for blocking syscalls
- inspect for file/network/console I/O
- inspect for dynamic dispatch or callback flows that escape into unsafe code

### Step 4: Check memory ordering rationale

Where atomics are used:

- identify the chosen ordering
- identify the dependency the ordering is meant to enforce
- determine whether that dependency is real, missing, or overstated

This is especially important where comments rely on:

- relaxed ordering
- acquire/release on flags
- implied single-writer discipline

### Step 5: Verify docs against implementation

Check that thread-safety claims in:

- docs
- headers
- style guides
- planning docs

match the implementation reality.

This audit should explicitly call out any case where:

- docs say a path is audio-thread-safe but implementation is unclear
- docs imply a thread boundary that code does not enforce
- a primitive is recommended in docs but used inconsistently in code

## Deliverables

### 1. Findings-first audit memo

The main output should be a review document with findings ordered by severity.

Each finding should include:

- file and line references
- the violated invariant or unsafe assumption
- why it matters for real-time or thread-safety correctness
- whether it is a proven bug, a likely bug, or a trust/documentation gap

### 2. Thread-boundary map

A compact map of:

- major threads
- major subsystems
- coordinating primitives

This should make it easy for future contributors to understand the actual concurrency model.

### 3. Primitive-usage matrix

For each approved primitive:

- intended use
- observed major uses
- correctness verdict
- follow-up actions if any

### 4. Hardening plan

If issues are found, produce a concrete hardening plan grouped by:

- correctness bugs
- unclear or risky assumptions
- documentation drift
- testing/verification gaps

## Recommended Files To Inspect

At minimum, the audit should inspect:

- `core/runtime/`
- `core/state/`
- `core/audio/`
- `core/view/`
- `core/format/`
- relevant platform hosts in macOS and iOS code
- current thread-model docs and policies

Particularly high-signal starting points include:

- [runtime.hpp](/Users/danielraffel/Code/pulp/core/runtime/include/pulp/runtime/runtime.hpp)
- [store.hpp](/Users/danielraffel/Code/pulp/core/state/include/pulp/state/store.hpp)
- [parameter.hpp](/Users/danielraffel/Code/pulp/core/state/include/pulp/state/parameter.hpp)
- [audio_bridge.hpp](/Users/danielraffel/Code/pulp/core/view/include/pulp/view/audio_bridge.hpp)
- [buffering_reader.hpp](/Users/danielraffel/Code/pulp/core/audio/include/pulp/audio/buffering_reader.hpp)
- [workgroup.hpp](/Users/danielraffel/Code/pulp/core/audio/include/pulp/audio/workgroup.hpp)

## Testing Expectations

This is an audit, not just a code read.

If feasible, the reviewer should also recommend or run:

- TSan where applicable on non-real-time testable paths
- targeted stress tests for lock-free primitives
- deterministic cross-thread tests for listener and bridge behavior
- teardown/race tests for background-reader and event-loop shutdown

The audit should also identify where TSan is useful and where it is insufficient because the hardest invariants are semantic rather than merely race-based.

## Acceptance Criteria

This audit is complete when:

1. The repo has a concrete thread-boundary map.
2. Major approved primitives have been reviewed both in implementation and in representative call sites.
3. Real-time safety claims have been checked against actual hot-path code.
4. Memory ordering choices have been reviewed where they materially affect correctness.
5. Findings are documented with file/line references and severity.
6. Any mismatch between docs and code is called out explicitly.
7. The result is actionable enough that a maintainer can prioritize hardening work.

## Suggested Prompt For An Agent

```text
Perform the dedicated real-time and thread-safety correctness audit described in planning/realtime-thread-safety-correctness-audit-spec.md.

Use these as context:
- planning/15-validation-plan.md
- core/runtime/include/pulp/runtime/runtime.hpp
- docs/policies/code-style.md
- docs/policies/agent-contribution-rules.md

Goals:
- verify whether Pulp’s documented thread model matches the implementation
- audit correctness of lock-free and atomic communication patterns
- identify any real-time safety violations or risky assumptions
- produce a findings-first audit memo with file/line references and a concrete hardening plan

Requirements:
- treat this as an adversarial correctness audit, not a style review
- do not assume comments are true without checking code
- distinguish proven bugs from risky assumptions and from documentation drift
- pay special attention to audio/UI/host/background thread boundaries

Deliverables:
- findings-first audit memo
- thread-boundary map
- primitive-usage matrix
- hardening recommendations
```

## Bottom Line

If Pulp wants to impress experienced audio engineers, it needs both:

- a clean, trustworthy repo surface
- and a defensible real-time correctness story

This audit exists to answer the second question directly.
