# Parity Validation and Testing Plan

## Validation Philosophy

Pulp's validation strategy verifies that the framework reaches meaningful capability parity with established audio plugin frameworks WITHOUT contaminating the implementation. All validation is **black-box**: we test observed behavior and outputs, never internal implementation details.

### Core Principles

1. **Test what it does, not how it does it.** Validation checks that a plugin produces correct audio output, responds to parameters correctly, saves and restores state accurately, and passes format validators. It never checks that internal code follows a specific pattern.

2. **Use official validators wherever possible.** Each plugin format has official or community-standard validation tools. These are the authoritative sources of correctness.

3. **Automate everything.** Every test in this plan runs in CI without human intervention. Manual testing is reserved for subjective evaluation (UI appearance, audio quality perception) and is documented separately.

4. **Golden files are the source of truth for audio.** Reference audio outputs are computed independently (or captured from known-correct implementations) and committed to the repository. Any deviation beyond tolerance is a failure.

---

## Behavioral Test Harnesses

### Audio Processing Validation

#### Golden-File Tests

Golden-file testing verifies that Pulp's audio processing produces correct output by comparing against pre-computed reference files.

**Methodology:**
1. Generate known input signals programmatically (not from audio files, to avoid copyright/licensing issues)
2. Process through the plugin in headless mode
3. Compare output against reference golden files
4. Report pass/fail with deviation metrics

**Input Signals:**
| Signal | Purpose | Parameters |
|--------|---------|------------|
| Sine wave | Basic processing verification | 440 Hz, 1 second |
| Impulse | Impulse response capture | Single sample at t=0 |
| White noise | Statistical behavior verification | PRNG with fixed seed |
| Sine sweep | Frequency response measurement | 20 Hz to 20 kHz, 5 seconds |
| Silence | Verify no output when input is silent | All zeros, 1 second |
| DC offset | DC handling verification | +0.5 DC, 1 second |
| Full-scale square wave | Clipping and headroom behavior | 1 kHz, 1 second |

**Comparison Modes:**
- **Bit-exact:** For integer format pipelines and deterministic processing. Output must match reference exactly.
- **Floating-point tolerance:** For float/double pipelines. Default tolerance: 1e-6 (absolute) or configurable via test parameter. Accounts for platform-specific floating-point behavior (x87 vs SSE vs ARM NEON).
- **Spectral comparison:** For non-deterministic processing (e.g., dithering, noise generators). Compare frequency-domain magnitude spectrum within tolerance bands.

**Test Configurations:**
Run golden-file tests across all combinations of:

| Parameter | Values |
|-----------|--------|
| Sample rate | 44100, 48000, 88200, 96000, 176400, 192000 Hz |
| Buffer size | 32, 64, 128, 256, 512, 1024, 2048, 4096 samples |
| Channel config | Mono, Stereo |
| Sample format | Float32, Float64 |
| Plugin format | VST3, AU, CLAP, Standalone, Headless |

Not all combinations need to run on every CI push. A subset runs on every push; the full matrix runs nightly and before releases.

#### Latency Measurement

Verify that declared latency matches actual processing latency.

**Method:**
1. Query the plugin's reported latency (via format-specific API)
2. Send an impulse through the plugin
3. Measure the sample offset of the impulse in the output
4. Verify: actual offset == declared latency (within 1 sample tolerance for floating-point rounding)

**Run for:** All plugin formats, multiple sample rates, multiple buffer sizes.

#### Buffer Size Stress Test

Verify correct operation across the full range of buffer sizes that DAWs may request.

**Buffer sizes to test:** 1, 2, 4, 8, 16, 32, 64, 128, 256, 512, 1024, 2048, 4096, 8192

**Checks:**
- No crashes or memory errors at any buffer size
- Output is correct at all buffer sizes (compare against golden file generated at reference buffer size)
- No audio glitches (discontinuities at buffer boundaries)
- Buffer size of 1 (single-sample processing) must work correctly

#### Sample Rate Test

Verify correct operation across all standard sample rates.

**Sample rates to test:** 44100, 48000, 88200, 96000, 176400, 192000 Hz

**Checks:**
- Plugin initializes correctly at each sample rate
- Audio processing produces correct output at each sample rate
- Sample-rate-dependent parameters (e.g., filter coefficients) are recalculated correctly
- No crashes when sample rate changes between processing blocks

#### Multi-Channel Test

Verify correct operation with various channel configurations.

**Channel configurations to test:**
| Configuration | Channels | Common Use |
|--------------|----------|------------|
| Mono | 1 | Mono effects, guitar plugins |
| Stereo | 2 | Standard stereo processing |
| LCR | 3 | Left-Center-Right |
| Quad | 4 | Quadraphonic |
| 5.1 Surround | 6 | Film/TV mixing |
| 7.1 Surround | 8 | Immersive audio |

**Checks:**
- Plugin reports supported channel configurations correctly
- Processing produces correct output for each configuration
- Channel mapping is correct (left is left, center is center, etc.)
- Sidechain inputs (where applicable) are correctly separated from main inputs

---

### MIDI Validation

#### MIDI Message Round-Trip

Verify that MIDI messages pass correctly through all format wrappers.

**Messages to test:**
| Message Type | Test Cases |
|-------------|------------|
| Note On | All channels (1-16), all notes (0-127), all velocities (1-127) |
| Note Off | All channels, all notes, velocity 0 and 64 |
| Control Change | All controllers (0-127), all values (0-127) |
| Pitch Bend | Min (-8192), center (0), max (+8191) |
| Aftertouch (Channel) | All channels, all values |
| Aftertouch (Poly) | Multiple notes, all values |
| Program Change | All programs (0-127) |
| System Exclusive | Short (3 bytes) and long (>256 bytes) messages |

**Checks:**
- Message type is preserved
- Channel is preserved
- Data bytes are preserved
- No messages are dropped or duplicated

#### MIDI Timing Accuracy

Verify that MIDI events are sample-accurately timestamped within the audio buffer.

**Method:**
1. Send a MIDI Note On at a known sample offset within the buffer
2. Plugin generates audio starting at that sample offset
3. Verify the audio onset matches the MIDI event's sample position (within 1 sample)

**Run for:** Multiple buffer sizes (64, 256, 1024) and sample rates (44100, 48000, 96000).

#### MPE Message Handling

Verify correct handling of MIDI Polyphonic Expression (MPE) messages.

**Checks:**
- MPE Configuration Message (MCM) is parsed correctly
- Per-note pitch bend is applied independently per note
- Per-note slide (CC74) is applied independently per note
- Per-note pressure is applied independently per note
- Master channel messages are applied globally
- Zone assignment is correct (Lower Zone, Upper Zone)

#### MIDI 2.0 UMP Encoding/Decoding

Verify correct encoding and decoding of Universal MIDI Packet (UMP) messages.

**Checks:**
- UMP message types are correctly encoded and decoded
- 32-bit and 64-bit UMP messages are handled
- MIDI 1.0 Channel Voice Messages are correctly translated to/from UMP
- MIDI 2.0 Channel Voice Messages (higher resolution) are handled
- System messages are correctly wrapped in UMP

#### System Exclusive Handling

**Checks:**
- Short SysEx messages (< 256 bytes) are transmitted correctly
- Long SysEx messages (> 256 bytes, up to 64 KB) are transmitted correctly
- SysEx messages are not fragmented or corrupted during transmission
- Multiple SysEx messages in a single buffer are handled correctly

---

### Plugin Format Validation

Official and community validators are the primary correctness check for plugin format compliance.

| Format | Validator | Platform | What It Checks |
|--------|-----------|----------|----------------|
| AU | `auval` | macOS | Apple's official Audio Unit validation tool. Checks: initialization, parameter handling, state save/restore, audio rendering, tail time, latency reporting, channel configurations, factory presets, property queries. |
| AUv3 | `auval` | macOS / iOS | Audio Unit v3 (App Extension) validation. Additional checks: extension lifecycle, inter-process communication, memory limits. |
| VST3 | `pluginval` | All | Community validator (originally from the audited framework's ecosystem). Runs multiple test levels with increasing strictness. Tests: parameter enumeration, state save/restore, audio processing, threading, edge cases (zero-length buffers, very large buffers). |
| VST3 | VST3 SDK `validator` | All | Steinberg's official validation tool, included in the VST3 SDK. Tests strict VST3 specification compliance. |
| CLAP | `clap-validator` | All | Official CLAP community validator. Tests: plugin scanning, parameter enumeration, audio processing, state handling, extension compliance. |
| AAX | PACE validator | macOS / Win | Avid's official AAX validation tool (available via AAX SDK). Required for Pro Tools certification. |
| LV2 | `lv2lint` | Linux | Community LV2 specification compliance checker. Verifies: manifest correctness, port declarations, URI compliance, extension usage. |

**Validation strictness levels:**
- **CI on every push:** pluginval at strictness 5, auval, clap-validator (fast)
- **Nightly CI:** pluginval at strictness 10, full auval suite, all validators (thorough)
- **Pre-release:** All validators at maximum strictness, including AAX and LV2

---

### State Validation

#### Save/Restore Round-Trip

**Method:**
1. Set all parameters to non-default values
2. Save state (via format-specific state save API)
3. Reset all parameters to defaults
4. Restore state (via format-specific state load API)
5. Compare all parameter values: they must be identical to step 1

**Run for:** All plugin formats. Test with both binary state and preset file formats.

#### Cross-Session State

**Method:**
1. Load plugin in DAW, set parameters, save DAW project
2. Close DAW
3. Reopen DAW, load project
4. Verify all parameters are identical

**Note:** This is a manual/semi-automated test (requires DAW automation scripting, e.g., via Reaper's ReaScript or Logic's AppleScript).

#### Cross-Version State Loading (Forward Compatibility)

**Method:**
1. Save state from plugin version N
2. Load state into plugin version N+1 (which may have additional parameters)
3. Verify: all parameters that exist in both versions have correct values
4. Verify: new parameters (only in N+1) have their default values
5. Verify: no crash or error

#### State Fuzz Testing

**Method:**
1. Generate corrupted state data: random bytes, truncated data, oversized data, invalid headers, wrong version numbers
2. Attempt to load each corrupted state into the plugin
3. Verify: no crash, no memory corruption, no undefined behavior
4. Verify: plugin either loads gracefully with defaults or reports a clear error
5. Use AddressSanitizer (ASan) and UndefinedBehaviorSanitizer (UBSan) during fuzz testing

---

### DAW Compatibility Matrix

Test Pulp plugins in actual DAWs to verify real-world compatibility. This is the ultimate validation.

| DAW | Platform | VST3 | AU | AUv3 | AAX | CLAP | Priority |
|-----|----------|------|-----|------|-----|------|----------|
| Ableton Live | macOS, Win | Yes | Yes | - | - | Yes | P0 |
| Logic Pro | macOS | Yes | Yes | Yes | - | - | P0 |
| Pro Tools | macOS, Win | - | - | - | Yes | - | P1 |
| Reaper | All | Yes | Yes | - | - | Yes | P0 |
| FL Studio | Win, macOS | Yes | - | - | - | Yes | P1 |
| Bitwig Studio | All | Yes | - | - | - | Yes | P1 |
| Cubase/Nuendo | macOS, Win | Yes | - | - | - | - | P1 |
| Studio One | macOS, Win | Yes | Yes | - | - | - | P2 |
| GarageBand | macOS, iOS | - | Yes | Yes | - | - | P1 |
| Ardour | Linux | Yes | - | - | - | - | P2 |
| LMMS | Linux | Yes | - | - | - | - | P2 |

**Priority levels:**
- **P0:** Must work before any release. Test on every release candidate.
- **P1:** Must work before v1.0. Test on major releases.
- **P2:** Should work. Test periodically. Known issues documented.

**DAW Test Checklist (per DAW, per format):**
- [ ] Plugin appears in DAW's plugin scanner
- [ ] Plugin loads without errors
- [ ] Audio processing works (input -> output)
- [ ] Parameters appear in DAW's parameter list
- [ ] Parameters automate correctly (draw automation, verify plugin responds)
- [ ] Plugin UI opens and displays correctly
- [ ] Plugin UI is resizable (if supported)
- [ ] State saves with DAW project
- [ ] State restores when project is reopened
- [ ] Plugin can be instantiated multiple times
- [ ] Plugin can be removed and re-added without crashes
- [ ] No audio glitches during normal operation
- [ ] CPU usage is reasonable (comparable to similar plugins)

---

## Build Matrix Validation

### Target Platforms

| Platform | Architecture | Compiler | CI Runner | Status |
|----------|-------------|----------|-----------|--------|
| macOS | ARM64 (Apple Silicon) | Xcode/Clang | `macos-14` | Primary |
| macOS | x86_64 (Intel) | Xcode/Clang | `macos-13` | Secondary (if still relevant) |
| Windows | x64 | MSVC 2022 | `windows-latest` | Primary |
| Windows | x64 | Clang-cl | `windows-latest` | Secondary |
| Linux | x64 | Clang 17 | `ubuntu-latest` | Primary |
| Linux | x64 | GCC 13 | `ubuntu-latest` | Primary |
| iOS | ARM64 | Xcode/Clang | `macos-14` | For AUv3 |
| Android | ARM64 | NDK/Clang | `ubuntu-latest` | Long-term (Oboe) |

### Build Configuration Matrix

| Configuration | Compiler Flags | Purpose |
|--------------|---------------|---------|
| Release | `-O2` / `/O2` | Production builds |
| Debug | `-g -O0` / `/Od /Zi` | Development builds |
| RelWithDebInfo | `-O2 -g` / `/O2 /Zi` | Profiling builds |
| ASan | `-fsanitize=address` | Memory error detection |
| UBSan | `-fsanitize=undefined` | Undefined behavior detection |
| TSan | `-fsanitize=thread` | Data race detection |

**CI schedule:**
- Every push: Release and Debug builds on all primary platforms
- Nightly: Full matrix including sanitizers, secondary platforms, and secondary compilers

---

## Packaging Validation

### macOS .pkg Validation

**Checks:**
- [ ] `.pkg` installs cleanly on a fresh macOS system (tested in clean VM)
- [ ] Components are installed to correct locations:
  - VST3: `~/Library/Audio/Plug-Ins/VST3/` or `/Library/Audio/Plug-Ins/VST3/`
  - AU: `~/Library/Audio/Plug-Ins/Components/` or `/Library/Audio/Plug-Ins/Components/`
  - CLAP: `~/Library/Audio/Plug-Ins/CLAP/` or `/Library/Audio/Plug-Ins/CLAP/`
  - Standalone: `/Applications/`
- [ ] `.pkg` is signed with Developer ID Installer certificate
- [ ] `.pkg` is notarized (passes `spctl --assess --type install`)
- [ ] Notarization ticket is stapled
- [ ] Uninstall script (if provided) removes all components
- [ ] No files are installed outside the expected locations
- [ ] Installer UI displays correct product name, version, and license

### Windows Installer Validation

**Checks:**
- [ ] Installer runs cleanly on a fresh Windows system (tested in clean VM)
- [ ] Components are installed to correct locations:
  - VST3: `C:\Program Files\Common Files\VST3\`
  - CLAP: `C:\Program Files\Common Files\CLAP\`
  - Standalone: `C:\Program Files\<ProductName>\`
  - AAX: `C:\Program Files\Common Files\Avid\Audio\Plug-Ins\`
- [ ] Installer is Authenticode signed
- [ ] No SmartScreen warnings (requires EV certificate or reputation)
- [ ] Registry entries are correct (uninstall info in Add/Remove Programs)
- [ ] Uninstaller removes all components and registry entries
- [ ] Silent install mode works (`/S` flag)

### Linux .tar.gz Validation

**Checks:**
- [ ] Archive extracts cleanly
- [ ] File structure matches expected layout
- [ ] Install script copies to standard locations:
  - VST3: `~/.vst3/` or `/usr/lib/vst3/`
  - CLAP: `~/.clap/` or `/usr/lib/clap/`
  - LV2: `~/.lv2/` or `/usr/lib/lv2/`
- [ ] File permissions are correct (no world-writable files)
- [ ] No absolute paths baked into binaries (use relative paths or runtime detection)

### Auto-Update Validation

**Full cycle test:**
1. Install version 1.0.0
2. Publish version 1.0.1 (update appcast)
3. Application detects update is available
4. Application downloads the update
5. Application verifies EdDSA signature of downloaded artifact
6. Application installs the update
7. Application restarts as version 1.0.1
8. Verify all plugin state is preserved across the update

**Edge cases:**
- [ ] Update with no internet connection: graceful failure, no crash
- [ ] Update with corrupted download: EdDSA verification fails, update is rejected
- [ ] Update with tampered appcast: signature mismatch detected
- [ ] Downgrade attempt: handled according to policy (block or allow)
- [ ] Update from very old version (1.0.0 -> 2.0.0): state migration works

---

## Visual Regression Testing

### Screenshot-Based Comparison

**Method:**
1. Render plugin UI at a fixed size (e.g., 800x600) in headless mode
2. Capture screenshot as PNG
3. Compare against reference screenshot using perceptual difference metric
4. Threshold: < 0.1% pixel difference for pass (accounts for sub-pixel rendering differences)

**Test cases:**
- Default state (all parameters at defaults)
- All parameters at minimum values
- All parameters at maximum values
- Light theme
- Dark theme
- Custom theme (if configured)

### Cross-Platform Visual Parity

Render the same UI on all platforms and compare:
- macOS vs Windows vs Linux: major layout discrepancies are failures
- Minor font rendering differences are expected and acceptable
- Color values must match within 1/255 per channel

### GPU vs CPU Rendering Parity

When GPU rendering (Visage) is enabled, verify visual parity with CPU rendering:
- Render same UI with GPU and CPU backends
- Compare screenshots
- Tolerance: slightly higher than same-platform comparison (< 1% pixel difference) due to different rasterization

### Resize Testing

- [ ] UI resizes smoothly without visual artifacts
- [ ] All widgets remain correctly positioned after resize
- [ ] Minimum and maximum size constraints are respected
- [ ] UI renders correctly at common DPI values (1x, 1.5x, 2x, 3x)

---

## Performance Benchmarks

| Metric | Target | How to Measure | Failure Threshold |
|--------|--------|----------------|-------------------|
| Audio callback jitter | < 1ms variance | High-resolution timer (`mach_absolute_time`, `QueryPerformanceCounter`, `clock_gettime`) measured over 1000 consecutive callbacks at 256-sample buffer size | > 2ms variance |
| Audio callback duration | < 50% of buffer period | Timer around processing call; at 256 samples / 48kHz, buffer period = 5.3ms, so callback must complete in < 2.7ms | > 75% of buffer period |
| Plugin scan time | < 100ms per plugin | Time the host's plugin scanning/instantiation sequence | > 500ms |
| State save | < 10ms | Time state serialization for a plugin with 100 parameters | > 50ms |
| State load | < 10ms | Time state deserialization for a plugin with 100 parameters | > 50ms |
| UI paint time (CPU) | < 16ms per frame | Profile paint call duration (60 FPS target) | > 20ms |
| UI paint time (GPU) | < 8ms per frame | GPU profiler or frame time measurement (120 FPS target) | > 12ms |
| UI resize time | < 50ms | Time from resize event to completed repaint | > 100ms |
| Build time (incremental, single file change) | < 10s | Time `cmake --build build` after changing one source file | > 30s |
| Build time (clean, full framework) | < 120s | Time `cmake --build build` from clean state | > 300s |
| Project creation | < 30s | Time `/pulp:create` from start to buildable project | > 60s |
| Memory usage (idle plugin) | < 50 MB | Measure RSS after plugin loads with UI open | > 100 MB |
| Memory usage (processing) | < 100 MB | Measure RSS during active audio processing | > 200 MB |

**Benchmark environment:**
- macOS: Apple M1 or later, 16 GB RAM
- Windows: Intel i7 or AMD Ryzen 7, 16 GB RAM
- Linux: Intel i7 or AMD Ryzen 7, 16 GB RAM

**Benchmark frequency:**
- Every push: Audio callback and paint time benchmarks (fast)
- Nightly: Full benchmark suite
- Pre-release: Full benchmark suite on all platforms

**Regression detection:**
- Benchmarks are tracked over time (stored in CI artifacts or a benchmark database)
- A > 20% regression in any metric triggers a warning
- A > 50% regression triggers a failure

---

## Documentation Completeness

### API Documentation

- [ ] Every public class has a doc comment with description and usage example
- [ ] Every public method has a doc comment with parameter descriptions and return value
- [ ] Every public enum has doc comments for each value
- [ ] Every public type alias has a doc comment explaining its purpose
- [ ] No orphan public symbols (everything documented)

### Guides

- [ ] Getting Started guide works end-to-end when followed by a new developer
- [ ] Architecture guide accurately reflects the current implementation
- [ ] Migration guide helps developers transition from other frameworks
- [ ] Each guide has been tested by at least one person who is not the author

### Command Documentation

- [ ] Every `/pulp:` command has help text accessible via `/pulp:<command> --help`
- [ ] Help text includes: description, usage, options, examples
- [ ] Command behavior matches documentation

### Example Projects

- [ ] Every example project builds on all platforms without modification
- [ ] Every example project runs correctly and demonstrates its stated purpose
- [ ] Example code is well-commented and follows Pulp conventions
- [ ] Examples cover: basic effect, basic synth, standalone app, GPU UI, Swift UI

---

## Developer Experience Tests

These tests verify that the overall developer experience is smooth and error-free.

### New Developer Onboarding

**Scenario:** A developer with C++ and CMake experience but no Pulp experience creates their first plugin.

**Target:** Complete in under 30 minutes, following only Pulp documentation.

**Steps verified:**
1. Clone Pulp repository
2. Run `/pulp:create` to scaffold a project
3. Open project in IDE (Xcode, Visual Studio, CLion)
4. Build the project
5. Load the plugin in a DAW
6. Modify a parameter and rebuild
7. Verify the change is reflected in the DAW

### Error Handling

- [ ] `/pulp:create` with invalid project name: clear error message
- [ ] `/pulp:create` in a directory that already contains a project: warning and confirmation
- [ ] `/pulp:build` with missing dependencies: clear error message listing what is needed
- [ ] `/pulp:build` with CMake errors: error is surfaced clearly, not buried in CMake output
- [ ] `/pulp:ci secrets` with missing `.env` file: clear error message
- [ ] `/pulp:ship` with unsigned builds: clear error explaining which secrets are missing

### Build System Intelligence

- [ ] `/pulp:build` with no changes: skips rebuild (detects unchanged state)
- [ ] `/pulp:build` after CMakeLists.txt change: regenerates CMake configuration
- [ ] `/pulp:build` after source file change: incremental build (only recompiles changed files)
- [ ] `/pulp:build` after adding a new source file: CMake detects the new file (via glob or explicit list)

### Secrets Management

- [ ] `/pulp:ci secrets` syncs all secrets from `.env` to GitHub without exposing values in process lists
- [ ] `/pulp:ci secrets --verify` correctly identifies missing secrets
- [ ] `/pulp:ci secrets --list` shows which secrets are set without revealing values
- [ ] Secrets with special characters (quotes, spaces, newlines) are handled correctly

### Release Pipeline

- [ ] `/pulp:ship` produces signed artifacts on first try (given correct credentials)
- [ ] `/pulp:ship` with missing credentials: fails early with a clear message, not halfway through the pipeline
- [ ] `/pulp:ship` notarization timeout: retries automatically, reports if notarization fails
- [ ] `/pulp:ship` creates correct GitHub Release with all artifacts

---

## Claude Code Plugin Tests

### Command Execution

- [ ] All `/pulp:` commands execute without errors on macOS
- [ ] All `/pulp:` commands execute without errors on Windows
- [ ] All `/pulp:` commands execute without errors on Linux
- [ ] Commands that require platform-specific features (e.g., macOS signing) gracefully skip on other platforms

### Multi-Stage Flows

- [ ] `/pulp:create` (5-stage flow) completes successfully when all inputs are valid
- [ ] `/pulp:create` handles cancellation at any stage (no partial/corrupted project)
- [ ] `/pulp:create` handles restart after cancellation (can re-run cleanly)
- [ ] `/pulp:setup-gpu` handles Visage clone failure gracefully
- [ ] `/pulp:setup-gpu` handles patch application failure with diagnostic output

### Skills Activation

- [ ] `pulp-starter` skill activates when a new Pulp project is detected
- [ ] `pulp-gpu` skill activates when GPU-related topics are discussed
- [ ] `pulp-swift` skill activates when SwiftUI-related topics are discussed
- [ ] `pulp-theme` skill activates when theme-related topics are discussed
- [ ] Skills do not activate on unrelated topics (no false positives)

### Template Replacement

- [ ] Project name is correctly substituted in all generated files
- [ ] Plugin identifiers (4-character codes) are correctly placed
- [ ] Manufacturer information is correctly placed
- [ ] Generated files are valid C++ (compile without errors)
- [ ] Generated CMakeLists.txt is valid CMake
- [ ] Generated CI workflows are valid YAML

### Generated CI Workflows

- [ ] Generated `build.yml` passes on all platforms when run on the generated project
- [ ] Generated `validate.yml` passes all validators on the generated project
- [ ] Workflow files use correct GitHub Actions syntax
- [ ] Matrix configurations are correct for each platform

---

## Acceptance Criteria Summary

The following checklist must be fully satisfied before Pulp v1.0 public release:

### Core Functionality
- [ ] All v1-required capabilities from the capability matrix are implemented and tested
- [ ] Audio processing is correct at all standard sample rates and buffer sizes
- [ ] MIDI processing is correct for all standard message types
- [ ] State save/restore is correct across all plugin formats

### Format Compliance
- [ ] AU format passes `auval` validation
- [ ] VST3 format passes `pluginval` validation (strictness level 10)
- [ ] VST3 format passes Steinberg's official validator
- [ ] CLAP format passes `clap-validator`
- [ ] All formats pass golden-file audio comparison tests

### DAW Compatibility
- [ ] Plugins load and operate correctly in Ableton Live (macOS and Windows)
- [ ] Plugins load and operate correctly in Logic Pro (macOS)
- [ ] Plugins load and operate correctly in Reaper (all platforms)
- [ ] Plugins load and operate correctly in at least 2 additional DAWs from the compatibility matrix

### Build and Distribution
- [ ] Build succeeds on macOS ARM64, Windows x64, and Linux x64
- [ ] macOS: signed, notarized `.pkg` installs cleanly
- [ ] Windows: signed installer installs cleanly
- [ ] Linux: `.tar.gz` extracts and installs correctly
- [ ] Auto-update cycle works end-to-end on macOS and Windows

### Performance
- [ ] Audio callback jitter < 1ms variance
- [ ] UI paint time < 16ms (CPU) / < 8ms (GPU)
- [ ] Incremental build time < 10 seconds
- [ ] Plugin scan time < 100ms

### Developer Experience
- [ ] New developer can create, build, and run a plugin in under 30 minutes
- [ ] All `/pulp:` commands work correctly on all platforms
- [ ] Documentation is complete and accurate

### Quality
- [ ] No known critical bugs
- [ ] No memory leaks detected by sanitizers
- [ ] No undefined behavior detected by sanitizers
- [ ] 3 or more real-world plugins successfully built and distributed with Pulp
