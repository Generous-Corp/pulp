# Native-Language Components

Pulp lets you write audio components — DSP cores, modular nodes, and non-real-time
domain logic — in native languages other than C++, **Rust first**, behind a
stable, language-neutral **C ABI**. The feature is opt-in, desktop-first, and
designed so the boundary that ships today is the same boundary a public node ABI
freezes later.

This is the rationale-and-scope document. It states what Pulp supports, what it
deliberately does not, and why — in the same spirit as
[`layout-model.md`](layout-model.md). The companion engineering reference for the
node-interface generation and the public `pulp_node_v1` C ABI is
[`node-abi.md`](node-abi.md).

> **Status: experimental, implemented.** The full seam has landed across its
> phased rollout — the Processor-level FFI (`native_core.h`) and its
> `NativeCoreProcessor` adapter, the opt-in Rust staticlib lane, native non-RT
> domain logic via `editor_command`, stateful custom `SignalGraph` nodes, the
> public `pulp_node_v1` node ABI, and signed dynamic node packs. It remains
> **experimental** and is **not a frozen binary ABI** — source-rebuild against the
> SDK is still required, and contracts may still evolve additively before any
> freeze.

## What Pulp supports

- **Rust (and C / Zig / generated FAUST·Cmajor) DSP behind `Processor`.** You
  source-build a native DSP core and a thin C++ `pulp::format::Processor` adapter
  owns it through a private, C-shaped FFI. The native core sees POD structs —
  descriptor, parameters, audio-buffer views, a sorted parameter-event view, MIDI
  views, an opaque state span, a process context, and status codes — never C++
  types.
- **One language-neutral boundary, multiple bindings.** The stable contract is a
  **C ABI**, usable from Rust, C, Zig, generated DSP, and later WebAssembly
  loaders. Rust is the first exercised language in-tree through opt-in Cargo
  staticlibs with hand-mirrored `#[repr(C)]` reference cores, but Rust is never
  *the* boundary.
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
- **Source-built custom `SignalGraph` nodes and a public node ABI.**
  `CustomNodeType` hosts stateful source-built native nodes (opaque per-node
  instance, save/load state preserved across graph serialization), and the public
  `pulp_node_v1` C ABI ships for precompiled third-party nodes — distributed as
  signed node packs on desktop and Android (compiled out on iOS). See
  [`node-abi.md`](node-abi.md).

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
  CMake/Cargo glue, hand-mirrored Rust reference cores, and ABI/RT-safety tests.
  DSP API surface and crate-selection opinions are bring-your-own.

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
5. **Get one narrow thing boringly correct first.** The seam started with
   Rust-behind-`Processor`, desktop-first, source-built, then extended the same
   primitives outward to stateful graph nodes, the public `pulp_node_v1` ABI, and
   signed node packs. The phased rollout kept any public ABI from freezing before
   the source-built shape had proven itself — and nothing is frozen yet.

## The C ABI contract

The canonical, hand-written contract lives in
`core/native-components/include/pulp/native_components/native_core.h` (module
`pulp::native-components`). It is the *Processor-level* FFI — deliberately
independent of `SignalGraph` — and is shaped *like* a binary ABI so the future
public freeze is a relabel, not a rewrite:

- POD structs with a leading `size` + `abi_version`; opaque instance handles;
  status-code returns; no STL, exceptions, references, or unwind across the
  boundary.
- **Host-owned, borrowed planar `float32` audio** for the call; in-place
  legal; sidechain read-only; the core never retains or frees host buffers.
- A **sorted parameter-event view** mirroring Pulp's `ParameterEventQueue`:
  plain-domain values, sample offsets, linear ramps, fixed 1024 capacity with
  an overflow flag, and a *NULL vs present-but-empty* distinction.
- **Stable parameter identity**: a UTF-8 string id plus its FNV-1a/64 hash
  (one definition, in `native_core.hpp`, shared by host and binding generators).
- **Opaque, versioned state** spans, validate-before-commit, never unwinding on
  malformed bytes; empty span == defaults.
- **Explicit lifecycle** (suspended ⇄ active, plus reset); any sample-rate or
  block-size change is a fresh `prepare()`; per-instance opaque handles with no
  process-wide mutable globals; **modulation and automation as distinct**
  events; and **paired allocator ownership** (every core-owned pointer names its
  free function).

Additive evolution only: new fields/behaviour are negotiated by `size`/version
checks and capability flags, never by widening a required field. The contract
tests in `test/test_native_core_ffi.cpp` pin one case per decision so the shape
cannot silently drift.

## What this means for Rust DSP

- **You bring the DSP; Pulp brings the contract.** "Write DSP however you want;
  here's the boundary." Pulp ships the C header contract, CMake/Cargo glue,
  hand-mirrored Rust reference cores, and ABI/RT-safety tests — not a DSP API
  surface or crate-selection opinions.
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

## Dynamic node packs (desktop + Android)

Where the platform allows it, a precompiled `pulp_node_v1` node can ship as a
**signed node pack**: a dynamic library (`.dylib` / `.so` / `.dll`) exporting
`pulp_node_v1_entry`, plus a JSON manifest declaring the pack identity, ABI
major, the binary's SHA-256, declared node type-ids/capabilities, resource
declarations, runtime requirements, and an **Ed25519 signature** by a publisher
key. The host loader (`core/host/node_pack.hpp`) verifies trust and host policy
*before* it loads any code:

1. the signer key must be in the host's trust set (drop a key to revoke it);
2. the signature over `pack_id + abi_major + binary-hash + declared node
   type-ids/capabilities + resources + runtime requirements` must be authentic;
3. declared capabilities, realtime requirements, audio-thread allocation policy,
   block size, and memory ceilings must fit the host's `NodePackHostPolicy`;
4. required resource declarations must have stable IDs, kinds, and hashes;
5. the on-disk binary's SHA-256 must match the signed hash;
6. the entry's `abi_major` must match the host's `pulp_node_v1` major;
7. the loaded descriptor's stable ID and capability flags must match one of the
   signed node declarations.

Any failure rejects the pack and loads nothing — untrusted, tampered, or
ABI-mismatched packs never execute. This is the host-level integrity gate; OS
code-signing / notarization (Gatekeeper, Authenticode) is an additional,
separate distribution step. **iOS / AUv3 / sandboxed targets do not load node
packs at all** — `core/host` is compiled out there, and native components are
static-bundled and signed with the app.

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
