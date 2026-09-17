# GPU-audio P4 evidence files

P4 performance campaigns write `pulp.gpu-audio.p4.raw.v1` JSONL. The format
keeps every measured block and makes incomplete trials invalid. It is an
evidence format, not a public SDK API.

The first record identifies the exact source and benchmark binary, Release
flags, machine, macOS version, adapter backend/registry/vendor/device identity,
Dawn revision and asset digest, campaign kind, and one matrix row. Complete
`trial_begin` / `block` / `trial_end` groups follow. Trials
alternate the staged-async and shared-async paths after warmup, and `pair_id`
binds the two independent trials used for confidence estimation. A trial end
records the block count and SHA-256 of its canonical block records, so a killed
or partially copied run cannot be interpreted as a shorter successful run.

Each block carries `(engine_id, generation, sequence)`, the GPU terminal and
delivery dispositions, deadline/fallback state, and the named payload-transfer
counters. Shared records with any `WriteBuffer`, output-copy, `MapAsync`, or
mapped-readback memcpy count are invalid.
Every trial end supplies closed-world `gpu_terminal_counts` and `delivery_counts`
that must exactly equal its blocks. A `device_lost` block requires the trial's
`device_loss` flag. GPU delivery requires completed work without a deadline miss,
watchdog expiry, or resync drop. Each normal-load async trial must contain eligible
GPU delivery; all-priming or all-fallback trials cannot qualify. Engine and
generation stay constant within a trial, and identity integers cannot be booleans.

Timing observations never encode an unavailable boundary as zero. Each field
records `value_ns`, availability, clock domain, observer, API source, and
whether the value is direct, correlated, or inferred. Dawn GPU-start admission
and other boundaries that cannot be observed honestly remain unavailable.
Each observation also requires `start_clock_domain`, `end_clock_domain`,
`start_observer`, `end_observer`, `callback_mode`, `event_pump_strategy`,
`timestamp_scope`, `correlation_method`, `uncertainty_ns`,
`instrumentation_overhead_ns`, and `instrumentation_control`. Direct intervals
require matching endpoint clocks and observers with correlation method
`not_required`; correlated intervals require an explicit calibration method.
Available uncertainty and overhead are finite nonnegative nanoseconds; unavailable
values are null. Provenance, including declared uncertainty/overhead bounds, must
remain constant for each metric within each path. Inferred observations remain in
raw/CSV screening data but cannot supply verdict statistics. Confirmation,
default, and overload campaigns require direct or correlated verdict timings.
This validates the declaration, not the correctness of the physical calibration
or instrumentation control, which must accompany physical results.

Total CPU per block sums mutually exclusive `callback_cpu`, `worker_pack_copy`,
`encode_cpu`, `submit_cpu`, `event_processing_cpu`, `retirement_cpu`, and
`worker_other_cpu` spans. The callback span includes fallback work; other worker
CPU covers all service outside the separately named spans. Event pumping and
terminal collection must not disappear from the total or be counted twice.

Validate and summarize a capture with:

```bash
python3 tools/scripts/gpu_audio_p4_evidence.py raw.jsonl \
  --benchmark-binary ./pulp-gpu-audio-p4-benchmark \
  --summary summary.json --csv blocks.csv
```

The summary retains the complete machine, provider, binary, build, and timing
provenance together with p50, p95, p99, p99.9, maximum, sample counts, miss
counts, total duration per path, and deterministic 95% bootstrap intervals over
paired-trial improvements in total CPU per block and submit-to-completion p99 in
both percent and absolute nanoseconds. The CSV contains typed block and
`trial_end` rows and preserves every timing observation's availability, clock,
observer, API source, relation, named transfer counters, UI/duration observations,
and terminal health flags.
Block percentiles use linear interpolation at `(sample_count - 1) * percentile`.
Bootstrap samples resample complete matched-trial improvements with replacement,
then take the 2.5th/97.5th percentiles of their means. The seed and resample count
are retained. These intervals describe trial variation; they do not absorb
systematic calibration or instrumentation error. UI summary percentiles describe the distribution of trial-level
UI p99 values, not pooled raw frames. The UI gate instead requires every complete
matched pair's shared UI p99 to stay within 10% of its staged value; one slow
staged trial cannot conceal regressions in the other pairs. Raw trial UI p99
values remain available in the CSV for plots.
Both detached outputs carry the SHA-256 of the exact raw JSONL bytes. The CLI
hashes the file before and after loading and refuses a capture that changes
during analysis. A default long-tail manifest also names the confirmation
campaign and exact confirmation-summary SHA-256 it extends; the later matrix
aggregator must verify that binding before assigning a program verdict.

`row_gate` mechanically applies the gates that one matched row can prove:
closed-world transfer elimination, complete paired metrics, a confidence-backed
20% CPU or completion-tail win, the non-winning metric limit, the 25 us absolute
condition for a latency win, health/fallback/resync comparisons, callback p99,
UI p99, and the default long-tail requirement when applicable. It deliberately
leaves `program_verdict` and the top-level `verdict` as `unassigned`. A single
row cannot authorize `GO_AUTO`, `GO_EXPLICIT`, or `KILL`; that decision requires
the complete declared matrix plus the correctness, lifecycle, contention,
fallback, and default-profile evidence in the canonical plan.
The authoritative gates and required physical artifacts are in the
[canonical execution plan, sections 6, 7, and 13](https://github.com/danielraffel/pulp-planning/blob/b525edca0d22b4e9e7556bee4534c790fc18dbe7/2026-09-15-gpu-audio-shared-memory-execution-plan.md)
(private planning repository). This standalone contract supplies no product
benchmark implementation or physical performance result. Physical acceptance
also needs authenticated expected/observed provider and build attestations,
correctness evidence, and the complete matrix specified there.

The supplied benchmark binary must hash to the manifest identity. Screening
captures may use a small trial count. Confirmation captures require at least 30
complete matched pairs and 10,000 deterministic bootstrap resamples. The
separate default-profile long-tail capture may use one pair, but requires at
least 100,000 blocks per trial under concurrent UI/GPU load. This avoids turning
the 100,000-block gate into 30 redundant long runs. The structural checks do
not replace the zero-miss acceptance gate. Parsing, duplicate-identity checks,
and percentile aggregation use temporary SQLite spools, so validation does not
retain millions of JSON objects in memory. The raw JSONL remains the durable
source; the temporary databases are disposable analysis state.
Provenance tracking keeps one value plus a mismatch flag per declared path/metric,
and invalid captures retain at most 100 diagnostics plus a suppression notice.
Trial metadata and bootstrap samples scale with the declared trial/resample count;
the JSON parser still needs memory for the largest individual record.
The deterministic `gpu-audio-p4-evidence-selftest` CTest uses `audio;gpu;evidence`
labels so it runs in the required lane, whose timing-benchmark exclusions do not
apply to this schema/statistics check.

An overload campaign must use `row.load=overload`, declare a watchdog greater
than its deadline, keep the verdict timings observable, and exercise at least
one typed miss, watchdog, late/resync, non-completed terminal, or fallback
disposition. A mislabeled quiet campaign cannot bypass the normal-load gates,
and a no-op overload run is invalid evidence.
