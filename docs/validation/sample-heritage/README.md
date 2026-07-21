# Sample Heritage calibration and listening runbook

This directory describes the evidence layer around Pulp's neutral Sample
Heritage profiles. The companion standard-library tool is
`tools/audio/heritage-calibration/heritage_calibration.py`.

The toolkit has five deliberately separate jobs:

1. verify capture provenance and content hashes;
2. reconcile the executed session against every mandatory plan row and role;
3. measure C1-C5 captures with independent time/frequency-domain analyzers;
4. prove cyclic and adaptive parameter recovery against narrow production
   renderers before using a hardware capture; and
5. produce deterministic, level-matched, blinded A/B listening packs.

It is not a second sampler or stretch implementation. Its independent Python
oracles recover known behavior from WAVs produced by narrow production-engine
renderers. The product engine's analytic, composition, realtime, latency, and
budget checks remain the G1-G3 C++ gates.

## Capture session manifest

A session is one directory containing `session.json` and its artifacts. Paths
must be normalized relative paths: absolute paths and `.`/`..` components are
rejected. SHA-256 values must be lowercase, and byte counts are mandatory; the
verifier rejects paths outside the session directory. Do not record a computer
name, hostname, workstation name, login name, or other development-host
identity. Those facts do not make a hardware measurement reproducible.

Target hardware and the measurement signal chain are different: factual
manufacturer/model/serial/revision data is useful provenance and is allowed.
Hardware manufacturer and product names are trademarks of their respective
owners and identify measured equipment only. No affiliation or endorsement is
implied.

Abbreviated structure (one linked row and role are shown; a readiness-complete
manifest contains every expanded row and required role):

```json
{
  "schema": "pulp.heritage.capture-session.v1",
  "session_id": "neutral-session-2026-07-a",
  "captured_at": "2026-07-19T20:00:00Z",
  "operator_id": "operator-a",
  "target": {
    "manufacturer": "Factual manufacturer name",
    "model": "Factual model name",
    "serial": "private-session serial or redacted-public-copy",
    "revision": "hardware/OS revision"
  },
  "conditions": {
    "psu_and_calibration_state": "warmed 30 minutes; calibration checked",
    "temperature_c": 22.5,
    "gain_staging": "stimulus, input, replay, and capture levels"
  },
  "capture_chain": [
    {"stage": "generator", "description": "interface output 1, 48 kHz"},
    {"stage": "target", "description": "line input; normalized replay"},
    {"stage": "capture_converter", "description": "interface input 1, 48 kHz float"}
  ],
  "capture_plan": {
    "schema": "pulp.heritage.capture-plan.v2",
    "sha256": "SHA-256 of the exact capture-plan.json used for the session"
  },
  "rows": [
    {
      "row_id": "c1-normalized-replay-pair",
      "protocol_id": "C1",
      "parameters": {"operation": "normalized-replay-pair"}
    }
  ],
  "artifacts": [
    {
      "path": "wav/c1-recorded-replay.wav",
      "sha256": "64 lowercase hexadecimal characters",
      "bytes": 123456,
      "test_id": "C1-normalized-replay-pair",
      "row_id": "c1-normalized-replay-pair",
      "role": "recorded_replay"
    }
  ],
  "trademark_notice": "Hardware manufacturer and product names are trademarks of their respective owners and identify measured equipment only. No affiliation or endorsement is implied."
}
```

Record enough detail in `capture_chain` for another operator with the same
target model to repeat the measurement. Add session-specific fields when
useful, but keep the required fields and artifact hashes intact. A private
evidence repository may retain serial numbers, operator mapping, raw notes,
stimulus scripts, and unredacted manifests; a public derivative can redact
those values while retaining content hashes.

Verify before analysis and again after copying or archiving:

```bash
python3 tools/audio/heritage-calibration/heritage_calibration.py \
  capture-verify evidence/session.json
```

Then prove session readiness against the exact plan:

```bash
python3 tools/audio/heritage-calibration/heritage_calibration.py \
  capture-ready evidence/session.json \
  --plan evidence/capture-plan.json
```

### Session protocol

[`capture-plan.json`](capture-plan.json) is the machine-readable session script.
Copy it into the private session directory and bind its exact SHA-256 in the
session manifest. Each protocol declares parameter axes, valid row variants,
mandatory selectors, and the artifact roles needed for an executed row. The
readiness audit expands those declarations rather than interpreting prose. It
rejects missing mandatory rows, missing roles, duplicate coverage, orphaned
artifacts, rows outside the declared axes, and a plan-hash mismatch.

With no target-applicability declarations, the checked-in plan expands to 575
mandatory rows. Each applicable C1-C5 or C7 protocol requires its whole
declared matrix. C6 requires the literal union of every impulse-train
row and every 100-percent analytic-stimulus row, including all declared
adaptive quality/width combinations. The licensed break and vocal C6 rows are
always optional; they are listening/calibration material, not analytic core.
Other valid C6 rows may be pruned, but a mandatory selector may not. A manifest
row has a session-local `row_id`, an exact `protocol_id`, and an exact
`parameters` object; every artifact links back with the same `row_id` and one
of the plan's required roles.

A target session should first prune the protocol set to the questions in that
target's research note. The canonical plan gives every protocol an evidence-
bound applicability capability; it never permits arbitrary row-by-row skips.
Bind each negative capability or research-applicability finding to the same
canonical plan and to a hashed evidence file:

```json
{
  "target_applicability": {
    "schema": "pulp.heritage.target-applicability.v1",
    "capture_plan_sha256": "same lowercase SHA-256 used by capture_plan",
    "declarations": [
      {
        "capability": "offline-stretch",
        "supported": false,
        "evidence_path": "research/offline-stretch-applicability.json",
        "evidence_sha256": "lowercase SHA-256 of that evidence file"
      }
    ]
  }
}
```

The referenced evidence file is itself validated rather than treated as an
opaque attachment:

```json
{
  "schema": "pulp.heritage.applicability-evidence.v1",
  "capture_plan_sha256": "same lowercase SHA-256 used by capture_plan",
  "capability": "offline-stretch",
  "session_id": "same session_id used by the capture manifest",
  "target": {
    "manufacturer": "target manufacturer",
    "model": "target model",
    "serial": "target serial",
    "revision": "target revision"
  },
  "conclusion": "not-applicable",
  "finding": "The target has no offline stretch function.",
  "sources": ["research note and primary-source section supporting the finding"]
}
```

The readiness audit accepts an omission only when the canonical plan explicitly
allows that capability to be absent and the evidence path/hash verifies. The
evidence should cite the target research note and the primary source or operator
check that makes the protocol inapplicable. With no declaration, the full
canonical matrix remains required.

The seven protocol families are:

- C1 records and digitally loads the same stimulus, then replays both, so the
  null isolates the record path rather than mislabeling it as playback color.
- C2 renders fixed tones and sweeps at -24/-12/0/+12 semitones to identify the
  pitch family and clock/filter geometry.
- C3 repeats at reference and -20 dB, then gain-matches, to expose
  level-dependent coding.
- C4 captures digital silence with a voice active and true idle separately.
- C5 captures impulse and step responses at every C2 transpose.
- C6 covers the cyclic/adaptive stretch grid. Impulse-train and 100-percent
  rows are mandatory; licensed break/vocal material is optional calibration
  and listening evidence.
- C7 captures licensed kicks, snares, and breaks across C2 transposes for
  transient-dependent listening.

For each row, retain the exact generated stimulus, target settings, raw
capture, normalized derivative (if any), and a short session log as separate
hashed artifacts. The toolkit does not automate hardware control because the
supported transport and storage interfaces vary; the declarative plan is the
portable script and the manifest records the executed result.

## Independent C1-C5 and adaptive-C6 analysis

The standard-library analyzer reads WAVs named by a path-contained request and
writes deterministic JSON. It does not share DSP code with the sampler and does
not assign a machine identity. Its neutral measurements are:

- C1: least-squares gain-matched record-vs-load null, residual, and correlation;
- C2: fixed-tone measurements plus a start-to-end short-time FFT trace of every
  swept-sine row, including unfolded/folded predictions and per-window error;
- C3: declared-level normalization followed by a gain-matched null;
- C4: active/idle RMS, peak, DC, dominant component, centroid, and spectral tilt;
- C5: impulse peak/energy centroid and step final value/overshoot/transition; and
- adaptive C6: variable splice positions and widths, decoded source anchors,
  and aggregate factor from the same index-coded probe used by the cyclic
  bootstrap.

Create `analysis.json` next to its WAV inputs:

```json
{
  "schema": "pulp.heritage.analysis-request.v1",
  "analyses": [
    {
      "id": "c1-record-null",
      "protocol": "C1",
      "inputs": {
        "recorded_replay": "wav/c1-recorded-replay.wav",
        "loaded_replay": "wav/c1-loaded-replay.wav"
      }
    },
    {
      "id": "c2-transpose-matrix",
      "protocol": "C2",
      "inputs": {
        "captures": [
          {"transpose_semitones": -24, "stimulus": "fixed-period-tone", "stimulus_path": "wav/c2-tone-source.wav", "path": "wav/c2-tone-m24.wav"},
          {"transpose_semitones": -12, "stimulus": "fixed-period-tone", "stimulus_path": "wav/c2-tone-source.wav", "path": "wav/c2-tone-m12.wav"},
          {"transpose_semitones": 0, "stimulus": "fixed-period-tone", "stimulus_path": "wav/c2-tone-source.wav", "path": "wav/c2-tone-0.wav"},
          {"transpose_semitones": 12, "stimulus": "fixed-period-tone", "stimulus_path": "wav/c2-tone-source.wav", "path": "wav/c2-tone-p12.wav"}
        ]
      }
    },
    {
      "id": "c3-level-law",
      "protocol": "C3",
      "level_difference_db": -20,
      "inputs": {"reference_level": "wav/c3-0.wav", "lower_level": "wav/c3-m20.wav"}
    },
    {
      "id": "c4-noise",
      "protocol": "C4",
      "inputs": {"active": "wav/c4-active.wav", "idle": "wav/c4-idle.wav"}
    },
    {
      "id": "c5-responses",
      "protocol": "C5",
      "inputs": {
        "captures": [
          {"transpose_semitones": 0, "stimulus": "unit-impulse", "path": "wav/c5-impulse-0.wav"},
          {"transpose_semitones": 0, "stimulus": "unit-step", "path": "wav/c5-step-0.wav"}
        ]
      }
    },
    {
      "id": "c6-adaptive-probe",
      "protocol": "C6-adaptive",
      "inputs": {
        "source": "wav/c6-indexed-source.wav",
        "captures": [
          {
            "path": "wav/c6-adaptive-capture.wav",
            "factor_percent": 200,
            "cycle_ms": "auto",
            "adaptive_quality": 50,
            "adaptive_width": 25,
            "decision_hop_samples": 2048,
            "search_radius_samples": 256,
            "search_stride_samples": 1,
            "crossfade_samples": 128,
            "splice_shape": "equal_power_overlap_add"
          }
        ]
      }
    }
  ]
}
```

Run it and archive both the request and report beside the session evidence:

```bash
python3 tools/audio/heritage-calibration/heritage_calibration.py \
  analyze evidence/analysis.json > evidence/analysis-report.json
```

The report binds the canonical request and every input WAV by SHA-256, so a
later rerun can distinguish a changed capture from a changed analysis result.
Adaptive analysis fails rather than emits a successful report when recovered
factor, hop, or explicitly declared crossfade behavior misses tolerance.

The C1-C5 unit fixtures exercise known gain, full-sweep frequency evolution,
level, noise, impulse, and step behavior, including a deliberately reversed
sweep that the C2 metric must reject. A separate product bootstrap runs before
those fixtures in the focused CTest. It obtains all positive evidence from
Pulp's production record-commit, pitch, voice-converter, bus-noise, and
hold/droop APIs, then makes the independent Python analyzers recover the
declared parameters. Swapped inputs and deliberately wrong laws or declarations
provide a negative control for every protocol. The product bootstraps below
prove both stretch paths independently. Ordinary program material is useful after
bootstrap, but it cannot replace the indexed probe for exact source-anchor
recovery.

Run the C1-C5 product bootstrap directly when changing an analyzer:

```bash
tools/ci/governed-build.sh cmake --build build \
  --target pulp-heritage-c1-c5-calibration-render
python3 tools/audio/heritage-calibration/heritage_calibration.py \
  c1-c5-bootstrap --out /tmp/heritage-c1-c5-bootstrap \
  --renderer build/test/pulp-heritage-c1-c5-calibration-render
```

The resulting `report.json` binds the renderer and ten float-WAV artifacts by
SHA-256. It records the declared-versus-recovered tolerances and the rejection
result for each negative control. This bootstrap validates the calibration
oracles; it is not capture evidence for a named hardware profile.

## Cyclic pseudo-hardware bootstrap

The bootstrap creates an index-coded unit-impulse basis and renders the declared
cyclic snap law. In this oracle, `cycle_frames` is always the output-domain snap
period; a nonzero splice width does not shorten that period. It independently
recovers:

- cycle spacing from the recurring splice-boundary anomalies;
- splice width from the anomaly run length;
- source anchors from the impulse coefficients; and
- stretch factor from the source-anchor slope.

The recovered parameters must then reproduce a separate sparse, signed impulse
train exactly. That holdout keeps the identification probe from grading only
itself. The bootstrap also runs an intentional negative control using the wrong
input-domain cycle law. A report passes only when the requested parameters and
sparse impulse positions/splices are recovered and that wrong law is rejected.

Use a non-unity factor and a splice of at least two frames. Factor 1 is
transparent, while zero- and one-frame splices have the same sampled behavior;
neither case contains enough information to identify the requested law.

```bash
python3 tools/audio/heritage-calibration/heritage_calibration.py \
  cyclic-bootstrap --out /tmp/heritage-bootstrap \
  --renderer build/test/pulp-heritage-cyclic-calibration-render \
  --factor 1.75 --cycle-frames 64 --splice-frames 8
```

Build only that focused renderer when it is not already present:

```bash
tools/ci/governed-build.sh cmake --build build \
  --target pulp-heritage-cyclic-calibration-render
```

The indexed probe and sparse impulse holdout are rendered by Pulp's production
`SampleHeritageLiveCyclicStretch`; the Python recovery/oracle does not render
the positive evidence. `report.json` records the renderer hash and the four
hashed float-WAV fixtures. This is the required calibration-pipeline check
before pointing the same kind of oracle
at real captures. Synthetic inputs are sufficient for this bootstrap; hardware
captures remain optional calibration evidence and do not block the neutral SDK.

## Adaptive product bootstrap

The adaptive bootstrap invokes only the production record-commit API. Its
renderer creates an indexed source, applies the typed adaptive block, and writes
the committed result. The independent Python oracle then recovers output hops,
splice anomaly extent, source anchors, and factor and compares them with the
declared neutral settings. A mismatch exits nonzero; Python never manufactures
the positive product evidence.

```bash
tools/ci/governed-build.sh cmake --build build \
  --target pulp-heritage-adaptive-calibration-render
python3 tools/audio/heritage-calibration/heritage_calibration.py \
  adaptive-bootstrap --out /tmp/heritage-adaptive-bootstrap \
  --renderer build/test/pulp-heritage-adaptive-calibration-render \
  --factor 1.75 --decision-hop 64 --search-radius 8 \
  --search-stride 1 --crossfade 8 --source-frames 8192
```

`report.json` binds both WAVs and the renderer by SHA-256 and records the exact
declared-versus-recovered comparison. The renderer identifies its production
splice as `equal_power_overlap_add`; a hardware request must declare a splice
shape only when it has an evidence-backed frame interpretation. Opaque hardware
quality/width labels remain recorded settings and are not silently converted to
frame counts.

## Keyed blinded listening packs

Create a pair manifest next to the source WAVs:

```json
{
  "schema": "pulp.heritage.listening-pairs.v1",
  "pairs": [
    {
      "pair_id": "stretched-break-recipe-01",
      "reference": "reference.wav",
      "candidate": "candidate.wav"
    }
  ]
}
```

Make a random key once and keep it outside the listener-facing pack:

```bash
umask 077
openssl rand 32 > listening.key
python3 tools/audio/heritage-calibration/heritage_calibration.py listening-pack \
  --pairs pairs.json --out listening-pack \
  --answers-out private/answers.json --key-file listening.key
```

The candidate is RMS-matched to the reference. When either peak would exceed
-0.18 dBFS, a common gain is then applied to both sides. Both are rewritten as
deterministic float32 WAVs, and HMAC-SHA256 assigns A/B order and opaque
filenames. `listening-pack/manifest.json` contains only blind labels, hashes,
format facts, and a key identifier. The key and answer roles are not embedded
there, nor is there a public answer hash that could be brute-forced. The private
answer file is authenticated with the key and binds the exact packed WAV hash
to each role. Keep `private/answers.json` and `listening.key` away from listeners
until responses are frozen.

Each pair must already have matching sample rate, channel count, frame count,
and intended time alignment. The packer refuses mismatches instead of silently
trimming, resampling, or aligning evidence. Write into an empty output directory
so an obsolete file cannot leak an answer or be mistaken for part of the pack.

Reusing the same pair IDs and key in a new empty output directory reproduces the
same order and bytes. A new key creates a new randomization. Verify hashes,
authenticated role bindings, keyed order, the 0.01 dB RMS matching bound, and
the peak ceiling before listening:

```bash
python3 tools/audio/heritage-calibration/heritage_calibration.py listening-verify \
  --manifest listening-pack/manifest.json \
  --answers private/answers.json --key-file listening.key
```

Listeners should record `A`, `B`, or `no preference`, plus a short reason,
before unblinding. For the neutral SDK, G4 gates the reproducible keyed pack and
verification mechanism. Human judgment gates publication of a particular
calibrated recipe, not the neutral mechanism SDK, and remains a judgment of
whether that recipe is convincing rather than a claim of exact identity to one
serial-numbered unit.

## Focused self-test

```bash
cd tools/audio/heritage-calibration
python3 -m unittest test_heritage_calibration
```

When configured through CMake, the same test is registered as
`heritage-calibration-toolkit`.
