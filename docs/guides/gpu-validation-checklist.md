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

## Authenticate a published hardware run

A checked-in `pulp doctor gpu --json` file proves measured GPU behavior, but
its JSON alone does not authenticate the machine that produced it. For a run
that will support a protected hardware-coverage claim, publish the result and
the canonical
[`pulp.gpu-health-run-attestation.v1`](../contracts/gpu-health-run-attestation-v1.schema.json)
schema in one commit, then produce the signed sibling attestation and publish
that file in a later commit. The separation is required because a file cannot
contain the SHA of the commit that contains that same file without a circular
hash.

The producer reads the health result and schema from the exact evidence commit,
selects one required passing probe with authentic hardware name/backend/device,
hashes the exact producer binary, and signs the canonical statement with a
pre-provisioned Ed25519 SSH host key. It never creates a key or changes host
configuration:

```bash
python3 tools/scripts/gpu_health_run_attestation.py \
  --repository "$PWD" \
  --health-result docs/validation/gpu-health/a1/m5/pulp-doctor-gpu.json \
  --output docs/validation/gpu-health/a1/m5/run-attestation.json \
  --signing-key /path/to/pre-provisioned/m5-ed25519 \
  --host-id m5 --stable-machine-id '<platform UUID from host inventory>' \
  --configuration 'power=low;fallback=false' \
  --probe-id gpu-compute-magnitude \
  --implementation-revision '<40-character implementation SHA>' \
  --evidence-publication-revision '<40-character result/schema commit SHA>' \
  --producer-binary /absolute/path/to/pulp-cpp \
  --producer-build-id '<exact build ID>' \
  --producer-code-signature '<exact code-signature identity or digest>'
```

Verification is independent of the producer. Its local trusted-host registry
maps one `host_id` to the expected stable machine ID and SSH public key; the
registry contains no private key. The verifier requires the implementation,
evidence, and containing attestation revisions to form the expected ancestry,
requires the latter two to be ancestors of the named protected ref, re-reads
the schema and health result from Git, re-hashes the live producer binary,
checks the caller's exact selection policy, verifies the host signature, and
applies an explicit freshness ceiling:

```bash
python3 tools/scripts/verify_gpu_health_run_attestation.py \
  --repository "$PWD" \
  --attestation-revision '<40-character commit containing run-attestation.json>' \
  --attestation-path docs/validation/gpu-health/a1/m5/run-attestation.json \
  --protected-ref origin/main \
  --trusted-hosts /path/to/local/trusted-gpu-hosts.json \
  --producer-binary /absolute/path/to/pulp-cpp \
  --expected-producer-build-id '<exact build ID>' \
  --expected-producer-code-signature '<exact code-signature identity or digest>' \
  --expected-host-id m5 --expected-stable-machine-id '<platform UUID>' \
  --expected-configuration 'power=low;fallback=false' \
  --expected-adapter-name '<exact Dawn adapter name>' \
  --expected-backend Metal --expected-device '<exact Dawn device identity>' \
  --max-age-seconds 1800
```

The trusted-host registry is a closed JSON object with
`schema: pulp.gpu-health-trusted-hosts.v1`, `version: 1`, and a `hosts` array.
Each host has exactly `host_id`, `stable_machine_id`, and `public_key` (the
single-line `ssh-ed25519 ...` public key). A missing trust entry, stale run,
unprotected commit, changed binary/result/schema, or cross-host/configuration/
adapter reuse is a verification failure, never unavailable-as-pass.

## DPR experiment evidence (A4 v2)

The committed A4 corpus defines an evidence-only experiment; it does not change
Pulp's scale policy. The canonical result is deliberately `inconclusive` with
zero v2 cells until the protected A3 product policy supplies every scenario's
frame budget, timer-noise bound, memory-sampler resolution, and required M5
coverage. Validate that truthful boundary before scheduling hardware work:

```bash
python3 tools/scripts/gpu_dpr_experiment.py validate-manifest
python3 tools/scripts/gpu_dpr_experiment.py validate-result \
  docs/validation/gpu-dpr/terminal-result.json
```

`emit-plan` is a structural nonterminal fixture surface, not collection
authority. The terminal runner derives revisions, policy, and manifest from the
fixed receipts described below; a caller-generated plan cannot replace them.

The v2 plan covers exactly 84 original and 84 same-machine repeat cells: seven
scenarios, DPR 1/1.5/2/3, and exact/configured-max/nonshipping-adaptive modes.
Every cell has five reset warm-ups, 30 aligned measured triplets of at least 240
frames, and 20 reset fresh-process first-nonblank trials. Mode order and all
bootstrap seeds use canonical UTF-8 length-prefixed fields and SHA-256
counter-mode; candidate intervals use exactly 10,000 aligned-trial percentile
resamples. The Forge DAW aggregate additionally requires AUv2 in Logic plus VST3
and CLAP in REAPER for every cell. Metrics remain independently retained; the
three subreceipts must agree on terminal verdict, gates, and bound identity.

A2T Perfetto coverage and the protected A3 policy/campaign are hard
prerequisites. Missing categories, a missing/changed pair or repeat, substituted
host/provider/format, stale dimensions, unavailable metric, exact-baseline or
fidelity failure, SKIP, or INCONCLUSIVE leaves v2 inconclusive. Historical v1
receipts are retained only as `historical-v1-nonterminal`; they count as zero v2
cells and cannot select B5.

### Execute and resume the matrix after A3 authority

Do not initialize collection while `v2_protocol.status` is
`blocked-product-policy`. Do not ask a caller to update or supply the manifest.
After the fixed A2T, A3 DPR product-policy, and A3 runtime terminal receipts are
ordinary blobs at live protected Pulp `main`, `init-v2` validates those exact
bytes and derives the authorized manifest itself. The run directory must be an
absolute absent path created by the runner. Each v2 adapter is an absolute
executable path and receives `--request <json> --output <directory>`; its
producer receipt must name eight real artifacts that the runner can snapshot
and revalidate.

```bash
python3 tools/scripts/gpu_dpr_runner.py init-v2 \
  --run-dir /absolute/owned/pulp-dpr-run \
  --experiment-id <campaign-id> \
  --trace-analyzer /absolute/path/to/the/a2t-authorized-analyzer
python3 tools/scripts/gpu_dpr_runner.py run-v2 \
  --run-dir /absolute/owned/pulp-dpr-run \
  --cell 'original__dense-text-thin-strokes__exact__dpr-1' \
  --adapter /absolute/path/to/a/v2-measurement-adapter
python3 tools/scripts/gpu_dpr_runner.py status-v2 \
  --run-dir /absolute/owned/pulp-dpr-run
```

The existing `init`/`run`/`issue`/`ingest` protocol and checked-in native/web
adapters remain historical v1 collection machinery. They are nonterminal until
a real product-specific v2 adapter emits the closed producer receipt and eight
artifact kinds; do not pass a v1 adapter to `run-v2` or wrap missing product
legs with invented JSON. A v2 timeout, per-stream output overflow, bad exit,
missing receipt, or rejected bytes closes that nonce as inconclusive and allows
a fresh attempt. It never completes the cell.

The checked-in Pulp-native v1 adapter supports both a capture-only preflight and
a v1 measurement producer for the three frozen native fixtures. Build the
producer with benchmark counters and tracing enabled, then point the adapter at
that exact executable:

The native DPR measurement producer and its process/IPC tests are currently
POSIX-desktop-only (macOS and Linux). Windows and Emscripten builds retain GPU
recipe discovery and the portable probe model, but do not configure the native
process/session producer until it has an equivalent platform implementation.

```bash
cmake -S . -B build-dpr -DCMAKE_BUILD_TYPE=Release \
  -DPULP_BENCHMARK=ON -DPULP_TRACING=ON
tools/ci/governed-build.sh cmake --build build-dpr --config Release \
  --target pulp-gpu-dpr-native-measurement
PULP_DPR_NATIVE_MEASUREMENT_BIN="$PWD/build-dpr/tools/cli/gpu_probe/pulp-gpu-dpr-native-measurement" \
python3 tools/scripts/gpu_dpr_runner.py run \
  --run-dir /tmp/pulp-dpr-run \
  --adapter dense-text-thin-strokes="$PWD/tools/scripts/gpu_dpr_pulp_native_adapter.py" \
  --adapter shader-heavy-controls="$PWD/tools/scripts/gpu_dpr_pulp_native_adapter.py" \
  --adapter meters-waveforms="$PWD/tools/scripts/gpu_dpr_pulp_native_adapter.py"
```

The producer uses one public editor-surface/WidgetBridge tree for capture,
logical input, 30 steady frame/counter samples, authentic Dawn identity, and
the nonce-correlated Perfetto trace. First-frame timing comes from 20 sequential
fresh child processes. Each child ledger row binds the attempt number/nonce,
unique PID, producer and content digests, Pulp build identity, exact adapter,
and its sample; the producer, adapter, and runner all verify that provenance.
Without `PULP_DPR_NATIVE_MEASUREMENT_BIN`, the adapter retains its safe
capture-only preflight and deliberately returns `INCONCLUSIVE`.

Each raw metric carries an explicit `measured`, `derived`, or `unavailable`
provenance plus its definition. `unavailable` has no samples or invented
percentiles and cannot make a cell policy-eligible. GPU timing also carries an
empirical resolution estimate and five baseline/five known-extra-work trials;
the extra-work control must be independently distinguishable before any timer
sample is accepted. The input oracle is frozen in the scenario manifest. The
producer reports the actual physical pointer event, logical point recovered
from that event, and hit target rather than supplying both sides of its own
expectation.

Fidelity compares two independently hashed PNGs of the same content/state and
reports numeric pixel similarity, small-text luminance variation, and
thin-stroke coverage. Text/stroke statistics are computed only inside the
scenario's frozen logical ROIs; whole-frame content variance cannot satisfy
those feature oracles. Adaptive trials retain the measured over/under-budget
samples and every scale transition; copied mode metadata is not adaptive
evidence. The browser producer requires exactly `playwright-core@1.61.1` and
refuses another installed version. Old receipts invalidated by these instrument
rules remain preserved, but
`docs/validation/gpu-dpr/instrument-validity-state.json` marks them
`SUPERSEDED`/`NONCOUNTED`; never delete or silently promote them. Install the
browser dependency from its exact lock with `npm ci --ignore-scripts` in
`examples/web-demos/super-convolver-ui/browser-test`; do not use an unpinned
global Playwright package.

Historical v1 `ingest` accepts an independently produced cell receipt and
applies the v1 fidelity, logical-input, artifact-hash, identity, trace-category,
and raw-sample checks. Every closed trace question must return exactly the cell's issued
32-hex attempt nonce as its sole GPU evidence ID; IDs from another cell or a
capture containing ambiguous cohorts fail closed rather than being unioned.
For v2, `finalize-v2 --run-dir /absolute/owned/pulp-dpr-run` accepts no
manifest, draft, result, or disposition argument. It freshly revalidates the
terminal dependencies and rederives all 168 nonce-bound runner receipts and
1,344 artifact files before it recomputes
all candidate-vs-exact and adaptive-vs-configured intervals, regression gates,
same-unit repeat tolerances, class support, and the simplest-policy tie-break.
A candidate needs repeated material affected DPR-3 evidence in Pulp-native,
Forge-native/DAW, and web classes. `no-change` cancels B5; either candidate
leaves B5 `waiting-trigger` on `B0-adopted-vellum-api-refresh`. Every B5 receipt
keeps `authorizes_policy_change=false`.

A complete measurement JSON is still only a publication candidate. Publish the
runner-derived bytes only at the fixed manifest/result repository paths. From a
clean checkout at the exact fresh live protected Pulp `main` head, run
`verify-live-v2 --evidence-root /absolute/owned/pulp-dpr-run`; it emits a durable
receipt binding those ordinary Git blobs and bytes. `validate-result` requires
and freshly recomputes that receipt, unions
classic branch-protection and repository-ruleset required checks (including app
IDs), exhausts bounded pagination, and requires the unique latest matching
result to be successful. An open PR, wrong-app same-name check, truncated API
response, dirty/symlinked/outside/substituted candidate, wrong blob/type/head,
local commit, or caller-written `true` remains nonterminal. The complete
operator contract is in `docs/validation/gpu-dpr/README.md`.

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

The A5 clean-agent harness exercises that flow rather than merely checking a
catalog lookup. Starting only from `compute-readback-mismatch`, it selects the
live CLI's unique callable recipe, scaffolds a workspace, executes the real
seeded failure, diagnoses its completed typed pass, removes only the documented
`--negative-control` seed, and proves the repaired rerun with stable
input/oracle hashes. See
[`docs/validation/gpu-clean-agent/README.md`](../validation/gpu-clean-agent/README.md)
for the standalone invocation and structural v4 evidence contract. Its
verification receipt is nonterminal; protected planning acceptance remains a
separate human-reviewed artifact after the exact Pulp head lands.

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
