# GPU Rendering Validation Checklist

Status of GPU rendering verification across platforms and configurations.

## Start With the Bounded Health Probe

Run the installed diagnostic before inspecting build files or opening a live
window:

```bash
pulp doctor gpu --json
```

The command performs a known offscreen draw plus pixel readback and a known
compute submission plus mapped-result verification. Use
`pulp doctor gpu --no-render --json` only for inventory/preflight when device
acquisition itself must be avoided; it performs no active render or compute
work and returns unverified. Both commands work outside a source checkout.
Agents can request the same typed evidence with the
`pulp_gpu_doctor` MCP tool.

Interpret the exit status with the JSON evidence:

- `0` means all required real-work proofs passed and at least one required
  probe reported authentic adapter identity.
- `1` means the work completed but a measured assertion failed.
- `2` means a requested stage was unavailable or could not be verified.

Do not turn exit 2 into a skipped pass. Likewise, do not infer a discrete or
hardware adapter from a Metal, D3D12, Vulkan, or other backend label. Trust only
the adapter identity returned by Dawn; a null/software adapter is useful for
negative coverage but is never hardware proof. Identities are per probe and do
not claim Renderer3D, HeadlessSurface, and GpuCompute acquired the same device.

If the diagnostic reports unavailable or unverified, follow the remediation in
the result, then use the [Skia GPU build skill](../../.agents/skills/skia-gpu-build/SKILL.md)
to inspect bundle discovery and ABI details. If it reports a completed failure,
preserve the JSON result and reproduce with the focused render/compute tests
before moving to a live application.

## DPR experiment evidence (A4 foundation)

The committed A4 corpus defines a reproducible matrix; it does not change
Pulp's scale policy. Validate and expand it before scheduling hardware work:

```bash
python3 tools/scripts/gpu_dpr_experiment.py validate-manifest
python3 tools/scripts/gpu_dpr_experiment.py emit-plan \
  --experiment-id "a4-YYYYMMDD-NNN" \
  --plan-revision "$PULP_DPR_PLAN_REVISION" \
  --pulp-sha "$(git rev-parse HEAD)" \
  --forge-sha "$PULP_DPR_FORGE_SHA" > /tmp/pulp-dpr-plan.json
```

Export `PULP_DPR_PLAN_REVISION` and `PULP_DPR_FORGE_SHA` as the exact 40-hex
commits under test first; placeholder or branch names are rejected.

The plan covers DPR 1, 1.5, 2, and 3 in exact, configured-max, and explicitly
nonshipping adaptive modes. A complete measured result must cover every matrix
cell, bind the Pulp and Forge revisions and artifacts, preserve identical
logical content and input coordinates, and include the required frame, memory,
fidelity, and interaction observations. Dense text must have a legibility
oracle; a content-floor pass alone is insufficient.

A2T named Perfetto-question coverage and an A3 budget receipt are hard
prerequisites for a complete result. Missing trace categories, absent Forge or
web canaries, changed logical input, an unavailable small-text oracle, SKIP, or
INCONCLUSIVE leaves the experiment incomplete. Planned or synthetic evidence
may test automation but cannot justify `configured-max-candidate` or
`adaptive-candidate`. Those dispositions identify later Vellum policy work;
they do not authorize a Pulp render-policy change.

### Execute and resume the matrix

Initialize a durable run journal, then supply only the scenario adapters that
exist on the machine. Each adapter is an absolute executable path and receives
`--request <json> --receipt <json>`; it must preserve its raw samples, capture,
Perfetto trace, input receipt, machine identity, and graphics-adapter identity
inside the requested cell directory.

```bash
python3 tools/scripts/gpu_dpr_runner.py init \
  --plan /tmp/pulp-dpr-plan.json --run-dir /tmp/pulp-dpr-run
python3 tools/scripts/gpu_dpr_runner.py run \
  --run-dir /tmp/pulp-dpr-run \
  --adapter dense-text-thin-strokes=/absolute/path/to/pulp-capture-adapter \
  --limit 1
python3 tools/scripts/gpu_dpr_runner.py status \
  --run-dir /tmp/pulp-dpr-run --json
```

Re-running `run` resumes incomplete cells. Missing adapters, adapter timeouts,
SKIP, INCONCLUSIVE, rejected evidence, or failed exit/receipt agreement are
durable incomplete attempts, never measurements. Existing Pulp screenshot,
Three.js, Forge-native, real-DAW, browser, logical-input, and A2T tools may be
wrapped as scenario adapters, but no generic adapter is implied: absent product
legs remain explicit dependencies.

The checked-in Pulp-native adapter is a safe first rung for the three frozen
native fixtures. Point it at an exact-SHA `pulp-screenshot` build:

```bash
PULP_DPR_SCREENSHOT_BIN=/absolute/exact-build/tools/screenshot/pulp-screenshot \
python3 tools/scripts/gpu_dpr_runner.py run \
  --run-dir /tmp/pulp-dpr-run \
  --adapter dense-text-thin-strokes="$PWD/tools/scripts/gpu_dpr_pulp_native_adapter.py" \
  --adapter shader-heavy-controls="$PWD/tools/scripts/gpu_dpr_pulp_native_adapter.py" \
  --adapter meters-waveforms="$PWD/tools/scripts/gpu_dpr_pulp_native_adapter.py"
```

It performs a real Skia/GPU capture, verifies frozen source bytes and physical
dimensions, and preserves binary/machine identity plus a capture digest in each
cell. It deliberately returns `INCONCLUSIVE`: a subprocess PNG cannot truthfully
provide same-process adapter identity, correlated A2T spans, A3 budget evidence,
GPU/frame/memory/upload measurements, logical-input delivery, or independent
text/stroke/reference oracles. The durable dependency list is the handoff to a
product-specific measured adapter; subprocess wall time is recorded only as
preflight context and is never relabeled as frame time.

`ingest` accepts an independently produced cell receipt and applies the same
fidelity, logical-input, artifact-hash, identity, trace-category, and raw-sample
checks. Every closed trace question must return exactly the cell's issued
32-hex attempt nonce as its sole GPU evidence ID; IDs from another cell or a
capture containing ambiguous cohorts fail closed rather than being unioned.
`finalize` requires all 84 cells plus exact A2T and A3 receipts. A
`no-change` disposition cancels B5; either candidate leaves B5
`waiting-trigger` on `B0-adopted-vellum-api-refresh`. Neither result authorizes
a Pulp policy change.

## Verified (Real Hardware)

| Platform | Backend | Surface | Rendering | Status |
|----------|---------|---------|-----------|--------|
| macOS (Apple Silicon) | Metal | CAMetalLayer (NSView) | Skia Graphite | **Verified** — GPU demo runs at 60fps |
| macOS (Apple Silicon) | Metal | CAMetalLayer (PluginViewHost) | Skia Graphite | **Verified** — DAW-embedded rendering |

## Implemented (Awaiting Hardware Validation)

| Platform | Backend | Surface | Rendering | Status |
|----------|---------|---------|-----------|--------|
| iOS | Metal | CAMetalLayer (UIView) | Skia Graphite | **Implemented** — IOSGpuWindowHost + IOSGpuPluginViewHost |
| Windows | D3D12 | HWND (SDL3) | Skia Graphite | **Implemented** — SDL3 HWND extraction + Dawn D3D12 |
| Linux/X11 | Vulkan | X11 Window (SDL3) | Skia Graphite | **Implemented** — SDL3 X11 extraction + Dawn Vulkan |
| Android | Vulkan | ANativeWindow (SurfaceView) | Skia Graphite | **Implemented** — ANativeWindow extraction + Dawn Vulkan |

## Architecture

```
Platform Window (NSView/UIView/HWND/SurfaceView/SDL3)
    │
    ▼
Native Surface Handle (CAMetalLayer*/HWND/ANativeWindow/X11 Window)
    │
    ▼
GpuSurface (Dawn/WebGPU)
    ├── Metal backend (macOS/iOS)
    ├── D3D12 backend (Windows)
    └── Vulkan backend (Linux)
    │
    ▼
SkiaSurface (Skia Graphite)
    │
    ▼
SkCanvas → View tree painting
```

## Render Loop

| Platform | Mechanism | Target FPS |
|----------|-----------|------------|
| macOS | CVDisplayLink → main queue dispatch | Display refresh (60-120Hz) |
| iOS | CADisplayLink | Display refresh (60-120Hz) |
| Android | AChoreographer | Display refresh |
| Windows | DwmFlush, with timer fallback if DWM is unavailable | Display refresh or 60Hz fallback |
| Linux | Timer fallback until native present-sync is wired | 60Hz |
| WASM | requestAnimationFrame | Display refresh |

## Known Limitations

- **Windows GPU**: `DwmFlush` gives compositor-paced frames when DWM is available; headless or remote sessions degrade to the 60Hz timer fallback
- **Linux GPU**: X11 surface creation is wired, but frame pacing is still the 60Hz timer fallback
- **iOS GPU**: CADisplayLink frame pacing exists, but device runtime validation is still pending
- **Linux Wayland**: SDL3 can extract Wayland handles, but `GpuSurface` presentation consumes X11 handles only today
- **WASM**: WebGPU support depends on browser (Chrome 113+, Firefox 120+)

## Test Coverage

Start a clean-agent investigation with catalog discovery, not a remembered
recipe ID:

```bash
pulp gpu recipes list --json
pulp gpu recipes list --symptom compute-readback-mismatch --json
mkdir -p "$PWD/artifacts/gpu"
pulp gpu recipes scaffold gpu-compute.magnitude.v1 \
  --output "$PWD/artifacts/gpu/magnitude-workspace"
```

The scaffold destination itself must not exist; its parent must already exist.

Run the scaffolded baseline twice, then its negative control. Baseline exit 0
means verified pass; the deliberate mutation must produce exit 1; exit 2 means
the requested evidence was unavailable or unverified and must not be counted as
a pass. Correlate the emitted `gpu_evidence_id` with the `gpu-probe` Perfetto
question when the wrong value is downstream of scheduling or frame work.

The A5 clean-agent gate exercises that flow rather than merely checking a
catalog lookup. Starting only from `compute-readback-mismatch`, it selects the
live CLI's unique callable recipe, scaffolds a workspace, executes the real
seeded failure, diagnoses its completed typed pass, removes only the documented
`--negative-control` seed, and proves the repaired rerun with stable
input/oracle hashes. See
[`docs/validation/gpu-clean-agent/README.md`](../validation/gpu-clean-agent/README.md)
for the standalone invocation and receipt contract.

For a live product, query the exact instance separately with
`dev.pulp.gpu/health.read@1` under `inspect-readonly`. That cheap snapshot is a
first-frame/control-plane signal, not a replacement for an offline oracle or a
Perfetto capture. The catalog's `callable` field is compile/runtime capability;
it does not claim that a host published the live operation.

Treat trace localization, platform-race proof, and product acceptance as three
separate gates. For example, a trace can show paint averaging about 1 ms while
resize spans remain near one 60 Hz frame (roughly 15.7 ms median and 18.7 ms at
p99), narrowing the investigation to acquire, present, or compositor ordering.
It cannot by itself prove the root cause. Use an AppKit/GPU event-order harness
with a planted-old-behavior negative control to prove a redundant same-size
resize callback or retained-cover lifetime race, then close the loop with a
60 fps recording and human interaction/feel validation. Trace evidence narrows
the stage; the deterministic harness proves the platform race; product proof
shows the fix solved the user-visible problem.

- 13 cross-platform render tests (GpuSurface + SkiaSurface)
- GPU demo validates continuous animation, vector drawing, resize
- Headless tests verify surface creation and texture lifecycle
- `pulp doctor gpu` verifies bounded render/readback and compute/map work using
  the same public rendering primitives an installed consumer uses
- `pulp gpu probe --recipe <id> --artifacts <dir>` localizes wrong values and
  pixels with deterministic inputs, independent CPU/content oracles, authentic
  adapter identity, and bounded hash-declared artifacts. Run the matching
  `--negative-control` path to prove the oracle detects a real seeded mutation;
  its expected exit status is 1 with typed failure evidence.
- In builds that advertise it, `threejs.multi-pass.v1` uses the hash-verified installed/source
  `three.webgpu.js` runtime through V8 and Dawn. Its three bounded RGBA
  readbacks show where background, intermediate, and final content first
  diverge; the C++ color-region oracle does not consume Three.js's own expected
  values. Run the baseline twice before the seeded mutation when diagnosing
  nondeterminism.
