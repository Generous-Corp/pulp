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

> **Status: once published.** At the time of writing, the plugin's publish to
> the Figma Community is in progress. The **component library** below is already
> published and usable today. Until the plugin is live in the Community, use the
> [local development install](#b-local-development-install).

1. **Install the plugin.** In Figma, install the **"Design for Pulp"** plugin
   from the Figma Community.

2. **Add the Pulp component library.** Add the published example library so you
   can drop "Pulp / Knob", "Pulp / Fader", and "Pulp / Meter" instances into
   your design:
   <https://www.figma.com/community/file/1642409275200893924>

3. **Design.** Lay out your plugin UI. Drop in Pulp library components wherever
   you want a recognized, parameter-bound control. Restyle them however you like
   — the [semantic link survives restyling](../reference/design-import-model.md#restyle-freely).

4. **Export.** Select the frame(s) you want, run the plugin, and choose
   **Export to Pulp**. The plugin downloads a `*.pulp.json` file (or a
   `*.pulp.zip` when your design includes image assets — see
   [Exports with assets](#exports-with-assets)).

5. **Import into Pulp:**

   ```bash
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

- **`*.pulp.json`** — when your design has no image assets. This is the file you
  pass directly to `pulp import-design --from figma-plugin`.
- **`*.pulp.zip`** — when your design includes image assets. The zip contains a
  `scene.pulp.json` plus the asset files. Unzip it and point the importer at the
  extracted `scene.pulp.json`; the importer resolves the bundled assets from the
  accompanying asset manifest.

The plugin runs **entirely on your machine** — its manifest declares
`networkAccess: { allowedDomains: ["none"] }`, so no design data leaves your
computer.

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

Two things live in the Figma Community and should be cross-linked for users:

- the **component library** (already published):
  <https://www.figma.com/community/file/1642409275200893924>
- the **plugin** (this publish flow).

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
