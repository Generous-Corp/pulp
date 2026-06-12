# Host Compatibility Matrix

Tracks which DAWs Pulp plug-ins have been validated against, per format and
capability. Maintained alongside `planning/production-readiness/STATUS.md` —
when an entry moves from `unvalidated` to `stable` here, the corresponding
row in STATUS can claim `production_validated: true`.

The point of the matrix is to prevent optimism drift: "it compiled" ≠
"it works in Cubase." Adding a row here means a human loaded a Pulp plug-in
in that host and exercised the capability.

**Status legend:**
- ✅ **stable** — validated on latest host release, no known issues
- 🟡 **usable** — loads and runs; at least one known quirk documented below
- 🔴 **broken** — loads but produces incorrect results
- — **unvalidated** — nobody has tried yet
- n/a **unsupported by host or format** — the DAW or plug-in format does not
  expose that capability

Each non-placeholder update must cite durable evidence. For manual DAW sessions,
store the filled-in script and `.daw-bench.json` manifest under
`docs/validation/daw-bench/results/YYYY-MM-DD/` and run:

```bash
python3 tools/scripts/check_daw_bench_evidence.py \
    docs/validation/daw-bench/results/YYYY-MM-DD \
    --require-any
python3 tools/scripts/check_host_matrix_evidence.py
python3 tools/scripts/summarize_daw_bench_results.py \
    docs/validation/daw-bench/results \
    --require-any
```

If a capability stays `n/a`, name the host or format reason in the Notes column.
If it stays `—`, do not infer support from adapter unit tests, pluginval,
clap-validator, or a successful build.

---

## VST3

| Host            | Version | Load | Params | MIDI | Sidechain | Multi-bus | ARA | Notes |
|-----------------|---------|------|--------|------|-----------|-----------|-----|-------|
| Cubase          | 13      | —    | —      | —    | —         | —         | —   | ARA validation target for 6.3 |
| Studio One      | 7       | —    | —      | —    | —         | —         | —   | ARA validation target for 6.3 |
| Reaper          | 7.74    | 🟡   | 🟡     | —    | 🟡        | 🟡        | —   | VST3 bench evidence: `docs/validation/daw-bench/results/2026-06-12/` |
| Live            | 12      | —    | —      | —    | —         | —         | n/a | Live doesn't support ARA |
| FL Studio       | 21      | —    | —      | —    | —         | —         | n/a | |
| Bitwig          | 5       | —    | —      | —    | —         | —         | n/a | Prefers CLAP path |

## CLAP

| Host            | Version | Load | Params | MIDI | Sidechain | Multi-bus | ARA | Notes |
|-----------------|---------|------|--------|------|-----------|-----------|-----|-------|
| Bitwig          | 5       | —    | —      | —    | —         | —         | —   | Bitwig's ARA extension for CLAP is in progress |
| Reaper          | 7.74    | 🟡   | 🟡     | —    | 🟡        | —         | —   | CLAP bench evidence: `docs/validation/daw-bench/results/2026-06-12/` |
| Studio One      | 7       | —    | —      | —    | —         | —         | —   | CLAP-ARA bridge available |

## AU (macOS)

| Host            | Version | Load | Params | MIDI | Sidechain | ARA | Notes |
|-----------------|---------|------|--------|------|-----------|-----|-------|
| Logic Pro       | 11      | —    | —      | —    | —         | —   | ARA validation target for 6.4 |
| GarageBand      | 10      | —    | —      | —    | —         | n/a | |
| Live            | 12      | —    | —      | —    | —         | n/a | |
| MainStage       | 3       | —    | —      | —    | —         | n/a | |

## AUv3 (iOS + iPadOS)

| Host            | Version | Load | Params | MIDI | Notes |
|-----------------|---------|------|--------|------|-------|
| AUM             | iPadOS  | —    | —      | —    | Primary target; PR #227 example |
| GarageBand iOS  | 2.4+    | —    | —      | —    | |
| Cubasis         | 3       | —    | —      | —    | |
| Logic Pro iPad  | 1.1+    | —    | —      | —    | |

## LV2 (Linux)

| Host            | Version | Load | Params | MIDI | Notes |
|-----------------|---------|------|--------|------|-------|
| Ardour          | 8       | —    | —      | —    | |
| Carla           | 2.5     | —    | —      | —    | |
| Reaper          | 7       | —    | —      | —    | LV2 support varies by distro |

---

## Validation recipe

For a (host, format, capability) row:

1. Build the reference plug-in with `PULP_ENABLE_ARA=ON` (if the capability is ARA) or the plain profile otherwise.
   ```bash
   cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DPULP_ENABLE_ARA=ON -DPULP_ARA_SDK_DIR=$PWD/external/ara-sdk
   cmake --build build --target AraPitchTracker-CLAP PulpCompressor-VST3 PulpSynth-CLAP
   ```
2. Install to the host's scan path (`~/Library/Audio/Plug-Ins/VST3`, `~/.clap`, etc.).
3. Launch the host; confirm the plug-in appears in its browser.
4. For each capability column: load a project that exercises it.
   - **Params**: automate one from the host, confirm roundtrip.
   - **MIDI**: route from a MIDI track; confirm note-on reaches `process()`.
   - **Sidechain**: activate a second bus; confirm `sidechain_input()` returns non-null.
   - **Multi-bus**: instantiate with multiple output buses, verify channel assignment.
   - **ARA**: for Cubase/Studio One/Logic — drop an audio clip on the plug-in's track, open the ARA editor, confirm `begin_editing` logs (see `examples/ara-pitch-tracker/`).
5. Update the cell in this file from `—` to ✅/🟡/🔴 and, if non-stable, add a "Notes" entry below.

## Known quirks

Quirk notes must point to the durable DAW-bench result folder or to a focused
adapter/runtime issue. Keep this section scoped to behavior observed in a real
host session; speculative accommodations belong in the host-quirks catalog until
bench evidence exists.

Current checked-in matrix state has one usable REAPER VST3 row backed by the
dated DAW-bench evidence above. Do not promote any additional row out of `—`
until the matching evidence manifest validates.

## See also

- `planning/production-readiness/STATUS.md` — per-workstream rollup with
  `production_validated` flags that mirror this table's ✅ rows.
- `docs/validation/daw-bench/README.md` — manual DAW-bench scripts and evidence
  manifest workflow.
- `docs/reference/host-quirks-policy.md` — runtime host-quirk policy, tiers,
  and opt-in/opt-out controls.
- `.agents/skills/ara/SKILL.md` — automation-side ARA usage recipes.
- `docs/guides/ara.md` — Pulp's ARA integration overview.
- `examples/ara-pitch-tracker/` — the canonical smoke-test plug-in for
  the CLAP-ARA path.
