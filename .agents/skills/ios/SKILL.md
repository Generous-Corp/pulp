---
name: ios
description: iOS platform development for Pulp — iPhone/iPad AUv3 app extensions, iOS Simulator builds, UIKit window host, CoreAudio IO audio, touch & Apple Pencil input, XcodeBuildMCP automation. Covers configure, build, deploy to simulator/device, and the gotchas discovered during iOS bringup.
requires:
  scripts:
    - tools/cmake/PulpUtils.cmake
  tools:
    - xcodebuild
  optional_tools:
    - xcrun simctl
    - XcodeBuildMCP
---

# iOS Skill

Build, deploy, and debug Pulp on iOS — both standalone AUv3 host apps and AUv3 app extensions. This skill captures iOS-specific architecture and gotchas uncovered during the iOS Simulator bringup (PR #222, issue #218).

## Architecture Overview

```
Swift / UIKit (iOS UI)        C++ (Pulp core)
├── HostApp (Xcode target)    ├── libpulp-format.a
│   └── AVAudioEngine         │   └── PulpAudioUnit (AUv3)
├── AUv3 App Extension        │
│   ├── Info.plist/NSExtension│   ├── core/view/platform/ios/
│   └── MetalView (optional)  │   │   ├── window_host_ios.mm (UIView)
└── CoreAudio IO              │   │   ├── plugin_view_host_ios.mm
                              │   │   └── accessibility_ios.mm
                              │   └── core/platform (UIKit stubs)
```

### Key Files

| File | Purpose |
|------|---------|
| `core/view/platform/ios/window_host_ios.mm` | UIView root, touch/pencil dispatch → `View::on_mouse_*` |
| `core/view/platform/ios/plugin_view_host_ios.mm` | AUv3 editor UIView |
| `core/view/platform/ios/accessibility_ios.mm` | UIAccessibility bridge |
| `core/format/src/au_view_controller_ios.mm` | AUv3 NSExtensionPrincipalClass |
| `core/platform/CMakeLists.txt` (`IOS` branch) | UIKit link, omit Cocoa/fork-exec |
| `tools/cmake/PulpUtils.cmake` (`PULP_IOS` blocks) | AUv3 bundle, Info.plist generation, `pulp_add_ios_auv3()` helper |
| `templates/ios-auv3/Info.plist.in` | AUv3 extension manifest template |

## Configure & Build

### iOS Simulator (arm64)

```bash
# Xcode generator is required — the bundled tools/cmake/ios.toolchain.cmake
# has a recursive enable_language bug with Makefile/Ninja generators.

cmake -S . -B build-ios-sim -G Xcode \
  -DCMAKE_SYSTEM_NAME=iOS \
  -DCMAKE_OSX_SYSROOT=iphonesimulator \
  -DCMAKE_OSX_ARCHITECTURES=arm64 \
  -DCMAKE_OSX_DEPLOYMENT_TARGET=16.4 \
  -DPULP_ENABLE_GPU=OFF \
  -DPULP_BUILD_TESTS=OFF \
  -DPULP_BUILD_EXAMPLES=OFF

# Build a target
xcodebuild -project build-ios-sim/Pulp.xcodeproj \
  -target pulp-format -configuration Debug \
  -sdk iphonesimulator -arch arm64 \
  IPHONEOS_DEPLOYMENT_TARGET=16.4 build
```

### iOS Device (arm64)

```bash
cmake -S . -B build-ios \
  -G Xcode \
  -DCMAKE_SYSTEM_NAME=iOS \
  -DCMAKE_OSX_SYSROOT=iphoneos \
  -DCMAKE_OSX_ARCHITECTURES=arm64 \
  -DCMAKE_OSX_DEPLOYMENT_TARGET=16.4 \
  -DCMAKE_XCODE_ATTRIBUTE_CODE_SIGN_IDENTITY="Apple Development" \
  -DCMAKE_XCODE_ATTRIBUTE_DEVELOPMENT_TEAM=<TEAM_ID>
```

## Deploying to the Simulator

Pulp standardizes on **XcodeBuildMCP** when available (fast, structured output). Falls back to raw `xcodebuild` + `xcrun simctl`.

### With XcodeBuildMCP (preferred)

Check availability: if the `mcp__XcodeBuildMCP__*` tools are exposed in the session, use them.

```text
session_show_defaults         # inspect current project/scheme/sim
list_sims                     # enumerate booted/shutdown simulators
build_sim                     # build for selected simulator
install_app_sim               # install .app
launch_app_sim                # launch + stream logs
screenshot                    # capture for visual regression
```

Preferred simulator: **iPhone 17 Pro** on iOS-26 runtime for most testing (fastest on Apple Silicon). Use iPad Pro 13-inch for layout validation.

### Without XcodeBuildMCP (fallback)

```bash
# List simulators
xcrun simctl list devices available

# Boot + install + launch
xcrun simctl boot "iPhone 17 Pro"
xcrun simctl install booted path/to/MyApp.app
xcrun simctl launch --console booted com.example.MyApp
```

**Policy**: assume XcodeBuildMCP when the user has it configured; do not hard-require or auto-install it. Leave install to user preference.

## AUv3 App Extension

> **macOS AU v3 is architecturally different — see the `auv3` skill.**
> The macOS path (Phase 3.5) uses framework + stub .appex + container .app;
> iOS stays monolithic. The threading hard guard (XPC queue →
> `dispatch_async(main)` before any UIKit access) applies to BOTH
> platforms, and is implemented in
> `core/format/src/au_view_controller_ios.mm` for iOS. The lifecycle
> fix (`createAudioUnitWithComponentDescription:` must actually
> instantiate `PulpAudioUnit`, then `rebuildEditorIfReady` shared
> between `viewDidLoad` and `setAudioUnit:`) also applies here. Full
> recipe + diagnostics live in
> `.agents/skills/auv3/SKILL.md → "macOS AU v3 packaging"`.

An AUv3 plugin on iOS ships as an **App Extension** bundled inside a **host app** (App Store requires a host container). Both targets must be in the same Xcode project.

Minimal structure:

```
examples/ios-auv3-synth/
├── HostApp/
│   ├── HostApp.entitlements
│   ├── Info.plist
│   ├── ContentView.swift          # simple SwiftUI AUv3 host
│   └── Assets.xcassets
├── AUv3Extension/
│   ├── Info.plist                 # NSExtension / audiocomponents
│   ├── AUv3Extension.entitlements
│   └── AudioUnitViewController.mm # wraps PulpAudioUnit
└── CMakeLists.txt                 # uses pulp_add_ios_auv3()
```

### Required Info.plist keys (extension)

```xml
<key>NSExtension</key>
<dict>
  <key>NSExtensionAttributes</key>
  <dict>
    <key>AudioComponents</key>
    <array>
      <dict>
        <key>description</key><string>MySynth</string>
        <key>manufacturer</key><string>EXMP</string>
        <key>name</key><string>Example: MySynth</string>
        <key>sandboxSafe</key><true/>
        <key>subtype</key><string>mySy</string>
        <key>tags</key><array><string>Synth</string></array>
        <key>type</key><string>aumu</string>
        <key>version</key><integer>0x00010000</integer>
      </dict>
    </array>
  </dict>
  <key>NSExtensionPointIdentifier</key>
  <string>com.apple.AudioUnit-UI</string>
  <key>NSExtensionPrincipalClass</key>
  <string>AudioUnitViewController</string>
</dict>
```

## Validation

```bash
# On device (AUv3 must be installed from App Store or sideload)
auval -v aumu mySy EXMP        # type / subtype / manufacturer

# Simulator sanity (compile + load, no audio render)
xcodebuild test -project ... -scheme AUv3Tests -sdk iphonesimulator
```

## Gotchas (hard-won)

### Platform

- **Deployment target ≥ 16.4** — iPhoneSimulator SDK 26.x marks `std::to_chars`
  (`<format>`/`<charconv>` floating-point) as available only on iOS 16.3+. Anything
  lower will fail with cryptic errors inside `__format/formatter_floating_point.h`.
- **No `Cocoa.h`** — mac platform files (`clipboard_mac.mm`, `file_dialog_mac.mm`,
  `popup_menu_mac.mm`) must be excluded via `APPLE AND NOT IOS`. Link `UIKit`
  instead of `Cocoa` on iOS.
- **No `posix_spawn_file_actions_addchdir_np`** — wrap the call in
  `#if !(TARGET_OS_IPHONE || TARGET_OS_TV || TARGET_OS_WATCH)`. `TargetConditionals.h`
  only exists on Apple, so include it under `#ifdef __APPLE__`.
- **No `CoreAudio/AudioToolbox` device APIs** — iOS uses `AVAudioSession` for the
  device-level work that `AudioHardwarePropertyDefaultOutputDevice` and friends
  do on macOS. Gate with `PULP_HAS_COREAUDIO_DEVICE`.
- **No FSEvents** — `choc_FileWatcher.h` pulls `FSEventStreamRef` which does not
  exist on iOS. `hot_reload.hpp` must be gated on `NOT IOS` (or provided a no-op
  iOS implementation). This is currently the last blocker for a full `pulp-view`
  iOS build (as of PR #222).

### Toolchain

- **Use the Xcode generator**, not Unix Makefiles/Ninja with
  `tools/cmake/ios.toolchain.cmake` — the vendored toolchain has a recursive
  `enable_language(C)` bug with non-Xcode generators.
- **`CLI` and `MCP` targets are desktop-only** — gate with `NOT IOS AND NOT ANDROID`
  (they depend on process spawning and `MACOSX_BUNDLE` install rules).
- **`pulp::inspect` is desktop-only** — it needs Dawn/Skia GPU. Gate the link
  in `core/format/CMakeLists.txt`.

### Input

- **Touch events** — UIKit's `touchesBegan/Moved/Ended/Cancelled` route through
  `window_host_ios.mm`. The handlers call `View::on_mouse_{down,up,drag}(Point)` —
  pass `me.position`, not the full `MouseEvent` (the View API is `Point`-based).
- **Apple Pencil** — `UITouch.type == UITouchTypePencil` sets
  `MouseEvent::pointer_type = PointerType::pen` plus altitude/azimuth.
- **Multi-touch** — each `UITouch*` gets a stable `pointer_id` via
  `stableIdForTouch:` so widgets that use `set_pointer_capture()` work correctly.

### Plugin Editor Child Views

- **`PluginViewHost` now mirrors `WindowHost` for native child views** —
  `plugin_view_host_ios.mm` supports `attach_native_child_view(...)`,
  `set_native_child_view_bounds(...)`, and `detach_native_child_view(...)`
  for `UIView` children embedded inside AUv3/plugin editors.
- **Subview trees inherit `plugin_view_host()` recursively before
  `Processor::on_view_opened(...)` fires** — if a `View` creates a
  `WebViewPanel` or other native-backed child in a plugin editor, wire it
  through `View::plugin_view_host()`, not a standalone `WindowHost`.
- **Detach on close** — plugin-editor child views must be explicitly detached
  in `on_view_closed()`/destructors just like standalone window-hosted child
  views; the host clears propagated references when subtrees are removed.
- **Standalone `WindowHost` now reports live content bounds on iOS** —
  `window_host_ios.mm` exposes `WindowHost::get_content_size()` and
  `set_resize_callback(...)` on both the CPU and Metal hosts, driven from
  `layoutSubviews`. For native child embeds, size from the host's reported
  content bounds instead of hard-coding `UIScreen.mainScreen.bounds`.

### Accessibility

- `UIAccessibility` is the iOS equivalent of NSAccessibility. `accessibility_ios.mm`
  bridges `AccessibilityNode` → `UIAccessibilityElement`. Unlike macOS, iOS
  accessibility is **opt-in per view**: `isAccessibilityElement = YES`.

### Sandboxing

- AUv3 extensions run in a sandbox with **no filesystem access** beyond the
  container. Presets must be bundled in the app's resources or fetched via
  `NSFileCoordinator` from the host app's group container.
- No `fork`/`exec`, no `NSTask` — all child-process code must be `NOT IOS`.

### Permissions (pulp::platform::Permissions)

`core/platform/platform/ios/permissions_ios.mm` is the iOS backend for the
cross-platform `pulp::platform::Permissions` API. Three rules:

1. **Always hop to the main queue** before firing the user `RequestCallback`.
   AVFoundation/UserNotifications completion blocks don't promise a specific
   queue, and iOS callers invariably touch UIKit from the callback. The
   backend owns a `RequestCallback*` on the heap, dispatches to
   `dispatch_get_main_queue()`, and deletes after invocation.
2. **`CBManager.authorization` is iOS 13.1+ only** — anything older has no
   discrete authorization surface. Wrap in `@available` and fall back to
   `PermissionState::Granted`; the system will prompt on first
   `CBCentralManager` instantiation provided `NSBluetoothAlwaysUsageDescription`
   is set in Info.plist.
3. **Don't forget the Info.plist keys.** The iOS backend only surfaces the
   request; the prompt itself is gated on `NSMicrophoneUsageDescription`,
   `NSCameraUsageDescription`, `NSBluetoothAlwaysUsageDescription`, and
   `NSLocalNetworkUsageDescription` being present. No key → prompt never
   fires and the callback delivers `Denied`.

The required frameworks (`AVFoundation`, `CoreBluetooth`,
`UserNotifications`) are linked in the `IOS` branch of
`core/platform/CMakeLists.txt`.

## `pulp_add_ios_auv3()` helper

```cmake
pulp_add_ios_auv3(
    NAME               PulpSineSynth
    BUNDLE_ID          com.pulp.examples.sinesynth
    MANUFACTURER       Pulp
    MANUFACTURER_CODE  Pulp        # exactly 4 characters
    SUBTYPE_CODE       PsSn        # exactly 4 characters
    AU_TYPE            aumu        # aumu | aufx | aumi
    VERSION            0.1.0
    SOURCES            src/sine_synth.cpp src/sine_synth.hpp
)
```

Builds the `.appex` only. iOS App-Store policy requires the `.appex` to ship inside a containing HostApp `.app` — pair the call above with `pulp_add_ios_host_app(...)` below to get an installable Simulator / device bundle.

## `pulp_add_ios_host_app()` helper (Phase iOS-B)

`pulp_add_ios_host_app(...)` (in `tools/cmake/PulpIosHostApp.cmake`) builds a SwiftUI HostApp `.app` and embeds the AUv3 `.appex` into `${target}.app/PlugIns/`.

```cmake
pulp_add_ios_host_app(PulpSineSynth_HostApp
    AUV3_EXTENSION    PulpSineSynth_AUv3        # must exist; pulp_add_ios_auv3 sets up
    BUNDLE_ID         com.pulp.examples.sinesynth.host
    NAME              "PulpSineSynth"           # display name (defaults to target)
    VERSION           0.1.0                       # defaults to "1.0.0"
    DEPLOYMENT_TARGET 16.4                        # defaults to 16.0
    # SOURCES ...                                 # optional override; default is the
                                                  # shipped HostApp/ SwiftUI template
)
```

### How the helper works

1. **Reads the AudioComponentDescription off the `.appex` target.** `_pulp_add_auv3_ios(...)` stashes `PULP_AUV3_MANUFACTURER_CODE` / `_SUBTYPE_CODE` / `_AU_TYPE` / `_VERSION_INT` / `_PLUGIN_NAME` / `_MANUFACTURER_NAME` as target properties when it creates the `.appex` target. The HostApp helper reads them back so the HostApp's `Info.plist AudioComponents` entry matches the extension exactly. **Descriptor drift between the HostApp and the extension silently breaks `AVAudioUnitComponentManager.components(matching:)`** — the helper enforces parity by reading from one source of truth.
2. **Generates the HostApp `Info.plist`** from `templates/ios-auv3/HostApp/Info.plist.in` — declares `LSRequiresIPhoneOS`, the audio background mode, supported orientations, `NSMicrophoneUsageDescription`, and the mirrored `AudioComponents` block.
3. **Embeds via a sentinel target.** `${target}_Embed` is an `ALL`-dep custom target whose sentinel depends on the `.appex` bundle output, so a `.appex`-only rebuild forces a re-embed even when the HostApp itself didn't relink. The macOS framework + appex container does the same thing for the same reason (see `PulpAuv3.cmake` comments).

### Deploy recipe

See `docs/getting-started/ios-deployment.md` for the full end-to-end: Simulator install + launch via `xcrun simctl install` / `launch`, physical-iPad install via `xcrun devicectl device install app` (Xcode 15+), GarageBand iOS validation checklist for Phase iOS-C.

### Quick reference — exact commands

```bash
# Simulator
cmake -S . -B build-ios-sim -G Xcode \
  -DCMAKE_SYSTEM_NAME=iOS -DCMAKE_OSX_SYSROOT=iphonesimulator \
  -DCMAKE_OSX_ARCHITECTURES=arm64 -DCMAKE_OSX_DEPLOYMENT_TARGET=16.4 \
  -DPULP_ENABLE_GPU=OFF -DPULP_BUILD_TESTS=OFF
cmake --build build-ios-sim --target PulpSineSynth_HostApp --config Release -- -sdk iphonesimulator
xcrun simctl boot "iPad Pro 13-inch (M5)"
xcrun simctl install booted build-ios-sim/AUv3/Release-iphonesimulator/PulpSineSynth.app
xcrun simctl launch --console booted com.pulp.examples.sinesynth.host

# Physical iPad (signed)
# Prefer the validated sourceable helper — it errors clearly on missing
# keys / unfilled placeholders. See docs/guides/ios-dev-signing.md for
# the full reusable dev-signing stub (schema template + one-time setup).
. tools/scripts/source_dev_creds.sh
cmake -S . -B build-ios-device -G Xcode \
  -DCMAKE_SYSTEM_NAME=iOS -DCMAKE_OSX_SYSROOT=iphoneos \
  -DCMAKE_OSX_ARCHITECTURES=arm64 -DCMAKE_OSX_DEPLOYMENT_TARGET=16.4 \
  -DCMAKE_XCODE_ATTRIBUTE_CODE_SIGN_IDENTITY="Apple Development" \
  -DCMAKE_XCODE_ATTRIBUTE_DEVELOPMENT_TEAM="${PULP_TEAM_ID}" \
  -DCMAKE_XCODE_ATTRIBUTE_CODE_SIGN_STYLE="Automatic"
cmake --build build-ios-device --target PulpSineSynth_HostApp --config Release -- -sdk iphoneos
xcrun devicectl device install app --device <DEVICE_UDID> \
  build-ios-device/AUv3/Release-iphoneos/PulpSineSynth.app
```

### Smoke test coverage

`test/cmake/test_ios_auv3_configure.sh` exercises both helpers:
- Default behavior (since 2026-05-27): configures + builds both the AUv3 `.appex` and the HostApp `.app`, asserts the `.appex` is embedded under `PlugIns/`, and validates the HostApp `Info.plist` carries the matching `AudioComponents.subtype`. This catches link-time regressions that configure-only smoke cannot — for example a missing `pulp::audio` PUBLIC link in `core/view` or an unguarded `<pulp/host/*>` include in a view header (both real iPad-walkthrough regressions).
- Override with `PULP_IOS_AUV3_SMOKE_BUILD=0` to fall back to configure-only when iterating locally on a slow machine.

`test/cmake/test_ios_hostapp_links.sh` is the companion link-time
regression test — it always builds the HostApp and asserts the
produced bundle has a real Info.plist (lint-clean, non-empty
`CFBundleIdentifier`), a real Mach-O executable at
`CFBundleExecutable`, and an embedded `.appex` under `PlugIns/`. Use
this script directly when triaging a "configure smoke is green but
the user's iPad has no plugin" report.

## Follow-ups / Known Gaps

- ~~Full `pulp-view` iOS build requires gating `hot_reload.hpp`.~~ Done 2026-05-27: `choc::file::Watcher` (FSEventStream-based) is now `#if !TARGET_OS_IPHONE`-gated and `HotReloader` ships a no-op iOS stub.
- ~~`pulp_add_ios_auv3()` ships the `.appex`; host-app target generation~~ Done in Phase iOS-B via `pulp_add_ios_host_app()`.
- Device-target code signing is documented but not scripted.
- Visual regression for iOS UIs not wired up yet (see #330, #249).

## Gotchas

### Cross-subsystem deps in `pulp-view-core` must hold on iOS

`pulp::host` is intentionally not added on iOS (App Store policy plus
`std::format` / `long double to_chars` libc++ availability on the
iPhoneSimulator SDK). That guard lives in the root `CMakeLists.txt`
and in `core/view/CMakeLists.txt`'s `pulp-view-core` link list. Two
follow-on contracts must hold for `pulp-view-core` to actually link
on iOS:

1. Every `pulp::*` library that a view source `#include`s must be in
   the PUBLIC link list. The iOS lane catches this only if the smoke
   actually builds the HostApp (it does as of 2026-05-27). A real
   regression from PR #3059 was `visualizers.cpp` including
   `<pulp/audio/audio_thumbnail.hpp>` without `pulp::audio` being
   linked.
2. Any view header that includes `<pulp/host/...>` must wrap that
   include in `#if defined(__has_include) && __has_include(<pulp/host/...>)`
   so transitive consumers on iOS get a clean "feature absent" macro
   rather than a "file not found" error. Affected headers today:
   `core/view/include/pulp/view/widgets/graph_editor_view.hpp`,
   `core/view/include/pulp/view/plugin_manager_panel.hpp`,
   `core/view/include/pulp/view/hosted_editor_attachment.hpp`. Each
   defines a `PULP_VIEW_HAS_*` companion macro downstream code can
   key off.

If you add a new view source or header that pulls from `pulp::audio`,
`pulp::host`, or any other subsystem that may be conditionally
absent, the iOS HostApp link smoke
(`test/cmake/test_ios_hostapp_links.sh`) is the canonical local
reproducer.

### `add_compile_options(-Wall ...)` must be gated to C/C++/ObjC languages

The Swift driver rejects clang's `-Wall` / `-Wextra` / `-Wpedantic`
flags with `Driver threw unknown argument: '-Wall' without emitting
errors`. The root `CMakeLists.txt` adds these via `add_compile_options`,
which (without a language genex) attaches them to **every** language in
the build — including any Swift target like the iOS HostApp's
`PulpHostApp.swift`. Always wrap with
`$<$<COMPILE_LANGUAGE:C,CXX,OBJC,OBJCXX>:-Wall>` etc. so Swift sees
only the flags it understands.

### iOS deployment target must be ≥ 16.3 for `std::format`

The iPhoneSimulator26.x SDK's libc++ marks `std::to_chars` for floating-
point types as `introduced in iOS 16.3 simulator`. `std::format`
unconditionally instantiates `to_chars` for `float` / `double` / `long
double` as part of its formatter type list, so any TU that includes
`<pulp/runtime/log.hpp>` (which uses `std::format`) fails to compile if
the active `IPHONEOS_DEPLOYMENT_TARGET` is below 16.3 — even when the
caller passes only strings. `pulp_add_ios_auv3()` and
`pulp_add_ios_host_app()` both default to the user-supplied
`CMAKE_OSX_DEPLOYMENT_TARGET` when present, otherwise pin to `16.3`.
Don't lower either floor below 16.3 unless `std::format` is removed
from `core/runtime/log.hpp` first.

### iOS link line: don't link `CoreAudioTypes` standalone

On macOS, `CoreAudioTypes` ships as
`/System/Library/Frameworks/CoreAudioTypes.framework`. On iOS it
**does not exist as a top-level framework** — the same headers ship
inside `AudioToolbox.framework`. Linking it standalone fails with
`framework 'CoreAudioTypes' not found` on `iphonesimulator26.x`.
Link `AudioToolbox` only; the `AudioComponentDescription` /
`AVAudioUnitComponent` types resolve through that.

### `FileDialog` / `PopupMenu` stubs must compile on iOS

`core/platform/src/file_dialog_stub.cpp` defines
`FileDialog::open_file` / `save_file` / `choose_folder` under
`#if !defined(__APPLE__)` and `core/platform/src/popup_menu_stub.cpp`
defines `PopupMenu::show` / `show_at_view` under the same guard.
macOS has native impls in `file_dialog_mac.mm` / `popup_menu_mac.mm`;
iOS has neither (UIDocumentPicker + UIMenu wiring are
follow-ups). Without an explicit iOS branch the link step fails on
`Undefined symbols` for any iOS bundle that pulls `pulp-view-core`
(WidgetBridge wires both into the JS bridge). The fix is to widen the
`#if` to also include iOS:
`#if !defined(__APPLE__) || (defined(TARGET_OS_IPHONE) && TARGET_OS_IPHONE)`.
Callers see `nullopt` / empty results — honest "unsupported" signaling
— until the native UIDocumentPicker impl lands.

### `PulpAUViewController::dealloc` — never call `_bridge->close()` explicitly

Touched here because `core/format/src/au_view_controller_ios.mm` is dual-owned by `ios` + `auv3` + `view-bridge`. The view controller's ivars are declared `_bridge`, `_fallbackView`, `_viewHost` (the GPU-plugin-view-host work reordered them — see below) and destroyed in REVERSE declaration order, so `_viewHost` is destroyed FIRST. That makes `root_.set_plugin_view_host(nullptr)` / `set_frame_clock(nullptr)` in `~PluginViewHost` safe on BOTH paths (the bridge's view and the `_fallbackView` are still alive). Calling `_bridge->close()` explicitly in `dealloc` reverses that order, frees the View first, and the host's destructor then dereferences a dangling reference — crashes AUv3 editor close. Full rationale lives in the `auv3` skill under "PulpAUViewController::dealloc — never call `_bridge->close()` explicitly".

**Ivar order is load-bearing (2026-05 fallback-UAF fix):** the old order
`_bridge, _viewHost, _fallbackView` destroyed `_fallbackView` BEFORE `_viewHost`.
On the no-`audioUnit` preview path `_fallbackView` *is* the View `_viewHost->root_`
references, so the host then cleared a back-pointer into a freed View. Declaring
`_viewHost` LAST (so it destroys first) fixes both paths. Don't reorder back.

### AUv3 headless guard must not create a fallback host

If `PULP_DISABLE_PLUGIN_EDITOR`, `PULP_HEADLESS`, `PULP_TEST_MODE`, or
`CI` is set, `PulpAUViewController` should return without constructing a
`ViewBridge`, `PluginViewHost`, or fallback empty view. That guard exists
to prevent native editor windows during validation and agent runs; the
fallback view remains only for preview/no-audioUnit cases.

### iOS GPU host must run idle_callback inside the CADisplayLink tick (pulp #1402)

`IOSGpuWindowHost::tick()` (in `core/view/platform/ios/window_host_ios.mm`) is the per-vsync entry point and MUST invoke `idle_callback_()` BEFORE the `needs_repaint` check. Without this, JS `requestAnimationFrame` / `setTimeout` / async-result queues never fire on iOS GPU because `set_idle_callback` was inheriting the `WindowHost` base class no-op (parallel gap to the macOS one fixed in PR #1400 and the Android one in #1404).

The fix is two pieces:
1. Override `set_idle_callback` to store the callback in a non-atomic field (CADisplayLink fires on `mainRunLoop` so the read-and-invoke happens on main only — no atomic guard needed unlike macOS GPU's CV thread).
2. In `tick()`, run `idle_callback_()` first; the callback can `request_repaint`, which arms `needs_repaint_` and triggers `render_frame()` in the same tick.

CPU iOS host (`IOSWindowHost`) has the same gap but no display link; `repaint()` just calls `[root_view_ setNeedsDisplay]` and UIKit drives `drawRect:` only on user interaction. A separate CADisplayLink-on-CPU-path fix is owed; not blocking AUv3 use cases since AUv3 host apps go through the GPU path.

### iOS window hosts register the main-thread dispatcher while visible

`window_host_ios.mm` registers `pulp::events::MainThreadDispatcher` backends
from both standalone window hosts so worker code can marshal work to UIKit
without depending on platform APIs. Keep these invariants:

- **Register after UIKit objects are ready, before any CADisplayLink tick can
  run.** The GPU host registers immediately after `makeKeyAndVisible` and
  before `start_display_link()` so the first idle/rAF callback sees a live
  dispatcher.
- **Unregister before clearing UIKit callbacks.** Destructors set the host
  liveness token false, unregister the dispatcher token, then nil `onResize`,
  display-link targets, root views, and window/controller references. That order
  prevents queued resize/display callbacks from reaching freed host state.
- **Do not treat `run_event_loop()` like macOS.** On iOS it returns after
  handing ownership to UIKit, so the dispatcher token must stay on the host and
  be released in teardown instead of on method exit.

### Embeddable iOS GPU *plugin* host (`IOSGpuPluginViewHost`) — distinct from the window host

`core/view/platform/ios/plugin_view_host_ios.mm` is the AUv3 plugin embed
host (vs `IOSGpuWindowHost` for standalone). Two things to know (2026-05
GPU-plugin-view-host work):

- **It was dead code on a stale render API.** The prior version did
  `std::make_unique<render::GpuSurface>()` (abstract!) and
  `SkiaSurface::initialize` with `dawn_device`/`dawn_queue` Config fields that
  no longer exist. It compiled never because **no `ios-gpu` Skia libs are
  fetched**, so `PULP_HAS_SKIA` is OFF for iOS and the block is `#ifdef`'d out.
  It now uses the current API (`GpuSurface::create_dawn()` + `SkiaSurface::create()`).
  **This whole path is CI-unvalidated** until iOS Skia is fetched/built — verify
  on a device/simulator.
- **Embeddability matches mac:** display link starts on `-didMoveToWindow`
  (not `attach_to_parent`); `-layoutSubviews` re-syncs size/scale; `tick()`
  pumps `idle_callback_` first (the idle-pump-in-tick contract — keep it), then frame clock
  + CSS animations; liveness token; GpuSurface resized PHYSICAL, SkiaSurface
  LOGICAL+scale; CPU fallback in the factory when `is_gpu_backed()` is false.

### Sim audio loop validation — recordVideo does NOT capture audio

`xcrun simctl io booted recordVideo` produces a video-only `.mov` —
the Simulator routes audio natively through the Mac's output device
but never into the recording. This caught the 2026-05-27 iOS AUv3
audio-path bringup loop: HostApp launched cleanly, all `PULP_` triage
prints showed `INSTANTIATE_OK`/`NOTE: ON`/`engine.start`-succeeded,
but the recorded video file was silent. Don't waste cycles trying to
get audio out of `recordVideo`.

Options for audio-path validation on iOS:

1. **Trust the wiring proof.** When the HostApp prints
   `PULP_INSTANTIATE_OK`, `PULP_MIDI_BLOCK: ready`,
   `PULP_NOTE: ON`, and `engine.start()` returns without throwing,
   the audio path is wired. The Sim renders through CoreAudio →
   Mac output device → host speakers; an empty audio buffer would
   manifest as `engine.start` failing or `INSTANTIATE_ERROR`. For
   most regressions this proof is sufficient.
2. **Mac audio routing tool.** BlackHole or Loopback can capture the
   Mac's output device into an audio file; combine with `recordVideo`
   for an A/V record. Worth the setup time only for true audio QA.
3. **Real device + headphone jack / mic.** Authoritative — installs
   the same `.appex` PluginKit registers on real iOS, plays through
   the device's own audio hardware. Required for any audio-quality
   judgement (the Sim's `mainMixerNode` resamples; not a faithful
   reproduction of the device).

When debugging "did the plug-in actually produce sound?" prefer
option 1 + option 3 over chasing a Sim audio capture. See
`.agents/skills/auv3/SKILL.md` "iOS AUv3 diagnostic recipe" for the
chain of `PULP_` prints to grep.

## See Also

- `android` skill — parallel structure for Android NDK.
- `view-bridge` skill — `au_v2_cocoa_view.mm` + `au_view_controller_ios.mm` linkage.
- `ci` skill — how iOS builds integrate into PR validation (when added).
- Issue #218 — AUv3 mobile validation workstream.
