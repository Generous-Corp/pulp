# Headless extraction

The Pulp Figma plugin has two ways to produce a `.pulp.zip`:

1. **Interactive** — designer opens the plugin in Figma, clicks
   **Export to Pulp**. The published Community plugin path.
2. **Headless** — agents or scripts produce the same envelope without
   the click. Two flavours, picked by what's available:

| Path | Needs | Producer | When to pick |
|---|---|---|---|
| **REST** | A Figma PAT with `file_content:read` | [`tools/import-design/figma_rest_export.py`](../../import-design/figma_rest_export.py) (Python, Agent A's lane) | Default. CI-friendly, no Figma Desktop, batchable, works for any team file the PAT has access to. |
| **MCP** | Figma Desktop running + Figma MCP connected | `dist/headless.js` driven via an agent's `mcp__figma__use_figma` call | Conformance oracle (byte-identical to the published plugin) and dev sessions where Figma is already open. |

Both flavours emit the same `figma-plugin-export-v1` envelope (with
the `provenance.adapter` field distinguishing them). The plugin↔REST
conformance gate at
[`planning/fixtures/figma-plugin/conformance/`](../../../planning/fixtures/figma-plugin/conformance/)
catches drift between the two extractors on re-capture.

## REST flavor (production headless path)

Set up the token once:

```bash
# Generate a PAT at figma.com → Settings → Security → Personal access
# tokens (scope: file_content:read only). Then drop it at:
mkdir -p ~/.config/pulp
chmod 700 ~/.config/pulp
echo 'figd_YOUR_TOKEN' > ~/.config/pulp/figma-token
chmod 600 ~/.config/pulp/figma-token
```

Extract any node:

```bash
python3 tools/import-design/figma_rest_export.py \
  --file-key <FILE_KEY> --node <NODE_ID> \
  --out scene.pulp.json
# → writes scene.pulp.json + assets/<sha>.png next to it
```

URL convenience form (extracts `--file-key` + `--node` from a Figma URL):

```bash
python3 tools/import-design/figma_rest_export.py \
  --url 'https://figma.com/design/<KEY>/...?node-id=3-42' \
  --out scene.pulp.json
```

The `--no-assets` flag skips PNG capture for a geometry-only envelope.

The script writes `assets/<content_hash>.png` to a sibling directory.
To package into a `.pulp.zip` (which `pulp import-design --from
figma-plugin` consumes directly), zip `scene.pulp.json` plus the
`assets/` dir together. The wrapper script at
[`planning/fixtures/figma-plugin/build_font_smoke.py`](../../../planning/fixtures/figma-plugin/build_font_smoke.py)
is the simplest reference for that step (it bundles a single TTF
alongside the envelope; the same pattern works for the REST
extractor's PNG fallout).

## MCP flavor (conformance oracle + dev exploration)

Build the bundle:

```bash
cd tools/figma-plugin
npm run build         # produces dist/headless.js (~25 KB minified)
```

Generate the agent-ready JS payload:

```bash
node scripts/run-headless.mjs 26:3 > /tmp/payload.js
# Pass /tmp/payload.js's contents verbatim as the `code` parameter to
# mcp__figma__use_figma. Tool returns { envelope, envelope_json,
# assets, node_count, asset_count, ... } — pack envelope_json as
# scene.pulp.json + each assets[i].bytes as assets/<content_hash>.<ext>
# into a zip.
```

Pass `--selection` instead of a node id to fall back to whatever the
user has selected in Figma.

### Size discipline

`use_figma` caps its `code` parameter at 50 000 characters. The
headless build asserts `dist/headless.js` stays under 49 KB (50 KB
minus 1 KB headroom for the injected `TARGET_NODE_ID` prelude). If
the extractor grows past the cap, `npm run build` fails loudly. The
fix is to split shared code (the pure helpers are already in
`src/extract-pure.ts`); see [P2 in the headless roadmap](#).

### Response-size limit

The MCP tool result is itself capped at roughly 20 KB. For small-to-
medium designs (kitchen-sink ≈ 60 nodes ≈ 21 KB pretty-printed
envelope) this works end-to-end. For large designs (ELYSIUM ≈ 213
nodes ≈ 200 KB pretty envelope), the response truncates and you'd
only get the envelope head. Either (a) use the REST flavor above,
which has no response-size constraint, or (b) reduce the response by
having the headless bundle return `JSON.stringify(envelope)` (no
indent) — drops ~60 % of bytes. The REST flavor is the right move
in practice.

## Conformance check (plugin vs REST port)

Both extractors are independent implementations of the same envelope
contract. Drift is caught by a normaliser + structural diff:

```bash
python3 planning/fixtures/figma-plugin/conformance/diff_envelopes.py \
    planning/fixtures/figma-plugin/conformance/kitchen-sink-headless.pulp.json \
    planning/fixtures/figma-plugin/conformance/kitchen-sink-rest.pulp.json
```

The normaliser strips `provenance.exported_at` (timestamp),
`provenance.adapter` / `version` (extractor identity), asset_id naming
conventions, and other expected-to-differ fields. Anything left over
is real drift.

Re-capture both halves whenever `tools/figma-plugin/src/extract.ts` or
`serialize.ts` or `extract-pure.ts` changes, OR
`tools/import-design/figma_rest_export.py` changes, OR the source
Pulp Library file is edited.

The headless half currently has only the kitchen-sink fixture pinned;
ELYSIUM-scale captures require a different approach (response-size
constraint above). The REST half can pin arbitrarily large designs.

## Related files

| File | Purpose |
|---|---|
| `src/headless.ts` | Headless entry point — extractScene + serializeExport with no UI loop. Writes Promise to `globalThis.__pulp_headless_result` for the agent-side tail to await. |
| `src/extract-pure.ts` | Pure helpers (color/CSS, axis mappers, mapNodeType, font catalog, vector heuristic). Shared between code.ts + headless.ts, and the canonical mirror for the Python REST port. |
| `scripts/build.mjs` | esbuild driver. `buildHeadless()` asserts the 49 KB size budget. |
| `scripts/run-headless.mjs` | Node CLI that emits the agent-ready JS payload. |
| `tools/import-design/figma_rest_export.py` | Python REST port (Agent A's lane). Production headless path. |
| `planning/fixtures/figma-plugin/conformance/diff_envelopes.py` | Plugin↔REST drift detector. Normaliser + structural diff. |
| `planning/fixtures/figma-plugin/build_font_smoke.py` | Builds a synthetic `.pulp.zip` from any TTF/OTF — useful as a `.pulp.zip` packaging reference and for #43b font runtime testing. |
