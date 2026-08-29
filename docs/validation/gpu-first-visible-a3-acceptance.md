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
A3_EVIDENCE=/absolute/path/to/a3-evidence
PULP_REVISION=$(git rev-parse HEAD)
PLAN_REVISION=$(git -C /absolute/path/to/pulp-planning rev-parse HEAD)
A3_ADAPTER="$PWD/tools/scripts/gpu_first_visible_a3_external_adapter.py"

python3 tools/scripts/gpu_first_visible_a3_acceptance.py ratify-budget \
  --cold budget-cold.json --warm budget-warm.json \
  --plan-revision "$PLAN_REVISION" --pulp-revision "$PULP_REVISION" \
  --evidence-root "$A3_EVIDENCE" --output "$A3_EVIDENCE/budget.json"

PULP_A3_CAMPAIGN_PRODUCER=/absolute/path/to/standalone-a3-producer \
PULP_A3_BLANK_CONTROL_BIN=/absolute/path/to/pulp-test-control-gpu-health-standalone-product \
PULP_A3_AUDIO_CONTROL_BIN=/absolute/path/to/pulp-test-control-gpu-health-provider \
python3 tools/scripts/gpu_first_visible_a3_campaign.py run-role \
  --role standalone --identity "$A3_EVIDENCE/standalone-identity.json" \
  --budget-receipt "$A3_EVIDENCE/budget.json" \
  --budget-cold "$A3_EVIDENCE/budget-cold.json" \
  --budget-warm "$A3_EVIDENCE/budget-warm.json" \
  --adapter "$A3_ADAPTER" \
  --output-dir "$A3_EVIDENCE/standalone-run" --require-controls

PULP_A3_CAMPAIGN_PRODUCER=/absolute/path/to/headless-a3-producer \
python3 tools/scripts/gpu_first_visible_a3_campaign.py run-role \
  --role headless-constrained \
  --identity "$A3_EVIDENCE/headless-identity.json" \
  --budget-receipt "$A3_EVIDENCE/budget.json" \
  --budget-cold "$A3_EVIDENCE/budget-cold.json" \
  --budget-warm "$A3_EVIDENCE/budget-warm.json" \
  --adapter "$A3_ADAPTER" \
  --output-dir "$A3_EVIDENCE/headless-run"

PULP_A3_CAMPAIGN_PRODUCER=/absolute/path/to/reaper-a3-producer \
python3 tools/scripts/gpu_first_visible_a3_campaign.py run-role \
  --role daw --identity "$A3_EVIDENCE/daw-identity.json" \
  --budget-receipt "$A3_EVIDENCE/budget.json" \
  --budget-cold "$A3_EVIDENCE/budget-cold.json" \
  --budget-warm "$A3_EVIDENCE/budget-warm.json" \
  --adapter "$A3_ADAPTER" \
  --output-dir "$A3_EVIDENCE/daw-run"

PULP_A3_CAMPAIGN_PRODUCER=/absolute/path/to/exact-forge-shell-a3-producer \
python3 tools/scripts/gpu_first_visible_a3_campaign.py run-role \
  --role forge --identity "$A3_EVIDENCE/forge-identity.json" \
  --budget-receipt "$A3_EVIDENCE/budget.json" \
  --budget-cold "$A3_EVIDENCE/budget-cold.json" \
  --budget-warm "$A3_EVIDENCE/budget-warm.json" \
  --adapter "$A3_ADAPTER" \
  --output-dir "$A3_EVIDENCE/forge-run"
```

The checked-in external adapter is an evidence envelope, not a substitute for
the product lifecycle. It snapshots the executable named by
`PULP_A3_CAMPAIGN_PRODUCER`, runs that immutable copy, bounds its output and
runtime, and preserves its exact digest in `run.json`. A role producer accepts
the same `--request PATH --receipt PATH` argv, writes
`pulp.gpu-first-visible-campaign-producer.v1`, and uses the supplied
`artifact_directory`. Its receipt has the adapter receipt's identity and
outcome fields but exactly these artifacts: `health_result`, `raw_cold`,
`raw_warm`, `product_artifact`, `host_artifact`, `trace`, and
`trace_analysis`. It uses the same outcome exit map. Missing or invalid
producer configuration, timeout, schema drift, exit mismatch, or an omitted
artifact cannot become a pass.

The producer owns the facts only its product can observe: 10 real cold and 10
real warm lifecycles, the declared cache reset/reopen boundary, first nonblank
native-compositor presentation (or constrained headless capture completion),
exact product/host identity, and same-campaign GPU/trace evidence IDs. The
envelope never manufactures those fields. For the one `--require-controls`
role, it separately snapshots and runs the two focused harness binaries named
above and rejects a passing producer when either independent control is absent.

The DAW adapter must first prove the exact format scans and opens in REAPER;
for example, a VST3 preflight uses `reaper_smoke.py --mode editor-open --format
vst3 --plugin-name NAME --plugin-path /absolute/product.vst3`. That smoke is a
prerequisite, not the 20-trial campaign: SKIP/INCONCLUSIVE remains pending, and
capture completion cannot substitute for native compositor presentation. The
Forge adapter likewise drives the exact standalone shell and binds both Pulp
and Forge SHAs. The existing standalone product test is a useful wiring
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

Copy each passing `run.json` campaign fragment and the requested control refs
into the final template, add the same-instance A2T bundle, and run the closed
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
close the causal gate. A product provider's locally complete snapshot is therefore input
evidence, never self-sufficient acceptance. Artifact traversal rejects symlinks
and parses the same immutable byte snapshot it hashes. Partial observations
belong in `observations`; every remaining gap must be named in
`missing_evidence` while `status` remains `incomplete`.
