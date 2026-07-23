---
name: import-design
description: Import designs from Figma, Stitch, v0, Pencil, React Native, or Claude Design into Pulp web-compat JS with automated visual validation. Claude Design imports also scaffold a pulp::view::EditorBridge handler file. Versioned parser, format, and compatibility-schema detection lives behind `--detect-only` and `--report-new-format`.
---

# Import Design

Import a design from an external tool (Figma, Stitch, v0, Pencil, React Native, Claude Design, or the experimental JSX runtime lane) into this Pulp project.

## TOOLS THIS SKILL ALREADY SHIPS — reach for these before hand-rolling (read this first)

Every one of these is documented further down this file. That was not enough:
an agent verifying an import hand-rolled PIL crop scripts instead, because the
guidance sat ~1,000 lines deep and a sibling skill told it to use PIL. If you
are about to write a script that opens two PNGs and diffs them, stop — it
exists:

| Need | Tool |
|---|---|
| **"It looks off" → which node, and by how many px** | `pulp-import-design … --validate --dump-layout L.json` then `python3 tools/import-design/layout_parity.py L.json` |
| **"It looks wrong but nothing failed" → was a property DROPPED?** | `node tools/import-design/material_audit.mjs --fig d.fig --frame F` — run this FIRST on any fidelity complaint |
| Labeled N-panel comparison montage | `python3 tools/import-design/montage.py --out cmp.png ...` |
| Per-widget fidelity audit + JSON report | `python3 tools/import-design/fidelity_diff.py --render r.png --scene scene.pulp.json --assets-dir DIR --frame-reference src.png` |
| Side-by-side + heatmap + top offending regions | `python3 tools/scripts/figma_import_diff.py` — use after EVERY codegen change |
| Render an import at the design's own canvas size | `tools/scripts/render-figma-import.sh` |
| Masked per-region diff vs a reference | `python3 tools/import-validation/diff_against_reference_regions.py` |
| Re-import regression vs a golden | `python3 tools/import-validation/golden_regression.py` |
| Rasterize Figma vector frames | `python3 tools/import-design/figma_rasterize_vector_frames.py` |

The full, machine-checked list is **`docs/status/tools.yaml`** (with inputs,
outputs, and availability for each), and its digest is generated into CLAUDE.md
so it is always in context. The table above is the fast path for this skill's
own work; the registry is the source of truth, and a coverage sweep in
`tools/scripts/tools_registry_check.py` fails CI if a tool lands here without
an entry — so nothing can go quiet the way `fidelity_diff.py` did.

**Check geometry BEFORE you look at pixels.** A `.fig` carries Figma's
already-SOLVED rect for every node — auto-layout children included — so where
each node belongs is a known number, not something to infer from a screenshot.
`--dump-layout` writes Pulp's laid-out rects plus the design's own (as a
`<stem>.geometry.json` sibling), and `layout_parity.py` joins them by node id:

```bash
pulp-import-design --from fig --file d.fig --frame 'Main' --output ui.js \
    --validate --screenshot-backend skia --dump-layout /tmp/layout.json
python3 tools/import-design/layout_parity.py /tmp/layout.json   # exit 1 = findings
```

`--validate` writes its render PNG **beside `--output`** (e.g. `ui.js` →
`<name>-<source>-render.png` in the same directory), not into the CWD — so it no
longer litters whatever directory you ran it from. Pass `--output` into a temp
dir to keep artifacts together.

This is pixel-free — no thresholds, no anti-aliasing noise — and it answers the
question a screenshot cannot: *which* node, off by *how much*, and under which
parent. Deltas are parent-relative and clustered by parent, so a shifted panel
is ONE finding rather than one per descendant, and a group of children sharing
one offset is reported as an alignment/padding drop on the parent by name.
Unmatched ids are listed as dropped/extra — a completeness check no pixel
heuristic can match. Reach for this FIRST when a layout looks wrong; the
montage/heatmap tools tell you *that* something moved, this tells you *what*.

`--gate-px 16` blocks only on displacement above 16px and reports the rest as
advisory drift — a correct import of the reference design tops out at 12px,
while a dropped auto-layout contract produces ten findings above 16px. The
`import-layout-parity-gate` ctest runs the whole pipeline on
`test/fixtures/imports/fig/synthetic.fig` and is strict (exact parity).

**It validates BOXES, not the ink inside them.** A label whose glyphs are shoved
to one side of a correctly-placed box, an icon drawn at the wrong scale inside
right-sized bounds, a colour, a gradient — all invisible to it, by construction.
This is not hypothetical: an icon-placement change that moved glyphs *within*
their correct boxes made `fg-icon` findings disappear and layout_parity went
greener while the render got worse. **Never read a clean layout_parity as "the
render is right."** It means the boxes are right.

EXTRA means "the render invented a node" — and only that. The synthetic
`<key>/mask-scope` clip wrappers the mask lowering adds (scene.mjs) have no
design counterpart *by construction*, so they are tallied separately
(`synthetic_extra` in the JSON, an "ignored N synthetic mask-scope node(s)"
line in the text) instead of polluting EXTRA with ~16 fixed false positives per
FX file. The whitelist is a narrow suffix match (`/mask-scope`, `" (mask
scope)"`), never a substring grep — a real node containing the marker mid-id
still counts as EXTRA.

The other half is TWO tools, not one, because they fail differently.
`material_audit.mjs` counts what the `.fig` DECLARES against what the import
emits, so it answers "was this dropped?" — deterministically, with no reference
image. `thumb_parity.py` compares block means against Figma's own raster, so it
answers "is the colour roughly right?" — and it is blind to anything that
preserves a region's mean. Reach for the audit first: a property that never
survived cannot be diagnosed by looking at pixels, and it is the cheaper
question. Neither proves the render is CORRECT; a human looking at a montage is
still the final say. (The audit's checks iterate declared *occurrences*, so a
node declaring the same property twice — knob bases carry doubled DROP_SHADOWs
in the reference files — would print the same finding line twice;
`dedupFindings` collapses byte-identical findings at the report boundary while
the count tables keep tallying occurrences.)

The one thing this design's fg-icon bug proves about all of them: **every checker
here went green while the icons were visibly broken.** layout_parity said the
boxes were right — and they were, because the box had collapsed to width 0 and
the glyph inside it was absolutely positioned, so the finding it *did* report
(`dx=+6.5 dw=-12`) read as a placement problem rather than the sizing one it was.
Read a tool's number as the answer to the question it asks, never as "the import
is fine."

**Free offline ground truth:** every `.fig` is a ZIP containing `thumbnail.png`
(Figma's own raster of the design) and a `meta.json` whose `render_coordinates`
+ `thumbnail_size` give an EXACT canvas→thumbnail transform — no image
alignment, no MCP, no REST, no rate limit. Note the thumbnail is ~0.4× (400 px
wide for a 1004 px design), so it adjudicates layout, colour, and presence —
NOT glyph-level detail. Do not draw fine conclusions from a 5× upscale of it.

**`--validate` renders at the design's own canvas size** by default. Do not
pass `--render-size` unless you specifically want a different size; a mismatched
render and reference makes every similarity score meaningless.

## LOCAL-FIRST — never start with the REST script when Figma desktop is open (read this first)

The headless REST exporter (`figma_rest_export.py`, used in the steps below) is
the **CI / true-headless fallback**. On a dense real file it **WILL be
rate-limited** (HTTP 429): Figma's `/images` render endpoint is a strict Tier-1
budget keyed to the *file's* plan, so a big frame can 429 for many minutes.

**But the Figma MCP is ALSO quota-limited — do not treat it as free.** On a
**View/Collab seat the MCP is 6 tool calls per MONTH** (any plan); Dev/Full seats
get 200–600/day + a per-minute cap. Every *read* tool counts (`get_metadata`,
`get_screenshot`, `get_design_context`); write tools (`whoami`,
`generate_figma_design`) are exempt. This is a hard MONTHLY quota — **backoff
cannot clear it**. So be maximally frugal. Order of preference, smartest first:

0. **Reuse a CACHED artifact.** Prior sessions cache Figma PNGs/scenes under the
   session scratchpad. `find … -iname '<file>*'` BEFORE spending a call — a cached
   source screenshot suffices for source-vs-implementation checks with zero calls.
1. **Export a scene for import** → the **Pulp Figma desktop plugin**
   (`tools/figma-plugin`) exports the `figma-plugin-export-v1` envelope (SVG +
   node tree) directly from the open file — **no MCP tool call, no REST budget**.
   This is the truly-unlimited local path; once you hold the envelope, all
   importer/render work is offline forever. Prefer this to spend ZERO quota.
2. **Inspect / verify a design** → the **Figma desktop MCP**, but BUDGET the
   6/month. When you must call, use ONE `get_design_context` (reference code +
   screenshot + metadata together) instead of separate `get_metadata` +
   `get_screenshot`, and never re-fetch what you cached.
3. **Only in true headless / CI** (no desktop, no MCP) → `figma_rest_export.py`.
   Its `figma_get` honors `429 Retry-After` with backoff; if it 429s it prints a
   loud one-time advisory pointing back here — **switch to (0)/(1), don't wait out
   6×300s of backoff.** Pass **`--cache-dir DIR`** to memoize the two REST-heavy
   payloads (the `/nodes` geometry JSON + the frame SVG) per `(file_key, node_id)`:
   the first run populates the cache, every re-run reads it with **ZERO REST calls**
   and needs **no token** — so iterating on the importer against a real Triaz frame
   costs one fetch, not one per test. `--refresh-cache` busts it. This is the
   transparent form of the manual `--node-json` / `--frame-svg` inputs.

Everything below documents lane (3)'s mechanics because it's the scriptable one,
but the ordering above governs *which lane to start in*. `figma_rest_export.py`
mirrors the desktop plugin field-for-field, so a scene from lane (2) or (3) is
interchangeable downstream.

**There is no lane (4): `pulp import-design --url` cannot import from Figma.**
Do not reach for `--from figma --url 'https://figma.com/design/…'` — the CLI
help and docs advertised it for a long time, but it never worked. `--url` is a
bare unauthenticated `curl -fsSL` (`fetch_url_to_file`) and the CLI has no
credential flag at all, so figma.com returns **403** for a private file and the
web app's **HTML shell** for a public one; the shell then dies deep inside
`choc::json::parse` with an error that looks like a parser bug rather than an
auth problem. The CLI now rejects figma.com `design/`/`file/`/`proto/` URLs up
front (`tools/import-design/figma_url.hpp`) and names the lanes above. Note the
two `--url` flags are **different**: `figma_rest_export.py --url` is real — it
parses `file-key` + `node-id` out of the link and fetches with a token. The only
authenticated Figma REST client in the repo is that script; nothing in the C++
CLI can talk to Figma.

## Figma → Pulp, faithful (1:1) — THE WORKING LANE (read first)

When the goal is a **visually faithful (1:1)** import of a component that lives in
the **Figma file** (e.g. the Ink & Signal library, file key `q9iDYZzg86YrOQKr6I3bY0`),
use the **figma-plugin faithful-vector lane**. This is the lane that actually
reproduces the design; the others below do NOT and waste hours:

- ❌ **Do NOT hand-write a C++ `paint()`** to mimic the design. It is never 1:1
  (SVG icons, gradients, shadows, pills) and is pure slop. The framework exists
  to render the design, not to re-draw it by hand.
- ❌ **Do NOT use `--from claude` for layout.** On a standalone/bundled HTML it
  falls back to regex *text* extraction ("0 widgets, N labels", ~58% and it even
  scrapes CSS comments) — no CSS layout, no geometry.

**The lane that works (Figma is the source of truth):**
```bash
# 1) Export the Figma NODE to a scene (faithful vectors + geometry + assets).
#    Token resolves from --token, $FIGMA_TOKEN, then ~/.config/pulp/figma-token.
#    ALWAYS pass --cache-dir: it memoizes the /nodes JSON + frame SVG, so a
#    fix-loop iteration costs ZERO REST calls on a cache hit. Figma MCP allows
#    only ~6 calls/MONTH on a View seat — re-fetching every iteration burns the
#    quota for nothing.
python3 tools/import-design/figma_rest_export.py \
  --file-key q9iDYZzg86YrOQKr6I3bY0 --node 187:2 \
  --cache-dir .pulp-figma-cache \
  --out scene.pulp.json            # → "N nodes, faithful-vector SVG, interactive elements"

# 2) Import the scene (the source-of-truth lane: audio-widget + library matching,
#    faithful-vector render) and validate against the Figma render.
build-gpu/tools/import-design/pulp-import-design \
  --from figma-plugin --file scene.pulp.json --output ui.js \
  --validate --screenshot-backend skia \
  --reference <figma-node-render.png> --diff diff.png --render-size 1356x781
```

### Offline `.fig` lane (`--from fig`) — no account, no network

When you have a **local Figma save file** (`File → Save local copy…`, a `.fig`),
`--from fig` decodes it offline — no Figma account, PAT, MCP, or network. It
produces the same `figma-plugin` envelope as `figma_rest_export.py`, then runs
the standard figma-plugin lane, so everything above (audio-widget/library
matching, faithful-vector render, `--validate`) applies unchanged. Requires
Node ≥ 22 on PATH (native zstd); the decoder is `tools/import-design/fig_decode.mjs`.

A `.fig` can carry hundreds of frames across many pages, so **outline first,
then pick a frame** — importing the whole file is never right:
```bash
# 1) Inventory the file (read-only): pages → frames with guid, size, node count.
pulp import-design --from fig --file design.fig --outline          # or --outline --json

# 2) Import ONE frame, by name or guid (from the outline). --page scopes the lookup.
pulp import-design --from fig --file design.fig --frame '102:1624' --output ui.js
```
Frame selection accepts a guid (`102:1624`, unambiguous) or an exact
case-insensitive name.

**Mirrored layers: `m02`/`m12` is the transform ORIGIN, not the box corner.**
A `.fig` transform with `m00 = -1` (horizontal flip — Figma's ⇋ on an icon)
stores the box's RIGHT edge in `m02`; the visual box spans `[m02 - w, m02]`.
Three consequences, all learned from the Triaz "Reverse/Env" chips
(`mirrorAwareOrigin` in `scene.mjs` owns the rule):

- *Sidecar truth*: `geometry.json` must report the visual min-corner, or
  `layout_parity` cries "misplaced by exactly one width" (`dx = -10.4` = the
  icon's own width) at a node the flex pass placed perfectly.
- *Container flips are dropped*: an emitted frame carries no `scaleX(-1)`, so
  a vector under a net-mirrored ancestor chain must have the ancestor mirror
  baked into its own normalized path (`geometryToPath`'s `mirror` arg) — the
  icon frame's flip cancels its child Union's own baked flip in Figma, and
  without the re-bake the "reverse" arrow renders pointing forwards.
- *Scope*: TRUE mirrors only (exactly one negative axis). BOTH axes negative
  is a 180° rotation whose raw-`m02` placement is pinned by the slider-fill
  lesson (see the orthogonal-rotation test in `fig.test.mjs`) — do not
  "fix" it to min-corner.

**A flowing vector's flex slot is its NODE box, not its ink.** Figma's
auto-layout never sees a stroke, but the emitted ink box includes it (CENTER/
OUTSIDE bands, round caps overhang). The decoder reconciles the two with
negative margins on the child's `layout` (widget keeps the exact ink; Yoga
gets the node box back) — gated to overhangs ≤ `strokeWeight + 1`, because
through instance expansion the ink is solved at INSTANCE scale while
`node.size` can still be the master's, and that gap is a scale delta, not a
stroke. The JS codegen lowers `layout.margin_*` via `setFlex` — it silently
dropped them before, so if margins ever stop landing, check
`emit_js_layout_constraints` in `design_codegen.cpp` first.

**Known residual (parity, not render): resized-instance auto-layout re-solve.**
A 32×32 INSTANCE of a 40×40 auto-layout SYMBOL re-solves child layout at
32×32 in both Figma and our Yoga pass, but the geometry sidecar composes the
MASTER's 40×40-solved child transforms, so parity reports a phantom offset on
such children (designers-pick "HiHat 01"/"Cymbal 01", `dx,dy ≈ -4`). Truth-side
fix would need the decoder to re-solve (or read `derivedSymbolData` layout for
swapped subtrees); the render itself matches Figma's re-layout behavior.

**The `.fig` lane cannot capture multi-state designs.** Alternate frames are
lowered ONLY by the faithful_svg path, and the `.fig` decoder emits a
widget-recognition tree — no `render_mode`, no `svg_asset_id` (grep
`tools/import-design/fig/*.mjs`: zero hits). Repeated `--frame` here is refused
with exit 2. The lane's merge plumbing is written and tested and will start
working the day the decoder learns faithful export; until then, do NOT wire a
multi-state surface onto it.

**Multi-state capture is `--file` repeated, on a faithful lane.** Export one
envelope per state with a lane that emits faithful frames — the REST
faithful-vector export (`figma_rest_export.py --faithful-vector`) or the Figma
plugin — then merge them at import:

```bash
python3 tools/import-design/figma_rest_export.py \
  --file-key <KEY> --node 187:15 --out typing.pulp.json --faithful-vector
python3 tools/import-design/figma_rest_export.py \
  --file-key <KEY> --node 187:349 --out piano.pulp.json --faithful-vector
pulp import-design --from figma-plugin \
  --file typing.pulp.json --file piano.pulp.json \
  --emit cpp --mode baked --output kbd.cpp
```

The first `--file` is frame 0, the second frame 1, … — that index is what a
design's `swap <n>` layer targets, so **reordering the flags re-points every
swap**. The merge (`envelope_merge.cpp`, shared with the fig lane) folds each
later envelope's root into the first's `alternate_frames`; the C++ codegen and
the native materializer each emit one `add_frame` per entry, in order. A single
`--file` skips the merge entirely and behaves exactly as it always has — keep
it that way when touching this lane.

**Captured states nobody can render are exit 2, not a diagnostic.**
`find_unrenderable_alternate_frames` reports every node carrying
`alternate_frames` that is not a renderable faithful node; the CLI refuses the
import. Without it, the states are dropped and the import "succeeds" with one
frame — the user asks for N states and silently gets one. That silent no-op is
what this surface shipped as before the guard existed; keep the guard.

**Swap elements come from the plugin lane only.** `faithful-vector.ts` reads a
layer named `swap <n>` into a swap element; the REST exporter does not detect
swaps. A REST-captured multi-state component holds all its frames but is driven
by `set_active_frame(i)` from consumer code.

**A swap whose target frame was never captured is reported, never silently
dead.** `apply_swap_target_verification` flags an unset (`-1`), negative,
out-of-range, or self-targeting swap with a conflict signal, so it rides the
SAME channel as any other unresolved control: `--import-report <json>` shows it and
`--fail-on-unresolved` exits 2. Do NOT add a parallel diagnostic channel for
frame problems — route through the import report. This fires on single-frame
imports too (a design whose swap was always dead now says so).

**Gotcha — an alternate frame is a SIBLING axis to `children`, not a
descendant.** Any pass that walks the IR and must see every control has to
descend `alternate_frames` as well (`collect_import_report` and
`apply_placement_verification` both do). A walk that only follows `children`
silently ignores every control on frame 1+. Each alternate is also its own
render region: verify its overlays against ITS width/height, not frame 0's — a
mode toggle routinely swaps to a differently sized frame.

**Gotcha — never skip an alternate frame whose SVG fails to resolve.** Frame
indices are positional, so dropping frame k renumbers every later frame and
silently re-points the swaps that target them. Both generators add the frame
blank (overlays intact) and diagnose it instead. The decoder is purely structural — geometry, style, text,
and bundled raster assets; **widget recognition stays the importer's job** (a
node's name flows through untouched for the resolver to classify). Fidelity
losses are reported as named warnings in the emitted envelope's `diagnostics`
(`vector-simplified`, `gradient-approximated`, `asset-missing`) rather than
silently dropped. External-library instances and cross-file variables that a
local file can't resolve surface the same way — treat them as data, not failures,
and fill in the critical ones by hand.

**Gotcha — a hand-written envelope fixture using `data:` URI SVG assets loses
its faithful node under `--mode baked`.** The baked lanes run
`refresh_design_ir_asset_manifest`, which resolves assets against the filesystem
and drops inline `data:` entries ("0 assets"); the faithful node then can't
resolve its SVG and falls back to plain widgets, so `render_mode`,
`svg_asset_id`, and `interactive_elements` all vanish from the output and the
import report goes empty. This looks exactly like a codegen bug and is not one.
Write real `.svg` files and reference them by relative `local_path` (what a real
`.fig` decode emits). Note also that `--emit cpp` requires `--mode baked`, and
`looks_like_figma_plugin_export` keys off `figma-plugin-v1` /
`"adapter": "figma-plugin"` — a fixture missing those but containing `"version"`
+ `"root"` is parsed as a serialized DesignIR instead of an envelope.

**Gotcha — the `.fig` fixture-determinism test compares decoded content, not raw
bytes.** `fig.test.mjs`'s "generator output is deterministic" test regenerates
`synthetic.fig` and compares it to the committed fixture. It must compare the
*decompressed* schema + message + rasters (`unpackFig(...)`), never the raw file
bytes: the inner fig-kiwi chunks are DEFLATE-compressed, and zlib's exact output
varies by Node/zlib version, so a raw byte-compare tests zlib rather than the
generator and goes red on any CI host whose zlib differs from the one that
committed the fixture (this is exactly what reddened a VM runner while the same
commit stayed green on bare-metal). If you touch `make_synthetic_fig.mjs`,
regenerate the fixture from the canonical toolchain, but keep the comparison at
the decoded layer.

**Component/instance semantics are preserved end-to-end via one normalized
figma-block contract.** All three Figma producers emit the same field names in
a node's `figma` block — `component_key`, `main_component_name`,
`main_component_id`, `component_set_name`, `remote_library` (emitted only when
true), `variant_properties` (`{axis: value}`), and `component_properties`
(`{name: {type, value}}`, type ∈ TEXT/BOOLEAN/NUMBER/VARIANT/INSTANCE_SWAP…) —
and `design_ir_json.cpp::parse_ir_node` preserves all of them into namespaced
node attributes: `figmaComponentKey`, `figmaMainComponentName`,
`figmaMainComponentId`, `figmaComponentSetName`, `figmaRemoteLibrary`,
`figmaVariant.<axis>`, `figmaComponentProperty.<name>` +
`figmaComponentPropertyType.<name>`. The consumer strips Figma's `"#<id>"`
property-name uniquifier (colliding stripped names fall back to the raw key)
and stringifies values (bool → `"true"`/`"false"`, round numbers without
`.0`). Per lane: the plugin captures instance `componentProperties` /
`variantProperties` directly; REST derives `variant_properties` from
VARIANT-typed `componentProperties` entries (REST has no separate variant map)
and reads `remote` / set name from the `/nodes` components maps; the `.fig`
decoder resolves a state-group member's `variantPropSpecs` through the set's
`componentPropDefs` (member-name `"a=b, c=d"` parse as fallback) and modern
`componentPropAssignments` (`{defID, value}`; `value.textValue` is a TextData
struct — the string is `.characters`; INSTANCE_SWAP `guidValue` resolves to
the swapped-in component's name when in-file). A kiwi COMPONENT_SET is a FRAME
with `isStateGroup: true` — the set, not the member SYMBOL, owns the VARIANT
defs. Nothing is fabricated: legacy files (no assignments) emit identity only.
SLOT nodes carry no component identity in the Plugin API (`SlotNode` is a
frame defined by a component property reference), so only their name flows.

**Mixed text runs are emitted by all three Figma lanes, on ONE offset unit:
UTF-8 bytes.** A text node with per-range styling (a bold word, a colored
span) carries an ordered `runs` array of style DELTAS against the dominant
style — `{start, end, fontSize?, fontWeight?, fontStyle?, color?,
letterSpacing?, textDecoration?}` (camelCase; the exact shape
`design_ir_json.cpp::parse_ir_text_runs` reads). `start`/`end` are `[start,
end)` **UTF-8 byte offsets into `content`** in every lane — Figma indexes text
in UTF-16 code units (plugin `getStyledTextSegments`, REST
`characterStyleOverrides`, `.fig` `TextData.characterStyleIDs` all do), so
each producer converts through a per-code-point map (plugin
`extract-pure.ts::utf16ToUtf8ByteOffsets`, REST's `u16_to_byte`, `.fig`
`extractFigTextRuns`). A code-unit passthrough corrupts every run after the
first non-ASCII character — the tests pin an `é`/emoji fixture in each lane.
Homogeneous text emits NO runs array (the flat dominant style is the whole
story; the consumer prefers that path). Lane quirks: the plugin backfills the
dominant style from segment 0 when node-level reads return `figma.mixed` (a
symbol — `typeof` guards silently skip it otherwise); the `.fig` format has no
numeric fontWeight, so run weights derive from the override `fontName.style`
NAME ("Bold" → 700, "SemiBold" → 600, …); `.fig` styleOverrideTable rows are
NodeChange structs keyed by `styleID`. Codegen coverage (compat
`text-per-range-styles`): web-compat nested `<span>`s, JS-native
`setTextRuns` → AttributedString (single-line), SwiftUI concatenated `Text`
segments; baked C++ and live-native flatten — honest partial.

**`textAlignVertical` is design authority; the tall-slot heuristic is only a
fallback.** All three producers emit `style.vertical_align`
(top/middle/bottom; kiwi omits the `.fig` TOP default so it only appears when
set). Codegen honors it verbatim in both JS arms — including an explicit
`top`, which SUPPRESSES the old slot-taller-than-font centering guess (that
heuristic now fires only when the source never says). Expect imports to MOVE
text vs pre-emission renders where designs author TOP in a reserved slot —
that movement is fidelity, not a regression (layout dumps stay byte-identical;
vertical align is paint, not geometry). Auto-resize / truncation / max-lines /
whole-node hyperlink are preserved as namespaced attributes
(`figma:text_auto_resize`, `figma:text_truncation`, `figma:max_lines`,
`figma:hyperlink`), not lowered; paragraph/list/OpenType metadata is not
captured (compat `text-extended-metadata`).

**Gotcha - old-style instance swap lives in `overriddenSymbolID`, not
componentPropAssignments.** A file that predates component properties swaps a
nested instance's component with a `symbolOverrides` entry carrying
`overriddenSymbolID` — there are no `componentPropAssignments` /
`componentPropertyReferences` anywhere, so searching for the modern swap
machinery concludes "no swap" while every channel's icon IS swapped.
`expandInstance` (scene.mjs) honors the field: applyOverrideEntry's generic
copy lands it on the clone, and the expansion re-points at that master when it
resolves in-file (falling back to the authored `symbolData.symbolID` plus an
`external-component` diagnostic when it doesn't). Deeper override paths keep
resolving after the swap because the swap target's children carry the matching
`overrideKey`s. Symptom when broken: N siblings render identical component
content under N correct per-instance text overrides (sixteen mixer channels,
one kick-drum icon).

**Gotcha - Figma "Clip content" is `frameMaskDisabled` (inverted), and groups
never clip.** The `.fig` decoder emits `style.overflow = 'clip'` for a
FRAME/SYMBOL/INSTANCE with `frameMaskDisabled: false` unless the node is a
GROUP (`resizeToFit: true` — groups store the flag but ignore it). The REST
lane's equivalent is `clipsContent → overflow`. The native JS codegen lowers a
non-default `style.overflow` to `setOverflow(id, 'clip')` (bridge maps clip →
`View::Overflow::hidden`); `visible` is the View default and is deliberately
not emitted. This matters for expanded instances: a master whose decoration
overhangs its symbol bounds renders clipped in Figma, so an unclipped import
paints the overhang over whatever sits below the instance (a channel strip's
noise card ran 19px past its 235px symbol and buried the transport's step row).

**Gotcha - a `mask: true` child paints NOWHERE; materializing it as content
occludes everything painted after it.** Figma's mask layer clips the siblings
painted ABOVE it in the same parent and never renders its own fill. ALL THREE
Figma lanes now reconstruct this sibling-mask structure with one wrapper
contract: the masked siblings move into a synthetic
`<mask name> (mask scope)` wrapper (spans the parent, `audio_widget: 'none'`,
node_id `<maskId>/mask-scope`) whose `style.clip_path = path("<d>")` carries
the mask outline in PARENT space. Per lane: the `.fig` decoder
(`scene.mjs::walkChildren` + `maskClipOutline`/`boxMaskOutline`;
`geometryToClipPath` in paths.mjs skips the 0,0-viewBox normalization
`geometryToPath` does because a CSS clip-path is consumed in the clipped
view's border-box space), the plugin (`extract.ts::beginMaskScope` over the
pure helpers in `extract-pure.ts` — `maskClipOutline` transforms
`fillGeometry` into parent space via inv(parent.absoluteTransform) ∘
node.absoluteTransform, group-proof; box-model masks get a synthesized
outline), and REST (`figma_rest_export.py::_begin_mask_scope`, same helpers
in Python; axis-aligned masks translate by absoluteBoundingBox deltas, robust
to REST's GROUP-parent coordinate quirks). The chain downstream already
existed end-to-end (`parse_ir_style('clipPath')` → codegen `setClipPath` →
`SkPath::FromSVGString` clip). Siblings BELOW the mask stay outside the
wrapper (Figma's scope); a second mask opens a nested wrapper so stacked
masks intersect. Fidelity is diagnosed, never silent: an outline clip is
exact for VECTOR (outline) masks and opaque-solid alpha masks; LUMINANCE
masks keep the best-effort outline clip plus a `mask-luminance-approximated`
warning, soft/partial alpha masks (image/gradient fill, partial opacity) get
`complex-mask-flattened` (plugin/REST; the `.fig` lane keeps its original
`mask-approximated` code), and unresolvable outlines / masks inside
auto-layout parents degrade with `mask-approximated` — but the mask itself is
never painted, in any branch of any lane. Arcs in vector mask geometry don't
survive a general affine, so an `A` command falls back to the box outline
(approximate clip beats no clip). Symptom when broken: a "gray" element whose
accent color is right there in the data — the selected mixer channel's red
tab read gray because the master's opaque `Bg PAnel` mask (invisible in
Figma) painted over it, and every channel body sat one gray lighter than the
design.

**Gotcha - a Figma slider stores a value-driven fill position that can detach
from the thumb.** A slider component (track + progress fill + round thumb) keeps
the fill's x/width per-instance; Figma's LIVE component render recomputes the
fill against the thumb at draw time, but the stored `.fig` (and REST/plugin
exports of it) only carry the frozen geometry - so an instance can persist a fill
that floats in a gap away from the thumb, and a faithful render draws a broken
detached bar. `reconnect_slider_fill` (`core/view/src/design_import.cpp`, run in
the native codegen arm BEFORE `synthesize_primitive_paths` bakes the width into
`path_data`) detects the triplet STRUCTURALLY - short wide container,
near-full-width thin track, shorter thin fill whose color differs from the track,
round thumb whose height fills the bar; never by layer name - and bridges the
fill to the thumb ONLY when they are horizontally disjoint. Do not "fix" a
detached fill by inventing a slider value or by matching on names: the structural
gap-bridge is the whole intervention, and a fill already touching its thumb (plus
track+thumb-only faders and every non-slider row) is left exactly as stored.
Covered by the `[slider]` cases in `test_design_import_codegen.cpp`.

**Gotcha - a "universal" IR fix is only universal across the emit paths that
carry it.** `normalize_border_shorthand` splits `style.border` into
`border_color`/`border_width` for every lane, but the border only PAINTS if the
node's emit path calls `setBorder`. `design_codegen.cpp` has TWO native-frame emit
paths: `emit_js_container` (recognized containers) and `emit_js_generic_frame`
(the fall-through for a childless node whose kind is neither
container/widget/vector/image/text — a v0/claude/stitch `button`/`canvas`/`input`
div). `emit_js_generic_frame` emitted background/gradient/corner-radius but NOT
setBorder, so a bordered generic-frame node silently lost its stroke on EVERY
lane even though the shorthand was split correctly. When you add a style emit to
one frame path, add it to BOTH — and verify a real lane, not a grep: import
`test/fixtures/v0-dev/audio-control-panel.tsx` and count declared-vs-emitted
(`setBorder`/`setBackgroundGradient`/`setCornerRadius`) in the output JS. A
grep-level "the field reaches the IR" check misses a drop that lives one layer
down in codegen.

**Gotcha - the v0 TSX parser does not resolve `style={constObject}`
references.** `extract_jsx_style_body` (`design_import_v0_tsx.cpp`) resolves only
inline `style={{...}}` object literals. A hand-authored/v0 pattern that hoists
the style into `const panelStyle = {...}` and passes `style={panelStyle}` is
dropped whole — on `audio-control-panel.tsx` the root panel loses its
background, padding, border, and radius. Resolving it means extracting
`const …Style = {…}` decls and threading a registry through
`apply_v0_jsx_attribute`; treat it as a focused parser change with its own tests,
not a one-liner.

**Gotcha - the HTML regex fallback (`parse_stitch_html`, shared by the claude
lane via `parse_claude_html`) must skip non-visible elements.** Its
`<([a-z][a-z0-9]*)[^>]*>([^<]*)</\1>` regex matches ANY tag, so a `<script>` body
(a Stitch `tailwind.config = {...}` block, a Claude bundler placeholder) or a
`<style>` sheet became a visible text LABEL — raw JS/CSS painted into the
imported UI. The fix filters a `kNonVisibleTags` set
(script/style/noscript/template/head/title/meta/link/base). When you touch this
fallback, verify with a real fixture: import
`test/fixtures/imports/stitch/2025.04/code.html` and confirm the element count
drops (the `<script>` label is gone) while the visible `<div>` text survives.
Note this is the LOSSY fallback — a JSON-IR or runtime-DOM claude/stitch input
never reaches it; it fires only on raw non-JSON HTML.

**Gotcha - `.fig` layer rotation was dropped, so a rotated needle rendered as an
axis-aligned stub.** `scene.mjs`'s `styleFor` took only the translation column
(`m02`/`m12`) of a node's affine transform and threw away the rotation
(`m00/m01/m10/m11`). A knob's value needle — a thin ROUNDED_RECTANGLE rotated to
the value angle — then imported as a vertical bar floating off-centre instead of
a radial pointer (reported on TRIAZ "Rnd Pan"). The fix extracts
`atan2(m10, m00)`, emits `transform: rotate(<deg>deg)`, and **compensates
left/top for the renderer's centre transform-origin** (Figma rotates about the
layer origin; the view rotates about centre) — so NO `setTransformOrigin` is
emitted and CSS-lane `rotate()` (which is also centre-pivot) stays correct. The
native codegen lowers `transform: rotate()` to `setRotation` in the shared
`emit_js_visual_overrides`. TWO scope guards, both load-bearing:
1. **Non-orthogonal only.** Apply the transform ONLY when the angle is not a
   multiple of 90deg (`mod90 > 0.5 && mod90 < 89.5`). A multiple of 90deg keeps a
   rect axis-aligned, and for a solid fill a 180deg spin is a visual no-op — the
   centre-pivot compensation then only shifts the box off its row. That exact
   case floated a slider's 180deg-rotated progress fill ABOVE its track (a
   regression from the first cut of this fix). Orthogonal rotations fall through
   to plain `m02`/`m12` placement.
2. **Box-model only.** A `VECTOR_LIKE` node bakes its rotation into `path_data`,
   so re-applying it double-rotates the glyph (guard `!VECTOR_LIKE.has(node.type)`;
   verify a rotated icon like the "Reverse" ↩ button is byte-identical
   before/after).
Covered by `[rotation]` in `test_design_import_codegen.cpp` + a decoder test in
`fig/fig.test.mjs` (45deg needle rotates; VECTOR and 180deg fill do not).

**All three lanes now lower rotation** (audit "Rotation / transform" row). The
plugin (`extract.ts` walk + `extract-pure.ts::decodeRelativeTransform`) and
REST (`figma_rest_export.py::_decode_rotation`) producers decode
`relativeTransform` and emit the same `transform: rotate(<deg>deg)` spelling,
with the `.fig` guards mirrored field-for-field (non-orthogonal tolerance
`0.5 < mod90 < 89.5`; vector-like exclusion — extended in plugin/REST to EVERY
node-export capture, i.e. widget-instance and pure-vector-illustration PNGs,
because `exportAsync`/the REST images render bakes the rotation into the
pixels; an image FILL asset is raw bytes, not a node render, so image-filled
boxes still rotate). Placement differs from `.fig` by design: plugin/REST
position by absoluteBoundingBox deltas, and the rotated AABB's center IS the
node's center, so they emit the UNTRANSFORMED size (`node.width/height`,
REST `size` — both ride with the export) centered in the AABB and let the
center-pivot `rotate()` land it (the origin-pivot compensation formula is only
the plugin's `node.x/y` fallback path). Two additions the `.fig` lane does not
have:
1. **Skew / non-unit scale / mirror-plus-rotation** is NOT representable as a
   single center `rotate()` — the producers raise a
   `transform-skew-approximated` warning (`unsupported_property`), keep the
   node axis-aligned at its AABB (the pre-fix behavior), and never fake an
   angle. A pure orthogonal flip stays silent (axis-aligned box already
   occupies the right pixels — matches all shipped lanes).
2. **`figma:transform_matrix`** — the full 2x3 affine
   (`"m00,m01,m02,m10,m11,m12"`, row-major, trimmed like the geometry attrs)
   is preserved as a namespaced provenance attribute on every rotated or
   diagnosed node, so a future matrix-capable renderer needs no re-export.
   Same documented-provenance-sink contract as the `figma:arc_data` /
   `figma:stroke_*` attrs; nothing consumes it today. The `.fig` lane does
   not emit it (its rotation logic is frozen; envelope stays byte-identical).
Tests: `transform.test.ts` (plugin decode), rotation cases in
`test_figma_rest_export.py` (placement math, skew diagnostic, vector-leaf and
flowing-child exclusions), and plugin/REST end-to-end `[rotation]` cases in
`test_design_import_codegen.cpp`.

**Gotcha - a "label" token names the caption, not the control.**
`detect_audio_widget` (`design_import.cpp`) whole-token-matches "knob"/"fader"/…
to promote a node to a built-in widget. A node named `sound / knob label`
tokenizes to {sound, knob, label} and matched "knob", so the caption FRAME
promoted to a knob and painted a stock knob disc over its text (a "Classic"
filter-mode caption imported as a dark knob). The recognizer now returns `none`
whenever the name carries a `label` token — the caption is text; the real
control (`sound / knob / small unipolar`, no `label`) keeps its recognition.
This is the same "the layer name IS the art, not a control" rule as the
knob-art-layer suppression, one level up. Verified: `createKnob` count on a real
`.fig` dropped from 6 (all captions) to 0 while every art-layer knob still
rendered. Covered in `detect_audio_widget` tests.

**Gotcha - a stroke-band vector must not ALSO carry a CSS border.** Figma stores
a stroke as an already-expanded fillable band (`strokeGeometry`); `geometryToPath`
resolves it and `scene.mjs` paints it as a FILL in the stroke color (re-stroking
it would outline the outline). But the generic stroke→`border` lowering
(`strokePaints` + `strokeWeight` → `style.border`) then fired on that same node,
so codegen filled the band AND stroked it — two parallel lines where the design
has one (a triad-pad triangle and every stroked ring rendered doubled/too-thick;
the button rings even read as dark filled discs). The fix tracks
`vectorStrokeExpressed` (band painted OR stroke channels lowered) and skips the
border for those nodes. Covered by a materialize-level test in
`fig/paths.test.mjs`. One general fix cleared the triangle weight AND the
transport △○□ button rings at once.

**Stroke survival — the four lanes a Figma stroke renders through** (the
stroke-survival fix; `fig/scene.mjs` `lowerStrokeChannels` + `fig/paths.mjs`):

1. **Stroke-only vector, CENTER align** — the baked `strokeGeometry` band is
   exact (caps/joins/dashes realized in the outline); painted as a FILL in the
   stroke color. Unchanged, still the faithful path.
2. **Fill + stroke on one vector** — the stroke rides the SAME path as real
   stroke channels (`stroke`/`strokeGradient`/`strokeWidth` →
   `setSvgStroke`/`setSvgStrokeGradient`/`setSvgStrokeWidth`): SvgPathWidget
   fills then strokes one path, and for CENTER align the fill outline IS the
   stroke centerline, so this is exact. No CSS border (would double the edge).
   Fractional widths survive (the border lane rounded to whole px).
3. **INSIDE/OUTSIDE-aligned stroke bands are baked UNCLIPPED** — Figma's blob
   holds the boundary outlined at ±weight (verified: a weight-2 INSIDE
   `Polygon 5` carries a 4px band; render-time clipping against the fill
   region is what trims it, and we don't clip). Filling that band verbatim
   painted the XY-pad triangle fat and bright — a #373737 grey where Figma renders #2b2b2b. The
   decoder now prefers the FILL outline + a centered stroke channel — right
   width and color, half a weight off in position — and raises
   `stroke-align-approximated`.
4. **Gradient strokes** — `GRADIENT_LINEAR` lowers to the `strokeGradient`
   channel end-to-end (`SvgPathWidget::set_stroke_gradient` →
   `Canvas::set_stroke_gradient_linear`; solid composite kept beside it as the
   parse-failure fallback). Works on vectors AND on ellipse primitives whose
   path is synthesized later — `design_ir_json.cpp` captures the stroke
   channels UNCONDITIONALLY (not path_data-gated) precisely so
   `synthesize_primitive_paths` can grow the path afterwards.

**Vector-network fallback.** Symbol children can carry EMPTY
`fillGeometry`/`strokeGeometry` on the master AND every instance's derived
data (the FX knobs' `Oval` rim), leaving only `vectorData.vectorNetworkBlob`.
`paths.mjs` decodes that network (layout self-validated by exact byte
consumption: 12-byte header + 12B vertices + 28B segments; tangents are
vertex-relative control offsets) into the shape's CENTERLINE and marks the
result `centerline: true` — the caller STROKES it via the channels above,
never fills. Refused (→ existing plain-box diagnostic) when regions are
present, any vertex joins 3+ segments, or the byte count is off. Never used
for `BOOLEAN_OPERATION` (operand children are the faithful fallback).

**Diagnostics carry the instance-path node id.** `pushDiag` records
`node.__key || guidKey(node.guid)` — recording only the master guid attached
every symbol-expanded diagnostic to the MASTER while the envelope/materials
sidecar name the clone, so `material_audit`'s per-node join scored honest
out-loud degradations as SILENT drops (diagnosed column read 0).

### Design contract (`pulp design compile`) — the token/widget allowlist

Before generating or hand-writing a UI, compile the **design contract**: the
closed set of tokens and components the UI is allowed to bind to. It is compiled
from the buildable sources of truth — a `Theme` (the token allowlist) and the
`pulp::design::catalog()` component set (each widget's native class plus the exact
theme tokens it paints through) — so it never drifts from the code.

```bash
# Built-in Ink & Signal system (default). Writes design-manifest.json +
# design-binding-prompt.md into --out-dir (default: cwd).
pulp design compile --out-dir build/design

# A project's own tokens instead of the built-in system:
pulp design compile --design-md DESIGN.md --out-dir build/design
pulp design compile --theme my-theme.json --dark --stdout --json
```

Two artifacts, one source of truth:
- **`design-manifest.json`** — deterministic (all lists sorted): every token
  (name, kind, value) and every component contract (native class, category,
  `reskin_tokens` allowlist). This is the machine-readable allowlist an adherence
  check validates generated JS against.
- **`design-binding-prompt.md`** — an LLM-ready Markdown fragment listing the
  allowed tokens and components with an explicit "do not invent token names"
  directive. Embed it in an importer/codegen prompt so the model binds only to
  real tokens and widgets. A value or widget outside the contract is a fidelity
  break, not a silent drift.

The module is `core/view/src/design_manifest.cpp`
(`pulp::design::compile_design_manifest` / `manifest_to_json` /
`emit_binding_prompt`), gated behind `PULP_ENABLE_DESIGN_IMPORT` alongside the
rest of the authoring cluster.

### Fidelity ledger (`--fidelity-report`) — a diffable record of import warnings

The import-time fidelity self-check (`design_fidelity.hpp`: skew, dropped-vector,
widget-size, …) always warns on stderr, and `--strict-fidelity` turns the hard
findings into exit 4. Those warnings are transient — they scroll past and are
lost. Pass `--fidelity-report <file>` to also persist the run's findings as a
**named, machine-readable ledger** so an import's fidelity is a durable artifact
you can diff across revisions or gate in CI:

```bash
pulp import-design --from fig --file synth.fig --frame 0:2 \
  --output ui.js --fidelity-report build/fidelity.json
```

The ledger (`core/view/src/design_fidelity_ledger.cpp`,
`pulp::view::fidelity_ledger_json`) carries:
- a **`summary`** — `total`, `warnings` (the hard findings that gate
  `--strict-fidelity`), `informational`, and a `by_kind` count map;
- the **`findings`** — each with `kind`, `severity` (`warning` for a hard
  finding, `info` for an advisory one), `node_id`, `node_name`, `detail`;
- the **`taxonomy`** — every fidelity kind the checks can emit, each with a
  stable slug, default severity, and one-line summary, so a consumer can render
  a kind it does not itself know about.

It is emitted regardless of `--strict-fidelity`: warnings are data, not
failures. The taxonomy in `fidelity_taxonomy()` must stay in step with the kinds
`design_fidelity.hpp` emits — add a kind to one, add it to the other.

### Adherence lint (`pulp design lint-adherence`) — the mechanical backstop

The binding prompt tells the model what's allowed; the adherence lint proves the
generated JS actually stayed inside the contract. It flags three high-signal
drifts and is the CI-gateable counterpart to the prompt:

```bash
# Lint an imported/generated UI against the built-in system (or a manifest):
pulp design lint-adherence ui.js
pulp design lint-adherence ui.js --manifest build/design/design-manifest.json
pulp design lint-adherence ui.js --design-md DESIGN.md --strict
```

- **raw-color** (error): a hex literal (`#rrggbb`/`#rgb`/`#rrggbbaa`) where a
  bound theme should be referenced via `var(--token)`. When the value is one the
  system defines, the finding names the token to bind instead.
- **unknown-token** (error): a `var(--name)` reference whose token is not in the
  manifest — a hallucinated or renamed token that silently falls back at runtime
  (`resolve_color` returns the default). This is the exact failure the binding
  prompt exists to prevent, caught mechanically.
- **raw-dimension** (info): an `<n>px` literal whose value matches a dimension
  token — prefer the token.

Exit 0 when clean, 1 on any error-severity finding (`--strict` fails on info too).
The scan is purely lexical (`core/view/src/design_adherence.cpp`,
`pulp::design::lint_adherence`): line/block comments are ignored, string literals
are scanned, and token→`var(--x)` mapping mirrors `export_css_variables`
(`.`→`-`). Pair it with `compile` — the prompt and the lint share one manifest.

### Project design ledger (`pulp design record`) — resumability + review status

Import/design sessions span days and multiple agents; "which of these five `ui.js`
revisions did the human approve" is a signal that otherwise lives only in chat.
The **design ledger** (`.pulp-design-meta.json`) makes it a durable, CLI-owned
record: each emitted artifact as a named, versioned asset with its source
provenance, viewport, bound design system(s), and a review status.

```bash
# Record an emitted artifact (auto-assigns v1, v2, … per name):
pulp design record --name main-panel --asset ui.js \
  --source fig --viewport 340x280 --system ink-signal

# Approve a revision (an approved version must never be silently regenerated):
pulp design record --name main-panel --version v2 --asset ui.js --status approved

# Read on resume; drop a stale entry; reconcile hand-deleted files:
pulp design record --list            # or --list --json
pulp design record --remove main-panel@v1
pulp design record --reconcile       # drop entries whose file is gone
```

**Discipline: only the CLI writes the ledger; skills/agents READ it.** On resume,
read it (`--list --json`) to recover the bound design system (⇒ load its binding
prompt from `compile` without re-asking) and to see which revisions are
`approved` vs `needs-review` vs `changes-requested`. Never hand-edit the file and
never regenerate an `approved` asset without an explicit request. The ledger
operations are pure (`core/view/src/design_ledger.cpp`,
`pulp::design::parse_ledger` / `ledger_to_json` / `upsert_asset` / `remove_asset`
/ `reconcile`); all file IO lives in the `pulp design record` CLI.

**THE #1 LESSON — `--validate` does NOT render the faithful SVG.** This is what
cost hours. The scene's root carries `render_mode=faithful_svg` + the embedded
SVG, and the **C++ runtime** honors it (`design_import_native_common.cpp` →
`make_faithful_svg_frame` → `DesignFrameView`/SkSVGDOM, line ~1734). But
`pulp import-design --validate` renders the **emitted native-widget JS**
(`build_native_view_tree`/codegen materialization), NOT the faithful SVG — so it
mis-lays composite vectors (e.g. piano black keys grouped/dropped) and reports
~18/255 *even though the faithful render is pixel-perfect*. **Do not trust
`--validate`'s number as the faithful fidelity.** The CLI now DETECTS a
`faithful_svg` scene and prints a caveat before the similarity number ("…renders
the native-materialized widget tree, NOT the 1:1 faithful SVG… native-materialize
fidelity… will UNDERSTATE the true faithful render. Verify with pulp-svg-probe"),
so the trap is self-documenting — but the note is a signpost, not a fix: still run
`pulp-svg-probe` for the real 1:1 number.

**Validate the FAITHFUL render with `pulp-svg-probe`** (renders an SVG via
`DesignFrameView`/SkSVGDOM, the real 1:1 path):
```bash
# extract the data:image/svg+xml base64 from scene.pulp.json → faithful.svg, then:
build-gpu/tools/import-design/pulp-svg-probe faithful.svg out.png 1356 781
python3 tools/figma-import/verify_region.py source.png out.png 80 9.0   # → ~1.08/255 = 1:1
```
This is how Musical Typing was proven 1:1 (1.08/255 vs the design). Pulp's
SkSVGDOM **does** render Figma's effects-heavy SVG (67 filters, 61 masks)
faithfully — the export and the SkSVGDOM render are both fine; only the
native-materialize/codegen path is lossy.

**Bitmap assets inside the faithful SVG (`<image>` + base64 data URI).** Figma
exports raster fills as `<image href="data:image/png;base64,…">`. Two Skia traps
made every one of those render **blank** (a hole where the art should be), and
both are now fixed in `core/canvas/src/svg_dom_cache.cpp` — know them, because a
blank image in a faithful render looks like an export bug and is not:

* **SkSVGDOM needs a resource provider.** `SkSVGImage::LoadImage` returns early
  on a null provider, and `SkSVGDOM::Builder` defaults it to null. `SvgDomCache`
  installs `skresources::DataURIResourceProviderProxy` (+ registers the PNG /
  JPEG / WebP / GIF codecs — Skia's codec registry starts EMPTY, so
  `SkCodec::MakeFromData` fails until something calls `SkCodecs::Register`).
  Gated on `PULP_HAS_SKRESOURCES`, a **try-link** probe in
  `core/canvas/CMakeLists.txt`: the pinned native slices compile skresources
  into `libsvg.a` (there is no `libskresources.a` at all), other slices ship a
  standalone archive, and the wasm slice ships neither — only a try-link gets
  all three right.
* **Skia only parses `xlink:href`, never SVG-2 `href`.** The literal attribute
  name its parser matches is `xlink:href`; Figma emits the bare `href` with no
  xlink namespace, so Skia drops the attribute and the IRI is empty *no matter
  how good the resource provider is*. `SvgDomCache` rewrites the attribute name
  (tag-scoped, never inside a value or text) before parsing.

If an `<image>` still comes out blank, check the configure line
`Pulp: SVG <image> data-URI decoding enabled (skresources links)` — the opposite
message means the active Skia bundle can't link skresources and `<image>` is
genuinely unavailable in that build.

**One-command path (USE THIS): `tools/import-design/make_catalog_component.py`.**
It runs the whole lane — exports the Figma node, embeds the faithful SVG (chunked
base64), and emits the `DesignFrameView` subclass + the catalog/CMake/showcase
paste-ins. Example:
```bash
python3 tools/import-design/make_catalog_component.py \
  --name "Channel Strip" --class ChannelStripView --node 182:2 \
  --category containers --usage "Pro channel strip…"
```
Then paste the printed lines into `core/view/CMakeLists.txt`, the
`design_system.{hpp,cpp}` catalog, the showcase, and add a test
(`test_faithful_specimens.cpp` pattern). Validate with `pulp-svg-probe` +
`verify_region.py`. This is how Musical Typing, Channel Strip, and 7 specimen
components were built — never hand-paint.

**No-metadata source? Use the annotated-capture lane (`pulp-annotated-capture-import`).**
`make_catalog_component.py` needs Figma as the source of truth — typed nodes,
node ids, component names. A **bare SVG capture** (a vectorized screenshot, a
hand-authored asset, or a JUCE UI run through an extractor) has none of that: it
is just paths and rects. The annotated-capture lane
(`tools/import-design/annotated_capture.{hpp,cpp}` + the
`pulp-annotated-capture-import` CLI) is the no-metadata analog: feed it the bare
SVG plus a **sidecar manifest** that supplies the missing semantics per element,
and it emits the SAME artifacts (populated `DesignFrameElement` table +
`DesignFrameView` subclass + `<snake>_svg.cpp` + CMake paste-ins).
```bash
build/tools/import-design/pulp-annotated-capture-import \
  --svg capture.svg --manifest ui_manifest.json --class ReverbPanelView
```
The sidecar schema is intentionally aligned with **pulp-import-juce's
`ui_manifest.json`** so a JUCE UI extractor (component-type → `kind`, bounds →
`geometry`, parameterID → `param_key`) can emit a manifest this lane consumes
directly. Each element declares `{selector, kind, param_key, geometry, needle,
options, …}` (full field list in `annotated_capture.hpp`). When any element
carries a `param_key`, the generated ctor calls
`route_changes_to_host_params(true)` so the view runs unchanged embedded in
JUCE / iPlug2 / native. Gotcha baked into a regression test: emitted float
literals must be valid C++ (`120.f`, never the invalid `120f`).

**Under the hood: a 1:1 catalog component = subclass `DesignFrameView` with the embedded SVG.**
See `core/view/{include/pulp/view,src}/musical_typing_keyboard*` +
`design_system.cpp` catalog entry: the class is
`MusicalTypingKeyboard : public DesignFrameView`, constructed from the
base64-embedded Figma SVG (`musical_typing_keyboard_svg.cpp`). Reskin/extend by
re-exporting the node and re-embedding — never re-draw by hand. The **C++
codegen** (`generate_pulp_cpp`) now lowers a `faithful_svg` node to a
`DesignFrameView` — it embeds the node's SVG as chunked base64 (resolved via the
shared `resolve_svg_document`, the same bytes the runtime materializer uses) and
reconstructs the typed `interactive_elements` overlays, so the generated C++ is
1:1, not just the runtime materializer. If the SVG asset can't be resolved at
codegen time it falls back to the native widget emit (so the output always
compiles). Remaining follow-ups: the **JS** emit path still lowers to native
widgets (a faithful frame needs a JS-bridge primitive that doesn't exist yet),
and `--validate` still renders the native-materialized output rather than the
faithful SVG.

**Interactive-overlay kinds the IR carries end-to-end.** The faithful_svg
`interactive_elements` IR (`InteractiveElementKind` in `design_ir.hpp`) supports
`knob, fader, toggle, dropdown, text_field, tab_group, stepper, swap, action,
xy_pad, value_label` — each maps 1:1 in `to_frame_elements()`
(`design_import_native_common.cpp`) to the `DesignFrameElement::Kind` the runtime
already backs, and the schema (`figma-plugin-export-v1.json`
`interactive_element.kind`) accepts exactly that set. The schema segments
`required` per-kind: `knob` needs `cx/cy/hit_radius`; every other kind needs the
box `x/y/w/h`. Field map: **fader** translates `svg_patch_d` along the track;
**toggle** is a click-to-flip rect (a toggle WITH `svg_patch_d` is a switch; with
`flash` it is a press-flash command button); **swap** carries `target_frame`;
**action** carries the command id `action`; **xy_pad** adds `default_value_y`
(Y axis; X reuses `default_value`); **value_label** carries `text` +
`value_left_align`. When you add a kind, touch the whole chain in one commit —
schema → `gen-types` (`types.generated.ts`) → producer (`faithful-vector.ts`,
incl. `detectOverlayControls` if it should auto-detect) → IR enum →
`design_ir_json.cpp` parse/serialize → `to_frame_elements()` → the
`design_cpp_codegen.cpp` token switch + field emit — or it silently degrades.
`detectOverlayControls` auto-emits swap/action/xy_pad/value_label from explicit
whole-word node names (run AFTER the tuned dropdown/stepper/tab_group/text_field
detectors so those always win); the richer node-tree signals (prototype reactions
for swap, value patterns for value_label) land with P2's unified resolver. An
**unknown** kind string no longer silent-knobs: `interactive_kind_from_id`
reports it unrecognized and the parser emits a `log_warn` (the full ordered
resolution ladder + import report is the P7 work).

**Binding an interactive element to a host parameter (`param_key`).** A
faithful_svg interactive element carries an optional `param_key` (e.g.
`"filter.cutoff"`) that flows schema → `interactive_element.param_key` → IR
`IRInteractiveElement::param_key` → `to_frame_elements()` →
`DesignFrameElement::param_key`. This is the binding channel for
**geometry-detected** controls — the Triaz case, where a knob is a custom design
component (not a recognized Pulp-Library widget), so the recognized-widget path
that lowers its binding through `IRNode` never fires. When ANY element in a frame
carries a non-empty `param_key`, `make_faithful_svg_frame` auto-enables
`route_changes_to_host_params(true)`, so a user gesture drives the
framework-agnostic `HostParamSurface` (JUCE APVTS / iPlug2 / StateStore) directly
via `element_for_param_key` / `sync_from_host_params`. It is inert until a
producer emits a key: an all-unbound frame keeps routing OFF and behaves exactly
as before (the public `on_element_changed` / `on_gesture_*` callbacks fire
regardless of routing, so an existing consumer never changes).

**Producer emission (both lanes).** `faithful-vector.ts` (`labelAndBindKnobs`) and
`figma_rest_export.py` (`_label_elements`) resolve each geometry knob against the
frame's Figma node tree by position and stamp three things from the matched node:
a human `label`, provenance `source_node_id`, and — when the layer name carries an
opt-in `param:`/`bind:`/`meter:` sigil — a `param_key`. `paramKeyFromLayerName` /
`_param_key_from_layer_name` mirror the C++ `figma_binding_from_layer_name`
EXACTLY (leading-ws tolerant, case-insensitive sigil, trimmed value, ≥1 alnum), so
a sigil-named knob binds identically to a recognized widget. The **sigil is
load-bearing**: a bare/DESCRIPTIVE name (the real Triaz case — "Cutoff", "Res")
is NEVER auto-bound (verified: all Triaz panel captions classify as `label` with
zero false bindings in both lanes) — it gets a `label` + `source_node_id` so the
**annotated-manifest** lane can bind it by node id. Match tie-breaks: a sigil node
beats a plain-named one; within a rank, nearest center then smallest area; the
root frame is excluded so a panel name ("sound / main panel") never mis-binds a
centered knob. Tests: `[layer-name-binding]`/knob cases in
`faithful-vector.test.ts` + `ParamKeyBindingTest` in `test_figma_rest_export.py`.

**Annotated-manifest binding (`--param-binding-manifest`) — the descriptive-name
lane.** Real designs (Triaz) name knobs descriptively ("Cutoff"), not with a
sigil, so the producer stamps `source_node_id` but no `param_key`. Supply a JSON
object mapping that Figma node id to a host-param key —
`{"10:42": "filter.cutoff"}` — via `pulp import-design … --param-binding-manifest
bindings.json`. `apply_param_binding_manifest` (design_import.cpp) walks the parsed
IR and sets `param_key` on each interactive element whose `source_node_id` is in
the manifest AND whose key is still empty — so **an explicit layer-name sigil
always wins**; the manifest never overwrites one. Applied once in the C++ import
CLI after IR parse (before the import report), so all downstream lanes
(materialize / codegen / DesignIR) see the binding. Get the node ids from the
importer's provenance or the Figma MCP `get_metadata`. Tests: `[param-binding]`
cases in `test_design_import_ir.cpp` (library) + `test_import_design_tool.cpp`
(CLI wiring + error paths).

**Custom controls (P7 Tier-3) — the `name→View` factory registry.** A genuinely
novel control resolves to `kind=custom`, which carries a `factory_id` (+ opaque
`custom_props`, typically JSON Pulp doesn't parse). The runtime
`register_design_control_factory(id, factory)` (`design_frame_view.hpp`) maps an
id to a `std::function<unique_ptr<View>(const DesignControlContext&)>`;
`DesignFrameView::build_overlays` looks the factory up for a `Kind::custom`
element and builds the overlay. **UI-thread-only** (registration at host startup,
lookup at overlay build) — the registry has no locking by contract. If no factory
is registered the element renders INERT (the baked SVG still shows) and
`make_faithful_svg_frame` emits a `native-materialize-custom-factory-unregistered`
diagnostic — a custom control never blanks or silent-knobs. Schema requires
`factory_id` for `kind=custom`. This is the piece a shared control PACKAGE (P8)
registers into. Beyond the usual atomic chain, the two exhaustive
`DesignFrameElement::Kind` switches in `design_frame_view.cpp`
(`element_value`/`set_element_value`) need the `custom` case, and the inspector's
`frame_element_kind_name` switch in `inspect/src/inspector_window.cpp`.

**Import report (P7).** Implementations of the import-report and
placement-verification passes live in `core/view/src/design_ir_analysis.cpp`
(extracted from `design_ir_json.cpp`, which is the IR JSON serialization
*contract* — keep the analysis passes there, not in the serializer).
`collect_import_report(ir.root)` (`design_import.hpp`)
walks the IR's interactive elements and surfaces each control's resolution
provenance — `{source_node_id, kind, resolution_rung, confidence_score,
conflict_signals, verification_pass}` — plus summary counts (`conflicted` /
`low_confidence` / `unresolved`) and `ok()`. `pulp import-design` prints the
human summary (`import_report_to_text`) to STDERR for EVERY output mode (codegen
+ DesignIR-v1), writes the machine-readable JSON (`import_report_to_json`) when
`--import-report <path>` is given, and `--fail-on-unresolved` makes a conflicted
or inert control a nonzero (2) exit — the CI gate. So a low-confidence or
conflicted control is SEEN at import time, never discovered later in the DAW.
`apply_placement_verification(ir.root, frame_w, frame_h)` runs first (the
structural half of the render-golden gate): it flags an overlay with no
renderable extent (zero hit-radius AND zero-area box) or one entirely outside the
frame region — setting `verification_pass=false` + a conflict so the report/gate
catch it. Frame size 0 = "unknown" (skips the bounds half, keeps the
degenerate-extent check). The full PIXEL-level golden diff is the render-path
follow-up.

**Multi-frame / post-processed components need a DEDICATED re-embed lane —
`make_catalog_component.py` is single-frame and applies no neutralization.** The
Musical Typing Keyboard is TWO frames (typing 187:15 / piano 187:349) AND its
exported SVG is post-processed: the design bakes a "selected keys shown" demo
chord as lit `#16DAC2` key gradients, which must be NEUTRALIZED to the resting
key color (the live momentary overlay owns all pressed-state lighting) or the
`[regression] no baked-lit demo chord` test trips. So it has its own
`tools/import-design/reembed_mtk.py`: fetch both nodes via `/images?format=svg`,
neutralize, emit the chunked-base64 cpp. Neutralization is content-based, not
positional — a lit gradient is a `<linearGradient>` with both stops `#16DAC2` +
a `0.26→1.0` opacity ramp; classify white (gradient y-extent ≥ 65 → `#EBEEF1`)
vs black (< 65 → `#3A3F47`/`#16191E`) so it survives Figma edits. `--validate`
asserts the decoded SVGs still match the committed file. **Gotcha — strip the
opacity:** the lit gradient's top stop is `0.26` opacity (the design's press
fade). Neutralization must REMOVE that `stop-opacity="0.26"`, not just swap the
hex — a resting key is SOLID `#EBEEF1`, so a was-lit key that keeps the `0.26`
renders translucent over the dark bed and reads GRAY beside its solid neighbours
(the "gray E4" report). **Gotcha — reflow:** removing a toolbar element (e.g. the
top-right OCTAVE/VEL cluster) lets Figma's flex REFLOW siblings — the overview
strip widened (right edge 151→677) and the `>` arrow moved (~550→689). Any
hardcoded element/overlay coords in `musical_typing_keyboard.cpp` (strip bounds,
arrow rects) must be re-derived from the regenerated SVG after such an edit.

**Lesser gotchas:**
- `--validate`'s "Similarity %" also breaks on **size mismatch** (it renders at
  `--render-size`, often 2× the reference) — always diff at matched dimensions
  with `verify_region.py` (per-tile).
- `--render-size` must match the node aspect or content letterboxes → inflated diff.
- Skia backend is mandatory for image/asset compositing (`--screenshot-backend skia`).

This pairs with the upstream Figma-import toolkit (`tools/figma-import/`, which
captures the design HTML→Figma 1:1): **Figma is the single source** — design HTML
→ Figma (figma-import) → Pulp (this lane). Keep both ends improving from each
import's lessons.

## CRITICAL: pulp-design-tool requires the GPU host (PULP_HAS_SKIA)

Before debugging *any* runtime-import resize / sizing / layout issue in `pulp-design-tool` or `/tmp/<App>.app`, verify the binary is using `MacGpuWindowHost`, not the CPU `MacWindowHost`. The design viewport pin, aspect-lock, and uniform paint-scale all live in `MacGpuWindowHost` (gated by `#ifdef PULP_HAS_SKIA` in `core/view/platform/mac/window_host_mac.mm`). When Skia isn't linked, `WindowHost::create()` returns `MacWindowHost`, where `set_design_viewport()` and `set_fixed_aspect_ratio()` are base-class no-ops — the example still builds and runs, but resize behaves as if every fix you've shipped is missing.

**One-shot verification:**

```bash
# In the worktree
nm build/examples/design-tool/pulp-design-tool 2>/dev/null | grep -q MacGpuWindowHost \
  && echo "OK: GPU host present" || echo "FAIL: CPU-only build"

# In a packaged .app
strings /tmp/MyApp.app/Contents/MacOS/MyApp-Bin | grep -F "[gpu-host]" \
  && echo "OK: GPU host present" || echo "FAIL: CPU-only build"
```

**Recovery when Skia is missing** (e.g. fresh worktree with `external/skia-build/` containing only headers):

1. Reuse the primary checkout's populated cache:
   ```bash
   rm -rf external/skia-build
   ln -s /Users/<you>/Code/pulp/external/skia-build external/skia-build
   cmake -S . -B build -DCMAKE_BUILD_TYPE=Release   # should print "Skia: found at ..."
   ```
2. Or run `tools/build-skia.sh` to rebuild Skia binaries from scratch (~30 min).
3. Or pass `-DSKIA_DIR=/abs/path/to/external/skia-builder/build/<plat>-gpu`.

**Defenses already shipped** (don't bypass without reading the comments):

- `examples/design-tool/CMakeLists.txt` issues `FATAL_ERROR` at configure if `PULP_HAS_SKIA` is FALSE — design-tool is intentionally a GPU-only example.
- `examples/design-tool/main.cpp` checks `#ifndef PULP_HAS_SKIA` at startup and exits with EX_CONFIG (78) + a loud stderr message.
- `tools/cmake/PulpDependencies.cmake` already prints a screaming WARNING banner when `PULP_ENABLE_GPU=ON` but Skia isn't found; the release lane (`PULP_REQUIRE_GPU_FOR_SDK=ON` in `release-cli.yml`) escalates to FATAL_ERROR.

**If your runtime-import test produces a window where content doesn't scale, the dark fill is visible past the design surface, or aspect-lock doesn't engage during drag, the FIRST thing to check is `nm | grep MacGpuWindowHost`.** Don't tune `set_design_viewport` / `windowWillResize:` / `setContentAspectRatio:` until you've confirmed the GPU host is actually linked.

Detect which design source the user wants by checking:
1. If a Figma MCP server is available (com.figma.mcp), offer to read the current file/selection
2. If Stitch MCP is available (mcp__stitch__*), offer to list projects and get screens
3. If Pencil MCP is available (mcp__pencil__*), offer to read the current editor state
4. If the user mentions Claude Design or hands over a manually-exported HTML file from Anthropic Labs' Claude Design tool, treat as `--from claude` (no MCP — Anthropic has no public API; manual file export is the supported path, and Spectr's `editor.html` mapping is the precedent)
5. If the user provides a file path or URL, use that directly
6. If none of the above, ask the user for a source and file

## Workflow

### Imported designs: declare dimensions in CMake, never hand-roll `view_size()`

Pulp's SDK auto-sizes plugin editors from a single pair of CMake args, so
every imported-design plugin opens at the right size in AU / AUv3 / VST3 /
CLAP / Standalone with no per-format override. **Always use the CMake path
— don't hand-roll `Processor::view_size()` for imported designs.**

```cmake
pulp_add_plugin(MyPlugin
    FORMATS         AU AUv3 VST3 CLAP Standalone
    ...
    DESIGN_WIDTH    900     # preferred editor width in logical pixels
    DESIGN_HEIGHT   520     # preferred editor height in logical pixels
    # Optional explicit bounds; omit for auto-derivation:
    #   min  = preferred * 2/3
    #   max  = preferred * 2
    #   aspect_ratio = width / height
    # DESIGN_MIN_WIDTH 700
    # DESIGN_MIN_HEIGHT 400
    # DESIGN_MAX_WIDTH 1920
    # DESIGN_MAX_HEIGHT 1080
    SOURCES         my_plugin.hpp my_plugin.cpp
)
```

Mechanism:

1. `pulp_add_plugin` injects `PULP_PLUGIN_DESIGN_W/H/MIN_W/...` as
   `target_compile_definitions` on `${target}_Core` (PUBLIC, so every
   linked format adapter sees them).
2. `format::Processor::view_size()`'s default checks for the macros and
   calls `view_size_from_design(w, h, ...)` to derive the full `ViewSize`.
3. The derived `min > 0` is what enables CLAP's `gui_can_resize`
   (`core/format/include/pulp/format/clap_entry.hpp:391`) and prevents
   the corner-drag-crops-instead-of-resizes regression that landed on the
   m149 branch and bit us in Reaper.

Symptoms when this is missing (any of these = you forgot the CMake args):

- **CLAP / VST3 corner-drag crops instead of resizes.** Default `min = 0`
  fails `gui_can_resize`, host treats the editor as non-resizable.
- **Plugin opens at a random portrait size after re-launch.** With no
  design-side dimensions and no saved window rect, hosts fall back to
  the format's default content area (often portrait ~360×480).

How to choose the numbers:

1. Open the imported JSX bundle in `pulp-screenshot --script <bundle.js>`
   and try `--width / --height` values until the layout looks right with
   no letterboxing or content clipping. Those become `DESIGN_WIDTH/HEIGHT`.
2. Trust the auto-derivation for min/max unless you have a specific reason
   to clamp tighter (e.g., a widget that breaks below 700×400).
3. The derived `aspect_ratio = width/height` makes the host snap the
   corner-drag to the design AR (Phase 3 design viewport letterboxes the
   rest).

Stage B of #2784 — `pulp import-design` auto-emitting a `.size.json`
sidecar that `pulp_add_plugin` reads, so the dimensions are declared
**once** at import time — is the immediate follow-up. Until then,
`DESIGN_WIDTH / DESIGN_HEIGHT` is the codified path; the C++
`view_size_from_design()` helper is unit-tested in
`test/test_processor_defaults.cpp`.

**Do not write a `view_size()` override** for an imported-design plugin
unless you have a runtime-computed dimension (rare; usually a custom
non-script UI). Hand-rolling reintroduces the per-plugin maintenance
burden the SDK args were designed to eliminate.

### DesignIR v1 asset manifest lane

When a user asks for canonical IR or an import pipeline handoff, prefer
`pulp import-design --emit ir-json`. The output is a versioned DesignIR v1
envelope with a deterministic `assetManifest` sidecar. Local images, SVGs,
font URLs, CSS `url(...)` values, and data URIs are recorded by default.
HTTP(S) assets are resolve-only unless the user explicitly passes
`--allow-network-fetch`; fetched assets use `--asset-cache`, honor
`--asset-timeout-ms`, and can be pinned with repeated
`--asset-hash <uri=sha256>` flags.

For `--url` imports, relative asset references resolve against the original
source URL, not the temporary downloaded file. The manifest keeps the authored
value in `original_uri`, stores the resolved fetch target in `source_url`, and
nodes keep their raw URI attributes plus a stable companion such as
`srcAssetId` or `backgroundImageAssetId`.

DesignIR v1.5 also carries document-level provenance (`capture_method`,
`settle_rounds`, `fallback_reason`, `source_adapter`, `source_version`,
`imported_at`) and structured top-level diagnostics. Parse APIs return the
shared normalized form, including interactive-frame promotion from the Pulp
view library.

The shared native-resolution layer lives in
`core/view/src/design_import_native_common.{hpp,cpp}`. It consumes normalized
`DesignIR` plus `IRAssetManifest` and returns a deterministic
`ResolvedNativeNode` tree for later baked-native/baked-cpp materializers.
Mapping precedence is `audio_widget` first, then `IRNode.type`, then HTML
subtype attributes such as `input[type=range]`, with unsupported nodes and
properties degrading to diagnostics instead of throwing. When resolving frozen
DesignIR JSON, keep the embedded `assetManifest` fallback intact; do not
iterate `IRNode.attributes` directly when diagnostic order matters because it
is unordered. Use sorted attribute keys for stable output.

The `baked-native` materializer is the direct View-tree lane:
`build_native_view_tree(const DesignIR&, const IRAssetManifest&,
const NativeMaterializeOptions&)` is public in
`<pulp/view/design_import.hpp>`. It calls the shared native resolver, returns a
detached `std::unique_ptr<View>` subtree, and catches API-boundary failures into
diagnostics instead of throwing. Keep image assets routed through
`IRAssetManifest::resolve(asset_id)`; never interpolate raw filesystem paths
from IR attributes.

**Self-contained JS export (relative asset paths).** The emitted `ui.js`
never references decode-time locations: after `resolve_sprite_skins` stamps
absolute paths, `localize_ir_assets` (`sprite_skins.cpp`) copies every
referenced image/font into `assets/` NEXT TO the `--output` file and rewrites
`attributes["asset_path"]` / `font.resolved_path` to output-relative
`assets/<file>` before codegen. This is load-bearing for the `.fig` lane,
whose scratch dir (`$TMPDIR/pulp-fig-*`) is deleted when the run exits — an
export that kept absolute paths would silently lose all images on any later
render. Renderers resolve the relative form via
`WidgetBridge::set_script_base_dir(<script dir>)` (set by `--validate`,
`pulp-screenshot`, and `pulp-design-tool`; unset base = historical
CWD resolution). It resolves `setImageSource` / `setKnobSpriteStrip` /
`registerFont` / `loadFont` only, and unlike `set_asset_roots()` it never
restricts `loadAsset`. Skipped for `--dry-run` (must not write files) and
baked emits (cpp codegen takes no asset paths). `make_scratch_dir` also
sweeps stale (>24h) `pulp-<tag>-*` siblings — runs killed mid-decode leak
their scratch dirs, and hundreds had accumulated before the sweep existed.
When testing emitted JS, note the unquoted `// Source:` header comment still
names the decode-time input; assert on QUOTED paths (`'assets/...'`) when
pinning "no absolute references".

**Windows path-separator gotcha (asset/font paths baked into generated JS):**
when the CLI's asset pass (`resolve_sprite_skins` in `sprite_skins.cpp`) resolves an `asset_ref`/font `asset_id` to a path
that is stamped into `attributes["asset_path"]` / `font.resolved_path` (and from
there into `setImageSource(...)` / `registerFont(...)` in the generated JS),
convert it with `fs::path::generic_string()`, NOT `.string()`. On Windows
`.string()` emits native backslashes, which (a) bakes non-portable separators
into the generated UI and (b) breaks tests that assert on `assets/...`
substrings. `generic_string()` always emits `/`, and Windows file APIs accept
forward slashes. On POSIX the two are identical, so the change is a no-op there. The materialization call site itself should not run JS, but
do not market this as "no JS engine" globally because live React/parity lanes
still use the JS runtime. Baked/native consumers can link `pulp::view-core`;
live import, `ScriptEngine`, `WidgetBridge`, and scripted UI consumers should
link `pulp::view-script` or the full compatibility target, `pulp::view`.

The `baked-cpp` exporter emits native C++ source from the same resolved tree via
`generate_pulp_cpp(const DesignIR&, const IRAssetManifest&,
const CppExportOptions&)`. CLI usage is `pulp import-design --mode baked --emit
cpp --output imported_ui.cpp`; the tool writes the sibling `.hpp` by default.
Generated code should contain direct widget construction, stable anchor IDs,
token/asset constants, `bake_asset_manifest()`, and TODO comments for unresolved
audio parameter or meter bindings. Knob current value and reset default are
distinct: emitted `set_value(...)` may come from the normalized `value`
attribute, while `set_default_value(...)` must come from normalized
`audio_default`. Preserve duplicate token names even when their values alias,
and emit non-hex semantic color tokens as strings rather than trying to parse
them as colors.

#### W3C Design Tokens (DTCG) export — `--emit-w3c-tokens`

`pulp import-design ... --emit-w3c-tokens <path>` (`-` = stdout) additionally
writes the imported `IRTokens` as a DTCG document via
`pulp::view::to_w3c_tokens_json()` (`core/view/src/design_tokens_w3c.cpp`).
Purely additive — the envelope `tokens.json` / `--format` pipeline is
untouched. Shape: top-level `colors`/`dimensions`/`strings` groups (empty
groups omitted), `/` in token names nests into DTCG groups
(`brand/primary` → `colors.brand.primary`), dimensions use the object form
`{"value": N, "unit": "px"}`, and `IRTokens::source_identity` provenance
lands under `$extensions["dev.pulp.source"]` (id/collection/mode/adapter,
empty subfields omitted).

String-token policy (there is no standard DTCG "string" type, and the
emitter never invents one):

- **Font families promote to `$type: "fontFamily"`.** Conservative name
  heuristic, case-insensitive, segments split on both `/` and `.`: any
  segment equal to `font`/`fontFamily`/`font-family`/`typeface`, or a final
  segment of `family`/`font`. A comma-separated value ("Inter, sans-serif")
  emits the DTCG array form `["Inter", "sans-serif"]` (entries trimmed);
  otherwise the plain string. These live in the `strings` group with the
  same nesting + provenance as other tokens.
- **Everything else is PARKED, never dropped.** Ambiguous strings (easing
  names, content text, component-style values) collect losslessly under the
  document-root `$extensions["dev.pulp.nonStandardTokens"]` as
  `{"<full name>": {"value": "<text>", "id", "collection", "mode",
  "adapter"}}` (provenance subfields from `source_identity`, empties
  omitted). Root `$extensions` is valid DTCG, so the real token groups
  contain only standard-typed tokens.

`pulp::view::validate_dtcg(json)` (same header) is the in-repo,
dependency-free DTCG conformance check: it returns human-readable
violations (empty ⇒ conformant), covering resolvable + standard `$type`
per token (including group `$type` inheritance), `$value` shapes for
color/dimension/fontFamily, the reserved-`$`-key allowlist
(`$value`/`$type`/`$description`/`$extensions`/`$deprecated`), and
`$extensions` being an object with namespaced (dotted) keys. The emitter
test asserts every emitted document validates clean AND that known-bad
documents are reported. Do not confuse this emitter with `w3c_tokens.cpp`
(`export_w3c_tokens(Theme)`) — that is the always-compiled runtime Theme
pair with flat dot-groups and string dimension values; the DTCG emitter is
the authoring-side surface. Note the `fig` lane currently extracts no
variables into `IRTokens`, so a bare `.fig` import emits `{}` — the
figma-plugin envelope and designmd lanes are the ones that carry tokens.

#### figma-plugin `binding` → canonical `pulp*` binding contract

The figma-plugin extractor (`tools/figma-plugin/`) exports a recognized Pulp
Library control as an audio widget (`audio_widget`) plus a single free-form
`attributes["binding"]` string (e.g. `"filter.cutoff_hz"`). The whole native
binding pipeline — the materializer (`design_import_native_common.cpp`,
`NativeBindingMetadata::parse`) and the binding-manifest codegen
(`design_cpp_codegen.cpp`) — only consumes the `pulp*`-prefixed contract, so a
raw `binding` string is invisible to them on its own.

`parse_ir_node` (`design_ir_json.cpp`, `normalize_figma_plugin_binding`)
normalizes that string into the canonical `pulp*` contract at the IR-ingest
boundary, so there is exactly ONE downstream binding consumer. Rules to keep in
mind when touching this:

- **Recognized-widget gate.** Synthesis only fires when `audio_widget != none`.
  A generic/visual frame that happens to carry a `binding` attribute gets NO
  synthesized binding — it stays a generic node. Don't loosen this gate.
- **Opt-in LAYER-NAME binding (`figma_binding_from_layer_name`).** A recognized
  widget can declare its binding WITHOUT the explicit `binding` component
  property, via a sigil-prefixed layer name — `param:`, `bind:`, or `meter:`
  followed by `"<module>.<param>"` (e.g. a knob layer named
  `param:filter.cutoff_hz`). `normalize_figma_plugin_binding` resolves the binding
  from the `binding` attribute FIRST, then falls back to the layer-name sigil;
  the rest of the lowering (module/param split, param-vs-meter routing by
  `audio_widget`, `pulp*` synthesis) is identical either way. The **sigil is
  load-bearing**: a bare/ordinary layer name is NEVER treated as a binding (no
  false positives from names like "Big Cutoff Knob") — only a control the designer
  explicitly tagged binds by name. An explicit `binding` attribute always wins
  over the name. Case-insensitive on the sigil. This is the recognized-widget
  path; binding a *geometry-detected* faithful-vector overlay (a knob the importer
  found by geometry, not a Pulp Library component) by layer name is a separate
  `param_key`-on-`IRInteractiveElement` path — keep the sigil convention identical
  across both when you wire it. Pin any change with a
  `[layer-name-binding]` case in `test_design_import_sources.cpp`.
- **Name→kind resolution matches whole WORD TOKENS, not substrings — in ALL
  THREE lanes.** The C++ `detect_audio_widget`, the TS
  `audioWidgetKindFromName` (`extract-pure.ts`), and the Python
  `widget_kind_from_name` (`figma_rest_export.py`) all tokenize the layer name on
  non-alnum + camelCase (acronym-aware, so `VUMeter`→{vu,meter}) + letter↔digit,
  then match whole tokens (simple plural tolerated: `Knobs`→knob). The TS/Python
  lanes were substring-based until the P2 resolver unification; all three now
  share the boundary rule (mirrored, not one function — each has its own
  vocabulary/return enum). This is deliberate: the old `find()`/`includes()`/`in`
  substring match promoted `Dialog`/`Radial`→knob and `Parameter`/`Diameter`→meter
  (gap survey). Don't revert any lane to substring matching; add new keywords as
  tokens + a false-positive regression case (`test_design_import.cpp`,
  `audio-widget-name.test.ts`, `test_figma_rest_export.py`). Lockstep is on the
  VOCABULARY too, not just the boundary rule: the meter/waveform/spectrum aliases
  (`level`, `oscilloscope`, `analyzer`/`analyser`) must exist in all three lanes —
  the Python lane silently lagged on these until an adversarial-review follow-up,
  so when you add a token to one lane, add it to the other two and pin it in each
  lane's test in the SAME change. The faithful-vector
  overlay lane's `kindFromName` (`resolve-control.ts`, P7) shares the same
  whole-word convention for its own (InteractiveElementKind) vocabulary.

### Component content is designer art — the `audio_widget: "none"` opt-out

Name-token recognition must NEVER fire inside a component's own content: a
layer literally named "knob base" / "knob ring" IS the designer's knob art, and
promoting it paints Pulp's built-in silver knob over the design. The contract
(same across lanes):

- **The explicit opt-out**: an envelope node with `audio_widget: "none"` is a
  real statement, not an absence — `parse_ir_audio_widget`
  (`design_ir_json.cpp`) treats a literal `"none"` as *explicit* and skips
  `detect_node_audio_widget` for that node. Only `"none"` opts out; unknown
  strings still fall through to detection. The recognition resolver still runs
  afterwards (it is keyed on component identity, not names), so a matched
  library component becomes a real widget regardless.
- **The `.fig` lane** (`tools/import-design/fig/scene.mjs`) expands INSTANCE
  nodes into their master SYMBOL subtree (override guidPaths resolve by guid OR
  `overrideKey`; multi-segment paths forward into nested instances), stamps the
  instance + every expanded node `"none"`, and emits
  `figma.component_key` / `main_component_name` from the master.
- **The REST lane** (`figma_rest_export.py::walk`) stamps `"none"` on (1) any
  INSTANCE / COMPONENT and its whole subtree — identity comes from the /nodes
  `components` + `componentSets` maps, preferring the SET key (that is what the
  resolver tables are keyed by); (2) any DETACHED copy — a widget-named frame
  that directly owns raw shapes (`_owns_shape_art`) — one drawn widget whose
  parts are art; and (3) any widget-named CONTAINER it declined to promote —
  the pin matters because asset capture can collapse child containers into leaf
  images, and the C++ heuristic re-run on that DEGRADED envelope would
  re-promote the parent without it. Name promotion survives only for EMPTY /
  text-only placeholder frames, where there is no art to destroy.
- **Known asymmetry (follow-up)**: the TS plugin lane (`extract.ts`) and the
  C++-side detect on `.fig` DETACHED copies do not stamp the opt-out yet, so a
  detached knob's shape leaves can still be name-promoted there. Fix belongs in
  the shared detect gate or per-lane stamping — mirror the REST rules.
- Pinned by: `test_figma_rest_export.py` (instance/detached/pin cases),
  `fig.test.mjs` (expansion + opt-out contract), and the
  `explicit audio_widget 'none' suppresses name-based widget detection` case in
  `test_design_import_sources.cpp` (with a promoted control sibling).

### KEY-based recognition + the recognition-resolver merge module

NAME-token recognition (above) is a *fallback*. The AUTHORITATIVE recognition
signal is the Figma component identity — a `component_set_key`. This is a
SEPARATE mechanism from the 3-lane name-token vocabulary; do not conflate them.

- **The merge module is the single source of truth.** `core/view/.../recognition_resolver.{hpp,cpp}`
  (`RecognitionResolver`) is the ONE place that combines recognition SOURCES
  into a merged `component_set_key → kind` (and `→ factory_id`) table. Sources,
  in precedence order (later wins on key collision):
  1. built-in Pulp Figma Library (`RecognitionResolver::with_builtin_library()`,
     mirrored in code from `tools/figma-plugin/library-manifest.json` and pinned
     against the JSON by a drift-guard test),
  2. the user's `--recognition-manifest` (flat library-manifest shape),
  3. installed-package `design_controls` fragments (custom controls) —
     gathered by `discover_package_design_controls()` (walks up for the project's
     `packages.lock.json`, then reads the registry at `tools/packages/registry.json`
     — the canonical CLI layout from `find_registry_path`, NOT a lockfile sibling;
     a wrong path silently merges zero packages — builds ONE `RecognitionSource`
     per installed package that declares any `design_controls`, named by package id)
     and added via `add_source(...)` ONCE, in the same resolver-build block, NOT
     by threading a third lookup through the importer lanes. **Do not scatter the
     merge.** Any new recognition source becomes one more `add_source` call.
     A package fragment carries `factory_id` (no built-in `kind`), so a match
     routes to the custom-control materialize path below. With no custom-control
     package installed this contributes zero sources, so behavior is unchanged.
- **`--recognition-manifest <path>`** lets a designer map their OWN component-set
  keys / name prefixes to Pulp kinds. Shape (mirrors `library-manifest.json`):
  `{ "widgets": { "<name>": { "kind"?, "component_set_key", "name_prefix"?, "factory_id"? } } }`.
  `kind` defaults to the widget's map key. `factory_id` (no `kind`) is the
  custom-control path: a match resolves to a registered native overlay instead
  of a built-in widget (same shape installed-package `design_controls` use).
  Harvest keys from the Figma MCP `search_design_system`.
- **Which lane is wired (authoritative): the C++ CLI figma-plugin lane.** The
  plugin envelope carries each instance's `figma.component_key` /
  `main_component_name` EVEN when the in-Figma TS plugin did not recognize it
  (a third-party component) — `parse_ir_node` stamps these into
  `attributes.figmaComponentKey` / `figmaMainComponentName`. After parse, the
  CLI (`pulp_import_design.cpp`, figma / figma-plugin sources only) builds the
  resolver (built-in + optional user manifest) and calls
  `apply_recognition_resolver(ir.root, ...)`, which stamps `audio_widget` on any
  instance that matched but was not already recognized. This is the lane that
  turns a pixel-faithful-but-0-controls third-party design (the live Ink &
  Signal "NumberBox" case) into a wired one.
- **The TS plugin (`extract.ts` → `widgetKindByLibraryKey`) bakes recognition
  at CAPTURE time** for the built-in library only; it is NOT yet wired to a
  user manifest (it runs in the Figma sandbox; feeding it a manifest needs
  plumbing through the plugin UI). **The Python REST lane no longer bakes
  widget kinds for components at all** — it emits each instance's
  `figma.component_key` / `main_component_name` (from the /nodes `components` +
  `componentSets` maps, SET key preferred) and lets the C++ resolver do all
  key-based recognition, so `--recognition-manifest` works for URL imports.
  **Follow-up:** accept the user manifest in the TS lane too.
- **Never-silent-knob (P7) holds.** A component instance present in the design
  but matched by NO source is NEVER guessed into a kind — `apply_recognition_resolver`
  collects it into `UnmatchedComponent[]`, which the CLI surfaces as an
  `unmapped-component` import diagnostic. Additive guarantee: no manifest + no
  resolvable third-party key ⇒ behavior unchanged; an already-stamped
  `audio_widget` is never overridden.
- **Custom-control materialize half (the package lane's runtime side).** A
  custom-factory match has no built-in `audio_widget` to stamp — instead the
  resolver records the `recognitionFactoryId` node attribute. The CLI then runs
  `materialize_recognized_custom_controls(ir.root)` (same module), which converts
  every such node into a `kind=custom` `IRInteractiveElement` carrying that
  `factory_id` + the node's geometry. The native materializer
  (`make_faithful_svg_frame` → `to_frame_elements`) builds the overlay via the
  factory the package registered with `register_design_control_factory`. An
  unregistered factory renders inert (the baked SVG still shows) AND emits the
  `native-materialize-custom-factory-unregistered` diagnostic — never a silent
  knob. The conversion is idempotent and additive (a node with no
  `recognitionFactoryId` is untouched).
- **Merge ordering is deterministic and pinned.** Package sources are gathered
  in lockfile order; the resolver merges later sources OVER earlier ones, so on
  a `component_set_key` (or `name_prefix`) collision the LAST-added package wins.
  Tests pin this so a re-order is a visible change, not a silent one.

- **Module/param split.** Split on the FIRST `.`: `"filter.cutoff_hz"` →
  `pulpBindingModule="filter"`, `pulpBindingParam="cutoff_hz"`,
  `pulpParamKey="filter.cutoff_hz"`. No dot → empty module, whole string is the
  param. `param_key` (non-empty) is what drives both the manifest entry and the
  codegen `bind_knob`/`bind_fader` helper gate.
- **Meters differ.** `knob`/`fader`/`waveform` map to a writable param
  (`pulpParamKey`); `meter`/`spectrum` map to `pulpMeterSource` +
  `pulpMeterChannel` (no `pulpParamKey`), since a meter reads a metering input.
- **`pulpRouteId` is required** for the codegen helper to emit a live bind call;
  it's synthesized deterministically as `"figma-plugin:<binding>"`.
- **No-overwrite / no-regression.** Synthesis routes through
  `NativeBindingMetadata::serialize()` (skip-empty, no-overwrite) and bails if
  any canonical binding attr is already present, so a JSX/Claude node that
  already carries `pulp*` is untouched. The raw `attributes["binding"]` is
  always preserved — never delete the source evidence.
- **This is a generalizable importer rule**, not a per-fixture patch: it reads
  the figma-plugin data and produces the contract for ANY recognized widget.

### Skinned fader/meter width derivation

Recognized faders/meters must render their track/fill/bar at the captured art's
NARROW inset width, not the full node box. The sampler in
`core/view/src/widget_skin_derive.cpp` recovers horizontal extents from the
captured PNG pixels (`row_art_bounds` scans opaque pixels OUTWARD from the centre
column `cx`, so disjoint label glyphs on the same row never widen the result):

- **Meter:** `derive_meter_skin` reports `bar_width_px` = median opaque row width
  inside the bar's OWN vertical region `[top, bottom)` (found via `find_art_region`
  on `cx`). That excludes the label text below the bar.
- **Fader:** `derive_fader_skin` reports `thumb_width_px` (widest opaque row = the
  silver slab) and `track_width_px` (median of the NARROW rows ≤ ~40% of the
  widest = the thin track/fill column).

Gotchas learned wiring this:
- The **widest row in the whole asset is usually the label text**, not the thumb.
  `find_art_region`/`cx` scoping is what keeps the thumb measurement honest — do
  not measure the widest row over the entire image.
- `sprite_skins.cpp` divides art px by `asset_scale = img.width /
  node_box_width_px` (figma-plugin exports at 2×, but DERIVE it, don't hardcode
  2). It stamps `shape_width` = thumb/bar width (→ widget width) and
  `skin_track_width` (fader only). The column `min_width` keeps the box width so
  the narrow widget centres.
- Render path: meter codegen reads `shape_width` → widget width (already wired);
  fader needs BOTH — `shape_width` → widget/thumb width AND `setFaderTrackWidth`
  → `Fader::set_skin_track_width`, which makes `Fader::paint` draw the track at
  exactly that thin centred width instead of `0.18 * box`.
- **Verify by reference-diff, never by eyeball.** Measure visible-art width as a
  % of the node box in BOTH the captured asset and the rendered PNG; target
  within ~15%. For the smoke export the derived values were fader-track 5px
  (5.2% box), fader-thumb 28px (29% box), meter-bar 18px (26% box), all matching
  the reference within 3%.
- Everything is derived from sampled pixels / node data — NO per-instance or
  hardcoded pixel constants (repo rule: every visual importer fix must be a
  generalizable rule reading the design data).

### Native codegen fidelity gaps

The render uses `generate_native_node` in `core/view/src/design_codegen.cpp`
(createCol/createRow/createLabel/createKnob/setFlex…), NOT the `generate_node`
DOM path. Several styles only existed on the DOM path; the native path needs its
own emission. Fixes landed here, each grounded in the export data:

- **Nested padding.** The figma-plugin export sends container padding as a nested
  object `layout.padding = {top,right,bottom,left}`. `parse_ir_layout` in
  `design_ir_json.cpp` only understood a uniform float / camelCase per-side keys,
  so the nested form was dropped (→ 0, content hugged the edge). The parser now
  accepts all three forms; the native container path already emitted per-side
  `setFlex(id,'padding_*',…)`, so once parsed it renders.
- **Text wrap.** A text node's `style.width` was emitted only as `min_width`, so a
  long subtitle ran off the panel. Emit a hard `setFlex(id,'width',w)` +
  `setMultiLine(id,true)` ONLY when the design box is taller than one line
  (`height > font_size*1.6`) — that's the design's own signal that the string is a
  wrapping paragraph. A one-line title (height ≈ one line) is deliberately NOT
  bounded: forcing its hug-width as a wrap box makes it wrap when Pulp's font
  metrics run a hair wider than Figma's.
- **Knob value taper.** The native silver knob maps a 0..1 value LINEARLY to its
  [-135°,+135°] sweep, so the imported value must already encode the param taper.
  The knob path emitted the RAW `audio_default` (e.g. 880), which `set_value`
  clamps to 1.0 (and a linear normalise put 880 Hz at ~0.04 — indicator pointing
  the wrong way). Now: for a frequency-unit knob (`units` == hz/khz) use a LOG
  normalise so 880 Hz in [20,20000] lands ~0.55 (indicator ~straight up, matching
  the design); other units fall back to linear (value-min)/(max-min). The fader
  already normalised; the knob was the outlier.
- **font_weight/font_family** were ALREADY emitted on the native text path — no
  fix needed; a regression test now pins them.
- **Fader empty-track outline.** The captured empty track has a faint lighter
  edge. `derive_fader_skin` first tries to RECOVER it (brightest low-sat pixel on
  a dark track row vs the row-centre fill). **Gotcha:** the importer's in-tree
  minimal PNG decoder (`decode_png_rgba` in `sprite_skins.cpp`) FLATTENS the
  sub-pixel anti-aliased rim — it reads the whole thin track column as uniform
  fill, so the edge is unrecoverable from those pixels even though PIL sees it.
  Fallback: SYNTHESISE the rim by lightening the sampled dark track colour
  (`lum < 90` → `+30` per channel); a light/flat track stays borderless. Emitted
  via `setFaderTrackBorder(id,'#rrggbb')` → `Fader::set_skin_track_border_color`,
  which strokes the track rect in `Fader::paint`. Still derived from the captured
  track colour — no hardcode.
- **Knob bevel depth** (the silver knob reads more heavily 3D than the flatter
  captured disc) is a `WidgetRenderStyle::silver` cosmetic gap, NOT data-driven —
  left as a follow-up rather than guessing gradient constants that would affect
  every imported knob.

The Phase 5/7/9 benchmark harness lives at `pulp-design-import-bench` and is driven
by `tools/scripts/design_import_benchmark.py`. Run it under no-launch env
(`PULP_DISABLE_PLUGIN_EDITOR=1 PULP_HEADLESS=1 PULP_TEST_MODE=1
PULP_INSPECTOR_NO_LAUNCH=1`). It compares `live`, `baked-native`, and
`baked-cpp` lanes, emits startup/idle/interactive metrics, and computes the
Phase 9 gate from linked text+data section size using `size`/`llvm-size`; after
the target split it tracks live-runtime objects under `pulp-view-script`. Do
not use Debug object-file byte counts as the gate input. A valid report must
record the explicit binary-size delta and whether JS evaluation churn is
actually the dominant bottleneck for the measured fixture. Keep the legacy
top-level `comparison` entry pointed at `baked-native`, and add per-lane
results under `comparisons` for both baked lanes.

### Node-type dispatch — exhaustive, never silent (all three producers)

All three Figma producers dispatch node types EXHAUSTIVELY against the pinned
`@figma/plugin-typings` `SceneNode` union (version in
`tools/figma-plugin/package.json`); the old shape was a `default: "frame"`
fallthrough that made every unsupported node — FigJam stickies, Slides rows,
SLICE export regions — look like a successfully imported empty frame. The
three dispatch sites, kept in lockstep (one decision table, three spellings):

- plugin: `tools/figma-plugin/src/extract-pure.ts::dispatchNodeType`
  (`mapNodeType` remains as an emit-type accessor only)
- REST: `tools/import-design/figma_rest_export.py::dispatch_node_type`
- `.fig`: `tools/import-design/fig/scene.mjs` — `envelopeType` +
  the walk-top skip checks + `KNOWN_NODE_TYPES`/`SKIPPED_NODE_TYPES`

The decision table, per family:

| Family | Dispatch | Diagnostic |
|---|---|---|
| `TEXT_PATH` | emit `text`, characters preserved | `text-path-flattened` (on-path layout flattened) |
| `TRANSFORM_GROUP` (`.fig` kiwi spelling: `TRANSFORM`) | emit `frame`, explicit | none — it renders fine |
| `SLOT` | emit `frame` (placeholder) | `slot-placeholder` (bare import has no slot content) |
| `SLICE` | **skip** — an export region paints nothing; a frame here invented a box | `slice-skipped` |
| Editor/FigJam (`STICKY`, `CONNECTOR`, `SHAPE_WITH_TEXT`, `CODE_BLOCK`, `STAMP`, `WIDGET`, `EMBED`, `LINK_UNFURL`, `MEDIA`, `HIGHLIGHT`, `WASHI_TAPE`, `TABLE`, REST `TABLE_CELL`) + Slides (`SLIDE`, `SLIDE_ROW`, `SLIDE_GRID`, `INTERACTIVE_SLIDE_ELEMENT`) | **skip** | `unsupported-node` |
| Unknown/new type | emit `frame` (never crash) | `unknown-node-type` — the fallback is stated, not silent |

Gotchas:

- **Diagnostic taxonomy**: plugin/REST envelopes carry `kind:
  "unsupported_node"` (added to `ExtractedDiagnostic`, the envelope schema,
  and C++ `ImportDiagnosticKind`; `diagnostic_kind_from_code` classifies the
  four dispatch codes for kind-less producers). The `.fig` lane uses its own
  `pushDiag` channel — every new code MUST be registered in
  `DIAGNOSTIC_SEVERITY` (scene.mjs) or it silently downgrades to dropped
  `info`; the dispatch codes are all `warning` deliberately, including
  `slice-skipped`, because both `.fig` consumers drop `info`.
- **The `.fig` kiwi vocabulary is NOT the Plugin API's**: components are
  `SYMBOL`, rectangles `ROUNDED_RECTANGLE`, polygons `REGULAR_POLYGON`, and
  the transform group is `TRANSFORM`. Both spellings are accepted where they
  differ. Dump a file's actual enum via `readSchema(...).definitions` →
  `NodeType` before assuming a spelling.
- **Skips run before budgets/geometry**: plugin skips before the `maxNodes`
  count; `.fig` skips before instance expansion and the geometry ledger, so a
  skipped node also disappears from `--dump-layout` (expect node-count deltas
  vs pre-dispatch dumps ONLY for skipped families).
- **No new IR node types**: dispatch lands on existing envelope types
  (`frame`/`text`/`ellipse`/`vector`); `design_ir_json.cpp` needed only the
  new diagnostic kind, no node-type change.

### Vector shape primitives → synthesized SVG path

`vector`/`path`/`svg_path` nodes carrying an authored `path_data` (`d`) already
lower to a native `SvgPathWidget` (`createSvgPath`+`setSvgPath`).
The shape PRIMITIVES — `rect`/`rectangle`/`svg_rect`, `line`/`svg_line`,
`ellipse`/`circle`, `polygon`, `star` — usually arrive with NO `d`, so they used
to drop to an empty frame (caught by the `dropped-vector` invariant).
`synthesize_primitive_paths` (in `core/view/src/design_import.cpp`, declared in
`design_import.hpp`) now derives a `d` from geometry and stamps it onto the node
so codegen lowers it like any other path. Key facts / gotchas:

- **Runs as a codegen pre-pass, on a copy.** `generate_pulp_js` copies the IR
  root in the native arm, runs `synthesize_primitive_paths`, then emits AND runs
  the `dropped-vector` fidelity walk over that copy — so both see the synthesized
  `path_data`. The caller's IR and the web-compat arm are untouched. (Putting it
  in the parse pipeline instead would miss the `[object-coverage]` drift guard,
  which calls `generate_pulp_js` directly on hand-built nodes.)
- **Drop-case ONLY — zero behavior change for renderable nodes.** It fires only
  when a primitive has no `path_data`, no children, no visible fill
  (background_color/gradient/image), no `asset_path`, and is not an audio widget.
  A filled rect still renders via the generic-frame branch; a rect with children
  keeps them. Don't widen this to filled/childful nodes — converting them to a
  terminal `SvgPath` would drop their children / box-shadow / border.
- **`svg_fill` is forced to `"none"`.** `SvgPathWidget`'s default fill is OPAQUE
  BLACK (`has_fill_=true`, `{0,0,0,1}`). A synthesized shape with no IR fill must
  emit `setSvgFill(id,'none')` (→ `clear_fill()`) or it paints a phantom black
  box. A border becomes `svg_stroke`/`svg_stroke_width`.
- **Geometry only — source-agnostic.** Paths are derived from `width`/`height`,
  per-corner `border-radius` (rounded rect via SVG arcs; the `SvgPathWidget`
  parser supports `H`/`V`/`A`), and optional `pointCount` (polygon default 3,
  star default 5) / `innerRadius` ratio (star default 0.5) ATTRIBUTES — never a
  layer name. A `line` may have one zero extent (a horizontal/vertical rule).
  **MSVC gotcha:** the polygon/star angle math must NOT use `M_PI` — MSVC does
  not define it without `_USE_MATH_DEFINES`, which broke the Windows CLI build
  (and the whole release pipeline) once. Use the local `kSynthPi` constant.
- **Release-runner toolchain gotcha (C++20 P0960):** the GitHub-hosted macOS
  *release* runner's Apple clang is OLDER than the self-hosted PR-lane clang and
  does NOT implement **parenthesized aggregate initialization** (`Type p(arg)` for
  a ctor-less aggregate), even at `-std=c++20`. So `JsonParser p(snap.html_text)`
  in `import_detect.cpp` compiled on PR CI but **failed the release build** ("no
  matching constructor"), silently breaking every GitHub Release from ~v0.371 to
  v0.391 (the tag-triggered `sign-and-release.yml` / `release-cli.yml` Build step
  died; tags kept getting created so the breakage was invisible until the
  release-cadence watchdog flagged the tags-without-Releases). **Always brace
  aggregate init** (`Type p{arg}`) in CLI/import code — it's valid on every
  toolchain. (PR CI cannot catch this class; it only surfaces on the older
  release runner.)
- **`polyline` is intentionally NOT synthesized** — it is an open run of explicit
  points that geometry alone can't reconstruct; it stays `codegen: missing`
  (carry `path_data` or rasterize at export).
- **`is_vector_kind` is the shared classifier.** Exposed from
  `design_fidelity.hpp`; codegen's `is_path_kind` and the `dropped-vector`
  invariant both call it so they never disagree about what is a path node.
- Source of truth: `compat.json imports/object-coverage` (these 9 types are now
  `codegen: handled`) + the `[object-coverage]` drift guard + the
  `[view][import][codegen][vector]` tests.

### Vector fill rule (winding) — holes in multi-subpath icons

A multi-subpath vector's WINDING RULE decides which regions are holes, and
dropping it is perfectly silent: the icon fills as a solid slab, no parse
error anywhere. The designers-pick "Sub" speaker cabinet (a box with a hollow
woofer ring) was the sweep's worst dE2000 (~25) for exactly this reason.
The rule now flows end to end; facts to keep intact:

- **`.fig` geometry entries carry their own `windingRule` EACH.** A node's
  `fillGeometry` is a LIST of per-region entries `{windingRule, commandsBlob,
  styleID}` — and one node can MIX rules ("Sub" is `[NONZERO dot, ODD ring,
  ODD box-with-hole]`). `paths.mjs` concatenates the blobs into one path, so
  one rule must be chosen: **evenodd wins when any entry declares it.** Figma
  direction-corrects the contours of its NONZERO regions (holes wind opposite
  their outer — verified on real subtract bool-ops), and direction-corrected
  nesting fills identically under either rule; an ODD region's SAME-direction
  holes fill solid under nonzero. So evenodd is correct for both kinds. Do
  NOT "fix" this to first-entry-wins — that re-fills the Sub woofer.
- **Figma does NOT promise direction-corrected contours under ODD.** Never
  assume nonzero is safe because "baked geometry reverses holes" — that holds
  only for the NONZERO-declared entries.
- **The wire:** decoder emits `fillRule: 'evenodd'` on the envelope node
  (omitted for nonzero — widget default) → `design_ir_json.cpp` reads
  `fillRule`/`fill_rule`/`fill-rule` into `attributes["svg_fill_rule"]`
  (only `evenodd`/`nonzero` accepted) → JS codegen emits
  `setSvgFillRule(id,'evenodd')`; baked C++ codegen emits
  `->set_fill_rule(FillRule::evenodd)` (path case only — SvgRect/SvgLine have
  no fill rule, and `emit_svg_paint` is shared, so the emission lives at the
  svg_path call site); native materializer's `apply_svg_paint(SvgPathWidget&)`
  reads `svg_fill_rule` + raw `fill-rule`/`fillRule`.
- **Diagnostic:** `vector-fill-rule-approximated` (registered 'warning' —
  'info' is dropped by both consumers) fires for mixed rules on one node and
  for a MULTI-subpath vector with no declared rule (nonzero fallback may fill
  its holes solid). Single-contour shapes stay silent — both rules fill them
  identically.
- **Raster-testing gotcha:** the Skia screenshot backend composites onto an
  OPAQUE background and renders at its own pixel ratio (2x on Retina), so a
  "hole is transparent" assertion always reads opaque, and design-space pixel
  coords are wrong by the scale factor. Prove holes by DIFFERENCE between the
  two rules' renders of the same path (see the donut test in
  `test_widget_bridge_svg.cpp`).

### Figma resize constraints → flex/position

Figma layout **constraints** (a node's resize behavior relative to its parent)
parse into `IRLayout.h_constraint` / `v_constraint` (normalized tokens
`left|right|center|scale|stretch` and `top|bottom|center|scale|stretch`) and
lower to flex at codegen. Facts / gotchas:

- **Parse** (`design_ir_json.cpp`, `parse_ir_node`): reads `constraints:
  {horizontal, vertical}` at node level, also under a `figma{}` block, also
  inside `layout{}` — first non-empty wins. Figma's `MIN/MAX/CENTER/STRETCH/
  SCALE` normalize to the token set (`normalize_h_constraint` /
  `normalize_v_constraint`); unrecognized → unset. Source-agnostic.
- **Codegen map** (`design_codegen.cpp`, `emit_layout_constraints`, folded into
  `emit_position_if_absolute` so it fires at every create site, depth>0 only):
  `center` → `margin_left/right` (or `top/bottom`) `'auto'`; `right`/`bottom` →
  leading `margin_*: 'auto'` (push to trailing edge); `scale` → `flex_grow:1`;
  `stretch` (pin both edges) → `align_self:'stretch'`; `left`/`top` → flex
  default (emit nothing). The bridge `setFlex` accepts `'auto'` only for
  `margin_*` (not padding).
- **Best-effort, hence `codegen: partial`**: axis-exact `scale`/`stretch`
  depends on the parent's main axis, which the child doesn't carry. Stays inside
  Flexbox primitives — do NOT add block/table/float to make it axis-perfect
  (CLAUDE.md "Layout Model — Flex + Grid only").
- Native arm only so far; web-compat (`generate_node`) constraint emission is a
  follow-up. `compat.json features.constraints` tracks this (parsed handled,
  codegen partial); `features` rows are documented-only (not probed by the
  `[object-coverage]` drift guard). Tests: `[view][import][constraints]`.
- **Producers** (all three Figma lanes emit the shared node-level shape,
  passing their OWN raw spelling through untranslated — the parser owns
  normalization, so never add a translation table in a producer):
  - `.fig` (`fig/scene.mjs`): raw kiwi `horizontalConstraint` /
    `verticalConstraint` (`MIN/MAX/CENTER/STRETCH/SCALE`), possibly one axis
    only.
  - plugin (`figma-plugin/src/extract-pure.ts::extractConstraints` →
    `serialize.ts`): Plugin-API `node.constraints` (same spelling); guarded
    with a property check — GROUP/SLICE have no `constraints` member. The
    export schema enum rejects REST spellings by design (drift guard).
  - REST (`figma_rest_export.py::walk`): REST `constraints`
    (`LEFT/RIGHT/CENTER/LEFT_RIGHT/SCALE`, `TOP/BOTTOM/CENTER/TOP_BOTTOM/SCALE`).
- **Auto-layout gate**: all three producers emit constraints only for a node
  positioned in its parent's coordinate space — the same gate as absolute
  positioning (parent not auto-layout, OR the child opted out via
  `stackPositioning`/`layoutPositioning` `ABSOLUTE`). A FLOWING auto-layout
  child is sized by the stack; its stale constraints would fight the flex pass
  with margins/grow the design never asked for. Constraints are a no-op at
  design size (verified pixel-identical on a 1299-node real file) — they only
  change resize behavior.

### Auto Layout completion (child grow/align, wrap extras, GRID, aspect, min/max)

The audit's "Auto Layout" row (checklist #3). All three producers now emit the
complete auto-layout contract; the layout keys use the CONSUMER's camelCase
spelling (`flexGrow`, `alignSelf`, `alignContent`, `rowGap`, `columnGap`,
`aspectRatio`, `gridTemplateColumns/Rows`, `gridColumn/Row` — the exact members
`parse_ir_layout` reads), values in CSS spellings (`flex-start`,
`space-between`) because the flex bridge and `parse_flex_align` accept kebab,
NOT snake. Never emit a key the consumer doesn't read — that dead-key trap is
this subsystem's signature bug (see the `width_mode` comment in
`design_ir_json.cpp`).

- **Child grow/align** — gated exactly like constraints, but inverted: emitted
  only for a FLOWING child of a FLEX auto-layout parent (Figma leaves the
  fields stale everywhere else, and an `ABSOLUTE`-positioned child's grow/align
  would fight its coordinate placement). Source spellings: `.fig` kiwi
  `stackChildPrimaryGrow`/`stackChildAlignSelf`, plugin+REST
  `layoutGrow`/`layoutAlign` (`INHERIT` emits nothing — omitting align-self IS
  inherit). An `ABSOLUTE` stack child now also gets `position:absolute` +
  left/top in the plugin/REST lanes (it previously got neither flex nor
  coordinates and collapsed onto the stack origin).
- **Wrap extras** — `counterAxisSpacing` (kiwi `stackCounterSpacing`) is the
  gap BETWEEN wrapped tracks: a row's tracks stack vertically → `rowGap`, a
  column's → `columnGap`. `counterAxisAlignContent` (`stackCounterAlignContent`)
  `SPACE_BETWEEN` → `alignContent: "space-between"`; `AUTO` emits nothing.
  Both are wrap-only — never emit them on a single-line stack.
- **Figma GRID auto-layout** (`layoutMode`/`stackMode == "GRID"`; pinned
  plugin typings 1.127.0 expose the full surface, no runtime cast needed) —
  lowers to the existing IR grid contract. Plugin/REST: uniform tracks →
  `repeat(N, 1fr)` from `gridRowCount`/`gridColumnCount`, gaps from
  `gridRowGap`/`gridColumnGap`, children from 0-based
  `gridRowAnchorIndex`/`gridColumnAnchorIndex` (+`grid*Span`) → CSS 1-based
  lines. `.fig` kiwi: `gridRows`/`gridColumns` are GUID-keyed track lists whose
  fractional-index `position` string sorts byte-wise into track order;
  `gridColumnsSizing`/`gridRowsSizing` map track GUID → `{minSizing,maxSizing}`
  (`FLEX`→`fr`, `FIXED`→px, else `auto`); children carry
  `gridRowAnchor`/`gridColumnAnchor` GUIDs. Grid children FLOW: treat GRID
  like flex in the absolute-position, constraints, and mask gates (a grid
  child gets neither coordinates nor constraints). GRID mode ignores
  `stackSpacing` — it is flex residue; the grid gaps own spacing.
- **aspectRatio** — `targetAspectRatio` (kiwi OptionalVector `{x,y}`, REST
  number-or-vector) emits ONLY when some axis is flexible (grow, stretch, or a
  non-FIXED sizing mode). On a fully fixed node Yoga would re-derive the cross
  axis from the ratio and fight Figma's solved size over rounding.
- **min/max sizing** — style-level `min_width`/`max_width`/`min_height`/
  `max_height` (snake; `parse_ir_style` resolves both spellings, native
  materializer lowers to FlexStyle clamps, JS-web lane emits `style.minWidth`).
  Per-axis guard `> 0` — the `.fig` OptionalVector rides unset axes as 0 and a
  zero max collapses the node. Figma honored these while solving, so they only
  bind on host resize.
- **Consumer completions that shipped with this slice**: the JS codegen
  (`design_codegen.cpp`) now lowers explicit `flex_grow` values, `align_self`,
  `aspect_ratio`, `row_gap`/`column_gap`, `flex_wrap`, and `align_content`
  (the constraint fallbacks check `grow_done`/`stretch_done` so an explicit
  value is never doubled); the native materializer + cpp codegen read grid
  templates from `layout.grid_template_*` when the v0/TSX contract attributes
  are absent (previously a Figma GRID switched to grid mode with an EMPTY
  column list and dropped every child), and both gained per-child
  `gridColumn`/`gridRow` line parsing.
- **Known 0.5px caveat**: Pulp's grid engine stretches every child to its cell
  (`layout_grid()` has no per-child alignment yet), while Figma keeps a
  fixed-size child at its own size inside a larger track. On the reference
  file this shows as grid cells 33→33.5px tall (track height) with y-placement
  EXACT (the old absolute path was 0.5 off from rounding). Within the parity
  gate; fix belongs to the grid engine, not the producers.
- Real-file parity: `layout_parity.py` reports byte-identical findings before/
  after on the 1299-node reference frame; the only solved-rect deltas are the
  16 grid cells above (≤0.5px). Tests: `[view][import][autolayout]`,
  fig.test.mjs auto-layout completion block, `figma-plugin/test/layout.test.ts`,
  REST `AutoLayoutTest`.

### Strokes beyond the first uniform solid (per-side, dashed, provenance, multi-paint)

The audit's "Strokes" row (checklist #8). All three producers now lower the
full box-stroke model; the earlier shorthand/stroke-band/per-corner fixes are
untouched (the `vectorStrokeBand` gate in `fig/scene.mjs` still suppresses the
CSS border on baked stroke-band vectors — do not regress it).

- **Per-side box strokes** (Figma `individualStrokeWeights`): emitted as the
  discrete `border_{top,right,bottom,left}_width` fields — ALL FOUR, because
  an explicit 0 is a positive "no edge here" — plus the single Figma stroke
  color repeated as `border_{side}_color` on each painted side; the uniform
  `border`/`border_width` shorthand is omitted in that case. Producer
  spellings (verified against each wire, never guess):
  - `.fig` kiwi: `borderStrokeWeightsIndependent` (bool) +
    `border{Top,Right,Bottom,Left}Weight` (absent side = 0). The fields are in
    `SYMBOL_INHERITED_KEYS` so instances inherit them from masters.
  - plugin: `strokeTopWeight`/`strokeRightWeight`/`strokeBottomWeight`/
    `strokeLeftWeight` (`IndividualStrokesMixin`; `strokeWeight` reads
    `figma.mixed` exactly when they differ — four EQUAL side weights stay on
    the uniform path). Logic in `extract-pure.ts::extractStrokeStyle` (pure,
    testable); `extract.ts` merges style + attributes + diagnostics.
  - REST: the `individualStrokeWeights` object `{top, right, bottom, left}`
    (`figma_rest_export.py::extract_stroke_style`).
  Consumers: `parse_ir_style` reads all eight fields (both spellings), Yoga
  takes per-edge insets, the native materializer applies per-side View
  setters, and the JS codegen lowers painted sides to `setBorderSide` (0-width
  sides emit nothing; the uniform `setBorder` never fires beside them) —
  `design_codegen.cpp::emit_js_box_border`, shared by the container and
  generic-frame fall-through branches.
- **Dash pattern**: a non-empty `dashPattern` (.fig kiwi + plugin) /
  `strokeDashes` (REST) maps to `border_style: "dashed"` — a box border
  cannot express an arbitrary dash array, so this is the honest CSS
  approximation (View's dashed stroke is a fixed 3w/3w SkDashPathEffect) and
  the EXACT array is preserved as the `figma:dash_pattern` attribute.
  Codegen emits `setBorderStyle(id, 'dashed')` beside the border (solid stays
  silent); the native materializer maps `border_style` onto
  `View::BorderStyle` (dashed/dotted/none/hidden — others degrade to solid).
- **Preserved stroke provenance** (namespaced `figma:*` attributes, emitted
  only on nodes with a visible stroke and only for NON-DEFAULT values):
  `figma:stroke_align` (CENTER/OUTSIDE only — INSIDE is how a CSS box border
  already paints), `figma:stroke_cap` (≠ NONE), `figma:stroke_join` (≠ MITER),
  `figma:stroke_miter_limit` (≠ 4; REST's `strokeMiterAngle` in degrees is
  normalized to the limit: `1/sin(angle/2)`, 28.96° = 4.0). No renderer
  consumes these yet — Figma's own SVG/strokeGeometry export bakes caps/joins/
  dashes into geometry, so they are provenance for a future path renderer and
  for fidelity tooling. Do NOT emit them as lowered styles.
- **Multi-paint / complex strokes are never silent**: >1 visible stroke paint
  → `multi-paint-stroke` diagnostic (flattened to the FIRST SOLID); a
  non-solid top paint, no solid paint at all (border dropped), or a `.fig`
  variable-width brush stroke (`dynamicStrokeSettings`/`scatterStrokeSettings`/
  `stretchStrokeSettings`) → `complex-stroke-flattened`. Both codes are
  registered in `DIAGNOSTIC_SEVERITY` (fig lane), emitted with kind
  `capture_partial` (plugin/REST), and mapped in the C++
  `diagnostic_kind_from_code` fallback. Note the behavior CHANGE from the old
  first-visible-solid gate: a gradient stroke sitting ON TOP of a solid now
  flattens to that solid (with a diagnostic) instead of silently dropping the
  border.
- Real-file sanity (designers-pick frame 1:486, 1299 nodes): 0 per-side /
  0 dashed nodes (the file's one `dashPattern` frame sits outside 1:486),
  36 `figma:stroke_align` + 1 cap + 1 join attrs, 20 `complex-stroke-flattened`
  on gradient-stroked ovals the old code dropped silently; envelope otherwise
  byte-identical, `--dump-layout` byte-identical (strokes in this frame are
  paint-only — per-side widths WOULD move Yoga insets on files that use them).
  Tests: `[view][import]...[strokes]`, fig.test.mjs strokes block,
  `figma-plugin/test/strokes.test.ts` (+ serialize schema round-trip), REST
  `StrokesTest`.

### Primitive geometry metadata (arc/donut, star/polygon, smoothing, boolean op)

Audit "Geometry" row. All three producers preserve the primitive-shape fields a
future path renderer needs to rebuild the shape without a re-export from Figma,
as namespaced `figma:*` attributes on the node (the same
provenance-not-lowered contract as the stroke attrs — nothing consumes them
yet; the PNG/faithful-SVG capture preserves the pixels, these preserve the
semantics; tracked in `compat/imports.json`):

| Attribute | Source field (plugin / REST / `.fig` kiwi) | Emitted when |
|---|---|---|
| `figma:arc_data` = `"start,end,inner"` | `ELLIPSE.arcData` (radians, same spelling all lanes) | sweep is not a plain full circle, or `innerRadius > 0` (donut). `.fig` stamps a DEFAULT full-circle `arcData` on every ellipse — emitting unconditionally would grow an attr per node |
| `figma:star_point_count` | `STAR.pointCount` / — / `count` | always on STAR (required to rebuild it) |
| `figma:star_inner_radius` | `STAR.innerRadius` (0..1) / — / `starInnerScale` | always on STAR |
| `figma:polygon_point_count` | `POLYGON.pointCount` / — / `REGULAR_POLYGON.count` | always on POLYGON |
| `figma:corner_smoothing` | `cornerSmoothing` (0..1 squircle factor) | `> 0` only (per-corner radii already ride in style) |
| `figma:boolean_operation` | `booleanOperation`, lowercased | always on BOOLEAN_OPERATION. Kiwi's `XOR` is the Plugin API's `EXCLUDE` — normalize it (`fig/scene.mjs primitiveProvenanceAttrs`) so all lanes share one vocabulary |

- **REST cannot see star/polygon fields.** The REST wire schema exposes
  `arcData`, `cornerSmoothing`, and `booleanOperation`, but NOT
  `pointCount`/`innerRadius` (verified against the REST file-node-types /
  file-property-types docs) — so the star/polygon attrs are plugin/`.fig`-lane
  only. Do not fabricate them in `figma_rest_export.py`.
- Numbers are formatted to at most 4 decimals with trailing zeros trimmed
  (`fmtGeomNum` / `_fmt_geom_num`) so kiwi float32 vs Plugin-API double never
  produce different attr strings; the full-circle test uses a `1e-4` epsilon
  because kiwi's float32 2π ≠ `Math.PI * 2`.
- **Capture policy stays PNG for vector-like leaves.** `ImageView`'s decoder
  is PNG/JPEG; SVG rendering exists only at frame level via the faithful-vector
  lane (`render_mode: faithful_svg` → `DesignFrameView`), which is already
  default-on. Switching leaf `asset_ref` capture to SVG would hand `ImageView`
  bytes it cannot decode — leaf-level SVG is deferred until an SVG-capable
  image path exists.
- Real-file sanity (designers-pick frame 1:486, 1299 nodes): 140
  `figma:boolean_operation` (instance expansion multiplies the 41 raw scene
  nodes), 1 `figma:polygon_point_count`, 0 arc/star/smoothing (every ellipse
  in the frame is a plain full circle); envelope otherwise byte-identical and
  `--dump-layout` byte-identical — the attrs are metadata-only and can never
  move layout.
  Tests: `[view][import][figma-plugin][geometry]` (C++ attr round-trip),
  fig.test.mjs "primitive geometry" test,
  `figma-plugin/test/primitive-geometry.test.ts`, REST
  `test_primitive_geometry_*`.

### Dev metadata + export settings (description, dev status, annotations, export presets)

Audit "Dev metadata" (Medium) + "Export settings" (Low) rows — the last
coverage slice, and PROVENANCE-ONLY BY DESIGN: these attrs are for dev
tooling, diagnostics, and round-trip provenance. NO renderer, codegen path, or
importer heuristic reads them, and export settings NEVER override Pulp's
deterministic PNG/SVG capture policy (they are asset hints, not a capture
contract). Emitters: plugin `extract-pure.ts::extractDevMetadataAttrs`, REST
`figma_rest_export.py::extract_dev_metadata_attributes`, `.fig`
`scene.mjs::devMetadataAttrs`; all ride the free-form attributes passthrough
(`design_ir_json.cpp` ~1232 — no allowlist, every `attributes.*` key lands
verbatim in `IRNode::attributes`). Tracked in `compat/imports.json`
(`dev-metadata` + `export-settings` rows).

| Attribute | Source field (plugin / REST / `.fig` kiwi) | Emitted when |
|---|---|---|
| `figma:description` | `PublishableMixin.description` (COMPONENT / COMPONENT_SET) / the `/nodes` response `components`+`componentSets` maps keyed by node id (NOT on the document node) / kiwi `description`, falling back to `symbolDescription` | trimmed value is non-empty |
| `figma:dev_status` | `devStatus.type` lowercased (`ready_for_dev` / `completed`) / same / **absent — the kiwi schema has no per-node devStatus** (`sectionStatus` is the unrelated section build-status; do not map it) | `devStatus` is set (it is `null` by default) |
| `figma:annotations` | `annotations` → compact JSON `[{label, properties, category_id}]`; label falls back to `labelMarkdown` (`.fig`: `labelV2`) | array non-empty AND at least one entry has something to state |
| `figma:export_settings` | `exportSettings` → compact JSON `[{format, suffix, constraint, contents_only}]` | array non-empty (an export preset existing at all is author intent; within an entry, defaults stay silent) |

- **Value normalization is cross-lane, like `XOR → exclude`:** kiwi
  `imageType: JPEG` → the Plugin API's `jpg`; kiwi constraint types
  `CONTENT_SCALE/CONTENT_WIDTH/CONTENT_HEIGHT` → `scale:N`/`width:N`/`height:N`
  (the `SCALE:1` default stays silent, value via `fmtGeomNum`); kiwi
  SCREAMING_SNAKE annotation property types → Plugin-API camelCase
  (`FILL → fills`, `STACK_SPACING → itemSpacing`, `STROKE_WIDTH →
  strokeWeight`, `STACK_MODE → layoutMode`, `COMPONENT → mainComponent`, …
  — table + mechanical snake→camel in `scene.mjs::annotationPropertyName`).
- **Lane asymmetries (verified, don't "fix"):** REST does not expose
  `contentsOnly`, so `contents_only` never appears in the REST lane. The kiwi
  `Annotation.categoryId` is a file-local GUID ref (not the Plugin API's
  stable category-id string), so the `.fig` lane never emits `category_id`.
  `contents_only` is emitted only when explicitly `false` (Figma's default is
  `true`).
- **Deliberately NOT preserved:** plugin data / shared plugin data (arbitrary
  third-party payloads are envelope noise) and reactions/prototype metadata
  (out of the importer's scope).
- Real-file sanity (designers-pick frame 1:486, 1299 nodes): 16
  `figma:export_settings` (all the default preset every real `.fig` stamps,
  honestly reduced to `[{"format":"png"}]`), 0 descriptions / annotations /
  dev-status in that file (whole-file probe: 92 export-settings nodes, 0 of
  the rest); `--dump-layout` byte-identical before/after — the attrs are
  metadata-only and can never move layout or codegen output.
  Tests: `[view][import][figma-plugin][dev-metadata]` (C++ attr round-trip),
  fig.test.mjs "dev metadata + export settings" test,
  `figma-plugin/test/dev-metadata.test.ts`, REST `test_dev_metadata_*` +
  `test_component_description_*`.

### Figma variables → tokens + per-property bindings

Two halves, both preserved end to end (audit item 5):

- **Token definitions** — the envelope-level `tokens` block
  (`{colors, dimensions, strings}`) → `parse_ir_tokens` → `IRTokens`.
  Canonical names are `"collection/variable"` lowercased, whitespace stripped,
  `/` → `.`; a multi-mode collection emits the default mode under the bare
  name and every other mode suffixed `.<mode-slug>` (e.g. `theme.bg` +
  `theme.bg.dark`); aliases resolve per mode; BOOLEAN → `"true"`/`"false"`
  strings. The plugin's `tokens.ts` and REST's `variables_to_tokens()` share
  these rules; the `.fig` lane keys tokens by the VARIABLE node's raw name.
- **Per-property bindings** — which token drives which node property. Every
  producer emits `figma.bound_variables: {property: tokenName}` in the node's
  figma block; `parse_ir_node` preserves each entry as a
  `figmaBoundVariable.<property>` attribute (opaque passthrough, namespaced,
  additive — the reader preserves, it does not interpret). Property keys use
  the Plugin-API camelCase dialect in ALL three lanes (`fills`, `strokes`,
  `cornerRadius`, `itemSpacing`, …); array-valued properties bind index 0 to
  the bare key and later entries to `<property>.<i>`; nested alias maps bind
  `<property>.<key>`.

Producers:

- **plugin** (`extract.ts` reads `node.boundVariables`,
  `extract-pure.ts::extractBoundVariableBindings` resolves via the token
  pass's `variableIdToName`): an id the token pass didn't capture
  (remote-library / deleted variable) is DROPPED with a
  `variable-binding-unresolved` warning (once per variable) — the plugin has
  the full local table, so an unresolvable id is genuinely dangling.
- **REST** (`figma_rest_export.py`): `boundVariables` rides in the /nodes
  payload on every plan tier, but the `/v1/files/:key/variables/local`
  endpoint (token definitions + names) is **Enterprise-plan-gated** — expect
  HTTP 403 on most files (`variables-endpoint-unavailable` info diagnostic,
  empty token maps). Without the name map, bindings emit the RAW variable id
  (`VariableID:…`) plus one `variable-binding-unresolved` warning per id —
  a stable join key, never a fabricated name. Successful fetches are cached
  (`<file_key>__variables.json`, file-scoped).
- **`.fig`** (`fig/scene.mjs`): scalar bindings come from the kiwi
  `node.variableConsumptionMap.entries[]` (`variableField` enum →
  Plugin-API camelCase via `FIG_VARIABLE_FIELD_PROPERTY` + mechanical
  SNAKE→camel fallback; old-schema entries carrying only the numeric
  `nodeField` are skipped), paint-level color bindings from
  `paint.colorVar` on `fillPaints`/`strokePaints` (first visible bound paint
  per side → `fills`/`strokes`). Guids resolve against the file's own
  VARIABLE nodes — the same names `collectVariableTokens` keys the tokens
  maps by; a guid outside that table (remote-library variable) is dropped
  with a `variable-binding-unresolved` warning, once per variable.

Consumption status (compat `features.variables-tokens`, codegen **partial**):
token definitions feed the importer's existing token/theming plumbing;
bindings are preserved as queryable `figmaBoundVariable.*` attributes for
downstream theming/token-swap tooling, but codegen does NOT yet emit themeable
token references into the generated UI — swapping a token value does not
re-theme an already-generated import. Tests: `[view][import][variables]`
(C++), `bound-variables.test.ts` (plugin), `test_figma_rest_export.py`
variable tests (REST), the `variable bindings` case in `fig.test.mjs` (.fig).

### Grid containers → native grid bridge (NOT Yoga grid)

Pulp's engine has its **own** grid layout (`LayoutMode::grid` + `layout_grid()`
in `view.cpp`, driven by the `createGrid`/`setGrid` bridge + `GridStyle`) — the
vendored Yoga (v3.2.1) has **no** grid API (no `YGDisplayGrid`; grid only exists
on Yoga's unreleased `main`). So "wire grid" for design-import is *not* a Yoga
task — it's emitting `createGrid`/`setGrid` instead of `createCol`/`createRow`.
Facts / gotchas:

- **Parse** (`design_ir_json.cpp`, `parse_ir_layout`): `display:grid`,
  `gridTemplateColumns`/`Rows`, `gridAutoFlow`, and per-item `gridColumn`/`Row`
  (camelCase + snake_case) → `IRLayout.grid_template_columns`/`_rows`/
  `_auto_flow`/`grid_column`/`grid_row` (raw CSS strings).
- **Codegen** (`design_codegen.cpp`): `is_grid = is_container && (display=="grid"
  || a track template present)`. Emits `createGrid` + `setGrid(id,
  'template_columns'|'template_rows'|'auto_flow', …)` + `setGrid(id,'gap',…)`
  (NOT `setFlex` gap), and **suppresses flex `justify_content`/`align_items`**
  (guarded with `!is_grid` — they're meaningless for grid; do NOT wrap the child
  recursion loop, it's interwoven with the flex nudge heuristics and must run for
  grid too). Per-item placement (`emit_grid_item_placement`, folded into
  `emit_position_if_absolute`): `grid_column`/`grid_row` `"N / M"` → `setGrid(id,
  'column_start'/'column_end'/'row_start'/'row_end', N)`.
- **Span**: `"<start> / span <n>"` resolves to `column/row_end = start + n`.
  Still deferred (auto-placed): span-WITHOUT-a-start-line, named lines, and
  `minmax()` track sizing — `setGrid column/row_start/end` take **ints** only.
  Stays within Flex+Grid (CLAUDE.md layout-model contract).
- Native arm only; `compat.json features.grid-container` tracks it (parsed
  handled, codegen partial). Tests: `[view][import][grid]`.

### Ordered paint stacks, paint opacity, image scale modes (audit item 7)

Figma renders `fills` bottom→top (array index 0 at the BOTTOM). The IR has
exactly three background slots — one solid color, one gradient, one image —
painted in that order, so a stack is representable exactly when it reads
`[solid…, gradient?, image?]` bottom→top. All three producers (plugin
`extract-pure.ts::lowerFillPaints`, REST `figma_rest_export.py::
lower_fill_paints`, `.fig` `scene.mjs` styleFor box branch) share this slot
scan, kept in field-for-field lockstep:

- **Leading solids composite source-over** (exact under NORMAL blend) into
  `background_color` — a `[#4b4d51, white@0.55]` thumb is `#aeafb1`, not the
  dark base. A fully **opaque solid trims the stack** below it (also exact).
- **Paint-level opacity** (distinct from layer opacity) folds into the emitted
  color alpha: solids via `paintToColor`/`paint_to_color`/`compositeSolids`,
  gradients by scaling every stop's alpha. An IMAGE paint's opacity folds into
  the layer opacity only on a **childless** node (identical compositing);
  with children it raises `image-opacity-dropped` instead of fading them.
- **Image scale modes**: on image-shaped nodes (plugin/.fig lanes, where an
  image fill becomes an image node) `scaleMode`/`imageScaleMode` lowers to
  `object_fit` — FILL→`cover`, FIT→`contain` — which `ImageView::paint`
  actually honors (real rendered fix: a FILL photo no longer imports
  stretched). CROP (spelled `STRETCH` in `.fig` blobs) approximates as
  `cover` + `image-scale-approximated`; TILE has no painter → stretch default
  + the same diagnostic. On frame-shaped nodes (REST lane keeps
  `background_image`) it lowers to `background_size`/`background_repeat`
  (View storage slots via setBackgroundSize/setBackgroundRepeat; raster
  background paint honoring is still the deferred `style_extras` slot).
- **No silent drops**: paints beyond the slots raise `multi-paint-flattened`;
  VIDEO/PATTERN/unknown families raise `unsupported-paint-type` (and never
  shadow a lowerable solid below them); non-NORMAL paint `blendMode` raises
  `paint-blend-unsupported` (paint still lowers, composited normal). New
  `.fig` codes MUST be registered in `DIAGNOSTIC_SEVERITY` or both consumers
  drop them as invisible `info`.
- IR: `IRStyle.object_fit` / `background_size` are parsed (`objectFit` /
  `backgroundSize`, camel or snake) + emitted by JS/C++ codegens + the native
  importer; the plugin envelope schema carries `object_fit`/`background_size`
  (regenerate `types.generated.ts` via `npm run gen-types` after schema
  edits). Tests: `[view][import][paints]`, `tools/figma-plugin/test/
  paints.test.ts`, `test/test_figma_rest_export.py::PaintStackTest`, and the
  `.fig` `Paint Lab` fixture frame (regenerate `synthetic.fig` via
  `make_synthetic_fig.mjs` — a staleness gate compares decoded content).

### Layer blend modes: the shared supported-blend table + group isolation

A layer that COMPOSITES differently is not cosmetic (the motivating file lays
a light noise texture over its panels at `MULTIPLY`; ignoring it painted every
panel ~25/255 too bright, silently). One supported-blend table now spans all
three producers AND the consumer, kept in lockstep:

- **The table** (15 CSS-equivalent Figma modes): DARKEN, MULTIPLY, COLOR_BURN,
  LIGHTEN, SCREEN, COLOR_DODGE, OVERLAY, SOFT_LIGHT, HARD_LIGHT, DIFFERENCE,
  EXCLUSION, HUE, SATURATION, COLOR, LUMINOSITY — lowered by spelling
  transform (UPPER_SNAKE → lowercase-hyphen) into `style.mix_blend_mode`.
  Lives in `.fig` `scene.mjs::FIGMA_BLEND_CSS`, plugin
  `extract-pure.ts::FIGMA_BLEND_CSS` (+ `lowerLayerBlendMode`), REST
  `figma_rest_export.py::_FIGMA_BLEND_CSS` (+ `blend_mode_to_css`), consumer
  `design_ir_json.cpp::is_supported_blend_keyword`.
- **Unmappable modes** (LINEAR_BURN, LINEAR_DODGE, future families) lower to
  NOTHING and raise `blend-unsupported` in every producer — never
  approximate: LINEAR_BURN's natural CSS spelling `plus-darker` maps to the
  ADDITIVE kPlus in Skia/Chromium, which would LIGHTEN a layer the designer
  asked to darken. The raw Figma mode still rides in `figma.blend_mode` for
  provenance; the consumer promotes it into `style.mix_blend_mode` only when
  supported (unmappable raws stay out silently — the producer already
  diagnosed them, so the consumer must not double-diagnose).
- **Consumer chokepoint**: `validate_blend_modes` (called on every adapter
  parse path) clears any `style.mixBlendMode` outside the table WITH a
  `blend-unsupported` diagnostic — an unknown keyword is invalid CSS on the
  web path and a SILENT normal-fallback in `setMixBlendMode` on the native
  path, so it must never reach codegen. `plus-lighter`/`plus-darker` stay
  accepted for CSS-authored sources (the bridge maps both to kPlus).
- **Group compositing**: container `PASS_THROUGH` (Figma's default) IS the
  default web/native behavior — dropped silently and correctly. An EXPLICIT
  `NORMAL` on a container is Figma's "isolate" (CSS `isolation: isolate`);
  the flat lowering has no isolation layer, so when the subtree actually
  blends, every lane raises `group-isolation-approximated` (gated on a
  blending descendant — inert isolation is a no-op). A container with its own
  non-default blend needs nothing: CSS `mix-blend-mode` itself forms an
  isolated group, matching Figma. Faithful SVG capture remains the honest
  path for exact isolate-group compositing.
- **Layer vs paint channels stay separate**: layer opacity → `IRStyle.
  opacity`, layer blend → `mix_blend_mode`; paint opacity folds into paint
  color alpha and paint blend raises `paint-blend-unsupported` (see the
  paint-stack section above). No field double-applies.
- Plugin envelope schema carries `style.mix_blend_mode` (regenerate
  `types.generated.ts` after schema edits). Tests: `[blend]` in
  `test_design_import_ir.cpp` + `test_design_import_fidelity.cpp`,
  `tools/figma-plugin/test/blend.test.ts`,
  `test_figma_rest_export.py::LayerBlendTest`, and the `.fig` blend +
  isolate-group tests in `fig.test.mjs`.

### Radial / conic background gradients

Linear gradients were end-to-end; radial/conic used to round-trip the CSS string
but the renderer flattened them to the first stop color. The canvas + Skia +
CoreGraphics backends ALREADY implement radial/two-circle/conic — the gaps were
only the CSS parser, the View paint dispatch, and the Figma exporter:

- **Bridge parser** (`widget_bridge.cpp`, `setBackgroundGradient`): now parses
  `radial-gradient([circle][at X% Y%], stops…)` and `conic-gradient([from
  <angle>][at X% Y%], stops…)` in addition to linear (shared `parse_stops`
  lambda). Center defaults to 50% 50%; conic `from` is offset by −90° because
  CSS measures from the top while the canvas sweep starts at +x.
- **View** (`view.hpp`/`view.cpp`): `set_background_gradient_radial`/`_conic`
  store kind (`bg_gradient_type_` 2/3) + center/radius/angle; `View::paint`
  dispatches to `canvas.set_fill_gradient_radial`/`_conic`. `radius_frac`
  defaults to ~farthest-corner (0.7071 × max(w,h)). CSS radial extent keywords
  (`closest-side`/`closest-corner`/`farthest-side`/`farthest-corner`) map to an
  approximate `radius_frac` in the bridge parser — exact per-keyword geometry
  (needs w/h+center) and explicit `px` radii stay deferred (codegen partial).
- **Figma export** (`figma_rest_export.py`): `GRADIENT_RADIAL`/`GRADIENT_DIAMOND`
  → `radial-gradient(...)`, `GRADIENT_ANGULAR` → `conic-gradient(...)` (diamond
  approximated by radial). Falls back to flat only when there are no stops.
- **Design-import codegen is unchanged** — it already emits the gradient string
  verbatim to `setBackgroundGradient`; radial/conic now render instead of
  flattening.
- Tests: `[view][widget-bridge][gradient]` (parser→kind), `[view][gradient]
  [render]` (radial/conic differ from a flat fill — proves not the fallback),
  `[view][import][codegen][gradient]`, and the figma exporter python tests.
  `compat.json features.radial-angular-diamond-gradient`: parsed handled,
  codegen partial.

### Background gradients: the JS lane and the native/baked-C++ lane are SEPARATE

The section above is the **JS / scripted-UI lane** (`setBackgroundGradient`,
emitted by `generate_pulp_js`). There is a second, independent lane — the
**native materializer + baked C++ codegen** — and until 2026-06 it dropped
`background_gradient` *entirely* (linear included), even though `IRStyle`
carried it and `View` could paint it. Fixing one lane does NOT fix the other;
they share no code unless you make them.

- **Shared parser** (`core/view/src/css_gradient.cpp`,
  `apply_css_background_gradient`): the CSS linear/radial/conic parser was
  lifted out of `widget_bridge.cpp` into this free function. All three lanes now
  route through it — the JS bridge (`setBackgroundGradient` delegates), the
  native materializer, and baked C++ codegen — so gradients resolve identically.
- **Native materializer** (`design_import_native_common.cpp`,
  `apply_visual_style`): now calls `apply_css_background_gradient(view, …)` right
  after `set_background_color`. The gradient paints over the solid base color.
- **Baked C++ codegen** (`design_cpp_codegen.cpp`, `emit_visual_style`): emits a
  verbatim `pulp::view::apply_css_background_gradient(*var, "linear-gradient(…)")`
  runtime call plus `#include <pulp/view/css_gradient.hpp>` in the generated
  source prologue.
- **Why it mattered:** this was the dominant ELYSIUM dark/light parity gap. The
  light "hero" panel (`Rectangle 5`, a `linear-gradient(to bottom,#e4edf6,
  #b7c8db)`) and the cube/prism/tuning illustration fills are all CSS gradients.
  With them dropped, the panel rendered dark `#1c1d1d` and the `position_cylinder`
  GPU-ROI similarity was 0.055; after wiring it jumped to 0.979 (`range_prism`
  0.029→0.78, `grains_knob_cap` 0.15→0.79). Verify with
  `pulp-test-mac-platform-harness` (`PULP_DESIGN_GPU_DUMP_DIR=… ` dumps ROIs).
- **Gotcha — stale CLI binary:** after touching `emit_visual_style`, rebuild
  `pulp-import-design` before re-emitting `--emit cpp`; the tool links
  `pulp-view-core` statically and a stale binary silently emits the old output
  (no gradient call), which reads as a codegen bug that isn't there.
- Tests: `[view][import][native-materializer][gradient]` (materializer applies
  it), `[gradient]` cpp-emit section in the always-built `pulp-test-import-design-tool`
  (so the codegen path is covered even when the planning-gated cpp-codegen target
  is skipped), plus the gated `pulp-test-design-import-cpp-codegen`.

### Per-range text styles → nested `<span>`s

A text node used to take the FIRST-CHAR dominant style only (one run); mixed
text (a bold word, a colored span, a different size mid-string) lost its
per-range styling. Now:

- **IR**: `IRNode.text_runs` — an ordered list of `IRTextRun{start,end +
  optional font_size/font_weight/font_style/color/letter_spacing/
  text_decoration}`. `[start,end)` are offsets into `text_content`. The dominant
  style stays the node default; runs override.
- **Parse** (`design_ir_json.cpp`): reads a `runs`/`textRuns` array (start/end +
  the per-run fields, plus `italic:true` → `font_style:"italic"`). Source-
  agnostic.
- **Figma export** (`figma_rest_export.py`, `extract_text_runs`): groups
  consecutive `characterStyleOverrides` ids into ranges and resolves each
  through `styleOverrideTable` (fontWeight, fontName.style→italic,
  letterSpacing.value, textDecoration, fills→color). Emitted as the node's
  `runs`.
- **Codegen — web (primary)**: `emit_web_text_runs` emits the covered ranges as
  styled `<span style=…>` children and the gaps as plain `createTextNode` (so
  gaps inherit the dominant style). Single-run text keeps the plain
  `.textContent` path (no regression).
- **Codegen — native**: now wired. The native arm emits `setTextRuns(id,
  [...])`; the bridge builds a `canvas::AttributedString` from the runs over the
  Label's dominant style, and `Label::paint_attributed_` draws each span with
  its own font/color (advancing x by `measure_text`) for a SINGLE-LINE label.
  Multi-line mixed text still degrades to the dominant single-style path (the
  span loop is single-line), so `compat.json features.text-per-range-styles`
  stays **codegen partial**. `Label::set_attributed_string` + the `setTextRuns`
  bridge fn are the wiring (offsets are UTF-8 byte offsets, same as the web arm).
- **Offsets are UTF-8 BYTE offsets** into `text_content`. The Figma exporter
  builds a UTF-16-code-unit → UTF-8-byte map (`characterStyleOverrides` is
  UTF-16-indexed: a BMP char is 1 unit, an astral char / emoji is a surrogate
  pair = 2 units) so a run after an emoji lands on the right byte — a plain
  code-point slice was off by a byte-position per astral char. `emit_web_text_runs`
  slices by byte and snaps boundaries forward to the next codepoint start
  (continuation bytes are `10xxxxxx`) so a stray mid-codepoint offset never emits
  invalid UTF-8. Tests: `[view][import][text]` (incl. a multibyte case) + the
  figma exporter python tests (incl. an emoji/surrogate-pair case).

### Multi-frame components & swap-link toggles (mode frames)

A component with more than one state frame (e.g. a keyboard's typing vs piano
mode, switched by an in-design toggle) maps to `DesignFrameView`'s multi-frame
support, NOT a crop or a parallel view:

- Export each state **sub-frame** standalone (`figma_rest_export.py --node
  <sub-frame-id>`). When the design stacks the states in one spec frame to show
  them at once, the sub-frames are the individual states — import each as its
  own faithful SVG. `MusicalTypingKeyboard` (nodes 187:15 typing / 187:349
  piano) is the reference: `DesignFrameView(svg0, …)` + `add_frame(svg1, …)`.
- `set_active_frame(i)` swaps the rendered SVG AND the intrinsic size (the host
  re-lays-out), releasing any held momentary key first.
- The in-design toggle button is a `DesignFrameElement::Kind::swap` element with
  `target_frame` set — a click calls `set_active_frame`. This is the importer's
  `swap` link (see `planning/2026-06-17-figma-interaction-linking-vocabulary.md`
  for the swap / resize / modal / popover / navigate verb set).
- **Hit-rects are per-frame, in the sub-frame's own coords.** Pull them from the
  Figma node's `absoluteBoundingBox` minus the frame origin; the standalone SVG
  export adds a uniform shadow margin (6px for these frames). Do NOT transcribe
  combined-frame coordinates — the standalone export re-origins everything.

### Design-import IR round-trip + review-hardening gotchas

Lessons from the di-1..di-5 closeout review — keep these invariants:

- **Serialize every new IR field.** `serialize_design_ir` →
  `write_ir_layout_json` / `write_ir_node_json` must emit any field
  `parse_ir_layout` / `parse_ir_node` reads, or a frozen `.pulp` / `--emit
  ir-json` round-trip silently drops it. Constraints (`constraints:{horizontal,
  vertical}`), grid (`gridTemplate*`/`gridAutoFlow`/`gridColumn`/`gridRow`), and
  text `runs` are all written now; mirror this for future IR additions and add a
  `[serialization]` round-trip test.
- **Pencil stroke lives in an attribute.** Shape stroke can arrive as
  `attributes["stroke_color"]` (+ `stroke_width`/`stroke-width`), NOT
  `style.border_color`. `synthesize_primitive_paths` consumes both, else a
  stroked Pencil rect/ellipse synthesizes to an invisible `fill:none` path.
- **Grid needs a column track.** The native grid engine drops all children when
  its column list is empty, so a `display:grid` node with no
  `gridTemplateColumns` gets a default `'1fr'` column at codegen (don't emit a
  bare `createGrid` with no template).
- **Web-compat run children don't inherit.** Pulp's web-compat `<span>` Labels
  don't inherit typography from the parent, so `emit_web_text_runs` copies the
  node's dominant style onto each run child before applying the run override.
  A `STRETCH` constraint now also emits `min_width`/`min_height: '100%'` so it
  fills its axis even when the node has an explicit cross-axis size (Yoga clamps
  up to the min) — no longer a no-op. (Text-run UTF-16→byte offset conversion is
  also handled now — see the per-range text section.)

### Faithful-vector lane (Plan B): `faithful_svg` render mode → `DesignFrameView`

A parallel, newer rendering strategy to the per-widget sprite/native lanes
above: instead of recognizing each widget and rebuilding it, render the
node's **own SVG export** pixel-faithfully and overlay native interaction.
This is the lane that makes an imported frame look identical to the source
(gradients, multi-layer drop shadows, masks) while staying interactive.

**This lane is the DEFAULT** across every producer (REST exporter, plugin UI,
headless runner). A plain import yields the faithful frame WITH live overlays
(knobs, search field, dropdowns, steppers, tab groups). The legacy flat,
static node-tree export is opt-OUT: `--no-faithful-vector` (REST / headless)
or `extractScene(nodes, {faithfulVector:false})` (plugin API). Forgetting an
opt-IN flag was the old failure mode — a fresh import came through static /
non-interactive — so the default is flipped. When no frame SVG is obtainable
(e.g. `--node-json` with no token and no `--frame-svg`) the lane degrades
gracefully to the flat export with a warning.

Pieces, source-of-truth → runtime:
- **Rendering** — `Canvas::draw_svg` (SkiaCanvas, Skia `SkSVGDOM`, `libsvg.a`)
  renders an SVG document pixel-faithfully. Knob animation = wrap the needle
  `<path>` in `<g transform="rotate(a cx cy)">` and re-render; the rest of the
  chrome stays pixel-exact. `DesignFrameView` (core/view) renders the SVG,
  auto-crops to the panel (largest in-frame `<rect>`, frac 0.15–0.97 of the
  frame), and overlays interaction from a TYPED element list — it does NOT
  guess widgets from SVG structure.
- **IR** — a node opts in with `render_mode = NodeRenderMode::faithful_svg`,
  points `svg_asset_id` at an `IRAssetManifest` entry (mime `image/svg+xml`),
  and carries `interactive_elements[]` (`IRInteractiveElement`: cx, cy,
  hit_radius, svg_patch_d, default_value, source_node_id, **label**). These are
  source-side semantics filled by the importer, NOT inferred from the SVG.
  `InteractiveElementKind` is deliberately separate from `AudioWidgetType`.
  - **`source_node_id` now survives to the live element.** `to_frame_elements()`
    copies it onto `DesignFrameElement`, exposed at runtime via
    `DesignFrameView::element_source_node_id(i)`. So a live overlay can be mapped
    back to its design node — the dev inspector's **Wiring** tab (Cmd-I) uses this
    to flag controls that came from Figma but aren't wired up, and to fetch the
    matching frame. Previously only `stable_anchor_id` (`"figma:<id>"`) survived.
- **Element labels (§2.1 auto-labeling)** — `label` is the human-readable
  parameter NAME a host shows (embed ABI v5 `PulpEmbedParamInfo.name`), taken
  from the control's source Figma **layer name** when meaningful. The REST lane's
  `_node_label()` is deliberately conservative — it returns `""` (→ consumer
  falls back to the binding key, no regression) for auto-generated names
  (`Ellipse 12`, `Frame 41`, bare numbers) AND structural/kind words
  (`Dropdown`, `Search`, `Knob`, `Value`, …), because a WRONG name is worse than
  the synthetic key. `_label_elements()` assigns it: overlays resolve via their
  `source_node_id`; geometry knobs (no node link) match the named node whose
  frame-local center lands within the knob's hit radius (same coordinate
  convention as `_name_override_knobs`). unit/range are a follow-up — only the
  name flows today. Plugin-lane (TS) parity is the remaining lockstep item.
- **Regex hardening (watch out):** the import codegen scans the source for JS
  keyboard shortcuts (`extract_keyboard_shortcuts`, `design_import_shortcuts.cpp`).
  Because the faithful-vector envelope embeds a ~1 MB base64 SVG data URI, any
  unbounded leading-identifier regex over that source catastrophically
  backtracks (the `\b(\w{1,64})` bound fixes it). When adding new source-scanning
  regexes to the importer, bound/anchor the quantifiers — a 1 MB embedded asset
  is the realistic input, not a few KB of JSX.
- **Producer (REST lane)** — `figma_rest_export.py` (faithful-vector default-on;
  `--no-faithful-vector` for the legacy flat export) fetches
  the frame's own SVG (`/images?format=svg`, or `--frame-svg FILE` offline),
  embeds it as a `data:image/svg+xml;base64` asset (so the importer always
  resolves it — no dependency on local_path stamping), sets the root's
  `render_mode`/`svg_asset_id`, and attaches `interactive_elements` from
  `parse_frame_knobs(svg)`. That detector is the geometry auto-detect ported
  from the vector-knob PoC: a knob DOME is a gradient `<circle>` (`fill="url("`,
  r≥8); its NEEDLE is a thin **light-stroked** (`white` or `#ABABAB` — dark
  ticks are `#506274`) short vertical `<path d="Mx1 y1Lx2 y2">` just above the
  dome; pair each needle to its nearest dome and emit the EXACT `d` as
  `svg_patch_d` so the runtime can rotate that one path. `--knob-name SUBSTR`
  (repeatable) is the **name override**: it supplements geometry with any
  node whose name contains the substring (frame-local center from its abs
  bbox), but those carry NO `svg_patch_d` (hit + value, no visual rotation),
  the honest fallback for a knob geometry missed.
- **Rate limits (REST lane)** — every Figma GET in `figma_rest_export.py` routes
  through `figma_get()`, which honors the `429 Retry-After` header (capped
  exponential backoff when absent), retries transient 5xx + read-phase
  timeouts/resets, and on a terminal 429 raises with the diagnostic headers
  (rate-limit type / plan tier / upgrade link) instead of a traceback. **Watch
  out:** `/images?format=svg` (frame SVG) and the PNG captures are Figma **Tier-1**
  endpoints whose budget depends on the *plan of the file being requested* — a
  Starter-plan file can throttle a Full-seat token hard. Don't fire ad-hoc
  validation curls against `/images` next to an export; if you already have the
  SVG/nodes JSON, feed them back via `--frame-svg` / `--node-json` to spend zero
  budget on re-runs.
- **Producer (plugin lane)** — the Figma plugin mirrors the REST lane in
  lockstep, faithful-vector default-on: `extractScene(nodes)` (defaults
  `faithfulVector:true`; pass `false` to opt out) or headless
  `run-headless.mjs <node>` (default; `--no-faithful-vector` opts out, and
  injects the `FAITHFUL_VECTOR` global) captures each frame's SVG via
  `captureExportedNode(node,"SVG")`, decodes the bytes with `decodeSvgBytes`
  (the sandbox has NO `TextDecoder`), and runs the SAME knob detector
  (`src/faithful-vector.ts`, kept identical to the Python `parse_frame_knobs`).
  `serialize.ts` emits the three keys; the envelope schema
  (`figma-plugin-export-v1.json`) documents them. Keep `faithful-vector.ts`
  and `figma_rest_export.py`'s detector in sync — both are ES-conservative
  regex passes over the SVG text.
- **Materializer** — `materialize_node` (`design_import_native_common.cpp`)
  branches on `faithful_svg` first and builds a `DesignFrameView` via
  `make_faithful_svg_frame`: `resolve_svg_document()` resolves the SVG text
  from the asset — `data:image/svg+xml` (base64 AND percent-encoded) or an
  on-disk file (`local_path` / `file://` `original_uri`), read host-side at
  materialize time. An unresolved/missing SVG emits a
  `native-materialize-faithful-svg-unresolved` diagnostic and returns null so
  the node FALLS BACK to normal materialization — a bad asset degrades, never
  blanks the frame.

### Interactive overlays beyond knobs (Plan B "full A")

Knobs are **SVG-patch** (rotate the needle path in the SVG — pixel-perfect).
The other controls are **native-overlay**: `DesignFrameView` is a composite
View that hosts an opaque child widget over the element's rect (`build_overlays`
in the ctor, positioned in `layout_children()` via the SAME `panel_transform`
the SVG is painted with, so they track scaling/letterbox; `View::hit_test`
routes events to them, knob hit-test is the parent fallback). `IRInteractiveElement`
+ `DesignFrameElement` carry `kind {knob,text_field,dropdown,tab_group,stepper}` +
a rect (x,y,w,h) + `options`/`selected_index`/`placeholder`.
- `text_field` → `TextEditor` (tap-focus + caret + accent focus ring). To keep
  the baked leading icon (e.g. a search magnifier) visible, the producer INSETS
  the overlay rect to start at the placeholder text's x (past the icon) and emits
  the field's own box color (`bg_color`, from the box's SOLID fill). The overlay
  paints that exact color, so the inset edge — and any corner curves — blend
  seamlessly with the still-baked box+icon (same-color reveal). Empty `bg_color`
  → a default dark field. This is general (any leading-icon field), detected from
  source structure, not a per-design constant.
- `dropdown` → `ComboBox` (set_items from `options`; opens a popup on click).
  A real dropdown is detected only when the "dropdown"-named FRAME has a
  DOWN-chevron child (Material `expand_more`) AND its shown text isn't the
  unconfigured placeholder "Dropdown". Options carry ONLY the real shown value:
  a static design defines no alternatives, and ELYSIUM's selectors are plain
  frames (not component instances), so there are no variants to enumerate —
  fabricating placeholders would be misleading. A design that defines component
  variants would source the full list from its property definitions.
- `stepper` → `DesignStepper` (a `< >` header value cycled in place: paints the
  current option centered with `<`/`>` chevrons; left half steps to previous,
  right half to next, clamped — nothing painted behind the text so the header
  chrome shows through). This is the SAME "Dropdown"-named FRAME family as the
  dropdown, discriminated by its chevron child: a `< >` PAIR (`Frame 41` in
  ELYSIUM, or an explicit left+right chevron pair) and NOT a down-chevron, with
  shown text != "Dropdown". (Previously these were dropped as faithful-static;
  they are now live steppers.) Options carry only the real shown value, like
  dropdowns — ELYSIUM's `< >` headers are actually a decorative pair of
  `expand_more` icons inside `Frame 41` with no defined alternatives, so there is
  nothing to step to until a variant-carrying design or the developer supplies
  the list.
- `tab_group` → `DesignTabGroup` (a compact segmented control drawn opaque over
  the tab strip; click a slot to move the selection highlight). Detected
  structurally (`detect_tab_group`): a row of ≥3 similar-width container children
  with short text labels; the child carrying a visible SOLID fill is the selected
  tab. `--select-tab=N` is the design-import-standalone demo flag for capturing it.
- `momentary` → press/release primitive for keys / pads / drum triggers /
  sustain / transport. `on_gesture_begin(i)` = note-on, `on_gesture_end(i)` =
  note-off; `set_element_value(i, 1/0)` lights it via a NATIVE accent overlay
  (the SVG is never recolored, so a re-export still skins it). Carries `note`
  (typing keys = relative semitone 0..17, piano = absolute MIDI; consumers map
  by `note`, never positional index — a re-export may reorder) and `view_group`
  (per-view scope, e.g. typing=0 / piano=1; `set_active_view_group` releases any
  held key so notes never stick across a mode switch). `MusicalTypingKeyboard`
  is the reference consumer; its keys are code-defined (a rect table extracted
  from the Figma frame), NOT discovered from SVG geometry — the faithful baked
  SVG (dark) provides the pixels, the element list provides behavior. Gotchas:
  (1) **smallest-area hit tiebreak** — among momentary rects containing the
  point the smallest wins, so a narrow black key beats the white key it overlaps
  (order-independent, survives re-export reordering). (2) **highlight must carve
  out notches** — a lit white (larger) key's overlay rect would otherwise bleed
  teal over the black keys notching its top, swallowing them; `paint()` subtracts
  any smaller same-view momentary rect that GENUINELY notches the key's top
  (x-overlap, starts at/above the top, AND reaches down into the key), painting
  the highlight as bands that leave the black-key channels dark. The reach-into
  test is load-bearing when one frame shows two keyboards in the same view group
  (typing row above a piano keyboard): their keys overlap in x but not in y, so
  without it a typing black key was mistaken for a notch on a piano key and drew
  a tall bar across the inter-keyboard gap. The notch bottom is clamped to the
  key. (3) **match the design's own pressed paint, don't invent a flat color** —
  the lit fill replicates the figma's pressed-key gradient (in the MTK export,
  `paint36`: accent teal 26%→100% opacity, key top→bottom), set per key over its
  own height via `set_fill_gradient_linear`. A flat fill reads as a uniform slab
  and the key letter vanishes; the gradient's lighter top lets the letter show.
  Footgun that caused exactly this: `canvas::Color` channels are **float 0–1**
  (`Color::rgba8` takes 0–255 and converts) — assigning `c.a = 120` clamps to
  fully opaque, NOT 47%. Build alpha variants with `Color::rgba(r, g, b, 0.26f)`.
  All in `DesignFrameView` (`core/view/src/design_frame_view.cpp`).

### Re-importing a design revision (round-trip)

Designers WILL revise a frame and expect the change to flow back into Pulp.
The round-trip is **figma frame → `figma_rest_export.py` → embed**, and the
canonical source of the faithful frame is the **figma node**, NOT a design-system
export folder. Hard-won steps (from re-importing the MTK after an even-spacing fix):

1. **Find the source node.** It's in the embed's provenance header (e.g.
   `musical_typing_keyboard_svg.cpp`: "Figma file `<key>` node `<id>`"). A
   design-system HTML/CSS export folder (tokens, components, `*.html`) does NOT
   contain the detailed frame SVG — only a schematic. Don't try to source the
   faithful SVG from it.
2. **If the node still returns the OLD design, the revision lives elsewhere.** A
   byte-identical re-export means the figma node wasn't the thing edited (the
   designer changed a different node, or only the HTML/CSS kit). Either get the
   updated node's URL, or update the figma node yourself from the canonical spec
   (see step 3), then export.
3. **Map CSS intent → figma layout.** A folder's `flex` even-spacing
   (`.mtk-keys { display:flex; padding:6px; gap:0 }`, keys `flex:1`) is NOT a
   one-value figma fix. Figma auto-layout beds with FIXED-width, MIN-aligned
   children + ABSOLUTE overlays need: symmetric padding **and** the flow children
   set to fill (`resize` to `content/n`) **and** the absolute overlays
   repositioned onto the new boundaries (`use_figma`). Verify with a figma
   `get_screenshot` of the bed before exporting.
4. **Export + regenerate the embed.** `figma_rest_export.py --file-key … --node …
   --out scene.pulp.json --faithful-vector`. The frame SVG lands in the scene's
   `asset_manifest` as `frame-svg-<node>` → an `assets/<hash>.svg` file (NOT an
   inline JSON field). Base64-chunk it (≤8000 chars/literal) into the embed cpp,
   matching the existing `kParts[]`+join format.
5. **Re-extract interactive rects.** Geometry shifts on any spacing change. Re-run
   the hit-rect extraction (path bounding boxes from the new SVG) and update the
   element tables; positional index is NOT stable across a re-export.
6. **Neutralize baked pressed/selected states.** A revision may bake a demo
   "pressed" key (a teal gradient like `paint36`). For the interactive widget,
   rewrite that gradient's stops to the resting fill (`#EBEEF1`→`white`) so the
   live overlay owns every highlight — otherwise the widget shows a stuck-lit key.
7. **Verify headless both ways.** Resting render: even spacing, no stuck keys. Lit
   render: overlay lands exactly on the new key positions with the design's
   pressed gradient.

The `pulp-design-import-standalone` example has demo flags to capture overlay states
headlessly: `--focus-search` (focus ring) and `--open-dropdown=SUBSTR` (opens
the matching ComboBox's popup). Use `--raster=out.png` (Skia) not `--screenshot`
when the session's live GPU-present path is wedged — raster is the same paint,
GPU-safe, and DOES render the open ComboBox popup (it paints its list inline).

- **Detection is SOURCE-METADATA, not SVG geometry** (Codex): the producer
  walk has node names + `absoluteBoundingBox`. `detect_overlay_controls`
  (figma_rest_export.py) finds a node named ~`search` (skipping the
  `ic:round-search` icon; ELYSIUM names the placeholder TEXT "Search" with the
  field as its parent group). The plugin lane must mirror this when wired.
- **OCCLUSION GUARD — skip controls painted over by a later opaque node.** A
  design can carry a leftover/under-layer control that is fully covered by a
  panel drawn on top (e.g. a stray "Radio Button" 1/2/3/4 buried under the
  envelope graph). The baked SVG hides it, but a naive detector resurfaces it as
  a live overlay floating ON TOP. Guard: in detection, drop any candidate whose
  bbox is fully contained by a node painted AFTER its entire subtree (document
  preorder = paint order) that has an opaque fill. Key invariants that took a
  round to get right: (1) key the paint-index/subtree maps on OBJECT IDENTITY,
  not the figma `id` string (absent/dup ids collide); (2) compare against the
  candidate's `subtree_end`, NOT its own index — else the control's OWN
  background `<rect>` (a descendant that fills it) looks like an occluder and
  drops the control (this nuked the search field on the first cut); (3) "opaque"
  must accept GRADIENT fills, not just SOLID — ELYSIUM's occluding panel is a
  radial gradient. REST: `_opaque_cover` checks SOLID `a>=.99` or GRADIENT with
  all stops `a>=.99` + node opacity. TS (`faithful-vector.ts`): proxies opacity
  via `style.background_color` presence (the extractor sets it for SOLID and the
  flat fallback of GRADIENT) + the node `opacity` field; `Map<OverlayNode>` keys
  give object identity for free. KEEP both lanes' guards in sync.
- **COORDINATE GOTCHA (cost me real time):** the Figma node tree is frame-local
  (root at abs origin, 1000×600 for ELYSIUM) but the SVG export adds the
  drop-shadow margin (1146×746, panel at (73,50)). Node coords are NOT SVG
  coords. Map every node-derived overlay rect:
  `svg = (node_abs - root_abs) + panel_origin`, where `panel_origin` is
  `parse_panel_bounds(svg)` (mirrors `DesignFrameView::detect_panel`). Knobs are
  immune (parsed straight from the SVG); only node-tree-derived overlays need it.

Gotchas:
- `draw_svg` rebuilds the `SkSVGDOM` every call (a parsed-DOM cache is a
  planned optimization) — fine at interactive rates, but don't call it in a
  hot per-frame loop without profiling.
- The ASan/UBSan macOS runners link a PARTIAL Skia where `draw_svg` (and
  url()-mask compositing) is a no-op — render-comparison tests must SKIP when
  `similarity >= 0.999` rather than fail (mirror `test_image_view_fill`'s
  url()-mask guard and `test_design_frame_view`).
- Pure IR/round-trip + materializer tests (no actual SVG compositing) are
  safe on every lane, so put coverage there; reserve the SKIP-guarded
  render-diff assertions for the lanes that can composite.

### Interactive (turnable) sprite knobs — `--knob-style sprite`

`--knob-style sprite` no longer DEMOTES a recognized knob to a static image.
A captured-art knob now stays a native `Knob` that actually TURNS:

- **Importer hoist** (`resolve_sprite_skins` in `sprite_skins.cpp`, in the
  `!use_silver_knobs` block): the disposition is keyed on how many asset-backed image children
  (captured layers) the knob has:
  - **exactly one** (the ELYSIUM shape — a captured disc + a separate stroked
    pointer the native notch replaces): HOIST the disc's `asset_ref` +
    `renderBounds` onto the knob node and erase the child. The asset-resolution
    pass then stamps `asset_path` + `art_core_*` on the knob (opaque-core
    recovery is gated on `render_bounds`, which is why the bounds must be
    hoisted too). The knob stays interactive.
  - **more than one** (body + highlight + logo + …): DEMOTE to a plain
    container (`audio_widget = none`) — the single-frame sprite skin can hold
    only one layer and the leaf knob codegen would silently drop the rest, so
    every layer renders as an image instead (faithful but not turnable; a
    composited rotational strip is the Approach A follow-up). This preserves
    the pre-interactive-sprite behavior and avoids silent layer loss.
  - **zero**: leave the knob recognized; it falls through to the default knob.
  Knobs whose art lives on the node itself (the kitchen-sink "knob" image-node
  shape) already carried `asset_ref` and were never demoted — they just gained
  the overlay below.
- **Codegen** (`design_codegen.cpp` knob branch): emits
  `setKnobSpriteStrip(id, body, 1, 'vertical')` and, when the core was
  recovered, `setKnobSpriteCore(id, x, y, w, h)` (core rect in the frame's own
  pixel space). Silver mode is unchanged and still wins (`@silver`/global).
- **Engine** (`Knob::paint`, `widgets.cpp`): a single-frame strip is a static
  disc, so the engine overlays the native rotating indicator notch (factored
  into `draw_knob_indicator_notch`, shared with the silver path) and, when a
  sprite-core is set, CORE-FITS the frame so the disc fills the knob box (the
  soft shadow bleed extends beyond) instead of drawing at the PNG's natural
  2× size (which oversized it and overlapped neighbours). Multi-frame strips
  encode rotation in the frames themselves and get NO overlay.

Gotchas:
- The **REST export lane** (`figma_rest_export.py`) emits recognized knobs as
  leaf `audio_widget` nodes WITHOUT capturing their internal vectors as a PNG
  sprite (by design — see the REST-port capture rules). So a REST-exported
  knob has `render_bounds` but no `asset_ref`: in sprite mode there is nothing
  to skin, and it falls through to the default/standard knob. To get a
  captured-art sprite knob you need the **figma-plugin "Export to Pulp"**
  envelope (e.g. the ELYSIUM `.pulp.zip`), which captures the disc PNG.
- The knob's `asset_path` is a **RUNTIME** path, not build scratch. When the
  disc carries a baked-in indicator (`knob_ind_r_out`), `enrich_imported_image_
  asset_metadata` re-encodes a *cleaned* disc and repoints `asset_path` at it —
  and that path is serialized into DesignIR JSON and baked verbatim into
  codegen (`setKnobSpriteStrip('<abs path>')`), then loaded when the SHIPPED
  plugin's editor opens. So it must outlive the import process: write derived
  assets to `default_asset_cache_directory()` (`PULP_IMPORT_ASSET_CACHE`, then
  a per-user cache dir), NEVER `fs::temp_directory_path()`. A temp-hosted
  cleaned disc survives local testing and then silently unskins the knob after
  a temp sweep or reboot, with zero diagnostics — the failure surfaces on a
  customer's machine, far from the import. Key derived filenames by **sha256**
  of the content (`pulp::runtime::sha256_hex`), like `asset_id_for()` and
  `IRAssetRef::content_hash`; `std::hash` is implementation-defined and
  unstable across runs and compilers. Note `--asset-cache` currently steers
  only the manifest lane; the cleaned-disc write follows the env var / default.
- Validate turning headlessly: render the imported knob at value 0.0 / 0.5 /
  1.0 with `pulp-screenshot` and confirm the white notch sweeps
  lower-left → up → lower-right. The engine unit tests
  (`pulp-test-widgets [sprite]`) pin the notch presence + sweep + core-fit;
  the CLI tests (`pulp-test-cli-import-design [sprite]`) pin the hoist
  end-to-end with a synthetic envelope + synthetic PNG (no proprietary
  export).

#### The LIBRARY/materializer path has its own hoist (`hoist_captured_art_knobs`)

The sprite hoist above lives in the **CLI** (`resolve_sprite_skins` in
`sprite_skins.cpp`, gated on `--knob-style sprite`). The **library/runtime path** — `build_native_view_tree`,
used by the GPU harness, standalone/plugin editors, and any embedder — does NOT
go through the CLI, so without its own pass it synthesized a default `Knob` and
discarded the captured disc (a generic blue value-arc instead of the design's
skeuomorphic disc — this was the ELYSIUM `grains_knob_cap` residual).

`hoist_captured_art_knobs(DesignIR&)` (declared in `design_import.hpp`, defined
in `design_import.cpp` beside the sibling importer passes `enrich_*` /
`synthesize_primitive_paths`) is the library-side promotion. Contract:
- **Run it BEFORE `enrich_imported_image_asset_metadata`** so the hoisted
  `asset_ref` receives its absolute `asset_path` + opaque-core metadata. Order is
  `parse → absolutize → hoist_captured_art_knobs → enrich → build_native_view_tree`.
- **Layer disposition by captured-image area** (not just count, unlike the CLU's
  count-only rule): the largest asset-image child is the body disc; a secondary
  layer counts as SUBSTANTIAL only if its area ≥ 40% of the body (the CLI rule
  is count-only). ELYSIUM's `Vector 7` pointer is a 0-width stroke hairline
  (area 0) → not substantial →
  the knob HOISTS the disc and stays interactive. Two comparable layers (body +
  highlight) → demote to a static container.
- **Capture the design's OWN pointer, don't synthesize one.** The disc body PNG
  (ELYSIUM `Group 130`) is a CLEAN face with the min/center/max REFERENCE ticks
  baked in; the moving pointer is a SEPARATE node (`Vector 7`, a ~4×16 hairline).
  Before erasing the hairline, the hoist stamps its geometry onto the knob as
  fractions of the disc half-extent: `knob_ind_r_in` / `knob_ind_r_out` (the
  radii the line runs between, derived from the hairline's endpoints vs the disc
  center), `knob_ind_w` (stroke width frac, from `border_width`), `knob_ind_color`.
  The materializer forwards these via
  `Knob::set_captured_indicator`, and `Knob::paint` draws THAT pointer — pivoted
  at the disc CORE center on the same `[-135°,+135°]` arc — instead of the generic
  `draw_knob_indicator_notch`. Two bugs this fixes: (1) double line (the synthetic
  notch + the disc's baked center tick); (2) misalignment (the synthetic notch
  pivoted at the layout-BOX center with a guessed radius drifted off the baked
  ticks). Pivot at the disc center + the design's own line ⇒ it rides the ticks by
  construction. **The synthetic notch is still the fallback** for knobs with no
  captured indicator metadata.
- **The pointer scan must handle the DEMOTED hairline, not just asset images**
  (root-cause fix, 2026-06). `Vector 7` is a thin stroke vector, so the
  stroke→fill demotion pass (see "Hairline strokes") rewrites it from
  `type:image` into a **1px-wide frame** whose stroke color sits on
  `background_color`, with NO `asset_ref`. The original hoist scanned only
  asset-backed image children for the pointer, so it silently missed the demoted
  `Vector 7` entirely: `knob_ind_*` was never stamped (knob fell back to the
  synthetic notch) AND the stray 1px frame rendered as a stuck second line.
  Captured-pointer was effectively dead on ELYSIUM despite the metadata plumbing
  existing. The fix: the pointer scan walks **every** non-body, non-text child and
  picks the thinnest one (`min(w,h) ≤ 2.5px`), so it catches both the raw 0-width
  hairline AND the 1px demoted frame. It tags that node `__knob_pointer`, reads
  color from `border_color` → else `background_color` (demoted) → else `color`,
  and the erase predicate removes `__knob_pointer` nodes too. Test:
  `[knob][sprite]` "recognizes a stroke-demoted pointer frame".
- **Import-time disc clean** (`clean_baked_knob_indicator` →
  `clear_baked_knob_antenna`, `design_import_png.cpp`), NOT a render-time cover. Many
  captured discs (ELYSIUM's included) BAKE an indicator into the disc PNG — here
  it's a thin vertical ANTENNA standing straight up ABOVE the disc at 12 o'clock.
  Since we draw our own rotating pointer, the baked one is a stuck second line. So
  when a knob carries `knob_ind_*`, `enrich_imported_image_asset_metadata` decodes
  the disc PNG and removes the antenna, re-encodes via a minimal PNG
  encoder (`encode_rgba_png_for_import`, filter-0 scanlines + runtime
  `zlib_compress` + IHDR/IDAT/IEND with hand-rolled CRC32), writes
  `$TMPDIR/pulp-import-assets/knobclean_<hash>.png`, and repoints `asset_path`.
  **The removal MUST be non-destructive to the disc** — two earlier attempts cut a
  visible notch/gap into the ring's top:
    - *Copy-from-beside + alpha-punch* (v1): copying a face strip horizontally
      across the CURVED ring mismatched the rim, and `p[3]=0` punched holes in the
      ring. Wrong: never alpha-punch the disc, never copy across curvature.
    - *Clear a fixed column at the bbox center* (v2): the antenna is NOT at the
      opaque-bbox center — the bottom min/max ticks skew the bbox, so on the big
      knob the center column had no antenna and nothing was removed.
  The correct algorithm (`clear_baked_knob_antenna`, a pure RGBA8 function so it's
  unit-testable): scan the disc bbox from the TOP down; each row, measure the
  ACTUAL opaque span. A NARROW span (≤ ~18% of the disc width) is the antenna →
  clear exactly that span (wherever it sits). The FIRST WIDE span is the disc body
  → STOP. So the ring, face, and bottom ticks are never touched. Result: a single
  moving pointer at every value, antenna gone, no notch, min/max ticks intact.
  Test: `[knob][antenna]` in `pulp-test-design-import-native-common` (antenna
  cleared, disc body byte-for-byte intact; no-op when there's no antenna).
  **Requires no edit to the Figma source.**
- **Materializer skin** (`make_widget` knob branch): when the knob node carries
  an enrich-stamped `asset_path` (+ `png_natural_*`), it builds a single-frame
  `SpriteStrip` + `set_sprite_core` from `art_core_*`, then applies the captured
  indicator above. The knob is design-faithful AND turnable — Phase-D drag still
  passes (`pulp-test-mac-platform-harness` knob_drag_probe).
- **Gotcha:** the harness pins `count_ir_nodes(ir.root)` on the *parsed* scene —
  assert structural counts BEFORE calling the hoist, since it removes the
  captured layers.
- Tests: `[knob][sprite]` in `pulp-test-design-import-native-materializer`
  (hoist promote + demote + indicator-geometry capture + materializer forwarding,
  pure, no I/O) + the GPU harness (end-to-end skin + interactivity).

### Rasterized-vector image: suppress its baked stroke as a CSS border

A Figma vector exported as a PNG (the FILTER & EQ curve `Vector 3`, every grid
`Line`, dividers) carries its stroke as `border_color` + `border_width` in the
IR — but **the stroke is already in the raster**. `apply_visual_style` draws a
CSS border from those fields, which paints a spurious box outline around the
image (the visible bug was a bright purple rectangle around the EQ curve). The
materializer passes `skip_border=true` for `image_view` nodes so the border is
not redrawn. If you add a code path that materializes images, keep that guard.
Test: `[image][fidelity]` in `pulp-test-design-import-native-materializer`.

### Widget recognition by Figma layer NAME (dropdowns, search field)

Some interactive widgets are recognized by the designer's layer **name**, not the
node type — designers label these containers explicitly, so the name is a
source-honest signal (no content guessing). `kind_from_name` in
`design_import_native_common.cpp` runs in `resolve_node` AFTER audio-widget
detection and BEFORE `kind_from_type`:
- A `frame` named `Dropdown` → `combo_box` (a `ComboBox`) ONLY when
  `looks_like_real_dropdown`: it carries a real selected value AND a SINGLE
  square-ish down-chevron (aspect ≤ 1.8). The name alone over-matches — ELYSIUM
  reuses "Dropdown" for two non-dropdowns that must stay plain frames:
  - a prev/next preset **cycler** whose icon is a WIDE `< >` pair (e.g. the 42×16
    "Frame 41" on the ENVELOPE/FILTER/FX-RACK headers) — leave static (faithful)
    until a real cycler interaction exists;
  - an unconfigured design-system **template** whose value is the literal word
    "Dropdown" (the stray "VST Style" placeholder). `materialize_node` renders it
    as a hidden, zero-size, inert view (`is_unconfigured_dropdown_template`) so it
    can't surface between panels — excluding it from `combo_box` alone left it
    rendering as a static "Dropdown" frame.
  `combo_box` is a **leaf** in `materialize_node` (text + chevron children are NOT
  re-materialized — the ComboBox paints its own display) and owns its hits.
- A search field is a `frame` CONTAINER (`is_search_container`: it wraps a text
  child named `Search`/`SearchBox`) → `text_editor` sized to the WHOLE box (not
  the inner text cell, so the field spans the box and the placeholder isn't
  truncated). The inner text becomes the placeholder; the editor inherits its
  font size and a `content_inset_left` so the caret clears the leading magnifier.
  In `materialize_node` a promoted text_editor keeps only IMAGE children (the
  icon) and drops the placeholder text + bg-pill chrome.

**Web-compat parity caveat:** this recognition is NATIVE-only — the web-compat
codegen does not detect these names. The screenshot-parity fixtures contain no
`Dropdown`/`Search` nodes, so the invariant holds today; if you add one to a
parity fixture, mirror the detection in the codegen (same lesson as the text
vertical-centering split). Tests: `[combo-box]`, `[text-editor]` in
`pulp-test-design-import-native-materializer`.

#### `kind_from_type` Ink & Signal vocabulary (type-string aliases)

`kind_from_type` (the `type`-string fallback, after `kind_from_name`) also
recognizes the design-system / common-web component names so imported designs
map onto native widgets instead of falling through to `native-unsupported-node`:
`toggle`/`switch`→`toggle_button`; `combobox`/`combo_box`/`dropdown`/`select`→
`combo_box` (note: before this, `combo_box` had a `kind` but **no** type string
resolved to it — only the layer-NAME path above could reach it); `pan`/`panner`→
`fader` (1-D linear control); `badge`/`chip`/`tag`/`pill`→`label`;
`panel`/`sidebar`/`side_panel`/`toolbar`/`channel_strip`/`card`→`view`. Faithful
dedicated `NativeWidgetKind`s for the remaining gap widgets (Stepper, Toast,
InlineBanner, Dialog, Popover, EmptyState, Tab, ProgressBar) need new codegen
emitters across the C++/Swift backends — a follow-up, not these aliases. Test:
`[design-system]` in `pulp-test-design-import-native-common`. The `pulp::design`
catalog (`pulp/design/design_system.hpp`) is the authoritative component→native
mapping these aliases mirror.

#### Native widget fidelity is inherited — keep token keys correct

An imported design materializes these native widgets, so it inherits whatever
the **native defaults** look like. The native widgets were converged to the
Ink & Signal Figma source (Knob body+arc+dot, square Checkbox, teal Toggle,
filled `TextButton::Style::primary`, slab Fader thumb, segmented Stepper, area
Spectrum, bar Waveform, etc.), so a faithful import needs no per-instance skin
for the common case — getting the native default right is what makes the import
look right.

The recurring failure mode here is a **wrong token key**: a widget that calls
`resolve_color("typo_or_old.key", <hardcoded fallback>)` where the key isn't a
real theme token compiles, paints the hardcoded fallback, and silently ignores
the imported token set (the reskin never reaches it). This shipped the coral
`ProgressBar`/`Tab` and several grey `CallOutBox`/`ListBox`/key-mapping bugs.
Canonical keys are the `t.colors["…"]` names in `theme_presets.cpp` (dotted:
`progress.fill`, `tab.active`, `text.primary`, `control.border`, `meter.green`,
…) — never underscore/bare forms. Enforced by `tools/scripts/token_key_check.py`
(`token-key-correctness` ctest) and, on Pillow lanes, the
`component-visual-regression` per-primitive gate. See
[docs/guides/design-tokens.md](../../../docs/guides/design-tokens.md) →
"Use the *real* token key". GPU vs raster fill caveat: an area/shader fill
(`Canvas::draw_waveform`) shows nothing on the CPU raster path — draw fills with
raster primitives if they must render off-GPU (see the `skia-gpu-build` skill).

### Value-driven silhouette fill (illustration shapes — item 3)

A captured illustration PNG (ELYSIUM's prism / cylinder / pentagon / cube) can
be "filled" to a bound value via `ImageView::set_fill_value(0..1)` +
`set_fill_color`. `ImageView::paint` overlays the color from the bottom up to
`value` of the height, masked to the image's OWN alpha through the canvas
`save_layer_with_mask` url() path — so the fill clips to the shape silhouette.

**Per-shape gradient — each shape fills with ITS OWN colors, not one generic
color.** ELYSIUM's shapes are uniquely colored (DEPTH purple, POSITION magenta,
OFFSET green, SHIMMER amber). A single `set_fill_color` made all of them fill the
same purple — visually wrong. So the importer SAMPLES each shape's own vertical
gradient from its art and stamps `shape_fill_gradient` (`sample_shape_fill_gradient`
in `design_import_png.cpp`: average the opaque pixels in N bands bottom→top, emit
`#rrggbb` stops). `ImageView::set_fill_gradient(stops)` then paints that gradient
revealed to `fill_value` instead of the flat color, so the shape fills with its
real colors — "mapped to the original", only adjustable. **This is independent of
`render_bounds`** (the shapes carry none; only knobs/EQ-curve do), so sampling runs
for any non-knob colorful image. A near-grey image (logo/icon) is BELOW the
saturation gate (`max_sat < 0.18`) and gets no gradient — the capability won't
latch onto things that shouldn't fill.

**Opt-in is preserved.** The importer stamping `shape_fill_gradient`, and the
materializer forwarding it to `set_fill_gradient`, are BOTH inert until a fill
value is driven (`fill_value` stays −1 ⇒ `emit_silhouette_fill` early-returns).
So a plainly-imported design renders unchanged; only a deliberate post-import
step that drives `set_fill_value` turns a shape into a fillable control. That
post-import wiring is the opt-in. Example phrasings a user can ask for after an
import: *"for the shapes, use their own gradients as the fill color"*, *"wire the
DEPTH knob to fill the cylinder"*, *"make the prism fill adjustable with its own
gradient"*. What you should NOT do: auto-drive fills on every image (a logo with a
gradient would start filling) or hardcode per-shape colors in the importer — the
gradient must be SAMPLED from each shape so it stays faithful to any design.

The alpha-mask primitive: `SkiaCanvas::save_layer_with_mask` Phase 1 only
shipped gradient masks; `url(<file>)` image masks were future work. They are now
implemented (`skia_canvas_mask.cpp` `parse_url_image_mask` — decode the file to
an `SkImage`, build a kDecal shader scaled to the mask box; kDstIn keeps painted
content only where the image alpha is non-zero). Works on Skia raster AND
Graphite/GPU.

Binding which knob drives which shape is NOT in the figma source (no Figma
binding), so the import does not auto-wire it. The `design-import-standalone` example
demonstrates the opt-in by pairing each upper illustration with its column knob
(`apply_shape_knob_fills`, by laid-out x) and driving `set_fill_value` from the
knob each frame; the per-shape gradient flows through automatically because the
materializer already set it. Verify headlessly on the **Skia raster** backend,
not a GPU window: `render_to_png(view, ..., ScreenshotBackend::skia)` exercises
the url() mask without Dawn/Metal. `PULP_KNOB_VALUE=<0..1>` on the example sets
every knob, so a raster at 0.25 vs 0.9 shows the fill level rise with each shape's
own color (a quick visual check that the gradient is value-driven, not base art).

Tests: `[view][image][fill]` in `pulp-test-image-view-fill` (flat fill scales with
value; a 2-stop gradient fill differs from the flat fill; <2 stops clears it).
`[native-materializer][fill]` asserts the materializer forwards `shape_fill_gradient`
to `set_fill_gradient` WITHOUT driving the fill (opt-in intact). The mac harness
(`pulp-test-mac-platform-harness`) pins end-to-end sampling: ≥4 ELYSIUM shapes get
a sampled gradient while the grey chrome does not.

### Imported text vertical centering — fix BOTH render paths together

The figma-plugin IR drops `textAlignVertical`, so a single-line label in a slot
taller than its text rides the TOP of its box (visible bug: ELYSIUM "SEARCH" sits
high; the `1·2·3·4` tab digits look off). The rule: center when
`height > font_size * 1.15`. **It must be applied to BOTH render paths or the
parity gate fails** — `pulp-test-design-import-screenshot-parity` pins
`build_native_view_tree` (baked/native) == web-compat `generate_pulp_js` (live).

The two paths are SEPARATE codegens:
- **Native** (`apply_label_style`, `design_import_native_common.cpp`):
  `label.set_vertical_align(center)`.
- **Web-compat** is the **HTML/DOM** emitter (the `.style.*` block in
  `design_codegen.cpp` — `document.createElement('span')` + `.style.verticalAlign`),
  NOT the `createLabel`/`setVerticalAlign` branch (that serves bridge-native-js).
  It emits `span.style.verticalAlign = 'middle'`; the web-compat shim
  (`web-compat-style-decl-typography.js`) maps `verticalAlign` →
  `setVerticalAlign` → `Label::set_vertical_align`.

Both converge on one `Label` mechanism ⇒ identical pixels ⇒ parity holds. A
NATIVE-ONLY change diverged the `control-strip` fixture to 0.95 (< 0.97) because
the web-compat span otherwise top-aligns. Tests: `[text]` in the native
materializer suite + the parity suite (both fixtures green).

### Hairline strokes (EQ grid) → demoted to 1px frames; parse the rgba() fill

The FILTER & EQ background grid is 8 hairline `Line` nodes. By the time they
reach `materialize_node` an upstream pass has already demoted them from
stroke-only images to **1px frames** (one axis floored to 1, marked
`__stroke_demoted=1`) whose `background_color` is the stroke color — typically
`rgba(171, 171, 171, 0.1)`. They still painted nothing because
`apply_visual_style` resolved `background_color` with `parse_hex_color`, which
does NOT handle `rgb()/rgba()`, so the fill was dropped and the grid vanished.
Fix: `apply_visual_style` now falls back to the shared `parse_css_color`
(exported from `css_gradient.hpp`) for `rgb()/rgba()/transparent`. So the bug was
NOT a 0-area-paint drop (that red herring cost time — flooring the flex dim did
nothing because the node is already a 1px frame, not an image); it was an
unparsed rgba fill. Test: `[color]` in the native materializer suite.

### `IRStyle::box_shadow` is parsed layers, not a string

`IRStyle::box_shadow` is a `std::vector<IRBoxShadow>`, **not** an
`optional<string>`. CSS `box-shadow` is a comma-separated layer list; the old
single-string field silently dropped every layer past the first. Don't assign a
raw CSS string to it or compare it to one — use the helpers in `design_ir.hpp`:

- `parse_css_box_shadow(css)` → ordered `IRBoxShadow{offset_x,offset_y,blur,spread,color,inset,raw}` layers. Splits on **top-level commas only** (commas inside `rgba()`/`hsl()` stay intact); each layer keeps its trimmed `raw` text for lossless round-trip.
- `box_shadow_to_css(layers)` → CSS string (prefers each layer's `raw`).

Every IR ingest site (`design_ir_json` parse, `claude_bundle`, `v0_tsx`) parses
into the vector; every emit site (`design_ir_json` write, `design_codegen` web +
native, `native_common`) serializes back via `box_shadow_to_css`. Native
`setBoxShadow` reads `box_shadow.front()`'s parsed fields directly — no
re-tokenizing the raw string. **Gotcha:** the bridge takes one drop shadow, so
multi-layer stacks render only their first layer natively even though the IR now
preserves them all.

### Figma effects: blurs lower for real; new families are diagnosed

All three Figma producers lower the **ordered effect stack**, honoring
per-effect `visible: false` (skipped, no diagnostic — it's the designer's own
off switch):

- `DROP_SHADOW` / `INNER_SHADOW` → ordered `box_shadow` layers (above).
- `LAYER_BLUR` → `style.filter = "blur(Npx)"`. **The `.fig` kiwi schema spells
  it `FOREGROUND_BLUR`** — `scene.mjs` accepts both spellings; don't add a
  `LAYER_BLUR`-only dispatch to the offline lane. Consumed end to end:
  web-compat `.style.filter` → style-decl bridge `setFilter`; bridge-native
  `setFilter` → `View::FilterOp` chain → `save_layer_with_filters`.
- `BACKGROUND_BLUR` → `style.backdrop_filter = "blur(Npx)"` → codegen web
  `.style.backdropFilter` / native `setBackdropFilter(id, blur_px)` →
  `View::set_backdrop_blur`. **The native bridge setter is numeric**, so
  bridge-native codegen parses the radius out of the CSS value (mirrors the
  web-compat style bridge).
- Multiple blurs of one kind keep array order as a space-joined function
  sequence (`blur(2px) blur(6px)`) — `setFilter` sums the amounts.
- A `PROGRESSIVE` blur keeps its end radius as a uniform blur plus a
  `progressive-blur-approximated` (capture_partial) diagnostic (plugin/REST).
- Everything else (`NOISE`, `TEXTURE`/`GRAIN`, `GLASS`, anything newer) raises
  `effect-unsupported` in every lane — never a silent drop. The `.fig` lane's
  code registry (`DIAGNOSTIC_SEVERITY` in `scene.mjs`) enforces
  registered ⇔ emitted in both directions; register any new code there.

Shared lowering lives in `extract-pure.ts::lowerEffects` (plugin),
`figma_rest_export.py::extract_style` (REST), and
`scene.mjs::effectsToBoxShadow`/`effectsToFilters` (.fig);
`material_audit.mjs::effectSurvived` reads blurs through the survived arm
(`style.filter` / `style.backdrop_filter`), so regressing the lowering trips
the audit unless the diagnostic comes back. **Still-partial:** the C++ native
resolver lane (`design_import_native_common.cpp`) diagnoses `filter` /
`backdropFilter` as unsupported (it doesn't call the View setters), and
cpp/swiftui codegen don't emit them.

### Step 1: Identify source and input

Ask the user or detect from context:
- **CLI source**: figma, figma-plugin, stitch, v0, pencil, claude, designmd, or jsx.
  The runtime/source-contract lane also covers rn.
- **Input**: file path, URL, or an exported/generated artifact. MCP tools are
  acquisition helpers; do not pass raw provider MCP JSON to the CLI unless that
  source contract explicitly documents the shape. Claude is manual-file only;
  designmd is static-file only.

### Step 2: Read the design data

**`--from figma-plugin`, NOT `--from figma`, for any `.pulp.json`/`.pulp.zip` envelope.**
There are two distinct Figma sources: `--from figma` → `parse_figma_json` (the old Figma REST/file format) and `--from figma-plugin` → `parse_figma_plugin_json` (the plugin/headless **export envelope**, `format_version 2026.05-figma-plugin-v1`). Feeding a plugin envelope to `--from figma` historically produced a **silent empty import** — `parse_figma_json` found none of its structure and emitted only `createCol('root')` (`1 elements: 1 containers, 0 widgets, 0 labels`). The CLI now **auto-detects** the envelope and routes to the plugin parser with a `note:` on stderr (see `looks_like_figma_plugin_export` + `test_import_source_routing.cpp`), but always pass `--from figma-plugin` explicitly. Tell-tale of the old mistake: a ~13-line `ui.js` with only the root, or `0 widgets, 0 labels` on a design that clearly has widgets.

**Figma plugin export — `.pulp.zip` is the default ship shape**:
The "Export to Pulp" button in `tools/figma-plugin` emits a `.pulp.zip` containing `scene.pulp.json` + `assets/*.png` whenever the design has images. The CLI auto-unpacks ZIPs transparently (look for `Unpacked …` on stdout); point `--file` directly at either form:
```bash
pulp import-design --from figma-plugin --file design.pulp.zip --output ui.js  # canonical
pulp import-design --from figma-plugin --file scene.pulp.json --output ui.js  # also fine
```

**Headless alternative — `tools/import-design/figma_rest_export.py` (no plugin click).**
For iterative dev you don't have to open Figma desktop and click Export every time. This script pulls a frame via the Figma **REST API** and emits the same `figma-plugin-export-v1` envelope (it's a faithful PORT of `pulp-figma-plugin/src/extract.ts` — keep the two in sync). It captures vector/illustration nodes as PNG `asset_ref`s via the REST `/images` endpoint, exactly like the plugin's `exportAsync`.
```bash
# one-time: figma.com -> Settings -> Security -> Personal access tokens ->
# Generate, check ONLY file_content:read, save to ~/.config/pulp/figma-token (chmod 600)
python3 tools/import-design/figma_rest_export.py \
  --url 'https://figma.com/design/<KEY>/...?node-id=3-42' --out scene.pulp.json
pulp import-design --from figma-plugin --file scene.pulp.json --output ui.js
```
Token resolves from `--token`, then `$FIGMA_TOKEN`, then `~/.config/pulp/figma-token`; lifecycle (added/expiry) tracked in `~/.config/pulp/figma.json`. PATs expire (≤90 days) — regenerate same-scope; Figma OAuth2 is the permanent path (future). The plugin lane remains source-of-truth (audio-widget recognition, Pulp Library matching); the REST port covers generic frames/text/vectors/assets, which is what non-library designs (e.g. ELYSIUM) use. **Asset PNGs only composite when the render uses the SKIA backend.** Two independent gates: (1) a GPU/Skia-enabled build (`PULP_HAS_SKIA`), and (2) the **Skia screenshot backend** — `render_to_png`'s macOS default is CoreGraphics, whose canvas lacks `draw_image_from_file`, so `ImageView` draws each image's *filename as placeholder text* (empty boxes + scattered `*.png`) even in a Skia build. `pulp import-design --validate` now defaults to `--screenshot-backend skia` so a fresh import render is faithful; only pass `coregraphics` deliberately. If a validate render shows missing images + filename text, it's the backend, not the import — re-render with Skia. See the `screenshot` skill.

**Bundled (non-system) fonts — `font_family_assets` → `registerFont` (#43b).** When the envelope carries `font_family_assets` (#43a: `[{family, style, weight, asset_id}]`) with the `.ttf`/`.otf` shipped in `assets/`, the importer registers each so a non-system family actually loads instead of `setFontFamily` silently falling back to a same-named system font. Pipeline: `parse_figma_plugin_json` → `DesignIR.font_family_assets`; the CLI resolves each `asset_id` → absolute path (same pass as image `asset_path`); `generate_pulp_js` emits `registerFont('<family>','<path>')` in the JS header **before any `setFontFamily`**; the `registerFont` bridge calls `AssetManager::register_font_family` → `canvas::register_font_file` (Skia typeface; no-op stub off-GPU). **Gotcha:** if the design's font is *also* system-installed (e.g. Inter on macOS), a render can't distinguish bundled-load from system-fallback — verify with a non-system family. Covered by `[issue-43b]` in `test_design_import.cpp`.

**Two render-path traps behind `registerFont` (found chasing the ELYSIUM font gap).** Emitting `registerFont` is necessary but historically NOT sufficient — two silent failures made registered fonts not render:
1. **SkParagraph never saw registered fonts.** `register_font` populated only the Canvas2D `fillText` / `FontResolver` path. But every `Label` rasterizes through **SkParagraph** (`make_paragraph` in `skia_canvas_text.cpp`), whose `FontCollection` (`TextFontContext::font_collection()`) only registered the *emoji* typeface — user fonts fell back to a system face for ALL label text. The fix bridges the registry into the paragraph collection via `registered_typefaces_snapshot()` (in `bundled_fonts.cpp`), iterated in `text_font_context.cpp::font_collection()`. **Diagnostic:** if a registered font renders in raw `ctx.fillText` canvas calls but NOT in Labels/section headers, the paragraph bridge is the suspect.
2. **Variable fonts ignored `font-weight`.** A variable `.ttf` (Funnel Display: `wght` 300–800 def 300; Clash Grotesk: 200–700 def 700) registers as ONE typeface at its default instance. SkParagraph and the static matcher pick the closest *static* weight, so e.g. a `font-weight:700` section header rendered at the 300 default for every weight. Fix: `face_wght_axis()` detects the `wght` axis; `match_registered_typeface` treats a same-slant variable face as eligible past the 200-unit static tolerance; the resolver derives a synthetic `wght` variation from the request (clamped); and `font_collection()` pre-bakes one `makeClone` per CSS weight (100–900) under the family alias so the provider's closest-match picks the right instance. **Diagnostic:** if regular-weight text picks up the font but bold/heavy text stays on the fallback, it's the variable-weight path. Covered by `[variable-weight]` in `test_canvas_fonts.cpp`.

**The fonts the design uses may not be in the envelope at all.** The Figma API does NOT expose font binaries (only family NAMES via `_record_font`). A REST/plugin export of a design using restricted foundry fonts (Clash Grotesk, Funnel Display) yields `font_family_assets` with NO `asset_id` → no `registerFont` → system fallback. Options: (a) the plugin's drag-drop font escape hatch (`user-fonts.ts`) lets the user supply the `.ttf`; (b) obtain the font under permissive terms and stamp it into the bundle (Funnel Display is OFL on Google Fonts; Clash Grotesk is free-for-commercial from Fontshare) — add the file to `assets/<sha256>.ttf`, a `font/ttf` manifest entry, and the matching `asset_id` on each `font_family_assets` entry. **Embed note:** the portable-bundle path-resolver preamble (pulp-view-embed) must wrap `registerFont` (alongside `setImageSource`/`setKnobSpriteStrip`) or relative font paths won't resolve against the bundle dir.

REST-port capture rules that mirror the plugin's `extract.ts` — keep them in sync when the P2/P3 shared extractor lands:
- **Audio-widget recognition by name** (`widget_kind_from_name`): knob/fader/meter/dial/slider/xy-pad/waveform/spectrum nodes are emitted as leaf `audio_widget` nodes so the importer renders them NATIVELY (silver knob etc., at the node's own size) — NOT captured as raw image sprites from their internal component-instance vectors (compound `I…;…` ids), which renders misplaced fragments. This is what makes non-library designs (ELYSIUM) get real knobs.
- **`REGULAR_POLYGON` is a vector leaf type.** Figma REST reports polygons as `REGULAR_POLYGON` (the plugin SceneNode API says `POLYGON`). Omitting it makes polygon-based illustrations (ELYSIUM's Pentagon/RANGE shape) fail the pure-vector-illustration test → recurse into partial captures instead of rasterizing whole. Include both spellings.
- **`font_family_assets` capture** (`_record_font`): walk TEXT nodes, collect `{family, style, weight, italic?}` deduped, emit at the envelope root. REST can't fetch an uploaded font's `.ttf` binary, so `asset_id` is omitted and the consumer keeps the family NAME (system fallback) rather than registering a bundled file. Capturing the metadata keeps the REST envelope shape-conformant with the plugin.
- **sha256 `content_hash`** for captured PNGs: name + content-address each asset by `sha256(bytes)` (matches the plugin's `AssetCache`), not a node-id placeholder — dedupes identical captures and lets the importer verify bytes.
Detection uses ZIP magic (`PK\x03\x04`), not the file extension — `.zip` renames still get unpacked. The temp dir is auto-cleaned at process exit. Older CLI builds read input via `std::ifstream` text mode and silently truncated at the first NUL byte in the ZIP header; the symptom was `parser threw an unknown exception` on any `.pulp.zip`. If you see that error, the CLI predates the auto-unpack support — rebuild from current `main`.

The extractor refuses hostile archives at parse time: entries whose filename is so long it would truncate inside the stack buffer, entries containing `..` substrings, entries that resolve to an absolute path (`fs::path::is_absolute()`), entries with Windows drive-relative or UNC prefixes (`C:foo`, `C:\\foo`, `\\\\server\\share\\…`), entries beginning with `/` or `\\`, archives with more than 10000 entries, and archives whose total or per-file uncompressed size exceeds 256 MB / 64 MB respectively. Each rejection logs a labelled `Error: refusing unsafe zip entry (<reason>): <name>` (or `oversized filename`, `total uncompressed size > …`) on stderr and bails with a non-zero exit. If a legitimate plugin export ever trips one of these caps, the right move is to lift the cap deliberately in `extract_pulp_zip_if_present` rather than disable the guard.

**Measuring import fidelity — `tools/import-design/fidelity_diff.py`**:
Don't eyeball whether an imported+rendered design matches the Figma source — measure it. The fidelity diff harness (stdlib + PIL only) takes the rendered PNG, the `scene.pulp.json`, the captured `asset_ref` PNGs, and an optional whole-frame reference, and emits a per-widget pass/fail report against a configurable tolerance (default 15%).

```bash
python3 tools/import-design/fidelity_diff.py \
  --render <render.png> --scene scene.pulp.json --assets-dir assets/ \
  [--frame-reference frame.png] [--out-dir cmp/] [--json report.json] \
  [--tolerance 0.15]
```

Heuristics live in a registry (`HEURISTICS`) so new ones are cheap to add; each is a small, unit-tested function. Current set: `art_bounds` (scale-invariant signature-blob aspect + info `full_aspect` for the housing+fill+thumb extent), `declared_geometry` (render aspect vs scene `style` box, reference-normalized so a fader's thin track doesn't false-fail), `colors` (knob/fader palette nearest-match + meter green→red gradient stops, sampled over *matched* crops), `completeness` (every scene text + widget must render; flags MISSING nodes and text that overflows its declared width or is clipped at the panel edge — the no-wrap bug), `padding` (panel edge → first child gap vs `layout.padding`; flags content hugging the wall), `widget_detail` (fader track/housing stroke presence, knob indicator angle vs reference, meter housing + warm→cool gradient ramp), `text_style` (per-line glyph height = size proxy, stroke density = weight proxy, vs `font_size`/`font_weight`), `frame_overlay` (content-aware whole-frame alignment + similarity + side-by-side + diff heatmap), `side_by_side` (per-widget comparison PNGs). Exit 0 = within tolerance, 1 = regression.

**Trustworthiness gate**: feed the original `--frame-reference` back in as `--render` — it must score 0 fails. That ground-truth pass is what makes the fails on a real import trustworthy. A faithful importer should converge to that baseline.

Gotchas baked into the tool: (1) the render and the captured asset PNGs are at *different* canvas scales, so absolute pixels are info-only — pass/fail is on aspect ratio. (2) Widget detection has two layers: the per-kind *signature* mask (the colored blob — fill-only for fader/meter) for a stable aspect anchor, and `detect_full_widget` which flood-fills from that anchor through connected foreground (absorbing the dark housing slot + thumb) so the *compared crop* matches the full reference widget — never compare a fill-only render blob against a housing+fill reference. The full-widget flood-fill is clipped to the declared box so it can't bleed into a neighbour. (3) Whole-frame alignment needs the real panel, not the canvas: `detect_panel` finds the rounded panel as a dark blob on a light page margin OR (flush dark render) as the border-ring + content box; `interior_background` samples the modal interior (rounded corners leak the page color, so the corner sampler is wrong for a panel crop). (4) Text detection is bg-relative glyph brightness on the *panel crop* (not absolute luma over the whole image, which lights up a light page margin); the row-cluster snap takes a `prefer_y` so a tall search window locks onto the predicted line, not the neighbouring one. (5) `indicator_angle` is coarse (±~15°) and detects either a dark or light notch; `track_stroke` is presence-only, not thickness. (6) Large renders are down-scaled to `MAX_SCAN_DIM` before the pure-Python pixel scans — without this a 1520² render takes minutes. (7) The render PNG is written to the CLI's CWD as `<name>-figma plugin export-render.png`; `--render-size WxH` is honored even though the meta JSON keeps the declared canvas. (8) A run where NOTHING measured is `SKIPPED`, exit **3** — never "OK". Fig-lane scenes hit this shape today (every node is `audio_widget: "none"`, no `type == "text"` nodes, no root padding), so on that lane the tool has no verdict; do not list it as a passing lens there. `summary.ok` is false and `summary.measured` is 0 in the JSON. Regression coverage: `test/test_import_fidelity_diff.py` (CTest `import-fidelity-diff`, skips 77 without PIL) with tiny checked-in fixtures under `test/fixtures/import-fidelity/`, plus synthetic per-heuristic unit tests (panel detection, full-widget vs blob, text overflow/missing, padding hug, indicator angle).

**Figma (MCP available)**:
- Use `com.figma.mcp` to read the current file or selection
- Extract frames, auto-layout, fills, strokes, effects, text, components

**Figma Make runtime-import parser (Phase 6.6.3)**:
- The runtime-import lane accepts constrained, sanitized Figma Make React exports through `parse_figma_make_react()` and `source: 'figma'`. It normalizes the TSX file into the same bundle payload shape used by Claude and v0 runtime import.
- Accepted input is a single `.tsx`/`.jsx` React component with explicit Figma provenance, no `"use client"` directive, React hook imports from `react` only, and inline `style={{ ... }}` objects.
- Raw Figma Make defaults are intentionally rejected until a preprocessing step exists: unresolved `figma:asset/*` imports, versioned import paths like `<package>@<semver>`, Tailwind `className` utilities, Radix primitives, Code Connect glue files, Next.js wrappers, custom JSX components, non-range inputs, and network/storage/worker APIs.
- Representative fixtures live under `planning/fixtures/figma/`; the primary one is `level-meter-panel.tsx`. Run `tools/import-validation/figma-roundtrip.sh --parser-only` for the parser/dispatch gate, `tools/import-validation/figma-roundtrip.sh` for parser-emitted screenshot diff, and `tools/import-validation/figma-roundtrip.sh --coverage` before pushing parser PRs.

**Stitch (MCP available)**:
- Use `mcp__stitch__list_screens` to show available screens
- Use `mcp__stitch__get_screen` to read the selected screen
- Extract component tree, inline styles, design system tokens

**Pencil (MCP available)**:
- Use `mcp__pencil__get_editor_state` to check current file
- Use `mcp__pencil__batch_get` to read the node tree
- Use `mcp__pencil__get_variables` for design tokens
- Use `mcp__pencil__get_style_guide` for style references

**v0 (URL or TSX file)**:
- If URL provided, fetch the v0 generation
- If TSX file, read directly
- Extract JSX tree, Tailwind classes, shadcn/ui components

**DESIGN.md (Google design.md, Apache-2.0)**:
- DESIGN.md is a design *system* spec (YAML frontmatter + Markdown body), not a screen export. There is no screen tree to render; output is `tokens.json` only.
- Run `pulp import-design --from designmd --file path/to/DESIGN.md --tokens tokens.json`. **No `ui.js` is written** — the dispatch arm skips the codegen step. No bridge scaffold either.
- Detection is strict (all-of fingerprint, 95% min-confidence): filename `DESIGN.md` + `---` frontmatter fence + `name:` key + at least one of `colors`/`typography`/`rounded`/`spacing`/`components`. Generic Jekyll/Hugo blog posts will not match.
- Diagnostics: structured `[severity] code at path (line:col): message` on stderr. Exit codes — 0 OK, 1 usage/write, 2 detect-only no match, 3 parse error (malformed YAML, duplicate `##` section heading), 4 unsupported.
- **Format spec pin: tag `0.3.0`** (`paws-and-paths` fixture is byte-identical across 0.1.1→0.3.0, so only the pin strings move — provenance/NOTICE/licensing/compat.json). Frontmatter format coverage tracking 0.3.0: (1) `colors.*` values are **any valid CSS color**, not just hex — `looks_like_css_color()` accepts hex (3/4/6/8-digit), named keywords, and functional `rgb()/hsl()/hwb()/oklch()/oklab()/lch()/lab()/color-mix()`; a real non-color value still emits `color-shape`. (2) `colors`/`rounded`/`spacing` nest to **arbitrary depth** — `walk_color_node`/`walk_dimension_node` recurse and key on the **dot-joined** path (`background.light`, not dashed), matching the `{colors.background.light}` reference syntax that `lookup_color`/`lookup_dimension` already resolve. (3) `spacing` accepts a **bare number** (`base: 8` → 8px) because `parse_dimension`'s unit is optional. (4) **Unknown top-level keys warn** (`unknown-key`, warning-not-error) — catches typo'd keys like `color:`/`typgrphy:`. Numeric/boolean component scalars (`fontWeight: 600`, `enabled: true`) already flow through as strings via yaml-cpp coercion.
- **Markdown body sections are ALWAYS scanned** — with or without frontmatter. `parse_designmd` reads `name: value` list items and `| name | value |` table rows under `## Colors` (color tokens), `## Spacing` (`spacing-*` dims), `## Border Radius`/`## Rounded` (`rounded-*` dims), and `## Shadows`/`## Elevation` (`shadow-*` strings). **Frontmatter wins on conflict** (compared by normalized name, since frontmatter keys are dot-joined and case-preserving while body keys are lowercased and dash-joined); the body only fills gaps. This used to run **only** when a doc had no `---` frontmatter, which meant adding frontmatter to a prose-authored DESIGN.md silently dropped every body token — total and invisible for `## Shadows`, whose frontmatter key did not exist. `shadows:` is now a real frontmatter group. Table header/separator rows are skipped by requiring the value cell to be a real color/dimension/shadow. A `### Light Mode`/`### Dark Mode` subsection under `## Colors` routes to the bare name (light/default) or a `<name>.dark` suffix (dark) — the **same multi-mode convention the Figma plugin uses** (`tools/figma-plugin/src/tokens.ts`), so dark themes survive into the flat token maps. Emits a `body-tokens` info diagnostic when it recovers any.
- See `docs/reference/imports/designmd.md` for the full reference (supported subset, reference-resolution rules, attribution).

**v0.dev runtime-import parser (Phase 6.6.2)**:
- The runtime-import lane accepts constrained v0 React exports through `parse_v0_dev_react()` and `source: 'v0'`. It normalizes the artifact into the same bundle payload shape used by Claude runtime import.
- Accepted inputs: a bare single-file `.tsx`/`.jsx` React component, or a v0 code-project envelope containing `[V0_FILE]tsx:file="..."` blocks. Prefer the explicit `source: 'v0'` route; standalone TSX has no reliable v0-only marker.
- C-1 deliberately rejects default v0 surfaces that are outside the supported matrix: Tailwind `className` output, shadcn/Radix components, Next.js wrappers/imports, custom JSX components, non-range inputs, and network/storage/worker APIs. Use inline React `style` objects plus the supported DOM/CSS/API subset.
- Representative fixtures live under `planning/fixtures/v0-dev/`; the primary one is `audio-control-panel.tsx` (audio control panel + canvas meter). Run `tools/import-validation/v0-roundtrip.sh --parser-only` for the parser/dispatch gate.

**Google Stitch runtime-import parser (Phase 6.6.4)**:
- The runtime-import lane accepts constrained Stitch React exports through `parse_stitch_react()` and `source: 'stitch'`. Stitch has no reliable standalone file marker, so do not auto-detect arbitrary TSX as Stitch.
- Accepted input is a single-component React TSX module from Stitch's vanilla/inline-style export path. The default Tailwind export is out of scope until the shared Tailwind-to-inline-style preprocessor exists.
- C-3 deliberately rejects Tailwind `className`, external CSS imports, Next.js wrappers or `"use client"`, Radix/shadcn components, React Native imports, Stitch MCP JSON node trees, custom JSX components, non-range inputs, and network/storage/worker APIs.
- Representative fixtures live under `planning/fixtures/stitch/`; the primary one is `transport-bar.tsx` (transport controls + range sliders + canvas VU meter). Run `tools/import-validation/stitch-roundtrip.sh --parser-only` for the parser/dispatch gate.

**JSX-instrument runtime-import (experiment slice, 2026-05-17, planning/2026-05-17-jsx-instrument-import.md)**:
- Unlike v0/figma/stitch, the `jsx` source is NOT a synthetic shape-counter — it executes the user's real React tree. `parse_jsx_react(bundle_js, component_name)` wraps a pre-compiled IIFE bundle (esbuild output of React plus either ReactDOM or the @pulp/react native bridge + user JSX + nav/document sandbox shims) as a synthetic `ClaudeBundle`, then the existing `parse_claude_html_with_runtime` harness materializes it into a `DesignIR` via DOM walking or the native `WidgetBridge` snapshot fallback. **Per Codex/RepoPrompt review:** custom inline-defined components (knobs, faders, etc.) materialize as their underlying SVG primitives — DO NOT widget-promote them to native `<Knob>`/`<Fader>`, which would lose visual parity with the source JSX.
- The JSX→JS compile happens in Node, not in the C++ runtime. Run `tools/import-design/jsx-runtime/jsx-transform.mjs --in <file>.jsx --out <out>.js` to produce the IIFE bundle. The script ships its own `node_modules` (React 18.3.1 + ReactDOM 18.3.1 + react-reconciler 0.29.2 + scheduler 0.23.2 + esbuild 0.24.0 + `@babel/parser` 7.29.7 + `css-tree` 3.2.1) at `tools/import-design/jsx-runtime/node_modules/`. First-run `npm install` is required.
- For native-import validation, run `tools/import-design/jsx-runtime/jsx-contract-audit.mjs --in <file>.jsx --json <audit.json> --fail-on-weak-proof` before relying on visual screenshots. Shape should come from the source contract, not from visual inference: the audit extracts JSX structure, props, style semantics, SVG/vector geometry, `.map()` rows, and handler closures so the importer can normalize those into Pulp-native attributes. Keep the live runtime fallback whenever the source contract is too dynamic.
- Current Chainer/native bundles route `react-dom` through `pulp-react-dom-shim.mjs` and `@pulp/react`. The live lane writes that bundle verbatim. The baked lane first tries the DOM walker; when the bundle renders native views instead of DOM nodes, it freezes the `WidgetBridge` tree into DesignIR with `capture_method = runtime_native_snapshot` and `snapshotSource = native-view`, then can emit baked C++ from that IR.
- Supported input today: single-file `.jsx` or `.tsx`, default-exported React function component, hooks from `react` only, inline `style={{...}}` objects, SVG primitives (`svg/path/circle/line`), `<input>`/`<button>` form elements (text-input editing is degraded — plain text inputs fall back to a non-editable View per Codex review), `setInterval`/`requestAnimationFrame`/`getBoundingClientRect`. The TypeScript path strips TSX through the Node/esbuild transform before the C++ runtime parser sees the bundle.
- Out of scope until follow-up PRs: window-level `mousemove`/`mouseup` global fan-out (the canonical 2-week gotcha; static render works, interactive drag does not), viewport resize signaling (`window.innerWidth/innerHeight` hard-coded), screenshot-similarity acceptance gate (timers/random would need freezing for determinism), Babel-standalone embedding (replaces the Node shell-out).
- End-to-end harness: `tools/import-validation/jsx-roundtrip.sh` runs the transform + builds + runs the smoke test (`pulp-test-design-import-jsx-runtime` — asserts >9 IR nodes + Chainer-shaped text materializes) + optionally renders a `pulp-screenshot` PNG. Primary fixtures: `planning/fixtures/jsx/chainer-instrument.jsx` (762-line Chainer instrument with 9 inline custom components, SVG, drag, setInterval) and `planning/fixtures/jsx/typed-control.tsx` for TSX stripping.
- The bundle's banner-installed shim is critical: ES module imports get hoisted to the top of esbuild's IIFE body, so the navigator/document/HTML*Element ctor shims MUST be emitted as an esbuild `banner` (which is literally prepended outside the IIFE) — not inline in the entry source. Without that, React-DOM's DevTools UA sniff (`navigator.userAgent.indexOf("Chrome")`) crashes during module init. Mirrors the pre-payload shim block in `run_claude_bundle_payload_pipeline` (`design_import.cpp:1494`).

**React Native runtime-import parser (Phase 6.6.5)**:
- The runtime-import lane accepts constrained single-file RN component exports through `parse_react_native_export()` and `source: 'rn'`. The import `from 'react-native'` is unambiguous, so runtime dispatch may auto-detect RN when the source label is omitted.
- Accepted input is a single TSX component with React imports from `react`, RN imports from `react-native`, RN element vocabulary (`View`, `Text`, `Pressable`/`Touchable*`, `ScrollView`, `TextInput`), and `StyleSheet.create({...})` styles. Numeric RN style values are treated as CSS pixels.
- C-4 deliberately rejects native/device APIs and wrappers outside the matrix: `Animated`, Reanimated, Linking, Alert, AsyncStorage, Dimensions/Platform branching, Modal, virtualized lists, navigation, Expo modules, `NativeModules`, `requireNativeComponent`, image sources, DOM tags, and array-form styles.
- RN defaults `flexDirection` to column; the parser-emitted bundle preserves that by injecting column flex semantics into the normalized DOM surface. Representative fixtures live under `planning/fixtures/rn/`; the primary one is `gain-stage.tsx`. Run `tools/import-validation/rn-roundtrip.sh --parser-only` for the parser/dispatch gate, `tools/import-validation/rn-roundtrip.sh` for parser-emitted screenshot diff, and `tools/import-validation/rn-roundtrip.sh --coverage` before pushing parser PRs.

**Pencil runtime-import parser (Phase 6.6.6)**:
- The runtime-import lane accepts constrained Pencil/OpenPencil React exports through `parse_pencil_react()` and `source: 'pencil'`. Pencil has no reliable standalone file marker, so do not auto-detect arbitrary TSX as Pencil.
- Accepted input is the sanitized post-preprocessor form of Pencil's Tailwind JSX export: a single React component with Tailwind classes expanded to inline `style` objects and `--pencil-*` tokens resolved to literals. The MCP JSON node-tree path stays with the offline Pencil adapter and is not a runtime-import source parser.
- C-5 deliberately rejects Tailwind `className`, unresolved `--pencil-*` token references, MCP JSON envelopes, `.pen`/`.fig` binary references, external CSS imports, Next.js wrappers or `"use client"`, Radix/shadcn components, React Native imports, custom JSX components, non-range inputs, and network/storage/worker APIs.
- Representative fixtures live under `planning/fixtures/pencil/`; the primary one is `gain-stage-card.tsx` (gain slider + canvas level meter + bypass). Run `tools/import-validation/pencil-roundtrip.sh --parser-only` for the parser/dispatch gate, `tools/import-validation/pencil-roundtrip.sh` for parser-emitted screenshot diff, and `tools/import-validation/pencil-roundtrip.sh --coverage` before pushing parser PRs.

**Claude Design (manual HTML export)**:
- Anthropic Labs has no MCP / public API. The user runs Claude Design, exports the canvas as Standalone HTML (or "Send to Local Coding Agent"), and hands you the resulting file.
- Run `pulp import-design --from claude --file <path>` — the parser delegates to the Stitch HTML pipeline and tags the IR as Claude. **This is the static path** — it sees only the loader-shell HTML wrapping the bundled React app (~9 elements: title, bundler placeholders, inline styles, the `<script>` blob).
- Add `--execute-bundle` to invoke the **native-runtime path**: Pulp parses the JSON envelope, decodes the gzip+base64 asset map, evaluates the React + React-DOM + app payloads in a headless `ScriptEngine`, then walks the materialized DOM into the `DesignIR`. Falls back to the static path on any harness failure (engine error, walker output below the 9-node loader-shell floor, JS payload too large). Use this when the user's Claude export is a real bundled-React app and they need the actual editor tree, not just the shell.
- The CLI also writes a `bridge_handlers.cpp` scaffold next to the generated JS (override path with `--bridge-output`, skip with `--no-bridge-scaffold`). The scaffold demonstrates registering `pulp::view::EditorBridge` handlers and attaching to a `WebViewPanel` (or future `JsRuntime`).

**Inline `<script>` evaluation in `--execute-bundle`**: The harness evaluates inline `<script type="text/javascript">` (and untyped `<script>`) blocks AFTER the src-loaded payloads, then compiles + evaluates inline `<script type="text/babel">` (and `text/jsx`) blocks via the bundle's own Babel-standalone (looked up as `globalThis.Babel.transform`). After both, the harness dispatches a `readystatechange` → `DOMContentLoaded` → `readystatechange(complete)` → `window.load` sequence and pumps four message-loop / frame-callback cycles for async settling. This is what makes a real Spectr-style Claude bundle (where the actual React app lives in inline `text/babel` blocks, not src-loaded payloads) materialize beyond the 9-element shell. Per-script soft-fail matches the existing src-loaded payload pattern. Inline `application/json` (and other `*/json`) blocks are intentionally skipped — they're config blobs, not executable code.

**Inline-bundle implementation gotchas:**
- `core/view/js/web-compat.js`'s `document` is a plain object literal (not an `Element`), so it ships **without** `addEventListener` / `dispatchEvent`. The Step 3 dispatcher constructs events defensively — uses `new Event(t)` when available, falls back to `{type, target, bubbles:false, preventDefault, stop*}` literal otherwise — but bundles that call `document.addEventListener('DOMContentLoaded', ...)` only fire if the bundle (or some library it loads) installs `addEventListener`/`dispatchEvent` on `document`/`window` first. Real React-DOM does not do this — it attaches to the `root` element it controls — so the DCL dispatch is a best-effort safety net, not a guarantee. See test `DOMContentLoaded dispatch runs the queued handler when document supports it` in `test/test_design_import_inline_babel.cpp` for the exact shim shape that satisfies the contract.
- The harness's `error_out` is the **fallback reason** (or empty on success). Don't piggyback diagnostic warnings on it from inside the harness; on success the harness clears the slot. If you need to surface a warning that survives a successful run, push it through a different channel (e.g. add a `warnings` vector to `ClaudeRuntimeOptions`).
- Empty `<span>` (and other text-mapped tags: `p`, `label`, `h1`-`h6`, `a`, `strong`, `em`, `small`, `code`) get filtered out by the text-empty pruning in `json_to_ir_node`. If a fixture or test relies on observing an empty `<span>` with attributes round-tripping through the IR, use a `<div>` instead — divs map to `frame` and survive the prune.
- Babel-standalone's loaded test: probe `typeof globalThis.Babel.transform === 'function'` rather than `typeof Babel`. Some bundles install `Babel` as a sentinel object before the real implementation arrives, which would false-positive on the looser check.

**@pulp/react bundle dedup:** when emitting React+@pulp/react consumer bundles (Spectr et al.), the consumer's bundler MUST be able to dedup React across the @pulp/react boundary. This means @pulp/react's published `dist/index.mjs` must externalize `react`, `react-reconciler`, `react-reconciler/constants.js`, and `scheduler` — otherwise esbuild emits TWO independent React module instances (one for user code, one for the reconciler) and `ReactCurrentDispatcher.current` desyncs at first commit, manifesting as "cannot read property 'useState' of null" inside the user's `App()`. The fix is a 1-line addition to `packages/pulp-react/package.json`'s `build` script: `--external:react --external:react-reconciler --external:react-reconciler/constants.js --external:scheduler`. This must hold for any future package emitted from `pulp import-design` that pulls in @pulp/react.

**Spectr's `<svg><path>` doesn't auto-route to `<SvgPath>`:** Pulp v0.69.2+ ships an `<SvgPath>` JSX intrinsic that maps to the C++ `SvgPathWidget` shipped in v0.61.0 (#965/#991). However, plugin bundles emitted from Claude-Design exports (and similar) ship raw `<svg><path/></svg>` markup, not `<SvgPath>`. There's no automatic shim — the dom-adapter (or a future `pulp import-design` post-process) must rewrite `<svg>` → `<SvgPath>` for inline-icon use cases. Track plugin-side adoption when bumping SDK pin past v0.69.2. <!-- docs-noise-lint: skip — retained version provenance for SvgPath rollout -->

**SDK-version drift can close audit symptoms automatically:** segmented-control vertical stacking is closed by `display:flex` defaulting to `flex-direction:row`; FilterBank canvas is auto-resolved in current SDKs; App-root layout-bottom-strip is closed by the same flex-direction default; click-bubble dispatch is also handled in current SDKs. When auditing a freshly-imported plugin against an older SDK reference, run the WebView↔Native side-by-side at idle FIRST — many "broken" rows resolve via SDK upgrade alone with zero plugin-side work. Pattern documented in `spectr/planning/audit-2026-05-03-webview-vs-native-v0.69.1.md`.

**JS string-literal escaping for emitted user text:** when `core/view/src/design_import.cpp` emits user-supplied text into single-quoted JS literals (`createLabel('...')`, `var.textContent = '...'`), the text MUST go through `js_single_quote_escape()` — newlines, single quotes, and backslashes leak through otherwise and `pulp-screenshot` crashes with "unexpected end of string" at JS eval time. The helper sits next to the existing `v0_html_attr_escape()` and covers the six emission sites that take arbitrary text: four `createLabel(text)` calls in the audio-widget column path, the generic text-node `createLabel(text_content)`, and the web-compat `var.textContent = '...'` line. Pinned by `[issue-81]` in `test/test_design_import.cpp`, which asserts both the absence of unescaped patterns AND the parity of unescaped single-quote counts on every emitted `createLabel` line. If you add a new emission site that takes user-supplied text, route it through `js_single_quote_escape()` AND extend the test.

**File-based fallback**:
- Read the file and parse based on --from source type

## Source-Contracts Registry

Pulp keeps a permissive, machine-checkable source-contract registry at
`tools/import-validation/source-contracts.json`. It records each provider's
upstream anchors, runtime/static parser symbols, fixture paths, roundtrip
script, test tags, MCP lane, and minimum runtime surface references. This file
does not replace `compat.json`: `compat.json` still owns detection fingerprints,
parser-version, format-version, and compat-schema-version.

Run the warn-only checker after changing import parsers, source fixtures, or
roundtrip scripts:

```bash
python3 tools/import-validation/check-source-contracts.py
python3 tools/import-validation/check-source-contracts.py --format markdown
python3 -m pytest tools/import-validation/test_source_contracts.py -v
```

When cleaning Catch2 tags in source-contract test files, update the matching
`validation.test_tags` entry in `source-contracts.json` in the same change.
Prefer broad stable tags such as `[view][import][designmd]` when the registry row
represents a full validation surface; do not narrow it to one subarea tag if the
roundtrip/contract expects parser, lint, diff, export, and recovery coverage.

The checker also enforces coverage symmetry between
`parse_design_source()` labels and the registry. When adding a source label
such as `jsx`, add a matching row to `source-contracts.json` and reference
its roundtrip script there; otherwise pre-push will fail strict
source-contract validation.

**`parser.runtime_file` — runtime parsers extracted out of `design_import.cpp`.**
Each contract's `parser` block has a single `file` plus `runtime`/`static`
symbol names. After the P6-A3 refactor the *runtime* parsers
(`parse_*_react`, `parse_claude_html_with_runtime`, `parse_react_native_export`)
moved into `core/view/src/claude_bundle.cpp` while the *static* parsers stayed
in `design_import.cpp`. A single `parser.file` can no longer locate both, so
contracts whose runtime parser lives elsewhere set the optional
`parser.runtime_file`. The Phase 7 follow-up split `claude_bundle.cpp` again:
the per-design-tool source-detection families and their five public
`parse_*_react` entry points (`parse_v0_dev_react`, `parse_figma_make_react`,
`parse_stitch_react`, `parse_react_native_export`, `parse_pencil_react`) now
live in `core/view/src/claude_bundle_sources.cpp`; only `parse_jsx_react` and
`parse_claude_html_with_runtime` stayed in `claude_bundle.cpp`. So a contract's
`runtime_file` is now `claude_bundle_sources.cpp` for those five and
`claude_bundle.cpp` for the JSX/Claude-HTML pair. The checker
resolves `parser.runtime` and the `explicit-runtime-parser` dispatch symbol
against `runtime_file` (falling back to `parser.file`); `parser.static` always
resolves against `parser.file`. **If a future refactor moves a parser symbol
to a new file, update `parser.file` / `parser.runtime_file` in the same PR** —
otherwise the `Source-contract registry check` step fails for every PR.

Provider MCP lanes are input-acquisition lanes only unless the source contract
explicitly says otherwise. Current runtime parsers reject raw Figma/Stitch/Pencil
MCP JSON and accept only their constrained exported artifacts.

**Where the `runtime-import-dispatch` tests live (2026-05-17 P5-1 split):**
the `WidgetBridge::install_runtime_import_handlers` tests for Figma /
Stitch / v0 / Pencil / RN were moved out of `test/test_widget_bridge.cpp`
into a sibling `test/test_widget_bridge_runtime_import.cpp` so the 14k-line
god-test file can shrink toward per-surface modules. Both files are
listed in `source-contracts.json`'s per-source `test_files` arrays, and
both targets (`pulp-test-widget-bridge` and
`pulp-test-widget-bridge-runtime-import`) are registered in
`test_targets`. When adding a new runtime-import dispatch test, place
it in the runtime-import sibling — keeping the parent file shrinking.

### Two distinct "source-contract" subjects — don't conflate them

There are two unrelated things called "source-contract" under
`tools/import-validation/`:

1. **Source-PROVIDER registry** (`source-contracts.json`, the section above)
   — Claude/Figma/Stitch/v0/Pencil trust metadata: parser symbols, fixtures,
   roundtrip scripts. Per *provider*.

2. **Per-NODE source-contract evidence** — the route/value/event/state/style
   evidence about a single imported design's nodes. Two emitters historically
   inferred this independently and could disagree: the C++ importer
   (`source_contract_overlay.node_route_rows` on a route manifest) and the JS
   audit (`jsx-contract-audit.mjs` → `inputs.sourceAuditSummary`). The
   consumers are `tools/scripts/frontend_ir_routes.py`
   (`route_rows`/`row_node_id`/`route_counts`/`primitive_counts`) and
   `tools/scripts/frontend_ir_sources.py` (`count_map`/`source_spans`).

For (2), importer and audit output share one serialized shape that can be
validated against a single definition in tests:

- Schema: `tools/import-validation/schemas/source-contract-v0.schema.json`
  (`pulp-source-contract-v0`, draft-2020-12). Lives in the public repo (not
  `planning/schemas/`) so Python, JS, and C++ can all reach one definition.
  It pins the `node_route_row` field set and the audit `materiality` counts
  block; both deliberately keep `additionalProperties` permissive so existing
  C++/JS emission is unchanged.
- Golden fixtures + conformance test:
  `tools/import-validation/fixtures/source-contract-v0/` and
  `tools/import-validation/test_source_contract_schema.py`. The test runs an
  importer overlay and an audit summary through the *real* frontend-IR
  consumers and asserts they agree on the shared `golden-expectations.json`
  counts (the "two inference models can't disagree" proof). stdlib only — no
  `jsonschema` dependency; a small validator interprets the keyword subset in
  the `frontend_ir_validation.py` style.

The legacy inline `sourceAuditSummary` compat field stays in place; it is the
audit-side input the schema validates, not something to remove in this slice.

### Step 3: Translate to Pulp code

Use the appropriate mapping document as your translation reference:
- `planning/figma-to-pulp-mapping.md` for Figma designs
- `planning/stitch-to-pulp-mapping.md` for Stitch screens
- `planning/v0-to-pulp-mapping.md` for v0 generations
- `planning/pencil-to-pulp-mapping.md` for Pencil designs

Generate Pulp web-compat JavaScript:
- Layout: `document.createElement('div')` + `el.style.flexDirection`, etc.
- Typography: `el.style.fontSize`, `el.style.fontWeight`, etc.
- Colors: `el.style.backgroundColor`, `el.style.color`, etc.
- Audio widgets detected by name: knob/dial → `createKnob()`, fader/slider → `createFader()`, meter/level/vu → `createMeter()`, xypad → `createXYPad()`, waveform → `createWaveformView()`, spectrum/analyzer → `createSpectrumView()`
- Design tokens: `theme.colors["name"] = value`, `theme.dimensions["name"] = value`

### Step 4: Write output files

- Write the generated JS to `ui.js` (or user-specified output path)
- Extract design tokens to `tokens.json` in W3C Design Tokens format
- Report: number of elements, number of tokens, any warnings

### Step 5: Offer refinement

After generating, offer to:
- Adjust specific elements ("make the knob smaller")
- Add audio widgets ("add a meter next to the gain knob")
- Change theme tokens ("use darker background colors")
- Preview the UI if a preview tool is available

For interactive review of the current checkout, prefer:

```bash
pulp design
```

If the design tool lives in a nonstandard worktree/build setup, use:

```bash
pulp design --build-dir /path/to/build --script /path/to/design-tool.js
```

When run outside a Pulp checkout, automatic binding currently only works when the `pulp` binary
itself lives inside a Pulp build tree. In generic PATH-installed or split repo/SDK layouts, pass
`--build-dir` and `--script` explicitly.

## Mapping Quick Reference

### Figma → Pulp
| Figma | Pulp |
|-------|------|
| Frame (auto-layout) | `div` with flex |
| Text | `span` |
| Rectangle | `div` with background |
| Component | JS function |
| Fill (solid) | `backgroundColor` |
| Fill (gradient) | `background: linear-gradient(...)` |
| Stroke | `border` |
| Drop shadow | `boxShadow` |
| Corner radius | `borderRadius` |

### Stitch → Pulp
| Stitch | Pulp |
|--------|------|
| Container | `div` with flex |
| Text | `span` |
| Button | `button` |
| Input | `input` |
| Card | Panel |
| Design system colors | `theme.colors` |

### v0 → Pulp
| v0 (Tailwind) | Pulp |
|----------------|------|
| `flex flex-col` | `flexDirection: 'column'` |
| `gap-4` | `gap: '16px'` |
| `bg-slate-900` | `backgroundColor: '#0f172a'` |
| `rounded-lg` | `borderRadius: '8px'` |
| `<Button>` | `createButton()` |
| `<Slider>` | `createFader()` |

### DESIGN.md → Pulp
| DESIGN.md frontmatter | Pulp IR |
|------------------------|---------|
| `colors.<name>: "#hex"` | `IRTokens.colors[name] = "#hex"` |
| `typography.<level>.<field>: ...` | `IRTokens.strings["typography.<level>.<field>"] = ...` |
| `rounded.<size>: <Nx>` | `IRTokens.dimensions["rounded-<size>"] = N` |
| `spacing.<size>: <Nx>` | `IRTokens.dimensions["spacing-<size>"] = N` |
| `components.<name>.<prop>: <ref-or-value>` | `IRTokens.strings["components.<name>.<prop>"] = ...` (refs resolved in-place) |
| `{colors.primary}` | resolved to the primitive hex value at parse time |
| `{typography.<level>}` inside `components` | preserved verbatim (composite ref) |
| `{colors}` (group ref, outside components) | broken-ref warning |
| Markdown body `## Section` | retained in `DesignMdParseResult.sections` for Phase 2 lint |
| Unknown `## Section` (e.g. `## Iconography`) | preserved without error |
| Duplicate `## Section` | error-severity diagnostic → exit 3 |

### Pencil → Pulp
| Pencil | Pulp |
|--------|------|
| Frame (auto-layout) | `div` with flex |
| Text | `span` |
| Rectangle | `div` with background |
| Variables (COLOR) | `theme.colors` |
| Variables (FLOAT) | `theme.dimensions` |

## Stable-Anchor Identity (Phase 0a — inspector round-trip prerequisite)

Every tree-producing parser now stamps `IRNode::stable_anchor_id`,
`source_node_id` (when the source has a native ID), `provenance`
(adapter + version + source_uri), and `confidence` (PASS / DIVERGE /
NOT_IMPL). These fields key the tweaks layer (`pulp-tweaks.json`) so
inspector direct-manipulation edits survive re-import.

**If you add a new parser, you MUST:**

1. After the IR tree is built, stamp `ir.root.provenance` with the
   adapter name (e.g. `"figma"`, `"stitch-html"`, `"pencil"`), the
   adapter version, and the source URI.
2. Stamp `ir.root.confidence`:
   - `IRConfidence::pass` for a clean lowering
   - `IRConfidence::diverge` for lossy / regex / best-effort paths
   - `IRConfidence::not_impl` if the adapter can't lower the source yet
3. Call `assign_anchors(ir.root, strategy, adapter_name)` with the
   strategy that matches your source kind:
   - **adapter** strategy when the source has native stable IDs
     (Figma layer UUIDs, Pencil node IDs, Mitosis content-hash IDs).
     Requires `adapter_name` (becomes the `"<name>:"` prefix) AND
     `source_node_id` populated on each node.
   - **content-hash** strategy when there are no native IDs
     (Stitch HTML, v0 TSX, Claude Design HTML, raw JSX, generic HTML).
     The hash is FNV-1a 32-bit base-36 over (tag, role, normalized
     text, depth, sigIndex).
   - **path** strategy for RN-style file exports / hand-edited code
     where source position is the most stable identity.
4. The strategy default lives in `default_anchor_strategy()` in
   `core/view/include/pulp/view/anchor_strategy.hpp` — mirrors the TS
   `DEFAULT_ANCHOR_STRATEGY` map in `packages/pulp-import-ir/src/anchors.ts`.

**Why this matters:** Phase 1+ of the inspector roadmap writes user
inspector edits to a sidecar `pulp-tweaks.json` keyed by
`stable_anchor_id`. Without populated anchors the tweaks layer has
nothing to match against on re-import — defeating the "edit anywhere,
never lose work" principle. A parser that skips `assign_anchors` will
silently produce a tree where inspector edits get orphaned on the
next re-import.

**Codegen contract:** `generate_pulp_js` (both web-compat and
`bridge_native_js` modes) emits `// @pulp-anchor <id>` trail comments next to each
element when `opts.include_comments == true`. The runtime inspector
parses these to map generated elements back to their tweak-layer
identity. If you write a custom codegen path, preserve this pattern
so the inspector can still trace identity.

**Phase 0b — `setAnchor()` bridge wiring:** the web-compat codegen
path *also* emits a functional `setAnchor(<var>._id, '<anchor>')` call
after each createElement, AND the call is emitted unconditionally —
NOT gated on `opts.include_comments`. Rationale: the
`// @pulp-anchor` trail is cosmetic (for grep / debugging), but
`setAnchor()` is functional — the inspector cannot find a widget's
anchor without it, so dropping it in minified codegen would silently
break inspector tweaks in production. If you write a custom codegen
path, emit both: the comment (gated) for debuggability and the
setAnchor call (unconditional) for the runtime. The bridge side
(`WidgetBridge::setAnchor`) is a silent no-op on unknown widget IDs,
matching the rest of the bridge's tolerance for unmounted ids.

**Codex P1 follow-up (#2303):** The first argument to `setAnchor` <!-- docs-noise-lint: skip — retained: pins guidance to the PR that introduced the _id contract -->
MUST be the element's internal `_id` (the auto-generated `__el_N__`
that `document.createElement` assigns in `core/view/js/web-compat.js`),
NOT the generated JS variable name. The bridge keys `widget()` lookup
on `_id`; passing the var name silently no-ops and breaks the entire
anchor wiring chain for web-compat imports. Pre-fix codegen emitted
`setAnchor('var', ...)` (broken); post-fix emits `setAnchor(var._id, ...)`
(correct). If you write a new codegen variant or a non-web-compat
shim, ensure whatever id you pass matches the bridge's widget()
lookup key — not the JS-local variable name.

`bridge_native_js` codegen does NOT yet emit `setAnchor` (small
follow-up; the native-bridge JS codegen has many early-return branches
that need each to be wired). `bridge_native_js` is the default codegen
mode for imports; pass `--web-compat` only when the DOM-compatible JS
lane is required.

**Phase 5.1 — `setSource()` source-jump wiring:** alongside `setAnchor`,
the `@pulp/react` reconciler now forwards React's dev-mode `__source`
prop ({fileName, lineNumber, columnNumber}) through a `setSource(id,
file, line, col)` bridge call (`materializeUnder` →
`bindSourceLocation` in `packages/pulp-react/src/host-config.ts`). This
lands a `View::SourceLocation` on the live widget so the inspector's
`J` hotkey / `Inspector.jumpToSource` protocol method can open the
authoring JSX file:line in the user's editor. Gotchas:

- **`__source` is Babel-classic / automatic-dev-runtime only.** esbuild's
  `jsx: 'transform'` mode (current `jsx-transform.mjs` setting) does
  **not** inline `__source` props — only Babel's
  `@babel/plugin-transform-react-jsx-development` or esbuild's
  `jsx: 'automatic'` + `jsxDev: true` do. So today `bindSourceLocation`
  is a silent no-op for transform-mode bundles; it activates the moment
  the JSX runtime emits `__source`. Migrating `jsx-transform.mjs` to the
  automatic dev runtime is the remaining Phase 5.1 follow-up — do it as
  a deliberate, separately-validated change because it shifts
  `jsxFactory` semantics across every existing import fixture.
- **Source maps gate behind `PULP_JSX_SOURCEMAP=1`.** `jsx-transform.mjs`
  emits an inline source map only when that env var is set — off by
  default to keep production bundles lean (~10-30% size add). It is
  independent of the `__source` prop path.
- **`setSource` no-ops on an empty file path and unknown widget id** —
  same tolerance as `setAnchor`. A custom codegen / shim that wants
  source-jump must pass the same bridge-keyed `_id` as `setAnchor`.

### Phase 1 — Tweaks persistence (`pulp-tweaks.json`)

`TweakStore` (`inspect/include/pulp/inspect/tweak_store.hpp`) now reads
and writes a sidecar `pulp-tweaks.json` so inspector edits survive
process restart.

- **File location** — resolved by `TweakStore::default_tweaks_path()`:
  1. `$PULP_TWEAKS_FILE` env var if set (verbatim — useful for tests
     and headless CI runs).
  2. Otherwise walks up from `cwd` looking for a directory containing
     `package.json`; if found uses `<project_root>/pulp-tweaks.json`.
  3. Otherwise `<cwd>/pulp-tweaks.json`.
- **Schema** — `{ "$schema": "pulp-tweaks://v1", "version": 1,
  "tweaks": { anchor: { dottedPath: value } }, "bypassed": { anchor:
  true | string[] }, "sources"?: { anchor: { dottedPath: source } } }`.
  Mirrors `packages/pulp-import-ir/src/tweaks.ts` `TweaksFile` with
  the sibling `bypassed` overlay and `sources` sidecar Phase 1 adds.
  Files written by `@pulp/import-ir` (no integer `version`) load as
  v1 for back-compat; an explicit unknown `version` is a hard error
  so we never silently drop fields we don't understand.
- **Atomic write** — `save_to_disk()` writes `<path>.tmp` and renames
  over the target; no partial flush ever lands at the canonical path.
- **Auto-save** — opt-in via `TweakStore::set_auto_save(true)` or
  `Inspector.setAutoSave { enabled, path? }` over the protocol. OFF
  by default so unit tests don't touch disk by accident.
- **Protocol surface** — `Inspector.loadTweaks { path? }`,
  `Inspector.saveTweaks { path? }`, `Inspector.setAutoSave { enabled,
  path? }`. All three default `path` to `default_tweaks_path()`.

When wiring a new inspector client (web UI, CLI, MCP tool), prefer the
protocol methods over reaching into the C++ store directly — the
defaults + error reporting are the same and you get the resolved path
echoed back in the response for logging.

### Phase 4a — Lock-to-source, Path A (generated-TSX/JS rewrite)

`pulp/view/lock_to_source.hpp` (impl `core/view/src/lock_to_source.cpp`)
is the engine that **promotes a tweak back into the generated import
artifact** so the edit is permanent and survives a fresh re-import.
Path A only — the artifact `pulp import-design` lowers a design into.
Path B (live React-bundle AST patch, #1308) and Path C (DESIGN.md <!-- docs-noise-lint: skip — retained: pins Path B/C scope to their tracking issue -->
token export) are separate phases and are NOT in this engine.

- **How the element is found** — every web-compat element block carries
  the `// @pulp-anchor <id>` trail comment (emitted by `generate_pulp_js`
  when `include_comments` is on). `lock_tweak_into_source()` locates the
  block by that comment, then rewrites or inserts the matching
  `<var>.style.<prop>` assignment. The block ends at the next
  `// @pulp-anchor` comment or the first blank line (codegen emits
  exactly one blank line between elements).
- **Property-path mapping** — `lock_property_to_style_name()` collapses
  the dotted tweak paths (`paint.*`, `style.*`, `layout.*`, `transform.*`,
  or a bare name) onto the camelCase `el.style.<name>` surface. Hyphen/snake
  fragments camelCase. The allow-list is exactly the set of properties
  `generate_node()` emits — an unknown / mistyped path reports
  `unsupported_property` instead of writing a bogus assignment.
- **WYSIWYG T4 — reorder + proportional-resize round-trip.** Two inspector
  direct-manipulation gestures persist tweaks under non-`paint/style/layout`
  paths and need explicit handling here:
  - `layout.order` — the reflow-aware drag-to-reorder rewrites `flex().order`;
    it maps to the `order` style property (added to the `kKnown` allow-list).
  - `transform.scale` — the proportional Shift-resize persists a bare scale
    factor under the `transform` namespace. The namespace collapses onto the
    single `transform` style line, and `format_lock_value()` wraps the bare
    factor into the CSS function form (`1.5` → `transform = 'scale(1.5)'`). A
    value already containing `(` passes through so we never double-wrap.
  When you add a NEW transform sub-component (rotate/translate) or a new flex
  reorder property, extend both the `kKnown` allow-list AND `format_lock_value()`
  if the tweak value needs a CSS-function wrapper.
- **Status semantics** — `rewritten` / `inserted` mutate the text;
  `already_current` is the idempotent re-lock no-op; `anchor_not_found`
  and `unsupported_property` are graceful failures that leave the
  source byte-identical so the caller keeps the tweak in the sidecar.
- **`@generated` boundary guard** — `is_generated_source()` is the
  cheap check for the roadmap's "only lock into files marked
  `@generated`" rule. It accepts both the Pulp codegen banner
  (`// Generated by Pulp import-design …`) and a conventional
  `// @generated` marker. The CLI / inspector layer owns the
  read-confirm-write loop; the engine is pure text-in / text-out.
- **Round-trip contract** — locking a tweak into the generated text
  produces exactly the text `generate_pulp_js` would emit had the IR
  carried the tweaked value all along. `test_lock_to_source.cpp`
  pins that byte-for-byte.

If you add a codegen property to `design_codegen.cpp`'s `generate_node`,
add it to the `kKnown` allow-list in `lock_property_to_style_name()` too
— otherwise that property can never be locked to source.

- **WYSIWYG T5 — structural reparent (`reparent_in_source`).** A reflow-aware
  drop ("drop element A inside container B") is a TREE edit, not a style tweak.
  In the generated artifact every element block ends with
  `<parentVar>.appendChild(<var>);`. `reparent_in_source(source, {child_anchor,
  new_parent_anchor})` locates the child block by anchor, finds its
  `<oldParent>.appendChild(<childVar>);` line, resolves the new parent block's
  `const <var> =` name, and rewrites the receiver to that var. Status semantics
  mirror `lock_tweak_into_source` (`rewritten` / `already_current` /
  `anchor_not_found`). The shared block helpers `find_anchor_block()` +
  `block_var_name()` back both engines.
  - **Now physically relocates the block (gap closed).** `reparent_in_source`
    moves the element's FULL source subtree — the block PLUS every DFS-contiguous
    descendant block — to sit physically under the new parent, then re-indents it
    one 2-space step in. The receiver rewrite alone is enough for the live DOM
    (createElement + appendChild are order-independent), but a generated artifact
    must also read correctly and round-trip a fresh re-import, so the block moves
    too. **Subtree boundary gotcha:** `find_subtree_range()` detects the end of a
    subtree purely from `// @pulp-anchor` comments + indentation — codegen is
    depth-first, so a subtree is the run of lines until the next anchor at
    `indent <= base` (a sibling/ancestor) or a non-anchor line indented `< base`
    (the enclosing tail, e.g. `document.body.appendChild(root)`). Do NOT reuse
    `find_anchor_block()` for relocation — it stops at the first blank line, which
    is just ONE element block, not the whole subtree.
  - **Unsafe-reparent guard — REFUSE, don't rewrite (WYSIWYG sweep P2 fix).** If
    the new parent's anchor lies INSIDE the child's subtree (dropping a node under
    its own descendant) or the subtree span can't be resolved, the engine now
    rewrites NOTHING and returns `anchor_not_found` with `result.source` LEFT
    BYTE-IDENTICAL to the input (plus a `"refused reparent ... cyclic/invalid
    source"` message). **Gotcha — this is a behavior change:** the prior code
    skipped only the physical block move but STILL rewrote the `appendChild`
    receiver, which emitted `<descendant>.appendChild(<ancestor>);` — cyclic,
    invalid source the re-import engine chokes on. Receiver-rewrite-without-move is
    NOT a safe fallback for the cyclic case; the only safe outcome is to mutate
    nothing. The live gesture's `is_self_or_ancestor` already prevents this, but
    the source engine must defend independently. Test asserts `r.source == gen`.
  - **Live gesture → source wiring (gap closed).** `InspectorOverlay` exposes
    `set_reparent_source_sink(std::function<void(ReparentSourceEdit{child_anchor,
    new_parent_anchor})>)`. At the `commit_reflow_drop` undo-entry site, a genuine
    cross-parent reparent of an anchored view emits through the sink: the `do_fn`
    locks under the NEW parent, the `undo_fn` re-emits with the ORIGINAL parent so
    the host re-derives the inverse rewrite. **Gotcha:** `EditHistory::perform()`
    runs `do_fn` immediately (it calls `redo()`), so the sink fires once on the
    initial commit — don't ALSO call the sink inline before `perform()` or you
    double-rewrite. The overlay is filesystem-free by design; the HOST owns the
    source text + read/confirm/write loop. `examples/ui-preview` wires the sink to
    its `--script` file behind the `is_generated_source()` `@generated`-boundary
    guard, so a hand-authored script is never rewritten. Coverage:
    `pulp-test-lock-to-source [wysiwyg][t5]` (engine: relocation + idempotency +
    guard) and `pulp-test-inspector [wysiwyg][t5]` (gesture round-trip: live →
    source → undo reverts both → redo → idempotent; plus the live-only no-sink
    path).
  - **Insertion SLOT (WYSIWYG sweep P1 fix).** `ReparentToSourceEdit` /
    `ReparentSourceEdit` carry a third field `insert_after_anchor[_id]`: the
    anchor of the sibling the moved block should physically FOLLOW under the new
    parent, or `""` = first child. **Gotcha:** without it the relocation always
    dropped the block as the parent's FIRST child, silently discarding the drop
    position the user dragged to. The overlay computes the preceding visible
    sibling in flex-order (`preceding_sibling_anchor`) for BOTH the new-parent
    (do_fn) and old-parent (undo_fn) sides. Empty / unresolved slot → first-child
    fallback (prior behavior preserved). `insert_after_anchor` must resolve in the
    POST-erase buffer (after the moved subtree is removed), so the engine re-finds
    it then.
  - **Same-parent reorder IS persisted (WYSIWYG sweep P1 fix).** A same-parent
    reflow reorder rewrites `flex().order` live; `commit_reflow_drop` now ALSO
    emits a `layout.order` tweak (keyed by each view's OWN anchor) for the dragged
    child AND every sibling whose order was normalized. **Gotcha:** an old comment
    claimed the reorder was "persisted elsewhere" — it wasn't, so the new order
    vanished on a fresh re-import. `layout.order` was already in the lock
    allow-list (T4), so it round-trips as `el.style.order`. Un-anchored children
    are skipped (nothing to lock). The cross-parent source sink only fires for a
    genuine PARENT change; a pure reorder relies on the `layout.order` tweak path.
    Coverage: `pulp-test-lock-to-source [issue-wysiwyg-reflow-slot]` (slot
    after-sibling / first-child / unresolved-fallback) and
    `pulp-test-inspector [issue-wysiwyg-reflow-slot]` (reorder tweak round-trip;
    cross-parent slot carried to the sink).

### Phase 4b — Lock-to-source, Path B (hand-authored JSX/TSX patch)

`pulp/view/jsx_lock.hpp` (impl `core/view/src/jsx_lock.cpp`) is the
Path B sibling of Path A; the roadmap tracks Path B under issue #1308. <!-- docs-noise-lint: skip — retained: pins Path B scope to its tracking issue -->
Where Path A rewrites the *generated* web-compat artifact, Path B
patches the user's **own hand-authored JSX/TSX** — the source behind a
live React bundle (`--from jsx`, `--execute-bundle`). There is no
generated file to rewrite, so the engine edits the authored source
directly.

- **How the element is found** — an element-instrumentation pass injects
  a `data-pulp-anchor="<stable_anchor_id>"` attribute onto each authored
  element (the JS-side instrumentation is a separate deliverable; the
  engine only *consumes* the marker). `jsx_lock_tweak_into_source()`
  scans for the matching attribute, walks left to the opening `<` and
  right to the tag-closing `>` (respecting strings and `{…}` braces so a
  `>` inside `{a > b}` does not end the tag early).
- **Surgical patch, not an AST re-emit** — mirrors 4a/4c. The engine
  rewrites exactly one literal span: a property inside an inline
  `style={{…}}` object, or a bare attribute (`width={80}`,
  `color="#888"`). Every other byte — comments, imports, formatting,
  sibling props — is preserved byte-for-byte. It is deliberately NOT a
  general JSX printer (Codex capped Path B from ballooning into a
  multi-quarter parser).
- **`too_dynamic` is the safety valve** — anything that is not a plain
  rewritable literal fails as `too_dynamic` with a specific reason, and
  the source is returned unchanged so the caller keeps the tweak in the
  sidecar: a `style={{ ...base }}` spread, a computed key, a non-literal
  value (`padding={gap * 2}`, `color={theme.fg}`), `style={someVar}`, or
  a prop the author simply never wrote (Phase 4b patches *existing*
  props only — it never inserts, which would risk a malformed tag).
- **Other statuses** — `patched` mutates; `already_current` is the
  idempotent no-op; `anchor_not_found` and `anchor_ambiguous` (the same
  `data-pulp-anchor` on two elements — refuse to guess) and
  `unsupported_property` are graceful failures that leave the source
  byte-identical.
- **Value rendering** — a quoted prop keeps its quote style (contents
  rewritten, single-quotes escaped for a JS string body); a bare numeric
  prop stays bare when the new value is numeric, but is promoted to a
  quoted string when the new value carries a unit (`width={80}` →
  `width={'120px'}`).
- **`jsx_lock_property_to_key()`** shares the same `kKnown` allow-list as
  Path A's `lock_property_to_style_name()` — keep the two in sync so a
  tweak can target the same set of properties on either path.

The engine is pure text-in / text-out — no filesystem I/O — so the
overlay / CLI layer owns the read-confirm-write loop and the
authored-vs-generated routing (`is_authored_jsx_source()` is the cheap
guard: a file carrying the codegen banner is Path A's, not Path B's).
`test_jsx_lock.cpp` pins the patch, the formatting-preservation
contract, and every failure path.

### Phase 4c — token lock-to-source (`DESIGN.md` rewrite)

`token_lock.hpp` / `token_lock.cpp` (`core/view/`) lock a *token-typed*
inspector tweak back into the project's `DESIGN.md` so a re-import picks
up the corrected token instead of re-introducing the stale one. This is
the token-level sibling of Phase 4a (generated-TSX rewrite) and 4b
(JSX/TSX AST patch).

- **Token vs element classification** — `classify_token_tweak(anchor,
  property_path)` returns a `TokenTarget` when the tweak addresses a
  DESIGN.md token, `std::nullopt` for element-only tweaks (`paint.*`,
  `layout.*`, `text.*` — those lock via 4a/4b). Two signals, in
  priority order: a dotted property path whose head is a canonical
  token group (`colors` / `spacing` / `rounded` / `typography`), or a
  `designtoken:<group>.<name>` anchor. Typography paths must be
  three-segment (`typography.<level>.<field>`). `components.*` is
  deliberately **not** lockable — a component entry is a reference
  bundle, not a primitive value.
- **Surgical text rewrite, not `export_designmd`** — the lock parses
  the YAML frontmatter only to *locate* the token (yaml-cpp `Mark()`
  gives the value line), then edits exactly that one value span in the
  original file text. Prose sections, YAML comments, key order,
  indentation, and the value's original quote style are all preserved.
  `export_designmd(Theme, ...)` re-emits the whole file and would lose
  every one of those — never use it for a single-token lock. (It also
  still throws, gated on #1307.)
- **Conservatism** — `lock_token_in_designmd` fails (and returns the
  input byte-identical) on: no frontmatter, missing group/token/field,
  a nested color palette (not a scalar), or a source line that does not
  match the expected `key: <value>` shape. A failed lock never mutates
  DESIGN.md. The locator keys off the *group*, so a leaf name that
  appears in two groups (e.g. `md` under both `spacing` and `rounded`)
  still resolves unambiguously.
- **Overlay wiring deferred** — the inspector overlay "lock token"
  affordance is intentionally not wired here (`inspector_overlay.cpp`
  has in-flight PRs). The engine is pure data-in/data-out so the
  overlay/protocol layer can adopt it without a rebuild.

Spec + design:
[`planning/2026-05-18-inspector-direct-manipulation-roadmap.md`](../../../planning/2026-05-18-inspector-direct-manipulation-roadmap.md)

## Automated Validation Loop

### Freshness check (MUST run first)

Before running any roundtrip harness against the framework, **verify your checkout is current with `origin/main`**. A stale feature branch can produce "wrong UI variant" diff scores that reflect old framework code, not parser behavior.

The `tools/import-validation/*-roundtrip.sh` scripts now refuse to run on a stale checkout. Bypass only when you specifically want to validate a feature branch:

```bash
# Default: refuse to run if HEAD is behind origin/main
tools/import-validation/spectr-roundtrip.sh

# Explicitly allow staleness (e.g., validating a feature branch's code)
PULP_FRESHNESS_BYPASS=1 tools/import-validation/spectr-roundtrip.sh

# Or accept up to N commits behind
tools/scripts/check_workspace_freshness.sh --max-behind 10 && tools/import-validation/spectr-roundtrip.sh
```

Also verify the **installed SDK** matches your expectations:
```bash
pulp sdk status              # what's installed
pulp doctor --versions       # CLI vs project vs installed
```
If you ran `pulp upgrade` recently, the CLI bumped but the SDK might not have. Use `pulp sdk install` to pull the latest SDK matching the CLI.

### Diff loop

After generating Pulp code, ALWAYS validate by comparing with the source design:

1. **Screenshot the source design** via MCP:
   - Pencil: `get_screenshot(nodeId)`
   - Save to a temp file as the reference

2. **Render the generated JS** headlessly:
   ```bash
   pulp-screenshot --script generated.js --output render.png --width W --height H --backend skia
   ```

   > ⚠️ **Placeholders mean the wrong backend, not a broken import.** If a render
   > shows an image's *filename* in a box (e.g. "3_228.png") instead of the
   > picture, `ImageView::paint` drew a placeholder because the renderer could not
   > composite file-backed images. That is a backend problem — do NOT go looking
   > for an import bug.
   >
   > Only two things cause it:
   > 1. **`--screenshot-backend coregraphics`** — an explicit escape hatch.
   >    CoreGraphics does not composite file-backed images.
   > 2. **A GPU-off (non-Skia-linked) build** — the real decode lives in the Skia
   >    renderer, which isn't linked in a `PULP_ENABLE_GPU=OFF` importer build.
   >
   > **`--validate` defaults to the Skia backend and DOES composite file-backed
   > images** (`--screenshot-backend {skia|coregraphics}`, default `skia`; see
   > `pulp import-design --help`). So you do **not** need a separate
   > `pulp-screenshot` pass just because a design has image assets — that costs an
   > extra round-trip per iteration for nothing. Reach for `pulp-screenshot` when
   > you want a live/host-surface capture or a size/scale the importer's render
   > doesn't offer, not as a routine workaround.
   >
   > Both paths need a Skia-linked (`PULP_ENABLE_GPU=ON`) build to show images.
   > Verify the backend before diagnosing anything visual.

3. **Compare** reference vs render. The importer's built-in `--validate --reference` diff works for designs **with or without** image assets, because `--validate` renders on the Skia backend by default and composites file-backed images:
   ```bash
   pulp import-design --from X --file input --validate --reference source.png --diff diff.png
   ```
   Use `fidelity_diff.py` when you want **per-widget region checks** rather than one global similarity number — it detects widget regions and checks aspect/gradient/text per widget, which a whole-board score cannot:
   ```bash
   python3 tools/import-design/fidelity_diff.py --render render.png --scene scene.pulp.json --frame-reference source.png
   ```
   > **`--validate` alone is advisory.** It prints `PASS` or `NEEDS REVIEW` but
   > **exits 0 either way**, at any similarity — a 0%-similar render still exits
   > 0. Read the printed similarity (and the diff image); never infer success
   > from `$?` unless you asked for a gate.
   >
   > **To gate on it, add `--fail-below <pct>`** — the run then exits 5 when the
   > similarity is under `<pct>`. The value is a percentage 0-100 (`85`, not
   > `0.85`; a fraction is rejected rather than silently treated as 0.85%, which
   > would never fire). It requires `--reference`, since there is otherwise
   > nothing to compare against:
   > ```bash
   > pulp import-design --from X --file input \
   >     --validate --reference source.png --fail-below 85
   > ```

4. **Review the diff image** — red highlights show differences

5. **Iterate if needed** — adjust the generated code and re-render until similarity is acceptable (>=85%, the shared default in `pulp::view::kDefaultSimilarityThreshold`)

**Always show comparisons as a LABELED montage** — `tools/import-design/montage.py` stacks N renders into one image with a titled bar above each panel (labels ON by default), so a reference-vs-render(s) comparison is self-documenting (you can tell which panel is which without an external caption — a bare montage gets misread):
```bash
python3 tools/import-design/montage.py --out compare.png \
  "reference.png:1. Figma reference" \
  "render.png:2. Pulp render (real export)" \
  "rest.png:3. Pulp render (headless REST)"
# --columns N for side-by-side; --no-labels to opt out; --config montage.json for defaults
```
Smoke-tested by `test/test_import_montage.py` (CTest `import-montage`, skips without PIL).

### Keyboard shortcut extraction (UX best-practice default)

The library function `extract_keyboard_shortcuts(source, filename)` scans
imported React source for inline `onKeyDown={e => ...}`,
`window.addEventListener('keydown', ...)`, and `if (e.key === 'X')` patterns,
returning a `std::vector<DetectedShortcut>` for the design-import emitter
to register via Pulp's runtime `registerShortcut(key, modifiers, callback)`
surface. Modifier idioms (`metaKey || ctrlKey`) collapse to a single `meta`
entry per the cross-platform shortcut convention. Use
`serialize_detected_shortcuts()` to emit the stable JSON manifest.

The matcher is **lexical only** — handler bodies that reference React state
(e.g. `setActiveTab(2)`) can't be auto-wired yet; the manifest surfaces them
for human triage via a `handler_excerpt` field. V1 (this slice) only emits
the manifest; follow-up slices wire the CLI flag and emit `registerShortcut(...)`
into the generated JS. Default-on with a planned `--no-import-shortcuts` opt-out.

### Yoga Layout Rules (MUST follow)
- Every container needs explicit `height`, `min_height`, or `flex_grow`
- Labels need `min_height` (14px for normal text, 12px for small)
- Faders need `min_width >= 40px` for thumb rendering
- Meters need `min_width >= 20px` for bar visibility
- Knobs need `min_size >= 56px` for arc rendering
- Use `createCol`/`createRow` for containers (NOT `createPanel` which adds glass overlay)
- Row height = max child height; Column height = sum of child heights + gaps

### Proportional resize for fixed-design native-react imports

When you import a design that was authored at a known fixed size (Spectr's
editor.js at 1320×860, a Figma frame at 1440×900, etc.) and want the live
window to resize proportionally **without** re-layout, the right primitive
is `WindowHost::set_design_viewport(w, h)`:

```cpp
auto window = WindowHost::create(root, opts);
window->set_design_viewport(kDesignWidth, kDesignHeight);
window->set_fixed_aspect_ratio(kDesignWidth / kDesignHeight);
```

What it does: pins root.bounds at design size on every paint, applies an
aspect-correct scale + letterbox translate so the design fits inside the
current window, inverse-maps mouse coords before hit-test. The window can
change size; root never knows.

**Do not** try to solve proportional resize for fixed-design imports with:

1. **Per-child `set_scale()` on root children.** Scales chrome but
   `CanvasWidget` records its draw commands at the original size, so
   `<canvas>` content gets clipped on shrink. Tried and burned through 2026-05-13/14.
2. **Yoga `absolute + inset:0` propagation.** Chains of
   `position:absolute + inset:0` collapse to 0×0 in Pulp's runtime-import
   because Yoga only fills a containing block when the parent has a
   definite POINTS size — the cascade root→body→wrap→canvas never gets
   one. This is **architectural** (Yoga is flex+grid only), not a bug.
3. **JS-driven canvas refit via React refs.** Even with `getPublicInstance`
   correctly returning a DOM-shim Element so refs match the element that
   `getBoundingClientRect` queries, many native-react `resize()` functions
   (Spectr's included) bail on `wrapRef.current`/`canvasRef.current`
   existence checks and never run.

The design-viewport approach sidesteps all three by doing the resize at the
renderer (paint-time scale of the design surface), which is what a browser
webview effectively does at the layer level.

### Read-only verifier + verdict contract (hardens the loop)

The diff loop above tells you *how far off* a render is; it does not stop the two
failure modes that have cost real review cycles: a reviewer that **edits while
reviewing** (hiding the defect it was meant to catch) and a **verdict-less
"looks good"** on a render nobody actually saw. Harden it with three contracts:

1. **Vision probe first.** Before reading any capture into context for a visual
   judgment, run the vision probe (see the `screenshot` skill): a one-shot
   `VISION_OK` / `VISION_UNSUPPORTED` check against a tiny known PNG. On failure,
   save captures, report only paths, and disclose "visual review skipped". Never
   describe an image the harness may have dropped.

2. **Read-only verifier subagent.** Hand a fresh-context subagent only {project
   dir, changed files, render/log outputs, image-input status} and the prompt in
   [`verifier-prompt.md`](verifier-prompt.md). It checks logs, runs the
   unresolved-token audit (`pulp design lint-adherence`, exact and non-visual),
   and does root-cause layout probes (`pulp_inspect_dom` /
   `pulp_inspect_evaluate` — dump the offending element AND its parent so the
   finding names the *constraint*, not the pixel). It **may not edit**.

3. **Verdict contract.** The verifier's entire final message is `done` or
   `needs_work: <root cause>` — never prose. The main agent fixes the named
   cause and re-invokes the verifier with fresh context; loop until `done`. This
   degrades honestly when image input is unavailable (the token/log half still
   gates) instead of emitting confident fiction.

## CLI Alternative

The deterministic import tool is also available:
```bash
pulp import-design --from figma --file design.json
pulp import-design --from stitch --file screen.html
pulp import-design --from v0 --file component.tsx
pulp import-design --from pencil --file design.json
pulp import-design --from claude --file design.html   # writes ui.js + tokens.json + classnames.json + bridge_handlers.cpp (static parser — loader-shell only)
pulp import-design --from claude --file design.html --execute-bundle   # runs the bundled React app in QuickJS, walks the materialized DOM (#468)

# With validation
pulp import-design --from pencil --file design.json --validate --reference source.png --diff diff.png

# Override or skip the bridge scaffold (claude only)
pulp import-design --from claude --file design.html --bridge-output editor/handlers.cpp
pulp import-design --from claude --file design.html --no-bridge-scaffold

# Override or skip the classnames artifact (claude only — pulp #1035)
pulp import-design --from claude --file design.html --classnames editor/classnames.json
pulp import-design --from claude --file design.html --no-emit-classnames
```

Every successful JS-lane import (including `--dry-run`) ends with a one-line
per-stage timing breakdown on stdout, e.g.
`✓ imported "A Channel FX" (1264 nodes) in 4.47s  — decode 157ms · parse 178ms · codegen 3.93s · render 177ms`.
`decode` spans everything that produces the parseable envelope content (for
`--from fig`, the offline Node decode subprocess included); `render` appears
only when `--validate` actually rendered. Stages that don't run are omitted,
not shown as zero. The line prints only on success — failing imports (parse
error, `--strict-fidelity`, `--fail-below`) never end on the check mark.

Import artifact flag vocabulary:
- `--output <path>` is the destination for the primary artifact. JS defaults to `ui.js`; `--emit cpp` defaults to `imported_ui.cpp` and writes the sibling header; `--emit swiftui` defaults to `ImportedPulpView.swift` (with a per-view sibling `<RootView>Theme.swift` + `.bindings.json`).
- `--emit js`, `--emit ir-json`, `--emit cpp`, and `--emit swiftui` are implemented. `cpp` and `swiftui` require `--mode baked`. Legacy `--emit classnames` remains accepted for the Claude classnames sidecar.
- Built-in default is live runtime import: `--mode live --emit js`. Static sources emit generated JS in live mode; `--from jsx --mode live --emit js` writes the precompiled JSX bundle verbatim for runtime import and rejects `--validate`, `--reference`, `--diff`, and `--debug`. `--mode baked` emits canonical IR, baked C++, or baked SwiftUI via `--emit ir-json|cpp|swiftui`.

`--emit swiftui` (baked SwiftUI, Workstream B1) is a fourth DesignIR lowering in
`core/view/src/design_swift_codegen.cpp` (`generate_pulp_swift`), parallel to the
baked-C++ baker. It mirrors the C++ emit loop (`resolve_design_ir_native` + node
walk) but emits declarative SwiftUI: frame→VStack/HStack, text→Text, fixed
frame/padding/background, and knob/slider/toggle→`PulpKnob`/`PulpSlider`/
`PulpToggle`. Tokens lower to a code-first per-view `<RootView>Theme.swift` (enum
named per-view so two imports don't collide in one Swift target) reusing the same
base/`.dark` partition *algorithm* as `export_css_variables` (color.bg + color.bg.dark
→ a nested-private dynamic light/dark Color helper). Generated views are generic over
`PulpParameterResolving` and resolve a binding key by **exact `PulpParameter.name`
match** (there is no stable string param key; `missing`/`duplicate` are surfaced,
never silently mis-bound — see `apple/Sources/PulpSwift/PulpParameter.swift`).
B3 adds the remaining widgets (`PulpMeter`/`PulpXYPad`/`PulpWaveform`/
`PulpSpectrum` in `PulpViews.swift`, plus text buttons → SwiftUI `Button`). B4
brings the SwiftUI binding manifest to parity with the C++ manifest — it emits
the same `NativeBindingMetadata` field set per entry (a cross-check test asserts
the field/value pairs match `generate_pulp_cpp`'s manifest), adds a
`conventions` block (gesture grouping / normalized range / poll), and the
generated controls round-trip through a mock store (`PulpParameterTests`). B5
adds CSS grid → `LazyVGrid` (column COUNT from `grid-template-columns`, mapped
to equal `.flexible()` `GridItem`s; exact fr/px/minmax sizing + explicit
placement approximated → informational `swiftui-grid-tracks`, no longer a hard
divergence), image assets → `Image("<asset_id>")` (bundled, referenced by id in
the app's asset catalog) or `AsyncImage(url:)` (remote http(s)), and a host
scaffold under `templates/swiftui-design-host/` that mounts the generated root
view against a `PulpParameterStore`. The
visualizers (waveform/spectrum) have no audio buffer in a baked import and
xy_pad's second axis has no IR source, so they bind the one available parameter
and emit an informational fidelity note; svg/canvas vector leaves still degrade
to a sized `Color.clear` (rasterization not modelled), and an `xcassets` color
catalog for dark mode is deferred (the theme's dynamic colors already cover it).
The test gate is golden strings
**plus** a `swiftc -typecheck` of the generated Swift against the real PulpSwift
module (golden C++-string asserts alone can ship non-compiling Swift).

**B2 (full style + text-runs + flex-fidelity).** `emit_modifiers` now emits the
full visual set: opacity, corner radius (uniform; uneven per-corner → largest +
advisory note), border overlay stroke, box-shadow (first layer; SwiftUI radius =
CSS blur / 2), linear-gradient background, CSS transform, mix-blend-mode. CSS
colour parsing accepts hex AND `rgb()`/`rgba()` (`parse_css_color`). Mixed-style
text (`IRTextRun`) lowers to a `+`-concatenated chain of styled `Text` segments
(SwiftUI's Text-returning modifier overloads keep the chain typed as Text), byte
offsets snapped to UTF-8 boundaries. Flex→stack mapping: cross-axis `align` →
the stack's `alignment:` argument (emitted only when non-`.center`, so B1 goldens
are unchanged); `justify` space-between/around/flex-end approximated with
`Spacer()` interposition **only** when the resulting subview count stays ≤ 10
(the ViewBuilder arity limit), else flagged and dropped. Anything a SwiftUI stack
cannot reproduce becomes a `FidelityIssue` via the `SwiftExportOptions::
fidelity_report` sink (same sink the JS path uses): `swiftui-grid`,
`swiftui-flex-wrap`, `swiftui-flex-justify`, `swiftui-align-stretch`,
`swiftui-absolute-position` (approximated with `.offset` from the natural
position — CSS anchors top-left, SwiftUI has no flow-relative absolute layout),
`swiftui-transform` (skew/matrix/3D dropped), `swiftui-per-side-border`,
`swiftui-multi-shadow`, `swiftui-inset-shadow`. **Severity matters for
`--strict-fidelity`**: a finding that genuinely renders wrong (per-side
border/colour collapse, dropped shadow layer, inset shadow, absolute/grid/wrap/
skew) is non-informational and gates; a faithful-enough approximation
(Spacer-distributed justify, uneven-corner clamp, dropped gradient stop
positions) is `informational` and does not. The CLI's `--emit swiftui` branch
prints these as `fidelity:` lines and exits 4 under `--strict-fidelity`.
Gotchas the swiftc gate can't catch (so they have dedicated unit tests):
`fn_args` must match on an identifier boundary (`repeating-linear-gradient` must
NOT match `linear-gradient`); gradient stop-colour extraction must respect
parens so `rgba(0, 0, 0, .5)` isn't truncated at its internal space;
`parse_rgb_color` must reject partial numeric parses (`1px`) via the `std::stod`
consumed-index check.
- Persistent defaults live in `~/.pulp/config.toml` as `import_design.default_mode = "live|baked"` and `import_design.default_emit = "js|ir-json|cpp|swiftui"`, set through `pulp config set import_design.default_mode ...` and `pulp config set import_design.default_emit ...`. `PULP_IMPORT_DESIGN_DEFAULT_MODE` and `PULP_IMPORT_DESIGN_DEFAULT_EMIT` override config for one environment/session, and direct CLI flags override the matching preference. If only `default_mode=baked` is set, `ir-json` is implied.
- The standalone import helper and MCP status helper each have a small config reader for these defaults; keep them compatible with TOML single-quoted and double-quoted strings, matching the main CLI config reader.
- Mental model: live/runtime import means "run the original app"; baked DesignIR means "save the materialized UI tree"; baked C++ means "compile that saved tree into native code". You can move live iteration -> baked IR -> baked C++; you cannot reconstruct live React from baked IR because hooks, closures, loops, and arbitrary JS logic were not preserved.
- JSX baked snapshots accept both DOM-walked bundles and live/native bundles. Native bundles freeze through the `WidgetBridge` tree and record `snapshotSource=native-view`; generated baked C++ still constructs direct `View`/`Label` trees and should only require `pulp::view-core`.
- `--snapshot-semantics fail|warn|accept` is honored for JSX baked IR snapshots. `fail` rejects dynamic APIs by default, `warn` proceeds with a structured diagnostic, and `accept` proceeds silently. The scan covers timers, animation frames, clock/random APIs, and fetch while ignoring comments and string literals.
- URL imports fetch through argv-safe `curl` into a unique temporary file; literal `--file` paths are read directly and may contain normal filesystem punctuation, while `--url` rejects shell metacharacters before fetching.

Use `--dry-run` to preview without writing files.

### Token export formats (`--format`)

`--format` is the **token-format axis** (which token file to emit), distinct from
the `--emit` **artifact-kind axis** (js / ir-json / cpp). Values:

- `w3c` (default) — W3C DTCG `tokens.json`. The fidelity-first canonical form.
- `css-variables` — CSS custom properties (`export_css_variables`, `core/view/src/design_tokens.cpp`).
  Base tokens → `:root`; `.dark`-suffixed multi-mode tokens → `@media (prefers-color-scheme: dark)`.
  Names map `.`→`-` (`color.bg` → `--color-bg`); colors→hex, dims→`px`, strings verbatim.
  The sidecar default flips from `tokens.json` to **`theme.css`** when `--tokens` is unset.
- `tailwind` / `json-tailwind` / `css-tailwind` — Tailwind v3 JSON / v4 `@theme` CSS. **Still
  gated to `--from designmd`** (they re-parse DESIGN.md for section context). Generalizing these
  to any source is Workstream A2 (`planning/2026-06-02-design-token-export-and-swiftui-path.md`),
  not yet landed.

An unknown `--format` value is a hard error (exit 2), never a silent W3C fallback.
`css-variables` is an **external themeable artifact** — Pulp resolves `var(--x)`, but a runtime
loader that applies a themed `@media` CSS file is a separate, later step; the exporter does not
claim runtime consumption, and deliberately emits only `@media` (no `[data-theme]` selector).

```bash
pulp import-design --from figma --file design.json --format css-variables --tokens theme.css
pulp export-tokens --format css-variables                 # built-in dark theme → theme.css
```

Detect-only directory inputs (`pulp import-design --detect-only --directory <dir>`) prefer
`code.html`, then `index.html`, then the first sorted `.html` / `.htm` payload. Keep fixture
tests on that deterministic order; raw `std::filesystem::directory_iterator` order differs
between macOS and Linux.

## Bridge Handler Scaffold (Claude Design only)

For `--from claude`, the CLI emits a starter C++ file demonstrating how to wire `pulp::view::EditorBridge` so the imported design's editor JS can `postMessage` into the C++ processor:

- Replace the `MyPluginEditor` placeholder with the editor class that owns the `WebViewPanel`.
- Register one `bridge_.add_handler("type", ...)` per message type your editor emits. Use `EditorBridge::get_float / get_uint / get_string` for safe payload reads, and `EditorBridge::ok_response() / ok_response(extras) / err_response(msg)` for replies.
- Call `bridge_.attach_webview(*panel_)` to route WebView messages through the dispatcher.
- For the native-JS-runtime path, swap `attach_webview(...)` for `bridge_.attach_native_runtime(runtime, "<handler_name>")` once the runtime exposes its postMessage primitive.

See `docs/reference/editor-bridge.md` for the full API and the standard envelope-level error vocabulary (`malformed_json`, `unknown_type`, `missing_field`, `wrong_type`, `internal_error`).

## Classnames Artifact (Claude Design only)

For `--from claude`, the CLI also emits `classnames.json` mapping
`classname → { cssProp(camelCase): cssValue, ... }` for every plain-classname `<style>` rule it finds in the export. Mirrors the output of Spectr's `tools/extract-html-bundle/extract.mjs` so downstream consumers (`@pulp/css-adapt`, `dom-adapter`) can merge class-based styles into inline before forwarding to bridge calls — no separate Node-side extraction script needed.

What the extractor honours:

- Bundler envelopes — when `<script type="__bundler/template">` is present, the extractor walks both the loader shell *and* the unwrapped template HTML's `<style>` blocks.
- Multiple `<style>` blocks cascade: later blocks override earlier ones per-property; unrelated declarations from earlier blocks are preserved.
- Comma-separated selector lists (`.btn-primary, .btn-secondary { ... }`) emit one entry per classname with identical declarations.
- Hyphenated CSS properties are camelCased (`font-family` → `fontFamily`).

What it skips:

- `:root`, `.scheme-*` rules — those are theme-mode token overrides handled upstream as `tokens.json` artifacts.
- Pseudo-classes (`.foo:hover`), attribute selectors (`.foo[data-x]`), descendant combinators (`.foo .bar`, `.foo > .bar`) — anything that isn't a plain `.classname { ... }` rule.
- `@media` / `@keyframes` / other at-rule wrappers.
- `<style>` blocks whose first 200 chars contain `@font-face` (those carry only font-face rules, no classnames).

This skill must stay aligned with the `view-bridge` skill — `view-bridge` covers editor lifecycle (create_view, open/notify_attached/resize/close), this skill covers message dispatch over that lifecycle.

## Versioned Detection

`pulp import-design` ships a three-layer version model so the CLI surface stays stable as external tools evolve their export formats:

- **`parser-version`** — Pulp's parser implementation for a given source.
- **`format-version`** — the export shape Pulp recognises.
- **`compat-schema-version`** — the schema of `compat.json` itself.

The matrix is declared in [`compat.json`](../../../compat.json) and consumed by `pulp import-design --detect-only`. See [`docs/reference/imports/index.md`](../../../docs/reference/imports/index.md) for the full vocabulary, recognized matrix, and "add a new format-version" workflow.

### Detect-only flow

When the user hands you an unknown export, run detection first before guessing the source:

```bash
# File or directory; --detect-only prints (source, format-version,
# parser-version, match-count, confidence) and exits.
pulp import-design --detect-only --file <path>
pulp import-design --detect-only --directory <path>
```

Exit codes: `0` = match, `1` = usage error, `2` = no match.

If confidence is below 80%, the CLI emits a warning and an invitation to run `--report-new-format`:

```bash
pulp import-design --file <path> --report-new-format > stitch-2026-XX.json
```

`--report-new-format` emits JSON directly from detection strings. Keep every
source/version/token field JSON-escaped when touching
`tools/import-design/import_detect.cpp`; the regression test is
`render_new_format_json escapes generated string fields` in
`pulp-test-cli-import-detect`.

Hand-edit the resulting JSON into a new entry under `compat.json[imports/<source>/detected-formats]`. The `notes` field is mandatory — describe the upstream change in one line.
If no known source/version is close enough to detect, `candidate-source` and
`candidate-format-version` are emitted as empty strings; fill both manually
instead of treating them as detector evidence.

### Adding a fixture

Every new format-version needs a fixture so the detection gate covers it:

1. `mkdir -p test/fixtures/imports/<source>/<format-version>/`
2. Drop in the smallest representative export that triggers every fingerprint clause (synthetic is fine — clauses are content-addressed, not byte-addressed).
3. Add an `expected.json` sidecar with the assertion shape from existing fixtures (`source`, `format-version`, `parser-version`, `matched-clauses`, `total-clauses`, `min-confidence-pct`, `fingerprint-kinds`).
4. Run `ctest --test-dir build -R pulp-test-cli-import-detect` to confirm the fixture loop picks up the new row.

The detector module lives at `tools/import-design/import_detect.{hpp,cpp}` and is intentionally free of `pulp::view` / `pulp::state` link deps so the test target compiles fast and the unit tests don't drag the full design-import pipeline along.

## Canvas2D Bridge Gotchas (importer + shim authors MUST follow)

When translating browser `<canvas>` + Canvas2D code to Pulp's native bridge (`canvas*` globals), several spec-conforming browser idioms silently break against the bridge contract because the bridge surface is more limited and more direct than the HTML5 spec. These rules came from production debugging cycles on Spectr's analyzer port and its `canvas2d-shim.ts`; the importer must emit code that respects them.

### 1. `ctx.arc()` does NOT add to a path on its own — synthesize as line segments

**Spec:** `ctx.arc()` adds an arc sub-path; subsequent `ctx.fill()` / `ctx.stroke()` operate on it.

**Bridge reality:** `canvasArc(id, x, y, r, sa, ea, fillColor)` strokes immediately and returns. It does not contribute to the active path, so `ctx.beginPath() → ctx.arc() → ctx.fill()` renders a stroked outline ring (just the arc's stroke), not a filled circle. Radial-gradient cap-emission patterns degenerate into hollow ellipse outlines.

**Importer rule:** when emitting `arc()` translation, emit a polyline approximation (~32 segments scaled by radius, 8..64) via `canvasLineTo`. This makes the sub-path closeable and `ctx.fill()` honors any active `fillStyle` / gradient.

```ts
arc(x, y, r, sa, ea, ccw) {
    const segs = Math.max(8, Math.min(64, Math.ceil(r * 1.2)));
    const sweep = ccw ? -((sa - ea + 2*Math.PI) % (2*Math.PI)) : ((ea - sa + 2*Math.PI) % (2*Math.PI));
    for (let i = 0; i <= segs; i++) {
        const a = sa + sweep * (i / segs);
        canvasLineTo(id, x + Math.cos(a) * r, y + Math.sin(a) * r);
    }
}
```

Same applies to `arcTo()` (rounded-rect corners), `ellipse()`, and `roundRect()`.

### 2. Gradient stops are individual `(color, pos)` args — NOT a JSON string

**Spec:** `grad.addColorStop(pos, color)` accumulates stops; the gradient is opaquely passed via `ctx.fillStyle = grad`.

**Bridge reality:** `canvasSetLinearGradient(id, x0, y0, x1, y1, color1, pos1, color2, pos2, ...)` and `canvasSetRadialGradient(id, cx, cy, radius, color1, pos1, ...)` read each pair via positional `args.get<>()`. Passing stops as a single JSON string makes `i+1 < args.numArgs` false on the first iteration → zero stops parsed → bridge dispatch skipped → `fillStyle = grad` falls through to `canvasSetFillColor(id, "[object Object]")` → parseColor returns default white → uniform white fill instead of the rainbow ramp.

**Importer rule:** when serializing gradient stops, spread as variadic args:

```ts
const stopArgs: (string|number)[] = [];
for (const s of grad.stops) stopArgs.push(s.color, s.offset);
canvasSetLinearGradient(id, x0, y0, x1, y1, ...stopArgs);
```

### 3. Radial gradient: bridge takes single circle (cx, cy, R), not two-circle (x0,y0,r0,x1,y1,r1)

**Spec:** `createRadialGradient(x0,y0,r0,x1,y1,r1)` — two circles, with the gradient interpolating in the cone between them.

**Bridge reality:** `canvasSetRadialGradient(id, cx, cy, radius, ...stops)` only takes the single outer circle. Inner-circle / off-axis ring patterns degrade.

**Importer rule:** map JS 6-numeric form to bridge's 3-numeric form using the OUTER circle (`x1, y1, r1`). For Spectr-style center-bloom (`r0=0`, same center) this is visually identical to the spec; for true two-point gradients, file a Pulp issue rather than emitting silently-wrong output.

### 4. `ctx.clearRect()` was a parent-surface eraser pre-#1372

**Reality (pre-pulp v0.74.1):** `ctx.clearRect()` used `SkBlendMode::kClear` / `CGContextClearRect` directly on the parent surface — NOT on a per-canvas backing layer. JS code that clears its own canvas would erase pixels another `<canvas>` sibling had just painted. Symptom: first sibling renders correctly, gets wiped by second sibling's clearRect at the start of its frame.

**Now (pulp v0.74.1+):** each `CanvasWidget::paint` is wrapped in `save_layer`, isolating clearRect / Porter-Duff to that canvas's own buffer. Importer can emit `<canvas>` siblings without worrying about cross-erasure. Pin SDK `>= 0.74.1`.

### 5. Other Canvas2D methods missing from the bridge

- `ctx.measureText()` — bridge has `canvasMeasureText` but the shim should fall back to a per-char approximation (~6.5px for 10px monospace, ~px*0.6 for proportional) when not available
- `ctx.strokeText()` — bridge has no stroke path; fall back to `canvasFillText` and accept the visual gap
- `ctx.createPattern()` — not implemented; emit a solid-color fallback from the pattern's first stop
- `ctx.createConicGradient()` — bridge has no `canvasSetConicGradient` registration even though `SkiaCanvas::set_fill_gradient_conic` exists; either file a Pulp follow-up or fall back to a flat solid

### 6. SDK version requirements for canvas2d parity

| Capability | Min SDK |
|------------|---------|
| `canvasSetLinearGradient` / `canvasSetRadialGradient` | v0.72.4 |
| Gradient stops actually applied to fills | v0.72.5 |
| `set_blend_mode` on Skia (GPU) honored | already wired; CG/CPU is silent no-op |
| Per-canvas `save_layer` isolation (no sibling clearRect erase) | v0.74.1 |
| Canvas paint instrumentation (`PULP_LOG_CANVAS_PAINT=1`) | v0.75.0 |

Reject importer output that targets earlier SDK versions for canvas-heavy designs — the visual gaps will be silent and look like Pulp bugs.

### 7. Validation discipline

Always pixel-sample after rendering — visual inspection misses uniform-fallback bugs. A spectrum that renders "uniform light gray" instead of "rainbow gradient" looks roughly right at thumbnail scale but is structurally broken (every color stop resolved to white by the parseColor fallback). Sample horizontal cross-sections at the expected gradient axis and assert color variance > some threshold.

### 8. Pointer events need explicit `registerPointer(id)` AND don't bubble

**Spec:** `addEventListener('pointerdown', fn)` plus React synthetic-event bubbling: a click on a child reaches the parent's handler unless `stopPropagation` is called.

**Bridge reality:** Pulp gates pointer dispatch behind an explicit `registerPointer(id)` call (parallel to `registerClick(id)` and `registerHover(id)`). `@pulp/react`'s prop-applier currently only wires `registerHover` for `mouseenter/leave`, so `onPointerDown/Move/Up` listeners are installed in the JS dispatch table but never fired by the native View — the JS handler appears registered (`on(id, 'pointerdown', fn)`) yet clicks never invoke it. Additionally, **pulp dispatches pointer events to the hit-test target only — there is no synthetic-event bubbling.** A handler on a parent `<div>` will not fire when the click lands on a child `<canvas>` that visually overlays it.

This was the root cause of Spectr's "FilterBank renders rainbow but band drag is dead" symptom. Confirmed by `__spectrLog` probe at the top of `onPointerDown`: handler does NOT fire on `cliclick c:600,400` even though `on(pr_3, pointerdown, ...)` is registered.

**Importer rule:**

1. Whenever the importer emits an `onPointerDown / onPointerMove / onPointerUp / onPointerLeave / onWheel` handler, also emit a `registerPointer(id)` call against the same widget. Do this in the ref-mount callback (or its equivalent post-mount hook) so the bridge wires `on_pointer_event` into the View. Idempotent on the bridge side; safe to call on every remount.
2. **Do not assume bubbling.** If the design has a parent element with a pointer handler and child elements that visually cover it, mirror the same handler onto each direct child too. The handler can use the parent's `getBoundingClientRect()` for coord math so the same function works on every binding.

```ts
// Bind on parent + every interactive child:
<wrap onPointerDown={onPD} onPointerMove={onPM} onPointerUp={onPU}>
  <canvas onPointerDown={onPD} onPointerMove={onPM} onPointerUp={onPU} ... />
  <canvas onPointerDown={onPD} onPointerMove={onPM} onPointerUp={onPU} ... />
</wrap>
```

```ts
// In the ref-mount callback:
const id = inst.id;
if (typeof globalThis.registerPointer === 'function') globalThis.registerPointer(id);
```

The cleaner long-term fix is for `@pulp/react`'s prop-applier to call `registerPointer` automatically when it sees any pointer-event prop (parallel to its existing `registerHover` wiring) — track that as a follow-up Pulp issue rather than an importer-side workaround if you encounter it on a fresh import.

### 9. Shared widget promotion — `<div onClick>` → button

`pulp::view::promote_interactive_frames` walks the IR once during shared parse
normalization and re-types any `type == "frame"` carrying an interactive signal
to `type == "button"`. The CLI no longer owns a separate tool-local promotion
pass; `parse_design_ir_json()` and source adapters return the normalized form
for both library and CLI consumers. Adapters promote before assigning stable
anchors so content-hash anchors reflect the normalized type. Signal priority
(highest -> lowest):

1. `attributes["onclick"]` / `attributes["onClick"]` — strongest.
2. `attributes["role"] == "button"` — explicit ARIA semantic.
3. `style.cursor == "pointer"` — weakest; opt-out via `role="presentation"`.

Conservative on purpose: only frames are promoted; already-typed widgets
(`input`, `image`, `button`) are left alone, so a designer who wrote
`<input onClick={...}>` keeps the input.

**Gotcha**: the pass is source-agnostic, but it can only promote signals that
the adapter preserves in `IRNode::attributes` or `IRStyle::cursor`. Runtime
imports populate HTML attributes from the live DOM after React mount. Static
adapters that do not preserve `onclick` / `role` still cannot promote those
signals until their parser surfaces them.

When you re-import Spectr's `editor.html` via Claude Design + the runtime path, expect:

```
Promoted N interactive frame(s) to button widgets.
```

in the stdout summary. If you see `0 widgets` and no promotion line on a fixture you *know* contains `<div onClick>`, you're either on a non-runtime parser path or the React tree didn't mount during the harness eval and `attributes` is empty as a result.

## Live-host pump contract

Every live-host main loop that drives a `WidgetBridge` for a runtime-imported app **must call BOTH halves of the idle pump on every tick**:

```cpp
bridge->poll_async_results();      // async-exec results + queued frame callbacks
bridge->service_frame_callbacks(); // setTimeout / setInterval drain via __flushTimers__
```

`scripted_ui.cpp:67-81` documents the contract; `examples/design-tool/main.cpp` wires it through a GCD timer; `examples/ui-preview` does the equivalent. Skipping `service_frame_callbacks()` is a silent foot-gun — `setInterval(fn, N)` returns a valid id but `fn` never fires, so any imported app's polling-state-update path freezes. The chart/canvas (which paints from a separate ref-based store) keeps updating, but every React label that reads polled state stays at its initial value forever. Confirmed regression in design-tool when only the first half ran (Spectr's bands trigger frozen at "32" and zoom indicator frozen at "1.00×" — both drive their text from a 150ms `setInterval`).

Two safety nets enforce the contract going forward:

- **CI lint** at `tools/scripts/host_pump_lint.py` greps every host main.cpp listed in `HOST_FILES` for any `poll_async_results()` call not paired with `service_frame_callbacks()` within the same handler block. Fails CI on violation. Single-line bypass: append `// host-pump-lint: skip — <reason>` for genuine one-shot CLI tools.
- **Runtime smoke** at `tools/import-validation/live-host-pump-smoke.sh` launches each live-host binary against a tiny script that schedules `setInterval(fire, 50)`, runs for 2s, and asserts ≥5 fires landed. Generic — adding a new live host = one row in the `HOSTS=()` table; everything else is reused.

When adding a new live host (e.g. a future `examples/<thing>` binary), update **both**: append the new host to `HOST_FILES` in `host_pump_lint.py` AND to `HOSTS` in `live-host-pump-smoke.sh`. The lint catches source-level regressions at PR time; the smoke catches runtime breakage where the source pairing is correct but the run loop is misconfigured.

For unobtrusive smoke / CI runs, the design-tool exposes `--no-show-window` (uses `WindowOptions.initially_hidden` to skip Dock icon + window display while keeping the full bridge run loop active) and `--exit-after-ms <N>` (clean `request_close()` after N ms). Both flags compose, so `pulp-design-tool --script <probe.js> --no-show-window --exit-after-ms 2000` runs the full live-host code path with no GUI flash.

## Keyboard shortcut V2 wire-up

The import path detects `keydown`/`keyup` global-shortcut handlers in React-style source and emits two pieces of generated code in the host JS bundle:

1. **`registerShortcut(...)` calls** that bind each detected keycode + modifier combo to a synthetic-keydown re-dispatch handler. The native side intercepts the bare key (no DOM focus) and routes through this registration.
2. **Synthetic keydown re-dispatch** that builds a `KeyboardEvent`-shaped object with the right `key`, `code`, `keyCode`, and modifier flags (`ctrlKey`, `metaKey`, `shiftKey`, `altKey`), then dispatches it to the document so the original React handler's `if (e.ctrlKey && e.key === 's')` branch fires.

Two correctness gotchas the codegen MUST respect — both surfaced by real handler shapes:

- **`metaKey` and `ctrlKey` are separate axes — don't collapse**. The collector emits BOTH `"meta"` and `"ctrl"` when the source has the cross-platform `e.metaKey || e.ctrlKey` idiom, and emits separate `registerShortcut(...)` bindings per platform half. The synthetic event sets `ctrlKey`/`metaKey` according to which mask bits are present — a Ctrl-only source handler (`e.ctrlKey && e.key === 's'` on Win/Linux) gets a `ctrlKey: true, metaKey: false` synthetic event, not the flat "always Cmd" form.
- **`KeyboardEvent.code` letter/digit forms decode before keycode emission**. The extractor captures both `event.key` and `event.code`. `KeyS`, `KeyA`, …, `Digit1`, …, `Digit9` must be stripped to the letter/digit form before the keycode lookup, otherwise the table-miss returns `0` and `generate_pulp_js` drops the entire `registerShortcut(...)` line silently.

If you add a new shortcut shape detector to the extractor, mirror it in:
- The keycode table in `core/view/src/design_import.cpp::keycode_for(...)` (or its equivalent today)
- The modifier collector that walks the surrounding boolean expression
- The synthetic-event emitter that produces the `KeyboardEvent`-shaped JS object

Test coverage lives in `test/test_design_import.cpp` (E2E roundtrip — codegen → WidgetBridge → React-style handler). The roundtrip exercises both the registerShortcut emission and the synthetic-keydown re-dispatch; failing either half is a hard test failure.

## Default shortcuts (source-matched)

On top of the V2 extractor, the import pass auto-binds platform-convention chords (`Cmd+,` Settings, `Cmd+?` Help, bare `?` cheatsheet, `Cmd+N/O/S/F`, plus Win/Linux `Ctrl`/`F1` variants) when the dev's React source has a recognizable component. Lives in `core/view/src/design_import.cpp::detect_default_shortcuts(...)` + `apply_default_shortcuts(...)`. Accepted defaults are lowered into `DetectedShortcut` form and ride the V2 codegen path with no fork.

Hard rule (encoded): **a wrong auto-binding is worse than no binding**. Detector requires ≥2 signals to fire and emits a `collision` entry (no bind) when multiple candidates compete.

Signals scored per (component, pattern):
1. Component name keyword match (`SettingsModal`, `HelpPanel`, etc.)
2. `role="dialog"` / `role="alertdialog"` / `role="menu"` / `role="listbox"` in body
3. `aria-label="..."` text in body containing pattern keyword
4. `<h1>`/`<h2>`/`<h3>` heading text matching pattern keyword
5. `<kbd>` tag presence (cheatsheet disambiguator — required for cheatsheet match)
6. **Canonical-name bonus**: exact `<Pattern>{Modal,Dialog,Panel,Popover,Sheet,Window,Drawer}` shape counts as a second signal on its own. Real apps (Spectr's `SettingsModal`, `HelpPopover`) use inline-styled divs without ARIA, so the strict ≥2 ARIA-shape gate would skip every one of them. Generic non-canonical names (`SettingsList`) still require a real second signal.

Body-window extraction stops at the next top-level component declaration — otherwise sibling components bleed into each other (a cheatsheet `<kbd>` in `ShortcutsModal` would wrongly count as a signal for an adjacent `SettingsModal` defined right above it).

Cross-platform emission: the CLI runs at import time, but the generated `ui.js` ships to multiple platforms. The import driver emits BOTH macOS and Win/Linux variants for any default with a platform delta (Help: `Cmd+?` + `F1`; Settings: `Cmd+,` + `Ctrl+,`). At runtime only the chord matching the physical key fires its `registerShortcut` entry (exact-mask match on the bridge side).

JS-literal escape: any key string interpolated into `key: '...'` MUST be backslash- and quote-escaped — `key_string_to_keycode` accepts all printable ASCII (incl. `'` and `\`), so a source with `e.key === "'"` would otherwise produce syntactically-invalid JS. The `emit_binding` lambda escapes at emission time.

CLI surface:
- `--no-default-shortcuts` opts out (default ON)
- `<name>.defaults.json` diagnostic written alongside `shortcuts.json` showing accepted candidates + collisions

What's NOT in Phase A: Pulp-framework defaults for the built-in `SettingsPanel` Audio/MIDI sub-tabs (`Cmd+Opt+A` / `Cmd+Opt+M`). Phase B follow-up — needs `TabPanel` select-tab JS API + standalone-only emission gate. Spec: `planning/2026-05-16-default-keyboard-shortcuts.md`.

## Gradient paints — the transform is inverted, and only linear survives

`scene.mjs` lowers a Figma `GRADIENT_LINEAR` paint to a CSS
`linear-gradient(...)` string that `SvgPathWidget` / `setBackgroundGradient`
paints. Four things about that pipeline are non-obvious, and three of them fail
SILENTLY — the render just looks subtly wrong rather than erroring.

- **Figma's paint `transform` maps the node's box INTO gradient space.** The
  ramp runs (0,0)→(1,0) in gradient space, so the axis you want is the
  **inverse** image of those points. Using the matrix forward renders every
  gradient **180° flipped** — highlights light from below — and nothing warns
  you. `gradientPaintToCss` inverts it; the guard is the `fig.test.mjs` case
  "the paint transform is inverted, so a top→bottom ramp is not flipped".
- **Scale the axis into PIXEL space (×w, ×h) before taking its angle.** A
  normalized-space `atan2` is wrong on any non-square box.
- **Only linear is expressible.** `parse_svg_linear_gradient`
  (`svg_path_widget.cpp`) matches on the literal `linear-gradient(`, so RADIAL /
  ANGULAR / DIAMOND have no lowering even though `Canvas` itself has
  `set_fill_gradient_radial`. They keep flattening to the mean stop colour and
  keep their `gradient-approximated` diagnostic. Emitting a radial as a linear
  would silently paint the wrong gradient — worse than an honest approximation.
- **The widget's gradient line is the box's HALF-DIAGONAL, not the CSS
  gradient-line length**, and Figma's axis may start/end outside the box. So an
  angle + the raw stops is still wrong on the ramp's extent; the stops are
  resampled onto the widget's own axis.

`setSvgFill` and `setSvgFillGradient` do **not** race: the widget prefers the
gradient and falls back to `fill_color_` only when the string won't parse, so
carrying both is a safety net and emission order cannot decide the paint.

Whatever paint moves onto a synthesized path must be **cleared off the node's
own background** (`design_import.cpp`), or the frame paints a gradient/solid
RECTANGLE behind the circle — the stray box behind a knob.

**Counting `gradient-approximated` is not a fidelity metric.** The warnings are
per-INSTANCE, so a handful of distinct paints in a symbol master reports as
dozens (SmallTriaz2: 80 warnings from 9 distinct paints). Judge the fix by the
distinct nodes and the pixels, not the count.

## Figma-plugin lane — failure modes + fixes (2026-05)

Hard-won lessons from porting the ELYSIUM synthesizer Figma file end-to-end. Every item below was a SUBTLE visual bug that took a user callout to catch the first time. The fixes are all generalizable importer rules, NOT design-specific patches.

### What "import quality" actually means

`pulp import-design` lands a Figma → Pulp visual at three quality tiers:

1. **Renders, structurally faithful** (90% threshold) — frames, layout, text content, asset placements line up. Bottom row content not overlapping top row. ~A day of work on a new file.
2. **Pixel-honest** (95%) — every Figma effect that has a representation in the IR also paints (shadows, borders, gradients, rounded corners, sub-region tints).
3. **Designer-perceived parity** (99%) — chrome look, optical centering, kerning compensation, shadow extents, indicator notches all match what the designer drew.

Every fix below moved us up one tier. Each was a SINGLE-LINE FIGMA DATUM that was being dropped or mis-interpreted in the chain.

### Failure-mode catalogue

Each entry: *symptom → diagnostic step → root cause → fix*. Use this list when reviewing a new design import for the FIRST time — sample each known failure mode before claiming "done".

#### 1. `setCornerRadius('All', N)` silently dropped

- **Symptom**: every Figma frame with a single uniform `border-radius` paints with sharp corners.
- **Diagnostic**: pixel-sample a known-radius corner; if the transition from background to fill happens in one pixel, the radius isn't reaching the View.
- **Root cause**: the codegen emits `'All'` as the uniform-radius identifier, but the bridge only handled `'TopLeft'/'TopRight'/'BottomLeft'/'BottomRight'`.
- **Fix**: bridge accepts `'All'` and routes to `View::set_border_radius` (commit `00ea36202`).
- **Lesson**: when adding a new keyword to codegen, grep the BRIDGE for the dispatch table that consumes it. Don't assume "the bridge handles all keywords the codegen emits."

#### 2. Box-shadow CSS string passed verbatim to bridge that expects parsed args

- **Symptom**: every panel drops a generic, off-spec shadow regardless of the Figma values.
- **Diagnostic**: greppr the generated JS for `setBoxShadow(id, '0px Npx Mpx Kpx #...')` — if it's a string instead of numeric args, the bridge falls back to defaults.
- **Root cause**: bridge signature is `setBoxShadow(id, ox, oy, blur, spread, color, inset?)` — six args, not one CSS string.
- **Fix**: codegen parses `<ox> <oy> <blur> [<spread>] <color>` into args (commit `bf5e2d621`).
- **Lesson**: any time the IR stores a CSS-spec string (box-shadow, transform, filter), check whether the bridge wants the original string OR parsed components. Document the expectation alongside the bridge `register_function`.

#### 3. Flat-fallback gradient stored in `background_gradient` field

- **Symptom**: sub-region tints inside cells (slightly lighter or darker rectangles within a larger frame) never paint.
- **Diagnostic**: walk the IR for any node with `background_gradient: "#xxxxxx"` (a bare hex, not a `linear-gradient(...)` string).
- **Root cause**: `extract.ts` falls back to a flat first-stop colour when Figma reports `GRADIENT_RADIAL` / `GRADIENT_ANGULAR` / `GRADIENT_DIAMOND` (we don't support those). The fallback stored hex in the gradient field, but the codegen's `setBackgroundGradient` path requires a `linear-gradient(...)` string and silently failed.
- **Fix**: `extract.ts` now stores fallback in `background_color`; `parse_ir_style` demotes any `background_gradient` that's missing the `gradient(` substring (commit `91a67ac31`).
- **Lesson**: extractor fallbacks must produce data that survives the codegen's parser. If a fallback writes to a field that gets routed through a stricter parser downstream, the value never paints.

#### 4. Frame strokes dropped by codegen

- **Symptom**: vertical / horizontal separator lines between Figma columns (encoded as `border: 1px solid rgba(...)` on flex frames) don't appear.
- **Diagnostic**: grep the IR for nodes with `border_color` + `border_width > 0` — if their generated JS lacks `setBorder(...)`, the codegen branch never emitted it.
- **Root cause**: codegen had no `setBorder` emission for the container path.
- **Fix**: emit `setBorder(id, color, width, radius)` whenever `node.style.border_color` + `border_width > 0` exist (commit `80a3472f1`). Bridge's `parseColor` already accepts `rgba(...)`.
- **Lesson**: codegen branches must emit EVERY visual style the IR carries. When adding a new style field to `IRStyle`, add a codegen test case that asserts the bridge call lands.

#### 5. Zero-width-axis vectors invisible (Figma 1px lines)

- **Symptom**: thin separator lines and chart grid lines vanish.
- **Diagnostic**: any IR node with `type: "image"` and `width ≈ 0` (e.g. 5e-06) or `height ≈ 0`, plus a non-trivial `border_width`.
- **Root cause**: Figma stores 1px strokes as VECTOR nodes with one degenerate axis + a stroke. The captured PNG is a 1-pixel-wide image; downstream renderer paints essentially nothing.
- **Fix**: the "stroke promotion" (separator promotion) rule in `normalize_design_ir` (`design_ir_normalize.cpp`, called from `parse_ir_node`) — when an axis is < 0.5 and `border_width >= 0.5`, snap the axis to `max(stroke_weight, 1)` and demote `type: image` → `frame` with `background_color = border_color` (commits `f4ea1d067`, `496b738b8`).
- **Lesson**: don't trust Figma's bounding-box width/height for stroke geometry. Always re-derive from the stroke weight.
- **Threshold detail**: Figma "1px" strokes often land as 0.97px due to fractional raster alignment. Use 0.5 as the floor, not 1.0.

#### 6. Child fills don't inherit rounded parent's clip

- **Symptom**: a gradient panel that sits inside a rounded-corner parent paints with SHARP corners (the gradient rect is rectangular even though its container is rounded).
- **Diagnostic**: pair-grep for a frame with `border-radius > 0` and a child positioned at (0,0) matching the parent's size — if the child has `border-radius: 0`, it'll paint rectangular.
- **Root cause**: Figma relies on the parent's `overflow: clip` + radius to round the children visually. Pulp's renderer doesn't clip children to the parent's border-radius.
- **Fix**: in `normalize_design_ir` (`design_ir_normalize.cpp`, run by `parse_ir_node` after parsing children), propagate the parent's `border_radius` to any child that fills the parent at origin (commit `bf5e2d621`).
- **Lesson**: any time Figma uses a CSS spec that relies on PARENT geometry (overflow:clip, position:sticky, background-attachment:fixed), our IR has to either reproduce the clip or pre-bake it onto the child.

#### 7. Shadow-zone sibling overlaps swallow the shadow

- **Symptom**: a panel with a downward drop shadow looks like a hard-edge transition into the sibling below — no visible fade.
- **Diagnostic**: sample a vertical pixel strip from panel-bottom past the next sibling's top; if there's no transitional gradient, the shadow is being painted then covered.
- **Root cause**: Pulp draws box-shadow in the same z-layer as the View. A later sibling positioned at `panel.bottom + small_gap` paints over the shadow.
- **Fix**: the shadow-snap rule in `normalize_design_ir` (`design_ir_normalize.cpp`, run by `parse_ir_node` after children are parsed) detects `Fi` with a downward shadow + `Fi+1` sitting in the shadow zone; snaps `Fi+1` UP but leaves the shadow's `oy` worth of room so the shadow renders into the partial opening (commits `f4ea1d067`, `7d305ec2a`).
- **Lesson**: layout rules that "preserve visual continuity" need to model both the geometric box AND the effect extent. Box-shadow `oy + blur/2` is the canonical effect extent.

#### 8. Uppercase-transformed labels overflow their min-width

- **Symptom**: header text like "FILTER & EQ" overflows into the next sibling, producing "FILTER & EQHOLLOW PUNCH" with no space.
- **Diagnostic**: an uppercase label whose `style.width` (Figma's reported source-text width) is meaningfully less than the rendered uppercase glyph width.
- **Root cause**: Figma stores `Label.width` as the SOURCE-text rendered width but applies `text-transform: uppercase` at render time. Uppercase Latin runs ~15-20% wider.
- **Fix**: when emitting `min_width` for an uppercase-transformed label, multiply by 1.20 (commit `ae0d955ed`).
- **Lesson**: any IR width that was measured pre-transform must be inflated if a transform widens the glyphs. Same logic applies to `font-feature-settings`, `font-variant: small-caps`, `letter-spacing`.

#### 9. Cap-height vs math-center alignment

- **Symptom**: a colored-dot indicator next to a label glyph reads as "below the text", not centered with it. Visually the dot looks dropped.
- **Diagnostic**: pixel-sample the dot's center y and the text glyph optical center y; if they differ by ~font_size × 0.15, this is the bug.
- **Root cause**: Yoga's `align-items: center` aligns box-centers, but the line-box reserves descender space the uppercase glyphs don't use, so the GLYPH optical center sits ~font_size × 0.15 above the box-center. The dot, being math-centered, sits visually low.
- **Fix**: when a row has `align_items: center` + an uppercase text child + a small image child (`min(w,h) ≤ font_size`), emit `setFlex(image, 'margin_top', -round(font_size * 0.30))`. Factor of 0.30 = 2 × 0.15 because Yoga's flex centering shifts position by margin_top/2 (commit `53decc5e1`).
- **Lesson**: Yoga's `align-items` ignores baseline information for non-baseline children. When mixing icons with caps text, compensate at codegen.
- **Engine dependency**: Pulp's `yoga_layout.cpp` previously dropped negative margins (gated on `v > 0`). Fixed in the same commit by routing `Dimension::px` values through Yoga even when negative — CSS-spec compliance.

#### 10. Knob PNG natural-size vs layout box

- **Symptom**: a Figma silver-knob sprite renders squished to its layout box because the PNG has visible shadow bleed past the bounding box.
- **Diagnostic**: compare PNG pixel dims to `style.width × 2` (since plugin exports at 2× scale). If PNG is significantly larger, it has bleed.
- **Root cause**: PNG was being fit (via `draw_image_from_file_rect`) into the layout box, distorting aspect and shrinking the visible knob body.
- **Fix**: in `Knob::paint`, draw the PNG at its natural logical size (`pixel_size / 2`) centered on the layout box, allowing overflow. Generalize: when `pulp-import-design` resolves an asset that exceeds the layout box by ≥1.5× on either axis, emit `setObjectFit('none')` so `ImageView` honours natural pixel size (commits in the bf5e2d621 round + the asset_bleed flagging path).
- **Lesson**: PNG-encoded designs leak bleed past their bounding boxes. The fix isn't "force-fit", it's "honour natural size with overflow", because the bleed is the designer's intent.

#### 11. Negative margins silently dropped by Yoga wrapper

- **Symptom**: an emitted `setFlex(id, 'margin_top', -2)` has zero effect on layout.
- **Diagnostic**: trace the negative value through `yoga_layout.cpp::apply_margin` — the legacy float path gates on `v > 0`.
- **Root cause**: Pulp's wrapper over Yoga dropped negative margins. CSS spec supports them.
- **Fix**: route `Dimension::px` values through to Yoga even when negative (commit `53decc5e1`).
- **Lesson**: any time the importer emits a CSS-spec value that doesn't visually land, check the bridge wrapper. Pulp's wrappers over Yoga / Skia sometimes have legacy "only positive" / "only non-zero" gates that pre-date CSS-spec compliance.

#### 12. Shadow render-bounds vs bounding-bounds mismatch

- **Symptom**: a sibling that should butt against the panel bottom shows a visible 5px canvas-color gap.
- **Diagnostic**: walk absolute-positioned siblings within a parent; if `child.top - prev_sibling.bottom > 0` and the prev sibling has a downward shadow, this is the case.
- **Root cause**: Figma designs place the next sibling at `panel.bottom + 5px` expecting the shadow's blur (typically 18px) to fill that gap. Pulp's shadow ends precisely at the sibling, leaving canvas color showing.
- **Fix**: the shadow-snap rule (above, #7). Preserves the shadow's `oy` while closing the geometric gap.
- **Lesson**: visual "gaps" the designer drew often rely on effect extents the importer ignores. When designing layout rules, consider effect extents alongside geometric bounds.

#### 13. Connector line in flex row swallowed by first-item slot

- **Symptom**: a horizontal hairline used to communicate a DSP pipeline (`[SEND]——[1/4 DELAY]——[REVERB]——[+]`) renders as a short line on the left of the row, NOT visually connecting the boxes.
- **Diagnostic**: a flex ROW where the first child is a hairline (height ≤ 2px or width ≤ 2px) and subsequent siblings are widget-sized boxes — total flex content would overflow the row width if all participated.
- **Root cause**: Figma designs put the line as a FULL-ROW background BEHIND the boxes (z-order: first = behind). The dropdowns / buttons cover it, leaving visible segments BETWEEN them — which reads as "connection". Our flex layout sequenced the line as the first item, compressing it on the left.
- **Fix**: the connector-line rule in `normalize_design_ir` (`design_ir_normalize.cpp`, run by `parse_ir_node` after children are parsed) — when the row matches the pattern, mutate the line to `position: absolute`, `left: 0`, `width: row_width`, `top: (row_h - line_h) / 2`, centred vertically. Stays first in flex source order so the renderer still draws it behind subsequent siblings.
- **Lesson**: Figma's "I/O connection" / "pipeline" / "signal flow" visuals all use the same shape — a hairline as the FIRST flex child, boxes after. The importer needs to recognise it as a CONNECTOR, not as a sibling that should participate in flex sizing.

#### 14. Connector line extends past trailing "add" affordance

- **Symptom**: pattern #13 fixed the line threading, but the line continues past a trailing `+` / "add more" / settings-cog button, reading as "the + is part of the connection too".
- **Diagnostic**: row has ≥3 widget siblings AND the trailing sibling is significantly smaller than the others — a single-icon affordance vs medium-width dropdown boxes.
- **Root cause**: pattern #13 spans the line full-row-width by default. Designer intent is "pipeline ends at the LAST connected item", with the trailing affordance being a separate "add another" control.
- **Fix**: when `last_width / median_width < 0.6`, pull the line's right edge back by `trailing_width + gap` so the connection visual ends at the last real pipeline widget. ELYSIUM's FX RACK: line 226px → 190px so it stops at REVERB's right edge.
- **Lesson**: a trailing visually-smaller widget in a pipeline row is an affordance, not a stage. Same heuristic catches `[mode1][mode2][mode3][⚙]`, `[item1][item2][+]`, etc.

#### 15. Single-child `space_between` degenerates to left-align

- **Symptom**: a single piece of text/content in a flex container appears left-aligned even though the design clearly shows it centered (numbered tab buttons "1" "2" "3" "4" all sitting at the left edge of their boxes).
- **Diagnostic**: container has `justify_content: space_between` AND `children.size() == 1`. CSS / Yoga semantics: space-between with one child means "distribute remaining space between start and the only item" → item ends up at start.
- **Root cause**: common Figma designer pattern — they set the container to space-between meaning "spread items out when there are multiple", then drop a single item in. The intent is "center this solo item", but Figma serialises space-between literally regardless.
- **Fix**: in design_codegen, when emitting `justify_content`, if value is space_between AND there's exactly one child, emit `center` instead. Preserves designer intent uniformly.
- **Lesson**: Figma's auto-layout doesn't always reflect rendered intent on degenerate cases (single child, zero gap, infinite-size content). When the IR is the literal Figma value but renders wrong, look for these "single-N degenerates to default" cases.

### Subtleties you should catch BEFORE the user does

When importing a new Figma file, run these checks proactively:

1. **For every node with `background_gradient`**: assert it contains `gradient(` — bare hex / rgba values mean the extractor fell back from an unsupported gradient type and the codegen will swallow it. (Pattern #3.)
2. **For every uppercase-transformed label in a flex row**: confirm an adjacent image with `min(w,h) ≤ font_size` has a `margin_top: -N` emission. (Pattern #9.)
3. **For every frame with `border_radius > 0`**: confirm filling children inherit the radius. (Pattern #6.)
4. **For every frame with a downward `box_shadow`**: confirm the next absolute-positioned sibling sits within the shadow's effective bottom (`top + oy + blur/2`). (Pattern #7.)
5. **For every image node with `width < 0.5` or `height < 0.5`**: confirm `border_width >= 0.5` got promoted to a visible-axis rect. Greppr generated JS for `createImage` with degenerate `setFlex('width', tiny)` — that's a bug surface. (Pattern #5.)
6. **For every container with `setCornerRadius('All', N)`**: pixel-sample a corner; ensure the bridge actually applied the radius. (Pattern #1.)
7. **For every captured asset PNG**: compare pixel size to layout box. If ratio ≥ 1.5×, the asset has bleed and needs natural-size rendering (`setObjectFit('none')` for ImageView, sprite-strip natural-size for Knob).

### Tooling that should run on every Figma import

These three pieces, all checked in this branch, are the standard inner-loop for visual-fidelity work. Use them; don't eyeball.

1. `tools/scripts/figma_import_diff.py <ref.png> <render.png>` — side-by-side composite, pixel-diff heatmap, per-region delta scores. Top-K offending regions ranked by mean delta. Use after EVERY codegen change.
2. `tools/scripts/render-figma-import.sh <ui.js> <out.png>` — auto-reads the `<ui.js>.meta.json` sidecar (canvas size from root frame) and renders with the right `--width / --height`. No more remembering numbers.
3. `pulp-import-design ... --output <ui.js>` auto-emits `<ui.js>.meta.json` alongside. Has `{ canvas: { width, height }, source }`. Consume it; don't hardcode.

### Knob rendering — silver by default, sprite on opt-in

**Default for the figma-plugin lane: silver (native vector).** The native vector path is the durable answer for native UI rendering — crisp at any scale, no PNG bleed artefacts, no Skia Graphite raster→texture upload, works on CPU raster (`pulp-screenshot`) AND the GPU window. Knob captions ("VALUE") are synthesised when the original Figma component-instance had them baked into the PNG.

**Opt back into PNG sprites: `--knob-style=sprite`.** Use when the design depends on Figma's pixel-exact knob rendering — for example, a hero plugin whose marketing screenshots show specific chrome highlights, or a multi-frame rotational filmstrip the designer supplied. The cost is visible PNG bleed (shadow halos around the knob bottom edges that read as "brush stroke" bands across the gradient panel) and bigger file size. Accepted global values are `silver`, `default`, `standard`, `auto`, and `sprite`; unknown values exit 2 instead of silently falling back.

**Per-node override — Figma name suffix `@sprite` / `@silver`.** A node named `Knob/Hero@sprite` forces sprite for that one knob regardless of the global flag. `Knob/Send@silver` forces silver. Lets a designer cherry-pick a hero knob to be pixel-exact while everything else uses the crisper vector path. Convention chosen to match Figma's own `Knob/State=hover` variant syntax and Mitosis / Penpot's `@target` code-hint convention.

**Scope today (knob `@sprite`/`@silver` only)**: the per-node `@sprite` / `@silver` name-suffix convention is honoured only on Knob nodes (sprite-strip rendering is knob-only). Naming `Fader/Hero@sprite` won't break anything but won't have a visible effect. XYPad / Waveform / Spectrum sprite-strip support is still a follow-up.

### Fader + Meter hybrid skin — DERIVED, value-driven, default ON

Recognised **fader** and **meter** widgets are skinned to match the captured Figma appearance by default, while staying native + bound + value-driven. This generalises the knob's skinning idea but takes a different route than the knob sprite-strip, because a fader/meter PNG bakes the control AT its captured value — skinning with the flat image verbatim would FREEZE the thumb / fill.

- **How it works**: the import CLI's asset-resolution pass SAMPLES the captured PNG (via a minimal miniz-backed PNG→RGBA decoder in `pulp_import_design.cpp` — `AssetManager::decode_png` only stores raw bytes + IHDR dims, the real decode lives in Skia which isn't linked in the GPU-off importer build). `pulp::view::derive_fader_skin` / `derive_meter_skin` (`core/view/src/widget_skin_derive.cpp`) recover the fader's track/fill/thumb/border colours and the meter's gradient stops by locating the widget art (tallest opaque vertical run in the centre column) and classifying rows. The codegen emits `setFaderSkin(id, track, fill, thumb, border)` and `setMeterColors(id, bg, "#stop0,#stop1,...")`; the native `Fader`/`Meter` redraw those PROCEDURALLY so the thumb still moves with `setValue()` and the fill still tracks `setMeterLevel()`.
- **No hardcoding**: every colour/stop is read from the exported pixels — there are no per-instance pixel offsets, Y-coords, or asset-name special-cases (per the repo "Figma-import fixes must generalize" rule).
- **Opt-out**: `--fader-style=default` / `--meter-style=default` (aliases: `plain`) fall back to the plain native look; `skin` / `skinned` keep derived skinning. Unknown style values exit 2 instead of silently selecting skinning.
- **Value normalisation**: the codegen normalises `audio_default` from `[audio_min, audio_max]` to 0..1 before `setValue`/`setMeterLevel` (a raw dB value like `-6` would clamp to 0 and mis-place the thumb / read empty).
- **Gotcha — top-level `asset_ref`**: the figma-plugin lane stamps `asset_ref` as a TOP-LEVEL node member, not under `attributes`. The JSON parser now promotes it into `node.attributes["asset_ref"]` so asset resolution (and therefore both knob sprite + fader/meter skin) can find it. Before this, no widget in a figma-plugin export ever picked up its captured PNG.
- **Honest limitations**: the derived track/fill is drawn at a fraction of the widget width (the captured fader track is a thin line; the meter fill spans the full bar where the capture is slightly inset), and the meter background heuristic can pick a lighter dark row than the true near-black channel. The gradient colours, thumb shape/colour/position, and value-driven level clipping are faithful.

**Decision matrix**:

| Constraint | Recommendation |
|---|---|
| Quick visual prototype, want crisp result | Default (silver) |
| Plugin marketing screenshots must match Figma exactly | `--knob-style=sprite` |
| Designer supplied a 64-frame rotational filmstrip | `--knob-style=sprite` |
| Mostly silver but ONE hero knob must match Figma | Default + name the hero `Knob@sprite` |
| Want to A/B test which looks better | Render once each way; visual-diff against the Figma reference |

**Recommending sprite to a user**: don't position it as a "fallback". For designers who chose a specific Figma knob style, sprite IS the right path. Frame it as "pixel-exact PNG (with bleed)" vs "native vector (without bleed)" — the tradeoff is real and per-design.

**Claude Code surfacing**: when someone runs `/import-design` on a Figma file, ask if they want silver (default) or sprite. If they're unsure, default silver and add a note that they can re-import with `--knob-style=sprite` to compare. If they have one specific knob that "needs to look like the Figma", suggest the `@sprite` suffix on that node's name in the Figma file.

## Native-import gotchas

Non-obvious rules in the import + native-codegen path. Each cost a real
correctness bug before it was made explicit; treat them as invariants.

- **Sub-pixel geometry survives end-to-end, through TWO former rounding
  layers.** Concentric compositions (knob body ellipse + value-ring arc)
  are aligned only at fractional coordinates: "A Channel FX" solves the
  body at (7.51, 7.51, 22.53²) against ring arcs placed at 2-decimal
  precision, and whole-pixel rounding moves siblings RELATIVE to each
  other by up to ~0.7px/axis — every knob ring rendered visibly
  off-center. The two layers that used to round: (1) the .fig decoder's
  `styleFor` (scene.mjs) emitted `Math.round`ed left/top/width/height for
  box-model nodes while the VECTOR_LIKE lane kept `round2` — both now use
  `round2`; (2) Yoga's default pointScaleFactor of 1.0 re-rounded the
  laid-out boxes — imported roots opt out via
  `View::set_subpixel_layout(true)` (set by `build_native_view_tree` and
  by the emitted `setSubpixelLayout('', true)` bridge call in generated
  JS; discovered pass-wide in yoga_layout.cpp). When diagnosing a
  "shape misregistered by <1px" report, check BOTH layers before touching
  paint code — `--dump-layout` shows the post-Yoga truth, and integer-only
  x/y/width/height there means the opt-out is not reaching the pass.
  Fixing this also halved thumb_parity's bad-block count on
  designers-pick (77 → 39).

- **Text-editor value is `<textarea>`-only.** In `imported_widget_semantics`
  (design_import_native_common.cpp), a node's incidental display text
  (`text_content` — often a folded label/heading) must NOT become a text
  editor's contents. Only a `<textarea>` body is the value. Gate the display-
  text fallback on `pulpSourceFamily`/`jsxTag == "textarea"`; an `<input>` with
  no explicit value renders empty.
- **Indexed state bindings keep the index but resolve via the base.**
  `value={params[0]}` (design_import_v0_tsx.cpp) must keep `pulpValueKey =
  "params[0]"` (so the binding layer targets the element) while looking up
  `pulpInitialValue` under the **base** identifier `params` —
  `state_initial_values` is keyed by base. Returning the full indexed
  expression as the lookup key silently drops the initial value.
- **JSX computed-member keys come from the AST node, not a source slice.**
  In `jsx-contract-audit.mjs`, derive `obj[key]` member paths from the
  property node's type (StringLiteral/NumericLiteral/Identifier), never from
  `expressionText('', node)` — an empty source string collapses every
  computed access to `[]`.
- **frontend-IR gates are fail-closed; manifest classification ≠ proof.** The
  `tools/scripts/frontend_ir_*.py` gates must never let missing/`null`/`false`
  evidence pass: a `route_manifest` calling a node `native_cpp` is not binary
  proof; a child gate with zero checks verified nothing; a bare `{}` proof
  artifact is not proof. Generic helpers (`as_dict`/`as_list`/
  `non_negative_int`/`load_json`/`write_json`) live in `frontend_ir_common.py`
  and the canonical route set in `frontend_ir_validation.NATIVE_ROUTES` —
  import them, don't re-type them.

## Sprite/asset sizing — never skew, size from the pixels

Non-obvious rules for sizing captured image assets (knob graphics, icons,
rasterized shapes). Each cost a visible fidelity bug.

- **`render_bounds` is NOT a reliable size for scaled component instances.**
  The figma-plugin export's `render_bounds {w,h,dx,dy}` (the on-canvas bleed
  extent) can have a totally different aspect than the exported PNG — e.g. a
  knob graphic with `render_bounds` aspect 1.81 but a PNG aspect 0.87, because
  `render_bounds` reports the *component's* native box, not the scaled
  *instance*. Sizing an element to `render_bounds` and letting the renderer
  stretch the PNG into it skews the art ~2x. `setObjectFit` is **storage-only**
  (the ImageView paint slice ignores it), so aspect can ONLY be preserved by
  sizing the *element* itself.
- **Recover real dims + the opaque-core bbox from the PNG; the manifest dims
  are null.** The import CLI's asset-resolution pass stamps `png_natural_w/h`
  (PNG header) and, for nodes carrying `render_bounds`, the `art_core_*` bbox
  of pixels with alpha ≥ 0.5 (`compute_opaque_core`). Codegen scales the whole
  PNG so the solid core fits the layout box (`min(box_w/core_w, box_h/core_h)`)
  and positions it so the core lands on the box — the soft shadow then bleeds
  beyond. This is the data-driven fix for "right size, not skewed", and it
  generalizes to any sprite (knob disc, icon) — no layer-name matching.

## Widget recognition vs decorative children

- **A decorative stroke child must not block widget recognition.** A knob's
  ~0-width stroked pointer hairline is demoted image→frame by the degenerate-
  stroke pre-pass; before it was tagged, that lone leaf frame tripped the
  `has_child_containers` gate in `detect_node_audio_widget` and the whole knob
  fell through to a raw stack of images instead of a native `createKnob`. The
  demotion now tags the node `__stroke_demoted`; the recognition gate treats a
  tagged child as ornamentation, while a *populated* or genuinely structural
  frame/group child still disqualifies (a widget-named row of sub-widgets stays
  a row). When a degenerate stroke becomes a fill, also DROP its border — a
  1.5px line plus a 1.5px border draws on both edges and renders ~3x too wide.

## snake_case vs kebab-case + multi-line text

- **`parse_align` must accept snake_case.** The figma-plugin export emits
  `space_between`/`flex_end`; CSS sources emit `space-between`/`flex-end`.
  `parse_align` normalizes `'_'→'-'` so both spell the same — otherwise
  snake_case justify/align silently fall through to `flex_start`.
- **Multi-line text heuristic is line_height-aware, with a TIGHT fallback.**
  `multiline_box = height > line_h * 1.8`, where `line_h =
  line_height.value_or(font_size * 1.2)`. The `*1.2` fallback (not `*1.4`)
  matters: a genuine two-line paragraph (e.g. 26px box at 11px font, no
  declared line_height) must read as multi-line, while a single small line in a
  tall padded box (e.g. a search field: 17px box, 8px font, line_height 9.84)
  must stay single so its vertical centering survives.

## Rasterizing vector illustration frames

- **A pure-vector illustration FRAME must be flattened to one PNG.** The
  exporter rasterizes single vector *leaves* but walks a vector illustration
  *group* (a frame whose whole subtree is vector/shape content — e.g. a 3-D
  prism of rotated `REGULAR_POLYGON` faces) as a layout container; since Pulp
  is flex/grid-only with no rotated-polygon primitive, those faces degrade to
  axis-aligned bordered boxes. `tools/import-design/figma_rasterize_vector_frames.py`
  is a post-export pass that detects such frames (subtree all vector/shape, no
  text, no recognized widget — keyed on structure, NOT layer names) and
  replaces each with a single PNG rasterized via the Figma `/images` endpoint.
  It needs a Figma token + network, so it is a developer-time export helper,
  never run in CI.

## Fidelity self-checks (reference-free invariants) — `design_fidelity` module

- **All checks live in `core/view/src/design_fidelity.cpp`** (not codegen),
  each a small pure function `optional<FidelityIssue>(const FidelityContext&)`.
  A **registry** (`kChecks`, rows of `{FidelityElement applies_to, fn}`)
  dispatches by element kind via `run_fidelity_checks(ctx, sink)`. `FidelityContext`
  carries the node, sanitized id, the emitted w/h, and the element kind.
  `design_codegen.cpp` keeps only thin call-sites: it captures the geometry it
  already computes and calls `run_fidelity_checks` in the image and container
  branches. **Adding a PER-ELEMENT invariant = one function + one registry row +
  a case in `test_design_fidelity.cpp`.** Codegen does not grow.
- **Two invariant shapes.** Most checks are *per-element registry rows* (above).
  A few need subtree/coverage context a single `FidelityContext` can't carry —
  those are *tree passes*: free functions taking `(root, diagnostics, node_id_of,
  sink)`, called once from `generate_pulp_js` after the emit walk, NOT in the
  registry. A tree pass must mirror codegen's recursion exactly — descend EXCEPT
  into the terminal image/widget/text branches (which return without emitting
  children). Skipping that is a real FP source: a knob consumes its child
  ellipses into its native paint, so a naive full-tree walk would flag that
  consumed stroke-ellipse as a dropped vector. The pass also takes
  `DesignIR::diagnostics` so a node already carrying a render-affecting import
  diagnostic (matched by `stable_anchor_id` or structural `$`/`/children[i]`
  path) is suppressed — never double-report a drop the importer already surfaced.
- **Element dispatch is load-bearing.** A check runs ONLY for its element kind.
  Critical gotcha: the gross-size check must NOT see images — a bleed sprite's
  emitted box legitimately differs >3× from its style box (it sizes to
  render_bounds/core), so running gross-size on images yields a flood of false
  positives. The registry's `applies_to` prevents this; the e2e check below
  catches it if a new invariant is mis-mapped.
- **Checks shipped:** `check_image_sizing_fidelity` (image; bleed sprite emitted
  aspect vs source PNG → `skew` / `aspect-unverified`; ordinary images fill
  their box, never flagged), `check_gross_size_divergence` (container; a
  fixed/fixed node emitted >3× its source box → `gross-size`; hug/fill/absolute/
  display:none self-skip), `check_widget_intrinsic_size` (widget; a recognized
  audio widget emitted >1.5× its source intrinsic → `widget-size`, or
  `widget-undersized` when the source is below the widget's native minimum and
  codegen clamps up — keep its native-min table in sync with the `kMin*` floors
  in design_codegen.cpp), and `check_text_vertical_centering` (text; a
  single-line label in a tall slot left top-aligned → `text-vcenter`; the text
  call-site stamps `_emitted_vertical_align` so the check sees codegen's
  decision). All four fire 0 on a faithful import (regression guards).
- **Tree pass shipped:** `check_vector_renderability` (root walk; a visible
  vector/path-like node — `path`/`svg_path`/`rect`/`svg_rect`/`rectangle`/`line`/
  `svg_line`/`ellipse`/`circle`/`polygon`/`polyline`/`star`/`vector` — above a
  256px² area floor that produces no renderable primitive → `dropped-vector`).
  "Renders" = a rasterized `asset_path`, a native/audio widget, any children, or
  a visible fill (`background_color`/`gradient`/`image`). The childless
  no-fill-no-asset case hits codegen's generic-frame fall-through, which paints
  only `background_color` and drops stroke/border/path art to an empty
  `createRow` — that silent drop is the target. FP gates: invisible
  (`opacity:0`/`display:none`/`visibility:hidden`), sub-256px² area (kills the
  hairline dividers + EQ grid lines real designs are full of — e.g. ELYSIUM's
  width-0 separator vectors), and already-diagnosed (see tree-pass note above).
  Findings carry the exact bridge id via codegen's real node→id map. Fires 0 on
  a faithful import (substantive shape art is rasterized at export and routes
  through the image branch).
- **Informational vs hard findings (load-bearing for `--strict-fidelity`).**
  `FidelityIssue::informational` marks advisory findings the importer should
  surface but never fail on — currently `widget-undersized` (codegen
  legitimately clamps a sub-native-minimum widget up). The CLI derives its exit
  code from `count_strict_fidelity_failures()` (non-informational count), NOT the
  raw finding count. If you add an advisory check, set `informational=true` at
  construction AND assert it in a test — otherwise a "warn, don't fail" finding
  silently exits 4 and breaks faithful imports.
- **Auto-height text must self-skip the vcenter check.** When the IR carries no
  explicit height, codegen synthesizes `label_h = font*1.4`, which lands inside
  the tall-slot range and would falsely trip `text-vcenter` on ordinary labels.
  The call-site stamps `_emitted_vertical_align = "n-a"` in that case and the
  check treats `"n-a"` (like `"center"`) as a self-skip — only an EXPLICIT
  taller-than-font slot is held to the centering invariant.
- **`--strict-fidelity` covers BOTH codegen paths and `--dry-run`.** The checks
  run in `generate_native_node` AND in `generate_node` (web-compat, image-skew
  only — widget/text slot geometry differs on that path and full web-compat
  coverage is a tracked hardening follow-up). `--dry-run` returns
  `fidelity_failed ? 4 : 0`, not an unconditional 0 — a harness that imports with
  `--dry-run --strict-fidelity` still sees the non-zero exit.
- **`pulp import-design --strict-fidelity`** prints findings as `fidelity: …`
  warnings (informational ones tagged `[informational]`) and exits 4 when any
  HARD finding is present. Tests: unit cases per check in
  `test/test_design_fidelity.cpp`; codegen-routing + web-compat + informational
  cases in `test_design_import.cpp`.
- **Golden re-import regression — `tools/import-validation/golden_regression.py`.**
  Re-imports a design from scratch and compares its render to a committed
  baseline with TOLERANT/STRUCTURAL matching (per-pixel-over-tolerance fraction
  is the primary gate; a shift-dilated edge-agreement is a lenient backstop) —
  NOT exact-pixel, which false-positives on GPU AA noise + legit sub-pixel
  sizing. Source-agnostic (any `--from`). Proprietary baselines stay local; CI
  uses synthetic ones. Run it (and `--strict-fidelity` on a real import) after
  any change to the sizing/fidelity paths to prove no visual regression.
  - **uint8 wraparound gotcha:** the structural edge map (`edges()`) must cast
    luminance to a SIGNED dtype before `np.diff` — on raw uint8 a `255→0`
    dark-on-light edge wraps to `+1` and falls below the threshold, so the
    strongest edges in dark-text/light-bg designs vanish and a removed/moved
    thin feature can still read as "high edge agreement". The
    `int16` cast fixes it. `golden_regression.py --selftest` (ctest
    `golden-regression-selftest`, skips 77 without numpy) pins this.

## Runtime materializer and baked-C++ emitter share their lowering decisions

`PromotedChildHitPolicy` + the hit-ownership functions and `ImportedImageSizing` +
`imported_image_sizing_override` live ONCE in `design_import_native_common.{hpp,cpp}` and are
consumed by both the runtime import path and `design_cpp_codegen.cpp`. They used to be
copy-pasted per lane and had already drifted — the baked-C++ copy omitted `combo_box`, so a
baked plugin's hit ownership silently diverged from what the runtime importer produced from
the same IR. A Catch2 case now asserts the policy for every `NativeWidgetKind`, so the next
drift fails a test instead of shipping. When you add a widget kind or change a lowering
DECISION (as opposed to per-target emission syntax), it belongs in native_common — only the
target-specific string emission stays per-lane.

## Shared IR helpers — editing one is a cross-lane decision

Under native_common sits a second, narrower shared home: `design_ir_helpers.hpp` holds the
one definition of **how a lane reads the IR** (`attr`, `attr_bool`, `first_asset_id`,
`asset_uri`) plus the pure parsers those reads need (`parse_hex_color_rgba`, `hex_digit`,
`lower_copy`). Three lanes include it — the native materializer
(`design_import_native_common.cpp`), the baked-C++ emitter (`design_cpp_codegen.cpp`), and
the Swift emitter (`design_swift_codegen.cpp`). Each used to carry its own copy, so the same
IR value could lower differently per target by accident rather than by decision.

The consequence to internalize: **"fix `first_asset_id` for my lane" is no longer a local
edit.** One change to that header moves what the runtime importer, a baked plugin, and a
Swift export all resolve. That is the point of the header — but it means a genuinely
lane-specific need is met by **adapting at the call site, not by forking the helper**.
`parse_hex_color` in `design_import_native_common.cpp` is the pattern to copy: the shared
parser returns the raw 0..255 quad, the native lane wraps it into a `Color`, and
`design_cpp_codegen.cpp` turns the same quad into a `Color::rgba8(…)` source literal. The
parse decision is shared; the representation is not. Per-target string escaping, indenting,
and number formatting stay in their emitters for the same reason — a helper only belongs in
the shared header when its contract is identical for every lane.

Two things to watch:
- **The JS lane is NOT a consumer.** `design_codegen.cpp` (`generate_pulp_js` — web-compat
  and bridge-native JS) still reads `node.attributes` directly, so a helper fix does not
  reach it. Same separate-lanes hazard as background gradients above.
- **`parse_hex_color` still exists as a per-lane name.** In native_common it is now a thin
  wrapper over the shared `parse_hex_color_rgba`. Grepping the old name finds the wrapper,
  not the rules — those live in `design_ir_helpers.hpp`.

Tests: `[design-ir-helpers]` in `pulp-test-design-import-native-common` pins the contracts a
lane would otherwise re-guess — asset-key priority then a sorted fallback scan, both bool
polarities plus the fallback for unrecognized spellings, local-file-over-remote asset URIs,
and every accepted hex shape. Change a rule there and the failure lands in the lane-neutral
place, rather than surfacing later as one target's fidelity drift.

`design_ir_helpers.hpp` is private to `core/view/src/` and is not part of the installed SDK
surface — do not reference it from a public header.

## Importer accommodations are opt-in, not universal

Two point-in-time import fixes used to run in the core paint / hit paths for
*every* tree. They are now CSS-faithful by default and the materializer opts in
only where the accommodation is wanted — so imports keep their behavior while
native/authored trees clip strictly per CSS and pay nothing for the scan:

- **Circle-marker clip tolerance** (`View::set_clip_marker_tolerance()`, default
  OFF). Expands an `overflow:hidden`/`scroll` container's clip so an
  XY-pad-style value dot sitting at an edge value is not cropped. The native
  materializer turns it on for the clipping containers it materializes; the
  per-frame `O(children)` marker scan only runs when it is on. If an imported
  circular marker is being clipped, verify the container actually got
  `set_clip_marker_tolerance(true)` — a hand-built tree will not have it.
- **ScrollView `overflow:visible` hit inflation** — the old hard-coded ±500px
  hit expansion is gone; the hit area now follows the real overflow geometry.
  A tree relying on the old blanket inflation for off-bounds hit-testing must
  size its interactive children honestly instead.
