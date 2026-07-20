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

Build, deploy, and debug Pulp on iOS — both standalone AUv3 host apps and AUv3 app extensions. This skill captures iOS-specific architecture, simulator workflows, AUv3 packaging, and GPU host gotchas.

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

**Audio etiquette for Sim launches**: `simctl launch --console …` opens a virtual coreaudio device that routes through the host Mac's `coreaudiod` — any non-muted audio path the app exercises plays out the host's speakers. Per CLAUDE.md → *Working with AI Tools* → *Local-dev audio etiquette*, announce before launching ("heads up — about to launch the Sim, audio may be active for ~30s"), cap the verify duration, and `simctl terminate` + `simctl shutdown` when done. Tracked as issue [#3173](https://github.com/Generous-Corp/pulp/issues/3173).

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

> **Bundle-id containment is enforced at configure time.** The AUv3
> extension's bundle id (`pulp_add_ios_auv3(... BUNDLE_ID ...)`) must be
> the host app's bundle id (`pulp_add_ios_host_app(... BUNDLE_ID ...)`)
> plus at least one extra dot-component — e.g. host `com.example.host`,
> extension `com.example.host.MySynth`. A sibling id, or an id equal to
> the host's, builds fine but fails late at `xcrun simctl install` with
> `IXErrorDomain code=2 / Mismatched bundle IDs`. `pulp_add_ios_host_app`
> now FATAL_ERRORs at configure (`PulpIosHostApp.cmake`, "must be nested
> under") so the mistake surfaces immediately instead of after a
> multi-minute iOS build. Regression test:
> `test/cmake/test_ios_hostapp_bundle_guard.sh`.

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

### Required HostApp entitlement: `inter-app-audio`

On **iOS 11+ real device**, `AVAudioUnitComponentManager.components(matching:)`
returns an empty list when the host app lacks `inter-app-audio` — your AUv3
appears invisible even though `pkd` indexed it correctly. The iOS Simulator
does NOT enforce this, so the gap is silent until you test on hardware.

The fix lives in two places:

1. **One-time portal action** (covers every future Pulp example forever):
   Apple Developer portal → Identifiers → the `com.<you>.pulpdev.*` wildcard
   App ID → Edit → tick **Inter-App Audio** → Save. IAA is one of the few
   capabilities Apple allows on a wildcard. Xcode auto-fetches the regenerated
   profile on next build. See `docs/guides/ios-dev-signing.md`.
2. **HostApp entitlements file** (already wired in
   `templates/ios-auv3/HostApp/Entitlements.plist.in`):
   ```xml
   <key>inter-app-audio</key>
   <true/>
   ```
   `pulp_add_ios_host_app()` configures this into each HostApp's
   `${target}.entitlements` and sets `CODE_SIGN_ENTITLEMENTS`.

Verify after build:
```bash
codesign -d --entitlements :- path/to/HostApp.app | plutil -p -
# Expected: "inter-app-audio" => 1
```

Apple deprecated IAA in iOS 13 (no new IAA-only plug-ins on the Store), but the
entitlement still gates AUv3 host scanning. Do not strip it from host apps.

## Validation

```bash
# On device (AUv3 must be installed from App Store or sideload)
auval -v aumu mySy EXMP        # type / subtype / manufacturer

# Simulator sanity (compile + load, no audio render)
xcodebuild test -project ... -scheme AUv3Tests -sdk iphonesimulator
```

## Gotchas (hard-won)

### Platform

- **Deployment target floor is 16.3; recipes use 16.4** — iPhoneSimulator SDK
  26.x marks `std::to_chars` (`<format>`/`<charconv>` floating-point) as
  available only on iOS 16.3+. Anything lower will fail with cryptic errors
  inside `__format/formatter_floating_point.h`.
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
  exist on iOS. `hot_reload.hpp` is gated so the iOS path gets a no-op
  `HotReloader`; keep file-watched hot reload off for AUv3 / HostApp builds.

### Frame timing (CADisplayLink)

- **Never hand the animation system a hardcoded `1.0f/60.0f`.** Every iOS host
  drives frames from a `CADisplayLink`, and on a 120 Hz ProMotion iPad the link
  fires ~120×/s. A constant 1/60 per tick made animation time run at *double
  speed* on those devices (and slow whenever ticks were dropped or coalesced).
  Both iOS hosts now route through the shared pump in
  `core/view/include/pulp/view/host_frame_pump.hpp`:
  `begin_host_frame(root, clock, pump, frame_time, needs_repaint)` measures the
  dt, pumps the FrameClock's activity probes with it, and reports whether the
  frame composites; `advance_host_frame(root, clock, dt)` then advances the
  FrameClock, widget animations, and CSS timelines by that ONE dt.
- **Take the timestamp from the link, not from a wall-clock read.** Use
  `link.targetTimestamp` (when the frame will actually be *presented*) — it stays
  correct when the callback is delivered late. Seed the pump's nominal interval
  from `link.duration` (1/120 on ProMotion) via `set_nominal_frame_dt`, so the
  first frame and any wake-from-idle frame advance by one real frame of *that*
  display.
- **Suspend the pump whenever the link stops.** `HostFramePump::suspend()` on
  `stop_display_link()` / `start_display_link()`: a UI that idled at 0 fps for 30 s
  must not resume with a 30-second dt — that would teleport any animation that
  starts on wake straight to its end state.
- **`window_host_ios.mm` used to own no FrameClock at all** (only
  `plugin_view_host_ios.mm` did), so a *standalone* iOS app advanced nothing but
  gesture recognizers — no FrameClock subscribers, no widget animations, no CSS
  timelines. If you add a third iOS host, bind a `FrameClock` + `HostFramePump`
  and go through the shared pump; do not re-invent the tick.

### Toolchain

- **Use the Xcode generator**, not Unix Makefiles/Ninja with
  `tools/cmake/ios.toolchain.cmake` — the vendored toolchain has a recursive
  `enable_language(C)` bug with non-Xcode generators.
- **`CLI` and `MCP` targets are desktop-only** — gate with `NOT IOS AND NOT ANDROID`
  (they depend on process spawning and `MACOSX_BUNDLE` install rules).
- **`pulp::inspect` is desktop-only** — it needs Dawn/Skia GPU. Gate the link
  in `core/format/CMakeLists.txt`.
- **`clang -fsyntax-only` iOS check — two flags matter or you get false
  errors.** When syntax-checking an ObjC++ host file (e.g.
  `window_host_ios.mm`) against the iPhoneOS SDK, (1) target **iOS 16.3 or
  later** (`-target arm64-apple-ios17.0`): `runtime::log.hpp` pulls in
  `std::format`, whose float path calls `std::to_chars`, marked *unavailable
  before iOS 16.3* — a lower target fails deep in libc++ with an error that
  looks like your code but isn't; and (2) do **NOT** pass `-fobjc-arc` — the iOS
  host files use manual retain/release (`[super dealloc]`), so ARC turns
  pre-existing, correct code into "ARC forbids explicit dealloc" errors. Pull
  the real include dirs from the build's `OBJCXX_INCLUDES` line in
  `build/core/view/CMakeFiles/pulp-view-core.dir/flags.make` (note: the .mm uses
  `OBJCXX_INCLUDES`, not `CXX_INCLUDES`).

### Window host (GPU) — dpi_scale + design viewport

- **`IOSGpuWindowHost` implements `dpi_scale()` + the design viewport**, mirroring
  the macOS GPU host: `dpi_scale()` returns `window_.screen.scale` (the base
  default 1.0 was a live Retina coordinate bug on every 2x/3x device);
  `set_design_viewport` / `design_viewport_transform` pin the root to design
  size and letterbox it in `render_frame` via the shared, x-platform-tested
  `WindowHost::compute_design_viewport_transform`. `set_fixed_aspect_ratio` is
  deliberately left to the base (self-diagnosing) no-op — iOS has no
  user-resizable window to aspect-lock. NOTE: the standalone GPU host's
  `PulpMetalWindowView` currently processes **no touches**, so the paint-side
  design viewport has no touch coordinate to misalign yet; touch inverse-mapping
  is the follow-up when that view gains touch handling (contrast with
  `IOSGpuPluginViewHost` below, which does own the `pointTransform` touch map).

### Input

- **Touch events** — UIKit's `touchesBegan/Moved/Ended/Cancelled` route through
  `window_host_ios.mm`. The handlers build a root-space `MouseEvent` with a
  stable pointer id, `PointerType::touch` or `PointerType::pen`, and an explicit
  `MousePhase`, then call `View::dispatch_gesture_pointer_event(...)` before the
  legacy widget path. If the arbiter consumes the event, do not also dispatch
  `View::on_mouse_{down,up,drag}(Point)` or JS pointer fallback events. For
  `touchesCancelled`, set `is_cancelled = true` and use the release phase so
  recognizers cancel/fail consistently.
- **Apple Pencil** — `UITouch.type == UITouchTypePencil` sets
  `MouseEvent::pointer_type = PointerType::pen` plus altitude/azimuth.
- **Multi-touch** — each `UITouch*` gets a stable `pointer_id` via
  `stableIdForTouch:` so widgets that use `set_pointer_capture()` work correctly.
- **GPU AUv3 editor touch → JS pointer events** — the standalone CPU paths
  call only the position-based `View::on_mouse_{down,up,drag}(Point)` virtuals,
  which native widgets consume but which do NOT dispatch JS pointer events. The
  GPU AUv3 editor view (`PulpMetalPluginView` in `plugin_view_host_ios.mm`)
  originally had NO touch handlers at all, so a scripted/Three.js editor was
  inert to touch. It now hit_tests `rootView`, captures a per-pointer drag
  target, and fires `View::on_mouse_event` (→ JS `pointerdown`/`up`) plus the
  new `View::on_pointer_move` (→ JS `pointermove` carrying real
  `pointerId`+`pointerType:'touch'`, which `on_drag` collapses to
  `pointerId:0/'mouse'`), mirroring the mac `pulp_plugin_mouse_*` dispatch.
  Multi-pointer identity is what lets Three.js OrbitControls pinch-zoom track
  two fingers. **Gotcha:** dispatching pointer events to a canvas widget id is
  NOT enough — code that listens on `ownerDocument` (OrbitControls moves its
  move/up listeners there after `pointerdown`) needs the bridge's
  document fan-out in `__dispatch__` (widget_bridge.cpp); the element bubble
  walk never reaches the `document` object. **`view_is_in_tree`-style
  validation of a captured drag target MUST walk root→down (compare pointer
  identity), never target→parent** — the captured `View*` can be freed by an
  editor rebuild between touch events, so walking its `parent()` is a UAF.
- **Touch injection on the Simulator is not scriptable here** — `simctl` has no
  touch/gesture command, the Simulator exposes no AX window, and
  `cliclick`/pyobjc-Quartz aren't installed, so a live finger-drag can't be
  auto-injected. To prove a touch→JS chain, drive synthetic events through the
  real `__dispatch__(<canvas._id>, 'pointermove', {...})` entrypoint (the same
  one the native handlers call) from a hot-patched `scene.js` and assert camera
  state / `document`-level listener counts in the `dev.pulp.runtime` log.
- **GPU AUv3 editor fills the pane (responsive), no forced design viewport** —
  `IOSGpuPluginViewHost` CAN scale a fixed design viewport (it overrides
  `set_design_viewport`/`set_fixed_aspect_ratio`/`set_design_viewport_top_align`/
  `window_to_root_point`, and `render_frame` applies an aspect-correct
  translate+scale that the Three.js cube composites through coherently). BUT
  the iOS AU view controller (`au_view_controller_ios.mm`) deliberately does
  NOT force a design viewport: aspect-locked scaling letterboxed the pane (dark
  bars on the sides) and pushed header text to the edge. It instead lays the
  root out at the ACTUAL pane bounds (`resizeEditorToViewBounds` → `set_size` +
  `bridge->resize`) so a responsive flex scene fills edge-to-edge. The scene
  must be responsive to benefit: `examples/ios-auv3-jsc-threejs/js/scene.js`
  flex-fills the body/shell at `width:100%` and `syncCanvasSize()` resizes the
  Three.js drawing buffer + camera aspect to the canvas's measured size each
  frame (a fixed-size canvas would just sit small in a wide pane). To fill the
  pane you ALSO need `ContentView.swift` to give the editor
  `.frame(maxWidth:.infinity, maxHeight:.infinity)`. A genuinely fixed-aspect
  editor that wants letterboxing can call `set_design_viewport` itself.
- **Host-app AUParameter slider must mirror value in `@State`** — binding a
  SwiftUI `Slider` straight to `AUParameter.value` (get/set) does NOT update:
  SwiftUI doesn't observe `AUParameter`, so dragging writes the param but never
  re-renders, leaving the thumb + readout stuck at 0.00. The `ParameterRow`
  view in `templates/ios-auv3/HostApp/ContentView.swift` mirrors the value in
  `@State` (slider + readout bind to that, so they move live), writes through
  in `onChange` via `setValue(_, originator: token)`, and installs an
  `AUParameterObserverToken` to reflect host/automation changes back — passing
  the token as originator so the observer doesn't echo the gesture.

### Plugin Editor Child Views

- **`PluginViewHost` now mirrors `WindowHost` for native child views** —
  `plugin_view_host_ios.mm` supports `attach_native_child_view(...)`,
  `set_native_child_view_bounds(...)`, `set_native_child_view_clip(...)`, and
  `detach_native_child_view(...)` for `UIView` children embedded inside
  AUv3/plugin editors.
- **Native-child clipping uses a `CALayer` mask, and UIKit layers are already
  top-left** — `set_native_child_view_clip(...)` (used by the `NativeViewHost`
  widget so an embedded WebView/text-field/video layer clips to its scroll
  ancestor) sets `child.layer.mask` to a `CALayer` sized to the visible sub-rect
  expressed in the child's own `[0,0,w,h]` box. Unlike AppKit (where a
  non-flipped `NSView` layer is bottom-left and the helper flips the Y), a
  `UIView`'s layer is top-left origin, so the local clip maps **directly** with
  no Y-flip — do not copy the mac flip math into the iOS helper. `has_clip=false`
  removes the mask. Masking clips WITHOUT resizing the child, so a `WKWebView`
  does not reflow when it scrolls past a viewport edge.
- **Subview trees inherit `plugin_view_host()` recursively before
  `Processor::on_view_opened(...)` fires** — if a `View` creates a
  `WebViewPanel` or other native-backed child in a plugin editor, wire it
  through `View::plugin_view_host()`, not a standalone `WindowHost`.
- **Detach on close** — plugin-editor child views must be explicitly detached
  in `on_view_closed()`/destructors just like standalone window-hosted child
  views; the host clears propagated references when subtrees are removed.
- **Gate `notify_attached()` on `is_attached()`, never assume attach
  succeeded** — `PluginViewHost` exposes `is_attached() const noexcept` and
  `[[nodiscard]] try_attach_to_parent(...)`. The iOS hosts override
  `is_attached()` with the truthful native check (`view_.superview != nil`,
  and `metal_view_.superview != nil` for the GPU host). A foreign or
  AUv3 embedder must call `try_attach_to_parent()` (or check `is_attached()`
  after `attach_to_parent()`) before firing `ViewBridge::notify_attached()` —
  firing it when the parent rejected the view leaves the editor open/close
  lifecycle unbalanced. The base default is conservative (`false`), so a host
  that has not opted in never reports a phantom attach. Query on the UI thread.
- **Standalone `WindowHost` now reports live content bounds on iOS** —
  `window_host_ios.mm` exposes `WindowHost::get_content_size()` and
  `set_resize_callback(...)` on both the CPU and Metal hosts, driven from
  `layoutSubviews`. For native child embeds, size from the host's reported
  content bounds instead of hard-coding `UIScreen.mainScreen.bounds`.
- **`UIViewControllerRepresentable` observer registrations must be paired
  with `dismantleUIViewController` removal** — the HostApp template's
  `PulpAUv3EditorView` mounts the AUv3 editor by registering a closure on
  the `@StateObject` `PulpAUv3Host` that captures the container
  `UIViewController`. SwiftUI rebuilds the representable on orientation
  change, scene reset, and iPad split-view shuffles, calling
  `makeUIViewController` each time. If the observer list is append-only,
  every rebuild leaks one container VC for the lifetime of the host
  (which is the lifetime of the SwiftUI app). Use the
  install-token / remove-by-token pattern in
  `templates/ios-auv3/HostApp/ContentView.swift`: store the token in a
  `Coordinator`, call `removeEditorObserver(token)` from the static
  `dismantleUIViewController(_:coordinator:)` hook, and capture the
  container VC weakly inside the closure as a second layer of safety.

### Native drag-and-drop (UIDrop/UIDrag)

- **iOS drag-and-drop is interaction-based, not the AppKit `NSDraggingSession`
  model.** `plugin_view_host_ios.mm` installs a `PulpIOSDragDrop` coordinator
  (conforms to `UIDropInteractionDelegate` + `UIDragInteractionDelegate`) on
  BOTH host views (`PulpPluginUIView` CPU + `PulpMetalPluginView` GPU), bridging
  to the same cross-platform dispatch core (`dispatch_drag_*` / `dispatch_drop`)
  the mac/win/linux hosts use.
- **`start_file_drag()` ARMS, it does not start.** UIKit begins a drag only from
  the system long-press lift, which calls
  `dragInteraction:itemsForBeginningSession:`. So `PluginViewHost::start_file_drag`
  on iOS stages the `FileDragRequest.file_paths` and returns true; the next lift
  consumes them (`itemsForBeginningSession` returns `@[]` when nothing is armed,
  so no stray drags). This differs from AppKit, where `begin_file_drag` starts a
  drag synchronously inside the mouse handler. A Pulp widget that wants outbound
  drag must call `start_file_drag` during its own long-press handling.
- **Drop coordinate transform differs by host.** The drop point
  (`[session locationInView:hostView]`) is root-space directly on the CPU host
  (identity transform) but must go through the Metal view's **live**
  `pointTransform` (the inverse design-viewport map touches use) on the GPU host.
  `plugin_view_host_ios.mm` currently compiles without ARC in the generated
  iOS host-app CMake/Xcode path, so copy the transform block into the
  coordinator ivar and release it in `dealloc`; storing the caller's stack
  block directly can dangle after install.
- **`performDrop` completion runs on the main queue** (UIKit guarantee), so it
  touches the view tree safely — but keep the coordinator alive with explicit
  MRC retain/release while the async `loadObjectsOfClass:` completion is
  outstanding and re-check the root pointer inside the block. An async
  completion can outlive the host; the coordinator's `invalidate` (called from
  both host dtors) nils the root so a late completion is a no-op.
- **No headless test** — UIKit drag/drop is gesture-driven and can't be exercised
  by the macOS Catch2 suite; validated by local Simulator compile
  (`clang -fsyntax-only -target arm64-apple-ios…-simulator`) + manual gesture.
  Same gesture-untestable category as touch input.

### Accessibility

- `UIAccessibility` is the iOS equivalent of NSAccessibility. `accessibility_ios.mm`
  bridges `AccessibilityNode` → `UIAccessibilityElement`. Unlike macOS, iOS
  accessibility is **opt-in per view**: `isAccessibilityElement = YES`.
- **UIKit has no role STRING — only traits.** `View::AccessRole` is a much
  richer vocabulary than `UIAccessibilityTraits`, so several roles legitimately
  collapse onto `UIAccessibilityTraitButton` (checkbox, radio, combo box, menu
  item, tab); VoiceOver then announces "button" and reads the element's
  `accessibilityValue` for state. That is UIKit's own idiom (UISwitch does the
  same) — do not "fix" it by inventing a trait.
- **`AccessRole::text_field` carries NO trait, and that is a real gap.**
  VoiceOver derives "text field" from the element conforming to `UITextInput`,
  not from a trait. Pulp exposes plain `UIAccessibilityElement`s, so a
  `TextEditor` announces as untyped text. Closing this means implementing
  `UITextInput` on the accessibility element, not adding a case to
  `access_role_to_traits`.
- `access_role_to_traits()` deliberately has **no `default:` case** — adding an
  `AccessRole` must be a compile error in `accessibility_ios.mm`, not a silent
  fall-through to `UIAccessibilityTraitNone`. The iOS lane is not built by the
  required macOS gate, so this is the only thing that catches an unmapped role.

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
    BUNDLE_ID          com.pulp.examples.sinesynth.host.PulpSineSynth
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
    DEPLOYMENT_TARGET 16.4                        # defaults to 16.3
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
- Visual regression for iOS UIs is not wired into CI yet; use simulator/device
  screenshots for visual validation.

## Platform Gotchas

### Cross-subsystem deps in `pulp-view-core` must hold on iOS

`pulp::host` is intentionally not added on iOS (App Store policy plus
`std::format` / `long double to_chars` libc++ availability on the
iPhoneSimulator SDK). That guard lives in the root `CMakeLists.txt`
and in `core/view/CMakeLists.txt`'s `pulp-view-core` link list. Two
follow-on contracts must hold for `pulp-view-core` to actually link
on iOS:

1. Every `pulp::*` library that a view source `#include`s must be in
   the PUBLIC link list. The iOS lane catches this only if the smoke
   actually builds the HostApp. The typical failure mode is a view
   source including a subsystem header, such as
   `<pulp/audio/audio_thumbnail.hpp>`, without linking that subsystem.
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

The same SDK availability constraint can surface through direct
`std::from_chars` use for floating-point parsing in view code. Prefer integer or
manual decimal parsing for small numeric-token paths that must build for the
current iOS deployment floor; only use floating `from_chars` when the active SDK
and deployment target both prove it is available.

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

Touched here because `core/format/src/au_view_controller_ios.mm` is dual-owned by `ios` + `auv3` + `view-bridge`. The view controller's ivars are declared `_bridge`, `_fallbackView`, `_viewHost` (the GPU-plugin-view-host work reordered them — see below) and destroyed in REVERSE declaration order, so `_viewHost` is destroyed FIRST. That makes `root_.set_plugin_view_host(nullptr)` / `set_frame_clock(nullptr)` in `~PluginViewHost` safe on BOTH paths (the bridge's view and the `_fallbackView` are still alive). Calling `_bridge->close()` explicitly in `dealloc` reverses that order, frees the View first, and the host's destructor then dereferences a dangling reference — crashes AUv3 editor close. Full rationale lives in the `auv3` skill under "PulpAUViewController::dealloc — never call `_bridge->close()` explicitly". **Teardown must also run on the MAIN thread:** the GPU host's `CVDisplayLink` idle pump is dispatched to the main queue and dereferences the bridge, so `dealloc` resets `_viewHost` via `dispatch_sync(main)` when off-main (an AUv3 controller can be released on the XPC queue) before the reverse-order ivar destruction — otherwise a queued idle block races the free (SIGSEGV in `display_link_callback`). See the `view-bridge` skill, "AU v3 teardown must ALSO run on the main thread."

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

### Mounting an AUv3 editor inside a SwiftUI HostApp on iOS (iOS-D.2)

On macOS, AU v3 hosts open the editor inside their own window and the
developer never has to wire SwiftUI to a `UIViewController`. On iOS the
HostApp container ships with the .appex and has to mount the editor
itself if the dev wants the in-app smoke to surface the plug-in UI
(GarageBand iOS / AUM do their own mounting in production).

The contract (proven end-to-end on iPad Pro 13-inch M5 in iOS-D.2):

1. After `AVAudioUnit.instantiate(with:options:.loadOutOfProcess)`
   succeeds, call `node.auAudioUnit.requestViewController { vc in … }`
   on the main queue. The XPC view-controller fetch can take 1–2
   render-loop ticks — render a placeholder SwiftUI label in the
   meantime, not an empty container.
2. Wrap the returned `UIViewController` in a
   `UIViewControllerRepresentable` whose container `UIViewController`
   adopts the editor as a child VC (`addChild`, pinned constraints,
   `didMove(toParent:)`). Do NOT try to mount the AUv3's own `view`
   directly — the editor lifecycle expects the parent VC to be present.
3. Re-mount on every refresh: the AU host can swap the editor (e.g.
   after a preset reset), so keep the `setEditor(_:)` pathway live
   instead of mounting once in `makeUIViewController`.

See `templates/ios-auv3/HostApp/ContentView.swift` for the canonical
SwiftUI host implementation that ships in the template; the
`PulpAUv3EditorView` struct is the reference pattern.

### iOS GPU host must run idle_callback inside the CADisplayLink tick

`IOSGpuWindowHost::tick()` (in `core/view/platform/ios/window_host_ios.mm`) is the per-vsync entry point and MUST invoke `idle_callback_()` BEFORE the `needs_repaint` check. Without this, JS `requestAnimationFrame` / `setTimeout` / async-result queues never fire on iOS GPU because `set_idle_callback` can otherwise behave like a no-op relative to the CADisplayLink loop.

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
  It now uses the current API (`GpuSurface::create_dawn()` +
  `SkiaSurface::create()`). This path is only validated where iOS Skia is
  fetched and linked; the `examples/ios-auv3-jsc-threejs` path below is the
  current simulator proof. Verify other AUv3 GPU-host changes on a
  device/simulator.
- **Embeddability matches mac:** display link starts on `-didMoveToWindow`
  (not `attach_to_parent`); `-layoutSubviews` re-syncs size/scale; `tick()`
  pumps `idle_callback_` first (the idle-pump-in-tick contract — keep it), then frame clock
  + CSS animations; liveness token; GpuSurface resized PHYSICAL, SkiaSurface
  LOGICAL+scale; CPU fallback in the factory when `is_gpu_backed()` is false.

### `examples/ios-auv3-jsc-threejs` — Three.js cube through the AUv3 GPU host

The `PulpThreeJsDemo` example runs real `three.webgpu.js` in JSC inside an
AUv3 `.appex`, painting through `PulpMetalPluginView` → Dawn → Skia Graphite →
CAMetalLayer. The rotating cube renders on the iOS Simulator when iOS Skia libs
are present at `external/skia-build/build/ios-gpu/.../libskia.a`, which the GPU
build requires. Build with `-DPULP_ENABLE_GPU=ON
-DPULP_REQUIRE_GPU_FOR_SDK=ON` for iphonesimulator arm64; the host app
auto-presents the editor.

The deep WebGPU-bridge gotchas (geometry uploaded as zero via
`getMappedRange`, JS-guessed bind-group layout vs `layout:"auto"`, the
load-bearing Sim `skip_validation` toggle, capturing the out-of-process appex's
logs via the `dev.pulp.runtime` os_log subsystem, and the
`$<PLATFORM_ID:Darwin,iOS,...>` host-classification link gate) live in the
**`threejs-bridge`** skill — read it before touching the WebGPU shim or the
iOS GPU draw path.

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

### iOS GPU (Skia/Dawn) header-namespace gotcha — Phase iOS-D.1

When the iOS configure first turns on Skia (i.e.
`PULP_ENABLE_GPU=ON` + `PULP_HAS_SKIA` is set), two pre-existing bugs
in `core/view/platform/ios/{window_host,plugin_view_host}_ios.mm`
surfaced — they were inert because no prior iOS build had ever defined
`PULP_HAS_SKIA`:

1. **Skia / Dawn / Metal headers `#include`d inside an open
   `namespace pulp::view {`.** Symptom is a wall of compile errors
   from Apple's Metal headers — "Objective-C declarations may only
   appear in global scope" on every `@protocol`. Cause: the open
   namespace nests every header symbol under `pulp::view`, and
   `@protocol` outside the global scope is a hard syntax error.
   Fix: close the namespace before the `#include` block, reopen it
   after.

2. **`std::make_unique<render::GpuSurface>()` /
   `<render::SkiaSurface>()`.** Both classes are abstract — use the
   factories `render::GpuSurface::create_dawn()` and
   `render::SkiaSurface::create(GpuSurface&, Config)`. The Config
   struct exposes only `width` / `height` / `scale_factor`; Dawn
   device/queue/instance are queried from the GpuSurface internally.

3. **`pulp::render` was not linked into `pulp-view-core` on iOS.** The
   mac branch in `core/view/CMakeLists.txt` linked `pulp::render`
   under `if(PULP_HAS_SKIA)`; the iOS branch did not. Phase iOS-D.1
   adds the same gate.

### iOS GPU AUv3 acceptance log markers — Phase iOS-D.1

A GPU-backed AUv3 view that's actually using Skia/Dawn on iOS emits a
specific log sequence (see
`planning/2026-05-28-ios-d-gpu-auv3-crosscheck.md`):

```
[plugin-gpu-host] adapter mode=custom use_gpu=true ... requires_gpu_host=true
GpuSurface: created Metal surface from CAMetalLayer
GpuSurface: Dawn initialized (surface: presentable)
GpuSurface: backend_type=Metal     ← added by Phase iOS-D.1
AU iOS: view controller loaded, ... mode=custom, gpu=true
```

Capture with:

```bash
xcrun simctl spawn booted log stream \
  --predicate 'process == "<AppName>" OR eventMessage CONTAINS "GpuSurface" OR eventMessage CONTAINS "[plugin-gpu-host]" OR eventMessage CONTAINS "AU iOS"' \
  --level debug
```

Missing `backend_type=Metal` (or `backend_type=Null` / `OpenGL`)
means GPU init silently fell back to a non-Metal adapter — treat
that as a P0 since the AUv3 is then rendering through software. The
first frame may also log a benign
`WebGPU error (2): First instance (1) must be zero` — that's an
upstream Skia/Dawn first-frame issue, not a Pulp regression.

### iOS GPU configure hard-fail — `PULP_REQUIRE_GPU_FOR_SDK`

The release-lane guard makes the contradiction
`PULP_REQUIRE_GPU_FOR_SDK=ON + PULP_ENABLE_GPU=OFF` hard-fail at
configure time. Test:
`bash test/cmake/test_require_gpu_for_sdk.sh <pulp-src>` — three
cases (REQUIRE+ENABLE+missing-Skia fail; REQUIRE off succeeds;
REQUIRE on + ENABLE off fails). Add a case here whenever a new flag
contradicts an existing one.

### `IOSGpuPluginViewHost::gpu_surface()` exposes the host's wgpu::Surface (Phase iOS-D.3b Slice 1)

`PluginViewHost` now has a `virtual render::GpuSurface* gpu_surface()`
mirroring `WindowHost::gpu_surface()`. `IOSGpuPluginViewHost` overrides
it to return `gpu_surface_.get()`; the CPU `IOSPluginViewHost` inherits
the nullptr default.

The AUv3 iOS view controller calls
`bridge->scripted_ui()->attach_gpu_surface(_viewHost->gpu_surface())`
right after `PluginViewHost::create()` succeeds, so the JS-side
`navigator.gpu` / `canvas.getContext('webgpu')` shim talks to Pulp's
real Dawn instance. Without it the JS GPU bridge falls through to mocks
and any embedded WebGPU content (Three.js, raw WebGPU) renders black.

See the `view-bridge` skill's "GpuSurface plumbing into WidgetBridge"
section for the cross-platform contract and
`planning/2026-05-29-ios-d3b-threejs-webgpu-program.md` § Slice 1 for
the full rationale.

## See Also

- `android` skill — parallel structure for Android NDK.
- `view-bridge` skill — `au_v2_cocoa_view.mm` + `au_view_controller_ios.mm` linkage.
- `ci` skill — how iOS builds integrate into PR validation (when added).
- `threejs-bridge` skill — iOS WebGPU and Three.js runtime contracts.

## Three.js inside AUv3 on iOS

Pulp ships a Three.js-on-iPad path inside AUv3 extensions via JSC + an
esbuild-bundled IIFE wrapper for `three.webgpu.js`. The non-obvious bits worth
remembering:

### JSC's ESM module-loader API is NOT public on iOS

`iPhoneOS.sdk/JavaScriptCore.framework/Headers/` ships **no** `JSScript.h`, **no** `JSModuleLoaderDelegate`, **no** `setModuleLoaderDelegate:`. Shipping any code path that uses those in an `.appex` risks App Store rejection at submission time. The supported path is:

1. Bundle `three.webgpu.js` (ESM) at build time into a self-contained IIFE wrapper via `tools/scripts/bundle_threejs_for_jsc.mjs` — the script delegates ESM resolution and IIFE generation to esbuild, strips dev-only `http:` imports through an esbuild plugin, then wraps the namespace onto `globalThis.THREE`.
2. Load the resulting `three.iife.js` at runtime via `pulp::view::threejs_iife_source()` (`core/view/include/pulp/view/threejs_resources.hpp` + `core/view/src/threejs_resources_apple.mm`). The NSBundle loader walks up from `PulpAUViewController` to the `.appex` and reads `threejs/three.iife.js`.
3. JSC `evaluate()` the source — `THREE` lands on `globalThis`. No resolver needed.

If you reach for `setModuleLoaderDelegate:` because it's mentioned in WebKit internal docs, stop. Use the IIFE path. Future contributors who don't see this note will spend a day discovering this on their own.

### Bundle invocation in CMake

`tools/cmake/PulpAuv3.cmake` `_pulp_add_auv3_ios()` adds a `POST_BUILD` step that runs `node tools/scripts/bundle_threejs_for_jsc.mjs --input <threejs.webgpu.js> --output <appex>/threejs/three.iife.js`. The step is gated on `find_program(node)` — if Node.js is missing on the build host, the embed is skipped with a STATUS message and `pulp::view::threejs_iife_source()` returns `std::nullopt` at runtime (clean failure, not a build break).

### Log marker chain (grep these on iPad device walks)

```
[plugin-gpu-host] adapter mode=custom use_gpu=true ... requires_gpu_host=true
GpuSurface: backend_type=Metal
PULP_WEBGPU_BRIDGE: canvas.getContext('webgpu') ok (presentable=true)
PULP_WEBGPU_BRIDGE: context.configure ok (format=bgra8unorm, size=WxH)
PULP_WEBGPU_BRIDGE: queue.submit ok (canvas=X, commands=N)
PULP_THREEJS: bundle loaded (N bytes)
PULP_THREEJS: globalThis.THREE available
PULP_THREE_SHIM: ready
PULP_THREE_SHIM: webgpu-renderer-present
```

The `presentable=true|false` boolean is the program's most load-bearing signal — `false` means JS draws went to an offscreen texture, not the visible swapchain. If the iPad demo shows a black editor pane, grep for `presentable=false` first.

### Buffered-draw probe

`globalThis.__phase13BufferedSkips` is the canonical "bridge gave up" probe. After a Three.js render call, the array should be empty. If it's non-empty, each entry is a `JSON.stringify(...)` of the skipped draw — that's where a missing native-bridge function call surfaces. The macOS V8 lane uses the same probe; the JSC lane behaves identically by design (`widget_bridge.cpp` registers each `__gpu*Impl` engine-agnostically via `engine_.register_function(...)`).

### Memory probe

The .appex peak resident-set after `first frame painted` is the program's memory exit gate. Phase iOS-D.3b deliberately defers adding Increased Memory Limit (`com.apple.developer.kernel.increased-memory-limit`) and Extended Virtual Addressing (`com.apple.developer.kernel.extended-virtual-addressing`) entitlements — instrument the number first, escalate only if real-device testing shows pressure.

### Putting the IIFE in front of a plugin's scripted-UI script (iOS-D.3c, 2026-05-29)

The iOS-D.3b plumbing landed the IIFE bundler and the NSBundle loader, but it did NOT add a "register native function before script runs" hook on `WidgetBridge` / `ScriptedUiSession`. A Pulp plugin that wants `globalThis.THREE` populated before its scene code runs has exactly two practical options today:

1. **Concatenate at runtime (recommended for plugin examples).** In `Processor::create_view()`, read `pulp::view::threejs_iife_source()`, read the bundled scene script via `[NSBundle pathForResource:ofType:inDirectory:@"threejs"]`, concatenate `IIFE + shim + scene`, write to `NSTemporaryDirectory()`, and hand THAT path to `ScriptedUiSession::ScriptedUiOptions::script_path`. The session's `load()` eval-orders everything correctly because it all lives in one file. Cleanup happens in `Processor::on_view_closed()`. This is what `examples/ios-auv3-jsc-threejs/` does — keep the per-instance filename unique (encoding `this` works) so split-view / multi-insert hosts don't collide on the tempfile.

2. **Use the framework's default editor path** (`PULP_UI_SCRIPT_PATH` via `pulp_add_plugin`). The IIFE has to live INSIDE that script file (you'd `configure_file` it in at build time). Bigger build hammer, but means no runtime concat. Suitable for plugin authors who already manage their script via Pulp's existing tooling.

Don't reach for "register a native function on the bridge that returns the IIFE source then call eval(...) from JS first thing." There is no public API on `ScriptedUiSession` / `WidgetBridge` to register native functions BEFORE `load_script` runs, and registering AFTER means the JS evaluate has already failed on `new THREE.WebGPURenderer(...)`. Concatenation is the smaller change.

### iOS bundle layout reminder

`<appex>/threejs/three.iife.js` (flat). NOT `<appex>/Resources/threejs/three.iife.js` (that's the macOS layout; iOS bundles are flat). The runtime loader (`core/view/src/threejs_resources_apple.mm`) queries the flat path. If you see "three.iife.js missing from bundle" from a successful build, double-check the POST_BUILD wrote under `<appex>/threejs/`, not `<appex>/Resources/threejs/`.

### Per-example POST_BUILD copies

When an example bundles its OWN sibling resources (e.g. `examples/ios-auv3-jsc-threejs/js/scene.js`), use a `add_custom_command(TARGET <name>_AUv3 POST_BUILD ...)` that writes under the same `$<TARGET_BUNDLE_DIR:...>/threejs/` directory so the NSBundle lookup finds it via the same `inDirectory:@"threejs"` query as Three.js. Don't put example assets under a sibling folder unless you also update the loader to search that folder.

### Arming PULP_HAS_THREEJS on the iOS target

The root `tools/cmake/PulpDependencies.cmake` gate that fetches Three.js is `if(PULP_BUILD_TESTS AND PULP_ENABLE_GPU)`. iOS forces `PULP_BUILD_TESTS=OFF` (the root `CMakeLists.txt` does this at top), so the FetchContent doesn't fire automatically. An example that needs the bundler step running on iOS must do its own subdirectory-local FetchContent before calling `pulp_add_ios_auv3()`:

```cmake
if(NOT PULP_HAS_THREEJS OR NOT DEFINED threejs_SOURCE_DIR)
    include(FetchContent)
    pulp_register_fetchcontent_source(threejs REF <same-pin-as-root>)
    FetchContent_Declare(threejs GIT_REPOSITORY ... GIT_TAG ...)
    FetchContent_MakeAvailable(threejs)
    set(PULP_HAS_THREEJS TRUE)
endif()
```

The variables are subdir-scoped so OTHER iOS examples in the same configure (synth, chainer, gpu-smoke) don't accidentally pick up the FetchContent. The `_pulp_add_auv3_ios()` helper reads both variables in-function and arms the bundler POST_BUILD step.

### First-install discovery race

After a fresh `devicectl install` on an iPad (or first launch after deleting + reinstalling the HostApp), iOS may not have finished scanning the embedded `.appex` when the SwiftUI HostApp's `.onAppear` fires. `AVAudioUnitComponentManager.shared().components(matching:)` then returns 0 matches, the HostApp prints `PULP_DISCOVER: no matching Pulp AUv3 found`, and the user is stuck on that state until they manually kill + relaunch the app multiple times.

The HostApp template in `templates/ios-auv3/HostApp/ContentView.swift` now wires two recovery paths:

1. **Notification observer**: subscribes to `AVAudioUnitComponentManagerRegistrationsChangedNotification` (Apple's documented signal that the AU component registry changed) and re-runs `discover()` when iOS finishes its scan.
2. **Polling fallback**: 60 ticks at 1s each, guards the case where the notification doesn't fire on the first foreground after install. Each tick is a separate `Task { @MainActor in ... }` hop so Swift 6 strict-concurrency is happy.

**Idempotency is mandatory.** Without it, the polling fallback re-calls `AVAudioUnit.instantiate(...)` every tick — each call creates a fresh `AudioUnit` instance, which tears down the AUv3's editor view and crashes `pulp::format::ViewBridge::~ViewBridge()` with `EXC_BAD_ACCESS` because the editor is mid-render when the AU is replaced. The fix is the `instantiationInFlight: Bool` field on `PulpAUv3Host`:

```swift
func discover() {
    if audioUnit != nil { return }
    if instantiationInFlight { return }
    instantiationInFlight = true
    // ... scan + instantiate ...
}
```

Cleared in both the success path (inside the `Task { @MainActor in ... }` after `AVAudioUnit.instantiate` succeeds) AND the bail paths (no matching component found, instantiate callback returns `error != nil`). Forgetting either bail path leaks the in-flight flag and the discovery never retries.

Grep `PULP_DISCOVER` + `PULP_INSTANTIATE` in the launch log to trace the discovery cycle; the new code reuses these existing log markers and adds no new noise.
