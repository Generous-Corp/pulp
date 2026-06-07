# Testing the plugin against a real-world frame (ELYSIUM)

> ELYSIUM is a plugin UI by **Julian Behrens** ([vst-design.com](https://www.vst-design.com)),
> shared on the [Figma Community](https://www.figma.com/community/file/1476177446001979362).
> Pulp uses it **only** as a real-world test reference to harden the Figma
> importer — it is not vendored, bundled, or shipped, and nothing is hardcoded to
> it (the lessons are generalized). Credited in `docs/reference/licensing.md`.
> Thanks, Julian!

End-to-end smoke for Phase 2a slice 1.

**Recommended test file (Pro plan, allows local plugin install):**
`https://www.figma.com/design/KCKIyZoWXjde6qVNCm4qPa/Untitled?node-id=3-42` — same VST Style frame as the original ELYSIUM community file, just duplicated into a Pro workspace where local-development plugins are allowed. The original community file at `OnM7qmsi77W7ieIEvTOHQq` rejects local-development plugins.

---

## What this validates

- Plugin loads cleanly in Figma desktop
- Walker traverses the ELYSIUM frame without crashing
- Export produces a downloadable `.pulp.json` matching the v1 schema
- The JSON contains expected structural pieces (background frame, text labels, knob frames, etc.)

What it does **not** validate yet (deferred to slice 2 / Phase 3 / Phase 4):
- Image / SVG asset export
- Pulp Library component recognition (ELYSIUM doesn't use the Pulp library — everything emits as a generic frame, which is the correct behavior per plan §3.3)
- The C++ side consuming the JSON (Phase 4)

---

## Steps

1. **Build the plugin**:

   ```bash
   cd /Users/danielraffel/Code/pulp-figma-plugin/tools/figma-plugin
   npm run build
   ```

   Produces `dist/code.js` + `dist/ui.html`.

2. **Install in Figma desktop**:

   - Open Figma desktop.
   - **Plugins → Development → Import plugin from manifest…**
   - Pick `/Users/danielraffel/Code/pulp-figma-plugin/tools/figma-plugin/manifest.json`.
   - The plugin appears under **Plugins → Development → Design for Pulp**.

3. **Open the test file**:

   - Open `https://www.figma.com/design/KCKIyZoWXjde6qVNCm4qPa/Untitled?node-id=3-42`
   - (Or any other Pro-plan file you own — community files don't allow local plugin installs.)

4. **Select frame `3:42`** (the "VST Style" outer frame, 1000×600).

5. **Run the plugin**: **Plugins → Development → Design for Pulp**.

6. **Click "Export to Pulp"**.

   - Expected status line: `Exported N nodes` (likely 100-200 for this frame)
   - The plugin triggers a browser download to your usual Downloads folder
   - Filename: something like `VST-Style.pulp.json`

7. **Inspect the JSON**:

   ```bash
   jq '.format_version, .provenance.source_uri, .root.name, .root.type, (.root.children | length)' ~/Downloads/VST-Style.pulp.json
   ```

   Expected output:

   ```
   "2026.05-figma-plugin-v1"
   "figma://KCKIyZoWXjde6qVNCm4qPa/3:42"
   "VST Style"
   "frame"
   <some-number-around-12-to-20>
   ```

8. **Sanity check structure**:

   ```bash
   # Should find ELYSIUM, ENVELOPE, FILTER & EQ, FX Rack, GRAINS, CURSOR, RANGE, TUNING somewhere in the tree
   jq -r '[.. | .name? // empty | select(. != null)] | unique[]' ~/Downloads/VST-Style.pulp.json | head -30
   ```

9. **Diagnostics check** — these are expected for ELYSIUM:

   ```bash
   jq '.diagnostics' ~/Downloads/VST-Style.pulp.json
   ```

   Should include:
   - `image-fill-deferred` (Phase 2a slice 1 doesn't export images yet)
   - Possibly `complex-gradient` warnings (ELYSIUM uses some radial gradients)
   - Possibly `max-nodes-exceeded` if ELYSIUM is bigger than 5000 nodes (unlikely)

   Should **NOT** include any unexpected errors.

---

## Iteration

If the export fails or produces nonsense, copy the error from the plugin's status line back into the chat and we'll iterate. The most likely failure modes:

| Symptom | Likely cause |
|---|---|
| "Select at least one frame…" with selection clearly present | `figma.currentPage.selection` resolved to a different page; click into the frame first |
| Stack trace in the Figma desktop console | TypeScript compilation issue; surface in chat |
| "Walked 0 nodes" but selection has children | Walker hit a `visible: false` filter; toggle the slice 1 `includeHidden` option |
| Browser doesn't download anything | Blob/`URL.createObjectURL` permission issue inside Figma's iframe; we'd switch to `figma.ui.postMessage` with a "save dialog" approach |

When this works end-to-end on ELYSIUM, Phase 2a slice 1 is done and we move to **slice 2** (image/SVG asset export via `exportAsync`) + **Phase 3** (Pulp Library recognition) in parallel.
