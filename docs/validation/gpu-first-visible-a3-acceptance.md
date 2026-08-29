# GPU first-visible acceptance receipts

`gpu-first-visible-a3-acceptance.json` is the closed evidence receipt for the
GPU first-visible acceptance gate. Its schema deliberately keeps incomplete
work representable without allowing it to become a pass.

## Verify a receipt

```bash
python3 tools/scripts/gpu_first_visible_a3_acceptance.py verify \
  docs/validation/gpu-first-visible-a3-acceptance.json \
  --evidence-root docs/validation
```

The verifier returns `0` only for a terminal pass, `2` for a valid nonterminal
receipt, and `1` for malformed, inconsistent, missing, or digest-mismatched
evidence. A nonterminal result is never an acceptance pass.

## Generate a receipt

Author a template matching
`docs/contracts/gpu-first-visible-a3-acceptance-v1.schema.json`, using
`"sha256": "auto"` for artifact references and the paired
`"implementation_head": "auto"` / `"source_blobs": "auto"` markers. Then run:

```bash
python3 tools/scripts/gpu_first_visible_a3_acceptance.py generate template.json \
  --output acceptance.json --evidence-root /path/to/evidence
```

Artifact paths are relative to the evidence root and may not contain `..`.
Generation hashes the referenced files, applies the full schema and semantic
checks, binds the exact A3 implementation head plus its non-receipt source
blobs, and writes atomically only after validation. The implementation head
must equal `identity.pulp_revision`, remain an ancestor of the current checkout,
and have the same relevant blobs at historical HEAD, current HEAD, and the
working tree. The receipt is deliberately outside that set, avoiding a circular
self-hash.

## Run real product campaign roles

`gpu_first_visible_a3_campaign.py` is the executable boundary between the
closed verifier and product-specific lifecycle automation. It snapshots the
exact adapter and ratified budget before launch, caps runtime and output, and
preserves timeout, SKIP, and INCONCLUSIVE as a durable nonterminal `run.json`.
It never infers cache state from timing. A passing role adapter must supply its
real product and host binaries, 10 cold plus 10 warm trials with lifecycle,
process, and cache-boundary provenance, a full health response, and a nonempty
same-campaign trace plus typed analysis. Add `--require-controls` to exactly one
real role run to require the caught blank negative and external audio-thread
exclusion proof as part of the same runner-owned evidence directory.

Each adapter is an absolute executable that accepts `--request PATH --receipt
PATH`. The request is `pulp.gpu-first-visible-campaign-request.v1`; the adapter
must write `pulp.gpu-first-visible-campaign-adapter.v1` and keep every declared
artifact under the supplied `artifact_directory`. Adapter outcomes map exactly
to exit codes `pass=0`, `fail=1`, `inconclusive=2`, and `skip=3`.

After ratifying the budget, run all four roles with their exact identity files
and role-specific executables:

```bash
: "${A3_EVIDENCE:?new empty absolute evidence directory}"
: "${PULP_PLANNING_ROOT:?clean pulp-planning checkout}"
: "${BLANK_CONTROL_BIN:?exact final-head standalone blank-control test binary}"
: "${AUDIO_CONTROL_BIN:?exact final-head audio-thread-control test binary}"
: "${PULP_A3_ANALYZER_CARGO_TARGET_ROOT:?absolute Cargo target root outside the Pulp checkout}"
PULP_REVISION=$(git rev-parse HEAD)
PLAN_REVISION=$(git -C "$PULP_PLANNING_ROOT" rev-parse HEAD)
A3_ADAPTER="$PWD/tools/scripts/gpu_first_visible_a3_external_adapter.py"
A3_SUPPORT="$PWD/tools/scripts/gpu_first_visible_a3_role_producer.py"
TRACE_ANALYZER="$PWD/tools/scripts/gpu_first_visible_a3_trace_analyzer.py"
BUILD_VERIFIER="$PWD/tools/scripts/gpu_first_visible_a3_build_verifier.py"
: "${STANDALONE_PRODUCT_BIN:?exact final-head Standalone executable}"
: "${STANDALONE_DRIVER:?exact executable that automates the 20 Standalone lifecycles}"
: "${STANDALONE_DRIVER_SOURCE_PATH:?reviewed Pulp-relative driver path}"
: "${STANDALONE_BUILD_ATTESTATION:?closed Standalone build attestation JSON}"
: "${STANDALONE_BUILD_PROVENANCE:?digest-bound Standalone build receipt}"
: "${HEADLESS_PRODUCT_BIN:?exact final-head constrained-headless executable}"
: "${HEADLESS_DRIVER:?exact executable that automates the 20 headless lifecycles}"
: "${HEADLESS_DRIVER_SOURCE_PATH:?reviewed Pulp-relative driver path}"
: "${HEADLESS_BUILD_ATTESTATION:?closed headless build attestation JSON}"
: "${HEADLESS_BUILD_PROVENANCE:?digest-bound headless build receipt}"
: "${DAW_PRODUCT_BIN:?exact final-head plugin Mach-O or DLL}"
: "${DAW_PLUGIN_BUNDLE:?exact .vst3, .clap, or .component bundle}"
: "${REAPER_DRIVER:?exact executable that automates the 20 REAPER lifecycles}"
: "${REAPER_DRIVER_SOURCE_PATH:?reviewed Pulp-relative driver path}"
: "${REAPER_BUILD_ATTESTATION:?closed DAW-product build attestation JSON}"
: "${REAPER_BUILD_PROVENANCE:?digest-bound DAW-product build receipt}"
: "${FORGE_APP_BIN:?exact final-head Forge app executable}"
: "${FORGE_APP_BUNDLE:?exact final-head Forge .app bundle}"
: "${FORGE_DRIVER:?exact executable that automates the 20 Forge lifecycles}"
: "${FORGE_DRIVER_SOURCE_OWNER:?pulp or forge source authority for the driver}"
: "${FORGE_DRIVER_SOURCE_PATH:?reviewed authority-relative driver path}"
: "${FORGE_BUILD_ATTESTATION:?closed Forge build attestation JSON}"
: "${FORGE_BUILD_PROVENANCE:?digest-bound Forge build receipt}"
: "${FORGE_ROOT:?clean Forge source checkout at the identity revision}"

python3 tools/scripts/gpu_first_visible_a3_acceptance.py ratify-budget \
  --cold budget-cold.json --warm budget-warm.json \
  --plan-revision "$PLAN_REVISION" --pulp-revision "$PULP_REVISION" \
  --evidence-root "$A3_EVIDENCE" --output "$A3_EVIDENCE/budget.json"

PULP_A3_ROLE_PRODUCER_SUPPORT="$A3_SUPPORT" \
PULP_A3_PULP_ROOT="$PWD" \
PULP_A3_TRACE_ANALYZER="$TRACE_ANALYZER" \
PULP_A3_BUILD_VERIFIER="$BUILD_VERIFIER" \
PULP_A3_ANALYZER_CARGO_TARGET_ROOT="$PULP_A3_ANALYZER_CARGO_TARGET_ROOT" \
PULP_A3_STANDALONE_PRODUCT_BIN="$STANDALONE_PRODUCT_BIN" \
PULP_A3_STANDALONE_HOST_BIN="$STANDALONE_PRODUCT_BIN" \
PULP_A3_STANDALONE_DRIVER="$STANDALONE_DRIVER" \
PULP_A3_STANDALONE_DRIVER_SOURCE_OWNER=pulp \
PULP_A3_STANDALONE_DRIVER_SOURCE_PATH="$STANDALONE_DRIVER_SOURCE_PATH" \
PULP_A3_STANDALONE_BUILD_ATTESTATION="$STANDALONE_BUILD_ATTESTATION" \
PULP_A3_STANDALONE_BUILD_PROVENANCE="$STANDALONE_BUILD_PROVENANCE" \
PULP_A3_CAMPAIGN_PRODUCER="$PWD/tools/scripts/gpu_first_visible_a3_standalone_producer.py" \
PULP_A3_BLANK_CONTROL_BIN="$BLANK_CONTROL_BIN" \
PULP_A3_AUDIO_CONTROL_BIN="$AUDIO_CONTROL_BIN" \
python3 tools/scripts/gpu_first_visible_a3_campaign.py run-role \
  --role standalone --identity "$A3_EVIDENCE/standalone-identity.json" \
  --budget-receipt "$A3_EVIDENCE/budget.json" \
  --budget-cold "$A3_EVIDENCE/budget-cold.json" \
  --budget-warm "$A3_EVIDENCE/budget-warm.json" \
  --adapter "$A3_ADAPTER" \
  --output-dir "$A3_EVIDENCE/standalone-run" --require-controls

PULP_A3_ROLE_PRODUCER_SUPPORT="$A3_SUPPORT" \
PULP_A3_PULP_ROOT="$PWD" \
PULP_A3_TRACE_ANALYZER="$TRACE_ANALYZER" \
PULP_A3_BUILD_VERIFIER="$BUILD_VERIFIER" \
PULP_A3_ANALYZER_CARGO_TARGET_ROOT="$PULP_A3_ANALYZER_CARGO_TARGET_ROOT" \
PULP_A3_HEADLESS_PRODUCT_BIN="$HEADLESS_PRODUCT_BIN" \
PULP_A3_HEADLESS_HOST_BIN="$HEADLESS_PRODUCT_BIN" \
PULP_A3_HEADLESS_DRIVER="$HEADLESS_DRIVER" \
PULP_A3_HEADLESS_DRIVER_SOURCE_OWNER=pulp \
PULP_A3_HEADLESS_DRIVER_SOURCE_PATH="$HEADLESS_DRIVER_SOURCE_PATH" \
PULP_A3_HEADLESS_BUILD_ATTESTATION="$HEADLESS_BUILD_ATTESTATION" \
PULP_A3_HEADLESS_BUILD_PROVENANCE="$HEADLESS_BUILD_PROVENANCE" \
PULP_A3_CAMPAIGN_PRODUCER="$PWD/tools/scripts/gpu_first_visible_a3_headless_producer.py" \
python3 tools/scripts/gpu_first_visible_a3_campaign.py run-role \
  --role headless-constrained \
  --identity "$A3_EVIDENCE/headless-identity.json" \
  --budget-receipt "$A3_EVIDENCE/budget.json" \
  --budget-cold "$A3_EVIDENCE/budget-cold.json" \
  --budget-warm "$A3_EVIDENCE/budget-warm.json" \
  --adapter "$A3_ADAPTER" \
  --output-dir "$A3_EVIDENCE/headless-run"

PULP_A3_ROLE_PRODUCER_SUPPORT="$A3_SUPPORT" \
PULP_A3_PULP_ROOT="$PWD" \
PULP_A3_TRACE_ANALYZER="$TRACE_ANALYZER" \
PULP_A3_BUILD_VERIFIER="$BUILD_VERIFIER" \
PULP_A3_ANALYZER_CARGO_TARGET_ROOT="$PULP_A3_ANALYZER_CARGO_TARGET_ROOT" \
PULP_A3_REAPER_PRODUCT_BIN="$DAW_PRODUCT_BIN" \
PULP_A3_REAPER_HOST_BIN=/Applications/REAPER.app/Contents/MacOS/REAPER \
PULP_A3_REAPER_DRIVER="$REAPER_DRIVER" \
PULP_A3_REAPER_DRIVER_SOURCE_OWNER=pulp \
PULP_A3_REAPER_DRIVER_SOURCE_PATH="$REAPER_DRIVER_SOURCE_PATH" \
PULP_A3_REAPER_PLUGIN_BUNDLE="$DAW_PLUGIN_BUNDLE" \
PULP_A3_REAPER_SMOKE="$PWD/tools/testing/daw-smoke/reaper_smoke.py" \
PULP_A3_REAPER_SMOKE_LUA="$PWD/tools/testing/daw-smoke/insert_and_float.lua" \
PULP_A3_REAPER_BUILD_ATTESTATION="$REAPER_BUILD_ATTESTATION" \
PULP_A3_REAPER_BUILD_PROVENANCE="$REAPER_BUILD_PROVENANCE" \
PULP_A3_CAMPAIGN_PRODUCER="$PWD/tools/scripts/gpu_first_visible_a3_reaper_producer.py" \
python3 tools/scripts/gpu_first_visible_a3_campaign.py run-role \
  --role daw --identity "$A3_EVIDENCE/daw-identity.json" \
  --budget-receipt "$A3_EVIDENCE/budget.json" \
  --budget-cold "$A3_EVIDENCE/budget-cold.json" \
  --budget-warm "$A3_EVIDENCE/budget-warm.json" \
  --adapter "$A3_ADAPTER" \
  --output-dir "$A3_EVIDENCE/daw-run"

PULP_A3_ROLE_PRODUCER_SUPPORT="$A3_SUPPORT" \
PULP_A3_PULP_ROOT="$PWD" \
PULP_A3_TRACE_ANALYZER="$TRACE_ANALYZER" \
PULP_A3_BUILD_VERIFIER="$BUILD_VERIFIER" \
PULP_A3_ANALYZER_CARGO_TARGET_ROOT="$PULP_A3_ANALYZER_CARGO_TARGET_ROOT" \
PULP_A3_FORGE_ROOT="$FORGE_ROOT" \
PULP_A3_FORGE_PRODUCT_BIN="$FORGE_APP_BIN" \
PULP_A3_FORGE_HOST_BIN="$FORGE_APP_BIN" \
PULP_A3_FORGE_APP_BUNDLE="$FORGE_APP_BUNDLE" \
PULP_A3_FORGE_DRIVER="$FORGE_DRIVER" \
PULP_A3_FORGE_DRIVER_SOURCE_OWNER="$FORGE_DRIVER_SOURCE_OWNER" \
PULP_A3_FORGE_DRIVER_SOURCE_PATH="$FORGE_DRIVER_SOURCE_PATH" \
PULP_A3_FORGE_BUILD_ATTESTATION="$FORGE_BUILD_ATTESTATION" \
PULP_A3_FORGE_BUILD_PROVENANCE="$FORGE_BUILD_PROVENANCE" \
PULP_A3_CAMPAIGN_PRODUCER="$PWD/tools/scripts/gpu_first_visible_a3_forge_producer.py" \
python3 tools/scripts/gpu_first_visible_a3_campaign.py run-role \
  --role forge --identity "$A3_EVIDENCE/forge-identity.json" \
  --budget-receipt "$A3_EVIDENCE/budget.json" \
  --budget-cold "$A3_EVIDENCE/budget-cold.json" \
  --budget-warm "$A3_EVIDENCE/budget-warm.json" \
  --adapter "$A3_ADAPTER" \
  --output-dir "$A3_EVIDENCE/forge-run"
```

Before codesigning each product, embed the canonical build identity in the
exact executable that the campaign measures:

```bash
python3 tools/scripts/gpu_first_visible_a3_build_verifier.py emit \
  --identity product-build-identity.json --output product-build-identity.marker
```

The identity JSON contains exactly `pulp_revision`, `forge_revision`,
`build_id`, `product_id`, `product_name`, and `plugin_format`, matching the role
identity. On macOS, the product build may link the marker as a data section
(for example, `-Wl,-sectcreate,__DATA,__pulp_a3,product-build-identity.marker`)
before signing. The checked-in verifier scans the final executable, requires
exactly one canonical marker, and proves both a tampered-product and a
wrong-source negative. A build attestation or filename cannot substitute for
the embedded marker.

On the M5 endpoint, reserve 90–150 minutes after the exact final-head products,
drivers, and overhead variants exist. Budget ratification, the four campaigns,
and the trace-producer overhead control comprise at least 320 observations (20
budget observations, 80 role observations, and 220 four-state overhead
observations), with one additional REAPER scan/editor-open preflight, trace
replay, and human Perfetto UI correlation. Driver timeouts keep any one role
bounded; do not compress the run by reusing lifecycle IDs, omitting cold
process boundaries, or relabeling capture completion as native presentation.

The checked-in external adapter is an evidence envelope, not a substitute for
the product lifecycle. It snapshots the checked-in role entry point named by
`PULP_A3_CAMPAIGN_PRODUCER`, runs that immutable copy, bounds its output and
runtime, and preserves its exact digest in `run.json`. The four entry points
share `gpu_first_visible_a3_role_producer.py`; the engine snapshots itself,
the configured role driver, and exact product and host executable bytes before
the campaign. It ignores an inherited support override, then pins the support
file beside the checked-in role entry point under the exact Pulp source root.
Standalone, constrained headless, and Forge require product and host to resolve
to one executable. The isolated driver process group is reaped after success,
failure, timeout, or signal forwarding, so a returned campaign cannot leave
host automation able to mutate evidence. A role producer accepts the same
`--request PATH --receipt PATH`,
writes
`pulp.gpu-first-visible-campaign-producer.v1`, and uses the supplied
`artifact_directory`. Its receipt has the adapter receipt's identity and
outcome fields but exactly these artifacts: `health_result`, `raw_cold`,
`raw_warm`, `product_artifact`, `host_artifact`, `trace`, and
`trace_analysis`. It uses the same outcome exit map. Missing or invalid
producer configuration, timeout, schema drift, exit mismatch, or an omitted
artifact cannot become a pass.

Before invoking the role driver, the producer also requires a
`pulp.gpu-first-visible-product-build-attestation.v1` JSON document. Its exact
fields bind the requested Pulp/Forge revisions, build ID, product ID/name and
format to the product digest, complete bundle-tree digest when applicable,
the lifecycle-driver and source-bound trace-analyzer wrapper digests, the local-clean
provenance kind, and the digest of the separately retained build receipt. That
receipt must itself use `pulp.gpu-first-visible-local-build-provenance.v1` and
repeat the exact product identity, clean Pulp/Forge source revisions, product,
bundle, driver, and analyzer digests, bounded build command, builder identity,
and ordered UTC build timestamps. Opaque bytes or an unverified GitHub/Shipyard
claim are rejected. The configured binaries, build
documents, pinned driver/analyzer/verifier/support, source-bound smoke helpers, closed
driver request, and deterministic bundle snapshots are rehashed after the
driver and again after trace replay. A claim without those external build
inputs remains nonterminal; the producer never infers source provenance from a
filename or the current checkout. The analyzer wrapper runs the requested
revision's Rust analyzer with `--release --locked --offline` and a Cargo target
directory outside the source tree. Its structural campaign replay normally
returns `unverified`/exit 2 because it has no A3 budget; that structural verdict
is kept separate from the campaign health/budget verdict.
The separately source-bound build verifier scans the final executable, retains
its own closed receipt and negative controls, and is included in the immutable
host evidence; it is not retroactively asserted as a field in the build
attestation.

The role driver is the product/host automation seam. It accepts the closed
`pulp.gpu-first-visible-role-driver-request.v1` request and writes
`pulp.gpu-first-visible-role-driver-receipt.v1`. A pass requires 20 ordered
lifecycle-provenance rows, including the observed cache boundary, unique
lifecycle and process identities, both the lifecycle and process predecessor
for every same-process warm reopen, the owned OS host PID, and explicit
endpoint/native-present truth.
The predecessor must be an earlier observed lifecycle in the same process. The
producer cross-checks those rows against the raw cold/warm observations. It
returns four measured artifacts beneath its issued directory: health, raw cold,
raw warm, and the same-instance Perfetto trace. The
role producer rejects a short campaign, a relabeled cache state, a visible
trial without independent native presentation, a headless trial claiming
presentation, or mutation of any configured binary or snapshot. The producer,
not the driver, runs the pinned source-bound analyzer wrapper against both an invalid-
trace negative and the role trace, then derives the digest-bound campaign trace
analysis only when replay proves the health result's exact GPU evidence ID and
capture scope. The
digest-bound `host_artifact` tar retains the exact host, driver, producer
support, analyzer, build verifier, build documents, closed driver
request/receipt, and role
preflight. The driver may be
external because native AppKit/REAPER/Forge automation is product-specific, but
its runtime path and bytes must exactly match the declared reviewed file at the
requested clean Pulp or Forge Git revision, and its digest must also be bound by
the validated build receipt. Every process identity maps to one OS PID, and all
recorded host PIDs must be gone both before
trace replay and before PASS; a detached LaunchServices/GUI process is a hard
failure rather than a silent cache contaminant. Absence is a nonterminal
dependency, never synthesized evidence.

The producer therefore owns the facts only its product can observe: 10 real
cold and 10 real warm lifecycles, the declared cache reset/reopen boundary,
first nonblank native-compositor presentation (or constrained headless capture
completion), exact product/host identity, and same-campaign GPU/trace evidence
IDs. The campaign trace keeps the ratified startup-budget `verdict` separate
from `trace_replay_verdict`: a complete real `gpu-startup` replay is normally
`unverified` because that analyzer has no A3 budget. Its exact evidence ID,
Perfetto process identity, and process PID must resolve to one of the 20
recorded host lifecycles. For the one `--require-controls` role, the envelope
separately snapshots and runs the two focused harness binaries named above and
rejects a passing producer when either independent control is absent.

The DAW adapter must first prove the exact format scans and opens in REAPER;
for example, a VST3 preflight uses `reaper_smoke.py --mode editor-open --format
vst3 --plugin-name NAME --plugin-path /absolute/product.vst3 --reaper-bin
/Applications/REAPER.app/Contents/MacOS/REAPER`. That smoke is a
prerequisite, not the 20-trial campaign: SKIP/INCONCLUSIVE remains pending, and
capture completion cannot substitute for native compositor presentation. The
producer binds both the exact plugin bundle executable and the smoke harness's
checked-in `insert_and_float.lua` helper. It snapshots the complete bundle,
retains its deterministic digest, and rejects any smoke/lifecycle mutation.
The Forge adapter likewise drives one
exact executable inside the configured `.app` bundle and binds both Pulp and
Forge SHAs from source checkouts with no tracked or untracked changes. It also
snapshots and mutation-guards the complete `.app` bundle. The existing
standalone product test is a useful wiring
preflight but remains one observation; it is not a role adapter until a real
lifecycle harness supplies all 20 trials and role-appropriate presentation
evidence.

The external adapter selected for `--require-controls` produces both closed
control receipts with the focused, built harness binaries. Its equivalent
direct commands, useful for a preflight before the campaign, are:

```bash
PULP_A3_BLANK_NEGATIVE_RECEIPT_PATH="$ARTIFACT_DIRECTORY/blank-negative.json" \
  build-a3-release/test/a3-product/pulp-test-control-gpu-health-standalone-product \
  "exact Standalone product catches the seeded transparent first frame"

PULP_A3_AUDIO_THREAD_EXCLUSION_RECEIPT_PATH="$ARTIFACT_DIRECTORY/audio-thread.json" \
  build-a3-release/test/pulp-test-control-gpu-health-provider \
  "external harness observes every GPU health entry point off a registered audio thread"
```

The first command launches the real standalone product with the transparent
first-frame seed and writes a receipt only after observing
`gpu.startup.blank`. The second explicitly registers a live harness thread as
audio while exercising every provider entry point on non-audio threads, with a
separate wrong-thread rejection test protecting the writer guard. Its receipt intentionally says
`external-harness-only-not-product-runtime-proof`; the role adapter must not
upgrade that bounded claim.

## Ratify trace-producer overhead

The Pulp-owned `gpu_health_transition_first_visible` spans are real runtime
producers. The active 10-cold/10-warm role campaigns include their cost, but
that alone cannot distinguish the instrumentation's cost from the product
workload. Terminal A3 therefore also requires a four-state product control:

- exact pre-producer parent `5048ce72dd28d87974550a3feb526de0f44af32c`;
- exact final-head candidate built with tracing compiled out;
- the same candidate built with tracing compiled in but no session active; and
- that exact compiled-in binary with an active 128 MiB Perfetto ring.

All four states use one machine, product, workload, build family, and
source-bound measurement driver. Each raw document contains 5 warmups, 30
measured same-product trials, and 20 fresh-process trials. Every sample has a
unique evidence ID, positive duration, zero xruns, and zero audio-thread trace
events. The active state additionally retains a digest/size-bound trace for
every sample; inactive states may not claim one. The compiled-in idle and active
states must identify the same executable bytes, while the compile-out build is
distinct. The driver is a reviewed relative path in the candidate revision and
must remain byte-identical at final verification.

After the source-bound product driver has written the four raw documents under
the evidence root, derive the only accepted receipt with:

```bash
python3 tools/scripts/gpu_first_visible_a3_trace_producer_overhead.py ratify \
  --pre-change-baseline "$A3_EVIDENCE/overhead/pre-change-baseline.json" \
  --candidate-compile-out "$A3_EVIDENCE/overhead/candidate-compile-out.json" \
  --candidate-compiled-in-idle "$A3_EVIDENCE/overhead/candidate-compiled-in-idle.json" \
  --candidate-active "$A3_EVIDENCE/overhead/candidate-active.json" \
  --evidence-root "$A3_EVIDENCE" \
  --generated-utc "$(date -u +%Y-%m-%dT%H:%M:%SZ)" \
  --output "$A3_EVIDENCE/trace-producer-overhead.json"
```

Against the pre-change baseline, both measured and fresh-process medians/p95s
must remain within 1%/2% for compile-out, 2%/5% for compiled-in idle, and
5%/10% for active capture. A failed ceiling produces a derived FAIL receipt; a
missing final-head product variant produces `unavailable`. Neither is terminal.
The final acceptance template must set `trace_producer_overhead.status` to
`pass` and bind the derived receipt. The checked-in nonterminal receipt records
this control as unavailable because those exact M5 variants and trials have not
yet run; it does not waive them.

Copy each passing `run.json` campaign fragment and the requested control refs
into the final template, add the passing trace-producer-overhead receipt and
same-instance A2T bundle, and run the closed
generator above. Each final campaign preserves and re-verifies the exact outer
adapter and nested role-producer bytes, so an opaque or later-replaced harness
cannot inherit the trials. The generator independently derives exactly one B4
disposition from the validated causal campaign and replayed pinned analyzer;
any submitted disposition or evidence that disagrees is rejected.

## Current standalone production boundary

The standalone control host binds `dev.pulp.gpu/health.read@1` to the broker's
exact registration, instance, and publication identifiers. Its first ordinary
UI-loop observation can also carry that installed lifecycle identifier, native
adapter identity, back-buffer content statistics, capture-completion upper
bound, and observed image signature. An authentic nonblank capture without
direct GPU-submission proof is `unverified`; it is neither a startup pass nor
an unavailable capture.

The host may publish before that first UI-loop observation. A one-shot read in
that window returns the pending snapshot and is not campaign evidence; an
acceptance harness must wait for the post-capture snapshot before preserving
the response artifact.

The checked-in host observation does not claim an observed cold/warm cache boundary, GPU submission,
native presentation timestamp, compile/prepare/upload/hidden/present stage
timings, source or shader identity, or same-instance trace correlation. The
generic render lifecycle producers for those facts route to Vellum after
framework adoption. If a ratified campaign misses its budget and the existing
trace cannot separate compile or prepare, upload, hidden-frame, and native
present work from the unattributed interval, those causal fields remain null. A
validated, lossless, over-budget visible campaign may then derive
`queue-B4-investigation` before those producers exist, but only when its B4
receipt names every missing field, event, required argument, observed interval,
and exact `framework-authoritative-transferred` route. That disposition requests
post-adoption instrumentation and a causal rerun; it is not prewarm evidence.

If the validated disposition is `queue-B4` or `queue-B4-investigation`, the
first post-adoption Vellum experiment is Graphite `PipelineManager` work on a
bounded `SkExecutor` supplied through `ContextOptions`, before any custom
prewarm design. Instrument pipeline queued/start/end, cache hit/miss, pipeline
signature, and render-wait intervals, then compare the exact 10-cold/10-warm
workload. Keep executor work off the audio thread and define bounded ownership,
lifetime, and shutdown. Adopt nothing unless the rerun proves a causal,
material benefit; otherwise record `no-change`. A generic Vellum-installed
`SkLogHandler` can be a later diagnostic producer. Neither item is Horizon A or
part of the initial Pulp implementation.

The gap inventory uses the existing low-cardinality Perfetto vocabulary:
`gpu_shader_compile` for compile and source/shader signatures,
`gpu_resource_upload` for upload, `gpu_pipeline_prepare` for hidden-frame work,
and `gpu_present` for presentation. Required arguments are exact bounded
`debug.*` keys (including the GPU evidence ID and the field-specific signature,
resource, frame, visible-state, or presentation timestamp key). A syntactically
plausible substitute event or an abbreviated argument list is rejected.

Until a validated campaign actually misses the ratified budget, the B4
disposition remains unset. Missing transferred producers do not by themselves
justify prewarm or permit a hand-authored investigation disposition.

### Exact standalone product observation

`gpu-first-visible-a3-standalone-product-response.json` preserves one fresh
`dev.pulp.gpu/health.read@1` response from the real standalone composition at
source revision `c568198f356d4b961b4fb6bb68caa3d3ccebd3c4`. The canonical client
selected the exact installed instance, registration, and publication recorded
in `gpu-first-visible-a3-standalone-product-binding.json`, waited until the
startup trial existed, and retained the response and fixture digests.

This observation confirms that the shipped host can report an authentic
Metal-backed Dawn adapter, completed back-buffer readback, and nonblank content.
It also demonstrates the intended fail-closed boundary: `command_submitted` is
null, the submit event and aggregate health are `unverified`, startup is
`incomplete`, the budget is `unratified`, and frame-lifecycle and A2T
correlation categories are missing. It therefore advances product plumbing
without satisfying a campaign, terminal acceptance, or B4 disposition gate.

After building the product test target at the source revision, regenerate the
observation in a temporary location with:

```bash
PULP_A3_EVIDENCE_SOURCE_REVISION=c568198f356d4b961b4fb6bb68caa3d3ccebd3c4 \
PULP_A3_EVIDENCE_RESPONSE_PATH=/private/tmp/gpu-first-visible-a3-standalone-product-response.json \
build-a3-release/test/a3-product/pulp-test-control-gpu-health-standalone-product \
  "exact Standalone product instance publishes capture-only GPU health"
```

The response is intentionally a single product observation. The ratified
budget, four 10-cold/10-warm product campaigns, direct submission and the
role-appropriate endpoint, same-instance Perfetto/A2T
correlation, real blank negative, audio-thread exclusion, and legal B4
disposition remain required below.

## Terminal evidence

A complete receipt must bind the exact planning revision and digest, Pulp and
Forge revisions, build, product, plugin format, machine, product instance, and
campaign. It must also contain:

- A ratified `pulp.gpu-first-visible-budget.v1` receipt with its exact raw
  10-cold and 10-warm reference-host artifacts. Version 1 derives the threshold
  deterministically as the larger cache-state p95 plus one bound-host refresh
  interval, rounded up to whole milliseconds; a hand-selected threshold or an
  unbound/implausible reference-host refresh rate is rejected.
- Exactly one passing `pulp.gpu-health-read-result.v1` campaign for standalone,
  headless-constrained, a real DAW/plugin format, and the exact Forge shell.
  Each campaign carries its own raw 10-cold and 10-warm artifacts plus the
  measured product and host artifacts. Standalone, DAW, and Forge bind
  `native-compositor-presentation`; headless-constrained binds
  `headless-capture-complete` and must not claim compositor timing. Every trial
  has a measured end-to-end endpoint, nonblank target proof, and bounded hitch.
  Compile, upload, hidden-frame, present, source, and shader causal fields may
  be consistently null only for passing `no-change` or failing
  `queue-B4-investigation`, with exact coverage gaps named and routed. Every
  campaign also carries a nonempty trace and typed digest-bound campaign trace
  analysis. The selected causal campaign additionally receives the full pinned
  analyzer replay described below.
- An exact-digest A2T receipt, pinned analyzer executable, raw Perfetto trace,
  analyzed trace, and binding receipt that agree with one campaign's build,
  instance, campaign, GPU evidence, and trace evidence identifiers. The verifier
  anchors the analyzer digest in the A2T receipt and replays that snapshotted
  analyzer over the snapshotted trace; submitted analysis JSON is not trusted.
- A caught transparent-frame negative and an audio-thread exclusion receipt
  from an external instrumented harness. The receipt must cover the exact known
  `ControlGpuHealthProvider` entry points, record zero events on explicitly
  registered audio threads, and include a non-audio positive control. This is
  external harness evidence; it is deliberately not described as product-runtime
  thread proof.
- A passing four-state trace-producer overhead receipt derived from the exact
  pre-producer parent and final-head compile-out, compiled-in idle, and active
  variants. It binds the same host/workload/build family and source-bound driver,
  requires zero xruns and audio-thread trace events, enforces the 128 MiB ring,
  and checks measured plus fresh-process median/p95 ceilings. Missing,
  unavailable, or failed overhead evidence keeps A3 nonterminal.
- Exactly one disposition: `queue-B4`, `queue-B4-investigation`, or
  `no-change`. The verifier derives it from the validated causal campaign and
  replayed A2T result under `pulp.b4-disposition-policy.v1`; neither the health
  provider's advisory pipeline fields nor a hand-authored disposition can select
  the result. The disposition receipt binds the derived inputs and artifact
  digests.

The verifier recomputes nearest-rank p95 values, requires lossless capture for
every available category, rejects unavailable or unverified health campaigns,
and treats missing instrumentation categories separately from dropped or
truncated events. It cross-checks raw
samples against the health results, and verifies every declared artifact
digest. The A2T no-producer disposition must also be accepted by the exact
planning revision/digest bound by A3; a stale `requires-approval` receipt cannot
close the causal gate. That A2T disposition applies only to the offline
CLI/MCP analyzer and does not waive the separate product trace-producer overhead
receipt. A product provider's locally complete snapshot is therefore input
evidence, never self-sufficient acceptance. Artifact traversal rejects symlinks
and parses the same immutable byte snapshot it hashes. Partial observations
belong in `observations`; every remaining gap must be named in
`missing_evidence` while `status` remains `incomplete`.
