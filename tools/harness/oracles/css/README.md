# CSS oracle

The CSS oracle is a static reference table sourced from three places:

1. **`core/view/js/web-compat-style-decl.js`** — the JS-side router that
   translates `el.style.X = ...` assignments into bridge `setX(...)` calls.
   Every `case "X":` in `_applyProperty` is the strongest possible evidence
   that property `X` actually reaches the bridge from the CSS path. The
   adapter parses this file at init and uses the case-key set as the
   "wired" allow-list.

2. **`tools/import-design/catalogs/mdn-css.tsv`** — the 525-row MDN catalog
   (`<kebab-property>\tcss-property` per line). Used as a permissive OOS
   gate: catalog entries whose kebab-cased name is not in the MDN list are
   noteworthy but not auto-OOS, because the catalog also tracks legacy
   aliases (`word-wrap`) and webkit-prefixed props (`-webkit-line-clamp`).

3. **`packages/pulp-react/src/prop-applier.ts`** — the alternative route
   for the same CSS surface. css/* entries can be reached either through
   `el.style.X` (web-compat) OR through `<View prop=...>` (React prop-
   applier). For week-1 scope the harness treats the JS-side route as
   authoritative; if the catalog claims supported and the JS route is
   missing but prop-applier covers it, that surfaces as a DIVERGE drift
   for further investigation.

Static-reference (versus Chromium-headless snapshotting) is the right
choice for week 1 because:

* `web-compat-style-decl.js` is the truth-of-record for what reaches the
  bridge — re-implementing it via headless Chrome would be measuring the
  spec, not Pulp.
* CSS spec drift over Chromium versions would force an oracle-refresh
  cadence we don't want yet (deferred to week 3+ when visual snapshot
  testing comes online).
* Pulp's CSS surface is bounded — we explicitly carve out 31 `wontfix`
  entries (table-layout, list-style, page-break, etc.) that no audio
  plugin UI consumer needs.

## File: `css-supported.json`

Schema:

```json
{
  "version": "...",
  "source": "...",
  "notes": ["..."],
  "enums": {
    "<camelCase property name>": {
      "values": ["..."],     // CSS-spec enum value set
      "default": "..."        // optional — CSS default
    }
  }
}
```

Only enum-valued properties get an entry. Non-enum kinds (length, color,
shorthand, number) are short-circuited by the adapter — the wired-vs-not
check + `unsupportedValues` heuristic from the catalog is enough.

## Regeneration

The wired-prop set is recomputed from `web-compat-style-decl.js` on every
adapter init, so JS-side edits flow through automatically. The enum
table is hand-maintained — to refresh:

1. When a new enum-valued CSS property gains a `case "X":` in
   `web-compat-style-decl.js`, add an `enums.X` entry here citing MDN's
   value list.
2. Run `python3 tools/harness/verifier.py --surface=css` and review the
   drift list — entries newly classified DIVERGE/PASS will surface.
3. Bump `version` to reference the catalog or commit-sha that motivated
   the refresh.

The MDN list `tools/import-design/catalogs/mdn-css.tsv` is regenerated
out-of-band by `tools/import-design/scripts/refresh-catalogs.sh` (it
reads MDN's `bcd` JSON dump). A new MDN row is harmless until it shows
up in the harness drift list.
