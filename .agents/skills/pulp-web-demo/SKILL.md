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
- **OG/metadata checks are blocking** (never `continue-on-error`). The generator emits the
  `og:image` **tag** but does not **render** the image (rendering needs a browser; the generator
  is offline + deterministic). So with `ogImageStrategy: "screenshot"` your pipeline MUST render
  the images *before* `validate.mjs --check-metadata` runs — the validator fails if the tag points
  at a file that isn't there, **or isn't really a PNG**. Do not settle for an HTTP 200: a static
  host (Cloudflare Pages) will serve a missing asset as 200 and cheerfully confirm an image that
  does not exist. Check the bytes. Use `ogImageStrategy: "text"` if you don't render images.
- Regeneration is ownership-aware: the validator refuses to clobber a locally-edited owned file
  without reconciliation.

## File upload (dialog **and** drag-and-drop)

If a plugin takes a user-supplied file (a convolver's impulse response, a sample, a preset),
declare it with the per-plugin **`fileUpload`** config (`accept`, `label`, `hint`). The demo must
then offer **both** a file-dialog button **and** a drop zone — people drag files onto anything
that looks like a target.

**This is a PLAYER behavior, not a per-demo one.** Like every other UX invariant, the drop-zone
mechanics belong in the shared player so both ABIs inherit them. Re-implementing a drop zone in
one demo page is exactly the drift this skill exists to prevent — its WCLAP twin would then need
its own copy, and the two would diverge.

**Put it INSIDE the plugin, directly under the controls.** Placement is not cosmetics here. A
loader parked below the plugin panel — in the page chrome, past the scope and the meter — reads
as page furniture rather than part of the instrument, and people simply do not find it: it is
the one control the demo is *asking* them to use, and it is the one sitting outside the box
everything else lives in. It belongs in the panel, immediately under the last row of controls,
with the gap between them tight enough that they read as one unit. The player exposes a slot for
exactly this (chrome the plugin owns, rendered inside the panel); use it rather than appending to
the page.

Any implementation MUST satisfy all six rules. Each one, skipped, makes the zone feel broken:

1. **Swallow drops on the whole `document`.** The browser's default action for a file dropped
   anywhere on the page is to **navigate to it**, destroying the running demo — audio context,
   loaded state, knob positions. Add `document` listeners for `dragover` and `drop` that call
   `preventDefault()` and nothing else, so a ten-pixel miss is inert rather than session-ending.
   That is a brutal punishment for a gesture you invited. **Unbind them in `destroy()`.**
2. **Count `dragenter`/`dragleave` depth — do not toggle.** `dragleave` bubbles from the zone's
   own children, so dragging across a button *inside* the zone fires it and the highlight
   strobes. Keep a depth counter; clear the highlight only at zero.
3. **Scope the highlight to the drop zone**, never the whole plugin — the highlight is the thing
   that tells someone where the target is.
4. **Set `dataTransfer.dropEffect = "copy"`** on `dragover`, so the cursor shows a copy badge
   rather than a "no entry" sign.
5. **Handle the empty drop.** `dataTransfer.files[0]` can be `undefined` (dragged text, a URL).
   Say so; don't throw.
6. **Keep the button.** Drop is a shortcut, not a replacement — and it is the *only* path on
   touch devices.

**Two testing gotchas — both make a correct implementation look broken:**

- Drive it with real `DragEvent`s and a real `DataTransfer` carrying a real `File`. The real
  browser order when the pointer crosses into a child is **`dragenter` on the new target, then
  `dragleave` on the old** — so a test that fires a bare `dragleave` will report a
  highlight-flicker bug **that does not exist**.
- Regeneration is ownership-aware: the validator refuses to clobber a locally-edited owned file
  without reconciliation.

## Gallery and theme (both optional)

- **`gallery.emit`** — `"auto"` (default): emit the gallery landing page only when the catalog
  has more than one plugin (a gallery is pointless for a single plugin). `true` / `false` force
  it; use `false` to keep a hand-curated landing page of your own.
- **`theme`** — omit it entirely and the player uses its **own bundled skin**. Only set
  `tokensHref` / `fontHref` when you are actually supplying a token stylesheet.

## File upload (dialog **and** drag-and-drop)

If a plugin takes a user-supplied file (a convolver's impulse response, a sample, a preset),
declare it with the per-plugin **`fileUpload`** config (`accept`, `label`, `hint`). The demo must
then offer **both** a file-dialog button **and** a drop zone — people drag files onto anything
that looks like a target.

**This is a PLAYER behavior, not a per-demo one.** Like every other UX invariant, the drop-zone
mechanics belong in the shared player so both ABIs inherit them. Re-implementing a drop zone in
one demo page is exactly the drift this skill exists to prevent — its WCLAP twin would then need
its own copy, and the two would diverge.

Any implementation MUST satisfy all six rules. Each one, skipped, makes the zone feel broken:

1. **Swallow drops on the whole `document`.** The browser's default action for a file dropped
   anywhere on the page is to **navigate to it**, destroying the running demo — audio context,
   loaded state, knob positions. Add `document` listeners for `dragover` and `drop` that call
   `preventDefault()` and nothing else, so a ten-pixel miss is inert rather than session-ending.
   That is a brutal punishment for a gesture you invited. **Unbind them in `destroy()`.**
2. **Count `dragenter`/`dragleave` depth — do not toggle.** `dragleave` bubbles from the zone's
   own children, so dragging across a button *inside* the zone fires it and the highlight
   strobes. Keep a depth counter; clear the highlight only at zero.
3. **Scope the highlight to the drop zone**, never the whole plugin — the highlight is the thing
   that tells someone where the target is.
4. **Set `dataTransfer.dropEffect = "copy"`** on `dragover`, so the cursor shows a copy badge
   rather than a "no entry" sign.
5. **Handle the empty drop.** `dataTransfer.files[0]` can be `undefined` (dragged text, a URL).
   Say so; don't throw.
6. **Keep the button.** Drop is a shortcut, not a replacement — and it is the *only* path on
   touch devices.

**Two testing gotchas — both make a correct implementation look broken:**

- Drive it with real `DragEvent`s and a real `DataTransfer` carrying a real `File`. The real
  browser order when the pointer crosses into a child is **`dragenter` on the new target, then
  `dragleave` on the old** — so a test that fires a bare `dragleave` will report a
  highlight-flicker bug **that does not exist**.
- **`dropEffect` cannot be asserted from a synthetic drag.** The spec honours it only during a
  real user-initiated drag, so on a synthetic `DataTransfer` `dropEffect = "copy"` silently
  stays `"none"` — a browser-driven test will "fail" a line that is perfectly correct. Pin rule 4
  in a DOM shim (where the assignment is observable) and do **not** "fix" the code to satisfy a
  synthetic drag.

**The player implements this** (`src/ui/file-upload.js` in `@danielraffel/web-player`): declare
`fileUpload` and you get the zone, the button, and all six rules for free, on both ABIs. The
player owns the *interaction*; the **encoding is yours** — only the plugin knows how its bytes
want to look — so pass `onFile(file, api)` and use the `api.writeBlob()` you're handed, which
preserves the plugin's params (loading a file must never reset the knobs). With no `onFile`, the
player writes the file's raw bytes and lets the plugin decode them.

One related trap worth knowing: **`decodeAudioData` resamples**, so the decoded buffer never
carries the file's real sample rate. If you tell the user anything about their file, parse the
WAV/AIFF header instead of trusting the decoded buffer.

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
- **Pin a player version that actually HAS the features you declare.** The config pins the
  player, so declaring `fileUpload` against a version predating it yields a demo that silently
  renders no zone. `fileUpload` needs **>= 0.2.0**. When the player gains a feature, bump the
  pin in your config — the pin is what the generated page imports.
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
