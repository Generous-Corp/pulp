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

Builds the `.appex` only. The App Store container host-app target is authored separately — `templates/ios-auv3/HostApp/` has a SwiftUI host you can copy into Xcode. Auto-generating the host target is follow-on work.

## Follow-ups / Known Gaps

- Full `pulp-view` iOS build requires gating `hot_reload.hpp`.
- `pulp_add_ios_auv3()` ships the `.appex`; host-app target generation (consuming `templates/ios-auv3/HostApp/`) is follow-on.
- Device-target code signing is documented but not scripted.
- Visual regression for iOS UIs not wired up yet (see #330, #249).

## Gotchas

### `PulpAUViewController::dealloc` — never call `_bridge->close()` explicitly

Touched here because `core/format/src/au_view_controller_ios.mm` is dual-owned by `ios` + `auv3` + `view-bridge`. The view controller's ivars `_bridge`, `_viewHost`, `_fallbackView` are destroyed in REVERSE declaration order by the runtime; the resulting sequence (host destroyed before bridge) is what makes the `root_.set_plugin_view_host(nullptr)` call in `~PluginViewHost` safe. Calling `_bridge->close()` explicitly in `dealloc` reverses that order, frees the View first, and the host's destructor then dereferences a dangling reference — crashes AUv3 editor close. Codex P1 review on PR #653. Full rationale lives in the `auv3` skill under "PulpAUViewController::dealloc — never call `_bridge->close()` explicitly".

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

## See Also

- `android` skill — parallel structure for Android NDK.
- `view-bridge` skill — `au_v2_cocoa_view.mm` + `au_view_controller_ios.mm` linkage.
- `ci` skill — how iOS builds integrate into PR validation (when added).
- Issue #218 — AUv3 mobile validation workstream.
