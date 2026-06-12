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

Large DAW logs may stay outside the repo, but the manifest must include an
`external_log_url` when `logs` is empty. Do not use placeholders; unverified
rows should be recorded as `Not Triggered` or left out of the promotion PR.

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
