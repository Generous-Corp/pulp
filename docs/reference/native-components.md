# Native-Language Components

Pulp lets you write audio components — DSP cores, modular nodes, and non-real-time
domain logic — in native languages other than C++, **Rust first**, behind a
stable, language-neutral **C ABI**. The feature is opt-in, desktop-first, and
designed so the boundary that ships today is the same boundary a public node ABI
freezes later.

This is the rationale-and-scope document. It states what Pulp supports, what it
deliberately does not, and why — in the same spirit as
[`layout-model.md`](layout-model.md). The companion engineering reference for the
node-interface generation and the deferred `pulp_node_v1` C ABI is
[`node-abi.md`](node-abi.md).

> **Status: experimental.** The contract stance is committed; the implementation
> lands in readiness-gated phases. Nothing here is a frozen binary ABI yet.

## What Pulp supports

- **Rust (and C / Zig / generated FAUST·Cmajor) DSP behind `Processor`.** You
  source-build a native DSP core and a thin C++ `pulp::format::Processor` adapter
  owns it through a private, C-shaped FFI. The native core sees POD structs —
  descriptor, parameters, audio-buffer views, a sorted parameter-event view, MIDI
  views, an opaque state span, a process context, and status codes — never C++
  types.
- **One language-neutral boundary, multiple bindings.** The stable contract is a
  **C ABI**, usable from Rust, C, Zig, generated DSP, and later WebAssembly
  loaders. Rust is the first ergonomic binding (`pulp-rust-sys` raw bindings plus
  a safe-ish trait layer), but Rust is never *the* boundary.
- **Host-owned buffers, borrowed for the call.** The host owns every process
  buffer; the native side only borrows planar views for the duration of one
  `process()` call. This single rule is what makes static linking, future dynamic
  loading, and real-time safety all work at once.
- **Opaque, versioned, validate-before-commit state.** State crosses the boundary
  as a Rust-owned (or C-owned) versioned byte span, validated before commit, never
  unwinding on malformed input — the same model `plugin_state_io` and SignalGraph
  plugin-node persistence already use.
- **Stable parameter identity.** Parameters are identified by stable string/hash
  IDs with plain-domain values, explicit ranges, ramp duration, and sample-offset
  semantics — decoupled from the C++ `ParamInfo` memory layout, and modeling
  modulation and automation as distinct concepts.
- **Native non-RT domain logic behind `EditorBridge`.** Preset/patch browsers,
  sample indexing, library/package management, analysis, and import/migration can
  be Rust-owned: a C++ `EditorBridge` handler calls Rust over FFI and returns JSON,
  always off the audio thread.
- **Source-built custom `SignalGraph` nodes today, a public node ABI later.**
  `CustomNodeType` hosts simple source-built native nodes now and is extended
  toward the deferred `pulp_node_v1` C ABI for precompiled third-party nodes
  (desktop and Android only).

## What Pulp does NOT support — by design

- **The current C++ `Processor` / `PluginSlot` / `CustomNodeType` virtual + STL
  surfaces are not a binary-stable ABI.** They use `std::string` / `std::vector` /
  `std::span` / exceptions. Native cores must not implement the C++ virtual class
  directly; the C++ adapter translates to POD FFI structs. See
  [`node-abi.md`](node-abi.md).
- **`WidgetBridge` is never the native-component boundary.** `createKnob` /
  `createFader` / `setValue` are UI construction on the UI thread, not a DSP or
  native-language component ABI. Rust plugins may *use* a UI built through it, but
  Rust does not sit "behind `createKnob`."
- **No new audio engine for native components.** No foreign-owned audio thread, no
  foreign-owned process buffers, no bypass of `StateStore` /
  `ParameterEventQueue` / graph PDC, no rewrite of format adapters or view hosting.
- **No Rust toolchain dependency for default builds.** The entire feature is behind
  an opt-in CMake flag (OFF by default). A default Pulp build needs no `cargo` /
  `rustc`.
- **No runtime-loaded or downloaded native DSP on iOS / AUv3 / sandboxed targets.**
  See below — this is a platform-policy ceiling, not a roadmap gap.
- **Pulp does not ship a bundled Rust DSP framework.** It ships the FFI skeleton,
  bindings, CMake/Cargo glue, 1–2 reference cores, and ABI/RT-safety tests. DSP API
  surface and crate-selection opinions are bring-your-own.

## Why

1. **Reuse the engine, don't rebuild it.** Everything is built on primitives Pulp
   already has — `Processor`, `StateStore`, `ParameterEventQueue`,
   `plugin_state_io`, `SignalGraph`, `node_abi.hpp` — extended outward, not
   replaced. A parallel audio scheduler or a foreign-owned audio thread would be
   double maintenance and a real-time-safety hazard.
2. **Real-time safety is the whole game.** Rust does not make DSP RT-safe — `Vec`
   growth, `String` formatting, locks, `unwrap()`-panic, and unwinding across
   `extern "C"` are all easy mistakes, and a panic crossing the C boundary is
   catastrophic. The contract (preallocate in `prepare()`, host-owned buffers, no
   alloc/lock/log/IO in `process()`, status codes plus zero-fill on failure,
   `panic = "abort"`) plus a real allocation-interception hook is what keeps the
   boundary safe.
3. **A C ABI is the only honest cross-language contract.** A Rust-only public ABI
   would foreclose C, Zig, FAUST/Cmajor codegen, and WebAssembly. C++ virtual
   tables and STL types are not a stable binary contract across compilers, standard
   libraries, or flags.
4. **Framework → platform.** Today people write Pulp plugins in C++/JS. A
   language-neutral component seam lets people write reusable DSP/domain components
   that *other* people compose. Most plugin code is not DSP (presets, sample
   indexing, browsers, metadata, import/export, analysis) — a clean component seam
   serves all of it.
5. **Get one narrow thing boringly correct first.** Rust-behind-`Processor`,
   desktop-first, source-built. The same primitives then extend to graph nodes. The
   readiness gates ensure no public ABI is frozen before the source-built shape has
   proven itself.

## What this means for Rust DSP

- **You bring the DSP; Pulp brings the contract.** "Write DSP however you want;
  here's the boundary." Pulp ships scaffolding (`pulp-rust-sys`, a safe-ish trait,
  CMake/Cargo glue, reference gain/biquad cores) and the ABI/RT-safety tests — not
  a DSP API surface or crate-selection opinions.
- **License discipline applies to crates too.** The opt-in lane runs a `cargo-deny`
  license audit against Pulp's allowlist (MIT / BSD-2,3 / Apache-2.0 / ISC / zlib /
  BSL-1.0 / public-domain). CI verifies each crate's actual LICENSE at its pinned
  version.
- **Preallocate in `prepare()`, never in `process()`.** Plan FFTs, size scratch,
  and build filters while suspended; the `process()` path must not allocate, lock,
  log, do IO, or panic. Build with `panic = "abort"` (or a contained `catch_unwind`
  at every FFI entry — never unwind across `extern "C"`).

## What this means for iOS & App Store builds

Stated honestly, and repeated wherever it matters: **on iOS, native components must
be compiled, signed, and statically bundled** into the app or AUv3 extension. The
AUv3 `.appex` is built `-fapplication-extension` and static-link only, and
`core/host` (the `dlopen`-based SignalGraph host) is compiled out entirely under
the App Store `dlopen` policy.

Pulp will never promise downloaded or `dlopen`-ed native DSP on iOS App Store
builds. Dynamic native node packs are a **desktop-and-Android** capability,
separately gated. This mirrors the existing honest phrasing in
[`../guides/ios-auv3-guidance.md`](../guides/ios-auv3-guidance.md).

## Honest tradeoff

A language-neutral native component seam is more build/CI surface (Cargo + CMake +
Xcode + Windows + Android lanes, a `rustup` toolchain, a `cargo-deny` audit) and a
harder debugging story (mixed Rust/C++/ObjC/DAW crashes). Pulp accepts that cost
because the upside — reusable, composable, real-time-safe DSP and domain components
in the language each author prefers — is what turns Pulp from a framework into a
platform.

The discipline that makes it safe: the contract (FFI shape, RT rules,
param/state/timing semantics) is written and tested **before** the first Rust
prototype, and adapter parity across formats is hardened **before** any "Rust
parity" claim. If you need a capability the contract does not yet cover, the answer
is to extend the contract additively (leading `size`/`version` fields, capability
flags) — never to widen a required field in a way that breaks older cores.
