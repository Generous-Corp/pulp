# Pulp design-import IR

Pulp's design-import IR (`@pulp/import-ir`) is a typed intermediate
representation that survives source re-import. Adapters lower source
formats — Figma, Mitosis, Claude Design HTML, Stitch HTML, v0.dev,
Pencil, etc. — into a common `IRNode` tree keyed by
`stable_anchor_id`. The tweaks layer (`pulp-tweaks.json`) is keyed by
the same anchors, so dev overrides survive when the designer
regenerates the source.

The full design lives at `pulp/issues/1486` (umbrella) and the spec
draft (`/tmp/pulp-ir-spec-spike.md`). This guide is the consumer-facing
summary.

## Phase 1 spike (this PR)

Phase 1 ships:

- **Type definitions** (`packages/pulp-import-ir/src/types.ts`):
  `IRNode`, `TypedLayout`, `TypedPaint`, `TypedText`, `TokenRef`,
  `Drift`, `IRProvenance`, `Confidence`, `SourceAdapter`, `TweaksFile`,
  `LowerOptions`.
- **Three anchor strategies** (`anchors.ts`): `content-hash` (used by
  HTML-based sources; lifted from Mitosis/Builder's MIT-licensed
  `hashCodeAsString` algorithm — an FNV-1a fold over a stable-stringify
  of `(tag, role, text, depth)`), `path` (used by source files where
  reorders are explicit and rare), and `adapter` (used by Figma/Pencil
  with native node IDs).
- **Claude Design HTML adapter** (`adapters/claude-design-html/`):
  parses an HTML payload into a tree of `BuildNode`s, extracts
  `inline-style` properties into the typed `layout` / `paint` / `text`
  fields, preserves the verbatim `outerHtml` under
  `raw_source.outerHtml` for forward-compat, and resolves anchors via
  the content-hash strategy.
- **Tweaks layer** (`tweaks.ts`): `applyTweaks(node, tweaks)` deep-
  merges dotted-path overrides; orphaned tweaks (anchors missing from
  the freshly-lowered tree) attach to `meta.orphaned_tweaks` and
  surface via `diff()` as `Drift.orphaned-tweak`. Pure-sync, no IO
  in the core; `nodeFsTweaksIO()` wraps `node:fs/promises` for CLI
  consumers.
- **`diff()` (drift detection)** (`diff.ts`): sorted-by-anchor flat
  list of `added` / `removed` / `changed` / `reordered` /
  `orphaned-tweak` entries. Field-level diff over the typed sections.
- **JSX-equivalent lowering** (`lower-via-prop-applier.ts`): IR →
  `JSXLikeNode` tree → consumed by downstream renderers
  (`React.createElement(tag, props, …)`). Per spec §10 Q7 user-locked
  decision: route through the well-tested `@pulp/react` prop-applier
  rather than build a parallel renderer.
- **5-scenario validation harness** (`test/scenarios/scenarios.test.ts`):
  - **S1** Pure regen — tweaks survive trivially. ✓
  - **S2** Designer changed colors — tweaks survive (content-hash is
    color-agnostic). ✓
  - **S3** Designer added a new sibling section — existing IDs
    preserved, new section gets a new ID. ✓
  - **S4** Designer deleted a tweaked section — drift report flags
    the orphaned tweak. ✓
  - **S5** Designer reordered sections — content-hash preserves
    anchors across reorder. ✓

All 30 tests pass under `vitest run`.

## Architectural decisions (user-locked, per spec §10)

| Q | Decision |
|---|----------|
| Q2 — IR module location | TS-only at `packages/pulp-import-ir/`. Promote to C++ on profiling. |
| Q3 — DTCG token resolution | Post-lowering. `TokenRef` strings preserved in IR; resolver runs after lowering. Token-tree edits propagate without reimport. |
| Q7 — Lowering route | IR → JSX-equivalent intrinsics → existing `@pulp/react` prop-applier. Duplicative-but-safe; well-tested code path. |

## Public API

```ts
import {
    lowerClaudeDesignHtml,
    applyTweaks,
    diff,
    emptyTweaksFile,
    nodeFsTweaksIO,
    toJSXLikeTree,
    type IRNode,
    type TweaksFile,
} from '@pulp/import-ir';

// Lower a Claude Design HTML payload to IR.
const ir = await lowerClaudeDesignHtml(rawHtml);

// Apply dev tweaks read from disk.
const io = nodeFsTweaksIO();
const tweaks = (await io.readTweaks('homepage.tweaks.json'))
    ?? emptyTweaksFile('0.78.1', new Date().toISOString());
const tweakedIR = applyTweaks(ir, tweaks);

// Detect drift on next import.
const newIR = await lowerClaudeDesignHtml(updatedHtml);
const drifts = diff(ir, newIR);
for (const d of drifts) {
    if (d.kind === 'orphaned-tweak') {
        console.warn(`tweak orphaned: ${d.anchor} (${d.reason})`);
    }
}

// Hand off to @pulp/react via JSX-equivalent tree.
const jsxTree = toJSXLikeTree(tweakedIR);
// → render through React.createElement(tag, props, ...children).
```

## Per-source anchor strategy defaults (§4.4)

| Source                | Default strategy        |
|-----------------------|-------------------------|
| Figma MCP             | `adapter`               |
| Pencil MCP            | `adapter`               |
| Mitosis source        | `adapter`               |
| RN file export        | `path`                  |
| Claude Design HTML    | `content-hash`          |
| Stitch HTML           | `content-hash`          |
| v0.dev export         | `content-hash`          |
| Generic HTML/CSS      | `content-hash`          |

Override via `LowerOptions.anchorStrategy` per import; per-node escape
via `meta.anchor_id_override`.

## Phase 2 — what's next

The spec lays out follow-up adapters in priority order:

1. Figma MCP / Figma ZIP adapter (~1.5 weeks; uses adapter strategy)
2. Mitosis adapter (~1 week; adapter strategy)
3. Stitch HTML / v0.dev adapters (~3-5 days each; content-hash)
4. Pencil MCP adapter (~1 week; adapter strategy)
5. RN file export adapter (~5 days; path strategy)

Each adapter implements `SourceAdapter<R>` and reuses the shared
anchors / tweaks / diff infrastructure. The CLI's `pulp import design`
flow will pick the right adapter based on source kind.

## References

- Mitosis Builder content-hash IDs:
  `mitosis/packages/core/src/parsers/builder/builder.ts:1163`,
  `.../symbols/symbol-processor.ts:187` (MIT) — pattern lift, not
  vendoring.
- pulp #1486 (umbrella), pulp #1307 (CLI scaffolding), pulp #1432
  (ZIP import path).
- Codex /codex audit 2026-05-06 (architecture review).
