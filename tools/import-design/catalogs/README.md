# Bridge stress-test catalogs

Static fixture catalogs used by `tools/stress-bridge/pulp-stress-bridge` to
drive adversarial validation of pulp's `@pulp/react` + DOM-lite + Canvas2D
bridges. See umbrella issue [#1387][umbrella].

These TSVs are committed so CI does **not** depend on network access at
runtime. They are refreshed manually on a quarterly cadence (or sooner if
a Spectr-driven gap surfaces a missing prop).

## Sources

| File                  | Source URL                                                                  | What we extract                                                                  |
|-----------------------|-----------------------------------------------------------------------------|----------------------------------------------------------------------------------|
| `yoga.tsv`            | https://www.yogalayout.dev/docs/styling/                                    | Every Yoga style prop + value type (enum / length / percentage / number).        |
| `rn-viewstyle.tsv`    | https://reactnative.dev/docs/view-style-props                               | Every React Native ViewStyle prop + accepted values (color / enum / number etc). |
| `mdn-css.tsv`         | https://developer.mozilla.org/en-US/docs/Web/CSS/Reference/Properties       | Top-level CSS property names from the alphabetical index (~470 props, no values).|

## Format

Each row is tab-separated:

```
prop<TAB>value_type<TAB>notes
```

- `prop` — canonical name (CSS dash-case for Yoga / MDN; RN camelCase for `rn-viewstyle.tsv`).
- `value_type` — coarse category the harness uses to pick adversarial inputs:
  - `enum` — `notes` lists the allowed string values, comma-separated.
  - `length` — px or unitless number.
  - `percentage` — `<n>%` string.
  - `number` — bare number (incl. negative, fractional).
  - `length-or-percentage`, `length-or-percentage-or-auto` — combined forms.
  - `color` — CSS color (hex / rgb / rgba / hsl / named).
  - `transform-array` — RN array form (`[{ rotate: '45deg' }]`).
  - `shadow-offset` — RN object form (`{ width, height }`).
  - `edge-set` — shorthand expanded to per-edge values (top/right/bottom/left).
  - `string` — opaque CSS string (e.g. `box-shadow`, `filter`).
  - `css-property` — used for the MDN catalog where we only track the prop
    name, not the value grammar.
- `notes` — optional; for enums, the comma-separated value list. For
  unusual props (logical, RTL-relative, platform-gated) a short tag.

Properties are alphabetized within each file (case-insensitive) and
deduplicated. There is intentionally no header row — the file is a flat
catalog and tooling reads it line by line.

## Refresh process

These catalogs are produced semi-manually because:

- The Yoga page is small enough (~30 props) that hand maintenance is cheaper than scraping.
- The RN ViewStyle page is small (~50 props) and value semantics need a human eye.
- The MDN index is ~470 entries and is the only one that benefits from automation.

Refresh steps:

1. Open each source URL.
2. Pull the prop list (the WebFetch tool in agent loops is fine; for
   humans, `curl ... | <markdown extractor>` is also fine).
3. Diff against the existing TSV.
4. Add new rows in alphabetical position; preserve existing `notes`.
5. If the value type schema needs to grow (e.g. a new `value_type` tag),
   update this README and the harness `value_type → fixture generator`
   table in `tools/stress-bridge/SPEC.md`.

A future PR can wire this into a `tools/stress-bridge/refresh-catalogs.py`
script that scrapes and rewrites in one shot, but the first pass is
manual on purpose to avoid baking a flaky scraper into CI.

## Refresh cadence

- **Quarterly** baseline refresh.
- **On demand** when an `import-design` consumer (Spectr, etc.) reports
  a prop the bridge doesn't recognise — add the row, file the gap, run
  the harness against it.
- **Before each Pulp minor release** as part of the bridge-hardening
  checklist.

## Overlap with `compat.json`

`compat.json` (at repo root) is the **implementation status matrix** —
every prop that has a known mapping into pulp's bridge, with
`supported / partial / noop / missing / wontfix`. These TSVs are the
**reference catalog** — every prop the spec says exists, regardless of
whether pulp implements it.

The five status values are distinguished as follows:

- `supported` — full implementation; round-trips cleanly through the
  harness.
- `partial` — implemented but lacks coverage on some axis (missing
  enum values, percent units, multi-value form, etc.).
- `noop` — bridge entry point exists and accepts the value silently,
  but the body is an intentional stub pending a future subsystem.
  Distinct from `missing` because the registration is real; the no-op
  is intentional, not an oversight. Added in pulp #1475.
- `missing` — no implementation at all; no `case` in the JS-side router
  and no bridge handler.
- `wontfix` — explicitly out of scope (e.g. table-layout, page-break,
  list-style-image — not relevant to audio plugin UI).

The two are joined by `pulp-stress-bridge`:

- TSV row exists, no `compat.json` row → harness reports
  `silent-noop` and the row should be backfilled into `compat.json`
  with `status: missing`.
- Both exist, harness renders cleanly → status promoted toward
  `supported` (with the harness fixture path recorded in `tests`).
- Both exist, harness shows visual diff → status downgraded to
  `partial` and a tracker issue is filed.

[umbrella]: https://github.com/danielraffel/pulp/issues/1387
