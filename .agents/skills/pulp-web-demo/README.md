# pulp-web-demo — usage & customization

Generate WAM+WCLAP browser demos of Pulp plugins from one declarative config. The smarts
live in the pinned shared player (`@danielraffel/web-player`); this skill only wires it up and
stamps a deployable site. `SKILL.md` is the agent-facing contract; this is the human reference.

## Quick start

1. Add `<repo-root>/.pulp-web-demo/config.json` (copy an example from `examples/`).
2. Generate: `node <skill>/generate.mjs --config .pulp-web-demo/config.json --out docs`
3. Validate (blocking): `node <skill>/validate.mjs --config .pulp-web-demo/config.json --site docs --check-isolation --check-metadata --check-ownership`
4. Deploy `docs/` — WAM via GitHub Pages, WCLAP via Cloudflare Pages (or the coi fallback).

Determinism: no network, no clock/random — the same config always produces byte-identical
output, so regenerating is safe and diff-reviewable.

## Config reference (`config.schema.json`)

| Key | Purpose |
|---|---|
| `schemaVersion` | Must be `1`. Generator refuses a mismatched major. |
| `player.package` / `player.version` | The shared player, **pinned** (no ranges). Pages import it; never vendored. |
| `player.importBase` | Optional: serve the pinned player from your own origin/CDN instead of the default (esm.sh). |
| `theme.tokensHref` / `fontHref` / `mode` | Your skin — the player's 19-token + 1-font contract. `mode`: `auto`\|`light`\|`dark`. |
| `deploy.wam` / `deploy.wclap` | Per-ABI **deploy profile** (below). Selected independently. |
| `plugins[]` | Catalog: `{ id, title, subtitle?, mode?, abis?, artifacts?, paramOverrides?, midiViz? }`. Param controls come from the plugin's runtime `getParameterInfo()` — `paramOverrides` only refines them. |
| `gallery.title` / `layout` / `crossLinks[]` | Landing page + cross-links to the other ABI/gallery (declarative data). |
| `metadata.siteBase` / `ogImageStrategy` / `sourceLinkBase` | Unfurl policy. `ogImageStrategy`: `screenshot` (blocking per-page image) or `text`. |
| `modules.artifactResolver` / `adapterModule` | The **only** executable seams (see below). |

### Deploy profiles

Each of `deploy.wam` / `deploy.wclap` is `{ profile, basePath, publicUrl, outputDir, projectName? }`:

- **`profile`**:
  - `github-pages` — plain static. Correct for **WAM**.
  - `cloudflare` — server COOP/COEP/CORP via a generated `_headers`. **Default for WCLAP.**
  - `github-pages+coi` — a coi-serviceworker isolation **mirror**. Fallback only (larger failure surface).
- **`basePath`** — the *server path* the gallery is served under (e.g. `/my-plugins/`).
  Isolation headers/scope MUST cover this — the validator enforces it.
- **`publicUrl`** — the *absolute origin+base* this ABI is actually served from, for canonical/OG
  urls. **WAM and WCLAP usually live on different origins** — set both. (Distinct from `basePath`.)
- **`outputDir`** — repo-relative dir the site is written to. **`projectName`** — Cloudflare Pages project.

## How to customize

**Most customization is data — edit the config, not the skill.** Change theme URLs, hosts, base
paths, the plugin list, cross-links, OG policy. That's it for the common cases.

Two **typed module seams** exist for what data can't express (optional `.pulp-web-demo/hooks.mjs`):

- **`artifactResolver`** — `export function <name>(plugin, abi) { return { dspUrl, processorUrl, extraAssets: [] } }`.
  Use when your build emits WAM/WCLAP artifacts somewhere other than the default layout
  (`./wam-dsp.js` + `./wam-processor.js`; `./<id>.wasm` + the player's `wclap-processor.js`).
  This is the seam that prevents forking on a nonstandard build.
- **`adapterModule`** — export a non-default `createAdapter` (e.g. a custom WCLAP host). Omit to
  use the player's bundled WAM/WCLAP adapters.

**Don't** add general behavioral hooks — UX behavior belongs in the shared player, so both ABIs
inherit it. A hook that re-implements player behavior is the drift this skill exists to prevent.

## Upgrades & regeneration safety

Generated files are "owned": the run writes `.pulp-web-demo.manifest.json` with a hash per file.
`validate.mjs --check-ownership` fails if an owned file was hand-edited — so a regenerate won't
silently clobber local edits. To change generated output, change the **config or a template**,
not the emitted file.

## Validator checks (all blocking)

- Config schema + invariants (pinned version, valid profiles, `wclap` never on plain `github-pages`).
- **Cross-origin isolation covers the real `basePath`** (the top footgun: `_headers` on `/` while the
  gallery lives at `/my-plugins/` → page loads, threaded init fails).
- OG/metadata present on every page (`ogImageStrategy` honored).
- Owned-file ownership (no un-reconciled hand edits).

## Files

```
SKILL.md              agent contract          generate.mjs   deterministic generator
README.md             this guide              validate.mjs   blocking validator
config.schema.json    the config contract     templates/     versioned WAM/WCLAP/CI/hosting templates
examples/example.config.json                   copy-and-edit starting point
```
