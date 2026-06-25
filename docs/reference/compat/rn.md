# React Native compat

Status of the React Native style-prop surface exposed by Pulp's
`@pulp/react` consumer (`packages/pulp-react/src/prop-applier.ts`).
This is the renderer that turns RN-style JSX into bridge `setX` calls.

The authoritative inventory is `compat.json` (`rn/*` prefix).

## Inventory

The generated inventory in `compat.json` still records the audit
snapshot metadata (`_audit.generated` and `_audit.main_sha`). Treat that
metadata as provenance for the machine-readable catalog, not as the
freshness marker for this hand-maintained note.

Spec walk:
- [RN View Style Props](https://reactnative.dev/docs/view-style-props)
- [RN Text Style Props](https://reactnative.dev/docs/text-style-props)
- [RN Layout Props](https://reactnative.dev/docs/layout-props)
- [RN Transforms](https://reactnative.dev/docs/transforms)

## Current support

All 120 `rn/*` entries in `compat.json` are currently marked
`supported`. That status means the prop has an intentional lowering path
through `@pulp/react`, the widget bridge, or a documented Pulp subset;
it does not mean Pulp is a browser or that platform-specific RN behavior
is reproduced byte-for-byte.

### Layout and positioning

`@pulp/react` lowers RN layout props to the same Yoga-shaped bridge
surface used by the CSS adapter. Numbers are pixels, percent strings are
forwarded to Yoga's percent APIs, and margins additionally accept
`auto`; Yoga does not define `auto` padding.

`flex={n}` follows RN shorthand semantics: positive values expand to
grow/shrink/basis `(n, 1, 0)`, zero to `(0, 0, auto)`, and negative
values to `(0, 1, auto)`. `display: 'flex'` keeps the view visible and
emits a row flex direction when no explicit direction is present,
because Pulp's Yoga default is RN-style column.

Logical flow props are implemented with a physical-edge fast path:
`marginStart` / `paddingStart` / `borderStartWidth` / `start` route to
left-side setters, and the corresponding `End` props route to right-side
setters. `inset`, `insetBlock`, and `insetInline` expand to physical
top/right/bottom/left bridge calls. `direction` and `writingDirection`
reach `setDirection`, but the prop-applier does not remap the logical
edge fan-out at dispatch time.

### Paint, text, and platform-origin props

The RN visual surface includes the New Architecture props Pulp can
express directly (`boxShadow`, `filter`, `mixBlendMode`, outline
longhands, `isolation`) plus compatibility shims for common platform
props:

- `boxShadow` accepts CSS strings, RN object form, and RN Fabric array
  form. Each parsed shadow dispatches to `setBoxShadow`; `'none'` and
  `''` clear the shadow slot. `null` and `undefined` are no-ops in
  the generic prop dispatcher. The runtime applier accepts arrays, but
  the exported `BoxShadow` TypeScript prop type still lists only object,
  string, and null forms.
- `shadowColor`, `shadowOffset`, `shadowOpacity`, and `shadowRadius`
  are modeled as longhands over the same `View` box-shadow storage so a
  JSX diff can update one attribute without clobbering the others.
- `elevation` maps Android Material elevation to a single Pulp
  `BoxShadow` approximation. It preserves the visible shadow intent but
  is not a full Material dual-shadow model.
- `borderCurve: 'continuous'` switches rounded corners to Pulp's
  continuous-corner approximation; `circular` keeps the standard rounded
  rect path.
- `includeFontPadding` is accepted and stored, but Pulp's text shaper
  already uses tight glyph layout and does not add Android's legacy
  extra text padding.
- `textAlignVertical` and `verticalAlign` share the Label vertical-align
  bridge slot. `textDecorationColor`, `textDecorationStyle`, and
  `writingDirection` are also implemented cross-platform even though RN
  originally exposed parts of that surface through platform-specific
  APIs.
- `isolation` round-trips through the bridge. Pulp's per-view layer
  composition already provides the blend-containment and paint-order
  scoping authors usually request with `isolation: 'isolate'`.
- `mixBlendMode` forwards the W3C/RN blend-mode keyword set to the
  shared canvas blend enum. The bridge also accepts `plus-lighter` and
  `plus-darker` as the closest available additive blend; `plus-darker`
  remains an approximation rather than the draft spec's darker variant.
  Skia honors blend layers; non-Skia canvas backends may still fall back
  to a plain save-layer path.

Cursor support is platform-shaped. The bridge accepts the CSS cursor
keyword vocabulary used by RN/web imports, and macOS maps the native
cursor keywords AppKit exposes. Keywords without a platform cursor fall
back to the default cursor rather than fabricating a visual.

### Transforms

RN transform arrays are supported for `translateX`, `translateY`,
`rotate`, `rotateZ`, `scale`, `scaleX`, `scaleY`, `skewX`, and `skewY`.
The applier coalesces related axes before dispatching so
`[{ translateX: 10 }, { translateY: 20 }]` becomes one
`setTranslate(10, 20)` call. Independent `scaleX` / `scaleY` currently
collapse to the uniform `setScale` bridge slot, so the last scale axis
write wins. `rotateX`, `rotateY`, `perspective`, and matrix transforms
are accepted by the TypeScript type but intentionally have no 3D storage
in Pulp's 2D `View` model.

CSS transform strings are a CSS-adapter concern. For RN-flavored JSX,
pass the transform-array shape.

## SvgPath

The `<SvgPath>` intrinsic surfaces a typed JSX face for
`createSvgPath` + `setSvgPath` + `setSvgViewBox` + `setSvgFill` +
`setSvgFillRule` + `setSvgFillGradient` + `setSvgStroke` +
`setSvgStrokeWidth`. Lowercase `svg` is a container and lowercase
`path` creates the same `SvgPathWidget`, so imported SVG snippets do not
need to be rewritten to the PascalCase intrinsic before they can render.

`viewBox` accepts `[width, height]`, `"width height"`, and
`"min-x min-y width height"` forms. The bridge consumes width and height
today; min-x/min-y origin offsets remain a paint-side gap.

`fillRule` mirrors SVG's `fill-rule`
(`nonzero` | `evenodd`). Default `nonzero` matches the SVG / Canvas2D
default; `evenodd` is required for compound annular paths — e.g. a
stroked ellipse that a framework lowers to a two-subpath `M…Z M…Z`
fill (JUCE's `SVGGraphicsContext` does this for
`Graphics::drawEllipse`) only renders the ring's hole under even-odd
winding. The raw `<path>` web-compat path also accepts the hyphenated
`fill-rule` attribute.
