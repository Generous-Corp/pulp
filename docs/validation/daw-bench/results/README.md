# DAW Bench Results Evidence

This folder holds checked-in DAW-bench result records. Keep one dated folder per
bench session:

```text
docs/validation/daw-bench/results/2026-06-12/
  06-reaper-vst3.md
  reaper-vst3.daw-bench.json
  logs/
    Reaper-VST3-20260612T120000Z-pid42.log
```

The markdown file is the filled-in manual script. The `.daw-bench.json` file is
the machine-readable evidence manifest used by reviewers, agents, and host-quirk
promotion tooling to tell whether the result is complete enough to cite.

Validate a dated folder before using it as roadmap or tier-promotion evidence:

```bash
python3 tools/scripts/check_daw_bench_evidence.py \
    docs/validation/daw-bench/results/2026-06-12 \
    --require-any
```

Generate the recurring compatibility rollup after validation:

```bash
python3 tools/scripts/summarize_daw_bench_results.py \
    docs/validation/daw-bench/results \
    --require-any
```

Use `--format json` when a CI job or dashboard needs a machine-readable
artifact.

Large DAW logs may stay outside the repo, but the manifest must include an
`external_log_url` when `logs` is empty. Do not use placeholders; unverified
rows should be recorded as `Not Triggered` or left out of the promotion PR.
For known log-backed quirk flags, the validator also cross-checks the observed
status against the checked-in log events. A `Confirmed` row must include the
expected event in at least one listed log, and a `Not Triggered` row must not
contradict the listed logs.

## Manifest Schema

```json
{
  "schema_version": 1,
  "host": "REAPER",
  "format": "VST3",
  "daw_version": "7.16",
  "os": "macOS 15.5",
  "date": "2026-06-12",
  "script": "06-reaper-vst3.md",
  "pulp_commit": "33dc6cfd1f1f",
  "plugin_version": "0.395.0",
  "result_markdown": "06-reaper-vst3.md",
  "logs": ["logs/Reaper-VST3-20260612T120000Z-pid42.log"],
  "capabilities": [
    {
      "capability": "load",
      "observed": "Confirmed",
      "notes": "session_start and prepare events appeared in the checked-in log"
    }
  ],
  "quirks": [
    {
      "flag": "reaper_process_while_bypassed",
      "row": "R2",
      "observed": "Confirmed",
      "notes": "process_without_prepare appeared after bypass toggle"
    }
  ]
}
```

Allowed `format` values: `AU`, `AUv3`, `CLAP`, `Standalone`, `VST3`.

Allowed `quirks[].observed` values: `Confirmed`, `Refuted`, `Not Triggered`.

`capabilities` is optional for historical manifests but required for any
host-matrix cell promotion. Use lowercase capability identifiers matching the
matrix column names after normalization, such as `load`, `params`, `midi`,
`sidechain`, `multi-bus`, and `ara`. A promoted matrix cell must have a matching
manifest for the host/format lane and a `Confirmed` capability entry. The
validator cross-checks known capabilities against checked-in log events when
logs are present.
