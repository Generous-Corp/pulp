# Sample Heritage calibration and listening runbook

This directory describes the evidence layer around Pulp's neutral Sample
Heritage profiles. The companion standard-library tool is
`tools/audio/heritage-calibration/heritage_calibration.py`.

The toolkit has three deliberately separate jobs:

1. verify capture provenance and content hashes;
2. prove the cyclic-parameter recovery pipeline against analytic pseudo
   hardware before using a hardware capture; and
3. produce deterministic, level-matched, blinded A/B listening packs.

It is not a second sampler or stretch implementation. Its pseudo-hardware test
only proves that the calibration pipeline can recover a known splice law. The
product engine's analytic, composition, realtime, latency, and budget checks
remain the G1-G3 C++ gates.

## Capture session manifest

A session is one directory containing `session.json` and its artifacts. Paths
are relative to the manifest. SHA-256 and byte counts are mandatory; the
verifier rejects paths outside the session directory. Do not record a computer
name, hostname, workstation name, login name, or other development-host
identity. Those facts do not make a hardware measurement reproducible.

Target hardware and the measurement signal chain are different: factual
manufacturer/model/serial/revision data is useful provenance and is allowed.
Hardware manufacturer and product names are trademarks of their respective
owners and identify measured equipment only. No affiliation or endorsement is
implied.

Minimal schema:

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
  "artifacts": [
    {
      "path": "wav/c6-impulse-factor-175.wav",
      "sha256": "64 lowercase hexadecimal characters",
      "bytes": 123456,
      "test_id": "C6-impulse-factor-175",
      "role": "capture"
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

### Session protocol

[`capture-plan.json`](capture-plan.json) is the machine-readable session script.
Copy it into the private session directory, prune optional grid rows to the
target's supported features, and preserve every `required_rows` entry. Use the
protocol ID plus stable row parameters as each artifact's `test_id`.

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

## Cyclic pseudo-hardware bootstrap

The bootstrap creates an index-coded unit-impulse basis, renders the declared
cyclic snap law, and independently recovers:

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
before unblinding. G4 remains a human judgment of whether a neutral recipe is
convincing, not a claim of exact identity to one serial-numbered unit.

## Focused self-test

```bash
cd tools/audio/heritage-calibration
python3 -m unittest test_heritage_calibration
```

When configured through CMake, the same test is registered as
`heritage-calibration-toolkit`.
