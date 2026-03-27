# JUCE Ecosystem Porting Path

Date: 2026-03-25
Purpose: define a practical strategy for bringing useful JUCE-adjacent libraries into Pulp, without pretending Pulp should become JUCE-compatible at the API level.

## Scope

This is a repo-surface assessment only.

It is based on public repository structure, README/docs positioning, stated dependencies, and licensing signals.
It is not a code audit.

Reference projects used for this assessment:

- `Chowdhury-DSP/chowdsp_utils`
- `FigBug/Gin`
- `signalsmith-audio/dsp`
- `Chowdhury-DSP/chowdsp_wdf`
- `ffAudio/foleys_gui_magic`
- `ImJimmi/JIVE`
- `jatinchowdhury18/RTNeural`
- `cycfi/q`

## Executive Summary

Bringing useful JUCE ecosystem projects into Pulp is possible, but only selectively.

The clean dividing line is this:

- framework-neutral DSP and utility libraries can often be integrated or ported with low friction
- JUCE-light helper libraries can sometimes be selectively ported module by module
- JUCE-native UI, state, and plugin-framework layers are usually not "portable" in the normal sense and should be treated as rewrite-level efforts

Pulp should not try to become a JUCE-compatibility layer.

That would be expensive, leaky, and strategically confusing.

Pulp should instead support three migration modes:

1. direct integration of framework-neutral libraries
2. selective source porting of useful modules
3. concept-level reimplementation for JUCE-deep systems

## The Core Question

When someone asks, "Can I bring this JUCE module to Pulp?", the right answer is not yes or no.

The right answer is:

- what percentage of the library is plain C++ and DSP?
- what percentage is glued to JUCE types, threading, UI, state, or plugin contracts?
- is the value in the algorithms, or in the framework integration?

If the value is mostly algorithms, data structures, math, or real-time primitives, there is usually a path.

If the value is mostly `juce::Component`, `AudioProcessor`, `AudioProcessorValueTreeState`, `ValueTree`, `LookAndFeel`, attachment systems, or JUCE DOM/state abstractions, the path is usually "rewrite around the idea" rather than "port the code."

## Portability Tiers

### Tier A: Integrate Directly

Definition:

- plain C++ library
- no meaningful dependency on JUCE runtime or UI model
- useful as an external dependency without changing Pulp architecture

What Pulp should do:

- allow easy vendoring or FetchContent integration
- provide docs for real-time-safe third-party integration
- wrap only if a Pulp-native facade adds real value

### Tier B: Selective Port / Thin Adapter

Definition:

- library has useful modules that are mostly plain C++
- some modules depend on JUCE, but not all
- value is high enough to justify targeted adaptation

What Pulp should do:

- port only the modules that fill real capability gaps
- avoid dragging JUCE type semantics into Pulp
- rebuild tests and examples against Pulp contracts

### Tier C: Concept Port / Rewrite

Definition:

- library is organized around JUCE-specific UI, state, or plugin abstractions
- the interesting part is the product idea or workflow model, not the existing integration layer

What Pulp should do:

- capture the design idea
- rebuild it around `pulp-view`, `pulp-state`, `pulp-events`, and `pulp-format`
- do not chase source-level compatibility

### Tier D: Do Not Port

Definition:

- too coupled to JUCE internals
- low strategic value for Pulp
- licensing or maintenance burden outweighs benefit

What Pulp should do:

- document why it is out of scope
- if useful, provide migration notes for developers leaving that dependency behind

## Candidate Assessments

## 1. signalsmith-audio/dsp

Surface read:

- presented as a C++11 header-only DSP library
- MIT licensed
- includes headers like delay, envelopes, FFT, filters, spectral, windows
- positioned as a standalone DSP support library, not a JUCE add-on

Assessment:

- Tier A

Why it fits:

- this is the easiest class of ecosystem project for Pulp to adopt
- it is already framework-neutral
- it does not ask Pulp to imitate JUCE

Likely use in Pulp:

- direct third-party dependency
- optional import for teams that want specific DSP building blocks without waiting for native Pulp equivalents

What Pulp should do to support this:

- document a clean third-party DSP integration path
- provide sample wrappers for using foreign DSP blocks inside `pulp::format::Processor`
- document audio buffer interop expectations clearly

Recommendation:

- excellent pilot candidate

## 2. Chowdhury-DSP/chowdsp_wdf

Surface read:

- header-only Wave Digital Filter library
- BSD-3-Clause licensed
- real-time C++ WDF circuit modelling library
- optional SIMD dependency surface

Assessment:

- Tier A

Why it fits:

- algorithm library
- narrow scope
- strong value for analog-style DSP work
- not trying to replace a framework layer

Likely use in Pulp:

- direct dependency or curated optional integration
- example plugin showing Pulp host/parameter/state around an external WDF engine

Recommendation:

- one of the best migration-path examples because it is high value and low architectural risk

## 3. jatinchowdhury18/RTNeural

Surface read:

- lightweight real-time neural inferencing engine
- BSD-3-Clause licensed
- standalone C++ library with optional backends
- explicitly targeted at real-time systems and audio use

Assessment:

- Tier A

Why it fits:

- not really a JUCE porting problem at all
- it is the kind of third-party library Pulp should be good at hosting

Likely use in Pulp:

- direct dependency
- optional ML inference module examples
- future fit with `pulp-signal` and advanced examples

Recommendation:

- strong candidate for a "Pulp external DSP cookbook" rather than a port

## 4. cycfi/q

Surface read:

- cross-platform C++ audio DSP library
- MIT licensed
- stable API
- broad DSP scope with examples, docs, and tests

Assessment:

- Tier A to Tier B

Why it fits:

- framework-neutral and portable in principle
- broad enough that selective use is probably smarter than wholesale adoption

Main caution:

- broad DSP libraries can overlap heavily with Pulp's own long-term `signal` ambitions
- if imported carelessly, they can blur Pulp's native API story

Recommendation:

- good dependency candidate for user projects
- selective reference candidate for Pulp
- not an obvious "vendor the whole thing into core" candidate unless it fills a very specific gap

## 5. Chowdhury-DSP/chowdsp_utils

Surface read:

- positioned as JUCE modules
- explicitly supports both JUCE usage and a non-JUCE path for some DSP modules
- common and DSP modules appear separable from GUI/plugin layers
- repository uses mixed licenses by module, including BSD and GPLv3

Assessment:

- overall: Tier B
- some modules: Tier A-like
- GUI/plugin modules: Tier C or D

What looks promising:

- common utilities
- data structures
- serialization/logging/math helpers, where they are not redundant with Pulp
- DSP-focused modules that are not tightly bound to JUCE types

What looks risky or rewrite-heavy:

- GUI modules
- plugin-base layers
- plugin-state and attachment systems
- preset/UI/host integration layers

Main caution:

- the mixed-license surface means adoption cannot be discussed abstractly
- each module needs a license and dependency screen before any technical decision

Practical migration model:

1. classify modules individually, not at repo level
2. prefer BSD/common/plain-C++ modules first
3. only port modules that fill a real Pulp gap
4. do not import JUCE-flavoured plugin-state/UI abstractions into Pulp core

Recommendation:

- strong selective-port candidate
- bad candidate for wholesale adoption

## 6. FigBug/Gin

Surface read:

- comprehensive extra modules for JUCE
- explicitly depends on JUCE 7+
- includes core, DSP, GUI, graphics, plugin, location, metadata modules
- installation flow is JUCE-centric and assumes JUCE headers/modules

Assessment:

- repo overall: Tier C
- some DSP pieces: Tier B at best

Why it is harder:

- it presents as an extension layer on top of JUCE rather than a framework-neutral library
- even its module packaging is JUCE-first

What might be worth evaluating later:

- isolated DSP or utility pieces with minimal JUCE type dependence

What should not be a near-term goal:

- `gin_gui`
- `gin_plugin`
- anything depending on JUCE-wide assumptions through `JuceHeader.h`

Recommendation:

- not a good first migration target
- maybe useful later as a source of ideas for isolated DSP features

## 7. ffAudio/foleys_gui_magic

Surface read:

- GUI builder module for JUCE
- DOM model plus CSS-like styling
- drag-and-drop editor
- AudioProcessor parameter connection model
- module packaging is explicitly `juce_add_module()`-oriented

Assessment:

- Tier C

Why:

- the interesting part is the authoring model, not the concrete implementation surface
- it is deeply tied to JUCE plugin/UI assumptions

What Pulp should take from it:

- the workflow idea
- the visual editor idea
- the parameter-binding/editor-inspector story

What Pulp should not take from it:

- any expectation of source-level portability

Recommendation:

- do not plan a port
- do plan a Pulp-native design/editor workflow if that product direction remains important

## 8. ImJimmi/JIVE

Surface read:

- JUCE UI extension bundle
- declarative UI approach using JUCE `ValueTree`, `var`, and `DynamicObject`
- explicitly framed as a more modern approach to JUCE UI development

Assessment:

- Tier C

Why:

- the architecture is explicitly built on JUCE state/object systems
- this is exactly the class of project where the concept matters more than the code

What Pulp should take from it:

- lessons about declarative UI authoring
- layout/style-sheet ergonomics

What Pulp should not do:

- build `ValueTree` compatibility to make JIVE portable

Recommendation:

- rewrite-only territory

## What This Means for Pulp

The answer to "can we bring JUCE modules to Pulp?" is:

- yes for framework-neutral DSP and utility libraries
- sometimes for selectively isolated JUCE-adjacent modules
- no as a general compatibility promise

That is a healthy answer.

It means Pulp can benefit from the ecosystem without compromising its architecture.

## What Pulp Should Build to Make Migration Easier

## 1. A Third-Party DSP Integration Story

Pulp should make it easy for users to bring in plain C++ DSP libraries without ceremony.

Needed:

- clear CMake integration guidance
- examples showing external DSP blocks inside a Pulp processor
- explicit real-time-safety guidelines for foreign libraries

## 2. A Porting Guide from JUCE Concepts to Pulp Concepts

Pulp should document the conceptual map:

- `juce::AudioProcessor` -> `pulp::format::Processor`
- `juce::AudioBuffer` -> `pulp::audio::Buffer` / `BufferView`
- `AudioProcessorValueTreeState` -> `pulp::state::StateStore` + bindings
- `juce::Component` -> `pulp::view::View` and widgets
- `juce::MessageManager` patterns -> `pulp::events::EventLoop`

This is more valuable than pretending APIs match.

## 3. A Stable Low-Level "Porting Surface"

To make selective ports easier, Pulp should stabilize:

- buffer and buffer-view contracts
- parameter/state binding contracts
- timer/event handoff patterns
- file/watch/logging/json utilities
- optional portability helpers for common DSP idioms

## 4. A Small Compatibility Helpers Layer

Not full JUCE emulation.

Just enough helpers to lower friction:

- audio buffer adapters
- normalized parameter helpers
- sample-rate and block-size utilities
- attachment-style binding utilities

This should live as a porting/helper layer, not as the center of Pulp's architecture.

## 5. A "Ports" or "Interop" Area

If Pulp gets serious about migration, it should keep these efforts contained:

- `third_party/`
- `extras/ports/`
- or a similar non-core area

That keeps the core clean and prevents imported abstractions from polluting the main API.

## Recommended Pilot Projects

## P0: Do First

- `signalsmith-audio/dsp`
- `chowdsp_wdf`

Why:

- best effort-to-value ratio
- low architectural risk
- no need for JUCE compatibility

## P1: Do Selectively

- carefully chosen `chowdsp_utils` modules
- maybe isolated `Gin` DSP utilities if a specific gap exists

Why:

- there is real value, but the repo/module boundary matters
- needs license and dependency screening first

## P2: Treat as Product Inspiration, Not Port Targets

- `foleys_gui_magic`
- `JIVE`

Why:

- they are valuable as examples of UI authoring models
- but not sensible source-port candidates

## P3: Good Ecosystem Targets Even Though They Are Not JUCE Ports

- `RTNeural`
- `cycfi/q`

Why:

- these show the kind of external library Pulp should host well
- even if they are not "JUCE module migrations," they are useful benchmarks for Pulp's interop posture

## Recommended Plan

### Phase 1: Define the Migration Contract

Deliverables:

- doc: "bringing external DSP into Pulp"
- doc: "JUCE concept map to Pulp"
- one example of external DSP integration in a simple Pulp processor

### Phase 2: Prove the Low-Risk Path

Deliverables:

- integrate one framework-neutral DSP library as a sample or optional dependency
- validate build, tests, and documentation experience

### Phase 3: Prove the Selective-Port Path

Deliverables:

- choose one small BSD-style `chowdsp_utils`-class module
- port it into a non-core area
- rebuild tests around Pulp
- document the work required and what made it hard

### Phase 4: Decide What Not to Port

Deliverables:

- explicit decision record stating that Pulp will not pursue wholesale JUCE UI/plugin compatibility
- list of concept-only inspirations vs source-port candidates

## Bottom Line

Pulp can absolutely have a migration pathway.

But the pathway should be:

- "portable DSP and utility modules come over cleanly"
- "selected JUCE-light modules can be ported deliberately"
- "deep JUCE UI/plugin systems get reimagined, not emulated"

That is a realistic and strategically coherent position.

## Sources

- `chowdsp_utils`: GitHub repo and README surface: https://github.com/Chowdhury-DSP/chowdsp_utils
- `Gin`: GitHub repo and README surface: https://github.com/FigBug/Gin
- `signalsmith-audio/dsp`: GitHub repo and README surface: https://github.com/signalsmith-audio/dsp
- `chowdsp_wdf`: GitHub repo and README surface: https://github.com/Chowdhury-DSP/chowdsp_wdf
- `foleys_gui_magic`: GitHub repo and README surface: https://github.com/ffAudio/foleys_gui_magic
- `JIVE`: GitHub repo and README surface: https://github.com/ImJimmi/JIVE
- `RTNeural`: GitHub repo and README surface: https://github.com/jatinchowdhury18/RTNeural
- `cycfi/q`: GitHub repo and README surface: https://github.com/cycfi/q
