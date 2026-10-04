# Wave 2D Claude fixture readiness receipt

Date: 2026-10-03
Base: `origin/main` at `c3c37bfc05fd58686dcc0dc369e4aaff9f4ccb06`

## Scope

This audit covers the three optional real-bundle cases and the checked-in
Claude fixture/source-contract surface. It changes only the fixture manifest,
the Claude validation mapping in `tools/import-validation/source-contracts.json`,
and this receipt. No production parser, `DesignIR`, network/cache policy, or
roadmap file changed.

## Exact skipped cases

All three cases use the same `PULP_CLAUDE_BUNDLE_FIXTURE` input:

| Test source | Case | Result | Reason |
| --- | --- | --- | --- |
| `test/test_design_import_claude_bundle.cpp` | `parse_claude_bundle accepts a real Spectr editor.html fixture when PULP_CLAUDE_BUNDLE_FIXTURE is set` | **SKIPPED** | Environment variable unset in this checkout |
| `test/test_design_import_claude_runtime.cpp` | `parse_claude_html_with_runtime against the real Spectr fixture when PULP_CLAUDE_BUNDLE_FIXTURE is set` | **SKIPPED** | Environment variable unset in this checkout |
| `test/test_design_import_inline_babel.cpp` | `real Spectr Claude bundle materialises widgets when PULP_CLAUDE_BUNDLE_FIXTURE is set` | **SKIPPED** | Environment variable unset in this checkout |

No real-bundle pass or failure is claimed. Supplying an unreadable path would
also skip with the test's explicit `fixture not readable` reason.

## Checked-in fixture coverage

`test/fixtures/imports/claude/manifest.json` now records the five available
files and their SHA-256 digests:

- `2024.10/example.html` — standalone HTML envelope.
- `2024.10/expected.json` — expected static IR.
- `2024.10/expected-classnames.json` — expected classname manifest.
- `vite-assets.html` and `vite-assets-hero.png` — deterministic asset-candidate fixture pair.

These fixtures remain available without external credentials or a Claude export.
The optional external bundle is intentionally not checked in and remains
unavailable here.

## Source-contract result

`python3 tools/import-validation/check-source-contracts.py --strict` completed
with no findings. The Claude row still maps static parsing to
`core/view/src/design_import.cpp`, runtime parsing to
`core/view/src/claude_bundle.cpp`, and the checked-in `2024.10/example.html`
format fixture. The registry now explicitly names the three focused optional
fixture targets/files/tags so coverage ownership is visible without claiming
that the external fixture is present.

`python3 -m unittest tools/import-validation/test_source_contracts.py`:
29 tests passed.

The repository gates ran the source-only and Python-contract checks successfully;
build-dependent checks were not run because this fresh docs-only worktree has no
configured CMake build. The direct-push hook also requested a Vellum watch-event
file for the registry path. That generated event is outside this receipt's
three-file scope and was intentionally not added.

## Readiness

Deterministic checked-in fixture readiness: **READY**.

Real Spectr bundle readiness: **UNVERIFIED / BLOCKED on fixture availability**.
The three optional cases must be rerun with a readable
`PULP_CLAUDE_BUNDLE_FIXTURE`; this receipt deliberately records no inferred
result.
