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

Timing observations never encode an unavailable boundary as zero. Each field
records `value_ns`, availability, clock domain, observer, API source, and
whether the value is direct, correlated, or inferred. Dawn GPU-start admission
and other boundaries that cannot be observed honestly remain unavailable.

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

An overload campaign must use `row.load=overload`, declare a watchdog greater
than its deadline, keep the verdict timings observable, and exercise at least
one typed miss, watchdog, late/resync, non-completed terminal, or fallback
disposition. A mislabeled quiet campaign cannot bypass the normal-load gates,
and a no-op overload run is invalid evidence.
