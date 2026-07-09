# @pulp/web-player

A reusable, **skinnable**, **host-agnostic** web player for [Pulp](https://github.com/danielraffel/pulp)
audio plugins. It is the runtime behind the live WAM demo galleries
(`generouscorp.com/pulp-example-plugins`, `/pulp-classic-effects`), extracted into
a real package so any plugin author can mount a token-faithful demo of their own
plugin with one call.

> Status: `0.x`, and `"private": true` in `package.json` while it is dogfooded
> inside the SDK. The public `npm publish` is a single owner-gated step; everything
> up to a green `npm publish --dry-run` is done. See "Publishing" below.

What you get in one `mountDemo()`:

- Click/tap-to-start overlay + Stop lifecycle (synchronous AudioContext unlock in
  the gesture, secure-context guard, re-entrancy guard).
- Auto-generated parameter grid rendered as native Ink & Signal canvas widgets
  (knob / fader / toggle / combo) on the kCell/kGap/kRowH layout.
- On-screen **and** computer keyboard, WebMIDI (hot-plug), pitch-bend / mod-wheel.
- Loop (in-page synth arp) / Microphone / Off audio-source selector (with the
  Safari `audioSession` mic fix).
- Triggered oscilloscope + level meter, and an always-on **safety limiter**
  between the page and the speakers.
- The `plugin_state_io` **"PLST"** state container (CRC-checked, versioned — the
  same format the native VST3/AU/CLAP builds use, so web↔native state round-trips).
- MIDI-effect visualisers (transpose / MPE / inspector / SysEx echo) and a
  chained-synth voice pool for making a MIDI effect audible.

Everything is driven through a small **host-adapter interface**; the WAM adapter
ships as the default, and a WebCLAP (or any other) backend can be added later
without touching the shell.

---

## Install & mount

```js
import { mountDemo } from "@pulp/web-player";

mountDemo({
  root: document.getElementById("app"),
  title: "My Plugin",
  subtitle: "What it does, in one honest sentence.",
  // A built Pulp WAM plugin: the SINGLE_FILE DSP + the processor module,
  // co-located and served beside this page (see the SDK's build-web.sh).
  dspUrl: "./wam-dsp.js",
  processorUrl: "./wam-processor.js",
  mode: "audio-effect",   // "instrument" | "audio-effect" | "midi-effect" (else auto from descriptor)
  paramRows: 1,           // reserve grid height so the panel doesn't jump on start
});
```

The page must be a **secure context** (`https://` or `localhost`) — AudioWorklet
requires it. Serve `.js`/`.mjs` with a JavaScript MIME type.

### Key `mountDemo` options

| Option | Purpose |
|---|---|
| `root` | container element (defaults to `document.body`) |
| `title`, `subtitle` | header + overlay copy |
| `dspUrl`, `processorUrl` | the built plugin's module URLs |
| `mode` | force `"instrument"` / `"audio-effect"` / `"midi-effect"`; omit to derive from the descriptor |
| `paramRows` | rows to reserve in the param grid before params arrive |
| `widgets` | `{ [idOrLabel]: "knob"\|"fader"\|"toggle"\|"combo" }` widget overrides |
| `choices` | `{ [idOrLabel]: string[] }` names for a stepped int param |
| `initialParams` | `{ [label]: number }` opening values (does not touch plugin defaults) |
| `inputGain` | linear source trim in front of an effect |
| `midiViz` | `"transpose"` \| `"mpe"` \| `"inspector"` \| `"sysex"` |
| `synthUrls` | `{ dsp, processor }` for the "chain into a synth" voice pool |
| **`tokensHref`** | URL of YOUR design-token stylesheet (see contract below) |
| **`fontHref`** | URL of YOUR `@font-face` stylesheet |
| **`theme`** | `"dark"` \| `"light"` \| `"auto"` (default: auto — follows `prefers-color-scheme`) |
| `hostLabel`, `hostDocsHref` | header ABI label + link (default `WAM`) |
| `createAdapter` | inject a non-WAM backend factory (see interface) |

---

## Design-token contract (skinnability)

The player is 100% token-driven — **no hardcoded brand colors**. You skin it by
supplying `tokensHref` (and optionally `fontHref`); the bundled Ink & Signal skin
(`@pulp/web-player/theme/tokens.css`, `.../theme/fonts.css`) is only the default.
Your stylesheet must define these CSS custom properties on `:root` (and, if you
want forced light/dark via `theme`, mirror them under
`:root[data-theme="light"|"dark"]`; the shell only sets `data-theme` when `theme`
is `"dark"` or `"light"`).

**Consumed color tokens (19):**

| Token | Used for |
|---|---|
| `--accent-primary` | active keys, focus outlines, links, scope stroke fallback, control fill |
| `--accent-text` | text/glyph drawn on an accent fill |
| `--accent-error` | meter peak-hold above 0.9 |
| `--text-primary` | primary text, captions |
| `--text-secondary` | sub-text, hints, status, readouts |
| `--bg-surface` | inset fields (scope, inputs, source panels, log) |
| `--bg-primary` | the panel card; black-key separation ring |
| `--bg-elevated` | overlay gradient, start button |
| `--control-border` | every border / divider |
| `--control-fill` | fader/track fills in widgets |
| `--control-thumb` | fader/slider thumb |
| `--control-track` | fader/slider track |
| `--knob-arc` | knob value arc |
| `--knob-arc-bg` | knob arc background |
| `--knob-thumb` | knob pointer |
| `--focus-ring` | keyboard-focus cue inside canvas widgets |
| `--waveform-line` | oscilloscope stroke (falls back to `--accent-primary`, then `currentColor`) |
| `--key-white` | on-screen white keys |
| `--key-black` | on-screen black keys |

**Font token (1):** `--font-family-native` — the widget/label font family (the
bundled skin binds it to Inter, matching native paint parity). Supply it in your
`fontHref` stylesheet alongside your `@font-face`.

That is the entire contract: 19 colors + 1 font family. A consumer who exports the
same names (e.g. Pulp's `pulp export-tokens --format css-variables`) drops straight
in. Tokens the bundled skin also ships (`--radius-*`, `--spacing-*`, `--meter-*`,
…) are **not** required by the player.

> Two former hardcoded-hex leaks are fixed in this package relative to the vendored
> demo player: the on-screen keys now use `--key-white`/`--key-black` (were literal
> `#ECEFF3`/`#161A21`), and the scope stroke fallback is token-derived (was
> `#16dac2`).

---

## Host-adapter interface

The shell (`src/shell.js`) never imports a plugin backend. It talks only to a
`HostAdapter` produced by a `createAdapter(ctx, { dsp, processor }) → Promise<adapter>`
factory. `@pulp/web-player` injects the **WAM** adapter (`src/adapters/wam.js`) by
default; pass `opts.createAdapter` to use another backend or a test stub. The full
typed contract is in [`src/adapters/adapter.d.ts`](./src/adapters/adapter.d.ts):

```
descriptor          // getter: { name, isInstrument, hasMidiInput/Output, hasAudioInput/Output, ... }
audioNode           // getter: the plugin's AudioNode (graph wiring)
getParameterInfo()  → Promise<ParameterInfo[]>
setParameterValue(id, value)
getParameterValue(id) → Promise<number>
scheduleMidi(status, d1, d2, offset?)
sendSysex(bytes, offset?)
getState()          → Promise<Uint8Array>     // opaque; the shell wraps it in PLST
setState(bytes)
onMidiOut           // assignable: (events, meta?) => void
onParamsChanged     // assignable: (values, params) => void
createSecondary(urls) → Promise<HostAdapter>  // another instance on the SAME ctx
destroy()
```

To add e.g. a WebCLAP backend, implement a factory of the same shape as
`adapters/wam.js` (over `wclap-host.mjs`) and pass it as `createAdapter` — nothing
in the shell changes.

---

## Package layout & exports

```
src/
  index.js              # public entry: mountDemo (WAM-default), createWamAdapter, widgets
  shell.js              # host-agnostic shell — imports NO backend
  adapters/
    wam.js              # default WAM adapter (the only backend import)
    adapter.d.ts        # the host-adapter interface (typed contract)
  widgets/              # canvas knob/fader/toggle/combo/meter + base
  theme/                # default Ink & Signal skin: tokens.css, fonts.css, Inter
  vendor/pulp-wasm/     # vendored SDK WAM runtime (see Provenance)
```

| Export | Path |
|---|---|
| `.` | `./src/index.js` |
| `./shell` | `./src/shell.js` (host-agnostic; you always pass `createAdapter`) |
| `./adapters/wam` | `./src/adapters/wam.js` |
| `./widgets` | `./src/widgets/index.js` |
| `./theme/tokens.css`, `./theme/fonts.css` | the default skin |

---

## Provenance & licensing

`src/vendor/pulp-wasm/` is a **copy** of the Pulp SDK's main-thread WAM runtime
(`core/format/src/wasm/{wam-plugin.js, wam-runtime.mjs, wam-scope.mjs,
wam-processor.js}`) so the package is self-contained. The demo repos keep their own
vendored player until this package is proven; a later step re-points them here.

- Inter font (`src/theme/Inter-Regular.ttf`, `inter.woff2`) — SIL OFL-1.1.
- The start-overlay `play` glyph inlined in `shell.js` — Lucide, ISC.
- Everything else — MIT (this package).

---

## Testing

- `npm test` — a static invariant check that the shell stays host-agnostic and the
  WAM adapter exposes the full contract (`test/adapter-seam.test.mjs`).
- `node scripts/pack-smoke.mjs` — packs the package, installs the tarball into a
  scratch consumer, and drives a real built WAM plugin headless (audio + a param +
  a state round-trip). See that script's header for what it asserts.

## Publishing (owner-gated)

`npm publish --dry-run` from this directory reports exactly what would ship. The
actual `npm publish` fixes the public name/scope/support expectation and is the
**single owner-gated step** — do not run it without the owner's go. Package-name
availability was verified: `@pulp/web-player`, `@pulp-audio/web-player`, and
`pulp-web-player` are all unregistered on npm; `@pulp/web-player` is the default
(matching the sibling `@pulp/react` package), with the other two as fallbacks if
the `@pulp` scope is unavailable to the publisher.
