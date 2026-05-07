# @pulp/import-ir

Pulp design-import IR — typed intermediate representation that survives
source re-import.

## What it is

When a designer regenerates a Figma / Claude Design / Mitosis source,
Pulp re-imports it through an adapter into a tree of `IRNode`s. Every
node carries a `stable_anchor_id` so dev tweaks (`pulp-tweaks.json`)
keyed by the same anchor survive the re-import. A `diff()` produces a
sorted Drift list — added / removed / changed / reordered / orphaned-
tweak — for the CLI to surface as a "what changed" report.

## Package layout

```
src/
├── types.ts                 — IRNode, TypedLayout, TypedPaint, TypedText,
│                              TokenRef, Drift, IRProvenance, Confidence,
│                              SourceAdapter, TweaksFile, LowerOptions
├── anchors.ts               — content-hash / path / adapter strategies
├── tweaks.ts                — applyTweaks, setByDottedPath, fs IO wrapper
├── diff.ts                  — sorted-by-anchor Drift list
├── lower-via-prop-applier.ts — IR → JSX-equivalent tree (consumed by @pulp/react)
├── adapters/
│   └── claude-design-html/
│       └── lower.ts         — first-spike adapter
└── index.ts                 — public API
```

## Quick start

```ts
import { lowerClaudeDesignHtml, applyTweaks, diff } from '@pulp/import-ir';

const ir = await lowerClaudeDesignHtml(rawHtml);
const tweaked = applyTweaks(ir, tweaks);
const drifts = diff(prevIR, ir);
```

See `docs/guides/import-ir.md` for the full guide.

## License

MIT — pulp #1486 spike.
