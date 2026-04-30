# Codecov component classification — durable status

Parent issue: [#1049](https://github.com/danielraffel/pulp/issues/1049).
Audit resolution: [#1055](https://github.com/danielraffel/pulp/issues/1055).
Sibling precedent: [#841](https://github.com/danielraffel/pulp/issues/841)
(`inspect/` was promoted to a first-class component rather than left
represented-but-uncategorized).

## Contract

Every first-party file under `core/`, `tools/`, `apple/`, `android/`,
or `inspect/` matches **at least one** Codecov component declared in
`codecov.yml :: component_management.individual_components`.
"Unclassified" first-party files are bugs — the dashboard would silently
omit their coverage from every component view.

## Why files can match more than one component (by design)

Pulp's components use three intentionally cross-cutting axes:

1. **Subsystem** — `audio`, `canvas`, `dsl`, `events`, `format`, `host`,
   `midi`, `osc`, `platform`, `render`, `runtime`, `signal`, `state`,
   `view`. One per top-level directory under `core/`.
2. **Platform** — `android`, `apple`, `linux`, `windows`. Cross-cuts
   the subsystem axis so a question like "what fraction of `core/audio`
   is exercised on Windows" has a one-click answer on the dashboard.
3. **Surface** — `cli`, `inspect`, `ship`, `tools`. First-party
   non-core code.

A platform shim like `core/audio/platform/mac/coreaudio_device.mm`
therefore legitimately appears in **two** components — `audio`
(subsystem) and `apple` (platform). This is the explicit documented
intent in `codecov.yml` (header comment lines 79-83 and 142-160) and in
[`docs/guides/coverage.md` §"Axes"](../guides/coverage.md#axes):

> Codecov splits each upload along three axes so you can cross-filter —
> for example, "how's one subsystem covered on one OS."

The strict "exactly one component per file" check from the audit recipe
in #1055 conflicts with this multi-axis design and is replaced by the
contract above (≥1 component per file, multi-bucket allowed when it
fits the documented axes).

## Allowed cross-axis overlaps

Each line below names an `(axis_a, axis_b)` pair that is legitimately
populated by the same file. The structural test
`tools/scripts/test_codecov_components.py` enforces that every
multi-bucket file fits one of these pairs and nothing else.

| Subsystem | × | Platform |   |
|-----------|---|----------|---|
| `audio`   | × | `android`, `apple`, `linux`, `windows` | `core/audio/platform/<plat>/**` |
| `canvas`  | × | `apple` | `core/canvas/platform/mac/**` |
| `midi`    | × | `android`, `apple`, `linux`, `windows` | `core/midi/platform/<plat>/**` |
| `platform`| × | `android`, `apple`, `linux`, `windows` | `core/platform/platform/<plat>/**`, `core/platform/src/android/**`, `core/platform/include/pulp/platform/<plat>/**` |
| `render`  | × | `android` | `core/render/platform/android/**` |
| `view`    | × | `android`, `apple`, `linux`, `windows` | `core/view/platform/<plat>/**` |

| Surface | × | Surface (more-specific wins by intent) |   |
|---------|---|------|---|
| `cli`   | × | `tools` | `tools/cli/**` — the broader `tools/**` glob still matches by design; `cli` is the canonical attribution and is preserved by the `cli`-component path being more specific. The Codecov flag layer documents this same precedence (`codecov.yml` line 187-191). |

## Deliberate-uncategorized files

None at this revision. Every first-party path matches at least one
component. If a future surface lands as deliberately uncategorized
(e.g. throwaway scaffolding, vendored support code that is not
coverage-bearing but is still tracked), append it here with a one-line
rationale — the structural test reads this section as an allowlist.

## Drift policy

When a new top-level subsystem, platform, or surface is added:

1. Add a matching component to `codecov.yml :: individual_components`
   (and a matching path-flag under `flags:`) — `tools/scripts/test_codecov_config.py`
   already enforces 1:1 alignment with the live `core/*` tree.
2. If the new code lives in a platform shim under an existing
   subsystem, add the new `(subsystem × platform)` row to the table
   above.
3. Run `python3 tools/scripts/test_codecov_components.py` and confirm
   it stays green.
