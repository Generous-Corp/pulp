# SwiftUI design-host scaffold

Mounts a baked SwiftUI design import (`pulp import-design --mode baked --emit
swiftui`) in a standalone app so you can run the imported design in the iOS
Simulator or as a macOS app without a DAW.

## What the import emits

```
pulp import-design --from figma --file design.json \
  --mode baked --emit swiftui --output ImportedPulpView.swift
```

produces:

| File | Role |
|------|------|
| `ImportedPulpView.swift` | the generated root `View` (generic over `PulpParameterResolving`) |
| `ImportedPulpViewTheme.swift` | code-first token theme (light/dark dynamic colors) |
| `ImportedPulpView.bindings.json` | binding manifest — bound controls + the full `pulp*` contract |

## Wiring the host

1. Create an app target (iOS 16+ / macOS 13+).
2. Add `DesignHostApp.swift` (this directory), the three generated files, and
   the `PulpSwift` Swift package (`apple/`).
3. Back the `PulpParameterStore` with your plugin's C++ `StateStore` via the
   PulpBridge so the generated controls resolve real parameters. With no backend
   installed the store is empty and each bound control renders its visible
   "missing parameter" placeholder — never a silent mis-bind.
4. Bundled image assets referenced by the design (`Image("<asset_id>")`) must be
   added to the app's asset catalog under their asset id; remote images load via
   `AsyncImage`. Generating an `Assets.xcassets` automatically is deferred.

## Validation

The generated Swift is type-checked in CI by the `swiftc` gate in
`test/test_design_swift_codegen.cpp`. A full Simulator build + screenshot is the
manual visual-fidelity step (XcodeBuildMCP): build the host target for a
Simulator destination, launch, and compare against the source design.
