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

The rollup includes confirmed runs and a "scripted lanes without checked-in
manifests" backlog derived from the manual scripts in
`docs/validation/daw-bench/`. That backlog is not support evidence; it is the
remaining host-lab work queue. Use `--format json` when a CI job or dashboard
needs a machine-readable artifact. The JSON report includes
`scripted_lane_count`, `covered_scripted_lane_count`, and
`missing_scripted_lane_count` so CI can track host-lab coverage without
scraping markdown.

For local planning only, add `--include-local-host-availability` to annotate
missing scripted lanes with whether this Mac appears to have that DAW installed
under `/Applications`:

```bash
python3 tools/scripts/summarize_daw_bench_results.py \
    docs/validation/daw-bench/results \
    --require-any \
    --include-local-host-availability
```

If a DAW is installed outside `/Applications`, add one or more explicit
overrides instead of leaving the lane marked unavailable:

```bash
python3 tools/scripts/summarize_daw_bench_results.py \
    docs/validation/daw-bench/results \
    --require-any \
    --include-local-host-availability \
    --host-app "Ableton Live=/Volumes/Audio Apps/Ableton Live 12 Suite.app" \
    --host-app "Studio One=/Volumes/Audio Apps/Studio One 6.app"
```

Those annotations are scheduling hints, not validation evidence. They do not
change `covered_scripted_lane_count`, and they do not close any missing lane.

Use the stricter completion gate only when a branch is claiming that the
scripted host-lab backlog is closed:

```bash
python3 tools/scripts/summarize_daw_bench_results.py \
    docs/validation/daw-bench/results \
    --require-any \
    --require-complete-scripted-lanes
```

That command fails while any manual DAW-bench script lacks a matching
checked-in manifest. A failing result is expected on branches that only add
partial coverage.

Large DAW logs may stay outside the repo, but the manifest must include an
`external_log_url` when `logs` is empty. Do not use placeholders; unverified
rows should be recorded as `Not Triggered` or left out of the promotion PR.
For known log-backed quirk flags, the validator also cross-checks the observed
status against the checked-in log events. A `Confirmed` row must include the
expected event in at least one listed log, and a `Not Triggered` row must not
contradict the listed logs.

AU preflight output can be captured separately when debugging scan/discovery
failures:

```bash
python3 tools/scripts/check_au_component_preflight.py \
    ~/Library/Audio/Plug-Ins/Components/PulpHostBench.component \
    --expect-type aumf \
    --expect-subtype PHBn \
    --expect-manufacturer Pulp \
    --expect-factory PulpHostBenchAUFactory \
    --expect-symbol PulpHostBenchAUFactory \
    --check-permissions \
    --check-codesign \
    --check-gatekeeper \
    --check-auval-list \
    --run-auval \
    --auval-repeat 2 \
    --format json
```

That JSON is diagnostic context only. It proves package/discovery preflight
state, not DAW host behavior; a missing Logic AU result still needs a real
filled-in script, log, and `.daw-bench.json` manifest before it counts as
host-lab evidence. When a DAW-bench manifest cites preflight JSON via
`preflight_reports`, the validator only checks that the file exists and has the
expected `check_au_component_preflight.py --format json` shape. The reported
preflight may pass or fail because both outcomes are useful diagnostics.
When `check-auval-list` fails, the diagnostic also summarizes whether `auval -a`
listed any non-Apple components. A result that lists only Apple components means
the local AU registrar is not exposing third-party components, so a missing
Logic AU lane should stay open until that machine-level condition is fixed and
stable repeated `auval` passes are captured.
When Gatekeeper reports `Adhoc Signed App` or `Notary Ticket Missing`, rerun the
preflight with `--check-signing-identity "Developer ID Application: ..."` before
attempting a notarized replacement install; that check signs a temporary copy
and catches keychain private-key access failures without mutating the installed
component.

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
  "plugin_version": "1.0.0",
  "result_markdown": "06-reaper-vst3.md",
  "logs": ["logs/Reaper-VST3-20260612T120000Z-pid42.log"],
  "preflight_reports": ["preflight/logic-au-preflight.json"],
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
logs are present. When checked-in logs include `plugin_version=` or
`pulp_bench_plugin=` fields, the validator also checks them against the
manifest's `plugin_version`.

`preflight_reports` is optional diagnostic context. Each entry must reference a
checked-in JSON file emitted by `check_au_component_preflight.py --format json`.
Preflight reports do not count as logs and do not close any missing scripted
lane in the compatibility rollup.
