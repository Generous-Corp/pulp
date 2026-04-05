# v3 Phase Status Tracker

**Last updated:** 2026-04-02
**Tracking:** DSL / Agentic Validation / WebGPU / UI Framework v3

---

## Phase Status

| Phase | Name | Branch | Status | PR | Verified |
|-------|------|--------|--------|-----|----------|
| 1 | Contract/truth pass | phase/dsl-agentic-webgpu-v3-phase1 | **complete** | — | baseline landed |
| 2 | Validation harness + RTSan/vstvalidator | phase/dsl-agentic-webgpu-v3-phase2 | **complete** | [#59](https://github.com/danielraffel/pulp/pull/59) | pending Phase 14 |
| 3 | FAUST offline codegen | phase/dsl-agentic-webgpu-v3-phase3 | **complete** | [#61](https://github.com/danielraffel/pulp/pull/61) | pending Phase 14 |
| 4 | Cmajor adapter | phase/dsl-agentic-webgpu-v3-phase4 | parked (license policy blocker) | — | — |
| 5 | JSFX adapter | phase/dsl-agentic-webgpu-v3-phase5 | blocked (needs 4 / licensing decision) | — | — |
| 6 | Multi-window framework | phase/dsl-agentic-webgpu-v3-phase6 | **complete** | [#60](https://github.com/danielraffel/pulp/pull/60) | pending Phase 14 |
| 7 | WebView integration (iPlug2-style) | feature/v3-phase7-webview | **complete** (core merged; Windows/Linux opt-in backend follow-up active) | [#91](https://github.com/danielraffel/pulp/pull/91) | focused native proof on macOS; opt-in `PULP_BUILD_WEBVIEW=ON` configure/build/test plus `pulp-webview-palette` build now proven on macOS, Windows, and Ubuntu via [#92](https://github.com/danielraffel/pulp/issues/92) |
| 8 | Audio visualization (STFT/spectrogram) | phase/dsl-agentic-webgpu-v3-phase8 | **complete** | [#62](https://github.com/danielraffel/pulp/pull/62) | pending Phase 14 |
| 9 | Widgets + assets + themes | phase/dsl-agentic-webgpu-v3-phase9 | **complete** | [#72](https://github.com/danielraffel/pulp/pull/72) | pending Phase 14 |
| 10 | JS engine abstraction (V8/JSC) | phase/dsl-agentic-webgpu-v3-phase10 | **complete** (core abstraction merged; readiness floor now partially proven) | [#63](https://github.com/danielraffel/pulp/pull/63) | pending Phase 14 |
| 11 | WebGPU compute exploration | phase/dsl-agentic-webgpu-v3-phase11 | **complete** | [#64](https://github.com/danielraffel/pulp/pull/64) | pending Phase 14 |
| 12 | Offline video exploration | feature/v3-phase12-offline-video | **complete** (exploration only; library-first recommendation) | [#101](https://github.com/danielraffel/pulp/pull/101) | macOS PNG-sequence proof + explicit offline transport stepping |
| 13 | Three.js / WebGPU JS bridge | feature/v3-phase13-threejs-bridge | **complete** | [#103](https://github.com/danielraffel/pulp/pull/103) | readiness floor proven; bridge merged; exact-SHA smoke passed on macOS, Ubuntu, and Windows |
| 14 | Verification pass | — | blocked (needs [#92](https://github.com/danielraffel/pulp/issues/92), [#102](https://github.com/danielraffel/pulp/issues/102), plus explicit parked disposition for 4/5) | — | — |

**Status values:** not started, in progress, complete, blocked (needs X), failed, parked

**Verified column:** intermediate phases now record merge/proof status here; the final integrated verification still belongs to Phase 14.

---

## Completed Merge Wave

These phases are already on `main`:
- [x] Phase 2 — Validation harness
- [x] Phase 3 — FAUST
- [x] Phase 6 — Multi-window
- [x] Phase 8 — Audio visualization
- [x] Phase 9 — Widgets + assets + themes
- [x] Phase 10 — JS engine abstraction
- [x] Phase 11 — WebGPU compute

## Remaining Phase Queue

Current recommended landing order:
- [x] Status / planning truth refresh
- [x] Phase 13 readiness pass (see `planning/v3-phase13-readiness.md`; typed arrays + V8 proof + first host-object slice + first promise slice + first truthful bridge smoke slice landed)
- [x] Phase 12 — Offline video exploration
- [x] Phase 13 — Three.js / WebGPU JS bridge
- [~] [#92](https://github.com/danielraffel/pulp/issues/92) — Windows/Linux native WebView follow-up
- [ ] [#102](https://github.com/danielraffel/pulp/issues/102) — local CI stale-root/config-drift guardrails
- [ ] Phase 14 — Final verification pass

Parked pending license-policy resolution:
- [ ] Phase 4 — Cmajor adapter
- [ ] Phase 5 — JSFX adapter
- [ ] Track final disposition in [#89](https://github.com/danielraffel/pulp/issues/89)

## Active Execution Order

This is the queue to work against until the remaining v3 track is closed:
1. Land [#92](https://github.com/danielraffel/pulp/issues/92) so Phase 7 is no longer macOS-only in shipped form.
2. Land [#102](https://github.com/danielraffel/pulp/issues/102) so the remaining verification loop cannot silently queue the wrong SHA or dead host.
3. Run Phase 14 verification against the final integrated `main` state.
4. In Phase 14, explicitly account for:
   - parked licensing disposition for Phases 4/5 in [#89](https://github.com/danielraffel/pulp/issues/89)
   - the exact shipped WebView platform split after [#92](https://github.com/danielraffel/pulp/issues/92)
5. Only after Phase 14 closes should the parked Phase 4/5 follow-up or later parity/platform work be resumed.

## Phase 13 Readiness Gate

Before starting Phase 13 implementation work in earnest, prove the current merged prerequisites are actually sufficient:
- [x] verify `PULP_JS_ENGINE=quickjs|jsc|v8|auto` configure/build behavior on current `main`
- [x] prove `quickjs` and `jsc` build/test cleanly on current `main`
- [x] capture the real `v8` configure contract on current `main` (`V8_INCLUDE_DIR` + `V8_LIB_DIR` required)
- [x] prove `V8` really builds when enabled with external libs
- [x] assess whether the merged `JsEngine` layer already provides enough TypedArray support for the first readiness slice
- [x] raise and document the first host-object slice (native-backed object descriptors with properties + native methods)
- [x] raise and document the first promise slice (native callbacks surfaced as JS `Promise` objects)
- [x] write down the exact remaining deferred-async / bridge capability floor
- [x] define the first truthful Phase 13 test slice (engine selection, bridge smoke, then Three.js init)

Readiness note:
- Current evidence is recorded in `planning/v3-phase13-readiness.md`.
- Stock-checkout truth today is: `quickjs` and `jsc` are proven; `v8` is optional and dependency-conditional, but now has a real explicit provider path on this machine via Homebrew Node + `V8_LIBRARY_PATH`.
- Phase 13 should start as `v8`-preferred bridge work, while keeping the current engine-agnostic readiness smoke in regression coverage.

## Open MVP Blockers

- [ ] [#92](https://github.com/danielraffel/pulp/issues/92) — add native WebView support on Windows and Linux so the Phase 7 API is not macOS-only in shipped form

## Sequential Chain: DSLs

- [x] Phase 3 (FAUST) → merged to main
- [ ] Phase 4 (Cmajor) → currently parked on licensing
- [ ] Phase 5 (JSFX) → blocked behind Phase 4

## Sequential Chain: Multi-window → WebView

- [x] Phase 6 (multi-window) → merged to main
- [x] Phase 7 (WebView) → merged to main with macOS-native proof; Windows/Linux parity deferred to [#92](https://github.com/danielraffel/pulp/issues/92)

## Sequential Chain: Late phases

- [x] Phase 10 + 11 both merged → Phase 13 (Three.js bridge)
- [x] Phase 2 + 11 both merged → Phase 12 (offline video)
- [ ] All remaining shippable phases merged and 4/5 disposition explicitly tracked → Phase 14 (verification pass)

---

## Per-Phase Completion Checklist

When marking a phase **complete**, confirm ALL of the following:

- [ ] All deliverables listed in the phase prompt are implemented
- [ ] All tests pass (`ctest --test-dir build --output-on-failure`)
- [ ] Known test exclusions documented (`AudioWorkgroup`, `GpuSurface`, etc.)
- [ ] CLI commands added/updated (`tools/cli/pulp_cli.cpp`)
- [ ] Claude/Codex skills or agent workflows updated when behavior changes
- [ ] Docs updated (`docs/reference/cli.md`, capabilities docs, status manifests)
- [ ] No overclaims in docs — only claims what code backs
- [ ] PR created and appropriate CI/local proof is recorded
- [ ] Merged to `main`
- [ ] This status doc updated

---

## Phase Completion Log

Record when each phase lands with notes on what was delivered vs what was deferred.

### Phase 1 — Contract/truth pass
- **Merged:** 2026-03-31
- **Delivered:** machine-readable validation contract (`docs/contracts/phase1-reality-snapshot.yaml`), truth pass on docs/planning, status labels, dependency map
- **Deferred:** nothing
- **Notes:** all subsequent phases reference this as the authoritative baseline

### Phase 2 — Validation harness + RTSan/vstvalidator
- **Merged:** 2026-04-02 (`cf85cf8`)
- **Delivered:** validation harness improvements, `pulp validate --all` coverage, runtime sanitizer and validator plumbing, targeted proof path for later phases
- **Deferred:** final integrated verification remains Phase 14 work
- **Notes:** this is one of the dependency unlocks for Phase 12

### Phase 3 — FAUST offline codegen
- **Merged:** 2026-04-02 (`898d561`)
- **Delivered:** `core/dsl` contract, FAUST wrapper layer, `PulpFaust.cmake`, FAUST examples/tests
- **Deferred:** Cmajor and JSFX adapters remain later phases
- **Notes:** this is the substrate Phase 4 and Phase 5 build on

### Phase 4 — Cmajor adapter
- **Merged:** —
- **Delivered:** —
- **Deferred:** —
- **Notes:** parked for now because the official Cmajor docs describe a dual GPLv3/commercial license, which conflicts with this repo's no-copyleft/no-commercial-dependency policy; tracked explicitly in [#89](https://github.com/danielraffel/pulp/issues/89)

### Phase 5 — JSFX adapter
- **Merged:** —
- **Delivered:** —
- **Deferred:** —
- **Notes:** blocked on Phase 4 and the underlying DSL-licensing decision; future disposition also tracked in [#89](https://github.com/danielraffel/pulp/issues/89)

### Phase 6 — Multi-window framework
- **Merged:** 2026-04-02 (`130756a`)
- **Delivered:** WindowManager framework, shared state/theme hooks, multi-window plumbing and validation
- **Deferred:** WebView-specific embedding remains Phase 7
- **Notes:** this is the dependency unlock for Phase 7

### Phase 7 — WebView integration
- **Merged:** 2026-04-02 (`ac9a8e2`)
- **Delivered:** native WebView bridge on macOS, bundled resource serving, host embedding seam, palette example, Monaco example, focused WebView tests, WebView guide, and `webview-ui` skill
- **Deferred:** broader runtime parity/polish across Windows/WebView2 and Linux/WebKitGTK remains follow-up platform work
- **Notes:** [#92](https://github.com/danielraffel/pulp/issues/92) is now the active follow-up. Its focused proof extends `PULP_BUILD_WEBVIEW=ON` configure/build/test coverage and `pulp-webview-palette` example builds to Windows and Ubuntu without changing the public Phase 7 API.

### Phase 8 — Audio visualization
- **Merged:** 2026-04-02 (`de946d1`)
- **Delivered:** STFT/spectrogram/metering visualization foundation
- **Deferred:** later bridge/video work remains in Phases 12 and 13
- **Notes:** merged as a completed supporting subsystem

### Phase 9 — Widgets + assets + themes
- **Merged:** 2026-04-02 (`1c2a6ae`)
- **Delivered:** theme presets, asset manager, contrast helpers, higher-level widget/theme surface
- **Deferred:** none specific beyond later verification
- **Notes:** forms part of the UI baseline for the remaining phases

### Phase 10 — JS engine abstraction
- **Merged:** 2026-04-02 (`e2f7dda`)
- **Delivered:** JS engine abstraction with QuickJS/JSC/V8 backends and tests
- **Deferred:** production Three.js / Dawn bridge work and any later held-resolver async plumbing that real bridge work proves necessary
- **Notes:** this dependency is now backed by a proven readiness floor: typed-array proof, host-object descriptors, promise-returning native functions, and the first truthful browser-style bridge smoke slice

### Phase 11 — WebGPU compute exploration
- **Merged:** 2026-04-02 (`097d994`)
- **Delivered:** WebGPU compute feasibility slice and benchmark/proof work
- **Deferred:** production Three.js/WebGPU bridge remains Phase 13; offline video remains Phase 12
- **Notes:** this unlocks both Phase 12 and Phase 13

### Phase 12 — Offline video exploration
- **Merged:** 2026-04-02 (`8e9257e`)
- **Delivered:** feasibility report, explicit `HeadlessHost` transport-context stepping, and a macOS-first deterministic PNG-sequence proof
- **Deferred:** in-tree MP4/ProRes encoding, cross-platform capture parity, and GPU-faithful offscreen capture proof
- **Notes:** recommendation is to proceed only as a library-first / PNG-sequence capability if followed up; do not overclaim a shipped offline-video feature yet

### Phase 13 — Three.js / WebGPU JS bridge
- **Merged:** 2026-04-02 (`2434b40`)
- **Delivered:** browser-shaped slices for `HTMLCanvasElement` `getContext('2d')`, a mock `GPUCanvasContext` behind `getContext('webgpu')`, `window.pulp.gpu.getInfo()` backed by native truth, `navigator.gpu.requestAdapter()` Promise seams returning mock adapter objects, a first mock `GPUAdapter`/`GPUDevice`/`GPUQueue`/resource object graph, core WebGPU globals (`GPUBufferUsage`, `GPUTextureUsage`, `GPUMapMode`, `GPUShaderStage`, `GPUColorWrite`), browser utility shims (`performance`, storage, `Image`, `atob`/`btoa`, `crypto.getRandomValues`, `TextEncoder`, `TextDecoder`, `structuredClone`), and an asset-backed browser helper slice for `fetch`, `Response`, `Blob`, and `URL.createObjectURL`
- **Deferred:** real Dawn-backed `GPUAdapter` / `GPUDevice` object graph, actual WebGPU canvas presentation, WGSL execution, Three.js compatibility claims, and DOM/native id parity
- **Notes:** prerequisites and readiness floor were proven before merge. Final focused smoke passed on the exact Phase 13 SHA across macOS, Ubuntu, and Windows before landing [#103](https://github.com/danielraffel/pulp/pull/103).

### Phase 14 — Verification pass
- **Merged:** —
- **Delivered:** —
- **Deferred:** —
- **Notes:** final end-to-end verification remains after [#92](https://github.com/danielraffel/pulp/issues/92) and [#102](https://github.com/danielraffel/pulp/issues/102), plus explicit disposition tracking for the parked Cmajor/JSFX phases ([#89](https://github.com/danielraffel/pulp/issues/89))
