# Modular Repository Layout

Version: 0.1.0-draft
Date: 2026-03-24

## 1. Monorepo vs Multi-Repo Decision

**Decision: Monorepo.**

| Factor | Monorepo | Multi-repo |
|---|---|---|
| Cross-subsystem changes | Single PR, atomic | Multiple PRs, coordination overhead |
| Versioning | Unified, simple | Per-repo, complex compatibility matrix |
| CI/CD | Single pipeline, test everything together | Per-repo pipelines, integration testing is separate |
| Developer onboarding | Clone once, everything is there | Clone N repos, configure relationships |
| Dependency management | In-tree, always consistent | Published packages, version pinning |
| Build times | Longer (but mitigated by subsystem-level caching) | Shorter per-repo |

The monorepo disadvantage of longer build times is mitigated by CMake's target-level build caching and CI job parallelism. The advantages of atomic cross-subsystem changes and unified versioning strongly outweigh the costs for a framework project of this scope.

---

## 2. Proposed Repository Layout

```
pulp/
|-- README.md
|-- LICENSE.md                          # MIT or Apache 2.0
|-- CLAUDE.md                           # AI context file (agentic-first design)
|-- CMakeLists.txt                      # Root CMake configuration
|-- Package.swift                       # SPM manifest (Apple Swift targets)
|-- .github/
|   +-- workflows/                      # CI/CD workflow definitions
|
|-- core/                               # Cross-platform C++ subsystems
|   |-- platform/                       # pulp-platform
|   |   |-- CMakeLists.txt
|   |   |-- include/pulp/platform/
|   |   |   |-- detect.h               # Platform detection macros
|   |   |   |-- handles.h              # Native handle typedefs
|   |   |   +-- bridges.h              # ObjCBridge, ComBridge
|   |   |-- src/
|   |   |-- platform/
|   |   |   |-- mac/
|   |   |   |-- ios/
|   |   |   |-- win/
|   |   |   |-- linux/
|   |   |   +-- android/
|   |   +-- tests/
|   |
|   |-- runtime/                        # pulp-runtime
|   |   |-- CMakeLists.txt
|   |   |-- include/pulp/runtime/
|   |   |   |-- log.h                  # Leveled logger
|   |   |   |-- result.h              # Result<T, E>
|   |   |   |-- lockfree.h            # Lock-free containers
|   |   |   |-- simd.h                # SIMD operations
|   |   |   |-- utf8.h                # UTF-8 string helpers
|   |   |   |-- binary_io.h           # BinaryReader/BinaryWriter
|   |   |   +-- compression.h         # ZlibCodec
|   |   |-- src/
|   |   |-- platform/
|   |   |   |-- mac/                   # macOS-specific (app paths, etc.)
|   |   |   |-- win/
|   |   |   +-- linux/
|   |   +-- tests/
|   |
|   |-- events/                         # pulp-events
|   |   |-- CMakeLists.txt
|   |   |-- include/pulp/events/
|   |   |   |-- event_loop.h          # EventLoop (constructible, not singleton)
|   |   |   |-- timer.h               # Timer
|   |   |   |-- async.h               # AsyncDispatcher
|   |   |   +-- signal.h              # Signal<Args...>, Connection
|   |   |-- src/
|   |   |-- platform/
|   |   |   |-- mac/                   # CFRunLoop backend
|   |   |   |-- win/                   # Win32 message pump backend
|   |   |   +-- linux/                 # epoll/kqueue backend
|   |   +-- tests/
|   |
|   |-- canvas/                         # pulp-canvas
|   |   |-- CMakeLists.txt
|   |   |-- include/pulp/canvas/
|   |   |   |-- canvas.h              # Abstract Canvas interface
|   |   |   |-- path.h                # Path
|   |   |   |-- transform.h           # Transform
|   |   |   |-- brush.h               # Brush (solid, gradient, pattern)
|   |   |   |-- font.h                # Font
|   |   |   |-- text_layout.h         # TextLayout
|   |   |   |-- image.h               # Image
|   |   |   +-- color.h               # Color
|   |   |-- src/
|   |   |-- platform/
|   |   |   |-- mac/                   # CoreGraphics backend
|   |   |   |-- win/                   # Direct2D backend
|   |   |   +-- linux/                 # Skia backend
|   |   +-- tests/
|   |
|   |-- view/                           # pulp-view
|   |   |-- CMakeLists.txt
|   |   |-- include/pulp/view/
|   |   |   |-- view.h                # View base class
|   |   |   |-- widgets/              # Widget headers
|   |   |   |   |-- knob.h
|   |   |   |   |-- fader.h
|   |   |   |   |-- toggle.h
|   |   |   |   |-- selector.h
|   |   |   |   |-- text_input.h
|   |   |   |   |-- list_view.h
|   |   |   |   |-- tree_view.h
|   |   |   |   |-- label.h
|   |   |   |   |-- panel.h
|   |   |   |   +-- image_view.h
|   |   |   |-- layout.h              # LayoutEngine (flex/grid)
|   |   |   |-- theme.h               # Theme, per-widget style structs
|   |   |   +-- accessibility.h       # AccessibilityProvider
|   |   |-- src/
|   |   +-- tests/
|   |
|   |-- audio/                          # pulp-audio
|   |   |-- CMakeLists.txt
|   |   |-- include/pulp/audio/
|   |   |   |-- audio_system.h        # AudioSystem (device enumeration)
|   |   |   |-- audio_device.h        # AudioDevice (open/config/start/stop)
|   |   |   |-- audio_callback.h      # AudioCallback function signature
|   |   |   +-- audio_buffer.h        # AudioBuffer (non-owning view)
|   |   |-- src/
|   |   |-- platform/
|   |   |   |-- mac/                   # CoreAudio
|   |   |   |-- ios/                   # AVAudioSession + CoreAudio
|   |   |   |-- win/                   # WASAPI
|   |   |   |-- linux/                 # ALSA, PipeWire
|   |   |   +-- android/              # Oboe
|   |   +-- tests/
|   |
|   |-- midi/                           # pulp-midi
|   |   |-- CMakeLists.txt
|   |   |-- include/pulp/midi/
|   |   |   |-- midi_system.h         # MidiSystem (device enumeration)
|   |   |   |-- midi_port.h           # MidiPort (I/O)
|   |   |   |-- midi_event.h          # MidiEvent (lightweight message)
|   |   |   |-- midi_buffer.h         # MidiBuffer (timestamped events)
|   |   |   |-- ump.h                 # UmpMessage (MIDI 2.0)
|   |   |   +-- mpe.h                 # MpeState
|   |   |-- src/
|   |   |-- platform/
|   |   |   |-- mac/                   # CoreMIDI
|   |   |   |-- ios/                   # CoreMIDI (iOS)
|   |   |   |-- win/                   # Windows MIDI Services
|   |   |   +-- linux/                 # ALSA MIDI
|   |   +-- tests/
|   |
|   |-- signal/                         # pulp-signal
|   |   |-- CMakeLists.txt
|   |   |-- include/pulp/signal/
|   |   |   |-- signal_block.h        # SignalBlock (non-owning buffer view)
|   |   |   |-- processor.h           # Processor concept (C++20)
|   |   |   |-- fft.h                 # Fft
|   |   |   |-- convolver.h           # Convolver
|   |   |   |-- filters/
|   |   |   |   |-- biquad.h
|   |   |   |   |-- svf.h             # StateVariableFilter
|   |   |   |   +-- crossover.h       # CrossoverFilter
|   |   |   |-- oversampler.h         # Oversampler
|   |   |   |-- delay_buffer.h        # DelayBuffer
|   |   |   |-- envelope.h            # EnvelopeFollower
|   |   |   |-- dynamics/
|   |   |   |   |-- compressor.h
|   |   |   |   +-- limiter.h
|   |   |   +-- oscillator.h          # Oscillator
|   |   |-- src/
|   |   |-- platform/
|   |   |   |-- mac/                   # vDSP FFT backend
|   |   |   +-- generic/              # pffft / KissFFT fallback
|   |   +-- tests/
|   |
|   |-- state/                          # pulp-state
|   |   |-- CMakeLists.txt
|   |   |-- include/pulp/state/
|   |   |   |-- parameter.h           # Parameter<T>
|   |   |   |-- parameter_group.h     # ParameterGroup (tree)
|   |   |   |-- state_store.h         # StateStore (serialize/undo/redo)
|   |   |   |-- binding.h             # Binding<T> (reactive link)
|   |   |   +-- automation.h          # AutomationGesture
|   |   |-- src/
|   |   +-- tests/
|   |
|   +-- format/                         # pulp-format
|       |-- CMakeLists.txt
|       |-- include/pulp/format/
|       |   |-- descriptor.h          # PluginDescriptor
|       |   +-- exporter.h            # FormatExporter
|       |-- src/
|       |   |-- vst3/                  # VST3 adapter
|       |   |-- au/                    # Audio Unit v2 adapter
|       |   |-- auv3/                  # AUv3 adapter (C++)
|       |   |-- aax/                   # AAX adapter
|       |   |-- clap/                  # CLAP adapter
|       |   |-- lv2/                   # LV2 adapter
|       |   +-- standalone/            # Standalone app adapter
|       +-- tests/
|
|-- apple/                              # Apple-specific Swift subsystems
|   |-- Sources/
|   |   |-- PulpView/                  # pulp-view-swift (SwiftUI layer)
|   |   |   |-- ParameterBinding.swift
|   |   |   |-- PulpAudioUnitView.swift
|   |   |   |-- MetalCanvas.swift
|   |   |   +-- ViewRepresentable.swift
|   |   +-- PulpFormat/               # pulp-format-swift (AUv3 in Swift)
|   |       |-- PulpAudioUnit.swift
|   |       |-- ParameterTree.swift
|   |       +-- RenderBridge.swift
|   +-- Tests/
|       |-- PulpViewTests/
|       +-- PulpFormatTests/
|
|-- gpu/                                # Optional GPU rendering (pulp-gpu)
|   |-- CMakeLists.txt
|   |-- include/pulp/gpu/
|   |   |-- gpu_surface.h
|   |   +-- gpu_theme.h
|   |-- src/
|   +-- external/                      # Visage or similar (submodule/FetchContent)
|
|-- tools/                              # Build and development tooling
|   |-- cmake/                          # pulp-build CMake modules
|   |   |-- PulpAddPlugin.cmake        # pulp_add_plugin() function
|   |   |-- PulpAddApp.cmake           # pulp_add_app() function
|   |   |-- PulpBinaryData.cmake       # pulp_add_binary_data() function
|   |   |-- PulpFormatSDK.cmake        # pulp_set_format_sdk_path() function
|   |   +-- PulpUtils.cmake            # Shared utilities
|   |-- scripts/
|   |   |-- build.sh                   # Cross-platform build script
|   |   |-- sign.sh                    # Code signing helper
|   |   |-- notarize.sh               # macOS notarization helper
|   |   +-- package.sh                # Installer packaging helper
|   +-- templates/
|       |-- plugin/                    # Plugin starter template
|       |   |-- CMakeLists.txt.in
|       |   |-- src/
|       |   |   |-- Processor.h.in
|       |   |   |-- Processor.cpp.in
|       |   |   |-- Editor.h.in
|       |   |   +-- Editor.cpp.in
|       |   +-- .env.in
|       |-- app/                       # Standalone app template
|       +-- website/                   # Download page template
|
|-- test/                               # pulp-test framework
|   |-- harness/
|   |   |-- include/pulp/test/
|   |   |   |-- audio_test_harness.h   # Offline render + compare
|   |   |   +-- visual_test_harness.h  # Screenshot comparison
|   |   +-- src/
|   +-- validation/
|       |-- au_validator.sh            # Audio Unit validation wrapper
|       |-- vst3_validator.sh          # VST3 compliance checker
|       +-- clap_validator.sh          # CLAP compliance checker
|
|-- ship/                               # pulp-ship packaging
|   |-- macos/
|   |   |-- create_pkg.sh             # .pkg creation
|   |   |-- notarize.sh               # Notarization script
|   |   +-- distribution.xml.in       # Installer config template
|   |-- windows/
|   |   |-- installer.nsi.in          # NSIS installer template
|   |   +-- sign.ps1                   # Authenticode signing
|   +-- linux/
|       |-- create_deb.sh             # .deb package creation
|       +-- create_tarball.sh         # .tar.gz creation
|
|-- ci/                                 # pulp-ci
|   |-- workflows/
|   |   |-- build.yml                  # Multi-platform build
|   |   |-- test.yml                   # Full test suite
|   |   |-- release.yml               # Build + sign + publish
|   |   +-- nightly.yml               # Extended nightly validation
|   +-- scripts/
|       |-- sync_secrets.sh            # Sync .env to GitHub secrets
|       +-- release.sh                 # Release automation helper
|
|-- claude/                             # pulp-claude plugin
|   |-- .claude-plugin/
|   |   +-- plugin.json
|   |-- commands/
|   |   |-- create.md
|   |   |-- build.md
|   |   |-- test.md
|   |   |-- ci.md
|   |   |-- port.md
|   |   |-- ship.md
|   |   |-- status.md
|   |   |-- setup-gpu.md
|   |   |-- setup-ios.md
|   |   |-- setup-updates.md
|   |   |-- setup-swift.md
|   |   |-- vm.md
|   |   |-- website.md
|   |   +-- theme.md
|   |-- skills/
|   |   |-- pulp-starter/
|   |   |   |-- SKILL.md
|   |   |   +-- references/
|   |   |-- pulp-gpu/
|   |   |   |-- SKILL.md
|   |   |   +-- references/
|   |   |-- pulp-swift/
|   |   |   |-- SKILL.md
|   |   |   +-- references/
|   |   +-- pulp-theme/
|   |       |-- SKILL.md
|   |       +-- references/
|   |-- README.md
|   +-- image.png
|
|-- examples/                           # Example projects
|   |-- simple-synth/
|   |   |-- CMakeLists.txt
|   |   |-- src/
|   |   +-- README.md
|   |-- simple-effect/
|   |   |-- CMakeLists.txt
|   |   |-- src/
|   |   +-- README.md
|   |-- standalone-app/
|   |   |-- CMakeLists.txt
|   |   |-- src/
|   |   +-- README.md
|   +-- gpu-ui-demo/
|       |-- CMakeLists.txt
|       |-- src/
|       +-- README.md
|
|-- docs/                               # Documentation
|   |-- getting-started.md
|   |-- architecture.md
|   |-- api/                           # Generated API docs (Doxygen output)
|   +-- guides/
|       |-- creating-a-plugin.md
|       |-- adding-parameters.md
|       |-- custom-ui.md
|       |-- swift-ui-guide.md
|       |-- gpu-rendering.md
|       |-- cross-platform.md
|       +-- testing.md
|
+-- external/                           # Third-party dependencies
    |-- vst3-sdk/                      # Steinberg VST3 SDK (MIT option)
    |-- clap/                          # CLAP (MIT)
    |-- lv2/                           # LV2 (ISC)
    |-- oboe/                          # Oboe for Android (Apache 2.0)
    +-- skia/                          # Skia (BSD-style, optional)
```

---

## 3. Header Organization

### 3.1 Public Headers

Public headers live under `include/pulp/<subsystem>/` within each subsystem directory. These headers form the **public API surface** -- the only files that downstream consumers include.

```
core/runtime/include/pulp/runtime/log.h
core/audio/include/pulp/audio/audio_buffer.h
core/signal/include/pulp/signal/processor.h
```

Consumer include paths:
```cpp
#include <pulp/runtime/log.h>
#include <pulp/audio/audio_buffer.h>
#include <pulp/signal/processor.h>
```

### 3.2 Private Headers

Private/internal headers live under `src/` within each subsystem. They are never installed or exposed to consumers.

```
core/runtime/src/internal_helpers.h    // NOT public
core/audio/src/coreaudio_impl.h        // NOT public
```

### 3.3 Include Path Rules

| Path | Visibility | Purpose |
|---|---|---|
| `include/pulp/<subsystem>/` | Public | Stable API consumed by framework users |
| `src/` | Private | Internal implementation details |
| `platform/<os>/` | Private | Platform-specific implementation |

CMake enforces this separation:
```cmake
target_include_directories(pulp-runtime
    PUBLIC  include/                    # Consumers see include/pulp/runtime/
    PRIVATE src/                        # Only this target sees src/
    PRIVATE platform/${PULP_PLATFORM}/  # Only this target sees platform code
)
```

---

## 4. Platform Code Organization

### 4.1 Directory Structure

Each subsystem that has platform-specific code contains a `platform/` directory:

```
platform/
|-- mac/           # macOS-specific (.mm, .cpp)
|-- ios/           # iOS-specific
|-- win/           # Windows-specific (.cpp)
|-- linux/         # Linux-specific (.cpp)
+-- android/       # Android-specific (.cpp)
```

### 4.2 Rules

1. **Never mix platform conditionals in cross-platform source files.** If a function needs a platform-specific implementation, the cross-platform header declares the interface, and each `platform/<os>/` directory provides the implementation.

2. **Platform selection happens in CMake, not in source code.** CMake selects which `platform/` directory to compile. Source files within a platform directory do not contain `#if PULP_PLATFORM_*` guards (they are only compiled on their platform).

3. **Shared logic stays in `src/`.** If 80% of a function is cross-platform and 20% is platform-specific, extract the platform-specific part into a small platform file and call it from the shared implementation.

### 4.3 Example: Audio System

```
core/audio/
|-- include/pulp/audio/
|   +-- audio_system.h          # Cross-platform interface
|-- src/
|   +-- audio_system_common.cpp # Shared logic (device list caching, etc.)
+-- platform/
    |-- mac/
    |   +-- audio_system_mac.mm # CoreAudio implementation
    |-- ios/
    |   +-- audio_system_ios.mm # AVAudioSession implementation
    |-- win/
    |   +-- audio_system_win.cpp # WASAPI implementation
    |-- linux/
    |   +-- audio_system_linux.cpp # ALSA/PipeWire implementation
    +-- android/
        +-- audio_system_android.cpp # Oboe implementation
```

CMakeLists.txt for pulp-audio:
```cmake
target_sources(pulp-audio PRIVATE
    src/audio_system_common.cpp
)

if(APPLE AND NOT IOS)
    target_sources(pulp-audio PRIVATE platform/mac/audio_system_mac.mm)
elseif(IOS)
    target_sources(pulp-audio PRIVATE platform/ios/audio_system_ios.mm)
elseif(WIN32)
    target_sources(pulp-audio PRIVATE platform/win/audio_system_win.cpp)
elseif(LINUX)
    target_sources(pulp-audio PRIVATE platform/linux/audio_system_linux.cpp)
elseif(ANDROID)
    target_sources(pulp-audio PRIVATE platform/android/audio_system_android.cpp)
endif()
```

---

## 5. Build System Details

### 5.1 Requirements

| Tool | Minimum Version | Purpose |
|---|---|---|
| CMake | 3.24 | Build system |
| C++ compiler | C++20 support | Core framework |
| Swift | 5.9 | Apple Swift layer (optional) |
| Xcode | 15.0 | macOS/iOS builds |
| MSVC | 17.8 (VS 2022) | Windows builds |
| GCC | 13 | Linux builds |
| Clang | 17 | Linux builds (alternative) |

### 5.2 Root CMakeLists.txt

The root `CMakeLists.txt` configures global options and adds subdirectories:

```cmake
cmake_minimum_required(VERSION 3.24)
project(Pulp VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Options
option(PULP_BUILD_TESTS "Build test suite" ON)
option(PULP_BUILD_EXAMPLES "Build example projects" ON)
option(PULP_ENABLE_GPU "Enable GPU rendering (pulp-gpu)" OFF)
option(PULP_ENABLE_SWIFT "Enable Swift layer (Apple only)" OFF)

# Platform detection
include(tools/cmake/PulpUtils.cmake)
pulp_detect_platform()

# Core subsystems (order matters for dependencies)
add_subdirectory(core/platform)
add_subdirectory(core/runtime)
add_subdirectory(core/events)
add_subdirectory(core/canvas)
add_subdirectory(core/view)
add_subdirectory(core/audio)
add_subdirectory(core/midi)
add_subdirectory(core/signal)
add_subdirectory(core/state)
add_subdirectory(core/format)

# Optional GPU subsystem
if(PULP_ENABLE_GPU)
    add_subdirectory(gpu)
endif()

# Build tooling (CMake modules, available to consumers)
include(tools/cmake/PulpAddPlugin.cmake)
include(tools/cmake/PulpAddApp.cmake)
include(tools/cmake/PulpBinaryData.cmake)

# Tests
if(PULP_BUILD_TESTS)
    enable_testing()
    add_subdirectory(test)
endif()

# Examples
if(PULP_BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()
```

### 5.3 Per-Subsystem CMakeLists.txt

Each subsystem defines a library target with explicit dependencies:

```cmake
# core/runtime/CMakeLists.txt
add_library(pulp-runtime)

target_sources(pulp-runtime PRIVATE
    src/log.cpp
    src/lockfree.cpp
    src/simd.cpp
    src/utf8.cpp
    src/binary_io.cpp
    src/compression.cpp
)

# Platform-specific sources
pulp_add_platform_sources(pulp-runtime)

target_include_directories(pulp-runtime
    PUBLIC  include/
    PRIVATE src/
)

target_link_libraries(pulp-runtime
    PUBLIC pulp-platform
)

target_compile_features(pulp-runtime PUBLIC cxx_std_20)

if(PULP_BUILD_TESTS)
    add_subdirectory(tests)
endif()
```

### 5.4 Consuming Pulp as a Dependency

**Via CMake FetchContent:**
```cmake
include(FetchContent)
FetchContent_Declare(
    pulp
    GIT_REPOSITORY https://github.com/pulp-framework/pulp.git
    GIT_TAG v0.1.0
)
FetchContent_MakeAvailable(pulp)

pulp_add_plugin(MyPlugin
    FORMATS VST3 AU CLAP Standalone
    DESCRIPTOR_FILE src/descriptor.h
    SOURCES src/Processor.cpp src/Editor.cpp
)
```

**Via SPM (Apple Swift targets):**
```swift
// Package.swift
dependencies: [
    .package(url: "https://github.com/pulp-framework/pulp.git", from: "0.1.0")
],
targets: [
    .target(
        name: "MyPluginUI",
        dependencies: [
            .product(name: "PulpView", package: "pulp"),
            .product(name: "PulpFormat", package: "pulp"),
        ]
    )
]
```

**Via vcpkg (future):**
```
vcpkg install pulp
```

### 5.5 CMake Presets

Provided presets for common configurations:

```json
{
  "configurePresets": [
    {
      "name": "default",
      "generator": "Ninja Multi-Config",
      "binaryDir": "${sourceDir}/build",
      "cacheVariables": {
        "PULP_BUILD_TESTS": "ON",
        "PULP_BUILD_EXAMPLES": "ON"
      }
    },
    {
      "name": "release",
      "inherits": "default",
      "cacheVariables": {
        "PULP_BUILD_TESTS": "OFF",
        "PULP_BUILD_EXAMPLES": "OFF"
      }
    },
    {
      "name": "gpu",
      "inherits": "default",
      "cacheVariables": {
        "PULP_ENABLE_GPU": "ON"
      }
    },
    {
      "name": "swift",
      "inherits": "default",
      "cacheVariables": {
        "PULP_ENABLE_SWIFT": "ON"
      },
      "condition": {
        "type": "equals",
        "lhs": "${hostSystemName}",
        "rhs": "Darwin"
      }
    }
  ]
}
```

---

## 6. Swift Package Manager Integration

### 6.1 Package.swift

The root `Package.swift` defines Swift targets that wrap the C++ core:

```swift
// swift-tools-version: 5.9
import PackageDescription

let package = Package(
    name: "Pulp",
    platforms: [
        .macOS(.v13),
        .iOS(.v17)
    ],
    products: [
        .library(name: "PulpView", targets: ["PulpView"]),
        .library(name: "PulpFormat", targets: ["PulpFormat"]),
    ],
    targets: [
        .target(
            name: "PulpView",
            path: "apple/Sources/PulpView",
            swiftSettings: [
                .interoperabilityMode(.Cxx)
            ]
        ),
        .target(
            name: "PulpFormat",
            path: "apple/Sources/PulpFormat",
            swiftSettings: [
                .interoperabilityMode(.Cxx)
            ]
        ),
    ]
)
```

### 6.2 C++ / Swift Interop

Swift targets access C++ headers via a module map generated during the CMake build or provided manually:

```
apple/Sources/PulpView/include/module.modulemap
```

The module map exposes `pulp-state` public headers to Swift, enabling direct access to `Parameter<T>`, `StateStore`, and `Binding<T>` from Swift code.

---

## 7. External Dependencies

### 7.1 Third-Party Dependency Management

| Dependency | License | Inclusion Method | Required |
|---|---|---|---|
| VST3 SDK | Dual (GPLv3/proprietary) | Submodule or FetchContent | Optional (format) |
| CLAP | MIT | Submodule or FetchContent | Optional (format) |
| LV2 | ISC | Submodule or FetchContent | Optional (format) |
| AAX SDK | Proprietary | User-provided path | Optional (format) |
| Oboe | Apache 2.0 | FetchContent | Optional (Android) |
| Skia | BSD-style | FetchContent or system | Optional (Linux canvas, GPU) |
| Visage | TBD | Submodule | Optional (GPU UI) |
| Catch2 | BSL-1.0 | FetchContent | Tests only |
| Sparkle | MIT | FetchContent | Optional (auto-updates, macOS) |
| WinSparkle | MIT | FetchContent | Optional (auto-updates, Windows) |

### 7.2 Dependency Rules

1. **No required external dependencies** for the core framework (beyond the C++20 standard library and platform SDKs).
2. Format SDKs are only needed if the corresponding format target is enabled.
3. Test dependencies (Catch2) are only fetched when `PULP_BUILD_TESTS` is ON.
4. Optional features (GPU, auto-updates) fetch their dependencies only when enabled.

---

## 8. Versioning

### 8.1 Scheme

Semantic versioning: `MAJOR.MINOR.PATCH`

- **MAJOR:** Breaking API changes
- **MINOR:** New features, backward-compatible
- **PATCH:** Bug fixes, backward-compatible

### 8.2 Monorepo Versioning

All subsystems are versioned together. A single version number applies to the entire framework. This is a deliberate monorepo advantage: consumers never deal with version compatibility matrices between subsystems.

### 8.3 Release Process

1. Update version in root `CMakeLists.txt` and `Package.swift`.
2. Update `CHANGELOG.md`.
3. Tag the commit: `git tag v0.1.0`.
4. CI builds, tests, and publishes artifacts.

---

## 9. CLAUDE.md (AI Context File)

The root `CLAUDE.md` is a key part of Pulp's agentic-first design. It provides structured context for AI-assisted development tools.

### 9.1 Contents

```markdown
# Pulp Audio Framework

## Architecture
- Two-pillar: processing (audio/DSP/state) and presentation (canvas/view/GPU)
- C++20 cross-platform core, Swift 6 Apple layer
- Subsystems: platform, runtime, events, canvas, view, audio, midi, signal, state, format
- See docs/architecture.md for full specification

## Build
- CMake 3.24+, C++20
- cmake --preset default && cmake --build build
- Tests: cmake --build build --target test

## Key Types
- Processor concept: prepare(sampleRate, maxFrames) / process(block) / reset()
- SignalBlock: non-owning multi-channel audio buffer for DSP
- AudioBuffer: non-owning multi-channel buffer for device I/O
- Parameter<T>: typed parameter with atomic access
- StateStore: parameter management, serialization, undo/redo
- View: UI base class with RAII child ownership
- Canvas: abstract 2D drawing context
- EventLoop: constructible (not singleton) event loop

## Conventions
- Public headers: core/<subsystem>/include/pulp/<subsystem>/
- Platform code: core/<subsystem>/platform/<os>/
- No platform conditionals in cross-platform source files
- Real-time safe functions annotated [[pulp::realtime]]
- Result<T, E> for errors, not exceptions
- std:: first, custom types only for genuinely missing functionality
```

### 9.2 Maintenance Rule

`CLAUDE.md` is updated whenever:
- A new subsystem is added
- A key type's interface changes
- Build instructions change
- Naming conventions change

---

## 10. Governance

### 10.1 API Stability

| Change Type | Requirement |
|---|---|
| Public API addition | Review required |
| Public API modification | Review + deprecation period (1 minor version) |
| Public API removal | Review + major version bump |
| Internal implementation change | Standard review |

### 10.2 Platform Parity

- Platform-specific changes must not break other platforms.
- CI matrix runs on all supported platforms for every PR.
- Platform-specific code is isolated in `platform/` directories and only compiled on its platform.

### 10.3 Test Requirements

- All subsystems must have tests.
- New public API must include tests.
- DSP changes must include golden-file comparison tests.
- UI changes should include visual regression tests where practical.

### 10.4 Documentation Requirements

- Public headers must include Doxygen-style documentation comments.
- `CLAUDE.md` kept up-to-date (enforced by CI check).
- Guides updated when user-facing workflows change.
