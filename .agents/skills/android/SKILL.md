---
name: android
description: Android platform development for Pulp — NDK cross-compilation, Oboe audio, Dawn/Skia GPU rendering, JNI bridge, touch interaction, emulator workflows. Covers build, deploy, debug, and the gotchas discovered during bringup.
requires:
  scripts:
    - tools/cmake/PulpAndroid.cmake
  tools:
    - adb
    - emulator
---

# Android Skill

Build, deploy, and debug Pulp on Android. This skill captures the architecture decisions and hard-won gotchas from the Android platform bringup.

## Architecture Overview

```
Kotlin (Android UI)          C++ (Pulp core)
├── PulpApplication          ├── libpulp.so (arm64)
│   └── System.loadLibrary   │   ├── core/* subsystems
├── PulpSurfaceView          │   ├── Dawn (Vulkan backend)
│   ├── SurfaceHolder.Cb     │   ├── Skia Graphite
│   └── onTouchEvent ──JNI──→│   └── Oboe audio
├── PulpActivity             │
├── PulpAudioService         └── gpu_surface_android.cpp
└── PulpMidiService              ├── Surface lifecycle
                                 ├── Touch → View routing
                                 └── Widget hierarchy
```

### Key Files

| File | Purpose |
|------|---------|
| `tools/cmake/PulpAndroid.cmake` | NDK toolchain integration, Oboe, platform wiring |
| `core/render/platform/android/gpu_surface_android.cpp` | Vulkan surface, touch routing, widget hierarchy |
| `android/app/src/main/kotlin/com/pulp/render/PulpSurfaceView.kt` | SurfaceView + touch dispatch |
| `android/app/src/main/kotlin/com/pulp/PulpApplication.kt` | JNI load, lifecycle |
| `core/platform/src/android/jni_bridge.cpp` | JNI_OnLoad, class caching, exception guards |
| `android/build-skia-android.sh` | Skia m144 Graphite + Dawn arm64 build script |

### Branch & Worktree

Android work lives on `feature/android-targeting`, typically in a worktree at `../pulp-android`:

```bash
# Create worktree (if not already present)
git worktree add ../pulp-android feature/android-targeting

# Always work in the worktree, not the main repo
cd ../pulp-android
```

## Verify dev environment first

Before any Android work — `pulp doctor android` is the single-stop
verifier. It checks:

- Android SDK location (ANDROID_HOME, ANDROID_SDK_ROOT, or per-host
  default at `~/Library/Android/sdk` / `~/Android/Sdk` / `%LOCALAPPDATA%\Android\Sdk`).
- NDK install + version listing.
- `adb` (platform-tools) on PATH or under the SDK.
- `emulator` + at least one configured AVD.
- **Optional accelerator** — Google's Android CLI (#355) for faster
  agent-driven iteration. See § "Android CLI accelerator" below for
  when to reach for it and which platforms ship a binary.

Each missing piece comes with a per-host install hint
(brew / apt / winget / sdkmanager).

## Android CLI (#355) — what it actually is, when to reach for it

Google's "Android CLI" (the `android` binary at
`~/.android-cli/bin/android` after install) is **not** a Gradle
replacement — there is no `android build` subcommand. It's an
agent-side toolkit that bundles:

| Command                       | What you'd otherwise do |
|-------------------------------|--------------------------|
| `android create [template]`   | Scaffold a new project from a template (vs. Android Studio's New Project wizard). `android create list` shows the templates. |
| `android describe`            | Emit project metadata as JSON — apk paths, build targets, etc. The agent reads this instead of poking around `app/build/outputs`. |
| `android run --apks=…`        | `adb install` + `am start` rolled into one. **Does not build** — you give it pre-built APKs. |
| `android emulator create/list/start/stop` | AVD lifecycle without `avdmanager`. ⚠️ `emulator` subcommands disabled on Windows; use `$ANDROID_HOME/emulator/emulator.exe` directly there. |
| `android layout [-d]`         | Dump the running app's view hierarchy as JSON; `-d` returns just what changed since the last call. |
| `android screen capture` / `screen resolve` | Screenshot + label-to-coordinates so the agent can `input tap #5` style scripting. |
| `android docs search/fetch`   | Query the Android Knowledge Base from the CLI (`kb://` URLs). |
| `android sdk install/list/remove/update` | Replaces `sdkmanager` for package management. |
| `android skills add/remove/list/find` | Install Google's published Android Skills (see § Catalog below). |
| `android init`                | One-shot install of the `android-cli` skill into the host agent's skill directory. |
| `android info`                | Print the active SDK path. |
| `android update` / `-V`       | Self-update / version check. |

So: **Pulp's actual build still runs through Gradle + CMake/NDK** —
the CLI doesn't accelerate that leg. What the CLI accelerates is
**agent productivity** (less hand-rolled `adb` scripting, less
manual screenshot capture, structured project metadata).

### Platform support matrix (Google-published)

| Host                   | CLI binary | Use it? |
|------------------------|------------|---------|
| macOS arm64            | ✅          | Yes — fast-iteration mode. Install via `pulp doctor android` hint. |
| Linux x86_64           | ✅          | Yes — same as above. |
| Windows x86_64         | ✅          | Yes — but note: per Google's Known Issues, the `android emulator` subcommand is currently disabled on Windows. Run the emulator directly from `%ANDROID_HOME%\emulator\emulator.exe` (which `pulp doctor android` already discovers). The build subcommand is unaffected. |
| **Linux arm64**        | ❌          | **No binary.** Stay on Gradle. Pulp's CI Linux ARM64 host (`ssh ubuntu`) is in this bucket. |
| **Windows arm64**      | ❌          | **No binary.** Stay on Gradle. Pulp's `ssh win2` ARM64 Windows host is in this bucket. |
| **macOS Intel**        | ❌          | Not in Google's matrix. Stay on Gradle. |

`pulp doctor android` reports your host's status under "Google
Android CLI (optional accelerator)" — green when the binary is
installed on a supported host, green-with-explanation on
unsupported hosts (no install hint to follow), yellow with an
install command on supported hosts where the binary is absent.

### When to reach for it

- **Iterating on the Kotlin shell**: you've changed C++, rebuilt
  via Gradle (or `pulp build --android` once that wraps it), and
  now need to push the APK + relaunch:
  `android run --apks=android/app/build/outputs/apk/debug/app-debug.apk`
  is faster than `adb install` + `am start` with the right activity.
- **UI scripting from the agent**: `android layout -d` followed by
  `android screen capture --annotate` + `screen resolve` lets you
  drive the running app without computing tap coordinates by hand.
- **Project scaffolding**: `android create empty-activity-agp-9` is
  a defensible starting point if Pulp's example Android shell ever
  needs regenerating from a current AGP template.
- **Knowledge-base lookups**: `android docs search 'foreground service lifecycle'`
  is the agent-facing replacement for hand-rolling
  developer.android.com queries.

### When NOT to use it

- **Builds** — there is no `android build`. Gradle (+ Pulp's CMake
  NDK invocation) stays the authoritative path. Don't claim the
  CLI is a build accelerator; it isn't.
- **Release / signing** — Gradle through `pulp ship` is the only
  store-grade path. The CLI's `run` is debug-install only.
- **CI** — Pulp's CI runs on mixed-arch hosts (Linux ARM64 via
  `ssh ubuntu`, Windows ARM64 via `ssh win2`) where the CLI binary
  doesn't exist. CI scripts use `adb` / `gradle` directly.
- **NDK / C++ rebuilds** — out of scope for the CLI.

### Agent compatibility

The Android CLI is **agent-agnostic**. Per Google's own docs
(developer.android.com/tools/agents): *"Use any agent of your
choice while still being able to easily leverage Android development
best practices."* Claude Code, Codex, plain bash — all use it the
same way. It is **not** Gemini-locked.

### Empirical note

Tested 2026-04-18 against this repo: the Android CLI binary
itself installs cleanly and `android info` / `android emulator
list` / `android skills list` work. But `android run --apks=...`
needs a successfully-built APK first, and Pulp's `./gradlew
assembleDebug` can fail for reasons outside the CLI's reach
(stale local checkout missing `tools/cmake/PulpInstrumentation.cmake`,
NDK version mismatch with AGP, etc.). Treat the CLI as a
post-build amplifier; it doesn't fix build-stage breakage.

A future `pulp doctor android` extension should add a
"build-configures-cleanly" check (run `./gradlew
:app:configureCMakeDebug --dry-run` and grep for `CMake Error`)
so the agent catches stale-checkout / missing-NDK issues before
attempting a full build cycle.

### Fallback contract

There is no `pulp` flag that wraps the CLI today, and given the
CLI's actual command surface (no build subcommand) there's no
clean wrap to add. The CLI is invoked directly:

```
android run --apks=android/app/build/outputs/apk/debug/app-debug.apk
android layout -p
android screen capture --output=ui.png
```

When the CLI is absent, fall back to the underlying tools the CLI
itself wraps:

| CLI absent → use this instead |
|------------------------------|
| `android run --apks=X` → `adb install -r X && adb shell am start -n com.pulp/.MainActivity` |
| `android layout` → `adb shell uiautomator dump` |
| `android screen capture` → `adb exec-out screencap -p > ui.png` |
| `android emulator start` → `$ANDROID_HOME/emulator/emulator -avd <name>` |
| `android sdk install/list` → `$ANDROID_HOME/cmdline-tools/.../bin/sdkmanager` (if installed) or via Android Studio |
| `android docs search` → just google it |

`pulp doctor android` reports CLI status; if it's missing the
agent should fall back to these commands without asking.

## Google's Android Skills catalog (github.com/android/skills)

Google publishes a set of agent skills under the
[agentskills.io](https://agentskills.io/home) open standard at
[github.com/android/skills](https://github.com/android/skills).
These follow the same `SKILL.md` + frontmatter convention Pulp
uses, so any agent that loads `.agents/skills/` can also consume
them by cloning the Android repo into its skill search path.

### Pulp-relevant skills (curated subset)

Not all 6 published skills apply to Pulp's Android target. Pulp's
Android UI is C++/Skia rendered to a `SurfaceView`, not Jetpack
Compose, so the Compose-flavoured skills are inert here.

| Google skill | Path | Pulp relevance |
|--------------|------|----------------|
| **AGP 9 upgrade** | `build/agp/agp-9-upgrade` | ✅ Use when bumping `android/app/build.gradle.kts` past AGP 8.x. Skill walks the migration steps; pair with `pulp doctor android` to confirm Gradle/SDK versions. |
| **R8 analyzer** | `performance/r8-analyzer` | 🟡 Use only if/when Pulp's Android shell ever enables R8 shrink/optimize. Pulp ships JNI bridges (`com.pulp.*`) that need explicit `-keep` rules; R8 audit is the right tool when that day comes. |
| **Make app edge-to-edge** | `system/edge-to-edge` | ✅ Pulp's full-screen Vulkan Surface should declare edge-to-edge support so the system bars don't crop the canvas. Useful when wiring `WindowCompat.setDecorFitsSystemWindows`. |
| Migrate to Compose | `jetpack-compose/...` | ❌ N/A — Pulp UI is C++/Skia, not Compose. |
| Set up Navigation 3 | `navigation/...` | ❌ N/A — Pulp's shell is single-Activity SurfaceView; no Compose Navigation. |
| Upgrade Play Billing | `play/...` | ❌ N/A — Pulp doesn't ship in-app purchases. |

### How to install

Three options, in order of preference for an agent context:

1. **`android skills add`** (requires the Android CLI):
   ```
   android skills add --all                # install everything for detected agents
   android skills add --skill=edge-to-edge # install just one
   android skills add --agent=claude-code --skill=agp-9-upgrade
   ```
   `android skills add` defaults to `~/.gemini/antigravity/skills` if
   no agents are detected, so pass `--agent` explicitly when
   installing for Claude Code or Codex. List installed skills with
   `android skills list --long`. `android init` is a one-shot helper
   that installs only the `android-cli` skill into the host agent.

2. **Clone the repo** if you want to read or version-pin offline:
   ```
   git clone https://github.com/android/skills \
       ~/.agents/external/android-skills
   ```
   Then add the relevant subdirectory to the agent's skill search
   path manually.

3. **`WebFetch` ad-hoc** when the applicability is one-off — the
   Android skills are short Markdown files and the raw URL is
   stable.

When in doubt, ask `pulp doctor android` first — it'll confirm the
CLI is present so `android skills add` is even an option.

## Build

### Cross-compile for Android

```bash
# NDK must be installed (Android Studio SDK Manager or standalone)
# Set NDK path — typically:
export ANDROID_NDK=$HOME/Library/Android/sdk/ndk/30.0.8

# Configure for Android arm64
cmake -S . -B build-android \
  -DCMAKE_TOOLCHAIN_FILE=$ANDROID_NDK/build/cmake/android.toolchain.cmake \
  -DANDROID_ABI=arm64-v8a \
  -DANDROID_NATIVE_API_LEVEL=26 \
  -DCMAKE_BUILD_TYPE=Release

# Build
cmake --build build-android -j$(sysctl -n hw.ncpu)
```

### Build the APK

```bash
cd android
./gradlew assembleDebug
# Output: android/app/build/outputs/apk/debug/app-debug.apk
```

### Host build (for tests)

```bash
# Regular host build still works in the android worktree
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j$(sysctl -n hw.ncpu)
ctest --test-dir build --output-on-failure
```

## Deploy & Test on Emulator

### Start emulator

```bash
# List available AVDs
emulator -list-avds

# CRITICAL: use -gpu host, NOT swiftshader_indirect
# swiftshader is a CPU software rasterizer that starves the audio HAL,
# causing pcm_writei I/O errors and permanent audio dropout.
# -gpu host uses Metal/MoltenVK on Apple Silicon — fast, stable audio.
QEMU_AUDIO_DRV=coreaudio emulator -avd Medium_Phone_API_36 -gpu host -no-snapshot &

# Wait for boot
adb wait-for-device
adb shell getprop sys.boot_completed  # returns "1" when ready
```

### Install and launch

```bash
adb install -r android/app/build/outputs/apk/debug/app-debug.apk
adb shell am start -n com.pulp/.PulpActivity

# If killed for slow start (Dawn init takes 3+ seconds), just retry:
adb shell am start -n com.pulp/.PulpActivity
```

### Screenshot verification

```bash
adb exec-out screencap -p > /tmp/pulp-android.png
# View with: open /tmp/pulp-android.png
```

### Touch testing via adb

```bash
# Tap at coordinates (dp)
adb shell input tap 200 400

# Drag (swipe) from point A to point B over 300ms
adb shell input swipe 200 400 200 300 300

# Take screenshot after to verify state changed
sleep 0.5 && adb exec-out screencap -p > /tmp/after-tap.png
```

### Logcat

```bash
# Filter to Pulp logs
adb logcat -s PulpAudio PulpRender Pulp

# Clear and watch
adb logcat -c && adb logcat -s Pulp
```

## Critical Gotchas

These are hard-won lessons from the bringup. Violating any of these will cause crashes or subtle bugs.

### Platform Detection Order

```cpp
// WRONG — __linux__ matches Android too!
#if defined(__linux__)
    // Linux code...
#elif defined(__ANDROID__)
    // Never reached!

// RIGHT — check Android first
#if defined(__ANDROID__)
    // Android code...
#elif defined(__linux__)
    // Linux code...
```

In CMake, `ANDROID` must be checked before `UNIX` for the same reason.

### No posix_spawn on Android

Android Bionic doesn't have `posix_spawn`. Use `fork`/`exec` instead. The `ChildProcess` subsystem already handles this with `#ifdef __ANDROID__` guards.

### Vulkan Swapchain Format

Android Vulkan uses **RGBA8**, not BGRA8 (which macOS/desktop Vulkan uses). The Dawn backend texture info must match exactly, or Skia canvas operations (especially text rendering) will crash or produce garbage.

### SkiaSurface Scaling — Don't Double-Scale

`SkiaSurface` applies `scale_factor` internally via `canvas->scale()`. If you also call `canvas->scale(density, density)` in your render function, everything renders 2x too large.

```cpp
// WRONG
canvas->scale(density, density);  // SkiaSurface already did this!
root->paint(*canvas);

// RIGHT — just paint, SkiaSurface handles the scale
root->paint(*canvas);
```

### Touch Coordinate Conversion

Android touch events arrive in physical pixels. Convert to dp before hit-testing:

```cpp
float dp_x = pixel_x / display_density;
float dp_y = pixel_y / display_density;
auto* hit = root->hit_test({dp_x, dp_y});
```

Then convert dp to view-local coordinates by walking up the parent chain:

```cpp
static view::Point to_local(view::View* target, float dp_x, float dp_y) {
    float abs_x = 0, abs_y = 0;
    for (auto* v = target; v != nullptr; v = v->parent()) {
        abs_x += v->bounds().x;
        abs_y += v->bounds().y;
    }
    return {dp_x - abs_x, dp_y - abs_y};
}
```

### MouseEvent Dispatch — Both Rich and Legacy

Some widgets (like Fader) use `on_mouse_event(MouseEvent)` to track drag state. Others use the simple `on_mouse_down(Point)`. Always dispatch both:

```cpp
view::MouseEvent ev;
ev.position = local;
ev.window_position = dp;
ev.button = view::MouseButton::left;
ev.pointer_type = view::PointerType::touch;
ev.pressure = pressure;
ev.is_down = true;
ev.click_count = click_count;

hit->on_mouse_event(ev);   // Rich event (Fader needs this)
hit->on_mouse_down(local);  // Legacy event (Toggle, Knob need this)
```

### Fader Orientation Default

`Fader::orientation_` defaults to `Orientation::vertical`. If you want horizontal faders, set explicitly:

```cpp
fader->set_orientation(Fader::Orientation::horizontal);
```

### View Hierarchy Depth and Flex Layout

Deeply nested Panel containers (Panel > Panel > Widget) break flex layout propagation — child bounds remain {0,0,0,0} and hit_test returns the parent Panel instead of the widget. Keep hierarchies flat:

```cpp
// WRONG — nested wrappers break layout
auto wrapper = std::make_shared<Panel>();
auto inner = std::make_shared<Panel>();
inner->add_child(knob);
wrapper->add_child(inner);
root->add_child(wrapper);

// RIGHT — add directly or one level deep
auto row = std::make_shared<Panel>();
row->add_child(knob1);
row->add_child(knob2);
root->add_child(row);
```

### Don't Call layout_children() on Sub-Panels Early

Calling `layout_children()` on a sub-panel before it has been assigned bounds by its parent's layout pass will hang or produce zero-size children. Only call `layout_children()` on the root after setting root bounds.

### JNI Symbol Export

Use `--whole-archive` when linking `libpulp.so` to ensure JNI symbols are exported:

```cmake
target_link_options(pulp-jni PRIVATE
    -Wl,--whole-archive $<TARGET_FILE:pulp-platform> -Wl,--no-whole-archive
)
```

### Skia Build Dependencies

Skia headers reference `src/` and `modules/` from the source tree. Dawn headers need both source and generated includes. The build script `build-skia-android.sh` handles this, but if you're debugging include paths:

```cmake
target_include_directories(pulp-render PRIVATE
    ${SKIA_SOURCE_DIR}          # for src/ headers
    ${SKIA_SOURCE_DIR}/modules  # for skparagraph etc.
    ${DAWN_SOURCE_DIR}/include
    ${DAWN_BUILD_DIR}/gen/include
)
```

### Oboe Compiler Warnings

NDK 30 Clang is stricter than desktop Clang. Suppress warnings in Oboe with `-w`:

```cmake
if(TARGET oboe)
    target_compile_options(oboe PRIVATE -w)
endif()
```

### Emulator StorageManager Corruption

If you see `NullPointerException` on `StorageManager.getVolumes()`, the emulator's data partition is corrupted. Fix:

```bash
# Kill emulator
adb emu kill
# Restart with data wipe
emulator -avd <avd_name> -wipe-data -no-snapshot
```

### Guard PulpAndroid.cmake with if(ANDROID)

The `include(PulpAndroid.cmake)` in root CMakeLists.txt must be wrapped in `if(ANDROID)` to prevent any side effects on non-Android builds:

```cmake
if(ANDROID)
    include(${CMAKE_CURRENT_SOURCE_DIR}/tools/cmake/PulpAndroid.cmake)
endif()
```

## Widget Painting Convention

When mapping a 0..1 value to a pixel position with a circular/rect indicator, **always inset by the indicator's radius** so it stays fully within widget bounds:

```cpp
// Convention for slider-like widgets:
float usable = length - 2.0f * radius;
float pos = radius + value * usable;
```

This applies to: Fader thumb, XYPad dot, CorrelationMeter bar, ColorPicker cursors. Knob and Toggle already self-inset by design.

## Widget Animations Without a Render Loop

Many widgets use `ValueAnimation` for visual state changes (Toggle thumb position, Knob hover scale, Fader hover). Without a continuous render loop (AChoreographer), `advance_animations(dt)` is never called, so animations are started but never progress — the widget's visual state appears stuck even though the logical state changed.

**Fix**: Before each paint, advance animations with a large dt to snap to completion:

```cpp
// Recursively advance all widget animations
static void advance_view_animations(view::View* v, float dt) {
    if (auto* k = dynamic_cast<view::Knob*>(v))   k->advance_animations(dt);
    if (auto* f = dynamic_cast<view::Fader*>(v))   f->advance_animations(dt);
    if (auto* t = dynamic_cast<view::Toggle*>(v))  t->advance_animations(dt);
    for (size_t i = 0; i < v->child_count(); ++i)
        advance_view_animations(v->child_at(i), dt);
}

// In render function, before paint:
advance_view_animations(g_root_view.get(), 1.0f);  // 1s snaps to target
```

This is a workaround until AChoreographer provides a proper frame loop with real dt values.

### SkFontMgr Requires a FreeType Scanner (Skia m132+)

Since Skia m132, `SkFontMgr_New_Android` and `SkFontMgr_New_FontConfig` require a non-null `SkFontScanner`. Passing `nullptr` causes SIGSEGV when rasterizing glyphs (not during font manager creation — the crash is deferred to `drawSimpleText`).

```cpp
// WRONG — crashes in drawSimpleText
mgr = SkFontMgr_New_Android(nullptr, nullptr);

// RIGHT — pass a FreeType scanner
#include "include/ports/SkFontScanner_FreeType.h"
mgr = SkFontMgr_New_Android(nullptr, SkFontScanner_Make_FreeType());
```

Do NOT use `SkFontMgr_New_AndroidNDK` for API < 30 — it has locale API bugs on Android 10.

### Safe Area Insets — Status Bar, Nav Bar, Notch

Android surfaces render edge-to-edge. Content must account for system UI (status bar, navigation bar, display cutouts). Pass insets from Kotlin to C++ via JNI:

```kotlin
// Kotlin: read insets and pass to C++ as dp values
ViewCompat.setOnApplyWindowInsetsListener(this) { _, insets ->
    val bars = insets.getInsets(WindowInsetsCompat.Type.systemBars())
    nativeSetSafeAreaInsets(
        bars.top / density, bars.bottom / density,
        bars.left / density, bars.right / density
    )
    insets
}
```

```cpp
// C++: apply to root panel padding
g_root_view->flex().padding_top = std::max(12.0f, safe_top + 4.0f);
g_root_view->flex().padding_bottom = std::max(12.0f, safe_bottom + 4.0f);
```

Without this, content renders under the status bar. The `+4.0f` adds breathing room beyond the bare minimum.

### Label Widget Text Not Rendering Under Nested Skia Graphite Clips

`Label::paint()` calls `canvas.fill_text()` which works in isolation, but text doesn't appear when painted through the View hierarchy's nested `save/translate/clip_rect` stack on Skia Graphite + Vulkan. This is likely a Graphite-specific issue.

**Workaround**: Draw section labels directly on the canvas after `paint_all()`:

```cpp
// After paint_all, draw labels at absolute dp coordinates
canvas.set_fill_color(Color::rgba(205, 214, 244));
canvas.set_font("sans-serif", 22);
canvas.fill_text("PULP SYNTH", label_x, pad_top + 22);
```

Use spacer Panels in the flex layout to reserve vertical space for each label.

### Desktop-Only Targets Must Be Guarded on Android

CLI, MCP server, inspector, tests, and examples are desktop-only. Guard them in the root CMakeLists.txt:

```cmake
if(NOT ANDROID)
    add_subdirectory(tools/cli)
    add_subdirectory(tools/mcp)
endif()

if(PULP_BUILD_TESTS AND NOT ANDROID)
    # ...tests...
endif()

if(PULP_BUILD_EXAMPLES AND NOT ANDROID)
    add_subdirectory(examples)
endif()
```

For libraries that conditionally link desktop-only targets (like `pulp-format` → `pulp::inspect`), use `if(NOT ANDROID)` guards rather than `if(TARGET ...)` since the target may not exist yet at configure time due to CMake ordering.

### Dawn API Renames After Rebase

Dawn regularly renames WebGPU types. After rebasing on main, watch for:
- `ShaderModuleWGSLDescriptor` → `ShaderSourceWGSL`
- `choc::value::ValueView` temporaries may not bind to non-const references — use `auto` (not `auto&`)

### No posix_spawn — Also Affects View Code

`posix_spawn` isn't just missing from platform code — view utilities like `ContentSharer` on the Linux `#else` fallback path also hit it. Guard with `#elif defined(__ANDROID__)` stubs:

```cpp
#elif defined(__ANDROID__)
// Android: file sharing done via JNI/Intent — stub for now
void ContentSharer::share_file(const std::filesystem::path&, void*) {}
```

## Oboe Audio Integration

The demo synth uses Oboe with shared atomic parameters for lock-free UI → audio communication:

```cpp
// Shared params — written by UI thread, read by audio callback
struct SynthParams {
    std::atomic<float> osc_pitch{0.5f};
    std::atomic<float> filter_cutoff{0.65f};
    // ...
};

// UI sync (called each frame from render loop)
static void sync_ui_to_synth() {
    auto& p = synth_params();
    p.osc_pitch.store(knob->value(), std::memory_order_relaxed);
}

// Audio callback (Oboe, lock-free)
oboe::DataCallbackResult onAudioReady(...) {
    float pitch = p.osc_pitch.load(std::memory_order_relaxed);
    // ... generate audio ...
}
```

Start audio after the render loop starts, stop in `nativeOnShutdown` (NOT in `android_surface_destroyed`).

### Oboe Stream Mode — Shared, Not Exclusive

**Never use `SharingMode::Exclusive` or `PerformanceMode::LowLatency` on the emulator.** The Ranchu virtual audio HAL can't handle exclusive access — it produces `pcm_writei I/O error` storms that permanently break the audio pipe until emulator restart. Use:

```cpp
builder.setPerformanceMode(oboe::PerformanceMode::None)
    ->setSharingMode(oboe::SharingMode::Shared)
```

On a real device, Exclusive + LowLatency is fine and preferred. Detect emulator vs hardware at runtime if you need both paths.

### Audio Lifecycle — Don't Kill on Surface Transitions

The Activity lifecycle causes surface destroy/recreate during normal transitions (configuration change, initial layout). Three rules:

1. **Don't stop the synth in `android_surface_destroyed()`** — the surface gets destroyed and recreated during Activity transitions. The synth should survive this.
2. **Don't abandon audio focus in `onPause()`** — the Activity gets briefly paused during surface transitions. Move `abandonFocus()` to `onDestroy()`.
3. **Stop the synth only in `nativeOnShutdown()`** — called from `onDestroy()` when the Activity is actually going away.

Violating any of these causes audio to play for 0.4–1.7 seconds then go silent, making it look like an emulator bug when it's actually a lifecycle bug.

### Audio Focus Is Required

Without `AudioFocusRequest`, Android may silently mute the Oboe stream. Request focus in `onResume()`:

```kotlin
audioFocus = PulpAudioFocus(this)
// in onResume:
audioFocus.requestFocus()
// in onDestroy:
audioFocus.abandonFocus()
```

### CLI Audio Debugging

Monitor audio output without speakers using logcat peak levels:

```bash
adb logcat -s Pulp | grep "Audio peak"
# Output: Audio peak: 0.130 (-17.7 dB)
```

This logs every ~3 seconds. If peaks are >0 but you hear nothing, the issue is the emulator audio pipe, not the synth.

## Render Thread with Looper (ANR Prevention)

Dawn shader compilation takes 10-15 seconds on the emulator. Running it on the main thread causes ANR. The fix: a dedicated render thread with its own Looper for AChoreographer:

```kotlin
// Kotlin: launch render thread in surfaceCreated
renderThread = Thread({
    Looper.prepare()              // AChoreographer needs a Looper
    renderLooper = Looper.myLooper()
    nativeOnSurfaceCreated(holder.surface)  // Dawn init happens here (slow)
    initComplete = true
    Looper.loop()                 // Blocks — processes AChoreographer callbacks
}, "PulpRenderThread").also { it.start() }
```

Key rules:
- **AChoreographer requires a thread with a Looper** — plain `std::thread` won't work
- **Dawn/Skia context must be used from the thread that created them** — render-thread affinity
- **Touch events must NOT call android_render_frame()** — the choreographer loop handles all rendering
- **surfaceDestroyed must be non-blocking during init** — if Dawn is still compiling, don't join/block
- **Quit the Looper on surface destroy**: `renderLooper?.quitSafely()` + `thread.join(5000)`

### Emulator GPU Mode — NEVER Use swiftshader_indirect

`swiftshader_indirect` is a CPU software rasterizer. Dawn shader compilation under swiftshader pegs virtual CPU at 100%, starving the VirtIO audio pipe and System UI threads. Symptoms:
- `pcm_writei failed with I/O error` from Ranchu HAL (permanent audio dropout)
- "System UI isn't responding" ANR dialog
- App killed during startup

**Always use `-gpu host`** on macOS Apple Silicon. This maps Vulkan to Metal via MoltenVK, offloading rendering to the GPU.

## JS-Scripted UI (QuickJS)

The synth UI can be created entirely via JavaScript using QuickJS (via CHOC). The JS script is embedded as a C++ string literal — no APK asset loading needed:

```cpp
#include "synth_ui.js.h"  // contains kSynthUiScript

// In create_demo_view_hierarchy:
auto engine = std::make_unique<ScriptEngine>();
auto bridge = std::make_unique<WidgetBridge>(*engine, root, store);
bridge->load_script(kSynthUiScript);  // JS creates all widgets
root.layout_children();
```

The JS API: `createKnob()`, `createFader(id, "horizontal", parent)`, `createToggle()`, `createXYPad()`, `createMeter()`, `createLabel()`, `createRow()`, `createCol()`, `setFlex()`, `setValue()`, `setFontSize()`, `setTextColor()`, `setBackground()`, `on()`.

Widget refs for synth param sync are captured by ID:
```cpp
auto* knob = dynamic_cast<Knob*>(bridge->widget("osc-0"));
```

Falls back to C++ widget hierarchy if JS fails (any exception → catch → C++ fallback).

## Known Blockers

1. **x86_64 Skia build** — Only arm64 Skia is built. Emulator runs arm64 via translation but an x86_64 build would be faster.

---

*Android CLI integration last verified against Google's docs:
2026-04-18 (Pulp #389). New CLI commands, install-URL changes, and
known-issue resolutions are tracked by the planned
`android-cli-monitor` workflow (#390) which polls Google's docs
twice a month and files an issue when the surface drifts. Until
that lands, refresh by hand if it's been > 1 month since the
verification date above.*
