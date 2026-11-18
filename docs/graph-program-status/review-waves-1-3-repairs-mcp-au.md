# Adversarial review of graph waves 1-3: MCP diff and AU v2 timing repairs (2026-11-17)

Scope: the MCP graph diff and AU v2 slice-timing findings from the second-model
review of the merged graph PRs. The host findings are in
`review-waves-1-3-repairs.md`. Each finding was re-read against `origin/main`
and reproduced by a test that failed before the fix.

## Fixed

| Finding | Repair | Test |
|---|---|---|
| MCP graph diff passed `before`/`after` to the CLI with JSON escapes still encoded (a Windows path kept doubled backslashes; a trailing escaped backslash broke parsing) | `extract_decoded_string` resolves every escape to UTF-8 and refuses an unpaired surrogate or escaped NUL; a non-string value is `invalid_option` | `MCP graph diff hands the CLI the decoded before and after paths`, `MCP JSON string decoding resolves escapes and refuses what cannot be text` |
| MCP graph diff returned empty error text when the CLI failed silently (timeout, output cap, cancellation, crash) | the error names the cause | the same diff test (`exited with code 3`) |
| AU v2: an infinite tempo with a slice offset made the beat infinite and the bar conversion undefined | a non-finite host beat or tempo stays unknown; `derive_bar_from_beats` leaves a non-finite or out-of-range bar at 0 for every adapter | `AU v2 host callbacks leave a non-finite beat or tempo unknown`, `derive_bar_from_beats: a position with no representable bar stays at bar 0` |

## Left open

- MCP graph diff ignores unknown request properties, and a path that begins
  with `--` is read by the CLI as an option.
