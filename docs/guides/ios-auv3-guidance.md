# iOS AUv3 & Mobile Guidance

Practical guide for shipping Pulp plugins and apps on iOS and iPadOS. Covers deployment-form trade-offs, JS engine selection under App Store rules, sandbox constraints, and rendering options.

> Status: living document. Paired with the `ios` agent skill (`.agents/skills/ios/SKILL.md`) — the skill is for automation, this is for humans.

---

## Deployment forms

A Pulp project on iOS is one of three things. Pick before you scaffold — they have different Info.plist, entitlements, lifecycle, and sandbox rules.

### 1. AUv3 App Extension (recommended for plugins)

- Ships as an **App Extension bundled inside a host container app** (App Store requires the host).
- Host opens it, or any AUv3 host (AUM, GarageBand, Cubasis, Logic Pro on iPad) loads it.
- Pros: works in every iOS DAW; MIDI, transport, and automation are host-driven.
- Cons: strictest sandbox; no `fork`/`exec`, no inter-process file access, limited network.
- Info.plist: `NSExtension` → `NSExtensionPointIdentifier = com.apple.AudioUnit-UI`, `AudioComponents` array with `type/subtype/manufacturer`.
- Principal class: `AudioUnitViewController` (Swift or Obj-C++) that wraps a `PulpAudioUnit`.

### 2. Standalone app (utility app that happens to make sound)

- Plain UIKit/SwiftUI app with an `AVAudioEngine` or `AVAudioSession` driving a Pulp `Processor`.
- No host — your UI is the product.
- Pros: own the whole experience; can persist files in the app container; Inter-App Audio is deprecated so no need to care about it.
- Cons: no MIDI routing from DAWs, no transport sync; users who want DAW integration will be unhappy.
- Good for: standalone synths, tuners, metronomes, sketch apps.

### 3. Audio Unit host

- Your app loads other vendors' AUv3 plugins (using `AVAudioUnitComponentManager`).
- Pulp's `core/host/` is the cross-platform layer; on iOS the adapter is `plugin_slot_au.mm`.
- Same sandbox rules as (2), plus `audio-unit-host` entitlement.

Most Pulp users will ship form (1) for commercial plugins and form (2) for demos.

---

## Project scaffolding

Recommended layout — mirrors `templates/ios-auv3/`:

```
examples/ios-auv3-synth/
├── HostApp/                  # required App Store container
│   ├── HostApp.entitlements
│   ├── Info.plist
│   ├── ContentView.swift
│   └── Assets.xcassets
├── AUv3Extension/            # the actual plugin
│   ├── Info.plist            # NSExtension + AudioComponents
│   ├── AUv3Extension.entitlements
│   └── AudioUnitViewController.mm
└── CMakeLists.txt            # uses pulp_add_ios_auv3()
```

`tools/cmake/PulpUtils.cmake` already has the `pulp_add_ios_auv3()` helper (see the `PULP_IOS` blocks). It reads `${PULP_IOS_AUV3_TEMPLATE_DIR}` (defaults to `templates/ios-auv3/`) for the `Info.plist.in`, entitlements, **and a minimal SwiftUI host (`HostApp/ContentView.swift` + `HostApp/PulpHostApp.swift`)** that plug-in authors can copy in to sanity-check the extension loads. The template host instantiates the AUv3 via `AVAudioUnit.instantiate`, lists parameters from the AU's `parameterTree`, and gives you a play/stop toggle — enough to verify the bundle + Info.plist round-trip without a full DAW.

---

## JS engine selection

Pulp supports QuickJS, JavaScriptCore (JSC), and V8 for scripted UIs. **On iOS, App Store rules constrain your choice.**

| Engine | iOS rule | Use when |
|---|---|---|
| **JavaScriptCore** | Always allowed — part of the OS | Default pick. Fast, no JIT concerns inside the app sandbox. |
| **QuickJS** | Allowed (interpreter, no JIT) | You want the same engine behaviour as desktop; small binary size matters. |
| **V8** | **Not allowed on iOS** — V8 requires JIT, which App Review rejects | Never. The `engine` skill's auto-selector already blacklists V8 on iOS. |

Rule of thumb: start with JSC for parity with the host OS; fall back to QuickJS if you hit a JSC quirk. Your `Processor` subclass doesn't change either way — only the `PULP_JS_ENGINE` build flag.

---

## Rendering

| Option | When to use | Gotchas |
|---|---|---|
| **CoreGraphics via `CoreGraphicsCanvas`** | Default. Low power, works on simulator and every device, perfect for knob-and-meter UIs. | CPU-bound; complex paths over ~30fps on older iPads drop frames. |
| **Metal via Dawn + Skia Graphite** | Visualisers, waveform/FFT displays, shader-heavy designs, SDF text. | AUv3 extensions get a **restricted Metal budget**; the host co-owns the GPU. Keep draw count low. |
| **WebView** | Reusing an existing HTML UI, design-tool imports from Figma/Stitch. | Heavier memory footprint; first-paint latency visible. |

For Metal, `core/view/platform/ios/plugin_view_host_ios.mm` provides `IOSGpuPluginViewHost` under `PULP_HAS_SKIA` — wrap your root View with it instead of the CoreGraphics `IOSPluginViewHost`.

---

## Sandbox constraints (what breaks compared to macOS)

- **No `fork` / `exec`** — `pulp::platform::exec` stubs to a no-op on AUv3; never count on spawning helpers.
- **No `posix_spawn_file_actions_addchdir_np`** — gated via `TargetConditionals` in `core/platform/platform/posix/child_process_posix.cpp`; working-directory changes for the child are silently dropped. Pass absolute paths.
- **No FSEvents** — `choc_FileWatcher` doesn't compile for iOS; hot-reload (`hot_reload.hpp`) is disabled on iOS. Ship bundled assets.
- **No arbitrary filesystem** — you own `NSHomeDirectory()` and not much else. For preset sharing, use `NSFileCoordinator` against the host app's group container or a files provider.
- **No background spawn** — `pulp::events::ChildProcessManager` will return `std::nullopt` on iOS. Workstream 03 slice 3.3b (`pulp-scan-worker`) is desktop-only.
- **Audio in background** requires `audio` in `UIBackgroundModes` *and* `AVAudioSession` set active. The `PluginDescriptor::ios_requires_background_audio` flag drives the host-app wiring.
- **Notarization / signing** — iOS apps use standard Apple Distribution / TestFlight; no notarize step like macOS. `pulp ship` delegates to `xcrun altool` / the new Transporter on iOS.

---

## Deployment target

- **Minimum: iOS 16.4** (simulator needs it for `std::to_chars` in `<format>`/`<charconv>`).
- Pulp's pre-built Skia binaries require ≥16.0; tests/widgets may need newer.
- Pass `-DCMAKE_OSX_DEPLOYMENT_TARGET=16.4` at configure time and `IPHONEOS_DEPLOYMENT_TARGET=16.4` to `xcodebuild`.

---

## Touch & Apple Pencil

`core/view/platform/ios/window_host_ios.mm` and `plugin_view_host_ios.mm` handle UIKit's `touchesBegan/Moved/Ended/Cancelled` → `View::on_mouse_{down,drag,up,cancel}()`. Notes:

- Each `UITouch*` gets a stable `pointer_id` so `View::set_pointer_capture()` works for multi-touch gestures.
- Apple Pencil sets `MouseEvent::pointer_type = PointerType::pen` plus altitude/azimuth — your widgets can branch on `me.isPen()`.
- `touchesCancelled` routes to `View::on_mouse_cancel()` (distinct from `on_mouse_up`) so gesture rollback doesn't fire accidental clicks.
- iPadOS trackpad hover comes through `UIHoverGestureRecognizer` → `View::simulate_hover()`.

---

## Accessibility

- Bridge lives in `core/view/platform/ios/accessibility_ios.mm`.
- Unlike macOS, every `UIAccessibilityElement` is opt-in: set `isAccessibilityElement = YES` on the container view. Pulp's bridge handles this.
- Test with VoiceOver on a real device; Simulator's Accessibility Inspector (`xcrun simctl ui booted appearance`) covers 80 %.

---

## Validation

```bash
# Simulator — configure + build
cmake -S . -B build-ios-sim -G Xcode \
  -DCMAKE_SYSTEM_NAME=iOS \
  -DCMAKE_OSX_SYSROOT=iphonesimulator \
  -DCMAKE_OSX_ARCHITECTURES=arm64 \
  -DCMAKE_OSX_DEPLOYMENT_TARGET=16.4

xcodebuild -project build-ios-sim/Pulp.xcodeproj \
  -target pulp-format -configuration Debug \
  -sdk iphonesimulator -arch arm64 build

# Device — swap in iphoneos SDK and supply team / signing identity
cmake -S . -B build-ios -G Xcode \
  -DCMAKE_SYSTEM_NAME=iOS -DCMAKE_OSX_SYSROOT=iphoneos \
  -DCMAKE_XCODE_ATTRIBUTE_DEVELOPMENT_TEAM=<TEAM_ID>

# Host-matrix smoke (device only):
#   AUM, GarageBand, Cubasis, Logic Pro (iPad)
#   auval -v aumu <subtype> <manufacturer>
```

The `ios` agent skill documents the XcodeBuildMCP preference: when those tools are available, use `list_sims` / `build_sim` / `install_app_sim` / `launch_app_sim` instead of raw `xcodebuild` for faster round-trips.

---

## What's on deck

| Slice | Status |
|---|---|
| 5.1 `examples/ios-auv3-synth/` | 🔴 open — template exists, example not yet scaffolded |
| 5.4 Metal on-device validation | 🔴 open — headless render test needed |
| 5.6 this document | ✅ first draft |

Track in `planning/production-readiness/05-auv3-mobile.md` and issue #218.

---

## See also

- [`.agents/skills/ios/SKILL.md`](../../.agents/skills/ios/SKILL.md) — automation-side gotchas and build recipes
- [`core/view/platform/ios/`](../../core/view/platform/ios/) — UIKit view hosts, accessibility bridge
- [`templates/ios-auv3/`](../../templates/ios-auv3/) — AUv3 Info.plist + entitlements templates
- Apple: [App Extension Programming Guide — Audio Unit](https://developer.apple.com/documentation/audiotoolbox/audio_unit_v3_plug-ins)
