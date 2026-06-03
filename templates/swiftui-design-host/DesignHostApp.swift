// Generic SwiftUI host scaffold for a Pulp design-imported view (Workstream B5
// of planning/2026-06-02-design-token-export-and-swiftui-path.md).
//
// `pulp import-design --mode baked --emit swiftui` writes three files:
//   - <RootView>.swift          — the generated `ImportedPulpView` (a `View`
//                                  generic over `PulpParameterResolving`)
//   - <RootView>Theme.swift     — the code-first token theme (light/dark)
//   - <RootView>.bindings.json   — the binding manifest (host pre-flight)
//
// This scaffold mounts that generated root view against a live
// `PulpParameterStore` so the imported design runs in the iOS Simulator or as a
// macOS app WITHOUT loading a DAW — the parallel of the AUv3 host at
// templates/ios-auv3/HostApp/ContentView.swift, but for the baked SwiftUI path.
//
// Usage:
//   1. Add this file + the three generated files + the `PulpSwift` package to
//      an app target (iOS 16+ / macOS 13+).
//   2. If you passed `--output` a custom root-view name, replace
//      `ImportedPulpView` below with your generated type name.
//   3. Back `PulpParameterStore` with your plugin's C++ StateStore via the
//      PulpBridge (`PulpBridgeRuntime.installBackend(...)`) so the generated
//      controls resolve real parameters; without a backend the store is empty
//      and bound controls render their "missing parameter" placeholder (by
//      design — never a silent mis-bind).

import SwiftUI
import PulpSwift

@main
struct PulpDesignHostApp: App {
    var body: some Scene {
        WindowGroup {
            DesignHostView()
        }
    }
}

struct DesignHostView: View {
    // Resolves the generated view's bound controls by exact `PulpParameter.name`
    // (the B1 resolution contract). In a real plugin host this store is backed
    // by the C++ StateStore through the PulpBridge; here it reloads whatever the
    // installed backend exposes.
    @StateObject private var store = PulpParameterStore()

    var body: some View {
        // Replace `ImportedPulpView` with your generated root view's type name
        // if you exported with a custom `--output`.
        ImportedPulpView(resolver: store)
            .frame(maxWidth: .infinity, maxHeight: .infinity)
            .onAppear { store.reload() }
    }
}
