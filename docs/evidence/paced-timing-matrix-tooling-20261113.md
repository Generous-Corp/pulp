# Shared paced-timing matrix tooling (2026-11-13)

Status: **TOOLING ONLY**. This adds a runner that can make a quiet-host,
100,000-callback paced/deadline claim for several graph fixtures. It makes no
such claim itself: the only runs recorded here are short mode and planted
negatives on a loaded shared builder, and every one reports
`timing_qualified=false`.

## What it runs

`tools/graph_paced_timing_matrix.py` runs the same fixed-origin paced-callback
measurement over these fixtures. Each cell is one process that runs one hidden
Catch2 case:

| fixture | executable (build dir) | hidden case | variants | rates | frames |
|---|---|---|---|---|---|
| `g5-graph-event` | `test/pulp-test-group-trigger-graph` | `G5 graph event paced matrix cell` | direct, routed | 48000 | 64, 128, 256, 4096 |
| `g7-edge-probe` | `test/pulp-test-signal-graph-executor-parity` | `G7 edge probe paced matrix cell` | attached, detached | 48000 | 32, 64, 128, 256 |
| `g6-voice-bank` | `test/pulp-test-voice-bank-binding` | `G6 voice bank paced matrix cell` | eight-slot | 48000 | 32, 64, 128, 256 |
| `g10-routed-spectral` | `test/pulp-test-routed-spectral-ports` | `G10 routed spectral paced matrix cell` | identity-mask | 48000 | 64, 128, 256, 4096 |
| `g12-activation` | `test/pulp-test-group-trigger-graph` | `G12 paced activation receipt cell` (existing) | direct, routed | 44100, 48000, 96000 | 32, 64, 128, 256 |

Where each fixture comes from:

- **G-12** uses the existing receipt case and its `G12_ROW` format unchanged.
  Its audio and transition oracle is read through the typed validator in
  `graph_performance_qualification.parse_activation_output`.
- **G-5** already had a paced case, but it ran a fixed 128 callbacks and printed
  only summaries. Its producer/consumer graph-event chain is now built by
  `build_paced_graph()`, which the old case and the new matrix cell share.
- **G-7, G-6 and G-10** had no paced entry point. Each got a thin hidden case
  that reuses the existing fixture graph:
  - G-7: the named audio edge-probe gain chain from the executor-parity test.
    When attached, one concurrent reader drains the probe.
  - G-6: the authored eight-slot bank from `examples/g6-voice-bank-consumer`,
    with all eight slots held for the whole run.
  - G-10: the analysis → identity → real mask → synthesis graph. It is now
    built by `with_spectral_graph()`, which the existing render test also uses.

No production code changed. The shared harness is
`test/harness/paced_timing_cell.hpp`. It runs callback `i` on a dedicated
`std::thread`, targeting `origin + i*frames/rate` with integer arithmetic, and a
late callback never resets the origin. It times only the fixture's process
call, plus any planted sleep. The fixture's oracle runs after the end
timestamp. Rows go into preallocated storage and are printed after the last
callback. Timing cells carry no allocation or lock probes.

## Usage

```bash
# Build the four executables (governed, Release)
PULP_BUILD_JOBS=4 nice tools/ci/governed-build.sh cmake --build build --target \
  pulp-test-group-trigger-graph pulp-test-signal-graph-executor-parity \
  pulp-test-voice-bank-binding pulp-test-routed-spectral-ports

# CI/short mode: 32 callbacks per cell, all 48 cells, never qualifies
python3 tools/graph_paced_timing_matrix.py record --dry-run --output-dir <dir>

# Planted negative (sleep inside the timed window of every 4th callback)
python3 tools/graph_paced_timing_matrix.py record --mode short --callbacks 32 \
  --fixtures g5-graph-event g7-edge-probe g6-voice-bank g10-routed-spectral python-reference \
  --frames 128 4096 --planted-sleep-us 90000 --planted-every 4 --output-dir <dir>

# Recompute every verdict from the retained raw rows
python3 tools/graph_paced_timing_matrix.py validate <dir>/receipt.json

# Cells and expected paced wall time, without running anything
python3 tools/graph_paced_timing_matrix.py record --mode qualification --output-dir <dir> --plan
```

`python3 tools/graph_performance_qualification.py matrix ...` runs the same CLI.

## Receipt schema: `pulp.graph_paced_timing_matrix_receipt.v1`

One `receipt.json` is written per run, next to one raw stdout file per cell.

- `mode`: `short` or `qualification`. `status`: `COMPLETE` or `REFUSED`.
- `source`: `{sha, dirty}`. `build_profile`: `{CMAKE_BUILD_TYPE, PULP_SANITIZER}`.
- `host_identity_class`: OS, release, machine, CPU model, hardware model,
  logical CPUs and rounded memory in GiB. It never includes the hostname or a
  user name; the self-test checks that `platform.node()` does not appear.
- `admission`: `{rule, max_load_per_cpu, logical_cpus, before, after}`. Each
  sample is `{load[1,5,15], verdict, admitted, reason}`.
- `requested`: `{fixtures, callbacks, frames_filter}`.
  `planted`: `{sleep_us, every}`.
- `cells[]`: `{fixture, variant, rate, frames, callbacks, test_case, executable,
  executable_sha256, source_file, compiled_source, build_type, build_dirty,
  extras, returncode, raw_stdout{path, sha256}, metrics}`.
- `metrics`: `{period_ns, duration_ns{p50,p99,max}, wake_ns{p50,p99,max},
  deadline_misses, oracle_failures, thresholds_met}`. Percentiles are nearest
  rank and are computed in Python from the raw rows.
- `totals`, `disqualifiers[]`, `timing_qualified`, `timing_rule`,
  `receipt_sha256`. The digest covers the canonical JSON without its own field.

**Deadline miss.** A callback misses when `(end - target) * rate > frames *
1e9`, meaning it finished later than one block after its target. This is the
same rule as the G-12 lane.

**`timing_qualified` rule.** The full text is stored in every receipt.
`timing_qualified` is true only when all of these hold:

- mode is `qualification`, every qualifying fixture cell ran 100,000 callbacks,
  and no frames filter was used;
- admission passed both before and after the run;
- the build is Release with no sanitizer, and the clean source SHA equals every
  cell's compiled source;
- no fault was planted;
- every cell has zero deadline misses, zero oracle failures, a p99 callback
  duration under 70% of the block, and a p99 absolute wake under 0.1 sample.

The duration and wake thresholds are the G-12 thresholds.

**Admission.** The run is admitted when `os.getloadavg()[0] <= 0.25 × logical
CPUs` (`--max-load-per-cpu`). The existing G-12 `record-activation` lane has no
load admission at all. The nearest precedent, the GPU-audio host preflight
(`tools/scripts/gpu_audio_p2_host_preflight.py`), uses `0.5 × ncpu`. Paced
timing is more sensitive than that lane, so this threshold is stricter.
Qualification mode refuses before any cell runs: it writes a `REFUSED` receipt
and exits with code 3. A post-run sample above the threshold disqualifies the
run. Short mode only records the verdict.

`validate` re-hashes each raw file and re-parses it. It recomputes every
metric, the matrix completeness and the disqualifier list, and rejects any
mismatch.

## Results on this host (m3, loaded dedicated builder)

Source: base `origin/main` 250be0889974 plus this change, Release, GPU off.

Host class: Darwin 27.2.0, arm64, Apple M3 Ultra, Mac15,14, 28 logical CPUs,
96 GiB.

**Short mode** (`--dry-run`, 48 cells × 32 callbacks, about 14 s):

- `status=COMPLETE`, `timing_qualified=false`.
- Admission `refuse` before (load1 19.96 > 7) and after (17.81).
- 1,536 callbacks, 69 deadline misses, 0 oracle failures, 0/48 cells meeting
  the thresholds.
- p50 callback durations were 0.4–21 µs, plus about 128 µs for G-10 at 4096
  frames. p50 wake ranged from 5% to 36% of a block period (median about 32%). The
  misses come from wake latency on the loaded host, not from processing time.
- `validate` passed.

**Planted negative** (90 ms sleep on every 4th callback, 32 callbacks):

- At 4096 frames (85.3 ms block), every fixture recorded exactly **8 misses**,
  one per planted callback: G-5 direct and routed, G-10, and the Python
  reference. The unplanted short run recorded **0 misses** in the same cells.
- At 128 frames, every cell recorded 32/32 misses. A 90 ms overrun pushes
  every later callback in that 32-callback window past its deadline, because
  the origin is fixed.
- Oracle failures were 0 throughout, so the planted sleep changes timing and
  not audio.

**Self-test** (`tools/test_graph_paced_timing_matrix.py`, ctest
`graph-paced-timing-matrix-selftest`, about 6 s, no C++ build): covers the
receipt schema and validator, tamper and re-sign rejection, quiet-host refusal,
the miss-rule boundary at 1 ns, the planted negative with its clean control,
and a satisfiability check that an ideal receipt qualifies while each single
defect disqualifies it. Each test was broken once to confirm it can fail:

- forcing the miss counter to 0 fails both miss tests;
- removing the refusal branch fails the refusal test.

`PULP_PACED_MATRIX_BUILD_DIR=build` also runs the planted negative through the
compiled G-5 fixture; it passed here.

## What this can and cannot prove

It can prove:

- for the named fixtures on one host, at this exact source and binary:
  per-callback duration and wake distributions, deadline misses against a fixed
  origin, and an every-block output oracle under paced callbacks;
- that the miss counter sees a planted overrun.

It cannot prove:

- **WCET**;
- behaviour on a real audio device or DAW callback thread;
- resource safety (allocation, lock and I/O probes are separate fixtures);
- other hosts or platforms;
- that the G-7 probe reader matches a real UI client.

The harness thread runs at default QoS and never requests a real-time or
time-constraint scheduling policy. On this loaded host the p50 wake reached
about a third of a block. Unless the quiet host wakes far more tightly, the
inherited **p99 wake < 0.1 sample** criterion (about 2 µs at 48 kHz) will fail
every cell on that wake term alone. Decide whether the harness should request a
time-constraint thread, or whether the wake criterion should change, before
spending the full run.

## Command for the quiet-host job

On a quiet host, at an exact clean head, after the governed Release build of
the four targets above:

```bash
# 1. Pilot (about 3 min of paced schedule): confirms wake behaviour on the quiet host. Never qualifies.
python3 tools/graph_paced_timing_matrix.py record --mode short --callbacks 512 \
  --output-dir docs/evidence/<date>-paced-matrix-pilot

# 2. Full qualification: 48 cells × 100,000 callbacks, about 10.2 h of paced
#    schedule (about 8.7 h without g12-activation). Refuses on a loaded host.
python3 tools/graph_paced_timing_matrix.py record --mode qualification \
  --build-dir build --output-dir <durable-dir>/paced-matrix-qualification
python3 tools/graph_paced_timing_matrix.py validate <durable-dir>/paced-matrix-qualification/receipt.json
```

Raw outputs total roughly 0.5 GB, mostly the G-12 per-sample audio rows. Keep
them outside the repository and retain only the receipt and a hash manifest.
