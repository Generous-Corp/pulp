# Clean-Room Contamination Risk Analysis

## Overview

Pulp is designed using a **clean-room methodology** to ensure that no copyrightable expression from the audited framework (a widely-used C++ audio application framework) is carried into Pulp's codebase. This document identifies contamination risks and establishes mitigation strategies.

---

## Clean-Room Methodology

Pulp's design follows a strict **three-layer separation**:

### Layer 1: Observed Capability
What the audited framework can do, described in behavioral terms. Example: "The audited framework provides a real-time audio callback that delivers interleaved or non-interleaved audio buffers to user code at a configurable sample rate and buffer size."

### Layer 2: Inferred Requirement
The generic, framework-agnostic requirement derived from the observed capability. Example: "An audio framework must provide a mechanism for user code to process audio in real time, receiving buffers of samples at the hardware-configured sample rate and block size."

### Layer 3: Proposed Pulp Design
An original design that satisfies the inferred requirement without borrowing any code, identifiers, comments, module names, class names, file layouts, API names, or proprietary terminology from the audited framework. Example: "Pulp's `pulp-audio` subsystem provides a `StreamCallback` protocol. Implementations receive a `SampleBlock` containing per-channel float arrays, a `TimeStamp`, and a mutable `OutputBlock` to fill."

**The cardinal rule**: No element from Layer 1 (observed implementation detail) may appear in Layer 3 (Pulp design). Only Layer 2 (generic requirements) may inform Layer 3.

---

## High-Risk Areas

These areas carry the greatest risk of accidental contamination because the audited framework's designs are distinctive and well-known in the audio development community.

### 1. Module / Subsystem Naming

**Risk**: The audited framework organizes code into modules with a distinctive naming convention (prefixed names like `juce_core`, `juce_audio_basics`, `juce_gui_basics`, etc.). Pulp must avoid mirroring this decomposition structure or naming pattern.

**Mitigation**: Pulp uses its own subsystem vocabulary derived from the project's identity:
- `pulp-runtime`, `pulp-events`, `pulp-canvas`, `pulp-view`, `pulp-audio`, `pulp-midi`, `pulp-signal`, `pulp-format`, `pulp-host`, `pulp-state`, `pulp-platform`, `pulp-build`, `pulp-test`, `pulp-ship`, `pulp-ci`, `pulp-claude`, `pulp-gpu`

The decomposition boundaries are driven by Pulp's architectural goals (e.g., separating event dispatch from UI rendering), not by mirroring the audited framework's module graph.

### 2. Class Naming Patterns

**Risk**: The audited framework uses distinctive type names such as `AudioProcessor`, `AudioProcessorEditor`, `AudioBuffer`, `Component`, `LookAndFeel`, `ValueTree`, and `AudioDeviceManager`. These names, while composed of common English words, form recognizable patterns associated specifically with that framework.

**Mitigation**: Pulp must use completely original type names. All public API names will be reviewed against the audited framework's header files before finalization. Pulp favors shorter, more modern naming conventions (e.g., `Renderer`, `Patch`, `StreamGraph`, `Widget`, `Theme`, `StateTree`).

### 3. CMake API Surface

**Risk**: The audited framework exposes CMake functions with distinctive names (e.g., `juce_add_plugin()`, `juce_add_gui_app()`, `juce_generate_juce_header()`). Pulp's build tooling must not replicate these function signatures.

**Mitigation**: Pulp's build API uses its own naming convention:
- `pulp_declare_plugin()`
- `pulp_declare_app()`
- `pulp_enable_format()`

The CMake API is designed from scratch around Pulp's module system, not adapted from the audited framework's approach.

### 4. Parameter System Design

**Risk**: The audited framework has a distinctive parameter system involving a tree-state object that binds parameters to a reactive data structure, with specific parameter types (ranged audio parameter, float parameter, int parameter, bool parameter, choice parameter). The behavioral requirements are generic (plugins need typed parameters with host automation and state serialization), but the API shape is framework-specific.

**Mitigation**: Pulp's `pulp-state` subsystem designs its parameter model independently:
- Parameters are defined declaratively using a `ParamSpec` descriptor
- Binding to host automation uses a `HostLink` protocol
- State serialization uses a separate `Snapshot` mechanism
- No monolithic "value tree state" object; instead, parameters are registered individually or in groups

### 5. Plugin Format Wrappers

**Risk**: The internal adapter patterns between the audited framework's central processor abstraction and VST3/AU/AAX/LV2 format APIs are implementation-specific. Studying or replicating these adapter patterns would constitute contamination.

**Mitigation**: Pulp implements all format wrappers from scratch using only:
- The VST3 SDK (MIT license) and its official documentation
- The AudioUnit SDK (Apache 2.0) and Apple's AU documentation
- The CLAP specification (MIT license)
- The LV2 specification (ISC license)
- The AAX SDK obtained directly from Avid (separate license required)

No adapter code from the audited framework will be referenced during implementation.

### 6. Styling / Theming Architecture

**Risk**: The audited framework uses a monolithic styling class with per-widget drawing methods. This is a distinctive architectural choice. Widgets delegate all their visual rendering to a single theming object that provides draw methods for every widget type.

**Mitigation**: Pulp uses a completely different styling architecture:
- A **token-based theme system** where visual properties (colors, spacing, radii, fonts) are defined as design tokens
- Widgets consume tokens from a `ThemeContext` rather than delegating drawing to a monolithic style class
- Custom rendering is achieved by overriding a widget's `paint()` method directly, not by subclassing a global style object
- CSS-like cascade semantics are supported for property inheritance

### 7. View / Native Window Model

**Risk**: The audited framework uses a specific hierarchy: a UI element class that maps to a "peer" abstraction which wraps native platform windows. While the general concept of a UI tree backed by native windows is universal, the specific API contract and naming are distinctive.

**Mitigation**: Pulp's `pulp-view` subsystem uses its own model:
- `Widget` is the base UI element (not "Component")
- `Surface` is the native window abstraction (not "Peer")
- `Compositor` manages the widget-to-surface mapping
- The relationship between widgets and surfaces follows Pulp's own rules (e.g., most widgets share a surface; only top-level containers and special overlays get their own)

### 8. Reactive Tree Data Structure

**Risk**: The audited framework provides a reactive, tree-structured data model with identifier-based keys and variant-typed values. This is a distinctive design used for plugin state, undo/redo, and inter-component communication.

**Mitigation**: Pulp's state management uses a different approach:
- `StateTree` in `pulp-state` is an observable, typed tree built on modern C++ (or Swift) value semantics
- Keys are strings, not a custom identifier type
- Values are strongly typed using `std::variant` or Swift enums, not a custom variant class
- Change observation uses a subscription/callback model, not a listener-inheritance pattern

### 9. Message Dispatching

**Risk**: The audited framework uses a singleton message manager that serializes all UI and inter-thread communication through a single dispatch point. This singleton pattern is distinctive.

**Mitigation**: Pulp's `pulp-events` subsystem uses a different architecture:
- An `EventLoop` abstraction that wraps the platform's native run loop (CFRunLoop, Win32 message loop, epoll/GLib)
- No global singleton; event loops are scoped to threads
- Cross-thread dispatch uses an `Executor` protocol with explicit queue semantics
- Timer callbacks are scheduled on a specific `EventLoop` instance, not through a global manager

### 10. Plugin Code Generation

**Risk**: The audited framework uses 4-letter plugin codes with a specific derivation algorithm for generating unique identifiers. This is framework-specific behavior.

**Mitigation**: Pulp generates plugin identifiers using standard format requirements:
- VST3 uses the official Steinberg UID generation approach
- AU uses the standard 4-character type/subtype/manufacturer codes as defined by Apple
- CLAP uses reverse-domain identifiers as specified by the CLAP standard
- No proprietary code-generation algorithm from the audited framework is used

---

## Medium-Risk Areas

These areas involve patterns that are somewhat distinctive but also have industry precedent outside the audited framework.

- **Audio callback interface shape**: The "process block" pattern with separate input/output buffer pointers is common across many frameworks (PortAudio, RtAudio, CoreAudio), but the specific method signatures and class hierarchy around it in the audited framework are distinctive. Pulp should design its callback interface from first principles and platform SDK conventions.

- **MIDI message representation**: The audited framework represents MIDI messages as a value type that encapsulates raw bytes with convenience accessors. While the concept is standard, the specific API (constructors, accessor names, internal storage) is distinctive. Pulp's `pulp-midi` subsystem will use its own `MidiPacket` type designed from the MIDI 1.0/2.0 specifications.

- **DSP chain composition**: The audited framework provides a variadic template-based processor chain for composing DSP operations. This pattern, while drawing on general C++ template techniques, has a distinctive API. Pulp's `pulp-signal` subsystem will use its own `Pipeline` abstraction.

- **Module format specification**: The audited framework uses a specific declaration block syntax within module files to specify dependencies, version, and metadata. Pulp uses standard CMake for module metadata and dependency declaration.

---

## Low-Risk Areas (Generic Patterns)

These patterns are universal and not owned by any single framework:

- **Audio buffer as array of channel pointers** — This is the standard representation used across CoreAudio, ASIO, VST3, and virtually all audio APIs
- **Plugin format specifications** — VST3, AU, AAX, CLAP, and LV2 all have their own SDKs and documentation
- **CMake as build system** — Standard industry practice
- **MIDI 1.0/2.0 message formats** — Defined by the MIDI Manufacturers Association specifications
- **Platform conditional compilation** — Universal C/C++ pattern using `#ifdef` or CMake conditionals
- **Lock-free ring buffers** — Well-documented computer science data structures
- **FFT algorithms** — Mathematical operations with many independent implementations
- **Observer/listener patterns** — Standard software design patterns

---

## Mitigation Strategies

1. **Implement format wrappers from official SDKs**: VST3 SDK (MIT), AU SDK (Apache 2.0), CLAP (MIT), LV2 (ISC). Read only the format vendor's documentation and headers when implementing wrappers.

2. **Design Pulp's API surface independently**: Start from inferred requirements, not from the audited framework's solutions. Write Pulp's API in a separate design phase before any implementation.

3. **Use modern C++ idioms and Swift patterns**: Pulp targets C++20/23 and Swift 6, which naturally diverge from the audited framework's legacy C++ style (pre-C++11 idioms, custom containers, macro-heavy patterns).

4. **Name subsystems from Pulp's own vocabulary**: All module and type names originate from Pulp's naming conventions, never adapted from the audited framework.

5. **Review all public API names against the audited framework's headers**: Before finalizing any public API, perform a mechanical check to ensure no name collisions with the audited framework's public types, functions, or macros.

6. **Maintain an explicit contamination checklist**: During implementation, each pull request must include a declaration that no audited-framework source code was referenced during development of that PR.

7. **Separate the team**: If possible, developers who have recently studied the audited framework's source code in detail should not implement the corresponding Pulp subsystem until a cooling-off period has elapsed.

8. **Document derivation**: For each major design decision, document which requirement (Layer 2) motivated it and confirm that the design (Layer 3) is original.

---

## Third-Party Dependencies That Can Be Freely Used

These libraries have permissive licenses and can be incorporated directly into Pulp:

| Dependency | License | Purpose |
|---|---|---|
| VST3 SDK | MIT | VST3 plugin format support |
| AudioUnit SDK | Apache 2.0 | AU/AUv3 plugin format support |
| CLAP | MIT | CLAP plugin format support |
| LV2 SDK | ISC | LV2 plugin format support |
| zlib | zlib | Compression utilities |
| HarfBuzz | MIT | Text shaping (Unicode, complex scripts) |
| FreeType | FreeType License / GPL | Font rendering (Linux, optional elsewhere) |
| Oboe | Apache 2.0 | Android audio I/O |
| bgfx | BSD 2-Clause | GPU rendering abstraction (optional) |
| Dear ImGui | MIT | Debug/development UI (optional) |
| nlohmann/json | MIT | JSON parsing/generation |
| pugixml | MIT | XML parsing/generation |

---

## Dependencies Requiring Independent Licensing

These dependencies have proprietary or restrictive licenses and must be obtained separately:

| Dependency | License | How to Obtain |
|---|---|---|
| AAX SDK | Proprietary (Avid) | Apply directly through Avid's developer program |
| ASIO SDK | Proprietary (Steinberg) | Download from Steinberg's developer portal, or use alternatives (PortAudio, WASAPI exclusive mode) |

---

## Contamination Checklist Template

For each Pulp subsystem implementation, the developer must confirm:

- [ ] I did not reference the audited framework's source code during this implementation
- [ ] I did not copy any class names, function names, or API patterns from the audited framework
- [ ] I designed the API from Pulp's requirements documents, not from the audited framework's API
- [ ] I implemented format wrappers using only the official format SDK documentation
- [ ] I reviewed all new public type/function names against the audited framework's headers and confirmed no collisions
- [ ] Any algorithms used are either original, from published academic sources, or from permissively-licensed third-party libraries
