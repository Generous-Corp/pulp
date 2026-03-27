# Swift/C++ Language Strategy

**Subject:** Language strategy for Pulp — Swift-first on Apple platforms with C++ real-time core
**Date:** 2026-03-24

---

## Table of Contents

1. [Executive Summary](#executive-summary)
2. [Swift/C++ Interop State of the Art](#swiftc-interop-state-of-the-art)
3. [Real-Time Audio in Swift](#real-time-audio-in-swift)
4. [AUv3 in Swift](#auv3-in-swift)
5. [Recommended Architecture](#recommended-architecture)
6. [SwiftUI for Plugin UIs](#swiftui-for-plugin-uis)
7. [Build System Strategy](#build-system-strategy)
8. [What Must Be C++ vs What Should Be Swift](#what-must-be-c-vs-what-should-be-swift)
9. [Risk Assessment](#risk-assessment)
10. [Case Studies](#case-studies)
11. [Migration Path](#migration-path)
12. [Timeline Impact](#timeline-impact)

---

## Executive Summary

**Swift-first on Apple platforms is realistic and recommended.** C++ remains essential for the real-time DSP render loop and cross-platform code. The language boundary is well-defined and production-ready.

### The Core Insight

Audio plugin development has two fundamentally different domains:

| Domain | Constraints | Best Language |
|--------|------------|---------------|
| **Real-time audio processing** | No allocation, no locks, no exceptions, deterministic timing, cross-platform | **C++** |
| **Everything else** (UI, parameters, state, host integration, lifecycle) | ARC is fine, allocation is fine, platform-native is advantageous | **Swift** (on Apple) |

Swift/C++ direct interop has been production-ready since Swift 5.9 (September 2023), with significant improvements in Swift 6.0 (September 2024) and Swift 6.2 (September 2025). The function call boundary has **zero overhead** — confirmed by Apple at WWDC23.

### The Recommendation

```
Layer 1: Cross-Platform C++ Core (all platforms)
  - DSP render loop
  - Plugin format adapters (VST3, CLAP, LV2)
  - Lock-free utilities
  - MIDI processing
  - Audio buffer types

Layer 2: Apple Platform Swift Layer (macOS, iOS)
  - AUAudioUnit subclass (AU/AUv3 host interface)
  - SwiftUI plugin editor
  - Parameter binding (Swift <-> C++ parameter system)
  - Preset/state management
  - App lifecycle and host integration
  - Metal custom rendering (when needed)
```

This architecture gives Pulp the best of both worlds: cross-platform C++ for real-time DSP and format adapters, plus Swift's productivity, safety, and platform integration on Apple.

---

## Swift/C++ Interop State of the Art

### Timeline of Interop Capabilities

| Release | Date | Key Interop Feature |
|---------|------|-------------------|
| Swift 5.9 | Sep 2023 | Direct C++ interop (no bridging header for C++) |
| Swift 5.10 | Mar 2024 | Improved C++ template support |
| Swift 6.0 | Sep 2024 | Full data race safety, improved C++ type mapping |
| Swift 6.1 | Mar 2025 | Expanded C++ STL type bridging |
| Swift 6.2 | Sep 2025 | "Safe interoperability mode" for C/C++ pointer types |

### How Interop Works

Swift/C++ interop operates at the **module level**. A C++ module (defined by a module map or a CMake target) can be imported directly into Swift:

```swift
// Swift file
import PulpCore  // imports the C++ module

let buffer = AudioBuffer(channels: 2, samples: 512)
let processor = MyDSPProcessor()
processor.prepare(sampleRate: 44100.0, blockSize: 512)
processor.process(&buffer)
```

The interop layer automatically maps between Swift and C++ types:

| C++ Type | Swift Mapping |
|----------|--------------|
| `int`, `float`, `double` | `Int32`, `Float`, `Double` |
| `bool` | `Bool` |
| `std::string` | `std.string` (or manual conversion to `String`) |
| `std::vector<T>` | `std.vector<T>` (iterable in Swift) |
| `struct`/`class` | Swift struct/class (value/reference semantics preserved) |
| `enum class` | Swift enum |
| `std::optional<T>` | `Optional<T>` |
| `std::span<T>` | `UnsafeBufferPointer<T>` (with safe mode in 6.2) |
| `const T&` | Passed by value (copy) |
| `T*` | `UnsafeMutablePointer<T>` |
| `const T*` | `UnsafePointer<T>` |
| `std::function<R(Args...)>` | Not directly mapped (use C function pointers or callbacks) |
| C++ templates | Partial support (must be explicitly instantiated) |
| C++ exceptions | **Not caught by Swift** — UB if thrown across boundary |

### Zero-Overhead Function Calls

Apple confirmed at WWDC23 that Swift/C++ interop calls have **zero overhead at the function call boundary**. The compiler generates direct calls without thunks, trampolines, or boxing. This is critical for the audio processing path:

```swift
// This call has zero overhead compared to calling from C++
cppProcessor.processBlock(buffer.floatPointer, numSamples)
```

### Bidirectional Interop

Interop is bidirectional — Swift can call C++, and C++ can call Swift:

**Swift calls C++:**
- Import C++ module
- Call C++ functions, construct C++ objects
- Access C++ struct members
- Iterate C++ containers

**C++ calls Swift:**
- Swift classes/structs annotated with `@_expose(Cxx)` are visible to C++
- Swift protocols can be implemented in C++ (via generated C++ header)
- Swift closures can be passed as C function pointers (with `@convention(c)`)

### Limitations

| Limitation | Severity | Workaround |
|------------|----------|------------|
| C++ exceptions not caught in Swift | High | Don't throw across boundary; use error codes or `std::expected` |
| Partial template support | Medium | Explicitly instantiate needed template specializations |
| `std::function` not mapped | Medium | Use C function pointers or type-erased callbacks |
| Struct copying overhead for large types | Low | Pass by pointer; use `UnsafeMutablePointer` |
| No C++ virtual inheritance mapping | Low | Use composition or C++20 concepts instead |
| Module map authoring | Low | CMake generates module maps automatically with `cxx-interoperability-mode` |

### Swift 6.2 Safe Interoperability Mode

Swift 6.2 (September 2025) introduced a "safe interoperability mode" that allows C and C++ pointer types to be used more safely in Swift:

- `UnsafePointer<T>` can be used with bounds-checking in debug mode
- `std::span<T>` maps to a bounds-checked type
- Pointer arithmetic is validated
- Null pointer dereference is caught

This is particularly relevant for audio buffer access, where pointer safety matters but performance must not be compromised.

---

## Real-Time Audio in Swift

### The Core Problem

Real-time audio threads have strict constraints:

| Constraint | Reason |
|-----------|--------|
| No memory allocation (malloc/free) | Heap allocation can block on a mutex |
| No ARC retain/release | ARC reference counting involves atomic operations that can contend |
| No Objective-C message sends | May trigger autorelease pool or class realization |
| No locks (pthread_mutex_lock) | Can cause priority inversion |
| No I/O (file, network, console) | Blocking operations |
| No exceptions | Stack unwinding is non-deterministic |
| Deterministic execution time | Jitter causes audio glitches |

Standard Swift violates several of these constraints:
- **ARC:** Every class instance has retain/release on the audio thread
- **String:** Allocates on heap
- **Array:** Copy-on-write with potential heap allocation
- **Closures:** May capture reference types (triggering ARC)
- **Dynamic dispatch:** Protocol existentials may allocate

### Can Swift Be Used on the Audio Thread?

**Standard Swift: No.** The language's memory management (ARC) and standard library types are not real-time safe.

**Disciplined Swift: Partially.** With strict rules, Swift can be used for simple real-time code:

| Technique | What It Enables |
|-----------|----------------|
| Value types only (struct, enum, tuple) | No ARC |
| Pre-allocated buffers | No runtime allocation |
| `UnsafeMutablePointer` for buffer access | No bounds checking overhead |
| `@inlinable` for performance-critical functions | Compiler can inline across modules |
| `withUnsafeBufferPointer` for array access | Direct pointer access without copy-on-write |

### Experimental Swift Annotations

Swift has experimental annotations for real-time safety:

```swift
@_noLocks
@_noAllocation
func processAudio(buffer: UnsafeMutablePointer<Float>, count: Int) {
    // Compiler enforces: no allocation, no locking
    // Currently overly restrictive — rejects some safe patterns
}
```

**Assessment:** These annotations exist but are not production-ready. They reject valid real-time-safe patterns (e.g., accessing a pre-allocated array through a stored property) and are not stable API. They demonstrate Apple's intent to support real-time Swift but are not usable today.

### SignalKit: Pure Swift Real-Time DSP (2026)

SignalKit is a 2026 open-source project that proves pure Swift real-time DSP is possible with discipline:

| Pattern | How SignalKit Achieves Real-Time Safety |
|---------|---------------------------------------|
| Pre-allocated everything | All buffers, lookup tables, and state allocated before audio starts |
| Value types | Structs for all DSP state — no ARC |
| Unsafe pointers in hot path | `UnsafeMutableBufferPointer<Float>` for audio buffer access |
| No ARC on audio thread | No class instances touched during processing |
| Inlining | `@inlinable` and `@inline(__always)` on all DSP functions |
| vDSP/Accelerate | Platform-optimized SIMD through Apple's Accelerate framework |

**Assessment:** SignalKit demonstrates that pure Swift DSP is achievable with significant discipline. However, it requires deep expertise in Swift's memory model and sacrifices many of Swift's safety guarantees (heavy use of `Unsafe*Pointer`). For Pulp, **C++ remains the pragmatic choice for the DSP core** because:
1. C++ real-time safety is well-understood and battle-tested
2. C++ DSP code is cross-platform (Windows, Linux, Android)
3. The existing audio plugin ecosystem assumes C++ for real-time code
4. C++/Swift interop has zero overhead, so there's no performance penalty

### Embedded Swift

Apple's Embedded Swift initiative strips the Swift runtime entirely and targets resource-constrained environments (microcontrollers, kernel extensions). In theory, this could apply to DSP kernels:

- No ARC (value types only)
- No dynamic dispatch
- No standard library heap allocation
- Compiles to bare-metal code

**Assessment:** Interesting long-term possibility but not practical today. Embedded Swift is focused on microcontrollers, not audio DSP. C++ remains the correct choice for the real-time core.

### Recommendation

**C++ for the real-time audio render loop. Swift for everything else on Apple platforms.** The boundary is at the `processBlock()` / `internalRenderBlock` callback:

```
┌─────────────────────────────────────┐
│           Swift Layer               │
│  AUAudioUnit, Parameters, State,   │
│  SwiftUI Editor, Host Integration  │
├─────────────────────────────────────┤
│     Zero-overhead interop           │
│     (function call boundary)        │
├─────────────────────────────────────┤
│         C++ DSP Core                │
│  processBlock(), filters, FFT,     │
│  oscillators, delay lines           │
└─────────────────────────────────────┘
```

---

## AUv3 in Swift

### Apple's Recommended Architecture

Apple explicitly recommends Swift for AUv3 (Audio Unit v3) development. The pattern is:

1. **Swift** `AUAudioUnit` subclass — implements the Audio Unit interface
2. **C++ DSP kernel** — implements the signal processing
3. Swift calls the C++ kernel from the `internalRenderBlock`

This is the pattern used by:
- Apple's sample code ("Creating Custom Audio Effects" and "Creating an Audio Unit Extension")
- AudioKit (major open-source audio framework for Apple platforms)
- Multiple shipping AUv3 plugins on the App Store
- iPlug2's experimental SwiftUI support

### AUAudioUnit Subclass in Swift

The `AUAudioUnit` subclass manages:

| Responsibility | Details |
|---------------|---------|
| **Parameter tree** | `AUParameterTree` with `AUParameter` nodes — the host-visible parameter list |
| **Render block** | `internalRenderBlock` — returns a closure called on the audio thread |
| **Presets** | Factory presets and user preset management |
| **Bus management** | Input and output bus arrays |
| **State** | `fullState` dictionary for save/load |
| **Latency** | `latency` property |
| **Tail time** | `tailTime` property |

```swift
// Simplified AUAudioUnit subclass pattern (pseudocode)
class PulpAudioUnit: AUAudioUnit {
    private let dspKernel: UnsafeMutablePointer<PulpDSPKernel>  // C++ kernel

    override var internalRenderBlock: AUInternalRenderBlock {
        let kernel = self.dspKernel  // capture once, no ARC on audio thread
        return { flags, timestamp, frameCount, outputBus, bufferList, pullInput, events in
            // Pull input from host
            let pullResult = pullInput?(flags, timestamp, frameCount, 0, bufferList)
            // Process with C++ kernel (zero-overhead call)
            kernel.pointee.process(bufferList, frameCount, events)
            return noErr
        }
    }

    override func allocateRenderResources() throws {
        try super.allocateRenderResources()
        dspKernel.pointee.prepare(outputBusses[0].format.sampleRate,
                                   maximumFramesToRender)
    }
}
```

### Why Swift for AUAudioUnit?

| Reason | Explanation |
|--------|-------------|
| **Apple's intent** | AUv3 is an app extension — Apple designs extensions for Swift |
| **Parameter tree** | `AUParameterTree` API is designed for Swift (closures, optionals, enums) |
| **State management** | `fullState` is a `[String: Any]` dictionary — natural in Swift |
| **Host integration** | `NSExtensionContext`, `NSExtensionRequestHandling` — pure Swift APIs |
| **SwiftUI editor** | Plugin UI can be pure SwiftUI, managed from Swift |
| **Type safety** | AUParameterAddress, AudioComponentDescription — safer in Swift |
| **Future-proof** | Apple invests in Swift APIs; Objective-C/C++ will receive less attention |

### Parameter Tree in Swift

```swift
// Parameter tree construction (runs once, not on audio thread)
let parameterTree = AUParameterTree.createTree(withChildren: [
    AUParameterTree.createParameter(
        withIdentifier: "frequency",
        name: "Frequency",
        address: 0,
        min: 20.0,
        max: 20000.0,
        unit: .hertz,
        flags: [.flag_IsReadable, .flag_IsWritable],
        valueStrings: nil,
        dependentParameters: nil
    ),
    AUParameterTree.createParameter(
        withIdentifier: "gain",
        name: "Gain",
        address: 1,
        min: -60.0,
        max: 12.0,
        unit: .decibels,
        flags: [.flag_IsReadable, .flag_IsWritable],
        valueStrings: nil,
        dependentParameters: nil
    )
])

// Parameter change observer (called from any thread)
parameterTree.implementorValueObserver = { [weak self] param, value in
    self?.dspKernel.pointee.setParameter(param.address, value)
}
```

### Preset Management in Swift

```swift
// Factory presets
override var factoryPresets: [AUAudioUnitPreset]? {
    return [
        AUAudioUnitPreset(number: 0, name: "Init"),
        AUAudioUnitPreset(number: 1, name: "Warm Pad"),
        AUAudioUnitPreset(number: 2, name: "Sharp Lead")
    ]
}

// State save/load
override var fullState: [String: Any]? {
    get {
        var state = super.fullState ?? [:]
        state["pulpState"] = dspKernel.pointee.getState()  // C++ -> Data
        return state
    }
    set {
        super.fullState = newValue
        if let data = newValue?["pulpState"] as? Data {
            dspKernel.pointee.setState(data)  // Data -> C++
        }
    }
}
```

---

## Recommended Architecture

### Layer Diagram

```
┌──────────────────────────────────────────────────────┐
│                    SwiftUI Editor                      │
│  Plugin-specific UI (knobs, graphs, controls)         │
│  Metal custom views via MTKView + representable       │
│  Parameter observation via @Observable                 │
├──────────────────────────────────────────────────────┤
│              Swift Platform Layer                      │
│  AUAudioUnit subclass (AU/AUv3 format)               │
│  AUParameterTree construction and binding             │
│  Preset/state management                              │
│  Host integration (NSExtensionContext)                 │
│  App lifecycle (for standalone)                       │
├──────────────────────────────────────────────────────┤
│         Swift ↔ C++ Interop Boundary                  │
│  Zero-overhead function calls                         │
│  UnsafeMutablePointer<Float> for buffer access        │
│  Value types (structs) passed by copy                 │
│  C function pointers for callbacks                    │
├──────────────────────────────────────────────────────┤
│           Cross-Platform C++ Core                     │
│  DSP Engine (processBlock, filters, FFT, oscillators)│
│  Parameter System (non-normalized, with ranges)       │
│  State Serialization (binary or JSON)                 │
│  MIDI Processing (MIDI 1.0, MIDI 2.0 / UMP)         │
│  Lock-free Utilities (SPSC queue, ring buffer)       │
│  Format Adapters: VST3, CLAP, LV2                   │
│  SIMD: vDSP, SSE/AVX, NEON                          │
├──────────────────────────────────────────────────────┤
│          Platform Audio/MIDI I/O                      │
│  macOS/iOS: CoreAudio, CoreMIDI                      │
│  Windows: WASAPI, Windows MIDI Services              │
│  Linux: ALSA, JACK, ALSA MIDI                        │
│  Android: Oboe, Android MIDI                         │
└──────────────────────────────────────────────────────┘
```

### Module Structure

```
pulp/
├── core/                          # Cross-platform C++ (CMake)
│   ├── dsp/                       # DSP primitives and utilities
│   │   ├── AudioBuffer.h
│   │   ├── FloatVectorOps.h       # SIMD operations
│   │   ├── FFT.h                  # FFT with platform backends
│   │   ├── Filters.h              # IIR, FIR, SVF, TPT
│   │   ├── Oversampling.h
│   │   ├── DelayLine.h
│   │   └── SmoothedValue.h
│   ├── plugin/                    # Plugin abstraction
│   │   ├── PluginProcessor.h      # Core plugin concept
│   │   ├── ParameterSystem.h      # Non-normalized parameters
│   │   ├── StateSerializer.h      # State save/load
│   │   └── MidiProcessor.h
│   ├── formats/                   # Format adapters
│   │   ├── VST3Adapter.h/cpp
│   │   ├── CLAPAdapter.h/cpp
│   │   └── LV2Adapter.h/cpp
│   ├── io/                        # Audio/MIDI device I/O
│   │   ├── AudioDevice.h
│   │   └── MidiDevice.h
│   └── util/                      # Lock-free, threading
│       ├── SPSCQueue.h
│       ├── RingBuffer.h
│       └── AtomicHelpers.h
├── apple/                         # Apple platform Swift (SPM)
│   ├── Sources/PulpAU/            # AUv3 Audio Unit
│   │   ├── PulpAudioUnit.swift    # AUAudioUnit subclass
│   │   ├── ParameterTree.swift    # AUParameterTree construction
│   │   └── PresetManager.swift    # Preset handling
│   ├── Sources/PulpUI/            # SwiftUI plugin UI
│   │   ├── PluginEditor.swift     # Main editor view
│   │   ├── Controls/              # Reusable controls
│   │   │   ├── Knob.swift
│   │   │   ├── Slider.swift
│   │   │   ├── Meter.swift
│   │   │   └── WaveformView.swift
│   │   ├── MetalView.swift        # Custom Metal rendering
│   │   └── Theme.swift            # Theming system
│   ├── Sources/PulpApp/           # Standalone app + AUv3 host
│   │   ├── App.swift
│   │   └── ContentView.swift
│   └── Package.swift              # SPM package definition
├── windows/                       # Windows-specific (CMake)
│   └── ...
├── linux/                         # Linux-specific (CMake)
│   └── ...
├── android/                       # Android-specific (Gradle + CMake)
│   └── ...
└── CMakeLists.txt                 # Top-level CMake
```

### Data Flow for AU/AUv3

```
Host (Logic Pro, GarageBand, AUM, etc.)
  │
  ├── Sends: audio buffers, MIDI events, parameter changes, transport
  │
  ▼
Swift: PulpAudioUnit (AUAudioUnit subclass)
  │
  ├── Parameter changes → dspKernel.setParameter(address, value)
  ├── State save/load → dspKernel.getState() / setState()
  │
  ├── internalRenderBlock (called on audio thread):
  │     ├── pullInput() — get audio from host
  │     ├── dspKernel.process(bufferList, frameCount, events)
  │     └── return noErr
  │
  ▼
C++: PulpDSPKernel
  │
  ├── processBlock() — actual DSP processing
  ├── Parameter smoothing, filter updates, oscillator generation
  ├── Lock-free queue: push meter/spectrum data to UI
  │
  ▼
Swift: PluginEditor (SwiftUI)
  │
  ├── @Observable parameter model bound to AUParameterTree
  ├── Receives meter data from lock-free queue
  └── Renders UI at display refresh rate
```

### Data Flow for VST3/CLAP (Non-Apple Platforms)

```
Host (Ableton, Reaper, Bitwig, etc.)
  │
  ├── Sends: audio buffers, MIDI events, parameter changes, transport
  │
  ▼
C++: VST3Adapter / CLAPAdapter
  │
  ├── Translates format-specific API to PulpPlugin interface
  │
  ▼
C++: PulpPlugin (concept-based)
  │
  ├── processBlock() — actual DSP processing
  ├── Parameter management
  ├── State serialization
  │
  ▼
C++: Platform-specific UI (or headless)
  │
  ├── Windows: Custom renderer (Direct3D 12 / WebGPU)
  ├── Linux: Custom renderer (Vulkan / WebGPU)
  └── Headless: no UI (for testing, server-side processing)
```

---

## SwiftUI for Plugin UIs

### SwiftUI 5.0 Performance (2025)

SwiftUI 5.0 (shipped with macOS 15 / iOS 18, Fall 2025) includes a rebuilt rendering pipeline:

| Improvement | Details |
|-------------|---------|
| Metal-based rendering | SwiftUI now renders directly to Metal, ~40% performance improvement |
| `drawingGroup()` | Composites view subtree into a single GPU layer — essential for complex plugin UIs |
| Canvas API | Immediate-mode 2D drawing within SwiftUI — paths, gradients, images, text |
| Custom Metal shaders | `colorEffect()`, `distortionEffect()`, `layerEffect()` with Metal shader functions |
| `TimelineView` | Continuous animation driven by display link |

### Plugin UI Techniques

#### Standard Controls

SwiftUI provides controls that work well for many plugin UI elements:

| SwiftUI Control | Plugin Use |
|----------------|-----------|
| `Slider` | Parameter control (linear, with custom appearance) |
| `Toggle` | Boolean parameter |
| `Picker` | Enum/choice parameter |
| `Text` | Parameter labels, value display |
| `Canvas` | Custom 2D drawing (meters, waveforms, envelopes) |
| `GeometryReader` | Responsive layouts |

#### Custom Knob Control

```swift
struct Knob: View {
    @Binding var value: Double
    let range: ClosedRange<Double>
    let label: String

    var body: some View {
        Canvas { context, size in
            // Draw arc, indicator, value text
            let angle = mapToAngle(value, range: range)
            // ... custom drawing via Canvas API
        }
        .gesture(
            DragGesture(minimumDistance: 0)
                .onChanged { gesture in
                    // Map vertical drag to parameter change
                    value = clamp(value - gesture.translation.height * sensitivity, range)
                }
        )
        .accessibilityValue(Text("\(value, specifier: "%.1f")"))
        .accessibilityAdjustableAction { direction in
            switch direction {
            case .increment: value = min(value + step, range.upperBound)
            case .decrement: value = max(value - step, range.lowerBound)
            }
        }
    }
}
```

#### GPU-Accelerated Custom Drawing

For complex visualizations (spectrum analyzers, waveform displays, oscilloscopes):

```swift
struct SpectrumView: View {
    let magnitudes: [Float]  // from lock-free queue

    var body: some View {
        Canvas { context, size in
            var path = Path()
            for (i, mag) in magnitudes.enumerated() {
                let x = CGFloat(i) / CGFloat(magnitudes.count) * size.width
                let y = size.height * (1.0 - CGFloat(mag))
                if i == 0 { path.move(to: CGPoint(x: x, y: y)) }
                else { path.addLine(to: CGPoint(x: x, y: y)) }
            }
            context.stroke(path, with: .color(.green), lineWidth: 2)
        }
        .drawingGroup()  // GPU-accelerated compositing
    }
}
```

#### Full Metal Control

For maximum GPU control (custom shaders, 3D rendering, particle systems):

```swift
struct MetalVisualizerView: NSViewRepresentable {  // or UIViewRepresentable on iOS
    let renderer: PulpMetalRenderer  // Custom Metal renderer

    func makeNSView(context: Context) -> MTKView {
        let view = MTKView()
        view.device = MTLCreateSystemDefaultDevice()
        view.delegate = renderer
        view.preferredFramesPerSecond = 60
        view.enableSetNeedsDisplay = false  // continuous rendering
        return view
    }

    func updateNSView(_ view: MTKView, context: Context) {}
}
```

### Parameter Binding with @Observable

```swift
@Observable
class PluginParameters {
    private let parameterTree: AUParameterTree

    var frequency: Double {
        didSet { parameterTree.parameter(withAddress: 0)?.value = AUValue(frequency) }
    }
    var gain: Double {
        didSet { parameterTree.parameter(withAddress: 1)?.value = AUValue(gain) }
    }

    init(parameterTree: AUParameterTree) {
        self.parameterTree = parameterTree
        self.frequency = Double(parameterTree.parameter(withAddress: 0)?.value ?? 440.0)
        self.gain = Double(parameterTree.parameter(withAddress: 1)?.value ?? 0.0)

        // Observe parameter changes from host
        parameterTree.implementorValueObserver = { [weak self] param, value in
            DispatchQueue.main.async {
                switch param.address {
                case 0: self?.frequency = Double(value)
                case 1: self?.gain = Double(value)
                default: break
                }
            }
        }
    }
}

struct PluginEditor: View {
    @Bindable var params: PluginParameters

    var body: some View {
        VStack {
            Knob(value: $params.frequency, range: 20...20000, label: "Frequency")
            Knob(value: $params.gain, range: -60...12, label: "Gain")
        }
    }
}
```

### Limitations of SwiftUI for Plugin UIs

| Limitation | Severity | Mitigation |
|------------|----------|------------|
| Apple-only | High | Windows/Linux need separate UI implementation (or headless/web UI) |
| No sub-pixel layout control | Low | Canvas API and Metal views provide pixel-perfect control |
| Minimum deployment target | Low | SwiftUI 5.0 requires macOS 15+ / iOS 18+ |
| Complex state management for large UIs | Medium | Use `@Observable` + composition; avoid deeply nested state |
| Not designed for audio knob interaction | Medium | Custom `DragGesture` + `Canvas` for knobs; well-proven pattern |
| Startup time for complex views | Low | `drawingGroup()` and lazy loading |

---

## Build System Strategy

### Dual Build System

Pulp uses two build systems that interoperate:

| Build System | Scope | Used For |
|-------------|-------|----------|
| **CMake** | Cross-platform C++ core | DSP, format adapters, lock-free utilities, Windows/Linux targets |
| **Swift Package Manager (SPM)** | Apple platform Swift layer | AUAudioUnit, SwiftUI UI, standalone app, iOS app |

### How They Connect

SPM consumes the C++ core as a package dependency. The C++ code is exposed to Swift via a module map:

```
Package.swift:
  ├── .target(name: "PulpCore", ...)      // C++ library
  │     ├── Sources/PulpCore/include/      // Public C++ headers
  │     └── Sources/PulpCore/src/          // C++ implementation
  ├── .target(name: "PulpAU", ...)        // Swift AU layer
  │     dependencies: ["PulpCore"]
  ├── .target(name: "PulpUI", ...)        // SwiftUI plugin UI
  │     dependencies: ["PulpAU"]
  └── .target(name: "PulpApp", ...)       // Standalone app
        dependencies: ["PulpUI"]
```

SPM supports C++ targets with `cxxLanguageStandard: .cxx20` and Swift/C++ interop with `swiftSettings: [.interoperabilityMode(.Cxx)]`.

### Xcode Integration

For the final AUv3 app extension bundling, Xcode handles:
- Code signing (Developer ID, App Store)
- App extension packaging (AUv3 inside host app)
- Entitlements (audio, inter-app audio)
- App Store submission
- TestFlight distribution

The Xcode project can be generated from SPM or maintained manually with SPM package dependencies.

### CMake for Non-Apple Platforms

On Windows, Linux, and Android, CMake handles the entire build:

```cmake
# Top-level CMakeLists.txt
cmake_minimum_required(VERSION 3.24)
project(PulpPlugin CXX)

set(CMAKE_CXX_STANDARD 20)

# Core library (shared across all platforms)
add_library(pulp_core STATIC
    core/dsp/AudioBuffer.cpp
    core/plugin/ParameterSystem.cpp
    # ...
)

# VST3 plugin
add_library(PulpPlugin_VST3 MODULE
    core/formats/VST3Adapter.cpp
)
target_link_libraries(PulpPlugin_VST3 PRIVATE pulp_core)

# CLAP plugin
add_library(PulpPlugin_CLAP MODULE
    core/formats/CLAPAdapter.cpp
)
target_link_libraries(PulpPlugin_CLAP PRIVATE pulp_core)

# Standalone app
add_executable(PulpPlugin_Standalone
    core/standalone/StandaloneApp.cpp
)
target_link_libraries(PulpPlugin_Standalone PRIVATE pulp_core)
```

### Build Matrix

| Target | macOS | iOS | Windows | Linux | Android |
|--------|-------|-----|---------|-------|---------|
| C++ core | CMake or SPM | SPM | CMake | CMake | CMake (via Gradle) |
| AU/AUv3 | SPM | SPM | N/A | N/A | N/A |
| VST3 | CMake | N/A | CMake | CMake | N/A |
| CLAP | CMake | N/A | CMake | CMake | N/A |
| LV2 | CMake | N/A | CMake | CMake | N/A |
| Standalone | SPM (SwiftUI) | SPM | CMake | CMake | Gradle |
| SwiftUI UI | SPM | SPM | N/A | N/A | N/A |

---

## What Must Be C++ vs What Should Be Swift

### Must Be C++

These components **must** be C++ because they either run on the real-time audio thread or must be cross-platform:

| Component | Reason |
|-----------|--------|
| **DSP render loop** (`processBlock`) | Real-time thread — no ARC, no allocation |
| **Audio filters** (IIR, FIR, SVF, TPT) | Real-time, cross-platform |
| **FFT** | Real-time, platform-optimized (vDSP, FFTW, MKL) |
| **Oscillators and wavetables** | Real-time |
| **Delay lines** | Real-time |
| **Oversampling** | Real-time |
| **Convolution engine** | Real-time |
| **SIMD operations** (FloatVectorOps) | Real-time, cross-platform |
| **Lock-free utilities** (SPSC queue, ring buffer) | Real-time, cross-platform |
| **Parameter smoothing** (SmoothedValue) | Real-time, cross-platform |
| **MIDI processing** | Real-time, cross-platform |
| **VST3 format adapter** | Cross-platform (Windows, macOS, Linux) |
| **CLAP format adapter** | Cross-platform |
| **LV2 format adapter** | Cross-platform (primarily Linux) |
| **State serialization** (binary format) | Cross-platform (state must be loadable on any platform) |
| **Audio device I/O** (WASAPI, ALSA, JACK) | Platform-specific but C-based APIs |

### Should Be Swift (on Apple Platforms)

These components benefit from Swift because they don't run on the real-time thread and gain from Swift's language features:

| Component | Reason |
|-----------|--------|
| **AUAudioUnit subclass** | Apple's recommended architecture; Swift API design |
| **AUParameterTree construction** | Tree builder API designed for Swift |
| **Preset management** | Dictionary-based state, file I/O — Swift is natural |
| **SwiftUI plugin editor** | Most productive UI framework on Apple platforms |
| **Parameter binding** (@Observable) | Reactive binding is a Swift strength |
| **App lifecycle** (standalone app) | SwiftUI `@main`, `App` protocol |
| **Metal rendering** (custom views) | MetalKit is Swift-friendly; `MTKViewDelegate` |
| **Accessibility** | NSAccessibility/UIAccessibility are Swift-native |
| **CoreMIDI configuration** (device selection) | Swift API wrappers |
| **Accelerate framework** (non-real-time uses) | vDSP, BNNS — Swift-friendly APIs |
| **Host integration** (NSExtensionContext) | Pure Swift API |
| **In-app purchase** (if applicable) | StoreKit 2 is Swift-only |
| **iCloud sync** (for presets) | CloudKit is Swift-native |
| **Notification handling** | UserNotifications framework — Swift |

### The Interop Boundary

The boundary between C++ and Swift should be at a **small, stable API surface**:

```cpp
// C++ side: PulpDSPKernel.h (the interop surface)
struct PulpDSPKernel {
    void prepare(double sampleRate, int maxBlockSize);
    void process(AudioBufferList* buffers, int frameCount,
                 const AURenderEvent* events);
    void setParameter(uint64_t address, float value);
    float getParameter(uint64_t address) const;
    void getState(void* data, int* size);    // serialize to buffer
    void setState(const void* data, int size); // deserialize from buffer
    void reset();
};
```

This is a small surface area (7 methods) that is unlikely to change. All complexity lives on either side of this boundary, not at the boundary itself.

---

## Risk Assessment

| Risk | Severity | Likelihood | Mitigation |
|------|----------|-----------|------------|
| **Swift/C++ interop breaks in future Swift version** | High | Low | Keep C++ API surface small and simple (7 methods). Apple provides compatibility modes. They cannot break interop without breaking AudioKit, their own sample code, and thousands of apps. |
| **SwiftUI performance insufficient for complex plugin UIs** | Medium | Low | `drawingGroup()` for GPU compositing. `Canvas` for custom 2D drawing. `MTKView` via representable for full Metal control. Audio plugins don't need 10,000-row tables — knobs, sliders, and visualizations are well within SwiftUI's capability. |
| **Build system complexity (CMake + SPM)** | Medium | Medium | Proven by AudioKit (SPM + C), iPlug2 (CMake + Xcode), and many production audio apps. SPM's C++ support improves with each Xcode release. Provide build scripts and templates that abstract the complexity. |
| **Team hiring — audio devs expect C++** | Medium | Medium | C++ core is still there and is the majority of the codebase by logic complexity. Swift is only the Apple platform layer. Audio DSP code remains C++. Candidates with C++ experience can work on the core; Swift experience is only needed for Apple UI/integration. |
| **Minimum deployment target (SwiftUI 5.0 requires macOS 15)** | Low | Low | macOS 15 shipped September 2025. By the time Pulp ships, the vast majority of active macOS users will be on 15+. Fall back to AppKit for older targets if needed. |
| **Swift ABI stability across OS versions** | Low | Very Low | Swift ABI has been stable since Swift 5.0 (2019). Module stability since Swift 5.1. Not a realistic concern. |
| **Embedded Swift for DSP** | Low | N/A | Not recommending this — C++ for DSP is the safe choice. Embedded Swift is a future possibility, not a current dependency. |
| **Android/Windows parity** | Medium | Medium | Swift code doesn't help on these platforms. Separate UI implementations needed. Consider: headless + web UI as cross-platform fallback, or Compose/WinUI as native alternatives. |
| **C++ exception across Swift boundary** | High | Low | Design rule: never throw C++ exceptions across the interop boundary. Use error codes or `std::expected`. Enforced by code review and static analysis. |
| **std::function not mapped to Swift** | Low | Medium | Use C function pointers with context parameter (`void*`) at the interop boundary. Or use Swift closures captured as `@convention(c)`. |

---

## Case Studies

### AudioKit

AudioKit is the most prominent example of Swift + C++ for audio on Apple platforms:

| Aspect | Details |
|--------|---------|
| Language split | Swift (UI, AudioEngine, node graph) + C++ (DSP via SoundPipe, STK, libfaust) |
| Build system | SPM for Swift, embedded C code in package targets |
| AUv3 | Swift AUAudioUnit subclass + C DSP kernels |
| UI | SwiftUI-based controls (AudioKitUI package) |
| Users | Thousands of apps on the App Store |
| Open source | MIT licensed |

AudioKit proves that Swift + C for audio is production-viable and commercially successful.

### iPlug2 SwiftUI Example

iPlug2 includes an experimental `IPlugSwiftUI` example that demonstrates:
- C++ plugin core with iPlug2's architecture
- SwiftUI view as the plugin editor
- Bridge layer connecting iPlug2's parameter system to SwiftUI

This proves that even a C++-first framework can integrate SwiftUI for its editor.

### Apple Sample Code

Apple provides official sample code for AUv3 in Swift:
- "Creating Custom Audio Effects" — Swift AUAudioUnit + C++ DSP
- "Creating an Audio Unit Extension" — Swift-first architecture
- WWDC sessions on Audio Unit development consistently use Swift

### Shipping AUv3 Plugins in Swift

Multiple commercial AUv3 plugins on the iOS App Store use Swift for the Audio Unit layer:
- Moog Model 15 (Swift UI layer)
- Korg Module (Swift integration layer)
- Various AudioKit-based instruments and effects

---

## Migration Path

### For Existing JUCE/C++ Developers

Developers with existing C++ plugin code can adopt Pulp incrementally:

| Phase | What Changes | Effort |
|-------|-------------|--------|
| Phase 1: C++ Core Only | Port DSP code to Pulp's C++ core. No Swift. All platforms. | Low — C++ to C++ |
| Phase 2: Swift AU Layer | Replace C++ AU wrapper with Swift AUAudioUnit. C++ DSP stays. | Medium — learn Swift basics |
| Phase 3: SwiftUI Editor | Replace C++ editor with SwiftUI. Reuse C++ DSP. | Medium — learn SwiftUI |
| Phase 4: Full Swift Integration | State, presets, host integration all in Swift. | Low — incremental from Phase 2-3 |

Each phase is independently useful. A developer can stop at Phase 1 and have a working cross-platform plugin. Phase 2 and beyond are Apple-platform optimizations.

### For Swift Developers New to Audio

Developers coming from iOS/macOS app development can start with Swift and add C++ DSP as needed:

| Phase | What They Write | What Pulp Provides |
|-------|----------------|-------------------|
| Phase 1: SwiftUI App | Standalone app with simple audio processing | Template with basic oscillator in C++ |
| Phase 2: AUv3 Plugin | AUAudioUnit subclass hosting the app as a plugin | AU adapter template |
| Phase 3: Custom DSP | Replace template DSP with their own C++ processing | DSP library and examples |

---

## Timeline Impact

### Setup Overhead

| Task | Time | Notes |
|------|------|-------|
| CMake + SPM integration | 1 week | One-time setup, template-based |
| C++ module map for Swift interop | 2-3 days | Standard SPM C++ target configuration |
| Swift AUAudioUnit template | 1 week | Reusable across all Pulp plugins |
| SwiftUI control library (Knob, Slider, Meter) | 2 weeks | Reusable component library |
| Parameter binding system (@Observable + AUParameterTree) | 1 week | Template code |
| **Total initial setup** | **+2-4 weeks** | One-time cost |

### Ongoing Development Speed

After initial setup, Swift development on Apple platforms is **significantly faster** than C++:

| Activity | C++ Time | Swift Time | Speedup |
|----------|----------|-----------|---------|
| UI layout and styling | Hours (manual resize, repaint) | Minutes (SwiftUI previews, hot reload) | 5-10x |
| Parameter binding | 30+ lines (attachments, listeners, value conversion) | 5 lines (@Observable + Binding) | 6x |
| State management | Manual serialization, binary/XML | Codable protocol, automatic | 3-5x |
| Preset management | Custom file I/O, manual UI | FileDocument + SwiftUI file picker | 5x |
| Accessibility | Manual per-widget implementation | Automatic from SwiftUI controls | 10x |
| Testing | Custom framework, complex setup | XCTest + SwiftUI preview testing | 3x |

### Net Effect

- **+2-4 weeks** initial setup cost
- **Then faster** Apple platform development (SwiftUI is dramatically more productive than C++ UI)
- **No change** to cross-platform C++ development timeline
- **No change** to DSP/audio quality (same C++ core everywhere)
- **Better Apple platform integration** (native AUv3, proper host integration, App Store readiness)
- **Better user experience** on Apple (SwiftUI animations, accessibility, Dark Mode, Dynamic Type)
