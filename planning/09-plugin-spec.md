# Claude-Native Pulp Plugin Specification

Version: 0.1.0-draft
Date: 2026-03-24

## 1. Overview

The Pulp Claude Code plugin (`pulp-claude`) provides an AI-native development workflow for building audio plugins and applications with the Pulp framework. It follows the proven command-and-skill architecture: commands are Markdown instruction files that define staged workflows, and skills are contextual knowledge bases that provide domain expertise.

The plugin is framework-specific to Pulp but architecturally framework-agnostic in its patterns -- the same command/skill structure could be adapted for any framework.

---

## 2. Plugin Architecture

### 2.1 Directory Structure

```
plugins/pulp/
|-- .claude-plugin/
|   +-- plugin.json                  # Plugin manifest
|-- commands/
|   |-- create.md                    # Project scaffolding
|   |-- build.md                     # Build wrapper
|   |-- test.md                      # Test runner
|   |-- ci.md                        # CI/CD management
|   |-- port.md                      # Cross-platform porting
|   |-- ship.md                      # Package, sign, distribute
|   |-- status.md                    # Project status dashboard
|   |-- setup-gpu.md                 # Add GPU rendering (Dawn/Skia/QuickJS)
|   |-- setup-ios.md                 # Add iOS target
|   |-- setup-updates.md             # Add auto-update support
|   |-- setup-swift.md               # Add Swift UI layer (Apple)
|   |-- inspect.md                   # Component inspector
|   |-- design.md                    # AI-driven design session
|   |-- vm.md                        # VM management
|   |-- website.md                   # Download page generation
|   +-- theme.md                     # Theme designer
|-- skills/
|   |-- pulp-starter/
|   |   |-- SKILL.md                 # Template reference
|   |   +-- references/
|   |-- pulp-render/
|   |   |-- SKILL.md                 # GPU rendering (Dawn/Skia/QuickJS) guide
|   |   +-- references/
|   |-- pulp-inspect/
|   |   |-- SKILL.md                 # Component inspector and design workflow
|   |   +-- references/
|   |-- pulp-swift/
|   |   |-- SKILL.md                 # Swift layer guide
|   |   +-- references/
|   +-- pulp-theme/
|       |-- SKILL.md                 # Theme design workflow
|       +-- references/
|-- README.md
+-- image.png
```

### 2.2 Plugin Manifest

```json
{
  "name": "pulp",
  "displayName": "Pulp Audio Framework",
  "description": "Development workflow commands for the Pulp cross-platform audio framework",
  "version": "0.1.0",
  "commands": [
    "create", "build", "test", "ci", "port", "ship", "status",
    "setup-gpu", "setup-ios", "setup-updates", "setup-swift",
    "inspect", "design",
    "vm", "website", "theme"
  ],
  "skills": [
    "pulp-starter", "pulp-render", "pulp-inspect", "pulp-swift", "pulp-theme"
  ]
}
```

---

## 3. Command Specifications

### 3.1 `/pulp:create` -- Project Scaffolding

**Purpose:** Create a new Pulp plugin or standalone application project from a template.

**Arguments:**

| Argument | Type | Required | Description |
|---|---|---|---|
| `name` | string | Yes | Project name (human-readable) |
| `type` | enum | No | `plugin` (default) or `app` |
| `formats` | list | No | Plugin formats: `vst3`, `au`, `auv3`, `clap`, `aax`, `lv2`, `standalone` |

**Stages:**

| Stage | Action |
|---|---|
| 1. Environment check | Verify CMake 3.24+, C++20 compiler, platform tools (Xcode, MSVC, GCC). Report missing dependencies. |
| 2. Template location | Locate the Pulp-Starter template (bundled with framework or fetched from repository). |
| 3. Collect settings | Prompt for: project name, vendor name, plugin UID, category (effect/instrument/utility), target formats, feature flags. |
| 4. Feature flags | Offer optional features: GPU UI (`pulp-gpu`), Swift layer (`pulp-view-swift`), auto-updates (`pulp-ship`), diagnostic kit, iOS target. |
| 5. Execute | Clone template, apply substitutions, generate CMakeLists.txt, create initial source files. |
| 6. Verify | Run `cmake --preset default` to confirm the project configures. |
| 7. Summary | Display created file tree, next steps, available commands. |

**Developer settings reuse:** If a `.pulp/developer.yaml` exists in the user's home directory, reuse vendor name, signing identity, team ID, and other persistent settings. Prompt to create one if it does not exist.

**Differences from prior art:**
- No 4-letter plugin codes. Pulp generates format-specific IDs internally from the UID string (e.g., `com.vendor.plugin` hashed to a 32-bit AU code).
- Feature flags are first-class: GPU UI, Swift layer, and auto-updates are optional modules toggled at creation time.
- Template produces a working plugin from minute one: builds, loads in a host, passes basic validation.

---

### 3.2 `/pulp:build` -- Build Wrapper

**Purpose:** Intelligent build command that wraps CMake with project-aware logic.

**Arguments:**

| Argument | Type | Required | Description |
|---|---|---|---|
| `target` | string | No | Specific target (default: all) |
| `action` | enum | No | `local` (default), `test`, `sign`, `notarize`, `package`, `publish`, `uninstall` |
| `config` | enum | No | `debug` (default) or `release` |
| `platform` | enum | No | `native` (default), `ios`, `windows`, `linux` |

**Stages:**

| Stage | Action |
|---|---|
| 1. Detect changes | Check if CMakeLists.txt or any `.cmake` file changed since last configure. If so, re-run CMake configure. |
| 2. Build | Invoke `cmake --build` with the selected preset, target, and config. |
| 3. Post-build | Copy plugin binaries to standard locations for host scanning. |
| 4. Action dispatch | Execute the requested action (sign, notarize, package, etc.). |

**Actions detail:**

| Action | Description |
|---|---|
| `local` | Build and install to local plugin directories |
| `test` | Build and run test suite (delegates to `/pulp:test`) |
| `sign` | Code-sign built binaries |
| `notarize` | Submit to Apple notarization (macOS only) |
| `package` | Create distributable installer |
| `publish` | Create GitHub Release with artifacts |
| `uninstall` | Remove locally installed plugin binaries |

**Intelligent CMake regen:** Hashes `CMakeLists.txt` and all included `.cmake` files. Stores hash in `.pulp/build-state.json`. Only re-runs configure when hashes change. This avoids unnecessary configure cycles on incremental builds.

---

### 3.3 `/pulp:test` -- Test Runner

**Purpose:** Run the project's test suite with audio-specific test harnesses.

**Arguments:**

| Argument | Type | Required | Description |
|---|---|---|---|
| `suite` | enum | No | `all` (default), `unit`, `validate`, `audio`, `visual` |
| `filter` | string | No | Test name filter pattern |
| `update-golden` | flag | No | Update golden reference files |

**Stages:**

| Stage | Action |
|---|---|
| 1. Build tests | Ensure test targets are built (invoke build if needed). |
| 2. Unit tests | Run Catch2/doctest unit tests. |
| 3. Plugin validation | Run format-specific validators (`auval` for AU, compliance checks for VST3/CLAP). |
| 4. Audio golden-file tests | Instantiate processors offline, render test signals, compare output against golden reference files. Report RMS/peak deviation. |
| 5. Visual regression tests | If GPU UI or `pulp-view` is used, render views offscreen and compare screenshots against references. |
| 6. Report | Generate test report with pass/fail counts, deviation metrics, and failing screenshots. |

**Golden file workflow:**
- First run with `--update-golden` captures reference files.
- Subsequent runs compare against references.
- Tolerance is configurable per test (default: RMS < -120 dB for audio, perceptual diff < 0.1% for visual).

---

### 3.4 `/pulp:ci` -- CI/CD Management

**Purpose:** Manage GitHub Actions CI/CD pipelines.

**Arguments:**

| Argument | Type | Required | Description |
|---|---|---|---|
| `action` | enum | Yes | `status`, `logs`, `secrets`, `dispatch`, `setup` |

**Actions:**

| Action | Description |
|---|---|
| `status` | Show current CI run status, recent results |
| `logs` | Fetch and display logs for a specific run |
| `secrets` | Sync secrets from `.env` to GitHub repository secrets (interactive confirmation) |
| `dispatch` | Trigger a workflow run manually |
| `setup` | Generate initial workflow files from templates in `pulp-ci` |

**Workflow templates provided:**
- `build.yml` -- Build on macOS, Windows, Linux
- `test.yml` -- Run full test suite
- `release.yml` -- Build, sign, notarize, package, publish
- `nightly.yml` -- Nightly builds with extended validation

---

### 3.5 `/pulp:port` -- Cross-Platform Porting

**Purpose:** Analyze and fix cross-platform compatibility issues.

**Arguments:**

| Argument | Type | Required | Description |
|---|---|---|---|
| `target` | enum | Yes | `windows`, `linux`, `ios`, `android` |

**Stages:**

| Stage | Action |
|---|---|
| 1. Static analysis | Scan source files for platform-specific patterns that will not compile on the target. |
| 2. Dependency check | Verify all dependencies are available on the target platform. |
| 3. Report | List issues with file/line references and suggested fixes. |
| 4. Fix (interactive) | Offer to apply fixes automatically with user confirmation. |

**Pattern scanning detects:**
- Apple framework imports in cross-platform code
- Windows API calls outside `platform/win/`
- POSIX-only calls in non-Linux paths
- Missing `#if PULP_PLATFORM_*` guards
- Swift code referenced from non-Apple targets

---

### 3.6 `/pulp:ship` -- Package and Distribute

**Purpose:** Create release packages, sign, notarize, and publish.

**Arguments:**

| Argument | Type | Required | Description |
|---|---|---|---|
| `platform` | enum | No | `all` (default), `macos`, `windows`, `linux` |
| `action` | enum | No | `package` (default), `sign`, `notarize`, `publish` |
| `version` | string | No | Version string (default: read from project config) |

**Per-platform workflow:**

| Platform | Steps |
|---|---|
| **macOS** | Build Release -> Code sign (Developer ID) -> Create .pkg -> Notarize -> Staple -> Verify |
| **Windows** | Build Release -> Create installer (NSIS/WiX) -> Authenticode sign (or Azure Trusted Signing) -> Verify |
| **Linux** | Build Release -> Create .tar.gz -> Create .deb (optional) -> GPG sign -> Verify |

**Publishing steps:**
1. Generate EdDSA signature for auto-update feeds (if auto-updates enabled).
2. Create GitHub Release with version tag.
3. Upload platform-specific artifacts.
4. Update download page (if configured via `/pulp:website`).
5. Update appcast / update manifest.

---

### 3.7 `/pulp:status` -- Project Status Dashboard

**Purpose:** Display a comprehensive overview of the project's configuration and state.

**Output sections:**

| Section | Content |
|---|---|
| Project info | Name, vendor, version, UID, category |
| Format targets | Enabled formats with build status |
| Feature flags | GPU UI, Swift layer, auto-updates, diagnostics |
| Build state | Last build time, configuration, any errors |
| Test results | Last test run summary |
| CI status | Latest CI run result per workflow |
| VM status | Running VMs and their state (if any) |
| Platform support | Which platforms are configured and tested |

---

### 3.8 `/pulp:setup-gpu` -- Add GPU Rendering (Dawn/Skia/QuickJS)

**Purpose:** Add GPU-accelerated UI rendering to an existing project using Dawn (WebGPU), Skia Graphite for 2D drawing, and QuickJS for JS-scripted widget logic.

**Stages:**

| Stage | Action |
|---|---|
| 1. Check compatibility | Verify project structure, platform GPU support, and dependencies. |
| 2. Fetch Dawn + Skia | Download or build Dawn (WebGPU implementation) and Skia Graphite rendering backend. |
| 3. Fetch QuickJS | Add QuickJS engine for JS-scripted widget definitions and hot-reload. |
| 4. Generate bridge code | Create `GpuSurface` integration with the project's `View` hierarchy and JS widget API bindings. |
| 5. Update CMake | Add `pulp-gpu` dependency and compilation targets. |
| 6. Design token setup | Initialize design token files (JSON/CSS/C++/OKLCH) and token pipeline. |
| 7. Theme migration | Generate GPU-compatible theme and design tokens from existing theme (if any). |
| 8. Verify | Build and confirm GPU rendering initializes with hot-reload active. |

**Note:** Visage/bgfx was the original GPU path explored during Phase 0 audit. Dawn/Skia Graphite is now the recommended GPU rendering stack due to broader platform support (including web/WASM), active maintenance, and native WebGPU compatibility.

---

### 3.9 `/pulp:inspect` -- Component Inspector

**Purpose:** Launch the component inspector on a running plugin. Click widgets to see properties, bounds, tokens, and render stats. Supports live editing of design tokens.

**Arguments:**

| Argument | Type | Required | Description |
|---|---|---|---|
| `target` | string | No | Plugin instance or window to inspect (default: auto-detect running instance) |

**Stages:**

| Stage | Action |
|---|---|
| 1. Attach | Connect to a running plugin process or launch one if needed. |
| 2. Overlay | Display inspector overlay on the plugin window. |
| 3. Interact | Click any widget to see: properties, bounding box, applied design tokens, render stats (draw calls, GPU time). |
| 4. Live edit | Modify design tokens in real-time; changes reflect immediately in the running UI. |
| 5. Export | Optionally export modified tokens back to source files. |

---

### 3.10 `/pulp:design` -- AI-Driven Design Session

**Purpose:** AI-driven design session. Describe a look in natural language, preview renders live, and export design tokens. Works with Claude Code, Codex, Stitch, or any AI tool.

**Arguments:**

| Argument | Type | Required | Description |
|---|---|---|---|
| `prompt` | string | No | Natural language description of desired look (interactive if omitted) |
| `export` | enum | No | Export format: `json`, `css`, `cpp`, `oklch`, `all` |

**Stages:**

| Stage | Action |
|---|---|
| 1. Describe | Accept natural language description of the desired visual style. |
| 2. Generate | AI generates candidate design tokens matching the description. |
| 3. Preview | Render live previews of widgets with the candidate tokens. |
| 4. Iterate | Refine via follow-up prompts until the design is satisfactory. |
| 5. Export | Export final design tokens to JSON, CSS custom properties, C++ constants, and/or OKLCH color definitions. |

---

### 3.12 `/pulp:setup-ios` -- Add iOS Target

**Purpose:** Add an iOS AUv3 target to an existing project.

**Stages:**

| Stage | Action |
|---|---|
| 1. Check prerequisites | Verify Xcode, iOS SDK, provisioning profile. |
| 2. Create targets | Add iOS app target and AUv3 extension target. |
| 3. Configure Swift | If Swift layer is enabled, set up iOS-specific SwiftUI views. |
| 4. Update CMake | Add iOS CMake preset and targets. |
| 5. Verify | Build for iOS Simulator. |

---

### 3.13 `/pulp:setup-updates` -- Add Auto-Update Support

**Purpose:** Integrate auto-update infrastructure (Sparkle on macOS, WinSparkle on Windows).

**Stages:**

| Stage | Action |
|---|---|
| 1. Generate keys | Create EdDSA key pair for update signing. |
| 2. Fetch framework | Add Sparkle (macOS) and/or WinSparkle (Windows) as dependencies. |
| 3. Configure | Set up appcast URL, update check interval, UI preferences. |
| 4. Update CMake | Link update frameworks. |
| 5. Doctor mode | `--doctor` flag verifies the entire update pipeline: key presence, appcast reachability, signature verification. |

---

### 3.14 `/pulp:setup-swift` -- Add Swift UI Layer (Apple Only)

**Purpose:** Add the SwiftUI-based UI layer for Apple platforms.

**Stages:**

| Stage | Action |
|---|---|
| 1. Check prerequisites | Verify Swift 5.9+, Xcode version, C++ interop support. |
| 2. Create SPM package | Generate `Package.swift` with `PulpView` and `PulpFormat` Swift targets. |
| 3. Generate AUAudioUnit subclass | Create Swift `PulpAudioUnit` that wraps the C++ processor. |
| 4. Generate SwiftUI views | Create template SwiftUI views with `@ParameterBinding` property wrappers. |
| 5. Configure interop | Set up Swift/C++ interop module map and bridging configuration. |
| 6. Update CMake | Link Swift targets into the macOS/iOS builds. |
| 7. Verify | Build and confirm SwiftUI view loads in AUv3 host. |

---

### 3.15 `/pulp:vm` -- VM Management

**Purpose:** Manage virtual machines for cross-platform testing.

**Actions:**

| Action | Description |
|---|---|
| `list` | Show available and running VMs |
| `start` | Start a VM (Windows, Linux) |
| `stop` | Stop a running VM |
| `sync` | Sync project files to a VM |
| `build` | Build the project inside a VM |
| `ssh` | Open SSH session to a VM |

Uses Tart (macOS host for macOS/Linux VMs) or UTM/QEMU as backends.

---

### 3.16 `/pulp:website` -- Download Page Generation

**Purpose:** Generate a static download page for the plugin/application.

**Stages:**

| Stage | Action |
|---|---|
| 1. Collect info | Read project metadata, version, platform artifacts. |
| 2. Generate page | Create static HTML/CSS download page from template. |
| 3. Deploy | Push to GitHub Pages or specified hosting. |

---

### 3.17 `/pulp:theme` -- Theme Designer

**Purpose:** Interactive theme design workflow for plugin UIs.

**Stages:**

| Stage | Action |
|---|---|
| 1. Inspect current | Display current theme configuration (colors, fonts, spacing). |
| 2. Modify | Apply theme changes interactively (modify style structs). |
| 3. Preview | Render preview screenshots of widgets with the updated theme. |
| 4. Export | Write updated theme code to source files. |

---

## 4. Skills

### 4.1 pulp-starter

**SKILL.md contents:** Complete reference for the Pulp-Starter project template. Documents every file in the template, every substitution variable, and every configuration option. Includes:
- Project structure walkthrough
- CMakeLists.txt anatomy
- Processor boilerplate explanation
- Editor/View boilerplate explanation
- How to add parameters
- How to add DSP processing
- How to add UI controls

**References:** Template source files, example configurations, format-specific notes.

### 4.2 pulp-render

**SKILL.md contents:** Guide for integrating GPU-accelerated rendering with Dawn/Skia Graphite and QuickJS scripting. Covers:
- When to use GPU rendering vs. standard Canvas rendering
- Dawn (WebGPU) and Skia Graphite setup and configuration
- QuickJS widget scripting and the JS widget API
- Hot-reload workflow for rapid UI iteration
- Design token system: defining, applying, and exporting tokens (JSON/CSS/C++/OKLCH)
- Audio visualization primitives (waveforms, spectra, meters with GPU acceleration)
- Custom GPU widget development
- Performance profiling and optimization
- Troubleshooting GPU issues on different platforms
- Web/WASM target considerations

**References:** Dawn API documentation, Skia Graphite rendering guide, QuickJS embedding guide, design token schema, benchmark data.

### 4.3 pulp-inspect

**SKILL.md contents:** Guide for using the component inspector and AI-driven design workflow. Covers:
- Launching and attaching the inspector to running plugins
- Interpreting widget properties, bounds, and render stats
- Live editing of design tokens via the inspector overlay
- AI-driven design sessions with `/pulp:design`
- Exporting design tokens in multiple formats
- Integration with Claude Code, Codex, Stitch, and other AI tools

**References:** Inspector UI guide, design token format specification, AI design workflow examples.

### 4.4 pulp-swift

**SKILL.md contents:** Guide for the Swift UI layer on Apple platforms. Covers:
- Swift/C++ interop patterns and gotchas
- `@ParameterBinding` usage
- SwiftUI view lifecycle in AUv3 hosts
- Metal custom rendering in SwiftUI
- Debugging Swift/C++ interop issues
- Performance considerations

**References:** Swift interop documentation, AUv3 hosting requirements, example SwiftUI plugin views.

### 4.5 pulp-theme

**SKILL.md contents:** Theme design workflow and reference. Covers:
- Theme architecture (composable style structs)
- Per-widget style properties
- Color system (light/dark mode, high contrast)
- Typography scale
- Spacing and layout tokens
- Theme inheritance and override patterns
- GPU theme compatibility

**References:** Default theme source, style struct API documentation, example custom themes.

---

## 5. Configuration Model

### 5.1 Environment Configuration (`.env`)

The `.env` file remains the central configuration for build-time settings. Proven pattern, works well with CI secret injection.

```bash
# Project identity
PULP_PROJECT_NAME="My Synth"
PULP_VENDOR_NAME="My Company"
PULP_PLUGIN_UID="com.mycompany.mysynth"
PULP_VERSION="1.0.0"

# Signing (macOS)
PULP_SIGN_IDENTITY="Developer ID Application: My Company (TEAMID)"
PULP_TEAM_ID="TEAMID"
PULP_NOTARIZE_PROFILE="notary-profile"

# Signing (Windows)
PULP_WIN_SIGN_TOOL="azuresigntool"
PULP_WIN_SIGN_VAULT="https://myvault.vault.azure.net"

# Auto-updates
PULP_APPCAST_URL="https://mycompany.com/appcast.xml"
PULP_EDDSA_PRIVATE_KEY="base64-encoded-key"

# CI
GITHUB_TOKEN="ghp_..."

# VM
PULP_VM_BACKEND="tart"
```

### 5.2 Structured Project Configuration (`.pulp/config.yaml`)

For complex settings that don't fit key-value pairs, a YAML configuration file supplements `.env`.

```yaml
project:
  name: "My Synth"
  vendor: "My Company"
  uid: "com.mycompany.mysynth"
  version: "1.0.0"
  category: synthesizer

formats:
  - vst3
  - au
  - auv3
  - clap
  - standalone

features:
  gpu_ui: false
  swift_layer: true
  auto_updates: true
  diagnostic_kit: false
  ios_target: true

platforms:
  macos:
    min_version: "13.0"
    architectures: [arm64, x86_64]
  ios:
    min_version: "17.0"
  windows:
    min_version: "10"
  linux:
    distros: [ubuntu-22.04, fedora-38]
```

### 5.3 Developer Settings (`~/.pulp/developer.yaml`)

Persistent per-developer settings, reused across projects.

```yaml
vendor: "My Company"
team_id: "TEAMID"
sign_identity: "Developer ID Application: My Company (TEAMID)"
default_formats: [vst3, au, clap, standalone]
preferred_editor: "cursor"
```

---

## 6. Mapping from Prior Art

The following table maps concepts from a typical JUCE-based Claude Code plugin workflow to their Pulp equivalents. This serves as a migration guide for developers familiar with that ecosystem.

| Prior Art Concept | Pulp Equivalent | Change Type |
|---|---|---|
| JUCE-Plugin-Starter template | Pulp-Starter template | Replaced |
| `juce_add_plugin` CMake macro | `pulp_add_plugin()` | Renamed and redesigned |
| 4-letter plugin codes (AU) | Auto-generated from UID string | Removed from user-facing |
| AudioProcessor class (virtual inheritance) | `Processor` concept (C++20) | Redesigned |
| AudioProcessorEditor class | `View` subclass or SwiftUI view | Redesigned |
| AudioProcessorValueTreeState | `StateStore` + `Parameter<T>` | Redesigned |
| LookAndFeel (monolithic class) | `Theme` (composable per-widget styles) | Redesigned |
| PluginVal external tool | `pulp-test` integrated validation suite | Generalized |
| JUCE as FetchContent dependency | Pulp as SPM / vcpkg / FetchContent | Multiple distribution paths |
| juce::Component (manual ownership) | `View` (RAII unique_ptr ownership) | Redesigned |
| juce::MessageManager (singleton) | `EventLoop` (constructible object) | Redesigned |
| juce::Graphics (software renderer primary) | `Canvas` (GPU-first backends via Dawn/Skia Graphite) | Redesigned |
| JUCE modules (custom module system) | CMake targets (standard build system) | Redesigned |
| Projucer / CMake hybrid | CMake-only (SPM for Apple Swift) | Simplified |
| JUCE dual license (GPL/commercial) | MIT or Apache 2.0 | Changed |
| Single-language (C++ only) | C++ core + Swift Apple layer | Extended |

### What stays the same

These patterns proved effective and carry forward in spirit:

| Pattern | How It Continues |
|---|---|
| Commands as Markdown instruction files | Identical architecture |
| Skills as contextual knowledge bases | Identical architecture |
| `.env` as central configuration | Same pattern, different variable names |
| Staged command workflows | Same pattern |
| Developer settings reuse | Same pattern (`~/.pulp/developer.yaml`) |
| VM management for cross-platform | Same approach |
| CI/CD GitHub Actions integration | Same approach |
| EdDSA signing for auto-updates | Same approach |

### What is new

| New Capability | Description |
|---|---|
| `/pulp:test` command | Dedicated test runner with audio golden-file comparison and visual regression |
| `/pulp:setup-swift` command | First-class Swift UI layer setup for Apple platforms |
| `/pulp:inspect` command | Component inspector with live design token editing |
| `/pulp:design` command | AI-driven design session with natural language prompts and live preview |
| `pulp-render` skill | Knowledge base for Dawn/Skia Graphite/QuickJS GPU rendering, design tokens, and audio visualization |
| `pulp-inspect` skill | Knowledge base for component inspector and AI design workflow |
| `pulp-swift` skill | Knowledge base for Swift/C++ interop patterns |
| YAML structured config | `.pulp/config.yaml` for complex settings beyond key-value |
| Headless plugin mode | Build and test processing without any UI dependency |
| Multiple UI paths | Choose between C++ views, SwiftUI, or GPU-accelerated rendering (Dawn/Skia) |
| JS-scripted GPU UIs | QuickJS-powered widget definitions with hot-reload |
| Design token system | Exportable design tokens in JSON/CSS/C++/OKLCH formats |
| Plugin CLI + MCP server | Plugins operate as both CLIs and MCP servers for AI tool integration |
| Web/WASM target | Dawn/Skia stack enables web deployment via WebGPU/WASM |

---

## 7. Design Principles for Plugin Commands

### 7.1 Progressive Disclosure

Commands start with sensible defaults and only prompt for advanced options when the developer requests them. A new project can be created with just a name; everything else has defaults.

### 7.2 Verifiable Steps

Every command stage that modifies the project verifies its result before proceeding. Build commands check exit codes. Configuration commands verify the output parses. Setup commands run a test build.

### 7.3 Idempotent Operations

Running a setup command twice does not break the project. Commands detect existing configuration and either skip or update gracefully.

### 7.4 Transparent Tooling

Commands log the underlying tool invocations (CMake commands, signing commands, API calls) so developers can understand and reproduce them manually. No opaque magic.

### 7.5 Offline Capability

Core commands (`create`, `build`, `test`, `status`) work fully offline once dependencies are cached. Network-dependent commands (`ci`, `ship`, `website`) degrade gracefully with clear error messages.

### 7.6 Plugin CLI and MCP Server Modes

Each Pulp plugin command can operate in two modes:

- **CLI mode:** Standard command-line invocation for scripts, CI pipelines, and direct developer use.
- **MCP server mode:** The plugin exposes its commands as an MCP (Model Context Protocol) server, allowing any AI tool (Claude Code, Codex, Stitch, or others) to invoke Pulp workflows programmatically. This enables AI agents to create projects, build, inspect components, run design sessions, and manage releases without custom integration per tool.

Both modes share the same command logic; the MCP server wraps CLI commands with structured input/output for tool interoperability.
