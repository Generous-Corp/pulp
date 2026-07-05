# The "Design for Pulp" Figma plugin

This guide covers getting the Figma plugin, exporting a design, and feeding it
to Pulp. There are **two install paths** and they are easy to mix up, so they
are spelled out separately below:

- **[A) Install from the Figma Community](#a-install-from-the-figma-community)** —
  no build step, for designers.
- **[B) Local development install](#b-local-development-install)** — for
  contributors and pre-publish testing.

For the conceptual model behind the import (semantic-not-visual, the
additive-fidelity guarantee, restyling), read
[The Figma → Pulp Design-Import Model](../reference/design-import-model.md)
first. For the full CLI reference, see
[Design Import API Reference](../reference/design-import.md).

---

## A) Install from the Figma Community

This is the path for designers. **No build step, ever.**

> **Status: in review.** The plugin has been submitted to the Figma Community
> and is **awaiting Figma's review** (typically 1–3 business days). Until it is
> approved, its Community URL resolves **only for the author** — everyone else
> gets a 404 — so use the [local development install](#b-local-development-install)
> for now. The **component library** below is already published and usable today.

1. **Install the plugin.** Once approved, install the **"Design for Pulp"** plugin
   from the Figma Community:
   <https://www.figma.com/community/plugin/1642456870947996392>

2. **Add the Pulp component library.** Add the published example library so you
   can drop "Pulp / Knob", "Pulp / Fader", "Pulp / Meter", "Pulp / XYPad",
   "Pulp / Waveform", and "Pulp / Spectrum" instances into your design:
   <https://www.figma.com/community/file/1642409275200893924>

3. **Design.** Lay out your plugin UI. Drop in Pulp library components wherever
   you want a recognized, parameter-bound control. Restyle them however you like
   — the [semantic link survives restyling](../reference/design-import-model.md#restyle-freely).

4. **Export.** Select the frame(s) you want, run the plugin, and choose
   **Export to Pulp**. The plugin downloads a `*.pulp.zip` when your design
   includes image assets, or a bare `*.pulp.json` when it doesn't (see
   [Exports with assets](#exports-with-assets)).

5. **Import into Pulp** — point the importer straight at whatever the plugin
   gave you; the CLI reads a `.pulp.zip` directly (it unpacks it for you):

   ```bash
   pulp import-design --from figma-plugin my-design.pulp.zip
   # …or a bare .pulp.json export:
   pulp import-design --from figma-plugin my-design.pulp.json
   ```

That's the whole loop. No build, no toolchain.

---

## B) Local development install

This is the path for contributors, and the only path until the plugin is
published to the Community.

### Build the plugin

```bash
cd tools/figma-plugin
npm install
npm run gen-types   # only if the export schema changed
npm run build       # produces dist/code.js + dist/ui.html
```

`npm run gen-types` regenerates `src/types.generated.ts` from
`schema/figma-plugin-export-v1.json`; you only need it when that schema
changes. `npm run build` is what produces the runnable plugin in `dist/`.

### Import it into Figma (first time only)

In Figma **desktop**:

1. **Plugins → Development → Import plugin from manifest…**
2. Pick `tools/figma-plugin/manifest.json` from your checkout.

The plugin now appears under **Plugins → Development → Design for Pulp**.

### Rebuild, don't re-import

This is the **#1 point of confusion**, so read it carefully:

> **After the first import, you pick up code changes by *rebuild + re-run*, NOT
> by re-importing.** Figma re-reads `dist/` fresh every time you run the plugin.
> You only **re-import the manifest** if `manifest.json` itself changes.

In other words:

| You changed…                              | What to do                       |
|-------------------------------------------|----------------------------------|
| anything under `tools/figma-plugin/src/**` | `npm run build`, then re-run the plugin in Figma |
| the export schema (`schema/…-v1.json`)    | `npm run gen-types && npm run build`, then re-run |
| `manifest.json`                           | **re-import** the manifest in Figma |

**When you need to rebuild:** any time you pull new plugin changes, or edit
anything under `src/**` or the export schema. To avoid running `npm run build`
by hand each time, use the watcher:

```bash
npm run build:watch
```

Then just re-run the plugin in Figma after each save.

### Gotcha: local-dev plugins need a Figma Pro workspace

Figma only loads local-development plugins in a workspace/file that allows them
— that's **Figma Pro**. Community and free files **reject** local-dev plugins.
If the plugin won't load, **duplicate your design into a Pro file** and test
there.

---

## Exports with assets

The exporter downloads one of two things:

- **`*.pulp.json`** — when your design has no image assets.
- **`*.pulp.zip`** — when your design includes image assets. The zip contains a
  `scene.pulp.json` plus the asset files.

**Pass either one straight to the importer** — `pulp import-design --from
figma-plugin <file>` reads a `.pulp.zip` directly (it unpacks the archive and
resolves the bundled assets from the manifest for you). You only need to unzip
by hand for a hand-authored archive whose internal layout differs from the
plugin's; in that case, point the importer at the extracted `scene.pulp.json`.

The plugin runs **entirely on your machine** — its manifest declares
`networkAccess: { allowedDomains: ["none"] }`, so no design data leaves your
computer.

---

## Two ways to get a design into Pulp — and which one avoids Figma's limits

There are two ways to pull a Figma design into Pulp, and they hit **very
different** rate limits. Pick the one that matches your Figma seat.

| Path | What it is | Figma rate limit |
|------|-----------|------------------|
| **Plugin export** (this guide) | The "Design for Pulp" plugin exports a `*.pulp.json` / `*.pulp.zip` from inside Figma. | **None.** Runs inside Figma desktop; no Figma API/MCP quota, no REST `/images` budget. **Unlimited.** |
| **Figma MCP** (`get_metadata` / `get_screenshot` / `get_design_context`) | An AI agent reads the file directly to inspect structure or grab a reference render. | **Metered.** See the seat table below — as low as **6 calls per _month_**. |
| **Headless REST** (`figma_rest_export.py`) | CI / no-desktop fallback that renders frames via Figma's REST `/images`. | Strict per-file **Tier-1 budget**; a dense frame `429`s for many minutes. Honors `Retry-After` with backoff, but slow. |

**Recommendation: export with the plugin.** It is the only unlimited path and it
produces the exact envelope the importer consumes — so it works on **any seat or
plan**, including Starter and View/Collab, with zero quota concerns.

### If you (or your agent) will use the Figma MCP, your seat matters

The Figma MCP's read tools are metered, and the free/viewer tiers are **too low
for iterative work** ([Figma's rate-limit
docs](https://developers.figma.com/docs/figma-mcp-server/rate-limits-access/)):

| Seat | Starter | Professional | Organization | Enterprise |
|------|---------|--------------|--------------|------------|
| **View / Collab** | 6 / month | 6 / month | 6 / month | 6 / month |
| **Dev / Full** | 6 / month | 200 / day · 10/min | 200 / day · 15/min | 600 / day · 20/min |

- **View/Collab seat, or any Starter plan → 6 MCP calls per _month_.** This is a
  hard monthly quota (not a transient limit — backoff cannot clear it). At this
  tier, **do not rely on the MCP**; use the **plugin export** above, which needs
  none. Reserve MCP calls for a one-off inspection, and prefer a single
  `get_design_context` (it returns code + screenshot + metadata together) over
  separate `get_metadata` + `get_screenshot`.
- **To use the MCP for real design-to-code work, you need a Dev or Full seat on
  Professional or higher** (200–600 calls/day). If an agent keeps hitting the
  monthly cap, that is the upgrade to make — or switch that workflow to the
  plugin export.

Only *read* tools are metered; write tools (`whoami`, `generate_figma_design`)
are exempt. Figma may change these numbers — the linked doc is authoritative.

---

## Publishing the plugin to the Figma Community (maintainers)

Publishing is a **manual action in Figma desktop**. There is no CI step for it.

1. In Figma desktop: **Plugins → Development → "Design for Pulp" → Publish…**
   (or **Manage plugins → Publish**).

2. Fill in Figma's publish form. It requires:
   - a **128×128 icon**,
   - a **cover image**,
   - a **tagline + description**,
   - **tags**,
   - a **support contact**.

   The manifest's `networkAccess` is `"none"` (no data leaves the machine),
   which eases Figma's review.

3. **Submit for review.** Approval typically takes a few days. Once approved,
   the plugin is public.

### The three Figma URLs (don't mix them up)

| URL | What it is | Who uses it |
|-----|-----------|-------------|
| <https://www.figma.com/community/plugin/1642456870947996392> | The **plugin** in the Community (post-approval) | Designers — install it |
| <https://www.figma.com/community/file/1642409275200893924> | The **component library** in the Community (published) | Designers — add it, then drop in "Pulp / Knob" etc. |
| <https://www.figma.com/design/vxW6btjzQtc4t9ITLNjev0/Pulp-Library> | The **editable library source** file | Maintainers — open via the Figma MCP to add new Pulp library widgets (Fader, Meter, future XYPad/Waveform/Spectrum) and republish |

The first two are what a designer installs; the third is the source of truth
maintainers edit. The plugin's canonical published ID
(`1642456870947996392`) is pinned in `tools/figma-plugin/manifest.json`.

---

## See also

- [The Figma → Pulp Design-Import Model](../reference/design-import-model.md) —
  the conceptual model: semantic-not-visual, additive fidelity, restyle freely.
- [Importing Designs](importing-designs.md) — the broader import guide across
  all sources.
- [Design Import API Reference](../reference/design-import.md) — CLI flags, IR,
  validation, token sync.
- `tools/figma-plugin/README.md` — plugin internals and dev layout.
- `tools/figma-plugin/docs/building-the-pulp-library.md` — how the Pulp Figma
  library file itself is built.
