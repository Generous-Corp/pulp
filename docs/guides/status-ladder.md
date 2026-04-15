# Status Ladder

Pulp's `docs/status/support-matrix.yaml` is the authoritative source of truth for
capability maturity labels. To keep labels meaningful, we enforce a **ladder
rule** on the `usable` tier:

> A capability may be labeled `usable` only if it has evidence of validation.

Evidence can take three forms:

1. **Platform-scoped entry.** A capability nested under a platform parent
   (`macos`, `ios`, `android`, `windows`, `linux`, `web`, `wasm`) is considered
   already scoped and passes the rule by construction — its validation is
   inherent to that platform.

2. **Validation language in `notes:`.** The notes field mentions one of:
   `test`, `tests`, `validated`, `validator`, `ci`, `golden`, `auval`,
   `pluginval`, `clap-validator`, `lv2lint`, `round-trip`, `headless`,
   `screenshot`.

3. **Explicit waiver.** The entry's YAML path appears in
   `.status-ladder-waivers.txt` at the repo root. Waivers exist for the
   migration backlog — every new `usable` entry should aim to satisfy (1) or
   (2) instead of adding a waiver.

## Allowed status values

- `stable` — hardened, long-lived API, full cross-platform coverage
- `usable` — production-worthy per the ladder rule above
- `experimental` — implemented but not production-validated
- `partial` — implemented for a subset of platforms / features
- `planned` — spec exists, no code yet
- `unsupported` — intentionally not supported

## Tooling

- `tools/check_status_ladder.py --mode=warn` — list violations, exit 0
- `tools/check_status_ladder.py --mode=report` — list violations, exit 1
- `tools/check-docs.sh` runs warn-mode by default; set
  `PULP_STATUS_LADDER_STRICT=1` to run report-mode.

## Phased rollout

The rule enters service in **warn-mode** so we can land it without regressing
`pulp docs check`. The migration backlog is captured in
`.status-ladder-waivers.txt`; that file will shrink as we attach real
validation language to existing entries.

After one release of warn-mode soak, CI will flip to report-mode and PRs that
introduce unwaived violations will fail.

## Adding a new `usable` entry

Preferred, in order:

1. Put it under a platform parent (macos/ios/android/windows/linux/web/wasm).
2. Mention the validator in `notes:` (e.g. "Validated in CI via clap-validator
   and pluginval strict").
3. If neither is possible, add the path to `.status-ladder-waivers.txt` **and**
   justify it in the PR body.

## Removing a waiver

When a waived entry gains real validation:

1. Add the validation evidence to its `notes:` in
   `docs/status/support-matrix.yaml`.
2. Remove the line from `.status-ladder-waivers.txt`.
3. Run `python3 tools/check_status_ladder.py --mode=report` and verify no new
   violations surface.

## Design references

- `planning/production-readiness/08-docs-consistency.md` — spec (workstream 08)
- `tools/check_status_ladder.py` — implementation
- `.status-ladder-waivers.txt` — migration backlog
