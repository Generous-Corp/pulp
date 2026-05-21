# DesignIR v1

DesignIR v1 is the canonical interchange format for design import. Source
adapters may accept older shortcut shapes, but the public writer emits this
versioned envelope:

```json
{
  "version": 1,
  "source": "figma",
  "sourceFile": "design.json",
  "root": {},
  "tokens": {},
  "assetManifest": { "version": 1, "assets": [] }
}
```

Round-trip compatibility is canonical equivalence, not byte identity:

1. Parse a legacy or source-adapter shape.
2. Normalize to `DesignIR`.
3. Serialize with `serialize_design_ir()`.
4. Parse that canonical JSON with `parse_design_ir_json()`.
5. Compare the canonical serialization of both parsed documents.

The permissive reader accepts both camelCase and snake_case for source
metadata fields where older adapters already emitted one form. The canonical
writer uses the names listed below.

## Node Metadata

Each node keeps enough provenance for downstream diffing and re-import:

| Field | Type | Meaning |
|---|---|---|
| `stable_anchor_id` | string | Stable tweak/re-import anchor. |
| `anchor_strategy` | string | `adapter`, `content-hash`, or `path`. |
| `source_node_id` | string | Source-native node ID when available. |
| `source_adapter` | string | Adapter that produced the node. |
| `source_version` | string | Adapter schema/parser version. |
| `confidence` | string | `pass`, `diverge`, or `not_impl`. |
| `raw_source` | string | Optional source snippet for diagnostics. |

## Layout

`IRLayout` v1 includes the existing flex fields plus the fields needed by
static design adapters and later baked materializers:

| Field | Type |
|---|---|
| `display` | string |
| `direction` | `row` or `column` |
| `gap`, `rowGap`, `columnGap` | number |
| `paddingTop`, `paddingRight`, `paddingBottom`, `paddingLeft` | number |
| `marginTop`, `marginRight`, `marginBottom`, `marginLeft` | number |
| `justify`, `align`, `alignSelf`, `alignContent` | string |
| `wrap` | bool |
| `flexGrow`, `flexShrink`, `aspectRatio` | number |
| `flexBasis` | string |
| `order` | integer |
| `overflowX`, `overflowY` | string |
| `widthMode`, `heightMode` | `fixed`, `hug`, or `fill` |

## Style

`IRStyle` v1 carries CSS-like visual fields in their resolved form:

| Field | Type |
|---|---|
| `backgroundColor`, `backgroundGradient`, `backgroundImage`, `backgroundRepeat` | string |
| `color`, `opacity` | string / number |
| `border`, `borderColor`, `borderWidth`, `borderStyle` | string / number |
| `borderTopColor`, `borderRightColor`, `borderBottomColor`, `borderLeftColor` | string |
| `borderTopWidth`, `borderRightWidth`, `borderBottomWidth`, `borderLeftWidth` | number |
| `borderRadius`, `borderTopLeftRadius`, `borderTopRightRadius`, `borderBottomRightRadius`, `borderBottomLeftRadius` | number |
| `boxShadow`, `filter`, `backdropFilter` | string |
| `fontFamily`, `fontStyle`, `textAlign`, `textTransform`, `textDecoration`, `whiteSpace`, `textOverflow` | string |
| `fontSize`, `fontWeight`, `letterSpacing`, `lineHeight` | number |
| `overflow`, `cursor`, `position`, `transform` | string |
| `top`, `left`, `right`, `bottom`, `zIndex` | number |
| `width`, `height`, `minWidth`, `minHeight`, `maxWidth`, `maxHeight` | number |

## Token Identity

Resolved theme tokens remain flat (`colors`, `dimensions`, `strings`) so
existing runtime consumers keep working. Source identity is preserved in a
sidecar map keyed by canonical token path:

```json
{
  "tokens": {
    "colors": { "accent": "#57a6ff" },
    "sourceIdentity": {
      "colors.accent": {
        "sourceId": "var-accent",
        "sourceCollection": "palette",
        "sourceMode": "dark",
        "sourceAdapter": "figma"
      }
    }
  }
}
```

## Asset Manifest

Every canonical DesignIR carries an asset manifest. It records local files,
HTTP(S) resources, CSS `url(...)` references, font URLs, and inline data URIs:

```json
{
  "version": 1,
  "assets": [
    {
      "asset_id": "asset-...",
      "original_uri": "meter.png",
      "local_path": "/abs/path/meter.png",
      "content_hash": "sha256-hex",
      "mime": "image/png",
      "width": 640,
      "height": 360,
      "font_family": "Inter",
      "license": "",
      "source_url": "",
      "diagnostics": []
    }
  ]
}
```

Network assets are not fetched by default. `pulp import-design --emit ir-json`
requires `--allow-network-fetch` for first-time HTTP(S) resolution. Fetched
bytes are cached by content hash, indexed by URL, and subsequent imports verify
any expected hash supplied with `--asset-hash <uri=sha256>`. When the import
source came from `--url`, relative asset references resolve against that source
URL; the manifest keeps the authored value in `original_uri` and stores the
resolved fetch target in `source_url`.

Phase 1 does not auto-load a previous manifest to detect remote drift. Callers
that need drift detection should pass the prior manifest's hash back through
`--asset-hash`; cache hits still verify that expected hash and emit
`asset-hash-mismatch` on disagreement.

External files and URLs keep distinct manifest entries even when their bytes
match, so diagnostics and provenance remain tied to the authored resource.
Data URIs are recorded with their decoded content hash so duplicated inline
assets can be deduplicated by later baked outputs. Nodes preserve their raw
URI attributes and may receive a parallel `*AssetId` attribute such as
`srcAssetId` or `backgroundImageAssetId` for stable manifest references.

## C++ API

```cpp
std::string serialize_design_ir(const DesignIR&, const DesignIrJsonOptions& = {});
DesignIR parse_design_ir_json(const std::string& json);
IRAssetManifest collect_design_ir_assets(const DesignIR&, const DesignIrAssetOptions& = {});
void refresh_design_ir_asset_manifest(DesignIR&, const DesignIrAssetOptions& = {});
```
