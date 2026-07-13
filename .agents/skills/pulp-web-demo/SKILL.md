---
name: pulp-web-demo
description: Generate and maintain browser demos of Pulp audio plugins (both web ABIs — WAM and WCLAP) from one declarative config, so every demo mounts the SAME shared player and the two ABIs stay in lockstep. Invoke it whenever you publish/update a browser demo of a Pulp plugin, add a plugin to a demo gallery, stand up a new gallery, or touch demo pages/hosting. It runs a deterministic generator (config → stamped WAM+WCLAP site + owned-files manifest) and a blocking validator (config schema, cross-origin-isolation coverage, OG metadata, regeneration ownership). Customization is declarative config in <repo>/.pulp-web-demo/config.json (schema bundled) plus at most two typed module seams; the shared player (pinned @danielraffel/web-player) holds all UX behavior and is never vendored. Works in Claude and Codex.
---

# pulp-web-demo — generative web-demo skill

*The* way Pulp plugin web demos are built so the two web ABIs behave **identically**:
**WAM** (Emscripten → AudioWorklet, single-thread, plain static hosting) and **WCLAP**
(threaded CLAP-compiled-to-wasm hosted by a worklet-resident CLAP host, needs cross-origin
isolation). Apply by default when creating/updating a demo, adding a plugin to a gallery, or
touching hosting.

## The one rule

**Both ABIs mount the SAME shared player.** Every UX behavior — no-autoplay overlay,
iOS/mobile touch hygiene, keyboard/polyphony, scope/meter/safety-limiter, PLST state
round-trip, token theming — lives in **`@danielraffel/web-player`** (npm, **pinned**). The
generated pages **import** that pinned player; they **never vendor a copy** (vendoring is the
historical drift hazard). Fix any UX behavior once, in the player; both ABIs inherit it.

## How it works

Customization is **declarative data + at most two executable seams**. The smarts are in the
player; this skill is only the wiring + publish pipeline. Everything runs from the skill dir:

1. **Config** — `<repo-root>/.pulp-web-demo/config.json`, validated against the bundled
   `config.schema.json` (`schemaVersion: 1`, unknown keys rejected). Declares: the pinned
   player, theme token/font hrefs, per-ABI deploy profiles, the plugin catalog, gallery
   nav/cross-links, and OG/metadata policy. See `examples/example.config.json` and `README.md`.
2. **Generate** — `node generate.mjs --config <cfg> --out <siteDir>`. Deterministic (no
   network, no clock/random): identical config ⇒ byte-identical output. Stamps versioned
   templates into a WAM+WCLAP site and writes a `.pulp-web-demo.manifest.json` of owned files
   (with hashes) for upgrade safety. Use `--check` for a dry run.
3. **Validate (blocking)** — `node validate.mjs --config <cfg> --site <siteDir>
   --check-isolation --check-metadata --check-ownership`. Fails CI on: schema/invariant
   violations, cross-origin-isolation not covering the **deployed base path**, missing OG
   tags, or locally-modified owned files.

## Deploy profiles (declarative, per ABI)

Selected independently for WAM and WCLAP under `deploy.wam` / `deploy.wclap`:

- **`github-pages`** — plain static. Correct for **WAM** (no isolation needed).
- **`cloudflare`** — server COOP/COEP/CORP via a generated `_headers`. **Default for WCLAP.**
- **`github-pages+coi`** — a coi-serviceworker isolation **mirror**. A **labeled fallback
  only** — it enlarges the failure surface around isolation; prefer a header-capable host.

Each profile carries a **`basePath`** (server path — isolation headers/scope MUST cover it)
and a **`publicUrl`** (the ABI's own absolute origin+base for canonical/OG urls). WAM and
WCLAP commonly live on **different origins** — keep both accurate.

## The only two executable seams (optional `.pulp-web-demo/hooks.mjs`)

Everything else is data. Reach for these only when a value can't express it:

- **`artifactResolver`** — `(plugin, abi) => { dspUrl, processorUrl, extraAssets[] }`. Maps a
  nonstandard build's outputs into the canonical site manifest. This is THE seam that stops
  people forking when their build emits `dist/web/<target>/…` instead of the default layout.
- **`adapterModule`** — a non-default `createAdapter` (e.g. a custom WCLAP host). Omit to use
  the player's bundled WAM/WCLAP adapters.

Do **not** add general behavioral hooks or an "escape hatch" — they become places to recreate
player behavior outside the package, which is the whole thing this skill prevents.

## Non-negotiable pipeline policy

- Pinned player, imported not vendored; **cache-bust the main-thread import only** (`?v=<hash>`),
  never the worklet/dsp URLs (both sides must resolve one processor name).
- **Cross-origin isolation must cover the real deployed path** (e.g. `/my-plugins/`), and
  every worklet/wasm dependency must be isolation-compatible — validated, not assumed.
- **OG/metadata generation + checks are blocking** (never `continue-on-error`).
- Regeneration is ownership-aware: the validator refuses to clobber a locally-edited owned file
  without reconciliation.

## Gallery and theme (both optional)

- **`gallery.emit`** — `"auto"` (default): emit the gallery landing page only when the catalog
  has more than one plugin (a gallery is pointless for a single plugin). `true` / `false` force
  it; use `false` to keep a hand-curated landing page of your own.
- **`theme`** — omit it entirely and the player uses its **own bundled skin**. Only set
  `tokensHref` / `fontHref` when you are actually supplying a token stylesheet.

## Gotchas (learned the hard way — do not re-derive)

- **Never put a `?query` on the player's import specifier.** An import map keys on the *exact*
  bare specifier, so `import ... from "<pkg>?v=<hash>"` never matches the `"<pkg>"` key and the
  module fails to resolve — the demo does not load at all. The cache-bust belongs on the
  **mapped URL**. A versioned CDN pin is already its own cache key.
- **A `midi-effect` demo without `synthUrls` is SILENT.** It renders fine and looks correct, but
  a MIDI effect emits notes with nothing to play them. `synthUrls` chains it into a synth voice
  pool. Always set it for `mode: "midi-effect"`.
- **The WAM artifact set is three files, not two:** `wam-dsp.js`, `wam-processor.js`, **and
  `wam-runtime.mjs`**. The worklet imports the runtime *relative to itself*, so it must be
  co-located with the processor. Miss it and `audioWorklet.addModule()` fails with a bare
  `AbortError: Unable to load a worklet's module`.
- **Never invent a `tokensHref`.** A made-up relative path just 404s and silently drops the skin.
- **Cache-bust the main-thread entry only** — never the worklet/dsp URLs; both sides must resolve
  to one processor name.

## Example

`examples/example.config.json` is a copy-and-edit starting point: one plugin, WAM on GitHub
Pages, WCLAP on Cloudflare, with placeholder origins. Drop it at `.pulp-web-demo/config.json`
in your plugin repo and change the URLs, theme, and plugin catalog. The reader-facing
WAM-vs-WCLAP explainer that ships with the demo galleries is the companion doc.

## Provenance

`templates/hosting/coi/coi-serviceworker.js` is vendored MIT (Guido Zuidhof / coi-serviceworker).
Everything else here is part of the Pulp SDK. Follow `pulp-feature-lifecycle` for review/PR/merge
around any change to demos or this skill.
