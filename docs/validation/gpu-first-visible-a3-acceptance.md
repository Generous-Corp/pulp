# GPU first-visible acceptance receipts

`gpu-first-visible-a3-acceptance.json` is the closed evidence receipt for the
GPU first-visible acceptance gate. Its schema deliberately keeps incomplete
work representable without allowing it to become a pass.

The canonical instance now uses
`docs/contracts/gpu-first-visible-a3-acceptance-v2.schema.json`. It truthfully
records `blocked-product-policy` with both `product-policy` and
`required-coverage` blockers: no protected product-budget authority currently
names the seven thresholds or the constrained-adapter assignment. Collection
must not begin until those protected inputs exist. Version 1 remains readable
for historical evidence, but its former `complete` state is nonterminal.

V2 derives this exact ordered role vocabulary and refuses substitutions:

1. `pulp-standalone`
2. `forge-modular-standalone`
3. `forge-modular-auv2-logic`
4. `forge-modular-vst3-reaper`
5. `forge-modular-clap-reaper`
6. `headless-reference`
7. `constrained-adapter`

The protected authority, not the executor, freezes the exact M5 host identity,
host/application/format mapping, first-visible, first-interaction, steady CPU
and GPU thresholds, Pulp standalone canary, A4 scenario budgets, and objective
constrained-adapter predicate/configuration with support-matrix and A1 evidence.

## Verify a receipt

```bash
python3 tools/scripts/gpu_first_visible_a3_acceptance.py verify \
  docs/validation/gpu-first-visible-a3-acceptance.json \
  --evidence-root docs/validation
```

The verifier returns `0` only for a terminal pass, `2` for a valid nonterminal
receipt, and `1` for malformed, inconsistent, missing, or digest-mismatched
evidence. A nonterminal result is never an acceptance pass.

Terminal v2 verification is intentionally online and canonical-path-only. The
receipt may request publication only as `Generous-Corp/pulp`, `main`, the exact
canonical receipt path, and a complete sorted artifact-digest set. It may not
self-attest a head, blob, protection bit, or check result. The verifier derives
fresh live `main`, verifies the checkout is clean at that exact head, compares
the local/indexed/GitHub receipt blob, unions classic branch protection and
ruleset requirements, exhausts paginated check-run/status results, preserves
required app identities, and rejects missing, ambiguous-latest, pending, or
non-successful results.

The same live pass resolves the product-policy source to its exact Git blob,
requires its revision at a protected branch head, and verifies Daniel's stable
GitHub user ID as exact-head author or approver. Support-matrix and A1 inputs
are not accepted as bare JSON claims: each is a digest-bound wrapper whose
source-bound executable producer is rerun against the exact payload. Producer
identity reuses the existing passing embedded build-verifier and independent
exact-source rebuild receipts; a new self-attestation protocol is not accepted.
Campaign samples use the same rule, with producer output bound to raw-sample,
trace, and canonical identity digests.

Every terminal campaign retains the exact raw samples, trace, sample-producer
provenance, prepared analyzer and analyzer provenance, and a closed binding for
role/campaign/instance/build, GPU/trace evidence, and process PID/UPID. The
verifier hashes the trace and analyzer, runs the pinned analyzer over those
exact bytes, and requires the fresh replay to agree with the closed category,
zero-drop, completed-flush, evidence, and process bindings. A submitted trace
analysis sidecar cannot pass by itself.

The per-sample `blank=false` and zero audio-work counters are necessary but not
sufficient. Terminal v2 separately requires a caught, trace-digest-bound blank
negative; an executable, external-instrumented-harness audio-thread exclusion
receipt bound to the same campaign traces; and the existing derived four-state
trace-producer overhead receipt with a passing verdict at the implementation
head. Missing or unavailable controls keep the canonical receipt nonterminal.

## Historical v1 generation and collection reference

The remaining generation and product-collection commands document the
historical v1 harness only. They cannot create terminal A3 evidence. Author a template matching
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

## Historical v1 product campaign harness

`gpu_first_visible_a3_campaign.py` is the executable boundary between the
closed verifier and product-specific lifecycle automation. It snapshots the
exact adapter and ratified budget before launch, caps runtime and output, and
preserves timeout, SKIP, and INCONCLUSIVE as a durable nonterminal `run.json`.
It never infers cache state from timing. A passing role adapter must supply its
real product and host binaries, 10 cold plus 10 warm trials with lifecycle,
process, and cache-boundary provenance, a full health response, and a nonempty
same-campaign trace plus typed analysis. Add `--require-controls` to exactly one
real role run to require the caught blank negative and external audio-thread
exclusion proof as part of the same runner-owned evidence directory. Those
controls must be the two native CMake targets named below, built from the exact
clean `PULP_A3_CONTROL_SOURCE_ROOT` revision. Each executable embeds its target,
source path, Git revision/blob, configuration, and build ID; the adapter retains
that marker, executable digest, and producer receipt through final validation.
A copied script or shape-valid JSON receipt is not control evidence.

Each adapter is an absolute executable that accepts `--request PATH --receipt
PATH`. The request is `pulp.gpu-first-visible-campaign-request.v1`; the adapter
must write `pulp.gpu-first-visible-campaign-adapter.v1` and keep every declared
artifact under the supplied `artifact_directory`. Adapter outcomes map exactly
to exit codes `pass=0`, `fail=1`, `inconclusive=2`, and `skip=3`.

The command transcript below preserves the old four-role harness for evidence
readability. It is not the v2 seven-role campaign and cannot supply product
policy authority:

```bash
: "${A3_EVIDENCE:?new empty absolute evidence directory}"
: "${PULP_PLANNING_ROOT:?clean pulp-planning checkout}"
: "${BLANK_CONTROL_BIN:?exact final-head standalone blank-control test binary}"
: "${AUDIO_CONTROL_BIN:?exact final-head audio-thread-control test binary}"
PULP_REVISION=$(git rev-parse HEAD)
PLAN_REVISION=$(git -C "$PULP_PLANNING_ROOT" rev-parse HEAD)
A3_ADAPTER="$PWD/tools/scripts/gpu_first_visible_a3_external_adapter.py"
A3_SUPPORT="$PWD/tools/scripts/gpu_first_visible_a3_role_producer.py"
TRACE_ANALYZER="$PWD/tools/scripts/gpu_first_visible_a3_trace_analyzer.py"
BUILD_VERIFIER="$PWD/tools/scripts/gpu_first_visible_a3_build_verifier.py"
: "${STANDALONE_PRODUCT_BIN:?exact final-head Standalone executable}"
: "${STANDALONE_DRIVER:?exact executable that automates the 20 Standalone lifecycles}"
: "${STANDALONE_DRIVER_SOURCE_PATH:?reviewed Pulp-relative driver path}"
: "${STANDALONE_BUILD_DRIVER:?source-bound driver that rebuilds the exact product}"
: "${STANDALONE_BUILD_DRIVER_SOURCE_PATH:?reviewed Pulp-relative build-driver path}"
: "${STANDALONE_BUILD_ATTESTATION:?closed Standalone build attestation JSON}"
: "${STANDALONE_BUILD_PROVENANCE:?digest-bound Standalone build receipt}"
: "${HEADLESS_PRODUCT_BIN:?exact final-head constrained-headless executable}"
: "${HEADLESS_DRIVER:?exact executable that automates the 20 headless lifecycles}"
: "${HEADLESS_DRIVER_SOURCE_PATH:?reviewed Pulp-relative driver path}"
: "${HEADLESS_BUILD_DRIVER:?source-bound driver that rebuilds the exact product}"
: "${HEADLESS_BUILD_DRIVER_SOURCE_PATH:?reviewed Pulp-relative build-driver path}"
: "${HEADLESS_BUILD_ATTESTATION:?closed headless build attestation JSON}"
: "${HEADLESS_BUILD_PROVENANCE:?digest-bound headless build receipt}"
: "${DAW_PRODUCT_BIN:?exact final-head plugin Mach-O or DLL}"
: "${DAW_PLUGIN_BUNDLE:?exact .vst3, .clap, or .component bundle}"
: "${REAPER_DRIVER:?exact executable that automates the 20 REAPER lifecycles}"
: "${REAPER_DRIVER_SOURCE_PATH:?reviewed Pulp-relative driver path}"
: "${REAPER_BUILD_DRIVER:?source-bound driver that rebuilds the exact plugin bundle}"
: "${REAPER_BUILD_DRIVER_SOURCE_PATH:?reviewed Pulp-relative build-driver path}"
: "${REAPER_BUILD_ATTESTATION:?closed DAW-product build attestation JSON}"
: "${REAPER_BUILD_PROVENANCE:?digest-bound DAW-product build receipt}"
: "${FORGE_APP_BIN:?exact final-head Forge app executable}"
: "${FORGE_APP_BUNDLE:?exact final-head Forge .app bundle}"
: "${FORGE_DRIVER:?exact executable that automates the 20 Forge lifecycles}"
: "${FORGE_DRIVER_SOURCE_OWNER:?pulp or forge source authority for the driver}"
: "${FORGE_DRIVER_SOURCE_PATH:?reviewed authority-relative driver path}"
: "${FORGE_BUILD_DRIVER:?source-bound driver that rebuilds the exact Forge app}"
: "${FORGE_BUILD_DRIVER_SOURCE_OWNER:?pulp or forge source authority for the build driver}"
: "${FORGE_BUILD_DRIVER_SOURCE_PATH:?reviewed authority-relative build-driver path}"
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
PULP_A3_STANDALONE_PRODUCT_BIN="$STANDALONE_PRODUCT_BIN" \
PULP_A3_STANDALONE_HOST_BIN="$STANDALONE_PRODUCT_BIN" \
PULP_A3_STANDALONE_DRIVER="$STANDALONE_DRIVER" \
PULP_A3_STANDALONE_DRIVER_SOURCE_OWNER=pulp \
PULP_A3_STANDALONE_DRIVER_SOURCE_PATH="$STANDALONE_DRIVER_SOURCE_PATH" \
PULP_A3_STANDALONE_BUILD_DRIVER="$STANDALONE_BUILD_DRIVER" \
PULP_A3_STANDALONE_BUILD_DRIVER_SOURCE_OWNER=pulp \
PULP_A3_STANDALONE_BUILD_DRIVER_SOURCE_PATH="$STANDALONE_BUILD_DRIVER_SOURCE_PATH" \
PULP_A3_STANDALONE_BUILD_ATTESTATION="$STANDALONE_BUILD_ATTESTATION" \
PULP_A3_STANDALONE_BUILD_PROVENANCE="$STANDALONE_BUILD_PROVENANCE" \
PULP_A3_CAMPAIGN_PRODUCER="$PWD/tools/scripts/gpu_first_visible_a3_standalone_producer.py" \
PULP_A3_CONTROL_SOURCE_ROOT="$PWD" \
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
PULP_A3_HEADLESS_PRODUCT_BIN="$HEADLESS_PRODUCT_BIN" \
PULP_A3_HEADLESS_HOST_BIN="$HEADLESS_PRODUCT_BIN" \
PULP_A3_HEADLESS_DRIVER="$HEADLESS_DRIVER" \
PULP_A3_HEADLESS_DRIVER_SOURCE_OWNER=pulp \
PULP_A3_HEADLESS_DRIVER_SOURCE_PATH="$HEADLESS_DRIVER_SOURCE_PATH" \
PULP_A3_HEADLESS_BUILD_DRIVER="$HEADLESS_BUILD_DRIVER" \
PULP_A3_HEADLESS_BUILD_DRIVER_SOURCE_OWNER=pulp \
PULP_A3_HEADLESS_BUILD_DRIVER_SOURCE_PATH="$HEADLESS_BUILD_DRIVER_SOURCE_PATH" \
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
PULP_A3_REAPER_PRODUCT_BIN="$DAW_PRODUCT_BIN" \
PULP_A3_REAPER_HOST_BIN=/Applications/REAPER.app/Contents/MacOS/REAPER \
PULP_A3_REAPER_DRIVER="$REAPER_DRIVER" \
PULP_A3_REAPER_DRIVER_SOURCE_OWNER=pulp \
PULP_A3_REAPER_DRIVER_SOURCE_PATH="$REAPER_DRIVER_SOURCE_PATH" \
PULP_A3_REAPER_BUILD_DRIVER="$REAPER_BUILD_DRIVER" \
PULP_A3_REAPER_BUILD_DRIVER_SOURCE_OWNER=pulp \
PULP_A3_REAPER_BUILD_DRIVER_SOURCE_PATH="$REAPER_BUILD_DRIVER_SOURCE_PATH" \
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
PULP_A3_FORGE_ROOT="$FORGE_ROOT" \
PULP_A3_FORGE_PRODUCT_BIN="$FORGE_APP_BIN" \
PULP_A3_FORGE_HOST_BIN="$FORGE_APP_BIN" \
PULP_A3_FORGE_APP_BUNDLE="$FORGE_APP_BUNDLE" \
PULP_A3_FORGE_DRIVER="$FORGE_DRIVER" \
PULP_A3_FORGE_DRIVER_SOURCE_OWNER="$FORGE_DRIVER_SOURCE_OWNER" \
PULP_A3_FORGE_DRIVER_SOURCE_PATH="$FORGE_DRIVER_SOURCE_PATH" \
PULP_A3_FORGE_BUILD_DRIVER="$FORGE_BUILD_DRIVER" \
PULP_A3_FORGE_BUILD_DRIVER_SOURCE_OWNER="$FORGE_BUILD_DRIVER_SOURCE_OWNER" \
PULP_A3_FORGE_BUILD_DRIVER_SOURCE_PATH="$FORGE_BUILD_DRIVER_SOURCE_PATH" \
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

Before invoking the role driver, the producer independently runs a second,
source-bound build driver from the exact clean Pulp or Forge revision. Its
closed request exposes only those source roots and a fresh output directory,
not the configured measured product path. PASS requires the rebuilt executable
digest—and complete rebuilt bundle-tree digest for DAW/Forge—to equal the
measured bytes. The rebuilt Forge bundle must also bind
`CFBundleExecutable`, `CFBundleIdentifier`, and `CFBundleName` in its regular
`Contents/Info.plist` to the requested shell. This independent reproduction is
the product proof; caller-authored build documents remain supplemental.

The producer also requires a
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
filename or the current checkout. The analyzer wrapper runs a one-shot sealed
prepare protocol for the requested revision's Rust analyzer with `--release
--locked --offline`. It strips caller Cargo/Rust flags and runners, links only
locked offline caches into a fresh config-free Cargo home, uses a fresh target,
and retains exact Cargo/rustc paths, versions, digests, manifest/lock digests,
and the produced analyzer digest. The producer executes only that prepared
binary. Its structural campaign replay normally
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
for every same-process warm reopen, the owned OS host PID, producer-observed
process-start identity and executable digest, and explicit endpoint/native-
present truth. Every row answers a producer-issued nonce challenge while the
exact configured host is alive; the producer resolves and hashes that live
executable before acknowledging it. The driver names the one challenged PID
that produced the trace, and replay must select that exact PID rather than any
member of the campaign.
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
Perfetto process identity, and process PID must resolve to the trace-producing
live-host challenge. For the one `--require-controls` role, the envelope
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
The Forge adapter likewise drives the `CFBundleExecutable` named by the
configured app's regular `Contents/Info.plist`; its bundle identifier and name
must equal the requested product identity. It binds both Pulp and
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

- exact pre-producer parent `e3b4ee453b955262a243bb1a5c54c6909553551f`;
- exact final-head candidate built with tracing compiled out;
- the same candidate built with tracing compiled in but no session active; and
- that exact compiled-in binary with an active 128 MiB Perfetto ring.

All four states use one machine, product, workload, build family, selected A3
campaign identity, and source-bound measurement driver. Each raw document
contains 5 warmups, 30 measured same-product trials, and 20 fresh-process
trials. Every sample references immutable runtime metrics binding its host PID,
process-start identity, executable digest, audio-thread TIDs, monotonic
start/end timestamps, and zero xruns; duration is derived from those
timestamps. Fresh-process `(PID,start)` pairs are globally unique. The active
state additionally retains a digest/size-bound trace for every sample. Its
source-bound replay must find one matching session identity, the real
`gpu_health_transition_first_visible` span on a declared non-audio host thread,
and zero foreign producer or xrun events. Arbitrary marker-bearing bytes are
not traces. Inactive states may not claim one. The compiled-in idle and active
states must identify the same executable bytes, while the compile-out build is
distinct. The driver is a reviewed relative path in the candidate revision and
must remain byte-identical at final verification.

This is the exact execution matrix. Do not substitute a synthetic executable,
change the workload between rows, or infer an inactive session from a missing
trace:

| State | Source/build | Runtime session | Required trace evidence |
|---|---|---|---|
| `pre-change-baseline` | exact `e3b4ee453b955262a243bb1a5c54c6909553551f` product | tracing absent | none |
| `candidate-compile-out` | final candidate, `PULP_TRACING=OFF` | tracing absent | none |
| `candidate-compiled-in-idle` | final candidate, `PULP_TRACING=ON` | no session | none; binary digest must equal active |
| `candidate-active` | same compiled-in binary bytes | active 128 MiB ring | one lossless binary Perfetto trace per sample |

Each collection request is a
`pulp.gpu-first-visible-trace-producer-collection-request.v1` JSON object. It
must bind the state, the two revisions, selected passing role/campaign/product,
machine and plugin format, exact product binary SHA, and one identical
workload/build-family identity across all four files. It must also bind both
immutable producer packages exactly:

- `fefbfecd9fc014df54fc55d6f3259524f1179a49` for
  `gpu_health_transition_first_visible`;
- `b4ba22f1d700621366afdbc72bb8615336964cd1` for the five macOS
  input-to-present paths and its exact 20 producer signatures. The 16 slices
  are `native_drag_dispatch`, `pointer_coalescer_flush`, `mac_mouse_dragged`,
  `gpu_acquire`, `skia_begin`, `gpu_submit`, `gpu_present`,
  `editor_bridge_dispatch_json`, `editor_bridge_json_parse`,
  `view_repaint_request`, `repaint_request`, `frame_callback_pump`,
  `raf_flush`, `dom_event_dispatch`, `dom_event_evaluate`, and
  `dom_event_microtask_pump`; the four counters are
  `delivered_drag_samples`, `pointer_coalescer_flushes`,
  `pointer_samples_merged`, and `raw_drag_samples`. The immutable contract also
  binds every signature's slice/counter kind, `state`/`render`/`js` category,
  and fields (`callbacks` for `raf_flush`, `value` for counters).

`measurement_driver` and `workload.adapter_*` name the same reviewed,
candidate-relative executable path, revision, and digest. The driver receives
the product binary only through the collector's closed request; it owns the
real lifecycle, timing, xrun, audio-thread-TID, and trace artifacts. The
collector independently challenges the live executable 55 times per state
(5 warmup + 30 measured + 20 fresh), records 110 challenge/ack artifacts plus
the transcript, verifies exact executable/start identity, and refuses leftover
processes. `evidence_id` is derived from the challenge/attempt/logical process,
never from a reusable PID.

`state_build_driver` is a second, candidate-relative `{revision,path,sha256}`
record. Before measuring a row, the collector exports the row's exact
`source_revision` with `git archive`, verifies both the Git tree identity and a
normalized extracted-tree digest, and invokes that driver in a fresh macOS
default-deny sandbox. The driver receives only a
`pulp.gpu-first-visible-trace-producer-state-build-request.v1` naming the
attempt, state, baseline/candidate/source revisions, tree/archive digests,
fresh source/output directories, expected product digest, product identity,
tracing state, and driver digest. It does **not** receive the measured product
path. The sandbox denies the measured binary, original worktree, evidence
tree, ambient user/workspace build outputs, and network, while allowing the
exported source, fresh output, and declared system toolchain. A build that
needs another immutable input must declare and snapshot that input in the
closed contract; ambient cache or SDK discovery is not provenance.

The driver must return one
`pulp.gpu-first-visible-trace-producer-state-build-receipt.v1` with the same
closed identity/configuration, bounded builder and command, start/finish UTC,
relative product path and digest, and 1–16 exact toolchain
`{path,sha256,version}` records. The collector requires the independently
rebuilt executable bytes to equal the measured bytes, requires exactly one
`PULP_TRACING_COMPILED_IN__DO_NOT_SHIP` sentinel for each compiled-in row and
none for compile-out/baseline, and retains the source archive, build
driver/request/receipt, rebuilt product, stdout/stderr, and snapshotted
toolchain executables. Offline ratification rehashes and replays all of those
artifacts; a driver assertion without the independent bytes cannot pass.

Create a pinned session-config artifact under the evidence root with schema
`pulp.gpu-first-visible-trace-session-config.v1`, categories
`dsp,gpu,js,metadata,render,state`, `fill_policy=ring-buffer`, and
`ring_bytes=134217728`.
All requests reference its digest; only the active row declares that ring
active. Then collect the four states through the collector—never by invoking
the measurement driver directly:

```bash
: "${A3_EVIDENCE:?fresh absolute evidence root}"
: "${A3_CANDIDATE_ROOT:?clean exact final-candidate checkout}"
: "${A3_OVERHEAD_DRIVER:?candidate-relative reviewed measurement driver}"
: "${A3_OVERHEAD_BUILD_DRIVER:?candidate-relative reviewed state-build driver}"
: "${A3_PRECHANGE_BIN:?exact baseline product executable}"
: "${A3_COMPILE_OUT_BIN:?exact final-head compile-out executable}"
: "${A3_COMPILED_IN_BIN:?exact final-head compiled-in executable used idle and active}"
: "${TRACE_PROCESSOR:?pinned Perfetto v57.2 trace_processor_shell}"

for STATE in pre-change-baseline candidate-compile-out candidate-compiled-in-idle candidate-active; do
  case "$STATE" in
    pre-change-baseline) BIN="$A3_PRECHANGE_BIN" ;;
    candidate-compile-out) BIN="$A3_COMPILE_OUT_BIN" ;;
    *) BIN="$A3_COMPILED_IN_BIN" ;;
  esac
  python3 tools/scripts/gpu_first_visible_a3_trace_producer_overhead.py collect-state \
    --request "$A3_EVIDENCE/overhead/requests/$STATE.json" \
    --evidence-root "$A3_EVIDENCE" \
    --source-root "$A3_CANDIDATE_ROOT" \
    --binary "$BIN" \
    --driver "$A3_CANDIDATE_ROOT/$A3_OVERHEAD_DRIVER" \
    --build-driver "$A3_CANDIDATE_ROOT/$A3_OVERHEAD_BUILD_DRIVER" \
    --trace-processor "$TRACE_PROCESSOR" \
    --output "$A3_EVIDENCE/overhead/$STATE.json"
done
```

Production active traces must be binary Perfetto and replay only through the
exact Pulp v57.2 platform SHA pin. Chrome trace JSON is accepted solely by
planted fixture tests; the production CLI has no bypass. Replay requires
finished slices, zero loss/no-flush stats, one stable UPID/session challenge,
the challenged host PID, the health span, and all 20 b4ba signature counters.
Each sample must observe `gpu_acquire`, `gpu_submit`, and `gpu_present`; the
other 17 signatures are workload-conditional but remain counted. A zero
workload-conditional count is emitted as `reported-not-covered-not-zero-cost`,
never silently converted to zero overhead. All observed b4ba slices/counters
must belong to the challenged host process and zero may occur on declared
audio TIDs. The four-state receipt reports the full signature inventory,
unobserved disposition, aggregate input-to-first-visible overhead, and explicit
audio/xrun disposition.

After the source-bound product driver has written the four raw documents under
the evidence root, derive the only accepted receipt with:

```bash
python3 tools/scripts/gpu_first_visible_a3_trace_producer_overhead.py ratify \
  --pre-change-baseline "$A3_EVIDENCE/overhead/pre-change-baseline.json" \
  --candidate-compile-out "$A3_EVIDENCE/overhead/candidate-compile-out.json" \
  --candidate-compiled-in-idle "$A3_EVIDENCE/overhead/candidate-compiled-in-idle.json" \
  --candidate-active "$A3_EVIDENCE/overhead/candidate-active.json" \
  --evidence-root "$A3_EVIDENCE" \
  --trace-processor "$A3_EVIDENCE/tooling/trace_processor_shell" \
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
yet run; it does not waive them. Final verification also requires the selected
campaign/product/format/machine identity to match one validated A3 role and the
active binary digest to equal that campaign's measured product. A pinned
`--trace-processor` is required for binary Perfetto protobuf traces; strict
Chrome trace JSON is reserved for planted fixture controls.

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

### Exact B4 Vellum handoff

This handoff is executable only after A3 derives `queue-B4` or
`queue-B4-investigation`; it is not authority to change the current Pulp
renderer. The pinned Skia header makes
`skgpu::graphite::ContextOptions::fExecutor` a non-owning `SkExecutor*`, says
Graphite currently uses it for pipeline compilation, and requires the client
to keep it alive for the complete `Context` lifetime. `SkExecutor`'s FIFO/LIFO
factories provide a fixed worker count but do not themselves prove bounded
queue capacity or backpressure. Vellum must supply those bounds or reject the
candidate; fixed workers alone do not qualify as `background_bounded`.

The present Pulp compatibility seam is
`core/render/src/skia_surface.cpp::SkiaSurfaceImpl::init`, immediately before
`ContextFactory::MakeDawn`. Do not install generic executor policy there.
After adoption, the authority is Vellum's
`graphics/include/vellum/graphics/skia_dawn_surface.hpp` and
`graphics/src/skia_dawn_surface.mm`, with proof in
`graphics/tests/gpu_style_test.cpp` and
`graphics/tests/text_shaping_concurrency_test.cpp` (or their B0-verified
successors). A Vellum `Impl` owns a `std::unique_ptr<SkExecutor>` or equivalent
before the Graphite context member so reverse destruction drains/destroys
recorders and context first, releases the executor next, and releases borrowed
Dawn state last. Initialization must select either a null synchronous control
or one bounded executor; it must define worker count, queue capacity,
backpressure/rejection, cancellation, worker failure, partial-init rollback,
and queued-work shutdown, and must not add a second pipeline scheduler around
Graphite. No callback, queue operation, compilation, or teardown wait may run
on an audio thread.

The first experiment compares the null/synchronous control with that bounded
executor on the same exact Pulp/Vellum/Skia/Dawn build, provider asset, host,
trace session, product workload, and 10 cold plus 10 warm lifecycle rows. It
does not add signature prewarm first. Vellum owns these minimum events on one
documented monotonic-nanosecond clock:

| Event | Required correlation/payload |
|---|---|
| `vellum.gpu.context.create` | runtime/context IDs, provider release/SHA, backend, adapter, policy |
| `vellum.gpu.pipeline.request` | context/request IDs, stable pipeline-key hash, cache state |
| `vellum.gpu.pipeline.queue` | request ID, enqueue time, bounded depth/capacity |
| `vellum.gpu.pipeline.compile` | request ID, begin/end, outcome, worker thread |
| `vellum.gpu.prewarm` | session ID, begin/end, requested/ready/failed counts |
| `vellum.gpu.frame.paint` | surface/frame IDs, CPU begin/end |
| `vellum.gpu.frame.submit` | surface/frame IDs, recording/submit begin/end |
| `vellum.gpu.drawable.acquire` | surface/frame IDs, begin/end, status |
| `vellum.gpu.present` | surface/frame IDs, call time, result |
| `vellum.gpu.readback` | surface/frame IDs, begin/end, bytes, result |
| `vellum.gpu.executor.saturation` | context ID, depth/capacity, dropped/rejected count |

Every event/result binds process, runtime, context, surface, frame, trace
session, provider asset, build, and (where applicable) pipeline-request IDs.
Checked query output must carry the source-trace digest, provider release/SHA,
process UPID, trace-session identity, units, event counts, p50/p95/p99,
loss/flush disposition, and exact slice/counter evidence IDs. The saved queries
must answer which stage delayed first-visible readiness; which pipelines were
cold, warm, duplicated, failed, or late; how much time was queued, compiling,
or blocking render; whether the executor reduced latency or merely moved
contention; and whether trace drops or audio xruns occurred. Missing identity,
events, flush, or loss truth is `unverified`, not a zero-duration conclusion.

The current A3/Pulp comparison remains independently replayable. Its
`pulp_a3_trace_session` metadata must contain
`debug.gpu_evidence_id`, `debug.process_start_identity`,
`debug.executable_sha256`, `debug.session_config_sha256`,
`debug.audio_thread_tids_sha256`, `debug.collection_challenge_nonce`,
`debug.ring_bytes`, and `debug.session_active`. The same challenged host
UPID/session must contain `gpu_health_transition_first_visible` plus at least
one each of `gpu_acquire`, `gpu_submit`, and `gpu_present`, all carrying the
exact `debug.gpu_evidence_id`, with zero producer slices on declared audio TIDs,
zero `xrun*`/`deadline_miss*`, zero loss/no-flush stats, and no incomplete
slices. It must also report counts for the exact remaining 17 b4ba signatures
under active `js`, `render`, and `state` categories; unobserved signatures stay
explicitly not covered. B6 compares those saved A3 queries with the Vellum
result contract; renamed events need an explicit compatibility mapping, never
silent drift.

The B4 test matrix includes null/synchronous, compiled-in idle, and active
bounded-executor controls; saturation/backpressure, worker failure, partial
initialization, wrong instance/build/provider, queued shutdown, and a planted
executor-before-context lifetime failure; exact 10-cold/10-warm standalone,
real-DAW, and Forge campaigns; and CPU, memory, frame-miss/hitch, trace-overhead,
audio-thread, and xrun comparisons. `background_bounded` is accepted only when
the exact campaign proves a causal, material first-visible improvement while
every correctness, bounded-resource, trace-loss, and audio gate passes.
Otherwise leave `fExecutor` null and close `no-change` or
`cancelled-no-change`, retaining useful observability.

After an m153+ provider is adopted, `SkLogHandler` is a separate future generic
diagnostic producer in Vellum. Gate it on `GetInstance`/`SetInstance`, refuse to
replace an unknown foreign handler, and retain an installed handler for process
lifetime. Its callback may only copy bounded/truncated priority and message
data into a bounded queue—no JSON, filesystem/network, unbounded lock, heavy
formatting, or audio-thread work. A non-RT drain adds monotonic/process/thread,
provider/build, drop-count, and optional surface/context identity.

B6 deletes ownership duplication: Vellum retains the generic executor, log
handler, runtime trace schema/state, and checked queries; Pulp retains
product/host lifecycle selection, genuinely AppKit-specific
acquire/submit/present spans if still required, unified control/CLI/MCP
projection, and the standalone/REAPER/Forge campaign harnesses. Compare saved
queries first, then remove superseded Pulp-local generic producers, state
machines, and adapters. `core/render/src/skia_surface.cpp` becomes a thin
compatibility/adoption adapter or disappears when the Vellum surface owns that
seam. Deletion tests must reject duplicate global handlers, executors, runtime
policy, and generic events. Update Doxygen, this guide, status/support surfaces,
controls, capabilities, and mapped skills in the adoption package—not in the
initial A3 implementation.

`docs/status/gpu-vellum-handoff.yaml` is the closed v2 implementation ledger
for that future work. It pins the plan, Pulp, Vellum, issue, and comment
authorities; names B1 through B6 packages with exact existing/proposed path
objects; and records APIs, ownership/lifetime/RT rules, trace fields, tests,
performance gates, Pulp adoption, and terminal outcomes. Every current Pulp
path is explicitly retained or listed as one exact delete candidate. Horizon A
has no delete candidates: every `delete_paths` array is empty and every
deletion gate is default-deny. Prose, a future phase name, or a Vellum issue is
never deletion authority. B0 must refresh all pinned revisions and rerun the
closed validator before an adoption tranche can propose any exact deletion.

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

## Historical v1 terminal-evidence shape

A structurally complete v1 receipt used to bind the exact planning revision and digest, Pulp and
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
  digests. These checks preserve historical readability but never return a
  terminal v2 acceptance result.

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
