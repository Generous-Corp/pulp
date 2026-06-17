# HTML → Figma faithful import

Codified, repeatable pipeline for importing the "Ink & Signal" design-system
specimens (`Pulp Components.html`) into the Figma library
(`q9iDYZzg86YrOQKr6I3bY0`) **pixel-perfectly**, in both **dark** and **light**,
one component per page with the two themes side by side.

The earlier approach — a hand-rolled DOM extractor that placed one absolute
Figma node per element — was leaky (stray fragments, overlaps), unstructured,
and never got below ~14/255 mean diff. This pipeline uses Figma's native
`generate_figma_design` (html-to-design) capture and gets components to
**~1–1.3/255** with crisp vector structure.

## The pipeline

```
bundle render  ──►  per-component standalone HTML  ──►  capture  ──►  verify
(once)              gen_standalone.py + capture_prep.js   generate_   verify_region.py
                                                          figma_design
```

### Discovery hardening — there are TWO specimen families (don't key on one)

`Pulp Components.html` renders specimens in **two different markups**. Keying
discovery on only the first silently drops whole components (this happened:
Range Slider/NumberBox were imported from the wrong block, and Inline Value
Editor / Property Panel / Group Box were wrongly declared "absent"):

| Family | Markup | Examples | Generator |
|---|---|---|---|
| A — card | `div.card` with an `h2/h3` title | Channel Strip, Musical Typing, Waveform, Knob States, XY Pad, Spectrum | `gen_standalone.py` (reads `ds_dump.json`) |
| B — eyebrow | `p.lbl` ("Name — desc · group") + next `.themepane` or `.grid` | Range Slider, Inline Value Editor, Property Panel, Group Box, Number Box, Modulation Rings | `gen_eyebrow.py` (reads `ds_specimens.json`) |

**Discovery checklist (run before mapping any component):**
1. Enumerate BOTH families: `document.querySelectorAll('.card')` AND
   `[...document.querySelectorAll('p.lbl')].filter(e=>/[—-]\s/.test(e.textContent))`.
2. Cross-check every Figma component page name against the union — if a page has
   no matching specimen, you mapped it wrong or missed a family. Do NOT conclude
   "no source exists" until both families are listed.
3. A component's real source is the block that carries the **full state matrix**
   (e.g. Range Slider's eyebrow themepane has 7 states; the `.card` "Sliders"
   has 2). Prefer the richer block.
4. `.themepane` / `.grid` are single-theme (adapt to `data-theme`) — no embedded
   dark+light; the data-theme flip still produces both themes.

### Build the inventory once.

`Pulp Components.html` is a self-reconstructing bundled artifact. Open it in
Chrome (it self-assembles), then dump BOTH families:
- `ds_dump.json` — inline CSS (~107 KB, base64 @font-face) + every `.card` outerHTML.
- `ds_specimens.json` — same CSS + every eyebrow specimen (`p.lbl` + its content block) captured as one unit (so the board includes the eyebrow header).

Extractor for `ds_dump.json` (cards):

   ```js
   // chrome-devtools evaluate_script, saved to ds_dump.json
   () => ({
     css: [...document.head.querySelectorAll('style')].map(s=>s.textContent).join('\n'),
     cards: [...document.querySelectorAll('.card')].map((c,i)=>({
       i, title:(c.querySelector('h1,h2,h3,h4')||{}).textContent?.trim(),
       w:Math.round(c.getBoundingClientRect().width),
       h:Math.round(c.getBoundingClientRect().height),
       html:c.outerHTML }))
   })
   ```

2. **Generate a standalone page** per component+theme:

   ```bash
   python3 gen_standalone.py <card_index> <dark|light> out.html
   ```

   It wraps one card in a minimal page that (a) inlines the full design-system
   CSS, (b) sets `data-theme` on `<html>` (the design system is theme-adaptive,
   so one card → both themes), (c) **pins the card to the width it had in the
   master page** so its flex/grid reflows identically, (d) embeds
   `capture_prep.js`, and (e) loads the html-to-design `capture.js`.

3. **Capture** with `generate_figma_design` (LOCAL / script-tag mode):
   - serve the design folder: `python3 -m http.server 8773`
   - call `generate_figma_design { fileKey }` → get a `captureId`
   - navigate Chrome to
     `…/standalone/<file>.html?v=N#figmacapture=<id>&figmaendpoint=<url>&figmadelay=2000`
     **(the `?v=N` cache-buster is required** — a hash-only change is a
     same-document nav and the page won't reload with the new HTML)
   - wait for `window.figma` to exist + fonts to settle, then poll
     `generate_figma_design { fileKey, captureId }` until `completed`
   - new boards land on the **first page (Cover)**; move them to the component's
     page and rename `… — Studio · Dark` / `… — Paper · Light`.

4. **Verify** per region (see "ensuring nothing is missed"):

   ```bash
   python3 verify_region.py source.png figma.png 60 8.0   # exits 1 if any tile drifts
   ```

## capture_prep.js — what html-to-design can't capture, and how we fix it

The capture is faithful for plain boxes, text, **linear**-gradients (meters,
faders) and inline SVG (chevrons). It fails on two design-system features; both
are rewritten in-page before capture, with zero layout change:

| Feature | Why the capture drops/breaks it | Fix |
|---|---|---|
| **Knob value ring** (`.pulp-knob .ring`) | a `conic-gradient` masked to a 6px annulus — capture flattens conic+mask and drops it | replace `.ring` with an equivalent inline `<svg>` arc (track + teal value arc), colors read from computed styles → theme-correct, crisp vector |
| **Text descenders** (`g/p/y` in tight `overflow:hidden` labels) | capture sizes text frames to the line box; descender ink below it is clipped | set `overflow:visible` on text-only leaves whose text fits horizontally → full glyphs, no reflow |

`capture_prep.js` must run **after `document.fonts.ready`** (glyph metrics are
wrong under the fallback font) and is idempotent.

## Lessons learned (why things were missed, and the guardrails)

These are the reasons defects slipped through, and the checks now in the loop:

1. **A global mean diff hides local defects.** A 56px knob ring is ~0.15% of a
   1400px board, so dropping it left the whole-board mean at 1.03/255 — looked
   "perfect." **Guardrail:** `verify_region.py` tiles the image and reports the
   worst tiles; the dropped ring shows up as a 12/255 tile even when the average
   is clean. Always region-check, never trust the global average alone.

2. **Browser-faithful ≠ capture-faithful.** The descender clip was ~0.4px in the
   browser but 3–4px in the capture. Comparing the *source render to itself*
   won't reveal capture-only defects. **Guardrail:** always diff the **Figma
   capture** against the source, zoomed, on the known-hard features (knobs,
   conic/masked rings, tight text, glows).

3. **Measure ink, not just boxes.** `Range.getBoundingClientRect` reports the
   line box, not descender ink, so box-based clip detection underestimates.
   **Guardrail:** for text de-clip we don't rely on the measurement — we declip
   any short text leaf whose text fits, which is always safe.

4. **Known-hard CSS list.** conic-gradient, CSS mask, `filter`/`box-shadow`
   glow, and overflow-clipped text are the features to spot-check on every new
   component before declaring it done. Linear gradients and inline SVG are safe.

5. **Same-document nav trap.** Changing only the URL `#hash` does not reload the
   page; the capture then runs against stale HTML. Always add a `?v=N` query.

## Status

- **Channel Strip** (card 35) imported dark+light, pixel-perfect (~1.3/255),
  knob rings + descenders fixed. Page `Channel Strip` (`169:2`).
- Remaining composite components roll out through the same loop, one at a time.
