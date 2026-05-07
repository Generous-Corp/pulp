# RN ViewStyle oracle

The rn oracle is a static reference table sourced from React Native's
`ViewStyle`/`TextStyle`/`TransformsStyle` type definitions (RN 0.79) plus
`tools/import-design/catalogs/rn-viewstyle.tsv`. The harness uses it to
classify each `rn/*` entry in `compat.json` against three layers of
evidence:

1. **The oracle** (`rn-viewstyle.json`) — what RN itself supports for
   this prop, including platform flags (iOS-only / Android-only / RN
   New-Architecture-only) and pulp/RN extension markers (e.g.
   `overlay` is a Pulp-only extension; `d`/`fill`/`stroke` are
   `react-native-svg` props that @pulp/react re-exposes).
2. **The catalog payload** (`mapsTo`, `supportedValues`,
   `unsupportedValues`, `notes`) — what the catalog claims pulp does
   today.
3. **The bridge surface** (`packages/pulp-react/src/prop-applier.ts`
   switch-cases + `core/view/src/widget_bridge.cpp` register_function
   calls). The adapter parses prop-applier once at init and stores the
   set of `case 'X':` keys; it cross-references catalog `mapsTo`
   bridge-function names against the registered set in
   `widget_bridge.cpp`.

We chose "static reference" over "shell out to the live RN typechecker"
for the same reasons as yoga: portability, no extra build dependency
at verifier-runtime, small bounded surface (~120 properties).

## File: `rn-viewstyle.json`

Schema:

```json
{
  "version": "rn-0.79",
  "source": "...",
  "properties": {
    "<camelCase prop>": {
      "kind": "enum | number | color | length | length-or-percentage | length-or-percentage-or-auto | edge-set | string | transform-array | shadow-offset | tuple | enum-or-number | string-array | boolean",
      "values": ["..."],         // for enum: full RN-supported value list
      "default": "...",          // optional — RN default
      "platformOnly": "ios | android",  // optional — narrows scope
      "rnExtension": "react-native-svg",  // optional — third-party RN ext
      "pulpExtension": true,     // optional — Pulp-only addition
      "notes": "..."             // optional
    }
  },
  "bridgeFunctions": {
    "registered": ["setX", "setY", ...]  // currently-registered bridge fns
  }
}
```

## How the adapter classifies entries

* `wontfix` (catalog) → **OOS**
* `platformOnly` (oracle) → **OOS** (iOS-only / Android-only props
  aren't in the cross-platform pulp surface)
* `pulpExtension` (oracle) — the entry is unique to pulp, treated as
  a normal entry (typically PASS when prop-applier wires it).
* `mapsTo` markers signalling no-implementation (`no branch`,
  `no @pulp/react prop`, `not surfaced`, `intentionally NOT
  dispatched`, `no bridge support`, `no prop-applier case`) →
  **NOT_IMPL**.
* `mapsTo` references a `setX` bridge function that **isn't** in the
  registered list → **DIVERGE** (catalog disagrees with reality).
* prop-applier has a matching `case 'X':` → has binding. For enum
  kinds we then compare `supportedValues` against the oracle's full
  enum value set:
  * superset (every oracle value is supported) → **PASS**
  * subset (some oracle values supported) → **DIVERGE**
  * empty / disjoint → **NOT_IMPL** (binding exists but no values
    claimed)
* Non-enum kinds with no listed `unsupportedValues` and a binding →
  **PASS**; otherwise **DIVERGE**.

## Regeneration

The oracle is hand-maintained for now. To refresh:

1. Bump the RN version pin in `version`.
2. Walk RN's `Libraries/StyleSheet/StyleSheetTypes.d.ts` (or the
   public docs at <https://reactnative.dev/docs/view-style-props>)
   for new props / values / platform flags.
3. Re-run `grep -nE '"set[A-Z]' core/view/src/widget_bridge.cpp` and
   refresh `bridgeFunctions.registered`.
4. Mirror changes into `rn-viewstyle.json` and bump `version`.
5. Run `python3 tools/harness/verifier.py --surface=rn` and review
   the drift list.
