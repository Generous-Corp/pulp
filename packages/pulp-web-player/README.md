# @danielraffel/web-player

A reusable, **skinnable**, **host-agnostic** web player for [Pulp](https://github.com/danielraffel/pulp)
audio plugins. It is the runtime behind the live WAM demo galleries
(`generouscorp.com/pulp-example-plugins`, `/pulp-classic-effects`), extracted into
a real package so any plugin author can mount a token-faithful demo of their own
plugin with one call.

> Status: **published on npm** — `@danielraffel/web-player`; the next corrected
> package version is `0.2.2`. Still
> `0.x`, so minor versions may break. Releases go out via `scripts/publish.sh`; see
> "Publishing" below.

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
import { mountDemo } from "@danielraffel/web-player";

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

The package is MIT-licensed. Its bundled Inter font files are distributed
under the SIL Open Font License 1.1; see `LICENSE` and
`THIRD_PARTY_NOTICES.md` in the npm package.

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
| `galleryHref`, `sourceHref` | the "← Gallery" link, and the per-page source link (else read from `<meta name="pulp:source">`) |
| `stateMemo` | `true` — show the state-memo surface (a text note that saves/reloads with the plugin) |
| `controllers` | `true` — show the controller/preset surface |
| **`fileUpload`** | `{ accept, label, hint, revertLabel?, onFile?, onRevert? }` — a file-dialog button **and** a drag-and-drop zone (see below) |
| `createAdapter` | inject a non-WAM backend factory (see interface) |
| `customUi` | **replace** the parameter grid with your own renderer (falls back to the grid on failure) |
| `onReady` | **add** plugin-specific page chrome once the demo is live — `({ adapter, ctx, params, mode, root }) => handle?` |

`onReady` is the seam for chrome that needs the live `HostAdapter` and/or the
demo's `AudioContext`, neither of which exists at module scope. (For a *file* affordance
you no longer need it — use `fileUpload` below, which owns the dialog + drop zone so every
demo gets identical behaviour instead of hand-rolling one.) It runs after the audio graph
is wired, never gates the demo (a
throwing `onReady` is logged and the demo keeps running), and its returned handle's
`destroy()` is called on Stop. It is **not** `customUi`: that one *replaces* the
parameter grid.

---

## File upload (dialog **and** drag-and-drop)

If a plugin takes a user-supplied file — a convolver's impulse response, a sample, a
preset — pass `fileUpload` and the player renders **both** a file-dialog button and a
drop zone. A visible button is not enough (people drag files onto anything that looks
like a target) and drop is not enough either (it does not exist on a phone, so the
button is the only path on touch).

```js
mountDemo({
  /* … */
  fileUpload: {
    accept: "audio/*,.wav,.aif,.aiff,.flac",
    label: "Load impulse response…",
    hint: "or drop a file here",
    revertLabel: "Built-in reverb",          // optional: a "back to default" button
    // The ENCODING is yours — only the plugin knows how its bytes want to look.
    onFile: async (file, api) => {
      const pcm = await decodeToWhateverThePluginWants(file);
      await api.writeBlob(pcm);              // params-preserving (see below)
      api.setMessage(`Loaded ${file.name}.`);
    },
  },
});
```

**`api.writeBlob(bytes)`** writes into the plugin's PLST blob while **preserving its
parameters** — so loading a file never resets the knobs. Omit `onFile` and the player
writes the file's raw bytes and lets the plugin decode them itself.

The zone implements the behaviours that, each one skipped, make a drop zone feel broken —
notably a **document-level guard** (the browser's default action for a file dropped
anywhere on the page is to *navigate to it*, which would destroy the running demo:
audio context, loaded state, knob positions) and **depth-counted** `dragenter`/`dragleave`
(it bubbles from the zone's own children, so a naive toggle strobes the highlight).
`test/file-upload.test.mjs` pins all of it.

## Design-token contract (skinnability)

The player is 100% token-driven — **no hardcoded brand colors**. You skin it by
supplying `tokensHref` (and optionally `fontHref`); the bundled Ink & Signal skin
(`@danielraffel/web-player/theme/tokens.css`, `.../theme/fonts.css`) is only the default.
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
factory. `@danielraffel/web-player` injects the **WAM** adapter (`src/adapters/wam.js`) by
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
onLatencyChanged    // assignable: (samples) => void        (plugin latency changed)
onStateDirty        // assignable: () => void               (plugin marked state dirty)
beginGesture(id)    // param gesture start (host automation)
endGesture(id)      // param gesture end
createSecondary(urls) → Promise<HostAdapter>  // another instance on the SAME ctx
destroy()
```

A second backend ships: the **WebCLAP** adapter (`adapters/wclap.js`,
`createWclapAdapter`). It hosts a threaded WebCLAP `.wasm` (a CLAP plugin compiled
to WebAssembly) in real time via a **worklet-resident CLAP host** — the whole CLAP
host runs inside the AudioWorklet (`vendor/pulp-wasm/wclap-processor.js`), because a
CLAP plugin calls the host event vtable synchronously during `process()`. It
implements the SAME contract, so the identical shell hosts it unchanged:

```js
import { mountShell, createWclapAdapter } from "@danielraffel/web-player";
mountShell({
  root, title: "PulpGain", mode: "audio-effect", hostLabel: "WebCLAP",
  dspUrl: "./PulpGain.wasm",                 // the threaded WebCLAP module
  processorUrl: "./…/vendor/pulp-wasm/wclap-processor.js",
  createAdapter: createWclapAdapter,
});
```

A WebCLAP page **must be cross-origin isolated** (`crossOriginIsolated === true`,
i.e. COOP + COEP on the document and CORP on every subresource) because the module
imports a shared `WebAssembly.Memory` — GitHub Pages can't send those headers, so
WebCLAP's canonical home is Cloudflare Pages. `getState`/`setState` work when the
wasm exposes the `clap.state` extension (Pulp's CLAP entry does — it serializes the
same PLST blob the native builds use); if a wasm lacks it, `descriptor.hasState` is
`false` and state degrades gracefully. To add yet another backend, implement a
factory of the same shape and pass it as `createAdapter` — nothing in the shell
changes.

---

## Package layout & exports

```
src/
  index.js              # public entry: mountDemo (WAM-default), createWamAdapter, widgets
  shell.js              # host-agnostic shell — imports NO backend
  adapters/
    wam.js              # default WAM adapter
    wclap.js            # WebCLAP adapter (worklet-resident CLAP host)
    adapter.d.ts        # the host-adapter interface (typed contract)
  state/
    plugin-state.js     # the SDK's "PLST" plugin-state container (parse/build/splice)
  ui/
    custom-ui.js        # the customUi seam (replace the parameter grid)
    file-upload.js      # the file dialog + drag-and-drop zone
  state/
    plugin-state.js     # the PLST container: parse/build, setPluginBlob/getPluginBlob
  widgets/              # canvas knob/fader/toggle/combo/meter + base
  theme/                # default Ink & Signal skin: tokens.css, fonts.css, Inter
  vendor/pulp-wasm/     # vendored SDK WAM runtime + WebCLAP host (see Provenance)
    wclap-processor.js  # self-contained classic AudioWorklet: the CLAP host + RT plugin
    wclap-abi.mjs       # single source of truth for the CLAP wasm32 ABI (parity-tested)
```

| Export | Path |
|---|---|
| `.` | `./src/index.js` |
| `./shell` | `./src/shell.js` (host-agnostic; you always pass `createAdapter`) |
| `./adapters/wam` | `./src/adapters/wam.js` |
| `./adapters/wclap` | `./src/adapters/wclap.js` |
| `./wclap-processor.js` | the worklet-resident WebCLAP host |
| `./state` | `./src/state/plugin-state.js` (PLST container helpers) |
| `./state` | `./src/state/plugin-state.js` — read/replace a plugin's OWN opaque blob without disturbing its parameters. ABI-agnostic (`getState`/`setState` are identical on WAM and WebCLAP), so a page can hand a plugin binary data — a sample, an impulse response, a wavetable — with no per-ABI entry point, and it survives a save/restore because it *is* the state. |
| `./widgets` | `./src/widgets/index.js` |
| `./theme/tokens.css`, `./theme/fonts.css` | the default skin |
| `./package.json` | for consumers that need to read the manifest |

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

- `npm test` — four suites:
  - `adapter-seam.test.mjs` — the shell stays host-agnostic and the WAM adapter exposes the full contract
  - `wclap-adapter.test.mjs` — the WebCLAP adapter + its wasm32 ABI stay in lockstep
  - `custom-ui.test.mjs` — `customUi` replaces the parameter grid and *nothing else*
  - `file-upload.test.mjs` — the drop zone's six behaviours (incl. the document guard and
    depth-counted `dragenter`/`dragleave`)

  They run the real shell against a mock adapter on a small DOM shim (`test/dom-shim.mjs`).
- `node scripts/pack-smoke.mjs` — packs the package, installs the tarball into a
  scratch consumer, and drives a real built WAM plugin headless (audio + a param +
  a state round-trip). See that script's header for what it asserts.

## Publishing

`scripts/publish.sh` is the only supported path. Publishing by hand is a footgun: a
stale checkout ships source that does not match `main` under a fresh version number,
and **npm versions are immutable**.

```sh
./scripts/publish.sh --check-auth   # is this machine set up to publish unattended?
./scripts/publish.sh --dry-run      # every guard, no publish
./scripts/publish.sh                # publish
```

It refuses unless HEAD **is** `origin/main`, the tree is clean, the version is not
already published, the tests pass on that exact commit, and the tarball actually
carries the entrypoints it claims.

Auth is **unattended**: the npm token is read from a `0600` file
(`~/.config/pulp/secrets/npm-token`), written to a `0600` temp npmrc, and shredded on
exit. 1Password holds only a *backup* — `./scripts/publish.sh --bootstrap` restores the
file once per machine. (npm 2FA here is a passkey with no typeable OTP, so the token must
be a **granular token with "Bypass 2FA" enabled**.)
