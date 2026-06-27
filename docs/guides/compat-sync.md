# Compat-Sync Policy

Pulp ships a **compatibility matrix** at the repo root
(`compat.json`) that tells users which React Native, CSS, Yoga, React,
HTML, Canvas2D, and import-resolver props are supported on Pulp. The
matrix is split into per-surface files under `compat/` for reviewable
edits, then checked against the aggregate by
`tools/scripts/compat_aggregate.py`.

The compat-sync gate generalizes the skill-sync / version-bump /
docs-sync pattern to this domain: when a PR's diff touches a path
mapped to a compat surface, the matching artifacts (`compat.json`
prefix entry, doc page, test glob) must be updated in the same diff,
or the PR must carry an explicit bypass trailer.

## Three-layer enforcement

Same shape as [versioning.md § Three-layer gate](versioning.md#three-layer-gate):

```
Layer 1 (fast, per-edit, agent-specific)
    hooks/scripts/cli-plugin-sync.sh, Claude Code PostToolUse hooks
    → compat_sync_check.py --mode=hint, advisory

Layer 2 (pre-push, agent-agnostic, enforcing by default)
    .githooks/pre-push
    → compat_sync_check.py --mode=report --enforce
    → PULP_DISABLE_PREPUSH_GATES=1 demotes failures to advisory

Layer 3 (PR gate, authoritative)
    .github/workflows/version-skill-check.yml
    → compat_sync_check.py --mode=report --enforce
    → no bypass without commit trailer
```

All three layers call into the same script
(`tools/scripts/compat_sync_check.py`); only the enforcement is
different.

## The path map

`tools/scripts/compat_path_map.json` is the single source of truth for
"this source file requires which compat artifacts." Each source-path
entry is an array of `{kind, prefix|path|glob}` requirements:

```json
{
  "core/view/js/web-compat-canvas.js": [
    {"kind": "compat-json", "prefix": "canvas2d"},
    {"kind": "doc", "path": "docs/reference/compat/canvas2d.md"},
    {"kind": "test", "glob": "test/test_canvas_widget*.cpp"}
  ]
}
```

Three requirement kinds:

| Kind          | Means                                                 |
|---------------|-------------------------------------------------------|
| `compat-json` | The named section in `compat.json` must exist and contain at least one entry, unless `compat.json` is modified in the same diff. `prefix=*` means "any/all sections". |
| `doc`         | The named doc page must be touched in the same diff. `{prefix}` in the path expands per-prefix when the source spans multiple compat sections. |
| `test`        | At least one file matching the glob must be touched in the same diff. Tests are the bridge's correctness contract. |

When a path-map entry has multiple requirements, they all share the
"effective prefix" determined by the `compat-json` requirement (or
the wildcard set if it's `*`). That means a single bypass trailer
covers compat-json + doc + test for one source file.

## Bypass trailer

Tip-commit (or any commit in the diff range) trailer matching the
existing `Version-Bump: skip` / `Skill-Update: skip` shape:

```
Compat-Update: skip prefix=<css|rn|yoga|react|html|canvas2d|imports|*> reason="..."
```

- Multiple `Compat-Update: skip` lines allowed, one per prefix.
- `prefix=*` covers every section.
- Bare `Compat-Update: skip` (no `prefix=`, no `reason=`) is rejected.
- Empty reasons (`reason=""`) are rejected.

The audit trail lives in git, not in the PR body. A bypass is a
recorded admission that the author thought about the rule and decided
it doesn't apply.

## Modes

| Mode     | Behavior |
|----------|----------|
| `hint`   | Always exit 0. Print advisory text. Used by agent PostToolUse hooks. |
| `report` | Print drift. Exit 1 iff `--enforce` or `PULP_ENFORCE_PREPUSH=1`; otherwise print advisory drift and exit 0. CI uses `--enforce`; the pre-push hook blocks through its own gate aggregation unless demoted with `PULP_DISABLE_PREPUSH_GATES=1`. |
| `apply`  | Like `report` but also writes missing top-level sections into `compat.json`. New sections are scaffolds; they still need real entries, docs, and tests before becoming a useful public contract. |

## Adding a new mapped path

1. Identify the prefix(es) the source spans:
   - `core/view/src/widget_bridge.cpp` → `*` (every section).
   - `core/view/js/web-compat-canvas.js` → `canvas2d` only.
   - `core/view/js/web-compat-style-decl.js` → `css` only.

2. Add the entry to `tools/scripts/compat_path_map.json`:

   ```json
   "core/view/js/web-compat-foo.js": [
     {"kind": "compat-json", "prefix": "<prefix>"},
     {"kind": "doc", "path": "docs/reference/compat/<prefix>.md"},
     {"kind": "test", "glob": "test/test_widget_foo*.cpp"}
   ]
   ```

3. Run the gate locally to verify your map entry is honored:

   ```bash
   python3 tools/scripts/compat_sync_check.py --mode=report
   ```

4. Update `tools/scripts/test_compat_sync_check.py` if the new path
   class needs a regression test. (Adding entries that follow an
   existing pattern usually doesn't.)

## Adding a new compat section

`compat.json` recognizes seven top-level sections by default:
`css`, `rn`, `yoga`, `react`, `html`, `canvas2d`, `imports`. To add a
new one:

1. Add it to `KNOWN_PREFIXES` in `tools/scripts/compat_sync_check.py`.
2. Add the populated section to `compat.json` and regenerate/sync the
   split `compat/` part files.
3. Update this doc, plus
   `tools/scripts/test_compat_sync_check.py` if the new section
   warrants a self-check coverage row.

## See also

- [docs/guides/versioning.md](versioning.md) — the parent three-layer
  enforcement design (skill-sync + version-bump).
- [tools/scripts/compat_sync_check.py](../../tools/scripts/compat_sync_check.py)
  — the gate.
- [tools/scripts/compat_path_map.json](../../tools/scripts/compat_path_map.json)
  — the path map.
- [compat.json](../../compat.json) — the aggregate compatibility matrix.
- [compat/](../../compat/) — split per-surface matrix files.
- [tools/scripts/compat_aggregate.py](../../tools/scripts/compat_aggregate.py)
  — split/aggregate consistency check.
